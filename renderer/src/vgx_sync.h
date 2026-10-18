// vgx_sync.h: a counting semaphore for the hand-offs between the game thread, the record player and the back end (W5 PIPE).
//
// A condition-variable signal is lost when it is sent between the waiter's predicate test and its block; VRS found that on the emulator's pthread layer
// (the player waited for an empty slot that was empty, the back end for a full one) and worked around it with 2 ms timed waits. A semaphore cannot lose
// a post: the count is the state. On the Vita this is the kernel's own semaphore (what the platform layer's frame-send worker already uses), elsewhere a
// POSIX sem_t. No clock is read (the navigation harness replaces clock_gettime for the whole process; a timed wait would spin under it), so Linux waits
// are untimed; on the Vita Wait() returns after `logAfterMs` of waiting, once, to say so in the log ("[vgx-sync] ...") and then keeps waiting.
#ifndef VGX_SYNC_H
#define VGX_SYNC_H

#include <cstdio>

#if defined(__vita__)
#include <psp2/kernel/threadmgr.h>
#else
#include <errno.h>
#include <semaphore.h>
#endif

namespace vgx {

class Sema {
public:
    explicit Sema(int initial = 0, const char* name = "vgx") {
#if defined(__vita__)
        m_id = sceKernelCreateSema(name, 0, initial, 0x7FFFFFFF, nullptr);
        m_name = name;
#else
        (void)name;
        sem_init(&m_sem, 0, static_cast<unsigned>(initial));
#endif
    }
    void Post() {
#if defined(__vita__)
        sceKernelSignalSema(m_id, 1);
#else
        sem_post(&m_sem);
#endif
    }
    // Takes one unit, waiting for it. Returns the milliseconds waited (0 when it was there).
    void Wait(int logAfterMs = 2000) {
#if defined(__vita__)
        SceUInt us = 0;
        if (sceKernelWaitSema(m_id, 1, &us) >= 0) return;   // (a zero timeout: take it if it is there)
        for (int slice = 1;; ++slice) {
            us = static_cast<SceUInt>(logAfterMs) * 1000u;
            if (sceKernelWaitSema(m_id, 1, &us) >= 0) {
                if (slice > 1) std::fprintf(stderr, "[vgx-sync] \"%s\" posted after %d ms\n", m_name, slice * logAfterMs);
                return;
            }
            std::fprintf(stderr, "[vgx-sync] still waiting on \"%s\" after %d ms\n", m_name, slice * logAfterMs);
        }
#else
        (void)logAfterMs;
        while (sem_wait(&m_sem) != 0 && errno == EINTR) {}
#endif
    }
    bool TryWait() {
#if defined(__vita__)
        SceUInt us = 0;
        return sceKernelWaitSema(m_id, 1, &us) >= 0;
#else
        return sem_trywait(&m_sem) == 0;
#endif
    }

private:
#if defined(__vita__)
    SceUID m_id = -1;
    const char* m_name = "";
#else
    sem_t m_sem;
#endif
};

} // namespace vgx

#endif // VGX_SYNC_H
