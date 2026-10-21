/**
 * SimpleOTAUrl.h - base URL for check() and status posts.
 *
 * Firmware downloads use the pre-signed URL from the server and do not
 * come through here. No Arduino types, so the host tests can include
 * this header.
 */

#ifndef SIMPLEOTA_URL_H
#define SIMPLEOTA_URL_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/** check() and status posts use this until setServerUrl() accepts a URL. */
static const char kSotaDefaultServerUrl[] = "https://simpleota.com";

/** Longest accepted server base, not counting the terminating NUL. */
static const size_t kSotaServerUrlMaxLen = 256;

enum SotaUrlStatus {
    SOTA_URL_OK = 0,
    SOTA_URL_NOT_HTTPS = 1,
    SOTA_URL_MALFORMED = 2
};

static inline bool sotaIsDigit(char c)
{
    return c >= '0' && c <= '9';
}

static inline bool sotaIsAlpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static inline bool sotaIsHex(char c)
{
    return sotaIsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static inline bool sotaEqIgnoreCase(const char* s, size_t n, const char* lit)
{
    size_t i = 0;
    for (; lit[i] != '\0'; ++i) {
        if (i >= n) return false;
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != lit[i]) return false;
    }
    return i == n;
}

/** True when url begins with https://, in any case. */
static inline bool sotaStartsWithHttps(const char* url)
{
    if (!url || strlen(url) < 8u) return false;
    return sotaEqIgnoreCase(url, 5, "https") &&
           url[5] == ':' && url[6] == '/' && url[7] == '/';
}

/** A numeric host must be a dotted IPv4 address. Leading zeros are rejected. */
static inline bool sotaIpv4Ok(const char* s, const char* end)
{
    const char* p = s;
    for (int i = 0; i < 4; ++i) {
        if (p >= end || !sotaIsDigit(*p)) return false;
        if (*p == '0' && p + 1 < end && sotaIsDigit(p[1])) return false;
        unsigned v = 0;
        int digits = 0;
        while (p < end && sotaIsDigit(*p)) {
            if (digits == 3) return false;
            v = v * 10u + (unsigned)(*p - '0');
            if (v > 255u) return false;
            ++p;
            ++digits;
        }
        if (digits == 0) return false;
        if (i < 3) {
            if (p >= end || *p != '.') return false;
            ++p;
        }
    }
    return p == end;
}

/** Hostname (LDH labels) or IPv4. An all-numeric host must be four octets. */
static inline bool sotaHostOk(const char* s, const char* end)
{
    const size_t n = (size_t)(end - s);
    if (n == 0 || n > 253u) return false;
    int labels = 0;
    bool allNumeric = true;
    const char* p = s;
    while (p < end) {
        const char* label = p;
        bool numeric = true;
        int labLen = 0;
        while (p < end && *p != '.') {
            const unsigned char c = (unsigned char)*p;
            if (!sotaIsDigit((char)c)) {
                if (!(sotaIsAlpha((char)c) || c == '-')) return false;
                numeric = false;
            }
            ++p;
            ++labLen;
            if (labLen > 63) return false;
        }
        if (labLen == 0) return false;
        if (label[0] == '-' || label[labLen - 1] == '-') return false;
        if (!numeric) allNumeric = false;
        ++labels;
        if (p < end && *p == '.') {
            ++p;
            if (p == end) return false;
        }
    }
    if (labels < 1) return false;
    if (allNumeric) {
        if (labels != 4) return false;
        return sotaIpv4Ok(s, end);
    }
    return true;
}

/** Bracketed IPv6, without a dotted IPv4 tail. */
static inline bool sotaIpv6Ok(const char* s, const char* end)
{
    if (s >= end) return false;
    int groups = 0;
    bool compressed = false;
    const char* p = s;
    if (p + 1 < end && p[0] == ':' && p[1] == ':') {
        compressed = true;
        p += 2;
        if (p == end) return true;
    } else if (*p == ':') {
        return false;
    }
    while (p < end) {
        if (*p == ':') return false;
        int digits = 0;
        while (p < end && sotaIsHex(*p) && digits < 4) {
            ++p;
            ++digits;
        }
        if (digits == 0) return false;
        if (p < end && sotaIsHex(*p)) return false;
        ++groups;
        if (p == end) break;
        if (*p != ':') return false;
        ++p;
        if (p < end && *p == ':') {
            if (compressed) return false;
            compressed = true;
            ++p;
            if (p == end) break;
        } else if (p == end) {
            return false;
        }
    }
    if (compressed) return groups < 8;
    return groups == 8;
}

static inline bool sotaPortOk(const char** pp, const char* end)
{
    const char* p = *pp;
    if (p >= end || !sotaIsDigit(*p) || *p == '0') return false;
    unsigned port = 0;
    int digits = 0;
    while (p < end && sotaIsDigit(*p)) {
        if (digits == 5) return false;
        port = port * 10u + (unsigned)(*p - '0');
        if (port > 65535u) return false;
        ++p;
        ++digits;
    }
    if (digits == 0) return false;
    *pp = p;
    return true;
}

static inline bool sotaPathByteOk(unsigned char c)
{
    if (sotaIsAlpha((char)c) || sotaIsDigit((char)c)) return true;
    switch (c) {
    case '-': case '.': case '_': case '~':
    case '!': case '$': case '&': case '\'':
    case '(': case ')': case '*': case '+':
    case ',': case ';': case '=': case ':':
        return true;
    default:
        return false;
    }
}

/** Absolute path, or empty. "." and ".." segments are rejected. */
static inline bool sotaPathOk(const char* p, const char* end)
{
    if (p == end) return true;
    if (*p != '/') return false;
    while (p < end) {
        if (*p != '/') return false;
        ++p;
        if (p == end) return true;
        const char* seg = p;
        while (p < end && *p != '/') {
            const unsigned char c = (unsigned char)*p;
            if (c == '%') {
                if ((size_t)(end - p) < 3u || !sotaIsHex(p[1]) || !sotaIsHex(p[2])) {
                    return false;
                }
                p += 3;
                continue;
            }
            if (!sotaPathByteOk(c)) return false;
            ++p;
        }
        const size_t n = (size_t)(p - seg);
        if (n == 0 ||
            (n == 1 && seg[0] == '.') ||
            (n == 2 && seg[0] == '.' && seg[1] == '.')) {
            return false;
        }
    }
    return true;
}

static inline SotaUrlStatus sotaValidateServerUrl(const char* url)
{
    if (!url || url[0] == '\0') return SOTA_URL_MALFORMED;
    const size_t len = strlen(url);
    if (len > kSotaServerUrlMaxLen) return SOTA_URL_MALFORMED;

    const char* p;
    if (sotaStartsWithHttps(url)) {
        p = url + 8;
    } else {
        const char* sep = strstr(url, "://");
        const size_t schemeLen = sep ? (size_t)(sep - url) : 0;
        if (sep && sotaEqIgnoreCase(url, schemeLen, "http")) return SOTA_URL_NOT_HTTPS;
        return SOTA_URL_MALFORMED;
    }
    const char* end = url + len;
    if (p >= end) return SOTA_URL_MALFORMED;

    if (*p == '[') {
        ++p;
        const char* bracket = p;
        while (bracket < end && *bracket != ']') ++bracket;
        if (bracket >= end || !sotaIpv6Ok(p, bracket)) return SOTA_URL_MALFORMED;
        p = bracket + 1;
    } else {
        const char* host = p;
        while (p < end && *p != ':' && *p != '/') ++p;
        if (!sotaHostOk(host, p)) return SOTA_URL_MALFORMED;
    }

    if (p < end && *p == ':') {
        ++p;
        if (!sotaPortOk(&p, end)) return SOTA_URL_MALFORMED;
    }
    if (!sotaPathOk(p, end)) return SOTA_URL_MALFORMED;
    return SOTA_URL_OK;
}

/** Token used in the setServerUrl rejection line. */
static inline const char* sotaUrlStatusName(int status)
{
    switch (status) {
    case SOTA_URL_OK:        return "ok";
    case SOTA_URL_NOT_HTTPS: return "not_https";
    default:                 return "malformed";
    }
}

/**
 * Candidate replaces current only when it validates. On rejection the
 * current pointer is returned unchanged.
 */
static inline const char* sotaSelectServerUrl(const char* current,
                                              const char* candidate,
                                              SotaUrlStatus* status)
{
    const SotaUrlStatus st = sotaValidateServerUrl(candidate);
    if (status) *status = st;
    if (st != SOTA_URL_OK) return current;
    return candidate;
}

/** configured NULL uses the default host. */
static inline const char* sotaEffectiveServerUrl(const char* configured)
{
    return configured ? configured : kSotaDefaultServerUrl;
}

/**
 * Join base and an absolute path into out. A trailing slash on the base
 * is dropped. Returns false when the base is not an accepted https URL,
 * the path is relative, or the result does not fit (including the NUL).
 */
static inline bool sotaBuildApiUrl(const char* base, const char* path,
                                   char* out, size_t outLen)
{
    if (!base || !path || path[0] != '/' || !out || outLen == 0) return false;
    if (!sotaStartsWithHttps(base)) return false;
    if (sotaValidateServerUrl(base) != SOTA_URL_OK) return false;
    size_t blen = strlen(base);
    while (blen > 8u && base[blen - 1] == '/') --blen;
    const size_t plen = strlen(path);
    if (blen > outLen - 1u) return false;
    if (plen > (outLen - 1u) - blen) return false;
    memcpy(out, base, blen);
    memcpy(out, "https://", 8);
    memcpy(out + blen, path, plen + 1u);
    return true;
}

#endif  // SIMPLEOTA_URL_H
