// test_simwake.cpp (simfix20): stress of np_sim's kick / sleep / join hand-shake (include/np/simwake17.h, the code NpSimRun17.cpp runs), spinUs = 0 so
// np_sim always takes the sleep path. A fake np_sim thread does a tiny "step" per kick; the game thread kicks and joins N times and times every join.
// PASS: no join waited longer than the limit (default 50 ms). A join that waits STALL_MS (default 500) is a lost wake-up: reported and the test fails at once.
//   test_simwake [kicks=300000] [limit_ms=50]
// Mutants (compiled in by the Makefile targets simwakemut / simwakemut2): old ordering with a delay between the load of simSleeping and the store of kickGen.
//   -DNP_SIMWAKE_MUTANT_REORDER=40 -DNP_SIMWAKE_MUTANT_NORESIGNAL=1   the old code: must STALL (exit 1)
//   -DNP_SIMWAKE_MUTANT_REORDER=40                                    the reorder with the join's re-signal: must pass, the re-signal counter > 0
#include "np/simwake17.h"
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static NpSimWake W;
static std::atomic<unsigned long long> gSteps{0};
static std::atomic<int> gQuit{0};

static double Now()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void* SimThread(void*)
{
    uint32_t seen = 0;
    for (;;)
    {
        // spinUs = 0: no spin, straight to the sleep (the real loop: `if (g.spinUs == 0 || ...) NpSimWakeSleep`)
        while (W.kickGen.load(std::memory_order_acquire) == seen)
        {
            NpSimWakeSleep(W, seen);
            if (gQuit.load()) return nullptr;
            break;
        }
        seen = W.kickGen.load(std::memory_order_acquire);
        gSteps.fetch_add(1, std::memory_order_relaxed);   // the step
        W.doneGen.store(seen, std::memory_order_release);
    }
}

int main(int argc, char** argv)
{
    const unsigned long long n = argc > 1 ? strtoull(argv[1], 0, 10) : 300000ull;
    const double limit = (argc > 2 ? atof(argv[2]) : 50.0) * 1e-3;
    const double stall = (getenv("STALL_MS") ? atof(getenv("STALL_MS")) : 500.0) * 1e-3;
    pthread_t th;
    pthread_create(&th, nullptr, SimThread, nullptr);
    double maxJoin = 0, sum = 0;
    unsigned long long over = 0, slow1ms = 0;
    for (unsigned long long i = 1; i <= n; ++i)
    {
        const uint32_t gen = W.kickGen.load(std::memory_order_relaxed) + 1;
        const double t0 = Now();
        NpSimWakeKick(W, gen);
        const double spinEnd = t0 + 20e-6;
        while (W.doneGen.load(std::memory_order_acquire) != gen)
        {
            const double t = Now();
            if (t >= spinEnd)
            {
                NpSimWakeResignal(W, gen);
                sched_yield();
            }
            if (t - t0 >= stall)
            {
                printf("STALL: kick %llu: the join waited %.0f ms (np_sim asleep = %d): lost wake-up\n", i, (t - t0) * 1e3, W.simSleeping.load());
                return 1;
            }
        }
        const double w = Now() - t0;
        sum += w;
        if (w > maxJoin) maxJoin = w;
        if (w > limit) ++over;
        if (w > 1e-3) ++slow1ms;
    }
    // stop np_sim: it is asleep or about to be
    gQuit.store(1);
    for (int k = 0; k < 1000; ++k)
    {
        NpSimWakeSignal(W);
        usleep(100);
        if (k > 5) break;
    }
    printf("%llu kicks, %llu steps, join avg %.1f us, max %.2f ms, joins > 1 ms: %llu, joins > %.0f ms: %llu, re-signals by the join: %llu\n", n, gSteps.load(), sum / n * 1e6, maxJoin * 1e3, slow1ms, limit * 1e3, over,
           W.resignals.load());
    if (gSteps.load() != n || over)
    {
        printf("FAIL\n");
        return 1;
    }
    printf("PASS\n");
    return 0;
}
