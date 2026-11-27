// crash26b: the log streams' printf family without newlib's FILE.
//
// The SDK's newlib has no FILE lock (built without _RETARGETABLE_LOCKING): two threads printing to stderr at once lost updates of the stream's
// _p / _w and __sfvwrite_r copied (size_t)-1 bytes over the heap (the two console dumps of 18 Nov, w8/crash26.md). crash26 made the streams
// unbuffered, which put newlib's 1 KiB __sbprintf buffer (+ a second vfprintf frame) on the stack of every printf, and the console stalled at the end
// of start-up (s29-crash26, 6 of 6 runs, no dump). Leading suspect: the 4 KiB SceGxmDisplayQueue thread prints from the display callback (a 1.1 KiB
// frame of its own); the static frame sums of that path are ~1.7 KiB line-buffered and ~3.2-3.6 KiB unbuffered before the SDK's own frames.
//
// Here the wrapped stdio calls (vita.mk -Wl,--wrap=printf,... in logging builds, vita_silent_stdio.cpp) never touch stdout / stderr's FILE:
//   - text calls (fputs, fwrite, puts, putc, fputc, putchar) hand their bytes to the sink at once (VitaLog::Write has its own mutex);
//   - printf calls format with vsnprintf (its string FILE is on the caller's stack: the same depth as the 1.2 path, no 1 KiB buffer) into ONE
//     shared 4 KiB buffer under a spin lock, hand the whole text to the sink and release the lock. Nothing inside the lock prints or calls the
//     wrapped functions; the call that runs after the sink (the console refresh) runs after the release.
//   - the lock is never waited for without a bound: a thread that already holds it (a print from inside the sink) or that waited 1 s formats into
//     256 bytes of its own instead (counted). Longer texts than the buffer: a heap copy, outside the lock.
// Header only so that the host stress test (tests/test_log_stdio.cpp) runs exactly this code with host threads.
#pragma once

#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__vita__)
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#define NP_LOGSTDIO_TID() static_cast<int>(sceKernelGetThreadId())
#define NP_LOGSTDIO_NAP(us) sceKernelDelayThread(us)
#define NP_LOGSTDIO_NOW_US() static_cast<unsigned long long>(sceKernelGetProcessTimeWide())
#else
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#define NP_LOGSTDIO_TID() static_cast<int>(::syscall(SYS_gettid))
#define NP_LOGSTDIO_NAP(us) ::usleep(us)
inline unsigned long long NpLogStdioNowUs() { timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return static_cast<unsigned long long>(ts.tv_sec) * 1000000ull + static_cast<unsigned long long>(ts.tv_nsec) / 1000ull; }
#define NP_LOGSTDIO_NOW_US() NpLogStdioNowUs()
#endif

namespace NpLogStdio {

using Sink = void (*)(const char* data, size_t size);   // called with the lock held for printf texts; must not print
using After = void (*)();                               // called after the release (may print)

struct Stats {
    std::atomic<unsigned long long> calls{0}, bytes{0};
    std::atomic<unsigned> waits{0}, timeouts{0}, nested{0}, longTexts{0}, errors{0};
};

constexpr size_t kBuffer = 4096;
constexpr unsigned kSpins = 64;
constexpr unsigned kNapUs = 50;
constexpr unsigned kWaitUs = 1000000;   // 1 s, then the caller's own 256 bytes

struct State {
    std::atomic<int> owner{0};
    char buffer[kBuffer] = {};
    Sink sink = nullptr;
    After after = nullptr;
    Stats stats;
};

// Constant-initialised (every member has a constant initialiser: no __cxa_guard at the first print). The first prints come from a static constructor
// (np_vita_main.cpp EarlyBoot, init_priority 101), before the threads layer is up; a guard there runs libstdc++'s gthread once / mutex:
// the first s29-crash26b build had one and wrote an empty log in the emulator.
inline State g_state;
inline State& G() { return g_state; }

inline void Configure(Sink sink, After after) {
    G().sink = sink;
    G().after = after;
}

inline void Emit(const char* data, size_t size) {
    State& s = G();
    if (!size || !s.sink) return;
    s.stats.calls.fetch_add(1, std::memory_order_relaxed);
    s.stats.bytes.fetch_add(size, std::memory_order_relaxed);
    s.sink(data, size);
}

inline void AfterCall() {
    if (G().after) G().after();
}

// Text calls: no formatting, no shared buffer.
inline int Put(const char* data, size_t size) {
    Emit(data, size);
    AfterCall();
    return static_cast<int>(size);
}

inline bool Acquire(int self) {
    State& s = G();
    int expect = 0;
#if defined(NP_LOGSTDIO_MUTANT_NOLOCK)
    (void)expect; (void)self;
    return true;   // host test's negative control only: no lock at all
#endif
    if (s.owner.compare_exchange_strong(expect, self, std::memory_order_acquire)) return true;
    s.stats.waits.fetch_add(1, std::memory_order_relaxed);
    unsigned long long start = 0;
    for (unsigned i = 0;; ++i) {
        expect = 0;
        if (s.owner.compare_exchange_weak(expect, self, std::memory_order_acquire)) return true;
        if (i < kSpins) continue;
        const unsigned long long now = NP_LOGSTDIO_NOW_US();
        if (!start) start = now;
        else if (now - start >= kWaitUs) return false;
        NP_LOGSTDIO_NAP(kNapUs);   // (the holder may be a lower-priority thread on this core: let it run)
    }
}

inline void Release() {
    G().owner.store(0, std::memory_order_release);
}

// The lock could not be had (or this thread holds it): 256 bytes on this thread's stack, a longer text is cut.
__attribute__((noinline)) inline int VPrintOwn(const char* format, va_list args) {
    char own[256];
    const int n = std::vsnprintf(own, sizeof own, format, args);
    if (n < 0) {
        G().stats.errors.fetch_add(1, std::memory_order_relaxed);
        return n;
    }
    if (static_cast<size_t>(n) < sizeof own) {
        Emit(own, static_cast<size_t>(n));
    } else {
        own[sizeof own - 2] = '\n';   // cut: 254 bytes and a newline, so the next text starts a line of its own
        Emit(own, sizeof own - 1);
    }
    AfterCall();
    return n;
}

inline int VPrint(const char* format, va_list args) {
    State& s = G();
    if (!format) return 0;
    const int self = NP_LOGSTDIO_TID();
    if (s.owner.load(std::memory_order_relaxed) == self) {   // a print from inside the sink: never wait for ourselves
        s.stats.nested.fetch_add(1, std::memory_order_relaxed);
        return VPrintOwn(format, args);
    }
    if (!Acquire(self)) {
        s.stats.timeouts.fetch_add(1, std::memory_order_relaxed);
        return VPrintOwn(format, args);
    }
    va_list first;
    va_copy(first, args);
    const int n = std::vsnprintf(s.buffer, kBuffer, format, first);
    va_end(first);
    if (n < 0) {
        Release();
        s.stats.errors.fetch_add(1, std::memory_order_relaxed);
        return n;
    }
    if (static_cast<size_t>(n) < kBuffer) {
        Emit(s.buffer, static_cast<size_t>(n));
        Release();
        AfterCall();
        return n;
    }
    Release();
    s.stats.longTexts.fetch_add(1, std::memory_order_relaxed);
    char* big = static_cast<char*>(std::malloc(static_cast<size_t>(n) + 1));
    if (!big) return VPrintOwn(format, args);
    std::vsnprintf(big, static_cast<size_t>(n) + 1, format, args);
    Emit(big, static_cast<size_t>(n));
    std::free(big);
    AfterCall();
    return n;
}

inline int Print(const char* format, ...) __attribute__((format(printf, 1, 2)));
inline int Print(const char* format, ...) {
    va_list args;
    va_start(args, format);
    const int n = VPrint(format, args);
    va_end(args);
    return n;
}

} // namespace NpLogStdio
