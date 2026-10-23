/**
 * @file SimpleOTAClient.h
 * @brief SimpleOTAClient Arduino client library for ESP32 (Arduino-ESP32 core).
 *
 * See README.md for the build-number / NVS rule. It is load-bearing.
 */

#ifndef SIMPLEOTACLIENT_H
#define SIMPLEOTACLIENT_H

#include <Arduino.h>
#include <stdint.h>

#include "sota_core/sota_app.h"
#include "sota_core/sota_engine.h"
#include "sota_core/sota_net.h"
#include "sota_core/sota_runner.h"
#include "sota_core/sota_sys.h"

// ---------------------------------------------------------------------------
// Compile-time configuration. Override with -D or a #define before #include.
// ---------------------------------------------------------------------------

#ifndef SIMPLEOTA_CHECK_INTERVAL_S
/**
 * @brief Default check interval in seconds (informational only).
 *
 * The library does not schedule checks internally; this constant is exposed
 * for examples and user code that polls in loop().
 */
#define SIMPLEOTA_CHECK_INTERVAL_S 3600
#endif

#ifndef SIMPLEOTA_HTTP_TIMEOUT_MS
#ifdef SIMPLEOTA_TIMEOUT_MS
// The name before 1.0.0.
#define SIMPLEOTA_HTTP_TIMEOUT_MS SIMPLEOTA_TIMEOUT_MS
#else
/**
 * @brief HTTP timeout in milliseconds, as CONFIG_SIMPLEOTA_HTTP_TIMEOUT_MS on
 *        ESP-IDF.
 */
#define SIMPLEOTA_HTTP_TIMEOUT_MS 15000
#endif
#endif

#ifndef SIMPLEOTA_DEBUG
/**
 * @brief Verbose logging switch. 0 = silent (default); 1 = verbose Serial
 *        output with a [SimpleOTAClient] prefix.
 */
#define SIMPLEOTA_DEBUG 0
#endif

#ifndef SIMPLEOTA_CONFIRM_TIMEOUT_S
/**
 * @brief Default trial-install confirm deadline in seconds.
 *
 * After a successful apply(), the new firmware must call confirm()
 * within this many seconds, or the library reboots into the previous
 * partition. See setConfirmTimeout().
 */
#define SIMPLEOTA_CONFIRM_TIMEOUT_S 300
#endif

/**
 * @brief check() retry interval in seconds during a TRIAL boot (managed mode).
 *
 * Instead of the normal checkIntervalSec, the managed task retries on this
 * shorter interval until a successful server response is received or the
 * confirm timeout expires. Fixed by the shared core.
 */
#define SIMPLEOTA_TRIAL_RETRY_INTERVAL_S SOTA_TRIAL_RETRY_INTERVAL_S

// ---------------------------------------------------------------------------
// Public types
// ---------------------------------------------------------------------------

/**
 * @brief Result codes returned by apply().
 */
enum OTAResult {
    OTA_SUCCESS        = 0,  ///< Firmware flashed and validated; reboot pending.
    OTA_CHECKSUM_FAIL  = 1,  ///< Downloaded payload did not match expected SHA-256.
    OTA_FLASH_FAIL     = 2,  ///< Network or partition write failure.
    OTA_NO_OFFER       = 3,  ///< apply() called without a successful preceding check().
    OTA_SIGNATURE_FAIL = 4,  ///< Ed25519 signature verification failed; image rejected before it was marked bootable.
    OTA_REBOOT_PENDING = 5,  ///< An update is flashed and waits for rebootForUpdate() (setAutoReboot(false)). Nothing was downloaded.
    OTA_UNCONFIRMED    = 6   ///< The running image is on trial. Call confirm(), then apply() again. The offer is kept.
};

/**
 * @brief Lifecycle events passed to onEvent(), in the order of the ESP-IDF
 *        client's simpleota_event_id_t.
 */
enum OTAEvent {
    OTA_EVENT_CHECK_DONE,         ///< A check completed; no update offered.
    OTA_EVENT_UPDATE_AVAILABLE,   ///< An update was offered.
    OTA_EVENT_DOWNLOAD_PROGRESS,  ///< bytesDone / bytesTotal advanced.
    OTA_EVENT_UPDATE_FAILED,      ///< The update was dropped; reason says why.
    OTA_EVENT_BEFORE_REBOOT,      ///< Flashed and verified; the reboot comes next.
    OTA_EVENT_TRIAL_BOOT,         ///< Running an image that is not confirmed yet.
    OTA_EVENT_CONFIRMED,          ///< The trial image's confirmed report was delivered.
    OTA_EVENT_ROLLING_BACK        ///< The trial failed; the previous image starts next.
};

/** @brief One event. Its strings live only for the callback. */
struct OTAEventInfo {
    OTAEvent    id;
    const char* reason;       ///< A reason token or server status, or nullptr.
    uint32_t    buildNumber;  ///< The build the event is about, 0 when none.
    size_t      bytesDone;    ///< OTA_EVENT_DOWNLOAD_PROGRESS: bytes written.
    size_t      bytesTotal;   ///< OTA_EVENT_DOWNLOAD_PROGRESS: bytes expected, 0 when unknown.
};

/**
 * @class SimpleOTAClient
 * @brief Over-the-air firmware update client for ESP32.
 *
 * Provides check/apply firmware update mechanics, optional lifecycle event
 * reporting, a trial-install/rollback subsystem, and on-device Ed25519
 * firmware signature verification for signed artifacts.
 *
 * A device runs one instance: the trial record and the boot slot are the
 * device's. A second instance, while the first exists, does nothing, and its
 * check() prints why.
 */
class SimpleOTAClient {
public:

    /**
     * @brief Construct a SimpleOTAClient.
     *
     * @param token             Project or device token, sent as
     *                          "Authorization: Bearer <token>".
     * @param chipFamily        Chip family string, e.g. "esp32" or "esp32s3".
     *                          nullptr (default) = the chip this sketch is
     *                          built for, as on ESP-IDF.
     * @param deviceId          Stable per-unit identifier. Pass nullptr (default)
     *                          to auto-derive from the Wi-Fi MAC address
     *                          ("aa:bb:cc:dd:ee:ff"). Supply your own string for
     *                          a different identifier scheme.
     * @param boardId           Optional board identifier. nullptr = field omitted.
     * @param hardwareRevision  Optional hardware revision. nullptr = field omitted.
     */
    SimpleOTAClient(const char* token,
                    const char* chipFamily       = nullptr,
                    const char* deviceId         = nullptr,
                    const char* boardId          = nullptr,
                    const char* hardwareRevision = nullptr);

    /// Stops the managed task and lets another instance run.
    ~SimpleOTAClient();

    // The core keeps a pointer to this instance, so it cannot be copied.
    SimpleOTAClient(const SimpleOTAClient&) = delete;
    SimpleOTAClient& operator=(const SimpleOTAClient&) = delete;

    /**
     * @brief Poll /api/v1/ota/check/ for a firmware update.
     *
     * Stores the offer (url, checksum, size, deployment_id, build_number) on
     * the instance when one is available.
     *
     * Polling mode only: false, with a warning, while begin()'s task runs.
     *
     * @return true if an update is offered; false on no-update or transport failure.
     */
    bool check();

    /**
     * @brief Execute the pending offer from the most recent successful check().
     *
     * Reports lifecycle events to /api/v1/ota/status/. On success, persists the
     * new build number in NVS and (by default) calls esp_restart().
     *
     * Polling mode only: OTA_NO_OFFER, with a warning, while begin()'s task
     * runs.
     *
     * @return An OTAResult code. On OTA_SUCCESS with auto-reboot enabled
     *         (default), this function does not return.
     */
    OTAResult apply();

    /**
     * @brief Override the CA certificate (PEM) used for HTTPS verification.
     *
     * By default the library uses the bundled kSimpleOtaRootCA (ISRG Root X1),
     * which covers both the SimpleOTA API and its firmware storage. Call this
     * only if you need to pin a different certificate.
     *
     * TLS is always verified. For a local server, give it a certificate from
     * your own CA and pass that CA here.
     *
     * @param pemRootCA  PEM-encoded root CA certificate. nullptr is ignored,
     *                   with a warning.
     */
    void setCACert(const char* pemRootCA);

    /**
     * @brief Point check() and status posts at another SimpleOTA server.
     *
     * Call before begin(), or after end(): while the managed task runs it
     * returns false with a warning. The pointer is stored, not copied, and
     * must outlive this object. Until a URL is accepted the base is https://simpleota.com.
     *
     * https is accepted in any case. http and any other value return false,
     * print a [SimpleOTAClient] line (not_https or malformed), and leave the
     * previous URL in place. Downloads still use the pre-signed URL. TLS
     * verification is unchanged; use setCACert() for a different CA.
     *
     * @param url  Server base.
     * @return true when the URL is accepted.
     */
    bool setServerUrl(const char* url);

    /**
     * @brief Control whether apply() automatically reboots after a successful flash.
     *
     * When true (default), apply() calls esp_restart() after a successful flash.
     * When false, apply() returns OTA_SUCCESS and the caller is responsible for
     * rebooting at a convenient time. Until that reboot, apply() returns
     * OTA_REBOOT_PENDING without downloading, because the next update slot
     * holds the waiting image. The "validated" status event is reported
     * before reboot in either case.
     *
     * @param enabled  true = auto-reboot (default); false = caller-managed reboot.
     */
    void setAutoReboot(bool enabled);

    /**
     * @brief Emit a "reboot" status event and restart the device.
     *
     * Convenience helper for applications using setAutoReboot(false). Posts
     * `event="reboot"` to /api/v1/ota/status/ for the update that is waiting
     * (best-effort; the function does not block on network failure beyond the
     * normal HTTPClient timeout), then calls esp_restart(). Does not return.
     *
     * The event names the waiting update's deployment even after later
     * check() calls, or after end() and begin(). With no update waiting
     * nothing is posted; the device reboots either way.
     */
    void rebootForUpdate();

    /**
     * @brief Set the partition profile reported to the server.
     *
     * Required when your SimpleOTA artifacts are tagged with a partition_profile;
     * the server uses it to filter hardware-compatible builds.
     * Example values: "default_4mb", "minimal_spiffs_4mb".
     *
     * @param profile  Partition profile string. nullptr (default) = field omitted.
     */
    void setPartitionProfile(const char* profile);

    /**
     * @brief Set the NVS schema version reported to the server.
     *
     * Increment when you restructure your own NVS namespace layout so the server
     * can gate builds that require the new schema. The server assumes version 1
     * when the field is absent; this library always sends it explicitly.
     *
     * @param version  Schema version number (default: 1).
     */
    void setNvsSchemaVersion(uint8_t version);

    /**
     * @brief Set arbitrary key/value metadata for server-side deployment targeting.
     *
     * @param jsonObject  A valid JSON object literal, e.g. "{\"site\":\"factory-A\"}".
     *                    The string is embedded verbatim in the request body; no
     *                    escaping is performed. nullptr (default) = field omitted.
     */
    void setLabels(const char* jsonObject);

    /**
     * @brief Set the security mode advertised to the server.
     *
     * Used for server-side compatibility filtering.
     *
     * @param mode  Security mode string, e.g. "basic". Use the SECURITY_MODE_xxx
     *              constants for known values. nullptr (default) = field omitted.
     */
    void setSecurityMode(const char* mode);

    /**
     * @brief Pin the project's Ed25519 public signing key (replaces any
     *        previously pinned keys).
     *
     * When the server offers a build with security_mode "signed", the
     * downloaded image's Ed25519 signature is verified against the pinned
     * key(s) BEFORE the image is marked bootable. On mismatch the update is
     * aborted, a "failed" event with reason "signature_invalid" is reported,
     * and apply() returns OTA_SIGNATURE_FAIL; the device keeps running its
     * current firmware.
     *
     * The public key is not a secret: compile it into the sketch. Get it
     * from the project's "Firmware signing keys" section in the dashboard,
     * or from GET /api/v1/projects/{id}/signing-keys/.
     *
     * Once a key is pinned AND setSecurityMode(SECURITY_MODE_SIGNED) is set,
     * verification is mandatory: a signed offer whose signature is missing or
     * does not verify is rejected, and so is an offer that arrives WITHOUT a
     * signature at all (a server or MITM cannot downgrade the device to
     * checksum-only by stripping the signature fields). If no key is pinned,
     * signed offers are applied checksum-only with a warning on each such
     * apply, which keeps basic-to-signed fleet migration flowing.
     *
     * On an invalid PEM this returns false and leaves any previously pinned
     * key untouched (it does not fail open).
     *
     * @param pem  PEM "BEGIN PUBLIC KEY" (SubjectPublicKeyInfo) Ed25519 key,
     *             exactly as issued by SimpleOTA. Must remain valid only for
     *             the duration of this call (the raw key bytes are copied).
     * @return true if the PEM parsed as an Ed25519 public key.
     */
    bool setSigningPublicKey(const char* pem);

    /**
     * @brief Pin an additional signing key (key rotation support, max 2).
     *
     * During a rotation window, pin both the outgoing and the incoming key;
     * an offer signed with either verifies. When the offer carries a
     * signing_key_id matching one of the given keyIds, only that key is
     * tried; otherwise all pinned keys are tried.
     *
     * @param keyId  The server-side key id (e.g. "prod-2026"); may be nullptr.
     * @param pem    PEM Ed25519 public key (copied; see setSigningPublicKey()).
     * @return true if the key was added; false when 2 keys are already
     *         pinned or the PEM is invalid.
     */
    bool addSigningPublicKey(const char* keyId, const char* pem);

    /**
     * @brief Set a human-readable version label for the currently-running firmware.
     *
     * Sent as version_label and shown on the dashboard under
     * "Reported firmware state > Version label". Display only; not used for
     * compatibility checks.
     *
     * When nullptr (default), the library reads the value persisted in NVS by the
     * last successful apply(), so post-OTA boots report the correct label
     * automatically without any code change in the new firmware image.
     *
     * @param label  Version label string, e.g. "1.4.1". nullptr = read from NVS.
     */
    void setVersionLabel(const char* label);

    /**
     * @brief Set the release channel this device subscribes to.
     *
     * Only applied by the server on first registration or when the device has no
     * channel yet.
     *
     * @param channel  Channel name, e.g. "stable" or "beta".
     *                 nullptr (default) = field omitted.
     */
    void setChannel(const char* channel);

    /**
     * @brief Return the version label from the last check() offer.
     *
     * Valid between a successful check() and the apply() that consumes the offer.
     *
     * @return Offered version label string, e.g. "1.4.2". Returns an empty string
     *         before any offer has been received, or after apply() clears the offer.
     */
    const char* lastOfferedVersion() const;

    /**
     * @brief The build number this device reports, 0 before its first update.
     */
    uint32_t currentBuild() const;

    /**
     * @brief Get lifecycle events: checks, download progress, failures with
     *        their reason, the reboot, and the trial's confirm or rollback.
     *
     * Called on the task that runs the update: the managed task, or the one
     * that calls check(), apply() or confirm(). Keep it quick, and
     * don't call the client from it.
     *
     * @param callback  nullptr to stop.
     */
    void onEvent(void (*callback)(const OTAEventInfo& event));

    // ------------------------------------------------------------------
    // Trial-install / rollback API
    //
    // After a successful apply() that reboots into a new image, the device
    // enters a "trial" state. The new firmware must call confirm()
    // within setConfirmTimeout() seconds, or the library reboots into the
    // previous partition and queues a "rolled_back" report, sent at the start
    // of the next check().
    //
    // In managed mode (begin()), the library auto-calls confirm() on
    // the first HTTP 200 from /check/ unless setManagedAutoConfirm(false)
    // was called. In polling mode (manual check()/apply()), the application
    // is responsible for calling confirm() once it determines the new
    // image is healthy.
    //
    // A crash or reset before confirm(), even in a global constructor,
    // makes the bootloader return to the previous partition, and a hang from
    // setup() onwards is reset by the chip watchdog. Building with
    // SIMPLEOTA_DISABLE_BOOTLOADER_ROLLBACK leaves the rollback to the
    // library, which then misses a crash before its first call.
    // See the README's Rollback and Limitations sections.
    // ------------------------------------------------------------------

    /**
     * @brief Control whether managed mode auto-confirms on a successful /check/ response.
     *
     * Managed mode only. Default: true.
     *
     * When true, the managed task auto-calls confirm() on the first HTTP
     * 200 from /check/ during a TRIAL boot. When false, the application must call
     * confirm() itself (for example after its own
     * health checks complete).
     *
     * @param enabled  true = auto-confirm (default); false = manual confirm required.
     */
    void setManagedAutoConfirm(bool enabled);

    /**
     * @brief Set the trial-install confirm deadline.
     *
     * Default: SIMPLEOTA_CONFIRM_TIMEOUT_S (300 seconds). Clamped to [10, 86400].
     *
     * Must exceed your worst-case boot-to-connect-to-first-server-response time.
     * For cellular/PPP fleets where modem registration can take 60+ seconds,
     * increase this accordingly (e.g. setConfirmTimeout(600)).
     *
     * Call this before begin() in managed mode, or before the first check() in
     * polling mode, on the trial boot. Changing it after the timer is armed has
     * no effect on the current trial.
     *
     * @param seconds  Confirm deadline in seconds. Clamped to [10, 86400].
     */
    void setConfirmTimeout(uint32_t seconds);

    /**
     * @brief Confirm that the currently-running firmware is healthy.
     *
     * Tells the bootloader the image is good, cancels the rollback timer, and
     * clears the trial record so later boots are normal. Safe to call
     * unconditionally from setup(), even before check() or begin().
     *
     * @return true if a trial was active and is now confirmed; false if the
     *         device was not in a trial (a no-op), or if the bootloader could
     *         not record the image as good (the trial carries on; call again).
     */
    bool confirm();

    /// The name before 1.0.0.
    __attribute__((deprecated("use confirm()"))) bool confirmRunning() { return confirm(); }

    /**
     * @brief Query whether this boot is a not-yet-confirmed trial install.
     *
     * Returns true if this boot is running a new, unconfirmed image and the
     * rollback timer is still armed. Useful for status displays or to delay
     * non-essential application work until the trial resolves. Safe to call
     * before check() or begin().
     *
     * @return true if a trial is in progress; false otherwise.
     */
    bool isTrialBoot();

    /// The name before 1.0.0.
    __attribute__((deprecated("use isTrialBoot()"))) bool isTrialInstall() { return isTrialBoot(); }

    /**
     * @brief Enable or disable verbose Serial logging at runtime.
     *
     * Equivalent to compiling with -DSIMPLEOTA_DEBUG=1, but settable from the
     * sketch. Note: a "#define SIMPLEOTA_DEBUG 1" in the .ino does NOT affect
     * the library's translation unit. Default: off, unless the library was
     * compiled with -DSIMPLEOTA_DEBUG=1.
     *
     * @param enabled  true = verbose logging on; false = silent.
     */
    static void setDebug(bool enabled);

    /**
     * @brief Start the managed-mode background task.
     *
     * Starts a FreeRTOS task that periodically checks and applies. The
     * library is transport-agnostic: it does not manage Wi-Fi, Ethernet, or PPP.
     * The application owns connectivity.
     *
     * No-op if a task is already running. Safe to call once from setup().
     *
     * @param checkIntervalSec  How often to check for updates, in seconds.
     *                          Default: SIMPLEOTA_CHECK_INTERVAL_S.
     * @param isConnected       Optional callback returning true when IP connectivity
     *                          is available. The task polls this every second and
     *                          only attempts a check when it returns true. If nullptr
     *                          (default), the task always attempts a check; transport
     *                          failures are absorbed by the normal retry paths.
     *                          Pass it when connectivity may not be up yet (e.g.
     *                          right after WiFi.begin()): otherwise a first check
     *                          that fails waits a full checkIntervalSec to retry.
     * @param initialDelayMs    Milliseconds to wait inside the task before the first
     *                          check(). Use this to avoid lwIP contention when other
     *                          TLS sessions (e.g. MQTT) are starting concurrently at
     *                          boot. Default: 0 (no delay).
     */
    void begin(uint32_t checkIntervalSec = SIMPLEOTA_CHECK_INTERVAL_S,
               bool (*isConnected)()       = nullptr,
               uint32_t initialDelayMs     = 0);

    /**
     * @brief Wake the managed task to check now. False when begin()'s task
     *        is not running.
     */
    bool checkNow();

    /**
     * @brief Stop the managed-mode background task started by begin().
     *
     * The task stops between steps, so a download or flash write is never
     * cut short: this waits for the current step, up to a minute. Called
     * from the task itself (an onEvent callback), it returns at once and
     * the task stops after the callback.
     */
    void end();

    /**
     * @brief PEM bundle covering simpleota.com and its pre-signed object-storage host.
     *
     * The default for setCACert().
     */
    static const char* kSimpleOtaRootCA;

    /// @name Chip family constants
    /// Pass to the chipFamily constructor parameter. Using these avoids typos
    /// and provides IDE autocomplete. A raw string literal is also accepted
    /// for chips not listed here.
    /// @{
    static const char* const CHIP_ESP32;    ///< "esp32"
    static const char* const CHIP_ESP32S2;  ///< "esp32s2"
    static const char* const CHIP_ESP32S3;  ///< "esp32s3"
    static const char* const CHIP_ESP32C3;  ///< "esp32c3"
    static const char* const CHIP_ESP32C6;  ///< "esp32c6"
    static const char* const CHIP_ESP32H2;  ///< "esp32h2"
    /// @}

    /// @name Security mode constants
    /// Pass to setSecurityMode(). Raw string literals are also accepted for
    /// forward compatibility.
    /// @{
    static const char* const SECURITY_MODE_BASIC;   ///< "basic"  - HTTPS + SHA-256 checksum (default).
    static const char* const SECURITY_MODE_TOKEN;   ///< "token"  - per-device token auth (partial; see wiki).
    static const char* const SECURITY_MODE_SIGNED;  ///< "signed" - on-device Ed25519 firmware signature verification. Pin a key with setSigningPublicKey().
    /// @}

    /**
     * @brief Runtime gate for SOTA_LOG.
     *
     * Public so the logging macro in the .cpp can read it without a friend
     * declaration. Do not write to it directly; use setDebug().
     */
    static bool _debugEnabled;

private:
    sota_engine_t _engine;         ///< The shared core; borrows the strings below.
    bool          _engineOk;       ///< The core accepted the set-up.
    char          _deviceIdBuf[SOTA_DEVICE_ID_LEN];  ///< The default device ID.
    const char*   _caCert;         ///< Never nullptr: TLS is always verified.
    bool          _managedAutoConfirm;
    void*         _download;       ///< The open firmware download (SimpleOTANet.cpp).

    // Managed mode: the core's task.
    sota_runner_t _runner;
    void        (*_onEvent)(const OTAEventInfo&);
    bool        (*_isConnected)();
    static bool  connectedFor(void* self);
    bool         polling(const char* caller);  ///< setUp(), and no managed task.
    bool         setUp(const char* caller);  ///< _engineOk, with a warning when false.

    friend void sota_app_event(void* user, const sota_event_t* event);
    friend void sota_app_wake(void* user);
    friend int  sota_net_post(void* user, const char* url, const char* token,
                              const char* body, char* resp, size_t cap);
    friend int  sota_net_dl_open(void* user, const char* url, int64_t* len, char* location,
                                 size_t location_cap);
    friend int  sota_net_dl_read(void* user, uint8_t* buf, size_t cap);
    friend void sota_net_dl_close(void* user);
};

#endif  // SIMPLEOTACLIENT_H
