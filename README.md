# SimpleOTAClient

> Thin Arduino-ESP32 client library for the [SimpleOTA](https://simpleota.com) firmware update platform.

![Platform: ESP32](https://img.shields.io/badge/platform-ESP32-blue)
![Framework: Arduino](https://img.shields.io/badge/framework-Arduino-teal)
![Version: 1.0.0](https://img.shields.io/badge/version-1.0.0-green)
![License](https://img.shields.io/badge/license-MIT-lightgrey)

Integrates your ESP32 project with SimpleOTA in a few lines: check for an update, stream and flash it with on-the-fly SHA-256 verification, and let the library handle all the protocol bookkeeping. The application owns Wi-Fi and decides when to check; this library does the rest.

Local OTA (ArduinoOTA, espota, ElegantOTA) is for a board on your desk. SimpleOTA is managed fleet OTA for ESP32 when those devices are in the field, without pulling you into a full IoT platform.

You upload a .bin from PlatformIO, ESP-IDF, Arduino, or CI, and devices check in on their own. Eligible ones get a short-lived pre-signed URL and pull firmware straight from object storage. Rollouts can start at 5% with deterministic bucketing, so the same unit stays in the same cohort. Builds can be Ed25519-signed and verified on the device during download, before the new partition is marked bootable. After reboot there's a trial window: crash or no successful check-in, and the device rolls back.

Signing here protects the delivery path. It is not Secure Boot. They stack if you already use Secure Boot.

[Quickstart (project, client, first update)](https://wiki.simpleota.com/getting-started/?utm_source=arduino_lib&utm_medium=readme&utm_campaign=signup_v1)

Starting a new project? [simpleOTA-starter-arduino](https://github.com/wassie-dev/simpleOTA-starter-arduino) is a template repo with a working sketch and a GitHub Actions release workflow already set up.

[Site](https://simpleota.com/?utm_source=arduino_lib&utm_medium=readme&utm_campaign=signup_v1)

---

## Table of contents

- [Features](#features)
- [Requirements](#requirements)
- [Installation](#installation)
- [Quick start](#quick-start)
  - [Managed mode](#managed-mode)
  - [Polling mode](#polling-mode)
- [Build numbers](#build-numbers)
- [API reference](#api-reference)
- [OTA lifecycle](#ota-lifecycle)
- [Security](#security)
  - [Signed firmware](#signed-firmware)
- [Rollback](#rollback)
  - [Testing rollback](#testing-rollback)
- [Configuration](#configuration)
- [Logging](#logging)
- [Partition table](#partition-table)
- [Footprint](#footprint)
- [Limitations](#limitations)
- [Troubleshooting](#troubleshooting)
- [Arduino and ESP-IDF](#arduino-and-esp-idf)
- [License](#license)

---

## Features

- **Two-method integration:** `check()` then `apply()` is all most projects need.
- **Streaming download with on-the-fly SHA-256:** firmware is verified before the partition is committed; no second pass, no large RAM buffer.
- **NVS build-number persistence:** the library reads and writes the SimpleOTA-assigned build number automatically.
- **Status event reporting:** reports the full update lifecycle back to the SimpleOTA backend.
- **On-device firmware signature verification:** signed artifacts are verified against your pinned Ed25519 public key, streamed during download, before the image is ever marked bootable. See [Signed firmware](#signed-firmware).
- **Trial install with timeout-based rollback:** the library records the previous image before switching to the new one, then rolls back to it if the new image crashes, hangs, or does not call `confirm()` within the configurable timeout. See [Rollback](#rollback).
- **No external dependencies:** uses `HTTPClient` and `NetworkClientSecure` / `WiFiClientSecure` from the Arduino-ESP32 core, and ESP-IDF's OTA and NVS APIs. The update logic is [SimpleOTAClient-core](https://github.com/wassie-dev/simpleOTAClient-core), shared with the ESP-IDF client and copied into `src/sota_core`, with a vendored copy of [Monocypher](https://monocypher.org) 3.1.3 for Ed25519; nothing to install.
- **Compatible with Arduino-ESP32 2.x and 3.x**.
- **Secure by default:** the bundled ISRG Root X1 root CA is used automatically; no configuration needed for production.

---

## Requirements

| Requirement | Version |
| --- | --- |
| Arduino-ESP32 core | 2.x or 3.x |
| Target hardware | Any ESP32 variant |
| Partition table | Must have two OTA app partitions (see [Partition table](#partition-table)) |
| Network | Application must establish IP connectivity before calling `check()` |

---

## Installation

**Arduino IDE (Library Manager)**

Search for `SimpleOTAClient` in *Sketch → Include Library → Manage Libraries*.

**Manual**

Clone or download this repository and copy the folder into your Arduino
`libraries/` directory:

```
~/Documents/Arduino/libraries/SimpleOTAClient/
```

**PlatformIO**

Add to `platformio.ini`:

```ini
lib_deps =
    https://github.com/xanderwasserman/SimpleOTAClient-Arduino.git
```

---

## Quick start

### Managed mode

Call `begin()` from `setup()`. The library starts a FreeRTOS background task that checks for an update on a fixed interval, flashes it, and then sleeps before repeating. Your application loop is untouched.

The library is **transport-agnostic**: it does not manage Wi-Fi, Ethernet, or PPP. Bring up your network however you like, and give `begin()` an `isConnected` probe so the task waits for it.

```cpp
#include <WiFi.h>
#include <SimpleOTAClient.h>

SimpleOTAClient ota("soto_proj_xxxx");

static bool wifiUp() { return WiFi.status() == WL_CONNECTED; }

void setup() {
    Serial.begin(115200);
    WiFi.mode(WIFI_STA);
    WiFi.begin("ssid", "password");
    ota.begin(3600, wifiUp);   // check every hour once Wi-Fi is up
}

void loop() {
    // your application code
}
```

The task polls `isConnected` once a second and only attempts a check while it returns `true`. Without a probe, the task checks as soon as it starts, whether or not the link is up. On a normal boot, a check that fails because the link is not up yet returns `false`, and the next attempt waits the full `checkIntervalSec`; a sketch that calls `begin()` with no probe straight after `WiFi.begin()` usually does not check in until an hour after boot.

For Ethernet, cellular, or any other stack, supply the equivalent probe (`ETH.linkUp()`, your modem's `isConnected()`, etc.).

To be notified of failures, progress or the trial's outcome, register [`onEvent()`](#void-oneventvoid-callbackconst-otaeventinfo-event):

```cpp
ota.onEvent([](const OTAEventInfo& e) {
    if (e.id == OTA_EVENT_UPDATE_FAILED)
        Serial.printf("OTA failed: %s\n", e.reason);
});
```

`checkNow()` wakes the task for an immediate check.

### Polling mode

Call `check()` and `apply()` from your own loop or FreeRTOS task for full control over timing and error handling. Polling and managed mode don't mix: while `begin()`'s task runs, `check()` returns `false` and `apply()` returns `OTA_NO_OFFER`, with a warning.

```cpp
void loop() {
    if (ota.check()) {
        OTAResult r = ota.apply();   // reboots on success
        if (r != OTA_SUCCESS) {
            Serial.println("[app] OTA failed, continuing");
        }
    }
    delay((uint32_t)SIMPLEOTA_CHECK_INTERVAL_S * 1000UL);   // check again in 1 hour
}
```

Three runnable examples are bundled:

| Example | What it shows |
| --- | --- |
| [`examples/BasicOTA/BasicOTA.ino`](examples/BasicOTA/BasicOTA.ino) | Minimal polling integration: Wi-Fi up, `check()` + `apply()` from `loop()`. Start here. |
| [`examples/ManagedOTA/ManagedOTA.ino`](examples/ManagedOTA/ManagedOTA.ino) | Hands-off managed mode using `begin()` with an `isConnected` probe and `onEvent()`. |
| [`examples/AdvancedOTA/AdvancedOTA.ino`](examples/AdvancedOTA/AdvancedOTA.ino) | Custom `deviceId` / `boardId` / `hardwareRevision`, `setAutoReboot(false)` with `rebootForUpdate()` for the application-driven restart, verbose logging. |

---

## Build numbers

SimpleOTA assigns its own **strictly monotonic build number** each time you upload a firmware artifact. The server uses this number (not your `APP_VERSION` string, not `__DATE__`) to decide whether a device needs an update.

**How the library manages it for you:**

| Situation | What happens |
| --- | --- |
| Factory-fresh device (empty NVS) | Library sends `current_build_number: 0` |
| After a successful `apply()` | Library writes the build number from the check response into NVS (`simpleota` / `sota_build`, uint32) |
| With `setAutoReboot(false)`, while the update waits for `rebootForUpdate()` | `sota_build` already holds the flashed build and `check()` sends it, so the server does not offer that build again before the reboot |
| Every subsequent `check()` | Library reads `sota_build` from NVS and sends it |
| Firmware installed another way (USB, ArduinoOTA, or the `Update` class directly) | `sota_build` does not change: the library keeps sending the last build it installed itself, or 0 if it has not installed one |

**Do not write to the `simpleota`/`sota_build` NVS key from your own code.** Overwriting it with any value other than one the server gave you will break the server's comparison: the device will either never receive another update, or be offered a build it already has.

---

## API reference

### Constructor

```cpp
SimpleOTAClient(const char* token,
          const char* chipFamily       = nullptr,
          const char* deviceId         = nullptr,
          const char* boardId          = nullptr,
          const char* hardwareRevision = nullptr);
```

> **Note:** `deviceId` must point to a buffer that outlives the `SimpleOTAClient` object. A `static char` array formatted in `setup()` (see [Quick start](#quick-start)) is the standard pattern.

| Parameter | Description |
| --- | --- |
| `token` | Project token or device token. Sent as `Authorization: Bearer <token>`. A **project token** (`soto_proj_...`) authenticates any device in the project; the server uses `deviceId` to identify the specific device. A **device token** authenticates a single pre-registered device; the server resolves it directly and ignores `deviceId` from the payload. |
| `chipFamily` | Optional. When `nullptr` (default), the chip the sketch is built for, as on ESP-IDF. Pass a `SimpleOTAClient::CHIP_*` constant or a string only to report a different family. |
| `deviceId` | Optional. Stable per-unit identifier. When `nullptr` (default), the library automatically uses the Wi-Fi MAC address formatted as `"aa:bb:cc:dd:ee:ff"`. Pass a custom string to override. |
| `boardId` | Optional. Omitted from requests when `nullptr`. |
| `hardwareRevision` | Optional. Omitted from requests when `nullptr`. |

String arguments are not copied; they must remain valid for the lifetime of the `SimpleOTAClient` object. String literals and `static` buffers are fine. When `deviceId` is `nullptr`, the library uses its own internal buffer.

---

### `bool check()`

POSTs to `/api/v1/ota/check/` with the device identity and the current build number from NVS.

Returns `true` if the server offers an update and stores the offer details (`url`, `checksum`, `build_number`, `deployment_id`) on the instance for `apply()` to consume.

Returns `false` on no-update, malformed response, or transport failure.

Per the SimpleOTA protocol, **all business outcomes (no update, device over limit, project inactive) return HTTP 200**. The library never retries a 200 response, so calling `check()` in a polling loop is safe.

---

### `OTAResult apply()`

Executes the offer stored by the most recent successful `check()`.

| Return value | Meaning |
| --- | --- |
| `OTA_SUCCESS` | Firmware flashed and build number persisted. Device reboots before this is observed when `setAutoReboot(true)` (the default); with `setAutoReboot(false)` the caller is responsible for rebooting. |
| `OTA_CHECKSUM_FAIL` | Downloaded payload did not match the expected SHA-256. Partition was **not** committed. Safe to retry. |
| `OTA_FLASH_FAIL` | Network or flash write error. Partition was not committed. |
| `OTA_NO_OFFER` | `check()` had not been called or returned `false`. |
| `OTA_SIGNATURE_FAIL` | Ed25519 signature verification failed for a signed artifact. Partition was **not** committed; a `failed` event with reason `signature_invalid` was reported. Treat as a security signal, not flakiness. See [Signed firmware](#signed-firmware). |
| `OTA_UNCONFIRMED` | The running firmware is on trial and has not been confirmed. Nothing was downloaded or reported, because the other slot holds the previous firmware the trial may still return to. Call `confirm()`, then `apply()` again; the offer is kept. |
| `OTA_DOWNGRADE` | The offered build number is not above the running one. Nothing was downloaded, and a `failed` event with reason `downgrade_refused` was reported. See [Security](#security). |
| `OTA_REBOOT_PENDING` | With `setAutoReboot(false)`, an earlier `apply()` flashed an update that is waiting for its reboot. Nothing was downloaded or reported, because the next update slot holds the waiting image. Call `rebootForUpdate()` to install it. |

> **Note on `validated`:** this event is reported after `Update.end()` succeeds (meaning "the image was flashed cleanly"), not after a successful boot. A subsequent `reboot` event is emitted immediately before `esp_restart()` on the auto-reboot path; a `confirmed` event follows once the trial is confirmed (see [Rollback](#rollback)).

---

### `void onEvent(void (*callback)(const OTAEventInfo& event))`

Calls `callback` for each lifecycle event, the same events the ESP-IDF client's `event_cb` gets: `OTA_EVENT_CHECK_DONE`, `OTA_EVENT_UPDATE_AVAILABLE`, `OTA_EVENT_DOWNLOAD_PROGRESS` (`bytesDone` of `bytesTotal`), `OTA_EVENT_UPDATE_FAILED` (`reason` says why), `OTA_EVENT_BEFORE_REBOOT`, `OTA_EVENT_TRIAL_BOOT`, `OTA_EVENT_CONFIRMED` and `OTA_EVENT_ROLLING_BACK`. `buildNumber` is the build the event is about. It runs on the task that drives the update (the managed task, or the one calling `check()`, `apply()` or `confirm()`), so keep it short and don't call the client from it. Pass `nullptr` to stop.

### Status events

The library posts every status event itself. Full event vocabulary defined by the API:

```
offered  download_started  downloaded  flashed  validated  reboot  confirmed  failed  rolled_back
```

Events posted by `apply()`: `download_started`, `downloaded`, `flashed`, `validated`, `failed`, and (on the auto-reboot path) `reboot`. The library also queues `confirmed` when a trial is confirmed and `rolled_back` after a rollback, and sends them at the start of the next `check()` (see [Server interaction](#server-interaction)).

---

### `void setCACert(const char* pemRootCA)`

Overrides the CA certificate used for HTTPS verification. By default the library uses the bundled `kSimpleOtaRootCA` (ISRG Root X1), which covers both the SimpleOTA API and its firmware storage. Call this only if you need to pin a different certificate, such as your own CA for a local server. `nullptr` is ignored with a warning: TLS is always verified.

---

### `bool setServerUrl(const char* url)`

Points `check()` and status posts at another SimpleOTA server. Call it before `begin()` or after `end()`: while the managed task runs it returns `false` with a warning. The pointer is stored and must outlive the client. Until a URL is accepted, the base is `https://simpleota.com`.

```cpp
static const char kOtaBase[] = "https://ota.example.com";
ota.setServerUrl(kOtaBase);
```

`https` is accepted in any case. `http` and any other value return `false`, print a `[SimpleOTAClient]` line (`not_https` or `malformed`), and leave the previous URL in place. Downloads still use the pre-signed URL. TLS verification is unchanged; use `setCACert()` for a different CA.

---

### `void setAutoReboot(bool enabled)`

Default: `true`. When `false`, `apply()` returns `OTA_SUCCESS` without calling `esp_restart()`, giving the application control over when the reboot happens. The NVS build number write and `validated` event still occur before the function returns. The library does **not** emit a `reboot` event in this mode; use [`rebootForUpdate()`](#void-rebootforupdate) when the application is ready to restart so the server sees the same event sequence as the auto-reboot path. Until that reboot, further `apply()` calls return `OTA_REBOOT_PENDING` without downloading.

---

### `void rebootForUpdate()`

Convenience for applications running with `setAutoReboot(false)`. Emits the `reboot` lifecycle event for the update that is waiting, then calls `esp_restart()`. Does not return.

The event names the waiting update's deployment even after later `check()` calls, or after `end()` and `begin()`. With no update waiting nothing is posted; the device reboots either way.

---

### `void setPartitionProfile(const char* profile)`

Reports the device's partition layout to the server (e.g. `"default_4mb"`, `"minimal_spiffs_4mb"`). A blank `partition_profile` on either side matches any value. Pass `nullptr` (default) to omit the field.

The value should match the scheme you selected in the Arduino IDE under *Tools → Partition Scheme*, or the `board_build.partitions` value in your `platformio.ini`.

---

### `void setNvsSchemaVersion(uint8_t version)`

Default: `1`. Increment this when you restructure your own NVS namespace layout and want the server to gate builds that require a specific schema version. The library always sends this field explicitly; the server assumes `1` when it is absent.

---

### `void setLabels(const char* jsonObject)`

Arbitrary key/value metadata sent to the server for deployment targeting. Must be a valid JSON object literal string, e.g. `"{\"site\":\"factory-A\"}"`. The string is embedded verbatim in the request body; no escaping or validation is performed by the library. Pass `nullptr` (default) to omit the field.

---

### `void setSecurityMode(const char* mode)`

Advertises the device's security capability to the server. Used for server-side compatibility filtering: a mismatch between the device's declared mode and the artifact's required mode blocks the update. Pass `nullptr` (default) to omit the field.

Use the provided constants to avoid typos:

| Constant | Value | Description |
|---|---|---|
| `SimpleOTAClient::SECURITY_MODE_BASIC` | `"basic"` | HTTPS + SHA-256 checksum verification. Default. |
| `SimpleOTAClient::SECURITY_MODE_TOKEN` | `"token"` | Per-device token authentication. Partially implemented on the server; advisory today. |
| `SimpleOTAClient::SECURITY_MODE_SIGNED` | `"signed"` | On-device Ed25519 firmware signature verification. Pin your key with `setSigningPublicKey()`; see [Signed firmware](#signed-firmware). |

A raw string literal is also accepted for forward compatibility.

---

### `bool setSigningPublicKey(const char* pem)`

Pins the project's Ed25519 **public** signing key (replacing any previously pinned keys) and enables on-device signature verification for signed artifacts. Takes the PEM exactly as issued by the SimpleOTA dashboard or API; the raw key bytes are copied, so the string need not outlive the call. Returns `false` if the PEM is not a valid Ed25519 public key.

See [Signed firmware](#signed-firmware) for the full behavior contract.

---

### `bool addSigningPublicKey(const char* keyId, const char* pem)`

Pins an additional key (maximum two) for key-rotation windows. When the server's offer names a `signing_key_id` matching one of the given `keyId`s, only that key is tried; otherwise all pinned keys are tried and the signature is accepted if it verifies under any of them. Returns `false` when two keys are already pinned or the PEM is invalid.

---

### `void setVersionLabel(const char* label)`

Sets the human-readable version label of the firmware **currently running** (e.g. `"1.4.1"`). Sent as `version_label` and shown on the dashboard under *Reported firmware state → Version label*. Display only; not used for compatibility checks.

When `nullptr` (default), the library automatically reads the value persisted in NVS by the last successful `apply()`, so post-OTA boots report the correct label without any code change in the new firmware image. You only need this call to provide the initial label before any OTA has occurred, or to override the NVS value.

---

### `void setChannel(const char* channel)`

Sets the release channel this device subscribes to (e.g. `"stable"`, `"beta"`). Only applied by the server on first registration or when the device has no channel yet. Pass `nullptr` (default) to omit the field.

---

### `const char* lastOfferedVersion()`

Returns the version label string from the last update offered by `check()` (e.g. `"1.4.2"`). Valid between a successful `check()` and the `apply()` call that consumes the offer. Returns an empty string before any offer has been received and after `apply()` clears the offer.

---

### `uint32_t currentBuild()`

The build number this device reports to the server, or `0` before its first SimpleOTA update.

---

### `static void setDebug(bool enabled)`

Enables or disables verbose `[SimpleOTAClient]` logging on `Serial` at runtime. See [Logging](#logging).

---

### `void setManagedAutoConfirm(bool enabled)`

In managed mode (`begin()`), controls whether the library automatically calls `confirm()` on the first successful response (HTTP 200) from `/api/v1/ota/check/` after a trial boot. Default: `true`. Set to `false` to require an explicit `confirm()` call from your application code instead. No effect in polling mode (where you always call `confirm()` yourself).

---

### `void setConfirmTimeout(uint32_t seconds)`

Sets the per-instance trial-install confirm timeout in seconds. Defaults to `SIMPLEOTA_CONFIRM_TIMEOUT_S` (compile-time default `300`). Clamped to `[10, 86400]`. On a trial boot, call it before `begin()` in managed mode, or before the first `check()`, `apply()`, `isTrialBoot()` or `confirm()` in polling mode; the trial's timer starts then. See [Rollback](#rollback).

---

### `bool confirm()`

Called `confirmRunning()` before 1.0.0; that name still compiles, with a deprecation warning.

Confirms that the currently-running firmware is healthy and ends the trial install. Tells the bootloader the image is good, stops the rollback timer and the chip watchdog, clears the trial record from NVS, and queues a `confirmed` status event, sent at the start of the next `check()`. Returns `true` if a trial was in progress and is now confirmed. Returns `false` on a normal boot (a safe no-op), and also when the bootloader could not record the image as good; the trial then carries on and you can call it again.

Call from your application code once you are satisfied the new firmware is working. In polling mode this is required after every successful OTA; in managed mode it is only required if you have called `setManagedAutoConfirm(false)`.

---

### `bool isTrialBoot()`

Called `isTrialInstall()` before 1.0.0; that name still compiles, with a deprecation warning.

Returns `true` while a trial install is in progress (i.e. the current boot is the first boot of a new firmware image and `confirm()` has not yet been called). Use this to avoid expensive health-probe code on normal boots.

---

### `void begin(...)`

Starts a FreeRTOS background task (stack: 8 KB, priority: 1) that runs the following loop:

1. **If `initialDelayMs` > 0**, sleep for that many milliseconds (one-shot, at task start only). Use this to stagger the first HTTPS request relative to other TLS sessions starting concurrently at boot (e.g. MQTT). Default: `0`.
2. If `isConnected` was supplied, wait until it returns `true` (polled every second). Otherwise skip this step.
3. **If a trial install is in progress**, run a short-retry inner loop: retry `/check/` every 10 s until the server responds with HTTP 200 or the confirm timeout expires. With `setManagedAutoConfirm(true)` (default), call `confirm()` on the first such response, send the `confirmed` event straight away, and `apply()` an update that check offered; that check counts as this cycle's, so the loop goes on to step 5. Skip this step on a normal (non-trial) boot.
4. Check in. If an update is available, apply it.
5. Sleep for `checkIntervalSec` seconds, or until `checkNow()`, then repeat.

Outcomes arrive through [`onEvent()`](#void-oneventvoid-callbackconst-otaeventinfo-event).

The library is transport-agnostic and does not import any networking stack. The application is responsible for bringing up Wi-Fi, Ethernet, PPP, or whatever connectivity it uses. Pass `isConnected` whenever connectivity may not be up when `begin()` runs, such as straight after `WiFi.begin()`. Without it, the first check runs without waiting for the link, a transport failure returns `false` from `check()`, and the retry waits for the next interval, `checkIntervalSec` later.

Safe to call once from `setup()`. Calling `begin()` while a task is already running is a no-op.

Signature: `begin(checkIntervalSec = SIMPLEOTA_CHECK_INTERVAL_S, isConnected = nullptr, initialDelayMs = 0)`. Before 1.0.0 the second argument was an `onResult` callback; use `onEvent()` instead.

---

### `bool checkNow()`

Wakes the managed task to check now, like `simpleota_check_now()` on ESP-IDF. Returns `false` when `begin()`'s task is not running.

---

### `void end()`

Stops the background task started by `begin()`. The task stops between steps, so a download or flash write is never cut short: `end()` waits for the current step, up to a minute.

---

### `static const char* SimpleOTAClient::kSimpleOtaRootCA`

Bundled root CA PEM (ISRG Root X1 / Let's Encrypt), used by default for all HTTPS connections. Both the SimpleOTA API and its firmware storage chain to this root, so no additional configuration is needed.

---

## OTA lifecycle

The sequence of events reported to the server during a successful `apply()`:

```
check()  →  download_started  →  downloaded  →  flashed  →  validated  →  reboot  →  [esp_restart()]  →  confirmed
```

The terminal `confirmed` event is queued when `confirm()` runs on the new image (managed mode does this automatically by default; polling mode requires an explicit call) and sent at the start of the next `check()`. If the trial fails instead, the device returns to the previous firmware and a `rolled_back` event takes the place of `confirmed`.

On failure at any stage, a `failed` event is reported with a short reason token before the method returns:

| Reason token | Stage |
| --- | --- |
| `downgrade_refused` | The offered build is not above the running one |
| `insecure_url` | URL did not begin with `https://` |
| `https_begin_failed` | Could not open HTTPS connection to download URL |
| `http_status_<N>` | Server returned non-200 for the firmware GET, or a fourth redirect |
| `redirect_not_https` | A download redirect led to a plain HTTP URL |
| `redirect_malformed` | A download redirect had no usable `Location` |
| `update_begin_failed` | `Update.begin()` failed, likely a partition issue |
| `update_write_failed` | Flash write error during streaming |
| `stream_error` | Socket error mid-download |
| `download_stalled` | No bytes received within `SIMPLEOTA_HTTP_TIMEOUT_MS` |
| `short_read` | Stream closed before `Content-Length` bytes arrived |
| `checksum_mismatch` | SHA-256 of received bytes did not match server's value |
| `signature_invalid` | Signed artifact failed Ed25519 verification, or no key is pinned; see [Signed firmware](#signed-firmware) |
| `state_save_failed` | The trial record could not be saved to NVS before the slot switch |
| `update_end_failed` | `Update.end()` failed after streaming completed |

If [rollback](#rollback) is enabled and a trial install crashes, hangs, or is not confirmed within the timeout, the device returns to the previous partition and queues a separate `rolled_back` event, sent by the next `check()`. Its reason says what ended the trial; see [Server interaction](#server-interaction).

---

## Security

> **Read this section before deploying to a production fleet.**

The integrity guarantee of this library rests on the SHA-256 `checksum` field from the `/check/` response. That field travels over the same TLS channel as the firmware `url`. **If that TLS channel is not verified, an on-path attacker can supply any `(url, checksum)` pair (including for a malicious firmware image), and the device will flash it without complaint.**

### Default mode: secure

The library defaults to verifying TLS using the bundled ISRG Root X1 root CA (`kSimpleOtaRootCA`). This covers both the SimpleOTA API and the firmware download endpoint. No additional configuration is needed for production.

### TLS is always verified

There is no way to turn verification off. `setCACert(nullptr)` keeps the current CA and prints a warning.

To test against a local server, issue it a certificate from your own CA (for example with `mkcert`) for the hostname or IP the device connects to. For an IP, put it in the subjectAltName. Then pass that CA to `setCACert()`. The ESP-IDF client works the same way with `cert_pem`.

### Other hardening built into the library

- Anti-rollback: an offer whose build number is not above the running build is refused (`downgrade_refused`), because an old image's signature still verifies and old builds can carry fixed bugs. To go back, release the old code under a new, higher build number. The running build is the one stored with the image SimpleOTA installed; an image flashed some other way, such as over serial, reports build 0, so a device reused for another project takes that project's builds.
- Download URLs that don't begin with `https://` are rejected before any connection is made.
- Project tokens containing CR/LF or control characters are rejected to prevent HTTP header injection.
- The library never retries a received HTTP response; only transport-level failures trigger a retry. This prevents fleet-scale retry storms.

### Signed firmware

The library verifies **Ed25519 firmware signatures on the device**, upgrading the integrity guarantee from "trust the TLS channel" to "trust only images approved by the holder of the project's private signing key". Even a fully compromised delivery path (server, storage, TLS) cannot make the device flash an unapproved image.

Setup ([full guide](https://wiki.simpleota.com/guides/signed-firmware/)):

```cpp
ota.setSecurityMode(SimpleOTAClient::SECURITY_MODE_SIGNED);
ota.setSigningPublicKey(
    "-----BEGIN PUBLIC KEY-----\n"
    "MCowBQYDK2VwAyEA...your project public key...\n"
    "-----END PUBLIC KEY-----\n");
```

The signature (delivered in the `/check/` response for signed artifacts) is verified incrementally over the exact downloaded bytes, in the same streaming pass as the SHA-256 checksum, **before** `Update.end()` marks the new image bootable. No extra RAM buffer, no second pass over flash.

A device is **enforcing** once it both reports signed mode
(`setSecurityMode(SECURITY_MODE_SIGNED)`) and has a key pinned. For an
enforcing device the decision is made from device state, not from what the
offer claims, so a compromised server or MITM cannot downgrade it by
stripping the signature fields:

| Offer | Enforcing? | Result |
| --- | --- | --- |
| Signed, valid signature | yes | Verified and flashed. |
| Signed, tampered signature | yes | Rejected before the image is bootable. `apply()` returns `OTA_SIGNATURE_FAIL`; a `failed` event with reason `signature_invalid` is reported (shown on the dashboard device row). Device keeps its current firmware. |
| Signature missing, or offer arrives **without** signed fields | yes | Rejected before the download even starts (same `OTA_SIGNATURE_FAIL` / `signature_invalid`). This is the anti-downgrade guarantee. |
| Signed | no key pinned | Applied with checksum verification only, plus a `Serial` warning on each such apply. Keeps fleet migration safe (below). |
| Not signed | not enforcing | Identical to previous releases. |

#### How the server matches modes

The server treats `security_mode` as a compatibility dimension: an artifact is only offered to a device when the two values are **exactly equal, or either side is blank**. A device that never calls `setSecurityMode()` reports nothing and matches every artifact. The rule is symmetric:

| Device reports | `basic` artifact | `signed` artifact |
| --- | --- | --- |
| *(nothing)* | offered | offered |
| `basic` | offered | **not offered** |
| `signed` | **not offered** | offered |

A blocked device does not error: it receives a normal `no_compatible_build` response and silently stays on its current build.

#### Migrating a fleet from `basic` to `signed`

Keys are compiled into the firmware, and the device's reported mode is changed by the firmware it runs, never by the mode of the artifact that delivered it. So:

1. Ship one final **`basic`** upload whose code adds `setSigningPublicKey(...)` and `setSecurityMode(SECURITY_MODE_SIGNED)`. A `basic` artifact reaches devices reporting `basic` *and* devices that never reported a mode, so the whole fleet gets it regardless of history.
2. Devices running it report `security_mode: "signed"` and verify every subsequent update on-device.
3. From then on, **upload only signed artifacts**: per the matrix above, a `signed` device is never offered a `basic` artifact, so there is no quiet fallback to unsigned releases.

Do not upload the migration build itself as `signed`: devices explicitly reporting `basic` will never be offered it, and devices that would accept it cannot verify it yet anyway.

> **Watch for stragglers.** After migrating, check the project device list for devices still reporting `basic` (stuck on an old build): they missed the migration artifact and will receive nothing once you publish only signed uploads.

**Key rotation:** create the new key server-side, ship a firmware update pinning **both** keys via `addSigningPublicKey()`, start signing with the new key, then revoke the old key server-side and drop it from the next firmware.

#### Scope: this is not Secure Boot

Verification runs inside the application, so the trust anchor is the firmware the device is already running. That protects the **OTA delivery path** (compromised account, leaked upload token, tampered storage, MITM); it does **not** protect the device itself. An attacker with physical access can still reflash arbitrary firmware over UART/USB via the ESP32's ROM download mode, and firmware that is already compromised will not verify itself. The hardware answer to those threats is [ESP32 Secure Boot](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/security/secure-boot-v2.html) (plus Flash Encryption), which is complementary: SimpleOTA signing protects the supply chain to the device, Secure Boot protects the device itself. Note that stock Arduino-ESP32 cores ship precompiled bootloaders built **without** Secure Boot, so enabling it requires building under ESP-IDF; on the Arduino path, OTA-level signing is the strongest firmware-integrity protection available.

Ed25519 verification uses a vendored, unmodified copy of [Monocypher](https://monocypher.org) 3.1.3 (dual-licensed CC0-1.0 / BSD-2-Clause; see `src/sota_core/LICENSE-vendor.md`).

---

## Rollback

The library supports a **library-managed trial install with timeout-based rollback**. Before `apply()` switches the boot slot to the new image, it saves a trial record: the outgoing image's partition address and identity (build number, hash, version label). A power cut at any point therefore leaves either the old image running or the new image on trial, never the new image unsupervised. Then it reboots into the new firmware. The new boot enters a "trial" state: if the application calls `confirm()` before `SIMPLEOTA_CONFIRM_TIMEOUT_S` (default 300 s), the image is accepted and the trial record is cleared. Otherwise the library switches back to the previous partition and reboots into it.

The stock Arduino-ESP32 bootloader can switch back to the previous firmware by itself, but the core normally accepts every new image before `setup()` runs. The library tells the core to leave a SimpleOTA trial on probation (it defines `verifyRollbackLater()`), and only a SimpleOTA trial: an image written by ArduinoOTA or another updater is accepted as usual. So a crash or a reset at any point before `confirm()`, including in a global constructor or early in `setup()`, makes the bootloader return to the previous firmware on the next boot. The `rolled_back` report then carries the reset that ended the trial: `panic`, `int_wdt`, `task_wdt`, `watchdog`, `brownout`, `power_on`, `deep_sleep` or `reset`. Deep sleep also ends a trial: an image that wakes from deep sleep before `confirm()` goes back to the previous firmware, with the reason `deep_sleep`, so confirm before sleeping. `confirm()` tells the bootloader the image is good first; if that fails, it returns `false` and the trial carries on.

A sketch cannot define its own `verifyRollbackLater()` alongside the library. Build with `-DSIMPLEOTA_DISABLE_BOOTLOADER_ROLLBACK` to leave the core's behaviour alone. The library then rolls back itself, as the bootloader would: on the confirm timeout, and on any reset before `confirm()` (a crash, a watchdog, a brownout, a power loss, `ESP.restart()` or deep sleep), so confirm before restarting on purpose. It misses an image that crashes before the library's first call on every boot. See [Limitations](#limitations).

A hang that also stops the FreeRTOS confirm timer is caught by the chip's own watchdog (the RTC or LP watchdog, which runs on the slow clock). It starts in `verifyRollbackLater()` before `setup()` for the compile-time `SIMPLEOTA_CONFIRM_TIMEOUT_S`, restarts with this instance's confirm time when the library first runs, and stops on `confirm()`. It waits for the confirm time plus a margin: the larger of 10 percent and the time one request can take at `SIMPLEOTA_HTTP_TIMEOUT_MS` (70 s at the default), and never less than 60 s. When it fires, the chip resets, the previous firmware comes back, and the report says `watchdog`. So call `confirm()` within the confirm time, and if your bring-up before the first library call can take longer than the default, raise `SIMPLEOTA_CONFIRM_TIMEOUT_S` with a `-D` flag. At the usual internal slow clock the watchdog can wait at most about 8 hours (about 13 hours on the S2); a longer confirm time prints a warning and the watchdog fires at its limit. On core 3.x it stops before deep sleep, so the wake reports `deep_sleep`; on core 2.x a sleep longer than the rest of the confirm time ends in a watchdog reset instead. Either way the trial rolls back, so confirm before sleeping. Build with `-DSIMPLEOTA_DISABLE_TRIAL_WATCHDOG` to leave it out. That flag must reach the C compiler too: put it in a `build_opt.h` next to the sketch, or set both `compiler.c.extra_flags` and `compiler.cpp.extra_flags`.

### Polling mode

You must call `confirm()` explicitly. The natural place is at the top of `loop()`, *after* your application has proven it can do real work:

```cpp
void loop() {
    if (WiFi.status() != WL_CONNECTED) connectWiFi();
    ota.confirm();   // no-op when not in trial; safe to call every iteration
    // ... your app code ...
    if (ota.check()) ota.apply();
    delay(SIMPLEOTA_CHECK_INTERVAL_S * 1000UL);
}
```

If you never call `confirm()`, every successful OTA will roll back.

### Managed mode (default)

`begin()` runs the trial loop on its own. By default it auto-confirms the trial as soon as the OTA task gets its first successful response (HTTP 200) from `/api/v1/ota/check/`, which transitively proves boot, connectivity, TLS, token auth, and server reachability. No app code change is needed.

To gate confirmation on your own application-level health check instead:

```cpp
ota.setManagedAutoConfirm(false);
ota.setConfirmTimeout(120);   // give your app 120 s to confirm
ota.begin(...);
// ...
if (ota.isTrialBoot() && applicationHealthy()) {
    ota.confirm();
}
```

See `examples/RollbackOTA` for the full pattern.

### Cellular and other high-latency transports

The default 300 s timeout assumes Wi-Fi. For cellular or other transports where attaching, registering, and reaching the API can routinely take several minutes, raise `setConfirmTimeout()` accordingly. The clamp is `[10, 86400]` seconds.

### Server interaction

After a rollback the previous image's identity (build number, hash, version label) is restored in NVS, and a `rolled_back` status event with the failed deployment's `deployment_id` and `build_number` is queued. Its reason is `confirm_timeout` when the confirm timer ran out, otherwise the reset that ended the trial (see above). On a successful trial (`confirm()` ran), a `confirmed` event is queued.

Queued events are kept in NVS, at most two, oldest dropped first, and sent at the start of each `check()`. An event counts as delivered as soon as the server returns any HTTP response; the library does not act on what the response says. An event that gets no response stays queued for the next `check()`. Queued events never hold up an update.

### Testing rollback

Before relying on rollback in the field, try each way a trial can fail on one device of every board you ship. Deploy a known-good build first, then try each of these from it and watch the serial log and the dashboard:

| Test | Expected result |
| --- | --- |
| Calls `confirm()` as usual | Stays on the new build; `confirmed` |
| Never calls `confirm()` (managed mode with `setManagedAutoConfirm(false)`) | Back on the known-good build after the confirm time; `rolled_back` with `confirm_timeout` |
| Calls `abort()` early in `setup()` | Back on the known-good build after one reset; `rolled_back` with `panic` |
| Hangs in `setup()` (`for (;;) {}`) | Back on the known-good build; `rolled_back` with `confirm_timeout`, or with the watchdog reset that caught the hang |
| Calls `disableCore0WDT()`, then starts a task at the highest priority on each core that spins forever, so the confirm timer cannot run either | Back on the known-good build after the confirm time plus the watchdog margin; `rolled_back` with `watchdog` |
| Loses power before `confirm()` | Back on the known-good build; `rolled_back` with `power_on` |
| Loses power during the download | Stays on the known-good build and downloads the update again on a later check |

Keep the confirm time short while testing, for example `setConfirmTimeout(60)`, and turn on [logging](#logging) to see each boot classified. Run the table once on a device that still has 0.4.0, so the first update into this release is covered too. If your devices use deep sleep, also deep-sleep a test build before it confirms: it should wake on the known-good build, with `rolled_back` and `deep_sleep`.

---

## Configuration

Override any of the following with a `-D` compiler flag or a `#define` placed **before** `#include <SimpleOTAClient.h>`:

| Define | Default | Description |
| --- | --- | --- |
| `SIMPLEOTA_HTTP_TIMEOUT_MS` | `15000` | Milliseconds before an HTTP request or a stalled download is abandoned. Called `SIMPLEOTA_TIMEOUT_MS` before 1.0.0, which still works. |
| `SIMPLEOTA_CHECK_INTERVAL_S` | `3600` | Default check interval (in seconds) used by `begin()`. Also exposed as a constant for manual scheduling in polling mode. |
| `SIMPLEOTA_CONFIRM_TIMEOUT_S` | `300` | Default trial-install confirm timeout (in seconds). See [Rollback](#rollback). Overridable per-instance via `setConfirmTimeout()`. |
| `SIMPLEOTA_DISABLE_TRIAL_WATCHDOG` | unset | `-D` flag: leave the chip watchdog out. Crashes and resets still roll back. |
| `SIMPLEOTA_DISABLE_BOOTLOADER_ROLLBACK` | unset | `-D` flag: do not define `verifyRollbackLater()`; the library rolls back by itself, which misses an image that crashes before its first call on every boot. |
| `SIMPLEOTA_DEBUG` | `0` | Compile-time default for verbose `[SimpleOTAClient]` logging on `Serial`. Prefer the runtime `SimpleOTAClient::setDebug(true)` toggle from your sketch; see [Logging](#logging). |

**Retry policy:** `check()` and status posts retry once after a 2-second delay on transport failure (HTTP code ≤ 0). They never retry on any received HTTP response. The firmware download in `apply()` does not retry; a failure returns `OTA_FLASH_FAIL` and the next `check()` cycle can try again.

---

## Logging

Enable verbose logging at runtime from your sketch:

```cpp
void setup() {
    Serial.begin(115200);
    SimpleOTAClient::setDebug(true);
    // ...
}
```

All messages are prefixed with `[SimpleOTAClient]` and written via `Serial.printf`. Ensure `Serial.begin()` has been called before any library method is invoked.

The runtime toggle is the recommended path. A compile-time default also exists: build with `-DSIMPLEOTA_DEBUG=1` to make logging on by default. (A `#define SIMPLEOTA_DEBUG 1` placed in your sketch will **not** work, because the library's `.cpp` is compiled in a separate translation unit that doesn't see sketch-level defines. Use `setDebug(true)` or a real build flag.)

Warnings, such as a rejected token, always print on `Serial`, regardless of this setting.

---

## Partition table

A scheme works for SimpleOTA when its CSV defines both `ota_0` and `ota_1` (see `tools/partitions/` in [Arduino-ESP32 3.3.12](https://github.com/espressif/arduino-esp32/tree/3.3.12/tools/partitions)). Choose the Partition Scheme entry in the Tools menu, or set `board_build.partitions`. On 4 MB boards, common choices include *Default 4MB with spiffs (1.2MB APP/1.5MB SPIFFS)* (`default.csv`, 1280 KB per slot), *Default 4MB with ffat (1.2MB APP/1.5MB FATFS)* (`default_ffat.csv`, 1280 KB), and *Minimal SPIFFS (1.9MB APP with OTA/128KB SPIFFS)* (`min_spiffs.csv`, 1920 KB; some boards label this *Minimal SPIFFS (Large APPS with OTA)*). *Huge APP*, *Minimal (1.3MB APP/700KB SPIFFS)* and the plain *No OTA* entries have one app slot, so `Update.begin()` fails and `apply()` reports `update_begin_failed`.

### Changing the scheme

A normal Arduino IDE or arduino-cli USB sketch upload uses esptool to write the bootloader, `partitions.bin` at `0x8000`, `boot_app0.bin` at `0xe000`, and the app, so the selected partition scheme is applied. Network OTA, including a SimpleOTA update, writes only the app image, so the scheme cannot be changed over the air. If you are moving off a single-slot scheme, enable *Erase All Flash Before Sketch Upload* for that USB flash (PlatformIO: `pio run -t erase` first) so NVS and stored build numbers reset cleanly. Boards already on a two-slot layout do not need a full erase just to upload a new sketch.

For mixed schemes in one fleet, use [`setPartitionProfile()`](#void-setpartitionprofileconst-char-profile) with matching artifact profiles. A blank `partition_profile` on the artifact or on the device matches any value for that field only; `chip_family`, `board_id`, `framework` and `hardware_revision` still filter offers.

---

## Footprint

The figures below come from building examples/SignedOTA with arduino-cli 1.5.1 and esp32:esp32@3.3.12 on the Default 4MB scheme (1280 KB slot), compared to a baseline sketch that already uses `NetworkClientSecure`, `HTTPClient` GET, `Preferences`, and `Update.begin`.

| Target | Over HTTPS baseline | Over Wi-Fi only | Free in 1280 KB slot (*Default 4MB with spiffs*) |
| --- | --- | --- | --- |
| ESP32 | +46 KB flash, +0.7 KB RAM | +184 KB flash, +4.0 KB RAM | 241 KB (19%) |
| ESP32-S3 | +45 KB flash, +0.7 KB RAM | +203 KB flash, +3.0 KB RAM | 257 KB (20%) |
| ESP32-C6 | +48 KB flash, +0.7 KB RAM | +209 KB flash, +3.6 KB RAM | 137 KB (11%) |

---

## Limitations

| Limitation | Detail |
| --- | --- |
| Hangs before `verifyRollbackLater()` | A hang in a global constructor happens before the chip watchdog starts, so nothing resets it. A crash there still rolls back. |
| One instance per device | The trial record and the boot slot belong to the device, so one `SimpleOTAClient` runs at a time. A second instance does nothing until the first is destroyed; its `check()` and `begin()` print a warning. |
| No automatic `reboot` event when `setAutoReboot(false)` | The library only emits `reboot` on the auto-reboot path it controls. Applications that drive their own restart should call `rebootForUpdate()` (which emits the event then calls `esp_restart()`) instead of `ESP.restart()` directly; see AdvancedOTA. |
| Application owns connectivity | The library is transport-agnostic and does not manage Wi-Fi, Ethernet, PPP, or reconnects. Establish a working IP connection before calling any library method, or supply an `isConnected` probe to `begin()`. |
| TLS uses bundled root CA | The bundled ISRG Root X1 cert covers current SimpleOTA infrastructure. If the platform migrates storage providers to one using a different root, a library update will be required. |

---

## Troubleshooting

| Symptom | Likely cause |
| --- | --- |
| `check()` always returns `false` | Bad token, or no active deployment targeting this device. Enable `SIMPLEOTA_DEBUG` and check the HTTP response code in the logs. |
| `Update.begin failed` | Partition table has no OTA slot, or the OTA partition is too small for the firmware being applied. |
| `OTA_CHECKSUM_FAIL` | The firmware object was replaced or corrupted on the server between the `/check/` response and the download. |
| Device is offered the same build repeatedly | The `simpleota`/`sota_build` NVS key was cleared or overwritten. See [Build numbers](#build-numbers). A `current_build_number` of `0` always matches a pending deployment, so a wiped NVS will keep re-offering the same build. |
| Download stalls or times out | Weak Wi-Fi signal, or `SIMPLEOTA_HTTP_TIMEOUT_MS` is too short for the link speed and firmware size. Increase the timeout or improve signal quality. |
| `[SimpleOTAClient] WARNING: setCACert: TLS is always verified; keeping the current CA` | `setCACert(nullptr)` was called. Verification can't be turned off. See [Security](#security). |

---

## Arduino and ESP-IDF

The [Arduino](https://github.com/xanderwasserman/SimpleOTAClient-Arduino) and [ESP-IDF](https://github.com/xanderwasserman/SimpleOTAClient-ESP-IDF) clients run the same core, so they have the same features, defaults and behaviour. Only the spelling follows each platform's style.

| What | Arduino | ESP-IDF |
|---|---|---|
| Set up | `SimpleOTAClient ota(token, ...)` and setters | `simpleota_init(&config)` |
| Managed mode | `begin(interval, isConnected, initialDelayMs)`, `end()` | `simpleota_start()`, `simpleota_stop()` |
| Check now | `checkNow()` | `simpleota_check_now()` |
| Polling mode | `check()`, `apply()` returning `OTAResult` | `simpleota_check()`, `simpleota_apply()` returning `simpleota_result_t` |
| Events | `onEvent(callback)` | `config.event_cb` |
| Confirm a trial | `confirm()` | `simpleota_confirm()` |
| Confirm by hand in managed mode | `setManagedAutoConfirm(false)` | `config.manual_confirm = true` |
| Trial state | `isTrialBoot()` | `simpleota_is_trial_boot()` |
| Confirm timeout | `setConfirmTimeout(s)` | `config.confirm_timeout_s` |
| Reboot when the app is ready | `setAutoReboot(false)`, `rebootForUpdate()` | `config.disable_auto_reboot`, `simpleota_reboot_for_update()` |
| Server URL | `setServerUrl(url)` | `config.base_url`, `simpleota_set_server_url(url)` |
| TLS trust | `setCACert(pem)` | `config.cert_pem` |
| Signing keys | `setSigningPublicKey()`, `addSigningPublicKey()` | `config.signing_keys` |
| Build and offer | `currentBuild()`, `lastOfferedVersion()` | `simpleota_current_build()`, `simpleota_last_offered_version()` |
| Logging | `setDebug(true)` | `esp_log_level_set("simpleota", ...)` |
| HTTP timeout | `-DSIMPLEOTA_HTTP_TIMEOUT_MS` | `CONFIG_SIMPLEOTA_HTTP_TIMEOUT_MS` |

---

## License

MIT License. See [LICENSE](LICENSE) for the full text.
