/*
 * Trial watchdog, in C. hal/wdt_hal.h does not compile as C++ on the S2
 * and the C3 in core 2.0.17.
 */

#include "sdkconfig.h"
#include "SimpleOTARollback.h"

#include <stdbool.h>
#include <stdint.h>

#ifndef SIMPLEOTA_TRIAL_WDT_MARGIN_S
#define SIMPLEOTA_TRIAL_WDT_MARGIN_S 60
#endif

#if (defined(CONFIG_APP_ROLLBACK_ENABLE) || defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE)) \
    && !defined(SIMPLEOTA_DISABLE_BOOTLOADER_ROLLBACK) \
    && !defined(SIMPLEOTA_DISABLE_TRIAL_WATCHDOG) \
    && __has_include("hal/wdt_hal.h")
#define SOTA_WDT_C 1
#include "hal/wdt_hal.h"
#include "soc/rtc.h"
#include "esp_idf_version.h"
#include "esp_sleep.h"
#if __has_include("esp_private/esp_clk.h")
#include "esp_private/esp_clk.h"
#define SOTA_HAVE_SLOWCLK_CAL 1
#else
#define SOTA_HAVE_SLOWCLK_CAL 0
#endif
/* esp_deep_sleep_register_hook() needs ESP-IDF 5.1 or later (Arduino core 3.x). */
#define SOTA_HAVE_DEEP_SLEEP_HOOK (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 0))
#else
#define SOTA_WDT_C 0
#endif

static bool s_armed = false;
static bool s_clamped = false;

static void sota_wdt_hw_off(void) {
#if SOTA_WDT_C
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

#if SOTA_WDT_C
/* Slow-clock ticks for the deadline plus margin. Sets s_clamped when the
   32-bit stage cannot hold the whole deadline. */
static uint32_t sota_wdt_ticks(uint32_t confirm_sec) {
    uint32_t margin = sotaWatchMarginSec(confirm_sec, SIMPLEOTA_TRIAL_WDT_MARGIN_S);
    uint64_t total_sec = (uint64_t)confirm_sec + (uint64_t)margin;
    uint64_t ticks = 0;
#if SOTA_HAVE_SLOWCLK_CAL
    ticks = sotaSlowclkTicks(total_sec * 1000000ull, esp_clk_slowclk_cal_get());
#endif
    if (ticks == 0) {
        ticks = (uint64_t)sotaNominalSlowTicks(total_sec, rtc_clk_slow_freq_get_hz());
    }
    s_clamped = ticks >= 0xFFFFFFFFu;
    if (s_clamped) return 0xFFFFFFFFu;
    if (ticks < 2) return 2;
    return (uint32_t)ticks;
}

#if SOTA_HAVE_DEEP_SLEEP_HOOK
/* A deep-sleep wake is a new boot, and verifyRollbackLater() arms the
   watchdog again if the image is still on trial. Stop it before sleeping
   so a long sleep does not end in a watchdog reset. */
static void sota_wdt_before_deep_sleep(void) {
    sota_wdt_disarm();
}
#endif
#endif

void sota_wdt_arm(uint32_t confirm_sec) {
#if SOTA_WDT_C
    wdt_hal_context_t hal;
    wdt_hal_init(&hal, WDT_RWDT, 0, false);
    wdt_hal_write_protect_disable(&hal);
    wdt_hal_config_stage(&hal, WDT_STAGE0, sota_wdt_ticks(confirm_sec),
                         WDT_STAGE_ACTION_RESET_RTC);
    wdt_hal_set_flashboot_en(&hal, false);
    wdt_hal_enable(&hal);
    wdt_hal_write_protect_enable(&hal);
    s_armed = true;
#if SOTA_HAVE_DEEP_SLEEP_HOOK
    /* Registering the same hook again is a no-op. If the hook table is
       full, the README already asks for confirmRunning() before sleeping. */
    (void)esp_deep_sleep_register_hook(&sota_wdt_before_deep_sleep);
#endif
#else
    (void)confirm_sec;
    s_armed = false;
#endif
}

void sota_wdt_disarm(void) {
    if (!s_armed) return;
    sota_wdt_hw_off();
}

/* Called on every boot that is not a held trial. With the bootloader
   watchdog enabled, the bootloader has already replaced whatever this
   library armed before the reset, and the startup code or the sketch owns
   that watchdog now, so leave it alone. Without it, a watchdog armed before
   a software restart can still be counting, so stop it. */
void sota_wdt_quiesce(void) {
#if defined(CONFIG_BOOTLOADER_WDT_ENABLE)
    s_armed = false;
    s_clamped = false;
#else
    sota_wdt_hw_off();
#endif
}

bool sota_wdt_is_armed(void) {
    return s_armed;
}

bool sota_wdt_is_clamped(void) {
    return s_armed && s_clamped;
}
