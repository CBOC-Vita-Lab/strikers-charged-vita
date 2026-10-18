// np_vclock.cpp: the native port's clock (np_time.h): real, or the deterministic virtual clock of
// the harness. Ported from the recompiled build's harness (one/vita/src/plat_headless.cpp,
// namespace vclock) with these changes: the four-function API of np_time.h in front of it, the
// game-thread test by a recorded thread id instead of "tid == pid" on the Vita, i386 added to the
// glibc interposition, and the recompiled runtime's guest-clock cache hook (QW2) dropped.
//
// Virtual time: see np_time.h. Variables (env.txt on the Vita):
//   MSC_VIRTUAL_TIME=1       switch it on
//   MSC_VIRTUAL_TICK_NS=N    virtual nanoseconds per clock read on the game thread (default 1000)
// Ubuntu's GCC defines _TIME_BITS=64 on armhf: <time.h> then redirects clock_gettime and friends to
// their __*64 names, and this file defines both families (32-bit-time callers, built by clang or
// by older code, use the first; code built with 64-bit time_t the second). Plain names here.
#if defined(_TIME_BITS)
#undef _TIME_BITS
#endif
#include "np_time.h"

#pragma GCC diagnostic ignored "-Wnonnull-compare"   // glibc marks the arguments nonnull; the interposers test them anyway

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <ctime>

#if defined(__vita__)
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/rtc.h>
#include <pthread.h>
#elif defined(__linux__)
#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#if defined(__linux__) && (defined(__x86_64__) || defined(__i386__) || defined(__arm__)) && !defined(NP_NO_TIME_INTERPOSE)
#define NP_INTERPOSE_LIBC 1
#else
#define NP_INTERPOSE_LIBC 0
#endif

namespace npclock {
// Fixed start: CLOCK_MONOTONIC at 1000 s, CLOCK_REALTIME at 2026-01-01 12:00:00 UTC.
constexpr int64_t kMonoBaseNs = 1000ll * 1000000000ll;
constexpr int64_t kRealBaseNs = 1767268800ll * 1000000000ll;
std::atomic<int> g_mode{-1};          // -1 not decided, 0 real clock, 1 virtual
std::atomic<int64_t> g_ns{0};         // virtual nanoseconds since start
std::atomic<uint64_t> g_reads{0};     // clock reads on the game thread
int64_t g_tickNs = 1000;

// The game thread is the process's main thread: this object is constructed by the main thread
// before main() (init_priority: before any other static constructor that might read the clock).
struct MainThread {
    MainThread() {
#if defined(__vita__)
        self = sceKernelGetThreadId();
#elif defined(__linux__)
        self = static_cast<long>(syscall(SYS_gettid));
#endif
    }
    long self = 0;
};
MainThread g_main __attribute__((init_priority(101)));

bool Enabled() {
    int m = g_mode.load(std::memory_order_relaxed);
    if (m < 0) {
        // Decided on first use (a static initialiser of the game or the SDK layer, usually).
        const char* e = std::getenv("MSC_VIRTUAL_TIME");
        m = (e && *e && *e != '0') ? 1 : 0;
        if (const char* t = std::getenv("MSC_VIRTUAL_TICK_NS")) g_tickNs = std::strtoll(t, nullptr, 10);
        g_mode.store(m, std::memory_order_relaxed);
    }
    return m == 1;
}
bool OnGameThread() {
#if defined(__vita__)
    return sceKernelGetThreadId() == g_main.self;
#elif defined(__linux__)
    // Cached per thread. NOT while g_main is unset: a library constructor that runs before ours (Mesa, libstdc++ ...) and reads
    // the interposed libc clock on the main thread would otherwise cache "not the game thread" for the whole run (the game's
    // clock reads then never advance the clock: alarms one tick short of their deadline hang the idle scheduler).
    static thread_local int t_main = -1;
    if (t_main < 0) {
        if (g_main.self == 0) return false;
        t_main = (syscall(SYS_gettid) == g_main.self) ? 1 : 0;
    }
    return t_main == 1;
#else
    return true;
#endif
}
// Only the game thread moves the clock; other threads read it.
int64_t Read() {
    if (OnGameThread()) {
        g_reads.fetch_add(1, std::memory_order_relaxed);
        return g_ns.fetch_add(g_tickNs, std::memory_order_relaxed);
    }
    return g_ns.load(std::memory_order_relaxed);
}
void Advance(int64_t ns) { if (ns > 0) g_ns.fetch_add(ns, std::memory_order_relaxed); }
void AdvanceTo(int64_t ns) {
    int64_t cur = g_ns.load(std::memory_order_relaxed);
    while (cur < ns && !g_ns.compare_exchange_weak(cur, ns, std::memory_order_relaxed)) {}
}

#if defined(__linux__)
template <typename Fn> Fn Next(const char* name) { return reinterpret_cast<Fn>(dlsym(RTLD_NEXT, name)); }
bool IsMono(clockid_t id) {
    return id == CLOCK_MONOTONIC || id == CLOCK_MONOTONIC_RAW || id == CLOCK_MONOTONIC_COARSE || id == CLOCK_BOOTTIME;
}
bool IsReal(clockid_t id) { return id == CLOCK_REALTIME || id == CLOCK_REALTIME_COARSE; }
// The real clocks, bypassing the interposed names.
int64_t RealNs(clockid_t id) {
    using Fn = int (*)(clockid_t, struct timespec*);
    static const Fn fn = Next<Fn>("clock_gettime");
    struct timespec ts{};
    if (fn) fn(id, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000ll + ts.tv_nsec;
}
#endif
} // namespace npclock

extern "C" {

int NpVirtualTimeActive(void) { return npclock::Enabled() ? 1 : 0; }

int64_t NpMonotonicNs(void) {
    if (npclock::Enabled()) return npclock::Read() + npclock::kMonoBaseNs;
#if defined(__vita__)
    return static_cast<int64_t>(sceKernelGetSystemTimeWide()) * 1000ll;
#elif defined(__linux__)
    return npclock::RealNs(CLOCK_MONOTONIC);
#else
    return 0;
#endif
}

int64_t NpRealtimeNs(void) {
    if (npclock::Enabled()) return npclock::Read() + npclock::kRealBaseNs;
#if defined(__vita__)
    SceKernelTimeval tv;
    if (sceKernelLibcGettimeofday(&tv, nullptr) < 0) return 0;
    return static_cast<int64_t>(tv.sec) * 1000000000ll + static_cast<int64_t>(tv.usec) * 1000ll;
#elif defined(__linux__)
    return npclock::RealNs(CLOCK_REALTIME);
#else
    return 0;
#endif
}

void NpSleepNs(int64_t ns) {
    if (ns <= 0) return;
    if (npclock::Enabled() && npclock::OnGameThread()) {
        npclock::Advance(ns);
#if defined(__linux__)
        sched_yield();
#endif
        return;
    }
#if defined(__vita__)
    sceKernelDelayThread(static_cast<SceUInt32>((ns + 999) / 1000));
#elif defined(__linux__)
    using Fn = int (*)(const struct timespec*, struct timespec*);
    static const Fn real = npclock::Next<Fn>("nanosleep");
    struct timespec ts{static_cast<time_t>(ns / 1000000000ll), static_cast<long>(ns % 1000000000ll)};
    if (real) real(&ts, nullptr);
#endif
}

void NpVirtualAdvanceNs(int64_t ns) { if (npclock::Enabled()) npclock::Advance(ns); }
int64_t NpVirtualNowNs(void) { return npclock::Enabled() ? npclock::g_ns.load(std::memory_order_relaxed) + npclock::kMonoBaseNs : 0; }

double NpRealSeconds(void) {
#if defined(__vita__)
    return sceKernelGetSystemTimeWide() * 1e-6;
#elif defined(__linux__)
    return npclock::RealNs(CLOCK_MONOTONIC) * 1e-9;
#else
    return 0.0;
#endif
}
double NpVirtualSeconds(void) { return npclock::g_ns.load(std::memory_order_relaxed) * 1e-9; }
uint64_t NpVirtualReads(void) { return npclock::g_reads.load(std::memory_order_relaxed); }

} // extern "C"

#if NP_INTERPOSE_LIBC
// The definitions below take precedence over libc's for every caller in the process (the
// executable is searched first). With the real clock they forward to libc unchanged.
extern "C" {
int clock_gettime(clockid_t id, struct timespec* ts) noexcept {
    using Fn = int (*)(clockid_t, struct timespec*);
    static const Fn real = npclock::Next<Fn>("clock_gettime");
    if (npclock::Enabled() && ts && (npclock::IsMono(id) || npclock::IsReal(id))) {
        const int64_t ns = npclock::Read() + (npclock::IsMono(id) ? npclock::kMonoBaseNs : npclock::kRealBaseNs);
        ts->tv_sec = static_cast<time_t>(ns / 1000000000ll);
        ts->tv_nsec = static_cast<long>(ns % 1000000000ll);
        return 0;
    }
    return real(id, ts);
}
int nanosleep(const struct timespec* req, struct timespec* rem) {
    using Fn = int (*)(const struct timespec*, struct timespec*);
    static const Fn real = npclock::Next<Fn>("nanosleep");
    if (npclock::Enabled() && req && npclock::OnGameThread()) {
        npclock::Advance(static_cast<int64_t>(req->tv_sec) * 1000000000ll + req->tv_nsec);
        if (rem) rem->tv_sec = 0, rem->tv_nsec = 0;
        sched_yield();
        return 0;
    }
    return real(req, rem);
}
int clock_nanosleep(clockid_t id, int flags, const struct timespec* req, struct timespec* rem) {
    using Fn = int (*)(clockid_t, int, const struct timespec*, struct timespec*);
    static const Fn real = npclock::Next<Fn>("clock_nanosleep");
    if (npclock::Enabled() && req && npclock::OnGameThread() && (npclock::IsMono(id) || npclock::IsReal(id))) {
        const int64_t t = static_cast<int64_t>(req->tv_sec) * 1000000000ll + req->tv_nsec;
        if (flags & TIMER_ABSTIME) npclock::AdvanceTo(t - (npclock::IsMono(id) ? npclock::kMonoBaseNs : npclock::kRealBaseNs));
        else npclock::Advance(t);
        if (rem) rem->tv_sec = 0, rem->tv_nsec = 0;
        sched_yield();
        return 0;
    }
    return real(id, flags, req, rem);
}
time_t time(time_t* out) noexcept {
    using Fn = time_t (*)(time_t*);
    static const Fn real = npclock::Next<Fn>("time");
    if (npclock::Enabled()) {
        const time_t t = static_cast<time_t>((npclock::Read() + npclock::kRealBaseNs) / 1000000000ll);
        if (out) *out = t;
        return t;
    }
    return real(out);
}
int gettimeofday(struct timeval* tv, void* tz) noexcept {
    using Fn = int (*)(struct timeval*, void*);
    static const Fn real = npclock::Next<Fn>("gettimeofday");
    if (npclock::Enabled() && tv) {
        const int64_t ns = npclock::Read() + npclock::kRealBaseNs;
        tv->tv_sec = static_cast<time_t>(ns / 1000000000ll);
        tv->tv_usec = static_cast<suseconds_t>((ns % 1000000000ll) / 1000);
        return 0;
    }
    return real(tv, tz);
}
#if defined(__arm__) || defined(__i386__)
// 32-bit glibc 2.34+: code built with 64-bit time_t (libstdc++.so, where steady_clock::now and
// the out-of-line sleep live) calls these names instead; objects built here use the 32-bit ones
// above. Layouts as glibc's __timespec64 / __timeval64 on a little-endian 32-bit target.
struct NpTimespec64 { int64_t tv_sec; int32_t tv_nsec; int32_t pad; };
struct NpTimeval64 { int64_t tv_sec; int64_t tv_usec; };
int __clock_gettime64(clockid_t id, NpTimespec64* ts) {
    using Fn = int (*)(clockid_t, NpTimespec64*);
    static const Fn real = npclock::Next<Fn>("__clock_gettime64");
    if (npclock::Enabled() && ts && (npclock::IsMono(id) || npclock::IsReal(id))) {
        const int64_t ns = npclock::Read() + (npclock::IsMono(id) ? npclock::kMonoBaseNs : npclock::kRealBaseNs);
        ts->tv_sec = ns / 1000000000ll;
        ts->tv_nsec = static_cast<int32_t>(ns % 1000000000ll);
        ts->pad = 0;
        return 0;
    }
    return real(id, ts);
}
int __nanosleep64(const NpTimespec64* req, NpTimespec64* rem) {
    using Fn = int (*)(const NpTimespec64*, NpTimespec64*);
    static const Fn real = npclock::Next<Fn>("__nanosleep64");
    if (npclock::Enabled() && req && npclock::OnGameThread()) {
        npclock::Advance(req->tv_sec * 1000000000ll + req->tv_nsec);
        if (rem) rem->tv_sec = 0, rem->tv_nsec = 0;
        sched_yield();
        return 0;
    }
    return real(req, rem);
}
int __clock_nanosleep_time64(clockid_t id, int flags, const NpTimespec64* req, NpTimespec64* rem) {
    using Fn = int (*)(clockid_t, int, const NpTimespec64*, NpTimespec64*);
    static const Fn real = npclock::Next<Fn>("__clock_nanosleep_time64");
    if (npclock::Enabled() && req && npclock::OnGameThread() && (npclock::IsMono(id) || npclock::IsReal(id))) {
        const int64_t t = req->tv_sec * 1000000000ll + req->tv_nsec;
        if (flags & TIMER_ABSTIME) npclock::AdvanceTo(t - (npclock::IsMono(id) ? npclock::kMonoBaseNs : npclock::kRealBaseNs));
        else npclock::Advance(t);
        if (rem) rem->tv_sec = 0, rem->tv_nsec = 0;
        sched_yield();
        return 0;
    }
    return real(id, flags, req, rem);
}
int64_t __time64(int64_t* out) {
    using Fn = int64_t (*)(int64_t*);
    static const Fn real = npclock::Next<Fn>("__time64");
    if (npclock::Enabled()) {
        const int64_t t = (npclock::Read() + npclock::kRealBaseNs) / 1000000000ll;
        if (out) *out = t;
        return t;
    }
    return real(out);
}
int __gettimeofday64(NpTimeval64* tv, void* tz) {
    using Fn = int (*)(NpTimeval64*, void*);
    static const Fn real = npclock::Next<Fn>("__gettimeofday64");
    if (npclock::Enabled() && tv) {
        const int64_t ns = npclock::Read() + npclock::kRealBaseNs;
        tv->tv_sec = ns / 1000000000ll;
        tv->tv_usec = (ns % 1000000000ll) / 1000;
        return 0;
    }
    return real(tv, tz);
}
#endif // __arm__ || __i386__
} // extern "C"
#endif // NP_INTERPOSE_LIBC
