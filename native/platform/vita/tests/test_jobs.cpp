// Unit tests of the job pool (src/np_jobs.cpp) on the host: x86-64, armhf under qemu.
//   exit status 0 = all passed. Run through tests/run_jobs_tests.sh (several environments).
#include "np_jobs.h"
#include "np_time.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <unistd.h>
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

// 7. (nxcpu25, nxr6a) where the lane on the system core sits among the workers a call wakes. Lanes: 1 = core 2 (spins), 2 = core 1, 3 = core 3 (the fake core 3 of the host).
//    MSC_JOBS_C3FIRST unset/1 (default, nxr6a): core 2 -> core 3 -> core 1: a call for 2 lanes uses lane 1 only (no core 3 wake, no LaneGuard exposure), a call for 3 lanes uses lanes 1 and 3 (off core 1).
//    =2 (the 25 Nov front order): a call for 2 lanes goes to lane 3. =0: the old order (lane 3 only for a call with work for every lane).
//    With MSC_VITA_CORE3_SLOW_US=1 and TEST_C3F_RETIRE=1 the lane is given up after a few slow calls and must then never be woken again (3-lane calls: the first that uses it).
static uint32_t Run7(int items, int perLane, int round, std::atomic<int>& slow3, uint32_t* usedOut, bool& ok) {
    std::vector<uint32_t> out(items, 5);
    std::atomic<uint32_t> seen{0};
    struct C7 { std::vector<uint32_t>* out; std::atomic<uint32_t>* seen; std::atomic<int>* slow3; } c{&out, &seen, &slow3};
    (void)round;
    *usedOut = NpParallelFor(out.size(), perLane, [](void* ctx, size_t b, size_t e, uint32_t lane) {
        C7& cc = *static_cast<C7*>(ctx);
        cc.seen->fetch_or(1u << lane);
        for (size_t i = b; i < e; ++i) {
            volatile uint64_t x = 0; for (int k = 0; k < 60000; ++k) x += k;
            if (lane == 3 && cc.slow3->load()) usleep(3000);
            (*cc.out)[i] = static_cast<uint32_t>(i * i + 1);
        }
    }, &c);
    for (size_t i = 0; i < out.size(); ++i) ok = ok && out[i] == static_cast<uint32_t>(i * i + 1);
    return seen.load();
}
static void Section7(uint32_t lanes) {
    const bool core3Lane = std::getenv("MSC_VITA_CORE3_WORK") != nullptr && std::getenv("NP_CORE3_FAKE") != nullptr && lanes >= 3;
    if (core3Lane && lanes >= 4 && !std::getenv("MSC_JOBS_CORES") && !std::getenv("MSC_JOBS_VERIFY")) {   // (the verifier's lane-dependent job keeps the caller waiting: the guard may have retired the lane by now)
        const char* c3f = std::getenv("MSC_JOBS_C3FIRST");
        const int mode = (c3f && !std::strcmp(c3f, "0")) ? 0 : (c3f && !std::strcmp(c3f, "2")) ? 2 : 1;   // 0 off, 1 second place (default), 2 first place
        const bool retire = std::getenv("TEST_C3F_RETIRE") != nullptr;
        std::atomic<int> slow3{retire ? 1 : 0};
        uint32_t e2 = 0, l2 = 0, e3 = 0, l3 = 0, e4 = 0;
        bool ok = true;
        for (int round = 0; round < (retire ? 120 : 40); ++round) {
            uint32_t used = 0;
            const uint32_t m2 = Run7(8, 4, round, slow3, &used, ok);    // work for two lanes
            CHECK(used <= 2);
            CHECK((m2 & (1u << 2)) == 0);   // the core 1 lane has work for nobody here
            if (round < 20) e2 |= m2; else l2 |= m2;
            {
                const uint32_t m3 = Run7(12, 4, round, slow3, &used, ok);   // work for three lanes
                CHECK(used <= 3);
                if (round < 20) e3 |= m3; else l3 |= m3;
                if (mode == 1 && !retire) CHECK((m3 & (1u << 2)) == 0);   // default: a call for 3 lanes is off core 1
                if (!retire) e4 |= Run7(16, 4, round, slow3, &used, ok);        // work for every lane: all four may join
            }
        }
        CHECK(ok);
        std::printf("core3 mode %d retire %d: 2-lane calls early 0x%x late 0x%x; 3-lane early 0x%x late 0x%x; 4-lane 0x%x\n", mode, retire, e2, l2, e3, l3, e4);
        if (mode == 1 && !retire) { CHECK((e2 & (1u << 3)) == 0); CHECK(e2 & (1u << 1)); CHECK(e3 & (1u << 3)); CHECK(e3 & (1u << 1)); CHECK((e3 & (1u << 2)) == 0); }
        if (mode == 2 && !retire) { CHECK(e2 & (1u << 3)); CHECK((e2 & (1u << 1)) == 0); CHECK((l2 & (1u << 1)) == 0); }
        if (mode == 0) { CHECK((e2 & (1u << 3)) == 0); CHECK(e2 & (1u << 1)); CHECK((e3 & (1u << 3)) == 0); }
        if (mode == 0 && !retire) CHECK(e4 & (1u << 2));
        if (mode == 2 && retire) { CHECK(e2 & (1u << 3)); CHECK((l2 & (1u << 3)) == 0); CHECK(l2 & (1u << 1)); }   // given up: the call goes on with the core 2 lane
        if (mode == 1 && retire) { CHECK((e2 & (1u << 3)) == 0); CHECK(e2 & (1u << 1)); CHECK(e3 & (1u << 3)); CHECK((l3 & (1u << 3)) == 0); CHECK(l3 & (1u << 1)); }   // a 2-lane call never reaches the lane (it cannot retire it); the 3-lane calls do, and then go on with core 2 only
    }
}

int main() {
    const uint32_t lanes = NpJobsInit(0);
    std::printf("lanes in use: %u\n", lanes);
    CHECK(lanes >= 1 && lanes <= NP_JOBS_MAX_LANES);
    if (std::getenv("TEST_C3F_RETIRE")) {   // (the given-up test runs alone: a threshold of 1 us would retire the lane in the first sections)
        Section7(lanes);
        NpJobsLogStats("the end of the test");
        NpJobsShutdown();
        std::printf("%d checks, %d failed\n", g_checks, g_fail);
        return g_fail ? 1 : 0;
    }

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

    // 6. (lane24 FIX-20) NpParallelForAvoid: workers on the avoided cores (and the caller's core 0) are not woken; every item still runs once; avoid 0 is NpParallelFor.
    //    Default worker cores are 2, 1, 0 (then 3 for the lane on the system core): lane 1 = core 2, lane 2 = core 1, lane 3 = core 0 or, with the core 3 lane, core 3.
    const bool core3Lane = std::getenv("MSC_VITA_CORE3_WORK") != nullptr && std::getenv("NP_CORE3_FAKE") != nullptr && lanes >= 3;   // (the last worker is the lane on the system core)
    const bool core1Lane = core3Lane ? lanes >= 4 : lanes >= 3;                                                                      // (a worker on core 1 exists: lane 2)
    if (core1Lane && !std::getenv("MSC_JOBS_CORES") && !(std::getenv("MSC_JOBS_ORDER") && !std::strcmp(std::getenv("MSC_JOBS_ORDER"), "interleave"))) {
        std::atomic<uint32_t> seenAvoid{0}, seenPlain{0};
        bool ok = true;
        for (int round = 0; round < 40; ++round) {
            std::vector<uint32_t> out(150, 5);
            SquareCtx c{&out, &seenAvoid};
            struct Slow { SquareCtx* c; };
            const uint32_t used = NpParallelForAvoid(out.size(), 3, [](void* ctx, size_t b, size_t e, uint32_t lane) {
                SquareCtx& cc = *static_cast<SquareCtx*>(ctx);
                for (size_t i = b; i < e; ++i) { volatile uint64_t x = 0; for (int k = 0; k < 20000; ++k) x += k; (*cc.out)[i] = static_cast<uint32_t>(i * i + 1); }
                if (cc.lanesSeen) cc.lanesSeen->fetch_or(1u << lane);
            }, &c, 1u << 1);
            for (size_t i = 0; i < out.size(); ++i) ok = ok && out[i] == static_cast<uint32_t>(i * i + 1);
            CHECK(used >= 1 && used <= lanes - 1);   // (the core 1 worker is never used: one lane fewer than the pool has)
            std::vector<uint32_t> out2(150, 5);
            SquareCtx c2{&out2, &seenPlain};
            NpParallelForAvoid(out2.size(), 3, SquareJob, &c2, 0);
            for (size_t i = 0; i < out2.size(); ++i) ok = ok && out2[i] == static_cast<uint32_t>(i * i + 1);
        }
        CHECK(ok);
        const uint32_t m = seenAvoid.load();
        std::printf("avoid core 1: lanes that took part 0x%x\n", m);
        CHECK((m & (1u << 2)) == 0);                      // lane 2 is the core 1 worker
        if (!core3Lane && lanes >= 4) CHECK((m & (1u << 3)) == 0);   // lane 3 is the core 0 worker (the caller's own core)
        CHECK(m & 1u);
        std::vector<uint32_t> out3(60, 1);
        SquareCtx c3{&out3, nullptr};
        const uint32_t usedAll = NpParallelForAvoid(out3.size(), 3, SquareJob, &c3, 0xF);   // every worker core avoided: serial
        CHECK(usedAll == 1);
        bool ok3 = true;
        for (size_t i = 0; i < out3.size(); ++i) ok3 = ok3 && out3[i] == static_cast<uint32_t>(i * i + 1);
        CHECK(ok3);
    }

    Section7(lanes);

    NpJobsLogStats("the end of the test");
    NpJobsShutdown();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
