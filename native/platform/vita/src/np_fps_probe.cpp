// What each core spent, for MSC_FPS=auto (np/docs/wave5-FPS.md; platform/compat/np_fps.cpp reads it through NpFpsPlatformProbe, found as a weak symbol).
//
// Every thread the player starts and pins (VitaThreads::PinCurrentThread: game, render, workers, frame send, audio, helpers; np_plat_vita.cpp apply_role: the
// SDK's OS threads, the timers) is registered here with a kind. A probe reads sceKernelGetThreadInfo for each of them (one syscall each, once per drawn frame),
// takes the growth of runClocks (microseconds) since the previous probe and adds it to the core the thread last ran on. Cumulative per core, as NpFpsProbeOut says:
//   kind 2  the game (the OS threads run one at a time on the game's core): counted on its core; the core of the game thread is reported as gameCore
//   kind 1  threads whose work follows time, not frames (audio output, the AX mixer, movie decoding, timers): counted, and also in timeMs
//   kind 0  per-frame threads (render, workers, frame send, vitaGL's collector): counted
// A thread that has ended drops out. Nothing here changes how the game runs; with MSC_FPS=30 (the default) the probe is only read at the first frame.
// (the platform layer is compiled without the game's NP_NATIVE; the interface of include/np/fps.h is the native one)
#ifndef NP_NATIVE
#define NP_NATIVE 1
#endif
#include "np/fps.h"

#if defined(__vita__)
#include <psp2/kernel/threadmgr.h>

#include <atomic>
#include <cstdint>
#include <cstring>

namespace {

struct Thread {
    SceUID id;
    int kind;
    uint64_t prev;   // runClocks at the previous probe (microseconds)
};
constexpr int kMax = 48;
Thread g_threads[kMax];
int g_count = 0;
std::atomic_flag g_lock = ATOMIC_FLAG_INIT;
double g_cum[4] = {0, 0, 0, 0}, g_time[4] = {0, 0, 0, 0};
int g_gameCore = 0;

struct Guard {
    Guard() { while (g_lock.test_and_set(std::memory_order_acquire)) sceKernelDelayThread(50); }
    ~Guard() { g_lock.clear(std::memory_order_release); }
};

bool Info(SceUID id, SceKernelThreadInfo& info) {
    std::memset(&info, 0, sizeof info);
    info.size = sizeof info;
    return sceKernelGetThreadInfo(id, &info) >= 0;
}

} // namespace

extern "C" {

// kind: 2 game, 1 follows time, 0 per frame. Called with the calling thread's id (also again when a thread is pinned a second time).
void NpFpsProbeRegisterThread(int tid, int kind)
{
    Guard guard;
    for (int i = 0; i < g_count; ++i)
        if (g_threads[i].id == tid) { g_threads[i].kind = kind; return; }
    if (g_count >= kMax) return;
    SceKernelThreadInfo info;
    if (!Info(tid, info)) return;
    g_threads[g_count].id = tid;
    g_threads[g_count].kind = kind;
    g_threads[g_count].prev = info.runClocks;
    ++g_count;
}

int NpFpsPlatformProbe(NpFpsProbeOut* out)
{
    Guard guard;
    for (int i = 0; i < g_count;)
    {
        SceKernelThreadInfo info;
        if (!Info(g_threads[i].id, info)) { g_threads[i] = g_threads[--g_count]; continue; }   // the thread has ended
        const uint64_t run = info.runClocks;
        const double ms = run > g_threads[i].prev ? (double)(run - g_threads[i].prev) * 1e-3 : 0.0;
        g_threads[i].prev = run;
        int core = info.lastExecutedCpuId;
        if (core < 0 || core > 3) core = 0;
        g_cum[core] += ms;
        if (g_threads[i].kind == 1) g_time[core] += ms;
        if (g_threads[i].kind == 2) g_gameCore = core;
        ++i;
    }
    for (int c = 0; c < 4; ++c) { out->cpuMs[c] = g_cum[c]; out->timeMs[c] = g_time[c]; }
    out->gameCore = g_gameCore;
    return g_count > 0 ? 1 : 0;
}

// CPU time of the calling thread in milliseconds (the game thread's simulation cost, np_fps.cpp NpFpsSimBegin/End).
double NpFpsPlatformThreadClockMs(void)
{
    SceKernelThreadInfo info;
    if (!Info(sceKernelGetThreadId(), info)) return 0.0;
    return (double)info.runClocks * 1e-3;
}

} // extern "C"
#endif
