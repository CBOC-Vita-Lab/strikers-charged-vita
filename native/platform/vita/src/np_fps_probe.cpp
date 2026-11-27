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
#include "np_vita_internal.h"
#include <psp2/kernel/threadmgr.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

namespace {
// gt27 (MSC_FPS_PROBE_ASYNC=1, default 0): the per-core probe (one sceKernelGetThreadInfo per registered thread, ~0.6 ms of the game thread every picture on the
// console, [gsplit-rest] probe) is read by a thread of its own on core 2, kicked by the game thread's probe call; the call returns the last finished reading (cumulative
// per core, as before) at once. What the controller hears is one picture older; nothing else reads the probe (np_fps.cpp: Auto::Predict and the free-run gate only).
struct AsyncSnap { double cpu[4], time[4]; int gameCore; int ok; };
AsyncSnap g_snap;
std::atomic<uint32_t> g_snapSeq{0};   // 0 = unready; odd = write in progress; even >= 2 = published seqlock
std::atomic<int> g_busy{0};
std::atomic<int> g_asyncState{0};     // 0 = uninit, 1 = initializing, 2 = disabled, 3 = enabled
SceUID g_sema = -1;
std::atomic<unsigned long> g_asyncCalls{0}, g_asyncWaited{0}, g_asyncReads{0}, g_asyncStale{0};
int ReadAll(NpFpsProbeOut* out);
int AsyncMain(SceSize, void*)
{
    // int28: move off busy Core 2 onto Core 3 when proven and not online-held; register before re-checking held/lost to avoid TOCTOU,
    // and keep low priority (0x10000100 + 20 = abs 180) so online retreat to Cores 1-2 never preempts player (160) or render (148).
    if (VitaThreads::Core3Proven && VitaThreads::Core3Proven() && !VitaThreads::g_core3HeldNow())
    {
        const SceUID self = sceKernelGetThreadId();
        if (sceKernelChangeThreadCpuAffinityMask(self, 0x00080000) >= 0)
        {
            VitaThreads::Core3Register(self);
            if (VitaThreads::g_core3HeldNow() || VitaThreads::g_core3LostNow())
            {
                sceKernelChangeThreadCpuAffinityMask(self, 0x00010000 << 2);
            }
            else
            {
                std::fprintf(stderr, "[fps] probe async thread placed on core 3 (prio 0x%X)\n", static_cast<unsigned>(0x10000100 + 20));
            }
        }
    }
    for (;;)
    {
        if (sceKernelWaitSema(g_sema, 1, nullptr) < 0) { sceKernelDelayThread(1000); continue; }
        NpFpsProbeOut o;
        std::memset(&o, 0, sizeof o);
        const int ok = ReadAll(&o);
        const uint32_t s = g_snapSeq.load(std::memory_order_relaxed);
        g_snapSeq.store(s + 1u, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for (int c = 0; c < 4; ++c) { g_snap.cpu[c] = o.cpuMs[c]; g_snap.time[c] = o.timeMs[c]; }
        g_snap.gameCore = o.gameCore;
        g_snap.ok = ok;
        std::atomic_thread_fence(std::memory_order_release);
        g_snapSeq.store(s + 2u, std::memory_order_release);
        g_asyncReads.fetch_add(1, std::memory_order_relaxed);
        g_busy.store(0, std::memory_order_release);
    }
    return 0;
}
bool AsyncStart()
{
    g_sema = sceKernelCreateSema("np_fpsprobe", 0, 0, 1, nullptr);
    if (g_sema < 0) return false;
    const SceUID th = sceKernelCreateThread("np_fpsprobe", AsyncMain, 0x10000100 + 20, 16 * 1024, 0, 0x00010000 << 2, nullptr);
    if (th < 0) return false;
    return sceKernelStartThread(th, 0, nullptr) >= 0;
}
} // namespace

int NpFpsPlatformProbe(NpFpsProbeOut* out)
{
    int state = g_asyncState.load(std::memory_order_acquire);
    if (state < 2)
    {
        int expected = 0;
        if (g_asyncState.compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
        {
            const char* e = std::getenv("MSC_FPS_PROBE_ASYNC");
            const bool want = (e && *e && *e != '0');
            if (want && !AsyncStart())
            {
                std::fprintf(stderr, "[fps] MSC_FPS_PROBE_ASYNC=1: the probe thread cannot start: the probe stays on the game thread\n");
                g_asyncState.store(2, std::memory_order_release);
            }
            else if (want)
            {
                std::fprintf(stderr, "[fps] probe async on (MSC_FPS_PROBE_ASYNC=1): the per-core reading is made off the game thread by its own thread; the controller hears the reading one picture older\n");
                g_asyncState.store(3, std::memory_order_release);
            }
            else
            {
                g_asyncState.store(2, std::memory_order_release);
            }
        }
        else
        {
            while ((state = g_asyncState.load(std::memory_order_acquire)) < 2) sceKernelDelayThread(50);
        }
        state = g_asyncState.load(std::memory_order_acquire);
    }
    if (state != 3) return ReadAll(out);
    const unsigned long calls = g_asyncCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (g_snapSeq.load(std::memory_order_acquire) == 0)
    {
        const int ok = ReadAll(out);   // (the first call: a reading now, then the thread takes over)
        if (g_busy.exchange(1, std::memory_order_acq_rel) == 0) sceKernelSignalSema(g_sema, 1);
        return ok;
    }
    if (g_busy.load(std::memory_order_acquire) != 0) g_asyncStale.fetch_add(1, std::memory_order_relaxed);
    AsyncSnap local{};
    bool got = false;
    for (int tries = 0; tries < 64; ++tries)
    {
        const uint32_t s0 = g_snapSeq.load(std::memory_order_acquire);
        if (s0 & 1u)
        {
#if defined(__arm__)
            __asm__ __volatile__("yield" ::: "memory");
#endif
            continue;
        }
        std::memcpy(&local, &g_snap, sizeof(local));
        std::atomic_thread_fence(std::memory_order_acquire);
        if (g_snapSeq.load(std::memory_order_relaxed) == s0) { got = true; break; }
    }
    if (!got) return ReadAll(out);
    for (int c = 0; c < 4; ++c) { out->cpuMs[c] = local.cpu[c]; out->timeMs[c] = local.time[c]; }
    out->gameCore = local.gameCore;
    const int ok = local.ok;
    if (g_busy.exchange(1, std::memory_order_acq_rel) == 0) sceKernelSignalSema(g_sema, 1);
    if (calls % 3600 == 0)
        std::fprintf(stderr, "[fps] probe async: %lu calls, %lu readings by the probe thread, %lu calls waited for the last reading, %lu heard an older reading again (stale)\n",
                     calls, g_asyncReads.load(std::memory_order_relaxed), g_asyncWaited.load(std::memory_order_relaxed), g_asyncStale.load(std::memory_order_relaxed));
    return ok;
}

namespace {
int ReadAll(NpFpsProbeOut* out)
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
} // namespace

// CPU time of the calling thread in milliseconds (the game thread's simulation cost, np_fps.cpp NpFpsSimBegin/End).
double NpFpsPlatformThreadClockMs(void)
{
    SceKernelThreadInfo info;
    if (!Info(sceKernelGetThreadId(), info)) return 0.0;
    return (double)info.runClocks * 1e-3;
}

} // extern "C"
#endif
