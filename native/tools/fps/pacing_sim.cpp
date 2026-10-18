// A small model of the game's frame loop around platform/compat/np_fps.cpp, to run the real policy code without the game:
//   a virtual 59.94 Hz VI, glxSwap.cpp swap_Post's wait modes (copied: 0..3 the game's, 4 the 30 cap), frames that cost what a model says.
//   clang++ -std=c++11 -O1 -DNP_NATIVE=1 -Iinclude -Iplatform/compat tools/fps/pacing_sim.cpp platform/compat/np_fps.cpp -o pacing_sim
//   MSC_FPS=60 ./pacing_sim <seconds> <P ms> <M ms/step> <core1 ms> <core2 ms>      (costs per frame; the game core is P + M x steps)
// It prints the refreshes each frame was held (the histogram), the mean rate, and with MSC_FPS=auto the switches np_fps.cpp logs.
// This is a MODEL of the wait logic, not the game: the game's own swap code runs in the host32 runs (tools/fps/job_*.sh).
#include "np/fps.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

namespace {
const double R = 1001.0 / 60000.0;
double g_now = 1000.0;            // virtual seconds
double g_steps_acc = 0.02;        // FixedUpdateTask accumulator
uint32_t g_retraceAtSwap = 0;

uint32_t Retrace() { return (uint32_t)floor((g_now - 1000.0) / R); }
void WaitRetrace() { g_now = 1000.0 + (Retrace() + 1) * R + 1e-9; }
double SinceRetrace() { return (g_now - 1000.0) - Retrace() * R; }
}

extern "C" {
int NpVirtualTimeActive(void) { return 1; }
int64_t NpVirtualNowNs(void) { return (int64_t)(g_now * 1e9); }
uint32_t NpVI_GetRetraceCount(void) { return Retrace(); }
}

int main(int argc, char** argv) {
    const double seconds = argc > 1 ? atof(argv[1]) : 20.0;
    const double P = argc > 2 ? atof(argv[2]) : 8.0, M = argc > 3 ? atof(argv[3]) : 0.0;
    const double c1 = argc > 4 ? atof(argv[4]) : 8.0, c2 = argc > 5 ? atof(argv[5]) : 8.0;
    char spec[128];
    snprintf(spec, sizeof spec, "const:%g,%g,0,%g,%g,0", P, M, c2, c1);   // P, M, W, D (core 2), B (core 1), A
    setenv("MSC_FPS_MODEL", spec, 1);
    setenv("MSC_FPS_LOG", getenv("MSC_FPS_LOG") ? getenv("MSC_FPS_LOG") : "1", 1);
    double last = g_now;
    uint64_t frames = 0, hist[5] = {0, 0, 0, 0, 0};
    unsigned taskState = 2;
    const double end = g_now + seconds;
    double lastHandover = g_now;
    const int gameMode = argc > 6 ? atoi(argv[6]) : 3;   // the game's swap mode: 3 in a match, 1 in the menus
    while (g_now < end) {
        const double interval = g_now - last;
        last = g_now;
        NpFpsBeginFrame((float)interval, taskState);
        // simulation: steps the accumulator holds
        g_steps_acc += interval;
        unsigned steps = 0;
        while (g_steps_acc >= 0.02) { g_steps_acc -= 0.02; ++steps; }
        // swap_Post
        const uint32_t retrace = Retrace();
        const int mode = NpFpsSwapMode(gameMode);
        NpFpsSwapWaitBegin();
        if (mode == 2) { WaitRetrace(); if (Retrace() < g_retraceAtSwap + 2) WaitRetrace(); }
        else if (mode == 1) { WaitRetrace(); }
        else if (mode == 3) { if (SinceRetrace() * 1e6 > 12000.0 || retrace == g_retraceAtSwap) WaitRetrace(); }
        else if (mode == 4) { while (Retrace() < g_retraceAtSwap + 2) WaitRetrace(); }
        NpFpsSwapWaitEnd();
        // fork + the frame's picture is handed over (swap_Pre of the job): record the retrace count
        g_retraceAtSwap = Retrace();
        const double held = (g_now - lastHandover) / R;
        lastHandover = g_now;
        if (frames > 0) ++hist[held < 1.5 ? 0 : held < 2.5 ? 1 : held < 3.5 ? 2 : held < 4.5 ? 3 : 4];
        ++frames;
        // the harness charges the frame's cost after the hook
        const long long ns = NpFpsModelCharge(taskState, -1, steps);
        g_now += (double)ns * 1e-9;
    }
    printf("policy %d (mode %d), %llu frames in %.1f s = %.1f fps; held 1/2/3/4/5+ refreshes: %llu/%llu/%llu/%llu/%llu\n", NpFpsPolicy(), NpFpsMode(),
           (unsigned long long)frames, g_now - 1000.0, frames / (g_now - 1000.0), (unsigned long long)hist[0], (unsigned long long)hist[1], (unsigned long long)hist[2],
           (unsigned long long)hist[3], (unsigned long long)hist[4]);
    return 0;
}
