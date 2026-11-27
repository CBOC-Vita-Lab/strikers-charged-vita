// HttpSignal: see np_signal_http.h.
#include "platform/sdk/net/online/np_signal_http.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <deque>
#include <string>
#include <vector>

#include "platform/sdk/net/online/np_app_version.h"
#include "platform/sdk/net/online/np_text_safe.h"
#include "platform/sdk/net/online/np_http.h"
#include "platform/sdk/net/online/np_json.h"
#include "platform/sdk/net/online/np_service_busy.h"

extern "C" size_t NpMpDiagSessionHeader(char* out, size_t cap) __attribute__((weak));   // np_mpdiag.cpp: the newest session's numbers as the X-Msc-Stats text (absent in the small tests)

namespace NpNet {
namespace {

void CopyStr(char* dst, size_t cap, const std::string& src) { SafeCopy(dst, cap, src.data(), src.size()); }   // MSC_NET_TEXT_ASCII: a name or sentence never carries a byte outside 0x20..0x7E (np_text_safe.h)

// What the player reads for an error code of the service (PROTOCOL.md section 1); the code is never shown.
std::string Sentence(const std::string& code, const std::string& message, int status, const std::string& latest) {
    if (code == "room_not_found" || code == "bad_code") return "That invite code was not found. Check it and try again.";
    if (code == "room_full") return "That room is already full.";
    if (code == "room_expired" || code == "room_closed") return "That room is closed. Ask for a new invite code.";
    if (code == "own_room") return "That is your own room. Give the code to a friend.";
    if (code == "in_ranked_match") return "That player is in a ranked match.";
    if (code == "update_required" || code == "upgrade_required") return latest.empty() ? "Different game version: please update the game." : "Different game version: please update the game to " + latest + ".";
    if (code == "unauthorized") return "Your online account was not accepted. Sign in again.";
    if (code == "banned") return "This online account cannot be used.";
    if (code == "daily_cap") return "Daily limit reached. Try again tomorrow.";
    if (code == "rate_limited") return "Too many tries. Wait a moment and try again.";
    if (code == "service_disabled") return message.empty() ? "Online play is switched off for now." : message;
    if (code == "server_full" || code == "busy") return "The online service is busy. Try again in a moment.";
    if (code == "not_member") return "You are not in that room any more.";
    if (status >= 500) return "The online service had a problem. Try again later.";
    char st[24];
    snprintf(st, sizeof st, "status %d", status);
    return "The online service refused the request (" + (code.empty() ? std::string(st) : code) + ").";
}

std::string ClosedSentence(const std::string& reason, bool expired) {
    if (expired) return "The room timed out.";
    if (reason == "host_left") return "The host left the room.";
    if (reason == "guest_left") return "Your friend left the room.";
    if (reason == "replaced") return "The host opened another room.";
    if (reason == "match_over") return "The match is over.";
    return "The room was closed.";
}

int NatFromName(const std::string& n) {
    if (n == "open") return NAT_OPEN;
    if (n == "cone" || n == "restricted" || n == "full" || n == "port") return NAT_CONE;
    if (n == "symmetric") return NAT_SYMMETRIC;
    return NAT_UNKNOWN;
}
const char* NatName(int nat) {
    switch (nat) {
    case NAT_OPEN: return "open";
    case NAT_CONE: return "cone";
    case NAT_SYMMETRIC: return "symmetric";
    default: return "unknown";
    }
}

class HttpSignal : public Signal {
public:
    explicit HttpSignal(const HttpSignalConfig& c)
        : cfg_(c), status_(IDLE), now_(SteadyNowMs), nowCtx_(0), baseIdx_(0), cur_(0), retryAt_(0), nextPoll_(0), infoDone_(false), started_(false),
          startedSent_(false), haveOffset_(false), offsetMs_(0), nonceGen_(0), haveNonce_(false), failStreak_(0), curAt_(0), host_(false), relayOnly_(false),
          relayOnlySet_(false), turnState_(TURN_NONE), scalePct_(100), curTimeoutMs_(0) {
        turnError_[0] = 0;
        // split the base URL list
        std::string all = cfg_.baseUrl;
        size_t p = 0;
        while (p <= all.size()) {
            size_t e = all.find(',', p);
            if (e == std::string::npos) e = all.size();
            std::string u = all.substr(p, e - p);
            while (!u.empty() && u[u.size() - 1] == '/') u.resize(u.size() - 1);
            if (!u.empty()) bases_.push_back(u);
            p = e + 1;
        }
        error_[0] = 0;
        name_[0] = 0;
        code_[0] = 0;
        memset(nonce_, 0, sizeof nonce_);
    }
    ~HttpSignal() override {
        if (cur_) HttpCancel(cur_);
    }

    void SetClock(uint64_t (*now)(void*), void* ctx) override {
        now_ = now;
        nowCtx_ = ctx;
    }

    void Host(const char* name) override { Begin(name, 0); }
    void Join(const char* invite, const char* name) override { Begin(name, invite ? invite : ""); }

    void PublishCandidates(const SigCandidates& mine) override {
        mine_ = mine;
        if (!haveNonce_ || nonceGen_ != mine.gen) {   // a rebuilt link is a new attempt: the partner sees a new nonce and punches afresh
            if (!SystemRandom(0, nonce_, sizeof nonce_)) {   // a nonce from a dead generator repeats: no punching on it
                Fail(RandomFailureSentence());
                return;
            }
            nonceGen_ = mine.gen;
            haveNonce_ = true;
        }
        room_.members[room_.mySlot < 0 ? 0 : room_.mySlot].cands = mine;
        Job j;
        j.kind = K_ENDPOINT;
        j.method = "POST";
        j.path = "/v1/rooms/" + std::string(code_) + "/endpoint";
        j.body = EndpointBody();
        for (size_t i = 0; i < queue_.size(); ++i)
            if (queue_[i].kind == K_ENDPOINT) { queue_.erase(queue_.begin() + long(i)); break; }   // only the newest matters
        queue_.push_back(j);
    }

    void ReportPunch(const PunchReport& r) override {
        Job j;
        j.kind = K_PUNCH;
        j.method = "POST";
        j.path = "/v1/rooms/" + std::string(code_) + "/punch";
        const char* path = r.pathKind == 0 ? "lan" : (r.pathKind == 2 ? "relay" : "direct");
        char body[160];
        const unsigned ms = unsigned(r.ms > 120000 ? 120000 : r.ms);   // the service's limit: a larger value is a 400, which ends the signaling (a late report must not)
        if (r.ok) snprintf(body, sizeof body, "{\"result\":\"ok\",\"path\":\"%s\",\"ms\":%u}", path, ms);
        else snprintf(body, sizeof body, "{\"result\":\"fail\",\"path\":\"direct\",\"ms\":%u}", ms);
        j.body = body;
        queue_.push_back(j);
    }

    void SetRelayOnly(bool on) override {
        relayOnly_ = on;
        relayOnlySet_ = true;
    }

    void RequestTurn() override {
        if (status_ != IN_ROOM) return;
        turnState_ = TURN_WORKING;
        turnError_[0] = 0;
        for (size_t i = 0; i < queue_.size(); ++i)
            if (queue_[i].kind == K_TURN) return;
        Job j;
        j.kind = K_TURN;
        j.method = "POST";
        j.path = "/v1/turn";
        j.body = "{\"room\":";
        JsonEscapeAppend(&j.body, code_);
        j.body += "}";
        queue_.push_back(j);
    }
    TurnStatus TurnState() const override { return turnState_; }
    const TurnInfo& Turn() const override { return turn_; }
    const char* TurnError() const override { return turnError_; }
    void ReportTurnUsage(uint64_t bytes) override {
        if (bytes == 0 || bases_.empty()) return;
        char body[64];
        snprintf(body, sizeof body, "{\"bytes\":%llu}", (unsigned long long)bytes);
        HttpFire("POST", bases_[baseIdx_] + "/v1/turn/usage", body, Headers(), 5000);
    }

    void Started() override {
        if (startedSent_ || status_ != IN_ROOM) return;
        startedSent_ = true;
        Job j;
        j.kind = K_STARTED;
        j.method = "POST";
        j.path = "/v1/rooms/" + std::string(code_) + "/started";
        queue_.push_back(j);
    }

    void Leave() override {
        if (cur_) { HttpCancel(cur_); cur_ = 0; }
        queue_.clear();
        if (code_[0] && (status_ == IN_ROOM || status_ == WORKING) && !bases_.empty()) {
            // fire and forget: the room closes for a host, a guest leaves it (it reopens)
            // The session's anonymous numbers ride on the leave request as one header (np_mpdiag.h SessionHeader: duration, input delay, rebuilds, loss, starved steps, error flag;
            // no name, id or address). A header, not a body: a service that does not know it ignores it, so this client never breaks against an older service.
            std::vector<std::string> h = Headers();
            char stats[96];
            if (NpMpDiagSessionHeader && NpMpDiagSessionHeader(stats, sizeof stats) > 0) h.push_back(std::string("X-Msc-Stats: ") + stats);
            HttpFire("POST", bases_[baseIdx_] + "/v1/rooms/" + code_ + "/leave", "{}", h, 5000);
        }
        if (status_ != FAILED) status_ = CLOSED;
    }

    void Poll() override {
        HttpPump();
        if (status_ != WORKING && status_ != IN_ROOM) return;
        const uint64_t now = now_(nowCtx_);
        if (cur_) {
            HttpResponse r;
            const int rc = HttpPoll(cur_, &r);
            if (rc == 1) {
                cur_ = 0;
                OnResponse(r, now);
            } else if (rc < 0) {
                // the request is gone (the table lost it): that is a failed attempt like any other, not a silent hole in the queue
                cur_ = 0;
                HttpResponse t;
                t.error = 1;
                t.text = "The online service request was lost.";
                OnResponse(t, now);
            } else if (SinceMs(now, curAt_) > curTimeoutMs_ + 3000) {
                HttpCancel(cur_);
                cur_ = 0;
                HttpResponse t;
                t.error = 1;
                t.text = "The online service did not answer in time.";
                OnResponse(t, now);
            }
            return;
        }
        if (now < retryAt_) return;
        if (!queue_.empty()) {
            Job j = queue_.front();
            queue_.pop_front();
            StartJob(j, now);
            return;
        }
        if (status_ == IN_ROOM && now >= nextPoll_) {
            Job j;
            j.kind = K_POLL;
            j.method = "GET";
            char v[64];
            snprintf(v, sizeof v, "?v=%u", unsigned(room_.version));
            j.path = "/v1/rooms/" + std::string(code_) + (room_.version ? v : "");
            if (room_.version && cfg_.holdSec) {
                // held poll: the service answers when the room changes or after `wait` seconds (an old service ignores the parameter and answers at once)
                uint32_t sec = (startedSent_ && cfg_.holdStartedSec ? cfg_.holdStartedSec : cfg_.holdSec) * uint32_t(scalePct_) / 100;
                if (hintWaitSec_) sec = hintWaitSec_;   // gxnet14: the service's own number (MSC_NET_PACE_HINTS=1; it already holds the service's pollScale)
                if (sec < 1) sec = 1;
                if (sec > 55) sec = 55;
                snprintf(v, sizeof v, "&wait=%u", unsigned(sec));
                j.path += v;
                j.extraTimeoutMs = sec * 1000;
            }
            StartJob(j, now);
        }
    }

    Status GetStatus() const override { return status_; }
    const SigRoom& Room() const override { return room_; }
    const char* Error() const override { return error_; }
    const char* StunList() const override { return stunList_.c_str(); }

private:
    enum Kind { K_INFO, K_ME, K_ME_PUT, K_CREATE, K_JOIN, K_POLL, K_ENDPOINT, K_PUNCH, K_STARTED, K_TURN };
    struct Job {
        Kind kind;
        std::string method, path, body;
        int tries;
        uint32_t extraTimeoutMs;   // a held poll may take that long longer
        Job() : kind(K_POLL), tries(0), extraTimeoutMs(0) {}
    };

    void Begin(const char* name, const char* invite) {
        host_ = invite == 0;
        CopyStr(name_, sizeof name_, name ? name : "");
        error_[0] = 0;
        queue_.clear();
        if (bases_.empty() || !cfg_.token[0]) {
            Fail(bases_.empty() ? "The online service address is not set." : "You are not signed in to the online service.");
            return;
        }
        if (!(invite && invite[0] == '@') && NpNet::ServiceBusyBlocksNew()) {
            // the service said a moment ago that it takes no new sessions today: no request, the sentence at once (a ranked pairing's own room, "@CODE", is not new)
            Fail(BusySentence(BusyState().level));
            return;
        }
        status_ = WORKING;
        Job info;
        info.kind = K_INFO;
        info.method = "GET";
        info.path = "/v1/info";
        queue_.push_back(info);
        Job j;
        j.kind = host_ ? K_CREATE : K_JOIN;
        j.method = "POST";
        if (host_) {
            j.path = "/v1/rooms";
        } else if (invite[0] == '@') {
            // "@CODE": a room the service already made with this player in it (a ranked pairing, PROTOCOL.md section 5: "continue exactly
            // like an invite room with room.code"). It cannot be joined by code; reading it is the entry (and the join acknowledgement).
            std::string code;
            for (const char* p = invite + 1; *p; ++p)
                if (isalnum(static_cast<unsigned char>(*p))) code.push_back(char(toupper(static_cast<unsigned char>(*p))));
            j.method = "GET";
            j.path = "/v1/rooms/" + code;
        } else {
            j.path = "/v1/rooms/join";
            std::string code;
            for (const char* p = invite; *p; ++p)
                if (*p != ' ' && *p != '-') code.push_back(char(toupper(static_cast<unsigned char>(*p))));
            j.body = "{\"code\":";
            JsonEscapeAppend(&j.body, code.c_str());
            j.body += "}";
        }
        pending_ = j;
    }

    std::vector<std::string> Headers() const {
        std::vector<std::string> h;
        h.push_back(std::string("Authorization: Bearer ") + cfg_.token);
        h.push_back("X-Msc-Proto: 1");
        h.push_back(std::string("X-Msc-App: ") + AppVersionOrDefault(cfg_.appVersion));   // always: the production service refuses a request without it
        if (cfg_.build[0]) h.push_back(std::string("X-Msc-Build: ") + cfg_.build);
        return h;
    }

    std::string EndpointBody() const {
        std::string b = "{";
        if (!mine_.relayOnly) {
            // privacy: a relay-only account sends nothing but its relayed address and nonce (the service refuses anything else)
            const Addr pub = mine_.pub.IsZero() ? mine_.local : mine_.pub;   // "public" is required: without a STUN answer the LAN address is all there is
            b += "\"public\":\"";
            b += AddrStr(pub);
            b += "\",\"local\":\"";
            b += AddrStr(mine_.local);
            b += "\",";
        }
        if (!mine_.relay.IsZero()) {
            b += "\"relay\":\"";
            b += AddrStr(mine_.relay);
            b += "\",";
        }
        b += "\"nonce\":\"";
        b += Base64Encode(nonce_, sizeof nonce_);
        b += "\"";
        if (!mine_.relayOnly) {
            b += ",\"nat\":\"";
            b += NatName(mine_.nat);
            b += "\",\"candidates\":[";
            b += "{\"type\":\"host\",\"addr\":\"";
            b += AddrStr(mine_.local);
            b += "\"}";
            if (!mine_.pub.IsZero()) {
                b += ",{\"type\":\"srflx\",\"addr\":\"";
                b += AddrStr(mine_.pub);
                b += "\"}";
            }
            if (!mine_.pub2.IsZero() && mine_.pub2 != mine_.pub) {
                b += ",{\"type\":\"srflx\",\"addr\":\"";
                b += AddrStr(mine_.pub2);
                b += "\"}";
            }
            b += "]";
        }
        b += "}";
        return b;
    }

    void StartJob(Job& j, uint64_t now) {
        std::string err;
        const std::string url = bases_[baseIdx_] + j.path;
        curTimeoutMs_ = cfg_.requestTimeoutMs + j.extraTimeoutMs;
        cur_ = HttpStart(j.method.c_str(), url, j.body, Headers(), curTimeoutMs_, j.extraTimeoutMs ? HeldIoMs(j.extraTimeoutMs) : 0, &err);
        curJob_ = j;
        curAt_ = now;
        if (!cur_) {
            HttpResponse t;
            t.error = 1;
            t.text = err;
            OnResponse(t, now);
        }
    }

    void Retry(uint64_t now, uint32_t minWaitMs = 0) {
        static const uint32_t backoff[] = {1000, 2000, 4000, 8000, 15000};
        const int n = curJob_.tries < 4 ? curJob_.tries : 4;
        uint32_t wait = backoff[n];
        if (wait < minWaitMs) wait = minWaitMs;
        curJob_.tries++;
        queue_.push_front(curJob_);
        retryAt_ = now + wait;
    }

    void Fail(const std::string& why) {
        CopyStr(error_, sizeof error_, why);
        status_ = FAILED;
        queue_.clear();
        NPN_LOG("signaling failed: %s\n", error_);
    }

    void OnResponse(const HttpResponse& r, uint64_t now) {
        const Job& j = curJob_;
        const bool decisive = j.kind == K_INFO || j.kind == K_ME || j.kind == K_ME_PUT || j.kind == K_CREATE || j.kind == K_JOIN;
        std::string code, message, latest, reason, level;
        Json err;
        int retryAfter = 0;
        if (!r.error && (r.status < 200 || r.status >= 300) && err.Parse(r.body.data(), r.body.size())) {
            const int e = err.Get(err.Root(), "error");
            code = err.GetStr(e, "code");
            message = err.GetStr(e, "message");
            latest = err.GetStr(e, "latestVersion");
            reason = err.GetStr(e, "reason");
            level = err.GetStr(e, "level");
            retryAfter = NpNet::ClampRetryAfterSec(err.GetNum(e, "retryAfterSec", 0));
            if (code == "rate_limited" && err.GetStr(e, "reason") == "daily_cap") code = "daily_cap";   // a limit of the day: waiting a moment does not help
        }
        if (j.kind == K_TURN) {
            OnTurnResponse(r, now, code, message, retryAfter);
            return;
        }
        const bool edgeBusy = NpNet::IsEdgeBusyPage(r.error, r.status, !code.empty());   // gxnet14: Cloudflare's own 429 page (quota spent): the same as over_budget
        if (edgeBusy) { retryAfter = NpNet::kEdgeBusyQuietSec; level.clear(); }
        if ((r.status == 503 && code == "over_budget") || edgeBusy) {
            // the service is over its daily budget (PROTOCOL.md "Budget and pacing"): remember it, never hammer. A decisive call fails at once with a
            // plain sentence; a poll waits the service's Retry-After (5 to 60 s); the rest asks again after at most 30 s.
            NpNet::NoteServiceBusy(level.c_str(), retryAfter);
            if (decisive) { Fail(BusySentence(BusyLevelOf(level.c_str()))); return; }
            if (j.kind == K_POLL) { nextPoll_ = now + PollWaitAfterBusyMs(retryAfter); return; }
            if (j.tries >= 5) return;
            Retry(now, uint32_t(retryAfter > 30 ? 30 : retryAfter) * 1000);
            return;
        }
        if (r.status == 429 && reason == "daily_cap") {
            // a per-account daily limit (rooms, matches, ...): retryAfterSec runs to midnight UTC, waiting it out is pointless. Terminal, with a sentence.
            Fail("Daily limit reached. Try again tomorrow.");
            return;
        }
        // (reason "too_early" on a result is an ordinary 429: waited out by the retry below / the result queue)
        const bool transient = r.error || r.status >= 500 || r.status == 429;
        if (transient) {
            ++failStreak_;
            if (decisive && j.tries + 1 >= cfg_.maxAttempts) {
                if (r.error) {
                    if (bases_.size() > 1 && j.kind == K_INFO) {   // another server of the list
                        baseIdx_ = (baseIdx_ + 1) % bases_.size();
                    }
                    Fail(r.text.empty() ? "The online service could not be reached." : r.text);
                } else {
                    Fail(Sentence(code, message, r.status, latest));
                }
                return;
            }
            if (!decisive && j.tries >= 6 && j.kind != K_POLL) return;   // a lost report is not worth more
            if (j.kind == K_POLL) {   // the poll comes round again by itself, slower
                static const bool legacyPollWait = [] { const char* e = getenv("MSC_NET_POLL_BACKOFF"); return e && e[0] == '0'; }();   // 0 = the old 15 s ceiling
                uint32_t wait = NpNet::PollFailWaitMs(failStreak_, now, legacyPollWait);
                if (r.status == 429 && retryAfter > 0) wait = NpNet::RetryAfterMs(retryAfter);
                nextPoll_ = now + wait;
                return;
            }
            if (r.error && bases_.size() > 1 && j.kind == K_INFO) baseIdx_ = (baseIdx_ + 1) % bases_.size();
            Retry(now, r.status == 429 ? NpNet::RetryAfterMs(retryAfter) : 0u);
            return;
        }
        failStreak_ = 0;
        if (r.status < 200 || r.status >= 300) {
            // a refusal. Fatal for the requests that decide the room; a closed room for the others
            if (decisive || code == "unauthorized" || code == "update_required" || code == "upgrade_required" || code == "banned" || code == "service_disabled") {
                Fail(Sentence(code, message, r.status, latest));
                return;
            }
            if (code == "room_closed" || code == "room_expired" || code == "not_member" || r.status == 404 || r.status == 410) {
                status_ = CLOSED;
                CopyStr(error_, sizeof error_, ClosedSentence(code == "room_expired" ? "" : "", code == "room_expired"));
                return;
            }
            if (j.kind == K_ENDPOINT || j.kind == K_PUNCH || j.kind == K_STARTED) {
                // 409 room_not_ready: the partner's side is not there yet; asking again is right. Any other refusal of these will not change by
                // asking again, and a dropped endpoint would leave the host waiting for a partner who can never see it: the room fails, with a sentence.
                if (r.status == 409 && j.kind != K_ENDPOINT && j.tries < 5) {
                    Retry(now);
                    return;
                }
                Fail(Sentence(code, message, r.status, latest));
                return;
            }
            NPN_LOG("signaling: %s refused (%d %s)\n", j.path.c_str(), r.status, code.c_str());
            return;
        }
        Json doc;
        if (!doc.Parse(r.body.data(), r.body.size())) {
            NPN_LOG("signaling: unreadable answer to %s\n", j.path.c_str());
            if (decisive) Fail("The online service sent an answer the game does not understand.");
            return;
        }
        const int root = doc.Root();
        switch (j.kind) {
        case K_INFO: {
            const int kill = doc.Get(root, "kill");
            for (int i = 0; i < doc.Size(kill); ++i) {
                const std::string& k = doc.Str(doc.At(kill, i));
                if (k == "all" || k == "rooms") {
                    const std::string& m = doc.GetStr(root, "message");
                    Fail(m.empty() ? "Online play is switched off for now." : m);
                    return;
                }
            }
            const double serverMs = doc.GetNum(root, "serverMs", 0);
            if (serverMs > 0) {
                // the server clock against ours, with half the round trip taken off (the answer is that old)
                offsetMs_ = int64_t(serverMs) - int64_t(now - (now - curAt_) / 2);
                haveOffset_ = true;
            }
            const int stun = doc.Get(root, "stun");
            stunList_.clear();
            for (int i = 0; i < doc.Size(stun) && i < 4; ++i) {
                if (i) stunList_ += ",";
                stunList_ += doc.Str(doc.At(stun, i));
            }
            infoDone_ = true;
            if (relayOnlySet_) {
                // the service filters addresses by the profile's relay-only flag, so the profile must say what this console does
                Job me;
                me.kind = K_ME;
                me.method = "GET";
                me.path = "/v1/me";
                queue_.push_back(me);
            } else {
                ReleasePending();
            }
            break;
        }
        case K_ME: {
            const bool has = doc.GetBool(root, "relayOnly", false);
            if (has != relayOnly_) {
                Job put;
                put.kind = K_ME_PUT;
                put.method = "PUT";
                put.path = "/v1/me";
                put.body = std::string("{\"relayOnly\":") + (relayOnly_ ? "true" : "false") + "}";
                queue_.push_back(put);
            } else {
                ReleasePending();
            }
            break;
        }
        case K_ME_PUT:
            ReleasePending();
            break;
        case K_TURN:
            break;
        case K_CREATE:
        case K_JOIN:
        case K_POLL:
        case K_ENDPOINT:
        case K_PUNCH:
        case K_STARTED:
        {
            // the service's pacing (PROTOCOL.md "Held polls"): `pollScale` stretches holds and gaps (only above 1), `heldMs` says it held this answer
            const double ps = doc.GetNum(root, "pollScale", 1);
            scalePct_ = (ps > 1.0 && ps <= 1.5) ? int(ps * 100 + 0.5) : 100;
            const bool held = doc.Has(doc.Get(root, "heldMs"));
            const uint32_t heldMs = held ? uint32_t(doc.GetNum(root, "heldMs", 0)) : 0;
            if (NpNet::PaceHintsOn()) {   // gxnet14: the service dictates the rhythm (nextWaitSec / nextPollMs, kept until the next answer says otherwise)
                hintWaitSec_ = NpNet::PaceWaitSec(doc.GetNum(root, "nextWaitSec", 0));
                hintGapMs_ = NpNet::PaceGapMs(doc.GetNum(root, "nextPollMs", 0));
            }
            ApplyRoom(doc, root, now);
            if (j.kind == K_ENDPOINT) nextPoll_ = now + 300;   // the partner's endpoint may be there already
            if (j.kind == K_POLL) {
                const uint32_t base = startedSent_ ? cfg_.pollStartedMs : cfg_.pollMs;
                uint32_t gap = base * uint32_t(scalePct_) / 100;
                if (!held && hintGapMs_) gap = hintGapMs_;   // gxnet14: the pause after an answer that was not held
                if (held && cfg_.holdSec) {   // the service held it: ask again soon (never faster than once a second in all, whatever the service says)
                    gap = cfg_.heldGapMs;
                    if (heldMs < 1000 && gap < 1000 - heldMs) gap = 1000 - heldMs;
                }
                nextPoll_ = now + gap;
            }
        }
            break;
        }
    }

    void ReleasePending() {
        if (pending_.kind == K_CREATE || pending_.kind == K_JOIN) {
            queue_.push_back(pending_);
            pending_ = Job();
        }
    }

    void OnTurnResponse(const HttpResponse& r, uint64_t now, const std::string& code, const std::string& message, int retryAfter) {
        const Job& j = curJob_;
        auto failTurn = [&](const std::string& sentence) {
            turnState_ = TURN_FAILED;
            CopyStr(turnError_, sizeof turnError_, sentence);
            NPN_LOG("relay credentials refused: %s\n", turnError_);
        };
        const bool again = r.error || r.status >= 500 || r.status == 429;
        const bool hard = code == "over_budget" || code == "turn_budget" || code == "turn_unavailable" || code == "service_disabled" || code == "turn_player_limit" || code == "daily_cap";
        if (!r.error && r.status >= 200 && r.status < 300) {
            Json doc;
            if (!doc.Parse(r.body.data(), r.body.size())) { failTurn("The relay service sent an answer the game does not understand."); return; }
            const int root = doc.Root();
            TurnInfo t;
            t.ttlSec = uint32_t(doc.GetNum(root, "ttlSec", 3600));
            const int servers = doc.Get(root, "servers");
            for (int i = 0; i < doc.Size(servers); ++i) {
                const int sv = doc.At(servers, i);
                const std::string& user = doc.GetStr(sv, "username");
                if (user.empty()) continue;   // the plain STUN entry
                CopyStr(t.user, sizeof t.user, user);
                CopyStr(t.pass, sizeof t.pass, doc.GetStr(sv, "credential"));
                const int urls = doc.Get(sv, "urls");
                for (int u = 0; u < doc.Size(urls) && t.nUrls < 4; ++u) {
                    const std::string& url = doc.Str(doc.At(urls, u));
                    if (url.compare(0, 5, "turn:") != 0 || url.find("transport=tcp") != std::string::npos) continue;   // UDP only: the Vita has no TCP
                    CopyStr(t.urls[t.nUrls++], sizeof t.urls[0], url);
                }
                break;
            }
            if (t.nUrls == 0 || !t.user[0]) { failTurn(TurnSentence("turn_unavailable", "")); return; }
            turn_ = t;
            turnState_ = TURN_READY;
            NPN_LOG("relay credentials received (%d server address(es), valid %u s)\n", t.nUrls, unsigned(t.ttlSec));
            return;
        }
        if (again && !hard) {
            if (j.tries >= 2) {
                failTurn(r.error ? std::string("The relay service could not be reached.") : TurnSentence(code, message));
                return;
            }
            Retry(now, r.status == 429 ? NpNet::RetryAfterMs(retryAfter) : 0u);
            return;
        }
        failTurn(TurnSentence(code, message));
    }

    static bool ParseIp(const std::string& s, uint32_t* ip) {
        Addr a;
        if (!ParseAddr(s.c_str(), 0, &a)) return false;
        *ip = a.ip;
        return true;
    }

    void ApplyRoom(const Json& d, int v, uint64_t now) {
        if (!d.Has(d.Get(v, "state"))) return;   // not a room view
        if (d.Has(d.Get(v, "changed")) && !d.GetBool(v, "changed", true)) {
            const std::string& st = d.GetStr(v, "state");
            if (st == "closed" || st == "expired") CloseWith(st == "expired", "");
            return;
        }
        const std::string& state = d.GetStr(v, "state");
        const int slot = int(d.GetNum(v, "slot", 0));
        if (slot < 0 || slot >= 2) return;
        SigRoom r;
        CopyStr(r.invite, sizeof r.invite, d.GetStr(v, "code"));
        CopyStr(code_, sizeof code_, d.GetStr(v, "code"));
        r.mySlot = slot;
        r.version = uint32_t(d.GetNum(v, "version", 0));
        const int you = d.Get(v, "you");
        SigMember& me = r.members[slot];
        me.present = true;
        me.slot = uint8_t(slot);
        CopyStr(me.name, sizeof me.name, d.GetStr(you, "name"));
        ParseIp(d.GetStr(you, "vip"), &me.vip);
        me.cands = mine_;
        const int peer = d.Get(v, "peer");
        if (d.Has(peer)) {
            const int ps = int(d.GetNum(peer, "slot", 1 - slot));
            if (ps >= 0 && ps < 2 && ps != slot) {
                SigMember& pm = r.members[ps];
                pm.present = true;
                pm.slot = uint8_t(ps);
                CopyStr(pm.name, sizeof pm.name, d.GetStr(peer, "name"));
                ParseIp(d.GetStr(peer, "vip"), &pm.vip);
                const std::string& link = d.GetStr(peer, "link");
                pm.link = link == "lost" ? 2 : (link == "quiet" ? 1 : 0);
                const int ep = d.Get(peer, "endpoint");
                if (d.Has(ep)) {
                    ParseAddr(d.GetStr(ep, "public").c_str(), 0, &pm.cands.pub);
                    ParseAddr(d.GetStr(ep, "local").c_str(), 0, &pm.cands.local);
                    pm.cands.nat = uint8_t(NatFromName(d.GetStr(ep, "nat")));
                    pm.stale = d.GetBool(ep, "stale", false);
                    uint8_t nonce[32];
                    const int n = Base64Decode(d.GetStr(ep, "nonce"), nonce, sizeof nonce);
                    uint32_t gen = 1;
                    for (int i = 0; i < n && i < 4; ++i) gen = gen * 257u + nonce[i];   // a republish with a new nonce is a new generation
                    pm.cands.gen = gen ? gen : 1;
                    ParseAddr(d.GetStr(ep, "relay").c_str(), 0, &pm.cands.relay);
                    pm.cands.valid = !pm.cands.pub.IsZero() || !pm.cands.local.IsZero() || !pm.cands.relay.IsZero();
                }
            }
        }
        const int sess = d.Get(v, "session");
        if (!d.Has(sess)) {
            memcpy(r.key, room_.key, 32);   // a view without a session (the room ended, an older answer) does not take the match key away
            r.cookie = room_.cookie;
        } else {
            uint8_t key[64];
            const int n = Base64Decode(d.GetStr(sess, "key"), key, sizeof key);
            bool nonzero = false;
            for (int i = 0; i < n && i < 32; ++i)
                if (key[i]) nonzero = true;
            if (n != 32 || !nonzero) {   // never an empty / short key: it would be a key everybody knows
                Fail("The online service sent an unusable key for this match. Try again.");
                return;
            }
            memcpy(r.key, key, 32);
            r.cookie = uint32_t(d.GetNum(sess, "cookie", 0));
            const int steer = d.Get(sess, "steer");
            r.steerRelay = d.GetBool(steer, "relay", false);
            CopyStr(r.steerReason, sizeof r.steerReason, d.GetStr(steer, "reason"));
            const double punchAt = d.GetNum(sess, "punchAtMs", 0);
            if (punchAt > 0 && haveOffset_) {
                const int64_t local = int64_t(punchAt) - offsetMs_;
                // a time far ahead (a clock far off) is not waited for
                r.punchAtMs = (local > int64_t(now) && local < int64_t(now) + 6000) ? uint64_t(local) : 0;
            }
        }
        room_ = r;
        status_ = IN_ROOM;
        const std::string& reason = d.GetStr(v, "closedReason");
        if (state == "closed" || state == "expired") CloseWith(state == "expired", reason);
    }

    void CloseWith(bool expired, const std::string& reason) {
        if (status_ == CLOSED || status_ == FAILED) return;
        CopyStr(error_, sizeof error_, ClosedSentence(reason, expired));
        NPN_LOG("room closed: %s\n", error_);
        status_ = CLOSED;
    }

    HttpSignalConfig cfg_;
    Status status_;
    uint64_t (*now_)(void*);
    void* nowCtx_;
    std::vector<std::string> bases_;
    size_t baseIdx_;
    std::deque<Job> queue_;
    Job pending_, curJob_;
    int cur_;
    uint64_t retryAt_, nextPoll_;
    bool infoDone_, started_, startedSent_, haveOffset_;
    int64_t offsetMs_;
    uint8_t nonce_[8];
    uint32_t nonceGen_;
    bool haveNonce_;
    int failStreak_;
    uint64_t curAt_;
    bool host_;
    SigCandidates mine_;
    SigRoom room_;
    std::string stunList_;
    bool relayOnly_, relayOnlySet_;
    TurnStatus turnState_;
    int scalePct_;                // the service's pollScale in percent (100 = none): holds and gaps are stretched by it
    uint32_t hintWaitSec_ = 0, hintGapMs_ = 0;   // gxnet14: the service's pacing hints (MSC_NET_PACE_HINTS=1), 0 = none
    uint32_t curTimeoutMs_;
    TurnInfo turn_;
    char turnError_[200];
    char error_[96], name_[16], code_[16];
};

}  // namespace

std::string TurnSentence(const std::string& code, const std::string& message) {
    if (code == "over_budget") return "The online service is very busy today. Relay play is not available right now; try again later, or play from a network that allows direct connections.";
    if (code == "turn_budget") return "Relay play is unavailable right now: the relay allowance of the online service is used up. Try again later, or play from a network that allows direct connections.";
    if (code == "turn_player_limit") return "You have used all of today's relay connections. Try again tomorrow, or play from a network that allows direct connections.";
    if (code == "daily_cap") return "Daily limit reached. Relay play is available again tomorrow, or play from a network that allows direct connections.";
    if (code == "turn_unavailable") return "Relay play is not available on this online service.";
    if (code == "service_disabled") return message.empty() ? "Online play is switched off for now." : message;
    if (code == "turn_upstream") return "The relay service did not answer. Try again in a moment.";
    if (code == "turn_not_allowed" || code == "not_member") return "The relay can only be used inside a room with both players.";
    if (code == "unauthorized") return "Your online account was not accepted. Sign in again.";
    return "The relay could not be set up (" + (code.empty() ? std::string("no answer") : code) + ").";
}

Signal* CreateHttpSignal(const HttpSignalConfig& config) { return new HttpSignal(config); }

}  // namespace NpNet
