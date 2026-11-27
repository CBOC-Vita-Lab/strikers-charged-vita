// np_keep_awake.h: the power tick fires about once a second inside a session and never outside one; single player reads no clock.
//   c++ -std=c++11 -Wall -Wextra -I<native> native/platform/sdk/tests/test_keep_awake.cpp -o /tmp/test_keep_awake && /tmp/test_keep_awake
// Mutants (each must fail): -DMUTANT_ALWAYS (ticks outside a session too), -DMUTANT_NEVER (never ticks), -DMUTANT_EVERY_CALL (no one-second limit),
// -DMUTANT_NO_IDLE (no idle limit: ticks for ever), -DMUTANT_IDLE_IN_MATCH (the idle limit also applies inside a match), -DMUTANT_NO_RESUME (input does not resume).
#include "platform/sdk/net/np_keep_awake.h"

#include <cstdio>

static int g_checks = 0, g_failed = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_failed; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint64_t g_now = 0;
static unsigned g_reads = 0;
static uint64_t Clock() { ++g_reads; return g_now; }

// The caller as vlink_vita.cpp is written, with the mutants spliced in.
static bool g_match = false;
static uint32_t g_activity = 0;
static unsigned Frame(NpKeepAwake::Gate& g, bool session) {
#if defined(MUTANT_ALWAYS)
    session = true;
#endif
#if defined(MUTANT_NEVER)
    session = false;
#endif
#if defined(MUTANT_EVERY_CALL)
    g.nextUs = 0;
#endif
#if defined(MUTANT_NO_IDLE)
    g.lastActiveUs = g_now;
#endif
#if defined(MUTANT_IDLE_IN_MATCH)
    return g.Step(true, session, false, g_activity, Clock);
#elif defined(MUTANT_NO_RESUME)
    return g.Step(true, session, g_match, g.paused ? g.lastActivity : g_activity, Clock);
#else
    return g.Step(true, session, g_match, g_activity, Clock);
#endif
}

int main() {
    // single player: 6000 pictures, no tick, no clock read
    NpKeepAwake::Gate sp;
    unsigned spTicks = 0;
    for (int i = 0; i < 6000; ++i) { g_now += 16667; spTicks += (Frame(sp, false) & NpKeepAwake::kTick) ? 1u : 0u; }
    CHECK(spTicks == 0);
    CHECK(g_reads == 0);
    CHECK(sp.clockReads == 0 && !sp.ticking);

    // a session of 60 s at 60 pictures a second: about one tick a second, one start line
    NpKeepAwake::Gate mp;
    unsigned ticks = 0, started = 0, stopped = 0;
    for (int i = 0; i < 3600; ++i) {
        g_now += 16667;
        const unsigned r = Frame(mp, true);
        ticks += (r & NpKeepAwake::kTick) ? 1u : 0u;
        started += (r & NpKeepAwake::kStarted) ? 1u : 0u;
    }
    CHECK(ticks >= 59 && ticks <= 61);
    CHECK(started == 1);
    // leaving the session: one stop line, then nothing and no clock read again
    const unsigned readsBefore = g_reads;
    for (int i = 0; i < 600; ++i) { g_now += 16667; const unsigned r = Frame(mp, false); stopped += (r & NpKeepAwake::kStopped) ? 1u : 0u; ticks += (r & NpKeepAwake::kTick) ? 1u : 0u; }
    CHECK(stopped == 1);
    CHECK(ticks >= 59 && ticks <= 61);
    CHECK(g_reads == readsBefore);
    // the switch off (MSC_MP_KEEP_AWAKE=0): nothing in a session either
    NpKeepAwake::Gate off;
    for (int i = 0; i < 600; ++i) { g_now += 16667; CHECK(off.Step(false, true, false, 0, Clock) == 0); }
    // idle limit (F3): a waiting lobby (no match, no input) ticks for 10 minutes, then stops with one "paused" line; input resumes; a match never pauses
    NpKeepAwake::Gate id;
    unsigned idTicks = 0, paused = 0, resumed = 0, ticksAt9 = 0, ticksAt11 = 0, ticksAt19 = 0;
    g_match = false;
    for (int s = 0; s < 20 * 60; ++s) {   // one frame a second for 20 minutes
        g_now += 1000000;
        const unsigned r = Frame(id, true);
        idTicks += (r & NpKeepAwake::kTick) ? 1u : 0u; paused += (r & NpKeepAwake::kPaused) ? 1u : 0u; resumed += (r & NpKeepAwake::kResumed) ? 1u : 0u;
        if (s == 9 * 60) ticksAt9 = idTicks;
        if (s == 11 * 60) ticksAt11 = idTicks;
    }
    CHECK(ticksAt9 >= 530 && ticksAt9 <= 541);   // still ticking before the limit
    CHECK(ticksAt11 == idTicks && idTicks >= 598 && idTicks <= 601);   // none after the 10th minute
    CHECK(paused == 1 && resumed == 0 && id.paused);
    ticksAt19 = idTicks;
    g_activity++;   // a button
    for (int s = 0; s < 30; ++s) { g_now += 1000000; const unsigned r = Frame(id, true); idTicks += (r & NpKeepAwake::kTick) ? 1u : 0u; resumed += (r & NpKeepAwake::kResumed) ? 1u : 0u; paused += (r & NpKeepAwake::kPaused) ? 1u : 0u; }
    CHECK(resumed == 1 && paused == 1 && !id.paused);
    CHECK(idTicks >= ticksAt19 + 29 && idTicks <= ticksAt19 + 31);
    // a running match: 30 minutes with no input still ticks, never pauses
    NpKeepAwake::Gate mt;
    unsigned mtTicks = 0, mtPaused = 0;
    g_match = true;
    for (int s = 0; s < 30 * 60; ++s) { g_now += 1000000; const unsigned r = Frame(mt, true); mtTicks += (r & NpKeepAwake::kTick) ? 1u : 0u; mtPaused += (r & NpKeepAwake::kPaused) ? 1u : 0u; }
    CHECK(mtPaused == 0 && mtTicks >= 1799 && mtTicks <= 1801);
    // the match ends: the 10 minutes start from there (not from the last button), then it pauses
    g_match = false;
    unsigned afterTicks = 0, afterPaused = 0;
    for (int s = 0; s < 12 * 60; ++s) { g_now += 1000000; const unsigned r = Frame(mt, true); afterTicks += (r & NpKeepAwake::kTick) ? 1u : 0u; afterPaused += (r & NpKeepAwake::kPaused) ? 1u : 0u; }
    CHECK(afterPaused == 1 && afterTicks >= 598 && afterTicks <= 601);
    std::printf("idle: %u ticks in 20 min, paused %u, resumed %u; match 30 min %u ticks, paused %u\n", ticksAt11, paused, resumed, mtTicks, mtPaused);
    std::printf("single player: %u ticks, %u clock reads; session 60 s: %u ticks\n", spTicks, 0u, ticks);
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
