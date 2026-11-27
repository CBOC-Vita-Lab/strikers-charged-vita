// The game's ranked matchmaking (DWC_ConnectToAnybodyAsync and the DWC transport) on top of the online service's ranked queue and the
// port's own online link (1.3). The game's flow is unchanged: Online Central -> RANKED MATCH -> SHOnlinePlayerCount -> NetworkLobby::
// StartMatchmaking -> DWC_ConnectToAnybodyAsync; the matched callback; the reliable layer over DWC_SendUnreliable; the sorted draft; the match;
// NetworkStatsManager's result and score puts. Here:
//
//   queue    POST /v1/ranked/queue, then GET every 2 s (PROTOCOL.md section 5). The service pairs two players and makes a ranked room with both
//            in it; `matched` carries the room view (code, slot, match id). 409 in_ranked_match (a room left over from a crash) is left first.
//   link     the room is entered with the same signaling, hole punching and relay as an invite room (NpNetJoin("@CODE"), np_online.cpp); the
//            matched callback runs once the link is confirmed both ways (the service then starts the match record).
//   aids     the room slot is the AID (slot 0 = the player who waited longer = the session's host); DWC_SendUnreliable(aid) is a frame to that slot.
//   result   NpRankedMatchResult (NetworkStatsManager::CalculateAndReportGameResult, ranked only) puts the result report into the persistent
//            retry queue (np_online_queue.cpp): score of each slot and the end-of-match digest that NpRankedEndOfMatch (cGame::ChangeGameState to
//            GS_END_GAME, the same simulation step on both consoles) recorded. The service rates the match only when both reports agree, and
//            the ranking client holds the game's score puts until the match is final (QueueMayPut).
//
// Bounded: the queue at most 6 minutes (the service drops a ticket after 5), the link 90 s, every request 15 s. Cancel (DWC_CancelMatching)
// leaves the queue. Every request carries X-Msc-Proto / X-Msc-App / X-Msc-Build; 426 is "please update the game".
// MSC_ONLINE_RANKED=0 turns ranked matchmaking off (DWC_ConnectToAnybodyAsync answers FALSE; the menu hides the entry).
#include <dwc/dwc_main_fwd.h>
#include <dwc/dwc_match_fwd.h>
#include <dwc/dwc_transport_fwd.h>
#include <dwc/dwci_error.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "platform/sdk/dwc/np_dwc.h"
#include "np/host_alloc.h"
#include "platform/sdk/dwc/np_online_name.h"
#include "platform/sdk/dwc/np_online_queue.h"
#include "platform/sdk/net/online/np_http.h"
#include "platform/sdk/net/online/np_json.h"
#include "platform/sdk/net/online/np_online.h"
#include "platform/sdk/net/online/np_service_busy.h"
#include "platform/sdk/net/online/np_service_url.h"

#define MATCH_LOG(...) fprintf(stderr, "[ranked] " __VA_ARGS__)

extern "C" void NpNetConfigureService(const char* baseUrl, const char* token, const char* appVersion, const char* buildHash);
// (mpbeta27) the session trail of a problem report (np_mpdiag.h; weak: the small tests link without it). Stage numbers: 3 queue, 4 match found, 6 connect.
extern "C" void NpMpDiagReach(int stage) __attribute__((weak));
extern "C" void NpMpDiagError(int stage, int code, const char* word) __attribute__((weak));
extern "C" int NpNetJoin(const char* inviteCode, const char* playerName);
extern "C" void NpNetPoll(void);
extern "C" void NpNetLeave(void);
extern "C" int NpNetPhase(void);
extern "C" const char* NpNetError(void);
extern "C" void NpLanOnlineSessionTuning(int on) __attribute__((weak));   // np_lan.cpp: the LAN session's keep-alive / input transport settings

namespace {

using NpOnline::NowMs;

const uint64_t kPollMs = 2000, kQueueLimitMs = 6 * 60 * 1000, kLinkLimitMs = 90000, kRequestMs = 15000;
// held queue polls (PROTOCOL.md "Held polls"): GET /v1/ranked/queue?wait=<kHoldSec> is answered when the pairing happens or after that long
const uint32_t kHoldSec = 12, kHeldGapMs = 250;

enum class Phase { Idle, Joining, Queued, Linking, Connected, Closed };

struct Match {
    Phase phase;
    DWCMatchedCallback matched;
    void* matchedParam;
    DWCConnectionClosedCallback closed;
    void* closedParam;
    DWCUserRecvCallback recv;
    uint8_t* recvBuf[4];
    int recvSize[4];
    int http;                 // the request in flight (NpNet::HttpStart handle)
    int httpKind;             // 1 join, 2 poll, 3 leave a stale room
    uint64_t httpAt, nextAt, startedAt, linkAt;
    uint32_t reqTimeoutMs;    // the request in flight may take that long (a held poll: its hold on top)
    int scalePct;             // the service's pollScale in percent (100 = none)
    uint32_t hintWaitSec;     // gxnet14: nextWaitSec from the last answer (MSC_NET_PACE_HINTS=1), 0 = none
    bool cancel, retriedStale, linkStarted, closedSent;
    // (1.3: on the host heap: set on the game thread, kept for the whole matchmaking and match; np/host_alloc.h)
    np::HostString ticketId, ticketToken, room, matchId;
    int slot;
    bool quick;               // this attempt is the unranked quick queue
    int failStreak;           // requests in a row that failed without an answer (the gap before the next one grows: FailGapMs)
    Match() { Reset(); recv = 0; closed = 0; closedParam = 0; memset(recvBuf, 0, sizeof recvBuf); memset(recvSize, 0, sizeof recvSize); }
    void Reset() {
        phase = Phase::Idle; matched = 0; matchedParam = 0; http = 0; httpKind = 0; httpAt = nextAt = startedAt = linkAt = 0; reqTimeoutMs = uint32_t(kRequestMs); scalePct = 100; hintWaitSec = 0;
        cancel = retriedStale = linkStarted = closedSent = false; ticketId.clear(); ticketToken.clear(); room.clear(); matchId.clear(); slot = -1; quick = false; failStreak = 0;
    }
};

pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
Match g_m;
bool g_pumpSet = false;
bool g_quickRequested = false;   // NpDwcSetQuickMatch: the next matchmaking is the unranked quick queue

// the end-of-match digest (NpRankedEndOfMatch) and the match it belongs to; one report per match id
struct EndDigest { bool seen; uint32_t frame, checksum, seed; } g_end = {false, 0, 0, 0};
np::HostString g_reportedMatch;   // (1.3: on the host heap, kept for the session; np/host_alloc.h)
// (std::string <-> np::HostString: different allocators, so explicit copies)
// (MSC_HEAP_GUARD=1: each string is registered at its first write and checked at every stadium destroy: none may lie in a game heap)
inline void PutStr(np::HostString& d, const std::string& s) {
    d.assign(s.data(), s.size());
    np::HeapGuard("ranked match string (np_dwc_match.cpp)", d);
}
inline std::string StdStr(const np::HostString& s) { return std::string(s.data(), s.size()); }

bool RankedEnabled() {
    const char* e = getenv("MSC_ONLINE_RANKED");
    return !(e && e[0] == '0');
}

// The service address: the same one the sign-in used (np_service_url.h NpOnlineCentralBase).
std::string ServiceBase() { return NpOnlineCentralBase(); }

std::vector<std::string> Headers(const char* token) {
    const NpOnline::ClientInfo& info = NpOnline::GetClientInfo();
    std::vector<std::string> h;
    h.push_back(std::string("Authorization: Bearer ") + token);
    h.push_back("X-Msc-Proto: 1");
    h.push_back(std::string("X-Msc-App: ") + info.appVersion);
    if (info.build[0]) h.push_back(std::string("X-Msc-Build: ") + info.build);
    return h;   // (no Content-Type line: both HTTP clients send application/json with a body; np_https.h)
}

// The wait before asking again after requests that got no answer (a dead network, a service that is down): 2, 4, 8, then 10 s, so a long outage does not
// open a connection every 2 s for the whole 6 minutes (the console's socket table is small and shared; np_https_sys.h). An answer resets it.
const int kMaxNoAnswer = 5;   // requests in a row without any answer (offline, DNS, connection, TLS, time-out): the search ends with the connection error
uint64_t FailGapMs(int streak) {
    uint64_t gap = kPollMs;
    for (int i = 1; i < streak && gap < 10000; ++i) gap *= 2;
    return gap > 10000 ? 10000 : gap;
}

// Ends the attempt: the game's matched callback with an error and the DWC error the game shows. Called with the lock held; releases it.
void FailLocked(NpOnline::Fail fail, const char* why) {
    int type = 0, code = 0;
    NpDwcErrorFor(fail, &type, &code);
    MATCH_LOG("matchmaking failed: %s (error %d code %d)\n", why, type, code);
    if (NpMpDiagError) NpMpDiagError(0, code, "matchmaking_failed");   // (mpbeta27) the DWC code the game shows, in the stage it happened
    DWCMatchedCallback cb = g_m.matched;
    void* param = g_m.matchedParam;
    const bool linked = g_m.linkStarted;
    if (g_m.http) NpNet::HttpCancel(g_m.http);
    g_m.Reset();
    pthread_mutex_unlock(&g_lock);
    if (linked) NpNetLeave();
    NpDwcSetError(type, code);
    if (cb) cb(DWCError(type ? type : DWC_ERROR_NETWORK), 0, param);
    pthread_mutex_lock(&g_lock);
}

bool Start(const char* method, const std::string& path, const std::string& body, int kind, uint32_t extraTimeoutMs = 0) {
    // (each way out says why, once per attempt: on the console "not set up" alone did not tell a missing account from a refused request)
    NpOnlineCredentials creds;
    if (!NpOnlineLoadCredentials(&creds)) { MATCH_LOG("%s %s not started: no online account on this console (online.dat)\n", method, path.c_str()); return false; }
    const std::string base = ServiceBase();
    if (base.empty()) { memset(&creds, 0, sizeof creds); MATCH_LOG("%s %s not started: no service address\n", method, path.c_str()); return false; }
    std::string err;
    g_m.reqTimeoutMs = uint32_t(kRequestMs) + extraTimeoutMs;
    g_m.http = NpNet::HttpStart(method, base + path, body, Headers(creds.token), g_m.reqTimeoutMs, extraTimeoutMs ? NpNet::HeldIoMs(extraTimeoutMs) : 0, &err);
    memset(&creds, 0, sizeof creds);
    g_m.httpKind = kind;
    g_m.httpAt = NowMs();
    if (!g_m.http) MATCH_LOG("%s %s not started: %s\n", method, path.c_str(), err.c_str());
    return g_m.http != 0;
}

// A queue view: queued (wait), matched (the room), none / timeout (the attempt is over).
void OnQueueView(const NpNet::Json& j, int root) {
    const std::string status = j.GetStr(root, "status");
    const int ticket = j.Get(root, "ticket");
    if (ticket >= 0) { PutStr(g_m.ticketId, j.GetStr(ticket, "id")); PutStr(g_m.ticketToken, j.GetStr(ticket, "token")); }
    if (status == "matched") {
        const int room = j.Get(root, "room");
        const std::string code = j.GetStr(root, "roomCode");
        const std::string matchId = room >= 0 ? j.GetStr(room, "matchId") : std::string();
        const int slot = room >= 0 ? int(j.GetNum(room, "slot", -1)) : -1;
        if (code.size() < 4 || code.size() > 12 || matchId.empty() || matchId.size() > 30 || (slot != 0 && slot != 1)) {
            FailLocked(NpOnline::Fail::Protocol, "the pairing could not be read");
            return;
        }
        PutStr(g_m.room, code);
        PutStr(g_m.matchId, matchId);
        g_m.slot = slot;
        g_m.phase = Phase::Linking;
        g_m.linkAt = NowMs();
        MATCH_LOG("paired: room %s, match %s, slot %d\n", code.c_str(), matchId.c_str(), slot);
        if (NpMpDiagReach) { NpMpDiagReach(4); NpMpDiagReach(6); }   // (mpbeta27) match found, then the link is being made
        return;
    }
    if (status == "queued") {
        g_m.phase = Phase::Queued;
        // the service's pacing: `pollScale` (only above 1) stretches the holds and gaps, `heldMs` says this answer was held, so ask again at once
        const double ps = j.GetNum(root, "pollScale", 1);
        g_m.scalePct = (ps > 1.0 && ps <= 1.5) ? int(ps * 100 + 0.5) : 100;
        const bool held = j.Has(j.Get(root, "heldMs"));
        uint64_t gap = kPollMs * uint64_t(g_m.scalePct) / 100;
        if (NpNet::PaceHintsOn()) {   // gxnet14: the service dictates the rhythm (nextWaitSec: the hold to ask for next; nextPollMs: the pause after an answer that was not held)
            g_m.hintWaitSec = NpNet::PaceWaitSec(j.GetNum(root, "nextWaitSec", 0));
            const uint32_t pg = NpNet::PaceGapMs(j.GetNum(root, "nextPollMs", 0));
            if (pg && !held) gap = pg;
        }
        if (held) {
            const uint64_t heldMs = uint64_t(j.GetNum(root, "heldMs", 0));
            gap = kHeldGapMs;
            if (heldMs < 1000 && gap < 1000 - heldMs) gap = 1000 - heldMs;   // never faster than once a second in all
        }
        g_m.nextAt = NowMs() + gap;
        return;
    }
    FailLocked(NpOnline::Fail::Rejected, status == "timeout" ? "nobody to play was found in time" : "the queue entry is gone");
}

void OnResponse(const NpNet::HttpResponse& r) {
    const int kind = g_m.httpKind;
    g_m.http = 0;
    g_m.httpKind = 0;
    if (kind == 3) { g_m.phase = Phase::Joining; g_m.nextAt = NowMs() + 1000; return; }   // the stale room was left: queue again
    NpNet::Json j;
    const bool parsed = !r.error && j.Parse(r.body.data(), r.body.size());
    const int root = parsed ? j.Root() : -1;
    const int err = root >= 0 ? j.Get(root, "error") : -1;
    const std::string code = err >= 0 ? j.GetStr(err, "code") : std::string();
    if (r.status == 503 && code == "over_budget") {
        // over the service's daily budget: remember it for the console (np_service_busy.h); a ticket in the queue waits the Retry-After (5 to 60 s), a
        // fresh attempt fails at once with a plain sentence
        const int after = err >= 0 ? int(j.GetNum(err, "retryAfterSec", 0)) : 0;
        const std::string level = err >= 0 ? j.GetStr(err, "level") : std::string();
        NpNet::NoteServiceBusy(level.c_str(), after);
        if (g_m.phase == Phase::Joining) {
            NpOnline::SetStatusMessage(NpNet::BusySentence(NpNet::BusyLevelOf(level.c_str())));
            FailLocked(NpOnline::Fail::Disabled, "the online service is over its daily budget");
            return;
        }
        g_m.nextAt = NowMs() + NpNet::PollWaitAfterBusyMs(after);
        return;
    }
    if (r.error || r.status >= 500 || (r.status == 429 && code == "rate_limited" && j.GetStr(err, "reason") != "daily_cap")) {
        // transient: try again shortly (the queue keeps the place for 30 s without a poll; the overall limit still applies)
        const int after = err >= 0 ? int(j.GetNum(err, "retryAfterSec", 0)) : 0;
        if (r.error && ++g_m.failStreak >= kMaxNoAnswer) { FailLocked(NpOnline::Fail::Transport, "the online service cannot be reached"); return; }
        if (!r.error) ++g_m.failStreak;
        g_m.nextAt = NowMs() + (after > 0 && after < 60 ? uint64_t(after) * 1000 : FailGapMs(g_m.failStreak));
        return;
    }
    if (r.status == 426) { FailLocked(NpOnline::Fail::Upgrade, "this game version is too old for the service: please update the game"); return; }
    if (r.status == 401 || r.status == 403) { FailLocked(NpOnline::Fail::Auth, "the online account was not accepted"); return; }
    if (r.status == 503) { FailLocked(NpOnline::Fail::Disabled, "ranked matches are switched off for now"); return; }
    if (r.status == 409 && code == "in_ranked_match" && !g_m.retriedStale) {
        // a ranked room left over from an earlier run (a crash, the power button): leave it (no penalty before it started) and queue again
        const std::string stale = err >= 0 ? j.GetStr(err, "roomCode") : std::string();
        g_m.retriedStale = true;
        if (stale.size() >= 4 && stale.size() <= 12 && Start("POST", "/v1/rooms/" + stale + "/leave", "{}", 3)) return;
    }
    if (r.status != 200 || root < 0) {
        FailLocked(r.status == 429 ? NpOnline::Fail::RateLimited : NpOnline::Fail::Rejected, code.empty() ? "the service refused the queue" : code.c_str());
        return;
    }
    g_m.failStreak = 0;
    OnQueueView(j, root);
}

void PumpLocked() {
    const uint64_t now = NowMs();
    if (g_m.phase == Phase::Idle || g_m.phase == Phase::Closed) return;
    if (g_m.cancel) {
        const bool linked = g_m.linkStarted;
        const bool queued = g_m.phase == Phase::Joining || g_m.phase == Phase::Queued;
        if (g_m.http) NpNet::HttpCancel(g_m.http);
        g_m.Reset();
        pthread_mutex_unlock(&g_lock);
        if (queued) {   // leave the queue (fire and forget)
            NpOnlineCredentials creds;
            if (NpOnlineLoadCredentials(&creds) && !ServiceBase().empty())
                NpNet::HttpFire("DELETE", ServiceBase() + "/v1/ranked/queue", "", Headers(creds.token), 5000);
            memset(&creds, 0, sizeof creds);
        }
        if (linked) NpNetLeave();
        MATCH_LOG("matchmaking cancelled\n");
        pthread_mutex_lock(&g_lock);
        return;
    }
    if (g_m.http) {
        NpNet::HttpResponse r;
        const int rc = NpNet::HttpPoll(g_m.http, &r);
        if (rc == 1) OnResponse(r);
        else if (rc < 0 || now - g_m.httpAt > uint64_t(g_m.reqTimeoutMs) + 3000) {
            NpNet::HttpCancel(g_m.http); g_m.http = 0;
            if (++g_m.failStreak >= kMaxNoAnswer) { FailLocked(NpOnline::Fail::Transport, "the online service did not answer"); return; }
            g_m.nextAt = now + FailGapMs(g_m.failStreak);
        }
        return;
    }
    if ((g_m.phase == Phase::Joining || g_m.phase == Phase::Queued) && now - g_m.startedAt > kQueueLimitMs) {
        FailLocked(NpOnline::Fail::Timeout, "nobody to play was found in time");
        return;
    }
    if (now < g_m.nextAt) return;
    switch (g_m.phase) {
    case Phase::Joining:
        if (NpNet::ServiceBusyBlocksNew()) {   // said a moment ago: no request, the sentence at once
            NpOnline::SetStatusMessage(NpNet::BusySentence(NpNet::BusyState().level));
            FailLocked(NpOnline::Fail::Disabled, "the online service is over its daily budget (remembered)");
            return;
        }
        if (!Start("POST", "/v1/ranked/queue", g_m.quick ? "{\"mode\":\"quick\"}" : "{}", 1)) FailLocked(NpOnline::Fail::Transport, "the queue request could not be started");
        break;
    case Phase::Queued:
        {
            uint32_t wait = kHoldSec * uint32_t(g_m.scalePct) / 100;
            if (g_m.hintWaitSec) wait = g_m.hintWaitSec;
            if (wait < 1) wait = 1;
            if (wait > 55) wait = 55;
            if (!Start("GET", "/v1/ranked/queue?wait=" + std::to_string(wait), "", 2, wait * 1000)) g_m.nextAt = now + kPollMs;
        }
        break;
    case Phase::Linking: {
        if (!g_m.linkStarted) {
            NpOnlineCredentials creds;
            const bool ok = NpOnlineLoadCredentials(&creds);
            const NpOnline::ClientInfo& info = NpOnline::GetClientInfo();
            if (ok) NpNetConfigureService(ServiceBase().c_str(), creds.token, info.appVersion, info.build);
            memset(&creds, 0, sizeof creds);
            const std::string invite = "@" + StdStr(g_m.room);
            g_m.linkStarted = true;
            pthread_mutex_unlock(&g_lock);
            const int started = ok ? NpNetJoin(invite.c_str(), NpOnline::Name()[0] ? NpOnline::Name() : "Player") : 0;
            pthread_mutex_lock(&g_lock);
            if (!started) { FailLocked(NpOnline::Fail::Transport, "the online link could not start"); return; }
            return;
        }
        pthread_mutex_unlock(&g_lock);
        NpNetPoll();
        const int phase = NpNetPhase();
        pthread_mutex_lock(&g_lock);
        if (g_m.phase != Phase::Linking) return;
        if (phase == NpNet::OnlineNet::READY) {
            g_m.phase = Phase::Connected;
            DWCMatchedCallback cb = g_m.matched;
            void* param = g_m.matchedParam;
            MATCH_LOG("connected: I am AID %d of 2\n", g_m.slot);
            g_end.seen = false;
            pthread_mutex_unlock(&g_lock);
            if (NpLanOnlineSessionTuning) NpLanOnlineSessionTuning(1);
            if (cb) cb(DWC_ERROR_NONE, 0, param);
            pthread_mutex_lock(&g_lock);
        } else if (phase == NpNet::OnlineNet::FAILED || phase == NpNet::OnlineNet::RELAY_NEEDED || now - g_m.linkAt > kLinkLimitMs) {
            FailLocked(NpOnline::Fail::Timeout, NpNetError()[0] ? NpNetError() : "no route to the other player");
        }
        break;
    }
    default: break;
    }
}

// Connected: the link's frames go to the game's receive callback; a lost link is the game's "connection closed".
void PumpConnected() {
    pthread_mutex_lock(&g_lock);
    const bool on = g_m.phase == Phase::Connected;
    DWCUserRecvCallback recv = g_m.recv;
    pthread_mutex_unlock(&g_lock);
    if (!on) return;
    NpNetPoll();
    NpNet::OnlineNet* net = NpNetController();
    if (net) {
        uint8_t buf[2048];
        for (int i = 0; i < 64; ++i) {
            int slot = -1;
            const int n = net->RecvFrame(&slot, buf, sizeof buf);
            if (n <= 0) break;
            pthread_mutex_lock(&g_lock);
            uint8_t* dst = slot >= 0 && slot < 4 ? g_m.recvBuf[slot] : 0;
            const bool fits = dst && n <= g_m.recvSize[slot];
            if (fits) memcpy(dst, buf, size_t(n));
            pthread_mutex_unlock(&g_lock);
            if (fits && recv) recv(uint8_t(slot), dst, n);
        }
    }
    const int phase = NpNetPhase();
    if (phase == NpNet::OnlineNet::LOST || phase == NpNet::OnlineNet::FAILED || phase == NpNet::OnlineNet::OFF) {
        pthread_mutex_lock(&g_lock);
        DWCConnectionClosedCallback cb = g_m.closedSent ? 0 : g_m.closed;
        void* param = g_m.closedParam;
        const int peer = g_m.slot == 0 ? 1 : 0;
        g_m.closedSent = true;
        pthread_mutex_unlock(&g_lock);
        if (cb) {
            MATCH_LOG("the link to AID %d is gone (%s)\n", peer, NpNetError());
            cb(DWC_ERROR_DISCONNECTED, 0, 0, uint8_t(peer), 0, param);
        }
    }
}

void Pump() {
    pthread_mutex_lock(&g_lock);
    PumpLocked();
    pthread_mutex_unlock(&g_lock);
    PumpConnected();
}

void EnsurePump() {
    if (!g_pumpSet) { NpDwcAddPump(Pump); g_pumpSet = true; }
}

}  // namespace

// ---- the game's matchmaking calls -----------------------------------------------------------------------------------------------
extern "C" int DWC_ConnectToAnybodyAsync(unsigned char numEntry, const char* addFilter, DWCMatchedCallback matchedCallback, void* matchedParam,
                                         DWCEvalPlayerCallback evalCallback, void* evalParam) {
    (void)addFilter; (void)evalCallback; (void)evalParam;
    if (!RankedEnabled()) { MATCH_LOG("ranked matches are switched off (MSC_ONLINE_RANKED=0)\n"); return 0; }
    if (numEntry != 2) { MATCH_LOG("only two-player ranked matches exist (asked for %d)\n", int(numEntry)); return 0; }
    if (!NpOnline::RulesAllowRanked()) { MATCH_LOG("modified rules (MSC_GAME_FIXES nk): no ranked matches\n"); return 0; }
    if (NpDwcLoggedInProfile() <= 0 || ServiceBase().empty()) { MATCH_LOG("not signed in to the online service\n"); return 0; }
    pthread_mutex_lock(&g_lock);   // (the game calls this from its matchmaking thread; the work is done by the pump on the game thread)
    if (g_m.phase != Phase::Idle && g_m.phase != Phase::Closed) { pthread_mutex_unlock(&g_lock); return 0; }
    g_m.Reset();
    g_m.quick = g_quickRequested;
    g_m.phase = Phase::Joining;
    g_m.matched = matchedCallback;
    g_m.matchedParam = matchedParam;
    g_m.startedAt = NowMs();
    g_m.nextAt = 0;
    EnsurePump();
    pthread_mutex_unlock(&g_lock);
    MATCH_LOG("looking for an opponent%s\n", g_quickRequested ? " (quick match: unranked)" : "");
    if (NpMpDiagReach) NpMpDiagReach(3);   // (mpbeta27) the queue
    return 1;
}

extern "C" int DWC_IsValidCancelMatching(void) {
    pthread_mutex_lock(&g_lock);
    const bool v = g_m.phase == Phase::Joining || g_m.phase == Phase::Queued;
    pthread_mutex_unlock(&g_lock);
    return v ? 1 : 0;
}

extern "C" int DWC_CancelMatching(void) {
    pthread_mutex_lock(&g_lock);
    const bool v = g_m.phase == Phase::Joining || g_m.phase == Phase::Queued || g_m.phase == Phase::Linking;
    if (v) g_m.cancel = true;
    pthread_mutex_unlock(&g_lock);
    if (v) Pump();
    return v ? 1 : 0;
}

extern "C" int DWC_SetMatchingOption(DWCMatchOptType, const void*, int) { return 0; }
extern "C" unsigned char DWC_AddMatchKeyInt(unsigned char keyID, const char*, const int*) { return keyID; }
extern "C" int DWC_GetMatchIntValue(int, const char*, int idefault) { return idefault; }   // the service pairs; nothing is evaluated here
extern "C" int DWC_GetMOMinCompState(unsigned long long* time) { if (time) *time = 0; return 0; }

extern "C" int DWC_SetConnectionClosedCallback(DWCConnectionClosedCallback callback, void* param) {
    pthread_mutex_lock(&g_lock);
    g_m.closed = callback;
    g_m.closedParam = param;
    pthread_mutex_unlock(&g_lock);
    return 1;
}

extern "C" int DWC_SetUserRecvCallback(DWCUserRecvCallback callback) {
    pthread_mutex_lock(&g_lock);
    g_m.recv = callback;
    pthread_mutex_unlock(&g_lock);
    return 1;
}

extern "C" int DWC_SetRecvBuffer(unsigned char aid, void* buffer, int size) {
    if (aid >= 4) return 0;
    pthread_mutex_lock(&g_lock);
    g_m.recvBuf[aid] = static_cast<uint8_t*>(buffer);
    g_m.recvSize[aid] = buffer ? size : 0;
    pthread_mutex_unlock(&g_lock);
    return 1;
}

extern "C" unsigned char DWC_GetMyAID(void) {
    pthread_mutex_lock(&g_lock);
    const int s = g_m.slot;
    pthread_mutex_unlock(&g_lock);
    return uint8_t(s >= 0 ? s : 0);
}

extern "C" int DWC_GetNumConnectionHost(void) {
    pthread_mutex_lock(&g_lock);
    const bool on = g_m.phase == Phase::Connected;
    pthread_mutex_unlock(&g_lock);
    return on ? 2 : 0;
}

extern "C" int DWC_SendUnreliable(unsigned char aid, const void* buffer, int size) {
    pthread_mutex_lock(&g_lock);
    const bool on = g_m.phase == Phase::Connected && aid < 2 && int(aid) != g_m.slot;
    pthread_mutex_unlock(&g_lock);
    NpNet::OnlineNet* net = on ? NpNetController() : 0;
    if (!net || !buffer || size <= 0) return 0;
    return net->SendFrame(aid, buffer, size_t(size)) > 0 ? 1 : 0;
}

extern "C" int DWC_CloseAllConnectionsHard(void) {
    pthread_mutex_lock(&g_lock);
    const bool linked = g_m.linkStarted;
    const bool queued = g_m.phase == Phase::Joining || g_m.phase == Phase::Queued;
    if (queued) g_m.cancel = true;
    if (!queued) {
        if (g_m.http) NpNet::HttpCancel(g_m.http);
        DWCConnectionClosedCallback closed = g_m.closed;
        void* closedParam = g_m.closedParam;
        DWCUserRecvCallback recv = g_m.recv;
        g_m.Reset();
        g_m.closed = closed; g_m.closedParam = closedParam; g_m.recv = recv;
    }
    pthread_mutex_unlock(&g_lock);
    if (queued) Pump();
    else if (linked) NpNetLeave();
    if (linked && NpLanOnlineSessionTuning) NpLanOnlineSessionTuning(0);
    return 0;
}

// ---- the result of a ranked match -------------------------------------------------------------------------------------------------
// cGame::ChangeGameState(GS_END_GAME): the simulation step the match ended on, the game's checksum and seed at that point. Deterministic: the
// same values on both consoles of a match in sync, so they make the end-state digest both reports must carry.
extern "C" void NpRankedEndOfMatch(unsigned frame, unsigned checksum, unsigned seed) {
    pthread_mutex_lock(&g_lock);
    if (g_m.phase == Phase::Connected && !g_end.seen) {
        g_end.seen = true;
        g_end.frame = frame;
        g_end.checksum = checksum;
        g_end.seed = seed;
        MATCH_LOG("match over at step %u: digest %08x/%08x\n", frame, checksum, seed);
    }
    pthread_mutex_unlock(&g_lock);
}

// NetworkStatsManager::CalculateAndReportGameResult of a ranked match: result 0 finished, 2 desync, 3/4 link lost. Queued once per match.
extern "C" void NpRankedMatchResult(int result, int localIsHome, int homeScore, int awayScore) {
    pthread_mutex_lock(&g_lock);
    const std::string matchId = StdStr(g_m.matchId);
    const int slot = g_m.slot;
    const EndDigest end = g_end;
    const bool quick = g_m.quick;
    const bool already = !matchId.empty() && matchId == StdStr(g_reportedMatch);
    if (!matchId.empty()) PutStr(g_reportedMatch, matchId);
    pthread_mutex_unlock(&g_lock);
    if (matchId.empty() || (slot != 0 && slot != 1) || already) {
        if (!already) MATCH_LOG("result %d not reported: no ranked match of the service is in progress\n", result);
        return;
    }
    const int local = localIsHome ? homeScore : awayScore, other = localIsHome ? awayScore : homeScore;
    NpOnline::ResultReport r;
    snprintf(r.matchId, sizeof r.matchId, "%s", matchId.c_str());
    r.result = result;
    r.score0 = slot == 0 ? local : other;
    r.score1 = slot == 0 ? other : local;
    // a finished match without a recorded end (should not happen) still reports; the service then calls it disputed if the other side differs
    snprintf(r.checksum, sizeof r.checksum, "%08x%08x%08x", end.seen ? end.frame : 0u, end.seen ? end.checksum : 0u, end.seen ? end.seed : 0u);
    snprintf(r.mode, sizeof r.mode, quick ? "unranked" : "ranked");
    const bool queued = NpOnline::QueueResult(r);
    MATCH_LOG("result %d %d-%d (slot %d) for match %s %s\n", result, r.score0, r.score1, slot, r.matchId, queued ? "queued" : "NOT queued");
    NpOnline::QueueKick();
}

// ContinuePostGame (SHPausePostGame.cpp): a session the ranked queue paired is one game. The service keeps one match record per pairing
// and closes the room when it is reported, so a second game of the game's series on the same link could never be rated; the players go
// back to Online Central instead (and may queue again).
extern "C" int NpRankedSingleGame(void) {
    pthread_mutex_lock(&g_lock);
    const bool on = !g_m.matchId.empty();
    pthread_mutex_unlock(&g_lock);
    return on ? 1 : 0;
}

extern "C" int NpRankedBusy(void) {   // the friends poll stays quiet while a match is made or played (np_dwc_friend.cpp)
    pthread_mutex_lock(&g_lock);
    const bool busy = g_m.phase != Phase::Idle && g_m.phase != Phase::Closed;
    pthread_mutex_unlock(&g_lock);
    return busy ? 1 : 0;
}

void NpDwcSetQuickMatch(bool quick) { g_quickRequested = quick; }
bool NpDwcQuickMatch() { return g_quickRequested; }
bool NpDwcQuickMatchActive() {
    pthread_mutex_lock(&g_lock);
    const bool q = g_m.quick && g_m.phase != Phase::Idle && g_m.phase != Phase::Closed;
    pthread_mutex_unlock(&g_lock);
    return q;
}

// tests
void NpRankedResetForTest() {
    pthread_mutex_lock(&g_lock);
    if (g_m.http) NpNet::HttpCancel(g_m.http);
    g_m = Match();
    g_pumpSet = false;   // (NpDwcResetForTest empties the pump list: the next search adds the hook again)
    g_end.seen = false;
    g_reportedMatch.clear();
    pthread_mutex_unlock(&g_lock);
}
const char* NpRankedMatchIdForTest() { return g_m.matchId.c_str(); }
