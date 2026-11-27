// Whole production adapter + platform probe, with deterministic platform boundaries.
#define NP_NATIVE 1
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdint.h>
#include "psp2/kernel/threadmgr.h"
#include FPS_SOURCE
#include PROBE_SOURCE

bool g_lan = false;
namespace {
int checks, failures, testFrame, callsThisFrame, callsTotal;
int failedFrame = -999;
int64_t testNowNs;
double testThreadMs, cumulativeCpu, simAccumulator;
bool kernelAvailable = true;
uint64_t kernelClocks;

void Check(bool pass, const char* what) {
    ++checks;
    if (!pass) { ++failures; std::printf("FAIL %s\n", what); }
}
double TestThreadClock() { return testThreadMs; }
int CumulativeProbe(NpFpsProbeOut* out) {
    ++callsThisFrame; ++callsTotal;
    if (testFrame == failedFrame) return 0;
    std::memset(out, 0, sizeof *out);
    out->cpuMs[0] = cumulativeCpu;
    return 1;
}
int ActualPlatformProbe(NpFpsProbeOut* out) {
    ++callsThisFrame; ++callsTotal;
    return NpFpsPlatformProbe(out);
}
void Seed(NpFpsProbeFn probe) {
    NpFpsSetProbe(probe);
    NpFpsSetThreadClock(TestThreadClock);
    callsThisFrame = 0;
    NpFpsBeginFrame(0, 2);
    Check(NpFpsMode() == 4, "actual Vita-default AUTO mode selected");
    Check(callsThisFrame == 1, "seed callback invoked once");
}
void Interval(double busyMs, int steps) {
    const double interval = 2 * (1001.0 / 60000.0);
    const int64_t target = static_cast<int64_t>(std::llround(testFrame * interval * 1e9));
    NpFpsSimBegin();
    for (int step = 0; step < steps; ++step) { NpFpsSimStep(); testThreadMs += 2; }
    NpFpsSimEnd();
    const int64_t wait = static_cast<int64_t>(std::llround((interval * 1000 - busyMs) * 1e6));
    testNowNs = target - wait;
    NpFpsSwapWaitBegin();
    testNowNs = target;
    NpFpsSwapWaitEnd();
    callsThisFrame = 0;
    NpFpsBeginFrame(static_cast<float>(interval), 2);
}
int Steps() {
    simAccumulator += 2 * (1001.0 / 60000.0);
    const int steps = static_cast<int>((simAccumulator + 1e-12) / .020);
    simAccumulator -= steps * .020;
    return steps;
}
void Gap() {
    failedFrame = 60;
    Seed(CumulativeProbe);
    for (testFrame = 1; testFrame <= 61; ++testFrame) {
        const int steps = Steps();
        const double busy = 6 + 2 * steps;
        cumulativeCpu += busy;
        Interval(busy, steps);
        if (testFrame == 60) {
            Check(steps == 2, "failure frame has actual two fixed steps");
            Check(callsThisFrame == 1, "failure frame invokes callback once");
            Check(!g.haveProbe, "failed callback invalidates measurement adjacency");
        }
        if (testFrame == 61) {
            Check(steps == 1, "recovery frame has actual one fixed step");
            Check(callsThisFrame == 1, "recovery callback invoked once");
            Check(std::fabs(g.lastPred60 - 8) < .001, "recovery seeds baseline and uses current-frame fallback8");
            Check(NpFpsPolicy() == npfps::kPolicy60, "frame61 evidence permits promotion without fabricated cumulative span");
            Check(g.haveProbe && g.probePrev.cpuMs[0] == cumulativeCpu, "recovery saves fresh successful cumulative baseline");
            std::printf("GAP prediction=%.6f policy=%d callbacks=%d cumulative=%.1f\n", g.lastPred60, NpFpsPolicy(), callsThisFrame, cumulativeCpu);
        }
    }
}
void FailedSeed() {
    failedFrame = 0;
    Seed(CumulativeProbe);
    Check(!g.haveProbe, "unavailable seed has no previous measurement");
    testFrame = 1; cumulativeCpu = 8;
    Interval(8, 1);
    Check(callsThisFrame == 1, "first successful baseline-only callback invoked once");
    Check(g.haveProbe && g.probePrev.cpuMs[0] == 8, "baseline-only success seeds without losing zero origin");
    Check(std::fabs(g.lastPred60 - 8) < .001, "baseline-only success uses current fallback rather than fabricated delta");
}
void Zero() {
    Seed(CumulativeProbe);
    Check(g.haveProbe && g.probePrev.cpuMs[0] == 0, "successful zero seed is preserved");
    testFrame = 1;
    Interval(6, 0);
    Check(callsThisFrame == 1 && g.haveProbe && g.probePrev.cpuMs[0] == 0, "successful zero delta preserves callback adjacency");
    Check(g.winProbeZero == 1 && g.winNoProbe == 0, "existing zero-cost diagnostic classification retained");
    testFrame = 2; cumulativeCpu = 8;
    Interval(8, 1);
    Check(callsThisFrame == 1 && g.winProbeOk == 1, "next nonzero measurement uses valid zero baseline");
    Check(std::fabs(g.lastPred60 - 8) < .001, "nonzero measurement after zero predicts current cost8");
}
void PlatformRegistration() {
    Seed(ActualPlatformProbe); // Actual provider initially has no registered threads.
    Check(!g.haveProbe, "actual no-thread provider seed unavailable");
    kernelClocks = 1000;
    NpFpsProbeRegisterThread(1, 2);
    kernelClocks = 9000;
    testFrame = 1;
    Interval(8, 0);
    Check(callsThisFrame == 1, "actual registration recovery invokes production provider once");
    Check(g.probePrev.cpuMs[0] == 8, "actual provider cumulative registration sample retained");
    kernelAvailable = false;
    testFrame = 2;
    Interval(8, 0);
    Check(callsThisFrame == 1, "actual all-threads-unavailable provider invoked once");
    Check(!g.haveProbe, "actual provider failure invalidates adjacency");
    kernelAvailable = true; kernelClocks = 20000;
    NpFpsProbeRegisterThread(1, 2);
    kernelClocks = 22000;
    testFrame = 3;
    Interval(8, 0);
    Check(callsThisFrame == 1 && g.haveProbe, "actual provider re-registration seeds once");
    Check(g.probePrev.cpuMs[0] == 10, "actual provider false samples do not advance cumulative totals");
    std::printf("PLATFORM cumulative=%.1f; unavailable interval not accumulated; registration required\n", g.probePrev.cpuMs[0]);
}
// wave 18 (MSC_FPS_PROBE_EVERY): the same synthetic game run with the probe read every picture (env 1) and once in N (env N); the shell script compares the RESULT lines.
// Checks inside: a window of N reads about 1 in N pictures; a picture that is longer than the one before, the picture after it, a scene change and a LAN session each read at once.
int64_t runningNs;
void IntervalR(double busyMs, int steps, int refreshes, unsigned scene = 2) {
    const double interval = refreshes * (1001.0 / 60000.0);
    runningNs += static_cast<int64_t>(std::llround(interval * 1e9));
    NpFpsSimBegin();
    for (int step = 0; step < steps; ++step) { NpFpsSimStep(); testThreadMs += 2; }
    NpFpsSimEnd();
    const int64_t wait = static_cast<int64_t>(std::llround((interval * 1000 - busyMs) * 1e6));
    testNowNs = runningNs - wait;
    NpFpsSwapWaitBegin();
    testNowNs = runningNs;
    NpFpsSwapWaitEnd();
    callsThisFrame = 0;
    NpFpsBeginFrame(static_cast<float>(interval), scene);
}
void Every() {
    const char* e = std::getenv("MSC_FPS_PROBE_EVERY");
    const int every = e ? std::atoi(e) : 1;
    Seed(CumulativeProbe);
    int sceneFrame = -1, promote = -1, down = -1, spike = -1, calls = 0, spikeReads = -1, afterReads = -1, sceneReads = -1, lanReads = 0, lanFrames = 0;
    for (testFrame = 1; testFrame <= 330; ++testFrame) {
        int refreshes = NpFpsPolicy() == npfps::kPolicy60 ? 1 : 2;   // (the picture length of the policy in force)
        simAccumulator += refreshes * (1001.0 / 60000.0);
        const int steps = static_cast<int>((simAccumulator + 1e-12) / .020);
        simAccumulator -= steps * .020;
        double busy = 6 + 2 * steps;
        unsigned scene = 2;
        if (spike < 0 && testFrame >= 30 && g.probeAge == 0) spike = testFrame;     // a long, heavy picture right after a read
        if (testFrame == spike) { busy = 40; refreshes = 4; }
        if (testFrame >= 40 && sceneFrame < 0 && g.probeAge == (every > 1 ? 1 : 0)) sceneFrame = testFrame;
        if (testFrame == sceneFrame) scene = 4;                                    // (a new scene for one picture)
        if (testFrame >= 220 && testFrame < 270) busy = 30;                          // the heavy stretch after the 60
        g_lan = testFrame >= 285 && testFrame < 293;
        cumulativeCpu += busy;
        IntervalR(busy, steps, refreshes, scene);
        calls += callsThisFrame;
        if (testFrame == spike) spikeReads = callsThisFrame;
        if (testFrame == spike + 1) afterReads = callsThisFrame;
        if (testFrame == sceneFrame) sceneReads = callsThisFrame;
        if (g_lan) { ++lanFrames; lanReads += callsThisFrame; }
        if (promote < 0 && NpFpsPolicy() == npfps::kPolicy60) promote = testFrame;
        if (promote > 0 && down < 0 && testFrame >= 220 && NpFpsPolicy() == npfps::kPolicy30) down = testFrame;
    }
    Check(promote > 0, "the controller went to 60 on a light stretch");
    Check(down > 0, "the controller came back to 30 on a heavy stretch");
    Check(spikeReads == 1, "a long heavy picture reads the probe at once");
    Check(afterReads == 1, "the picture after a long one reads the probe too");
    Check(sceneReads == 1, "a scene change reads the probe at once");
    Check(lanFrames == 8 && lanReads == 8, "a LAN session reads the probe every picture");
    if (every > 1) Check(calls * every * 10 <= 330 * 14, "with MSC_FPS_PROBE_EVERY=N the probe is read in at most 1.4 / N of the pictures (the forced reads included)");
    std::printf("RESULT every=%d promote=%d down=%d calls=%d spike=%d scene=%d\n", every, promote, down, calls, spike, sceneFrame);
}
}

extern "C" int64_t NpMonotonicNs() { return testNowNs; }
extern "C" int64_t NpVirtualNowNs() { return testNowNs; }
extern "C" int NpVirtualTimeActive() { return 0; }
extern "C" uint32_t NpVI_GetRetraceCount() { return static_cast<uint32_t>(testNowNs / 16683333); }
extern "C" unsigned long VIGetRetraceCount() { return NpVI_GetRetraceCount(); }
extern "C" void VIWaitForRetrace() { Check(false, "no real retrace wait in adapter fixture"); }
extern "C" void NpLatLatch() {}
extern "C" uint64_t NpLatHandedOverTag() { return 0; }
extern "C" uint64_t NpLatShownTag() { return 0; }
extern "C" float VgxGpuMinMs() { return 0; }
extern "C" int VgxMegaActive() { return 0; }
extern "C" void VgxPacedSceneHint(int) {}
extern "C" int VgxDynResGrace() { return 0; }
extern "C" int VgxDynResRoom() { return 0; }
bool NpLanActive() { return g_lan; }
extern "C" int sceKernelGetThreadInfo(SceUID id, SceKernelThreadInfo* out) {
    if (!kernelAvailable || id != 1) return -1;
    out->runClocks = kernelClocks;
    out->lastExecutedCpuId = 0;
    return 0;
}
extern "C" SceUID sceKernelGetThreadId() { return 1; }
extern "C" int sceKernelDelayThread(unsigned) { Check(false, "single-thread probe lock never contended"); return 0; }
// gt27 (MSC_FPS_PROBE_ASYNC, default 0): linked only; any call to them in the unit is a failure
extern "C" SceUID sceKernelCreateSema(const char*, unsigned, int, int, void*) { Check(false, "the async probe is off in the unit"); return -1; }
extern "C" int sceKernelWaitSema(SceUID, int, unsigned*) { Check(false, "the async probe is off in the unit"); return -1; }
extern "C" int sceKernelSignalSema(SceUID, int) { Check(false, "the async probe is off in the unit"); return -1; }
extern "C" SceUID sceKernelCreateThread(const char*, int (*)(SceSize, void*), int, unsigned, unsigned, int, void*) { Check(false, "the async probe is off in the unit"); return -1; }
extern "C" int sceKernelStartThread(SceUID, SceSize, void*) { Check(false, "the async probe is off in the unit"); return -1; }
extern "C" int sceKernelChangeThreadCpuAffinityMask(SceUID, int) { Check(false, "the async probe is off in the unit"); return -1; }

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    if (!std::strcmp(argv[1], "gap")) Gap();
    else if (!std::strcmp(argv[1], "failed-seed")) FailedSeed();
    else if (!std::strcmp(argv[1], "zero")) Zero();
    else if (!std::strcmp(argv[1], "platform-registration")) PlatformRegistration();
    else if (!std::strcmp(argv[1], "every")) Every();
    else return 2;
    std::printf("SUMMARY case=%s checks=%d failures=%d callbacks=%d\n", argv[1], checks, failures, callsTotal);
    return failures ? 1 : 0;
}
