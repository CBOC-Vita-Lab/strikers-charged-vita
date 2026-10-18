// W5 RT: a model of the frame loop around the REAL platform/compat/np_fps.cpp, for the input delay with and without the late wait (MSC_LATE_WAIT).
//   clang++ -std=c++11 -O1 -DNP_NATIVE=1 -Iinclude -Iplatform/compat tools/rt/late_sim.cpp platform/compat/np_fps.cpp -o late_sim
//   MSC_LATE_WAIT=0|1|2 ./late_sim <frames> <pre ms> <after-pad ms> <after-pad jitter ms> <downstream ms> <downstream jitter ms> [seed]
// A frame on the game thread: the tasks before FixedUpdateTask (pre), FixedUpdateTask (the late wait, then the pads are read), the rest of the frame
// (after-pad, uniform jitter), swap_Post (swap mode 4 under policy 30: the wait of the build as it was, or the late wait's fallback), the hand-over,
// then (MSC_LATE_WAIT=1) the wait after the send.
// After the hand-over the frame needs `downstream` (frame send, decode, back end, GPU: uniform jitter) to be ready for the display, which shows it at
// the first vblank after it is ready and at least two vblanks after the previous picture (the vgx display gate, W5 FPS).
// Prints: frames per second, the refreshes each picture was held (pacing), the hand-over spacing in retraces, and the delay pad read -> vblank shown
// (p50, p95, mean) -- the model's input delay. It is a MODEL of the wait logic (no game): the game's own swap code runs in the host32 runs.
#include "np/fps.h"

#include <algorithm>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>

namespace {
const double T = 1001.0 / 60000.0;   // one refresh, seconds
double g_now = 0.0;                  // virtual seconds
uint32_t Retrace() { return (uint32_t)floor(g_now / T); }
uint64_t g_rng = 12345;
double Rnd() {   // [0, 1)
    g_rng = g_rng * 6364136223846793005ull + 1442695040888963407ull;
    return (double)(g_rng >> 11) / 9007199254740992.0;
}
}

extern "C" {
int NpVirtualTimeActive(void) { return 1; }
int64_t NpVirtualNowNs(void) { return (int64_t)(g_now * 1e9); }
uint32_t NpVI_GetRetraceCount(void) { return Retrace(); }
unsigned long VIGetRetraceCount(void) { return Retrace(); }
void VIWaitForRetrace(void) { g_now = (Retrace() + 1) * T + 1e-9; }
}

int main(int argc, char** argv) {
    const int frames = argc > 1 ? atoi(argv[1]) : 3000;
    const double pre = (argc > 2 ? atof(argv[2]) : 1.0) / 1000.0, after = (argc > 3 ? atof(argv[3]) : 20.0) / 1000.0;
    const double afterJ = (argc > 4 ? atof(argv[4]) : 4.0) / 1000.0, down = (argc > 5 ? atof(argv[5]) : 30.0) / 1000.0;
    const double downJ = (argc > 6 ? atof(argv[6]) : 4.0) / 1000.0;
    if (argc > 7) g_rng = strtoull(argv[7], 0, 10);
    setenv("MSC_FPS_LOG", "0", 1);
    uint32_t retraceAtSwap = 0;
    double lastShownT = -1.0;
    uint32_t lastShownV = 0;
    bool have = false;
    std::vector<double> delay;
    uint64_t held[6] = {0, 0, 0, 0, 0, 0}, spacing[6] = {0, 0, 0, 0, 0, 0};
    uint32_t lastHandover = 0;
    double t0 = 0;
    std::vector<std::pair<double, double>> inflight;   // (ready time, pad time)
    for (int f = 0; f < frames; ++f) {
        if (f == 100) t0 = g_now;
        NpFpsBeginFrame((float)(1.0 / 30.0), 2);
        g_now += pre;
        NpFpsLateWaitRun(0);            // FixedUpdateTask::Run
        const double pad = g_now;       // the pads are read
        g_now += after + afterJ * Rnd();
        // swap_Post, swap mode 4 under policy 30
        const int mode = NpFpsSwapMode(3);
        NpFpsSwapWaitBegin();
        if (mode == 4 && NpFpsLateWait(4, retraceAtSwap + 2)) NpFpsLateWaitRun(1);
        else if (mode == 4) while (Retrace() < retraceAtSwap + 2) VIWaitForRetrace();
        NpFpsSwapWaitEnd();
        // the hand-over (glxSwapPreFork / swap_Pre)
        retraceAtSwap = Retrace();
        NpFpsLateWaitOwe(retraceAtSwap + 2);
        if (f > 100) { const uint32_t d = retraceAtSwap - lastHandover; ++spacing[d > 5 ? 5 : d]; }
        lastHandover = retraceAtSwap;
        const double ready = g_now + down + downJ * Rnd();
        NpFpsLateWaitAfterSend();       // glplatSendFrame / glplatSendFrameForked (MSC_LATE_WAIT=1)
        // the display gate: the first vblank after `ready`, at least two after the previous picture
        uint32_t v = (uint32_t)floor(ready / T) + 1;
        if (have && v < lastShownV + 2) v = lastShownV + 2;
        if (have && f > 100) { const uint32_t h = v - lastShownV; ++held[h > 5 ? 5 : h]; }
        lastShownV = v;
        lastShownT = v * T;
        have = true;
        if (f > 100) delay.push_back((lastShownT - pad) * 1000.0);
    }
    std::sort(delay.begin(), delay.end());
    double sum = 0;
    for (double d : delay) sum += d;
    const double secs = g_now - t0;
    printf("late=%s pre=%.1f after=%.1f+%.1f down=%.1f+%.1f ms: %.2f fps; pictures held 1/2/3/4/5+ refreshes: %llu/%llu/%llu/%llu/%llu; hand-over spacing "
           "1/2/3/4/5+ retraces: %llu/%llu/%llu/%llu/%llu; input delay p50 %.1f p95 %.1f mean %.1f ms\n",
           getenv("MSC_LATE_WAIT") ? getenv("MSC_LATE_WAIT") : "0", pre * 1000, after * 1000, afterJ * 1000, down * 1000, downJ * 1000,
           (frames - 101) / secs, (unsigned long long)held[1], (unsigned long long)held[2], (unsigned long long)held[3], (unsigned long long)held[4],
           (unsigned long long)held[5], (unsigned long long)spacing[1], (unsigned long long)spacing[2], (unsigned long long)spacing[3],
           (unsigned long long)spacing[4], (unsigned long long)spacing[5], delay[delay.size() / 2], delay[delay.size() * 95 / 100], sum / delay.size());
    return 0;
}
