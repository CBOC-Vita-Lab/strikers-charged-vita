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

static NpMutex g_lock;
static volatile int g_lock_ready = 0;
volatile int np_os_initialized = 0;
static __thread int tl_disabled = 0;
static __thread int tl_irq = 0;
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

static void lock_init_once(void) {
    if (!g_lock_ready) {
        np_mutex_init(&g_lock);
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
static __thread int tl_exit_owner = 0;   // the thread running the exit handlers: keeps "interrupts disabled" for good
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

BOOL OSDisableInterrupts(void) {
    if (!np_os_initialized) np_os_lazy_init();
    BOOL was = !tl_disabled;
    if (!tl_disabled) {
        np_mutex_lock(&g_lock);
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
