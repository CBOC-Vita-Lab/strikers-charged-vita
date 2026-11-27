// MSC_HELD_CADENCE=auto in the real np_fps.cpp (virtual retraces, no game data): the wiring around np_fps_cad3.h.
// From native/: clang++ -std=c++11 -O1 -Iinclude -Iplatform/compat tools/fps/cad3_wire_unit.cpp -o /tmp/cad3_wire_unit && /tmp/cad3_wire_unit
//   - a held scene (task state 16, a cut-scene) of 50 ms pictures: the steady 3 comes on after the skipped and the judged pictures, and the late wait then spaces the frames 3 refreshes
//   - the same pictures in open play (task state 2): never; the same in a LAN session: never; with the switch unset: never; without the late wait: never
//   - leaving the scene puts the 2 back at once
// (Not a Vita measurement: the console decides.)
#define NP_NATIVE 1
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "../../platform/compat/np_fps.cpp"

static double ticks;
static int lan;
static int failures, checks;
static void Check(bool ok, const char* what) { ++checks; if (!ok) { ++failures; std::printf("FAIL: %s\n", what); } }
extern "C" unsigned long VIGetRetraceCount() { return static_cast<unsigned long>(ticks); }
extern "C" void VIWaitForRetrace() { ticks = std::floor(ticks) + 1; }
extern "C" uint64_t NpLatHandedOverTag() { return 0; }
extern "C" uint64_t NpLatShownTag() { return 0; }
extern "C" int64_t NpMonotonicNs() { return static_cast<int64_t>(ticks * (1001.0 / 60000.0) * 1e9); }
extern "C" int64_t NpVirtualNowNs() { return NpMonotonicNs(); }
extern "C" int NpVirtualTimeActive() { return 0; }
extern "C" uint32_t NpVI_GetRetraceCount() { return static_cast<uint32_t>(VIGetRetraceCount()); }
extern "C" int NpFpsPlatformProbe(NpFpsProbeOut*) { return 0; }
extern "C" double NpFpsPlatformThreadClockMs() { return 0; }
extern "C" void NpLatLatch() {}
extern "C" float VgxGpuMinMs() { return 0; }
extern "C" int VgxMegaActive() { return 0; }
bool NpLanActive() { return lan != 0; }

// `n` frames of `ms` each at task state `state`; returns the period in force at the end, and the mean refreshes between the frame starts from the late wait
static float g_laneMs[3];   // heldpipe23: the renderer lanes (NpLateLaneNote) fed before each picture
static int Frames(unsigned state, int n, double ms, double* meanGap = 0) {
    double gapSum = 0; int gaps = 0; unsigned long prev = 0; bool have = false;
    for (int i = 0; i < n; ++i) {
        ticks += ms / (1001.0 / 60.0);
        for (int l = 0; l < 3; ++l) if (g_laneMs[l] > 0) NpLateLaneNote(l, g_laneMs[l]);
        NpFpsBeginFrame((float)(ms / 1000.0), state);
        const unsigned long start = VIGetRetraceCount();
        if (have) { gapSum += (double)(start - prev); ++gaps; }
        prev = start; have = true;
        NpFpsSwapMode(1);
        if (NpFpsLateWait(4, VIGetRetraceCount() + 2)) NpFpsLateWaitAfterSend();
    }
    if (meanGap) *meanGap = gaps ? gapSum / gaps : 0;
    return HeldPeriod();
}

int main() {
    setenv("MSC_FPS", "auto", 1);
    setenv("MSC_LATE_WAIT", "1", 1);
    setenv("MSC_HELD_CADENCE", "auto", 1);
    setenv("MSC_LAT_BP", "0", 1);
    setenv("MSC_LATE_CAUSE", "0", 1);   // held24: the cadence exit reads the lanes whatever MSC_LATE_CAUSE says
    Check(NpLateCauseOn() == 1, "MSC_LATE_CAUSE=0 with MSC_HELD_CADENCE=auto: the lanes are still fed");
    setenv("MSC_LAT_DEPTH", "0", 1);
    NpFpsBeginFrame(0.0334f, 2);   // (starts the policy)
    Check(g.cad3 != 0, "MSC_HELD_CADENCE=auto creates the decision");
    Check(g.policy == kPolicy30, "auto starts at 30");
    double gap = 0;
    Check(Frames(2, 60, 50.0) == 2, "open play (task state 2): 50 ms pictures never get the 3");
    Check(Frames(16, 10, 50.0) == 2, "a cut-scene: the first pictures are not judged yet");
    Check(Frames(16, 20, 50.0) == 3, "a cut-scene of 50 ms pictures: the steady 3 is on after the skipped 4 and the 20 judged");
    Frames(16, 4, 50.0);
    Check(Frames(16, 20, 50.0, &gap) == 3 && gap > 2.9 && gap < 3.1, "at the 3 the late wait spaces the frames 3 refreshes apart");
    Check(Frames(2, 1, 33.4) == 2, "back in open play: 2 again at once");
    Frames(16, 40, 33.4, &gap);
    lan = 1; Check(Frames(16, 80, 50.0) == 2, "LAN: never"); lan = 0;
    Check(Frames(2, 5, 33.4) == 2, "open play");
    Check(Frames(16, 60, 30.0) == 2, "a cut-scene of 30 ms pictures stays at 2");
    Check(Frames(8, 80, 50.0) == 3, "a replay (task state 8) of 50 ms pictures is a held scene too");
    Check(Frames(2, 1, 33.4) == 2, "leaving the replay: 2");
    // heldpipe23: 50 ms pictures whose front end takes 38 ms: the steady 3 is on, and the exit judges the largest lane, so it stays (work 38 > 28); with the lanes at 10 ms it lets go
    Check(Frames(2, 3, 33.4) == 2, "open play");
    g_laneMs[0] = 38; g_laneMs[1] = 33; g_laneMs[2] = 20;
    Check(Frames(16, 40, 50.0) == 3, "lanes: the steady 3 is on");
    Check(Frames(16, 120, 15.0) == 3, "lanes: a front end of 38 ms keeps the steady 3 (work = the largest lane, not the game thread's 15 ms)");
    g_laneMs[0] = g_laneMs[1] = g_laneMs[2] = 10;
    Check(Frames(16, 120, 15.0) == 2, "lanes of 10 ms: the work really is small, back to 2");
    g_laneMs[0] = g_laneMs[1] = g_laneMs[2] = 0;
    Check(Frames(2, 1, 33.4) == 2, "open play");
    // the switch unset: no decision, period 2 whatever the pictures
    delete g.cad3; g.cad3 = 0; g.cad3Period = 0;
    Check(Frames(16, 80, 50.0) == 2, "switch unset: never");
    std::printf(failures ? "FAILED (%d of %d)\n" : "cad3 wiring test: passed (%d checks)\n", failures ? failures : checks, checks);
    return failures ? 1 : 0;
}
