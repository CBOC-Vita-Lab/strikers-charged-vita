// The service host fallback (np_service_hosts.h) through BOTH request paths that use it: the signaling client's NpNet::HttpStart/HttpPoll (np_http.cpp) and
// the DWC HttpsTransport (np_online_https.cpp). The TLS layer is a local fake below (no sockets, no DNS, no real host name is ever used): each address
// answers with the result the test scripts for it. Build and run: tools/net/run_service_hosts_test.sh
#ifndef NP_SERVICE_HOSTS_TEST
#define NP_SERVICE_HOSTS_TEST 1
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <string>
#include <vector>

#include "platform/sdk/net/np_https.h"
#include "platform/sdk/net/online/np_http.h"
#include "platform/sdk/net/online/np_service_hosts.h"

// ---- the fake TLS layer ----------------------------------------------------------------------------------------------------------------------------
struct Script { NpHttpsError error; NpHttpsStage stage; int status; const char* body; };   // body: what the host answers with (NULL = "{}", the Worker's JSON)
static std::map<std::string, Script> g_script;     // by address prefix
struct Started { std::string url, method, body; std::vector<std::string> headers; uint32_t connectMs; };
static std::vector<Started> g_started;
static std::map<uint32_t, std::string> g_live;
static uint32_t g_h = 100;
static std::string g_text;
static uint64_t g_clock = 1000;

static const char* kNew = "https://new.test.local";
static const char* kOld = "https://old.test.local";

extern "C" {
NpHttpsHandle np_https_start(const NpHttpsRequest* rq, NpHttpsError* err) {
    Started s;
    s.url = rq->url;
    s.method = rq->method ? rq->method : "";
    s.body.assign(static_cast<const char*>(rq->body), rq->body ? rq->body_len : 0);
    for (size_t i = 0; rq->headers && rq->headers[i]; ++i) s.headers.push_back(rq->headers[i]);
    s.connectMs = rq->connect_timeout_ms;
    g_started.push_back(s);
    (void)err;
    ++g_h;
    g_live[g_h] = rq->url;
    return g_h;
}
int np_https_poll(NpHttpsHandle h, NpHttpsResult* out) {
    std::map<uint32_t, std::string>::iterator it = g_live.find(h);
    if (it == g_live.end()) return -1;
    memset(out, 0, sizeof *out);
    Script sc = {NP_HTTPS_OK, NP_HTTPS_S_DONE, 200, 0};
    for (std::map<std::string, Script>::iterator k = g_script.begin(); k != g_script.end(); ++k)
        if (it->second.compare(0, k->first.size(), k->first) == 0) sc = k->second;
    out->error = sc.error;
    out->stage = sc.stage;
    out->http_status = sc.error == NP_HTTPS_OK || sc.error == NP_HTTPS_E_HTTP_STATUS ? sc.status : 0;
    const char* b = sc.body ? sc.body : "{}";
    out->body = reinterpret_cast<const uint8_t*>(b);
    out->body_len = strlen(b);
    return 1;
}
void np_https_release(NpHttpsHandle h) { g_live.erase(h); }
void np_https_cancel(NpHttpsHandle h) { g_live.erase(h); }
const char* np_https_error_name(NpHttpsError e) { static char b[16]; snprintf(b, sizeof b, "E%d", int(e)); return b; }
const char* np_https_error_text(NpHttpsError e) { static char b[32]; snprintf(b, sizeof b, "text-%d", int(e)); return b; }
const char* np_https_stage_name(NpHttpsStage s) { static char b[16]; snprintf(b, sizeof b, "S%d", int(s)); return b; }
}
namespace NpNet { uint64_t SteadyNowMs(void*) { return g_clock; } }

static int g_bad = 0;
#define CHECK(c, w) do { if (!(c)) { printf("FAIL: %s (line %d)\n", w, __LINE__); ++g_bad; } } while (0)

static void Fresh() {
    g_script.clear(); g_started.clear(); g_live.clear();
    NpNet::ServiceHosts::Reset();
    NpNet::ServiceHosts::TestState().newBase = kNew;
    NpNet::ServiceHosts::TestState().oldBase = kOld;
    NpNet::ServiceHosts::TestState().clock = [] { return g_clock; };
    setenv("MSC_ONLINE_HOST", "auto", 1);   // explicit auto; the unset default (also auto) is checked in section 6
}
static void Script1(const char* base, NpHttpsError e, NpHttpsStage s = NP_HTTPS_S_DONE, int status = 200, const char* body = 0) { Script sc = {e, s, status, body}; g_script[base] = sc; }
static const char* kHtml = "<!DOCTYPE html><html><head><title>Just a moment...</title></head><body>challenge</body></html>";

// One request through the signaling client's layer; returns the result.
static NpNet::HttpResponse ViaHttp(const std::string& url, const char* method = "POST", const char* body = "{\"a\":1}") {
    std::string err;
    std::vector<std::string> h;
    h.push_back("X-Msc-App: 1.3.0");
    h.push_back("Authorization: Bearer test");
    const int id = NpNet::HttpStart(method, url, body, h, 15000, &err);
    NpNet::HttpResponse r;
    if (!id) { r.error = -1; r.text = err; return r; }
    for (int i = 0; i < 10; ++i) if (NpNet::HttpPoll(id, &r) != 0) return r;
    r.error = -2;
    return r;
}

// ---- the same cases through the DWC transport (np_online_https.cpp, compiled into this test) -------------------------------------------------------
#include "platform/sdk/dwc/np_online_http.h"
namespace NpOnline { Api* CreateHttpApi(HttpTransport* t) { delete t; return nullptr; } }   // (the API on top is not under test)
#include "platform/sdk/dwc/np_online_https.cpp"
static NpOnline::HttpResponse ViaDwc(NpOnline::HttpsTransport& t, const char* path = "/v1/me") {
    NpOnline::HttpRequest rq;
    rq.method = "GET"; rq.path = path; rq.timeoutMs = 10000; rq.headers.push_back("X-Msc-App: 1.3.0");
    const uint32_t id = t.Start(rq);
    NpOnline::HttpResponse r;
    if (!id) { r.fail = NpOnline::Fail::Transport; r.detail = "not started"; return r; }
    for (int i = 0; i < 10; ++i) if (t.Poll(id, &r)) return r;
    r.fail = NpOnline::Fail::Protocol; r.detail = "never finished";
    return r;
}

int main() {
    using namespace NpNet::ServiceHosts;
    const std::string newUrl = std::string(kNew) + "/v1/register";
    // 1. the new address works: nothing else is contacted, the answer comes back, no fallback state
    Fresh();
    NpNet::HttpResponse r = ViaHttp(newUrl);
    CHECK(r.error == 0 && r.status == 200, "new host answers");
    CHECK(g_started.size() == 1 && g_started[0].url == newUrl, "only the new host was contacted");
    CHECK(g_started[0].connectMs == 6000, "new host has the short connect limit (6 s)");
    CHECK(DownAt() == 0, "no down mark");

    // 2. resolution failure -> the old host, same request; later requests go straight to the old host; after the retry time the new one is tried again
    Fresh();
    Script1(kNew, NP_HTTPS_E_DNS, NP_HTTPS_S_DNS);
    r = ViaHttp(newUrl);
    CHECK(r.error == 0 && r.status == 200, "DNS failure falls back and answers");
    CHECK(g_started.size() == 2 && g_started[1].url == std::string(kOld) + "/v1/register", "second try went to the old host with the same path");
    CHECK(g_started[1].method == "POST" && g_started[1].body == "{\"a\":1}" && g_started[1].headers == g_started[0].headers, "same method, body and headers (User-Agent is set by np_https.cpp for both)");
    CHECK(g_started[1].url.compare(0, 8, "https://") == 0, "the fallback is https");
    CHECK(g_started[1].connectMs == 0, "the fallback keeps the normal connect limit");
    g_started.clear();
    r = ViaHttp(newUrl);
    CHECK(r.error == 0 && g_started.size() == 1 && g_started[0].url.compare(0, strlen(kOld), kOld) == 0, "while the new host is marked down the old one is used at once");
    g_clock += kRetryAfterMs + 1;
    g_started.clear();
    Script1(kNew, NP_HTTPS_OK);
    r = ViaHttp(newUrl);
    CHECK(r.error == 0 && g_started.size() == 1 && g_started[0].url == newUrl && DownAt() == 0, "after the retry time the new host is tried again and the mark clears");

    // 3. which failures fall back
    struct { NpHttpsError e; NpHttpsStage s; bool fb; const char* w; } cases[] = {
        {NP_HTTPS_E_DNS, NP_HTTPS_S_DNS, true, "DNS"}, {NP_HTTPS_E_CONNECT, NP_HTTPS_S_CONNECT, true, "CONNECT"},
        {NP_HTTPS_E_TLS_HANDSHAKE, NP_HTTPS_S_TLS, true, "TLS_HANDSHAKE"},
        {NP_HTTPS_E_TIMEOUT, NP_HTTPS_S_DNS, true, "TIMEOUT in DNS"}, {NP_HTTPS_E_TIMEOUT, NP_HTTPS_S_CONNECT, true, "TIMEOUT in CONNECT"},
        {NP_HTTPS_E_TIMEOUT, NP_HTTPS_S_TLS, true, "TIMEOUT in TLS"},
        {NP_HTTPS_E_TIMEOUT, NP_HTTPS_S_SEND, false, "TIMEOUT in SEND"}, {NP_HTTPS_E_TIMEOUT, NP_HTTPS_S_RESPONSE, false, "TIMEOUT in RESPONSE"},
        {NP_HTTPS_E_CERT, NP_HTTPS_S_TLS, false, "CERT"}, {NP_HTTPS_E_CLOCK, NP_HTTPS_S_QUEUED, false, "CLOCK"}, {NP_HTTPS_E_OFFLINE, NP_HTTPS_S_QUEUED, false, "OFFLINE"},
        {NP_HTTPS_E_IO, NP_HTTPS_S_RESPONSE, false, "IO"}, {NP_HTTPS_E_PROTOCOL, NP_HTTPS_S_RESPONSE, false, "PROTOCOL"}, {NP_HTTPS_E_UNAVAILABLE, NP_HTTPS_S_QUEUED, false, "UNAVAILABLE"},
        {NP_HTTPS_E_HTTP_STATUS, NP_HTTPS_S_DONE, false, "HTTP 503"}, {NP_HTTPS_E_REDIRECT, NP_HTTPS_S_DONE, false, "redirect"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        Fresh();
        Script1(kNew, cases[i].e, cases[i].s, 503);
        r = ViaHttp(newUrl);
        const bool contactedOld = g_started.size() == 2;
        char w[96];
        snprintf(w, sizeof w, "%s: falls back = %d", cases[i].w, int(cases[i].fb));
        CHECK(contactedOld == cases[i].fb, w);
        if (!cases[i].fb) {   // reported as it is: the new host's own error or answer, the old host never contacted, no mark
            CHECK(g_started.size() == 1 && DownAt() == 0, "no fallback: one request, no mark");
            if (cases[i].e == NP_HTTPS_E_HTTP_STATUS) CHECK(r.error == 0 && r.status == 503, "a 503 answer is passed on");
            else CHECK(r.error == int(cases[i].e), "the error is the one the new host gave");
        }
    }

    // 4. TLS certificate failure: never a silent move; the player gets the certificate error
    Fresh();
    Script1(kNew, NP_HTTPS_E_CERT, NP_HTTPS_S_TLS);
    r = ViaHttp(newUrl);
    CHECK(r.error == int(NP_HTTPS_E_CERT) && r.text == "text-" + std::to_string(int(NP_HTTPS_E_CERT)), "CERT: the certificate error reaches the caller with its sentence");
    CHECK(g_started.size() == 1, "CERT: the old host is not tried");

    // 5. both down: the error is the old host's (what the player saw before the new host existed)
    Fresh();
    Script1(kNew, NP_HTTPS_E_DNS, NP_HTTPS_S_DNS);
    Script1(kOld, NP_HTTPS_E_CONNECT, NP_HTTPS_S_CONNECT);
    r = ViaHttp(newUrl);
    CHECK(g_started.size() == 2, "both hosts tried once");
    CHECK(r.error == int(NP_HTTPS_E_CONNECT) && r.text == "text-" + std::to_string(int(NP_HTTPS_E_CONNECT)), "both down: the old host's error and sentence");

    // 6. the switch
    Fresh();
    setenv("MSC_ONLINE_HOST", "new", 1);
    Script1(kNew, NP_HTTPS_E_DNS, NP_HTTPS_S_DNS);
    r = ViaHttp(newUrl);
    CHECK(g_started.size() == 1 && r.error == int(NP_HTTPS_E_DNS), "MSC_ONLINE_HOST=new: no fallback");
    Fresh();
    setenv("MSC_ONLINE_HOST", "old", 1);
    CHECK(ApplyMode(kNew) == kOld, "MSC_ONLINE_HOST=old: the stock address becomes the old one");
    CHECK(ApplyMode("https://example.test") == "https://example.test", "another address is kept");
    CHECK(Route(std::string(kNew) + "/v1/time") == std::string(kOld) + "/v1/time", "old: a URL naming the new host goes to the old one");
    setenv("MSC_ONLINE_HOST", "auto", 1);
    CHECK(GetMode() == MODE_AUTO && ApplyMode(kNew) == kNew, "auto: the new address");
    setenv("MSC_ONLINE_HOST", "bogus", 1);
    CHECK(GetMode() == MODE_AUTO, "an unknown value is auto");
    unsetenv("MSC_ONLINE_HOST");
    CHECK(GetMode() == MODE_AUTO, "unset: auto (default)");
    CHECK(ApplyMode(kNew) == kNew, "unset: the stock address stays the new one");
    CHECK(Route(std::string(kNew) + "/v1/time") == std::string(kNew) + "/v1/time", "unset: the new host is used first");
    CHECK(Route("https://example.test/v1/time") == "https://example.test/v1/time", "unset: another address is kept");
    {   // unset through the request path: the new host is contacted, and the fallback is live
        g_started.clear(); g_script.clear(); Reset();
        Script1(kNew, NP_HTTPS_E_DNS, NP_HTTPS_S_DNS);
        r = ViaHttp(newUrl);
        CHECK(g_started.size() == 2 && g_started[0].url == newUrl && r.error == 0, "unset: new host first, DNS failure falls back");
    }
    CHECK(std::string(NewBase()) == kNew, "(test base in use)");

    // 6b. an answer from the new host that is not from our service: the zone answered (challenge / rate limit / block / 52x page) -> one resend to the old host
    {
        struct { int status; const char* body; bool fb; const char* w; } za[] = {
            {403, kHtml, true, "403 HTML challenge"}, {429, "Too many requests", true, "429 plain-text rate limit"}, {503, kHtml, true, "503 HTML"},
            {522, kHtml, true, "522 HTML"}, {403, "", true, "403 empty body"}, {200, kHtml, true, "200 with an HTML page"},
            {429, "{\"error\":{\"code\":\"rate_limited\"}}", false, "429 JSON from our service"}, {503, "  \n{\"error\":{\"code\":\"budget\"}}", false, "503 JSON (leading space) from our service"},
            {200, "{}", false, "200 JSON"}, {403, "{\"error\":{\"code\":\"forbidden\"}}", false, "403 JSON from our service"},
        };
        for (size_t i = 0; i < sizeof za / sizeof za[0]; ++i) {
            for (int path = 0; path < 2; ++path) {   // 0 = signaling client, 1 = DWC transport
                Fresh();
                NpOnline::HttpsTransport t(kNew);
                const NpHttpsError e = za[i].status == 200 ? NP_HTTPS_OK : NP_HTTPS_E_HTTP_STATUS;
                Script1(kNew, e, NP_HTTPS_S_DONE, za[i].status, za[i].body);
                Script1(kOld, NP_HTTPS_OK, NP_HTTPS_S_DONE, 200, "{\"ok\":1}");
                int err = 0, st = 0; std::string bodyGot;
                if (path == 0) { r = ViaHttp(newUrl); err = r.error; st = r.status; bodyGot = r.body; }
                else { NpOnline::HttpResponse d = ViaDwc(t); err = int(d.fail); st = d.status; bodyGot = d.body; }
                char w[128];
                snprintf(w, sizeof w, "%s (%s): resend to old = %d", za[i].w, path ? "DWC" : "http", int(za[i].fb));
                CHECK(g_started.size() == (za[i].fb ? 2u : 1u), w);
                if (za[i].fb) {
                    CHECK(g_started.size() == 2 && g_started[1].url.compare(0, strlen(kOld), kOld) == 0, "zone answer: the resend went to the old address");
                    CHECK(err == 0 && st == 200 && bodyGot == "{\"ok\":1}", "zone answer: the caller gets the old host's answer");
                    CHECK(DownAt() != 0, "zone answer: the new host is marked down");
                    g_started.clear();
                    path == 0 ? (void)ViaHttp(newUrl) : (void)ViaDwc(t);
                    CHECK(g_started.size() == 1 && g_started[0].url.compare(0, strlen(kOld), kOld) == 0, "zone answer: later requests go straight to the old address");
                } else {
                    CHECK(err == 0 && st == za[i].status && bodyGot == za[i].body, "our service's answer is passed on unchanged");
                    CHECK(DownAt() == 0, "our service's answer: no mark");
                }
            }
        }
        // our answer clears an earlier mark; the zone page on the OLD host (no further fallback) is passed on
        Fresh();
        NpNet::ServiceHosts::DownAt() = g_clock + 1;
        g_clock += kRetryAfterMs + 1;
        Script1(kNew, NP_HTTPS_OK, NP_HTTPS_S_DONE, 200, "{}");
        r = ViaHttp(newUrl);
        CHECK(g_started.size() == 1 && g_started[0].url == newUrl && DownAt() == 0, "JSON answer after the retry time clears the mark");
        Fresh();
        Script1(kNew, NP_HTTPS_E_HTTP_STATUS, NP_HTTPS_S_DONE, 403, kHtml);
        Script1(kOld, NP_HTTPS_E_HTTP_STATUS, NP_HTTPS_S_DONE, 403, kHtml);
        r = ViaHttp(newUrl);
        CHECK(g_started.size() == 2 && r.error == 0 && r.status == 403, "zone page on both: one resend only, the old host's answer is passed on");
        Fresh();
        setenv("MSC_ONLINE_HOST", "new", 1);
        Script1(kNew, NP_HTTPS_E_HTTP_STATUS, NP_HTTPS_S_DONE, 403, kHtml);
        r = ViaHttp(newUrl);
        CHECK(g_started.size() == 1 && r.status == 403, "MSC_ONLINE_HOST=new: a zone answer is final");
        unsetenv("MSC_ONLINE_HOST");
    }

    // 7. another address is not touched, and a look-alike prefix is not the new host
    Fresh();
    Script1("https://other.test.local", NP_HTTPS_E_DNS, NP_HTTPS_S_DNS);
    r = ViaHttp("https://other.test.local/v1/me");
    CHECK(g_started.size() == 1 && r.error == int(NP_HTTPS_E_DNS), "an address the player named gets no fallback");
    g_started.clear();
    Script1("https://new.test.local.evil.example", NP_HTTPS_E_DNS, NP_HTTPS_S_DNS);
    r = ViaHttp("https://new.test.local.evil.example/v1/me");
    CHECK(g_started.size() == 1, "a look-alike host name is not the new host");

    // 8. the same through the DWC transport
    {
        Fresh();
        NpOnline::HttpsTransport t(kNew);
        Script1(kNew, NP_HTTPS_E_DNS, NP_HTTPS_S_DNS);
        NpOnline::HttpResponse d = ViaDwc(t);
        CHECK(d.fail == NpOnline::Fail::None && d.status == 200, "DWC: DNS failure falls back and answers");
        CHECK(g_started.size() == 2 && g_started[1].url == std::string(kOld) + "/v1/me", "DWC: second try on the old host");
        CHECK(g_started[0].connectMs == 6000 && g_started[1].connectMs == 0, "DWC: short connect limit on the new host only");
        g_started.clear();
        d = ViaDwc(t);
        CHECK(d.fail == NpOnline::Fail::None && g_started.size() == 1 && g_started[0].url == std::string(kOld) + "/v1/me", "DWC: the old host first while the new one is down");
        Fresh();
        NpOnline::HttpsTransport t2(kNew);
        Script1(kNew, NP_HTTPS_E_CERT, NP_HTTPS_S_TLS);
        d = ViaDwc(t2);
        CHECK(d.fail == NpOnline::Fail::Transport && d.advice == "text-" + std::to_string(int(NP_HTTPS_E_CERT)) && g_started.size() == 1, "DWC: CERT is reported, the old host not tried");
        Fresh();
        NpOnline::HttpsTransport t3(kNew);
        Script1(kNew, NP_HTTPS_E_DNS, NP_HTTPS_S_DNS);
        Script1(kOld, NP_HTTPS_E_DNS, NP_HTTPS_S_DNS);
        d = ViaDwc(t3);
        CHECK(d.fail == NpOnline::Fail::Transport && d.advice == "text-" + std::to_string(int(NP_HTTPS_E_DNS)) && g_started.size() == 2, "DWC: both down: the DNS sentence the player saw before");
        Fresh();
        setenv("MSC_ONLINE_HOST", "new", 1);
        NpOnline::HttpsTransport t4(kNew);
        Script1(kNew, NP_HTTPS_E_DNS, NP_HTTPS_S_DNS);
        d = ViaDwc(t4);
        CHECK(d.fail == NpOnline::Fail::Transport && g_started.size() == 1, "DWC: MSC_ONLINE_HOST=new: no fallback");
        unsetenv("MSC_ONLINE_HOST");
    }
    printf(g_bad ? "service_hosts_test: %d FAILED\n" : "service_hosts_test: all passed\n", g_bad);
    return g_bad ? 1 : 0;
}
