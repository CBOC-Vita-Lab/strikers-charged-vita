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
#include "../../../include/np/gsplit.h"
#include "../../../include/np/lockwait.h"
#include "../../../include/np/stall.h"   // MSC_STALL_TIMERS (stall10)
#include "np_time.h"
#include "np_fasttls.h"   // NpTlsSlot89 (MSC_TLS_FAST)
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
#include <sched.h>

#if defined(__vita__)
#include "np_vita_internal.h"
#include <psp2/kernel/threadmgr.h>
#elif defined(__linux__)
#include <semaphore.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

extern "C" unsigned long long NpMemHeapLockSlow(void) __attribute__((weak));   // NL/nlMemory.cpp
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

extern "C" int NpSimtOnSimThread(void) __attribute__((weak));   // NpSimRun17.cpp: stack-address test, 1 on np_sim

namespace {
void CensusInstall();
void CensusReport();
void FlushCalls();
void LaneLogStats(const char* when);

constexpr int kWinSlots = 32768;           // window set slots (a power of two); at most kWinSlots / 2 inserts per window, the rest is "unknown" (its free is deferred)
constexpr int kMaxDeferred = 16384;
constexpr int kMaxDeferredPipe = 65536;    // the pipeline's window is a whole frame build (W5 PIPE)
constexpr int kMaxWrites = 512;
constexpr int kCallBytes = 32768;          // NpFsDeferCall: room for the deferred GX calls of one window
constexpr int kMaxGates = 16;

std::atomic<uint64_t> g_semOverflows{0};   // posts a bounded semaphore dropped (host emulation of the Vita's SEMA_OVF; the lane counts the real ones too)

struct Sem {
#if defined(__vita__)
    SceUID id = -1;
    // `maxCount` is the kernel's limit: sceKernelSignalSema on a semaphore that holds maxCount fails with SCE_KERNEL_ERROR_SEMA_OVF and the signal is LOST.
    // The frame-send hand-off posts once and waits once, strictly alternating, so its semaphores are created with 1; anything that can post twice before
    // the first wait must not rely on the count (the helper lane below uses only the binary "a wake-up is wanted" protocol with these).
    void Init(const char* name, int maxCount = 1) { id = sceKernelCreateSema(name, 0, 0, maxCount, nullptr); }
    bool Post() { return sceKernelSignalSema(id, 1) >= 0; }
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
    int cap = 0;   // MSC_SEM_MAX=<n> (host tests only): behave like the Vita's kernel semaphore of that maximum, a post to a full one is dropped
    void Init(const char*, int maxCount = 1) {
        sem_init(&s, 0, 0);
        static const int emu = [] { const char* e = std::getenv("MSC_SEM_MAX1"); return (e && *e && *e != '0') ? 1 : 0; }();
        cap = emu ? maxCount : 0;
    }
    bool Post() {
        if (cap > 0) {
            int v = 0;
            sem_getvalue(&s, &v);
            if (v >= cap) { ++g_semOverflows; return false; }   // SCE_KERNEL_ERROR_SEMA_OVF: the signal is lost
        }
        sem_post(&s);
        return true;
    }
    // seconds < 0: block. seconds >= 0: poll (the harness' virtual clock is interposed under libc's sem_timedwait, so there is no timed wait; the real
    // clock NpRealSeconds is read and the polling thread sleeps 100 us at a time)
    bool Wait(double seconds) {
        if (seconds < 0) {
            while (sem_wait(&s) != 0 && errno == EINTR) {}
            return true;
        }
        const double end = NpRealSeconds() + seconds;
        for (;;) {
            if (sem_trywait(&s) == 0) return true;
            if (NpRealSeconds() >= end) return false;
            usleep(100);
        }
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
    int held = 0;                    // rep27 (MSC_FS_HELD): 1 replays, 2 replays and cut-scenes are sent on the game thread
    int heldBits = 0;                // the task-state bits MSC_FS_HELD took out of `states`
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

// overlap18 (MSC_SIMT=2, np/simrun17.h): while the game thread draws the window, the simulation step runs on np_sim, a SECOND thread that allocates and frees from the game heap
// while a send may be pending. g_ovl is set by NpSimtRun for the length of the kick (cleared after the join). While it is set: (1) the window set and the deferred-free list are
// touched under g_hookLock by both threads (and by the join's tail); (2) np_sim never joins (only the game thread does): its gates wait until the send job has FINISHED
// (g_jobFinished, set by the worker; the join's bookkeeping stays the game thread's), and its frees of blocks the job may read go to the deferred list (drained at the join).
// With g_ovl clear nothing here changes anything.
std::atomic<int> g_ovl{0};
std::atomic<int> g_jobFinished{1};
std::atomic_flag g_hookLock = ATOMIC_FLAG_INIT;
std::atomic<uint64_t> g_simAllocs{0}, g_simFreesDeferred{0}, g_simFreesOwn{0}, g_simGateWaits{0}, g_simWaitUs{0}, g_simWaitMaxUs{0};   // (statistics: relaxed, read by the stats line from any thread)
inline bool OvlOn() { return g_ovl.load(std::memory_order_acquire) != 0; }
unsigned g_hookLockSlow = 0;   // acquisitions that ran out of spins (np/lockwait.h)
inline void HookLock() { int n = 0; while (g_hookLock.test_and_set(std::memory_order_acquire)) NpLockBackoff(n, &g_hookLockSlow); }
inline void HookUnlock() { g_hookLock.clear(std::memory_order_release); }
struct HookGuard {   // takes the lock only while a second game-heap thread exists
    bool on;
    HookGuard() : on(OvlOn()) { if (on) HookLock(); }
    ~HookGuard() { if (on) HookUnlock(); }
};

// MSC_FS_TLSFAST (default 1; 0 = every read goes through t_onWorker as before; 1.3 armcpu9). On the Vita a thread_local is emulated TLS (clang
// -femulated-tls): each read is a call to __emutls_get_address -> pthread_getspecific -> pte_osTlsGetValue -> __getreent_for_thread -> sceKernelGetTLSAddr,
// about 90 instructions, seven calls and a `dmb ish`. The game asks "am I on the worker?" some 3000 times a picture (glplatFrameAlloc, the heap hooks, the
// shadow-light lookups): host callgrind, The Dump. The same answer without the emulation: a thread knows itself by a token (the address of its TLS slot,
// one cheap call; on Linux the address of a native __thread variable) and the table below holds the answer for the threads that ever ran a job (the worker,
// and the game thread in serial / inline mode). A thread that is not in the table has never set t_onWorker: the answer is 0. Each thread reads and writes
// only its own entry; the entry's token is written before its value is set and never changes (the worker lives to the end of the process), so no ordering
// is needed. MSC_FS_TLSVERIFY=1 compares every answer with t_onWorker and counts the differences ([fs] tlsfast line at exit of the first mismatch).
#if defined(__vita__)
inline void* ThreadToken() { return NpTlsSlot89(); }   // the slot newlib's reent uses: one per thread, no system call (MSC_TLS_FAST: TPIDRURO + the proved offset, two instructions; else sceKernelGetTLSAddr(0x89))
#else
__thread char t_tokenAnchor;
inline void* ThreadToken() { return &t_tokenAnchor; }
#endif
struct WorkerSlot { void* token; int value; };
constexpr int kWorkerSlots = 8;
WorkerSlot g_slots[kWorkerSlots];
volatile int g_nSlots = 0;           // entries in use (appended under g_slotLock; read without it)
volatile int g_slotsFull = 0;        // more threads than slots: every read takes t_onWorker (never happens with one worker)
int g_tlsFast = 1;                   // MSC_FS_TLSFAST
int g_tlsVerify = 0;                 // MSC_FS_TLSVERIFY
volatile int g_tlsMismatch = 0;
pthread_mutex_t g_slotLock = PTHREAD_MUTEX_INITIALIZER;

inline int FastOnWorker() {
#if defined(NP_FS_TLSBREAK)   // (test mutant: the answer is always 0; MSC_FS_TLSVERIFY=1 must report it)
    return 0;
#endif
    void* const me = ThreadToken();
    const int n = g_nSlots;
    for (int i = 0; i < n; ++i)
        if (g_slots[i].token == me) return g_slots[i].value;
    return 0;
}
inline int OnWorker() {
    int v;
    if (g_tlsFast && !g_slotsFull) {
        v = FastOnWorker();
        if (__builtin_expect(g_tlsVerify != 0, 0) && v != t_onWorker) {
            if (!g_tlsMismatch) std::fprintf(stderr, "[fs] tlsfast MISMATCH: table %d, t_onWorker %d\n", v, t_onWorker);
            g_tlsMismatch = g_tlsMismatch + 1;
        }
    } else {
        v = t_onWorker;
    }
    return v;
}
void SetOnWorker(int v) {
    t_onWorker = v;
    void* const me = ThreadToken();
    const int n = g_nSlots;
    for (int i = 0; i < n; ++i)
        if (g_slots[i].token == me) { g_slots[i].value = v; return; }
    if (!v) return;   // never set: the table answers 0 for it
    pthread_mutex_lock(&g_slotLock);
    const int m = g_nSlots;
    if (m < kWorkerSlots) {
        g_slots[m].value = v;
        g_slots[m].token = me;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        g_nSlots = m + 1;
    } else {
        g_slotsFull = 1;
    }
    pthread_mutex_unlock(&g_slotLock);
}

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
    g_tlsFast = EnvFlag("MSC_FS_TLSFAST", true);
    g_tlsVerify = EnvFlag("MSC_FS_TLSVERIFY", false) ? 1 : 0;
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
    // rep27 (MSC_FS_HELD=0|1|2, code default 0, 1 in the Vita plan C): in a scene that never steps the simulation the game thread has room and the
    // send worker's core (core 1, shared with the render thread's front end) has none: console 18 Nov, replays: game thread p50 37-40 ms a picture of
    // which most is the send join (the next send waits for the last one), core 1 at 86 % of a 47 ms picture. Such a frame is sent the plain way, on the
    // game thread (the path the loading task's frames already take): the same GX stream, in the same order; nothing the simulation reads.
    // 1: replays (TASK_REPLAY 0x8, debug replay 0x20000); 2: also cut-scenes and the walk-out (TASK_NIS 0x10). Thread and serial modes (serial: the host proof), an explicit
    // MSC_FS_STATES wins (it names the forked states itself).
    if ((g.mode == NP_FS_THREAD || g.mode == NP_FS_SERIAL) && !std::getenv("MSC_FS_STATES")) {
        const char* h = std::getenv("MSC_FS_HELD");
        g.held = (h && *h) ? static_cast<int>(std::strtol(h, nullptr, 0)) : 0;
        if (g.held < 0 || g.held > 2) g.held = 0;
        const int bits = g.held >= 1 ? (0x8 | 0x20000 | (g.held >= 2 ? 0x10 : 0)) : 0;
        g.heldBits = g.states & bits;
        g.states &= ~bits;
    }
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
    {
        std::fprintf(stderr, "[fs] frame send: mode=%s%s states=0x%X freegate=%d task-mask=0x%X stack=%lu KiB tlsfast=%d%s\n", ModeName(g.mode), g.pipe ? " (two-stage pipeline)" : "",
                     static_cast<unsigned>(g.states), g.freeGate, static_cast<unsigned>(g.taskMask), static_cast<unsigned long>(g.stackBytes / 1024), g_tlsFast, g_tlsVerify ? " (verify)" : "");
        if (g.held > 0)
            std::fprintf(stderr, "[fs] MSC_FS_HELD=%d: frames of task states 0x%X (%s) are sent on the game thread, not forked to the send worker\n", g.held, static_cast<unsigned>(g.heldBits),
                         g.held >= 2 ? "replays, cut-scenes, walk-out" : "replays");
    }
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
    const int was = OnWorker();
    SetOnWorker(1);
    if (was) WriteFpscr(g_forkFpscr);   // (thread mode: only the worker thread has t_onWorker set before a job starts)   // a job runs under the worker's rules wherever it runs (the gates do not join, the heap hooks count)
    Mark(0x30000003);
    const double t0 = NpRealSeconds();
    fn(arg);
    g.workerBusySeconds += NpRealSeconds() - t0;
    Mark(0x30000004);
    SetOnWorker(was);
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
    SetOnWorker(1);
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
        g_jobFinished.store(1, std::memory_order_release);
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
    if (!g.pending || OnWorker() || !g.haveGameThread || !pthread_equal(pthread_self(), g.gameThread)) return;
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
int NpFsHeldStates(void) { Decide(); return g.heldBits; }
int NpFsFreeGate(void) { Decide(); return g.freeGate; }
int NpFsTaskMask(void) { Decide(); return g.taskMask; }
int NpFsOnWorker(void) { return OnWorker(); }
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
    {   // (under the hook lock with MSC_SIMT=2: np_sim's heap events must see the new window as a whole)
        HookGuard hg;
        g_jobFinished.store(0, std::memory_order_release);
        g.pending = true;
        WinReset();
    }
    __atomic_store_n(&NpFsBusy, 1, __ATOMIC_RELEASE);
    if (g.mode == NP_FS_THREAD) g.ready.Post();
}

static int s_npGsJoinPhase = NPGS_WAIT_JOIN;   // gt27: NPGS_WAIT_GATE while a gate joins (game thread only)
void NpFsJoin(void) {
    if (!g.pending) return;
    NpGsSeqW gsq(s_npGsJoinPhase);   // (gt27: the wait only; the work after it is e-joinwork. This file is built without NP_NATIVE: the old NP_GS here was a no-op on the Vita, so join waits were never booked)
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
    gsq.Next(NPGS_E_JOINWORK);
    g_jobFinished.store(1, std::memory_order_release);   // (serial mode: the job ran just now; thread mode: already set by the worker)
    int nDeferredAtJoin;
    {   // (overlap18: np_sim may be pushing deferred frees: the list is closed under the lock; once pending is false nobody pushes)
        HookGuard hg;
        g.pending = false;
        nDeferredAtJoin = g.nDeferred;
        g.nDeferred = 0;
    }
    if(diag)NpDiagEvent(NPD_FRAME_JOIN,g.mode,NpDiagFrame(),diagStart,NpDiagNow(),nDeferredAtJoin,g.nCalls,g.nWrites);
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
    if (nDeferredAtJoin > 0) {   // the frees the game thread (and, with MSC_SIMT=2, np_sim) made while the job ran (in their order), now that nothing reads the memory
        for (int i = 0; i < nDeferredAtJoin; ++i) g.freeFn(g.deferred[i]);
    }
}

int NpFsDeferCall(void (*fn)(const void*), const void* data, unsigned size) {
    if (!g.pending || OnWorker() || !pthread_equal(pthread_self(), g.gameThread)) return 0;
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

// np_sim (MSC_SIMT=2) has no join of its own: a gate it reaches waits until the job has finished (the memory is then safe to touch); the game thread joins later.
static void SimWaitJob() {
    if (g.mode != NP_FS_THREAD) return;   // (MSC_SIMT=2 is refused in the other modes: the job would only run at the game thread's join)
    if (g_jobFinished.load(std::memory_order_acquire) != 0) return;
    const double t0 = NpRealSeconds();
    int n = 0;
    while (g_jobFinished.load(std::memory_order_acquire) == 0) {
        if (++n > 200) {
#if defined(__vita__)
            sceKernelDelayThread(50);
#else
            sched_yield();
#endif
        } else {
#if defined(__arm__)
            __asm__ __volatile__("yield" ::: "memory");
#endif
        }
    }
    const double us = (NpRealSeconds() - t0) * 1e6;
    const uint64_t u = us > 0 ? static_cast<uint64_t>(us) : 0;
    g_simWaitUs.fetch_add(u, std::memory_order_relaxed);
    if (u > g_simWaitMaxUs.load(std::memory_order_relaxed)) g_simWaitMaxUs.store(u, std::memory_order_relaxed);
}

void NpFsGateSlow(const char* why) {
    if (OvlOn() && NpSimtOnSimThread && NpSimtOnSimThread()) {   // overlap18: np_sim (the only thread that runs this branch)
        if (g_jobFinished.load(std::memory_order_acquire) == 0) { g_simGateWaits.fetch_add(1, std::memory_order_relaxed); SimWaitJob(); }   // (no read of g.pending: the game thread's join writes it)
        return;
    }
    // (other threads - the disc and NAND workers, the mixer - reach the gates through shared SDK paths: they do not join, only the game thread does)
    if (OnWorker() || !g.pending || !pthread_equal(pthread_self(), g.gameThread)) return;
    if (why && !std::strcmp(why, "task")) ++g.joinsTask;
    else CountGate(why ? why : "?");
    NP_STALL_CM("frame-send join (gate)", 0, &why);   // (MSC_STALL_TIMERS: the game thread waits for the frame-send worker; `why` names the gate)
    s_npGsJoinPhase = NPGS_WAIT_GATE;
    NpFsJoin();
    s_npGsJoinPhase = NPGS_WAIT_JOIN;
}

void NpFsHeapAlloc(void* ptr) {   // the game thread (nlMemory.cpp), and np_sim with MSC_SIMT=2; in serial / inline mode the job runs here and its calls are counted instead
    if (OnWorker()) { NpFsWorkerHeapCall(); return; }
    HookGuard hg;   // (overlap18: both game-heap threads reach the window set)
    if (!g.pending) return;
    ++g.heapAllocsInWindow;
    if (hg.on && NpSimtOnSimThread && NpSimtOnSimThread()) g_simAllocs.fetch_add(1, std::memory_order_relaxed);
    WinInsert(ptr);
}

int NpFsHeapFree(void* ptr) {
    if (OnWorker()) { NpFsWorkerHeapCall(); return 0; }
    {
        HookGuard hg;
        if (!g.pending || !ptr) return 0;
        const bool sim = hg.on && NpSimtOnSimThread && NpSimtOnSimThread();
        ++g.heapFreesInWindow;
        if (WinErase(ptr)) {   // allocated inside the window: the job cannot refer to it
            ++g.freesOfWindowBlocks;
            if (sim) g_simFreesOwn.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
        if (g.freeGate == 2 && g.freeFn && g.deferred && g.nDeferred < g.maxDeferred) {   // the job may read the block: it is freed after the join
            g.deferred[g.nDeferred++] = ptr;
            ++g.deferredTotal;
            if (sim) g_simFreesDeferred.fetch_add(1, std::memory_order_relaxed);
            if (static_cast<uint64_t>(g.nDeferred) > g.deferredMax) g.deferredMax = static_cast<uint64_t>(g.nDeferred);
            return 1;
        }
        if (g.freeGate == 2) ++g.deferOverflow;
    }   // (the lock is released before a gate: the game thread's join takes it)
    if (g.freeGate != 0) NpFsGateSlow("free");
    return 0;
}

// overlap18: NpSimtRun brackets the kick..join of a MSC_SIMT=2 picture with this (game thread).
void NpFsSetOverlap(int on) { g_ovl.store(on ? 1 : 0, std::memory_order_release); }

void NpFsSetFreeFn(void (*fn)(void*)) { g.freeFn = fn; }

// Pointer-sized stores into tables the job reads (glTextureManager::mTextures: texture animations advance in GLInventory::Update, early in the next
// frame), kept until the job is done and made then in their order. Only with the pipeline: without it the "invupd" gate joined before the update.
int NpFsDeferWrite(void** slot, void* value) {
    if (!g.pending || !g.pipe || OnWorker() || !pthread_equal(pthread_self(), g.gameThread)) return 0;
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
    LaneLogStats(when);
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
    if (g_simAllocs.load() || g_simFreesDeferred.load() || g_simFreesOwn.load() || g_simGateWaits.load())
        std::fprintf(stderr, "[fs]   np_sim (MSC_SIMT=2): allocs in a window %llu, frees of window blocks %llu, frees deferred to the join %llu, gates that waited for the job %llu (%.3f ms, max %.1f ms)\n",
                     (unsigned long long)g_simAllocs.load(), (unsigned long long)g_simFreesOwn.load(), (unsigned long long)g_simFreesDeferred.load(), (unsigned long long)g_simGateWaits.load(), g_simWaitUs.load() * 1e-3, g_simWaitMaxUs.load() * 1e-3);
    std::fprintf(stderr, "[fs]   lock slow paths (spin budget used up; Vita: then a 20 us sleep, host: sched_yield): game heap %llu, frame-send hook %llu\n",
                 NpMemHeapLockSlow ? NpMemHeapLockSlow() : 0ull, (unsigned long long)__atomic_load_n(&g_hookLockSlow, __ATOMIC_RELAXED));
    std::fprintf(stderr, "[fs]   window: up to %d blocks allocated in one window, %llu windows overflowed the set\n", g.maxWinInserted, (unsigned long long)g.winOverflows);
#if defined(__linux__) && !defined(__vita__)
    if (g.stackLow && g.stackSize) {
        size_t i = 0;
        while (i < g.stackSize && g.stackLow[i] == 0xA5) ++i;   // first byte the worker touched (painted region only)
        std::fprintf(stderr, "[fs]   worker stack: %lu KiB reserved, high-water about %lu KiB\n", static_cast<unsigned long>(g.stackSize / 1024), static_cast<unsigned long>((g.stackSize - i) / 1024));
    }
#endif
}


// (block17, MSC_SIMT=1) The step loop runs on np_sim while the game thread blocks: for that time np_sim is the thread that gates and joins (the gates above act only for the
// game thread). Returns the previous owner; nothing changes before the first fork has named one.
pthread_t NpFsSwapGameThread(pthread_t t) {
    const pthread_t old = g.gameThread;
    if (g.haveGameThread) g.gameThread = t;
    return old;
}

} // extern "C"

// ---------------------------------------------------------------------------------------------------------------------------------
// The helper lane (np_framesend.h): one more thread, FIFO, for the game thread's data-parallel work.
//
// Hand-off design (1.2). The first version counted jobs in two kernel semaphores (one post per job to wake the lane, one post per finished job for
// the game thread's wait). On the Vita those semaphores have a maximum of 1 (Sem::Init above): a second post before the first was taken fails with
// SCE_KERNEL_ERROR_SEMA_OVF and is LOST. A cut-scene queues dozens of jobs at once, so the lane ran fewer jobs than were queued and the game thread
// then waited, for ever, for completion posts that had been dropped: no frame was handed on, and the render thread sat in "vgx slot full" (the 1.1
// console hang at the start of a match). The host's POSIX semaphores have no maximum, so no host run could show it (MSC_SEM_MAX1=1 emulates it).
//
// Now the counts are plain atomics and the semaphores are only a binary "somebody sleeps, wake it" signal, which a drop cannot harm:
//   tail      jobs published (game thread)      next      jobs taken      finished  jobs completed (strictly in order: one executor at a time)
//   busy      1 while an executor runs a job (the lane thread; the game thread only when the lane thread has stalled, see NpLaneWait)
//   sleeping  the lane thread is about to block on `wake`   waiting  the game thread is about to block on `done`
// A sleeper announces itself, then looks at the counter once more; the other side changes the counter, then looks at the announcement (both seq_cst):
// one of the two always sees the other, so a wake-up cannot be missed, and a surplus post is a spurious wake-up that the loop re-checks.
// The game thread never waits for ever: it waits in slices; if no job has finished for kLaneStallSlices slices and the lane thread is not inside a job
// (not scheduled, or not started), it runs the queued jobs itself, in order. A full queue is refused (NpLaneTrySubmit returns 0): the caller does the
// work itself instead of waiting for room.
namespace {
constexpr uint32_t kLaneQueue = 256;   // a power of two
constexpr double kLaneSliceSeconds = 0.004;
constexpr int kLaneStallSlices = 3;
struct Lane {
    int enabled = -1;                  // -1 undecided, 0 off, 1 on
    bool forced = false;               // gt27 (NpLaneForce): the lane runs for an explicit user (MSC_BLEND_ASYNC) without turning the skinning onto it
    bool started = false;
    pthread_t thread;
    Sem wake, done;
    struct Item { NpFsJobFn fn; void* arg; } q[kLaneQueue];
    alignas(64) std::atomic<uint32_t> tail{0};
    alignas(64) std::atomic<uint32_t> next{0};
    alignas(64) std::atomic<uint32_t> finished{0};
    alignas(64) std::atomic<int> busy{0};
    alignas(64) std::atomic<int> sleeping{0};
    alignas(64) std::atomic<int> waiting{0};
    std::atomic<uint32_t> waitTarget{0};
    // statistics (game thread, except where atomic)
    uint64_t submitted = 0, refusedFull = 0, waits = 0, waitsBlocked = 0, stalls = 0, longWaits = 0, takeovers = 0, takeoverCalls = 0;   // takeovers (lane24): jobs the game thread ran at once because the lane thread had not started them
    std::atomic<uint64_t> ranOnLane{0}, ranOnGame{0}, postDrops{0};
    double waitSeconds = 0, maxWait = 0;
};
Lane lane;

void LaneDelayUs(unsigned us) {
#if defined(__vita__)
    sceKernelDelayThread(us);
#else
    usleep(us);
#endif
}

// Runs the next queued job if there is one and nobody else is running one. 1: ran a job; 0: the queue is empty; -1: another executor is inside a job.
int LaneStep(bool onGameThread) {
    if (lane.next.load(std::memory_order_relaxed) == lane.tail.load(std::memory_order_acquire)) return 0;
    int expect = 0;
    if (!lane.busy.compare_exchange_strong(expect, 1, std::memory_order_acq_rel)) return -1;
    const uint32_t h = lane.next.load(std::memory_order_relaxed);
    if (h == lane.tail.load(std::memory_order_acquire)) { lane.busy.store(0, std::memory_order_release); return 0; }
    const Lane::Item it = lane.q[h & (kLaneQueue - 1)];   // (copied before `next` moves on: the producer may then reuse the slot)
    lane.next.store(h + 1, std::memory_order_release);   // (release: the copy of the slot above is done before the producer may reuse it)
    it.fn(it.arg);
    (onGameThread ? lane.ranOnGame : lane.ranOnLane).fetch_add(1, std::memory_order_relaxed);
    lane.finished.store(h + 1, std::memory_order_seq_cst);
    lane.busy.store(0, std::memory_order_release);
    if (lane.waiting.load(std::memory_order_seq_cst) != 0 && lane.finished.load(std::memory_order_seq_cst) == lane.waitTarget.load(std::memory_order_seq_cst)) {
        if (lane.waiting.exchange(0, std::memory_order_seq_cst) != 0 && !lane.done.Post()) lane.postDrops.fetch_add(1, std::memory_order_relaxed);
    }
    return 1;
}

void* LaneMain(void*) {
#if defined(__vita__)
    // (lane24 FIX-07, MSC_LANE_CORE3=1, default off) the lane's work is short bursts that never spin (the blend: ~1.5 ms a picture), the kind of load core 3's job lane already takes: with the proven fourth core
    // and no online session it runs there, away from the front-end record player on core 2; the online guard moves it off while a session is up (Core3Register), and a refused / unproven core is core 2 as before.
    VitaThreads::PinCurrentThread(VitaThreads::kHelper, false);   // (registers the thread for the memory report and the probe; core 2's mask first, then core 3 if asked)
    bool onCore3 = false;
    if (const char* c3 = std::getenv("MSC_LANE_CORE3")) {
        if (*c3 && *c3 != '0') {
            if (VitaThreads::Core3Proven && VitaThreads::Core3Proven() && !VitaThreads::g_core3HeldNow()) {
                const SceUID self = sceKernelGetThreadId();
                const int r = sceKernelChangeThreadCpuAffinityMask(self, 0x00080000);
                if (r >= 0) {
                    VitaThreads::Core3Register(self, 0x10000100 + 4);
                    if (VitaThreads::g_core3HeldNow() || VitaThreads::g_core3LostNow()) {
                        sceKernelChangeThreadCpuAffinityMask(self, 0x00010000 << 2);
                    } else {
                        onCore3 = true;
                    }
                }
                std::fprintf(stderr, "[lane] MSC_LANE_CORE3=1: helper lane thread on core 3 (%s)\n", onCore3 ? "ok" : "REFUSED/held, core 2");
            } else {
                std::fprintf(stderr, "[lane] MSC_LANE_CORE3=1: the fourth core is not proven / an online session is up: the lane stays on core 2\n");
            }
        }
    }
    if (onCore3) {
        if (const char* p = std::getenv("MSC_VITA_PRIO_LANE")) {   // (only elevate priority when actually placed on Core 3 so stock 3-core Vitas never suffer Core 2 priority inversion)
            const int prio = static_cast<int>(std::strtoul(p, nullptr, 0));
            const int r = sceKernelChangeThreadPriority(sceKernelGetThreadId(), prio);
            std::fprintf(stderr, "[lane] thread priority 0x%X (%s)\n", static_cast<unsigned>(prio), r >= 0 ? "ok" : "REFUSED");
        }
    }
#else
    if (const char* f = std::getenv("MSC_LANE_FREEZE_MS")) usleep(static_cast<unsigned>(std::strtoul(f, nullptr, 0)) * 1000u);   // (host test: a lane thread that is not scheduled)
#endif
    for (;;) {
        const int r = LaneStep(false);
        if (r > 0) continue;
        if (r < 0) { LaneDelayUs(50); continue; }   // the game thread is running the queue itself
        lane.sleeping.store(1, std::memory_order_seq_cst);
        if (lane.next.load(std::memory_order_seq_cst) != lane.tail.load(std::memory_order_seq_cst)) { lane.sleeping.store(0, std::memory_order_seq_cst); continue; }
        lane.wake.Wait(-1);
        lane.sleeping.store(0, std::memory_order_seq_cst);
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

void LaneLogStats(const char* when) {
    if (!lane.started) return;
    std::fprintf(stderr, "[lane] %s: %llu jobs queued (%llu run on the lane thread, %llu on the game thread after a stall), %llu refused because the queue was full; "
                 "%llu waits (%llu blocked, %.3f s, max %.1f ms), %llu stalls, %llu waits over 1 s, %llu wake posts dropped by the kernel semaphore; %llu jobs run by the game thread at the join before the lane thread had started them (%llu takeover calls)\n",
                 when ? when : "stats", (unsigned long long)lane.submitted, (unsigned long long)lane.ranOnLane.load(), (unsigned long long)lane.ranOnGame.load(),
                 (unsigned long long)lane.refusedFull, (unsigned long long)lane.waits, (unsigned long long)lane.waitsBlocked, lane.waitSeconds, lane.maxWait * 1000.0,
                 (unsigned long long)lane.stalls, (unsigned long long)lane.longWaits, (unsigned long long)(lane.postDrops.load() + g_semOverflows.load()),
                 (unsigned long long)lane.takeovers, (unsigned long long)lane.takeoverCalls);
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

int NpLaneForce(void) {   // gt27: start the lane for a caller of its own; NpLaneEnabled() (the skinning's switch, MSC_SKIN_LANE) is unchanged
    if (!lane.forced) {
        lane.forced = true;
        if (!LaneStart()) { lane.forced = false; return 0; }
        std::fprintf(stderr, "[lane] helper lane started for the asynchronous snapshot blend (MSC_BLEND_ASYNC); the skinning lane stays %s\n", NpLaneEnabled() ? "on" : "off");
    }
    return lane.started ? 1 : 0;
}

int NpLaneTrySubmit(NpFsJobFn fn, void* arg) {
    if ((!NpLaneEnabled() && !lane.forced) || !LaneStart()) return 0;
    const uint32_t t = lane.tail.load(std::memory_order_relaxed);
    if (t - lane.next.load(std::memory_order_acquire) >= kLaneQueue) { ++lane.refusedFull; return 0; }
    lane.q[t & (kLaneQueue - 1)] = {fn, arg};
    ++lane.submitted;
    lane.tail.store(t + 1, std::memory_order_seq_cst);
    if (lane.sleeping.exchange(0, std::memory_order_seq_cst) != 0 && !lane.wake.Post()) lane.postDrops.fetch_add(1, std::memory_order_relaxed);
    return 1;
}

double NpFsJoinWaitedSeconds(void) { return g.waitSeconds; }   // pace24: cumulative wall time the game thread spent blocked in NpFsJoin (the frame-send job not finished)
double NpLaneWaitedSeconds(void) { return lane.waitSeconds; }   // cumulative wall time the game thread spent in NpLaneWait (MSC_SKIN_PROF)

void NpLaneSubmit(NpFsJobFn fn, void* arg) {
    if (NpLaneTrySubmit(fn, arg)) return;
    if (!lane.started || (lane.enabled <= 0 && !lane.forced)) { fn(arg); return; }
    NpLaneWait();   // full: drain first (jobs of one submitter must not overtake each other), then queue again
    if (!NpLaneTrySubmit(fn, arg)) fn(arg);
}

// lane24 FIX-07: the lane thread has been woken but not scheduled (on core 2 the front-end record player runs at the lane's priority and a woken thread waits for the player's slice: 10-14 ms): the jobs
// still in the queue, none of them taken yet, are run here, in order, at once. Exact: it is the very job function with the very argument, and LaneStep's `busy` / `next` hand-over lets only one executor
// at a time have a job (the lane thread that wakes later finds the queue empty). A job the lane thread is inside is not touched (-1: the caller then waits for it). Returns jobs run. Before this the game
// thread waited kLaneStallSlices * kLaneSliceSeconds (12 ms) for a thread that had not started before it ran the queue itself: the 12.4-12.9 ms maximum waits of console s52.
int NpLaneTakeUnstarted(void) {
    if (!lane.started) return 0;
    int n = 0;
    while (lane.next.load(std::memory_order_relaxed) != lane.tail.load(std::memory_order_acquire)) {
#ifdef NP_LANE_TAKE_BREAK   // (host test mutant: the busy hand-over is ignored, a job the lane thread is inside is joined by a second executor)
        lane.busy.store(0, std::memory_order_release);
#endif
        if (LaneStep(true) <= 0) break;
        ++n;
    }
    ++lane.takeoverCalls;
    lane.takeovers += static_cast<uint64_t>(n);
    return n;
}

void NpLaneWait(void) {
    if (!lane.started) return;
    const uint32_t target = lane.tail.load(std::memory_order_relaxed);   // (the game thread is the only submitter)
    if (lane.finished.load(std::memory_order_acquire) == target) return;
    ++lane.waits;
    const double t0 = NpRealSeconds();
    uint32_t last = lane.finished.load(std::memory_order_seq_cst);
    int still = 0;
    double nextLog = 1.0;
    bool blocked = false;
    for (;;) {
        if (lane.finished.load(std::memory_order_seq_cst) == target) break;
        lane.waitTarget.store(target, std::memory_order_seq_cst);
        lane.waiting.store(1, std::memory_order_seq_cst);
        if (lane.finished.load(std::memory_order_seq_cst) == target) { lane.waiting.store(0, std::memory_order_seq_cst); break; }
        blocked = true;
        lane.done.Wait(kLaneSliceSeconds);
        lane.waiting.store(0, std::memory_order_seq_cst);
        const uint32_t f = lane.finished.load(std::memory_order_seq_cst);
        if (f == target) break;
        if (f != last) { last = f; still = 0; continue; }
        if (++still >= kLaneStallSlices && lane.busy.load(std::memory_order_acquire) == 0) {
            // The lane thread has not started a job for kLaneStallSlices * 4 ms (it is not scheduled, or it never started): run the queue here, in order.
            if (++lane.stalls <= 3) std::fprintf(stderr, "[lane] the lane thread made no progress for %.0f ms (%u of %u jobs done): the game thread runs the queue\n",
                                                  kLaneStallSlices * kLaneSliceSeconds * 1000.0, static_cast<unsigned>(f), static_cast<unsigned>(target));
            while (LaneStep(true) > 0) {}
            still = 0;
            last = lane.finished.load(std::memory_order_seq_cst);
        }
        const double waited = NpRealSeconds() - t0;
        if (waited >= nextLog) {   // inside a job that has not returned: nothing can be done but say so
            ++lane.longWaits;
            if (lane.longWaits <= 10) std::fprintf(stderr, "[lane] still waiting for the lane thread after %.0f s (%u of %u jobs done, busy %d)\n", waited,
                                                    static_cast<unsigned>(lane.finished.load()), static_cast<unsigned>(target), lane.busy.load());
            nextLog += 1.0;
        }
    }
    const double w = NpRealSeconds() - t0;
    if (blocked) ++lane.waitsBlocked;
    lane.waitSeconds += w;
    if (w > lane.maxWait) lane.maxWait = w;
}

} // extern "C"
