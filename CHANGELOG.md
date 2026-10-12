# Changelog

## 0.5.0

- A new SimpleOTA firmware stays unconfirmed until `confirmRunning()` runs, or until managed mode confirms it after the first successful check. A crash, a reset, or a hang during that time switches the board back to the previous firmware. Hangs are caught by a watchdog on the chip's slow clock, started before `setup()`.
- Polling sketches must call `confirmRunning()` within the confirm time. A sketch that first calls the library hours after boot is reset by the chip watchdog and switched back. AdvancedOTA calls `confirmRunning()` from `setup()`, BasicOTA at the top of `loop()`, and RollbackOTA once its health check passes.
- `apply()` returns `OTA_UNCONFIRMED` while the running firmware is unconfirmed. It does not send a `failed` event, and the offer is kept.
- With `setAutoReboot(false)`, `apply()` returns `OTA_REBOOT_PENDING` while a flashed update waits for `rebootForUpdate()`, and does not download. A second download could overwrite the waiting update, and if it was cut off, the next boot reported that update as rolled back.
- While a `rolled_back` report is waiting for the server's answer, `apply()` returns `OTA_ROLLBACK_PENDING`. It keeps the offer, flashes nothing, and reports nothing. The report is retried while there is no answer (no response, a 5xx, 401, 403, 408, or 429) and dropped once the server accepts it or rejects it for good (`accepted: false` or another 4xx), so a rejected report cannot block updates.
- While an update waits for `rebootForUpdate()`, `check()` takes no new offer, and `report()` and `rebootForUpdate()` keep using the flashed deployment.
- An accepted `confirmed` report clears its NVS keys only while they still name that deployment.
- `rolled_back` reports carry the reason saved when the rollback was detected: `confirm_timeout`, or the reset reason such as `panic`, `watchdog`, or `brownout`.
- Images written by ArduinoOTA or HTTPUpdate are accepted by the Arduino core on boot, because this library only holds a firmware unconfirmed when it wrote its own trial record.
- `-DSIMPLEOTA_DISABLE_BOOTLOADER_ROLLBACK` and `-DSIMPLEOTA_DISABLE_TRIAL_WATCHDOG` turn the bootloader switch-back and the chip watchdog off. See the README section Opting out.

## 0.4.0

On-device Ed25519 signature verification for signed artifacts.

## 0.3.0

`begin()` takes `initialDelayMs` to defer the first check after boot.

## 0.2.0

Library-managed trial install with a confirm timeout.

## 0.1.0

Initial release.
