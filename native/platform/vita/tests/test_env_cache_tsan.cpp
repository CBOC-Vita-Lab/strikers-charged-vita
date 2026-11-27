// nxr6a (NEON r6 #5): ThreadSanitizer litmus for np_env_cache.h. The writer makes plain (non-atomic) writes -- the stand-in for LoadEnvFile/ApplyPlan's setenv --
// then Settle(); a reader that sees Active() (or a new epoch through Get) must see them. On x86 a relaxed gate load passes any value test; ThreadSanitizer
// models happens-before, so a gate load that is not acquire is reported as a data race and this program exits non-zero. Built only by run_env_cache_test.sh.
#include "np_env_cache.h"
#include <cstdio>
#include <thread>
static int g_plain1, g_plain2;   // written before Settle, never again
static NpEnvCache::Slot slot;
int main() {
    int bad = 0;
    std::thread a([&] { while (!NpEnvCache::Active()) std::this_thread::yield(); if (g_plain1 != 42) ++bad; });
    std::thread b([&] {
        while (!NpEnvCache::Active()) std::this_thread::yield();
        // the epoch gate: a Get after Settle answers from the environment the writer finished; the read lambda reads the plain value
        const int v = NpEnvCache::Get(slot, [] { return g_plain2; });
        if (v != 7) ++bad;
    });
    g_plain1 = 42; g_plain2 = 7;
    NpEnvCache::Settle(nullptr);
    a.join(); b.join();
    std::printf("env cache tsan litmus: %s\n", bad ? "FAILED" : "ok");
    return bad ? 1 : 0;
}
