// NpOnline::Api over the service's HTTP/JSON protocol. See np_online_http.h and server/PROTOCOL.md.
#include "platform/sdk/dwc/np_online_http.h"
#include "platform/sdk/dwc/np_pow.h"   // gxnet14: the registration proof of work (Galaxy port, svcfix2)

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>

#include "platform/sdk/dwc/np_online_json.h"
#include "platform/sdk/net/online/np_text_safe.h"
#include "platform/sdk/net/online/np_service_busy.h"

#define HTTP_LOG(...) fprintf(stderr, "[online] " __VA_ARGS__)

namespace NpOnline {
namespace {

const uint32_t kStepTimeoutMs = 12000;

enum Kind { kLogin = 1, kTime, kPut, kGet, kReport, kStatus, kFriends, kPlayed, kFop };
enum Step { sInfo = 1, sMe, sRegister, sRename, sTime, sPut, sGet, sReport, sStatus, sFriends, sPlayed, sFop };

struct Op {
    Kind kind;
    Step step;
    uint32_t transportId;
    // result of the whole operation once finished
    bool done;
    Result result;
    LoginReply login;
    TimeReply time;
    GetReply get;
    PutReply put;
    MatchStatus match;
    FriendsReply friends;
    PlayedReply played;
    FriendOpReply fop;
    // inputs kept for the later steps
    LoginRequest loginRequest;
    int64_t serverMs;           // from /v1/info
    uint64_t serverMsAt;        // NowMs() when it arrived
    uint32_t getMode;
    bool heldFriends;           // a held friends poll (the service keeps the answer for the hold)
    // gxnet14: the registration proof of work (the service answered 428 pow_required): the solver runs in slices from Advance(), then the register is sent again with the proof
    bool solving;
    int powRounds;              // proofs asked for so far in this sign-up (the price may rise between two: at most kPowRounds)
    std::string powChallenge, powNonce;
    NpPow::Solver solver;
    Op() : kind(kLogin), step(sInfo), transportId(0), done(false), serverMs(0), serverMsAt(0), getMode(0), heldFriends(false), solving(false), powRounds(0) {}
};

std::string Num(long long v) { char b[32]; snprintf(b, sizeof b, "%lld", v); return b; }

bool SafeId(const char* id) {
    if (!id || !*id) return false;
    for (size_t i = 0; id[i]; ++i) {
        const char c = id[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-') || i > 40) return false;
    }
    return true;
}

unsigned g_heldFriendsEnded = 0;   // (test counter)

class HttpApi : public Api {
public:
    explicit HttpApi(HttpTransport* transport) : m_t(transport), m_next(0), m_region(2), m_pid(0) { pthread_mutex_init(&m_lock, nullptr); }
    ~HttpApi() override {
        for (std::map<uint32_t, Op>::iterator it = m_ops.begin(); it != m_ops.end(); ++it)
            if (it->second.transportId) m_t->Cancel(it->second.transportId);
        delete m_t;
        pthread_mutex_destroy(&m_lock);
    }

    uint32_t BeginLogin(const LoginRequest& request) override {
        Lock l(m_lock);
        SetNameRejectMessage("");
        Op op;
        op.kind = kLogin;
        op.loginRequest = request;
        m_region = request.region ? request.region : 2;
        if (!StartStep(&op, sInfo)) return 0;
        return Add(op);
    }
    uint32_t BeginTime() override {
        Lock l(m_lock);
        Op op;
        op.kind = kTime;
        if (!StartStep(&op, sTime)) return 0;
        return Add(op);
    }
    uint32_t BeginPutScore(const PutRequest& request) override {
        Lock l(m_lock);
        Op op;
        op.kind = kPut;
        HttpRequest http = Base("POST", "/v1/rank/put", true);
        uint32_t region = request.region;
        if (region != 1 && region != 2 && region != 4 && region != 8) region = m_region;   // the service wants exactly one region
        long long score = request.score;
        if (score < 0) score = 0;
        if (score > 999999) score = 999999;
        http.body = "{\"category\":" + Num(request.category) + ",\"region\":" + Num(region) + ",\"score\":" + Num(score) + ",\"data\":\"" +
                    Base64Encode(request.data, request.size) + "\"}";
        AddQueueHeaders(&http, request.attempt, request.build, nullptr);
        if (!Send(&op, sPut, http)) return 0;
        return Add(op);
    }
    uint32_t BeginGetScores(const GetRequest& request) override {
        Lock l(m_lock);
        Op op;
        op.kind = kGet;
        op.getMode = request.mode;
        if (NpNet::ServiceBusyBlocksBrowse()) {   // the service closed the rankings for today: no request
            SetStatusMessage(NpNet::BusySentence(NpNet::BUSY_BROWSE));
            return 0;
        }
        HttpRequest http = Base("POST", "/v1/rank/get", true);
        std::string body = "{\"mode\":" + Num(request.mode) + ",\"category\":" + Num(request.category) + ",\"region\":" + Num(request.region & 0xFF) +
                           ",\"sort\":" + Num(request.sort) + ",\"since\":" + Num(request.since) + ",\"limit\":" + Num(request.limit);
        if (request.mode == 3) {
            body += ",\"friends\":[";
            for (uint32_t i = 0; i < request.friendCount && i < kMaxFriends; ++i) body += (i ? "," : "") + Num(request.friends[i]);
            body += "]";
        }
        http.body = body + "}";
        if (!Send(&op, sGet, http)) return 0;
        return Add(op);
    }
    uint32_t BeginMatchStatus(const char* matchId) override {
        Lock l(m_lock);
        Op op;
        op.kind = kStatus;
        if (!SafeId(matchId)) return 0;
        if (!Send(&op, sStatus, Base("GET", std::string("/v1/matches/") + matchId, true))) return 0;
        return Add(op);
    }
    uint32_t BeginReportResult(const ResultReport& report) override {
        Lock l(m_lock);
        Op op;
        op.kind = kReport;
        if (!SafeId(report.matchId)) return 0;
        HttpRequest http = Base("POST", std::string("/v1/matches/") + report.matchId + "/result", true);
        http.body = "{\"result\":" + Num(report.result) + ",\"score0\":" + Num(report.score0) + ",\"score1\":" + Num(report.score1) +
                    ",\"checksum\":" + JsonQuote(report.checksum) + "}";
        AddQueueHeaders(&http, report.attempt, report.build, report.mode);
        if (!Send(&op, sReport, http)) return 0;
        return Add(op);
    }

    uint32_t BeginFriends(const FriendsRequest& request) override {
        Lock l(m_lock);
        Op op;
        op.kind = kFriends;
        if (NpNet::ServiceBusyBlocksBrowse()) { SetStatusMessage(NpNet::BusySentence(NpNet::BUSY_BROWSE)); return 0; }   // the friends list is browsing: no request
        std::string path = "/v1/friends";
        bool first = true;
        for (size_t i = 0; request.rev[i] && i < sizeof request.rev; ++i) if (!isalnum(static_cast<unsigned char>(request.rev[i]))) return 0;
        if (request.rev[0]) { path += std::string("?v=") + request.rev; first = false; }
        if (request.waitSec) path += std::string(first ? "?" : "&") + "wait=" + Num(request.waitSec > 55 ? 55 : request.waitSec);
        HttpRequest http = Base("GET", path, true);
        if (request.waitSec) {
            const uint32_t holdMs = (request.waitSec > 55 ? 55 : request.waitSec) * 1000;
            http.timeoutMs += holdMs;
            http.ioTimeoutMs = holdMs + 10000;   // the service holds the answer: no data for the hold is normal
        }
        op.heldFriends = request.waitSec != 0;
        snprintf(op.friends.rev, sizeof op.friends.rev, "%s", request.rev);   // (an answer given up on the caller's behalf keeps the revision it asked about)
        if (!Send(&op, sFriends, http)) return 0;
        return Add(op);
    }
    uint32_t BeginPlayedWith() override {
        Lock l(m_lock);
        Op op;
        op.kind = kPlayed;
        if (NpNet::ServiceBusyBlocksBrowse()) { SetStatusMessage(NpNet::BusySentence(NpNet::BUSY_BROWSE)); return 0; }
        if (!Send(&op, sPlayed, Base("GET", "/v1/friends/recent", true))) return 0;
        return Add(op);
    }
    uint32_t BeginFriendOp(const FriendOpRequest& request) override {
        Lock l(m_lock);
        Op op;
        op.kind = kFop;
        const char* path = "/v1/friends/add";
        switch (request.op) {
        case FriendOp::Add: path = "/v1/friends/add"; break;
        case FriendOp::Decline: path = "/v1/friends/decline"; break;
        case FriendOp::Remove: path = "/v1/friends/remove"; break;
        case FriendOp::Block: path = "/v1/friends/block"; break;
        case FriendOp::Unblock: path = "/v1/friends/unblock"; break;
        case FriendOp::Invite: path = "/v1/friends/invite"; break;
        case FriendOp::ClearInvite: path = "/v1/friends/invite/clear"; break;
        case FriendOp::SetStatus: path = "/v1/friends/status"; break;
        default: return 0;
        }
        if (request.op == FriendOp::Invite && NpNet::ServiceBusyBlocksNew()) { SetStatusMessage(NpNet::BusySentence(NpNet::BusyState().level)); return 0; }
        std::string body;
        if (request.op == FriendOp::SetStatus) {
            for (size_t i = 0; request.status[i]; ++i) {
                const char ch = request.status[i];
                if (!(isalnum(static_cast<unsigned char>(ch)) || ch == '+' || ch == '/' || ch == '=' || ch == '.' || ch == '-' || ch == '*') || i >= kStatusChars - 1) return 0;
            }
            body = std::string("{\"status\":") + JsonQuote(request.status) + "}";
        } else {
            if (request.pid > 0) body = "{\"pid\":" + Num(request.pid);
            else {
                for (size_t i = 0; request.code[i]; ++i) if (!(isalnum(static_cast<unsigned char>(request.code[i])) || request.code[i] == '-') || i > 12) return 0;
                if (!request.code[0]) return 0;
                body = std::string("{\"code\":") + JsonQuote(request.code);
            }
            if (request.op == FriendOp::Invite) {
                for (size_t i = 0; request.roomCode[i]; ++i) if (!isalnum(static_cast<unsigned char>(request.roomCode[i])) || i > 6) return 0;
                if (!request.roomCode[0]) return 0;
                body += std::string(",\"roomCode\":") + JsonQuote(request.roomCode);
            }
            body += "}";
        }
        HttpRequest http = Base("POST", path, true);
        http.body = body;
        if (!Send(&op, sFop, http)) return 0;
        return Add(op);
    }
    Result PollFriends(uint32_t id, FriendsReply* reply) override { Result r; Op* op = Advance(id, kFriends, &r); if (op && reply) *reply = op->friends; return Finish(id, op, r); }
    Result PollPlayedWith(uint32_t id, PlayedReply* reply) override { Result r; Op* op = Advance(id, kPlayed, &r); if (op && reply) *reply = op->played; return Finish(id, op, r); }
    Result PollFriendOp(uint32_t id, FriendOpReply* reply) override { Result r; Op* op = Advance(id, kFop, &r); if (op && reply) *reply = op->fop; return Finish(id, op, r); }

    Result PollLogin(uint32_t id, LoginReply* reply) override { Result r; Op* op = Advance(id, kLogin, &r); if (op && reply) *reply = op->login; return Finish(id, op, r); }
    Result PollTime(uint32_t id, TimeReply* reply) override { Result r; Op* op = Advance(id, kTime, &r); if (op && reply) *reply = op->time; return Finish(id, op, r); }
    Result PollPutScore(uint32_t id, PutReply* reply) override { Result r; Op* op = Advance(id, kPut, &r); if (op && reply) *reply = op->put; return Finish(id, op, r); }
    Result PollMatchStatus(uint32_t id, MatchStatus* status) override { Result r; Op* op = Advance(id, kStatus, &r); if (op && status) *status = op->match; return Finish(id, op, r); }
    Result PollGetScores(uint32_t id, GetReply* reply) override { Result r; Op* op = Advance(id, kGet, &r); if (op && reply) *reply = op->get; return Finish(id, op, r); }
    Result PollReportResult(uint32_t id) override { Result r; Op* op = Advance(id, kReport, &r); return Finish(id, op, r); }

    void Cancel(uint32_t id) override {
        Lock l(m_lock);
        std::map<uint32_t, Op>::iterator it = m_ops.find(id);
        if (it == m_ops.end()) return;
        if (it->second.transportId) m_t->Cancel(it->second.transportId);
        m_ops.erase(it);
    }

private:
    struct Lock { pthread_mutex_t& m; explicit Lock(pthread_mutex_t& mm) : m(mm) { pthread_mutex_lock(&m); } ~Lock() { pthread_mutex_unlock(&m); } };

    HttpRequest Base(const char* method, const std::string& path, bool auth) {
        HttpRequest r;
        r.method = method;
        r.path = path;
        r.timeoutMs = kStepTimeoutMs;
        const ClientInfo& info = GetClientInfo();
        r.headers.push_back("X-Msc-Proto: 1");
        r.headers.push_back(std::string("X-Msc-App: ") + info.appVersion);
        if (info.build[0]) r.headers.push_back(std::string("X-Msc-Build: ") + info.build);
        if (RulesTag()[0]) r.headers.push_back(std::string("X-Msc-Rules: ") + RulesTag());
        if (auth && !m_token.empty()) r.headers.push_back("Authorization: Bearer " + m_token);
        return r;
    }
    static bool ValidBuild(const char* build) {   // the service wants 8..64 hex digits
        size_t n = 0;
        for (; build && build[n]; ++n) if (!isxdigit(static_cast<unsigned char>(build[n]))) return false;
        return n >= 8 && n <= 64;
    }
    static void AddQueueHeaders(HttpRequest* r, uint32_t attempt, const char* build, const char* mode) {
        r->headers.push_back("X-Msc-Attempt: " + Num(attempt));
        if (ValidBuild(build)) {   // the build that earned it
            bool replaced = false;
            for (size_t i = 0; i < r->headers.size(); ++i)
                if (r->headers[i].compare(0, 13, "X-Msc-Build: ") == 0) { r->headers[i] = std::string("X-Msc-Build: ") + build; replaced = true; }
            if (!replaced) r->headers.push_back(std::string("X-Msc-Build: ") + build);
        }
        if (mode && mode[0]) r->headers.push_back(std::string("X-Msc-Mode: ") + mode);
    }
    uint32_t Add(const Op& op) { const uint32_t id = ++m_next; m_ops[id] = op; return id; }

    // The HTTPS client has one worker and one connection: a held friends poll (up to 45 s) would keep every other request waiting behind it, and the
    // callers (rankings 15 s, results 15 s, the clock 10 s) give up before it ends. So any other request first ends the held poll: it is finished as
    // "nothing new" (the friends module asks again at once, now behind the new request). Cost: one new connection. Applies to every transport.
    void EndHeldFriends() {
        for (std::map<uint32_t, Op>::iterator it = m_ops.begin(); it != m_ops.end(); ++it) {
            Op& o = it->second;
            if (o.kind != kFriends || o.done || !o.heldFriends || !o.transportId) continue;
            m_t->Cancel(o.transportId);
            o.transportId = 0;
            o.friends.changed = false;
            o.friends.heldMs = 1000;
            FinishOk(&o);
            ++g_heldFriendsEnded;
            HTTP_LOG("held friends poll ended early: another request is waiting\n");
        }
    }

    bool Send(Op* op, Step step, const HttpRequest& http) {
        if (step != sFriends) EndHeldFriends();
        const uint32_t tid = m_t->Start(http);
        if (!tid) return false;
        op->step = step;
        op->transportId = tid;
        return true;
    }

    bool StartStep(Op* op, Step step) {
        switch (step) {
        case sInfo: return Send(op, sInfo, Base("GET", "/v1/info", false));
        case sTime: return Send(op, sTime, Base("GET", "/v1/time", false));
        case sMe: return Send(op, sMe, Base("GET", "/v1/me", true));
        case sRegister: {
            HttpRequest h = Base("POST", "/v1/register", false);
            h.body = "{\"name\":" + JsonQuote(op->loginRequest.name) + ",\"region\":" + Num(m_region);
            if (!op->powNonce.empty()) h.body += ",\"pow\":{\"challenge\":" + JsonQuote(op->powChallenge) + ",\"nonce\":" + JsonQuote(op->powNonce) + "}";
            h.body += "}";
            return Send(op, sRegister, h);
        }
        case sRename: {
            HttpRequest h = Base("PUT", "/v1/me", true);
            h.body = "{\"name\":" + JsonQuote(op->loginRequest.name) + "}";
            return Send(op, sRename, h);
        }
        default: return false;
        }
    }

    static void FinishOk(Op* op) { op->done = true; op->result.status = Status::Ok; op->result.http = 200; }
    static void FinishFail(Op* op, Fail fail, int http, int retryAfter = 0) {
        op->done = true;
        op->result.status = Status::Failed;
        op->result.fail = fail;
        op->result.http = http;
        op->result.retryAfterSec = retryAfter;
    }

    // Turns a non-200 answer into a failure; sets the status message for conditions the player can act on.
    static Fail Classify(const HttpResponse& res, std::string* code, std::string* reason, int* retryAfter) {
        JValue j;
        std::string message;
        *retryAfter = 0;
        if (reason) reason->clear();
        if (JsonParse(res.body.data(), res.body.size(), &j)) {
            if (const JValue* e = j.Get("error")) {
                *code = e->Str_("code");
                if (reason) *reason = e->Str_("reason");
                message = e->Str_("message");
                *retryAfter = int(e->Num_("retryAfterSec", 0));
            }
        }
        if (res.status == 426 || *code == "upgrade_required") {
            std::string latest;
            if (const JValue* e = j.Get("error")) { const JValue* v = e->Get("latestVersion"); if (v && v->IsStr()) latest = v->s; else if (v && v->IsNum()) latest = Num((long long)v->n); }
            std::string text = "A newer version of the game is needed to play online";
            if (!latest.empty() && latest.size() < 24) text += " (" + latest + ")";
            SetStatusMessage((text + ".").c_str());
            return Fail::Upgrade;
        }
        if (NpNet::IsEdgeBusyPage(false, res.status, !code->empty())) {   // gxnet14: Cloudflare's own 429 page (the free quota is spent), not the service's JSON: "busy", remembered
            NpNet::NoteServiceBusy("", NpNet::kEdgeBusyQuietSec);
            *retryAfter = NpNet::kEdgeBusyQuietSec;
            SetStatusMessage(NpNet::BusySentence(NpNet::BUSY_ALL));
            return Fail::Disabled;
        }
        if (res.status == 503 && *code == "over_budget") {
            // over the service's daily budget (PROTOCOL.md "Budget and pacing"): remembered by the console (np_service_busy.h), a plain sentence for the player;
            // the retry queue waits the Retry-After instead of its own backoff
            std::string level;
            if (const JValue* e = j.Get("error")) level = e->Str_("level");
            NpNet::NoteServiceBusy(level.c_str(), *retryAfter);
            SetStatusMessage(NpNet::BusySentence(NpNet::BusyLevelOf(level.c_str())));
            return Fail::Disabled;
        }
        if (*code == "service_disabled") { SetStatusMessage(message.empty() ? "Online play is switched off for now." : message.c_str()); return Fail::Disabled; }
        if (res.status == 401) return Fail::Auth;
        if (res.status == 403) { if (*code == "banned") SetStatusMessage("This online profile is not allowed to play."); return Fail::Auth; }
        if (res.status == 429) return Fail::RateLimited;
        if (res.status >= 500) return Fail::Server;
        return Fail::Rejected;
    }

    // ---- gxnet14: the registration proof of work (PROTOCOL.md "Optional proof of work"; the Galaxy port's flow, svcfix2) -----------------------------------------
    // The service answers a register without proof with 428 pow_required, the error carrying { bits, challenge }: no extra request while it is off. The challenge is
    // solved here in slices (np_pow.h), then the register goes again with {"pow":{challenge, nonce}}. The price may rise between two tries (an address that registered
    // meanwhile): at most kPowRounds rounds. A price above NpPow::kMaxBits is not ground (about 2 s at 17 bits on a Vita, an ESTIMATE): the player reads "very busy".
    // MSC_NET_POW=0 switches the whole thing off (the 428 is then an ordinary refusal, as before wave 14).
    static const int kPowRounds = 3;
    static const uint32_t kPowSliceMs = 6;   // of every poll
    static bool PowOn() { const char* v = getenv("MSC_NET_POW"); return !(v && v[0] == '0' && v[1] == 0); }   // (read at each 428: a sign-up is rare)
    // a challenge as the service makes them: digits, dots and hex, 20..80 characters (anything else is not ours)
    static bool GoodChallenge(const std::string& c) {
        if (c.size() < 20 || c.size() > 80) return false;
        for (size_t i = 0; i < c.size(); ++i) if (!(isdigit(static_cast<unsigned char>(c[i])) || c[i] == '.' || (c[i] >= 'a' && c[i] <= 'f'))) return false;
        return true;
    }
    bool BeginPow(Op* op, const JValue& j) {
        if (!PowOn() || op->powRounds >= kPowRounds) return false;
        const JValue* e = j.Get("error");
        if (!e || !e->IsObj() || e->Str_("code") != "pow_required") return false;
        const double bits = e->Num_("bits", -1);
        const std::string ch = e->Str_("challenge");
        if (!GoodChallenge(ch) || !(bits >= 1 && bits <= 28)) { HTTP_LOG("sign-up: the proof-of-work challenge is not one this client knows\n"); return false; }
        if (bits > NpPow::kMaxBits) {   // more than about 10 s of grinding: not a difficulty a player waits for
            HTTP_LOG("sign-up: proof of work bits=%d is above this client's limit (%d)\n", int(bits), NpPow::kMaxBits);
            SetStatusMessage("Online play is very busy right now. Please try again in a little while.");
            FinishFail(op, Fail::Disabled, 428, 60);
            return true;
        }
        if (!op->solver.Start(ch.c_str(), int(bits))) return false;
        op->powChallenge = ch;
        op->powNonce.clear();
        op->solving = true;
        ++op->powRounds;
        HTTP_LOG("sign-up: solving a proof of work of %d bits (round %d)\n", int(bits), op->powRounds);
        return true;
    }
    // One slice; true when the register has been sent again (or the operation ended), false while still working.
    bool SolveSlice(Op* op) {
        const uint64_t t0 = NowMs();
        int rc = 0;
        do { rc = op->solver.Step(256); } while (rc == 0 && NowMs() - t0 < kPowSliceMs);
        if (rc == 0) return false;
        op->solving = false;
        if (rc < 0) { FinishFail(op, Fail::Protocol, 428); return true; }
        op->powNonce = op->solver.nonce;
        HTTP_LOG("sign-up: proof of work found\n");
        if (!StartStep(op, sRegister)) FinishFail(op, Fail::Transport, 0);
        return true;
    }

    // Advances an operation by one response; returns the op (or null for an unknown id).
    Op* Advance(uint32_t id, Kind kind, Result* r) {
        Lock l(m_lock);
        std::map<uint32_t, Op>::iterator it = m_ops.find(id);
        if (it == m_ops.end() || it->second.kind != kind) { r->status = Status::Failed; r->fail = Fail::Cancelled; return nullptr; }
        Op& op = it->second;
        if (!op.done && op.solving) {   // gxnet14: a few milliseconds of proof of work per call (the caller polls once per frame)
            if (!SolveSlice(&op)) { r->status = Status::Pending; return nullptr; }
        }
        if (!op.done) {
            HttpResponse res;
            if (!m_t->Poll(op.transportId, &res)) { r->status = Status::Pending; return nullptr; }
            op.transportId = 0;
            Process(&op, res);
        }
        *r = op.result;
        return &op;
    }
    Result Finish(uint32_t id, Op* op, const Result& r) {
        if (r.status == Status::Pending) return r;
        Lock l(m_lock);
        m_ops.erase(id);
        (void)op;
        return r;
    }

    void Process(Op* op, const HttpResponse& res) {
        if (res.fail != Fail::None) {   // no answer
            HTTP_LOG("request failed before an answer (%s)\n", res.detail.c_str());
            if (!res.advice.empty()) SetStatusMessage(res.advice.c_str());
            FinishFail(op, res.fail, 0);
            return;
        }
        std::string code, reason;
        int retryAfter = 0;
        JValue j;
        const bool parsed = JsonParse(res.body.data(), res.body.size(), &j) && j.IsObj();
        if (res.status == 428 && op->step == sRegister && parsed && BeginPow(op, j)) return;   // gxnet14: the service wants a proof of work first
        if (res.status != 200) {
            const Fail fail = Classify(res, &code, &reason, &retryAfter);
            if (HandleKnownError(op, res.status, code, reason, fail)) return;
            HTTP_LOG("the service answered %d %s\n", res.status, code.c_str());
            FinishFail(op, fail, res.status, retryAfter);
            snprintf(op->result.code, sizeof op->result.code, "%s", code.c_str());
            return;
        }
        if (!parsed) { FinishFail(op, Fail::Protocol, res.status); return; }
        switch (op->step) {
        case sInfo: OnInfo(op, j); break;
        case sTime: {
            const double ms = j.Num_("utcMs", 0);
            if (ms <= 0) { FinishFail(op, Fail::Protocol, 200); break; }
            op->time.unixMs = int64_t(ms);
            FinishOk(op);
            break;
        }
        case sMe: OnMe(op, j); break;
        case sRegister: OnRegister(op, j); break;
        case sRename: OnLoginDone(op); break;
        case sPut:
            op->put.stored = int32_t(j.Num_("stored", 0));
            op->put.clamped = j.Bool_("clamped", false);
            FinishOk(op);
            break;
        case sStatus: {
            op->match.final = j.Bool_("final", false);
            NpNet::SafeCopy(op->match.status, sizeof op->match.status, j.Str_("status").c_str());
            if (const JValue* c = j.Get("credit")) { op->match.points = int32_t(c->Num_("points", 0)); op->match.won = c->Bool_("won", false); }
            FinishOk(op);
            break;
        }
        case sGet: OnGet(op, j); break;
        case sFriends: OnFriends(op, j); break;
        case sPlayed: OnPlayed(op, j); break;
        case sFop: OnFop(op, j); break;
        case sReport: FinishOk(op); break;
        }
    }

    static const char* NameRejectSentence(const std::string& code, const std::string& reason) {
        if (code == "name_taken") return "That name is already taken. Choose another one.";
        if (reason == "reserved") return "That name is reserved. Choose another one.";
        if (reason == "filtered") return "That name is not allowed. Choose another one.";
        return "Use 2 to 10 letters or digits for the name.";
    }

    // Errors with a meaning for the operation itself. Returns true when the operation was finished here.
    bool HandleKnownError(Op* op, int status, const std::string& code, const std::string& reason, Fail fail) {
        (void)fail;
        if (op->step == sRegister && (code == "bad_name" || code == "name_taken")) {
            HTTP_LOG("the service did not accept the name (%s%s%s)\n", code.c_str(), reason.empty() ? "" : "/", reason.c_str());
            SetNameRejectMessage(NameRejectSentence(code, reason));
            op->login.profileId = 0;
            op->login.nameCheck = NameCheck::Invalid;
            FinishOk(op);
            return true;
        }
        if (op->step == sRename) {
            if (code == "bad_name" || code == "name_taken") {
                HTTP_LOG("the service did not accept the name (%s%s%s)\n", code.c_str(), reason.empty() ? "" : "/", reason.c_str());
                SetNameRejectMessage(NameRejectSentence(code, reason));
                op->login.nameCheck = NameCheck::Invalid;
                OnLoginDone(op, true);
                return true;
            }
            if (code == "rename_too_soon") {   // the account keeps its registered name; the ranking uploads will then be refused by the name check
                HTTP_LOG("the name cannot be changed yet (once per day); signing in with the registered name\n");
                OnLoginDone(op);
                return true;
            }
        }
        if (op->step == sReport && status == 409 && code == "match_closed") { FinishOk(op); return true; }   // already settled: nothing left to report
        return false;
    }

    void OnInfo(Op* op, const JValue& j) {
        const double proto = j.Num_("minProto", 1);
        if (proto > 1) {
            std::string text = "A newer version of the game is needed to play online";
            if (const JValue* app = j.Get("app")) { const JValue* latest = app->Get("latest"); if (latest && latest->IsStr() && !latest->s.empty() && latest->s.size() < 24) text += " (" + latest->s + ")"; }
            SetStatusMessage((text + ".").c_str());
            FinishFail(op, Fail::Upgrade, 200);
            return;
        }
        const JValue* kill = j.Get("kill");
        if (kill && kill->IsArr()) {
            for (size_t i = 0; i < kill->a.size(); ++i) {
                const std::string& k = kill->a[i].s;
                if (k == "all" || k == "register" || k == "ranking") {
                    const JValue* m = j.Get("message");
                    SetStatusMessage(m && m->IsStr() && !m->s.empty() ? m->s.c_str() : "Online play is switched off for now.");
                    FinishFail(op, Fail::Disabled, 200);
                    return;
                }
            }
        }
        const double ms = j.Num_("serverMs", 0);
        if (ms > 0) { op->serverMs = int64_t(ms); op->serverMsAt = NowMs(); }
        const bool known = op->loginRequest.haveToken();
        if (known) m_token = op->loginRequest.token; else m_token.clear();
        if (!known && NpNet::ServiceBusyBlocksNew()) {   // a new account while the service takes no new sessions today: no request
            SetStatusMessage(NpNet::BusySentence(NpNet::BusyState().level));
            FinishFail(op, Fail::Disabled, 503, 60);
            return;
        }
        if (!StartStep(op, known ? sMe : sRegister)) FinishFail(op, Fail::Transport, 0);
    }

    void OnMe(Op* op, const JValue& j) {
        const double pid = j.Num_("pid", 0);
        if (pid <= 0 || pid > 2147483647.0) { FinishFail(op, Fail::Protocol, 200); return; }
        op->login.profileId = int32_t(pid);
        NpNet::SafeCopy(op->login.code, sizeof op->login.code, j.Str_("code").c_str());
        const std::string registered = j.Str_("name");
        if (registered != op->loginRequest.name) {   // the player typed another name since: the service keeps one name per account
            if (StartStep(op, sRename)) return;
            FinishFail(op, Fail::Transport, 0);
            return;
        }
        OnLoginDone(op);
    }

    void OnRegister(Op* op, const JValue& j) {
        const double pid = j.Num_("pid", 0);
        const std::string token = j.Str_("token");
        if (pid <= 0 || pid > 2147483647.0 || token.size() < 16 || token.size() >= kTokenChars) { FinishFail(op, Fail::Protocol, 200); return; }
        op->login.profileId = int32_t(pid);
        op->login.newToken = true;
        NpNet::SafeCopy(op->login.code, sizeof op->login.code, j.Str_("code").c_str());
        snprintf(op->login.token, sizeof op->login.token, "%s", token.c_str());
        m_token = token;
        OnLoginDone(op);
    }

    void OnLoginDone(Op* op, bool nameRefused = false) {
        m_pid = op->login.profileId;
        if (!nameRefused) op->login.nameCheck = NameCheck::Valid;
        if (op->serverMs > 0) op->login.serverTimeMs = op->serverMs + int64_t(NowMs() - op->serverMsAt);
        FinishOk(op);
    }

    static void CopyText(char* dst, size_t size, const std::string& v) { NpNet::SafeCopy(dst, size, v.c_str()); }   // MSC_NET_TEXT_ASCII
    static bool GoodPid(double pid) { return pid > 0 && pid <= 2147483647.0; }
    static bool GoodRoom(const std::string& c) { if (c.empty() || c.size() > 6) return false; for (size_t i = 0; i < c.size(); ++i) if (!isalnum(static_cast<unsigned char>(c[i]))) return false; return true; }

    void OnFriends(Op* op, const JValue& j) {
        FriendsReply& r = op->friends;
        r.changed = j.Bool_("changed", true);
        CopyText(r.rev, sizeof r.rev, j.Str_("rev"));
        r.heldMs = uint32_t(j.Num_("heldMs", 0));
        const double ps = j.Num_("pollScale", 1);
        r.pollScalePct = (ps > 1.0 && ps <= 1.5) ? uint32_t(ps * 100 + 0.5) : 100;
        if (!r.changed) { FinishOk(op); return; }
        CopyText(r.ownCode, sizeof r.ownCode, j.Str_("code"));
        r.max = uint32_t(j.Num_("max", 0));
        const JValue* rows = j.Get("friends");
        if (rows && rows->IsArr()) {
            for (size_t i = 0; i < rows->a.size() && i < 200; ++i) {
                const JValue& v = rows->a[i];
                if (!v.IsObj() || !GoodPid(v.Num_("pid", 0))) continue;
                FriendRow row;
                row.pid = int32_t(v.Num_("pid", 0));
                CopyText(row.code, sizeof row.code, v.Str_("code"));
                CopyText(row.name, sizeof row.name, v.Str_("name"));
                const std::string st = v.Str_("state");
                row.state = st == "friend" ? FriendState::Friend : st == "incoming" ? FriendState::Incoming : FriendState::Outgoing;
                if (row.state == FriendState::Friend) {
                    const std::string pr = v.Str_("presence");
                    row.presence = pr == "menu" ? Presence::Menu : pr == "queue" ? Presence::Queue : pr == "room" ? Presence::Room : pr == "match" ? Presence::Match : Presence::Offline;
                    row.lastSeenDay = uint32_t(v.Num_("lastSeenDay", 0));
                    CopyText(row.status, sizeof row.status, v.Str_("status"));
                    const std::string rc = v.Str_("roomCode");
                    if (GoodRoom(rc)) CopyText(row.roomCode, sizeof row.roomCode, rc);
                    if (const JValue* inv = v.Get("invite")) { const std::string ic = inv->Str_("roomCode"); if (GoodRoom(ic)) CopyText(row.inviteRoom, sizeof row.inviteRoom, ic); }
                }
                r.rows.push_back(row);
            }
        }
        const JValue* bl = j.Get("blocked");
        if (bl && bl->IsArr()) {
            for (size_t i = 0; i < bl->a.size() && i < 200; ++i) {
                const JValue& v = bl->a[i];
                if (!v.IsObj() || !GoodPid(v.Num_("pid", 0))) continue;
                FriendRow row;
                row.pid = int32_t(v.Num_("pid", 0));
                CopyText(row.code, sizeof row.code, v.Str_("code"));
                CopyText(row.name, sizeof row.name, v.Str_("name"));
                r.blocked.push_back(row);
            }
        }
        FinishOk(op);
    }

    void OnPlayed(Op* op, const JValue& j) {
        const JValue* rows = j.Get("players");
        if (rows && rows->IsArr()) {
            for (size_t i = 0; i < rows->a.size() && i < 40; ++i) {
                const JValue& v = rows->a[i];
                if (!v.IsObj() || !GoodPid(v.Num_("pid", 0))) continue;
                PlayedRow row;
                row.pid = int32_t(v.Num_("pid", 0));
                CopyText(row.code, sizeof row.code, v.Str_("code"));
                CopyText(row.name, sizeof row.name, v.Str_("name"));
                row.wasRanked = v.Bool_("wasRanked", false);
                row.isFriend = v.Bool_("isFriend", false);
                row.day = uint32_t(v.Num_("day", 0));
                op->played.rows.push_back(row);
            }
        }
        FinishOk(op);
    }

    void OnFop(Op* op, const JValue& j) {
        FriendOpReply& r = op->fop;
        r.state = j.Str_("state") == "friend" ? FriendState::Friend : FriendState::Outgoing;
        r.pid = GoodPid(j.Num_("pid", 0)) ? int32_t(j.Num_("pid", 0)) : 0;
        CopyText(r.code, sizeof r.code, j.Str_("code"));
        CopyText(r.name, sizeof r.name, j.Str_("name"));
        FinishOk(op);
    }

    void OnGet(Op* op, const JValue& j) {
        GetReply& reply = op->get;
        reply.mode = op->getMode;
        reply.total = uint32_t(j.Num_("total", 0));
        reply.order = uint32_t(j.Num_("order", 0));
        const JValue* rows = j.Get("rows");
        if (rows && rows->IsArr()) {
            for (size_t i = 0; i < rows->a.size() && i < kMaxRows + 8; ++i) {
                const JValue& r = rows->a[i];
                if (!r.IsObj()) continue;
                Row row;
                const double pid = r.Num_("pid", 0), order = r.Num_("order", 0), score = r.Num_("score", 0);
                if (pid <= 0 || pid > 2147483647.0 || order < 0 || order > 4294967295.0 || score < -2147483648.0 || score > 2147483647.0) continue;
                row.pid = int32_t(pid);
                row.order = uint32_t(order);
                row.score = int32_t(score);
                row.region = uint32_t(r.Num_("region", 0));
                row.lastupdate = uint32_t(r.Num_("lastupdate", 0));
                std::vector<uint8_t> blob;
                if (!Base64Decode(r.Str_("data"), &blob) || blob.size() > kMaxRowData || blob.size() != size_t(r.Num_("size", -1))) continue;
                row.size = uint32_t(blob.size());
                if (!blob.empty()) memcpy(row.data, &blob[0], blob.size());
                reply.rows.push_back(row);
            }
        }
        FinishOk(op);
    }

    HttpTransport* m_t;
    pthread_mutex_t m_lock;
    std::map<uint32_t, Op> m_ops;
    uint32_t m_next;
    uint32_t m_region;
    int32_t m_pid;
    std::string m_token;   // the bearer token of this session (a secret: never logged)
};

}  // namespace

Api* CreateHttpApi(HttpTransport* transport) { return new HttpApi(transport); }

}  // namespace NpOnline
