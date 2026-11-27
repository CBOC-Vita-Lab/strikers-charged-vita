// pacing13: the pair-rate controller (np_pair_rate.h) against a cost model, five-minute matches. NP_PAIR_BREAK 1-5 must fail.
// Model: even 30: a picture every 33.4 ms (phase 0, shown 2 refreshes). Pair: a full picture (game thread = what the even 30 costs now) and a light one (the pair's true extra) per 33.3 ms; when the
// pair's cost (game thread or GPU) is over 33.3 ms the pair takes three refreshes (one picture held two). Noise 1.5 ms, optional hitches.
#include "np_pair_rate.h"
#include <cmath>
#include <cstdio>
#include <functional>
#include <random>
using namespace npfps;
static int g_fail = 0;
static void check(bool ok, const char* w) { if (!ok) { std::printf("FAIL: %s\n", w); ++g_fail; } }
struct Out { int entries, leaves, failed, switches, midPair; double pairShare; bool pairAtEnd; double extraGame; };
struct Costs { std::function<double(double)> game30, gpu30; double extraGameTrue, extraGpuTrue; };
static Out run(const Costs& c, double seconds, PairConfig cfg = PairConfig(), double hitchEvery = 0, double* firstEntry = nullptr, double extraFrom = -1, double extraGameTrue2 = 0) {
    PairRate r(cfg);
    std::mt19937 rng(11);
    std::normal_distribution<double> noise(0.0, 1.5);
    double t = 0, onTime = 0;
    bool pair = false;
    int phase = 0, sw = 0, mid = 0, lastPhase = 0, n = 0;
    Out o{};
    bool newFull = true;
    while (t < seconds) {
        PairPicture p; p.now = t; p.live = true;
        const double g30 = c.game30(t) + noise(rng), u30 = c.gpu30(t) + noise(rng) * 0.5;
        const double extra = (extraFrom >= 0 && t >= extraFrom) ? extraGameTrue2 : c.extraGameTrue;
        double dt;
        if (!pair) {
            p.phase = 0; p.gameMs = g30; p.gpuMs = u30; p.heldRefreshes = 2; dt = 33.4;
            phase = 0;
        } else {
            const double pairG = g30 + extra + noise(rng), pairU = u30 + c.extraGpuTrue;
            const double cost = pairG > pairU ? pairG : pairU;
            const bool full = newFull;
            p.phase = full ? 1 : 2;
            p.gameMs = full ? g30 : extra + noise(rng);
            p.gpuMs = full ? u30 : c.extraGpuTrue;
            const bool late = cost > 33.3;
            p.heldRefreshes = (late && !full) ? 2 : 1;
            dt = (late && !full) ? 33.4 : 16.7;
            newFull = !full;
            phase = p.phase;
            onTime += dt;
        }
        if (hitchEvery > 0 && ++n % (int)hitchEvery == 0) { p.gameMs += 400.0; dt += 400.0; p.heldRefreshes = 6; }
        const bool want = r.Picture(p);
        lastPhase = phase;
        if (want != pair) {
            ++sw;
            if (!want && lastPhase == 1) ++mid;   // a pair cut after its full picture
            if (want && !pair && firstEntry && *firstEntry < 0) *firstEntry = t;
            pair = want; newFull = true;
        }
        t += dt / 1000.0;
    }
    o.entries = r.Entries(); o.leaves = r.Leaves(); o.failed = r.FailedTries(); o.switches = sw; o.midPair = mid; o.pairShare = onTime / (seconds * 1000.0); o.pairAtEnd = pair; o.extraGame = r.ExtraGame();
    return o;
}
int main() {
    auto flat = [](double v) { return [v](double) { return v; }; };
    // 1. a scene that fits (game 21 ms at the even 30, light picture adds 8.5): the pair is entered once and kept
    { Costs c{flat(20.0), flat(14.0), 8.5, 9.0}; Out o = run(c, 300);
      std::printf("fits (20 + 8.5): %d entries, %d leaves, pair %.0f %% of the time\n", o.entries, o.leaves, o.pairShare * 100); 
      check(o.entries == 1 && o.leaves == 0 && o.pairShare > 0.95, "a scene that fits: one entry, no leave, the pair 95 %+ of a five-minute match");
      check(o.midPair == 0, "no switch inside a pair"); }
    // 2. a scene that does not fit (game 25 ms: 25 + 8.5 = 33.5 against 30.5): never enters
    { Costs c{flat(25.0), flat(14.0), 8.5, 9.0}; Out o = run(c, 300);
      check(o.entries == 0 && o.switches == 0, "a scene that does not fit stays at the even 30 without a single try"); }
    // 3. GPU-bound: the GPU at the even 30 is 24 ms, the pair adds 10: 34 against 30.5: never enters
    { Costs c{flat(18.0), flat(24.0), 8.5, 10.0}; Out o = run(c, 300);
      check(o.entries == 0, "a GPU the pair does not fit never tries"); }
    // 4. a cost that wanders across the limits (22 +- 4 over 40 s): a handful of switches
    { Costs c{[](double t) { return 21.0 + 4.0 * std::sin(t * 6.2831853 / 40.0); }, flat(14.0), 8.5, 9.0}; Out o = run(c, 300);
      std::printf("wandering 21 +- 4: %d switches, %d failed tries, pair %.0f %%\n", o.switches, o.failed, o.pairShare * 100);
      check(o.switches <= 16 && o.failed <= 3, "a cost across both limits: at most one in and one out per 40 s cycle, at most 3 failed tries");
      check(o.midPair == 0, "no switch inside a pair (wandering)"); }
    // 5. wrong guess: the light picture really adds 14 ms, the first guess is 8.5: one failed try, the learned extra stops the loop
    { Costs c{flat(20.0), flat(14.0), 14.0, 9.0}; Out o = run(c, 300);
      std::printf("light picture really costs 14 (guess 8.5): %d entries, %d failed tries, learned extra %.1f ms\n", o.entries, o.failed, o.extraGame);
      check(o.entries <= 3 && o.failed <= 2, "a wrong guess costs at most two failed tries, not a loop");
      check(o.extraGame > 11.0, "the extra cost is learned from the pair"); }
    // 6. the other wrong guess: the light picture really adds 5: the pair stays
    { Costs c{flat(17.0), flat(14.0), 5.0, 9.0}; PairConfig cfg; cfg.extraGameInit = 12.0; Out o = run(c, 300, cfg);
      check(o.pairAtEnd && o.entries <= 2, "a guess that was too high is learned down: the pair is entered after the first pairs and kept"); }
    // 7. a step in the cost: 21 for 100 s, then 27: out once (the even 30 holds), no way back
    { Costs c{[](double t) { return t < 100 ? 21.0 : 27.0; }, flat(14.0), 8.5, 9.0}; Out o = run(c, 300);
      check(o.entries == 1 && !o.pairAtEnd && o.leaves == 1, "in at 21, out for good at 27: two switches"); }
    // 8. fast recovery: 27 for 60 s then 21: back in within 10 s of the cost fitting
    { double firstIn = -1; Costs c{[](double t) { return t < 60 ? 27.0 : 21.0; }, flat(14.0), 8.5, 9.0}; Out o = run(c, 120, PairConfig(), 0, &firstIn);
      check(o.entries == 1 && firstIn > 59.0 && firstIn < 70.0, "the cost falls to 21 at 60 s: the pair is back within 10 s (no long timer)"); }
    // 9. hitches of 100 ms with a 90 ms cost every 3 s in the pair do not close it
    { Costs c{flat(20.0), flat(14.0), 8.5, 9.0}; Out o = run(c, 300, PairConfig(), 150);
      std::printf("hitch: %d entries %d leaves pair %.2f\n", o.entries, o.leaves, o.pairShare); check(o.entries >= 1 && o.leaves == 0 && o.pairShare > 0.8, "a hitch every 3 s does not close the pair"); }
    // 9b. a scene that fits for 8 s, then does not for 8 s, again and again: the failed tries thin out (the cool-down doubles)
    { Costs c{[](double t) { return std::fmod(t, 16.0) < 8.0 ? 20.0 : 28.0; }, flat(14.0), 8.5, 9.0}; PairConfig cfg; cfg.marginStep = 0; Out o = run(c, 300, cfg);
      std::printf("square wave 20/28: %d entries, %d failed tries\n", o.entries, o.failed);
      check(o.failed <= 10, "a scene that keeps failing: at most 10 failed tries in five minutes"); }
    // 10. play ends / the background has no room / overload: out at once, at a boundary, not blamed
    { PairRate r; PairPicture p; p.live = true; double t = 0;
      for (int i = 0; i < 400; ++i) { p.now = t; p.phase = 0; p.gameMs = 20; p.gpuMs = 12; r.Picture(p); t += 0.0334; }
      check(r.Wanted(), "in after the hold at 20 ms");
      for (int i = 0; i < 20; ++i) { p.now = t; p.phase = (i % 2) ? 2 : 1; p.gameMs = 10; p.gpuMs = 8; p.heldRefreshes = 1; r.Picture(p); t += 0.0167; }
      p.phase = 1; p.live = false; p.now = t; r.Picture(p);
      check(r.Wanted(), "play ends after a full picture: the light picture of the pair is still drawn");
      p.phase = 2; r.Picture(p);
      check(!r.Wanted() && r.FailedTries() == 0, "after the light picture the pair is over, and it is not a failed try"); }
    // 11. lessons17 fast leave (MSC_PAIR_FAST_LEAVE): 3 bad pairs among 16 leave a pair whose 30-pair mean is fine; 2 do not; 3 bad pairs 20 pairs ago do not
    {   auto feed = [](PairRate& r, double& t, int pairs, const std::function<double(int)>& cost) {
            PairPicture p; p.live = true;
            for (int i = 0; i < pairs; ++i) {
                p.now = t; p.phase = 1; p.gameMs = 20; p.gpuMs = 0; p.heldRefreshes = 1; r.Picture(p); t += 0.0167;
                p.now = t; p.phase = 2; p.gameMs = cost(i) - 20; p.gpuMs = 0; p.heldRefreshes = 1; r.Picture(p); t += 0.0167;
            }
        };
        auto enter = [](PairRate& r, double& t) { PairPicture p; p.live = true; for (int i = 0; i < 400 && !r.Wanted(); ++i) { p.now = t; p.phase = 0; p.gameMs = 20; p.gpuMs = 0; p.heldRefreshes = 2; r.Picture(p); t += 0.0334; } };
        PairConfig cfg; cfg.fastN = 3;
        { PairRate r(cfg); double t = 0; enter(r, t); check(r.Wanted(), "fast: in");
          feed(r, t, 40, [](int) { return 28.0; });
          check(r.Wanted(), "fast: a steady 28 ms pair is kept");
          feed(r, t, 16, [](int i) { return (i == 2 || i == 7 || i == 12) ? 40.0 : 28.0; });
          check(!r.Wanted() && r.Leaves() == 1, "fast: 3 of 16 pairs at 40 ms leave the pair although the 30-pair mean is 29.5 ms"); }
        { PairRate r(cfg); double t = 0; enter(r, t);
          feed(r, t, 40, [](int i) { return (i == 5 || i == 20) ? 40.0 : 28.0; });
          check(r.Wanted(), "fast: 2 bad pairs among 40 are kept"); }
        { PairRate r(cfg); double t = 0; enter(r, t);
          feed(r, t, 40, [](int i) { return (i == 3 || i == 6 || i == 9) ? 40.0 : 28.0; });
          check(!r.Wanted(), "fast: three bad pairs close together leave"); }
        // bad pairs 12 apart: no 16 pairs hold three of them (the 30 remembered pairs do: mutant 7)
        { PairRate r(cfg); double t = 0; enter(r, t);
          feed(r, t, 40, [](int i) { return (i == 2 || i == 14 || i == 26) ? 40.0 : 28.0; });
          check(r.Wanted(), "fast: three bad pairs 12 apart are not three of the last 16"); }
    }
    std::printf("%s: pair_rate_unit\n", g_fail ? "FAILED" : "passed");
    return g_fail ? 1 : 0;
}
