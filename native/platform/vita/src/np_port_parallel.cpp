/* np_port_parallel.cpp: the game's helper-core job interface (include/port/parallel.h, NPD's performance patches) on top of the job
 * pool (np_jobs.cpp). The game is compiled with -DNP_HAVE_PARALLEL=1 (np/Makefile NP_PARALLEL=1) and calls PortParallelFor() from
 * RenderSnapshot::Grab / Blend; with MSC_JOBS=1 (or one core) PortParallelWorkers() is 0 and the callers keep their original serial loops.
 * The declarations are repeated here (extern "C", same signatures) so that this file needs none of the game's headers. */
#include <stddef.h>
#include <stdint.h>

#include "np_jobs.h"

extern "C" {
typedef int (*PortParallelFn)(void* ctx, size_t begin, size_t end, uint32_t lane);

unsigned PortParallelLanes(void) { return NpJobsLanes(); }
unsigned PortParallelWorkers(void) { const uint32_t l = NpJobsLanes(); return l > 1 ? l - 1 : 0; }
}

namespace {
struct Trampoline {
    PortParallelFn fn;
    void* ctx;
    volatile int failed;   /* chunks run on several lanes; any store of 1 is enough (benign race on an int flag) */
};
void RunRange(void* c, size_t begin, size_t end, uint32_t lane) {
    Trampoline* t = static_cast<Trampoline*>(c);
    if (!t->fn(t->ctx, begin, end, lane)) t->failed = 1;
}
} // namespace

extern "C" int PortParallelFor(size_t count, size_t grain, PortParallelFn fn, void* ctx) {
    if (count == 0) return 1;
    Trampoline t = { fn, ctx, 0 };
    NpParallelFor(count, grain, RunRange, &t);
    return t.failed ? 0 : 1;
}
