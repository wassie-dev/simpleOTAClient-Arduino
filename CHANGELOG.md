# Changelog

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
