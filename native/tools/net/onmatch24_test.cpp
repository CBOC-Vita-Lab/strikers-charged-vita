// onmatch24: the ranked / quick queue of Online Central under the CONSOLE's rules, against a local TLS fake (tools/net/https_server.py), never a live service.
// The real np_dwc_match.cpp + np_http.cpp + np_https (Mbed TLS) are linked; the build defines NP_ONLINE_CONSOLE_URL_RULES (https:// only, as on the Vita).
//   1. the queue requests START and arrive complete (the console failed here: "[ranked] matchmaking failed: the online service is not set up (sign in first)
//      (error 6 code 20100)" because the HTTPS client refused the request's own "Content-Type" line before any network use);
//   2. 100+ failing polls (nothing listening, a held poll that never answers and is given up, a cancel in the middle of a request) and cancels leave no
//      socket and no file descriptor behind, never more than one open at once, and the budget (np_https_sys.h kMaxSockets) is never reached.
// Run by tools/net/run_match_test.sh (which also runs the mutants). Exit code = number of failures.
#include <dwc/dwc_account.h>
#include <dwc/dwc_error.h>
#include <dwc/dwc_init_fwd.h>
#include <dwc/dwc_main_fwd.h>
#include <dwc/dwc_match_fwd.h>

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "platform/sdk/dwc/np_dwc.h"
#include "platform/sdk/dwc/np_online_api.h"
#include "platform/sdk/dwc/np_online_fake.h"
#include "platform/sdk/dwc/np_online_name.h"
#include "platform/sdk/dwc/np_online_http.h"
#include "platform/sdk/dwc/np_online_queue.h"
#include "platform/sdk/net/np_https.h"
#include "platform/sdk/net/online/np_http.h"
#include "platform/sdk/net/online/np_online.h"

using namespace NpOnline;

static int g_fail = 0, g_checks = 0;
#define CHECK(c, ...)                                                                          \
    do {                                                                                       \
        ++g_checks;                                                                            \
        if (!(c)) { printf("FAIL %s:%d: %s -- ", __FILE__, __LINE__, #c); printf("" __VA_ARGS__); printf("\n"); ++g_fail; } \
    } while (0)

// ---- stand-ins for the link layer (a ranked match found would start it; this test never pairs) ----
extern "C" void NpNetConfigureService(const char*, const char*, const char*, const char*) {}
extern "C" int NpNetJoin(const char*, const char*) { return 0; }
extern "C" void NpNetPoll(void) {}
extern "C" void NpNetLeave(void) {}
extern "C" int NpNetPhase(void) { return 0; }
extern "C" const char* NpNetError(void) { return ""; }
NpNet::OnlineNet* NpNetController() { return nullptr; }
namespace NpNet {
uint64_t SteadyNowMs(void*) { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return uint64_t(t.tv_sec) * 1000 + uint64_t(t.tv_nsec) / 1000000; }
int OnlineNet::SendFrame(int, const void*, size_t) { return 0; }
int OnlineNet::RecvFrame(int*, void*, size_t) { return 0; }
}
extern "C" int NpOnlineImeAvailable(void) { return 0; }
extern "C" int NpOnlineImeOpen(const char*, const char*) { return -1; }
extern "C" int NpOnlineImePoll(char*, size_t) { return -1; }
extern "C" void NpOnlineImeCancel(void) {}

extern "C" void NpDwcFriendsShutdown(void);
void NpDwcFriendsResetForTest();
void NpRankedResetForTest();
extern "C" int NpRankedBusy(void);

static uint64_t g_now = 1000000;   // the game's clock (gaps, request limits): this test moves it, the sockets and the server run in real time
static uint64_t TestClock() { return g_now; }
alignas(8) static uint8_t g_block[0xB10];

static std::string g_dir, g_dataDir;
static int g_matchedCalls = 0, g_matchedError = 0;
static uint64_t g_matchedAt = 0;
static void Matched(DWCError error, int, void*) { ++g_matchedCalls; g_matchedError = int(error); g_matchedAt = g_now; }

static std::string ReadFile(const std::string& path) {
    std::string s;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { printf("cannot read %s\n", path.c_str()); exit(2); }
    char b[4096];
    size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
    fclose(f);
    return s;
}
static int PortOf(const char* tag) {   // ports.json is {"good": 123, ...}
    const std::string j = ReadFile(g_dir + "/ports.json");
    const size_t p = j.find(std::string("\"") + tag + "\"");
    if (p == std::string::npos) { printf("no port for %s\n", tag); exit(2); }
    return atoi(j.c_str() + j.find(':', p) + 1);
}
static int Fds() {
    int n = 0;
    if (DIR* d = opendir("/proc/self/fd")) { while (readdir(d)) n++; closedir(d); }
    return n;
}
static void SleepMs(int ms) { usleep(ms * 1000); }

static std::string GoodUrl(const char* path) { return "https://localhost:" + std::to_string(PortOf("good")) + path; }

// A plain request to the fake through the same client.
static std::string Fetch(const std::string& url) {
    NpNet::HttpResponse r;
    std::string err;
    const int h = NpNet::HttpStart("GET", url, "", std::vector<std::string>(), 8000, &err);
    if (!h) return "(not started: " + err + ")";
    for (int i = 0; i < 2500; ++i) {
        const int rc = NpNet::HttpPoll(h, &r);
        if (rc == 1) return r.body;
        if (rc < 0) break;
        SleepMs(4);
    }
    return "(no answer)";
}
static std::string Seen() { return Fetch(GoodUrl("/v1/_seen")); }
static void Hang(int on) { Fetch(GoodUrl("/v1/_hang?gets=") + std::to_string(on)); }
static int Count(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1)) ++n;
    return n;
}

static void Pump(int n = 1, int sleepMs = 0, uint64_t advance = 0) {
    for (int i = 0; i < n; ++i) { g_now += advance; DWC_ProcessFriendsMatch(); NpNet::HttpPump(); if (sleepMs) SleepMs(sleepMs); }
}

static void SignIn(const std::string& url) {
    NpDwcFriendsShutdown();
    NpDwcFriendsResetForTest();
    NpDwcResetForTest();
    NameResetForTest();
    NpRankedResetForTest();
    SetApi(nullptr);
    static FakeApi* fake = nullptr;
    delete fake;
    fake = new FakeApi(FakeOptions());
    SetApi(fake);
    NpOnlineForgetCredentials();
    QueueResetForTest(false);
    memset(g_block, 0, sizeof g_block);
    ForgetName();
    DWC_Init(DWC_SVR_RELEASE, "mschargedwii", 0x52345145, 0, 0);
    DWC_CreateUserData((DWCUserData*)g_block);
    DWC_InitFriendsMatch(0, (DWCUserData*)g_block, 0x2AAF, "mschargedwii", "key", 0, 0, 0, 0);
    unsigned short wide[8] = {'T', 'e', 's', 't', 0};
    static int done;
    done = 0;
    struct L { static void Cb(DWCError, int, void*) { done = 1; } };
    DWC_LoginAsync(wide, 0, L::Cb, 0);
    for (int i = 0; i < 40 && !done; ++i) Pump();
    NpOnlineCredentials c;
    memset(&c, 0, sizeof c);
    c.profileId = NpDwcLoggedInProfile();
    snprintf(c.token, sizeof c.token, "%s", "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNO1");   // a made-up token of the right shape; the local fake accepts any
    NpOnlineSaveCredentials(c);
    setenv("MSC_ONLINE_URL", url.c_str(), 1);
}

static void Sockets(int* live, int* opened, int* closed, int* refused) { np_https_socket_stats(live, opened, closed, refused); }

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc < 2) { printf("usage: onmatch24_test DIR (the https_server.py run folder)\n"); return 2; }
    g_dir = argv[1];
    char dir[] = "/tmp/onmatch24_XXXXXX";
    if (!mkdtemp(dir)) return 2;
    g_dataDir = dir;
    setenv("NP_DATA_DIR", dir, 1);
    unsetenv("MSC_ONLINE_NAME");
    unsetenv("MSC_ONLINE_RANKED");
    const std::string pem = ReadFile(g_dir + "/pki/trusted.pem");
    np_https_test_set_roots_pem(pem.c_str(), pem.size() + 1);
    np_https_test_reset_tls();
    SetClockForTest(TestClock);
    const std::string good = GoodUrl("");
    const std::string dead = "https://localhost:" + std::to_string(PortOf("closed"));

    printf("[1] the queue requests start and arrive complete (console rules: https only, one service address)\n");
    {
        SignIn(good);
        CHECK(NpDwcLoggedInProfile() > 0, "signed in to the fake");
        NpDwcSetQuickMatch(true);
        g_matchedCalls = 0;
        CHECK(DWC_ConnectToAnybodyAsync(2, 0, Matched, 0, 0, 0) == 1, "the quick search starts");
        std::string seen;
        for (int i = 0; i < 300 && Count(seen, "POST") == 0; ++i) { Pump(1, 10); if (i % 10 == 9) seen = Seen(); }
        printf("  server saw: %s\n", seen.c_str());
        CHECK(Count(seen, "\"POST\"") >= 1, "the POST /v1/ranked/queue reached the server");
        CHECK(seen.find("application/json") != std::string::npos, "with its content type");
        CHECK(seen.find("Bearer") != std::string::npos, "with the bearer token");
        CHECK(seen.find("quick") != std::string::npos, "the quick body");
        CHECK(g_matchedCalls == 0, "no failure callback (%d, error %d)", g_matchedCalls, g_matchedError);
        CHECK(NpRankedBusy() != 0, "the search is running");
        // queued: the next requests are the GET polls (the fake answers "queued" at once)
        for (int i = 0; i < 60; ++i) Pump(1, 5, 2500);
        seen = Seen();
        CHECK(Count(seen, "\"GET\"") >= 1, "the queue is polled");
        CHECK(g_matchedCalls == 0, "still no failure (%d)", g_matchedCalls);
        CHECK(DWC_CancelMatching() == 1, "cancel");
        std::string after;
        for (int i = 0; i < 100 && Count(after, "\"DELETE\"") == 0; ++i) { Pump(1, 10); if (i % 5 == 4) after = Seen(); }
        CHECK(Count(after, "\"DELETE\"") >= 1, "the cancel leaves the queue (DELETE sent)");
        CHECK(NpRankedBusy() == 0, "the search is over");
        // the ranked (not quick) entry sends an empty object
        NpDwcSetQuickMatch(false);
        CHECK(DWC_ConnectToAnybodyAsync(2, 0, Matched, 0, 0, 0) == 1, "the ranked search starts");
        std::string s2;
        for (int i = 0; i < 200; ++i) { Pump(1, 10); if (i % 10 == 9) { s2 = Seen(); if (s2.find("\"body\": \"{}\"") != std::string::npos) break; } }
        CHECK(s2.find("\"body\": \"{}\"") != std::string::npos, "the ranked body {}");
        DWC_CancelMatching();
        for (int i = 0; i < 40; ++i) Pump(1, 10);
    }

    printf("[2] sockets: 100+ failing polls, held polls given up, cancels in the middle: back to the baseline\n");
    {
        np_https_shutdown(5000);
        int live = -1, opened0 = 0, closed0 = 0, refused = -1;
        Sockets(&live, &opened0, &closed0, &refused);
        CHECK(live == 0 && opened0 == closed0 && refused == 0, "before: live %d opened %d closed %d refused %d", live, opened0, closed0, refused);
        const int fds0 = Fds();
        int worst = 0, polls = 0;
        auto Sample = [&] { int l = 0; Sockets(&l, 0, 0, 0); if (l > worst) worst = l; };
        // (a) nothing listening: every queue request fails at the connection and is tried again after the game's own gap
        SignIn(dead);
        {   // the gaps grow, and the search ends after 5 requests in a row without an answer: 2, 4, 8 s apart, not one every 2 s for 6 minutes
            NpDwcSetQuickMatch(true);
            g_matchedCalls = 0;
            int o0 = 0;
            Sockets(0, &o0, 0, 0);
            const uint64_t started = g_now;
            CHECK(DWC_ConnectToAnybodyAsync(2, 0, Matched, 0, 0, 0) == 1, "search against nothing");
            for (int i = 0; i < 60; ++i) { Pump(1, 8, 1000); Sample(); }
            int o1 = 0;
            Sockets(0, &o1, 0, 0);
            printf("  nothing listening, 60 s of the game's clock: %d connection attempts, ended with error %d (%d callback)\n", o1 - o0, g_matchedError, g_matchedCalls);
            CHECK(o1 - o0 == 5, "a dead service is asked 5 times with growing gaps (%d attempts)", o1 - o0);
            CHECK(g_matchedCalls == 1 && g_matchedError != 0, "then the search ends with the connection error (%d, %d)", g_matchedCalls, g_matchedError);
            CHECK(NpRankedBusy() == 0, "and is over");
            CHECK(g_matchedAt - started >= 20000, "the gaps grew: the fifth failure came %llu ms in (2 + 4 + 8 + 10 s)", (unsigned long long)(g_matchedAt - started));
            polls += o1 - o0;
        }
        for (int round = 0; round < 14; ++round) {   // the same again, the clock moving a whole gap a turn
            NpDwcSetQuickMatch(true);
            g_matchedCalls = 0;
            CHECK(DWC_ConnectToAnybodyAsync(2, 0, Matched, 0, 0, 0) == 1, "search against nothing %d", round);
            for (int i = 0; i < 12 && !g_matchedCalls; ++i) { Pump(1, 12, 11000); Sample(); }
            polls += 5;
            CHECK(g_matchedCalls == 1, "the round ended (%d)", g_matchedCalls);
            DWC_CancelMatching();
            for (int i = 0; i < 6; ++i) Pump(1, 10);
        }
        // (b) a held poll that never answers: the game gives it up after its limit (clock jump) and asks again; then a cancel mid-request
        SignIn(good);
        Hang(1);
        for (int round = 0; round < 6; ++round) {
            NpDwcSetQuickMatch(true);
            g_matchedCalls = 0;
            CHECK(DWC_ConnectToAnybodyAsync(2, 0, Matched, 0, 0, 0) == 1, "search %d", round);
            for (int i = 0; i < 20; ++i) { Pump(1, 8, 0); Sample(); }              // POST answered (queued), GET started and hanging
            for (int k = 0; k < 8; ++k) {
                Pump(1, 40, 31000);                                                  // past the request limit + 3 s: the game cancels it
                Pump(1, 40, 3000);                                                   // and starts the next poll
                Sample(); ++polls;
            }
            Pump(1, 40, 0);                                                          // a poll is in flight now
            DWC_CancelMatching();                                                    // cancel in the middle of it
            Sample();
            for (int i = 0; i < 6; ++i) { Pump(1, 15); Sample(); }
            ++polls;
            {   // the abandoned held poll must not keep the worker: the next request is served at once
                const uint64_t t = NpNet::SteadyNowMs(0);
                const std::string hello = Fetch(GoodUrl("/hello"));
                const uint64_t took = NpNet::SteadyNowMs(0) - t;
                CHECK(took < 2000 && hello.find("hello") != std::string::npos, "the next request is served at once after a cancel (%llu ms: %.40s)", (unsigned long long)took, hello.c_str());
            }
        }
        Hang(0);
        SleepMs(400);
        np_https_shutdown(5000);
        int opened = 0, closed = 0;
        Sockets(&live, &opened, &closed, &refused);
        printf("  %d polls: %d sockets opened, %d closed, %d open now (most after a turn: %d), %d refused; file descriptors %d -> %d\n", polls, opened - opened0, closed - closed0, live,
               worst, refused, fds0, Fds());
        CHECK(polls >= 100, "polls %d", polls);
        CHECK(opened - opened0 >= 60, "the polls really opened sockets (%d)", opened - opened0);
        CHECK(live == 0, "no socket open at the end (%d)", live);
        CHECK(worst <= 1, "never more than one socket at once (%d)", worst);
        CHECK(opened == closed, "every socket opened was closed (%d / %d)", opened, closed);
        CHECK(refused == 0, "the budget was never reached (%d refused)", refused);
        CHECK(Fds() <= fds0 + 1, "file descriptors back to the baseline (%d -> %d)", fds0, Fds());
        CHECK(NpRankedBusy() == 0, "no search is left running");
    }

    printf("[3] every request kind the console refused before the fix is started and sent: POST with a Content-Type line, PUT, DELETE (and GET)\n");
    {
        struct K { const char* method; const char* hdr; const char* body; } kinds[] = {
            {"POST", "Content-Type: application/json", "{\"a\":1}"},   // the ranked / quick queue, the invite rooms' account request, the report
            {"PUT", "X-Msc-Proto: 1", "{\"name\":\"x\"}"},            // PUT /v1/me (the sign-in after a rename, the signaling's relay-only change)
            {"DELETE", "X-Msc-Proto: 1", ""},                            // the queue leave, the account deletion
            {"GET", "X-Msc-Proto: 1", ""},
        };
        for (const K& k : kinds) {
            std::vector<std::string> h;
            h.push_back("Authorization: Bearer abcdefghijklmnopqrstuvwxyz");
            h.push_back(k.hdr);
            std::string err;
            const int id = NpNet::HttpStart(k.method, GoodUrl("/echo"), k.body, h, 8000, &err);
            CHECK(id != 0, "%s with '%s' is started (%s)", k.method, k.hdr, err.c_str());
            std::string body;
            for (int i = 0; id && i < 2500; ++i) { NpNet::HttpResponse r; const int rc = NpNet::HttpPoll(id, &r); if (rc == 1) { body = r.body; break; } if (rc < 0) break; SleepMs(4); }
            CHECK(body.find(std::string("\"method\": \"") + k.method + "\"") != std::string::npos, "%s arrived as such: %s", k.method, body.c_str());
            if (!strcmp(k.method, "POST")) CHECK(body.find("\"ctype\": \"application/json\"") != std::string::npos, "the content type arrived once: %s", body.c_str());
        }
    }

    printf("[4] a held friends poll does not keep another request waiting (one HTTPS worker, one connection)\n");
    {
        setenv("MSC_ONLINE_URL", good.c_str(), 1);
        np_https_shutdown(5000);
        Api* api = NpOnlineCreateHttpApi();
        CHECK(api != nullptr, "the HTTPS api (console rules)");
        if (api) {
            FriendsRequest fr;
            memset(&fr, 0, sizeof fr);
            snprintf(fr.rev, sizeof fr.rev, "abc");
            fr.waitSec = 6;   // (scaled: the service holds 45 s; callers give up after 10-15 s; here 6 s against a 3 s caller)
            const uint32_t held = api->BeginFriends(fr);
            CHECK(held != 0, "the held poll starts");
            SleepMs(300);
            const uint64_t t0 = NpNet::SteadyNowMs(0);
            const uint32_t tid = api->BeginTime();
            CHECK(tid != 0, "the clock request starts");
            TimeReply tr;
            Result r;
            r.status = Status::Pending;
            for (int i = 0; i < 2000 && r.status == Status::Pending; ++i) { r = api->PollTime(tid, &tr); if (r.status == Status::Pending) SleepMs(5); }
            const uint64_t took = NpNet::SteadyNowMs(0) - t0;
            printf("  the clock request answered after %llu ms (the hold is 6000 ms, a caller's deadline 3000 ms)\n", (unsigned long long)took);
            CHECK(r.ok() && tr.unixMs > 0, "the clock request was answered");
            CHECK(took < 3000, "answered inside a caller's deadline, not after the hold (%llu ms)", (unsigned long long)took);
            FriendsReply rep;
            Result fr2;
            fr2.status = Status::Pending;
            for (int i = 0; i < 400 && fr2.status == Status::Pending; ++i) { fr2 = api->PollFriends(held, &rep); if (fr2.status == Status::Pending) SleepMs(5); }
            CHECK(fr2.ok() && !rep.changed && rep.heldMs > 0 && !strcmp(rep.rev, "abc"), "the held poll finished as 'nothing new' for its caller (changed %d, heldMs %u, rev %s)", int(rep.changed), rep.heldMs, rep.rev);
            // and the poll can be asked again
            const uint32_t again = api->BeginFriends(fr);
            CHECK(again != 0, "the friends poll starts again");
            delete api;
        }
        np_https_shutdown(5000);
        int live = -1;
        Sockets(&live, 0, 0, 0);
        CHECK(live == 0, "no socket is left (%d)", live);
    }
    std::string cmd = "rm -rf " + g_dataDir;
    if (system(cmd.c_str())) {}
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
