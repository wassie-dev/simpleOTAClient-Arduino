# Changelog

## Unreleased

- Check intervals, the initial delay, and the confirm timeout are converted to FreeRTOS ticks in 64 bits. Durations longer than about 71 minutes no longer wrap to a much shorter wait.
- With `setAutoReboot(false)`, `apply()` returns the new `OTA_REBOOT_PENDING` while a flashed update waits for `rebootForUpdate()`, and does not download. A second download would have overwritten the waiting update.
- A second `SimpleOTAClient` in the boot that flashed a deferred update keeps the update waiting instead of reporting a false `rolled_back`.
- `rebootForUpdate()` reports the reboot under the waiting update's deployment, also after a later `check()` has fetched a newer offer.
- The RollbackOTA example runs its health-gated `confirmRunning()`. With managed auto-confirm off and that call commented out, every update it installed rolled back.
