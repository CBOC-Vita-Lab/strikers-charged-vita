// What a console remembers when the online service says it is over its daily budget (503 over_budget, PROTOCOL.md "Budget and pacing").
// The answer carries `level` ("browse-over", "new-over" or "over") and retryAfterSec. Every part of the client that talks to the service
// (signaling, the ranked queue, login and rankings, the retry queue) feeds this one memory, so that:
//   * a decisive call (host, join, queue, sign-up) fails at once with a plain sentence, and the next try within the remembered time is answered by
//     the console itself, with no request (a player pressing the button again does not spend the service's last requests);
//   * a poll waits the service's Retry-After (at least 5 s, at most 60 s) instead of its normal 1.7-2 s rhythm, so no retry storm;
//   * LAN and same-Wi-Fi play never come here.
// The memory lasts at most 10 minutes (the service says "try again tomorrow" in seconds: the console asks again after ten minutes, one request).
#ifndef NP_NET_SERVICE_BUSY_H
#define NP_NET_SERVICE_BUSY_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace NpNet {

enum BusyLevel { BUSY_NONE = 0, BUSY_BROWSE = 1, BUSY_NEW = 2, BUSY_ALL = 3 };

// The memory has its own clock (monotonic, the same everywhere it is read: the modules that feed it keep clocks of their own). Tests replace it.
inline uint64_t (*&BusyClock())() {
    static uint64_t (*clock)() = 0;
    return clock;
}
inline uint64_t BusyNowMs() {
    if (BusyClock()) return BusyClock()();
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000u + uint64_t(ts.tv_nsec / 1000000);
}

struct ServiceBusyState {
    uint64_t until;
    int level;
};
inline ServiceBusyState& BusyState() {
    static ServiceBusyState s = {0, 0};
    return s;
}
inline int BusyLevelOf(const char* name) {
    if (!name) return BUSY_ALL;
    if (!strcmp(name, "browse-over")) return BUSY_BROWSE;
    if (!strcmp(name, "new-over")) return BUSY_NEW;
    return BUSY_ALL;   // "over", or a level this build does not know: the safe reading
}
// The service said over_budget. `level` is its `level` member ("" when absent), retryAfterSec its hint.
inline void NoteServiceBusy(const char* level, int retryAfterSec) {
    int sec = retryAfterSec;
    if (sec < 30) sec = 30;
    if (sec > 600) sec = 600;
    ServiceBusyState& s = BusyState();
    s.level = BusyLevelOf(level);
    s.until = BusyNowMs() + uint64_t(sec) * 1000;
}
// gxnet14 (Galaxy quota32): an answer that is not the service's JSON error and says 429 is Cloudflare's own page ("1027 Worker exceeded free tier daily request limit"
// when the account's free quota is spent): to the player it is "busy", the same calm sentence and memory as the service's own over_budget, and the room-deciding calls
// do not retry it (every retry is one more request the spent quota refuses anyway). A 5xx page keeps its retries. MSC_NET_EDGE_BUSY=0 = the older behaviour (retries).
inline bool EdgeBusyOn() { const char* v = getenv("MSC_NET_EDGE_BUSY"); return !(v && v[0] == '0' && v[1] == 0); }
inline bool IsEdgeBusyPage(bool transportError, int status, bool haveErrorCode) { return EdgeBusyOn() && !transportError && !haveErrorCode && status == 429; }
// how long such a page keeps the console quiet (NoteServiceBusy clamps to 30..600 s): the quota is a day's, so a half minute is the least that makes sense
static const int kEdgeBusyQuietSec = 30;
// gxnet14: pacing the service dictates (server13 PROTOCOL.md "Pacing hints"; the Galaxy port obeys its service's poll plan the same way, svcfix2). The poll answers carry
// `nextWaitSec` (1..55: the hold to ask for next) and `nextPollMs` (the pause after an answer that was not held). Obeyed only with MSC_NET_PACE_HINTS=1 (default 0:
// a console timing change, to be measured on the console first); a number outside the bounds is ignored (0), so a hostile or broken service cannot stall or hammer a console.
inline bool PaceHintsOn() { const char* v = getenv("MSC_NET_PACE_HINTS"); return v && v[0] == '1' && v[1] == 0; }
inline uint32_t PaceWaitSec(double v) { return (v >= 1.0 && v <= 55.0) ? uint32_t(v) : 0u; }
inline uint32_t PaceGapMs(double v) { return (v >= 250.0 && v <= 60000.0) ? uint32_t(v) : 0u; }
inline void ResetServiceBusy() { BusyState().until = 0; BusyState().level = 0; }
inline bool ServiceBusyAt(int needLevel) {
    const ServiceBusyState& s = BusyState();
    return s.level >= needLevel && BusyNowMs() < s.until;
}
inline bool ServiceBusyBlocksNew() { return ServiceBusyAt(BUSY_NEW); }
inline bool ServiceBusyBlocksBrowse() { return ServiceBusyAt(BUSY_BROWSE); }
inline uint32_t ServiceBusyRemainingMs() {
    const ServiceBusyState& s = BusyState();
    const uint64_t nowMs = BusyNowMs();
    return nowMs < s.until ? uint32_t(s.until - nowMs) : 0;
}
// A poll that was told to wait: the service's Retry-After, never below 5 s and never above 60 s.
// retryAfterSec comes from the service's JSON answer, which may be anything (a number the size of 1e30, negative, NaN: int(double) of those is undefined
// behaviour, and uint32_t(seconds) * 1000 wraps). The number of seconds is clamped where it is read (0..86400: a day, the longest the service means, "closed until midnight"), and a wait that blocks the request queue is
// at most 60 s (the Galaxy port's rule; its client "honours retryAfterSec, clamped: no UB on huge values").
inline int ClampRetryAfterSec(double v) {
    if (!(v > 0.0)) return 0;          // negative, zero and NaN
    return v > 86400.0 ? 86400 : int(v);
}
inline uint32_t RetryAfterMs(int retryAfterSec) {
    if (retryAfterSec <= 0) return 0u;
    return retryAfterSec > 60 ? 60000u : uint32_t(retryAfterSec) * 1000u;
}
// Poll retry after a failed room poll (the Galaxy port's rule: back off to a minute with jitter, so a service that is down is not asked every 15 s by every console,
// and consoles that lost it together do not come back in step). `streak` counts consecutive failures (1 = the first). Streaks 1..4 keep the old waits (2, 4, 8, 16 s);
// from 5 on: 15 s, then 30 s, then 60 s for good; from streak 3 a deterministic jitter of +-12.5 percent from `seed` (any changing number, e.g. the clock).
// legacy = true gives the old rule exactly (MSC_NET_POLL_BACKOFF=0): 1000 << min(streak, 4) ms, 15000 ms from streak 5.
inline uint32_t PollFailWaitMs(int streak, uint64_t seed, bool legacy = false) {
    if (streak < 0) streak = 0;
    if (legacy) return streak > 4 ? 15000u : 1000u << (streak < 4 ? streak : 4);
    uint32_t base = streak <= 4 ? 1000u << streak : (streak == 5 ? 15000u : (streak == 6 ? 30000u : 60000u));
    if (streak >= 3) {
        const uint32_t h = uint32_t((seed * 2654435761ull) >> 11) ^ uint32_t(streak * 40503u);
        const int32_t spread = int32_t(base / 8);                 // +-12.5 %
        base = uint32_t(int32_t(base) + int32_t(h % uint32_t(2 * spread + 1)) - spread);
    }
    return base;
}
inline uint32_t PollWaitAfterBusyMs(int retryAfterSec) {
    long ms = long(retryAfterSec) * 1000;
    if (ms < 5000) ms = 5000;
    if (ms > 60000) ms = 60000;
    return uint32_t(ms);
}
inline const char* BusySentence(int level) {
    if (level == BUSY_BROWSE) return "The online rankings are closed for today because the service is very busy.";
    if (level == BUSY_NEW) return "The online service is very busy today. No new games can start. Try again later.";
    return "The online service has used up today's allowance. It opens again tomorrow.";
}

}  // namespace NpNet

#endif
