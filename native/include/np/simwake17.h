// np/simwake17.h (simfix20): the kick / sleep / join hand-shake of np_sim (src/Game/NpSimRun17.cpp), header-only so that the unit test
// (platform/vita/tests/test_simwake.cpp) runs the very same code.
//
// np_sim, idle:   lock; simSleeping = 1; while (kickGen == seen) cond_wait; simSleeping = 0; unlock.
// game thread:    kickGen = gen; if (simSleeping) { lock; signal; unlock }.
// This is a store-then-load-the-other-flag (Dekker) pattern: each side stores its own flag, then reads the other's. It is only correct when both stores are
// sequentially consistent (a release store has no barrier after it: on ARMv7 `dmb; str`, on x86 a plain mov, so the later load can pass it and both sides
// read the old value: np_sim sleeps for ever, the join waits for ever: the lost wake-up of fable-verify10 P1). So: seq_cst on the three accesses, and the
// join loop re-signals (under the mutex) while np_sim sleeps and the job is not done, so a lost wake-up could only cost one poll period.
//
// Test mutants (compile-time, never set in a build of the game):
//   NP_SIMWAKE_MUTANT_REORDER=<us>  the kick reads simSleeping BEFORE it stores kickGen and waits <us> in between: what the CPU does when the load passes the
//                                   store (the old release store), made deterministic.
//   NP_SIMWAKE_MUTANT_NORESIGNAL=1  the join never re-signals (the old join).
#ifndef NP_SIMWAKE17_H
#define NP_SIMWAKE17_H
#include <pthread.h>
#include <stdint.h>
#include <atomic>
#ifdef NP_SIMWAKE_MUTANT_REORDER
#include <time.h>
#endif

struct NpSimWake
{
    std::atomic<uint32_t> kickGen{0}, doneGen{0};
    std::atomic<int> simSleeping{0};
    std::atomic<unsigned long long> resignals{0};   // re-signals sent by the join (a lost wake-up that the join repaired shows here)
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
};

inline void NpSimWakeSignal(NpSimWake& w)
{
    pthread_mutex_lock(&w.mutex);
    pthread_cond_signal(&w.cond);
    pthread_mutex_unlock(&w.mutex);
}

// np_sim: sleep until kickGen moves away from `seen`.
inline void NpSimWakeSleep(NpSimWake& w, uint32_t seen)
{
    pthread_mutex_lock(&w.mutex);
    w.simSleeping.store(1, std::memory_order_seq_cst);
    while (w.kickGen.load(std::memory_order_seq_cst) == seen) pthread_cond_wait(&w.cond, &w.mutex);
    w.simSleeping.store(0, std::memory_order_seq_cst);
    pthread_mutex_unlock(&w.mutex);
}

// game thread: publish the kick (everything the step reads was written before this call) and wake np_sim when it sleeps.
inline void NpSimWakeKick(NpSimWake& w, uint32_t gen)
{
#ifdef NP_SIMWAKE_MUTANT_REORDER
    const int sleeping = w.simSleeping.load(std::memory_order_seq_cst);   // the load that passed the store
    struct timespec ts = {0, (long)(NP_SIMWAKE_MUTANT_REORDER) * 1000L};
    nanosleep(&ts, nullptr);
    w.kickGen.store(gen, std::memory_order_release);
    if (sleeping) NpSimWakeSignal(w);
#else
    w.kickGen.store(gen, std::memory_order_seq_cst);
    if (w.simSleeping.load(std::memory_order_seq_cst)) NpSimWakeSignal(w);
#endif
}

// join loop, once per poll after the spin phase: np_sim sleeps and the kick is not done: the wake-up was lost (or is about to arrive): signal again.
inline void NpSimWakeResignal(NpSimWake& w, uint32_t gen)
{
#ifndef NP_SIMWAKE_MUTANT_NORESIGNAL
    if (w.doneGen.load(std::memory_order_acquire) != gen && w.simSleeping.load(std::memory_order_seq_cst))
    {
        w.resignals.fetch_add(1, std::memory_order_relaxed);
        NpSimWakeSignal(w);
    }
#else
    (void)w;
    (void)gen;
#endif
}
#endif
