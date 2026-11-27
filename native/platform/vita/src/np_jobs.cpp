// np_jobs.cpp: the helper-core job pool (np_jobs.h has the contract and the environment).
//
// One persistent worker thread per extra lane, the caller is lane 0. A job is published by writing
// its description and bumping a generation counter; the workers that are needed are woken
// (an idle worker polls its flag for MSC_JOBS_SPIN_US, then sleeps on a condition variable);
// every lane then claims chunks of the range from one atomic counter until it is used up (dynamic
// balance: ten characters of which three are off screen cost very different amounts); the caller
// waits for the workers that took part. No allocation after start-up, no locks on the fork path
// when a worker is polling.
//
// The same file runs on the Vita (pthreads over sceKernel threads; each worker pins itself to its
// core and priority), on armhf Linux under qemu and on x86 (tests: tests/test_jobs.cpp).
#include "np_jobs.h"
#include "np_time.h"
#include "np_vita_core3.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <pthread.h>

#if defined(__vita__)
#include "np_vita_internal.h"
extern "C" int NpVitaCore3LaneOk(void);   // vita_threads.cpp (T8)
extern "C" int NpVitaCore3Work(void);     // vita_threads.cpp (core3x15): MSC_VITA_CORE3_WORK, bit 1 = a job lane on core 3
#include <psp2/kernel/error.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#elif defined(__linux__)
#include <sched.h>
#include <unistd.h>
#endif

extern "C" int NpSimtLevel(void) __attribute__((weak));   // src/Game/NpSimRun17.cpp (pace18)

namespace {

constexpr uint32_t kMaxWorkers = NP_JOBS_MAX_LANES - 1;
constexpr uint32_t kMaxChunks = 64;
constexpr uint64_t kSlotClosed = 1ull << 16;

enum class Order { kForward, kReverse, kShuffle, kInterleave };

struct alignas(64) Worker {
    pthread_t thread;
    uint32_t lane = 0;
    int core = -1;
    std::atomic<uint32_t> wakeGen{0};      // generation this worker was asked to run
#if defined(__vita__)
    SceUID wakeSem = -1;                   // int28: direct kernel binary semaphore (replaces pthread-embedded mutex/cond)
#else
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
#endif
    std::atomic<bool> sleeping{false};
    std::atomic<uint64_t> busyUs{0};
    std::atomic<uint64_t> items{0};
    std::atomic<uint64_t> startedUs{0};    // when it picked up the current job (monotonic real us)
    std::atomic<uint64_t> endUs{0};        // (core3x15) when it finished its share of the last job it took part in
    std::atomic<bool> withdrawn{false};    // (core3x15) the lane left at start (core 3 refused or not open): the pool does not count on it
};

struct Pool {
    bool started = false;
    uint32_t lanes = 1;
    uint32_t spinUs = 200;
    uint32_t recheckMs = 0;                // MSC_JOBS_RECHECK_MS (stab13): 0 = the untimed waits of 1.3; n = a sleeping lane / caller looks at its flag again every n ms
    std::atomic<uint64_t> recheckRepairs{0};   // timed wake-ups that found the awaited flag already set: a wake-up that was lost (the signal never reached the waiter)
    Order order = Order::kForward;
    bool verify = false;
    Worker workers[kMaxWorkers];
    // the current job (int29: separate producer/shared/consumer atomics onto distinct 64-byte cache lines to eliminate Cortex-A9 false sharing)
    alignas(64) NpRangeFn fn = nullptr;
    void* ctx = nullptr;
    size_t count = 0, chunk = 1;
    uint32_t nChunks = 0, usedLanes = 1;
    uint8_t perm[kMaxChunks];
    alignas(64) std::atomic<uint32_t> nextChunk{0};
    alignas(64) std::atomic<uint32_t> finished{0};     // workers done with the current job
    alignas(64) std::atomic<uint32_t> joinNeed{0xFFFFFFFFu}; // int28 (Carbon M3): workers that actually joined; suppresses intermediate doneSem wakes
    // Participation record of the current job: bits 32.. generation, bit 16 closed, bits 0..15
    // number of workers that started. A worker joins by CAS only while the generation is its own
    // and the job is open; the caller closes the job when its own share is done and waits only
    // for the workers that joined (a worker that wakes up late finds the job closed and goes back
    // to sleep: the caller never waits for a wake-up).
    alignas(64) std::atomic<uint64_t> slot{0};
    std::atomic<bool> inCall{false};       // a fork-join is in flight (not re-entrant, one producer)
#if defined(__vita__)
    SceUID doneSem = -1;                   // int28: direct kernel binary semaphore for caller join wait
#else
    pthread_mutex_t doneMutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t doneCond = PTHREAD_COND_INITIALIZER;
#endif
    alignas(64) std::atomic<bool> callerSleeping{false};
    std::atomic<uint64_t> lateStartsSeen{0};
    std::atomic<bool> stop{false};
    uint32_t generation = 0;
    uint64_t callSeq = 0;
    // statistics (producer thread writes the call counters; lanes write their own)
    // counters touched by any caller before the in-flight test are atomic; the rest only while a call holds inCall
    std::atomic<uint64_t> calls{0}, serialFallbacks{0}, busyFallbacks{0};
    uint64_t parallelCalls = 0, verifyRuns = 0, verifyMismatches = 0, lateStarts = 0;
    uint64_t lane0Items = 0, lane0BusyUs = 0, callerWaitUs = 0, wakeLatencyTotal = 0;
    uint32_t wakeLatencyMax = 0;
    // (core3x15, MSC_VITA_CORE3_WORK bit 1) the last worker is on the system core: used only by calls that have work for every lane, and given up when it keeps the caller waiting (LaneGuard)
    bool lane3 = false;
    uint32_t lane3Pos = 1;                 // (nxr6a) where the lane on the system core goes among the workers a call wakes: 1 = after the first (core 2 -> core 3 -> core 1), 0 = in front (nxcpu25's first order, MSC_JOBS_C3FIRST=2)
    bool lane3First = true;                // (nxcpu25, MSC_JOBS_C3FIRST=0|1|2, default 1 = second place (nxr6a), 2 = first place; MSC_JOBS_ORDER=core3first is the same as 1) the lane on the system core is the FIRST worker a call wakes, not the last (needs lane3)
    uint64_t lane3FirstCalls = 0, lane3FirstSmall = 0;   // calls that woke it in front / of those, calls that did not have work for every lane (the default order would not have used it)
    std::atomic<bool> lane3Off{false};
    NpVitaCore3::LaneGuard lane3Guard;
    uint64_t lane3Calls = 0, lane3Late = 0, lane3LateUsMax = 0;
};
Pool g_pool;
std::atomic<int> g_core3InJob{0};   // (lane24 FIX-09) 1 while the job lane on the system core is inside the chunks of a job: the core 3 watch does not start its 3 ms spell then (NpJobsCore3Busy)
thread_local int t_inJob = 0;

// (stab13, from the Galaxy port's pthread findings) A sleeping lane / the sleeping caller waits on a condition variable that VitaSDK's pthread-embedded may lose a wake-up on.
// With MSC_JOBS_RECHECK_MS=n the wait is timed (absolute CLOCK_REALTIME time, what the library compares) and the caller of this function re-reads its flag when the time is
// up; returns true when the wait ended by the time-out. n = 0: the plain wait of 1.3. NP_JOBS_TEST_DROPWAKE (host test only) makes every signal a no-op.
inline bool CondWait(pthread_cond_t* cond, pthread_mutex_t* mutex, uint32_t ms) {
    if (ms == 0) { pthread_cond_wait(cond, mutex); return false; }
    timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += ms / 1000u;
    until.tv_nsec += static_cast<long>(ms % 1000u) * 1000000L;
    if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
    return pthread_cond_timedwait(cond, mutex, &until) == ETIMEDOUT;
}
#ifdef NP_JOBS_TEST_DROPWAKE
#define NP_JOBS_SIGNAL(c) ((void)0)
#else
#define NP_JOBS_SIGNAL(c) pthread_cond_signal(c)
#endif

// (wave 9 simthread) The floating-point control word of the caller, applied by a worker before it runs its chunks: a job must compute exactly what the
// caller's serial loop would (flush-to-zero, default NaN and rounding are per thread on the A9; np_framesend.cpp does the same for its worker).
#if defined(__arm__)
inline uint32_t ReadFpscr() { uint32_t v; __asm__ volatile("vmrs %0, fpscr" : "=r"(v)); return v & 0x07C09F00u; }
inline void ApplyFpscr(uint32_t want) {
    uint32_t cur;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(cur));
    const uint32_t mixed = (cur & ~0x07C09F00u) | want;
    if (mixed != cur) __asm__ volatile("vmsr fpscr, %0" :: "r"(mixed));
}
#else
inline uint32_t ReadFpscr() { return 0; }
inline void ApplyFpscr(uint32_t) {}
#endif
uint32_t g_jobFpscr = 0;   // written by the caller before the job is published, read by the workers after they joined it

inline uint64_t NowUs() {
#if defined(__vita__)
    return sceKernelGetProcessTimeWide();   // int29: direct kernel microsecond clock (avoids double multiply/conversion per fork/join)
#else
    return static_cast<uint64_t>(NpRealSeconds() * 1e6);
#endif
}

// Runs this lane's chunks until the range is used up. Returns items processed.
uint64_t RunChunks(Pool& p, uint32_t lane) {
    uint64_t items = 0;
    t_inJob = 1;
    const auto run = [&](uint32_t i) {
        const size_t begin = static_cast<size_t>(i) * p.chunk;
        size_t end = begin + p.chunk;
        if (end > p.count) end = p.count;
        p.fn(p.ctx, begin, end, lane);
        items += end - begin;
    };
    if (p.order == Order::kInterleave) {
        // static assignment: lane L takes chunks L, L + usedLanes, ... (no shared counter)
        for (uint32_t i = lane; i < p.nChunks; i += p.usedLanes) run(i);
    } else {
        for (;;) {
            const uint32_t claim = p.nextChunk.fetch_add(1, std::memory_order_acq_rel);
            if (claim >= p.nChunks) break;
            run(p.order == Order::kReverse ? p.nChunks - 1 - claim : p.order == Order::kShuffle ? p.perm[claim] : claim);
        }
    }
    t_inJob = 0;
    return items;
}

void* WorkerMain(void* arg) {
    Worker& w = *static_cast<Worker*>(arg);
    Pool& p = g_pool;
#if defined(__vita__)
    // Affinity: this worker's core; priority just below the audio output thread, above the render
    // thread (a job is never queued behind a render burst: np/docs/vita.md "Core plan").
    {
        // (T8) A lane on the system core obeys the core 3 rules (np_vita_core3.h): it needs the proven core and no online session, it is the first thing the thread does (nothing runs on the
        // default mask first), and it never spins (spinUs below), so it only ever holds the core for the chunks of a job and sleeps on its condition variable between jobs.
        if (w.core == 3 && !NpVitaCore3LaneOk()) {
            std::fprintf(stderr, "[jobs] worker lane %u: core 3 is not available (not proven by the probe, lost, or an online session is up): the lane is withdrawn\n", w.lane);
            w.withdrawn.store(true);
            return nullptr;
        }
        int mask = 0x00010000 << w.core;
        const int r = sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), mask);
        if (w.core == 3 && r >= 0) VitaThreads::Core3Register(sceKernelGetThreadId());
        int prio = 0x10000100 - 16;
        if (const char* t = std::getenv("MSC_JOBS_PRIO")) prio = static_cast<int>(std::strtoul(t, nullptr, 0));
        const int r2 = sceKernelChangeThreadPriority(sceKernelGetThreadId(), prio);
        std::fprintf(stderr, "[jobs] worker lane %u on core %d: affinity 0x%X, priority 0x%X (%s/%s)\n", w.lane, w.core, static_cast<unsigned>(r),
                     static_cast<unsigned>(prio), r >= 0 ? "ok" : "REFUSED", r2 >= 0 ? "ok" : "REFUSED");
        if (VitaMem::RegisterThread) VitaMem::RegisterThread("job worker");
        // (W5 PIPE) Core 3 is the system core (SCE_KERNEL_CPU_MASK_SYSTEM): without the plug-in that lifts the affinity check the system refuses it, and the worker would
        // run on a game core and compete with the game's threads. It leaves instead; the pool takes only the workers that join a job, so the other lanes go on as before.
        if (r < 0 && w.core == 3) {
            std::fprintf(stderr, "[jobs] worker lane %u: the system refused core 3, the lane is withdrawn (MSC_JOBS_CORES without 3 avoids this line)\n", w.lane);
            w.withdrawn.store(true);
            return nullptr;
        }
    }
#elif defined(__linux__)
    if (w.core >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(static_cast<unsigned>(w.core), &set);
        sched_setaffinity(0, sizeof(set), &set);   // best effort (a test box may have fewer cores)
    }
#endif
    uint32_t seen = 0;
    // (T8) a lane on core 3 parks at once; the others keep MSC_JOBS_SPIN_US (default 200 us)
#if defined(__vita__)
    const uint32_t laneSpinUs = NpVitaCore3::JobSpinUs(w.core, p.spinUs);
#else
    const uint32_t laneSpinUs = p.spinUs;
#endif
    for (;;) {
        // wait for a new generation: poll (batched clock check), then sleep
        const uint64_t spinUntil = laneSpinUs ? (NowUs() + laneSpinUs) : 0;
        uint32_t spinIter = 0;
        bool spunOut = (laneSpinUs == 0);
        while (w.wakeGen.load(std::memory_order_acquire) == seen && !p.stop.load(std::memory_order_acquire)) {
            if (!spunOut && (spinIter++ & 31u) == 31u && NowUs() >= spinUntil) spunOut = true;
            if (spunOut) {
#if defined(__vita__)
                w.sleeping.store(true, std::memory_order_seq_cst);
                if (w.wakeGen.load(std::memory_order_seq_cst) == seen && !p.stop.load(std::memory_order_seq_cst)) {
                    SceUInt timeout = p.recheckMs ? (p.recheckMs * 1000u) : 0u;
                    const int rc = sceKernelWaitSema(w.wakeSem, 1, p.recheckMs ? &timeout : nullptr);
                    if (rc == static_cast<int>(SCE_KERNEL_ERROR_WAIT_TIMEOUT) && (w.wakeGen.load(std::memory_order_acquire) != seen || p.stop.load(std::memory_order_acquire)))
                        p.recheckRepairs.fetch_add(1, std::memory_order_relaxed);
                    else if (rc < 0 && rc != static_cast<int>(SCE_KERNEL_ERROR_WAIT_TIMEOUT))
                        sceKernelDelayThread(1000);
                }
                w.sleeping.store(false, std::memory_order_seq_cst);
                if (w.wakeGen.load(std::memory_order_acquire) != seen || p.stop.load(std::memory_order_acquire)) break;
                continue;
#else
                pthread_mutex_lock(&w.mutex);
                w.sleeping.store(true);
                while (w.wakeGen.load() == seen && !p.stop.load()) {
                    if (CondWait(&w.cond, &w.mutex, p.recheckMs) && (w.wakeGen.load() != seen || p.stop.load())) p.recheckRepairs.fetch_add(1, std::memory_order_relaxed);
                }
                w.sleeping.store(false);
                pthread_mutex_unlock(&w.mutex);
                break;
#endif
            }
#if defined(__arm__)
            __asm__ __volatile__("yield" ::: "memory");
#elif defined(__linux__)
            __asm__ __volatile__("" ::: "memory");
#endif
        }
        if (p.stop.load(std::memory_order_acquire)) return nullptr;
        seen = w.wakeGen.load(std::memory_order_acquire);
        // join the job of this generation, if it is still open
        uint64_t v = p.slot.load();
        bool joined = false;
        for (;;) {
            if (static_cast<uint32_t>(v >> 32) != seen || (v & kSlotClosed)) break;
            if (p.slot.compare_exchange_weak(v, v + 1)) { joined = true; break; }
        }
        if (!joined) { p.lateStartsSeen.fetch_add(1, std::memory_order_relaxed); continue; }
        const uint64_t t0 = NowUs();
        w.startedUs.store(t0, std::memory_order_relaxed);
        ApplyFpscr(g_jobFpscr);
        if (w.core == 3) g_core3InJob.store(1, std::memory_order_relaxed);
        const uint64_t items = RunChunks(p, w.lane);
        if (w.core == 3) g_core3InJob.store(0, std::memory_order_relaxed);
        const uint64_t tEnd = NowUs();
        w.endUs.store(tEnd, std::memory_order_relaxed);   // (before finished: the caller reads it after the join)
        w.busyUs.fetch_add(tEnd - t0, std::memory_order_relaxed);
        w.items.fetch_add(items, std::memory_order_relaxed);
        const uint32_t done = p.finished.fetch_add(1, std::memory_order_seq_cst) + 1u;
#if defined(__vita__)
        if (done >= p.joinNeed.load(std::memory_order_seq_cst) && p.callerSleeping.load(std::memory_order_seq_cst)) {
            if (p.callerSleeping.exchange(false, std::memory_order_seq_cst))
                sceKernelSignalSema(p.doneSem, 1);
        }
#else
        if (done >= p.joinNeed.load(std::memory_order_seq_cst) && p.callerSleeping.load()) {
            pthread_mutex_lock(&p.doneMutex);
            NP_JOBS_SIGNAL(&p.doneCond);
            pthread_mutex_unlock(&p.doneMutex);
        }
#endif
    }
}

bool EnvFlag(const char* name) {
    const char* e = std::getenv(name);
    return e && *e && *e != '0';
}

// (core3x15) Is the system core's lane usable now? Vita: proven, not lost, no online session (vita_threads.cpp). Host: NP_CORE3_FAKE=1 is the fake core-3 kernel of the proofs (the host has no system
// core; the lane then runs on whatever core the OS gives it, which is all the proofs need: they compare results, not speed).
bool Core3Open() {
#if defined(__vita__)
    return NpVitaCore3LaneOk() != 0;
#else
    return EnvFlag("NP_CORE3_FAKE");
#endif
}
int Core3Work() {
#if defined(__vita__)
    return NpVitaCore3Work();
#else
    return NpVitaCore3::WorkBits(std::getenv("MSC_VITA_CORE3_WORK"));
#endif
}

uint32_t HardwareCores() {
#if defined(__vita__)
    return 3;
#elif defined(__linux__)
    const long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? static_cast<uint32_t>(n) : 1;
#else
    return 1;
#endif
}

// "12" -> {2, 1}: the cores for workers 0, 1, 2 in the order given by MSC_JOBS_CORES; default 2, 1.
void WorkerCores(int out[kMaxWorkers]) {
    out[0] = 2; out[1] = 1; out[2] = 0;
#if defined(__vita__)
    // pace18: with MSC_SIMT=2 np_sim sits on core 2 (VitaThreads role kSim), so worker 0 goes to core 0, where the game thread draws the window and then waits for the join (the step's lanes are then
    // np_sim as caller, a worker on core 0 and one on core 1). Only a MSC_SIMT in force changes it; MSC_JOBS_CORES always wins. MSC_SIMT=0 and 1 keep 2, 1.
    if (!std::getenv("MSC_JOBS_CORES") && NpSimtLevel && NpSimtLevel() == 2) {
        out[0] = 0; out[1] = 1; out[2] = 2;
        std::fprintf(stderr, "[jobs] MSC_SIMT=2: worker cores 0, 1 (core 2 is np_sim's); MSC_JOBS_CORES overrides\n");
    }
#endif
    if (const char* t = std::getenv("MSC_JOBS_CORES")) {
        uint32_t n = 0;
        for (const char* c = t; *c && n < kMaxWorkers; ++c)
            if (*c >= '0' && *c <= '3') out[n++] = *c - '0';
    }
}

} // namespace

extern "C" {

uint32_t NpJobsInit(uint32_t lanes) {
    Pool& p = g_pool;
    if (p.started) return p.lanes;
    if (lanes == 0) {
        if (const char* e = std::getenv("MSC_JOBS")) lanes = static_cast<uint32_t>(std::strtoul(e, nullptr, 10));
        else lanes = HardwareCores() >= 3 ? 3 : 1;
    }
    if (lanes < 1) lanes = 1;
    if (lanes > NP_JOBS_MAX_LANES) lanes = NP_JOBS_MAX_LANES;
    if (const char* e = std::getenv("MSC_JOBS_SPIN_US")) p.spinUs = static_cast<uint32_t>(std::strtoul(e, nullptr, 10));
    if (const char* e = std::getenv("MSC_JOBS_RECHECK_MS")) { const unsigned long v = std::strtoul(e, nullptr, 10); p.recheckMs = v > 1000ul ? 1000u : static_cast<uint32_t>(v); }
    p.verify = EnvFlag("MSC_JOBS_VERIFY");
    if (const char* e = std::getenv("MSC_JOBS_C3FIRST")) { p.lane3First = !(*e == '0' && e[1] == 0); p.lane3Pos = (*e == '2' && e[1] == 0) ? 0u : 1u; }   // (nxcpu25; nxr6a: 2 = the front order, any other non-0 = the second place) =0: the lane on the system core is the last worker a call wakes, as before
    if (const char* e = std::getenv("MSC_JOBS_ORDER")) {
        if (!std::strcmp(e, "reverse")) p.order = Order::kReverse;
        else if (!std::strcmp(e, "shuffle")) p.order = Order::kShuffle;
        else if (!std::strcmp(e, "interleave")) p.order = Order::kInterleave;
        else if (!std::strcmp(e, "core3first")) p.lane3First = true;   // (nxcpu25) the same as MSC_JOBS_C3FIRST=1; the chunk order stays forward
    }
    int cores[kMaxWorkers];
    WorkerCores(cores);
    // (core3x15) MSC_VITA_CORE3_WORK bit 1: one more lane, on the system core, if it is proven (the probe runs here, once; unproven: nothing changes, not even the log)
    if ((Core3Work() & NpVitaCore3::kWorkJobs) && lanes >= 2 && Core3Open()) {
        const unsigned want = NpVitaCore3::JobLanesWith(Core3Work(), true, std::getenv("MSC_JOBS_CORES") != nullptr, lanes, NP_JOBS_MAX_LANES, cores);
        if (want != lanes) {
            lanes = want;
            p.lane3 = true;
            const char* e = std::getenv("MSC_VITA_CORE3_SLOW_US");
            p.lane3Guard = NpVitaCore3::LaneGuard(e ? static_cast<unsigned>(std::strtoul(e, nullptr, 10)) : 0u);
            std::fprintf(stderr, "[jobs] MSC_VITA_CORE3_WORK=1: lane %u on the system core (parks at once; used only by jobs with work for every lane; given up if it keeps the caller waiting)\n", lanes - 1);
        }
    }
    p.lanes = 1;
#if defined(__vita__)
    p.doneSem = sceKernelCreateSema("np_jobs_done", 0, 0, 1, nullptr);
    if (p.doneSem < 0) {
        std::fprintf(stderr, "[jobs] cannot create done semaphore (0x%X); running with 1 lane\n", static_cast<unsigned>(p.doneSem));
        p.started = true;
        return 1;
    }
#endif
    for (uint32_t i = 0; i + 1 < lanes; ++i) {
        Worker& w = p.workers[i];
        w.lane = i + 1;
        w.core = cores[i];
#if defined(__vita__)
        char sname[24];
        std::snprintf(sname, sizeof sname, "np_job_w%u", i);
        w.wakeSem = sceKernelCreateSema(sname, 0, 0, 1, nullptr);
        if (w.wakeSem < 0) {
            std::fprintf(stderr, "[jobs] worker %u: sema failed (0x%X); running with %u lane(s)\n", i + 1, static_cast<unsigned>(w.wakeSem), p.lanes);
            break;
        }
#endif
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 512 * 1024);
        const int rc = pthread_create(&w.thread, &attr, WorkerMain, &w);
        pthread_attr_destroy(&attr);
        if (rc != 0) {
#if defined(__vita__)
            sceKernelDeleteSema(w.wakeSem);
            w.wakeSem = -1;
#endif
            std::fprintf(stderr, "[jobs] worker %u: pthread_create failed (%d); running with %u lane(s)\n", i + 1, rc, p.lanes);
            break;
        }
        ++p.lanes;
    }
    if (p.lane3 && p.lanes != lanes) p.lane3 = false;   // (the last worker did not start: there is no lane on core 3)
    if (p.order == Order::kInterleave) p.lane3First = false;   // (its static chunk assignment wants the lanes in number order)
    if (p.lane3First) {
        if (p.lane3) std::fprintf(stderr, "[jobs] core 3 early (MSC_JOBS_C3FIRST=0 turns it off, =2 is the 25 Nov front order): a call wakes the core 2 lane, then the lane on the system core, the core 1 lane last, so a call with work for 2 lanes stays on the spinning core 2 lane and one for 3 lanes keeps off core 1; the lane obeys the same rules as before (never spins, online retreat, given up if it keeps the caller waiting)\n");
        else p.lane3First = false;   // (no lane on the system core: the order is as it was, no line)
    }
    p.started = true;
    std::fprintf(stderr, "[jobs] %u lane(s) (caller + %u worker(s)), spin %u us, order %s%s\n", p.lanes, p.lanes - 1, p.spinUs,
                 p.order == Order::kForward ? "forward" : p.order == Order::kReverse ? "reverse" : p.order == Order::kShuffle ? "shuffle" : "interleave",
                 p.verify ? ", verify on" : "");
    return p.lanes;
}

void NpJobsShutdown(void) {
    Pool& p = g_pool;
    if (!p.started) return;
    p.stop.store(true, std::memory_order_seq_cst);
    for (uint32_t i = 0; i + 1 < p.lanes; ++i) {
        Worker& w = p.workers[i];
#if defined(__vita__)
        if (w.wakeSem >= 0) sceKernelSignalSema(w.wakeSem, 1);
#else
        pthread_mutex_lock(&w.mutex);
        pthread_cond_signal(&w.cond);
        pthread_mutex_unlock(&w.mutex);
#endif
        pthread_join(w.thread, nullptr);
#if defined(__vita__)
        if (w.wakeSem >= 0) { sceKernelDeleteSema(w.wakeSem); w.wakeSem = -1; }
#endif
    }
#if defined(__vita__)
    if (p.doneSem >= 0) { sceKernelDeleteSema(p.doneSem); p.doneSem = -1; }
#endif
    p.started = false;
    p.lanes = 1;
    p.stop.store(false);
}

uint32_t NpJobsLanes(void) { return g_pool.started ? g_pool.lanes : 1; }
int NpJobsCore3Busy(void) { return g_core3InJob.load(std::memory_order_relaxed); }   // lane24 FIX-09: the lane on core 3 is inside a job
int NpInJob(void) { return t_inJob; }

static uint32_t ParallelForImpl(size_t count, size_t minPerLane, NpRangeFn fn, void* ctx, uint32_t avoidCores);

uint32_t NpParallelFor(size_t count, size_t minPerLane, NpRangeFn fn, void* ctx) { return ParallelForImpl(count, minPerLane, fn, ctx, 0); }

// (lane24 FIX-20) The same call for work whose caller does not want some cores' workers: avoidCores is a mask of cores (bit n = core n); workers on those cores, and always the caller's own core 0, are
// not woken for this call (they stay asleep, nothing of the pool changes for the other calls). With avoidCores 0 this IS NpParallelFor. The lanes used are then the remaining workers in worker order, and
// the lane on the system core only when the call has work for every remaining lane. Not with MSC_JOBS_ORDER=interleave (its static chunk assignment wants lane numbers 0..n-1: the avoid mask is ignored).
uint32_t NpParallelForAvoid(size_t count, size_t minPerLane, NpRangeFn fn, void* ctx, uint32_t avoidCores) { return ParallelForImpl(count, minPerLane, fn, ctx, avoidCores); }

static uint32_t ParallelForImpl(size_t count, size_t minPerLane, NpRangeFn fn, void* ctx, uint32_t avoidCores) {
    Pool& p = g_pool;
    if (!p.started) NpJobsInit(0);
    p.calls.fetch_add(1, std::memory_order_relaxed);
    if (count == 0) return 1;
    if (minPerLane == 0) minPerLane = 1;
#ifdef NP_JOBS_AVOID_BREAK   // (host test mutant: the avoid mask is ignored)
    avoidCores = 0;
#endif
    if (avoidCores != 0 && p.order == Order::kInterleave) avoidCores = 0;
    if (avoidCores != 0) avoidCores |= 1u;
    // the workers this call may use, in worker order (all of them unless avoidCores)
    uint32_t sel[kMaxWorkers];
    uint32_t nsel = 0;
    for (uint32_t i = 0; i + 1 < p.lanes && i < kMaxWorkers; ++i)
        if (avoidCores == 0 || !((avoidCores >> p.workers[i].core) & 1u)) sel[nsel++] = i;
    // (nxcpu25, MSC_JOBS_ORDER=core3first) the lane on the system core goes to the front of the call's workers while it is open; when it is given up, withdrawn, held or the core is lost it is left out (as the default
    // order drops it); a call that avoids core 3 does not have it in `sel` at all. Same worker, same rules; only which workers a call with work for fewer than every lane wakes changes.
    bool lane3Front = false;
    uint32_t lane3Pos = 0;
#ifdef NP_JOBS_C3FIRST_BREAK   // (host test mutants: 1 the order is not changed; 2 a lane that is given up / held is still woken first)
    if (NP_JOBS_C3FIRST_BREAK != 1)
#endif
    if (p.lane3First && p.lane3 && nsel > 0) {
        uint32_t at = nsel;
        for (uint32_t k = 0; k < nsel; ++k) if (sel[k] == p.lanes - 2) at = k;
        // (nxr6a, NEON r6 #4) the place it takes: the second worker (core 2 -> core 3 -> core 1: a call for 2 lanes stays on the core 2 lane that spins, a call for 3 lanes is still off core 1), or the first when it is the only one
        uint32_t pos = (p.lane3Pos == 0 || nsel < 2) ? 0u : 1u;
#if defined(NP_JOBS_C3FIRST_BREAK) && NP_JOBS_C3FIRST_BREAK == 3   // (host test mutant 3: the front place, nxcpu25's first order, is used again)
        pos = 0;
#endif
        lane3Pos = pos;
        if (at < nsel) {
            bool closed = p.lane3Off.load(std::memory_order_relaxed) || p.workers[p.lanes - 2].withdrawn.load(std::memory_order_relaxed) || !Core3Open();
#if defined(NP_JOBS_C3FIRST_BREAK) && NP_JOBS_C3FIRST_BREAK == 2
            closed = false;
#endif
            if (closed) {
                for (uint32_t k = at; k + 1 < nsel; ++k) sel[k] = sel[k + 1];
                --nsel;
            } else {
                for (uint32_t k = at; k > pos; --k) sel[k] = sel[k - 1];
                for (uint32_t k = at; k < pos; ++k) sel[k] = sel[k + 1];
                sel[pos] = p.lanes - 2;
                lane3Front = true;
            }
        }
    }
    uint32_t use = nsel + 1;
    const size_t byWork = count / minPerLane;
    if (byWork < use) use = byWork ? static_cast<uint32_t>(byWork) : 1;
    // (core3x15) the lane on the system core is the last one: a call that has work for every lane uses it while it is open, a smaller call never does; given up, withdrawn, lost or held: one lane fewer
    const bool useLane3 = !lane3Front && p.lane3 && use == nsel + 1 && nsel > 0 && sel[nsel - 1] == p.lanes - 2;
    if (useLane3 && (p.lane3Off.load(std::memory_order_relaxed) || p.workers[p.lanes - 2].withdrawn.load(std::memory_order_relaxed) || !Core3Open())) use = nsel;
    const bool lane3Called = (useLane3 && use == nsel + 1) || (lane3Front && use >= lane3Pos + 2);
    if (use <= 1) { p.serialFallbacks.fetch_add(1, std::memory_order_relaxed); t_inJob = 1; fn(ctx, 0, count, 0); t_inJob = 0; return 1; }
    // One producer, not re-entrant: a call from inside a job, or while another thread's call is
    // in flight, runs serially on the caller.
    bool expected = false;
    if (t_inJob || !p.inCall.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        p.busyFallbacks.fetch_add(1, std::memory_order_relaxed);
        const int was = t_inJob;
        t_inJob = 1; fn(ctx, 0, count, 0); t_inJob = was;
        return 1;
    }
    ++p.parallelCalls;
    if (lane3Front && use >= lane3Pos + 2) { ++p.lane3FirstCalls; if (use < nsel + 1) ++p.lane3FirstSmall; }
    // chunks: about four per lane, at most kMaxChunks
    size_t chunk = (count + use * 4 - 1) / (use * 4);
    if (chunk < 1) chunk = 1;
    uint32_t nChunks = static_cast<uint32_t>((count + chunk - 1) / chunk);
    while (nChunks > kMaxChunks) { chunk *= 2; nChunks = static_cast<uint32_t>((count + chunk - 1) / chunk); }
    p.fn = fn; p.ctx = ctx; p.count = count; p.chunk = chunk; p.nChunks = nChunks; p.usedLanes = use;
    if (p.order == Order::kShuffle) {
        for (uint32_t i = 0; i < nChunks; ++i) p.perm[i] = static_cast<uint8_t>(i);
        uint32_t s = static_cast<uint32_t>(++p.callSeq) * 2654435761u + 12345u;
        for (uint32_t i = nChunks; i > 1; --i) {
            s = s * 1664525u + 1013904223u;
            const uint32_t j = (s >> 8) % i;
            const uint8_t t = p.perm[i - 1]; p.perm[i - 1] = p.perm[j]; p.perm[j] = t;
        }
    }
    p.nextChunk.store(0, std::memory_order_relaxed);
    p.finished.store(0, std::memory_order_relaxed);
    p.callerSleeping.store(false, std::memory_order_relaxed);
    g_jobFpscr = ReadFpscr();   // (published by the slot store below)
    const uint64_t forkUs = NowUs();
    ++p.generation;
    p.slot.store(static_cast<uint64_t>(p.generation) << 32);
    for (uint32_t i = 0; i + 1 < use; ++i) {
        Worker& w = p.workers[sel[i]];
        w.wakeGen.store(p.generation, std::memory_order_seq_cst);
#if defined(__vita__)
        if (w.sleeping.load(std::memory_order_seq_cst)) {
            if (w.sleeping.exchange(false, std::memory_order_seq_cst))
                sceKernelSignalSema(w.wakeSem, 1);
        }
#else
        if (w.sleeping.load()) {
            pthread_mutex_lock(&w.mutex);
            NP_JOBS_SIGNAL(&w.cond);
            pthread_mutex_unlock(&w.mutex);
        }
#endif
    }
    // lane 0: the caller's own share
    const uint64_t t0 = NowUs();
    const uint64_t mine = RunChunks(p, 0);
    const uint64_t t1 = NowUs();
    p.lane0Items += mine;
    p.lane0BusyUs += t1 - t0;
    // join: close the job, then wait only for the workers that joined it (poll, then sleep)
    // (MSC_JOBS_ORDER=interleave assigns chunks to lanes statically, so every used lane must take
    // part: that test mode waits for all of them and never closes the job early.)
    if (p.order == Order::kInterleave) {
        while (p.finished.load() < use - 1) {}
    }
    uint64_t v = p.slot.load();
    while (!p.slot.compare_exchange_weak(v, v | kSlotClosed)) {}
    const uint32_t need = static_cast<uint32_t>(v & 0xFFFF);
    p.joinNeed.store(need, std::memory_order_seq_cst);
    if (p.finished.load(std::memory_order_acquire) < need) {
        const uint64_t spinUntil = t1 + p.spinUs;
        uint32_t spinIter = 0;
        bool spunOut = (p.spinUs == 0);
        while (p.finished.load(std::memory_order_acquire) < need) {
            if (!spunOut && (spinIter++ & 31u) == 31u && NowUs() >= spinUntil) spunOut = true;
            if (spunOut) {
#if defined(__vita__)
                p.callerSleeping.store(true, std::memory_order_seq_cst);
                if (p.finished.load(std::memory_order_seq_cst) < need) {
                    SceUInt timeout = p.recheckMs ? (p.recheckMs * 1000u) : 0u;
                    const int rc = sceKernelWaitSema(p.doneSem, 1, p.recheckMs ? &timeout : nullptr);
                    if (rc == static_cast<int>(SCE_KERNEL_ERROR_WAIT_TIMEOUT) && p.finished.load(std::memory_order_acquire) >= need)
                        p.recheckRepairs.fetch_add(1, std::memory_order_relaxed);
                    else if (rc < 0 && rc != static_cast<int>(SCE_KERNEL_ERROR_WAIT_TIMEOUT))
                        sceKernelDelayThread(1000);
                }
                p.callerSleeping.store(false, std::memory_order_seq_cst);
                continue;
#else
                pthread_mutex_lock(&p.doneMutex);
                p.callerSleeping.store(true);
                while (p.finished.load() < need) {
                    if (CondWait(&p.doneCond, &p.doneMutex, p.recheckMs) && p.finished.load() >= need) p.recheckRepairs.fetch_add(1, std::memory_order_relaxed);
                }
                p.callerSleeping.store(false);
                pthread_mutex_unlock(&p.doneMutex);
                break;
#endif
            }
#if defined(__arm__)
            __asm__ __volatile__("yield" ::: "memory");
#endif
        }
    }
    p.joinNeed.store(0xFFFFFFFFu, std::memory_order_relaxed);
    p.callerWaitUs += NowUs() - t1;
    if (lane3Called) {
        // (core3x15) how long the caller waited for the lane on the system core after its own share, if that lane took part and was the last
        Worker& w3 = p.workers[p.lanes - 2];
        if (w3.startedUs.load(std::memory_order_relaxed) >= forkUs) {
            const uint64_t e = w3.endUs.load(std::memory_order_relaxed);
            const unsigned late = e > t1 ? static_cast<unsigned>(e - t1) : 0u;
            ++p.lane3Calls;
            if (late > p.lane3LateUsMax) p.lane3LateUsMax = late;
            const NpVitaCore3::LaneGuard::Result gr = p.lane3Guard.Report(late);
            if (gr != NpVitaCore3::LaneGuard::kOk) ++p.lane3Late;
            if (gr == NpVitaCore3::LaneGuard::kRetire) {
                p.lane3Off.store(true, std::memory_order_relaxed);
                std::fprintf(stderr, "[jobs] core 3 lane given up: the caller waited for it more than %u us on %d calls in a window of %d (latest %u us, longest %llu us): jobs go on with %u lane(s) for the rest of the session\n",
                             p.lane3Guard.SlowUs(), NpVitaCore3::kSlowLimit, NpVitaCore3::kWindow, late, static_cast<unsigned long long>(p.lane3LateUsMax), p.lanes - 1);
            }
        }
    }
    p.lateStarts += (use - 1) - need;
    // slowest starter's latency
    uint32_t worst = 0;
    for (uint32_t i = 0; i + 1 < use; ++i) {
        const uint64_t s = p.workers[sel[i]].startedUs.load(std::memory_order_relaxed);
        const uint32_t lat = s > forkUs ? static_cast<uint32_t>(s - forkUs) : 0;   // (a worker that did not join keeps an old stamp: 0)
        if (lat > worst) worst = lat;
    }
    p.wakeLatencyTotal += worst;
    if (worst > p.wakeLatencyMax) p.wakeLatencyMax = worst;
    p.inCall.store(false, std::memory_order_release);
    return use;
}

uint32_t NpParallelForChecked(const char* name, size_t count, size_t minPerLane, NpRangeFn fn, void* ctx, NpHashFn outputHash, void* hashCtx) {
    Pool& p = g_pool;
    if (!p.started) NpJobsInit(0);
    const uint32_t used = NpParallelFor(count, minPerLane, fn, ctx);
    if (!p.verify || !outputHash || used <= 1) return used;
    const uint64_t a = outputHash(hashCtx);
    t_inJob = 1;
    fn(ctx, 0, count, 0);               // the serial reference, same inputs, same output slots
    t_inJob = 0;
    const uint64_t b = outputHash(hashCtx);
    ++p.verifyRuns;
    if (a != b) {
        ++p.verifyMismatches;
        if (p.verifyMismatches <= 20)
            std::fprintf(stderr, "[jobs] MISMATCH job \"%s\" (%lu items, %u lanes): parallel %016llx serial %016llx\n", name ? name : "?", static_cast<unsigned long>(count), used,
                         static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
    }
    return used;
}

void NpJobsGetStats(NpJobsStats* out) {
    Pool& p = g_pool;
    std::memset(out, 0, sizeof(*out));
    out->calls = p.calls.load(); out->parallelCalls = p.parallelCalls; out->serialFallbacks = p.serialFallbacks.load(); out->busyFallbacks = p.busyFallbacks.load();
    out->verifyRuns = p.verifyRuns; out->verifyMismatches = p.verifyMismatches; out->lateStarts = p.lateStarts;
    out->laneItems[0] = p.lane0Items; out->laneBusyUs[0] = p.lane0BusyUs;
    for (uint32_t i = 0; i + 1 < p.lanes && i < kMaxWorkers; ++i) {
        out->laneItems[i + 1] = p.workers[i].items.load();
        out->laneBusyUs[i + 1] = p.workers[i].busyUs.load();
    }
    out->callerWaitUs = p.callerWaitUs; out->wakeLatencyUsTotal = p.wakeLatencyTotal; out->wakeLatencyUsMax = p.wakeLatencyMax;
    out->lanes = p.started ? p.lanes : 1;
    out->recheckRepairs = p.recheckRepairs.load();
}

static uint32_t p_recheck() { return g_pool.recheckMs; }

void NpJobsLogStats(const char* when) {
    NpJobsStats s;
    NpJobsGetStats(&s);
    if (!s.calls) return;
    std::fprintf(stderr, "[jobs] at %s: %u lane(s); %llu calls: %llu parallel, %llu serial (small), %llu busy; caller waited %llu us in all, "
                         "mean wake latency %llu us (max %u us)",
                 when ? when : "?", s.lanes, static_cast<unsigned long long>(s.calls), static_cast<unsigned long long>(s.parallelCalls),
                 static_cast<unsigned long long>(s.serialFallbacks), static_cast<unsigned long long>(s.busyFallbacks),
                 static_cast<unsigned long long>(s.callerWaitUs),
                 static_cast<unsigned long long>(s.parallelCalls ? s.wakeLatencyUsTotal / s.parallelCalls : 0), s.wakeLatencyUsMax);
    for (uint32_t i = 0; i < s.lanes; ++i)
        std::fprintf(stderr, "; lane %u: %llu items, %llu us", i, static_cast<unsigned long long>(s.laneItems[i]), static_cast<unsigned long long>(s.laneBusyUs[i]));
    if (g_pool.lane3) std::fprintf(stderr, "; core 3 lane: %llu calls, %llu late (longest wait %llu us)%s", static_cast<unsigned long long>(g_pool.lane3Calls), static_cast<unsigned long long>(g_pool.lane3Late),
                                   static_cast<unsigned long long>(g_pool.lane3LateUsMax), g_pool.lane3Off.load() ? ", given up" : "");
    if (g_pool.lane3First) std::fprintf(stderr, "; core3early: %llu calls woke the core 3 lane (place %u among the workers; %llu of them with work for fewer than every lane)", static_cast<unsigned long long>(g_pool.lane3FirstCalls), g_pool.lane3Pos + 1u, static_cast<unsigned long long>(g_pool.lane3FirstSmall));
    if (s.lateStarts) std::fprintf(stderr, "; %llu worker wake-ups too late to take part", static_cast<unsigned long long>(s.lateStarts));
    if (p_recheck()) std::fprintf(stderr, "; recheck %u ms: %llu lost wake-up(s) repaired", p_recheck(), static_cast<unsigned long long>(s.recheckRepairs));
    if (s.verifyRuns) std::fprintf(stderr, "; verify: %llu runs, %llu mismatches", static_cast<unsigned long long>(s.verifyRuns), static_cast<unsigned long long>(s.verifyMismatches));
    std::fputc('\n', stderr);
}

} // extern "C"
