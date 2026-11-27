// The REAL np_fps.cpp drives the game-speed line (MSC_SPEED_LOG): synthetic frames on a virtual clock, the [speed] lines captured from stderr.
//   speed_log_unit on|off   (on: lines must appear with the right percentage; off: none)
#include "../../platform/compat/np_fps.cpp"
#include "gs_stub.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint64_t g_ns = 1000000000ull;
extern "C" int NpVirtualTimeActive(void) { return 1; }
extern "C" int64_t NpVirtualNowNs(void) { return (int64_t)g_ns; }
int main(int argc, char** argv) {
    const bool on = argc > 1 && !strcmp(argv[1], "on");
    setenv("MSC_FPS", "30", 1);
    setenv("MSC_BOOT_FPS", "0", 1);
    setenv("MSC_FPS_LOG", "0", 1);
    if (!on) setenv("MSC_SPEED_LOG", "0", 1);
    NpFpsBeginFrame(0.0334f, 2);
    double carry = 0;
    // 600 frames at 33.4 ms, 1.6667 steps a frame (full speed), then 300 frames at 33.4 ms with 1.5 steps (90 %), then a replay, then play again
    for (int i = 0; i < 600; ++i) { NpFpsBeginFrame(0.0334f, 2); g_ns += 33400000ull; carry += 50.0 * 0.0334; for (; carry >= 1.0; carry -= 1.0) NpFpsSimStep(); }
    for (int i = 0; i < 300; ++i) { NpFpsBeginFrame(0.0334f, 2); g_ns += 33400000ull; carry += 45.0 * 0.0334; for (; carry >= 1.0; carry -= 1.0) NpFpsSimStep(); }
    for (int i = 0; i < 200; ++i) { NpFpsBeginFrame(0.0334f, 8); g_ns += 33400000ull; }
    for (int i = 0; i < 200; ++i) { NpFpsBeginFrame(0.0334f, 0); g_ns += 33400000ull; }
    return 0;
}
