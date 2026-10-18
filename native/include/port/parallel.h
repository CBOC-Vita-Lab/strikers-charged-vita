// NP (gcdonor): helper-core job interface used by the performance patches taken from the GameCube
// port's Vita fork (robin994/strikersVita PR #1: parallel snapshot grab/blend, character pose
// preparation). The donor calls aurora::vita::parallel_for(count, grain, fn, ctx) and
// aurora::vita::worker_threads() / game_execution_lanes(); this header is the same contract under
// our names so the game code does not depend on a backend:
//
//   PortParallelWorkers()  helper threads that exist (0 = none: callers run their original serial loop)
//   PortParallelLanes()    execution lanes including the calling thread (3 on the Vita without the
//                          fourth core, 4 with it)
//   PortParallelFor(count, grain, fn, ctx)  runs fn(ctx, begin, end, lane) over [0, count) in chunks of
//                          at least `grain` items on the calling thread and the helpers, returns when
//                          all chunks are done; the chunks are disjoint and fn must only touch state
//                          owned by its items. Returns false if any chunk returned false.
//
// Without NP_HAVE_PARALLEL (host32 and armhf runs, and any build before NPV supplies the Vita
// implementation) the inline fallbacks below make PortParallelWorkers() zero, so every call site keeps
// its original serial path and nothing changes. NPV defines NP_HAVE_PARALLEL for the Vita target and
// provides the three extern "C" functions (platform/vita).
#ifndef PORT_PARALLEL_H
#define PORT_PARALLEL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*PortParallelFn)(void* ctx, size_t begin, size_t end, uint32_t lane);

#if defined(NP_HAVE_PARALLEL)
unsigned PortParallelWorkers(void);
unsigned PortParallelLanes(void);
int PortParallelFor(size_t count, size_t grain, PortParallelFn fn, void* ctx);
#else
static inline unsigned PortParallelWorkers(void) { return 0; }
static inline unsigned PortParallelLanes(void) { return 1; }
static inline int PortParallelFor(size_t count, size_t grain, PortParallelFn fn, void* ctx)
{
    (void)grain;
    return count == 0 ? 1 : fn(ctx, 0, count, 0);
}
#endif

#ifdef __cplusplus
}
#endif

#endif // PORT_PARALLEL_H
