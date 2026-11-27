// pace18: the pacing cost model with the step on its own thread (platform/compat/np_simpace18.h, used by np_fps_ctl.h Auto::Predict and np_fps.cpp's game clock).
//   clang++ -std=c++11 -O1 -Iplatform/compat tools/fps/simpace18_unit.cpp -o /tmp/simpace18_unit && /tmp/simpace18_unit
// 1. MSC_SIMT=0 and MSC_SIMT=1 (np_sim on the game core) give EXACTLY the old numbers: a copy of the old Predict / game-clock expressions (OldPredict below, taken from integ/r16 3522c20e)
//    against Auto::Predict, bit for bit (memcmp of the doubles), 300000 random frames, costs valid and not.
// 2. MSC_SIMT=2: window and step cost max(window, step) + overhead, never their sum; the free-run solution is a fixed point; the game clock's cost of a picture; the controller goes to 60
//    on a load where the sum would refuse it (and the same load with the step on the game thread, mode 0, stays at 30).
// NP_SIMPACE_BREAK=1..5 (compile with -DNP_SIMPACE_BREAK=n) must each make a check FAIL.
#include "np_fps_ctl.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
using namespace npfps;
static int g_fail = 0;
static void check(bool ok, const char* w) { if (!ok) { std::printf("FAIL: %s\n", w); ++g_fail; } }

static unsigned s_seed = 12345;
static double rnd(double lo, double hi) { s_seed = s_seed * 1664525u + 1013904223u; return lo + (hi - lo) * ((s_seed >> 8) / 16777216.0); }

// ---- the old code, verbatim in its arithmetic (integ/r16 np_fps_ctl.h Auto::Predict and np_fps.cpp's clock line) ----
struct OldOut { bool valid; double max60, maxFree, maxNow; int core; };
static OldOut OldPredict(const Config& cfg_, double& stepCost_, const FrameIn& f) {
    OldOut p; p.valid = false; p.max60 = 0; p.maxFree = 0; p.maxNow = 0; p.core = 0;
    const double R = cfg_.refresh;
    const double stepsPerRefresh = ceil(R / cfg_.stepSeconds - 1e-9);
    if (f.steps > 0 && f.simMs > 0) {
        const double c = f.simMs / f.steps;
        stepCost_ = stepCost_ > 0 ? 0.8 * stepCost_ + 0.2 * c : c;
    }
    if (f.costsValid) {
        p.valid = true;
        for (int c = 0; c < kMaxCores; ++c) {
            double now = f.cpuMs[c];
            double pred = now - f.timeMs[c] + f.timeMs[c] * (f.interval > 0 ? (R / f.interval) : 1.0);
            if (c == f.gameCore) {
                pred = pred - f.simMs + stepCost_ * stepsPerRefresh;
                if (pred < 0) pred = 0;
            }
            {
                double perPic = now - f.timeMs[c] + f.timeMs[c] * (f.interval > 0 ? (R / f.interval) : 1.0);
                double fr = perPic;
                if (c == f.gameCore) {
                    perPic -= f.simMs;
                    if (perPic < 0) perPic = 0;
                    fr = stepCost_ > 0 ? FreeRateCostMs(perPic, stepCost_, cfg_.stepSeconds * 1000.0, now, f.steps) : pred;
                }
                if (fr > p.maxFree) p.maxFree = fr;
            }
            if (pred > p.max60) { p.max60 = pred; p.core = c; }
            if (now > p.maxNow) p.maxNow = now;
        }
    } else {
        p.valid = f.gameBusyMs > 0;
        p.maxNow = f.gameBusyMs;
        double pred = f.gameBusyMs - f.simMs + stepCost_ * stepsPerRefresh;
        p.max60 = pred < 0 ? 0 : pred;
        { double pp = f.gameBusyMs - f.simMs; if (pp < 0) pp = 0; p.maxFree = stepCost_ > 0 ? FreeRateCostMs(pp, stepCost_, cfg_.stepSeconds * 1000.0, f.gameBusyMs, f.steps) : p.max60; }
        p.core = f.gameCore;
    }
    return p;
}
static bool same(double a, double b) { return std::memcmp(&a, &b, sizeof a) == 0; }

static FrameIn Random(int mode) {
    FrameIn f;
    std::memset(&f, 0, sizeof f);
    f.now = 0; f.interval = rnd(0.014, 0.075); f.steps = (int)rnd(0, 4.99); f.simMs = f.steps ? rnd(2, 14) * f.steps : 0; f.costsValid = rnd(0, 1) > 0.25;
    f.gameCore = (int)rnd(0, 2.99);
    for (int c = 0; c < kMaxCores; ++c) { f.cpuMs[c] = rnd(0, 30); f.timeMs[c] = rnd(0, 1) > 0.5 ? rnd(0, 6) : 0; }
    f.gameBusyMs = rnd(0, 40);
    f.simMode = mode; f.simOnGameCore = true;   // (mode 1 with np_sim on the game core)
    // the sim fields are present and noisy in mode 1: they must not matter there
    f.windowMs = mode ? rnd(0, 5) : 0; f.joinMs = mode ? rnd(0, 9) : 0; f.simWallMs = mode ? rnd(0, 12) : 0; f.simCore = mode ? 2 : -1;
    return f;
}

int main() {
    // ---- 1. modes 0 and 1 (np_sim on the game core): the old numbers, bit for bit ----
    for (int mode = 0; mode <= 1; ++mode) {
        Config cfg;
        Auto ctl(cfg);
        double stepCost = 0.0;
        int bad = 0, n = 0;
        for (int i = 0; i < 150000; ++i) {
            FrameIn f = Random(mode);
            const OldOut o = OldPredict(cfg, stepCost, f);
            const Prediction p = ctl.Predict(f);
            ++n;
            if (!(p.valid == o.valid && same(p.max60, o.max60) && same(p.maxFree, o.maxFree) && same(p.maxNow, o.maxNow) && p.core == o.core)) ++bad;
        }
        char w[96]; std::snprintf(w, sizeof w, "mode %d: Predict equals the old code bit for bit in %d frames (%d differ)", mode, n, bad);
        check(bad == 0, w);
        // the game clock's cost of a picture without its steps: the old expression
        npsimpace::Model m;
        int badc = 0;
        for (int i = 0; i < 100000; ++i) {
            const double real = rnd(10, 80), wait = rnd(0, 20), sim = rnd(0, 20), win = rnd(0, 6), join = rnd(0, 9);
            const double a = npsimpace::ClockPerPictureMs(mode, true, real, wait, sim, win, join, m), b = real - wait - sim;
            if (!same(a, b)) ++badc;
        }
        std::snprintf(w, sizeof w, "mode %d: the game clock's picture cost is the old expression (%d differ)", mode, badc);
        check(badc == 0, w);
    }
    // ---- 2a. the building blocks ----
    {
        npsimpace::Model m;   // no observation yet: overhead 0
        // base 8 ms, window 3, step 7: 15 (max), not 18 (sum)
        double c = npsimpace::GameCore60(npsimpace::kOverlap, true, 8.0 + 3.0, 0.0, 7.0, 3.0, m);
        check(std::fabs(c - 15.0) < 1e-9, "overlap: base 8 + max(window 3, step 7) = 15, not the sum 18");
        c = npsimpace::GameCore60(npsimpace::kOverlap, true, 8.0 + 9.0, 0.0, 7.0, 9.0, m);
        check(std::fabs(c - 17.0) < 1e-9, "overlap: window 9 longer than the step 7: base 8 + 9 = 17");
        // the join overhead: the join waited 5.2 ms where step 7 - window 3 = 4 explains 4: 1.2 ms is the overhead
        for (int i = 0; i < 100; ++i) m.Observe(3.0, 5.2, 7.0);
        check(std::fabs(m.OverheadMs() - 1.2) < 1e-6 && std::fabs(m.WindowMs() - 3.0) < 1e-6, "overhead = join wait less (step - window) = 1.2, window 3");
        c = npsimpace::GameCore60(npsimpace::kOverlap, true, 8.0 + 3.0, 0.0, 7.0, 3.0, m);
        check(std::fabs(c - 16.2) < 1e-6, "overlap with the overhead: 8 + max(3, 7) + 1.2 = 16.2");
        // a window longer than the step: no wait expected, a 0.4 ms join wait is all overhead
        npsimpace::Model m2;
        for (int i = 0; i < 100; ++i) m2.Observe(9.0, 0.4, 7.0);
        check(std::fabs(m2.OverheadMs() - 0.4) < 1e-6, "window 9 > step 7, join waited 0.4: overhead 0.4");
        // blocking mode with np_sim on another core: the game core does not hold the step: serial sum
        c = npsimpace::GameCore60(npsimpace::kBlock, false, 8.0, 7.0, 7.0, 0.0, m);
        check(std::fabs(c - 15.0) < 1e-9, "blocking, np_sim elsewhere: 8 + 7 = 15");
        // no probe: busy wall 8 + 3 + join 4: base 8; + max(3, 7)
        c = npsimpace::GameBusy60(npsimpace::kOverlap, true, 8.0 + 3.0 + 4.0, 7.0, 7.0, 3.0, 4.0, npsimpace::Model());
        check(std::fabs(c - 15.0) < 1e-9, "no probe, overlap: (15 - join 4 - window 3) + max(3, 7) = 15");
        // free-run: fixed point of P = b + max(W, kP) for k = step / 20 ms, floor and cap
        int badfp = 0;
        for (int i = 0; i < 20000; ++i) {
            const double b = rnd(2, 12), W = rnd(0, 8), st = rnd(2, 14);
            npsimpace::Model z;
            const double P = npsimpace::FreeRateOverlapMs(b, W, st, 20.0, z);
            const double k = st / 20.0;
            double it = b + W; for (int j = 0; j < 400; ++j) it = b + npsimpace::Max(W, k * it);   // the fixed point by iteration (k < 1: converges)
            const double floorMs = b + npsimpace::Max(W, st), cap = b + npsimpace::Max(W, 2 * st);
            double want = it; if (want < floorMs) want = floorMs; if (want > cap) want = cap;
            if (k < 0.9 && std::fabs(P - want) > 1e-6) ++badfp;
        }
        check(badfp == 0, "free-run overlap cost = the fixed point of P = base + max(window, k P) (clamped to one..two steps)");
        // the game clock: with the step overlapped, steps up to the window are free: perPic = real - wait - join - window + overhead
        const double pp = npsimpace::ClockPerPictureMs(npsimpace::kOverlap, true, 16.7, 0.5, 7.0, 3.0, 4.0, m);
        check(std::fabs(pp - (16.7 - 0.5 - 4.0 - 3.0 + 1.2)) < 1e-9, "game clock, overlap: the picture's cost without steps is real - wait - join - window + overhead");
    }
    // ---- 2b. the controller: a load that fits with the step overlapped and does not when it is on the game thread ----
    {
        int reached[3] = {0, 0, 0};   // frames at 60 by the end, mode 0 (step on the game thread), mode 2
        for (int mode = 0; mode <= 2; mode += 2) {
            Config cfg;
            Auto ctl(cfg);
            const double R = cfg.refresh;
            double now = 0;
            for (int i = 0; i < 1200; ++i) {
                FrameIn f; std::memset(&f, 0, sizeof f);
                f.now = now; f.interval = (ctl.Current() == kPolicy60 ? 1 : 2) * R; f.scene = 1;
                f.steps = (int)(f.interval / 0.02 + 0.5);
                const double stepMs = 4.0, winMs = 3.5, baseMs = 6.5;   // game picture: 6.5 base + 3.5 window; the step 4 ms
                f.simMs = stepMs * f.steps; f.costsValid = true; f.gameCore = 0;
                if (mode == 0) { f.cpuMs[0] = baseMs + winMs + f.simMs; }
                else {
                    f.simMode = npsimpace::kOverlap; f.simOnGameCore = false; f.simCore = 2;
                    f.windowMs = winMs; f.simWallMs = f.simMs; f.joinMs = f.simMs > winMs ? f.simMs - winMs : 0;
                    f.cpuMs[0] = baseMs + winMs; f.cpuMs[2] = f.simMs + 1.0;
                }
                ctl.Frame(f);
                now += f.interval;
                if (ctl.Current() == kPolicy60) ++reached[mode];
            }
        }
        // threshold: 0.8 of a refresh = 13.3 ms. mode 0: 6.5 + 3.5 + 4 = 14 (refused: stays 30); mode 2: 6.5 + max(3.5, 4) = 10.5 (fits: 60). The sum (BREAK=1) = 14 stays at 30.
        check(reached[0] == 0, "step on the game thread: 6.5 + 3.5 + 4 = 14 ms is over 80 % of a refresh: stays at 30");
        check(reached[2] > 600, "step overlapped: 6.5 + max(3.5, 4) = 10.5 ms fits: the controller goes to 60 and stays");
    }
    // ---- 2c. simt24: LOCKED 60 (Config::Lock60) with the step overlapped: a game thread that fits (14.8 ms mean, over the 14.5 ms planning limit, under a refresh), and the model fed WRONG figures
    //      (window and np_sim wall time scaled by a random 0.5..2 every picture): 60 must hold, no flapping (acceptance rule 5). A load that really does not fit (19 ms) must go to the relief once and not toggle.
    {
        for (int wrong = 0; wrong <= 1; ++wrong) {
            for (int scenario = 0; scenario < 2; ++scenario) {
                Config cfg = Config::Lock60();
                Auto ctl(cfg);
                const double R = cfg.refresh;
                double now = 0;
                int at60 = 0, frames = 0, lastPol = -1, flips = 0;
                s_seed = 777 + wrong * 10 + scenario;
                for (int i = 0; i < 6000; ++i) {
                    const bool is60 = ctl.Current() == kPolicy60;
                    const double meanMs = scenario == 0 ? 14.8 : 19.0;   // the game thread's real cost of a stepped picture, ms
                    double cost = meanMs + rnd(-1.5, 1.5);
                    if (!is60) cost += 0.0;
                    const double ivRefr = is60 ? (cost > R * 1000.0 ? 2.0 : 1.0) : 2.0;
                    FrameIn f; std::memset(&f, 0, sizeof f);
                    f.now = now; f.interval = ivRefr * R; f.scene = 1;
                    f.steps = (int)(f.interval / 0.02 + 0.5);
                    const double stepMs = 6.8, winMs = 3.2;
                    f.simMs = stepMs * f.steps; f.costsValid = true; f.gameCore = 0;
                    f.simMode = npsimpace::kOverlap; f.simOnGameCore = false; f.simCore = 2;
                    const double lie = wrong ? rnd(0.5, 2.0) : 1.0, lie2 = wrong ? rnd(0.5, 2.0) : 1.0;
                    f.windowMs = winMs * lie; f.simWallMs = f.simMs * lie2; f.joinMs = (f.simWallMs > f.windowMs ? f.simWallMs - f.windowMs : 0) + 0.3;
                    f.cpuMs[0] = cost; f.cpuMs[1] = 8.0; f.cpuMs[2] = f.simMs + 5.0;
                    const bool wasCap = false; (void)wasCap;
                    ctl.Frame(f);
                    now += f.interval;
                    ++frames;
                    const int pol = ctl.Current() == kPolicy60 ? 60 : 30;
                    if (lastPol >= 0 && pol != lastPol) ++flips;
                    lastPol = pol;
                    if (pol == 60) ++at60;
                }
                char d[120]; snprintf(d, sizeof d, "(wrong=%d mean %.1f ms: %d flips, %d of %d frames at 60)", wrong, scenario == 0 ? 14.8 : 19.0, flips, at60, frames);
                if (scenario == 0) { check(flips <= 2 && at60 > frames * 95 / 100, "simt24: a game thread that fits a refresh (14.8 ms) holds 60 under wrong model figures"); }
                else check(flips <= 40, "simt24: a game thread that does not fit (19 ms) does not toggle every few seconds (flips bounded by the retry back-off)");
                std::printf("  simt24 2c %s\n", d);
            }
        }
    }
    if (g_fail) { std::printf("simpace18 unit test: %d FAILED\n", g_fail); return 1; }
    std::printf("simpace18 unit test: passed\n");
    return 0;
}
