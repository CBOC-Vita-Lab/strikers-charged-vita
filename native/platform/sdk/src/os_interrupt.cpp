// OSDisableInterrupts / OSEnableInterrupts / OSRestoreInterrupts as a global lock.
//
// The console has one CPU; "interrupts" are the way handlers (alarms, the AX frame callback, disc
// completions, retrace) preempt the game. Here the handlers run on helper threads, and the game's
// critical sections (which it writes as OSDisableInterrupts ... OSRestoreInterrupts) take the same
// lock, so the game's exclusion against its handlers is preserved. The "enabled" state is per
// thread (as it is per context on the console): a thread that blocks in the scheduler with
// interrupts disabled releases the lock while it is parked (np_irq_release_for_wait).
#include <revolution/os.h>

#include "np_internal.h"

#include <stdlib.h>
#include <string.h>

static NpMutex g_lock;
static volatile int g_lock_ready = 0;
volatile int np_os_initialized = 0;
// (wave 14, from the Galaxy port's tls40) the per-thread words in ONE __thread block: the console's emulated TLS costs a call of __emutls_get_address per object per function,
// and OSDisableInterrupts / OSRestoreInterrupts (the game's critical sections) read two or three of them each. One block is one call. The old names stay as macros.
struct NpIrqTls { int disabled; int irq; int exitOwner; };
static __thread NpIrqTls tl_irqBlock;
#define tl_disabled (tl_irqBlock.disabled)
#define tl_irq (tl_irqBlock.irq)
#define tl_exit_owner (tl_irqBlock.exitOwner)
// Handler threads waiting for the lock. A game thread that re-takes the lock in a tight loop (disable, restore,
// disable ...) could keep barging in ahead of them (a mutex wake-up is not a hand-over), which on the console
// cannot happen: an enabled interrupt is taken at once. So the thread that gives the lock up waits a few
// microseconds for it to be taken when someone is waiting.
static volatile int g_irq_waiting = 0;
static inline void cpu_relax(void) {
#if defined(__arm__)
    __asm__ __volatile__("yield" ::: "memory");
#elif defined(__i386__) || defined(__x86_64__)
    __asm__ __volatile__("pause" ::: "memory");
#else
    __asm__ __volatile__("" ::: "memory");
#endif
}

static NpMutex g_statLock;
static void lock_init_once(void) {
    if (!g_lock_ready) {
        np_mutex_init(&g_lock);
        np_mutex_init(&g_statLock);   // (MSC_IRQ_WAITSTAT)
        g_lock_ready = 1;
    }
}

__attribute__((constructor(101))) static void np_irq_early_init(void) { lock_init_once(); }

extern "C" {

// Process exit: the handler threads must not run game code while the game's static destructors run (an AX callback
// into destroyed objects would turn a clean exit into a crash). np_exit_quiesce (an atexit handler registered by
// NpSdk_Init, so it runs before the destructors of everything constructed earlier) waits for a handler in progress by
// taking the interrupt lock and keeps it; handler threads that arrive later park for good.
static volatile int g_exiting = 0;
// tl_exit_owner (in the block above): the thread running the exit handlers: keeps "interrupts disabled" for good
static void park_forever(void) {
    for (;;) np_sleep_ns(1000000000ull);
}

void np_irq_enter(void) {
    lock_init_once();
    if (g_exiting) park_forever();
    __atomic_add_fetch(&g_irq_waiting, 1, __ATOMIC_RELAXED);
    np_mutex_lock(&g_lock);
    __atomic_sub_fetch(&g_irq_waiting, 1, __ATOMIC_RELAXED);
    if (g_exiting) {   // the exit handler holds the lock logically: it released it only to let this thread see the flag
        np_mutex_unlock(&g_lock);
        park_forever();
    }
    tl_disabled = 1;
    tl_irq = 1;
}
void np_exit_quiesce(void) {
    g_exiting = 1;
    if (!tl_disabled) {   // (a caller that already holds it, e.g. a handler calling exit, keeps it)
        np_mutex_lock(&g_lock);
        tl_disabled = 1;
    }
    tl_exit_owner = 1;   // the lock is never released: the destructors that follow may call OSDisable/RestoreInterrupts
}
void np_irq_leave(void) {
    tl_irq = 0;
    tl_disabled = 0;
    np_mutex_unlock(&g_lock);
}
int np_in_irq(void) { return tl_irq; }
int np_irq_disabled(void) { return tl_disabled; }

void np_irq_release_for_wait(int* saved) {
    *saved = tl_disabled && !tl_exit_owner;
    if (tl_disabled && !tl_exit_owner) {
        tl_disabled = 0;
        np_mutex_unlock(&g_lock);
    }
}
void np_irq_reacquire_after_wait(int saved) {
    if (saved) {
        np_mutex_lock(&g_lock);
        tl_disabled = 1;
    }
}

// MSC_IRQ_WAITSTAT=1 (diagnostic, default off; read once): what the interrupt lock costs the guest threads (the game thread, and the network
// threads the game creates). Every 5 s of real time one `[irq]` line: how often and how long a guest thread waited in OSDisableInterrupts
// for a handler (in practice the AX frame callback, every 3 ms, which the audio thread runs under the lock: dsp_hle.cpp), how long the
// thread that gave the lock up spun for a waiting handler (OSRestoreInterrupts), and how long the AX callback held the lock. On the console
// the callback preempted the game outright; here it runs on the audio thread's core and costs the game thread only these waits. Timing only:
// the lock is taken and given up exactly as without the switch.
static int g_waitstat = -1;
static inline int waitstat(void) {
    if (g_waitstat < 0) {
        const char* e = getenv("MSC_IRQ_WAITSTAT");
        g_waitstat = (e && e[0] == '1') ? 1 : 0;
    }
    return g_waitstat;
}
static struct {
    uint64_t waitNs, waitMax, spinNs, holdNs, holdMax, t0;
    uint32_t waits, takes, spins, holds;
} g_ws;
static void waitstat_add(int kind, uint64_t ns) {   // 0 a wait, 1 a spin, 2 an AX callback's hold, 3 an uncontended take
    np_mutex_lock(&g_statLock);
    const uint64_t now = np_time_real_ns();
    if (g_ws.t0 == 0) g_ws.t0 = now;
    if (kind == 0) { g_ws.waitNs += ns; if (ns > g_ws.waitMax) g_ws.waitMax = ns; ++g_ws.waits; ++g_ws.takes; }
    else if (kind == 1) { g_ws.spinNs += ns; ++g_ws.spins; }
    else if (kind == 2) { g_ws.holdNs += ns; if (ns > g_ws.holdMax) g_ws.holdMax = ns; ++g_ws.holds; }
    else ++g_ws.takes;
    if (now - g_ws.t0 >= 5000000000ull) {
        const double s = (double)(now - g_ws.t0) / 1e9;
        np_log("irq", "[irq] last %.1f s: guest threads took the interrupt lock %u times, waited %u times for %.2f ms (largest %.0f us), "
               "spun %.2f ms for a waiting handler (%u times); the AX frame callback held it %.2f ms in %u calls (largest %.0f us) (MSC_IRQ_WAITSTAT)",
               s, g_ws.takes, g_ws.waits, (double)g_ws.waitNs / 1e6, (double)g_ws.waitMax / 1e3, (double)g_ws.spinNs / 1e6, g_ws.spins,
               (double)g_ws.holdNs / 1e6, g_ws.holds, (double)g_ws.holdMax / 1e3);
        memset(&g_ws, 0, sizeof g_ws);
        g_ws.t0 = now;
    }
    np_mutex_unlock(&g_statLock);
}
void np_irq_note_hold(uint64_t ns) {
    if (waitstat()) waitstat_add(2, ns);
}
int np_irq_waitstat(void) { return waitstat(); }

BOOL OSDisableInterrupts(void) {
    if (!np_os_initialized) np_os_lazy_init();
    BOOL was = !tl_disabled;
    if (!tl_disabled) {
        if (__builtin_expect(waitstat(), 0) && np_on_os_thread()) {
            if (np_mutex_trylock(&g_lock)) {
                waitstat_add(3, 0);
            } else {
                const uint64_t t0 = np_time_real_ns();
                np_mutex_lock(&g_lock);
                waitstat_add(0, np_time_real_ns() - t0);
            }
        } else {
            np_mutex_lock(&g_lock);
        }
        tl_disabled = 1;
    }
    return was;
}

BOOL OSRestoreInterrupts(BOOL level) {
    BOOL was = !tl_disabled;
    if (level) {
        if (tl_disabled && !tl_exit_owner) {
            tl_disabled = 0;
            np_mutex_unlock(&g_lock);
            if (!tl_irq) {
                // a short spin, not a sleep: the waiter was woken by the unlock above and only has to get the lock
                // (a sleep costs a scheduler tick on the Vita, every 3 ms while the audio callback is due)
                if (__builtin_expect(waitstat(), 0) && __atomic_load_n(&g_irq_waiting, __ATOMIC_RELAXED) && np_on_os_thread()) {
                    const uint64_t t0 = np_time_real_ns();
                    for (int spin = 0; spin < 4000 && __atomic_load_n(&g_irq_waiting, __ATOMIC_RELAXED); ++spin) cpu_relax();
                    waitstat_add(1, np_time_real_ns() - t0);
                } else
                for (int spin = 0; spin < 4000 && __atomic_load_n(&g_irq_waiting, __ATOMIC_RELAXED); ++spin) cpu_relax();
                np_vic_poll_hint();   // virtual time: the events that fell due while interrupts were off are taken now
                np_os_preempt_check();
            }
        }
    } else if (!tl_disabled) {
        np_mutex_lock(&g_lock);
        tl_disabled = 1;
    }
    return was;
}

BOOL OSEnableInterrupts(void) { return OSRestoreInterrupts(TRUE); }

}  // extern "C"
