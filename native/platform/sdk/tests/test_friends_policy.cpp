// np_friends_policy.h (O4): Galaxy's friends-list pacing rules as a gate. Off (all zero): always yes, nothing read or kept. Idle stop after N s without input, request limit,
// a button press resumes, the minimum gap only ever lengthens a gap.
//   c++ -std=c++11 -Wall -Wextra -I<native> native/platform/sdk/tests/test_friends_policy.cpp -o /tmp/test_friends_policy && /tmp/test_friends_policy
// Mutants (each must fail): -DMUTANT_OFF_NOT_OFF (the gate acts with every switch at 0), -DMUTANT_NO_RESUME (input does not resume), -DMUTANT_NO_REQ_RESET (input does not reset
// the request count), -DMUTANT_GAP_SHORTENS (the minimum gap replaces a longer gap), -DMUTANT_IDLE_FROM_START (the idle clock is not reset by input).
#include "platform/sdk/dwc/np_friends_policy.h"

#include <cstdio>

static int g_checks = 0, g_failed = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_failed; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

using namespace NpFriendsPolicy;

static unsigned StepOf(Gate& g, const Config& c, uint64_t now, uint32_t act) {
#if defined(MUTANT_NO_RESUME)
    if (g.stopped && act != g.lastActivity) { g.lastActivity = act; return kHold; }
#endif
#if defined(MUTANT_NO_REQ_RESET)
    const uint32_t keep = g.reqsSinceInput;
    unsigned r = g.Step(c, now, act);
    if (act != 0 && keep > g.reqsSinceInput) g.reqsSinceInput = keep;
    return r;
#endif
#if defined(MUTANT_IDLE_FROM_START)
    const uint64_t start = g.lastActiveMs;
    unsigned r = g.Step(c, now, act);
    if (g.started) g.lastActiveMs = start;
    return r;
#endif
    return g.Step(c, now, act);
}

static uint64_t GapOf(const Config& c, uint64_t gap) {
#if defined(MUTANT_GAP_SHORTENS)
    return c.minGapMs ? c.minGapMs : gap;
#else
    return Gate::Gap(c, gap);
#endif
}

int main() {
    // off: nothing, whatever the clock and input do
    {
        Config c;
        Gate g;
#if defined(MUTANT_OFF_NOT_OFF)
        c.idleStopMs = 1000;
#endif
        for (uint64_t t = 0; t < 10 * 3600 * 1000ull; t += 45000) CHECK(StepOf(g, c, t, 0) == 0);
        CHECK(!g.started && g.holds == 0 && g.stops == 0 && !c.Any());
    }
    // idle stop: 180 s without input
    {
        Config c;
        c.idleStopMs = 180000;
        Gate g;
        uint64_t t = 0;
        CHECK(StepOf(g, c, t, 7) == 0);                       // first look: allowed
        for (t = 45000; t < 180000; t += 45000) CHECK(StepOf(g, c, t, 7) == 0);
        CHECK((StepOf(g, c, 180000, 7) & kStopped) != 0);     // 180 s of silence: stopped (and held)
        CHECK(g.stopped);
        CHECK((StepOf(g, c, 225000, 7) & kHold) != 0);
        CHECK(StepOf(g, c, 225001, 7) == kHold);              // no second 'stopped' event
        const unsigned r = StepOf(g, c, 230000, 8);           // a button press
        CHECK((r & kResumed) != 0 && (r & kHold) == 0);       // resumes at once
        CHECK(!g.stopped);
        CHECK(StepOf(g, c, 230000 + 179000, 8) == 0);         // the idle clock restarted at the press
        CHECK((StepOf(g, c, 230000 + 180000, 8) & kStopped) != 0);
        CHECK(g.stops == 2 && g.resumes == 1);
    }
    // an active player is never stopped by the idle rule
    {
        Config c;
        c.idleStopMs = 180000;
        Gate g;
        unsigned held = 0;
        for (uint64_t t = 0; t < 3600000; t += 20000) held += (StepOf(g, c, t, uint32_t(t / 20000)) & kHold) ? 1 : 0;
        CHECK(held == 0);
    }
    // request limit: 20 requests without a press
    {
        Config c;
        c.maxReqs = 20;
        Gate g;
        int allowed = 0;
        for (int i = 0; i < 40; ++i) {
            if (!(StepOf(g, c, uint64_t(i) * 1000, 3) & kHold)) { ++allowed; g.NoteRequest(); }
        }
        CHECK(allowed == 20);
        CHECK(g.stopped);
        CHECK((StepOf(g, c, 50000, 4) & kResumed) != 0);      // a press: the count restarts
        int more = 0;
        for (int i = 0; i < 40; ++i) if (!(StepOf(g, c, 51000 + uint64_t(i) * 1000, 4) & kHold)) { ++more; g.NoteRequest(); }
        CHECK(more == 20);
    }
    // the minimum gap only lengthens
    {
        Config c;
        c.minGapMs = 30000;
        CHECK(GapOf(c, 250) == 30000 && GapOf(c, 1000) == 30000 && GapOf(c, 45000) == 45000);
        Config off;
        CHECK(GapOf(off, 250) == 250);
    }
    std::printf(g_failed ? "test_friends_policy: FAILED (%d of %d)\n" : "test_friends_policy: PASS (%d failed of %d checks)\n", g_failed, g_checks);
    return g_failed ? 1 : 0;
}
