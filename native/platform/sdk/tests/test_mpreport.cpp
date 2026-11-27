// np_mpreport.{h,cpp}: the problem report's payload, the consent rule, the offer-once rule and what the player is told for each answer of the service.
//   c++ -std=c++17 -Wall -Wextra -I<native> native/platform/sdk/tests/test_mpreport.cpp native/platform/sdk/net/np_mpreport.cpp native/platform/sdk/net/np_mpdiag.cpp -pthread -o /tmp/t
// Mutants (each must FAIL): -DNP_MPDIAG_SCRUB_OFF, -DNP_MPREPORT_MUT_NOCAP, -DNP_MPREPORT_MUT_NOCONSENT, -DNP_MPREPORT_MUT_FORGET_ALWAYS, -DNP_MPREPORT_MUT_LIMIT_AS_FAIL,
//          -DNP_MPREPORT_MUT_401_FAIL, -DNP_MPDIAG_MUT_OFFER_ALWAYS
#include "platform/sdk/net/np_mpreport.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

#include "platform/sdk/net/online/np_json.h"

static int g_checks = 0, g_failed = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_failed; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_CLEAN(out, secret) do { ++g_checks; if (strstr((out), (secret))) { ++g_failed; std::printf("FAIL %s:%d: \"%s\" leaked in the payload\n", __FILE__, __LINE__, (secret)); } } while (0)

using namespace NpMpReport;

// ---- a fake service connection ---------------------------------------------------------------------------------------------------------------
struct Req {
    std::string method, url, body;
    std::vector<std::string> headers;
};
static std::vector<Req> g_reqs;
static bool g_haveAccount = true;
static int g_forgot = 0;
static uint64_t g_now = 1000;
static std::vector<Answer> g_answers;   // answers handed out in order, one per request
static int g_pollsBeforeAnswer = 0;
static int g_polls = 0;
static int g_cancelled = 0;

static bool FakeAccount(Account* a) {
    if (!g_haveAccount) return false;
    snprintf(a->name, sizeof a->name, "%s", "Mario");
    snprintf(a->token, sizeof a->token, "%s", "tok-SECRETSECRETSECRET0123456789");
    snprintf(a->urls, sizeof a->urls, "%s", "https://one.example,https://two.example");
    return true;
}
static void FakeForget() { ++g_forgot; }
static int FakeStart(const char* method, const std::string& url, const std::string& body, const std::vector<std::string>& headers, uint32_t, std::string*) {
    Req r;
    r.method = method;
    r.url = url;
    r.body = body;
    r.headers = headers;
    g_reqs.push_back(r);
    g_polls = 0;
    return int(g_reqs.size());
}
static int FakePoll(int handle, Answer* out) {
    if (handle <= 0 || size_t(handle) > g_reqs.size()) return -1;
    if (g_polls++ < g_pollsBeforeAnswer) return 0;
    const size_t at = size_t(handle) - 1;
    *out = at < g_answers.size() ? g_answers[at] : Answer();
    return 1;
}
static void FakeCancel(int) { ++g_cancelled; }
static uint64_t FakeNow() { return g_now; }
static const char* FakeVersion() { return "1.3.0"; }
static unsigned FakeFingerprint() { return 0x1234abcd; }

static void Reset() {
    g_reqs.clear();
    g_answers.clear();
    g_haveAccount = true;
    g_forgot = 0;
    g_now = 1000;
    g_pollsBeforeAnswer = 0;
    g_polls = 0;
    g_cancelled = 0;
    NpMpDiag::ClearForTest();
    NpMpDiag::LogTailClearForTest();
    Env e;
    e.account = FakeAccount;
    e.forget = FakeForget;
    e.start = FakeStart;
    e.poll = FakePoll;
    e.cancel = FakeCancel;
    e.nowMs = FakeNow;
    e.appVersion = FakeVersion;
    e.fingerprint = FakeFingerprint;
    SetEnv(e);
}
static Answer Http(int status, const char* body = "") {
    Answer a;
    a.status = status;
    a.body = body;
    return a;
}
static Answer Down() {
    Answer a;
    a.error = 1;
    a.text = "no route";
    return a;
}

static void LogLine(const char* format, ...) {
    va_list a;
    va_start(a, format);
    NpMpDiag::NoteLogLine(format, a);
    va_end(a);
}

// drive an op to its end (the fake answers at once)
static Op::Phase Run(Op& op) {
    for (int i = 0; i < 50 && op.GetPhase() == Op::kSending; ++i) {
        g_now += 10;
        op.Step();
    }
    return op.GetPhase();
}
static Op::Outcome Do(Op::Kind kind, const Answer& answer) {
    Reset();
    g_answers.push_back(answer);
    g_answers.push_back(answer);   // (a transport failure tries the second address)
    Op op;
    CHECK(op.Ask(kind));
    op.Consent();
    CHECK(op.Start());
    Run(op);
    CHECK(op.GetPhase() == Op::kDone);
    return op.GetOutcome();
}

static bool Parse(const std::string& body, NpNet::Json* j) { return j->Parse(body.data(), body.size()); }

int main() {
    // ---- the payload: addresses, ids and tokens never go ---------------------------------------------------------------------------------------
    {
        Reset();
        const char* secrets[] = {"192.168.1.23", "10.0.0.5", "203.0.113.77", "fe80::1c2d:3e4f:5a6b:7c8d", "aa:bb:cc:dd:ee:ff", "0123456789abcdef0123456789abcdef", "tok-SECRETSECRETSECRET0123456789"};
        std::string log, events;
        for (int i = 0; i < 20; ++i) {
            char line[300];
            snprintf(line, sizeof line, "[lan] peer %s:3658 mac %s id %s v6 %s auth Bearer %s\n", secrets[0], secrets[4], secrets[5], secrets[3], secrets[6]);
            log += line;
        }
        events = "link lost: ";
        events += secrets[1];
        events += " then ";
        events += secrets[2];
        events += "\n";
        Inputs in = {};
        in.version = "1.3.0";
        in.build = "1234abcd";
        in.name = "Mario";
        in.log = log.c_str();
        in.events = events.c_str();
        in.conn = "NAT: strict; peer 198.51.100.9 answered";
        char* out = new char[kMaxBytes + 16];
        const size_t n = BuildPayload(in, out, kMaxBytes + 16);
        CHECK(n > 0 && n == strlen(out));
        for (const char* s : secrets) CHECK_CLEAN(out, s);
        CHECK_CLEAN(out, "198.51.100.9");
        NpNet::Json j;
        CHECK(Parse(out, &j));
        CHECK(j.GetNum(j.Root(), "v") == 1);
        CHECK(j.GetStr(j.Root(), "name") == "Mario");
        CHECK(j.GetStr(j.Root(), "app") == "1.3.0");
        CHECK(j.GetStr(j.Root(), "build") == "1234abcd");
        CHECK(!strstr(out, "friend"));
        delete[] out;
    }
    // ---- the size limit: a huge log is cut to 48 KiB (oldest lines first) and the payload still parses -----------------------------------------------
    {
        Reset();
        std::string log;
        for (int i = 0; i < 20000; ++i) {
            char line[120];
            snprintf(line, sizeof line, "[lan] line number %d of a very long log\n", i);
            log += line;
        }
        // (the fields are capped at 24000 + 16000 characters first; text that needs escaping doubles in the JSON and goes over 48 KiB)
        std::string quoted;
        for (int i = 0; i < 400; ++i) quoted += std::string(79, '"') + "\n";
        log += quoted;
        std::string events;
        for (int i = 0; i < 300; ++i) events += std::string(52, '"') + "\n";
        Inputs in = {};
        in.version = "1.3.0";
        in.build = "1234abcd";
        in.name = "Mario";
        in.log = log.c_str();
        in.events = events.c_str();
        in.conn = "";
        char* out = new char[400 * 1024];
        const size_t n = BuildPayload(in, out, 400 * 1024);
        CHECK(n > 0);
        CHECK(n <= kMaxBytes);
        CHECK(n == strlen(out));
        NpNet::Json j;
        CHECK(Parse(out, &j));
        delete[] out;
        // the newest log line survives the cut
        char* out2 = new char[kMaxBytes + 16];
        std::string log2 = "[lan] OLDEST LINE\n" + std::string(100000, 'x') + "\n[lan] NEWEST LINE\n";
        in.log = log2.c_str();
        in.events = "";
        const size_t n2 = BuildPayload(in, out2, kMaxBytes + 16);
        CHECK(n2 > 0 && n2 <= kMaxBytes);
        CHECK(!strstr(out2, "OLDEST LINE"));
        delete[] out2;
        // a destination smaller than the frame: nothing is written beyond it, 0 returned
        char tiny[16];
        CHECK(BuildPayload(in, tiny, sizeof tiny) == 0);
    }
    // ---- the gathered payload: the log tail and the numbers of the last session, nothing else ------------------------------------------------------
    {
        Reset();
        LogLine("[lan] connecting to %s", "192.168.7.8:3658");
        LogLine("[lan] hello from the room");
        NpMpDiag::Note(NpMpDiag::kSessionStart, 1, 2);
        NpMpDiag::Note(NpMpDiag::kInputDelay, 4, 80, 1);
        NpMpDiag::Note(NpMpDiag::kSessionEnd);
        char* out = new char[kMaxBytes + 16];
        const size_t n = GatherPayload(out, kMaxBytes + 16);
        CHECK(n > 0 && n <= kMaxBytes);
        CHECK_CLEAN(out, "192.168.7.8");
        CHECK_CLEAN(out, "tok-SECRET");
        CHECK_CLEAN(out, "one.example");
        CHECK(strstr(out, "hello from the room") != nullptr);
        NpNet::Json j;
        CHECK(Parse(out, &j));
        CHECK(j.GetStr(j.Root(), "name") == "Mario");
        delete[] out;
    }
    // ---- the log tail: bounded, newest lines, scrubbed -------------------------------------------------------------------------------------------
    {
        Reset();
        for (int i = 0; i < 2000; ++i) LogLine("[lan] tail line %d to %s", i, "172.16.0.9");
        char tail[NpMpDiag::kLogTailBytes + 64];
        const size_t n = NpMpDiag::LogTail(tail, sizeof tail);
        CHECK(n > 0 && n < sizeof tail);
        CHECK(strstr(tail, "tail line 1999") != nullptr);
        CHECK(strstr(tail, "tail line 0\n") == nullptr);
        CHECK_CLEAN(tail, "172.16.0.9");
    }
    // ---- the offer is made once for a session that ended with an error --------------------------------------------------------------------------
    {
        Reset();
        CHECK(!NpMpDiag::OfferWanted());                    // nothing happened
        NpMpDiag::Note(NpMpDiag::kSessionStart, 1, 2);
        NpMpDiag::Note(NpMpDiag::kSessionEnd);              // a clean leave
        CHECK(!NpMpDiag::OfferWanted());
        NpMpDiag::Note(NpMpDiag::kSessionStart, 1, 2);
        NpMpDiag::Note(NpMpDiag::kDesync, 10, 2);           // an error
        CHECK(NpMpDiag::OfferWanted());
        NpMpDiag::MarkOffered();
        CHECK(!NpMpDiag::OfferWanted());                    // asked once: not again
        NpMpDiag::Note(NpMpDiag::kLinkLost, 0, 0, 0, "gone");   // more errors in the same session
        NpMpDiag::Note(NpMpDiag::kConnectionLost, 1, 2);
        CHECK(!NpMpDiag::OfferWanted());
        NpMpDiag::Note(NpMpDiag::kSessionStart, 1, 2);      // a new session
        CHECK(!NpMpDiag::OfferWanted());
        NpMpDiag::Note(NpMpDiag::kOnlineFail, 0, 0, 0, "boom");
        CHECK(NpMpDiag::OfferWanted());                     // its own error arms it again
    }
    // ---- consent: nothing is sent before SEND / YES -------------------------------------------------------------------------------------------
    {
        Reset();
        Op op;
        CHECK(!op.Start());                                 // never asked
        CHECK(g_reqs.empty());
        CHECK(op.Ask(Op::kReport));
        CHECK(g_reqs.empty());                              // showing what would be sent sends nothing
        CHECK(!op.Start());                                 // asked but not consented
        CHECK(g_reqs.empty());
        CHECK(op.GetPhase() == Op::kAsked);
        op.Cancel();                                        // CANCEL / Circle
        op.Consent();
        CHECK(!op.Start());
        CHECK(g_reqs.empty());
        CHECK(op.Ask(Op::kDelete));
        CHECK(!op.Start());                                 // the same for the delete
        op.Cancel();
        CHECK(g_reqs.empty() && g_forgot == 0);
        // with consent: exactly one request
        g_answers.push_back(Http(200));
        CHECK(op.Ask(Op::kReport));
        op.Consent();
        CHECK(op.Start());
        CHECK(g_reqs.size() == 1);
        CHECK(!op.Start());                                 // not twice
        CHECK(g_reqs.size() == 1);
    }
    // ---- what a report request looks like -----------------------------------------------------------------------------------------------------
    {
        Reset();
        LogLine("[lan] something");
        g_answers.push_back(Http(200));
        Op op;
        CHECK(op.Ask(Op::kReport));
        op.Consent();
        CHECK(op.Start());
        Run(op);
        CHECK(g_reqs.size() == 1);
        const Req& r = g_reqs[0];
        CHECK(r.method == "POST");
        CHECK(r.url == "https://one.example/v1/report");
        bool bearer = false;
        for (const std::string& h : r.headers) bearer |= h == "Authorization: Bearer tok-SECRETSECRETSECRET0123456789";
        CHECK(bearer);
        CHECK(r.headers.size() <= 8);                       // np_https takes at most 8 headers
        size_t bytes = 0;
        for (const std::string& h : r.headers) bytes += h.size() + 2;
        CHECK(bytes <= 1024);
        CHECK(r.body.size() <= kMaxBytes);
        CHECK_CLEAN(r.body.c_str(), "tok-SECRET");
        NpNet::Json j;
        CHECK(Parse(r.body, &j));
    }
    // ---- the answers ---------------------------------------------------------------------------------------------------------------------------
    CHECK(Do(Op::kReport, Http(200, "{\"ok\":true}")) == Op::kSent);
    CHECK(Do(Op::kReport, Http(429, "{\"error\":{\"code\":\"rate_limited\"}}")) == Op::kLimitReached);
    CHECK(Do(Op::kReport, Http(503, "{\"error\":{\"code\":\"over_budget\"}}")) == Op::kBusy);
    CHECK(Do(Op::kReport, Http(413)) == Op::kTooBig);
    CHECK(Do(Op::kReport, Http(404)) == Op::kNotYet);
    CHECK(Do(Op::kReport, Http(400)) == Op::kRefused);
    {   // both addresses down: no connection, and the second address was tried
        Reset();
        g_answers.push_back(Down());
        g_answers.push_back(Down());
        Op op;
        CHECK(op.Ask(Op::kReport));
        op.Consent();
        CHECK(op.Start());
        Run(op);
        CHECK(op.GetOutcome() == Op::kNoConnection);
        CHECK(g_reqs.size() == 2 && g_reqs[1].url == "https://two.example/v1/report");
        CHECK(!op.Succeeded());
        CHECK(op.Sentence() == "No connection to the online service. Check your Wi-Fi.");
    }
    {   // the first address is down, the second answers
        Reset();
        g_answers.push_back(Down());
        g_answers.push_back(Http(200));
        Op op;
        CHECK(op.Ask(Op::kReport));
        op.Consent();
        CHECK(op.Start());
        Run(op);
        CHECK(op.GetOutcome() == Op::kSent && op.Succeeded());
        CHECK(op.Sentence() == "Report sent. Thank you.");
    }
    {   // no account: nothing to send, said so, no request
        Reset();
        g_haveAccount = false;
        Op op;
        CHECK(!op.Ask(Op::kReport));
        CHECK(op.GetOutcome() == Op::kNoAccount);
        CHECK(op.Sentence() == "No online account on this Vita yet. Play online once first.");
        op.Consent();
        CHECK(!op.Start());
        CHECK(g_reqs.empty());
    }
    {   // the player gives up while it is sending: the request is cancelled, nothing else happens
        Reset();
        g_pollsBeforeAnswer = 1000;
        Op op;
        CHECK(op.Ask(Op::kReport));
        op.Consent();
        CHECK(op.Start());
        op.Step();
        CHECK(op.GetPhase() == Op::kSending);
        op.Cancel();
        CHECK(g_cancelled == 1 && op.GetPhase() == Op::kIdle);
    }
    {   // no answer for 30 s: no connection
        Reset();
        g_pollsBeforeAnswer = 100000;
        g_answers.push_back(Http(200));
        g_answers.push_back(Http(200));
        Op op;
        CHECK(op.Ask(Op::kReport));
        op.Consent();
        CHECK(op.Start());
        op.Step();
        g_now += 31000;
        op.Step();
        CHECK(op.GetOutcome() != Op::kSent);
    }
    // ---- DELETE MY ONLINE ACCOUNT ---------------------------------------------------------------------------------------------------------------
    {
        CHECK(Do(Op::kDelete, Http(200, "{\"ok\":true}")) == Op::kDeleted);
        CHECK(g_forgot == 1);
        CHECK(g_reqs.size() == 1 && g_reqs[0].method == "POST" && g_reqs[0].url == "https://one.example/v1/account" && g_reqs[0].body == "{}");
        CHECK(Do(Op::kDelete, Http(401, "{\"error\":{\"code\":\"bad_token\"}}")) == Op::kDeleted);   // already gone: that is what was asked
        CHECK(g_forgot == 1);
        CHECK(Do(Op::kDelete, Http(404)) == Op::kNotYet);   // an older service: nothing was deleted, so nothing is forgotten
        CHECK(g_forgot == 0);
        CHECK(Do(Op::kDelete, Http(503)) == Op::kBusy);
        CHECK(g_forgot == 0);
        CHECK(Do(Op::kDelete, Http(429)) == Op::kLimitReached);
        CHECK(g_forgot == 0);
        CHECK(Do(Op::kDelete, Http(400)) == Op::kRefused);
        CHECK(g_forgot == 0);
        CHECK(Do(Op::kDelete, Down()) == Op::kKeptNoConnection);   // no connection: the identity stays, so the player can try again
        CHECK(g_forgot == 0);
        Op op;
        Reset();
        g_answers.push_back(Http(200));
        CHECK(op.Ask(Op::kDelete));
        op.Consent();
        CHECK(op.Start());
        Run(op);
        CHECK(op.Succeeded());
        CHECK(op.Sentence() == "Your online account was deleted. This Vita forgot its name.");
    }
    {   // nothing to delete on the service (no token): this Vita's leftovers go, and it says so
        Reset();
        g_haveAccount = false;
        Op op;
        CHECK(op.Ask(Op::kDelete));
        op.Consent();
        CHECK(op.Start());
        CHECK(op.GetOutcome() == Op::kNoAccount);
        CHECK(g_reqs.empty());
    }
    // ---- every sentence is short enough for the settings line (60 characters) -------------------------------------------------------------------------
    {
        const Op::Kind kinds[] = {Op::kReport, Op::kDelete};
        const Op::Outcome outcomes[] = {Op::kSent, Op::kLimitReached, Op::kNoConnection, Op::kBusy, Op::kNoAccount, Op::kTooBig, Op::kRefused, Op::kDeleted, Op::kNotYet, Op::kKeptNoConnection};
        for (Op::Kind k : kinds)
            for (Op::Outcome o : outcomes) {
                Reset();
                // reach the outcome through the answers where one maps to it; the sentence itself is what is checked
                Answer a = o == Op::kSent || o == Op::kDeleted ? Http(200) : o == Op::kLimitReached ? Http(429) : o == Op::kBusy ? Http(503) : o == Op::kTooBig ? Http(413)
                           : o == Op::kNotYet ? Http(404) : o == Op::kNoConnection || o == Op::kKeptNoConnection ? Down() : Http(400);
                if (o == Op::kNoAccount) g_haveAccount = false;
                g_answers.push_back(a);
                g_answers.push_back(a);
                Op op;
                if (!op.Ask(k)) { CHECK(!op.Sentence().empty()); CHECK(op.Sentence().size() <= 60); continue; }
                op.Consent();
                op.Start();
                Run(op);
                CHECK(!op.Sentence().empty());
                CHECK(op.Sentence().size() <= 60);
            }
    }
    // ---- the texts ---------------------------------------------------------------------------------------------------------------------------------
    {
        const std::string c = ConsentText();
        CHECK(c.find("online name") != std::string::npos);
        CHECK(c.find("log") != std::string::npos);
        CHECK(c.find("connection check") != std::string::npos);
        CHECK(c.find("last online game") != std::string::npos);
        CHECK(c.find("IP addresses") != std::string::npos && c.find("removed") != std::string::npos);
        CHECK(c.find("friend list is not sent") != std::string::npos);
        CHECK(c.size() < 480);   // a popup holds 480 UTF-16 units
        CHECK(strlen(OfferText()) < 480);
    }
    std::printf("mpreport: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
