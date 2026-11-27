// See np_boot_io.h. One lock around every call that takes or gives back a slot of libc's FILE table, and the bounded start-up retry.
#include "np_boot_io.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <unistd.h>
#if defined(__vita__)
#include <reent.h>
#include <sys/types.h>
#endif

extern "C" {
FILE* __real_fopen(const char*, const char*);
FILE* __real_freopen(const char*, const char*, FILE*);
FILE* __real_fdopen(int, const char*);
FILE* __real_tmpfile(void);
int __real_fclose(FILE*);
}

namespace {

std::atomic<int> g_lock{0};
std::atomic<unsigned> g_calls{0}, g_waits{0}, g_timeouts{0};

unsigned long long NowUs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<unsigned long long>(ts.tv_sec) * 1000000ull + static_cast<unsigned long long>(ts.tv_nsec) / 1000ull;
}

// A spin that sleeps: the critical section is one open (a few ms on the card), so a waiter naps 0.5 ms at a time. After 3 s (a thread that died inside the call) it goes on
// without the lock: a possible slot race is better than a hang.
struct Guard {
    bool held = false;
    Guard() {
        g_calls.fetch_add(1, std::memory_order_relaxed);
        int expect = 0;
        if (g_lock.compare_exchange_strong(expect, 1, std::memory_order_acquire)) { held = true; return; }
        g_waits.fetch_add(1, std::memory_order_relaxed);
        for (unsigned i = 0; i < 6000; ++i) {
            if (i >= 20) usleep(500);
            expect = 0;
            if (g_lock.compare_exchange_weak(expect, 1, std::memory_order_acquire)) { held = true; return; }
        }
        g_timeouts.fetch_add(1, std::memory_order_relaxed);
    }
    ~Guard() { if (held) g_lock.store(0, std::memory_order_release); }
};

} // namespace

extern "C" {

FILE* __wrap_fopen(const char* path, const char* mode) { Guard g; return __real_fopen(path, mode); }
FILE* __wrap_freopen(const char* path, const char* mode, FILE* stream) { Guard g; return __real_freopen(path, mode, stream); }
FILE* __wrap_fdopen(int fd, const char* mode) { Guard g; return __real_fdopen(fd, mode); }
FILE* __wrap_tmpfile(void) { Guard g; return __real_tmpfile(); }
// fclose gives the slot back (its flags are cleared): under the same lock, so a slot is never claimed while it is being released.
int __wrap_fclose(FILE* stream) { Guard g; return __real_fclose(stream); }

#if defined(__vita__)
// (wave 14, from the Galaxy port's vgx-game/fd_guard.cpp, a start-up crash seen on the console) A seek on a file handle that is not open must fail, not crash.
// The Vita C library's _lseek_r (the call under fseek) asks __vita_fd_grab for the handle's record and reads it without checking for NULL (a data abort at address 4). Wrapped
// (vita.mk: -Wl,--wrap=_lseek_r): a handle with no record answers -1 / EBADF, which fseek reports like any failed seek; every other seek goes on to the library unchanged.
// MSC_FD_GUARD=0 forwards every call (read once).
void* __vita_fd_grab(int fd);
void __vita_fd_drop(void* descriptor);
off_t __real__lseek_r(struct _reent* reent, int fd, off_t offset, int whence);
off_t __wrap__lseek_r(struct _reent* reent, int fd, off_t offset, int whence) {
    static const bool on = [] { const char* e = std::getenv("MSC_FD_GUARD"); return !(e && e[0] == '0'); }();
    if (on) {
        void* const descriptor = __vita_fd_grab(fd);
        if (!descriptor) {
            static std::atomic<int> reported{0};
            if (reported.fetch_add(1, std::memory_order_relaxed) < 3) std::fprintf(stderr, "[io] a seek on file handle %d, which is not open, was refused (EBADF)\n", fd);
            if (reent) reent->_errno = EBADF;
            return static_cast<off_t>(-1);
        }
        __vita_fd_drop(descriptor);
    }
    return __real__lseek_r(reent, fd, offset, whence);
}
#endif

void np_stdio_lock_stats(unsigned* calls, unsigned* waits, unsigned* timeouts) {
    if (calls) *calls = g_calls.load();
    if (waits) *waits = g_waits.load();
    if (timeouts) *timeouts = g_timeouts.load();
}

// ---- the start-up retry -----------------------------------------------------------------------------------------------------------

static unsigned BudgetMs() {
    static const unsigned ms = [] {
        const char* e = std::getenv("MSC_BOOTIO_MS");
        return (e && *e) ? static_cast<unsigned>(std::strtoul(e, nullptr, 10)) : 3000u;
    }();
    return ms;
}

void np_boot_try_begin(NpBootTry* t) {
    t->attempts = 0;
    t->startUs = NowUs();
}

int np_boot_try_again(NpBootTry* t, const char* what, const char* path, int err) {
    ++t->attempts;
    const unsigned long long spentMs = (NowUs() - t->startUs) / 1000ull;
    const bool more = spentMs < BudgetMs();
    std::fprintf(stderr, "[bootio] %s: %s failed (attempt %u, %s, %llu ms)%s\n", what ? what : "file", path ? path : "(no path)", t->attempts,
                 err ? std::strerror(err) : "no error code", spentMs, more ? "; trying again" : "; giving up");
    if (!more) return 0;
    if (NpBootScreenNotice) NpBootScreenNotice();
    usleep(50000);
    return 1;
}

FILE* np_boot_fopen_once(const char* path, const char* mode) {
    static std::atomic<unsigned> injectLeft{[] {
        const char* e = std::getenv("MSC_BOOTIO_INJECT");
        return (e && *e) ? static_cast<unsigned>(std::strtoul(e, nullptr, 10)) : 0u;
    }()};
    unsigned left = injectLeft.load();
    while (left) {
        if (injectLeft.compare_exchange_weak(left, left - 1)) { errno = EIO; return nullptr; }   // an injected failure
    }
    errno = 0;
    FILE* f = std::fopen(path, mode);
    if (f && fileno(f) < 0) {
        // a stream that shares a slot with another (the race this file prevents): its descriptor is -1. Not closed: that would free the other owner's slot too.
        errno = EBADF;
        return nullptr;
    }
    if (!f && errno == 0) errno = EIO;
    return f;
}

FILE* np_boot_fopen(const char* path, const char* mode, const char* what) {
    NpBootTry t;
    np_boot_try_begin(&t);
    for (;;) {
        FILE* f = np_boot_fopen_once(path, mode);
        if (f) return f;
        const int err = errno;
        if (!np_boot_try_again(&t, what, path, err)) { errno = err; return nullptr; }
    }
}

} // extern "C"
