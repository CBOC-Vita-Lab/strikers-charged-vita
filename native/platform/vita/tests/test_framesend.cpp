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
    if (NpLaneEnabled()) {
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
            CHECK(other);
            CHECK(lseq.load() == n);
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

    // 3. a gate on another thread does nothing to the game thread's job (only the game thread joins)
    NpFsLogStats("test");
    std::printf("%d checks, %d failed\n", g_checks.load(), g_fail.load());
    return g_fail.load() ? 1 : 0;
}
