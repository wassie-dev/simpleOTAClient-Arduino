/*
 * Trial watchdog hardware calls, in C: hal/wdt_hal.h does not compile as
 * C++ on the S2 and the C3 in core 2.0.17. See SimpleOTAWatchdog.h.
 */

#include "sdkconfig.h"
#include "SimpleOTAWatchdog.h"

#include <stdbool.h>
#include <stdint.h>

/* Built only with bootloader rollback (the watchdog's reset is undone by it)
 * and without -DSIMPLEOTA_DISABLE_TRIAL_WATCHDOG. Both flags must reach the
 * C compiler too, for example through build_opt.h. */
#if (defined(CONFIG_APP_ROLLBACK_ENABLE) || defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE)) \
    && !defined(SIMPLEOTA_DISABLE_BOOTLOADER_ROLLBACK) \
    && !defined(SIMPLEOTA_DISABLE_TRIAL_WATCHDOG) \
    && __has_include("hal/wdt_hal.h")
#define SOTA_WDT_HW 1
#include "hal/wdt_hal.h"
#include "soc/rtc.h"
#include "esp_idf_version.h"
#include "esp_sleep.h"
#if __has_include("esp_private/esp_clk.h")
#include "esp_private/esp_clk.h"
#define SOTA_SLOWCLK_CAL() esp_clk_slowclk_cal_get()
#else
#define SOTA_SLOWCLK_CAL() 0u
#endif
/* esp_deep_sleep_register_hook() needs ESP-IDF 5.1 or later (core 3.x). */
#define SOTA_HAVE_DEEP_SLEEP_HOOK (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 0))
#else
#define SOTA_WDT_HW 0
#endif

static bool s_armed = false;
static bool s_clamped = false;

static void sota_wdt_hw_off(void) {
#if SOTA_WDT_HW
    wdt_hal_context_t hal;
    wdt_hal_init(&hal, WDT_RWDT, 0, false);
    wdt_hal_write_protect_disable(&hal);
    wdt_hal_set_flashboot_en(&hal, false);
    wdt_hal_disable(&hal);
    wdt_hal_write_protect_enable(&hal);
#endif
    s_armed = false;
    s_clamped = false;
}

#if SOTA_WDT_HW && SOTA_HAVE_DEEP_SLEEP_HOOK
/* Stop the watchdog before deep sleep, so a long sleep does not end in a
 * watchdog reset. Waking before the trial was confirmed rolls back anyway,
 * reported as deep_sleep (SOTA_BOOT_SLEPT_IN_TRIAL). */
static void sota_wdt_before_deep_sleep(void) {
    sota_wdt_disarm();
}
#endif

void sota_wdt_arm(uint32_t confirm_sec) {
#if SOTA_WDT_HW
    bool clamped = false;
    const uint32_t ticks = sotaWatchdogStageTicks(
        confirm_sec, SIMPLEOTA_TRIAL_WDT_MARGIN_S, SOTA_SLOWCLK_CAL(),
        rtc_clk_slow_freq_get_hz(), &clamped);
    wdt_hal_context_t hal;
    wdt_hal_init(&hal, WDT_RWDT, 0, false);
    wdt_hal_write_protect_disable(&hal);
    wdt_hal_config_stage(&hal, WDT_STAGE0, ticks, WDT_STAGE_ACTION_RESET_RTC);
    wdt_hal_set_flashboot_en(&hal, false);
    wdt_hal_enable(&hal);
    wdt_hal_write_protect_enable(&hal);
    s_armed = true;
    s_clamped = clamped;
#if SOTA_HAVE_DEEP_SLEEP_HOOK
    /* Registering the same hook again is a no-op. */
    (void)esp_deep_sleep_register_hook(&sota_wdt_before_deep_sleep);
#endif
#else
    (void)confirm_sec;
#endif
}

void sota_wdt_disarm(void) {
    if (s_armed) sota_wdt_hw_off();
}

/* With the bootloader watchdog enabled (the stock setting), the bootloader
 * reprograms the RTC watchdog on every reset and the startup code or the
 * sketch owns it afterwards, so leave it alone. Without it, a watchdog
 * armed before a software restart can still be counting. */
void sota_wdt_quiesce(void) {
#if SOTA_WDT_HW && !defined(CONFIG_BOOTLOADER_WDT_ENABLE)
    sota_wdt_hw_off();
#endif
}

bool sota_wdt_is_clamped(void) {
    return s_armed && s_clamped;
}
