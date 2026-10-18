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

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>

#if defined(__vita__)
#include "np_vita_internal.h"
#include <psp2/kernel/threadmgr.h>
#elif defined(__linux__)
#include <sched.h>
#include <unistd.h>
#endif

namespace {

constexpr uint32_t kMaxWorkers = NP_JOBS_MAX_LANES - 1;
constexpr uint32_t kMaxChunks = 64;
constexpr uint64_t kSlotClosed = 1ull << 16;

enum class Order { kForward, kReverse, kShuffle, kInterleave };

struct Worker {
    pthread_t thread;
    uint32_t lane = 0;
    int core = -1;
    std::atomic<uint32_t> wakeGen{0};      // generation this worker was asked to run
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
    std::atomic<bool> sleeping{false};
    std::atomic<uint64_t> busyUs{0};
    std::atomic<uint64_t> items{0};
    std::atomic<uint64_t> startedUs{0};    // when it picked up the current job (monotonic real us)
};

struct Pool {
    bool started = false;
    uint32_t lanes = 1;
    uint32_t spinUs = 200;
    Order order = Order::kForward;
    bool verify = false;
    Worker workers[kMaxWorkers];
    // the current job
    NpRangeFn fn = nullptr;
    void* ctx = nullptr;
    size_t count = 0, chunk = 1;
    uint32_t nChunks = 0, usedLanes = 1;
    uint8_t perm[kMaxChunks];
    std::atomic<uint32_t> nextChunk{0};
    std::atomic<uint32_t> finished{0};     // workers done with the current job
    // Participation record of the current job: bits 32.. generation, bit 16 closed, bits 0..15
    // number of workers that started. A worker joins by CAS only while the generation is its own
    // and the job is open; the caller closes the job when its own share is done and waits only
    // for the workers that joined (a worker that wakes up late finds the job closed and goes back
    // to sleep: the caller never waits for a wake-up).
    std::atomic<uint64_t> slot{0};
    std::atomic<bool> inCall{false};       // a fork-join is in flight (not re-entrant, one producer)
    pthread_mutex_t doneMutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t doneCond = PTHREAD_COND_INITIALIZER;
    std::atomic<bool> callerSleeping{false};
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
};
Pool g_pool;
thread_local int t_inJob = 0;

uint64_t NowUs() { return static_cast<uint64_t>(NpRealSeconds() * 1e6); }

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
        int mask = 0x00010000 << w.core;
        const int r = sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), mask);
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
    for (;;) {
        // wait for a new generation: poll, then sleep
        const uint64_t spinUntil = NowUs() + p.spinUs;
        while (w.wakeGen.load() == seen && !p.stop.load()) {
            if (p.spinUs == 0 || NowUs() >= spinUntil) {
                pthread_mutex_lock(&w.mutex);
                w.sleeping.store(true);
                while (w.wakeGen.load() == seen && !p.stop.load())
                    pthread_cond_wait(&w.cond, &w.mutex);
                w.sleeping.store(false);
                pthread_mutex_unlock(&w.mutex);
                break;
            }
#if defined(__linux__)
            __asm__ __volatile__("" ::: "memory");
#endif
        }
        if (p.stop.load()) return nullptr;
        seen = w.wakeGen.load();
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
        const uint64_t items = RunChunks(p, w.lane);
        w.busyUs.fetch_add(NowUs() - t0, std::memory_order_relaxed);
        w.items.fetch_add(items, std::memory_order_relaxed);
        p.finished.fetch_add(1);
        if (p.callerSleeping.load()) {
            pthread_mutex_lock(&p.doneMutex);
            pthread_cond_signal(&p.doneCond);
            pthread_mutex_unlock(&p.doneMutex);
        }
    }
}

bool EnvFlag(const char* name) {
    const char* e = std::getenv(name);
    return e && *e && *e != '0';
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
    p.verify = EnvFlag("MSC_JOBS_VERIFY");
    if (const char* e = std::getenv("MSC_JOBS_ORDER")) {
        if (!std::strcmp(e, "reverse")) p.order = Order::kReverse;
        else if (!std::strcmp(e, "shuffle")) p.order = Order::kShuffle;
        else if (!std::strcmp(e, "interleave")) p.order = Order::kInterleave;
    }
    int cores[kMaxWorkers];
    WorkerCores(cores);
    p.lanes = 1;
    for (uint32_t i = 0; i + 1 < lanes; ++i) {
        Worker& w = p.workers[i];
        w.lane = i + 1;
        w.core = cores[i];
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 512 * 1024);
        const int rc = pthread_create(&w.thread, &attr, WorkerMain, &w);
        pthread_attr_destroy(&attr);
        if (rc != 0) {
            std::fprintf(stderr, "[jobs] worker %u: pthread_create failed (%d); running with %u lane(s)\n", i + 1, rc, p.lanes);
            break;
        }
        ++p.lanes;
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
    p.stop.store(true);
    for (uint32_t i = 0; i + 1 < p.lanes; ++i) {
        Worker& w = p.workers[i];
        pthread_mutex_lock(&w.mutex);
        pthread_cond_signal(&w.cond);
        pthread_mutex_unlock(&w.mutex);
        pthread_join(w.thread, nullptr);
    }
    p.started = false;
    p.lanes = 1;
    p.stop.store(false);
}

uint32_t NpJobsLanes(void) { return g_pool.started ? g_pool.lanes : 1; }
int NpInJob(void) { return t_inJob; }

uint32_t NpParallelFor(size_t count, size_t minPerLane, NpRangeFn fn, void* ctx) {
    Pool& p = g_pool;
    if (!p.started) NpJobsInit(0);
    p.calls.fetch_add(1, std::memory_order_relaxed);
    if (count == 0) return 1;
    if (minPerLane == 0) minPerLane = 1;
    uint32_t use = p.lanes;
    const size_t byWork = count / minPerLane;
    if (byWork < use) use = byWork ? static_cast<uint32_t>(byWork) : 1;
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
    const uint64_t forkUs = NowUs();
    ++p.generation;
    p.slot.store(static_cast<uint64_t>(p.generation) << 32);
    for (uint32_t i = 0; i + 1 < use; ++i) {
        Worker& w = p.workers[i];
        w.wakeGen.store(p.generation);
        if (w.sleeping.load()) {
            pthread_mutex_lock(&w.mutex);
            pthread_cond_signal(&w.cond);
            pthread_mutex_unlock(&w.mutex);
        }
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
    const uint64_t spinUntil = t1 + p.spinUs;
    while (p.finished.load() < need) {
        if (p.spinUs == 0 || NowUs() >= spinUntil) {
            pthread_mutex_lock(&p.doneMutex);
            p.callerSleeping.store(true);
            while (p.finished.load() < need) pthread_cond_wait(&p.doneCond, &p.doneMutex);
            p.callerSleeping.store(false);
            pthread_mutex_unlock(&p.doneMutex);
            break;
        }
    }
    p.callerWaitUs += NowUs() - t1;
    p.lateStarts += (use - 1) - need;
    // slowest starter's latency
    uint32_t worst = 0;
    for (uint32_t i = 0; i + 1 < use; ++i) {
        const uint64_t s = p.workers[i].startedUs.load(std::memory_order_relaxed);
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
}

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
    if (s.lateStarts) std::fprintf(stderr, "; %llu worker wake-ups too late to take part", static_cast<unsigned long long>(s.lateStarts));
    if (s.verifyRuns) std::fprintf(stderr, "; verify: %llu runs, %llu mismatches", static_cast<unsigned long long>(s.verifyRuns), static_cast<unsigned long long>(s.verifyMismatches));
    std::fputc('\n', stderr);
}

} // extern "C"
