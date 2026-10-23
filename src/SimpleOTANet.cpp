// SimpleOTAClient: the core's HTTP transport (sota_net.h) on HTTPClient.

#include "SimpleOTAClient.h"

// On Arduino-ESP32 3.x the TLS client class was renamed from WiFiClientSecure
// to NetworkClientSecure to reflect that it is transport-agnostic (works over
// Wi-Fi, Ethernet, PPP). On 2.x only WiFiClientSecure exists.
#if __has_include(<NetworkClientSecure.h>)
  #include <NetworkClientSecure.h>
  using SimpleOtaTLSClient = NetworkClientSecure;
#else
  #include <WiFiClientSecure.h>
  using SimpleOtaTLSClient = WiFiClientSecure;
#endif
#include <HTTPClient.h>
#include <new>

namespace {

struct Download {
    SimpleOtaTLSClient tls;
    HTTPClient http;
    int64_t remaining;    ///< Body bytes still to come, or -1 when unknown.
};

}  // namespace

int sota_net_post(void* user, const char* url, const char* token, const char* body,
                  char* resp, size_t cap) {
    SimpleOTAClient* self = static_cast<SimpleOTAClient*>(user);
    SimpleOtaTLSClient tls;
    tls.setCACert(self->_caCert);
    HTTPClient http;
    http.setTimeout(SIMPLEOTA_HTTP_TIMEOUT_MS);
    if (!http.begin(tls, url)) return -1;
    http.addHeader("Authorization", String("Bearer ") + token);
    http.addHeader("Content-Type", "application/json");
    const int code = http.POST(reinterpret_cast<uint8_t*>(const_cast<char*>(body)), strlen(body));
    if (code > 0 && resp && cap) strlcpy(resp, http.getString().c_str(), cap);
    http.end();
    return code > 0 ? code : -1;
}

// No Authorization header: the pre-signed URL carries its own credentials,
// and the token must not reach the storage host. A redirect is handed back
// to the core, which checks where it leads.
int sota_net_dl_open(void* user, const char* url, int64_t* len, char* location,
                     size_t location_cap) {
    SimpleOTAClient* self = static_cast<SimpleOTAClient*>(user);
    Download* d = new (std::nothrow) Download();
    if (!d) return 0;
    d->tls.setCACert(self->_caCert);
    d->http.setTimeout(SIMPLEOTA_HTTP_TIMEOUT_MS);
    d->http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    static const char* kLocation[] = {"Location"};
    d->http.collectHeaders(kLocation, 1);
    if (!d->http.begin(d->tls, url)) {
        delete d;
        return 0;
    }
    const int code = d->http.GET();
    if (code != HTTP_CODE_OK) {
        if (sota_net_is_redirect(code) && location_cap) {
            // A Location that does not fit is left empty, never cut short.
            const String to = d->http.header("Location");
            if (to.length() < location_cap) {
                memcpy(location, to.c_str(), to.length() + 1);
            } else {
                location[0] = '\0';
            }
        }
        d->http.end();
        delete d;
        return code > 0 ? code : 0;
    }
    *len = d->http.getSize();
    d->remaining = *len > 0 ? *len : -1;
    self->_download = d;
    return code;
}

// A body that ends early returns 0 too: the core then reports short_read
// when it knows the length, and the checksum catches it when it does not.
int sota_net_dl_read(void* user, uint8_t* buf, size_t cap) {
    Download* d = static_cast<Download*>(static_cast<SimpleOTAClient*>(user)->_download);
    // getStreamPtr() is WiFiClient* on 2.x and NetworkClient* on 3.x.
    auto* stream = d->http.getStreamPtr();
    if (d->remaining == 0) return 0;
    const int avail = stream->available();
    if (avail < 0) return SOTA_NET_ERROR;  // a socket error on Arduino-ESP32
    if (avail == 0) return d->http.connected() ? SOTA_NET_AGAIN : 0;  // the core times a stall
    size_t want = (size_t)avail < cap ? (size_t)avail : cap;
    if (d->remaining > 0 && (int64_t)want > d->remaining) want = (size_t)d->remaining;
    const int n = stream->readBytes(buf, want);
    if (n <= 0) return 0;
    if (d->remaining > 0) d->remaining -= n;
    return n;
}

// The download's TLS session (about 45 KB of heap) is freed before the
// core's next status post opens its own.
void sota_net_dl_close(void* user) {
    SimpleOTAClient* self = static_cast<SimpleOTAClient*>(user);
    Download* d = static_cast<Download*>(self->_download);
    if (!d) return;
    d->http.end();
    delete d;
    self->_download = nullptr;
}
