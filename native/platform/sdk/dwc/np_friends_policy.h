// np_friends_policy.h (wave 13, netclient13, O4): the pacing rules of Galaxy 3.0's Friends list (CHANGELOG: "refreshes by itself while it is on screen, every 30 seconds
// at most, backs off after errors, stops after 3 minutes without a button press or 20 requests; START refreshes it by hand"), as far as they fit Strikers' held poll.
//
// Strikers' held poll (np_dwc_friend.cpp) already has: refresh only while the online menu is open (NpFriends::Start / Stop, paused in a match), a held request that the service
// answers at once when something changed and after 45 s when nothing did (so a quiet list costs one request per 45 s, the 30 s rule is met without a timer), never faster than
// once a second after an answer, a back-off after errors (15 s x the failure streak up to 4, 5 min after a refused sign-in or an upgrade demand, the service's Retry-After when it is
// busy). It lacks: the idle stop (a console left on the menu polls for ever) and a hand refresh. This header holds the two missing rules as a plain gate, off unless asked:
//   MSC_FRIENDS_IDLE_STOP_S=N   N > 0: no new friends request after N seconds without a button, stick or touch (Galaxy: 180). A button press starts the poll again at once.
//   MSC_FRIENDS_MAX_REQS=N      N > 0: no new friends request after N requests since the last button press (Galaxy: 20).
//   MSC_FRIENDS_MIN_GAP_S=N     N > 0: at least N seconds between two friends requests after a successful answer (Galaxy: 30). Costs invites and presence changes up to N seconds.
// Both stops cost a player who leaves the console idle on the menu the live invites ("X invites you") until the next button press; that is why they stay off until the console shows
// the trade is acceptable. With all three at 0 (the default) the gate always says yes and keeps no state that anything reads: the poll is exactly the earlier one.
// Header only, no platform types (tests/test_friends_policy.cpp). The caller supplies the clock and the input counter (np_vita_input.cpp NpVitaInputActivity).
#ifndef NP_FRIENDS_POLICY_H
#define NP_FRIENDS_POLICY_H

#include <stdint.h>

namespace NpFriendsPolicy {

struct Config {
    uint32_t idleStopMs;   // 0 = off
    uint32_t maxReqs;      // 0 = off
    uint32_t minGapMs;     // 0 = off
    Config() : idleStopMs(0), maxReqs(0), minGapMs(0) {}
    bool Any() const { return idleStopMs || maxReqs || minGapMs; }
};
enum : unsigned { kHold = 1, kStopped = 2, kResumed = 4 };   // Step() result bits: kHold = do not start a request now

struct Gate {
    bool started = false, stopped = false;
    uint32_t lastActivity = 0;
    uint64_t lastActiveMs = 0;
    uint32_t reqsSinceInput = 0;
    unsigned holds = 0, stops = 0, resumes = 0;   // counters for the log
    // Called when the engine is about to start a friends request. `activity`: the input counter (it moves on every poll that saw input; 0 forever on a host).
    unsigned Step(const Config& c, uint64_t now, uint32_t activity) {
        if (!c.Any()) return 0;                                   // switch off: nothing read, nothing kept
        if (!started) { started = true; lastActivity = activity; lastActiveMs = now; }
        unsigned r = 0;
        if (activity != lastActivity) {
            lastActivity = activity;
            lastActiveMs = now;
            reqsSinceInput = 0;
            if (stopped) { stopped = false; ++resumes; r |= kResumed; }
        }
        if (!stopped && ((c.idleStopMs && now - lastActiveMs >= c.idleStopMs) || (c.maxReqs && reqsSinceInput >= c.maxReqs))) { stopped = true; ++stops; r |= kStopped; }
        if (stopped) { ++holds; r |= kHold; }
        return r;
    }
    void NoteRequest() { ++reqsSinceInput; }
    // The gap after a successful answer: the service's own (held answer 250 ms .. 1 s) or the minimum, whichever is longer.
    static uint64_t Gap(const Config& c, uint64_t gapMs) { return c.minGapMs && gapMs < c.minGapMs ? c.minGapMs : gapMs; }
};

}  // namespace NpFriendsPolicy

#endif
