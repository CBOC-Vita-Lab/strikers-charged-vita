// Unit tests of the job pool (src/np_jobs.cpp) on the host: x86-64, armhf under qemu.
//   exit status 0 = all passed. Run through tests/run_jobs_tests.sh (several environments).
#include "np_jobs.h"
#include "np_time.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <vector>

static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_fail; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

// A job that squares each item into its own output slot (order independent, idempotent).
struct SquareCtx { std::vector<uint32_t>* out; std::atomic<uint32_t>* lanesSeen; };
static void SquareJob(void* ctx, size_t b, size_t e, uint32_t lane) {
    SquareCtx& c = *static_cast<SquareCtx*>(ctx);
    for (size_t i = b; i < e; ++i) (*c.out)[i] = static_cast<uint32_t>(i * i + 1);
    if (c.lanesSeen) c.lanesSeen->fetch_or(1u << lane);
}
static uint64_t HashVec(void* ctx) {
    const std::vector<uint32_t>& v = *static_cast<std::vector<uint32_t>*>(ctx);
    uint64_t h = 1469598103934665603ull;
    for (uint32_t x : v) h = (h ^ x) * 1099511628211ull;
    return h;
}
// A deliberately lane-dependent job: its output differs between a parallel and a serial run
// whenever another lane takes part (what a race or a hidden dependency looks like to the verifier).
static void LaneDependentJob(void* ctx, size_t b, size_t e, uint32_t lane) {
    SquareCtx& c = *static_cast<SquareCtx*>(ctx);
    volatile uint64_t x = 0;
    for (size_t i = b; i < e; ++i) {
        for (int k = 0; k < 200000; ++k) x += k;   // long enough that the workers arrive before the job is over
        (*c.out)[i] = static_cast<uint32_t>(lane * 1000u + i);
    }
}

struct NestCtx { std::vector<uint32_t>* out; uint32_t innerLanes; };
static void NestedJob(void* ctx, size_t b, size_t e, uint32_t) {
    NestCtx& c = *static_cast<NestCtx*>(ctx);
    for (size_t i = b; i < e; ++i) {
        SquareCtx inner{c.out, nullptr};
        // a call from inside a job must run serially on this lane
        c.innerLanes = NpParallelFor(1, 1, SquareJob, &inner);   // (count 1: still exercises the re-entrancy path via busy)
        (void)inner;
    }
}
static void* OtherThread(void* arg) {
    // concurrent producer: must get the busy fallback or run to completion, never corrupt
    std::vector<uint32_t> out(1000);
    SquareCtx c{&out, nullptr};
    for (int i = 0; i < 200; ++i) NpParallelFor(out.size(), 10, SquareJob, &c);
    bool ok = true;
    for (size_t i = 0; i < out.size(); ++i) ok = ok && out[i] == static_cast<uint32_t>(i * i + 1);
    *static_cast<bool*>(arg) = ok;
    return nullptr;
}

int main() {
    const uint32_t lanes = NpJobsInit(0);
    std::printf("lanes in use: %u\n", lanes);
    CHECK(lanes >= 1 && lanes <= NP_JOBS_MAX_LANES);

    // 1. every item exactly once, for many sizes and minimum shares
    for (size_t n : {size_t(0), size_t(1), size_t(2), size_t(3), size_t(7), size_t(10), size_t(63), size_t(64), size_t(65), size_t(150), size_t(1000), size_t(4096)}) {
        for (size_t minPer : {size_t(1), size_t(2), size_t(5), size_t(64)}) {
            std::vector<uint32_t> out(n, 0xDEADBEEFu);
            std::atomic<uint32_t> seen{0};
            SquareCtx c{&out, &seen};
            const uint32_t used = NpParallelFor(n, minPer, SquareJob, &c);
            bool ok = true;
            for (size_t i = 0; i < n; ++i) ok = ok && out[i] == static_cast<uint32_t>(i * i + 1);
            CHECK(ok);
            CHECK(used >= 1 && used <= lanes);
            if (n / minPer < 2 && n) CHECK(used == 1);
        }
    }

    // 2. several lanes really take part in a big job (when the pool has workers)
    if (lanes > 1) {
        // a job long enough that the workers certainly arrive before it is over
        struct Slow { std::atomic<uint32_t>* seen; };
        std::atomic<uint32_t> seen{0};
        Slow slow{&seen};
        NpParallelFor(64, 1, [](void* ctx, size_t b, size_t e, uint32_t lane) {
            Slow& s = *static_cast<Slow*>(ctx);
            s.seen->fetch_or(1u << lane);
            volatile uint64_t x = 0;
            for (size_t i = b; i < e; ++i) for (int k = 0; k < 300000; ++k) x += k;
        }, &slow);
        const uint32_t mask = seen.load();
        std::printf("lanes that took part in the slow job: 0x%x\n", mask);
        CHECK(mask & 1u);
        CHECK(__builtin_popcount(mask) >= 2);
    }

    // 3. re-entrancy and a concurrent producer
    {
        std::vector<uint32_t> out(8);
        NestCtx nc{&out, 99};
        NpParallelFor(8, 1, NestedJob, &nc);
        CHECK(nc.innerLanes == 1);   // the nested call ran serially
        bool ok = false;
        pthread_t t;
        pthread_create(&t, nullptr, OtherThread, &ok);
        std::vector<uint32_t> out2(1000);
        SquareCtx c{&out2, nullptr};
        for (int i = 0; i < 200; ++i) NpParallelFor(out2.size(), 10, SquareJob, &c);
        pthread_join(t, nullptr);
        CHECK(ok);
        bool ok2 = true;
        for (size_t i = 0; i < out2.size(); ++i) ok2 = ok2 && out2[i] == static_cast<uint32_t>(i * i + 1);
        CHECK(ok2);
    }

    // 4. stress: many small jobs of random sizes
    {
        uint32_t s = 12345;
        bool ok = true;
        for (int round = 0; round < 20000; ++round) {
            s = s * 1664525u + 1013904223u;
            const size_t n = 1 + (s >> 16) % 150;
            std::vector<uint32_t> out(n, 7);
            SquareCtx c{&out, nullptr};
            NpParallelFor(n, 1 + (s >> 8) % 4, SquareJob, &c);
            for (size_t i = 0; i < n; ++i) if (out[i] != static_cast<uint32_t>(i * i + 1)) { ok = false; break; }
            if (!ok) break;
        }
        CHECK(ok);
    }

    // 5. the verifier: a clean job passes, a lane-dependent one is caught (only meaningful with workers and MSC_JOBS_VERIFY=1)
    {
        NpJobsStats before, after;
        NpJobsGetStats(&before);
        std::vector<uint32_t> out(256);
        SquareCtx c{&out, nullptr};
        // long enough for the workers to take part in the job
        for (int i = 0; i < 50; ++i) NpParallelForChecked("clean", out.size(), 8, SquareJob, &c, HashVec, &out);
        NpJobsGetStats(&after);
        CHECK(after.verifyMismatches == before.verifyMismatches);
        if (std::getenv("MSC_JOBS_VERIFY") && lanes > 1) {
            CHECK(after.verifyRuns > before.verifyRuns);
            std::vector<uint32_t> small(24);
            SquareCtx c2{&small, nullptr};
            for (int i = 0; i < 50; ++i) NpParallelForChecked("lane-dependent", small.size(), 1, LaneDependentJob, &c2, HashVec, &small);
            NpJobsGetStats(&after);
            std::printf("verifier: %llu runs, %llu mismatches\n", (unsigned long long)after.verifyRuns, (unsigned long long)after.verifyMismatches);
            CHECK(after.verifyMismatches > before.verifyMismatches);
        }
    }

    NpJobsLogStats("the end of the test");
    NpJobsShutdown();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
