// np_keep_awake.h (1.3, wave 9, MSC_MP_KEEP_AWAKE): a player who waits untouched in a LAN or online lobby, the match queue, a friends invite wait or a
// two-console session screen gets the system's dimming and then its auto-standby, which suspends the console and drops the session. The game never
// called sceKernelPowerTick. While a session (any phase, lobby and queue included) exists the link layer ticks the power timer at most once a second.
// Single player pays nothing: outside a session Step() does not read the clock and returns 0.
// Header only and free of platform types (tests/test_keep_awake.cpp). The caller supplies the session test and the clock.
#ifndef NP_KEEP_AWAKE_H
#define NP_KEEP_AWAKE_H

#include <stdint.h>

namespace NpKeepAwake {

constexpr uint64_t kPeriodUs = 1000000;   // one tick a second (the shortest system dimming time is 30 s)
constexpr uint64_t kIdleLimitUs = 600ull * 1000000ull;   // r16 (fable-da21 F3): 10 minutes without a button or touch outside a match, then the ticks pause
enum : unsigned { kTick = 1, kStarted = 2, kStopped = 4, kPaused = 8, kResumed = 16 };

struct Gate {
    bool ticking = false;
    uint64_t nextUs = 0;
    unsigned clockReads = 0;   // counters for the tests and the log
    unsigned ticks = 0;
    bool paused = false;       // idle limit reached: no ticks until input
    uint32_t lastActivity = 0; // the input counter seen last (np_vita_input.cpp NpVitaInputActivity)
    uint64_t lastActiveUs = 0; // when the counter last moved, or a match was running
    // enabled: MSC_MP_KEEP_AWAKE; session: a LAN or online session, lobby, queue or invite wait exists. Clock: callable returning microseconds.
    // matchRunning: a match is being played (players press buttons anyway: the idle limit does not apply). activity: a counter the input poll
    // advances on every poll that saw a button, stick or touch. Idle limit: outside a match, kIdleLimitUs without the counter moving pauses the
    // ticks (kPaused once); the next movement of the counter resumes them (kResumed once).
    // Returns kTick when the caller must call sceKernelPowerTick now, kStarted / kStopped / kPaused / kResumed (alone or with kTick) on a change.
    template <class Clock> unsigned Step(bool enabled, bool session, bool matchRunning, uint32_t activity, Clock now) {
        if (!enabled || !session) {
            if (!ticking) return 0;   // single player: no clock read, nothing
            ticking = false;
            paused = false;
            return kStopped;
        }
        ++clockReads;
        const uint64_t t = now();
        unsigned r = 0;
        if (!ticking) { ticking = true; nextUs = t; lastActivity = activity; lastActiveUs = t; r |= kStarted; }
        if (matchRunning || activity != lastActivity) { lastActivity = activity; lastActiveUs = t; }
        if (paused) {
            if (t - lastActiveUs >= kIdleLimitUs) return r;   // (still idle)
            paused = false; nextUs = t; r |= kResumed;
        } else if (t - lastActiveUs >= kIdleLimitUs) {
            paused = true; return r | kPaused;
        }
        if (t >= nextUs) { nextUs = t + kPeriodUs; ++ticks; r |= kTick; }
        return r;
    }
};

}  // namespace NpKeepAwake

#endif
