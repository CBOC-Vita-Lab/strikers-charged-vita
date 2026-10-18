// Virtual time and the virtual interrupt controller (VIC).
//
// A harness run (scripts, state hashes, lock-step pair tests) must not depend on the speed of the host. With virtual time
// on (MSC_VIRTUAL_TIME=1 of the harness, np_time.h; or NP_VIRTUAL_TIME=1 for this layer alone) the clock only moves when
// the running OS thread reads it (1 us per read, np_vclock.cpp), sleeps, is charged modelled CPU time, or waits with
// nothing to run. Everything the console does "in the background" is then an EVENT with a due time on that clock instead
// of a host thread racing the game:
//
//   retrace (59.94 Hz), alarms (OSSetAlarm), the audio block (3 ms), disc and NAND completions
//
// Each module registers a source (np_vic_add) and arms it with the next due time (np_vic_arm). Events run INLINE, on the
// thread that reaches a poll point, in due-time order (ties: registration order), inside np_irq_enter/np_irq_leave like the
// console's interrupt handlers. Poll points are the places where a console takes an interrupt: when interrupts are
// re-enabled (OSRestoreInterrupts), when the thread yields or reads the time base with interrupts enabled, and when every
// thread is blocked (the idle scheduler warps the clock to the next event: a 16 ms wait for a retrace costs microseconds).
// Handlers never run while interrupts are disabled (they would take the same lock), exactly as on the console.
//
// The result is a run whose every observable (retrace counts, the order of completions, audio callbacks, time stamps) is a
// function of the game's own behaviour. Real-time mode is untouched: np_vt_on is 0 and every hook is one load and a branch.
//
// Limits: a thread that busy-waits on the clock without ever reading it, yielding or enabling interrupts hangs (the clock
// cannot move); polling APIs the game uses in loops (VIGetRetraceCount, DVDGetCommandBlockStatus, DVDGetDriveStatus) advance
// the clock a little for that reason. Network threads (platform/sdk/net) need real time and are not supported in this mode.
#include <revolution/os.h>

#include <stdlib.h>
#include <string.h>

#include "np_internal.h"

// The harness clock (platform/harness/src/np_vclock.cpp, np_time.h); weak: the SDK layer alone runs without it.
extern "C" {
int NpVirtualTimeActive(void) __attribute__((weak));
int64_t NpMonotonicNs(void) __attribute__((weak));
void NpVirtualAdvanceNs(int64_t ns) __attribute__((weak));
int64_t NpVirtualNowNs(void) __attribute__((weak));
}

extern "C" {

int np_vt_on = 0;          // 1: virtual time
uint64_t np_vt_hint = 0;   // earliest armed due time (0 = none): the cheap test of the poll points

// ---- the clock ---------------------------------------------------------------------------------
// Own clock (NP_VIRTUAL_TIME=1 without a harness): starts at 1000 s, +tick ns per read of the "game" thread (the thread that
// initialised it), sleeps and idle time advance it. With the harness the clock is theirs (same semantics).
static const int64_t kBaseNs = 1000ll * 1000000000ll;
static int64_t g_own_ns;        // virtual nanoseconds since start
static int64_t g_tick_ns = 1000;
static int g_use_harness;
static __thread int tl_vt_main;

static uint64_t own_read(void) {
    if (tl_vt_main) return (uint64_t)(kBaseNs + __atomic_fetch_add(&g_own_ns, g_tick_ns, __ATOMIC_RELAXED));
    return (uint64_t)(kBaseNs + __atomic_load_n(&g_own_ns, __ATOMIC_RELAXED));
}
static uint64_t harness_read(void) { return (uint64_t)NpMonotonicNs(); }

uint64_t np_vt_now(void) {   // peek: the clock as a read would show it, without moving it
    if (g_use_harness) return (uint64_t)NpVirtualNowNs();
    return (uint64_t)(kBaseNs + __atomic_load_n(&g_own_ns, __ATOMIC_RELAXED));
}
void np_vt_advance(uint64_t ns) {
    if (g_use_harness) NpVirtualAdvanceNs((int64_t)ns);
    else __atomic_fetch_add(&g_own_ns, (int64_t)ns, __ATOMIC_RELAXED);
}

void np_vt_init(void) {
    static int done = 0;
    if (done) return;
    done = 1;
    const char* own = getenv("NP_VIRTUAL_TIME");
    if (NpVirtualTimeActive && NpMonotonicNs && NpVirtualAdvanceNs && NpVirtualNowNs && NpVirtualTimeActive()) {
        g_use_harness = 1;
        np_vt_on = 1;
        np_time_set_source(harness_read);
        np_log("vt", "virtual time (harness clock)");
    } else if (own && *own && *own != '0') {
        const char* t = getenv("NP_VIRTUAL_TICK_NS");
        if (t && *t) g_tick_ns = strtoll(t, 0, 10);
        tl_vt_main = 1;
        np_vt_on = 1;
        np_time_set_source(own_read);
        np_log("vt", "virtual time (own clock, %lld ns per read)", (long long)g_tick_ns);
    }
}

// ---- the virtual interrupt controller ------------------------------------------------------------
#define NP_VIC_MAX 8
struct VicSrc {
    const char* name;
    NpVicRun run;
    void* ctx;
    uint64_t due;   // 0 = not armed
};
static VicSrc g_src[NP_VIC_MAX];
static int g_nsrc;
static __thread int tl_in_vic;

int np_vic_add(const char* name, NpVicRun run, void* ctx) {
    if (g_nsrc >= NP_VIC_MAX) return -1;
    g_src[g_nsrc].name = name;
    g_src[g_nsrc].run = run;
    g_src[g_nsrc].ctx = ctx;
    g_src[g_nsrc].due = 0;
    return g_nsrc++;
}

static void recompute_hint(void) {
    uint64_t best = 0;
    for (int i = 0; i < g_nsrc; ++i) {
        uint64_t d = __atomic_load_n(&g_src[i].due, __ATOMIC_RELAXED);
        if (d && (!best || d < best)) best = d;
    }
    __atomic_store_n(&np_vt_hint, best, __ATOMIC_RELAXED);
}

void np_vic_arm(int id, uint64_t due_ns) {
    if (id < 0 || id >= g_nsrc) return;
    __atomic_store_n(&g_src[id].due, due_ns, __ATOMIC_RELAXED);
    recompute_hint();
}

static int pick(uint64_t limit) {   // the earliest armed source with due <= limit (limit 0: any), -1 if none
    int best = -1;
    uint64_t bd = 0;
    for (int i = 0; i < g_nsrc; ++i) {
        uint64_t d = __atomic_load_n(&g_src[i].due, __ATOMIC_RELAXED);
        if (!d || (limit && d > limit)) continue;
        if (best < 0 || d < bd) {
            best = i;
            bd = d;
        }
    }
    return best;
}

static void run_one(int i) {
    VicSrc& s = g_src[i];
    uint64_t due = __atomic_load_n(&s.due, __ATOMIC_RELAXED);
    // the source re-arms itself (or disarms: np_vic_arm(id, 0)) from its run function
    __atomic_store_n(&s.due, 0, __ATOMIC_RELAXED);
    s.run(s.ctx, due);
    recompute_hint();
}

// Poll point: run every event that is due now. Only with interrupts enabled and outside interrupt context.
void np_vic_poll(void) {
    // only an OS thread takes the interrupts: a renderer or job thread that happens to call OSGetTime must not run the game's handlers
    if (!np_vt_on || tl_in_vic || np_in_irq() || np_irq_disabled() || !np_on_os_thread()) return;
    tl_in_vic = 1;
    for (int guard = 0; guard < 100000; ++guard) {
        int i = pick(np_vt_now());
        if (i < 0) break;
        run_one(i);
    }
    tl_in_vic = 0;
}

// The scheduler has nothing to run: warp the clock to the next event and run it. 0 when no event is armed (the caller
// then waits for real, e.g. for a network thread).
int np_vic_idle(void) {
    if (!np_vt_on || tl_in_vic) return 0;
    int i = pick(0);
    if (i < 0) return 0;
    const uint64_t now = np_vt_now();
    const uint64_t due = __atomic_load_n(&g_src[i].due, __ATOMIC_RELAXED);
    if (due > now) np_vt_advance(due - now);
    tl_in_vic = 1;
    run_one(i);
    tl_in_vic = 0;
    return 1;
}

// A polling API (VIGetRetraceCount, DVDGetCommandBlockStatus ...) was called on the running thread: a little time passes, and
// whatever became due runs.
void np_vt_poll_api(void) {
    if (!np_vt_on || !np_on_os_thread()) return;
    np_vt_advance(5000);   // 5 us
    np_vic_poll();
}

}  // extern "C"
