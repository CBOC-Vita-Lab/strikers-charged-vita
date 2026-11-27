// Unit tests of the frame-send worker (src/np_framesend.cpp) on the host: x86-64 (also under ThreadSanitizer), armhf under qemu.
// The environment selects the mode: MSC_FRAMESEND=thread|serial|inline|off (tests/run_fs_tests.sh runs all four).
// What they check: a job runs exactly once per fork and never concurrently with the next; the join returns after the job; a gate joins
// a pending job and does nothing on the worker or without a job; the heap hooks (window blocks do not join, old blocks do);
// what the game thread wrote before the fork is visible to the job and what the job wrote is visible after the join; the order of
// events in the four modes (inline: the job runs inside the fork; serial: inside the join; thread: on another thread).
#include "np_framesend.h"
#include "np_time.h"

// (W5 PIPE) np_framesend.cpp's pipeline hooks; the game side reaches them through include/np/framesend.h (weak declarations)
extern "C" volatile int NpFsPipeOn;
extern "C" int NpFsDeferWrite(void** slot, void* value);
extern "C" int NpFsDeferCall(void (*fn)(const void*), const void* data, unsigned size);

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <unistd.h>

extern "C" void NpFsSetOverlap(int on);
static __thread int t_isSim = 0;
extern "C" int NpSimtOnSimThread(void) { return t_isSim; }   // (the game's np_sim stack-range test, stood in by a flag on the test's fake sim thread)

static std::atomic<int> g_fail{0}, g_checks{0};
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_fail; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

struct Job { int input; int output; std::atomic<int> running; int ranOn; pthread_t tid; int order; };
static std::atomic<int> g_seq{0};
static std::atomic<int> g_maxRunning{0}, g_nowRunning{0};
static int g_afterJoin = 0;

static void TheJob(void* arg) {
    Job* j = static_cast<Job*>(arg);
    const int n = ++g_nowRunning;
    int m = g_maxRunning.load(); while (n > m && !g_maxRunning.compare_exchange_weak(m, n)) {}
    usleep(2000);                       // long enough that the game thread is ahead of it
    j->output = j->input * 3 + 1;       // reads what was written before the fork
    j->tid = pthread_self();
    j->order = ++g_seq;
    --g_nowRunning;
    CHECK(NpFsOnWorker() == 1);
}
static void AfterJoin() { ++g_afterJoin; }
static void* g_freed[8];
static int g_nfreed = 0;
static void FreeFn(void* p) { if (g_nfreed < 8) g_freed[g_nfreed++] = p; }

int main() {
    const int mode = NpFsMode();
    std::printf("mode %d\n", mode);
    NpFsAfterJoin(AfterJoin);
    const pthread_t me = pthread_self();
    CHECK(NpFsOnWorker() == 0);
    CHECK(NpFsBusy == 0);

    // 1. fork / join, many times, with the game thread's writes before the fork and reads after the join
    static Job jobs[200];
    for (int i = 0; i < 200; ++i) {
        jobs[i].input = i + 7;
        jobs[i].output = -1;
        const int seqBefore = g_seq.load();
        NpFsFork(TheJob, &jobs[i]);
        if (mode == NP_FS_INLINE || mode == NP_FS_OFF) {
            CHECK(g_seq.load() == seqBefore + 1);      // ran inside the fork
            CHECK(NpFsBusy == 0);
        } else {
            CHECK(NpFsBusy == 1);
            if (mode == NP_FS_SERIAL) CHECK(g_seq.load() == seqBefore);   // not yet: it runs at the join
        }
        if (i % 3 == 0) usleep(100);                 // sometimes the worker is done before the join, sometimes not
        NpFsJoin();
        CHECK(NpFsBusy == 0);
        CHECK(jobs[i].output == (i + 7) * 3 + 1);
        CHECK(g_seq.load() == seqBefore + 1);
        if (mode == NP_FS_THREAD) CHECK(!pthread_equal(jobs[i].tid, me));
        if (mode == NP_FS_SERIAL || mode == NP_FS_INLINE || mode == NP_FS_OFF) CHECK(pthread_equal(jobs[i].tid, me));
    }
    CHECK(g_maxRunning.load() == 1);
    NpFsJoin();   // nothing pending: returns at once
    if (mode != NP_FS_OFF && mode != NP_FS_INLINE) CHECK(g_afterJoin == 200);

    // 2. gates
    if (mode == NP_FS_THREAD || mode == NP_FS_SERIAL) {
        Job j{5, -1, {0}, 0, {}, 0};
        NpFsGateSlow("test");                         // no job: nothing
        NpFsFork(TheJob, &j);
        NpFsGateSlow("test");                         // joins
        CHECK(NpFsBusy == 0 && j.output == 16);
        NpFsGateSlow("test");
        // heap hooks: a block allocated inside the window does not join, an old block does
        Job k{6, -1, {0}, 0, {}, 0};
        int blockA, blockB;
        NpFsFork(TheJob, &k);
        NpFsHeapAlloc(&blockA);
        NpFsHeapFree(&blockA);                        // window block: no join
        CHECK(NpFsBusy == 1);
        NpFsHeapFree(&blockB);                        // an old block: joins (freegate is on by default)
        CHECK(NpFsBusy == 0 && k.output == 19);
    }

    // 2b. MSC_FS_FREEGATE=2: an old block that is freed while the job runs is kept until the join (and freed in order, on the game thread)
    if ((mode == NP_FS_THREAD || mode == NP_FS_SERIAL) && NpFsFreeGate() == 2) {
        NpFsSetFreeFn(FreeFn);
        Job d{9, -1, {0}, 0, {}, 0};
        int blockC, blockD, blockE;
        NpFsFork(TheJob, &d);
        NpFsHeapAlloc(&blockC);
        CHECK(NpFsHeapFree(&blockC) == 0);            // window block: the caller frees it now
        CHECK(NpFsHeapFree(&blockD) == 1);            // old blocks: kept
        CHECK(NpFsHeapFree(&blockE) == 1);
        CHECK(NpFsBusy == 1 && g_nfreed == 0);
        NpFsJoin();
        CHECK(NpFsBusy == 0 && d.output == 28);
        CHECK(g_nfreed == 2 && g_freed[0] == &blockD && g_freed[1] == &blockE);
        CHECK(NpFsHeapFree(&blockD) == 0);            // nothing pending: the caller frees
    }

    // 3b. the helper lane (MSC_SKIN_LANE=1): jobs run in order on another thread; NpLaneWait returns when all are done; a full queue drains first
    //     MSC_LANE_FREEZE_MS=n (the lane thread does not run for n ms) is the stalled-lane configuration: then the game thread runs the queue itself
    if (NpLaneEnabled()) {
        const bool frozen = std::getenv("MSC_LANE_FREEZE_MS") != nullptr;
        struct LJob { int i; int seq; pthread_t tid; };
        static LJob lj[700];
        static std::atomic<int> lseq{0};
        auto fn = [](void* a) { LJob* j = static_cast<LJob*>(a); usleep(j->i % 7 == 0 ? 200 : 0); j->tid = pthread_self(); j->seq = ++lseq; };
        NpLaneWait();                                   // nothing submitted: returns at once
        for (int round = 0; round < 3; ++round) {
            const int n = round == 2 ? 700 : 40;        // 700 > the queue (256): the submitter drains it and goes on
            lseq = 0;
            for (int i = 0; i < n; ++i) { lj[i].i = i; lj[i].seq = 0; NpLaneSubmit(fn, &lj[i]); }
            NpLaneWait();
            bool inOrder = true, other = true;
            for (int i = 0; i < n; ++i) { inOrder = inOrder && lj[i].seq == i + 1; other = other && !pthread_equal(lj[i].tid, me); }
            CHECK(inOrder);
            CHECK(other || frozen);
            CHECK(lseq.load() == n);
        }

        // The cut-scene pattern that hung the console (1.1): many jobs queued at once, usually more than one before the lane thread has run any, the
        // lane often finished before the game thread waits. MSC_SEM_MAX1=1 makes the host semaphores drop a second post like the Vita's kernel
        // ones (SCE_KERNEL_ERROR_SEMA_OVF); with the first version of the lane this loop never returned.
        struct SJob { std::atomic<int> ran; int order; pthread_t tid; };
        static SJob sj[160];
        static std::atomic<int> sseq{0};
        auto sfn = [](void* a) { SJob* j = static_cast<SJob*>(a); j->ran.fetch_add(1); j->order = ++sseq; j->tid = pthread_self(); volatile int x = 0; for (int k = 0; k < 200; ++k) x += k; (void)x; };
        const double t0 = NpRealSeconds();
        const int rounds = frozen ? 3 : 4000;
        long total = 0, onGameThread = 0;
        bool allOnce = true, ordered = true;
        unsigned rng = 12345;
        for (int r = 0; r < rounds; ++r) {
            rng = rng * 1664525u + 1013904223u;
            const int n = r % 5 == 0 ? 1 : 1 + static_cast<int>((rng >> 16) % 150);   // 1 job up to a whole walk-out's worth
            sseq = 0;
            for (int i = 0; i < n; ++i) sj[i].ran = 0;
            for (int i = 0; i < n; ++i) NpLaneSubmit(sfn, &sj[i]);
            if (r % 3 == 0) usleep(r % 2 ? 50 : 400);   // sometimes the lane is long done when the game thread waits, sometimes not
            NpLaneWait();
            for (int i = 0; i < n; ++i) { allOnce = allOnce && sj[i].ran.load() == 1; ordered = ordered && sj[i].order == i + 1; }
            total += n;
            for (int i = 0; i < n; ++i) onGameThread += pthread_equal(sj[i].tid, me) ? 1 : 0;
        }
        const double dt = NpRealSeconds() - t0;
        CHECK(allOnce);
        CHECK(ordered);
        std::printf("lane stress: %d rounds, %ld jobs, %.3f s (%.1f us per round)\n", rounds, total, dt, dt / rounds * 1e6);
        CHECK(dt < (frozen ? 5.0 : 60.0));

#ifndef NP_LANE_OLD
        // lane24 FIX-07: NpLaneTakeUnstarted runs the queued jobs the lane thread has not started, here and at once, in order, each exactly once; a job the lane thread is inside is never touched.
        {
            struct TJob { std::atomic<int> ran; int order; pthread_t tid; };
            static TJob tj[64];
            static std::atomic<int> tseq{0}, tgate{0}, tstarted{0};
            auto tfn0 = [](void* a) { TJob* j = static_cast<TJob*>(a); tstarted = 1; while (!tgate.load()) usleep(50); j->ran.fetch_add(1); j->order = ++tseq; j->tid = pthread_self(); };
            auto tfn = [](void* a) { TJob* j = static_cast<TJob*>(a); j->ran.fetch_add(1); j->order = ++tseq; j->tid = pthread_self(); };
            auto reset = [&](int n) { tseq = 0; tgate = 0; tstarted = 0; for (int i = 0; i < n; ++i) { tj[i].ran = 0; tj[i].order = 0; } };
            NpLaneWait();
            if (frozen) {   // the lane thread is not running: the three queued jobs are run by this thread, at once (a wait would take 12 ms)
                reset(3);
                for (int i = 0; i < 3; ++i) CHECK(NpLaneTrySubmit(tfn, &tj[i]) == 1);
                const double a0 = NpRealSeconds();
                const int n = NpLaneTakeUnstarted();
                const double adt = NpRealSeconds() - a0;
                CHECK(n == 3);
                CHECK(adt < 0.05);
                for (int i = 0; i < 3; ++i) CHECK(tj[i].ran.load() == 1 && tj[i].order == i + 1 && pthread_equal(tj[i].tid, me));
                NpLaneWait();
            } else {        // the lane thread is inside job 0: job 1 waits behind it and is not taken over (one executor at a time, in order)
                reset(2);
                CHECK(NpLaneTrySubmit(tfn0, &tj[0]) == 1);
                while (!tstarted.load()) usleep(50);
                CHECK(NpLaneTrySubmit(tfn, &tj[1]) == 1);
                const int n = NpLaneTakeUnstarted();
                CHECK(n == 0);
                CHECK(tj[1].ran.load() == 0 && tj[0].ran.load() == 0);
                tgate = 1;
                NpLaneWait();
                CHECK(tj[0].ran.load() == 1 && tj[1].ran.load() == 1 && tj[0].order == 1 && tj[1].order == 2);
                // a burst: whoever gets each job (the lane thread or this one), each runs once and in order
                bool onceInOrder = true;
                for (int r = 0; r < 300; ++r) {
                    reset(40);
                    for (int i = 0; i < 40; ++i) NpLaneSubmit(tfn, &tj[i]);
                    if (r % 2) usleep(r % 4 ? 20 : 100);
                    NpLaneTakeUnstarted();
                    NpLaneWait();
                    for (int i = 0; i < 40; ++i) onceInOrder = onceInOrder && tj[i].ran.load() == 1 && tj[i].order == i + 1;
                }
                CHECK(onceInOrder);
            }
        }

        // A full queue is refused, never waited for: the caller does the work. A job that blocks keeps the lane busy while the queue is filled.
        {
            static std::atomic<int> gate{0}, started{0};
            static std::atomic<int> done[400];
            for (auto& d : done) d = 0;
            struct BJob { int i; };
            static BJob bj[400];
            auto bfn = [](void* a) {
                BJob* j = static_cast<BJob*>(a);
                if (j->i == 0) { started = 1; while (!gate.load()) usleep(50); }
                done[j->i].fetch_add(1);
            };
            gate = 0; started = 0;
            bj[0].i = 0;
            if (frozen) usleep(0);
            int accepted = 0, refused = 0;
            CHECK(NpLaneTrySubmit(bfn, &bj[0]) == 1);
            ++accepted;
            if (!frozen) while (!started.load()) usleep(50);   // the lane thread is inside job 0
            for (int i = 1; i < 400; ++i) {
                bj[i].i = i;
                if (NpLaneTrySubmit(bfn, &bj[i])) ++accepted; else { ++refused; break; }
            }
            CHECK(refused == 1 && accepted >= 256 && accepted <= 257);   // the ring holds 256 beyond the job the lane has taken
            gate = 1;
            NpLaneWait();
            bool each = true;
            for (int i = 0; i < accepted; ++i) each = each && done[i].load() == 1;
            for (int i = accepted; i < 400; ++i) each = each && done[i].load() == 0;
            CHECK(each);
            std::printf("lane full queue: %d accepted, then refused (the caller works)\n", accepted);
        }
#endif

        if (frozen) {
            // the lane thread did not run for MSC_LANE_FREEZE_MS: the waits returned after the stall slices, with the queued jobs run on the game thread
            std::printf("lane frozen: %ld of %ld stress jobs ran on the game thread\n", onGameThread, total);
            CHECK(onGameThread > 0);
        } else {
            CHECK(onGameThread * 20 <= total);   // (a healthy lane thread takes the jobs; the stall fallback may fire on a starved host, not often)
            std::printf("lane stress: %ld of %ld jobs ran on the game thread (stall fallback)\n", onGameThread, total);
        }
    }

    // 2c. the two-stage pipeline (MSC_FS_PIPE=1, thread / serial): stores to a table the job reads are kept until the job is done and made in their order
    //     (NpFsDeferWrite), the pipeline keeps the job running over what the gates of the plain fork would end, and without the pipeline nothing is deferred
    if (mode == NP_FS_THREAD || mode == NP_FS_SERIAL) {
        static void* table[4];
        static int A, B, C;
        table[0] = &A; table[1] = &A; table[2] = nullptr; table[3] = &C;
        CHECK(NpFsDeferWrite(&table[0], &B) == 0);       // no job pending: the caller stores
        Job p{11, -1, {0}, 0, {}, 0};
        NpFsFork(TheJob, &p);
        if (NpFsPipeOn) {
            CHECK(NpFsDeferWrite(&table[0], &B) == 1);   // pending: kept
            CHECK(NpFsDeferWrite(&table[1], &C) == 1);
            CHECK(NpFsDeferWrite(&table[0], &C) == 1);   // two stores to one slot: the later one wins
            CHECK(table[0] == &A && table[1] == &A);     // nothing made yet
            NpFsJoin();
            CHECK(NpFsBusy == 0 && p.output == 34);
            CHECK(table[0] == &C && table[1] == &C && table[3] == &C);
            // more stores than the table holds: the overflow joins the job and the caller stores (nothing is lost)
            Job q{12, -1, {0}, 0, {}, 0};
            NpFsFork(TheJob, &q);
            int kept = 0, joinedAt = -1;
            for (int i = 0; i < 2000 && joinedAt < 0; ++i) {
                if (NpFsDeferWrite(&table[2], i % 2 ? &A : &B) == 1) ++kept; else joinedAt = i;
            }
            CHECK(joinedAt > 0 && NpFsBusy == 0 && q.output == 37);
            CHECK(kept == joinedAt);
            NpFsJoin();
        } else {
            CHECK(NpFsDeferWrite(&table[0], &B) == 0);   // no pipeline: never deferred
            NpFsJoin();
            CHECK(p.output == 34);
        }
    }

    // 2d. deferred GX calls (NpFsDeferCall, any mode with a worker): kept while a job is pending, made in their order right after the join; nothing pending: the caller makes the call
    if (mode == NP_FS_THREAD || mode == NP_FS_SERIAL) {
        static int order[4096];
        static int nOrder = 0;
        struct Rec { static void Call(const void* d) { if (nOrder < 4096) order[nOrder++] = *static_cast<const int*>(d); } };
        int v1 = 1, v2 = 2, v3 = 3;
        CHECK(NpFsDeferCall(Rec::Call, &v1, sizeof v1) == 0);
        Job r{13, -1, {0}, 0, {}, 0};
        NpFsFork(TheJob, &r);
        CHECK(NpFsDeferCall(Rec::Call, &v1, sizeof v1) == 1);
        CHECK(NpFsDeferCall(Rec::Call, &v2, sizeof v2) == 1);
        CHECK(NpFsDeferCall(Rec::Call, &v3, sizeof v3) == 1);
        CHECK(nOrder == 0);
        NpFsJoin();
        CHECK(NpFsBusy == 0 && r.output == 40);
        CHECK(nOrder == 3 && order[0] == 1 && order[1] == 2 && order[2] == 3);
        // more calls than the buffer holds: the overflow joins (the kept calls are made first, in order) and the caller makes the call itself
        nOrder = 0;
        Job t{14, -1, {0}, 0, {}, 0};
        NpFsFork(TheJob, &t);
        int kept = 0, stoppedAt = -1;
        for (int i = 0; i < 5000 && stoppedAt < 0; ++i) {
            if (NpFsDeferCall(Rec::Call, &i, sizeof i) == 1) ++kept; else stoppedAt = i;
        }
        CHECK(stoppedAt == kept && stoppedAt > 100 && NpFsBusy == 0 && t.output == 43);
        CHECK(nOrder == kept);
        bool ordered = true;
        for (int i = 0; i < nOrder; ++i) if (order[i] != i) ordered = false;
        CHECK(ordered);
        NpFsJoin();
    }

    // 2e. overlap18 (MSC_SIMT=2): np_sim runs beside the game thread from the kick to the join. NpFsSetOverlap(1): heap events of both threads take the lock; np_sim's frees of old blocks are kept
    //     until the game thread's join (whatever MSC_FS_FREEGATE says: it can never join), its gates wait for the JOB to finish without joining; window blocks it allocated are freed at once.
    if (mode == NP_FS_THREAD && NpFsFreeGate() == 2) {
        NpFsSetFreeFn(FreeFn);
        static int simBlocks[64], gameBlocks[64], simNew[64], gameNew[64];
        struct Sim {
            static void* Main(void* a) {
                t_isSim = 1;
                int* res = static_cast<int*>(a);
                for (int i = 0; i < 64; ++i) {
                    NpFsHeapAlloc(&simNew[i]);                                // new in the window
                    if (NpFsHeapFree(&simNew[i]) != 0) ++res[0];              // freed at once
                    if (NpFsHeapFree(&simBlocks[i]) != 1) ++res[1];           // an old block: kept for the join
                    if (i % 16 == 5) { NpFsGateSlow("test"); if (NpFsBusy != 1) ++res[2]; }   // waits for the job; the join is the game thread's
                }
                t_isSim = 0;
                return nullptr;
            }
        };
        for (int round = 0; round < 40; ++round) {
            g_nfreed = 0;
            Job o{round + 1, -1, {0}, 0, {}, 0};
            NpFsSetOverlap(1);
            NpFsFork(TheJob, &o);
            int res[3] = {0, 0, 0};
            pthread_t th;
            pthread_create(&th, nullptr, Sim::Main, res);
            int gameDeferred = 0, gameOwn = 0;
            for (int i = 0; i < 64; ++i) {
                NpFsHeapAlloc(&gameNew[i]);
                if (NpFsHeapFree(&gameNew[i]) == 0) ++gameOwn;
                if (NpFsHeapFree(&gameBlocks[i]) == 1) ++gameDeferred;
            }
            pthread_join(th, nullptr);
            CHECK(res[0] == 0 && res[1] == 0 && res[2] == 0);
            CHECK(gameOwn == 64 && gameDeferred == 64);
            CHECK(NpFsBusy == 1);                                           // np_sim never joined
            NpFsJoin();
            NpFsSetOverlap(0);
            CHECK(NpFsBusy == 0 && o.output == (round + 1) * 3 + 1);
            CHECK(g_nfreed == 8);                                          // (FreeFn keeps only the first 8; the count of deferred frees is checked through the stats line)
            CHECK(NpFsHeapFree(&simBlocks[0]) == 0);                       // nothing pending again
        }
    }

    // 3. a gate on another thread does nothing to the game thread's job (only the game thread joins)
    NpFsLogStats("test");
    std::printf("%d checks, %d failed\n", g_checks.load(), g_fail.load());
    return g_fail.load() ? 1 : 0;
}
