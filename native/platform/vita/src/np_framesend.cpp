// np_framesend.cpp: the frame-send worker (np_framesend.h has the contract and the environment).
//
// One persistent worker thread and one job slot. The game thread forks a job (the frame's send), goes on with the next frame's
// simulation and joins before it touches anything the send reads or writes (the gates of include/np/framesend.h, the task
// boundary of nlTask.cpp). The hand-over is two counting semaphores: POSIX sem_t on Linux (host32, armhf under qemu), kernel
// semaphores on the Vita (the recompiled build's split found that pthread-embedded's condition variable polls and lost
// wake-ups: split/NOTES.md 21:34). Memory order: a semaphore post / wait is a full barrier on both, so everything the game
// thread wrote before the fork is visible to the job and everything the job wrote is visible after the join.
//
// Modes: thread (the worker runs the job), serial (the job runs on the game thread at the join: the same reordered program on one
// thread), off (the job runs inside NpFsFork). All three go through the same gates, the same bookkeeping and the same marks, so
// the equality of their outputs separates "the program was reordered" from "two threads ran it".
#include "np_framesend.h"
#include "np_time.h"
#include "../../../include/np/touch_diag.h"
#pragma weak NpDiagActive
#pragma weak NpDiagNow
#pragma weak NpDiagFrame
#pragma weak NpDiagEvent

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#if defined(__linux__)
#include <execinfo.h>
#include <link.h>
#endif
#include <cstdlib>
#include <cstring>
#include <pthread.h>

#if defined(__vita__)
#include "np_vita_internal.h"
#include <psp2/kernel/threadmgr.h>
#elif defined(__linux__)
#include <semaphore.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

extern "C" {
volatile int NpFsBusy = 0;
volatile int NpFsPipeOn = 0;
void NpGlSendLogStats(void) __attribute__((weak));   // src/NL/gl/glPlat.cpp
}

#ifndef NP_FS_PIPE_DEFAULT
#define NP_FS_PIPE_DEFAULT 0   // -DNP_FS_PIPE_DEFAULT=1 makes the two-stage pipeline the default of a build (MSC_FS_PIPE=0/1 still overrides)
#endif
#ifndef NP_FS_DEFAULT_MODE
#define NP_FS_DEFAULT_MODE NP_FS_OFF   // -DNP_FS_DEFAULT_MODE=1 (NP_FS_THREAD) makes the second core the default of a build; MSC_FRAMESEND=off|thread|... still overrides
#endif

#if defined(__linux__) && !defined(__vita__)
extern "C" __attribute__((weak)) void vgxn_set_producer_hook(void (*fn)(void));   // only in a vgx library built with -DVGX_PRODUCER_CHECK (MSC_FS_GXCENSUS)
extern "C" char __executable_start, etext;
#endif

namespace {
void CensusInstall();
void CensusReport();
void FlushCalls();

constexpr int kWinSlots = 32768;           // window set slots (a power of two); at most kWinSlots / 2 inserts per window, the rest is "unknown" (its free is deferred)
constexpr int kMaxDeferred = 16384;
constexpr int kMaxDeferredPipe = 65536;    // the pipeline's window is a whole frame build (W5 PIPE)
constexpr int kMaxWrites = 512;
constexpr int kCallBytes = 32768;          // NpFsDeferCall: room for the deferred GX calls of one window
constexpr int kMaxGates = 16;

struct Sem {
#if defined(__vita__)
    SceUID id = -1;
    void Init(const char* name) { id = sceKernelCreateSema(name, 0, 0, 1, nullptr); }
    void Post() { sceKernelSignalSema(id, 1); }
    // seconds < 0: wait for ever; returns true when the semaphore was taken
    bool Wait(double seconds) {
        if (seconds < 0) return sceKernelWaitSema(id, 1, nullptr) >= 0;
        SceUInt us = static_cast<SceUInt>(seconds * 1e6);
        return sceKernelWaitSema(id, 1, &us) >= 0;
    }
    bool TryWait() {
        SceUInt us = 0;
        return sceKernelWaitSema(id, 1, &us) >= 0;
    }
#else
    sem_t s;
    void Init(const char*) { sem_init(&s, 0, 0); }
    void Post() { sem_post(&s); }
    bool Wait(double) {   // (no timed wait: the harness' virtual clock is interposed under libc's sem_timedwait)
        while (sem_wait(&s) != 0 && errno == EINTR) {}
        return true;
    }
    bool TryWait() {   // MSC_FS_NOTRY=1: always take the blocking wait (valgrind's helgrind does not know sem_trywait: the happens-before edge of the job's end would be lost)
        static const bool noTry = std::getenv("MSC_FS_NOTRY") != nullptr;
        return !noTry && sem_trywait(&s) == 0;
    }
#endif
};

struct State {
    bool decided = false;
    int mode = NP_FS_OFF;
    int states = 0x2;
    int freeGate = 1;
    int taskMask = 0;
    bool pipe = false;               // W5 PIPE: two-stage pipeline (MSC_FS_PIPE=1)
    int maxDeferred = kMaxDeferred;
    bool marks = false;
    size_t stackBytes = 0;

    bool started = false;
    pthread_t thread;
    Sem ready, done;
    NpFsJobFn job = nullptr;
    void* arg = nullptr;
    bool pending = false;
    pthread_t gameThread;            // the thread that forks (the game thread): only it joins and gates
    bool haveGameThread = false;
    void (*afterJoin)(void) = nullptr;

    // window bookkeeping (game thread only)
    // blocks allocated inside the window: an open-addressing set with a generation byte per slot (no clearing between windows);
    // the key 1 marks an erased slot. (Pipe windows are a whole frame build: a linear list made every free of an old block scan it all.)
    uintptr_t* winKey = nullptr;
    uint8_t* winGen = nullptr;
    uint8_t winCur = 1;
    int nWinAllocs = 0;              // live entries (inserted minus erased), for the statistics
    int winInserted = 0;             // inserts in this window (the table takes kWinSlots / 2)
    bool winOverflow = false;
    int maxWinInserted = 0;
    uint64_t winOverflows = 0;
    void** deferred = nullptr;       // blocks freed while the job ran (MSC_FS_FREEGATE=2): freed right after the join
    int nDeferred = 0;
    void (*freeFn)(void*) = nullptr;
    uint64_t deferredTotal = 0, deferredMax = 0, deferOverflow = 0;
    // NpFsDeferCall: GX calls the game thread makes while a job is pending, kept (in order, arguments copied) and made right after the join
    // (the recorder has ONE producer; made at the join they sit in the stream exactly where the serial program put them)
    alignas(8) unsigned char callBuf[kCallBytes];
    int callUsed = 0, nCalls = 0;
    uint64_t callsDeferred = 0, callOverflows = 0;
    void** writeSlot[kMaxWrites];    // NpFsDeferWrite: stores kept until the join
    void* writeVal[kMaxWrites];
    int nWrites = 0;
    uint64_t writesDeferred = 0, writeOverflows = 0;

    // statistics
    uint64_t forks = 0, joins = 0, joinsWaited = 0, joinsTask = 0;
    double waitSeconds = 0, maxWait = 0, workerBusySeconds = 0;
    uint64_t heapAllocsInWindow = 0, heapFreesInWindow = 0, freesOfWindowBlocks = 0, workerHeapCalls = 0;
    struct Gate { const char* why; uint64_t n; };
    Gate gates[kMaxGates];
    int nGates = 0;
    // worker stack high-water mark (Linux: painted at start)
    unsigned char* stackLow = nullptr;
    size_t stackSize = 0;
};
State g;
thread_local int t_onWorker = 0;

void Mark(long v) {
#if defined(__linux__)
    if (g.marks) syscall(0x7f00, v);
#else
    (void)v;
#endif
}

const char* ModeName(int mode) {
    return mode == NP_FS_THREAD ? "thread" : mode == NP_FS_SERIAL ? "serial" : mode == NP_FS_INLINE ? "inline" : "off";
}

bool EnvFlag(const char* name, bool def) {
    const char* e = std::getenv(name);
    if (!e || !*e) return def;
    return *e != '0';
}

void Decide() {
    if (g.decided) return;
    g.decided = true;
    const char* m = std::getenv("MSC_FRAMESEND");
    if (m && (!std::strcmp(m, "thread") || !std::strcmp(m, "1") || !std::strcmp(m, "on"))) g.mode = NP_FS_THREAD;
    else if (m && !std::strcmp(m, "serial")) g.mode = NP_FS_SERIAL;
    else if (m && !std::strcmp(m, "inline")) g.mode = NP_FS_INLINE;
    else if (m && *m) g.mode = NP_FS_OFF;     // (MSC_FRAMESEND set to anything else: off)
    else g.mode = NP_FS_DEFAULT_MODE;       // not set: the build's default (off unless built with -DNP_FS_DEFAULT_MODE=1 for thread)
    if (const char* s = std::getenv("MSC_FS_STATES")) g.states = static_cast<int>(std::strtoul(s, nullptr, 0));
    g.freeGate = 2;   // frees of old blocks wait for the join (a6/a7 proof runs: equal; 1 = join at the free costs two thirds of the overlap)
    if (const char* f = std::getenv("MSC_FS_FREEGATE")) g.freeGate = static_cast<int>(std::strtol(f, nullptr, 0));
    uint32_t mask = ~0u & ~(0xFFu << 9);   // all priorities except 9..16 (world update .. end frame)
    // W5 PIPE: the two-stage pipeline (send of frame N beside the build of frame N+1). Needs a mode that forks; the job is joined at the next
    // fork (glPlat.cpp) or at a gate, not at the task boundary; the default states are every state but the loading task's (2.5 of wave3-NPP2.md).
    if (g.mode != NP_FS_OFF) {
        const char* pe = std::getenv("MSC_FS_PIPE");
        g.pipe = EnvFlag("MSC_FS_PIPE", NP_FS_PIPE_DEFAULT != 0);
        (void)pe;
    }
    if (g.pipe) {
        mask = ~0u;
        if (!std::getenv("MSC_FS_STATES")) g.states = 0x3FFFFF & ~0x200000;
        g.maxDeferred = kMaxDeferredPipe;
        NpFsPipeOn = 1;
    }
    if (const char* t = std::getenv("MSC_FS_TASKS")) mask = static_cast<uint32_t>(std::strtoul(t, nullptr, 0));
    g.taskMask = static_cast<int>(mask);
    if (g.mode != NP_FS_OFF) CensusInstall();
    g.marks = EnvFlag("MSC_FS_MARKS", false) || std::getenv("MSC_QEMU_MARK");
#if defined(__vita__)
    g.stackBytes = 256 * 1024;
#else
    g.stackBytes = 1024 * 1024;
#endif
    if (const char* k = std::getenv("MSC_FS_STACK_KB")) g.stackBytes = static_cast<size_t>(std::strtoul(k, nullptr, 0)) * 1024;
    if (g.mode != NP_FS_OFF)
        std::fprintf(stderr, "[fs] frame send: mode=%s%s states=0x%X freegate=%d task-mask=0x%X stack=%lu KiB\n", ModeName(g.mode), g.pipe ? " (two-stage pipeline)" : "",
                     g.states, g.freeGate, static_cast<unsigned>(g.taskMask), static_cast<unsigned long>(g.stackBytes / 1024));
}

// The floating-point control register (rounding mode, flush-to-zero, default NaN) of the thread that forks: the job runs under the same
// settings as it would have on the game thread (a new thread's default is the platform's business; the recompiled build's fork does the same,
// split/NOTES.md). Control bits only: the flags and the cumulative exception bits are not copied.
#if defined(__arm__) && !defined(__SOFTFP__)
inline uint32_t ReadFpscr() { uint32_t v; __asm__ volatile("vmrs %0, fpscr" : "=r"(v)); return v & 0x07FF9F00u; }
inline void WriteFpscr(uint32_t v) {
    uint32_t cur;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(cur));
    cur = (cur & ~0x07FF9F00u) | (v & 0x07FF9F00u);
    __asm__ volatile("vmsr fpscr, %0" :: "r"(cur));
}
#else
inline uint32_t ReadFpscr() { return 0; }
inline void WriteFpscr(uint32_t) {}
#endif
uint32_t g_forkFpscr = 0;   // read by the game thread at the fork, applied by the worker at the start of the job

void RunJob(NpFsJobFn fn, void* arg) {
    const bool diag=NpDiagActive&&NpDiagActive();
    const uint64_t diagStart=diag?NpDiagNow():0;
    const uint64_t diagFrame=diag?NpDiagFrame()-1:0;
    const int was = t_onWorker;
    t_onWorker = 1;
    if (was) WriteFpscr(g_forkFpscr);   // (thread mode: only the worker thread has t_onWorker set before a job starts)   // a job runs under the worker's rules wherever it runs (the gates do not join, the heap hooks count)
    Mark(0x30000003);
    const double t0 = NpRealSeconds();
    fn(arg);
    g.workerBusySeconds += NpRealSeconds() - t0;
    Mark(0x30000004);
    t_onWorker = was;
    if(diag)NpDiagEvent(NPD_FRAME_SEND,g.mode,diagFrame,diagStart,NpDiagNow(),0,0,0);
}

#if defined(__linux__) && !defined(__vita__)
void PaintStack() {
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) != 0) return;
    void* addr = nullptr;
    size_t size = 0;
    pthread_attr_getstack(&attr, &addr, &size);
    pthread_attr_destroy(&attr);
    if (!addr || size < 16384) return;
    unsigned char* low = static_cast<unsigned char*>(addr);
    // paint everything below the frames in use (leave 4 KiB under the current sp)
    unsigned char* sp = static_cast<unsigned char*>(__builtin_frame_address(0));
    if (sp - 4096 > low) std::memset(low, 0xA5, static_cast<size_t>(sp - 4096 - low));
    g.stackLow = low;
    g.stackSize = size;
}
#endif

void* WorkerMain(void*) {
    t_onWorker = 1;
#if defined(__vita__)
    VitaThreads::PinCurrentThread(VitaThreads::kFrameSend);
    int prio = 0x100000F0;   // above the render thread and the mixer (159), below the audio output (128): split/NOTES.md FOR LAB
    if (const char* p = std::getenv("MSC_VITA_PRIO_FRAMESEND")) prio = static_cast<int>(std::strtoul(p, nullptr, 0));
    const int r = sceKernelChangeThreadPriority(sceKernelGetThreadId(), prio);
    std::fprintf(stderr, "[fs] worker priority 0x%X (%s)\n", static_cast<unsigned>(prio), r >= 0 ? "ok" : "REFUSED");
#elif defined(__linux__)
    PaintStack();
#endif
    for (;;) {
        g.ready.Wait(-1);
        RunJob(g.job, g.arg);
        g.done.Post();
    }
    return nullptr;
}

bool EnsureWorker() {
    if (g.started) return true;
    g.ready.Init("np_fs_ready");
    g.done.Init("np_fs_done");
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, g.stackBytes);
    const int rc = pthread_create(&g.thread, &attr, WorkerMain, nullptr);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        std::fprintf(stderr, "[fs] cannot start the worker thread (%d): frame send stays on the game thread\n", rc);
        g.mode = NP_FS_OFF;
        return false;
    }
    pthread_detach(g.thread);
    g.started = true;
    return true;
}

void CountGate(const char* why) {
    for (int i = 0; i < g.nGates; ++i)
        if (g.gates[i].why == why || !std::strcmp(g.gates[i].why, why)) { ++g.gates[i].n; return; }
    if (g.nGates < kMaxGates) { g.gates[g.nGates].why = why; g.gates[g.nGates].n = 1; ++g.nGates; }
}

inline uint32_t WinHash(const void* p) { return (static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p) >> 3)) * 2654435761u; }

void WinReset() {
    if (g.winInserted > g.maxWinInserted) g.maxWinInserted = g.winInserted;
    if (g.winOverflow) ++g.winOverflows;
    g.nWinAllocs = 0;
    g.winInserted = 0;
    g.winOverflow = false;
    if (!g.winKey) {
        g.winKey = static_cast<uintptr_t*>(std::calloc(kWinSlots, sizeof(uintptr_t)));
        g.winGen = static_cast<uint8_t*>(std::calloc(kWinSlots, 1));
        if (!g.winKey || !g.winGen) { std::free(g.winKey); std::free(g.winGen); g.winKey = nullptr; g.winGen = nullptr; }
    }
    if (++g.winCur == 0) {   // the generation byte wrapped: forget every slot
        if (g.winGen) std::memset(g.winGen, 0, kWinSlots);
        g.winCur = 1;
    }
    if (!g.deferred) g.deferred = static_cast<void**>(std::malloc(sizeof(void*) * static_cast<size_t>(g.maxDeferred)));
}

void WinInsert(void* ptr) {
    if (!g.winKey || g.winInserted >= kWinSlots / 2) { g.winOverflow = true; return; }
    uint32_t h = WinHash(ptr) & (kWinSlots - 1);
    while (g.winGen[h] == g.winCur && g.winKey[h] != 1) h = (h + 1) & (kWinSlots - 1);   // (an erased slot is reused)
    g.winKey[h] = reinterpret_cast<uintptr_t>(ptr);
    g.winGen[h] = g.winCur;
    ++g.winInserted;
    ++g.nWinAllocs;
}

bool WinErase(void* ptr) {   // true when the block was allocated in this window
    if (!g.winKey) return false;
    const uintptr_t key = reinterpret_cast<uintptr_t>(ptr);
    uint32_t h = WinHash(ptr) & (kWinSlots - 1);
    while (g.winGen[h] == g.winCur) {
        if (g.winKey[h] == key) { g.winKey[h] = 1; --g.nWinAllocs; return true; }
        h = (h + 1) & (kWinSlots - 1);
    }
    return false;
}


// ---------------------------------------------------------------------------------------------------------------------------------
// MSC_FS_GXCENSUS=1 (needs a vgx library built with -DVGX_PRODUCER_CHECK, a census build): lists the places where the game thread records a
// GX call while a frame-send job is pending. The recorder has ONE producer: in the two-stage pipeline every such place must join first (a gate),
// defer, or stay out of the pipelined states. The census runs in serial mode (the job runs at the join, so the game thread's records are
// plain to see) and prints, per distinct call stack (return addresses, symbolize with nm / addr2line), the number of records and the first fork.
#if defined(__linux__) && !defined(__vita__)
struct CensusSite { uint32_t pc[10]; int n; uint64_t hits, firstFork; };
constexpr int kMaxSites = 192;
CensusSite g_sites[kMaxSites];
int g_nSites = 0;
uint64_t g_censusLost = 0, g_censusTotal = 0;
bool g_census = false;

bool CodeAddr(uintptr_t v, uintptr_t* ret) {
    const uintptr_t lo = reinterpret_cast<uintptr_t>(&__executable_start), hi = reinterpret_cast<uintptr_t>(&etext);
    const uintptr_t a = v & ~static_cast<uintptr_t>(1);
    if (a < lo + 8 || a >= hi) return false;
#if defined(__arm__)
    if (v & 1) {   // Thumb: the call before the return address is BL / BLX imm (32 bit) or BLX Rm (16 bit)
        const uint16_t h2 = *reinterpret_cast<const uint16_t*>(a - 2);
        if ((h2 & 0xFF87) == 0x4780) { *ret = a; return true; }
        const uint16_t h1 = *reinterpret_cast<const uint16_t*>(a - 4);
        if ((h1 & 0xF800) == 0xF000 && (h2 & 0xC000) == 0xC000 && (h2 & 0x1000)) { *ret = a; return true; }
        return false;
    }
    const uint32_t w = *reinterpret_cast<const uint32_t*>(a - 4);   // ARM: BL / BLX imm, BLX Rm
    if ((w & 0x0F000000) == 0x0B000000 || (w & 0xFE000000) == 0xFA000000 || (w & 0x0FFFFFF0) == 0x012FFF30) { *ret = a; return true; }
    return false;
#else
    (void)ret;
    return false;
#endif
}

void CensusHook() {
    if (!g.pending || t_onWorker || !g.haveGameThread || !pthread_equal(pthread_self(), g.gameThread)) return;
    ++g_censusTotal;
    CensusSite site;
    std::memset(&site, 0, sizeof site);
#if defined(__arm__)
    const uintptr_t* sp = static_cast<const uintptr_t*>(__builtin_frame_address(0));
    for (int i = 0; i < 1024 && site.n < 10; ++i) {   // the return addresses on the next 4 KiB of the stack (candidates: the call before them is a call)
        uintptr_t r;
        if (CodeAddr(sp[i], &r)) site.pc[site.n++] = static_cast<uint32_t>(sp[i]);
    }
#elif defined(__linux__)
    void* bt[10];
    const int n = backtrace(bt, 10);
    for (int i = 0; i < n; ++i) site.pc[site.n++] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(bt[i]));
#endif
    for (int i = 0; i < g_nSites; ++i)
        if (g_sites[i].n == site.n && std::memcmp(g_sites[i].pc, site.pc, sizeof(uint32_t) * (site.n < 6 ? site.n : 6)) == 0) { ++g_sites[i].hits; return; }
    if (g_nSites >= kMaxSites) { ++g_censusLost; return; }
    site.hits = 1;
    site.firstFork = g.forks;
    g_sites[g_nSites++] = site;
}

void CensusInstall() {
    if (!EnvFlag("MSC_FS_GXCENSUS", false)) return;
    if (!vgxn_set_producer_hook) { std::fprintf(stderr, "[fs] MSC_FS_GXCENSUS=1: this vgx library has no producer hook (build with -DVGX_PRODUCER_CHECK)\n"); return; }
    g_census = true;
    vgxn_set_producer_hook(&CensusHook);
    std::fprintf(stderr, "[fs] GX census on: game-thread records while a job is pending are counted per call stack\n");
}

void CensusReport() {
    if (!g_census) return;
    dl_iterate_phdr([](struct dl_phdr_info* info, size_t, void*) -> int {
        std::fprintf(stderr, "[fs] census: load base of the executable 0x%lx (symbolize: address - base + the link address)\n", static_cast<unsigned long>(info->dlpi_addr));
        return 1;   // (the first object is the executable)
    }, nullptr);
    std::fprintf(stderr, "[fs] census: %llu game-thread records while a job was pending, %d distinct stacks (%llu not listed)\n",
                 (unsigned long long)g_censusTotal, g_nSites, (unsigned long long)g_censusLost);
    for (int i = 0; i < g_nSites; ++i) {
        std::fprintf(stderr, "[fs]   census site %d: hits %llu first-fork %llu pcs", i, (unsigned long long)g_sites[i].hits, (unsigned long long)g_sites[i].firstFork);
        for (int k = 0; k < g_sites[i].n; ++k) std::fprintf(stderr, " %x", g_sites[i].pc[k]);
        std::fprintf(stderr, "\n");
    }
}

#else
void CensusInstall() {}
void CensusReport() {}
#endif

// Entries: {fn, size, data (padded to 8)}.
struct CallHead { void (*fn)(const void*); uint32_t size; uint32_t pad; };
void FlushCalls() {
    const int n = g.nCalls;
    const unsigned char* p = g.callBuf;
    g.nCalls = 0;   // (a call made now is not deferred: nothing is pending)
    for (int i = 0; i < n; ++i) {
        const CallHead* h = reinterpret_cast<const CallHead*>(p);
        h->fn(p + sizeof(CallHead));
        p += sizeof(CallHead) + ((h->size + 7u) & ~7u);
    }
    g.callUsed = 0;
}

} // namespace

extern "C" {

int NpFsMode(void) { Decide(); return g.mode; }
int NpFsStates(void) { Decide(); return g.states; }
int NpFsFreeGate(void) { Decide(); return g.freeGate; }
int NpFsTaskMask(void) { Decide(); return g.taskMask; }
int NpFsOnWorker(void) { return t_onWorker; }
void NpFsAfterJoin(void (*fn)(void)) { g.afterJoin = fn; }

void NpFsFork(NpFsJobFn fn, void* arg) {
    Decide();
    if (g.mode == NP_FS_OFF || g.mode == NP_FS_INLINE || (g.mode == NP_FS_THREAD && !EnsureWorker())) {
        ++g.forks;   // (off / inline: the job runs here and now, with the same marks; inline is the snapshot path without any reordering)
        Mark(0x30000001);
        RunJob(fn, arg);
        Mark(0x30000002);
        return;
    }
    if (!g.haveGameThread) { g.gameThread = pthread_self(); g.haveGameThread = true; }
    if (g.pending) NpFsJoin();   // (the caller joins before forking again; this is only a guard)
    Mark(0x30000001);
    ++g.forks;
    g.job = fn;
    g.arg = arg;
    g_forkFpscr = ReadFpscr();
    g.pending = true;
    WinReset();
    __atomic_store_n(&NpFsBusy, 1, __ATOMIC_RELEASE);
    if (g.mode == NP_FS_THREAD) g.ready.Post();
}

void NpFsJoin(void) {
    if (!g.pending) return;
    const bool diag=NpDiagActive&&NpDiagActive();
    const uint64_t diagStart=diag?NpDiagNow():0;
    if (g.mode == NP_FS_THREAD) {
        Mark(0x30000005);
        if (!g.done.TryWait()) {
            const double t0 = NpRealSeconds();
#if defined(__vita__)
            int slices = 0;
            while (!g.done.Wait(10.0)) std::fprintf(stderr, "[fs] JOIN TIMEOUT: the frame-send job has not finished after %d s\n", 10 * ++slices);
#else
            g.done.Wait(-1);
#endif
            const double w = NpRealSeconds() - t0;
            ++g.joinsWaited;
            g.waitSeconds += w;
            if (w > g.maxWait) g.maxWait = w;
        }
    } else {
        RunJob(g.job, g.arg);   // serial mode: the job runs here
    }
    g.pending = false;
    if(diag)NpDiagEvent(NPD_FRAME_JOIN,g.mode,NpDiagFrame(),diagStart,NpDiagNow(),g.nDeferred,g.nCalls,g.nWrites);
    __atomic_store_n(&NpFsBusy, 0, __ATOMIC_RELEASE);
    ++g.joins;
    Mark(0x30000002);
    if (g.nWrites > 0) {   // the stores the game thread made while the job ran
        const int n = g.nWrites;
        g.nWrites = 0;
        for (int i = 0; i < n; ++i) *g.writeSlot[i] = g.writeVal[i];
    }
    if (g.nCalls > 0) FlushCalls();   // the GX calls the game thread made while the job ran, now that the recorder is its own again
    if (g.afterJoin) g.afterJoin();
    if (g.nDeferred > 0) {   // the frees the game thread made while the job ran (in their order), now that nothing reads the memory
        const int n = g.nDeferred;
        g.nDeferred = 0;
        for (int i = 0; i < n; ++i) g.freeFn(g.deferred[i]);
    }
}

int NpFsDeferCall(void (*fn)(const void*), const void* data, unsigned size) {
    if (!g.pending || t_onWorker || !pthread_equal(pthread_self(), g.gameThread)) return 0;
    const int need = static_cast<int>(sizeof(CallHead) + ((size + 7u) & ~7u));
    if (g.callUsed + need > kCallBytes) { ++g.callOverflows; NpFsGateSlow("callfull"); return 0; }   // (the join flushes the earlier calls first: the order holds)
    CallHead* h = reinterpret_cast<CallHead*>(g.callBuf + g.callUsed);
    h->fn = fn;
    h->size = size;
    h->pad = 0;
    if (size) std::memcpy(g.callBuf + g.callUsed + sizeof(CallHead), data, size);
    g.callUsed += need;
    ++g.nCalls;
    ++g.callsDeferred;
    return 1;
}

void NpFsGateSlow(const char* why) {
    // (other threads - the disc and NAND workers, the mixer - reach the gates through shared SDK paths: they do not join, only the game thread does)
    if (t_onWorker || !g.pending || !pthread_equal(pthread_self(), g.gameThread)) return;
    if (why && !std::strcmp(why, "task")) ++g.joinsTask;
    else CountGate(why ? why : "?");
    NpFsJoin();
}

void NpFsHeapAlloc(void* ptr) {   // the game thread only (nlMemory.cpp); in serial / inline mode the job runs here and its calls are counted instead
    if (t_onWorker) { NpFsWorkerHeapCall(); return; }
    if (!g.pending) return;
    ++g.heapAllocsInWindow;
    WinInsert(ptr);
}

int NpFsHeapFree(void* ptr) {
    if (t_onWorker) { NpFsWorkerHeapCall(); return 0; }
    if (!g.pending || !ptr) return 0;
    ++g.heapFreesInWindow;
    if (WinErase(ptr)) {   // allocated inside the window: the job cannot refer to it
        ++g.freesOfWindowBlocks;
        return 0;
    }
    if (g.freeGate == 2 && g.freeFn && g.deferred && g.nDeferred < g.maxDeferred) {   // the job may read the block: it is freed after the join
        g.deferred[g.nDeferred++] = ptr;
        ++g.deferredTotal;
        if (static_cast<uint64_t>(g.nDeferred) > g.deferredMax) g.deferredMax = static_cast<uint64_t>(g.nDeferred);
        return 1;
    }
    if (g.freeGate == 2) ++g.deferOverflow;
    if (g.freeGate != 0) NpFsGateSlow("free");
    return 0;
}

void NpFsSetFreeFn(void (*fn)(void*)) { g.freeFn = fn; }

// Pointer-sized stores into tables the job reads (glTextureManager::mTextures: texture animations advance in GLInventory::Update, early in the next
// frame), kept until the job is done and made then in their order. Only with the pipeline: without it the "invupd" gate joined before the update.
int NpFsDeferWrite(void** slot, void* value) {
    if (!g.pending || !g.pipe || t_onWorker || !pthread_equal(pthread_self(), g.gameThread)) return 0;
    if (g.nWrites >= kMaxWrites) {
        ++g.writeOverflows;
        NpFsGateSlow("texwrites");
        return 0;
    }
    g.writeSlot[g.nWrites] = slot;
    g.writeVal[g.nWrites] = value;
    ++g.nWrites;
    ++g.writesDeferred;
    return 1;
}

void NpFsWorkerHeapCall(void) {
    if (++g.workerHeapCalls == 1) std::fprintf(stderr, "[fs] WARNING: the frame-send job called the game heap (it must not)\n");
#if defined(__linux__)
    // MSC_FS_HEAPTRACE=1: where (raw return addresses, symbolise with nm / addr2line on the executable)
    static const bool trace = EnvFlag("MSC_FS_HEAPTRACE", false);
    if (trace && g.workerHeapCalls == 1) {
        dl_iterate_phdr([](struct dl_phdr_info* info, size_t, void*) -> int {
            std::fprintf(stderr, "[fs] heap trace: load base of the executable 0x%lx\n", static_cast<unsigned long>(info->dlpi_addr));
            return 1;
        }, nullptr);
    }
    if (trace && g.workerHeapCalls <= 40) {
        void* bt[14];
        const int n = backtrace(bt, 14);
        std::fprintf(stderr, "[fs] heap call %d from:", static_cast<int>(g.workerHeapCalls));
        for (int i = 0; i < n; ++i) std::fprintf(stderr, " %p", bt[i]);
        std::fprintf(stderr, "\n");
    }
#endif
}

void NpFsLogStats(const char* when) {
    if (!g.decided || (g.mode == NP_FS_OFF && g.forks == 0)) return;
    std::fprintf(stderr, "[fs] %s: mode=%s forks=%llu joins=%llu (task boundary %llu", when ? when : "stats", ModeName(g.mode),
                 (unsigned long long)g.forks, (unsigned long long)g.joins, (unsigned long long)g.joinsTask);
    for (int i = 0; i < g.nGates; ++i) std::fprintf(stderr, ", gate %s %llu", g.gates[i].why, (unsigned long long)g.gates[i].n);
    std::fprintf(stderr, ")\n");
    if (NpGlSendLogStats) NpGlSendLogStats();
    CensusReport();
    std::fprintf(stderr, "[fs]   joins that waited %llu (total %.3f s, max %.1f ms), job time %.3f s; heap in window: %llu allocs, %llu frees (%llu of window blocks); worker heap calls %llu\n",
                 (unsigned long long)g.joinsWaited, g.waitSeconds, g.maxWait * 1000.0, g.workerBusySeconds, (unsigned long long)g.heapAllocsInWindow,
                 (unsigned long long)g.heapFreesInWindow, (unsigned long long)g.freesOfWindowBlocks, (unsigned long long)g.workerHeapCalls);
    if (g.callsDeferred || g.callOverflows)
        std::fprintf(stderr, "[fs]   GX calls deferred to the join: %llu (%llu did not fit and joined)\n", (unsigned long long)g.callsDeferred, (unsigned long long)g.callOverflows);
    if (g.pipe)
        std::fprintf(stderr, "[fs]   stores deferred to the join (texture animation table): %llu (%llu did not fit and joined)\n", (unsigned long long)g.writesDeferred,
                     (unsigned long long)g.writeOverflows);
    if (g.freeGate == 2)
        std::fprintf(stderr, "[fs]   frees deferred to the join: %llu (up to %llu in one window, %llu did not fit and joined)\n", (unsigned long long)g.deferredTotal,
                     (unsigned long long)g.deferredMax, (unsigned long long)g.deferOverflow);
    std::fprintf(stderr, "[fs]   window: up to %d blocks allocated in one window, %llu windows overflowed the set\n", g.maxWinInserted, (unsigned long long)g.winOverflows);
#if defined(__linux__) && !defined(__vita__)
    if (g.stackLow && g.stackSize) {
        size_t i = 0;
        while (i < g.stackSize && g.stackLow[i] == 0xA5) ++i;   // first byte the worker touched (painted region only)
        std::fprintf(stderr, "[fs]   worker stack: %lu KiB reserved, high-water about %lu KiB\n", static_cast<unsigned long>(g.stackSize / 1024), static_cast<unsigned long>((g.stackSize - i) / 1024));
    }
#endif
}

} // extern "C"

// ---------------------------------------------------------------------------------------------------------------------------------
// The helper lane (np_framesend.h): one more thread, FIFO, for the game thread's data-parallel work.
namespace {
constexpr int kLaneQueue = 256;
struct Lane {
    int enabled = -1;                  // -1 undecided, 0 off, 1 on
    bool started = false;
    pthread_t thread;
    Sem wake, done;
    struct Item { NpFsJobFn fn; void* arg; } q[kLaneQueue];
    int head = 0;                      // next slot the lane takes (lane thread; reset by the game thread while the lane is idle)
    int tail = 0;                      // next slot the game thread fills
    int owed = 0;                      // done tokens the game thread has yet to take
};
Lane lane;

void* LaneMain(void*) {
#if defined(__vita__)
    VitaThreads::PinCurrentThread(VitaThreads::kHelper, false);
#endif
    for (;;) {
        lane.wake.Wait(-1);
        const Lane::Item it = lane.q[lane.head++];
        it.fn(it.arg);
        lane.done.Post();
    }
    return nullptr;
}

bool LaneStart() {
    if (lane.started) return true;
    lane.wake.Init("np_lane_wake");
    lane.done.Init("np_lane_done");
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    const int rc = pthread_create(&lane.thread, &attr, LaneMain, nullptr);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        std::fprintf(stderr, "[lane] cannot start the lane thread (%d): the work stays on the game thread\n", rc);
        lane.enabled = 0;
        return false;
    }
    pthread_detach(lane.thread);
    lane.started = true;
    return true;
}
} // namespace

extern "C" {

int NpLaneEnabled(void) {
    if (lane.enabled < 0) {
        const char* e = std::getenv("MSC_SKIN_LANE");
        lane.enabled = (e && *e && *e != '0') ? 1 : 0;
        if (lane.enabled) std::fprintf(stderr, "[lane] helper lane for the software skinning is on\n");
    }
    return lane.enabled;
}

void NpLaneSubmit(NpFsJobFn fn, void* arg) {
    if (!NpLaneEnabled() || !LaneStart()) { fn(arg); return; }
    if (lane.tail == kLaneQueue) NpLaneWait();   // full: drain first (jobs of one submitter must not overtake each other), then queue again
    lane.q[lane.tail++] = {fn, arg};
    ++lane.owed;
    lane.wake.Post();
}

void NpLaneWait(void) {
    if (!lane.started) return;
    while (lane.owed > 0) { lane.done.Wait(-1); --lane.owed; }
    lane.head = 0;   // the lane is idle: it is blocked in wake.Wait, and the next post orders these writes before its next read
    lane.tail = 0;
}

} // extern "C"
