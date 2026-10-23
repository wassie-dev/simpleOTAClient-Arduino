// SimpleOTAClient Arduino client library: the public API over the shared
// core (src/sota_core, vendored from SimpleOTAClient-core). The protocol,
// the trial record in NVS and the rollback rules are the core's, so they
// match the ESP-IDF client. HTTP is in SimpleOTANet.cpp.

#include "SimpleOTAClient.h"

#include <esp_mac.h>
#include <esp_system.h>
#include <nvs_flash.h>

#include "sdkconfig.h"
#include "sota_core/sota_app.h"
#include "sota_core/sota_sys.h"
#include "sota_core/sota_time.h"
#include "sota_core/sota_url.h"
#include "sota_core/sota_wdt_calc.h"

// Bootloader rollback: the core can leave a new image pending verify, so a
// crash or reset before confirmRunning() returns to the previous image.
// -DSIMPLEOTA_DISABLE_BOOTLOADER_ROLLBACK leaves the rollback to the library.
#if (defined(CONFIG_APP_ROLLBACK_ENABLE) || defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE)) \
    && !defined(SIMPLEOTA_DISABLE_BOOTLOADER_ROLLBACK)
#define SOTA_BOOTLOADER_ROLLBACK 1
#else
#define SOTA_BOOTLOADER_ROLLBACK 0
#endif

static_assert(OTA_SUCCESS == (int)SOTA_APPLY_OK &&
              OTA_CHECKSUM_FAIL == (int)SOTA_APPLY_CHECKSUM_FAIL &&
              OTA_FLASH_FAIL == (int)SOTA_APPLY_FLASH_FAIL &&
              OTA_NO_OFFER == (int)SOTA_APPLY_NO_OFFER &&
              OTA_SIGNATURE_FAIL == (int)SOTA_APPLY_SIGNATURE_FAIL &&
              OTA_REBOOT_PENDING == (int)SOTA_APPLY_REBOOT_PENDING &&
              OTA_UNCONFIRMED == (int)SOTA_APPLY_UNCONFIRMED,
              "OTAResult matches sota_apply_result_t");

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

// Tolerate `#define SIMPLEOTA_DEBUG` (no value) by treating any defined-and-
// truthy macro as enabled. The compile-time flag sets the default; setDebug()
// can flip it at runtime.
#if defined(SIMPLEOTA_DEBUG) && (SIMPLEOTA_DEBUG + 0)
bool SimpleOTAClient::_debugEnabled = true;
#else
bool SimpleOTAClient::_debugEnabled = false;
#endif

// Warnings and errors always print: they flag something to fix, such as a
// rejected token or a signed offer applied unverified. The rest needs
// setDebug(true).
void sota_app_log(int level, const char* fmt, ...) {
    if (level > SOTA_LOG_WARN && !SimpleOTAClient::_debugEnabled) return;
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    Serial.printf("[SimpleOTAClient] %s%s\n", level <= SOTA_LOG_WARN ? "WARNING: " : "", line);
}

// ---------------------------------------------------------------------------
// The core's application interface
// ---------------------------------------------------------------------------

// Events are not part of this API; check() and apply() return what they mean.
void sota_app_event(void*, const sota_event_t*) {}

// The confirm timeout ran out, in the esp_timer task. The rollback writes
// NVS and restarts, so it runs on a task: the managed one, or a short-lived
// one in polling mode.
static void sotaServiceTask(void* arg) {
    sota_engine_service(static_cast<sota_engine_t*>(arg));
    vTaskDelete(nullptr);
}

void sota_app_wake(void* user) {
    SimpleOTAClient* self = static_cast<SimpleOTAClient*>(user);
    self->wake();
}

// ---------------------------------------------------------------------------
// Bundled SimpleOTAClient root CA (ISRG Root X1 / Let's Encrypt). Public PEM,
// distributed widely. Used by default for all HTTPS connections; both the
// SimpleOTA API host and the firmware storage host chain to this root.
// ---------------------------------------------------------------------------

const char* SimpleOTAClient::kSimpleOtaRootCA =
    "-----BEGIN CERTIFICATE-----\n"
    "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\n"
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
// Construction / configuration. The engine borrows every string.
// ---------------------------------------------------------------------------

SimpleOTAClient::SimpleOTAClient(const char* token,
                     const char* chipFamily,
                     const char* deviceId,
                     const char* boardId,
                     const char* hardwareRevision)
    : _engine(),
      _engineOk(false),
      _deviceIdBuf{},
      _caCert(SimpleOTAClient::kSimpleOtaRootCA),
      _insecureWarned(false),
      _managedAutoConfirm(true),
      _download(nullptr),
      _onResult(nullptr),
      _isConnected(nullptr),
      _initialDelayMs(0),
      _taskHandle(nullptr) {
    if (!deviceId) {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(_deviceIdBuf, sizeof(_deviceIdBuf),
                 "%02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        deviceId = _deviceIdBuf;
    }
    sota_config_t cfg = {};
    cfg.token = token;
    cfg.device.device_id = deviceId;
    cfg.device.framework = "arduino";
    cfg.device.chip_family = chipFamily;
    cfg.device.board_id = boardId;
    cfg.device.hardware_revision = hardwareRevision;
    cfg.device.nvs_schema_version = 1;
    cfg.check_interval_s = SIMPLEOTA_CHECK_INTERVAL_S;
    cfg.confirm_timeout_s = SIMPLEOTA_CONFIRM_TIMEOUT_S;
    cfg.http_timeout_ms = SIMPLEOTA_TIMEOUT_MS;
    cfg.auto_reboot = true;
    cfg.rollback = true;
    cfg.user = this;
    _engineOk = sota_engine_init(&_engine, &cfg);
}

bool SimpleOTAClient::setServerUrl(const char* url) {
    const sota_url_status_t status = sota_validate_server_url(url);
    if (status != SOTA_URL_OK) {
        Serial.printf("[SimpleOTAClient] setServerUrl rejected (%s). Keeping %s\n",
                      sota_url_status_name(status), _engine.cfg.server_url);
        return false;
    }
    _engine.cfg.server_url = url;
    return true;
}

void SimpleOTAClient::setCACert(const char* pemRootCA)        { _caCert = pemRootCA; }
void SimpleOTAClient::setAutoReboot(bool enabled)             { _engine.cfg.auto_reboot = enabled; }
void SimpleOTAClient::setPartitionProfile(const char* profile){ _engine.cfg.device.partition_profile = profile; }
void SimpleOTAClient::setNvsSchemaVersion(uint8_t version)    { _engine.cfg.device.nvs_schema_version = version; }
void SimpleOTAClient::setLabels(const char* jsonObject)       { _engine.cfg.device.labels_json = jsonObject; }
void SimpleOTAClient::setSecurityMode(const char* mode)       { _engine.cfg.device.security_mode = mode; }

// Parsed into a temporary first, so a malformed PEM never wipes a pinned
// key: failing open would downgrade the device to checksum-only flashing.
bool SimpleOTAClient::setSigningPublicKey(const char* pem) {
    uint8_t key[32];
    if (!sota_parse_ed25519_public_key_pem(pem, key)) {
        SOTA_LOGW("signing: rejected public key (not a valid Ed25519 SPKI PEM)");
        return false;
    }
    _engine.num_keys = 0;
    return sota_engine_add_key(&_engine, nullptr, pem);
}

bool SimpleOTAClient::addSigningPublicKey(const char* keyId, const char* pem) {
    if (!sota_engine_add_key(&_engine, keyId, pem)) {
        SOTA_LOGW("signing: key not added (invalid PEM, or %u keys pinned already)",
                  (unsigned)SOTA_VERIFIER_MAX_KEYS);
        return false;
    }
    return true;
}

void SimpleOTAClient::setVersionLabel(const char* label)      { _engine.cfg.version_label = label; }
void SimpleOTAClient::setChannel(const char* channel)         { _engine.cfg.device.channel = channel; }
void SimpleOTAClient::setDebug(bool enabled)                  { _debugEnabled = enabled; }
void SimpleOTAClient::setRollbackEnabled(bool enabled)        { _engine.cfg.rollback = enabled; }

void SimpleOTAClient::setManagedAutoConfirm(bool enabled) {
    _managedAutoConfirm = enabled;
    if (_taskHandle) _engine.cfg.auto_confirm = enabled;
}

// Below 10 s leaves no time to debug a trial on the serial console; above
// 24 h is a bug, not a configuration.
void SimpleOTAClient::setConfirmTimeout(uint32_t seconds) {
    if (seconds < 10)    seconds = 10;
    if (seconds > 86400) seconds = 86400;
    _engine.cfg.confirm_timeout_s = seconds;
}

const char* SimpleOTAClient::lastOfferedVersion() const { return _engine.offer.version; }

// ---------------------------------------------------------------------------
// Polling API
// ---------------------------------------------------------------------------

bool SimpleOTAClient::check() {
    if (!_engineOk) {
        SOTA_LOGW("check: the token has control characters, or the server URL is unusable");
        return false;
    }
    return sota_engine_check(&_engine);
}

OTAResult SimpleOTAClient::apply() {
    return static_cast<OTAResult>(sota_engine_apply(&_engine));
}

bool SimpleOTAClient::report(const char* event, const char* reason) {
    const int status = sota_engine_report(&_engine, event, reason);
    return status >= 200 && status < 300;
}

// The event names the waiting update's deployment, kept in NVS by apply().
void SimpleOTAClient::rebootForUpdate() {
    sota_engine_reboot_for_update(&_engine);
    SOTA_LOGI("rebootForUpdate: no update is waiting; rebooting without a status post");
    esp_restart();
}

bool SimpleOTAClient::confirmRunning() {
    sota_engine_validate_boot(&_engine);
    return sota_engine_confirm(&_engine) == SOTA_CONFIRM_DONE;
}

bool SimpleOTAClient::isTrialInstall() {
    sota_engine_validate_boot(&_engine);
    return _engine.in_trial;
}

// ---------------------------------------------------------------------------
// Managed mode
// ---------------------------------------------------------------------------

void SimpleOTAClient::begin(uint32_t checkIntervalSec,
                      void (*onResult)(OTAResult),
                      bool (*isConnected)(),
                      uint32_t initialDelayMs) {
    if (_taskHandle != nullptr) return;
    _engine.cfg.check_interval_s = checkIntervalSec ? checkIntervalSec : SIMPLEOTA_CHECK_INTERVAL_S;
    _engine.cfg.auto_confirm = _managedAutoConfirm;
    _initialDelayMs = initialDelayMs;
    _onResult = onResult;
    _isConnected = isConnected;
    // On the caller's task, so the confirm timer runs before begin() returns.
    sota_engine_validate_boot(&_engine);
    xTaskCreate(_taskEntry, "simpleota", 8192, this, 1, &_taskHandle);
}

void SimpleOTAClient::end() {
    if (_taskHandle == nullptr) return;
    vTaskDelete(_taskHandle);
    _taskHandle = nullptr;
    _engine.cfg.auto_confirm = false;
}

void SimpleOTAClient::_taskEntry(void* arg) {
    static_cast<SimpleOTAClient*>(arg)->_taskLoop();
}

void SimpleOTAClient::wake() {
    if (_taskHandle) {
        xTaskNotifyGive(_taskHandle);
    } else if (xTaskCreate(sotaServiceTask, "sota_rollback", 4096, &_engine, 5, nullptr) != pdPASS) {
        SOTA_LOGE("no memory for the rollback task; restarting into the trial again");
        esp_restart();
    }
}

void SimpleOTAClient::_taskLoop() {
    // The initial delay keeps clear of other TLS sessions starting at boot
    // (lwIP contention), unless the confirm timeout is already due.
    if (_initialDelayMs > 0 && !_engine.timeout_pending) {
        ulTaskNotifyTake(pdTRUE, sota_ms_to_ticks(_initialDelayMs, configTICK_RATE_HZ));
    }
    for (;;) {
        const bool connected = !_isConnected || _isConnected();
        sota_apply_result_t r = SOTA_APPLY_NO_OFFER;  // step() sets it when apply() ran
        const uint32_t waitSec = sota_engine_step(&_engine, connected, &r);
        if (r != SOTA_APPLY_NO_OFFER && _onResult) _onResult(static_cast<OTAResult>(r));
        ulTaskNotifyTake(pdTRUE, sota_ms_to_ticks((uint64_t)waitSec * 1000u, configTICK_RATE_HZ));
    }
}

// ---------------------------------------------------------------------------
// Bootloader hook
// ---------------------------------------------------------------------------

#if SOTA_BOOTLOADER_ROLLBACK
// initArduino() accepts a pending image before setup() unless this returns
// true. Only a SimpleOTA trial is held; any other image, such as one written
// by ArduinoOTA, is accepted as usual. This runs before the core's own
// nvs_flash_init(): initialise here, never erase. Serial is not up yet, so
// nothing is logged. Kept in this file so a PlatformIO archive links it in
// with the class and it replaces the core's weak definition.
extern "C" bool verifyRollbackLater() {
    if (nvs_flash_init() != ESP_OK) return false;
    const bool hold = sota_engine_hold_pending_image();
    // A held trial gets the chip watchdog straight away, so a hang before
    // the sketch reaches the library is reset too. setConfirmTimeout() is
    // not known yet: the compile-time default applies until boot validation.
    if (hold) {
        sota_sys_wdt_arm(SIMPLEOTA_CONFIRM_TIMEOUT_S, sota_wdt_floor_sec(SIMPLEOTA_TIMEOUT_MS));
    } else {
        sota_sys_wdt_quiesce();
    }
    return hold;
}
#endif
