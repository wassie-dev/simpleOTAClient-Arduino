/**
 * SimpleOTATime.h - duration to FreeRTOS tick conversion.
 *
 * pdMS_TO_TICKS() multiplies in 32 bits, so at the 1000 Hz tick rate of
 * Arduino-ESP32 any duration past 4294 seconds wraps to a much shorter
 * one. These helpers multiply in 64 bits and saturate instead. Every
 * delay and timer period in the library goes through them.
 *
 * No Arduino or ESP-IDF types, so the host tests can include this header.
 */

#ifndef SIMPLEOTA_TIME_H
#define SIMPLEOTA_TIME_H

#include <stdint.h>

/** Ticks for a duration in milliseconds, saturated to UINT32_MAX. */
static inline uint32_t sotaMillisToTicks(uint64_t millis, uint32_t tickHz)
{
    const uint64_t ticks = millis * (uint64_t)tickHz / 1000u;
    return ticks > UINT32_MAX ? UINT32_MAX : (uint32_t)ticks;
}

/** Ticks for a duration in seconds, saturated to UINT32_MAX. */
static inline uint32_t sotaSecondsToTicks(uint32_t seconds, uint32_t tickHz)
{
    return sotaMillisToTicks((uint64_t)seconds * 1000u, tickHz);
}

#endif  // SIMPLEOTA_TIME_H
