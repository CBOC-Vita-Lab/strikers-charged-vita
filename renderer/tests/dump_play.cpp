// Host test of vgx_dumpplay.h (MSC_DUMP_START): The Dump's first match starts on the bottom step, nothing else changes.
//   dump_play: build with -DVGX_DUMPPLAY_BREAK=N to run a mutant (1 any stadium seeded, 2 a learned step overridden, 3 switch ignored): the checks named in the header must fail.
// The policy is the shipped vgx_hold.h Policy (five-size ladder, last step 10) fed simulated GPU times shaped by the console's r11 Dump runs (w8/console/r11/worstgpu1.log, gsdump.log):
// step 10 (704x400, 1x) 21-26 ms, step 9 (752x424, 1x) about 1.12 times that, budget 26.7 ms (80 % of the 33.3 ms period). A picture is late when its GPU time exceeds the 33.3 ms period.
// Every figure is an estimate; nothing here ran on a console.
#include "../src/vgx_dumpplay.h"
#include <cstdio>

using namespace vgx;
static int fails = 0, passes = 0;
#define CHECK(c, ...) do { if (c) { ++passes; } else { ++fails; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

struct Run { int moves = 0, lateFrames = 0, finalStep = 0, framesAtStart = 0, madeObjects = 0; };

static Run Simulate(int startStep, float step10Ms, int frames, float kickMs = 0.0f) {   // kickMs: step 10 GPU time of the first 300 frames (the walk-in camera), 0 = none
    hold::Policy p;
    p.SetOrder(true, 5, 3);
    p.readyMask = 0xFF;
    p.off[hold::kFxSize] = p.off[hold::kCrowd] = true;
    p.Start(startStep, 4, 480);   // (the match start: 480 frames excused from proof, as the backend does with MSC_PICTURE_HOLD_START)
    Run r;
    unsigned seed = 7;
    const int start = p.step;
    for (int i = 0; i < frames; ++i) {
        seed = seed * 1664525u + 1013904223u;
        const float noise = (static_cast<float>((seed >> 8) & 0xFFFF) / 65535.0f - 0.5f) * 8.0f;   // +-4 ms
        const float ms = ((i < 300 && kickMs > 0.0f) ? kickMs : step10Ms) * (p.step >= 10 ? 1.0f : 1.12f) + noise;
        if (ms > 33.3f) ++r.lateFrames;
        if (p.step == start) ++r.framesAtStart;
        const int to = p.Frame(ms, 26.7f, false);
        if (to >= 0) { ++r.moves; ++r.madeObjects; }   // (a size step makes pass objects in play: 2 and 5 in the console runs)
    }
    r.finalStep = p.step;
    return r;
}

int main() {
    const int kLast = 10;
    // 1 the memory: off, nothing is seeded
    { dumpplay::Memory m; dumpplay::Armed() = false;
      for (int s = -1; s < 40; ++s) CHECK(m.Get(s) == -1, "off: stadium %d must answer none, got %d", s, m.Get(s)); }
    // 2 on: The Dump only
    { dumpplay::Memory m; dumpplay::Armed() = true;
      CHECK(m.Get(4) == 10, "on: The Dump answers the bottom step, got %d", m.Get(4));
      for (int s = -1; s < 40; ++s) if (s != 4) CHECK(m.Get(s) == -1, "on: stadium %d is not The Dump and must answer none, got %d", s, m.Get(s));
      // a learned step wins, here and elsewhere; Put on another stadium is untouched
      m.Put(4, 7); m.Put(13, 3);
      CHECK(m.Get(4) == 7, "a learned step at The Dump wins over the seed, got %d", m.Get(4));
      CHECK(m.Get(13) == 3, "stadium 13 learned 3, got %d", m.Get(13));
      m.Put(4, 0);
      CHECK(m.Get(4) == 0, "a learned step 0 wins over the seed, got %d", m.Get(4));
      dumpplay::Armed() = false; }
    // 3 what the start rule makes of it (the shipped StartLevel, ladder with five sizes: aa2 = 8, first size 5)
    { dumpplay::Memory m;
      dumpplay::Armed() = false;
      const int off = hold::StartLevel(m.Get(4), hold::StadiumClass(4), kLast, 5, 8, 5);
      dumpplay::Armed() = true;
      const int on = hold::StartLevel(m.Get(4), hold::StadiumClass(4), kLast, 5, 8, 5);
      const int onVice = hold::StartLevel(m.Get(13), hold::StadiumClass(13), kLast, 5, 8, 5);
      const int onLava = hold::StartLevel(m.Get(7), hold::StadiumClass(7), kLast, 5, 8, 5);
      dumpplay::Armed() = false;
      const int offLava = hold::StartLevel(m.Get(7), hold::StadiumClass(7), kLast, 5, 8, 5);
      CHECK(off == 9, "off: The Dump starts on step 9 (the console log), got %d", off);
      CHECK(on == 10, "on: The Dump starts on step 10, got %d", on);
      CHECK(onVice == 0, "on: Vice (13) still starts at full size, got %d", onVice);
      CHECK(onLava == offLava && onLava == 9, "on: Lava Pit (7) unchanged (%d vs %d)", onLava, offLava);
      // a ladder with fewer sizes: the seed is clamped to it
      dumpplay::Armed() = true;
      CHECK(hold::StartLevel(m.Get(4), 2, 9, 5, 8, 4) == 9, "ladder ending at step 9: start 9");
      CHECK(hold::StartLevel(m.Get(4), 2, 8, 5, 8, 3) == 8, "ladder ending at step 8: start 8");
      dumpplay::Armed() = false; }
    // 4 the console's Dump (step 10 GPU 24 ms, step 9 about 27 ms): the old start climbs down once and pays for it, the new one never moves
    { const Run a = Simulate(9, 22.5f, 1800, 27.5f), b = Simulate(10, 22.5f, 1800, 27.5f);   // (kick-off 27.5 ms at step 10, then 22.5: the r11 lane windows 29.2 / 26.4 then 20-24 ms)
      std::printf("start 9: %d move(s), %d late frames, final step %d | start 10: %d move(s), %d late frames, final %d\n", a.moves, a.lateFrames, a.finalStep, b.moves, b.lateFrames, b.finalStep);
      CHECK(a.moves == 1 && a.finalStep == 10, "old start: one move, to 10");
      CHECK(b.moves == 0 && b.finalStep == 10, "new start: no move, stays on 10");
      CHECK(b.lateFrames <= a.lateFrames, "new start has no more late frames than the old (%d vs %d)", b.lateFrames, a.lateFrames);
      CHECK(b.madeObjects < a.madeObjects, "new start makes no pass objects in play (%d vs %d)", b.madeObjects, a.madeObjects); }
    // 5 a load that fits at step 9 anyway (GPU 20 ms at step 10): the old start would step nothing and the new one gives up 6 % per side for the match; it must not go UP or flap
    { const Run b = Simulate(10, 20.0f, 3600);
      CHECK(b.moves == 0 && b.finalStep == 10, "fits: the new start stays on the bottom (the proof to climb is the policy's own, none here)"); }
    std::printf("dump_play: %d checks passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
