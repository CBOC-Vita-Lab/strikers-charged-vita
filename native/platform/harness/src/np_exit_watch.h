// np_exit_watch.h (exit17): the end of a scripted run must not be able to vanish without a word.
//
// The 23 Nov console runs d1, discardblend0, heaponly0 and tlsverify1 printed "[script] f=N exit" and nothing more: not the final [watch] line, not the end-of-run
// reports, not "[plat] script exit at frame N, exiting". The runner saw no new byte for 90 s and said "stalled". Whether the game thread hung inside Finish() (and where) or
// the process left with its log tail unwritten cannot be told from such a log. This header is the fix for the evidence:
//   - Mark() names the step Finish() is in (a counter and a pointer to a literal) and, with the trace on, writes "[exit] step N: <name>" DIRECTLY to the file (not through the
//     sink's 250 ms buffer): the last such line in a log is the step that did not finish.
//   - Run() is the watchdog thread's loop: it sleeps in slices, and when ONE step has been going for the limit (default 30 s, MSC_EXIT_WATCHDOG_S, 0 off) it writes
//     "[plat] EXIT STUCK in step <name> after N s (step k): leaving the process" directly and forces the exit. A step that advances restarts the clock.
// Test-run machinery only: Finish() runs for a script's exit, MSC_MAX_FRAMES and the lab's time limit, never for a player (a player leaves through the game's own quit path).
// Pure and header-only (no clock, no thread, no platform include): tests/test_exit_watch.cpp runs this code with a virtual clock; np_harness.cpp supplies the thread and the writers.
#pragma once
#include <atomic>
#include <cstdio>

namespace NpExitWatch {

struct State {
    std::atomic<const char*> step{"not started"};
    std::atomic<unsigned> seq{0};
    std::atomic<int> started{0};   // 0 -> 1 once: the first Begin() starts the watchdog
    std::atomic<int> finished{0};  // set when the process is about to leave normally: the watchdog stops quietly
};

// The writer takes a whole line (with its newline). Direct: bypasses every buffer.
template <class Say>
inline void Mark(State& s, const char* name, bool trace, Say say) {
#if defined(NP_EXIT_WATCH_MUTANT_NOSTEP)
    (void)s;
#else
    s.step.store(name, std::memory_order_release);
#endif
    const unsigned n = s.seq.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (trace) {
        char line[160];
        std::snprintf(line, sizeof line, "[exit] step %u: %s\n", n, name);
        say(line);
    }
}

// The watchdog's loop. sleepMs(ms) sleeps (real time); force(code) leaves the process (does not return in the real thing). Returns true when it forced the exit.
// limitMs 0: never forces (returns false at once).
template <class Sleep, class Say, class Force>
inline bool Run(State& s, unsigned limitMs, unsigned sliceMs, int code, Sleep sleepMs, Say say, Force force) {
    if (limitMs == 0 || sliceMs == 0) return false;
    unsigned lastSeq = s.seq.load(std::memory_order_acquire);
    unsigned waited = 0;
    for (;;) {
        sleepMs(sliceMs);
        if (s.finished.load(std::memory_order_acquire)) return false;
        const unsigned q = s.seq.load(std::memory_order_acquire);
#if !defined(NP_EXIT_WATCH_MUTANT_NORESET)
        if (q != lastSeq) { lastSeq = q; waited = 0; }   // a step finished: the next one gets the whole limit
#endif
        waited += sliceMs;
        if (waited >= limitMs) {
            char line[220];
            std::snprintf(line, sizeof line, "[plat] EXIT STUCK in step %s after %u s (step %u): leaving the process\n", s.step.load(std::memory_order_acquire), waited / 1000u, q);
            say(line);
#if !defined(NP_EXIT_WATCH_MUTANT_NOFORCE)
            force(code);
#endif
            return true;
        }
    }
}

} // namespace NpExitWatch
