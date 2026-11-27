// Wave 9 (pacing, coordinator C): the free-run gate (np_fps_between.h) as a player meets it: how many times it switches in a five-minute match with a cost that sits near its limits, and
// that the sticky settings (the Vita plan's) stay out of a scene the free run would give an uneven 1/2 mix in. Pure code, synthetic costs. Mutants NP_BETWEEN_BREAK 1-2 are caught.
//   g++ -std=c++11 -Inative/platform/compat native/tools/fps/between_sticky_unit.cpp
#include "np_fps_between.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <functional>

using npfps::Between;
using npfps::BetweenConfig;

static int g_fail = 0;
static void check(bool ok, const char* what) { if (!ok) { std::printf("FAIL: %s\n", what); ++g_fail; } }

static BetweenConfig Old() { return BetweenConfig(); }   // 26.7 / 30 ms, 6 s cool-down, 30 s, 20 frames
static BetweenConfig Sticky() {   // the plan: enter under 20 ms (1.2 refreshes: 70 % of the pictures one refresh), leave over 25 for 60 frames, 45 s cool-down, a try under 90 s is a failed one, at most 4 min
    BetweenConfig c;
    c.enterMs = 20.0; c.leaveMs = 25.0; c.cool = 45.0; c.coolMax = 240.0; c.fail = 90.0; c.outFrames = 60;
    return c;
}

struct Out { int flips; double onShare; bool onAtEnd; };
// A match of `seconds` at the frame length of the state (on: the cost, at least a refresh; off: 33.4 ms). cost(t): the busiest stage's ms at time t.
template <class F> static Out Play(const BetweenConfig& c, double seconds, F cost) {
    Between b(c);
    std::mt19937 rng(7);
    std::normal_distribution<double> noise(0.0, 1.0);
    double t = 0, on = 0;
    int flips = 0;
    bool was = false;
    while (t < seconds) {
        const double ms = cost(t) + 1.5 * noise(rng);
        const bool want = b.Frame(t, 1u, true, ms > 1 ? ms : 1, was ? (ms > 16.7 ? ms : 16.7) : 33.4);
        if (want != was) { ++flips; was = want; }
        if (was) on += 1;
        t += (was ? (ms > 16.7 ? ms : 16.7) : 33.4) / 1000.0;
    }
    return {flips, on / (seconds * 1000.0 / 25.0), was};
}

int main() {
    // 1. a scene at 21.5 ms (1.3 refreshes, 44 pictures a second, 64 % / 36 % ones and twos in the model): the old gate runs it free from the first second; the sticky one keeps the clean 30
    {
        auto cost = [](double) { return 21.5; };
        const Out o = Play(Old(), 300, cost), s = Play(Sticky(), 300, cost);
        std::printf("21.5 ms flat: old %d flips, on at end %d; sticky %d flips, on at end %d\n", o.flips, o.onAtEnd, s.flips, s.onAtEnd);
        check(o.onAtEnd && o.flips == 1, "the old gate enters a 21.5 ms scene and stays in it (the uneven 1/2 mix)");
        check(s.flips == 0 && !s.onAtEnd, "the sticky gate keeps a 21.5 ms scene at the clean 30");
    }
    // 2. a scene at 17.5 ms (1.05 refreshes, 54 pictures a second): both run it free
    {
        auto cost = [](double) { return 17.5; };
        const Out s = Play(Sticky(), 300, cost);
        check(s.onAtEnd && s.flips == 1, "a 17.5 ms scene runs free (one entry) under the sticky gate");
    }
    // 2b. a spike of 12 frames at 45 ms (a goal flash, a collector pause) in a 17.5 ms scene does not end the free run
    {
        auto cost = [](double t) { return (t > 100.0 && t < 100.5) ? 45.0 : 17.5; };
        const Out s = Play(Sticky(), 200, cost);
        check(s.onAtEnd && s.flips == 1, "a half-second spike does not end the free run (the way out needs 60 frames over the limit)");
    }
    // 3. a cost that wanders across the limits (a busy match: 22 +- 4 ms over 40 s, plus noise): few switches
    {
        auto cost = [](double t) { return 22.0 + 4.0 * std::sin(t * 2.0 * 3.14159265 / 40.0); };
        const Out o = Play(Old(), 300, cost), s = Play(Sticky(), 300, cost);
        BetweenConfig naive = Sticky(); naive.cool = 1.0; naive.fail = 0.0; naive.outFrames = 5; naive.leaveMs = naive.enterMs;
        const Out n = Play(naive, 300, cost);
        std::printf("22 +- 4 ms over 40 s: old %d flips, sticky %d, no hysteresis %d (5 min)\n", o.flips, s.flips, n.flips);
        check(s.flips <= 4, "the sticky gate switches at most about four times in a five-minute match whose cost wanders over both limits");
        check(n.flips > s.flips + 4, "without the cool-down, the hysteresis and the frame count it bounces much more (what the sticky rule is for)");
    }
    // 4. a load step: 15 ms for a minute, then 28 ms: the way out within three seconds of the load, and no way back while it lasts
    {
        auto cost = [](double t) { return t < 60 ? 15.0 : 28.0; };
        const Out s = Play(Sticky(), 180, cost);
        check(!s.onAtEnd && s.flips == 2, "in at 15 ms, out for good at 28 ms: two switches");
        Between b(Sticky());
        double t = 0, outAt = -1;
        bool was = false;
        for (int i = 0; i < 20000 && outAt < 0; ++i) {
            const double ms = t < 5 ? 15.0 : 28.0;
            const bool w = b.Frame(t, 1u, true, ms, was ? 16.7 : 33.4);
            if (was && !w) outAt = t;
            was = w;
            t += (was ? 17.0 : 33.4) / 1000.0;
        }
        check(outAt > 5.0 && outAt < 8.5, "the way out takes under three seconds after the cost passes the limit (60 frames over 25 ms)");
    }
    // 5. a failed try (out within the fail window) doubles the cool-down; a try that lasted does not
    {
        Between b(Sticky());
        double t = 0;
        auto drive = [&](double seconds, double ms) { const double end = t + seconds; while (t < end) { b.Frame(t, 1u, true, ms, 20.0); t += 0.02; } };
        drive(2, 15);           // in
        check(b.On(), "in after a second at 15 ms");
        drive(2, 30);           // out within 90 s: failed
        check(!b.On(), "out");
        drive(44, 15);
        check(!b.On(), "still in the 45 s cool-down after a failed try 44 s later (doubled: 90 s)");
        drive(60, 15);
        check(b.On(), "in again after the doubled cool-down");
    }
    // 6. pacing12 (r10: the gate opened for 1.7 s at Vice and then took a 72 ms frame for the stage cost): single hitches and stalls do not close the gate; a majority of slow frames does
    {
        auto run = [](double seconds, std::function<void(int, double&, double&)> shape, bool* onAtEnd) {
            Between b(Sticky());
            double t = 0; bool was = false; int i = 0;
            while (t < 3.0) { b.Frame(t, 1u, true, 15.0, was ? 17.0 : 33.4); t += 0.02; was = b.On(); }   // in
            *onAtEnd = was;
            const double end = t + seconds;
            for (; t < end; ++i) { double busy = 15.0, iv = 17.0; shape(i, busy, iv); b.Frame(t, 1u, true, busy, iv); t += iv / 1000.0; was = b.On(); }
            *onAtEnd = was;
        };
        bool on = false;
        run(30, [](int i, double& busy, double& iv) { if (i % 170 == 0) { busy = 103.6; iv = 100.0; } }, &on);
        check(on, "a 100 ms hitch with a cost of 103 ms every 3 s does not close the free run");
        {
            Between b(Sticky());
            double t = 0; bool was = false;
            while (!was && t < 10.0) { was = b.Frame(t, 1u, true, 15.0, 33.4); t += 0.0334; }   // the frame that goes in
            b.Frame(t, 1u, true, 103.6, 100.0); t += 0.1;
            b.Frame(t, 1u, true, 15.0, 17.0); t += 0.017;
            b.Frame(t, 1u, true, 103.6, 100.0); t += 0.1;
            for (int i = 0; i < 40; ++i) { b.Frame(t, 1u, true, 15.0, 17.0); t += 0.017; }
            check(was && b.On(), "two hitches in the first frames after the switch (the display mode changes there) do not close the free run");
        }
        run(30, [](int i, double& busy, double& iv) { if (i % 400 < 90) { iv = 200.0; busy = 15.0; } }, &on);
        check(on, "stalls of 200 ms are excused (80 of them in a row: not counted as slow frames)");
        run(30, [](int i, double& busy, double& iv) { if (i % 5 < 1) { iv = 33.4; busy = 15.0; } }, &on);
        check(on, "one frame in five at two refreshes (an 80 % one-refresh mix: mean 20 ms) stays free");
        run(30, [](int i, double& busy, double& iv) { if (i % 5 < 3) { iv = 33.4; busy = 15.0; } }, &on);
        check(!on, "three in five at two refreshes (the mix under 70 % one-refresh: mean 27 ms against 25) goes back to the clean 30");
        run(30, [](int i, double& busy, double& iv) { if (i % 10 < 3) { iv = 100.0; busy = 90.0; } }, &on);
        check(!on, "a hitch that is every third frame is not a hitch: it closes the gate");
    }
    std::printf("%s: between_sticky_unit\n", g_fail ? "FAILED" : "passed");
    return g_fail ? 1 : 0;
}
