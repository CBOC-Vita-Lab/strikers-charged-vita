// Request table of the signaling's HTTP client: routes https:// to np_https and http:// to the plain desktop client (np_http_plain.cpp).
#include "platform/sdk/net/online/np_http.h"
#include "platform/sdk/net/online/np_service_hosts.h"
#include "platform/sdk/net/online/np_reqlog.h"

#if defined(__has_include)
#if __has_include("platform/sdk/net/np_https.h")
#define NP_HAVE_NPHTTPS 1
#include "platform/sdk/net/np_https.h"
#endif
#endif

#include <stdio.h>
#include <string.h>

namespace NpNet {

#if !defined(NP_TARGET_VITA)
void* PlainStart(const char* method, const std::string& url, const std::string& body, const std::vector<std::string>& headers, uint32_t timeoutMs, std::string* err);
int PlainPoll(void* request, HttpResponse* out);
void PlainCancel(void* request);
#endif

namespace {

struct Slot {
    int handle;       // 0 = free
    bool tls;
    bool fired;       // nobody asks about it: HttpPump reaps it
    void* plain;
    uint32_t tlsHandle;
    // what a fallback to the other service host needs (np_service_hosts.h): the request as the caller wrote it
    bool toNew;       // went to the new service host: a failure before the connection sends it again to the old one
    std::string method, body, url;
    std::string sentUrl;   // the address this attempt went to (the request log; np_reqlog.h)
    uint64_t startMs;
    std::vector<std::string> headers;
    uint32_t timeoutMs;
    uint32_t ioTimeoutMs;   // 0: the HTTPS default
    Slot() : handle(0), tls(false), fired(false), plain(0), tlsHandle(0), toNew(false), startMs(0), timeoutMs(0), ioTimeoutMs(0) {}
};
const int kSlots = 12;
Slot g_slots[kSlots];
int g_next = 1;
// The game thread is the only caller (a frame's worth of network work); no locks.

Slot* Find(int handle) {
    for (int i = 0; i < kSlots; ++i)
        if (g_slots[i].handle == handle) return &g_slots[i];
    return 0;
}

#if defined(NP_HAVE_NPHTTPS)
bool StartTls(Slot* s, const std::string& url) {
    std::vector<const char*> hv;
    for (size_t i = 0; i < s->headers.size(); ++i) hv.push_back(s->headers[i].c_str());
    hv.push_back(0);
    NpHttpsRequest rq;
    memset(&rq, 0, sizeof rq);
    rq.url = url.c_str();
    rq.method = s->method.c_str();
    rq.body = s->body.empty() ? 0 : s->body.data();
    rq.body_len = s->body.size();
    rq.headers = &hv[0];
    rq.total_timeout_ms = s->timeoutMs;
    rq.io_timeout_ms = s->ioTimeoutMs;
    rq.max_response = 32 * 1024;
    rq.keep_alive = 1;
    NpHttpsError e = NP_HTTPS_OK;
    const uint32_t h = np_https_start(&rq, &e);
    if (!h) return false;
    s->tls = true;
    s->tlsHandle = h;
    s->sentUrl = url;
    s->startMs = ServiceHosts::Now();
    return true;
}
#endif

int PollSlot(Slot* s, HttpResponse* out) {
#if defined(NP_HAVE_NPHTTPS)
    if (s->tls) {
        NpHttpsResult r;
        memset(&r, 0, sizeof r);
        const int rc = np_https_poll(s->tlsHandle, &r);
        if (rc == 0) return 0;
        if (rc < 0) {   // not a live handle any more: the slot would never be freed (nobody polls a dead handle again)
            np_https_release(s->tlsHandle);
            s->handle = 0;
            return -1;
        }
        const bool answered = r.error == NP_HTTPS_OK || r.error == NP_HTTPS_E_HTTP_STATUS;
        if (s->toNew && ServiceHosts::ShouldFallBack(s->url, true, answered ? 0 : int(r.error), int(r.stage), r.body, r.body_len)) {
            if (answered) fprintf(stderr, "[np-net] service host: answered by the zone (HTTP %d), trying the other address\n", r.http_status);
            else fprintf(stderr, "[np-net] service host: %s (%s), trying the other address\n", np_https_error_name(r.error), np_https_stage_name(r.stage));
            if (answered) ReqLog::Line(s->method.c_str(), s->sentUrl, r.http_status, 0, 0, r.elapsed_ms, r.reused_connection, 0, 0);   // the attempt that fell back is logged too
            else ReqLog::Line(s->method.c_str(), s->sentUrl, 0, np_https_error_name(r.error), r.message, r.elapsed_ms, 0, 0, 0);
            np_https_release(s->tlsHandle);
            s->toNew = false;
            if (StartTls(s, ServiceHosts::SwapToOld(s->url))) return 0;   // pending again, on the old address
            out->error = 1;
            out->text = "The online service could not be reached.";
            s->handle = 0;
            return 1;
        }
        if (answered) {
            out->error = 0;
            out->status = r.http_status;
            out->body.assign(reinterpret_cast<const char*>(r.body), r.body_len);
            ReqLog::Line(s->method.c_str(), s->sentUrl, r.http_status, 0, 0, r.elapsed_ms, r.reused_connection, out->body.data(), out->body.size());
        } else {
            ReqLog::Line(s->method.c_str(), s->sentUrl, 0, np_https_error_name(r.error), r.message, r.elapsed_ms, 0, 0, 0);
            out->error = int(r.error);
            out->status = r.http_status;
            out->text = np_https_error_text(r.error);
            out->body.clear();
            fprintf(stderr, "[np-net] https request failed: %s (%s; stage %s, %s)\n", np_https_error_name(r.error), r.message, np_https_stage_name(r.stage),
                    r.error == NP_HTTPS_E_CERT ? r.cert_detail : "");
        }
        np_https_release(s->tlsHandle);
        s->handle = 0;
        return 1;
    }
#endif
#if !defined(NP_TARGET_VITA)
    if (!s->tls) {
        const int rc = PlainPoll(s->plain, out);
        if (rc == 1) {
            s->handle = 0;
            const uint32_t ms = uint32_t(ServiceHosts::Now() - s->startMs);
            if (out->error) ReqLog::Line(s->method.c_str(), s->sentUrl, 0, "transport", out->text.c_str(), ms, 0, 0, 0);
            else ReqLog::Line(s->method.c_str(), s->sentUrl, out->status, 0, 0, ms, 0, out->body.data(), out->body.size());
        }
        return rc;
    }
#endif
    (void)out;
    s->handle = 0;
    return -1;
}

}  // namespace

int HttpStart(const char* method, const std::string& url, const std::string& body, const std::vector<std::string>& headers, uint32_t timeoutMs, std::string* err) {
    return HttpStart(method, url, body, headers, timeoutMs, 0, err);
}

int HttpStart(const char* method, const std::string& url, const std::string& body, const std::vector<std::string>& headers, uint32_t timeoutMs,
              uint32_t ioTimeoutMs, std::string* err) {
    Slot* s = 0;
    for (int i = 0; i < kSlots && !s; ++i)
        if (!g_slots[i].handle) s = &g_slots[i];
    if (!s) { *err = "Too many requests at once."; return 0; }
    const bool tls = url.compare(0, 8, "https://") == 0;
    s->toNew = false;
    if (tls) {
#if defined(NP_HAVE_NPHTTPS)
        s->method = method ? method : "";
        s->body = body;
        s->headers = headers;
        s->timeoutMs = timeoutMs;
        s->ioTimeoutMs = ioTimeoutMs;
        s->url = url;
        const std::string routed = ServiceHosts::Route(url);   // the old address while the new one is known to be down
        s->toNew = ServiceHosts::FallbackApplies(url) && routed == url;
        NpHttpsError e = NP_HTTPS_OK;
        {
            std::vector<const char*> hv;
            for (size_t i = 0; i < headers.size(); ++i) hv.push_back(headers[i].c_str());
            hv.push_back(0);
            NpHttpsRequest rq;
            memset(&rq, 0, sizeof rq);
            rq.url = routed.c_str();
            rq.method = method;
            rq.body = body.empty() ? 0 : body.data();
            rq.body_len = body.size();
            rq.headers = &hv[0];
            rq.total_timeout_ms = timeoutMs;
            rq.io_timeout_ms = ioTimeoutMs;
            rq.max_response = 32 * 1024;
            rq.keep_alive = 1;
            if (s->toNew) rq.connect_timeout_ms = ServiceHosts::kNewHostConnectMs;   // a dead new address must leave time for the old one
            const uint32_t h = np_https_start(&rq, &e);
            if (!h) { *err = np_https_error_text(e); return 0; }
            s->tls = true;
            s->tlsHandle = h;
            s->sentUrl = routed;
            s->startMs = ServiceHosts::Now();
        }
#else
        *err = "Secure connections are not available in this build.";
        return 0;
#endif
    } else {
#if !defined(NP_TARGET_VITA)
        s->plain = PlainStart(method, url, body, headers, timeoutMs, err);
        if (!s->plain) return 0;
        s->tls = false;
        s->method = method ? method : "";
        s->sentUrl = url;
        s->startMs = ServiceHosts::Now();
#else
        *err = "Only secure (https) addresses are allowed.";
        return 0;
#endif
    }
    s->fired = false;
    s->handle = g_next++;
    if (g_next > 0x3fffffff) g_next = 1;
    return s->handle;
}

int HttpPoll(int handle, HttpResponse* out) {
    Slot* s = Find(handle);
    if (!s) return -1;
    return PollSlot(s, out);
}

void HttpCancel(int handle) {
    Slot* s = Find(handle);
    if (!s) return;
#if defined(NP_HAVE_NPHTTPS)
    if (s->tls) np_https_release(s->tlsHandle);
#endif
#if !defined(NP_TARGET_VITA)
    if (!s->tls) PlainCancel(s->plain);
#endif
    s->handle = 0;
}

void HttpFire(const char* method, const std::string& url, const std::string& body, const std::vector<std::string>& headers, uint32_t timeoutMs) {
    std::string err;
    const int h = HttpStart(method, url, body, headers, timeoutMs, &err);
    if (!h) { fprintf(stderr, "[np-net] fire-and-forget %s not sent: %s\n", method ? method : "?", err.c_str()); return; }   // (never silent: a queue leave that is lost keeps a ticket alive)
    Find(h)->fired = true;
}

void HttpPump() {
    for (int i = 0; i < kSlots; ++i) {
        Slot& s = g_slots[i];
        if (s.handle && s.fired) {
            HttpResponse r;
            PollSlot(&s, &r);
        }
    }
}

}  // namespace NpNet
