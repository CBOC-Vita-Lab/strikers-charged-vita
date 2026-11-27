// np_stdio_lock.cpp on the host. Two builds of this file (run_stdio_lock_test.sh):
//   -DFAKE   the lock's calls into libc are replaced (-D__real_fopen=fake_fopen ...) by fakes that fail the test if two run at once and that hold the "slot table"
//            for a moment, as the console's unlocked search does: 16 threads open and close files through __wrap_fopen / __wrap_fclose.
//   (real)   linked with -Wl,--wrap=fopen,freopen,fdopen,tmpfile,fclose as the Vita build is: plain fopen/fclose calls from 16 threads go through the lock (the counters
//            prove it) and every file reads back its own contents.
//   MSC_BOOTIO_INJECT=n / MSC_BOOTIO_MS=ms (a mode argument selects the check): the retry of np_boot_fopen.
#include "np_boot_io.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

extern "C" {
FILE* __wrap_fopen(const char*, const char*);
int __wrap_fclose(FILE*);
#ifdef FAKE
std::atomic<int> g_inside{0};
std::atomic<int> g_overlap{0};
std::atomic<long> g_fake_calls{0};
static void Enter() {
    if (g_inside.fetch_add(1) != 0) g_overlap.fetch_add(1);
    g_fake_calls.fetch_add(1);
    usleep(300);
    g_inside.fetch_sub(1);
}
FILE* fake_fopen(const char* p, const char* m) { Enter(); return std::fopen(p, m); }
FILE* fake_freopen(const char*, const char*, FILE*) { Enter(); return nullptr; }
FILE* fake_fdopen(int, const char*) { Enter(); return nullptr; }
FILE* fake_tmpfile(void) { Enter(); return nullptr; }
int fake_fclose(FILE* f) { Enter(); return std::fclose(f); }
#endif
}

static std::string g_dir;

static void Hammer(int id, int rounds, std::atomic<int>* bad) {
    const std::string path = g_dir + "/f" + std::to_string(id);
    for (int i = 0; i < rounds; ++i) {
        FILE* f =
#ifdef FAKE
            __wrap_fopen(path.c_str(), "rb");
#else
            fopen(path.c_str(), "rb");
#endif
        if (!f) { bad->fetch_add(1); continue; }
        char buf[32] = {};
        const size_t n = std::fread(buf, 1, sizeof buf - 1, f);
        if (n == 0 || std::atoi(buf) != id) bad->fetch_add(1);
#ifdef FAKE
        __wrap_fclose(f);
#else
        fclose(f);
#endif
    }
}

static double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

int main(int argc, char** argv) {
    char tmpl[] = "/tmp/np_stdio_lock_XXXXXX";
    CHECK(mkdtemp(tmpl));
    g_dir = tmpl;
    const char* mode = argc > 1 ? argv[1] : "threads";
    for (int i = 0; i < 16; ++i) {
        FILE* f = std::fopen((g_dir + "/f" + std::to_string(i)).c_str(), "wb");
        CHECK(f);
        std::fprintf(f, "%d\n", i);
        std::fclose(f);
    }
    if (!std::strcmp(mode, "threads")) {
        std::atomic<int> bad{0};
        std::vector<std::thread> t;
        for (int i = 0; i < 16; ++i) t.emplace_back(Hammer, i, 300, &bad);
        for (auto& x : t) x.join();
        unsigned calls = 0, waits = 0, timeouts = 0;
        np_stdio_lock_stats(&calls, &waits, &timeouts);
        std::printf("threads: calls %u waits %u timeouts %u bad %d\n", calls, waits, timeouts, bad.load());
        CHECK(bad.load() == 0);
        CHECK(timeouts == 0);
        CHECK(calls >= 16u * 300u * 2u);   // every open and every close went through the lock
#ifdef FAKE
        std::printf("threads: overlapping calls inside the fake libc: %d of %ld\n", g_overlap.load(), g_fake_calls.load());
        CHECK(g_overlap.load() == 0);
        CHECK(waits > 0);   // the threads did contend
#endif
    } else if (!std::strcmp(mode, "inject")) {
        // MSC_BOOTIO_INJECT=2 (set by the script): two attempts fail, the third opens.
        const std::string path = g_dir + "/f3";
        const double t0 = Now();
        FILE* f = np_boot_fopen(path.c_str(), "rb", "test file");
        const double dt = Now() - t0;
        CHECK(f);
        char buf[8] = {};
        CHECK(std::fread(buf, 1, 7, f) > 0 && std::atoi(buf) == 3);
        std::fclose(f);
        std::printf("inject: opened after the injected failures in %.0f ms\n", dt * 1000);
        CHECK(dt >= 0.09 && dt < 1.0);   // two pauses of 50 ms
    } else if (!std::strcmp(mode, "giveup")) {
        // MSC_BOOTIO_INJECT=100000, MSC_BOOTIO_MS=400: bounded, returns null with EIO, does not hang
        const std::string path = g_dir + "/f3";
        const double t0 = Now();
        FILE* f = np_boot_fopen(path.c_str(), "rb", "test file");
        const double dt = Now() - t0;
        CHECK(!f && errno == EIO);
        std::printf("giveup: null after %.0f ms\n", dt * 1000);
        CHECK(dt >= 0.35 && dt < 1.5);
    } else if (!std::strcmp(mode, "missing")) {
        // a file that is not there: null after the budget (MSC_BOOTIO_MS=300), errno ENOENT
        const double t0 = Now();
        FILE* f = np_boot_fopen((g_dir + "/nope").c_str(), "rb", "missing file");
        const double dt = Now() - t0;
        CHECK(!f && errno == ENOENT);
        CHECK(dt < 1.5);
        std::printf("missing: null after %.0f ms\n", dt * 1000);
    } else if (!std::strcmp(mode, "now")) {
        // no failure: no pause at all
        const double t0 = Now();
        FILE* f = np_boot_fopen((g_dir + "/f5").c_str(), "rb", "plain");
        CHECK(f);
        std::fclose(f);
        CHECK(Now() - t0 < 0.04);
        std::printf("now: ok\n");
    } else {
        CHECK(!"mode");
    }
    std::string rm = "rm -rf " + g_dir;
    (void)!std::system(rm.c_str());
    std::printf("test_stdio_lock %s: ok\n", mode);
    return 0;
}
