/* np_time.h: the one clock of the native port (platform/harness).
 *
 * The SDK layer (platform/sdk: OSGetTime, OSGetTick, OSSleepTicks, alarms, VI pacing) and any
 * other platform code reads time ONLY through these four functions. With the real clock they are
 * thin wrappers over clock_gettime (Linux) or sceKernelGetSystemTimeWide (Vita). With
 * MSC_VIRTUAL_TIME=1 (set in the environment, or in ux0:data/mscharged/env.txt on the Vita) they
 * switch to the deterministic virtual clock of the harness (platform/harness/src/np_vclock.cpp):
 *   - the clock starts at a fixed instant (monotonic 1000 s, wall clock 2026-01-01 12:00 UTC),
 *   - every read on the game thread advances it by MSC_VIRTUAL_TICK_NS (default 1000 ns),
 *   - a sleep on the game thread advances it instead of sleeping,
 *   - MSC_VIRTUAL_FRAME_US / MSC_VIRTUAL_STEP_US charge modelled CPU time per presented frame /
 *     per simulation step (see the harness header comment),
 * so a run is bit-for-bit repeatable and independent of the speed of the host (x86-64, qemu-arm).
 *
 * On Linux (host32, armhf) the same file also interposes the libc clock functions
 * (clock_gettime, nanosleep, clock_nanosleep, time, gettimeofday and their time64 names), as the
 * recompiled build's harness did, so that code that reads libc directly (libstdc++'s
 * steady_clock, a donor file) sees the same virtual time. The Vita has no such interposition:
 * everything that must be deterministic there has to call the functions below.
 */
#ifndef NP_TIME_H
#define NP_TIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Monotonic nanoseconds since an arbitrary start. Any thread. */
int64_t NpMonotonicNs(void);
/* Wall clock, nanoseconds since 1970-01-01 UTC. Any thread. */
int64_t NpRealtimeNs(void);
/* Sleep at least `ns` nanoseconds (virtual mode, game thread: advance the clock instead). */
void NpSleepNs(int64_t ns);
/* 1 when the virtual clock is in use. */
int NpVirtualTimeActive(void);
/* Virtual mode only: advance the clock by `ns` (the harness charges modelled CPU time with it). */
void NpVirtualAdvanceNs(int64_t ns);
/* Virtual mode only: the monotonic clock as NpMonotonicNs() would show it now, without advancing it (the SDK layer's
 * virtual interrupt controller compares event times with it). 0 with the real clock. */
int64_t NpVirtualNowNs(void);
/* Real monotonic seconds, never virtual (harness timing lines). */
double NpRealSeconds(void);
/* Virtual seconds since the start of the run (0 with the real clock). */
double NpVirtualSeconds(void);
/* Number of clock reads on the game thread so far (virtual mode; 0 otherwise). */
uint64_t NpVirtualReads(void);

#ifdef __cplusplus
}
#endif
#endif
