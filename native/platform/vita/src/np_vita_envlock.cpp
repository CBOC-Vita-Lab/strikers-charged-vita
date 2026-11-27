// __env_lock / __env_unlock for the Vita build (wave 9, galcheck; Galaxy's finding of 13 Nov 2026).
//
// newlib's getenv, setenv, unsetenv and putenv bracket their walk of `environ` with __env_lock / __env_unlock. In the SDK's libc both are an EMPTY function (libc_a-envlock.o: a
// bare return; 2 bytes in image r9). setenv reallocates the environment array for every NEW name, and the new array usually lands in the block just freed, so a thread that is
// inside getenv at that moment walks freed memory (a data abort on a console: Galaxy's render thread in getenv -> _findenv_r -> strncmp, about 1 boot in 10). This object defines
// the two functions. Because it is linked directly (platform/vita/src), the archive member of the same name is never pulled and there is no duplicate symbol.
//
// The lock is RECURSIVE (_setenv_r calls _findenv_r with the lock held and getenv/_getenv_r paths nest), owned by the thread whose TLS register (TPIDRURO, `mrc p15,0,rN,c13,c0,3`: no
// system call, getenv is called often) is stored in the owner word, with a depth count only the owner touches. A waiter sleeps 100 us at a time (no spin). A holder that never lets go
// (a thread that died inside the walk) is waited for 2 s, then the waiter goes on without the lock: a possible race is better than a hang.
//
// Still true with the lock: the pointer getenv returns is valid for as long as nobody calls setenv for THAT name (newlib leaks the old string rather than freeing it, so it is valid even
// then), and the lock makes the walk atomic, not the use of the result.
#include <atomic>
#include <stdint.h>

#if defined(NP_ENVLOCK_HOST)
#include <unistd.h>
struct _reent;
static inline uint32_t SelfId() {
    static thread_local char tag;
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&tag));
}
static inline void Nap() { usleep(100); }
#else
#include <psp2/kernel/threadmgr.h>
#include <reent.h>
static inline uint32_t SelfId() {
    uint32_t v;
    __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(v));
    return v ? v : 1u;
}
static inline void Nap() { sceKernelDelayThread(100); }
#endif

namespace {
std::atomic<uint32_t> g_owner{0};
uint32_t g_depth = 0;   // the owner's
std::atomic<uint32_t> g_calls{0}, g_waits{0}, g_timeouts{0};
}

extern "C" {

void __env_lock(struct _reent*) {
    const uint32_t me = SelfId();
    g_calls.fetch_add(1, std::memory_order_relaxed);
    if (g_owner.load(std::memory_order_relaxed) == me) {   // only this thread can have stored `me`, so a relaxed read is exact
        ++g_depth;
        return;
    }
    uint32_t expect = 0;
    if (!g_owner.compare_exchange_strong(expect, me, std::memory_order_acquire)) {
        g_waits.fetch_add(1, std::memory_order_relaxed);
        for (unsigned i = 0;; ++i) {
            if (i >= 20000) {   // 2 s
                g_timeouts.fetch_add(1, std::memory_order_relaxed);
                return;          // no lock taken: __env_unlock below sees it is not the owner and does nothing
            }
            Nap();
            expect = 0;
            if (g_owner.compare_exchange_weak(expect, me, std::memory_order_acquire)) break;
        }
    }
    g_depth = 1;
}

void __env_unlock(struct _reent*) {
    if (g_owner.load(std::memory_order_relaxed) != SelfId()) return;
    if (--g_depth == 0) g_owner.store(0, std::memory_order_release);
}

void np_env_lock_stats(unsigned* calls, unsigned* waits, unsigned* timeouts) {
    if (calls) *calls = g_calls.load();
    if (waits) *waits = g_waits.load();
    if (timeouts) *timeouts = g_timeouts.load();
}

}
