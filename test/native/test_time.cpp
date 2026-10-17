/**
 * Host-side unit tests for SimpleOTATime.h (no Arduino, no framework).
 *
 *   g++ -std=c++11 -Wall -Wextra -Isrc -o test_time test/native/test_time.cpp && ./test_time
 */

#include <stdio.h>

#include "SimpleOTATime.h"

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

int main(void) {
    CHECK(sotaSecondsToTicks(3600, 1000) == 3600000u, "1 h at 1000 Hz");
    CHECK(sotaSecondsToTicks(21600, 1000) == 21600000u,
          "6 h at 1000 Hz does not wrap (pdMS_TO_TICKS gives about 125 s)");
    CHECK(sotaSecondsToTicks(86400, 1000) == 86400000u, "24 h at 1000 Hz");
    CHECK(sotaSecondsToTicks(86400, 100) == 8640000u, "24 h at 100 Hz");
    CHECK(sotaSecondsToTicks(0xFFFFFFFFu, 1000) == UINT32_MAX,
          "a duration past the tick range saturates");
    CHECK(sotaMillisToTicks(1000, 1000) == 1000u, "1 s in milliseconds");
    CHECK(sotaMillisToTicks(5000000, 1000) == 5000000u,
          "5,000,000 ms (about 83 min) at 1000 Hz does not wrap");
    CHECK(sotaMillisToTicks(15, 100) == 1u, "partial ticks round down");
    CHECK(sotaMillisToTicks(0, 1000) == 0u, "zero stays zero");

    printf("===========================\n");
    if (g_failures) {
        printf("%d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
