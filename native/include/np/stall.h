// 1.3 MSC_STALL_TIMERS (stall10): step timers for the game thread's long frames at a scene change (match start, goal cuts, replays). Diagnostic, changes no
// picture and no simulation. Header-only (C++11), no allocation, one relaxed load when off.
//
//   MSC_STALL_TIMERS=0|1   (code default 0 = off; the Vita plan turns it on in LAB builds only, vita_plan.cpp kLab)
//   MSC_STALL_MS=<ms>      a scope that took this long or longer prints a line (default 12)
//
// NP_STALL("name")            times the rest of the block
// NP_STALL_ARG("name", n)     the same with a number (a loading script function index, a task state)
// A line: `[stall] d<depth> <name> [<arg>] <ms> ms (state 0x<task state>, task frame <n>)`; depth 0 is the outermost scope open when it ended. Lines print when a scope ENDS, so an
// inner step comes before the step that holds it. `[stall] sum <why>: ...` (NpStallSum) prints the counters: scopes timed, lines printed, longest, total over the threshold.
// The clock is CLOCK_MONOTONIC: on a host run under virtual time or valgrind the milliseconds are host estimates; the names and counts are what counts.
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

namespace np {
namespace stall {

struct Counters
{
    unsigned timed;      // scopes that ran with the timers on
    unsigned printed;    // scopes at or over the threshold
    unsigned depth;      // open scopes now (game thread only)
    unsigned long long overUs;   // total of the printed scopes at depth 0
    unsigned long long maxUs;
    const char* maxName;
};

inline Counters& C()
{
    static Counters c = {0, 0, 0, 0, 0, 0};
    return c;
}

inline int On()
{
    static int w = -1;   // (constant-initialised: no guard, no barrier; <atomic> is not used because it must not come before NL/nlMemory.h's operator new)
    int v = __atomic_load_n(&w, __ATOMIC_RELAXED);
    if (v < 0)
    {
        const char* e = getenv("MSC_STALL_TIMERS");
        v = (e != 0 && *e != 0 && atoi(e) != 0) ? 1 : 0;
        __atomic_store_n(&w, v, __ATOMIC_RELAXED);
    }
    return v;
}

inline unsigned long long ThresholdUs()
{
    static int w = -1;
    int v = __atomic_load_n(&w, __ATOMIC_RELAXED);
    if (v < 0)
    {
        const char* e = getenv("MSC_STALL_MS");
        v = (e != 0 && *e != 0) ? atoi(e) * 1000 : 12000;
        __atomic_store_n(&w, v, __ATOMIC_RELAXED);
    }
    return (unsigned long long)v;
}

inline double WallS()   // CLOCK_REALTIME seconds: the same clock as the [cadence] lines' t=
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

inline unsigned long long NowUs()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000ull + (unsigned long long)(ts.tv_nsec / 1000);
}

// the task state and frame, for the line (set by nlTaskManager::RunAllTasks each picture)
inline unsigned& State() { static unsigned s = 0; return s; }
inline unsigned& Frame() { static unsigned f = 0; return f; }

struct Scope
{
    const char* name;
    const char* const* comment;   // (optional) a loading comment pointer, printed when the scope ends
    long arg;
    unsigned long long t0;
    unsigned depth;
    bool live;
    Scope(const char* n, long a = -1, const char* const* cm = 0) : name(n), comment(cm), arg(a), t0(0), depth(0), live(On() != 0)
    {
        if (live)
        {
            Counters& c = C();
            depth = c.depth++;
            t0 = NowUs();
        }
    }
    ~Scope()
    {
        if (!live) return;
        const unsigned long long us = NowUs() - t0;
        Counters& c = C();
        if (c.depth > 0) --c.depth;
        ++c.timed;
        if (us < ThresholdUs()) return;
        ++c.printed;
        if (depth == 0) c.overUs += us;
        if (us > c.maxUs) { c.maxUs = us; c.maxName = name; }
        if (arg >= 0 && comment != 0 && *comment != 0)
            fprintf(stderr, "[stall] d%u %s %ld \"%s\" %.1f ms (state 0x%x, task frame %u, t=%.3f)\n", depth, name, arg, *comment, us / 1000.0, State(), Frame(), WallS());
        else if (arg >= 0)
            fprintf(stderr, "[stall] d%u %s %ld %.1f ms (state 0x%x, task frame %u, t=%.3f)\n", depth, name, arg, us / 1000.0, State(), Frame(), WallS());
        else
            fprintf(stderr, "[stall] d%u %s %.1f ms (state 0x%x, task frame %u, t=%.3f)\n", depth, name, us / 1000.0, State(), Frame(), WallS());
    }
};

inline void Sum(const char* why)
{
    if (!On()) return;
    Counters& c = C();
    fprintf(stderr, "[stall] sum %s: %u scopes timed, %u at or over %llu ms, outermost total %.1f ms, longest %.1f ms (%s) (state 0x%x, task frame %u)\n", why, c.timed, c.printed,
            ThresholdUs() / 1000ull, c.overUs / 1000.0, c.maxUs / 1000.0, c.maxName ? c.maxName : "-", State(), Frame());
}

}   // namespace stall
}   // namespace np

#define NP_STALL_CAT2(a, b) a##b
#define NP_STALL_CAT(a, b) NP_STALL_CAT2(a, b)
#define NP_STALL(name) ::np::stall::Scope NP_STALL_CAT(npStallScope_, __LINE__)(name)
#define NP_STALL_ARG(name, n) ::np::stall::Scope NP_STALL_CAT(npStallScope_, __LINE__)(name, (long)(n))
#define NP_STALL_CM(name, n, cmp) ::np::stall::Scope NP_STALL_CAT(npStallScope_, __LINE__)(name, (long)(n), (cmp))
