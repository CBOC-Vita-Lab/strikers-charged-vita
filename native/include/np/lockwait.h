// np/lockwait.h (simfix20, fable-verify10 P4): what a spin lock does while it waits. The game-heap lock (src/NL/nlMemory.cpp) and the frame-send hook lock
// (platform/vita/src/np_framesend.cpp) were `64 tries, then sched_yield()`. On the Vita sched_yield (vitasdk libpthread) is pte_osThreadSleep(1) =
// sceKernelDelayThread(1000): a 1 ms sleep, and with MSC_SIMT=2 two threads share both locks, so one pre-emption of the holder cost the game thread a millisecond.
// Vita: 1000 tries with the ARM `yield` hint (a few us: the holder is on the other core and is about to let go), then sceKernelDelayThread(20).
// Host: unchanged (64 tries, then sched_yield). `*slow` counts the waits that ran out of tries (once per acquisition): 0 in a single-threaded run, the lock is uncontended there.
#ifndef NP_LOCKWAIT_H
#define NP_LOCKWAIT_H
#include <sched.h>
#if defined(__vita__)
#include <psp2/kernel/threadmgr.h>
#endif

// `spins` is the caller's counter of failed tries (starts at 0); call once per failed try.
static inline void NpLockBackoff(int& spins, unsigned* slow)
{
#if defined(__vita__)
    if (++spins <= 1000)
    {
        __asm__ volatile("yield" ::: "memory");
        return;
    }
    if (spins == 1001) __atomic_fetch_add(slow, 1u, __ATOMIC_RELAXED);
    sceKernelDelayThread(20);
#else
    if (++spins > 64)
    {
        if (spins == 65) __atomic_fetch_add(slow, 1u, __ATOMIC_RELAXED);
        sched_yield();
    }
#endif
}
#endif
