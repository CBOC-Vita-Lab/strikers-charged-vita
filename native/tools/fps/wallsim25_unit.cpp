// wallsim25_unit.cpp (nxr4c, 25 Nov 2026): replay of the console's [hold60] windows through the REAL np_fps_ctl.h Auto, MSC_HOLD60_WALLSIM off against on.
//   c++ -std=c++11 -O1 -Inative/platform/compat native/tools/fps/wallsim25_unit.cpp native/tools/fps/gsplit_stub.cpp -o wallsim25_unit && ./wallsim25_unit
// Mutant (-DNP_P24_BREAK=3, must FAIL): the wall sim priced with CPU ms.
// The data: windows of 6027 / 6032 / 6033 / 6034 (25 Nov Vice and Dump logs, w8/logs-1125). Per window: steps a picture (the [gsplit] line), the steps' wall ms a picture ([gsplit] sim), the wall ms
// lost inside the steps beyond their runClocks CPU ms ([hold60] clock check), the climb cost the console's controller computed (CPU pricing, MSC_HOLD60_WALLSIM=0), fe / be / GPU of the climb parts line,
// and the 1-step pictures' real work p50 ([gsplit-pic], net of waits). The replay rebuilds the pictures from those (steps 1 and 2 mixed to the window's mean, the stall per step), checks that the
// controller reproduces the console's cost with the CPU pricing (the calibration) and shows what the wall pricing says. Everything here is an ESTIMATE of the controller's input, not of the console's play.
#include "np_fps_ctl.h"
#include "np_fps45.h"
#include "np_game_clock.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
using namespace npfps;

static int g_fail = 0;
static void check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) ++g_fail; }
static const double R = 1001.0 / 60000.0, Rms = R * 1000.0;

struct Win { const char* log; double n, simWall, lost, gameOff, fe, be, gpu, p50; };
// 30-tier windows (about 1.6 steps a picture): the 60 is judged against 0.97 of a refresh
static const Win kWin30[] = {
    {"6027", 1.67, 7.80, 5.21, 17.3, 9.7, 5.8, 11.3, 13.50},
    {"6032", 1.71, 7.71, 4.78, 17.8, 10.4, 7.4, 11.6, 13.58},
    {"6032", 1.67, 7.82, 4.85, 18.1, 10.5, 7.9, 12.8, 14.44},
    {"6032", 1.67, 7.58, 4.61, 17.2, 10.3, 8.1, 12.7, 13.45},
    {"6033", 1.53, 7.24, 5.34, 18.3, 11.0, 5.0, 20.1, 14.15},   // The Dump: the GPU term (20 ms) keeps the 60 out on both sides
    {"6033", 1.63, 8.09, 4.98, 19.7, 12.6, 7.7, 20.9, 15.50},
    {"6034", 1.63, 8.12, 4.49, 19.1, 12.1, 8.1, 22.6, 15.28},
};
// 45-tier windows (about 1.12 steps a picture)
static const Win kWin45[] = {
    {"6027", 1.12, 5.66, 2.45, 18.7, 10.8, 7.3, 11.8, 17.10},
    {"6027", 1.16, 5.65, 2.80, 17.3, 10.2, 9.2, 11.8, 16.15},
    {"6032", 1.13, 5.54, 2.39, 17.9, 10.5, 9.6, 11.8, 16.08},
    {"6032", 1.16, 5.83, 2.82, 18.7, 10.6, 9.7, 12.5, 16.83},
};

struct Replay {
    Auto ctl;
    double now = 0, cost = 0;
    explicit Replay(bool wall) : ctl(Cfg(wall)) {}
    static Config Cfg(bool wall) { Config c = Config::Lock60Hold(); c.holdWallSim = wall; return c; }
    void Pic(const Win& w, int steps, double ms, bool cpuClockOnly = false) {
        FrameIn f; std::memset(&f, 0, sizeof f);
        const double wallStep = w.simWall / w.n, cpuStep = (w.simWall - w.lost) / w.n;
        // the console's cost with the CPU pricing = busy - simCpu + cpuStep, so the mean busy ms of the window:
        const double busy = w.gameOff + (w.n - 1.0) * cpuStep;
        const double nonStep = busy - w.n * wallStep;   // the picture's work outside the steps
        now += ms / 1000.0;
        f.now = now; f.interval = ms / 1000.0; f.steps = steps; f.simMs = steps * cpuStep; f.stepWallMs = cpuClockOnly ? steps * cpuStep : steps * wallStep;
        f.costsValid = true; f.gameCore = 0; f.cpuMs[0] = nonStep + steps * wallStep; f.cpuMs[1] = 8.0; f.scene = 2;
        f.workMs = nonStep + steps * wallStep; f.feMs = w.fe; f.beMs = w.be; f.gpuMinMs = w.gpu;
        ctl.Frame(f);
        cost = ctl.HoldUpCost();
    }
    // the window's 300 pictures at its cadence (steps 1 and 2 error-diffused to the mean) after a 60 that has fallen: the mean climb cost over them, and whether the 60 came back at any point
    double Run(const Win& w, double ms, bool* climbed) {
        Pic(w, 1, Rms);
        for (int i = 0; i < 400 && ctl.Current() == kPolicy60; ++i) Pic(w, 2, 50.0);
        double acc = 0, sum = 0; int n = 0; *climbed = false;
        for (int i = 0; i < 300; ++i) {
            acc += w.n; int k = (int)acc; acc -= k; if (k < 1) k = 1;
            Pic(w, k, ms);
            if (i >= 20) { sum += cost; ++n; }
            if (ctl.Current() == kPolicy60) *climbed = true;
        }
        return n ? sum / n : 0.0;
    }
};

int main() {
    std::printf("wallsim25: the climb cost the real controller gets for the console's windows (limit %.2f ms = 0.97 of a refresh)\n", 0.97 * Rms);
    std::printf("  log   steps  CPU-priced  wall-priced  console(CPU)  1-step p50 (real)  climbs off/on\n");
    int bothClimb30 = 0, onlyOn30 = 0, onlyOff30 = 0;
    double sumDelta30 = 0, sumErrOff30 = 0, sumErrOn30 = 0; int n30 = 0;
    for (const Win& w : kWin30) {
        bool cOff, cOn;
        Replay a(false), b(true);
        const double off = a.Run(w, 33.37, &cOff), on = b.Run(w, 33.37, &cOn);
        std::printf("  %s  %.2f   %6.2f      %6.2f       %6.2f        %6.2f             %d / %d%s\n", w.log, w.n, off, on, w.gameOff, w.p50, cOff, cOn, w.gpu > 16.18 ? "   (GPU-bound: the cost is the GPU term)" : "");
        if (w.gpu > 16.18) { check(!cOff && !cOn, "a GPU-bound window (The Dump, GPU 20 ms and over) does not climb on either side"); continue; }
        check(std::fabs(off - w.gameOff) < 1.2, "the replay with the CPU pricing reproduces the console's climb cost (calibration, within 1.2 ms)");
        check(on <= off + 1e-9, "wall pricing never costs more at one step than the CPU pricing at 1.5+ steps");
        check(!cOff || cOn, "a window that climbs with the CPU pricing still climbs with the wall pricing");
        if (cOff && cOn) ++bothClimb30; else if (cOn) ++onlyOn30; else if (cOff) ++onlyOff30;
        sumDelta30 += off - on; sumErrOff30 += std::fabs(off - w.p50); sumErrOn30 += std::fabs(on - w.p50); ++n30;
        // the cost the wall pricing reports is in the same clock as the frame: it can only be BELOW the CPU pricing by (n - 1) x the stall of a step
        const double expect = w.lost * (1.0 - 1.0 / w.n);
        check(std::fabs((off - on) - expect) < 0.8, "the drop equals the stall a step times the steps above one (lost x (1 - 1/n))");
    }
    std::printf("  30-tier windows: mean drop %.2f ms; |cost - real 1-step p50| mean %.2f ms (CPU pricing) -> %.2f ms (wall pricing); only wall climbs %d, only CPU climbs %d\n", sumDelta30 / n30, sumErrOff30 / n30, sumErrOn30 / n30, onlyOn30, onlyOff30);
    check(sumErrOn30 < sumErrOff30 - 1.0, "the wall pricing is closer to the real 1-step work than the CPU pricing (by over 1 ms on average)");
    check(onlyOn30 == 4, "all four Vice 30-tier windows climb only with the wall pricing (the console shows game 17.2-18.1 over 16.2 at the 30)");
    check(onlyOff30 == 0, "no window climbs only with the CPU pricing");
    // the 45-tier windows: about 1.1 steps a picture, so the change is small and the 60 stays out where the real work is over a refresh
    double maxDelta45 = 0;
    for (const Win& w : kWin45) {
        bool cOff, cOn; Replay a(false), b(true);
        const double off = a.Run(w, 22.24, &cOff), on = b.Run(w, 22.24, &cOn);
        std::printf("  %s  %.2f   %6.2f      %6.2f       %6.2f        %6.2f             %d / %d   (45 tier)\n", w.log, w.n, off, on, w.gameOff, w.p50, cOff, cOn);
        maxDelta45 = std::max(maxDelta45, off - on);
    }
    check(maxDelta45 < 0.8, "45-tier windows (about 1.1 steps): the change is under 0.8 ms (NEON's 2.9-5.2 ms does not apply there)");
    std::printf(g_fail ? "wallsim25 unit: %d FAILED\n" : "wallsim25 unit: all passed (%d failed)\n", g_fail);
    return g_fail ? 1 : 0;
}
