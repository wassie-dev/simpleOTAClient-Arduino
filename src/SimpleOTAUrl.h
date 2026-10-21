/**
 * Base URL for check() and status posts. Downloads use the pre-signed URL.
 */
#ifndef SIMPLEOTA_URL_H
#define SIMPLEOTA_URL_H
#include <stddef.h>
#include <string.h>

static const char kSotaDefaultServerUrl[] = "https://simpleota.com";
static const size_t kSotaServerUrlMaxLen = 256;
enum SotaUrlStatus { SOTA_URL_OK = 0, SOTA_URL_NOT_HTTPS = 1, SOTA_URL_MALFORMED = 2 };

static inline bool sotaHasScheme(const char* url, const char* scheme)
{
    for (; *scheme; ++url, ++scheme) {
        char c = *url >= 'A' && *url <= 'Z' ? (char)(*url - 'A' + 'a') : *url;
        if (c != *scheme) return false;
    }
    return true;
}

static inline SotaUrlStatus sotaValidateServerUrl(const char* url)
{
    if (!url || !url[0]) return SOTA_URL_MALFORMED;
    const size_t len = strlen(url);
    if (len > kSotaServerUrlMaxLen) return SOTA_URL_MALFORMED;
    for (size_t i = 0; i < len; ++i) {
        const unsigned char c = (unsigned char)url[i];
        if (c <= 0x20u || c == 0x7fu) return SOTA_URL_MALFORMED;
    }
    if (sotaHasScheme(url, "http://") && !sotaHasScheme(url, "https://"))
        return SOTA_URL_NOT_HTTPS;
    if (!sotaHasScheme(url, "https://")) return SOTA_URL_MALFORMED;
    const char* slash = strchr(url + 8, '/');
    return (slash ? (size_t)(slash - url) : len) == 8u ? SOTA_URL_MALFORMED : SOTA_URL_OK;
}

static inline const char* sotaUrlStatusName(int status)
{
    if (status == SOTA_URL_NOT_HTTPS) return "not_https";
    return status == SOTA_URL_OK ? "ok" : "malformed";
}

static inline const char* sotaSelectServerUrl(const char* current, const char* candidate,
                                              SotaUrlStatus* status)
{
    const SotaUrlStatus st = sotaValidateServerUrl(candidate);
    if (status) *status = st;
    return st == SOTA_URL_OK ? candidate : current;
}

static inline bool sotaBuildApiUrl(const char* base, const char* path, char* out, size_t outLen)
{
    if (!base || !path || !out || outLen == 0) return false;
    size_t n = strlen(base);
    while (n > 8u && base[n - 1] == '/') --n;
    const size_t plen = strlen(path);
    if (n >= outLen || plen > (outLen - 1u) - n) return false;
    memcpy(out, base, n);
    memcpy(out + n, path, plen + 1u);
    return true;
}
#endif
