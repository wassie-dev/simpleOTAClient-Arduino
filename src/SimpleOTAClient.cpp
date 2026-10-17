// SimpleOTAClient Arduino client library: implementation.

#include "SimpleOTAClient.h"

// On Arduino-ESP32 3.x the TLS client class was renamed from WiFiClientSecure
// to NetworkClientSecure to reflect that it is transport-agnostic (works over
// Wi-Fi, Ethernet, PPP). On 2.x only WiFiClientSecure exists. The class is a
// generic TLS-over-TCP client in both cores; the names are otherwise
// equivalent.
#if __has_include(<NetworkClientSecure.h>)
  #include <NetworkClientSecure.h>
  using SimpleOtaTLSClient = NetworkClientSecure;
#else
  #include <WiFiClientSecure.h>
  using SimpleOtaTLSClient = WiFiClientSecure;
#endif
#include <HTTPClient.h>
#include <Update.h>
#include <Preferences.h>
#include <mbedtls/sha256.h>
#include <memory>
#include <new>
#include <esp_system.h>
#include <esp_mac.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

#include "SimpleOTASigning.h"
#include "SimpleOTATime.h"
#include "SimpleOTATrial.h"
#include "SimpleOTAWatchdog.h"

#include "sdkconfig.h"
#include "nvs.h"
#include "nvs_flash.h"

// Bootloader rollback: the core can leave a new image pending verify, so a
// crash or reset before confirmRunning() returns to the previous image.
// -DSIMPLEOTA_DISABLE_BOOTLOADER_ROLLBACK keeps only the software timer.
#if (defined(CONFIG_APP_ROLLBACK_ENABLE) || defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE)) \
    && !defined(SIMPLEOTA_DISABLE_BOOTLOADER_ROLLBACK)
#define SOTA_BOOTLOADER_ROLLBACK 1
#else
#define SOTA_BOOTLOADER_ROLLBACK 0
#endif

static_assert((int)SOTA_RST_POWERON  == (int)ESP_RST_POWERON  &&
              (int)SOTA_RST_SW       == (int)ESP_RST_SW       &&
              (int)SOTA_RST_PANIC    == (int)ESP_RST_PANIC    &&
              (int)SOTA_RST_INT_WDT  == (int)ESP_RST_INT_WDT  &&
              (int)SOTA_RST_TASK_WDT == (int)ESP_RST_TASK_WDT &&
              (int)SOTA_RST_WDT      == (int)ESP_RST_WDT      &&
              (int)SOTA_RST_DEEPSLEEP == (int)ESP_RST_DEEPSLEEP &&
              (int)SOTA_RST_BROWNOUT == (int)ESP_RST_BROWNOUT,
              "SimpleOTATrial.h reset codes match esp_reset_reason_t");

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

// Tolerate `#define SIMPLEOTA_DEBUG` (no value) by treating any defined-and-
// truthy macro as enabled. The compile-time flag sets the default; setDebug()
// can flip it at runtime. The runtime check costs one global load per call
// site, which is negligible compared to the cost of the Serial.printf.
#if defined(SIMPLEOTA_DEBUG) && (SIMPLEOTA_DEBUG + 0)
bool SimpleOTAClient::_debugEnabled = true;
#else
bool SimpleOTAClient::_debugEnabled = false;
#endif

#define SOTA_LOG(fmt, ...) do { \
    if (SimpleOTAClient::_debugEnabled) \
        Serial.printf("[SimpleOTAClient] " fmt "\n", ##__VA_ARGS__); \
} while (0)

// ---------------------------------------------------------------------------
// NVS (build-number persistence)
// ---------------------------------------------------------------------------

static const char* kNvsNamespace = "simpleota";
static const char* kNvsKeyBuild  = "sota_build";
static const char* kNvsKeyHash    = "sota_hash";
static const char* kNvsKeyVersion = "sota_ver";

// Trial-install / rollback NVS keys. See SimpleOTAClient.h for the lifecycle.
// All names fit within the Preferences 15-char limit.
static const char* kNvsKeyTrial       = "sota_trial";       // uint8: 0/1/2
static const char* kNvsKeyPrevPart    = "sota_prev_part";   // uint32 partition addr
static const char* kNvsKeyPrevBuild   = "sota_prev_build";  // uint32
static const char* kNvsKeyPrevHash    = "sota_prev_hash";   // string
static const char* kNvsKeyPrevVer     = "sota_prev_ver";    // string
static const char* kNvsKeyFailDep     = "sota_fail_dep";    // string
static const char* kNvsKeyFailBuild   = "sota_fail_build";  // uint32
static const char* kNvsKeyConfirmPend = "sota_conf_pend";   // uint8: 1 if a "confirmed" report is pending
static const char* kNvsKeyStaged      = "sota_staged";      // uint8: record written, slot not switched yet
static const char* kNvsKeyOutbox      = "sota_outbox";      // blob: SotaOutbox
static const char* kNvsKeyRbReason    = "sota_rb_reason";   // string: why sota_trial is 2; missing means confirm_timeout

// Longest version label stored. Labels are short ("1.4.2"); a longer one
// from the server is not stored rather than spending NVS on it.
static const size_t kMaxVersionLabelLen = 64;

// A flashed update waits for its reboot: the boot slot already points at it.
// The next update slot holds it, so nothing may be downloaded until the
// reboot. File scope, because it describes this boot, not one instance: an
// instance created after end() and begin(), or a second one, sees it too.
static bool s_updateWaiting = false;

uint32_t SimpleOTAClient::readBuildNumber() {
    Preferences p;
    if (!p.begin(kNvsNamespace, /*readOnly=*/true)) return 0;
    uint32_t v = p.getUInt(kNvsKeyBuild, 0);
    p.end();
    return v;
}

void SimpleOTAClient::writeBuildNumber(uint32_t n) {
    Preferences p;
    if (!p.begin(kNvsNamespace, /*readOnly=*/false)) {
        SOTA_LOG("NVS open failed; build number not persisted");
        return;
    }
    p.putUInt(kNvsKeyBuild, n);
    p.end();
    SOTA_LOG("persisted build_number=%u", (unsigned)n);
}

String SimpleOTAClient::readHashFromNvs() {
    Preferences p;
    if (!p.begin(kNvsNamespace, /*readOnly=*/true)) return "";
    String v = p.getString(kNvsKeyHash, "");
    p.end();
    return v;
}

void SimpleOTAClient::writeHashToNvs(const char* hex) {
    Preferences p;
    if (!p.begin(kNvsNamespace, /*readOnly=*/false)) {
        SOTA_LOG("NVS open failed; hash not persisted");
        return;
    }
    p.putString(kNvsKeyHash, hex);
    p.end();
    SOTA_LOG("persisted current_hash");
}

String SimpleOTAClient::readVersionFromNvs() {
    Preferences p;
    if (!p.begin(kNvsNamespace, /*readOnly=*/true)) return "";
    String v = p.getString(kNvsKeyVersion, "");
    p.end();
    return v;
}

void SimpleOTAClient::writeVersionToNvs(const char* version) {
    if (!version || !*version) return;
    if (strlen(version) > kMaxVersionLabelLen) {
        SOTA_LOG("writeVersionToNvs: version string too long (%u), skipping",
                 (unsigned)strlen(version));
        return;
    }
    Preferences p;
    if (!p.begin(kNvsNamespace, /*readOnly=*/false)) {
        SOTA_LOG("NVS open failed; version not persisted");
        return;
    }
    p.putString(kNvsKeyVersion, version);
    p.end();
    SOTA_LOG("persisted version_label=%s", version);
}

// ---------------------------------------------------------------------------
// Tiny JSON helpers, sufficient for the known SimpleOTAClient payload shapes.
// We only look up specific top-level keys; no nested-object support is needed.
// ---------------------------------------------------------------------------

static void appendJsonString(String& out, const char* s) {
    out += '"';
    if (s) {
        for (const char* p = s; *p; ++p) {
            unsigned char c = (unsigned char)*p;
            if (c == '"' || c == '\\') { out += '\\'; out += (char)c; }
            else if (c == '\n')          out += "\\n";
            else if (c == '\r')          out += "\\r";
            else if (c == '\t')          out += "\\t";
            else if (c < 0x20)           { /* drop control chars */ }
            else                         out += (char)c;
        }
    }
    out += '"';
}

// Locate the start of the value for a top-level JSON key. Walks the string
// skipping over quoted string contents so that occurrences of the key inside
// a value can't false-match, and requires the matched key to be immediately
// followed (modulo whitespace) by ':', preventing prefix-key collisions
// (e.g. searching "url" matching "signed_url").
static bool jsonFindValue(const String& s, const char* key, int& valStart) {
    size_t klen = strlen(key);
    int n = (int)s.length();
    int p = 0;
    while (p < n) {
        char c = s[p];
        if (c == '"') {
            // Check whether this quoted token is exactly `key`.
            int q = p + 1;
            bool matches = true;
            for (size_t i = 0; i < klen; ++i) {
                if (q + (int)i >= n || s[q + (int)i] != key[i]) { matches = false; break; }
            }
            if (matches && q + (int)klen < n && s[q + (int)klen] == '"') {
                int after = q + (int)klen + 1;
                int look = after;
                while (look < n && (s[look] == ' ' || s[look] == '\t' ||
                                    s[look] == '\n' || s[look] == '\r')) ++look;
                if (look < n && s[look] == ':') {
                    int v = look + 1;
                    while (v < n && (s[v] == ' ' || s[v] == '\t' ||
                                     s[v] == '\n' || s[v] == '\r')) ++v;
                    valStart = v;
                    return true;
                }
            }
            // Not our key; skip the rest of this string, honoring backslash
            // escapes so embedded quotes don't terminate the scan early.
            p = q;
            while (p < n && s[p] != '"') {
                if (s[p] == '\\' && p + 1 < n) p += 2;
                else ++p;
            }
            if (p < n) ++p; // past closing quote
        } else {
            ++p;
        }
    }
    return false;
}

static bool jsonGetString(const String& s, const char* key, String& out) {
    int p;
    if (!jsonFindValue(s, key, p)) return false;
    int n = (int)s.length();
    if (p >= n || s[p] != '"') return false;
    ++p;
    String v;
    v.reserve(64);
    while (p < n && s[p] != '"') {
        char c = s[p];
        if (c == '\\' && p + 1 < n) {
            char esc = s[p + 1];
            if      (esc == 'n') v += '\n';
            else if (esc == 't') v += '\t';
            else if (esc == 'r') v += '\r';
            else                 v += esc;
            p += 2;
        } else {
            v += c;
            ++p;
        }
    }
    out = v;
    return true;
}

static bool jsonGetBool(const String& s, const char* key, bool& out) {
    int p;
    if (!jsonFindValue(s, key, p)) return false;
    if (s.substring(p, p + 4) == "true")  { out = true;  return true; }
    if (s.substring(p, p + 5) == "false") { out = false; return true; }
    return false;
}

static bool jsonGetUint(const String& s, const char* key, uint32_t& out) {
    int p;
    if (!jsonFindValue(s, key, p)) return false;
    int n = (int)s.length();
    uint32_t v = 0;
    bool any = false;
    while (p < n && s[p] >= '0' && s[p] <= '9') {
        v = v * 10u + (uint32_t)(s[p] - '0');
        ++p;
        any = true;
    }
    if (!any) return false;
    out = v;
    return true;
}

// ---------------------------------------------------------------------------
// Bundled SimpleOTAClient root CA (ISRG Root X1 / Let's Encrypt). Public PEM,
// distributed widely. Used by default for all HTTPS connections; both the
// SimpleOTA API host and the firmware storage host chain to this root.
// ---------------------------------------------------------------------------

const char* SimpleOTAClient::kSimpleOtaRootCA =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw\n"
    "TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh\n"
    "cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4\n"
    "WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu\n"
    "ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY\n"
    "MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc\n"
    "h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+\n"
    "0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U\n"
    "A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW\n"
    "T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH\n"
    "B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC\n"
    "B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv\n"
    "KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn\n"
    "OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn\n"
    "jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw\n"
    "qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI\n"
    "rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV\n"
    "HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq\n"
    "hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL\n"
    "ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ\n"
    "3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK\n"
    "NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5\n"
    "ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur\n"
    "TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC\n"
    "jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc\n"
    "oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq\n"
    "4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA\n"
    "mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d\n"
    "emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=\n"
    "-----END CERTIFICATE-----\n";

const char* const SimpleOTAClient::CHIP_ESP32   = "esp32";
const char* const SimpleOTAClient::CHIP_ESP32S2 = "esp32s2";
const char* const SimpleOTAClient::CHIP_ESP32S3 = "esp32s3";
const char* const SimpleOTAClient::CHIP_ESP32C3 = "esp32c3";
const char* const SimpleOTAClient::CHIP_ESP32C6 = "esp32c6";
const char* const SimpleOTAClient::CHIP_ESP32H2 = "esp32h2";

const char* const SimpleOTAClient::SECURITY_MODE_BASIC  = "basic";
const char* const SimpleOTAClient::SECURITY_MODE_TOKEN  = "token";
const char* const SimpleOTAClient::SECURITY_MODE_SIGNED = "signed";

// ---------------------------------------------------------------------------
// Construction / configuration
// ---------------------------------------------------------------------------

SimpleOTAClient::SimpleOTAClient(const char* token,
                     const char* chipFamily,
                     const char* deviceId,
                     const char* boardId,
                     const char* hardwareRevision)
    : _token(token),
      _deviceId(nullptr),
      _deviceIdBuf{},
      _chipFamily(chipFamily),
      _boardId(boardId),
      _hwRev(hardwareRevision),
      _partitionProfile(nullptr),
      _nvsSchemaVersion(1),
      _labels(nullptr),
      _securityMode(nullptr),
      _versionLabel(nullptr),
      _channel(nullptr),
      _caCert(SimpleOTAClient::kSimpleOtaRootCA),
      _autoReboot(true),
      _hasOffer(false),
      _size(0),
      _buildNumber(0),
      _offerSigned(false),
      _offerHasSignature(false),
      _signature{},
      _numSigningKeys(0),
      _rollbackEnabled(true),
      _managedAutoConfirm(true),
      _confirmTimeoutSec(SIMPLEOTA_CONFIRM_TIMEOUT_S),
      _bootValidated(false),
      _inTrial(false),
      _trialEnding(false),
      _timeoutMissed(false),
      _confirmTimer(nullptr),
      _lastCheckOk(false),
      _insecureWarned(false),
      _authWarned(false),
      _checkIntervalSec(0),
      _initialDelayMs(0),
      _onResult(nullptr),
      _isConnected(nullptr),
      _taskHandle(nullptr) {
    _outboxLock = xSemaphoreCreateMutexStatic(&_outboxLockBuf);
    portMUX_INITIALIZE(&_trialMux);
    if (deviceId) {
        _deviceId = deviceId;
    } else {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(_deviceIdBuf, sizeof(_deviceIdBuf),
                 "%02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        _deviceId = _deviceIdBuf;
    }
}

void SimpleOTAClient::setCACert(const char* pemRootCA)        { _caCert = pemRootCA; }
void SimpleOTAClient::setAutoReboot(bool enabled)             { _autoReboot = enabled; }
void SimpleOTAClient::setPartitionProfile(const char* profile){ _partitionProfile = profile; }
void SimpleOTAClient::setNvsSchemaVersion(uint8_t version)    { _nvsSchemaVersion = version; }
void SimpleOTAClient::setLabels(const char* jsonObject)       { _labels = jsonObject; }
void SimpleOTAClient::setSecurityMode(const char* mode)       { _securityMode = mode; }

bool SimpleOTAClient::setSigningPublicKey(const char* pem) {
    // Parse into a temporary and commit only on success, so a malformed PEM
    // never wipes an already-pinned key: a fail-open there would silently
    // downgrade the device to checksum-only flashing.
    uint8_t key[32];
    if (!sotaParseEd25519PublicKeyPem(pem, key)) {
        SOTA_LOG("signing: rejected public key (not a valid Ed25519 SPKI PEM)");
        return false;
    }
    memcpy(_signingKeys[0], key, 32);
    _signingKeyIds[0] = "";
    _numSigningKeys = 1;
    SOTA_LOG("signing: pinned public key 1/%u", (unsigned)kMaxSigningKeys);
    return true;
}

bool SimpleOTAClient::addSigningPublicKey(const char* keyId, const char* pem) {
    if (_numSigningKeys >= kMaxSigningKeys) {
        SOTA_LOG("signing: key limit (%u) reached, key not added",
                 (unsigned)kMaxSigningKeys);
        return false;
    }
    if (!sotaParseEd25519PublicKeyPem(pem, _signingKeys[_numSigningKeys])) {
        SOTA_LOG("signing: rejected public key (not a valid Ed25519 SPKI PEM)");
        return false;
    }
    _signingKeyIds[_numSigningKeys] = keyId ? keyId : "";
    ++_numSigningKeys;
    SOTA_LOG("signing: pinned public key %u/%u (id=%s)",
             (unsigned)_numSigningKeys, (unsigned)kMaxSigningKeys,
             keyId ? keyId : "(none)");
    return true;
}
void SimpleOTAClient::setVersionLabel(const char* label)      { _versionLabel = label; }
void SimpleOTAClient::setChannel(const char* channel)         { _channel = channel; }
void SimpleOTAClient::setDebug(bool enabled)                  { _debugEnabled = enabled; }
void SimpleOTAClient::setRollbackEnabled(bool enabled)        { _rollbackEnabled = enabled; }
void SimpleOTAClient::setManagedAutoConfirm(bool enabled)     { _managedAutoConfirm = enabled; }
void SimpleOTAClient::setConfirmTimeout(uint32_t seconds) {
    // Clamp to a sane range. Below ~10s gives users no time to debug a TRIAL
    // boot on the serial console; above 24h is a bug, not a configuration.
    if (seconds < 10)    seconds = 10;
    if (seconds > 86400) seconds = 86400;
    _confirmTimeoutSec = seconds;
}
bool SimpleOTAClient::isTrialInstall() {
    processBootValidation();
    return _inTrial;
}
const char* SimpleOTAClient::lastOfferedVersion() const       { return _offeredVersion.c_str(); }

void SimpleOTAClient::begin(uint32_t checkIntervalSec,
                      void (*onResult)(OTAResult),
                      bool (*isConnected)(),
                      uint32_t initialDelayMs) {
    if (_taskHandle != nullptr) return;
    _checkIntervalSec = checkIntervalSec;
    _initialDelayMs   = initialDelayMs;
    _onResult = onResult;
    _isConnected = isConnected;
    // Run boot validation synchronously on the caller (typically the Arduino
    // main task) so the rollback timer is armed before begin() returns. The
    // timer must exist by the time the managed task starts checking _inTrial.
    processBootValidation();
    xTaskCreate(_taskEntry, "simpleota", 8192, this, 1, &_taskHandle);
}

void SimpleOTAClient::end() {
    if (_taskHandle == nullptr) return;
    vTaskDelete(_taskHandle);
    _taskHandle = nullptr;
}

void SimpleOTAClient::_taskEntry(void* arg) {
    static_cast<SimpleOTAClient*>(arg)->_taskLoop();
    vTaskDelete(nullptr);  // should not be reached
}

void SimpleOTAClient::_taskLoop() {
    // Defer the first check if the caller requested an initial delay. This
    // avoids lwIP contention (e.g. udp_remove assert) when other TLS
    // sessions (MQTT, etc.) are starting concurrently at boot.
    if (_initialDelayMs > 0) {
        vTaskDelay(sotaMillisToTicks(_initialDelayMs, configTICK_RATE_HZ));
    }

    // While a trial is in progress, run the trial loop (short retry interval,
    // auto-confirm on first 2xx from /check/). Once the trial resolves, fall
    // through to the normal cadence loop. The independent FreeRTOS timer is
    // responsible for actually rebooting on confirm-timeout; _trialLoop()
    // exits when either confirmRunning() was called or the deadline passed.
    // When it confirmed on its own check, that check is this cycle's: the
    // next one waits the interval.
    if (_inTrial && _trialLoop()) {
        vTaskDelay(sotaSecondsToTicks(_checkIntervalSec, configTICK_RATE_HZ));
    }

    for (;;) {
        // Wait for IP connectivity if the application provided a probe.
        // Without one, just attempt the check and rely on transport-failure
        // handling to absorb a missing connection.
        if (_isConnected) {
            while (!_isConnected()) {
                vTaskDelay(sotaMillisToTicks(1000, configTICK_RATE_HZ));
            }
        }

        if (check()) {
            OTAResult r = apply();  // reboots on success by default
            if (_onResult) _onResult(r);
        }

        vTaskDelay(sotaSecondsToTicks(_checkIntervalSec, configTICK_RATE_HZ));
    }
}

// Managed-mode trial loop: short retry interval, deadline-bounded waits,
// auto-confirm on first 2xx from /check/ (when _managedAutoConfirm is true).
// Exits when the trial is confirmed (by us or by the app) or the deadline
// passes. The FreeRTOS confirm timer will fire performRollback() at the
// deadline regardless of where this loop is currently parked.
bool SimpleOTAClient::_trialLoop() {
    const uint32_t deadlineMs = millis() + _confirmTimeoutSec * 1000UL;

    while (_inTrial && (int32_t)(deadlineMs - millis()) > 0) {
        // Wait for IP connectivity if probe supplied. Bounded by the deadline
        // so a forever-disconnected transport does not spin past it.
        if (_isConnected) {
            while (!_isConnected() && (int32_t)(deadlineMs - millis()) > 0) {
                vTaskDelay(sotaMillisToTicks(1000, configTICK_RATE_HZ));
            }
        }
        if ((int32_t)(deadlineMs - millis()) <= 0) break;

        bool ok = check();
        // _lastCheckOk is set true by check() on a 200 from /check/,
        // regardless of update_available. This proves boot + connectivity +
        // TLS + token auth + server reachability: the correct trigger for
        // managed auto-confirm.
        if (_managedAutoConfirm && _lastCheckOk) {
            SOTA_LOG("trial: server reachable; auto-confirming (%s)",
                     ok ? "offer available" : "no update");
            if (confirmRunning()) {
                sendPendingReports();  // the confirmed report, without waiting a check
                if (ok) {
                    OTAResult r = apply();
                    if (_onResult) _onResult(r);
                }
                return true;
            }
            SOTA_LOG("trial: could not confirm; trying again");
        }

        // Bounded sleep before the next attempt.
        uint32_t now = millis();
        if ((int32_t)(deadlineMs - now) <= 0) break;
        uint32_t remaining = deadlineMs - now;
        uint32_t sleep = SIMPLEOTA_TRIAL_RETRY_INTERVAL_S * 1000UL;
        if (sleep > remaining) sleep = remaining;
        vTaskDelay(sotaMillisToTicks(sleep, configTICK_RATE_HZ));
    }

    // Either confirmRunning() was called externally (drops _inTrial) or the
    // deadline passed. In the latter case the FreeRTOS timer has fired or is
    // about to fire performRollback() -> esp_restart(). Returning here lets
    // the outer task loop continue normally if (somehow) we didn't roll back.
    return false;
}

// Emit a single Serial warning when running with setInsecure(). This is
// always on (independent of SIMPLEOTA_DEBUG) because shipping firmware that
// flashes server-supplied URLs without TLS verification is the kind of thing
// you want to notice during a first integration run.
void SimpleOTAClient::warnInsecureOnce() {
    if (_insecureWarned || _caCert) return;
    _insecureWarned = true;
    Serial.println(F("[SimpleOTAClient] WARNING: TLS verification disabled. "
                     "Call setCACert(SimpleOTAClient::kSimpleOtaRootCA) for production."));
}

void SimpleOTAClient::warnUnverifiedSigned() {
    // Always-on (not gated on setDebug): applying a signed artifact without
    // verification silently downgrades the security the fleet operator
    // believes they have. Fired on every such apply, not once per boot:
    // applies are inherently rare and each is a discrete security event.
    Serial.println(F("[SimpleOTAClient] WARNING: signed firmware offered but no "
                     "signing key is pinned; applying WITHOUT signature "
                     "verification. Call setSigningPublicKey() to enforce it."));
}

void SimpleOTAClient::clearOffer() {
    _hasOffer          = false;
    _url               = "";
    _checksum          = "";
    _size              = 0;
    _deploymentId      = "";
    _buildNumber       = 0;
    _offeredVersion    = "";
    _offerSigned       = false;
    _offerHasSignature = false;
    _sigKeyId          = "";
}

// ---------------------------------------------------------------------------
// HTTPS POST helper (used for /check and /status only, NOT for the firmware
// download, which streams via HTTPClient::GET in apply()).
//
// Retry policy: on transport failure (HTTP code <= 0), wait 2 s and retry once.
// Any received HTTP response (code > 0) is returned immediately with no retry;
// retrying a 4xx would be pointless and retrying a 5xx is left to the next
// scheduled check() cycle.
// ---------------------------------------------------------------------------

// Reject tokens containing CR/LF; defends against header injection if a
// caller ever sourced the token from untrusted input.
static bool tokenLooksSafe(const char* t) {
    if (!t) return false;
    for (const char* p = t; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c == '\r' || c == '\n' || c < 0x20 || c == 0x7f) return false;
    }
    return true;
}

int SimpleOTAClient::postJson(const char* path, const String& body, String* outResp) {
    if (!tokenLooksSafe(_token)) {
        SOTA_LOG("postJson: token contains illegal characters");
        return -1;
    }
    warnInsecureOnce();
    String url = String("https://simpleota.com") + path;
    for (int attempt = 0; attempt < 2; ++attempt) {
        SimpleOtaTLSClient client;
        if (_caCert) client.setCACert(_caCert);
        else         client.setInsecure();

        HTTPClient http;
        http.setTimeout(SIMPLEOTA_TIMEOUT_MS);
        if (!http.begin(client, url)) {
            SOTA_LOG("http.begin failed (attempt %d) url=%s", attempt, url.c_str());
        } else {
            http.addHeader("Authorization", String("Bearer ") + _token);
            http.addHeader("Content-Type", "application/json");
            int code = http.POST(body);
            if (code > 0) {
                if (outResp) *outResp = http.getString();
                http.end();
                SOTA_LOG("POST %s -> %d", path, code);
                if ((code == 401 || code == 403) && !_authWarned) {
                    _authWarned = true;
                    Serial.println(F("[SimpleOTAClient] WARNING: server rejected the token "
                                     "(HTTP 401/403). Check your project token."));
                }
                return code;  // any received response: no retry
            }
            SOTA_LOG("POST %s transport error %d (attempt %d)", path, code, attempt);
            http.end();
        }
        if (attempt == 0) delay(2000);  // only reached on transport failure (code <= 0)
    }
    return -1;
}

// ---------------------------------------------------------------------------
// /api/v1/ota/check/
// ---------------------------------------------------------------------------

bool SimpleOTAClient::check() {
    // Boot validation runs once per instance. Wired into check() so
    // polling-mode users get rollback support without an extra setup() call.
    // begin() also calls this directly so managed mode is symmetric.
    processBootValidation();

    // Queued confirmed / rolled_back reports. One that gets no response
    // stays queued for the next check(); none of them holds up an update.
    sendPendingReports();

    clearOffer();

    uint32_t currentBuild = readBuildNumber();
    SOTA_LOG("check: current_build_number=%u", (unsigned)currentBuild);

    String body;
    body.reserve(512);
    body += '{';
    body += "\"device_id\":";              appendJsonString(body, _deviceId);
    body += ",\"framework\":\"arduino\"";
    body += ",\"chip_family\":";           appendJsonString(body, _chipFamily);
    if (_boardId) {
        body += ",\"board_id\":";          appendJsonString(body, _boardId);
    }
    if (_hwRev) {
        body += ",\"hardware_revision\":"; appendJsonString(body, _hwRev);
    }
    body += ",\"current_build_number\":";
    body += String(currentBuild);
    body += ",\"nvs_schema_version\":";
    body += String(_nvsSchemaVersion);
    if (_partitionProfile) {
        body += ",\"partition_profile\":"; appendJsonString(body, _partitionProfile);
    }
    if (_labels) {
        // _labels is a pre-formed JSON object string, embedded verbatim.
        body += ",\"labels\":";
        body += _labels;
    }
    if (_securityMode) {
        body += ",\"security_mode\":";     appendJsonString(body, _securityMode);
    }
    {
        String nvVersion;
        const char* vLabel = _versionLabel;
        if (!vLabel) {
            nvVersion = readVersionFromNvs();
            if (nvVersion.length()) vLabel = nvVersion.c_str();
        }
        if (vLabel) {
            body += ",\"version_label\":"; appendJsonString(body, vLabel);
        }
    }
    {
        String nvHash = readHashFromNvs();
        if (nvHash.length()) {
            body += ",\"current_hash\":"; appendJsonString(body, nvHash.c_str());
        }
    }
    if (_channel) {
        body += ",\"channel\":";         appendJsonString(body, _channel);
    }
    body += '}';

    String resp;
    _lastCheckOk = false;
    int code = postJson("/api/v1/ota/check/", body, &resp);
    if (code != 200) {
        SOTA_LOG("check: non-200 (%d), no offer", code);
        return false;
    }
    _lastCheckOk = true;  // the server answered: it is reachable

    bool available = false;
    if (!jsonGetBool(resp, "update_available", available) || !available) {
        SOTA_LOG("check: no update available");
        return false;
    }

    String url, checksum, deploymentId;
    uint32_t buildNumber = 0, size = 0;
    if (!jsonGetString(resp, "url", url) ||
        !jsonGetString(resp, "checksum", checksum) ||
        !jsonGetUint  (resp, "build_number", buildNumber)) {
        SOTA_LOG("check: malformed offer payload");
        return false;
    }
    jsonGetUint  (resp, "size",          size);          // optional
    jsonGetString(resp, "deployment_id", deploymentId);  // optional
    String offeredVersion;
    jsonGetString(resp, "version",       offeredVersion); // optional

    // Signed-firmware fields (present only when the offered artifact has
    // security_mode "signed"; see the wiki signed-firmware guide).
    String offerMode, sigB64, sigAlgo, sigKeyId;
    jsonGetString(resp, "security_mode",       offerMode);  // optional
    jsonGetString(resp, "signature",           sigB64);     // optional
    jsonGetString(resp, "signature_algorithm", sigAlgo);    // optional
    jsonGetString(resp, "signing_key_id",      sigKeyId);   // optional

    _offerSigned       = (offerMode == SECURITY_MODE_SIGNED);
    _sigKeyId          = sigKeyId;
    _offerHasSignature = false;
    if (_offerSigned && sigB64.length()) {
        // An algorithm other than ed25519 leaves the signature unusable:
        // verification then fails closed in apply() when keys are pinned.
        bool algoOk = (sigAlgo.length() == 0) || (sigAlgo == "ed25519");
        if (algoOk && sotaDecodeSignatureB64(sigB64.c_str(), _signature)) {
            _offerHasSignature = true;
        } else {
            SOTA_LOG("check: signed offer carries unusable signature (algo=%s)",
                     sigAlgo.c_str());
        }
    }

    _url            = url;
    _checksum       = checksum;
    _checksum.toLowerCase();
    _size           = size;
    _deploymentId   = deploymentId;
    _buildNumber    = buildNumber;
    _offeredVersion = offeredVersion;
    _hasOffer       = true;

    SOTA_LOG("check: offer build=%u version=%s size=%u deployment=%s",
             (unsigned)buildNumber, offeredVersion.c_str(), (unsigned)size, deploymentId.c_str());
    return true;
}

// ---------------------------------------------------------------------------
// /api/v1/ota/status/
// ---------------------------------------------------------------------------

// Post one status event. Returns the HTTP status, or -1 when no response
// came back. The response body is stored in *outResp when given.
int SimpleOTAClient::postStatus(const char* event, const char* reason,
                                const char* deploymentId, uint32_t buildNumber,
                                String* outResp) {
    // Log the event name + deployment context BEFORE the network call so
    // it's visible even if the POST hangs, and so callers can distinguish
    // back-to-back status posts in the trace (e.g. validated vs. reboot).
    SOTA_LOG("status: event=%s build=%u deployment=%s%s%s",
             event, (unsigned)buildNumber, deploymentId,
             reason ? " reason=" : "", reason ? reason : "");

    String body;
    body.reserve(192);
    body += '{';
    body += "\"device_id\":";     appendJsonString(body, _deviceId);
    body += ",\"deployment_id\":"; appendJsonString(body, deploymentId);
    body += ",\"event\":";         appendJsonString(body, event);
    body += ",\"build_number\":";
    body += String(buildNumber);
    if (reason) {
        body += ",\"reason\":";    appendJsonString(body, reason);
    }
    body += '}';

    return postJson("/api/v1/ota/status/", body, outResp);
}

bool SimpleOTAClient::sendStatusFor(const char* event, const char* reason,
                                    const char* deploymentId, uint32_t buildNumber) {
    String resp;
    const int code = postStatus(event, reason, deploymentId, buildNumber, &resp);
    if (code < 200 || code >= 300) {
        SOTA_LOG("status: event=%s rejected (http=%d)", event, code);
        return false;
    }
    bool accepted = false;
    jsonGetBool(resp, "accepted", accepted);
    SOTA_LOG("status: event=%s accepted=%d", event, accepted ? 1 : 0);
    return accepted;
}

bool SimpleOTAClient::sendStatus(const char* event, const char* reason) {
    // Convenience wrapper for the common case: the currently-pending offer.
    return sendStatusFor(event, reason, _deploymentId.c_str(), _buildNumber);
}

bool SimpleOTAClient::report(const char* event, const char* reason) {
    // sendStatus() includes deployment_id and build_number; without a
    // deployment ID the backend can't correlate the event. _deploymentId is
    // populated by check() and retained through a successful apply() until
    // the next check() invocation, so report() is valid both while an offer
    // is pending and in the post-apply window (e.g. report("reboot")).
    if (_deploymentId.isEmpty()) {
        SOTA_LOG("report: ignored (no deployment context)");
        return false;
    }
    return sendStatus(event, reason);
}

// For setAutoReboot(false): emit the "reboot" event apply() emits on the
// auto-reboot path, then restart. Does not return. The event belongs to the
// waiting update, and a check() after apply() replaces the current offer,
// so its deployment comes from the context apply() saved.
void SimpleOTAClient::rebootForUpdate() {
    String   dep;
    uint32_t build = 0;
    if (s_updateWaiting) {
        Preferences p;
        if (p.begin(kNvsNamespace, /*readOnly=*/true)) {
            dep   = p.getString(kNvsKeyFailDep, "");
            build = p.getUInt  (kNvsKeyFailBuild, 0);
            p.end();
        }
    }
    if (!dep.isEmpty()) {
        sendStatusFor("reboot", nullptr, dep.c_str(), build);
    } else {
        SOTA_LOG("rebootForUpdate: no update is waiting; rebooting without a status post");
    }
    SOTA_LOG("rebootForUpdate: rebooting");
    delay(100);
    esp_restart();
    // not reached
}

// ---------------------------------------------------------------------------
// apply(): download firmware, hash on the fly, flash, persist build number,
// and (by default) reboot.
// ---------------------------------------------------------------------------

OTAResult SimpleOTAClient::apply() {
    // Boot validation moves any report left in the trial keys to the outbox
    // before a new trial record is written there.
    processBootValidation();

    // The next update slot is the one after the running image. While a
    // flashed image waits for its reboot, that slot holds it, and another
    // download would erase it.
    if (s_updateWaiting) {
        SOTA_LOG("apply: an update is waiting for rebootForUpdate(); not flashing");
        return OTA_REBOOT_PENDING;
    }
    // The other slot holds the previous image, which the trial may still
    // need to switch back to.
    if (_inTrial) {
        SOTA_LOG("apply: running image not confirmed yet; not flashing");
        return OTA_UNCONFIRMED;
    }
    if (!_hasOffer) {
        SOTA_LOG("apply: no offer");
        return OTA_NO_OFFER;
    }

    // Reject non-HTTPS download URLs. The server is expected to issue a
    // pre-signed https:// URL; anything else (especially in the absence of
    // CA pinning) would let an attacker downgrade firmware delivery.
    if (!_url.startsWith("https://")) {
        SOTA_LOG("apply: refusing non-https url");
        sendStatus("failed", "insecure_url");
        clearOffer();
        return OTA_FLASH_FAIL;
    }

    warnInsecureOnce();

    // Signed-firmware gate. Decided BEFORE the download so a rejectable
    // offer wastes no bandwidth or flash wear. The policy is a pure,
    // host-tested function (sotaSignedGate) so the anti-downgrade rule
    // cannot silently regress: a device in signed mode with a pinned key
    // MUST see a usable signature regardless of what the offer claims.
    const bool deviceReportsSigned =
        _securityMode && strcmp(_securityMode, SECURITY_MODE_SIGNED) == 0;
    bool verifySignature = false;
    switch (sotaSignedGate(deviceReportsSigned, _numSigningKeys,
                           _offerSigned, _offerHasSignature)) {
        case SOTA_GATE_SKIP:
            break;
        case SOTA_GATE_VERIFY:
            verifySignature = true;
            break;
        case SOTA_GATE_WARN_UNVERIFIED:
            warnUnverifiedSigned();
            break;
        case SOTA_GATE_FAIL_CLOSED:
            SOTA_LOG("apply: signed update required but signature "
                     "absent/unusable, rejecting");
            sendStatus("failed", "signature_invalid");
            clearOffer();
            return OTA_SIGNATURE_FAIL;
    }

    // Streaming Ed25519 verifier. Heap-allocated (not a ~700 B stack local)
    // so it does not coexist with buf[1024] + the TLS client on the managed
    // task's stack. Allocated here, before the download, so an allocation
    // failure fails closed rather than downgrading to checksum-only.
    std::unique_ptr<SotaSignatureVerifier> sigVerifier;
    if (verifySignature) {
        // Key selection: when the offer names a key id we have pinned,
        // verify against that key alone; otherwise try every pinned key.
        const uint8_t (*verifyKeys)[32] = _signingKeys;
        uint8_t numVerifyKeys = _numSigningKeys;
        if (_sigKeyId.length()) {
            for (uint8_t i = 0; i < _numSigningKeys; ++i) {
                if (_signingKeyIds[i] == _sigKeyId) {
                    verifyKeys = &_signingKeys[i];
                    numVerifyKeys = 1;
                    break;
                }
            }
        }
        sigVerifier.reset(new (std::nothrow) SotaSignatureVerifier());
        if (!sigVerifier) {
            SOTA_LOG("apply: could not allocate signature verifier, "
                     "failing closed");
            sendStatus("failed", "signature_invalid");
            clearOffer();
            return OTA_SIGNATURE_FAIL;
        }
        sigVerifier->begin(_signature, verifyKeys, numVerifyKeys);
    }

    sendStatus("download_started", nullptr);

    SimpleOtaTLSClient client;
    if (_caCert) client.setCACert(_caCert);
    else         client.setInsecure();

    HTTPClient http;
    http.setTimeout(SIMPLEOTA_TIMEOUT_MS);
    if (!http.begin(client, _url)) {
        SOTA_LOG("apply: http.begin failed");
        http.end();  // release the connection BEFORE the status POST: the
                     // TLS heap must be free or the report itself fails.
        sendStatus("failed", "https_begin_failed");
        clearOffer();
        return OTA_FLASH_FAIL;
    }

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        char r[32];
        snprintf(r, sizeof(r), "http_status_%d", code);
        SOTA_LOG("apply: GET firmware -> %d", code);
        http.end();  // free the download TLS session before the status POST
        sendStatus("failed", r);
        clearOffer();
        return OTA_FLASH_FAIL;
    }

    int contentLen = http.getSize();
    size_t expected = (contentLen > 0) ? (size_t)contentLen
                    : (_size > 0)      ? (size_t)_size
                                       : (size_t)UPDATE_SIZE_UNKNOWN;

    if (!Update.begin(expected)) {
        SOTA_LOG("apply: Update.begin failed (need=%u)", (unsigned)expected);
        http.end();  // free the download TLS session before the status POST
        sendStatus("failed", "update_begin_failed");
        clearOffer();
        return OTA_FLASH_FAIL;
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, /*is224=*/0);

    // getStreamPtr() returns WiFiClient* on Arduino-ESP32 2.x and
    // NetworkClient* on 3.x; auto* keeps us source-compatible with both.
    auto* stream = http.getStreamPtr();
    uint8_t buf[1024];
    size_t total = 0;
    uint32_t lastByteAt = millis();

    while (http.connected() && (contentLen < 0 || total < (size_t)contentLen)) {
        int avail = stream->available();
        if (avail < 0) {
            // Negative return == socket error on Arduino-ESP32.
            mbedtls_sha256_free(&sha);
            Update.abort();
            SOTA_LOG("apply: stream error during download at %u bytes", (unsigned)total);
            http.end();  // free the download TLS session before the status POST
            sendStatus("failed", "stream_error");
            clearOffer();
            return OTA_FLASH_FAIL;
        }
        if (avail > 0) {
            size_t want = ((size_t)avail < sizeof(buf)) ? (size_t)avail : sizeof(buf);
            int n = stream->readBytes(buf, want);
            if (n <= 0) break;
            if (Update.write(buf, n) != (size_t)n) {
                mbedtls_sha256_free(&sha);
                Update.abort();
                http.end();  // free the download TLS session before the status POST
                sendStatus("failed", "update_write_failed");
                clearOffer();
                return OTA_FLASH_FAIL;
            }
            mbedtls_sha256_update(&sha, buf, n);
            if (verifySignature) sigVerifier->update(buf, (size_t)n);
            total += (size_t)n;
            lastByteAt = millis();
            // Keep the IDLE task fed on fast links so the task watchdog
            // doesn't trip during long sustained downloads.
            yield();
        } else {
            if (millis() - lastByteAt > SIMPLEOTA_TIMEOUT_MS) {
                mbedtls_sha256_free(&sha);
                Update.abort();
                SOTA_LOG("apply: download stalled at %u bytes", (unsigned)total);
                http.end();  // free the download TLS session before the status POST
                sendStatus("failed", "download_stalled");
                clearOffer();
                return OTA_FLASH_FAIL;
            }
            delay(1);
        }
    }

    if (contentLen > 0 && total != (size_t)contentLen) {
        mbedtls_sha256_free(&sha);
        Update.abort();
        SOTA_LOG("apply: short read %u/%d", (unsigned)total, contentLen);
        http.end();  // free the download TLS session before the status POST
        sendStatus("failed", "short_read");
        clearOffer();
        return OTA_FLASH_FAIL;
    }

    // The stream is fully consumed: release the download connection (and its
    // ~45 KB TLS session) NOW, before any status POST opens its own TLS
    // session. With MQTT also connected, three concurrent sessions exhaust
    // the heap (field failure: mbedtls -0x7F00, lost status events).
    http.end();

    sendStatus("downloaded", nullptr);

    uint8_t hash[32];
    mbedtls_sha256_finish(&sha, hash);
    mbedtls_sha256_free(&sha);

    char hex[65];
    for (int i = 0; i < 32; ++i) sprintf(hex + i * 2, "%02x", hash[i]);
    hex[64] = 0;

    if (_checksum != hex) {
        SOTA_LOG("apply: checksum mismatch expected=%s actual=%s",
                 _checksum.c_str(), hex);
        Update.abort();
        sendStatus("failed", "checksum_mismatch");
        clearOffer();
        return OTA_CHECKSUM_FAIL;
    }

    // Ed25519 signature verification over the exact downloaded bytes, BEFORE
    // Update.end() marks the new image bootable. Checked after the checksum
    // on purpose: a signature failure here means the bytes arrived intact
    // but were not approved by the pinned key. That is a security signal,
    // not flakiness; the server surfaces the reason on the device row.
    if (verifySignature && !sigVerifier->final()) {
        SOTA_LOG("apply: signature verification FAILED (key_id=%s), rejecting image",
                 _sigKeyId.length() ? _sigKeyId.c_str() : "(none)");
        Update.abort();
        sendStatus("failed", "signature_invalid");
        clearOffer();
        return OTA_SIGNATURE_FAIL;
    }

    // The trial record goes in before Update.end() switches the boot slot,
    // so a power cut can never leave a bootable new image without one.
    if (_rollbackEnabled && !stageTrialRecord(hex)) {
        Update.abort();
        sendStatus("failed", "state_save_failed");
        clearOffer();
        return OTA_FLASH_FAIL;
    }

    if (!Update.end(true)) {
        SOTA_LOG("apply: Update.end failed err=%d", (int)Update.getError());
        if (_rollbackEnabled) abandonTrialRecord(true);
        sendStatus("failed", "update_end_failed");
        clearOffer();
        return OTA_FLASH_FAIL;
    }
    s_updateWaiting = true;

    if (_rollbackEnabled) {
        unstageTrialRecord();
    } else {
        writeBuildNumber(_buildNumber);
        writeHashToNvs(hex);
        writeVersionToNvs(_offeredVersion.c_str());
        saveConfirmContext();
    }
    sendStatus("flashed", nullptr);
    sendStatus("validated", nullptr);

    // Clear the download-related fields so a second apply() call (with
    // auto-reboot disabled) doesn't redownload and reflash the same image.
    // _deploymentId and _buildNumber are deliberately retained so the
    // "reboot" event below (and any application report() call) can correlate
    // with the just-applied deployment. The next check() invocation calls
    // clearOffer() at its top, which resets that context.
    _hasOffer       = false;
    _url            = "";
    _checksum       = "";
    _size           = 0;
    _offeredVersion = "";

    if (_autoReboot) {
        // Emit "reboot" immediately before esp_restart() so the server can
        // distinguish a clean apply-driven restart from a crash. Only fires
        // on the auto-reboot path; applications that drive their own restart
        // should call rebootForUpdate() instead of a bare ESP.restart().
        sendStatus("reboot", nullptr);
        SOTA_LOG("apply: rebooting");
        delay(100);
        esp_restart();
        // not reached
    }
    return OTA_SUCCESS;
}

// ---------------------------------------------------------------------------
// Trial record (SimpleOTATrial.h) and report outbox (SimpleOTAOutbox.h)
// ---------------------------------------------------------------------------

// The trial record and partitions this boot sees. NVS must be initialised.
static void sotaReadBootFacts(SotaBootFacts* facts) {
    *facts = SotaBootFacts();
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8 (h, kNvsKeyTrial,    &facts->trial);
        nvs_get_u8 (h, kNvsKeyStaged,   &facts->staged);
        nvs_get_u32(h, kNvsKeyPrevPart, &facts->prevPart);
        nvs_close(h);
    }
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* boot    = esp_ota_get_boot_partition();
    facts->runningPart   = running ? running->address : 0;
    facts->bootPart      = boot ? boot->address : 0;
    facts->deepSleepWake = esp_reset_reason() == ESP_RST_DEEPSLEEP;
}

// Tell the bootloader the running image is good. False when it could not.
static bool sotaMarkRunningValid() {
#if SOTA_BOOTLOADER_ROLLBACK
    const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK) {
        SOTA_LOG("mark valid failed: 0x%x", (unsigned)err);
        return false;
    }
#endif
    return true;
}

#if SOTA_BOOTLOADER_ROLLBACK
// True while the bootloader still has the running image on probation.
static bool sotaRunningImagePending() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    return running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
           state == ESP_OTA_IMG_PENDING_VERIFY;
}

// initArduino() accepts a pending image before setup() unless this returns
// true. Only a SimpleOTA trial is held; any other image, such as one written
// by ArduinoOTA, is accepted as usual. This runs before the core's own
// nvs_flash_init(): initialise here, never erase. Serial is not up yet, so
// nothing is logged. Kept in this file so a PlatformIO archive links it in
// with the class and it replaces the core's weak definition.
extern "C" bool verifyRollbackLater() {
    if (nvs_flash_init() != ESP_OK) return false;
    SotaBootFacts facts;
    sotaReadBootFacts(&facts);
    const bool hold =
        sotaHoldPendingImage(sotaRunningImagePending(), sotaClassifyBoot(&facts));
    // A held trial gets the chip watchdog straight away, so a hang before
    // the sketch reaches the library is reset too. setConfirmTimeout() is
    // not known yet: the compile-time default applies until boot validation.
    if (hold) {
        sota_wdt_arm(SIMPLEOTA_CONFIRM_TIMEOUT_S);
    } else {
        sota_wdt_quiesce();
    }
    return hold;
}
#endif

// Restart the chip watchdog with this instance's confirm time. It only helps
// while the bootloader can return to the previous image.
void SimpleOTAClient::armTrialWatchdog() {
#if SOTA_BOOTLOADER_ROLLBACK
    if (!sotaRunningImagePending()) return;
    sota_wdt_arm(_confirmTimeoutSec);
    if (sota_wdt_is_clamped()) {
        Serial.printf("[SimpleOTAClient] confirm timeout %us is longer than the chip "
                      "watchdog can wait; it resets the chip at its limit.\n",
                      (unsigned)_confirmTimeoutSec);
    }
#endif
}

// Write the trial record, with the new image's identity as the live one,
// before Update.end() switches the boot slot, so a bootable new image always
// has one. sota_staged marks a record whose slot switch has not happened
// yet; it goes first and sota_trial last, because each put reaches flash on
// its own. Returns false, with the record removed and the previous identity
// back in place, when it could not be saved.
bool SimpleOTAClient::stageTrialRecord(const char* newHash) {
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (!running) return false;
    const uint32_t prevBuild = readBuildNumber();
    const String   prevHash  = readHashFromNvs();
    const String   prevVer   = readVersionFromNvs();

    Preferences p;
    if (!p.begin(kNvsNamespace, /*readOnly=*/false)) return false;
    const String newVer =
        _offeredVersion.length() <= kMaxVersionLabelLen ? _offeredVersion : String();
    const bool snapshotOk = p.putUChar (kNvsKeyStaged,    SOTA_STAGED_STARTED) == 1
                         && p.putUInt  (kNvsKeyPrevPart,  running->address) == 4
                         && p.putUInt  (kNvsKeyPrevBuild, prevBuild) == 4
                         && p.putString(kNvsKeyPrevHash,  prevHash) == prevHash.length()
                         && p.putString(kNvsKeyPrevVer,   prevVer) == prevVer.length()
                         && p.putUChar (kNvsKeyStaged,    SOTA_STAGED_SNAPSHOT) == 1;
    bool ok = snapshotOk
           && p.putUInt  (kNvsKeyBuild,       _buildNumber) == 4
           && p.putString(kNvsKeyHash,        newHash) == strlen(newHash)
           && p.putString(kNvsKeyVersion,     newVer) == newVer.length()
           && p.putString(kNvsKeyFailDep,     _deploymentId) == _deploymentId.length()
           && p.putUInt  (kNvsKeyFailBuild,   _buildNumber) == 4
           && p.putUChar (kNvsKeyConfirmPend, 1) == 1
           && p.putUChar (kNvsKeyTrial,       1) == 1;
    p.end();
    SOTA_LOG("apply: trial record %s (prev_part=0x%x deployment=%s)",
             ok ? "saved" : "NOT saved", (unsigned)running->address,
             _deploymentId.c_str());
    if (!ok) abandonTrialRecord(snapshotOk);
    return ok;
}

// A staged record whose slot switch will not happen. restoreIdentity is
// false when the snapshot itself was not saved, so the live identity was
// never touched.
void SimpleOTAClient::abandonTrialRecord(bool restoreIdentity) {
    if (restoreIdentity) restorePreviousIdentity();
    dropTrialRecord();
}

// The boot slot now points at the new image.
void SimpleOTAClient::unstageTrialRecord() {
    Preferences p;
    if (!p.begin(kNvsNamespace, /*readOnly=*/false)) return;
    p.remove(kNvsKeyStaged);
    p.end();
}

// Remove the trial record and the report context that came with it.
void SimpleOTAClient::dropTrialRecord() {
    Preferences p;
    if (!p.begin(kNvsNamespace, /*readOnly=*/false)) return;
    p.remove(kNvsKeyTrial);
    p.remove(kNvsKeyStaged);
    p.remove(kNvsKeyPrevPart);
    p.remove(kNvsKeyPrevBuild);
    p.remove(kNvsKeyPrevHash);
    p.remove(kNvsKeyPrevVer);
    p.remove(kNvsKeyFailDep);
    p.remove(kNvsKeyFailBuild);
    p.remove(kNvsKeyConfirmPend);
    p.remove(kNvsKeyRbReason);
    p.end();
}

// With rollback disabled there is no trial: only the context for the
// "confirmed" report the next boot queues.
void SimpleOTAClient::saveConfirmContext() {
    Preferences p;
    if (!p.begin(kNvsNamespace, /*readOnly=*/false)) return;
    p.putString(kNvsKeyFailDep,     _deploymentId.c_str());
    p.putUInt  (kNvsKeyFailBuild,   _buildNumber);
    p.putUChar (kNvsKeyConfirmPend, 1);
    p.end();
}

// Back on the previous image: report the identity it actually has.
void SimpleOTAClient::restorePreviousIdentity() {
    Preferences p;
    if (!p.begin(kNvsNamespace, /*readOnly=*/false)) return;
    const uint32_t build = p.getUInt  (kNvsKeyPrevBuild, 0);
    const String   hash  = p.getString(kNvsKeyPrevHash,  "");
    const String   ver   = p.getString(kNvsKeyPrevVer,   "");
    // Zero and empty values are valid: the state before the first update.
    p.putUInt  (kNvsKeyBuild,   build);
    p.putString(kNvsKeyHash,    hash);
    p.putString(kNvsKeyVersion, ver);
    p.end();
}

// Once per instance. sotaClassifyBoot() decides; this carries it out.
void SimpleOTAClient::processBootValidation() {
    if (_bootValidated) return;
    _bootValidated = true;
    loadOutbox();

    if (s_updateWaiting) {
        SOTA_LOG("bootval: an update flashed this boot is waiting for its reboot");
        return;
    }

    Preferences p;
    if (!p.begin(kNvsNamespace, /*readOnly=*/true)) {
        // The namespace does not exist before the first update.
        SOTA_LOG("bootval: no saved OTA state yet");
        return;
    }
    const bool     confirmPend = p.getUChar (kNvsKeyConfirmPend, 0) != 0;
    const String   dep         = p.getString(kNvsKeyFailDep, "");
    const uint32_t build       = p.getUInt  (kNvsKeyFailBuild, 0);
    const String   rbReason    = p.getString(kNvsKeyRbReason, "");
    p.end();

    SotaBootFacts facts;
    sotaReadBootFacts(&facts);
    const SotaBootOutcome outcome = sotaClassifyBoot(&facts);
    if (outcome != SOTA_BOOT_TRIAL) sota_wdt_disarm();

    switch (outcome) {
    case SOTA_BOOT_NONE:
        // With rollback disabled, the last apply() left only the confirm context.
        if (confirmPend) {
            queueReport("confirmed", nullptr, dep.c_str(), build);
            dropTrialRecord();
        }
        return;
    case SOTA_BOOT_REPORT_PENDING:
        SOTA_LOG("bootval: a previous boot rolled back; queuing the report");
        queueReport("rolled_back", rbReason.length() ? rbReason.c_str() : "confirm_timeout",
                    dep.c_str(), build);
        dropTrialRecord();
        return;
    case SOTA_BOOT_ROLLED_BACK:
        // The bootloader switched back: the reset that ended the trial says why.
        SOTA_LOG("bootval: back on the previous image; queuing the report");
        restorePreviousIdentity();
        queueReport("rolled_back", sotaRollbackReason(esp_reset_reason()),
                    dep.c_str(), build);
        dropTrialRecord();
        return;
    case SOTA_BOOT_WAITING:
        SOTA_LOG("bootval: the flashed update is waiting for its reboot");
        s_updateWaiting = true;
        return;
    case SOTA_BOOT_ABANDONED:
        // The record went in with the new identity before a slot switch
        // that never happened.
        SOTA_LOG("bootval: update cut short before its slot switch; dropping it");
        abandonTrialRecord(sotaAbandonRestoresIdentity(facts.staged));
        return;
    case SOTA_BOOT_INCOMPLETE:
        SOTA_LOG("bootval: dropping a trial record that cannot be checked");
        dropTrialRecord();
        return;
    case SOTA_BOOT_SLEPT_IN_TRIAL:
        if (_rollbackEnabled) {
            SOTA_LOG("bootval: woke from deep sleep before confirming; rolling back");
            performRollback("deep_sleep");
            return;
        }
        startTrial(facts.staged != 0, dep.c_str(), build);  // accepts it
        return;
    case SOTA_BOOT_TRIAL:
        startTrial(facts.staged != 0, dep.c_str(), build);
        return;
    }
}

// First boot of the new image: supervise it, or accept it with rollback
// disabled. deploymentId and build are the report context of the update.
void SimpleOTAClient::startTrial(bool staged, const char* deploymentId, uint32_t build) {
    // Power was lost after the slot switch, before sota_staged was removed.
    if (staged) unstageTrialRecord();

    if (!_rollbackEnabled) {
        SOTA_LOG("bootval: rollback disabled; accepting the new image");
        sota_wdt_disarm();
        sotaMarkRunningValid();
        queueReport("confirmed", nullptr, deploymentId, build);
        dropTrialRecord();
        return;
    }

    _inTrial = true;
    armTrialWatchdog();
    _confirmTimer = xTimerCreate("sota_confirm",
                                 sotaSecondsToTicks(_confirmTimeoutSec, configTICK_RATE_HZ),
                                 pdFALSE,   // one-shot
                                 this,      // timer ID = instance pointer
                                 &SimpleOTAClient::_confirmTimerCb);
    if (_confirmTimer == nullptr) {
        SOTA_LOG("bootval: timer alloc failed; rolling back immediately");
        performRollback("confirm_timeout");
        return;
    }
    if (xTimerStart(_confirmTimer, 0) != pdPASS) {
        SOTA_LOG("bootval: timer start failed; rolling back immediately");
        xTimerDelete(_confirmTimer, 0);
        _confirmTimer = nullptr;
        performRollback("confirm_timeout");
        return;
    }
    SOTA_LOG("bootval: TRIAL armed (timeout=%us)", (unsigned)_confirmTimeoutSec);
}

// confirmRunning() and the confirm timer can run at the same time on
// different tasks. Only the first to get here ends the trial. When the timer
// loses to a confirm that is still in progress, it leaves _timeoutMissed for
// confirmRunning() to act on if the confirm fails.
bool SimpleOTAClient::claimTrialEnd(bool fromTimer) {
    portENTER_CRITICAL(&_trialMux);
    const bool claimed = _inTrial && !_trialEnding;
    if (claimed) {
        _trialEnding = true;
    } else if (fromTimer && _inTrial) {
        _timeoutMissed = true;
    }
    portEXIT_CRITICAL(&_trialMux);
    return claimed;
}

// FreeRTOS one-shot timer callback. Runs in the timer service task context.
// Safe to call performRollback() here: it does NVS writes + esp_restart().
void SimpleOTAClient::_confirmTimerCb(TimerHandle_t xTimer) {
    SimpleOTAClient* self =
        static_cast<SimpleOTAClient*>(pvTimerGetTimerID(xTimer));
    if (!self || !self->claimTrialEnd(/*fromTimer=*/true)) return;
    SOTA_LOG("confirm timer fired; rolling back");
    self->performRollback("confirm_timeout");
}

// The app partition at a flash address, or nullptr.
static const esp_partition_t* sotaFindAppPartition(uint32_t address) {
    if (address == 0) return nullptr;
    esp_partition_iterator_t it =
        esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, nullptr);
    const esp_partition_t* match = nullptr;
    for (; it != nullptr; it = esp_partition_next(it)) {
        const esp_partition_t* part = esp_partition_get(it);
        if (part->address == address) {
            match = part;
            break;
        }
    }
    esp_partition_iterator_release(it);
    return match;
}

// Switch back to the previous image, put its identity back, and set
// sota_trial to 2 with the reason so the next boot reports the rollback,
// then restart. A
// step that fails is logged and the restart still happens: a broken image
// left running as an endless trial would be worse than a partial restore.
// No delay(): this can run in the timer service task, which must not block.
void SimpleOTAClient::performRollback(const char* reason) {
    Preferences p;
    uint32_t prevPart = 0;
    if (p.begin(kNvsNamespace, /*readOnly=*/true)) {
        prevPart = p.getUInt(kNvsKeyPrevPart, 0);
        p.end();
    }
    const esp_partition_t* prev = sotaFindAppPartition(prevPart);
    if (!prev) {
        SOTA_LOG("rollback: no app partition at prev_part=0x%x", (unsigned)prevPart);
    } else {
        const esp_err_t err = esp_ota_set_boot_partition(prev);
        SOTA_LOG("rollback: boot partition 0x%x (err=0x%x)",
                 (unsigned)prev->address, (unsigned)err);
    }

    restorePreviousIdentity();
    if (p.begin(kNvsNamespace, /*readOnly=*/false)) {
        p.putString(kNvsKeyRbReason, reason);
        p.putUChar(kNvsKeyTrial, 2);  // fail_dep / fail_build stay for the report
        p.end();
    }
    SOTA_LOG("rollback: rebooting");
    esp_restart();
}

bool SimpleOTAClient::confirmRunning() {
    // Ensure boot validation has run regardless of whether begin()/check()
    // has been called yet. This makes confirmRunning() safe to call from
    // setup() in polling mode, before the first check() invocation.
    processBootValidation();
    if (!claimTrialEnd(/*fromTimer=*/false)) return false;
    // Tell the bootloader first. If it cannot record the image as good, the
    // trial goes on, timer included, and the caller can try again. A timeout
    // that fired meanwhile still rolls back.
    if (!sotaMarkRunningValid()) {
        portENTER_CRITICAL(&_trialMux);
        _trialEnding = false;
        const bool missed = _timeoutMissed;
        _timeoutMissed = false;
        portEXIT_CRITICAL(&_trialMux);
        if (missed) {
            SOTA_LOG("confirmRunning: could not confirm, and the trial timed out; rolling back");
            performRollback("confirm_timeout");
        }
        return false;
    }
    sota_wdt_disarm();
    portENTER_CRITICAL(&_trialMux);
    _inTrial = false;
    _trialEnding = false;
    _timeoutMissed = false;
    portEXIT_CRITICAL(&_trialMux);
    if (_confirmTimer) {
        xTimerStop(_confirmTimer, 0);
        xTimerDelete(_confirmTimer, 0);
        _confirmTimer = nullptr;
    }
    Preferences p;
    if (p.begin(kNvsNamespace, /*readOnly=*/true)) {
        const String   dep   = p.getString(kNvsKeyFailDep, "");
        const uint32_t build = p.getUInt  (kNvsKeyFailBuild, 0);
        p.end();
        queueReport("confirmed", nullptr, dep.c_str(), build);
    }
    dropTrialRecord();
    SOTA_LOG("confirmRunning: trial confirmed");
    return true;
}

namespace {
class LockGuard {
public:
    explicit LockGuard(SemaphoreHandle_t m) : _m(m) { xSemaphoreTake(_m, portMAX_DELAY); }
    ~LockGuard() { xSemaphoreGive(_m); }
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;
private:
    SemaphoreHandle_t _m;
};
}  // namespace

void SimpleOTAClient::loadOutbox() {
    LockGuard lock(_outboxLock);
    sotaOutboxInit(&_outbox);
    Preferences p;
    if (!p.begin(kNvsNamespace, /*readOnly=*/true)) return;
    SotaOutbox stored;
    if (p.getBytes(kNvsKeyOutbox, &stored, sizeof(stored)) == sizeof(stored) &&
        sotaOutboxValid(&stored)) {
        _outbox = stored;
    }
    p.end();
}

// Callers hold _outboxLock.
void SimpleOTAClient::saveOutbox() {
    Preferences p;
    if (!p.begin(kNvsNamespace, /*readOnly=*/false)) return;
    if (_outbox.count == 0) {
        p.remove(kNvsKeyOutbox);
    } else {
        p.putBytes(kNvsKeyOutbox, &_outbox, sizeof(_outbox));
    }
    p.end();
}

void SimpleOTAClient::queueReport(const char* event, const char* reason,
                                  const char* deploymentId, uint32_t build) {
    LockGuard lock(_outboxLock);
    if (!sotaOutboxPush(&_outbox, event, reason, deploymentId, build)) {
        SOTA_LOG("report: %s has no usable deployment context; not queued", event);
        return;
    }
    saveOutbox();
    SOTA_LOG("report: queued %s (deployment=%s build=%u)",
             event, deploymentId, (unsigned)build);
}

// Send queued reports, oldest first. Each is done once the server returns
// any HTTP response. Without one, it stays queued for the next check().
// The lock is not held while posting: the report is copied out, and popped
// only if a report queued meanwhile has not pushed it out already.
void SimpleOTAClient::sendPendingReports() {
    for (;;) {
        SotaReport r;
        {
            LockGuard lock(_outboxLock);
            if (_outbox.count == 0) return;
            r = _outbox.reports[0];
        }
        const int code = postStatus(r.event, r.reason[0] ? r.reason : nullptr,
                                    r.deployment, r.build, nullptr);
        if (!sotaReportDelivered(code)) {
            SOTA_LOG("report: %s not delivered; will retry", r.event);
            return;
        }
        LockGuard lock(_outboxLock);
        if (_outbox.count > 0 && memcmp(&_outbox.reports[0], &r, sizeof(r)) == 0) {
            sotaOutboxPop(&_outbox);
            saveOutbox();
        }
    }
}
