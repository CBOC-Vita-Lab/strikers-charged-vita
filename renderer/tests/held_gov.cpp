// Host test of the held-scene GPU governor (vgx_heldgov.h, scenes30 MSC_HELD_GOV) and of the fill governor's hold (vgx_fxgov.h MSC_FX_GOV_HOLD).
// VGX_HELDGOV_BREAK 1-5 must fail (run_heldgov_test.sh --mutants).
#include "../src/vgx_heldgov.h"
#include "../src/vgx_fxgov.h"
#include <cstdio>
#include <cstdlib>
using vgx::heldgov::Gov;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)
static const float B = 1001.0f / 60.0f * 2.0f;   // the 30 period, 33.37 ms
int main() {
    // 1. under budget: nothing ever moves (a held scene that makes 30 keeps its full look)
    {
        Gov g;
        g.Begin(0);
        for (int i = 0; i < 600; ++i) CHECK(g.Frame(28.0f + (i % 5), B) < 0);
        CHECK(g.level == 0 && g.moves == 0);
    }
    // 2. a walk-out at 45 ms: one level at a time, gap pictures apart, up to the top (3 without a size step); a single late picture never takes a level
    {
        Gov g;
        g.Begin(0);
        int firstAt = -1, at[4] = {-1, -1, -1, -1};
        for (int i = 0; i < 200; ++i) { const int r = g.Frame(45.0f, B); if (r > 0) { at[r - 1] = i; if (firstAt < 0) firstAt = i; } }
        CHECK(g.level == 3);
        CHECK(at[0] >= 10 && at[0] <= 20);                         // (the first gap, then the hot window)
        CHECK(at[1] - at[0] >= 10 && at[2] - at[1] >= 10);          // gap pictures between two moves
        Gov h;
        h.Begin(0);
        for (int i = 0; i < 300; ++i) h.Frame(i % 10 == 0 ? 40.0f : 25.0f, B);   // one late picture in ten: under lateN of 8, and the mean under 97 %
        CHECK(h.level == 0);
    }
    // 2b. a short spike (3 pictures at 60 ms, the mean over the line for 6 pictures) takes no level: the hot window is 8, the late count 3 of 8
    {
        Gov g;
        g.Begin(0);
        for (int i = 0; i < 20; ++i) g.Frame(25.0f, B);
        for (int i = 0; i < 3; ++i) g.Frame(60.0f, B);
        for (int i = 0; i < 40; ++i) g.Frame(25.0f, B);
        CHECK(g.level == 0);
    }
    // 3. a steady 4-of-8 late load sheds even with the mean under the line (35 / 30 alternate: mean 32.5 < 0.97 x 33.37)
    {
        Gov g;
        g.Begin(1);
        for (int i = 0; i < 40; ++i) g.Frame(i & 1 ? 35.0f : 30.0f, B);
        CHECK(g.level >= 1);
    }
    // 4. give back: only when the mean plus the level's saving stays under 85 % for 60 pictures; 26 ms + 1.1 (level 3's saving) = 27.1 < 28.4: comes back one level a time
    {
        Gov g;
        g.Begin(0);
        for (int i = 0; i < 100; ++i) g.Frame(45.0f, B);
        CHECK(g.level == 3);
        int backAt = -1;
        for (int i = 0; i < 80; ++i) if (g.Frame(26.0f, B) == 2 && backAt < 0) backAt = i;
        CHECK(backAt >= 59 && backAt <= 75);
        // at 27.5 ms level 2 (saving 2.3: 29.8 > 28.4) is NOT given back: the scene would be over again
        for (int i = 0; i < 300; ++i) g.Frame(27.5f, B);
        CHECK(g.level == 2);
    }
    // 5. a scene's end teaches the next one of its kind; the other kind starts on its own; a short scene (under 30 pictures) teaches nothing
    {
        Gov g;
        g.Begin(1);
        for (int i = 0; i < 200; ++i) g.Frame(48.0f, B);
        CHECK(g.End(1) == 3 && g.level == 0);
        CHECK(g.learned[1] == 3);
        g.Begin(1);
        CHECK(g.level == 3);
        g.End(1);
        g.Begin(0);
        CHECK(g.level == 0);
        for (int i = 0; i < 10; ++i) g.Frame(20.0f, B);
        g.End(0);
        CHECK(g.learned[0] == 0);
    }
    // 5b. heldcpu24 (MSC_HELD_GOV_SEED): the first replay starts at what the cut-scenes taught; once a replay has taught its own level that one wins; off: starts at 0
    {
        Gov g;
        g.seedReplay = true;
        g.Begin(0);
        for (int i = 0; i < 200; ++i) g.Frame(48.0f, B);
        g.End(0);
        CHECK(g.learned[0] == 3);
        g.Begin(1);
        CHECK(g.level == 3);                       // the first replay: seeded from the cut-scene
        for (int i = 0; i < 100; ++i) g.Frame(20.0f, B);   // easy replay: it gives levels back over time, ends mostly at a low level
        for (int i = 0; i < 400; ++i) g.Frame(20.0f, B);
        g.End(1);
        CHECK(g.known[1]);
        const int own = g.learned[1];
        CHECK(own < 3);
        g.Begin(1);
        CHECK(g.level == own);                     // the replay's own history wins over the seed
        g.End(1);
        Gov h;
        h.Begin(0);
        for (int i = 0; i < 200; ++i) h.Frame(48.0f, B);
        h.End(0);
        h.Begin(1);
        CHECK(h.level == 0);                       // seed off: as before
    }
    // 6. forced level (MSC_HELD_GOV_FORCE): fixed whatever the GPU says, never learned
    {
        Gov g;
        g.force = 2;
        g.Begin(0);
        CHECK(g.level == 2);
        for (int i = 0; i < 200; ++i) CHECK(g.Frame(i & 1 ? 60.0f : 10.0f, B) < 0 && g.level == 2);
        g.End(0);
        CHECK(g.learned[0] == 0);
        g.force = 4;
        g.maxLevel = 3;
        g.Begin(1);
        CHECK(g.level == 3);   // (no size step: the top is 3)
    }
    // 7. with a size step the top is 4
    {
        Gov g;
        g.maxLevel = 4;
        g.Begin(0);
        for (int i = 0; i < 300; ++i) g.Frame(55.0f, B);
        CHECK(g.level == 4 && vgx::heldgov::SizeStep(g.level, 6) && !vgx::heldgov::SizeStep(3, 6) && !vgx::heldgov::SizeStep(4, 0));
    }
    // 8. the fill governor's hold (MSC_FX_GOV_HOLD): with 0 a frame without sprites lets go at once (as before); with 60 the level stands through 60 such frames, then goes
    {
        vgx::FxGovernor f;
        f.on = true;
        for (int i = 0; i < 40; ++i) f.Frame(45, 45, B, 10.0f, false);
        CHECK(f.target == 512);
        f.Frame(45, 45, B, 0.2f, false);
        CHECK(f.target == 0);                       // the console's flap: one frame without sprites, all gone
        vgx::FxGovernor h;
        h.on = true;
        h.absentHold = 60;
        for (int i = 0; i < 40; ++i) h.Frame(45, 45, B, 10.0f, false);
        CHECK(h.target == 512);
        for (int i = 0; i < 60; ++i) { h.Frame(45, 45, B, 0.2f, false); CHECK(h.target == 512); }
        h.Frame(45, 45, B, 10.0f, false);           // the next burst: thinned from its first frame
        CHECK(h.target == 512 && h.level == 512);
        for (int i = 0; i < 61; ++i) h.Frame(45, 45, B, 0.2f, false);
        CHECK(h.target == 0);                       // a long absence: let go
        CHECK(!h.Holding(0.2f, false) || h.level > 0);
        vgx::FxGovernor m;                          // a Mega Strike (blocked): no hold, as before
        m.on = true;
        m.absentHold = 60;
        for (int i = 0; i < 40; ++i) m.Frame(45, 45, B, 10.0f, false);
        m.Frame(45, 45, B, 10.0f, true);
        CHECK(m.target == 0);
    }
    // 9. heldgpu24 (MSC_HELD_GOV_SPAN): console s54 Vice replay: GPU busy about 31 ms (mean) against a first-submit-to-last-done span of about 43 ms and a 33.4 ms budget. Fed the busy time the governor never moves
    //    (31 < 97 % of 33.4 and few pictures over 33.4); fed the larger of the two (what the host passes with the switch on) it takes its levels.
    {
        vgx::heldgov::Gov busy, span;
        busy.Begin(1); span.Begin(1);
        for (int i = 0; i < 187; ++i) {
            const float b = 29.0f + (i % 7) * 0.7f;       // 29.0 .. 33.2 ms, mean 31.1
            const float sp = 42.0f + (i % 5) * 0.8f;
            busy.Frame(b, 33.4f);
            span.Frame(std::max(b, sp), 33.4f);
        }
        CHECK(busy.level == 0 && busy.moves == 0);
        CHECK(span.level == 3);
    }
    if (g_fail) { std::printf("held_gov: %d FAILED\n", g_fail); return 1; }
    std::printf("held_gov: OK\n");
    return 0;
}
