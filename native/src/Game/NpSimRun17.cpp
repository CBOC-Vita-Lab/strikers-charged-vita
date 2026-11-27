// NpSimRun17.cpp (NP, block17 = plan-simthread17 stage 2a): the simulation's step loop on its own thread `np_sim` while the game thread BLOCKS.
//
//   MSC_SIMT=1     (default 0 in the code and in the Vita plan) FixedUpdateTask::Run hands its step loop to np_sim and waits for it. No overlap, so no gain (expect a
//                  few tenths of a ms lost): the point is to prove what the step needs of its thread before anything runs beside it: the game-heap identity
//                  (NpMemSetGameThread), the caller's floating-point mode, the frame-send gates, the stack, TLS-free probes. Same machine code, same order,
//                  one thread at a time: the det trace, the replay hash and the frame hashes must equal MSC_SIMT=0.
//   MSC_SIMT=2     (overlap18, default 0) the same thread, but the game thread does NOT wait: after the kick it draws the picture's window (mapi17's seven stages: world NPCs and the
//                  crowd, src/Game/NpSimOrder17.cpp) and joins when that is done; the rest of the picture is drawn after the join, as today. The result must equal MSC_SIMT=0
//                  MSC_SIMT_ORDER=B (the same window, before the step, one thread): det trace, replay hash (heap pointers masked), frame hashes. Needs MSC_RENDER_RNG=2 (the window's draws
//                  on the game thread then take the render stream, np_sim's the simulation stream: the np_sim stack is given to NpRngSetSimStack before the first kick), MSC_SIMT_ORDER unset
//                  or B, and a frame-send mode that is off or `thread`; otherwise it is refused (one log line) and the game runs as MSC_SIMT=0. Refused in LAN / online / recorded games like 1.
//                  Heap, FPSCR, virtual clock, frame-send gates: as for 1; while both threads run the frame-send module takes np_sim's frees and gates on their own rules (np_framesend.cpp, g_ovl).
//   MSC_SIMT_JITTER=<seed>[:<max us>]  (test, MSC_SIMT=2 + MSC_GSPLIT=1) both threads yield or spin a seeded random time at the phase marks (NP_GS): an interleave stress.
//   MSC_SIMT_LAN=1 allow it in LAN / online / recorded games (default: refused there, the step then runs inline; one log line per refusal run).
//   MSC_SIMT_STACK_KB=<n>  stack of np_sim (default 4096 = the game thread's, 1024 with MSC_SIMT=2; the high-water line says how far it can be trimmed).
//   MSC_VITA_CORE_SIM=<cores>  (pace18) Vita: cores of np_sim, READ by VitaThreads (platform/vita/src/vita_threads.cpp, role kSim; digits 0..2, "12" = cores 1 and 2, never 3). MSC_SIMT_CORE is its alias (same value; MSC_VITA_CORE_SIM wins).
//                  Default: MSC_SIMT=1 the game thread's own core (the blocked game thread leaves it idle), MSC_SIMT=2 core 2. MSC_VITA_PRIO_SIM=<n> / alias MSC_SIMT_PRIO: priority (default: MSC_SIMT=1 the game thread's, MSC_SIMT=2 one more urgent than the job workers).
//   MSC_SIMT_SPIN_US=<n>   np_sim spins this long for the next kick before it sleeps (default 80); MSC_SIMT_POLL_US=<n>: the joining thread's sleep between polls (default 100).
//   MSC_SIMT_BREAK=1|2     test mutants, never set in play: 1 = np_sim does not mark itself as a game-heap thread (its allocations go to the host heap: the
//                          `host-heap allocations` counter must be > 0); 2 = np_sim runs with the rounding mode toward zero (the det trace must differ); 3 (MSC_SIMT=2) = NpRngSetSimStack is not called (the window and the step then
//                          draw from the wrong streams: det and frame hashes must differ from order B).
//   Line: `[simt] ...` every 2000 steps and at exit: steps, kicks, join wait p50/max (us), stack high-water, GX records on the sim thread, host-heap allocations on it.
#ifdef NP_NATIVE
#include "np/simrun17.h"
#include "np/simwake17.h"
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#if defined(__vita__)
#include <psp2/kernel/threadmgr.h>
#endif

bool IsNetworkOrRecordedGame();   // InputManager.cpp
bool NpLanActive() __attribute__((weak));
extern "C" void NpMemSetGameThread(int on);   // NL/nlMemory.cpp
extern "C" unsigned long long NpMemHeapLockSlow(void) __attribute__((weak));   // NL/nlMemory.cpp (np/lockwait.h)
extern "C" double NpRealSeconds(void) __attribute__((weak));   // never virtual
extern "C" void NpVirtualAdoptGameThread(int on) __attribute__((weak));   // np_vclock.cpp (host harness): the virtual clock ticks for np_sim as for the game thread
extern "C" void NpHarnessAtExit(void (*fn)(void)) __attribute__((weak));
extern "C" pthread_t NpFsSwapGameThread(pthread_t t) __attribute__((weak));   // np_framesend.cpp: whose join the frame-send gates ask for
extern "C" void NpFsSetOverlap(int on) __attribute__((weak));
extern "C" void NpFpsSimOverlapNote(int mode, double windowUs, double joinUs, double simWallUs) __attribute__((weak));   // np_fps.cpp (pace18)
extern "C" int NpVitaPinSimThread(int level, unsigned* coreMask) __attribute__((weak));   // platform/vita/src/vita_threads.cpp (pace18): pin + priority of np_sim by role kSim   // np_framesend.cpp (overlap18): np_sim runs beside the game thread between kick and join
extern "C" int NpFsMode(void) __attribute__((weak));   // np_framesend.cpp: 0 off, 1 thread, 2 serial, 3 inline
extern "C" void NpRngSetSimStack(void* lo, void* hi);   // NpSimThread.cpp (rng17)
extern "C" void (*g_npGsMarkHook)(int);   // platform/compat/np_gsplit.cpp
extern "C" int NpSimOrder17RunPending(void);   // NpSimOrder17.cpp: the armed window's stages, on the calling thread

extern "C" volatile int gNpSimtCensus = 0;

namespace
{
struct Fp
{
    uint32_t a, b;
};

Fp ReadFp()
{
    Fp f = {0, 0};
#if defined(__arm__)
    uint32_t v;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(v));
    f.a = v & 0x07C09F00u;   // the lanes' mask (np_jobs.cpp): rounding, flush-to-zero, default NaN, exception enables, vector length
#elif defined(__i386__) || defined(__x86_64__)
    uint32_t mx = 0;
    uint16_t cw = 0;
    __asm__ volatile("stmxcsr %0" : "=m"(mx));
    __asm__ volatile("fnstcw %0" : "=m"(cw));
    f.a = mx & 0xFFC0u;
    f.b = cw;
#endif
    return f;
}

void ApplyFp(Fp f, bool mutate)
{
#if defined(__arm__)
    uint32_t cur;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(cur));
    uint32_t want = f.a;
    if (mutate) want |= 3u << 22;   // rounding toward zero
    const uint32_t mixed = (cur & ~0x07C09F00u) | want;
    if (mixed != cur) __asm__ volatile("vmsr fpscr, %0" ::"r"(mixed));
#elif defined(__i386__) || defined(__x86_64__)
    uint32_t mx = 0;
    __asm__ volatile("stmxcsr %0" : "=m"(mx));
    mx = (mx & ~0xFFC0u) | f.a;
    uint16_t cw = (uint16_t)f.b;
    if (mutate)
    {
        mx |= 0x6000u;   // RC = toward zero
        cw |= 0x0C00u;
    }
    __asm__ volatile("ldmxcsr %0" ::"m"(mx));
    __asm__ volatile("fldcw %0" ::"m"(cw));
#else
    (void)f;
    (void)mutate;
#endif
}

struct State
{
    int init = 0;
    int on = 0;           // MSC_SIMT
    int lanOk = 0;        // MSC_SIMT_LAN
    int breakMode = 0;    // MSC_SIMT_BREAK
    unsigned jitterSeed = 0, jitterMaxUs = 40;   // MSC_SIMT_JITTER
    unsigned long long overlapKicks = 0, overlapJoinWaited = 0, overlapNoWindow = 0;
    unsigned winUs[2048];
    unsigned nWin = 0, maxWin = 0;
    unsigned spinUs = 80, pollUs = 100;
    size_t stackBytes = 4096u * 1024u;
    unsigned coreMask = 0;   // the cores np_sim was pinned to (one bit per core, set by the pin hook; 0 = not known), np_fps.cpp reads it
    // thread
    pthread_t thread;
    int started = 0, failed = 0;
    std::atomic<int> ready{0};
    NpSimWake wake;   // kickGen, doneGen, simSleeping, mutex, cond (np/simwake17.h: the hand-shake and its ordering)
    NpSimtFn fn = nullptr;
    void* ctx = nullptr;
    Fp fp = {0, 0};
    volatile int phase = 0;
    uintptr_t stkLo = 0, stkHi = 0;   // address range of np_sim's stack (the on-sim-thread test)
    unsigned char* paintLo = nullptr;
    size_t paintSize = 0;
    // statistics
    unsigned long long kicks = 0, steps = 0, inlineRuns = 0, refused = 0, refusedLogged = 0;
    std::atomic<unsigned long long> hostAllocs{0}, gameAllocs{0}, gxRecords{0};
    unsigned waitUs[2048];
    unsigned nWait = 0, maxWait = 0, winKicks = 0;
    unsigned simUs[2048];                 // pace18: np_sim's wall time of each step run (a kick)
    unsigned nSim = 0, maxSim = 0;
    unsigned long long simUsTotal = 0, simKicksTotal = 0, joinWaitedBlk = 0;
    std::atomic<unsigned> lastSimUs{0};   // written by np_sim before its release store of doneGen
    unsigned long long nextLine = 2000;
    unsigned long long lastLineSteps = 0;
    int atexitSet = 0;
};
State g;
unsigned long sJitMarksGame, sJitMarksSim, sJitYields, sJitSpins;   // (each written by one thread at a time per field except the shared totals: plain counters, test only)

double Now() { return NpRealSeconds ? NpRealSeconds() : 0.0; }

void ReadEnv()
{
    g.init = 1;
    const char* e = getenv("MSC_SIMT");
    g.on = (e != nullptr && e[0] != 0 && e[0] != '0') ? 1 : 0;
    if (g.on && e[0] == '2')
    {
        g.on = 2;
        const char* rr = getenv("MSC_RENDER_RNG");
        const char* od = getenv("MSC_SIMT_ORDER");
        if (rr == nullptr || rr[0] != '2')
        {
            fprintf(stderr, "[simt] MSC_SIMT=2 refused: it needs MSC_RENDER_RNG=2 (the window and the step draw random numbers at the same time); running as MSC_SIMT=0\n");
            g.on = 0;
        }
        else if (od != nullptr && od[0] != 0 && od[0] != 'B' && od[0] != 'b' && od[0] != '1')
        {
            fprintf(stderr, "[simt] MSC_SIMT=2 refused: MSC_SIMT_ORDER=%s (it must be unset or B: the overlap is order B); running as MSC_SIMT=0\n", od);
            g.on = 0;
        }
    }
    e = getenv("MSC_SIMT_LAN");
    g.lanOk = (e != nullptr && e[0] == '1') ? 1 : 0;
    e = getenv("MSC_SIMT_BREAK");
    g.breakMode = e != nullptr ? atoi(e) : 0;
    if (g.on == 2) g.stackBytes = 1024u * 1024u;   // plan 1.2: the step's high-water mark is about 56 KiB (host, poisoned): 1 MiB is generous; MSC_SIMT_STACK_KB overrides
    if ((e = getenv("MSC_SIMT_STACK_KB")) != nullptr && atoi(e) >= 256) g.stackBytes = (size_t)atoi(e) * 1024u;
    if ((e = getenv("MSC_SIMT_SPIN_US")) != nullptr) g.spinUs = (unsigned)atoi(e);
    if ((e = getenv("MSC_SIMT_POLL_US")) != nullptr) g.pollUs = (unsigned)atoi(e);
    if ((e = getenv("MSC_SIMT_JITTER")) != nullptr && e[0] != 0)
    {
        g.jitterSeed = (unsigned)strtoul(e, nullptr, 0);
        if (g.jitterSeed == 0) g.jitterSeed = 1;
        const char* c = strchr(e, ':');
        if (c != nullptr && atoi(c + 1) > 0) g.jitterMaxUs = (unsigned)atoi(c + 1);
    }
}

// nearest-rank percentile of the first min(n, 2048) samples (pct 1..100); 0 when there are none
unsigned PctOf(const unsigned* a, unsigned n, int pct)
{
    if (n == 0) return 0;
    if (n > 2048) n = 2048;
    unsigned tmp[2048];
    memcpy(tmp, a, n * sizeof(unsigned));
    unsigned rank = (n * (unsigned)pct + 99u) / 100u;
    if (rank < 1) rank = 1;
    if (rank > n) rank = n;
    std::nth_element(tmp, tmp + (rank - 1), tmp + n);
    return tmp[rank - 1];
}

void PrintLine(const char* tag)
{
    unsigned p50 = 0, p95 = 0;
    if (g.nWait != 0)
    {
        const unsigned n = g.nWait < 2048 ? g.nWait : 2048;
        unsigned tmp[2048];
        memcpy(tmp, g.waitUs, n * sizeof(unsigned));
        std::nth_element(tmp, tmp + n / 2, tmp + n);
        p50 = tmp[n / 2];
        p95 = PctOf(g.waitUs, g.nWait, 95);
    }
    const unsigned s50 = PctOf(g.simUs, g.nSim, 50), s95 = PctOf(g.simUs, g.nSim, 95);
    size_t high = 0;
    if (g.paintLo != nullptr)
    {
        size_t i = 0;
        while (i < g.paintSize && g.paintLo[i] == 0xA5) ++i;
        high = g.paintSize - i;   // bytes below the entry sp that the thread touched (the painted region only)
    }
    unsigned w50 = 0;
    if (g.nWin != 0)
    {
        const unsigned n = g.nWin < 2048 ? g.nWin : 2048;
        unsigned tmp[2048];
        memcpy(tmp, g.winUs, n * sizeof(unsigned));
        std::nth_element(tmp, tmp + n / 2, tmp + n);
        w50 = tmp[n / 2];
    }
    if (g.on == 2 && g.jitterSeed != 0)
        fprintf(stderr, "[simt] jitter: phase marks seen on the game thread %lu, on np_sim %lu (np_sim yielded %lu, spun %lu times)\n", sJitMarksGame, sJitMarksSim, sJitYields, sJitSpins);
    if (g.on == 2)
        fprintf(stderr, "[simt] overlap: kicks %llu, window on the game thread p50 %u max %u us, join waited (step longer than the window) in %llu, kicks without a window %llu\n", g.overlapKicks, w50, g.maxWin,
            g.overlapJoinWaited, g.overlapNoWindow);
    g.nWin = 0;
    g.maxWin = 0;
    fprintf(stderr, "[simt]%s steps %llu kicks %llu inline %llu refused %llu | join wait p50 %u p95 %u max %u us (%u kicks) | sim thread busy per kick p50 %u p95 %u max %u us, mean %.2f ms a step (np_sim wall) | stack high-water %lu KiB of %lu KiB | GX records on the sim thread %llu | host-heap allocations on the sim thread %llu (game-heap %llu) | heap-lock slow paths %llu%s\n", tag,
            g.steps, g.kicks, g.inlineRuns, g.refused, p50, p95, g.maxWait, g.winKicks, s50, s95, g.maxSim, g.steps != 0 ? (double)g.simUsTotal / 1000.0 / (double)g.steps : 0.0, (unsigned long)((high + 1023) / 1024), (unsigned long)(g.stackBytes / 1024), (unsigned long long)g.gxRecords.load(),
            (unsigned long long)g.hostAllocs.load(), (unsigned long long)g.gameAllocs.load(), NpMemHeapLockSlow ? NpMemHeapLockSlow() : 0ull, g.breakMode != 0 ? " | BREAK MUTANT ON" : "");
    g.nWait = 0;
    g.maxWait = 0;
    g.winKicks = 0;
    g.nSim = 0;
    g.maxSim = 0;
}

void AtExit()
{
    static int done = 0;
    if (done++ == 0 && (g.kicks != 0 || g.refused != 0)) PrintLine(" final");
}

void PinSimThread()
{
#if defined(__vita__)
    // pace18: the role table decides (vita_threads.cpp: role kSim, MSC_VITA_CORE_SIM with the alias MSC_SIMT_CORE, MSC_VITA_PRIO_SIM / MSC_SIMT_PRIO; defaults by MSC_SIMT level).
    unsigned mask = 0;
    if (NpVitaPinSimThread)
    {
        NpVitaPinSimThread(g.on, &mask);
        g.coreMask = mask;
        return;
    }
    // (a build without the role table: the game thread's own mask and priority; overlap on core 2)
    int m = g.on == 2 ? 0x00040000 : sceKernelGetThreadCpuAffinityMask(sceKernelGetThreadId());
    const int r = sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), m);
    const int prio = sceKernelGetThreadCurrentPriority();
    fprintf(stderr, "[simt] np_sim: affinity 0x%X (%s), priority 0x%X (kept; no role table)\n", (unsigned)m, r >= 0 ? "ok" : "REFUSED", (unsigned)prio);
    g.coreMask = (unsigned)(m >> 16) & 7u;
#endif
}

void* SimMain(void*)
{
    volatile char anchor = 0;
    const uintptr_t sp = (uintptr_t)&anchor;
    g.stkHi = sp + 4096;
    g.stkLo = sp > g.stackBytes ? sp - g.stackBytes : 0;
    if (g.stackBytes >= 128u * 1024u)
    {
        // paint the unused stack below this frame (the thread's TLS block and bookkeeping sit above it; 64 KiB of margin at the bottom) so the high-water mark can be read
        const size_t span = g.stackBytes - 64u * 1024u;
        unsigned char* lo = (unsigned char*)(sp - span);
        const size_t n = span - 4096u;
        memset(lo, 0xA5, n);
        g.paintLo = lo;
        g.paintSize = n;
    }
    PinSimThread();
    if (NpVirtualAdoptGameThread) NpVirtualAdoptGameThread(1);   // (the game thread blocks while np_sim runs: one clock reader at a time)
    if (g.breakMode != 1) NpMemSetGameThread(1);   // the step allocates from the game heap like the game thread does; without it every `new` goes to the host heap
    gNpSimtCensus = 1;
    if (g.on == 2 && g.breakMode != 3) NpRngSetSimStack((void*)g.stkLo, (void*)g.stkHi);   // MSC_RENDER_RNG=2: from here on a caller on this stack gets the simulation's random streams (before the first kick)
    {   // positive control of the heap identity: one request made here must be a game-heap request (counted as such); with MSC_SIMT_BREAK=1 it goes to the host heap and is counted as that
        volatile char* probe = new char[16];   // (volatile: the compiler may not drop a new/delete pair)
        probe[0] = 1;
        delete[] const_cast<char*>(probe);
    }
    g.ready.store(1, std::memory_order_release);
    uint32_t seen = 0;
    for (;;)
    {
        const double t0 = Now();
        while (g.wake.kickGen.load(std::memory_order_acquire) == seen)
        {
            if (g.spinUs == 0 || (Now() - t0) * 1e6 >= g.spinUs)
            {
                NpSimWakeSleep(g.wake, seen);
                break;
            }
        }
        seen = g.wake.kickGen.load(std::memory_order_acquire);
        ApplyFp(g.fp, g.breakMode == 2);
        g.phase = 1;
        const double ts0 = Now();
        g.fn(g.ctx);
        const double dtUs = (Now() - ts0) * 1e6;
        g.lastSimUs.store(dtUs < 0 ? 0u : (dtUs > 4e9 ? 4000000000u : (unsigned)dtUs), std::memory_order_relaxed);
        g.phase = 0;
        g.wake.doneGen.store(seen, std::memory_order_release);
    }
    return nullptr;
}

// MSC_SIMT_JITTER (test): a seeded random yield / spin at every phase mark of either thread (the window's and the step's NP_GS marks), so that the two threads meet at different points on every run.
// Each thread keeps its own generator (chosen by the stack test, no TLS): the sequence of one thread is the same on every run, the interleave is not.
unsigned sJitGame, sJitSim;
void JitterHook(int)
{
    const bool sim = NpSimtOnSimThread() != 0;
    unsigned& r = sim ? sJitSim : sJitGame;
    if (sim) ++sJitMarksSim; else ++sJitMarksGame;
    r ^= r << 13; r ^= r >> 17; r ^= r << 5;
    const unsigned x = r;
    if ((x & 7) == 0) { sched_yield(); if (sim) ++sJitYields; }
    if (((x >> 3) & 31) == 0)
    {
        if (sim) ++sJitSpins;
        const double end = (NpRealSeconds ? NpRealSeconds() : 0.0) + ((x >> 9) % (g.jitterMaxUs + 1)) * 1e-6;
        while (NpRealSeconds && NpRealSeconds() < end) {}
    }
}

// after a join, on the game thread: np_sim's wall time of the step it just ran (published by its release store of doneGen), the picture's window and the wait; feeds the [simt] line and np_fps.cpp (np_simpace18.h)
void NoteJoin(int mode, double windowUs, double joinUs)
{
    const unsigned su = g.lastSimUs.load(std::memory_order_relaxed);
    g.simUs[g.nSim++ & 2047] = su;
    if (su > g.maxSim) g.maxSim = su;
    g.simUsTotal += su;
    ++g.simKicksTotal;
    if (NpFpsSimOverlapNote) NpFpsSimOverlapNote(mode, windowUs, joinUs, (double)su);
}

bool StartThread()
{
    if (g.started) return true;
    if (g.failed) return false;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, g.stackBytes);
    const int rc = pthread_create(&g.thread, &attr, SimMain, nullptr);
    pthread_attr_destroy(&attr);
    if (rc != 0)
    {
        g.failed = 1;
        fprintf(stderr, "[simt] pthread_create failed (%d): the step stays on the game thread\n", rc);
        return false;
    }
    while (!g.ready.load(std::memory_order_acquire)) sched_yield();
    g.started = 1;
    if (!g.atexitSet)
    {
        g.atexitSet = 1;
        atexit(AtExit);
        if (NpHarnessAtExit) NpHarnessAtExit(AtExit);   // (the harness leaves by its own path: Finish())
    }
    if (g.on == 2 && g.jitterSeed != 0)
    {
        sJitGame = g.jitterSeed * 2654435761u + 1;
        sJitSim = g.jitterSeed * 40503u + 0x9E3779B9u;
        g_npGsMarkHook = JitterHook;
        fprintf(stderr, "[simt] jitter stress on: seed %u, up to %u us (needs MSC_GSPLIT=1 for the marks)\n", g.jitterSeed, g.jitterMaxUs);
    }
    fprintf(stderr, "[simt] np_sim started: stack %lu KiB (painted), spin %u us, poll %u us%s\n", (unsigned long)(g.stackBytes / 1024), g.spinUs, g.pollUs, g.breakMode != 0 ? ", BREAK MUTANT ON" : "");
    return true;
}

// MSC_SIMT_REFUSE_AT=<n> (test, simfix20): from the n-th completed kick on this game is "refused" as a LAN / recorded one would be (its own getenv, not ReadEnv):
// the proof of P3 (an inline step after np_sim exists draws the simulation's streams) needs a refusal that turns on in the middle of a run.
unsigned long long RefuseAt()
{
    static unsigned long long v = ~0ull;
    static int read = 0;
    if (!read)
    {
        read = 1;
        const char* e = getenv("MSC_SIMT_REFUSE_AT");
        if (e && *e) v = strtoull(e, nullptr, 10);
    }
    return v;
}

bool Allowed()
{
    if (g.kicks >= RefuseAt()) return false;
    if (g.lanOk) return true;
    if (IsNetworkOrRecordedGame()) return false;
    if (NpLanActive && NpLanActive()) return false;
    return true;
}

// A step run on the game thread after np_sim exists (refused: LAN / online / recorded; thread failed): NpRngSeedFor must decide by the step depth, as in MSC_SIMT=0,
// not by the stack range of np_sim (which would hand this thread the render stream inside the step: fable-verify10 P3). np_sim is idle here. MSC_SIMT_BREAK=4: no
// such switch (test mutant: the old behaviour).
void RunInline(NpSimtFn fn, void* ctx)
{
    const bool ranged = g.on == 2 && g.started && g.breakMode != 3 && g.breakMode != 4;
    if (ranged) NpRngSetSimStack(nullptr, nullptr);
    fn(ctx);
    if (ranged) NpRngSetSimStack((void*)g.stkLo, (void*)g.stkHi);
}

void SleepPoll()
{
#if defined(__vita__)
    sceKernelDelayThread(g.pollUs != 0 ? g.pollUs : 1);
#else
    sched_yield();   // (not usleep: with the virtual clock a sleep on the game thread would move the clock)
#endif
}
} // namespace

extern "C" {

int NpSimtOnSimThread(void)
{
    if (g.stkHi == 0) return 0;
    char probe;
    const uintptr_t a = (uintptr_t)&probe;
    return a >= g.stkLo && a < g.stkHi ? 1 : 0;
}

void NpSimtPhase(int phase) { g.phase = phase; }

// pace18: the MSC_SIMT level in force (0 off or refused at start-up, 1 blocking, 2 overlap). Reads the environment once; the thread placement defaults (vita_threads.cpp, np_jobs.cpp) ask it.
int NpSimtLevel(void)
{
    if (!g.init) ReadEnv();
    return g.on;
}

// pace18: the cores np_sim was pinned to, one bit a core (0 = not pinned / not known)
unsigned NpSimtCoreMask(void) { return g.coreMask; }

void NpSimtNoteHostAlloc(void)
{
    if (NpSimtOnSimThread()) g.hostAllocs.fetch_add(1, std::memory_order_relaxed);
}

void NpSimtNoteGameAlloc(void)
{
    if (NpSimtOnSimThread()) g.gameAllocs.fetch_add(1, std::memory_order_relaxed);
}

void NpSimtNoteGxRecord(void)
{
    if (NpSimtOnSimThread()) g.gxRecords.fetch_add(1, std::memory_order_relaxed);
}

// overlap18: MSC_SIMT=2 asked for and not refused at start-up (NpSimOrder17 then arms the window in order B and leaves the stages to NpSimtRun)
int NpSimtOverlapMode(void)
{
    if (!g.init) ReadEnv();
    return g.on == 2 ? 1 : 0;
}

// overlap18: will this picture's step run on np_sim beside the window? Asked by NpSimOrder17Early (game thread, before the kick). Makes the thread at the first call.
int NpSimtOverlapNow(void)
{
    if (!g.init) ReadEnv();
    if (g.on != 2) return 0;
    if (!Allowed())
    {
        ++g.refused;
        if (g.refusedLogged == 0)
        {
            g.refusedLogged = 1;
            fprintf(stderr, "[simt] refused in this game (LAN / online / recorded; MSC_SIMT_LAN=1 allows it): the step runs on the game thread, the window in its old place\n");
        }
        return 0;
    }
    if (NpFsMode != nullptr && NpFsMode() == 2)   // serial frame send: the job only runs at the game thread's join, np_sim could not wait for it
    {
        static int warned;
        if (!warned++) fprintf(stderr, "[simt] MSC_SIMT=2 refused for this run: MSC_FRAMESEND=serial (np_sim cannot wait for a job that runs at the join); the step stays on the game thread\n");
        return 0;
    }
    return StartThread() ? 1 : 0;
}

void NpSimtAddSteps(int steps)
{
    if (!g.started) return;
    g.steps += (unsigned long long)steps;
    if (g.steps >= g.nextLine)
    {
        PrintLine("");
        while (g.nextLine <= g.steps) g.nextLine += 2000;
    }
}

void NpSimtRun(NpSimtFn fn, void* ctx)
{
    if (!g.init) ReadEnv();
    if (!g.on)
    {
        fn(ctx);
        return;
    }
    if (!Allowed())
    {
        ++g.refused;
        ++g.inlineRuns;
        if (g.on == 2) NpSimOrder17RunPending();
        if (g.refusedLogged == 0)
        {
            g.refusedLogged = 1;
            fprintf(stderr, "[simt] refused in this game (LAN / online / recorded; MSC_SIMT_LAN=1 allows it): the step runs on the game thread\n");
        }
        RunInline(fn, ctx);
        return;
    }
    if (!StartThread())
    {
        ++g.inlineRuns;
        if (g.on == 2) NpSimOrder17RunPending();
        RunInline(fn, ctx);
        return;
    }
    if (g.on == 2)
    {
        if (NpFsMode != nullptr && NpFsMode() == 2)
        {
            NpSimOrder17RunPending();
            RunInline(fn, ctx);
            return;
        }
        // overlap18: kick, draw the window on this thread, join
        g.fn = fn;
        g.ctx = ctx;
        g.fp = ReadFp();
        if (NpFsSetOverlap) NpFsSetOverlap(1);   // (np_framesend.cpp: from here two threads use the game heap; the gates and heap hooks take np_sim's rules)
        const double t0 = Now();
        const uint32_t gen = g.wake.kickGen.load(std::memory_order_relaxed) + 1;
        NpSimWakeKick(g.wake, gen);
        if (!NpSimOrder17RunPending()) ++g.overlapNoWindow;   // the window (nothing when the picture armed none)
        const double t1 = Now();
        const double spinEnd = t1 + g.spinUs * 1e-6;
        double nextWarn = t0 + 2.0;
        unsigned warns = 0;
        bool waited = false;
        while (g.wake.doneGen.load(std::memory_order_acquire) != gen)
        {
            waited = true;
            const double t = Now();
            if (t >= spinEnd)
            {
                NpSimWakeResignal(g.wake, gen);   // (a lost wake-up of a sleeping np_sim costs one poll period, not a hang)
                SleepPoll();
            }
            if (t >= nextWarn)
            {
                ++warns;
                fprintf(stderr, "[simt] JOIN TIMEOUT: np_sim has run %.1f s in phase %d (kick %llu, overlap)\n", t - t0, (int)g.phase, g.kicks + 1);
                nextWarn = t + 2.0;
#if !defined(__vita__)
                if (warns >= 15)
                {
                    fflush(stderr);
                    _exit(97);
                }
#endif
            }
        }
        if (NpFsSetOverlap) NpFsSetOverlap(0);
        const double t2 = Now();
        const double wd = (t1 - t0) * 1e6, jw = (t2 - t1) * 1e6;
        const unsigned wdu = wd < 0 ? 0u : (wd > 4e9 ? 4000000000u : (unsigned)wd);
        const unsigned jwu = jw < 0 ? 0u : (jw > 4e9 ? 4000000000u : (unsigned)jw);
        g.winUs[g.nWin++ & 2047] = wdu;
        if (wdu > g.maxWin) g.maxWin = wdu;
        g.waitUs[g.nWait++ & 2047] = jwu;
        if (jwu > g.maxWait) g.maxWait = jwu;
        NoteJoin(2, wdu, jwu);
        ++g.winKicks;
        ++g.kicks;
        ++g.overlapKicks;
        if (waited && jwu > 50) ++g.overlapJoinWaited;
        return;
    }
    // hand-over: everything the step reads was written by this thread before the release store; everything it wrote is read after the acquire load below
    g.fn = fn;
    g.ctx = ctx;
    g.fp = ReadFp();   // the caller's floating-point mode, adopted by np_sim at every kick
    // while the game thread waits, np_sim is the thread that forks and gates the frame send (np_framesend.cpp keys its gates on one thread)
    const pthread_t oldOwner = NpFsSwapGameThread ? NpFsSwapGameThread(g.thread) : g.thread;
    const double t0 = Now();
    const uint32_t gen = g.wake.kickGen.load(std::memory_order_relaxed) + 1;
    NpSimWakeKick(g.wake, gen);
    const double spinEnd = t0 + g.spinUs * 1e-6;
    double nextWarn = t0 + 2.0;
    unsigned warns = 0;
    while (g.wake.doneGen.load(std::memory_order_acquire) != gen)
    {
        const double t = Now();
        if (t >= spinEnd)
            {
                NpSimWakeResignal(g.wake, gen);   // (a lost wake-up of a sleeping np_sim costs one poll period, not a hang)
                SleepPoll();
            }
        if (t >= nextWarn)
        {
            ++warns;
            fprintf(stderr, "[simt] JOIN TIMEOUT: np_sim has run %.1f s in phase %d (kick %llu)\n", t - t0, (int)g.phase, g.kicks + 1);
            nextWarn = t + 2.0;
#if !defined(__vita__)
            if (warns >= 15)   // lab builds stop (a half-run step cannot be abandoned); the player build keeps waiting
            {
                fflush(stderr);
                _exit(97);
            }
#endif
        }
    }
    if (NpFsSwapGameThread) NpFsSwapGameThread(oldOwner);
    const double w = (Now() - t0) * 1e6;
    const unsigned wu = w < 0 ? 0u : (w > 4e9 ? 4000000000u : (unsigned)w);
    g.waitUs[g.nWait++ & 2047] = wu;
    if (wu > g.maxWait) g.maxWait = wu;
    NoteJoin(1, 0.0, wu);
    ++g.winKicks;
    ++g.kicks;
}

} // extern "C"
#endif
