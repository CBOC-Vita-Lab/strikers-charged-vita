// Wave 10 (pacing11): the free-run gate judges the cost AT THE FREE-RUN RATE (np_fps_ctl.h FreeRateCostMs / Prediction::maxFree), not the cost measured at a held 30.
// Console figures (r7/r9, Vice, PERFORMANCE mode): game thread 21.7 ms a picture at 30 (1.67 steps a picture), 18.4 free-running; the gate's 20 ms limit never opened on 21.7.
// NP_FREECOST_BREAK 1-3 must fail. g++ -std=c++11 -I native/platform/compat native/tools/fps/free_cost_unit.cpp
#include "np_fps_ctl.h"
#include "np_fps_between.h"
#include <cmath>
#include <cstdio>
#include <cstring>
using namespace npfps;
static int g_fail = 0;
static void check(bool ok, const char* w) { if (!ok) { std::printf("FAIL: %s\n", w); ++g_fail; } }

// One held-30 frame of the game core: perPic ms of per-picture work plus `steps` steps of stepMs.
static FrameIn Frame30(double now, double perPic, double stepMs, int steps, unsigned scene) {
    FrameIn f;
    memset(&f, 0, sizeof f);
    f.now = now; f.interval = 2 * 0.016683; f.steps = steps; f.simMs = stepMs * steps; f.costsValid = true; f.gameCore = 0; f.scene = scene;
    f.cpuMs[0] = perPic + f.simMs; f.cpuMs[1] = 9.0; f.cpuMs[2] = 6.0;
    return f;
}

struct Run { double now = 0; double maxFree = 0, maxNow = 0, meanNow = 0; };
static BetweenConfig Plan() { BetweenConfig c; c.enterMs = 20.0; c.leaveMs = 25.0; c.cool = 45.0; c.coolMax = 240.0; c.fail = 90.0; c.outFrames = 60; return c; }   // (the Vita plan's)
static Run Feed(Auto& a, Between& b, double perPic, double stepMs, double seconds, bool useFree, int* opened, unsigned scene = 5) {
    Run r;
    int n = 0, cnt = 0;
    double sumNow = 0;
    double t = 0;
    while (t < seconds) {
        const int steps = (n++ % 3 == 2) ? 1 : 2;   // 1.67 steps a picture
        const FrameIn f = Frame30(t, perPic, stepMs, steps, scene);
        a.Frame(f);
        const Prediction& p = a.LastPrediction();
        r.maxFree = p.maxFree; r.maxNow = p.maxNow;
        if (t > 1.0) { sumNow += p.maxNow; ++cnt; }
        const double cost = useFree ? p.maxFree : p.maxNow;
        if (b.Frame(t, scene, true, cost, b.On() ? 20.0 : 33.4) && opened && !*opened) *opened = 1;
        t += 0.0334;
    }
    r.meanNow = cnt ? sumNow / cnt : 0;
    return r;
}

int main() {
    // 1. the console's Vice case: 13.4 ms a picture + 5 ms a step: 21.7 at 30 (1.67 steps), 18.4 at one step a picture
    {
        Auto a; Between b(Plan()); int opened = 0;
        const Run r = Feed(a, b, 13.4, 5.0, 4.0, true, &opened);
        std::printf("Vice: measured at 30 %.1f ms, at the free-run rate %.1f ms\n", r.meanNow, r.maxFree);
        check(std::fabs(r.meanNow - 21.75) < 0.5, "measured at 30: 21.7 ms (13.4 + 1.67 x 5)");
        check(std::fabs(r.maxFree - 18.4) < 0.6, "at the free-run rate about 18 ms (13.4 + one step of 5 = 18.4; the console measured 18.4)");
        check(opened == 1 && b.On(), "the gate opens on it (limit 20 ms for a second) once the sticky settings' hold time has passed");
    }
    // 2. judged by the measured cost it never opens (what r9 did)
    {
        Auto a; Between b(Plan()); int opened = 0;
        Feed(a, b, 13.4, 5.0, 8.0, false, &opened);
        BetweenConfig c; (void)c;
        check(opened == 0, "(the wave 9 rule: 21.7 against its 20 ms limit never opens)");
    }
    // 3. a heavier scene (the old defaults at Vice: 21-22 ms free): not free, a clean 30
    {
        Auto a; Between b(Plan()); int opened = 0;
        const Run r = Feed(a, b, 16.5, 5.0, 8.0, true, &opened);
        std::printf("heavier: free-run cost %.1f ms\n", r.maxFree);
        check(r.maxFree > 21.0 && opened == 0, "a scene that costs 21.5 ms or more even free (over the 20 ms limit) stays at the clean 30");
    }
    // 4. the picture period never prices under one step or over two
    {
        check(std::fabs(FreeRateCostMs(10.0, 5.0, 20.0, 15.0, 1) - 15.0) < 0.1, "10 ms + 5 ms steps: one step a picture at least, 15 ms");
        check(std::fabs(FreeRateCostMs(16.0, 5.0, 20.0, 24.0, 2) - 21.33) < 0.1, "16 ms + 5 ms steps: 21.3 ms a picture (P = 16 / (1 - 5/20))");
        check(FreeRateCostMs(10.0, 0.0, 20.0, 10.0, 1) == 10.0, "no step cost: the per-picture part");
        check(FreeRateCostMs(10.0, 19.0, 20.0, 40.0, 2) <= 10.0 + 19.0 * 2.0 + 1e-9, "a step that takes nearly the whole step period stays bounded");
    }
    std::printf("%s: free_cost_unit\n", g_fail ? "FAILED" : "passed");
    return g_fail ? 1 : 0;
}
