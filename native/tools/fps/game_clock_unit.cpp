// pacing12: the game clock (np_game_clock.h) judged by the speed meter (np_speed.h). NP_CLOCK_BREAK 1-5 must fail.
// Model: a frame of real time `real` ms asks the clock for the game time to advance by; the simulation runs floor(acc / 20 ms) fixed steps; the meter counts them against the wall clock
// exactly as the game does (np_fps.cpp SpeedFrame). Thread costs: 11 ms a picture, 8 ms a step (r9g SPLIT.md: 11 and about 8).
#include "np_game_clock.h"
#include "np_speed.h"
#include "np_slip.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <functional>
#include <algorithm>
using namespace npfps;
static int g_fail = 0;
static void check(bool ok, const char* w) { if (!ok) { std::printf("FAIL: %s\n", w); ++g_fail; } }

struct Out { double pct = 0, worstWin = 100; unsigned maxSteps = 0, maxCap = 0, entries = 0, windows = 0, under = 0; double maxDebt = 0, shed = 0, forgiven = 0; bool overloadAtEnd = false; double overloadFirstS = -1; };
// real(i): the length of picture i in ms. useOld: the wave 9 rule (stateless cut + budget of 30 ms refilling at 2 %, two-step cap, task clamp 100 ms)
static Out run(std::function<double(int)> real, int n, bool useOld, double perPic = 11.0, double stepCost = 8.0, double targetMs = 33.4) {
    GameClock clk; SpeedMeter meter; SlipBudget bucket; bucket.Configure(0.030f, 0.02f);
    ClockConfig cc;
    double acc = 0, t = 0, seen = targetMs; uint64_t steps = 0; Out o; double adv = 0;
    for (int i = 0; i < n; ++i) {
        const double r = real(i);
        t += r * 0.001;
        if (meter.Frame(t, steps)) { const double p = meter.win().Percent(); if (p < o.worstWin) o.worstWin = p; }
        seen += (r - seen) / 8.0;
        if (useOld) {
            float dt = (float)(r * 0.001); if (dt > 0.1f) dt = 0.1f;
            const float limit = 0.0333f * 1.05f;
            const float oldAdv = SlipAdvance(dt, limit, 20, 2, nullptr);
            adv = bucket.Frame(dt, dt - oldAdv) * 1000.0;
        } else {
            const unsigned cap = CapSteps(cc, targetMs, seen, perPic, stepCost);
            adv = clk.Frame(r, cap);
            if (cap > o.maxCap) o.maxCap = cap;
            if (clk.DebtMs() > o.maxDebt) o.maxDebt = clk.DebtMs();
            if (clk.Overload() && o.overloadFirstS < 0) o.overloadFirstS = t;
        }
        acc += adv;
        unsigned k = 0;
        while (acc >= 20.0 - 1e-9) { acc -= 20.0; ++k; }
        steps += k;
        if (k > o.maxSteps) o.maxSteps = k;
    }
    o.pct = meter.match().Percent();
    o.windows = meter.windows(); o.under = meter.underRule();
    o.entries = clk.OverloadEntries(); o.shed = clk.ShedMs(); o.forgiven = clk.ForgivenMs(); o.overloadAtEnd = clk.Overload();
    return o;
}
int main() {
    const int N = 1800;
    // steady pictures of 25, 45 and 70 ms (a GPU-bound or hold-heavy scene): the game runs at real time
    for (double ms : {25.0, 45.0, 70.0}) {
        Out nw = run([&](int) { return ms; }, N * 33 / (int)ms + 200, false);
        Out ol = run([&](int) { return ms; }, N * 33 / (int)ms + 200, true);
        char w[160];
        std::printf("%2.0f ms pictures: debt clock %.2f %% (worst 5 s window %.1f %%, max %u steps in a picture, cap %u) | wave 9 budget %.2f %% (worst window %.1f %%)\n", ms, nw.pct, nw.worstWin, nw.maxSteps, nw.maxCap, ol.pct, ol.worstWin);
        std::snprintf(w, sizeof w, "%.0f ms pictures: match speed >= 99.5 %%", ms); check(nw.pct >= 99.5, w);
        std::snprintf(w, sizeof w, "%.0f ms pictures: no 5 s window under 98 %%", ms); check(nw.worstWin >= 98.0, w);
        std::snprintf(w, sizeof w, "%.0f ms pictures: never more steps in a picture than the cap", ms); check(nw.maxSteps <= nw.maxCap, w);
        std::snprintf(w, sizeof w, "%.0f ms pictures: no overload signalled", ms); check(nw.entries == 0, w);
    }
    {   // model check: the wave 9 rule holds a 45 ms scene at its floor (98 %) and a 70 ms scene near it, never better (that is what the owner sees on a clean match)
        Out ol = run([&](int) { return 45.0; }, 1200, true);
        check(ol.pct < 98.6, "model check: the wave 9 budget reads about 98 % on a clean 45 ms scene (its refill floor)");
    }
    // pacing13: the pair at 60 (0, 1 or 2 steps a picture): 16.7 ms pictures, a pair with every light picture late (16.7 / 33.4), and a hitch of 100 ms every 2 s over the pair
    {
        Out a = run([&](int) { return 16.7; }, 6000, false, 8.0, 8.0, 16.7);
        Out b = run([&](int i) { return i % 2 ? 33.4 : 16.7; }, 4000, false, 8.0, 8.0, 25.0);
        Out c = run([&](int i) { return i % 120 == 119 ? 100.0 : 16.7; }, 6000, false, 8.0, 8.0, 16.7);
        std::printf("pair at 60: steady %.2f %% (max %u steps), late light pictures %.2f %% (max %u), a hitch every 2 s %.2f %% (worst window %.1f %%)\n", a.pct, a.maxSteps, b.pct, b.maxSteps, c.pct, c.worstWin);
        check(a.pct >= 99.5 && a.maxSteps <= 2, "60 fps: steady 16.7 ms pictures: speed >= 99.5 %, never over 2 steps in a picture");
        check(b.pct >= 99.5 && b.maxSteps <= 3, "60 fps: a pair whose light picture is late: speed >= 99.5 %");
        check(c.pct >= 99.3 && c.worstWin >= 97.9, "60 fps: a 100 ms hitch every 2 s: speed >= 99.3 %, no window under 98 %");
    }
    // a hitch of 100 ms every 2 s over 25 ms pictures: owed, paid back, no window under 98 %
    {
        Out nw = run([&](int i) { return i % 80 == 79 ? 100.0 : 25.0; }, 2400, false);
        std::printf("100 ms hitch every 2 s over 25 ms pictures: %.2f %% (worst window %.1f %%, max debt %.0f ms)\n", nw.pct, nw.worstWin, nw.maxDebt);
        check(nw.pct >= 99.3, "hitches of 100 ms: match speed >= 99.3 %");
        check(nw.worstWin >= 97.9, "hitches of 100 ms: no 5 s window under 98 %");
        check(nw.maxDebt <= 100.0 + 1e-6, "the debt never passes its bound");
        check(nw.shed < 1.0, "a 100 ms hitch is owed and repaid, nothing is shed");
    }
    // a 600 ms stall every 10 s over 33 ms pictures: forgiven (not owed), no overload, the debt stays at its bound or less
    {
        Out nw = run([&](int i) { return i % 300 == 299 ? 600.0 : 33.4; }, 1500, false);
        std::printf("600 ms stall every 10 s over 33 ms pictures: %.2f %% (forgiven %.0f ms, shed %.0f ms, overload entries %u)\n", nw.pct, nw.forgiven, nw.shed, nw.entries);
        check(nw.forgiven > 1500.0, "stalls are forgiven (counted, not owed)");
        check(nw.entries == 0 && nw.shed < 1.0, "a stall is not an overload and sheds nothing");
        check(nw.maxDebt <= 100.0 + 1e-6, "the debt stays bounded across stalls");
    }
    // the back-pressure wait (np_fps_bp.h): a picture waits 0-3 refreshes before the pad read; the waits are real time and are not lost
    {
        Out nw = run([&](int i) { const int k = (i * 7 + i / 5) % 10; return 33.4 + (k == 0 ? 33.4 : (k == 1 ? 16.7 : 0.0)); }, 2400, false);
        std::printf("33 ms pictures with back-pressure waits of 0-2 refreshes now and then: %.2f %% (worst window %.1f %%)\n", nw.pct, nw.worstWin);
        check(nw.pct >= 99.5 && nw.worstWin >= 98.0, "back-pressure waits cost no game time");
    }
    // a mix of 16.7 / 33.4 / 50 ms pictures (a held 30 that sometimes holds three refreshes)
    {
        Out nw = run([&](int i) { const int k = i % 6; return k == 5 ? 50.1 : (k == 2 ? 16.7 : 33.4); }, 2400, false);
        std::printf("mixed 17/33/50 ms pictures: %.2f %% (worst window %.1f %%)\n", nw.pct, nw.worstWin);
        check(nw.pct >= 99.5 && nw.worstWin >= 98.0, "a hold mix runs at real time");
    }
    // a steady overload: the game thread itself is over budget (25 ms a picture, 12 ms a step; pictures of 45 ms): the clock sheds, honestly, signals it within a second or two and stays bounded
    {
        Out nw = run([&](int) { return 45.0; }, 1200, false, 25.0, 12.0);
        std::printf("overloaded game thread (25 + 12/step, 45 ms pictures): %.2f %% of real time, shed %.0f ms, overload signalled at %.1f s, max debt %.0f ms\n", nw.pct, nw.shed, nw.overloadFirstS, nw.maxDebt);
        check(nw.entries >= 1 && nw.overloadAtEnd, "a steady overload is signalled");
        check(nw.overloadFirstS > 0 && nw.overloadFirstS <= 2.5, "... within 2.5 s");
        check(nw.maxDebt <= 100.0 + 1e-6, "... and the debt stays bounded");
        check(nw.pct < 96.0, "model check: the overload really is slow (the clock does not hide it)");
    }
    std::printf("%s: game_clock_unit\n", g_fail ? "FAILED" : "passed");
    return g_fail ? 1 : 0;
}
