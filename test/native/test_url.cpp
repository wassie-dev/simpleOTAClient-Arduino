/**
 * Host-side unit tests for SimpleOTAUrl.h (no Arduino, no framework).
 *
 *   g++ -std=c++11 -Wall -Wextra -Werror -Isrc -o test_url test/native/test_url.cpp && ./test_url
 */

#include <stdio.h>
#include <string.h>

#include "SimpleOTAUrl.h"

static int g_failures = 0;

#define CHECK(cond, name)                                   \
    do {                                                    \
        if (cond) {                                         \
            printf("PASS  %s\n", name);                     \
        } else {                                            \
            printf("FAIL  %s (line %d)\n", name, __LINE__); \
            ++g_failures;                                   \
        }                                                   \
    } while (0)

static const struct {
    const char* in;
    SotaUrlStatus want;
} kUrl[] = {
    {"HTTPS://user:pass@ota.example:8443/a/../b", SOTA_URL_OK},
    {"HTTP://127.0.0.1", SOTA_URL_NOT_HTTPS},
    {"https://", SOTA_URL_MALFORMED},
    {"https:///", SOTA_URL_MALFORMED},
    {"ftp://simpleota.com", SOTA_URL_MALFORMED},
    {"https://a b", SOTA_URL_MALFORMED},
    {"https://ota\texample.com", SOTA_URL_MALFORMED},
    {"https://h\x7f", SOTA_URL_MALFORMED},
    {"", SOTA_URL_MALFORMED},
};

int main(void)
{
    size_t i;
    for (i = 0; i < sizeof(kUrl) / sizeof(kUrl[0]); ++i)
        CHECK(sotaValidateServerUrl(kUrl[i].in) == kUrl[i].want, kUrl[i].in);

    CHECK(sotaValidateServerUrl(NULL) == SOTA_URL_MALFORMED, "null");
    CHECK(strcmp(kSotaDefaultServerUrl, "https://simpleota.com") == 0, "default");
    CHECK(sotaValidateServerUrl(kSotaDefaultServerUrl) == SOTA_URL_OK, "default ok");
    CHECK(strcmp(sotaUrlStatusName(SOTA_URL_NOT_HTTPS), "not_https") == 0, "not_https");
    CHECK(strcmp(sotaUrlStatusName(SOTA_URL_MALFORMED), "malformed") == 0, "malformed");

    {
        char url[300];
        memcpy(url, "https://", 8);
        memset(url + 8, 'a', 248);
        url[256] = '\0';
        CHECK(strlen(url) == 256u && sotaValidateServerUrl(url) == SOTA_URL_OK, "256");
        url[256] = 'b';
        url[257] = '\0';
        CHECK(sotaValidateServerUrl(url) == SOTA_URL_MALFORMED, "257");
    }

    SotaUrlStatus st = SOTA_URL_OK;
    const char* cur = kSotaDefaultServerUrl;
    CHECK(sotaSelectServerUrl(cur, "http://h", &st) == cur && st == SOTA_URL_NOT_HTTPS,
          "rejected http keeps the previous url");
    const char* next = "https://staging.example";
    CHECK(sotaSelectServerUrl(cur, next, &st) == next && st == SOTA_URL_OK,
          "accepted url replaces the previous");

    char buf[16];
    memset(buf, 'X', sizeof(buf));
    CHECK(sotaBuildApiUrl("HTTPS://ab/", "/c", buf, 13) && strcmp(buf, "HTTPS://ab/c") == 0,
          "join");
    memset(buf, 'X', sizeof(buf));
    CHECK(!sotaBuildApiUrl("HTTPS://ab/", "/c", buf, 12) && buf[0] == 'X', "join overflow");

    if (g_failures) {
        printf("%d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
