// The service client on the game's TLS layer (platform/sdk/net/np_https.h, branch w12-mptls): the HttpTransport behind NpOnlineCreateHttpApi.
// Compiled only when np_https.h is part of the tree. The service URL comes from MSC_ONLINE_URL (environment / env.txt) or the build's
// NP_ONLINE_URL; without one there is no HTTP API (the "unavailable" API answers, and the game shows its connection error).
#if defined(__has_include)
#if __has_include("platform/sdk/net/np_https.h")

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/sdk/dwc/np_online_http.h"
#include "platform/sdk/net/np_https.h"
#include "platform/sdk/net/online/np_service_hosts.h"
#include "platform/sdk/net/online/np_reqlog.h"
#include "platform/sdk/net/online/np_service_url.h"
#include <map>
#if !defined(NP_TARGET_VITA)
#include "platform/sdk/net/online/np_http.h"
#endif

namespace NpOnline {
namespace {

#if !defined(NP_TARGET_VITA)
// Desktop builds only (1.3): the local fake of the service speaks plain HTTP (server/fake), as the menu flow's signaling already allows there.
// The same requests as the HTTPS transport, through the signaling's HTTP client (np_http.h); the console never compiles this.
class PlainTransport : public HttpTransport {
public:
    explicit PlainTransport(const std::string& base) : m_base(base) {}
    uint32_t Start(const HttpRequest& request) override {
        std::string err;
        const int h = NpNet::HttpStart(request.method.c_str(), m_base + request.path, request.body, request.headers, request.timeoutMs, request.ioTimeoutMs, &err);
        if (!h) fprintf(stderr, "[online] request not started: %s\n", err.c_str());
        return uint32_t(h > 0 ? h : 0);
    }
    bool Poll(uint32_t id, HttpResponse* out) override {
        NpNet::HttpResponse r;
        const int st = NpNet::HttpPoll(int(id), &r);
        if (st == 0) return false;
        *out = HttpResponse();
        if (st < 0) { out->fail = Fail::Cancelled; return true; }
        if (r.error) { out->fail = Fail::Transport; out->detail = r.text; return true; }
        out->status = r.status;
        out->body = r.body;
        return true;
    }
    void Cancel(uint32_t id) override { NpNet::HttpCancel(int(id)); }
private:
    std::string m_base;
};
#endif

class HttpsTransport : public HttpTransport {
public:
    explicit HttpsTransport(const std::string& base) : m_base(base) {}
    ~HttpsTransport() override {}

    // The id given out is the first handle; a request that moved to the old service address (np_service_hosts.h) keeps that id (m_pend maps it to the live handle).
    uint32_t Start(const HttpRequest& request) override {
        const std::string url = m_base + request.path;
        const std::string routed = NpNet::ServiceHosts::Route(url);
        const bool toNew = NpNet::ServiceHosts::FallbackApplies(url) && routed == url;
        const uint32_t h = StartUrl(request, routed, toNew);
        if (h) {   // (always kept now: the request log needs the address it went to and the start time; the cases that fall back or were rerouted are unchanged)
            Pend p;
            p.req = request;
            p.url = url;
            p.sent = routed;
            p.startMs = NpNet::ServiceHosts::Now();
            p.cur = h;
            p.toNew = toNew;
            m_pend[h] = p;
        }
        return h;
    }

    bool Poll(uint32_t id, HttpResponse* out) override {
        std::map<uint32_t, Pend>::iterator it = m_pend.find(id);
        const uint32_t cur = it == m_pend.end() ? id : it->second.cur;
        NpHttpsResult r;
        const int st = np_https_poll(cur, &r);
        if (st == 0) return false;
        *out = HttpResponse();
        if (st < 0) { out->fail = Fail::Cancelled; if (it != m_pend.end()) m_pend.erase(it); return true; }
        if (it != m_pend.end()) {   // the request log (np_reqlog.h): one line for every attempt that ended
            const bool ans = r.error == NP_HTTPS_OK || r.error == NP_HTTPS_E_HTTP_STATUS;
            if (ans) NpNet::ReqLog::Line(it->second.req.method.c_str(), it->second.sent, r.http_status, 0, 0, r.elapsed_ms, r.reused_connection, reinterpret_cast<const char*>(r.body), r.body_len);
            else NpNet::ReqLog::Line(it->second.req.method.c_str(), it->second.sent, 0, np_https_error_name(r.error), r.message, r.elapsed_ms, 0, 0, 0);
        }
        if (it != m_pend.end() && it->second.toNew) {
            const bool ok = r.error == NP_HTTPS_OK || r.error == NP_HTTPS_E_HTTP_STATUS;
            if (NpNet::ServiceHosts::ShouldFallBack(it->second.url, true, ok ? 0 : int(r.error), int(r.stage), r.body, r.body_len)) {
                if (ok) fprintf(stderr, "[np-net] service host: answered by the zone (HTTP %d), trying the other address\n", r.http_status);
                else fprintf(stderr, "[online] service host: %s (%s), trying the other address\n", np_https_error_name(r.error), np_https_stage_name(r.stage));
                np_https_release(cur);
                it->second.toNew = false;
                it->second.sent = NpNet::ServiceHosts::SwapToOld(it->second.url);
                const uint32_t h = StartUrl(it->second.req, it->second.sent, false);
                if (h) { it->second.cur = h; return false; }
                out->fail = Fail::Transport;
                out->detail = "request not started";
                m_pend.erase(it);
                return true;
            }
        }
        if (it != m_pend.end()) m_pend.erase(it);
        return Finish(cur, r, out);
    }

    void Cancel(uint32_t id) override {
        std::map<uint32_t, Pend>::iterator it = m_pend.find(id);
        if (it != m_pend.end()) { np_https_release(it->second.cur); m_pend.erase(it); return; }
        np_https_release(id);
    }

private:
    struct Pend {
        HttpRequest req;
        std::string url;
        std::string sent;     // the address the live attempt went to (the request log)
        uint64_t startMs;
        uint32_t cur;
        bool toNew;
        Pend() : startMs(0), cur(0), toNew(false) {}
    };

    uint32_t StartUrl(const HttpRequest& request, const std::string& url, bool toNew) {
        std::vector<const char*> headers;
        for (size_t i = 0; i < request.headers.size(); ++i) headers.push_back(request.headers[i].c_str());
        headers.push_back(nullptr);
        NpHttpsRequest rq;
        memset(&rq, 0, sizeof rq);
        rq.url = url.c_str();
        rq.method = request.method.c_str();
        rq.body = request.body.empty() ? nullptr : request.body.data();
        rq.body_len = request.body.size();
        rq.headers = &headers[0];
        rq.total_timeout_ms = request.timeoutMs;
        rq.io_timeout_ms = request.ioTimeoutMs;
        rq.keep_alive = 1;   // a login followed by a ranking call pays one handshake
        if (toNew) rq.connect_timeout_ms = NpNet::ServiceHosts::kNewHostConnectMs;   // a dead new address must leave time for the old one
        NpHttpsError err = NP_HTTPS_OK;
        const NpHttpsHandle h = np_https_start(&rq, &err);
        if (!h) fprintf(stderr, "[online] request not started: %s\n", np_https_error_name(err));
        return h;
    }

    bool Finish(uint32_t id, const NpHttpsResult& r, HttpResponse* out) {
        if (r.error == NP_HTTPS_OK || r.error == NP_HTTPS_E_HTTP_STATUS) {
            out->status = r.http_status;
            out->body.assign(reinterpret_cast<const char*>(r.body), r.body_len);
        } else {
            switch (r.error) {
            case NP_HTTPS_E_TIMEOUT: out->fail = Fail::Timeout; break;
            case NP_HTTPS_E_CANCELLED: out->fail = Fail::Cancelled; break;
            case NP_HTTPS_E_PROTOCOL: case NP_HTTPS_E_REDIRECT: case NP_HTTPS_E_TOO_LARGE: out->fail = Fail::Protocol; break;
            default: out->fail = Fail::Transport; break;
            }
            out->detail = std::string(np_https_error_name(r.error)) + ": " + r.message;
            if (r.error == NP_HTTPS_E_CLOCK || r.error == NP_HTTPS_E_CERT || r.error == NP_HTTPS_E_OFFLINE || r.error == NP_HTTPS_E_DNS)
                out->advice = np_https_error_text(r.error);
        }
        np_https_release(id);
        return true;
    }


    std::map<uint32_t, Pend> m_pend;
    std::string m_base;
};

}  // namespace
}  // namespace NpOnline

extern "C" NpOnline::Api* NpOnlineCreateHttpApi(void) {
    // (the address is NpOnlineCentralBase: the ranked / quick queue asks the same function, np_dwc_match.cpp)
    std::string base = NpOnlineCentralBase();
#if !defined(NP_ONLINE_HTTPS_ONLY)
    if (base.compare(0, 7, "http://") == 0) {
        fprintf(stderr, "[online] service (plain HTTP, desktop test build): %s\n", base.c_str());
        return NpOnline::CreateHttpApi(new NpOnline::PlainTransport(base));
    }
#endif
    if (base.empty()) {
        fprintf(stderr, "[online] no service URL (MSC_ONLINE_URL=https://...): online play is unavailable\n");
        return nullptr;
    }
    while (!base.empty() && base[base.size() - 1] == '/') base.erase(base.size() - 1);
    fprintf(stderr, "[online] service: %s\n", base.c_str());
    return NpOnline::CreateHttpApi(new NpOnline::HttpsTransport(base));
}

#endif
#endif
