/**
 * Host-side unit tests for SimpleOTAWatchdog.h (stage-value maths only).
 *
 *   g++ -std=c++11 -Wall -Wextra -Werror -Isrc -o test_watchdog test/native/test_watchdog.cpp && ./test_watchdog
 */

#include <stdio.h>

#include "SimpleOTAWatchdog.h"

static int g_failures = 0;

#define CHECK(cond, name)                                        \
    do {                                                         \
        if (cond) {                                              \
            printf("PASS  %s\n", name);                          \
        } else {                                                 \
            printf("FAIL  %s (line %d)\n", name, __LINE__);      \
            ++g_failures;                                        \
        }                                                        \
    } while (0)

// Q13.19 calibration for a given slow-clock frequency.
static uint32_t cal(uint32_t hz) { return (uint32_t)((1000000ull << 19) / hz); }

int main(void) {
    bool clamped = false;

    CHECK(sotaWatchdogMarginSec(300, 60) == 60u, "300 s gets the 60 s floor");
    CHECK(sotaWatchdogMarginSec(610, 60) == 61u, "above 600 s the margin is 10 percent");
    CHECK(sotaWatchdogMarginSec(7200, 60) == 720u, "7200 s gets 720 s");

    CHECK(sotaWatchdogStageTicks(300, 60, 0, 150000, &clamped) == 54000000u && !clamped,
          "300 s + 60 s at a nominal 150 kHz");
    const uint32_t calibrated = sotaWatchdogStageTicks(300, 60, cal(150000), 0, &clamped);
    CHECK(calibrated > 53990000u && calibrated < 54010000u && !clamped,
          "the calibrated clock gives the same deadline");
    CHECK(sotaWatchdogStageTicks(26030, 60, 0, 150000, &clamped) == 4294950000u && !clamped,
          "about 7.95 h at 150 kHz still fits the 32-bit stage");
    CHECK(sotaWatchdogStageTicks(26031, 60, 0, 150000, &clamped) == UINT32_MAX && clamped,
          "one second more is clamped and reported");
    CHECK(sotaWatchdogStageTicks(86400, 60, cal(136000), 0, &clamped) == UINT32_MAX && clamped,
          "24 h at 136 kHz is clamped");
    CHECK(sotaWatchdogStageTicks(86400, 60, cal(32768), 0, &clamped) < UINT32_MAX && !clamped,
          "24 h fits with a 32.768 kHz crystal");
    CHECK(sotaWatchdogStageTicks(UINT32_MAX, 60, cal(150000), 0, &clamped) == UINT32_MAX &&
              clamped,
          "a huge timeout saturates instead of overflowing");
    CHECK(sotaWatchdogStageTicks(0, 0, 0, 0, &clamped) == 2u,
          "the stage is never shorter than 2 ticks");

    printf("===========================\n");
    if (g_failures) {
        printf("%d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
