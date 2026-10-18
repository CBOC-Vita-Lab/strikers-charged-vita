// np_internal.h: declarations shared by the files of platform/sdk/src. Plain C (usable from the
// C files adapted from the decomp's SDK and from the C++ ones); includes no Wii SDK header.
#ifndef NP_INTERNAL_H
#define NP_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include "np/np_plat.h"
#include "np/np_sdk.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- the renderer's glue (platform/gx, NPR), weak: absent in builds without it (np_gx.h of that module).
// The SDK layer tells it which memory the game lives in (np_gx_register_ram), when data the GPU may have cached
// was written (cache flushes and disc reads: np_gx_notify_write) and which EFB size VIConfigure selected.
void np_gx_register_ram(const void* base, unsigned int size) __attribute__((weak));
void np_gx_notify_write(const void* p, unsigned int n) __attribute__((weak));
void np_gx_set_efb_size(int fb_width, int efb_height) __attribute__((weak));
static inline void np_notify_write(const void* p, unsigned long n) {
    if (np_gx_notify_write && n) np_gx_notify_write(p, (unsigned int)n);
}

// ---- os_interrupt.cpp: "interrupts" are a global lock. Code that the real console runs in an
// interrupt handler (alarm callbacks, the AX frame callback, disc and NAND completion callbacks,
// retrace callbacks) runs on a helper thread between np_irq_enter() and np_irq_leave(); the game's
// OSDisableInterrupts() takes the same lock, so the two exclude each other as on the console.
void np_irq_enter(void);
void np_exit_quiesce(void);       // atexit handler (NpSdk_Init): handler threads stop running game code
void np_irq_leave(void);
int np_in_irq(void);              // the calling thread is between enter and leave
int np_irq_disabled(void);        // the calling thread holds the lock
void np_irq_release_for_wait(int* saved); // blocks of the scheduler: drop the lock while parked
void np_irq_reacquire_after_wait(int saved);
void np_os_lazy_init(void);
extern volatile int np_os_initialized;

// ---- os_virtual.cpp: virtual time and the virtual interrupt controller (see the header comment of that file).
// np_vt_on is 1 when the clock is virtual: the helper threads of the real-time mode are replaced by VIC sources whose
// handlers run inline at poll points. np_vt_hint = the earliest armed due time (0 = none).
typedef void (*NpVicRun)(void* ctx, uint64_t due_ns);
extern int np_vt_on;
extern uint64_t np_vt_hint;
void np_vt_init(void);                      // decides the mode and installs the clock source (first OS call)
uint64_t np_vt_now(void);                   // the clock without moving it
void np_vt_advance(uint64_t ns);            // moves the clock
int np_vic_add(const char* name, NpVicRun run, void* ctx);   // a source; returns its id
void np_vic_arm(int id, uint64_t due_ns);   // its next due time on the np_time_ns clock; 0 = off. Re-arm from run().
void np_vic_poll(void);                     // run what is due (interrupts enabled, not in interrupt context)
int np_vic_idle(void);                      // nothing runnable: warp to the next event and run it; 0 if none armed
void np_vt_poll_api(void);                  // a polling API was called: a little time passes, then np_vic_poll
// The cheap form for the hot paths (OSRestoreInterrupts, OSGetTime ...)
static inline void np_vic_poll_hint(void) {
    if (np_vt_on && np_vt_hint && np_vt_now() >= np_vt_hint) np_vic_poll();
}

uint32_t NpVI_GetRetraceCount(void);         // vi_native.cpp: VIGetRetraceCount without polling

// ---- os_thread.c
void np_thread_hint_resched(void);        // an interrupt context made a thread ready
int np_sched_idle_poke(void);             // wake an idle scheduler host thread (call with the lock)
void np_os_threads_init(void);            // sets up the default thread (called by OSInit)
void np_os_preempt_check(void);           // after interrupts were re-enabled: reschedule if needed
int np_on_os_thread(void);                // the calling host thread is an OSThread (not a helper / renderer / job thread)

// ---- os_alarm.cpp
void np_alarm_init(void);

// ---- os_time.cpp
uint64_t np_os_ticks(void);               // 60.75 MHz time base, includes the RTC offset
uint64_t np_os_ticks_from_ns(uint64_t ns);
uint64_t np_os_ns_from_ticks(uint64_t ticks);   // inverse, on the np_time_ns clock
void np_time_init(void);

// ---- os_core.cpp
void np_os_arena_init(void);

// logging shorthand
#define NP_LOG(tag, ...) np_log(tag, __VA_ARGS__)

#ifdef __cplusplus
}
#endif
#endif
