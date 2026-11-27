// nxboot25 R2-12: host test of np_env_cache.h (the kept answers of the fourth-core readers). Built against the real header and, by run_env_cache_test.sh, against mutants (each must FAIL).
#include "np_env_cache.h"
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
static int failures = 0;
#define CHECK(c) do { if (!(c)) { ++failures; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
static unsigned g_envReads = 0;
static int ReadKey() { ++g_envReads; const char* e = std::getenv("NPT_KEY"); return e ? std::atoi(e) : -7; }
static NpEnvCache::Slot slotA, slotB;
int main() {
    // 1. before the first Settle: every call reads (the old behaviour, so the early readers of c3def18 never freeze "off")
    unsetenv("NPT_KEY");
    g_envReads = 0;
    CHECK(NpEnvCache::Get(slotA, ReadKey) == -7);
    setenv("NPT_KEY", "3", 1);
    CHECK(NpEnvCache::Get(slotA, ReadKey) == 3);
    CHECK(NpEnvCache::Get(slotA, ReadKey) == 3);
    CHECK(g_envReads == 3);
    // 2. after Settle: one read, then kept (also a 0 and a negative value)
    NpEnvCache::Settle(nullptr);
    g_envReads = 0;
    for (int i = 0; i < 1000; ++i) CHECK(NpEnvCache::Get(slotA, ReadKey) == 3);
    CHECK(g_envReads == 1);
    setenv("NPT_KEY", "0", 1);
    NpEnvCache::Unsettle(); NpEnvCache::Settle(nullptr);
    g_envReads = 0;
    for (int i = 0; i < 100; ++i) CHECK(NpEnvCache::Get(slotA, ReadKey) == 0);
    CHECK(g_envReads == 1);
    setenv("NPT_KEY", "-5", 1);
    NpEnvCache::Unsettle(); NpEnvCache::Settle(nullptr);
    g_envReads = 0;
    for (int i = 0; i < 100; ++i) CHECK(NpEnvCache::Get(slotA, ReadKey) == -5);
    CHECK(g_envReads == 1);
    // 3. a re-read of the settings file: Unsettle answers with fresh reads (the plan is being rewritten), Settle again keeps the new value; slots are independent
    setenv("NPT_KEY", "9", 1);
    NpEnvCache::Unsettle();
    CHECK(NpEnvCache::Get(slotA, ReadKey) == 9);
    setenv("NPT_KEY", "11", 1);
    CHECK(NpEnvCache::Get(slotA, ReadKey) == 11);   // (unsettled: not kept)
    NpEnvCache::Settle(nullptr);
    CHECK(NpEnvCache::Get(slotB, ReadKey) == 11 && NpEnvCache::Get(slotA, ReadKey) == 11);
    setenv("NPT_KEY", "12", 1);
    CHECK(NpEnvCache::Get(slotA, ReadKey) == 11);   // (kept until the next LoadEnvFile: documented: nothing writes the environment in play)
    NpEnvCache::Unsettle(); NpEnvCache::Settle(nullptr);
    CHECK(NpEnvCache::Get(slotA, ReadKey) == 12 && NpEnvCache::Get(slotB, ReadKey) == 12);
    // 4. MSC_ENV_CACHE=0: every call reads
    NpEnvCache::Unsettle(); NpEnvCache::Settle("0");
    g_envReads = 0;
    for (int i = 0; i < 10; ++i) CHECK(NpEnvCache::Get(slotA, ReadKey) == 12);
    CHECK(g_envReads == 10);
    NpEnvCache::Unsettle(); NpEnvCache::Settle("1");
    g_envReads = 0;
    for (int i = 0; i < 10; ++i) CHECK(NpEnvCache::Get(slotA, ReadKey) == 12);
    CHECK(g_envReads == 1);
    // 5. threads: readers hammer Get while the writer re-reads (Unsettle, set, Settle) 2000 times; after the last Settle every reader sees the last value at once
    //    (the writer's value is also the only one a reader may see while settled: v is written before Settle, a reader never sees a value of another round)
    std::atomic<bool> stop{false};
    std::atomic<int> last{0};
    std::atomic<unsigned long> bad{0};
    std::vector<std::thread> rd;
    for (int t = 0; t < 3; ++t) rd.emplace_back([&] {
        while (!stop.load()) {
            const int v = NpEnvCache::Get(slotA, [] { const char* e = std::getenv("NPT_KEY"); return e ? std::atoi(e) : -7; });
            if (v < 0 || v > 2000) bad.fetch_add(1);
        }
    });
    for (int i = 1; i <= 2000; ++i) {
        NpEnvCache::Unsettle();
        char b[16]; std::snprintf(b, sizeof b, "%d", i);
        setenv("NPT_KEY", b, 1);   // (the test's own reader reads getenv without the console's lock: no concurrent setenv of the same block is made worse by it: single writer, readers only read the value pointer)
        NpEnvCache::Settle(nullptr);
        last = i;
    }
    // (give the readers a moment so that they run on the final epoch, then ask from this thread and from a new one)
    for (int i = 0; i < 200000; ++i) NpEnvCache::Get(slotB, [] { return 0; });
    stop = true;
    for (auto& t : rd) t.join();
    CHECK(bad.load() == 0);
    int seen = -1;
    std::thread([&] { seen = NpEnvCache::Get(slotA, ReadKey); }).join();
    CHECK(seen == 2000 && NpEnvCache::Get(slotA, ReadKey) == 2000);
    std::printf("env cache test: %s (reads %u, hits %u)\n", failures ? "FAILED" : "ok", NpEnvCache::g_reads.load(), NpEnvCache::g_hits.load());
    return failures ? 1 : 0;
}
