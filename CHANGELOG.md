# Changelog

## Unreleased

- Check intervals, the initial delay, and the confirm timeout are converted to FreeRTOS ticks in 64 bits. Durations longer than about 71 minutes no longer wrap to a much shorter wait.
- With `setAutoReboot(false)`, `apply()` returns the new `OTA_REBOOT_PENDING` while a flashed update waits for `rebootForUpdate()`, and does not download. A second download would have overwritten the waiting update.
- A second `SimpleOTAClient` in the boot that flashed a deferred update keeps the update waiting instead of reporting a false `rolled_back`.
- `rebootForUpdate()` reports the reboot under the waiting update's deployment, also after a later `check()` has fetched a newer offer.
- The RollbackOTA example runs its health-gated `confirmRunning()`. With managed auto-confirm off and that call commented out, every update it installed rolled back.
- The trial record is saved before `apply()` switches the boot slot, marked by `sota_staged` until the switch happens. A power cut during an update no longer leaves the new image running without a trial. The record keeps the key layout of earlier releases, so updates from 0.4.0 and rollbacks to it behave as before.
- `confirmed` and `rolled_back` reports go to an outbox in NVS (two slots, oldest dropped first) and are sent at the start of each `check()`. A report counts as delivered on any HTTP response, and a pending report no longer has to wait for, or block, anything else.
- A deep-sleep wake that resumes the old image after `setAutoReboot(false)` keeps the flashed update waiting instead of reporting it as rolled back.
- With rollback disabled, a device that is back on the previous image reports `rolled_back` instead of confirming the update that did not run.
- A SimpleOTA trial stays on probation in the bootloader until `confirmRunning()`: the library defines `verifyRollbackLater()`, which holds only SimpleOTA trials. A crash or reset at any point before confirmation, including in a global constructor, returns to the previous firmware. Build with `-DSIMPLEOTA_DISABLE_BOOTLOADER_ROLLBACK` to keep the core's default.
- `rolled_back` reports carry the reset that ended the trial (`panic`, `watchdog`, `brownout`, and so on) or `confirm_timeout`.
- An unconfirmed trial image that wakes from deep sleep goes back to the previous firmware and reports `deep_sleep`. The core's bootloader resumes the image on a deep-sleep wake, so without this a device that sleeps before confirming would get a new confirm time on every wake and never roll back.
- `confirmRunning()` tells the bootloader first and returns `false`, with the trial still running, if it cannot.
- `apply()` returns the new `OTA_UNCONFIRMED` during a trial, without downloading, so the previous firmware stays available to return to.
