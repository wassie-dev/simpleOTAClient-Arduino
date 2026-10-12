/**
 * SimpleOTAWatchdog.h - the chip watchdog that resets a hung trial.
 *
 * During a SimpleOTA trial the RTC (or LP) watchdog runs on the chip's slow
 * clock, so it still fires when a hang stops the FreeRTOS confirm timer. It
 * resets the chip, and the bootloader then returns to the previous image.
 * It waits for the confirm time plus a margin: the larger of
 * SIMPLEOTA_TRIAL_WDT_MARGIN_S and 10 percent of the confirm time.
 *
 * The stage-value maths below has no Arduino or ESP-IDF types, so the host
 * tests can include it. The hardware calls live in SimpleOTAWdt.c, which is
 * C because hal/wdt_hal.h does not compile as C++ on the S2 and the C3 in
 * core 2.0.17.
 */

#ifndef SIMPLEOTA_WATCHDOG_H
#define SIMPLEOTA_WATCHDOG_H

#include <stdbool.h>
#include <stdint.h>

#ifndef SIMPLEOTA_TRIAL_WDT_MARGIN_S
#define SIMPLEOTA_TRIAL_WDT_MARGIN_S 60
#endif

/** Extra seconds on top of the confirm time. */
static inline uint32_t sotaWatchdogMarginSec(uint32_t confirmSec, uint32_t floorSec)
{
    const uint32_t tenth = confirmSec / 10u;
    return tenth > floorSec ? tenth : floorSec;
}

/**
 * Stage-0 value, in slow-clock ticks, for the confirm time plus its margin.
 * calQ13_19 is the cached slow-clock calibration (microseconds per tick,
 * Q13.19); when it is 0, nominalHz is used instead. The stage register is
 * 32 bits: *clamped is set when the deadline does not fit, and the value
 * saturates. At the usual internal slow clock that limit is about 8 hours
 * (about 13 hours on the S2).
 */
static inline uint32_t sotaWatchdogStageTicks(uint32_t confirmSec, uint32_t floorSec,
                                              uint32_t calQ13_19, uint32_t nominalHz,
                                              bool* clamped)
{
    const uint64_t seconds = (uint64_t)confirmSec + sotaWatchdogMarginSec(confirmSec, floorSec);
    uint64_t ticks;
    if (calQ13_19 != 0) {
        const uint64_t us = seconds * 1000000u;  // fits: seconds < 2^33
        ticks = us > (UINT64_MAX >> 19) ? UINT64_MAX : (us << 19) / calQ13_19;
    } else {
        ticks = seconds * (uint64_t)(nominalHz ? nominalHz : 1u);
    }
    *clamped = ticks > UINT32_MAX;
    if (*clamped) return UINT32_MAX;
    return ticks < 2 ? 2u : (uint32_t)ticks;
}

#ifdef __cplusplus
extern "C" {
#endif

/** Arm the watchdog for confirmSec plus the margin. No-op when compiled out. */
void sota_wdt_arm(uint32_t confirm_sec);
/** Stop the watchdog if this library armed it. */
void sota_wdt_disarm(void);
/** On a boot that is not a trial: stop a watchdog a reset left running. */
void sota_wdt_quiesce(void);
/** True when the armed deadline was cut to the watchdog's limit. */
bool sota_wdt_is_clamped(void);

#ifdef __cplusplus
}
#endif

#endif  // SIMPLEOTA_WATCHDOG_H
