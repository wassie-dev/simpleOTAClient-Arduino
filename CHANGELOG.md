# Changelog

## 1.0.0

The update logic is now [SimpleOTAClient-core](https://github.com/wassie-dev/simpleOTAClient-core), shared with the ESP-IDF client and copied into `src/sota_core`. Both clients have the same features, defaults and behaviour, and the API follows the ESP-IDF client's; the README maps each call to its ESP-IDF counterpart.

### Upgrading

- **Devices update from 0.6.0 and earlier over the air**, mid-trial too. The NVS keys and their meanings are unchanged, and a trial that fails returns to the old release, which reads the rollback as it always has. 1.0.0 adds one key, `sota_image`, which older releases ignore.
- **`begin()` no longer takes an `onResult` callback.** It is now `begin(checkIntervalSec, isConnected, initialDelayMs)`. Register `onEvent()` for outcomes: `OTA_EVENT_UPDATE_FAILED` carries the reason, `OTA_EVENT_CHECK_DONE` and `OTA_EVENT_UPDATE_AVAILABLE` the check.
- **`report()` is gone.** The library posts every status event itself; for the `reboot` event of a deferred update, call `rebootForUpdate()`.
- **`setRollbackEnabled()` is gone.** Every update runs as a trial and rolls back if it is not confirmed.
- **TLS is always verified.** `setCACert(nullptr)` no longer turns verification off; it keeps the current CA and prints a warning. To test against a local server, give it a certificate from your own CA and pass that CA to `setCACert()`.
- **Older builds are refused.** `apply()` returns the new `OTA_DOWNGRADE`, so a `switch` over `OTAResult` needs a case for it. To go back, release the old code under a new, higher build number.
- **A sketch flashed over USB reports build 0**, so the server offers it its current release. Firmware that is flashed over USB as a release can declare its build with `setImageBuild()`; see the README for when to. Before, it kept reporting the build SimpleOTA last installed.
- `check()` and `apply()` refuse to run while `begin()`'s task runs, and `setServerUrl()` refuses until `end()`. Use polling or managed mode, not both.
- Renamed, with the old names still compiling with a deprecation warning: `confirmRunning()` is `confirm()`, `isTrialInstall()` is `isTrialBoot()`, and `SIMPLEOTA_TIMEOUT_MS` is `SIMPLEOTA_HTTP_TIMEOUT_MS`.
- `SIMPLEOTA_TRIAL_WDT_MARGIN_S` is gone: the watchdog margin follows `SIMPLEOTA_HTTP_TIMEOUT_MS`.
- Warnings print on `Serial` without `setDebug(true)`.

### Added

- `onEvent()`: lifecycle events with download progress, failure reasons and the trial's outcome, the same events the ESP-IDF client's `event_cb` gets.
- `checkNow()` wakes the managed task for an immediate check, and `currentBuild()` returns the build the device reports.
- Anti-rollback: an offer whose build number is not above the running build is refused, `apply()` returns `OTA_DOWNGRADE`, and the server gets `failed` with `downgrade_refused`. An old image's signature still verifies, so this stops an old, vulnerable build being pushed.
- `setImageBuild()`: the build an image reports when it was flashed some other way than SimpleOTA, such as over USB. The running image's ELF SHA-256 is stored as `sota_image` to tell those images apart, so a device reused for another project no longer reports, and refuses offers below, the old project's build.
- `chipFamily` is optional and defaults to the chip the sketch is built for.

### Changed

- With `-DSIMPLEOTA_DISABLE_BOOTLOADER_ROLLBACK`, the chip watchdog still runs, and the library rolls back a trial itself, as the bootloader would: on any reset before `confirm()` (a crash, a watchdog, a brownout, a power loss, `ESP.restart()` or deep sleep). A rollback cut short by a power loss is finished on the next boot.
- A trial that boots after a panic or watchdog reset is rolled back, also when the bootloader resumed it.
- `end()` lets the managed task finish its current step instead of deleting it mid-download, which could cut a flash write short or leak its TLS session.
- One `SimpleOTAClient` runs at a time: a second instance does nothing, and its `check()` and `begin()` print why. The class cannot be copied, and its destructor stops the managed task.
- In polling mode, the confirm timeout rolls back from a short-lived task.
- Firmware downloads follow up to three redirects, each only to another `https://` URL (`redirect_not_https` otherwise).
- The update slot is erased as the download is written, not all at once before it, and an image larger than the slot is refused before the download.
- Server responses are read from their top-level fields only, and numbers too large for 32 bits are refused.
- Labels that are not one JSON object are left out of the check-in with a warning, instead of making every check-in invalid.
- The watchdog margin grows with `SIMPLEOTA_HTTP_TIMEOUT_MS`, so a trial can finish its current request and roll back first. `SIMPLEOTA_TRIAL_RETRY_INTERVAL_S` is fixed at 10 s.
- About 7 KB less flash for the examples (core 2.0.17, ESP32).

## 0.6.0

### Added

- `setServerUrl()` lets `check()` and status reports target another https SimpleOTA server. Firmware downloads still use the pre-signed URL from the offer, and an invalid URL is rejected while the previous one is kept.

## 0.5.0

### Upgrading

- Updates from 0.4.0 are protected by the new rollback: the trial record keeps the NVS key layout of earlier releases, and a rollback to 0.4.0 behaves as it did before.
- Polling sketches must call `confirmRunning()` within the confirm time. A sketch that first calls the library later than the confirm time plus the watchdog margin is reset by the chip watchdog and rolled back.
- Sketches that deep-sleep must call `confirmRunning()` before the first sleep after an update; waking from deep sleep before that rolls back.
- The library defines `verifyRollbackLater()`. A sketch that defines its own must build with `-DSIMPLEOTA_DISABLE_BOOTLOADER_ROLLBACK`.
- `apply()` can return two new results: `OTA_REBOOT_PENDING` and `OTA_UNCONFIRMED`.

### Added

- A SimpleOTA trial stays on probation in the bootloader until `confirmRunning()`: the library defines `verifyRollbackLater()`, which holds only SimpleOTA trials. A crash or reset at any point before confirmation, including in a global constructor, returns to the previous firmware. Build with `-DSIMPLEOTA_DISABLE_BOOTLOADER_ROLLBACK` to keep the core's default.
- A hang during a trial is reset by the chip's RTC or LP watchdog, armed before `setup()` for the confirm time plus a margin, and the bootloader returns to the previous firmware; the report says `watchdog`. It stops on `confirmRunning()` and, on core 3.x, before deep sleep. A confirm time past the watchdog's limit (about 8 h) prints a warning. Build with `-DSIMPLEOTA_DISABLE_TRIAL_WATCHDOG` to leave it out.
- `rolled_back` reports carry the reset that ended the trial (`panic`, `watchdog`, `brownout`, and so on) or `confirm_timeout`.
- An unconfirmed trial image that wakes from deep sleep goes back to the previous firmware and reports `deep_sleep`. The core's bootloader resumes the image on a deep-sleep wake, so without this a device that sleeps before confirming would get a new confirm time on every wake and never roll back.
- `apply()` returns `OTA_UNCONFIRMED` during a trial, without downloading, so the previous firmware stays available to return to.
- With `setAutoReboot(false)`, `apply()` returns `OTA_REBOOT_PENDING` while a flashed update waits for `rebootForUpdate()`, and does not download. A second download would have overwritten the waiting update.

### Changed

- `confirmed` and `rolled_back` reports go to an outbox in NVS (two slots, oldest dropped first) and are sent at the start of each `check()`. A report counts as delivered on any HTTP response, and a pending report never holds up an update.
- `confirmRunning()` tells the bootloader first and returns `false`, with the trial still running, if it cannot.

### Fixed

- The trial record is saved before `apply()` switches the boot slot. A power cut during an update no longer leaves the new image running without a trial.
- Check intervals, the initial delay, and the confirm timeout are converted to FreeRTOS ticks in 64 bits. Durations longer than about 71 minutes no longer wrap to a much shorter wait.
- A deep-sleep wake that resumes the old image after `setAutoReboot(false)` keeps the flashed update waiting instead of reporting it as rolled back.
- A trial record from a boot with rollback enabled is read correctly after rollback is disabled: a device back on the previous image reports `rolled_back` instead of confirming an update that did not run.
- A second `SimpleOTAClient` in the boot that flashed a deferred update keeps the update waiting instead of reporting a false `rolled_back`.
- `rebootForUpdate()` reports the reboot under the waiting update's deployment, also after a later `check()` has fetched a newer offer.
- The RollbackOTA example runs its health-gated `confirmRunning()`. With managed auto-confirm off and that call commented out, every update it installed rolled back.

## 0.4.0 (2026-07-12)

- On-device Ed25519 firmware signature verification for signed artifacts, with up to two pinned keys for rotation.

## 0.3.0 (2026-05-19)

- `begin()` takes an `initialDelayMs` to defer the first check, so it does not start at the same time as other TLS sessions at boot.

## 0.2.0 (2026-05-12)

- Trial install with timeout-based rollback, `confirmed` and `reboot` lifecycle events, and `rebootForUpdate()`.
- RollbackOTA example.

## 0.1.0 (2026-05-10)

- First release.
