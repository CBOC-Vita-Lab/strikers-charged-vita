// MSC_LOCK60 (vgx_lock60.h, the shipped header): the picture-size governor on simulated GPU-time traces, one per scene class, and the pieces around it (start level, the plan of
// rungs to make ahead, the steps after the floor). Pure: no GL, no clock.
//   (nxr6a: N=15 the nxr4a skip of 2, 16 GPU reset without the skip when EMA reset is off, 17 the host drops nothing)
//   lock60 [--break=N]     N=1: rungs are used whether or not they are made (the allocation check must fail), N=2: a governor with no proof and no block (the flapping check must fail),
//                          N=3: no probation (an up step that does not hold is not taken back: the failed-try check must fail), N=4: the Relief steps are not spaced (the spacing check must fail),
//                          N=10..14 (nxr4a): 10 the relief band is off (a step earned in a walk-out never comes back in Vice), 11 the band has no block (a scene that needs the step flaps), 12 the block does not double,
//                          13 play beginning does not forget the GPU mean (the fill governor steps up on the old scene's figure), 14 it forgets it but reads the two old-scene frames that arrive after the cut,
//                          N=9 (nxr3a): play after play also resets; N=5..8 (nxr3a): the menu's steady rule is not applied (5), the LAN hold is not applied (6), the freeze is not applied (7), the steady rule blocks a sustained overload too (8)
// The GPU time of a frame at a rung is fixed + pixels x the rung's pixel cost (true share `share`, the governor assumes its own 0.30), times the scene's load, with noise;
// the governor sees it two frames late (the console's notifications), and the two frames after a change are measured at the old size and skipped, as the renderer does.
#include "vgx_fxgov.h"
#include "vgx_lock60.h"
#include "vgx_scenefresh.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace vgx::lock60;

namespace {
int g_fail = 0, g_pass = 0, g_break = 0;
void Check(bool ok, const char* what, const char* detail = "") {
    if (ok) { ++g_pass; std::printf("ok   %s %s\n", what, detail); }
    else { ++g_fail; std::printf("FAIL %s %s\n", what, detail); }
}

struct Lv { float s, p; int ms; };
const Lv kLv[] = {{1.20f, 1.0f, 1}, {1.12f, 1.0f, 1}, {1.06f, 1.0f, 1}, {1.00f, 1.0f, 1}, {1.00f, 0.75f, 1}, {1.00f, 0.5f, 1}, {0.93f, 0.5f, 1},
                  {0.86f, 0.5f, 1}, {0.78f, 0.5f, 1}, {0.70f, 0.5f, 1}, {0.62f, 0.5f, 1}, {0.56f, 0.5f, 1}, {0.50f, 0.5f, 1}};
constexpr int kN = 13, kNative = 3;
float Pix(int l) { return kLv[l].s * kLv[l].s * (0.7f + 0.3f * kLv[l].p * kLv[l].p); }
const float kBudget = 1001.0f / 60.0f;

// deterministic noise
struct Rng {
    unsigned s = 12345u;
    float U() { s = s * 1664525u + 1013904223u; return static_cast<float>((s >> 8) & 0xFFFF) / 65535.0f; }
};

// A load: GPU ms at the game's own size as a function of the frame number.
struct Load {
    float fixedMs, varMs;                        // the size-independent part, the part that follows the pixels at the game's own size
    float (*mult)(int frame);                    // the scene's load over time (1 = as given)
};

struct Result {
    int downs = 0, ups = 0, takebacks = 0, late = 0, frames = 0, notReady = 0, allocs = 0, finalLevel = 0, maxLevel = 0, minLevel = 99, changesLate = 0;
    std::vector<int> changeAt;
};

// Run the governor over `frames` frames. ready[l]: the rung's target was made ahead; steps to a rung that is not ready would allocate in play.
Result Run(const Load& ld, int frames, int start, int best, int floorLv, const bool* ready, float share, Gov g, unsigned seed = 1, int skipNoise = 0) {
    Result r;
    Rng rng;
    rng.s = seed;
    g.Start(start, best, floorLv);
    int level = start, ignore = 0;
    std::vector<float> pipe(3, 0.0f);   // GPU times of the frames in flight, newest last: the governor sees the one two frames old
    const float pixShareTrue = share;
    for (int f = 0; f < frames; ++f) {
        const float base = ld.fixedMs + ld.varMs * 0.0f;
        const float cost = (1.0f - pixShareTrue) * (ld.fixedMs + ld.varMs) + pixShareTrue * (ld.fixedMs + ld.varMs) * Pix(level);
        (void)base;
        float ms = cost * (ld.mult ? ld.mult(f) : 1.0f) * (1.0f + (skipNoise ? 0.0f : (rng.U() - 0.5f) * 0.06f));
        // measured two frames later
        pipe[0] = pipe[1]; pipe[1] = pipe[2]; pipe[2] = ms;
        ++r.frames;
        if (ms > kBudget) ++r.late;
        const float seen = pipe[0];
        if (f < 2) continue;
        if (ignore > 0) { --ignore; continue; }
        auto rdy = [&](int l) { return g_break == 1 ? true : ready[l]; };
        const int to = g.Frame(seen, kBudget, Pix, rdy, false, false);
        if (to >= 0) {
            if (!ready[to] && to != level) { ++r.allocs; ++r.notReady; }
            const bool up = to < level;
            if (up) ++r.ups; else ++r.downs;
            r.changeAt.push_back(f);
            g.Changed(to, up);
            level = to;
            ignore = 2;
            r.maxLevel = std::max(r.maxLevel, level);
            r.minLevel = std::min(r.minLevel, level);
        }
    }
    r.takebacks = g.takebacks;
    r.finalLevel = level;
    return r;
}

Gov MakeGov() {
    Gov g;
    g.pixShare = 0.30f;
    if (g_break == 2) { g.proofFrames = 1; g.blockBase = 0; g.gap = 1; g.upFit = 0.99f; g.upHard = 1.5f; g.probeFrames = 0; }
    if (g_break == 3) { g.probeFrames = 0; }
    return g;
}

float One(int) { return 1.0f; }
float Spike(int f) { return (f >= 1200 && f < 1203) ? 2.2f : 1.0f; }                       // three frames at 2.2x, once
float Burst(int f) { return (f >= 1200 && f < 1500) ? 1.6f : 1.0f; }                        // 5 s at 1.6x, then back
float StepUp(int f) { return f >= 600 ? 1.5f : 1.0f; }                                      // the scene gets heavier for good
float Wobble(int f) { return 1.0f + 0.10f * std::sin(static_cast<float>(f) * 0.02f); }      // a load that drifts +-10 %
float Items(int f) { return (f % 420) < 60 ? 1.35f : 1.0f; }                                // an item every 7 s, 1 s of heavier frames
float Square(int f) { return ((f / 180) % 2) ? 1.45f : 1.0f; }                                 // 3 s of calm, 3 s of 1.45x, over and over
float Replay(int f) { return f < 300 ? 1.0f : 1.05f + 0.1f * std::sin(static_cast<float>(f) * 0.05f); }

float MenuBurst(int f) { return (f >= 800 && f < 803) ? 2.9f : 1.0f; }                      // nxr3a: a texture-upload burst in a menu: 3 frames at 2.9x (20 ms against a 6.9 ms mean)
float Burst46(int f) { const int p = f % 900; return p < 630 ? 1.0f : 1.25f; }   // flip25: a crowd burst of 4.5 s every 15 s (the console's size changes in pairs seconds apart)
void Flip25() {
    bool ready[kN];
    for (int i = 0; i < kN; ++i) ready[i] = i >= kNative;
    Load ld{9.5f, 6.0f, Burst46};
    Gov a = MakeGov(), b = MakeGov();
    b.upExtra = 600;
    const Result ra = Run(ld, 10800, 5, kNative, 9, ready, 0.30f, a), rb = Run(ld, 10800, 5, kNative, 9, ready, 0.30f, b);
    char d[200];
    std::snprintf(d, sizeof d, "(MODEL, 3 min of bursts: %d size changes, %d taken back, late %d; with UPSLOW=600: %d, %d, late %d)", static_cast<int>(ra.changeAt.size()), ra.takebacks, ra.late, static_cast<int>(rb.changeAt.size()), rb.takebacks, rb.late);
    Check(rb.changeAt.size() < ra.changeAt.size() && rb.notReady == 0 && rb.late <= ra.late + 2, "flip25: UPSLOW makes fewer size changes than the default on a bursty load, with no more late frames", d);
    Check(b.upExtra == 600 && MakeGov().upExtra == 0, "flip25: UPSLOW is off by default (the governor's own upExtra is 0)");
}

// nxr3a R3-4 / R3-6: the menu's steady rule, the freeze, the LAN hold.
Gov Steady(bool on) { Gov g = MakeGov(); g.steady = on && g_break != 5; if (on && g_break == 8) g.downAt = 2.0f; return g; }
void Nxr3a() {
    bool ready[kN];
    for (int i = 0; i < kN; ++i) ready[i] = i >= kNative;
    {   // 6122 f=504-506: a 3 frame burst in a calm menu. The plain governor steps down on the first late frame, the steady one does not.
        Load ld{6.0f, 1.0f, MenuBurst};
        const Result plain = Run(ld, 2400, 4, kNative, 9, ready, 0.30f, Steady(false)), st = Run(ld, 2400, 4, kNative, 9, ready, 0.30f, Steady(true));
        char d[160];
        std::snprintf(d, sizeof d, "(plain: %d down %d up; steady: %d down %d up)", plain.downs, plain.ups, st.downs, st.ups);
        Check(plain.downs >= 1, "menu burst: the plain governor steps down on one burst (what 6122 f=506 did)", d);
        Check(st.downs == 0, "menu burst: the steady governor does not step down on one late frame", d);
    }
    {   // a sustained overload in a menu still steps down (a real overload is not hidden)
        Load ld{14.0f, 8.0f, One};
        const Result st = Run(ld, 1800, 4, kNative, 9, ready, 0.30f, Steady(true));
        char d[160];
        std::snprintf(d, sizeof d, "(%d down, level %d, late %d of %d)", st.downs, st.finalLevel, st.late, st.frames);
        Check(st.downs >= 1 && st.finalLevel > 4, "menu overload: a sustained overload still steps down with the steady rule", d);
    }
    {   // the probation: one frame over probeAt right after an up step takes it back, unless steady
        for (int pass = 0; pass < 2; ++pass) {
            Gov g = Steady(pass == 1);
            g.Start(5, kNative, 9);
            int to = -1, f = 0;
            for (; f < 400 && to < 0; ++f) to = g.Frame(6.0f, kBudget, Pix, [&](int l) { return ready[l]; }, false, false);
            const bool up = to == 4;
            if (up) g.Changed(to, true);
            int r = -1;
            for (int i = 0; i < 8; ++i) r = g.Frame(6.0f, kBudget, Pix, [&](int l) { return ready[l]; }, false, false);
            r = g.Frame(0.97f * kBudget, kBudget, Pix, [&](int l) { return ready[l]; }, false, false);
            Check(up && (pass == 0 ? r == 5 : r < 0), pass == 0 ? "probation: one frame over 0.95 of the budget takes an up step back (plain)" : "probation: the steady governor keeps the up step after one such frame");
        }
    }
    {   // the freeze: no decision at all, whatever the load
        Gov g = MakeGov();
        g.frozen = g_break != 7;
        g.Start(4, kNative, 9);
        int moves = 0;
        for (int i = 0; i < 3000; ++i) {
            const float ms = (i / 300) % 2 ? 24.0f : 4.0f;
            if (g.Frame(ms, kBudget, Pix, [&](int l) { return ready[l]; }, false, false) >= 0) ++moves;
        }
        Check(moves == 0, "freeze: no size decision in 3000 frames of alternating overload and calm");
    }
    {   // the LAN hold: no step up on a calm load, a step down on an overload stays possible, and the plain governor does go up
        Load calm{6.0f, 2.0f, One}, hot{14.0f, 8.0f, One};
        Gov a = MakeGov(), b = MakeGov();
        b.noUp = g_break != 6;
        const Result ra = Run(calm, 1800, 7, kNative, 9, ready, 0.30f, a), rb = Run(calm, 1800, 7, kNative, 9, ready, 0.30f, b);
        Gov c = MakeGov();
        c.noUp = true;
        const Result rc = Run(hot, 1800, 4, kNative, 9, ready, 0.30f, c);
        char d[160];
        std::snprintf(d, sizeof d, "(plain %d ups, hold %d ups, overload with the hold: %d downs)", ra.ups, rb.ups, rc.downs);
        Check(ra.ups >= 1, "LAN hold: the plain governor steps up on a calm load", d);
        Check(rb.ups == 0, "LAN hold: no step up in 30 s of calm", d);
        Check(rc.downs >= 1, "LAN hold: a step down on overload is still taken", d);
    }
    {   // the scene-boundary reset: only live play entered from another kind resets the controller's GPU figures
        auto begins = [](int prev, int kind) { return g_break == 9 ? (kind == kPlay || kind == kHeavy) : PlayBeginsAfterOther(prev, kind); };
        bool ok = begins(kCut, kPlay) && begins(kCut, kHeavy) && begins(kMenu, kPlay) && begins(kReplay, kPlay) && begins(kMega, kPlay) && begins(-1, kPlay);
        Check(ok, "play begins after a cut-scene, menu, replay, Mega Strike or the start: the GPU figures are forgotten");
        Check(!begins(kPlay, kPlay) && !begins(kPlay, kHeavy) && !begins(kHeavy, kPlay) && !begins(kPlay, kCut) && !begins(kCut, kMenu) && !begins(kCut, kMega),
              "no reset play -> play, heavy <-> play, or when the scene that begins is not play");
    }
    {   // defaults: a plain Gov has all three off, so every other scene kind and every offline run is the governor as before
        const Gov g;
        Check(!g.steady && !g.frozen && !g.noUp, "defaults: steady, frozen and noUp are all off in a plain governor");
        // identical decisions with the flags at their defaults: the same trace through two governors, one of which has the flags set to false explicitly
        Load ld{12.0f, 5.0f, Items};
        Gov x = MakeGov(), y = MakeGov();
        y.steady = false; y.frozen = false; y.noUp = false;
        const Result rx = Run(ld, 5400, 6, kNative, 9, ready, 0.30f, x), ry = Run(ld, 5400, 6, kNative, 9, ready, 0.30f, y);
        Check(rx.changeAt == ry.changeAt && rx.late == ry.late, "defaults: the decisions are the same with the flags set to false explicitly");
    }
}


// nxr4a: the steps after the floor on the GPU means the console logged, and the fill governor at the first pictures of play.
struct OldRelief {   // the Relief of nxr3a and before (the band off), kept here as the reference the new one must equal with the band off
    int step = 0, maxStep = 3, hot = 0, calm = 0, wait = 0, stepEvery = 30, calmFrames = 300, hold = 120;
    int Frame(float ema, float budget, bool noRoom, bool holdUp) {
        if (wait > 0) --wait;
        if (noRoom && !holdUp && ema > 0.92f * budget) {
            calm = 0;
            if (++hot >= stepEvery && step < maxStep && wait == 0) { hot = 0; wait = hold; return ++step; }
        } else {
            hot = 0;
            if (!noRoom || ema < 0.70f * budget) { if (++calm >= calmFrames && step > 0 && wait == 0) { calm = 0; wait = hold; return --step; } }
            else calm = 0;
        }
        return -1;
    }
};
Relief MakeRelief() {
    Relief r;
    r.bandAt = g_break == 10 ? 0.0f : 0.84f;
    if (g_break == 11) r.blockFrames = 0;
    return r;
}
// the walk-out: 900 frames at 30 ms against the 60 budget earn the three steps, then live play at `mean` ms for `frames` frames, no rung below (the floor rung 728x512). Returns the step at the end.
int PlayAt(Relief& rl, float mean, int frames) {
    Rng rng;
    if (rl.step == 0) for (int i = 0; i < 900; ++i) rl.Frame(30.0f, kBudget, true, false);
    for (int i = 0; i < frames; ++i) rl.Frame(mean + (rng.U() - 0.5f) * 0.8f, kBudget, true, false);
    return rl.step;
}
void Nxr4a() {
    // the logged means in live play at the floor rung (level 2, no rung below): 6121.log:1780 12.1, 6032.log:1129 12.1, 6123.log:940 11.7, :1374 11.9, 6122.log:970 12.7, 6027.log:1547 11.2 (all "relief 2 or 3").
    // The relief bought in the walk-out (30 ms against 16.7) must come back in 2000 frames (33 s); the heavy ones (6032.log:1475 13.6, The Dump 6124.log:907 23.6, :1251 22.0, :1540 20.6) keep it.
    {
        const float vice[] = {11.2f, 11.7f, 11.9f, 12.1f, 12.7f, 13.6f};
        int stuck = 0;
        float worst = 0.0f;
        for (float m : vice) { Relief rl = MakeRelief(); if (PlayAt(rl, m, 2500) != 0) { ++stuck; worst = m; } }
        char d[100];
        std::snprintf(d, sizeof d, "(%d of %d logged Vice means kept a step after 2500 frames, last %.1f ms)", stuck, 6, worst);
        Check(stuck == 0, "relief: a step earned in the walk-out comes back in live play at the logged Vice means (11.2-12.7 ms of 16.7, and 13.6 of 6032.log:1475)", d);
        int kept = 0;
        const float heavy[] = {14.6f, 17.5f, 20.6f, 22.0f, 23.6f};
        for (float m : heavy) { Relief rl = MakeRelief(); if (PlayAt(rl, m, 4000) == 3) ++kept; }
        std::snprintf(d, sizeof d, "(%d of 5 kept all three steps)", kept);
        Check(kept == 5, "relief: the heavy stadiums' means (14.6 and over, The Dump 17.5-23.6: above the 0.84 line, 14.0 ms) keep all three steps", d);
        // the old rule is stuck on the same traces (the bug this fixes)
        int oldStuck = 0;
        for (float m : vice) { OldRelief o; Rng rng; for (int i = 0; i < 900; ++i) o.Frame(30.0f, kBudget, true, false); for (int i = 0; i < 2000; ++i) o.Frame(m + (rng.U() - 0.5f) * 0.8f, kBudget, true, false); if (o.step == 3) ++oldStuck; }
        Check(oldStuck >= 4, "relief: the rule as it was keeps step 3 at every logged Vice mean above 11.7 ms (the bug)", "");
    }
    {   // a scene that needs the step: the mean is 12 ms with it and 16.5 without. It may go up again, but the block and its doubling bound the flapping (10 minutes)
        Relief rl = MakeRelief();
        if (g_break == 12) rl.blockMaxMul = 1;
        int ups = 0;
        for (int i = 0; i < 900; ++i) rl.Frame(30.0f, kBudget, true, false);
        int prev = rl.step;
        for (int i = 0; i < 36000; ++i) {
            const float ema = rl.step >= 3 ? 12.0f : 16.5f - 0.5f * static_cast<float>(rl.step);
            rl.Frame(ema, kBudget, true, false);
            if (rl.step > prev) ++ups;
            prev = rl.step;
        }
        char d[100];
        std::snprintf(d, sizeof d, "(%d steps up in 10 minutes, %d band returns, %d re-climbs)", ups, rl.bandReturns, rl.bandReclimbs);
        Check(rl.bandReclimbs >= 1 && ups <= 6, "relief: a scene that needs the step flaps at most a few times in 10 minutes (block, doubled)", d);
        Relief off; off.bandAt = 0.0f;
        int ups2 = 0;
        for (int i = 0; i < 900; ++i) off.Frame(30.0f, kBudget, true, false);
        prev = off.step;
        for (int i = 0; i < 36000; ++i) { off.Frame(off.step >= 3 ? 12.0f : 16.5f, kBudget, true, false); if (off.step > prev) ++ups2; prev = off.step; }
        Check(off.step == 3 && ups2 == 0 && off.bandReturns == 0, "relief: with the band off the same scene keeps all three steps for the 10 minutes (no return)", "");
    }
    {   // one step back at a time, at least 5 s apart (calm 300 frames), and a lone spike in the dead zone does not start the count again
        Relief rl = MakeRelief();
        for (int i = 0; i < 900; ++i) rl.Frame(30.0f, kBudget, true, false);
        int at[4] = {-1, -1, -1, -1}, prev = rl.step;
        for (int i = 0; i < 3000; ++i) {
            const float ema = (i % 150 == 149) ? 14.5f : 12.5f;   // (a spike every 2.5 s: wiped the count before the leak)
            rl.Frame(ema, kBudget, true, false);
            if (rl.step < prev) { at[rl.step] = i; prev = rl.step; }
        }
        char d[100];
        std::snprintf(d, sizeof d, "(steps back at frames %d %d %d)", at[2], at[1], at[0]);
        Check(at[0] > 0 && at[2] > 0 && at[1] - at[2] >= 300 && at[0] - at[1] >= 300, "relief: one step back at a time, 5 s or more apart, spikes in the dead zone do not restart the count", d);
    }
    {   // the band off is the old Relief: 0 differing decisions on 300 random traces of 6000 frames
        int diff = 0;
        for (int t = 0; t < 300; ++t) {
            Rng rng; rng.s = 1000u + static_cast<unsigned>(t);
            Relief n; n.bandAt = 0.0f;
            OldRelief o;
            float level = 14.0f;
            for (int i = 0; i < 6000; ++i) {
                if (rng.U() < 0.01f) level = 6.0f + 24.0f * rng.U();
                const float ema = level + (rng.U() - 0.5f) * 3.0f;
                const bool noRoom = rng.U() < 0.97f, holdUp = rng.U() < 0.05f;
                if (n.Frame(ema, kBudget, noRoom, holdUp) != o.Frame(ema, kBudget, noRoom, holdUp)) ++diff;
            }
        }
        char d[60];
        std::snprintf(d, sizeof d, "(%d differing)", diff);
        Check(diff == 0, "relief: band off = the old rule, 0 differing decisions in 300 random traces", d);
    }
    {   // the fill governor at the first pictures of play (6123.log:1847-1897): the cut-scene's frames cost 30-36 ms at the 30 budget; the level 256 stands; play frames cost 9 ms
        const float budget = 1001.0f / 60.0f;
        auto run = [&](bool forget, uint32_t skip, int* maxTarget, int* releaseAt) {
            vgx::FxGovernor g;
            g.on = true; g.absentHold = 60;
            g.target = g.level = 256;
            float ema = 35.6f;
            uint32_t seq = 5397, ignoreUntil = 0;
            if (forget) vgx::scenefresh::PlayBegins(true, ema, ignoreUntil, seq, skip);
            *maxTarget = g.target; *releaseAt = -1;
            // timed frames arrive two late: 35.6, 33.0 (the cut's own), then the play's 9.1 ms
            for (int i = 0; i < 400; ++i) {
                const float ms = i == 0 ? 35.6f : (i == 1 ? 33.0f : 9.1f + 0.2f * static_cast<float>(i % 5));
                ++seq;
                if (seq <= ignoreUntil) continue;
                ema = ema < 0 ? ms : 0.75f * ema + 0.25f * ms;
                g.Frame(ema, ms, budget, 1.1f, false);
                *maxTarget = std::max(*maxTarget, g.target);
                if (g.target == 0 && *releaseAt < 0) *releaseAt = i;
            }
        };
        int mt0, rl0, mt1, rl1, mt2, rl2;
        run(false, 2, &mt0, &rl0);
        run(g_break != 13, g_break == 14 ? 0u : 2u, &mt1, &rl1);
        run(true, 0, &mt2, &rl2);
        char d[140];
        std::snprintf(d, sizeof d, "(old: target up to %d, 0 at %d; forgotten + 2 skipped: up to %d, 0 at %d)", mt0, rl0, mt1, rl1);
        Check(mt0 == 512, "fill governor: on the old scene's mean it steps 256 -> 512 in play, as the log shows (the bug)", d);
        Check(mt1 == 256 && rl1 > 0 && rl1 < 250, "fill governor: with the mean forgotten and the 2 old frames skipped it never steps up and lets go within 250 frames", d);
        Check(mt2 == 512, "fill governor: forgetting the mean but reading the 2 old-scene frames seeds it with 35.6 ms and still steps up (why the 2 frames are skipped)", "");
        // PlayBegins: off does nothing; on sets -1 and moves the mark forward, never back
        float e = 12.0f; uint32_t ig = 50;
        Check(!vgx::scenefresh::PlayBegins(false, e, ig, 100, 2) && e == 12.0f && ig == 50, "scenefresh: off changes nothing");
        Check(vgx::scenefresh::PlayBegins(true, e, ig, 100, 2) && e < 0.0f && ig == 102, "scenefresh: on forgets the mean and ignores the next 2 timed frames");
        ig = 500; e = 5.0f; vgx::scenefresh::PlayBegins(true, e, ig, 100, 2);
        Check(ig == 500, "scenefresh: a later mark is never moved back");
    }
}

int Changes(const Result& r, int from, int to) {
    int n = 0;
    for (int f : r.changeAt) if (f >= from && f < to) ++n;
    return n;
}

void Scenes() {
    bool allReady[kN];
    for (int i = 0; i < kN; ++i) allReady[i] = i >= kNative;   // rungs from the game's own size down are made ahead; the ones above are not
    const int best = kNative, floorPlay = 9, floorMega = 11;
    // 1. open play, stationary, a load that fits at 0.93 (16 ms at the game's own size, 0.30 true share): converges, then holds still
    {
        Load ld{12.0f, 5.0f, One};    // 17 ms at the game's own size: over, 14.9 ms at 0.93 under, 13.9 at 0.78
        Result r = Run(ld, 7200, 6, best, floorPlay, allReady, 0.30f, MakeGov());
        char d[160];
        std::snprintf(d, sizeof d, "(level %d, %d down %d up, late %d of %d, changes after 5 s: %d)", r.finalLevel, r.downs, r.ups, r.late, r.frames, Changes(r, 300, 7200));
        Check(r.notReady == 0, "open play: no step to a rung that is not ready", d);
        Check(Changes(r, 300, 7200) <= 2, "open play: no flapping once settled (at most 2 changes in 2 minutes)", d);
        Check(r.late * 100 < r.frames * 5, "open play: under 5 % of the frames over the 60 budget", d);
    }
    // 2. load drifting +-10 % around the edge of a rung: the proof rule and the block keep it from flapping
    {
        Load ld{10.5f, 6.0f, Wobble};
        Result r = Run(ld, 7200, 6, best, floorPlay, allReady, 0.30f, MakeGov());
        char d[160];
        std::snprintf(d, sizeof d, "(%d changes in 2 minutes, %d taken back)", static_cast<int>(r.changeAt.size()), r.takebacks);
        Check(static_cast<int>(r.changeAt.size()) <= 8, "drifting load: at most 8 size changes in 2 minutes", d);
        Check(r.notReady == 0, "drifting load: no step to a rung that is not ready", d);
    }
    // 3. one spike of 3 frames at 2.2x (a late frame in a cluster): the picture steps down, then recovers to where it was
    {
        Load ld{9.5f, 4.0f, Spike};
        Result r = Run(ld, 2400, 6, best, floorPlay, allReady, 0.30f, MakeGov());
        char d[160];
        std::snprintf(d, sizeof d, "(%d down %d up, final level %d, late %d)", r.downs, r.ups, r.finalLevel, r.late);
        Check(r.finalLevel <= 6, "spike: back at the starting rung or better within 20 s of it", d);
        Check(r.downs <= 3, "spike: at most 3 steps down for 3 late frames", d);
        Check(r.takebacks <= 1, "spike: at most one failed step up", d);
    }
    // 4. a burst of 5 s at 1.6x: down fast (latency), up again after the proof, once
    {
        Load ld{9.0f, 6.0f, Burst};
        Result r = Run(ld, 3600, 6, best, floorPlay, allReady, 0.30f, MakeGov());
        int firstDown = -1;
        for (int f : r.changeAt) if (f >= 1200) { firstDown = f; break; }
        char d[160];
        std::snprintf(d, sizeof d, "(first step %d frames after the burst began, %d down %d up, final %d, late %d)", firstDown < 0 ? -1 : firstDown - 1200, r.downs, r.ups, r.finalLevel, r.late);
        Check(firstDown >= 0 && firstDown - 1200 <= 8, "burst: the first step down within 8 frames (133 ms) of the load", d);
        Check(r.finalLevel <= 7 && r.ups >= 1, "burst: back up after it ends", d);
        Check(r.takebacks <= 1, "burst: at most one failed step up", d);
    }
    // 5. a scene that gets heavier for good (a stadium with a heavy mist): sizes down in a few steps and stays; 0 changes afterwards
    {
        Load ld{9.0f, 6.0f, StepUp};
        Result r = Run(ld, 3600, 6, best, floorPlay, allReady, 0.30f, MakeGov());
        char d[160];
        std::snprintf(d, sizeof d, "(level %d, changes after 10 s of the step: %d, late %d)", r.finalLevel, Changes(r, 1200, 3600), r.late);
        Check(r.finalLevel > 6 && Changes(r, 1200, 3600) <= 1, "heavier scene: settles on a lower rung", d);
        Check(r.notReady == 0, "heavier scene: no step to a rung that is not ready", d);
    }
    // 6. items: a second of heavier frames every 7 s: no change per item
    {
        Load ld{9.5f, 4.5f, Items};
        Result r = Run(ld, 7200, 6, best, floorPlay, allReady, 0.30f, MakeGov());
        char d[160];
        std::snprintf(d, sizeof d, "(%d changes in 2 minutes, %d after the first 10 s, late %d of %d)", static_cast<int>(r.changeAt.size()), Changes(r, 600, 7200), r.late, r.frames);
        Check(Changes(r, 600, 7200) <= 8 && r.late * 100 < r.frames, "items: at most 8 size changes in 2 minutes after the first 10 s (17 items), under 1 % late", d);
        Check(r.notReady == 0, "items: no step to a rung that is not ready", d);
    }
    // 7. replay: heavy and drifting, with the replay's floor (0.62: level 10): never past it, never to a rung that is not ready
    {
        Load ld{13.0f, 6.0f, Replay};
        Result r = Run(ld, 3600, 8, best, 10, allReady, 0.30f, MakeGov());
        char d[160];
        std::snprintf(d, sizeof d, "(level %d, max %d, late %d of %d)", r.finalLevel, r.maxLevel, r.late, r.frames);
        Check(r.maxLevel <= 10 && r.notReady == 0, "replay: never below its floor, never to a rung not ready", d);
    }
    // 8. a rung that was not made ahead is skipped: with the 0.78 rung missing the governor goes 0.86 -> 0.70, and never asks for it
    {
        bool gap[kN];
        for (int i = 0; i < kN; ++i) gap[i] = allReady[i];
        gap[8] = false;
        Load ld{9.0f, 8.0f, StepUp};
        Gov mg = MakeGov();
        mg.fit = 0.95f;   // (one rung is enough for the step: with the 0.78 rung ready it would be the one chosen)
        Result r = Run(ld, 2400, 7, best, floorPlay, gap, 0.30f, mg);
        char d[160];
        std::snprintf(d, sizeof d, "(final %d, %d not ready)", r.finalLevel, r.notReady);
        Check(r.notReady == 0 && r.finalLevel != 8, "a rung that was not made ahead is jumped over", d);
    }
    // 9. nothing ready below: no step, no_room says so (the controller's relief and the steps after the floor are what is left)
    {
        bool none[kN];
        for (int i = 0; i < kN; ++i) none[i] = (i <= 7);
        Gov g = MakeGov();
        g.Start(7, best, floorPlay);
        bool room = true;
        int to = -1;
        for (int i = 0; i < 40; ++i) {
            auto rdy = [&](int l) { return none[l]; };
            to = g.Frame(22.0f, kBudget, Pix, rdy, false, false);
            if (to >= 0) break;
        }
        room = !g.no_room;
        Check(to < 0 && !room, "no ready rung below: no step, and no_room is set");
    }
    // 10. a model with a different pixel share (true 0.55, assumed 0.30): still no flapping, still no unready step
    {
        Load ld{6.0f, 9.0f, One};
        Result r = Run(ld, 7200, 6, best, floorPlay, allReady, 0.55f, MakeGov());
        char d[160];
        std::snprintf(d, sizeof d, "(final %d, changes after 5 s: %d, late %d of %d)", r.finalLevel, Changes(r, 300, 7200), r.late, r.frames);
        Check(Changes(r, 300, 7200) <= 4 && r.notReady == 0, "wrong pixel share: at most 4 changes after settling", d);
    }
    // 11. a failed up step: the load sits just over the next rung's proof; it is tried, taken back, blocked for 2 s, then 4 s ...: at most 4 tries a minute
    {
        // 15.2 ms at 0.93 projects to 14.4 at 0.93 -> ok to try, but at the true 0.5 share costs more than modelled: use a load with a real share of 0.6
        Load ld{6.4f, 8.8f, One};
        Result r = Run(ld, 7200, 7, best, floorPlay, allReady, 0.60f, MakeGov(), 3);
        char d[160];
        std::snprintf(d, sizeof d, "(%d up %d down %d taken back in 2 minutes)", r.ups, r.downs, r.takebacks);
        Check(r.ups <= 6, "a rung that does not hold: at most 6 tries in 2 minutes (the block doubles)", d);
    }
    // 11b. a load that is calm for 3 s and heavy for 3 s: the up step the calm tempts is taken back (probation), its retry is blocked and the proof lengthens
    {
        Load ld{8.0f, 7.0f, Square};
        Result r = Run(ld, 7200, 8, best, floorPlay, allReady, 0.30f, MakeGov());
        char d[160];
        std::snprintf(d, sizeof d, "(%d up %d down %d taken back, late %d of %d)", r.ups, r.downs, r.takebacks, r.late, r.frames);
        Check(r.takebacks >= 1 && r.ups <= 8 && r.notReady == 0, "a calm that does not last: the failed up step is taken back and the next tries are blocked", d);
    }
    // 12. proof: 149 frames of room do not climb, 150 do (stationary 9.5 ms at 0.78, next rung 0.86 projects to 11 ms)
    {
        Gov g = MakeGov();
        g.Start(8, best, floorPlay);
        g.since = 1000;
        int to = -1, at = -1;
        for (int i = 0; i < 200 && to < 0; ++i) { to = g.Frame(9.5f, kBudget, Pix, [&](int l) { return allReady[l]; }, false, false); at = i + 1; }
        Check(to == 7 && at == g.proofFrames, "an up step needs the whole proof (150 frames)", "");
    }
    // 13. int33: PROBEAT=1.06 / LATEAT=1.08 lets a 13.8 ms load with isolated 16.4-16.8 ms blips climb and hold without probation take-back, while sustained 17.2 ms or 18.5 ms spikes still step down
    {
        Gov g = MakeGov();
        g.upFit = 0.88f; g.upHard = 0.97f; g.proofFrames = 90; g.probeAt = 1.06f; g.lateAt = 1.08f;
        g.Start(5, best, floorPlay);
        int to = -1;
        for (int i = 0; i < 120 && to < 0; ++i) to = g.Frame(13.0f, kBudget, Pix, [&](int l) { return allReady[l]; }, false, false);
        const bool climbed = (to == 4);
        if (climbed) g.Changed(to, true);
        int blipAct = -1;
        for (int i = 0; i < 30; ++i) {
            const float ms = (i == 10) ? 16.4f : (i == 22 ? 16.8f : 13.6f);
            const int r = g.Frame(ms, kBudget, Pix, [&](int l) { return allReady[l]; }, false, false);
            if (r >= 0 && blipAct < 0) blipAct = r;
        }
        int sustAct = -1;
        for (int i = 0; i < 30 && sustAct < 0; ++i) sustAct = g.Frame(17.2f, kBudget, Pix, [&](int l) { return allReady[l]; }, false, false);
        Check(climbed && blipAct < 0 && sustAct > 4 && g.takebacks >= 1, "int33 probeAt/lateAt: isolated 16.4-16.8 ms blips keep climb, sustained 17.2 ms still fails probation", "");
    }
    (void)floorMega;
}

void Pieces() {
    // start level: clamped, and the nearest ready rung
    {
        bool ready[kN];
        for (int i = 0; i < kN; ++i) ready[i] = i >= kNative && i != 6;
        auto rdy = [&](int l) { return ready[l]; };
        Check(StartLevel(6, 3, 9, rdy) == 7, "start level: the preset's rung is not ready, the next one down is used");
        Check(StartLevel(1, 3, 9, rdy) == 3, "start level: above the best rung clamps to it");
        Check(StartLevel(20, 3, 9, rdy) == 9, "start level: below the floor clamps to it");
        bool none[kN] = {};
        Check(StartLevel(6, 3, 9, [&](int l) { return none[l]; }) == -1, "start level: nothing ready gives -1");
    }
    // the plan of rungs to make ahead: play's start down to the deepest rung first, then rungs above while the memory allows
    {
        int out[16];
        const int n = TourOrder(6, 3, 12, 100000, [](int l) { return static_cast<long>(2300 * Pix(l)); }, out, 16);
        bool order = n == 10 && out[0] == 6 && out[6] == 12 && out[7] == 5 && out[9] == 3;
        char d[120];
        std::snprintf(d, sizeof d, "(%d rungs: %d %d ... %d | %d %d %d)", n, out[0], out[1], out[6], out[7], out[8], out[9]);
        Check(order, "plan: every rung from the start to the deepest, then the better ones", d);
        const int m = TourOrder(6, 3, 12, 9000, [](int l) { return static_cast<long>(2300 * Pix(l)); }, out, 16);
        long used = 0;
        for (int i = 0; i < m; ++i) used += static_cast<long>(2300 * Pix(out[i]));
        bool deepFirst = m >= 5;
        for (int i = 0; i < m; ++i) if (out[i] < 6 && i < 3) deepFirst = false;
        Check(used <= 9000 && deepFirst, "plan: a memory budget cuts the better rungs first", "");
    }
    // the scene kinds and presets
    {
        Check(KindOf(1, false, false) == kPlay && KindOf(1, false, true) == kHeavy && KindOf(2, false, false) == kReplay && KindOf(3, false, false) == kCut && KindOf(0, false, false) == kMenu &&
                  KindOf(1, true, false) == kMega,
              "scene kinds from the scene class, the Mega Strike and the heavy stadium");
        bool ok = true;
        for (int k = 0; k < kSceneKinds; ++k) { const Preset p = PresetFor(k); ok = ok && p.start >= p.floor && p.start <= 1.0f && p.floor >= 0.5f; }
        Check(ok, "presets: start at or above the floor, between 0.5 and 1.0");
        float sc[kN];
        for (int i = 0; i < kN; ++i) sc[i] = kLv[i].s;
        Check(LevelFor(sc, kN, kNative, 0.93f) == 6 && LevelFor(sc, kN, kNative, 0.70f) == 9 && LevelFor(sc, kN, kNative, 0.56f) == 11, "level for a scale: 0.93 -> 6, 0.70 -> 9, 0.56 -> 11");
    }
    // the steps after the floor: one step per 30 frames of overload, never two within 120 frames, one back after 300 calm frames
    {
        Relief rl;
        if (g_break == 4) { rl.hold = 0; rl.stepEvery = 1; }
        int step = 0, firstAt[4] = {-1, -1, -1, -1}, f = 0;
        for (; f < 900; ++f) {
            const int r = rl.Frame(17.5f, kBudget, true, false);
            if (r >= 0) { step = r; if (r < 4 && firstAt[r] < 0) firstAt[r] = f; }
        }
        char d[120];
        std::snprintf(d, sizeof d, "(steps at frames %d %d %d, final %d)", firstAt[1], firstAt[2], firstAt[3], step);
        Check(step == 3 && firstAt[2] - firstAt[1] >= 120 && firstAt[3] - firstAt[2] >= 120, "relief: three steps, at least 120 frames apart, under sustained overload", d);
        int back = 0;
        for (int i = 0; i < 1200; ++i) { const int r = rl.Frame(10.0f, kBudget, true, false); if (r >= 0) back = r; }
        Check(back == 0, "relief: all steps back after calm", "");
        Relief held;
        int steps = 0;
        for (int i = 0; i < 600; ++i) if (held.Frame(17.5f, kBudget, true, true) >= 0) ++steps;
        Check(steps == 0, "relief: none while the effects governor still has a step", "");
        Relief roomy;
        for (int i = 0; i < 600; ++i) if (roomy.Frame(17.5f, kBudget, false, false) >= 0) ++steps;
        Check(steps == 0, "relief: none while a rung is left below", "");
    }
}

// nxr6a (NEON round 6 #1): the frame order of a scene reset. The GPU timers come late by the queue depth: the swap of frame k waits for frame k-L, so at the call of frame k (DynamicResolution: reset first, then the read of
// the newest timed frame) the newest finished frame is k-1-L. Play begins at frame N; frames before it cost 33 ms (a walk-out), from it 9 ms. Count the old frames that a reader TAKES as a sample after the reset:
// the renderer (d.ema / g_gpuMinMs / the governor: one read of the newest per call, `seq > ignoreUntil`) and the host's queue judge (one swap later, DropSample). The real headers run the model.
void Nxr6a() {
    constexpr int N = 100;
    auto renderer = [&](int L, uint32_t skip, bool emaOn, bool gpuReset, int* firstFresh) {
        uint32_t seqSeen = 0, ignoreUntil = 0;
        int leaks = 0; *firstFresh = -1;
        for (int k = 1; k < N + 40; ++k) {
            if (k == N) {
                float ema = 33.0f;
                vgx::scenefresh::PlayBegins(emaOn, ema, ignoreUntil, seqSeen, skip);
                vgx::scenefresh::DiscardInFlight(gpuReset && !emaOn, ignoreUntil, seqSeen, skip);
            }
            const int c = k - 1 - L;
            if (c < 1 || static_cast<uint32_t>(c) == seqSeen) continue;
            seqSeen = static_cast<uint32_t>(c);
            if (seqSeen > ignoreUntil) {
                if (c < N) { if (k >= N) ++leaks; }
                else if (*firstFresh < 0) *firstFresh = c;
            }
        }
        return leaks;
    };
    auto host = [&](int L, uint32_t skip) {
        uint32_t last = 0, left = 0; int leaks = 0;
        for (int k = 1; k < N + 40; ++k) {   // (the swap of frame k: newest finished k-L; the epoch bumped at the call of frame N, seen at the swap of frame N)
            if (k == N) left = skip;
            const int c = k - L;
            if (c < 1 || static_cast<uint32_t>(c) == last) continue;
            last = static_cast<uint32_t>(c);
            if (vgx::scenefresh::DropSample(left)) continue;
            if (k >= N && c < N) ++leaks;
        }
        return leaks;
    };
    const uint32_t nowSkip = g_break == 15 ? 2u : vgx::scenefresh::kDefaultSkip;
    for (int L = 1; L <= 3; ++L) {
        int ff;
        const int leaks = renderer(L, nowSkip, true, true, &ff);
        char d[100]; std::snprintf(d, sizeof d, "(depth %d: %d old frames read, first play frame read is %d)", L, leaks, ff);
        if (L <= 2) Check(leaks == 0 && ff >= N && ff <= N + 1, "scene reset: no walk-out frame in flight is read after the reset (renderer, queue depth 1 and 2), the first read is the first play frame or the next (depth 1 over-skips by one)", d);
    }
    { int ff; const int l = renderer(2, 2, true, true, &ff); char d[80]; std::snprintf(d, sizeof d, "(%d)", l);
      Check(l == 1, "scene reset: the nxr4a skip of 2 let one walk-out frame through at queue depth 2 (the bug NEON found)", d); }
    for (int L = 1; L <= 3; ++L) {
        char d[60]; const int l = host(L, g_break == 17 ? 0u : nowSkip); std::snprintf(d, sizeof d, "(depth %d: %d)", L, l);
        if (L <= 2) Check(l == 0, "scene reset: the host's queue judge drops the old scene's frames after the ring reset", d);
    }
    { char d[40]; const int l = host(2, 0); std::snprintf(d, sizeof d, "(%d)", l); Check(l >= 2, "scene reset: without the drop the host's ring takes the walk-out frames back (the bug)", d); }
    {   // MSC_SCENE_GPU_RESET=1 with MSC_SCENE_EMA_RESET=0: the skip stands alone
        int ff; const int l = renderer(2, nowSkip, false, g_break == 16 ? false : true, &ff);
        char d[60]; std::snprintf(d, sizeof d, "(%d)", l);
        Check(l == 0, "scene reset: GPU reset alone (EMA reset off) also discards the frames in flight", d);
        int ff2; const int off = renderer(2, nowSkip, false, false, &ff2);
        Check(off >= 1, "scene reset: both switches off reads them (the old behaviour is kept for the A/B)", "");
    }
    { uint32_t ig = 7; Check(!vgx::scenefresh::DiscardInFlight(true, ig, 100, 0) && ig == 7, "scenefresh: a skip of 0 does nothing", "");
      Check(vgx::scenefresh::SkipFrom(-1) == 3 && vgx::scenefresh::SkipFrom(0) == 0 && vgx::scenefresh::SkipFrom(99) == 8, "scenefresh: MSC_SCENE_SKIP unset 3, 0 none, capped at 8", ""); }
}
} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (!std::strncmp(argv[i], "--break=", 8)) g_break = std::atoi(argv[i] + 8);
    Scenes();
    Pieces();
    Flip25();
    Nxr3a();
    Nxr4a();
    Nxr6a();
    std::printf("%d passed, %d failed%s\n", g_pass, g_fail, g_break ? " (deliberate break)" : "");
    return g_fail ? 1 : 0;
}
