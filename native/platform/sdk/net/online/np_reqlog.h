// One log line per request to the online service, for the console runs of the real service (MSC_NET_REQLOG=1; off by default, nothing is read or written otherwise).
//
//   [np-net] req 7 T+12.3s GET /v1/info host=new -> 200 85 ms reused=1 pollScale=1.2
//   [np-net] req 8 T+13.0s POST /v1/ranked/queue host=old -> 503 40 ms code=over_budget level=new-over retryAfterSec=15
//   [np-net] req 9 T+14.1s GET /v1/me host=new -> FAIL DNS (dns lookup failed) 6001 ms
//
// What it gives a run's log: whether each request was answered by the service (an HTTP status, 2xx or the service's own JSON error), which address answered
// (new = strikers.psvita.dev, old = the workers.dev name, other = an address named with MSC_ONLINE_URL), the running count and the time since the first request (so a
// requests-per-10-seconds and per-minute figure can be read), and the service's pacing and budget words from the answer's JSON body: `pollScale`, and for an error answer (status 400 or more) its
// `code`, `level`, `retryAfterSec`. The response HEADERS (X-Msc-Budget, Retry-After) are not exposed by the TLS layer and are not logged: the same level is in the body of every
// 503 over_budget. Never logged: query strings, the Authorization header, a token, a name, a room or invite code (a path segment that holds a digit or a capital is
// printed as :id), the body.
#ifndef NP_REQLOG_H
#define NP_REQLOG_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <string>

#include "platform/sdk/net/online/np_service_hosts.h"

extern "C" void NpMpDiagNoteRequest(const char* method, const char* path, int status, const char* failName, unsigned ms, int reused, int attempt, const char* codeWord) __attribute__((weak));

namespace NpNet {
namespace ReqLog {

inline bool On() {
    static const bool on = [] { const char* e = getenv("MSC_NET_REQLOG"); return e && e[0] == '1'; }();
    return on;
}

// "new" / "old" / "other" for the address a request went to.
inline const char* HostLabel(const std::string& url) {
    if (ServiceHosts::HasPrefix(url, ServiceHosts::NewBase())) return "new";
    if (ServiceHosts::HasPrefix(url, ServiceHosts::OldBase())) return "old";
    return "other";
}

// The path of `url` without the query; every segment after the first that holds a digit or a capital letter (an id, a code) or is longer than 20 characters is :id.
inline std::string SafePath(const std::string& url) {
    size_t p = url.find("://");
    p = p == std::string::npos ? 0 : url.find('/', p + 3);
    if (p == std::string::npos) return "/";
    std::string path = url.substr(p);
    const size_t q = path.find('?');
    if (q != std::string::npos) path.resize(q);
    std::string out;
    size_t i = 0, seg = 0;
    while (i < path.size()) {
        if (path[i] == '/') { out += '/'; ++i; continue; }
        size_t e = path.find('/', i);
        if (e == std::string::npos) e = path.size();
        bool id = e - i > 20;
        for (size_t k = i; k < e && seg > 0 && !id; ++k) if ((path[k] >= '0' && path[k] <= '9') || (path[k] >= 'A' && path[k] <= 'Z')) id = true;
        out += id ? std::string(":id") : path.substr(i, e - i);
        i = e;
        ++seg;
    }
    return out.empty() ? "/" : out;
}

// The text after `"key":` in a JSON body (a number or a short string), or "". Not a parser: the body is the service's own, and only these few words are read.
inline std::string Word(const char* body, size_t len, const char* key) {
    const std::string k = std::string("\"") + key + "\":";
    const std::string b(body ? body : "", body ? len : 0);
    size_t at = b.find(k);
    if (at == std::string::npos) return "";
    at += k.size();
    while (at < b.size() && b[at] == ' ') ++at;
    const bool quoted = at < b.size() && b[at] == '"';
    if (quoted) ++at;
    std::string v;
    while (at < b.size() && v.size() < 24) {
        const char c = b[at++];
        if (quoted ? c == '"' : !((c >= '0' && c <= '9') || c == '.' || c == '-')) break;
        v += (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_' ? c : '?';
    }
    return v;
}

inline void Line(const char* method, const std::string& url, int status, const char* failName, const char* failText, uint32_t ms, int reused, const char* body, size_t len) {
    // (mpbeta27) every request also goes to the problem report's [network] lines (np_mpdiag.h NoteRequest: counted, the last few kept with this same safe path and
    // the service's error word), whatever MSC_NET_REQLOG says. One call per request, never per frame. Weak: the small tests link without np_mpdiag.cpp.
    if (NpMpDiagNoteRequest) NpMpDiagNoteRequest(method ? method : "?", SafePath(url).c_str(), status, failName, ms, reused, 0, status >= 400 ? Word(body, len, "code").c_str() : "");
    if (!On()) return;
    static std::atomic<uint32_t> n(0);
    static const uint64_t t0 = ServiceHosts::Now();
    const uint32_t k = ++n;
    const uint64_t t = ServiceHosts::Now() - t0;
    char head[160];
    snprintf(head, sizeof head, "[np-net] req %u T+%u.%us %s %s host=%s", unsigned(k), unsigned(t / 1000), unsigned((t % 1000) / 100), method ? method : "?", SafePath(url).c_str(), HostLabel(url));
    if (failName) {
        fprintf(stderr, "%s -> FAIL %s (%s) %u ms\n", head, failName, failText ? failText : "", unsigned(ms));
        return;
    }
    std::string extra;
    // `code` is read only from an error answer: in a success body it is the account's public code
    const char* keys[] = {"pollScale", "code", "level", "retryAfterSec"};
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; ++i) {
        if (i > 0 && status < 400) break;
        const std::string v = Word(body, len, keys[i]);
        if (!v.empty()) extra += std::string(" ") + keys[i] + "=" + v;
    }
    fprintf(stderr, "%s -> %d %u ms reused=%d%s\n", head, status, unsigned(ms), reused, extra.c_str());
}

}  // namespace ReqLog
}  // namespace NpNet

#endif
