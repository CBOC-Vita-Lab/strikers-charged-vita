/* np_jobs.h: the helper-core job pool of the native port (platform/vita/src/np_jobs.cpp).
 *
 * The performance architecture (np/docs/vita.md, "Core plan") runs the game's frame on core 0 and
 * lends the other cores short fork-join jobs: pose blending and skinning of the ten characters,
 * culling, render-snapshot grab/blend, anything that is "the same work over N independent items".
 * The pattern is strikersVita PR #1's (aurora::vita::parallel_for), reimplemented here so that it
 * works unchanged on host32, armhf (qemu) and the Vita and can be PROVEN safe:
 *
 *   NpParallelFor(count, minPerLane, fn, ctx)   the calling thread is lane 0 and takes its share
 *                                               of [0, count); the workers take the rest; it
 *                                               returns when every item is done.
 *
 * Rules for a job function (the contract the proof tools rely on):
 *   - it reads only data that nobody writes during the call and writes only items of its own range
 *     (or private scratch): no allocation (nlMalloc/new), no logging, no game-event dispatch, no
 *     global counters, no static locals, no calls into the SDK layer;
 *   - it must not depend on the order in which chunks run (the tools below shuffle them);
 *   - it must be idempotent (the verifier runs it twice).
 * Proof tools (environment, all off by default; env.txt on the Vita):
 *   MSC_JOBS=N            lanes: 1 = serial (no workers), 2 = one worker, 3 = two (default on the Vita)
 *   MSC_JOBS_ORDER=reverse|shuffle|interleave   chunk order / assignment; results must not change
 *   MSC_JOBS_VERIFY=1     NpParallelForChecked runs the job in parallel, hashes its outputs with the
 *                         caller's hash function, runs it again serially and compares: a mismatch
 *                         is logged ("[jobs] MISMATCH ...") and counted
 *   MSC_JOBS_CORES=<set>  cores of the workers, e.g. "12" (default: workers on core 2 then 1)
 *   MSC_JOBS_PRIO=<n>     worker thread priority (Vita; default: just below audio output, above render)
 *   MSC_JOBS_SPIN_US=<n>  how long an idle worker polls before it sleeps (default 200)
 * Statistics: NpJobsLogStats() / the harness's end-of-run report.
 */
#ifndef NP_JOBS_H
#define NP_JOBS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*NpRangeFn)(void* ctx, size_t begin, size_t end, uint32_t lane);
typedef uint64_t (*NpHashFn)(void* ctx);   /* hash of everything the job wrote */

#define NP_JOBS_MAX_LANES 4

/* Starts the workers (idempotent). `lanes` = 0 takes MSC_JOBS or the default (3 on the Vita and on
 * armhf/host with at least 3 cores, else 1). Returns the number of lanes in use (1 = serial). */
uint32_t NpJobsInit(uint32_t lanes);
void NpJobsShutdown(void);
uint32_t NpJobsLanes(void);
/* 1 while the calling thread is inside a job function (game code may assert on it). */
int NpInJob(void);

/* Fork-join over [0, count); `minPerLane` items at least per lane before a second lane is used.
 * Serial fallback (same function, one call with lane 0) when the pool is off, busy (called from a
 * job or concurrently from another thread) or the job is too small. Returns the number of lanes used. */
uint32_t NpParallelFor(size_t count, size_t minPerLane, NpRangeFn fn, void* ctx);
/* The same, with the verifier (MSC_JOBS_VERIFY=1) around it; `name` appears in the log. */
uint32_t NpParallelForChecked(const char* name, size_t count, size_t minPerLane, NpRangeFn fn, void* ctx,
                              NpHashFn outputHash, void* hashCtx);

typedef struct NpJobsStats {
    uint64_t calls, parallelCalls, serialFallbacks, busyFallbacks;
    uint64_t verifyRuns, verifyMismatches;
    uint64_t lateStarts;              /* workers woken for a job that was already finished: not waited for */
    uint64_t laneItems[NP_JOBS_MAX_LANES];
    uint64_t laneBusyUs[NP_JOBS_MAX_LANES];
    uint64_t callerWaitUs;            /* time lane 0 waited for the workers after its own share */
    uint64_t wakeLatencyUsTotal;      /* sum over parallel calls of (worker start - fork) of the slowest starter */
    uint32_t wakeLatencyUsMax;
    uint32_t lanes;
} NpJobsStats;
void NpJobsGetStats(NpJobsStats* out);
void NpJobsLogStats(const char* when);

#ifdef __cplusplus
}
#endif
#endif
