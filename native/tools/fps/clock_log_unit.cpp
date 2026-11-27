// pacing12: the REAL np_fps.cpp game clock path (SlipFrame -> NpSlipDelta) on a virtual clock: 45 ms pictures and a 100 ms hitch every 3 s for 40 s in the gameplay state; the fixed steps
// the game would run (a task delta of NpSlipDelta(real) into a 20 ms accumulator) against the wall time.   clock_log_unit debt|slip   (debt: >= 99.5 %; slip: the wave 9 bucket, under 99 %)
#include "../../platform/compat/np_fps.cpp"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint64_t g_ns = 1000000000ull;
extern "C" int NpVirtualTimeActive(void) { return 1; }
extern "C" int64_t NpVirtualNowNs(void) { return (int64_t)g_ns; }
int main(int argc, char** argv) {
    const bool debt = argc > 1 && !strcmp(argv[1], "debt");
    setenv("MSC_FPS", "30", 1);
    setenv("MSC_BOOT_FPS", "0", 1);
    setenv("MSC_FPS_LOG", "0", 1);
    setenv("MSC_SLIP", "20", 1);
    setenv("MSC_SLIP_STEPS", "2", 1);
    setenv("MSC_SLIP_BUDGET_MS", "30", 1);
    setenv("MSC_SLIP_REFILL_PCT", "2", 1);
    setenv("MSC_CLOCK", debt ? "debt" : "slip", 1);
    NpFpsBeginFrame(0.0334f, 2);
    double acc = 0, wall = 0;
    unsigned long steps = 0;
    for (int i = 0; i < 1000; ++i) {
        const double real = (i % 67 == 66) ? 0.100 : 0.045;
        NpFpsBeginFrame((float)real, 2);
        g_ns += (uint64_t)(real * 1e9);
        wall += real;
        float dt = (float)real; if (dt > 0.1f) dt = 0.1f;
        acc += NpSlipDelta(dt);
        for (; acc >= 0.02 - 1e-9; acc -= 0.02) { NpFpsSimStep(); ++steps; }
    }
    const double pct = 100.0 * (double)steps * 0.02 / wall;
    printf("clock_log_unit %s: %.2f %% of real time over %.1f s (%lu steps)\n", debt ? "debt" : "slip", pct, wall, steps);
    if (debt && pct < 99.0) { printf("FAIL: the debt clock must read 99 %% or more\n"); return 1; }
    if (!debt && pct > 99.0) { printf("FAIL: model check: the slip bucket should read under 99 %%\n"); return 1; }
    return 0;
}
