/**
 * Host-side unit tests for SimpleOTAUrl.h (no Arduino, no framework).
 *
 *   g++ -std=c++11 -Wall -Wextra -Werror -Isrc -o test_url test/native/test_url.cpp && ./test_url
 */

#include <stdio.h>
#include <string.h>

#include "SimpleOTAUrl.h"

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

static const struct {
    const char* in;
    SotaUrlStatus want;
} kUrl[] = {
    {"https://simpleota.com", SOTA_URL_OK},
    {"https://simpleota.com/", SOTA_URL_OK},
    {"https://simpleota.com/ota/", SOTA_URL_OK},
    {"https://localhost", SOTA_URL_OK},
    {"https://localhost:8443", SOTA_URL_OK},
    {"https://ota.local", SOTA_URL_OK},
    {"https://my-ota.example.co.uk", SOTA_URL_OK},
    {"https://SimpleOTA.COM", SOTA_URL_OK},
    {"https://127.0.0.1", SOTA_URL_OK},
    {"https://127.0.0.1:8443", SOTA_URL_OK},
    {"https://192.168.0.1:443/base/", SOTA_URL_OK},
    {"https://10.0.0.2", SOTA_URL_OK},
    {"https://255.255.255.255", SOTA_URL_OK},
    {"https://0.0.0.0", SOTA_URL_OK},
    {"https://[::1]", SOTA_URL_OK},
    {"https://[::1]:8443", SOTA_URL_OK},
    {"https://[::1]/ota", SOTA_URL_OK},
    {"https://[2001:db8::1]", SOTA_URL_OK},
    {"https://[2001:DB8::1]/ota/", SOTA_URL_OK},
    {"https://[1:2:3:4:5:6:7:8]", SOTA_URL_OK},
    {"https://[1:2:3:4:5:6:7::]", SOTA_URL_OK},
    {"https://[::1:2:3:4:5:6:7]", SOTA_URL_OK},
    {"https://[1::]", SOTA_URL_OK},
    {"https://[::]", SOTA_URL_OK},
    {"https://[1:2:3:4:5:6:7:8::]", SOTA_URL_MALFORMED},
    {"https://[::1:2:3:4:5:6:7:8]", SOTA_URL_MALFORMED},
    {"https://[1:2:3:4:5:6:7:8:9]", SOTA_URL_MALFORMED},
    {"https://example.com:65535", SOTA_URL_OK},
    {"https://example.com/v1.2/ota", SOTA_URL_OK},
    {"https://example.com/a+b", SOTA_URL_OK},
    {"https://xn--e1afmkfd.example", SOTA_URL_OK},
    {"http://simpleota.com", SOTA_URL_NOT_HTTPS},
    {"HTTP://simpleota.com", SOTA_URL_NOT_HTTPS},
    {"HtTp://127.0.0.1:8080", SOTA_URL_NOT_HTTPS},
    {"http://localhost/ota", SOTA_URL_NOT_HTTPS},
    {"HTTPS://simpleota.com", SOTA_URL_OK},
    {"Https://ota.example.com:8443", SOTA_URL_OK},
    {"HtTpS://localhost/ota/", SOTA_URL_OK},
    {"ftp://simpleota.com", SOTA_URL_MALFORMED},
    {"https://", SOTA_URL_MALFORMED},
    {"https:///", SOTA_URL_MALFORMED},
    {"https:///etc", SOTA_URL_MALFORMED},
    {"https://:443", SOTA_URL_MALFORMED},
    {"https://example.com:", SOTA_URL_MALFORMED},
    {"https://example.com:/api", SOTA_URL_MALFORMED},
    {"https://example.com:0", SOTA_URL_MALFORMED},
    {"https://example.com:0443", SOTA_URL_MALFORMED},
    {"https://example.com:65536", SOTA_URL_MALFORMED},
    {"https://example.com:443a", SOTA_URL_MALFORMED},
    {"https://example.com:-1", SOTA_URL_MALFORMED},
    {"https://user:pass@example.com", SOTA_URL_MALFORMED},
    {"https://example.com@evil.com", SOTA_URL_MALFORMED},
    {"https://example.com/a/../b", SOTA_URL_MALFORMED},
    {"https://example.com/a/./b", SOTA_URL_MALFORMED},
    {"https://example.com/a//b", SOTA_URL_MALFORMED},
    {"https://example.com///", SOTA_URL_MALFORMED},
    {"https://example.com?q=1", SOTA_URL_MALFORMED},
    {"https://example.com#frag", SOTA_URL_MALFORMED},
    {"https://example.com/foo bar", SOTA_URL_MALFORMED},
    {"https://ex ample.com", SOTA_URL_MALFORMED},
    {"https://256.0.0.1", SOTA_URL_MALFORMED},
    {"https://192.168.001.1", SOTA_URL_MALFORMED},
    {"https://127.0.0.1.2", SOTA_URL_MALFORMED},
    {"https://127.1", SOTA_URL_MALFORMED},
    {"https://-bad.com", SOTA_URL_MALFORMED},
    {"https://bad-.com", SOTA_URL_MALFORMED},
    {"https://example.com.", SOTA_URL_MALFORMED},
    {"https://.example.com", SOTA_URL_MALFORMED},
    {"https://example..com", SOTA_URL_MALFORMED},
    {"https://ex_ample.com", SOTA_URL_MALFORMED},
    {"https://[::1", SOTA_URL_MALFORMED},
    {"https://[::1]:", SOTA_URL_MALFORMED},
    {"https://[]", SOTA_URL_MALFORMED},
    {"https://[:::1]", SOTA_URL_MALFORMED},
    {"https://[1::2::3]", SOTA_URL_MALFORMED},
    {"https://[12345::1]", SOTA_URL_MALFORMED},
    {"https://[gggg::1]", SOTA_URL_MALFORMED},
    {"https://[fe80::1%eth0]", SOTA_URL_MALFORMED},
    {"https://example.com\\path", SOTA_URL_MALFORMED},
    {" https://example.com", SOTA_URL_MALFORMED},
};

int main(void)
{
    char out[320];
    SotaUrlStatus st;
    size_t i;

    for (i = 0; i < sizeof(kUrl) / sizeof(kUrl[0]); ++i)
        CHECK(sotaValidateServerUrl(kUrl[i].in) == kUrl[i].want, kUrl[i].in);

    CHECK(strcmp(kSotaDefaultServerUrl, "https://simpleota.com") == 0,
          "default host is https://simpleota.com");
    CHECK(sotaValidateServerUrl(kSotaDefaultServerUrl) == SOTA_URL_OK,
          "default url validates");
    CHECK(strcmp(sotaEffectiveServerUrl(NULL), "https://simpleota.com") == 0,
          "unset base is the default");
    CHECK(sotaEffectiveServerUrl(kSotaDefaultServerUrl) == kSotaDefaultServerUrl,
          "explicit default pointer is kept");
    CHECK(sotaBuildApiUrl(sotaEffectiveServerUrl(NULL), "/api/v1/ota/check/",
                          out, sizeof(out)) &&
              strcmp(out, "https://simpleota.com/api/v1/ota/check/") == 0,
          "default check url");
    CHECK(sotaBuildApiUrl(sotaEffectiveServerUrl(NULL), "/api/v1/ota/status/",
                          out, sizeof(out)) &&
              strcmp(out, "https://simpleota.com/api/v1/ota/status/") == 0,
          "default status url");

    st = SOTA_URL_OK;
    CHECK(sotaSelectServerUrl(kSotaDefaultServerUrl, "http://127.0.0.1", &st) ==
              kSotaDefaultServerUrl,
          "http does not replace the default");
    CHECK(st == SOTA_URL_NOT_HTTPS, "http status is not_https");
    CHECK(sotaBuildApiUrl(sotaEffectiveServerUrl(sotaSelectServerUrl(
                              kSotaDefaultServerUrl, "http://simpleota.com", &st)),
                          "/api/v1/ota/check/", out, sizeof(out)) &&
              strcmp(out, "https://simpleota.com/api/v1/ota/check/") == 0,
          "rejected http still targets the default");

    st = SOTA_URL_OK;
    CHECK(sotaSelectServerUrl(kSotaDefaultServerUrl, "https://", &st) ==
              kSotaDefaultServerUrl,
          "malformed does not replace the default");
    CHECK(st == SOTA_URL_MALFORMED, "empty host is malformed");

    const char* staging = "https://staging.example.com:8443";
    st = SOTA_URL_MALFORMED;
    CHECK(sotaSelectServerUrl(kSotaDefaultServerUrl, staging, &st) == staging,
          "https staging accepted");
    CHECK(st == SOTA_URL_OK, "accepted status is ok");
    CHECK(sotaBuildApiUrl(staging, "/api/v1/ota/check/", out, sizeof(out)) &&
              strcmp(out, "https://staging.example.com:8443/api/v1/ota/check/") == 0,
          "staging check url");
    st = SOTA_URL_OK;
    CHECK(sotaSelectServerUrl(staging, "http://staging.example.com", &st) == staging,
          "later http keeps the accepted url");
    CHECK(st == SOTA_URL_NOT_HTTPS, "later http status");
    st = SOTA_URL_OK;
    CHECK(sotaSelectServerUrl(staging, NULL, &st) == staging,
          "null keeps the accepted url");
    CHECK(st == SOTA_URL_MALFORMED, "null is malformed");

    CHECK(strcmp(sotaUrlStatusName(SOTA_URL_OK), "ok") == 0, "ok name");
    CHECK(strcmp(sotaUrlStatusName(SOTA_URL_NOT_HTTPS), "not_https") == 0,
          "not_https name");
    CHECK(strcmp(sotaUrlStatusName(SOTA_URL_MALFORMED), "malformed") == 0,
          "malformed name");
    CHECK(strcmp(sotaUrlStatusName(99), "malformed") == 0, "unknown status name");

    CHECK(sotaBuildApiUrl("HTTPS://SimpleOTA.COM/ota/", "/api/v1/ota/check/",
                          out, sizeof(out)) &&
              strcmp(out, "https://SimpleOTA.COM/ota/api/v1/ota/check/") == 0,
          "scheme is written lowercase, host case kept");
    {
        const char* upper = "HTTPS://staging.example.com";
        st = SOTA_URL_MALFORMED;
        CHECK(sotaSelectServerUrl(kSotaDefaultServerUrl, upper, &st) == upper,
              "uppercase https stores the caller pointer");
        CHECK(st == SOTA_URL_OK, "uppercase https status");
    }
    CHECK(!sotaBuildApiUrl("HTTP://ota.example.com", "/api/v1/ota/check/",
                           out, sizeof(out)),
          "build refuses uppercase http");
    CHECK(sotaBuildApiUrl("https://ota.example.com/", "/api/v1/ota/status/",
                          out, sizeof(out)) &&
              strcmp(out, "https://ota.example.com/api/v1/ota/status/") == 0,
          "trailing slash stripped once");
    CHECK(!sotaBuildApiUrl("https://ota.example.com///", "/api/v1/ota/check/",
                           out, sizeof(out)),
          "build rejects empty path segments");
    CHECK(sotaBuildApiUrl("https://192.168.0.1:443/base/", "/api/v1/ota/check/",
                          out, sizeof(out)) &&
              strcmp(out, "https://192.168.0.1:443/base/api/v1/ota/check/") == 0,
          "path prefix kept");
    CHECK(!sotaBuildApiUrl("http://ota.example.com", "/api/v1/ota/check/",
                           out, sizeof(out)),
          "build refuses http");
    CHECK(!sotaBuildApiUrl("https://ota.example.com", "api/v1/ota/check/",
                           out, sizeof(out)),
          "build refuses a relative path");
    CHECK(!sotaBuildApiUrl(NULL, "/api/v1/ota/check/", out, sizeof(out)),
          "build refuses a null base");
    {
        char tiny[8];
        memset(tiny, 'X', sizeof(tiny));
        CHECK(!sotaBuildApiUrl(kSotaDefaultServerUrl, "/api/v1/ota/check/",
                               tiny, sizeof(tiny)) && tiny[0] == 'X',
              "short buffer fails and is not written");
    }
    {
        const char* exact = "https://ab";
        char fit[13];
        CHECK(sotaBuildApiUrl(exact, "/c", fit, sizeof(fit)) &&
                  strcmp(fit, "https://ab/c") == 0,
              "exact fit");
        CHECK(!sotaBuildApiUrl(exact, "/c", fit, 12), "one byte short fails");
    }

    CHECK(sotaValidateServerUrl(NULL) == SOTA_URL_MALFORMED, "null url");
    CHECK(sotaValidateServerUrl("") == SOTA_URL_MALFORMED, "empty url");
    CHECK(sotaValidateServerUrl("https://example.com\r\n") == SOTA_URL_MALFORMED,
          "crlf rejected");
    CHECK(sotaValidateServerUrl("https://example.com/foo\n") == SOTA_URL_MALFORMED,
          "newline in path rejected");
    {
        char url[96];
        memcpy(url, "https://", 8);
        memset(url + 8, 'a', 63);
        url[71] = '\0';
        CHECK(sotaValidateServerUrl(url) == SOTA_URL_OK, "63-byte label");
        memset(url + 8, 'b', 64);
        url[72] = '\0';
        CHECK(sotaValidateServerUrl(url) == SOTA_URL_MALFORMED, "64-byte label");
    }
    {
        char url[300];
        memcpy(url, "https://", 8);
        for (i = 0; i < 3; ++i) {
            memset(url + 8 + i * 64, 'a', 63);
            url[71 + i * 64] = '.';
        }
        memset(url + 200, 'b', 56);
        url[256] = '\0';
        CHECK(strlen(url) == 256u && sotaValidateServerUrl(url) == SOTA_URL_OK,
              "256-byte url");
        url[256] = 'c';
        url[257] = '\0';
        CHECK(sotaValidateServerUrl(url) == SOTA_URL_MALFORMED, "257-byte url");
    }

    printf("===========================\n");
    if (g_failures) {
        printf("%d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
