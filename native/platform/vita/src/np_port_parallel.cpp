/* np_port_parallel.cpp: the game's helper-core job interface (include/port/parallel.h, NPD's performance patches) on top of the job
 * pool (np_jobs.cpp). The game is compiled with -DNP_HAVE_PARALLEL=1 (np/Makefile NP_PARALLEL=1) and calls PortParallelFor() from
 * RenderSnapshot::Grab / Blend; with MSC_JOBS=1 (or one core) PortParallelWorkers() is 0 and the callers keep their original serial loops.
 * The declarations are repeated here (extern "C", same signatures) so that this file needs none of the game's headers. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "np_jobs.h"

// Wave 9 (simthread): the Vita build compiled the game WITHOUT NP_HAVE_PARALLEL until 1.3 wave 9, so the snapshot sites never ran on the console
// ("[jobs] ... 1 calls" in every console log). The Makefile now defines it for TARGET=vita; each site has its own run-time switch, default 0 = the
// original serial loop, exactly as before (PortParallelSite):
//   site 0 MSC_SNAP_PAR       RenderSnapshot::Grab (end of every simulation step) and ::Blend (every picture): the ten characters and 150 power-ups;
//   site 1 MSC_CROWD_POS_PAR  ImpostorManager::UpdatePositions (every picture): the crowd sprites' view matrices, one crowd character per item.
// Every item writes only its own object, so the results are the same bit for bit (host proof in w8/simthread.md: frame hashes and the det trace with 0
// and 1, and the proof mutants MSC_SNAP_PAR_BREAK / MSC_CROWD_POS_PAR_BREAK that the hashes must catch).
namespace {
int Knob(const char* name) {
    const char* e = getenv(name);
    return (e != nullptr && *e != 0 && *e != '0') ? 1 : 0;
}
uint32_t Lanes() {
    static int started = 0;
    if (!started) {
        started = 1;
        NpJobsInit(0);   // idempotent: the Vita platform has started the pool already; the host harness has not (host: MSC_JOBS=3 for proofs)
    }
    return NpJobsLanes();
}
} // namespace

extern "C" {
typedef int (*PortParallelFn)(void* ctx, size_t begin, size_t end, uint32_t lane);

unsigned PortParallelLanes(void) { return Lanes(); }
unsigned PortParallelWorkers(void) { const uint32_t l = Lanes(); return l > 1 ? l - 1 : 0; }

int PortParallelSite(int site) {
    static int v[2] = {-1, -1};
    if (site < 0 || site > 1) return 0;
    if (v[site] < 0) {
        static const char* const kNames[2] = {"MSC_SNAP_PAR", "MSC_CROWD_POS_PAR"};
        v[site] = Knob(kNames[site]);
        fprintf(stderr, "[snappar] %s=%d (%u lane(s))\n", kNames[site], v[site], Lanes());
    }
    return v[site];
}
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
