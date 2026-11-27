// Host test of np_exit_watch.h (exit17): the step marks and the watchdog loop, with a virtual clock (sleep advances it, and runs the "game thread's" scheduled steps).
// Build/run: run_exit_watch_test.sh. Mutants (-DNP_EXIT_WATCH_MUTANT_NOSTEP / _NORESET / _NOFORCE) must fail it.
#include "../src/np_exit_watch.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

using NpExitWatch::State;

struct World {
    State s;
    unsigned now = 0;
    std::vector<std::pair<unsigned, std::function<void()>>> events;   // virtual time -> what the game thread does then
    std::string said;
    int forced = -1;
    void SleepMs(unsigned ms) {
        now += ms;
        for (auto& e : events) if (e.second && e.first <= now) { auto f = e.second; e.second = nullptr; f(); }
        if (now > 600000) { std::puts("test runaway"); assert(false); }
    }
    void Say(const char* l) { said += l; }
    void Force(int c) { forced = c; }
};
static bool RunWatch(World& w, unsigned limitMs, unsigned sliceMs = 250) {
    return NpExitWatch::Run(w.s, limitMs, sliceMs, 3, [&](unsigned ms) { w.SleepMs(ms); }, [&](const char* l) { w.Say(l); }, [&](int c) { w.Force(c); });
}
static void Mark(World& w, const char* name, bool trace = true) { NpExitWatch::Mark(w.s, name, trace, [&](const char* l) { w.Say(l); }); }

int main() {
    // 1. a normal end: steps every 100 ms, then the process is about to leave; no alarm
    {
        World w;
        Mark(w, "script flush");
        const char* steps[] = {"frame-send join", "exit hooks", "memory report", "plat line"};
        unsigned t = 0;
        for (const char* n : steps) { t += 100; w.events.push_back({t, [&w, n] { Mark(w, n); }}); }
        w.events.push_back({t + 100, [&w] { w.s.finished.store(1); }});
        assert(!RunWatch(w, 2000));
        assert(w.forced == -1);
        assert(w.said.find("EXIT STUCK") == std::string::npos);
        assert(w.said.find("[exit] step 1: script flush") != std::string::npos);
        assert(w.said.find("[exit] step 5: plat line") != std::string::npos);
    }
    // 2. a step that never ends: named, with the step count and the time, exit forced with the code
    {
        World w;
        Mark(w, "script flush");
        w.events.push_back({100, [&w] { Mark(w, "frame-send join"); }});   // ...and it never finishes
        assert(RunWatch(w, 3000));
        assert(w.forced == 3);
        assert(w.said.find("[plat] EXIT STUCK in step frame-send join after ") != std::string::npos);
        assert(w.said.find("(step 2)") != std::string::npos);
        assert(w.now >= 3000 && w.now <= 3250 + 250);   // limit after the LAST step began (the step began at t=100)
    }
    // 3. every step is shorter than the limit but together they are longer: no alarm (the clock restarts at every step)
    {
        World w;
        Mark(w, "s0");
        for (int i = 1; i <= 6; ++i) { const unsigned at = i * 700; w.events.push_back({at, [&w, i] { char b[16]; std::snprintf(b, sizeof b, "s%d", i); static char keep[8][16]; std::strcpy(keep[i], b); Mark(w, keep[i]); }}); }
        w.events.push_back({6 * 700 + 800, [&w] { w.s.finished.store(1); }});
        assert(!RunWatch(w, 1000));
        assert(w.forced == -1);
    }
    // 4. limit 0: the watchdog is off
    {
        World w;
        assert(!RunWatch(w, 0));
        assert(w.forced == -1 && w.now == 0);
    }
    // 5. trace off: no step lines, but the step is still tracked for the alarm
    {
        World w;
        Mark(w, "quiet step", false);
        assert(w.said.empty());
        assert(RunWatch(w, 1000));
        assert(w.said.find("EXIT STUCK in step quiet step") != std::string::npos);
    }
    std::puts("test_exit_watch: ok");
    return 0;
}
