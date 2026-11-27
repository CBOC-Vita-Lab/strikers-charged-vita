// Host test of np_vita_envlock.cpp (__env_lock / __env_unlock): exclusion, recursion, a non-owner's unlock, and a model of newlib's environment array (a writer that reallocates it for
// every new name, readers that walk it) which corrupts without the lock. run_envlock_test.sh also builds a mutant (lock does nothing) that this test must fail.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unistd.h>
#include <vector>

struct _reent;
extern "C" void __env_lock(struct _reent*);
extern "C" void __env_unlock(struct _reent*);
extern "C" void np_env_lock_stats(unsigned*, unsigned*, unsigned*);

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

// newlib's _setenv_r: a NEW name allocates a bigger array, copies, frees the old one (poisoned here: the new one often lands on it)
static char** g_env = nullptr;
static size_t g_n = 0;
static std::atomic<int> g_bad{0};

static void Set(const char* s) {
    __env_lock(nullptr);
    char** n = static_cast<char**>(std::malloc((g_n + 2) * sizeof(char*)));
    for (size_t i = 0; i < g_n; ++i) n[i] = g_env[i];
    n[g_n] = const_cast<char*>(s);
    n[g_n + 1] = nullptr;
    char** old = g_env;
    g_env = n;
    ++g_n;
    usleep(50);                              // the window a reader can fall into
    if (old) { std::memset(old, 0xDD, g_n * sizeof(char*)); std::free(old); }
    __env_unlock(nullptr);
}
static void Unset() {
    __env_lock(nullptr);
    if (g_n) { g_env[--g_n] = nullptr; }
    __env_unlock(nullptr);
}
static int Get(const char* name) {
    __env_lock(nullptr);
    __env_lock(nullptr);                     // _findenv_r under getenv's lock: recursion
    int found = 0;
    char** e = g_env;
    for (size_t i = 0; e && i < g_n; ++i) {
        if (reinterpret_cast<uintptr_t>(e[i]) == 0xDDDDDDDDu || reinterpret_cast<uintptr_t>(e[i]) == 0xDDDDDDDDDDDDDDDDull) { g_bad.fetch_add(1); break; }   // a freed array
        if (e[i] && std::strncmp(e[i], name, std::strlen(name)) == 0) found = 1;
    }
    usleep(20);
    if (e != g_env) g_bad.fetch_add(1);      // the array changed while this thread walked it
    __env_unlock(nullptr);
    __env_unlock(nullptr);
    return found;
}

int main() {
    // 1. exclusion
    {
        std::atomic<int> inside{0}, overlap{0};
        std::vector<std::thread> t;
        for (int k = 0; k < 6; ++k)
            t.emplace_back([&] {
                for (int i = 0; i < 60; ++i) {
                    __env_lock(nullptr);
                    if (inside.fetch_add(1) != 0) overlap.fetch_add(1);
                    usleep(100);
                    inside.fetch_sub(1);
                    __env_unlock(nullptr);
                }
            });
        for (auto& x : t) x.join();
        CHECK(overlap.load() == 0);
    }
    // 2. recursion: depth 3 held, another thread cannot enter until the third unlock
    {
        std::atomic<int> entered{0};
        __env_lock(nullptr); __env_lock(nullptr); __env_lock(nullptr);
        std::thread o([&] { __env_lock(nullptr); entered = 1; __env_unlock(nullptr); });
        usleep(20000);
        CHECK(entered.load() == 0);
        __env_unlock(nullptr); usleep(5000); CHECK(entered.load() == 0);
        __env_unlock(nullptr); usleep(5000); CHECK(entered.load() == 0);
        __env_unlock(nullptr);
        o.join();
        CHECK(entered.load() == 1);
    }
    // 3. a thread that does not own the lock cannot release it
    {
        std::atomic<int> entered{0};
        __env_lock(nullptr);
        std::thread o([&] { __env_unlock(nullptr); });
        o.join();
        std::thread w([&] { __env_lock(nullptr); entered = 1; __env_unlock(nullptr); });
        usleep(20000);
        CHECK(entered.load() == 0);
        __env_unlock(nullptr);
        w.join();
    }
    // 4. the model of the environment array: one thread sets and unsets 400 names while readers walk
    {
        static char names[400][16];
        for (int i = 0; i < 400; ++i) std::snprintf(names[i], sizeof names[i], "SCR%03d=1", i);
        std::atomic<bool> stop{false};
        std::vector<std::thread> r;
        for (int k = 0; k < 3; ++k) r.emplace_back([&] { while (!stop.load()) { Get("SCR"); usleep(200); } });   // (a reader that is not in the lock all the time, as in the game)
        for (int i = 0; i < 400; ++i) Set(names[i]);
        for (int i = 0; i < 400; ++i) Unset();
        stop = true;
        for (auto& x : r) x.join();
        CHECK(g_bad.load() == 0);
    }
    unsigned calls = 0, waits = 0, to = 0;
    np_env_lock_stats(&calls, &waits, &to);
    CHECK(calls > 1000 && to == 0);
    std::printf("envlock test: %d failures (calls %u, waits %u, timeouts %u)\n", g_fail, calls, waits, to);
    return g_fail ? 1 : 0;
}
