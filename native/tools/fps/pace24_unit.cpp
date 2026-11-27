// pace24_unit.cpp (wave 24, 24 Nov 2026): host proofs of the frame-rate controller changes of helper pace24. The REAL np_fps_ctl.h Auto, np_fps45.h Fallback45 and np_game_clock.h, fed by small models.
//   c++ -std=c++11 -O1 -Inative/platform/compat native/tools/fps/pace24_unit.cpp native/tools/fps/gsplit_stub.cpp -o pace24_unit && ./pace24_unit
// Switches under test (all default OFF; every test also checks the OFF side is the old behaviour):
//   MSC_LOCK60_FASTGRACE  Config::holdFastGrace  - the 2 s fast window does not fall 60 inside the cut-in's grace unless the delivered rate collapses (FIX-03)
//   MSC_HOLD60_WALLSIM    Config::holdWallSim    - the climb prices the steps in wall ms like the frame (FIX-02)
//   MSC_F45_LATEONLY      Fallback45Config::rateWindow - the 45's fallback hears late pictures and the delivered rate, not planned holds (FIX-04)
//   MSC_LOCK60_SPREAD_CAP Spread60Cap debtCapMs  - the 60's catch-up spread takes a debt of two steps (X7)
// Mutants (-DNP_P24_BREAK=N, each must FAIL): 1 a collapse never falls inside the grace, 2 the fast grace does nothing, 3 the wall sim priced with CPU ms, 4 the rate rule never speaks, 5 the rate
// rule is inverted; -DNP_SPREAD_BREAK=2 the spread's debt cap is ignored.
#include "np_fps_ctl.h"
#include "np_fps45.h"
#include "np_game_clock.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
using namespace npfps;

static int g_fail = 0;
static void check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) ++g_fail; }
static const double R = 1001.0 / 60000.0, Rms = R * 1000.0;

// ---- FIX-03: the cut-in's grace and the 2 s window ---------------------------------------------------------------------------------------------------------------------------------------
struct Cut {
    Auto ctl;
    double now = 0;
    explicit Cut(bool grace) : ctl(Cfg(grace)) {}
    static Config Cfg(bool grace) { Config c = Config::Lock60Hold(); c.holdFastGrace = grace; return c; }
    void Pic(double ms) {
        FrameIn f; std::memset(&f, 0, sizeof f);
        now += ms / 1000.0;
        f.now = now; f.interval = ms / 1000.0; f.steps = 1; f.simMs = 5.0; f.costsValid = true; f.gameCore = 0;
        f.cpuMs[0] = 10.0; f.cpuMs[1] = 6.0; f.scene = 2; f.workMs = 14.0; f.feMs = 8.0; f.beMs = 8.0; f.gpuMinMs = 9.0;
        ctl.Frame(f);
    }
    // steady pictures of `ms` for `secs` after the scene start; the time 60 fell at, or -1
    double Steady(double ms, double secs) { const double t0 = now; Pic(Rms); while (now - t0 < secs) { Pic(ms); if (ctl.Current() != kPolicy60) return now - t0; } return -1.0; }
    // a cut into play: 1.2 s of 56 ms pictures (the console's cut-in: 25-56 ms), then 0.8 s of refresh-long pictures, then steady; returns the time 60 fell at (-1: it did not within `total` s)
    double Run(double burstSecs, double burstMs, double total) {
        const double t0 = now;
        Pic(Rms);   // the first picture of the scene: 60 is entered
        while (now - t0 < burstSecs) Pic(burstMs);
        while (now - t0 < total) { Pic(Rms); if (ctl.Current() != kPolicy60) return now - t0; }
        return -1.0;
    }
};

static void TestFastGrace() {
    // the transient: 1.2 s of 56 ms pictures (21) and 0.8 s at 60 (48) = 69 pictures in the 2 s window = 34.5 a second, under the 36 of the fast window
    { Cut off(false); const double t = off.Run(1.2, 56.0, 8.0); check(t > 1.9 && t < 2.2 && off.ctl.Current() != kPolicy60, "OFF: the 2 s window falls 60 at about 2.0 s after a heavy cut-in (the bug of the handoff, to-do 7)"); }
    { Cut on(true);  const double t = on.Run(1.2, 56.0, 8.0);  check(t < 0.0 && on.ctl.Current() == kPolicy60, "ON: the same cut-in keeps 60 (34.5 a second is not a collapse inside the grace)"); }
    // a collapse inside the grace still falls: 2 s of 70 ms pictures is 14 a second
    { Cut on(true);  const double t = on.Run(4.0, 70.0, 8.0);  check(t > 1.9 && t < 4.5 && on.ctl.Current() != kPolicy60, "ON: a collapse (14 a second) inside the grace still falls at the 2 s window"); }
    // after the grace the old rules are back: steady 40 ms pictures (25 a second) fall on the slow window once the 6 s grace is over
    { Cut on(true);  const double t = on.Steady(40.0, 30.0); check(t > 6.0 && t < 13.0 && on.ctl.Current() != kPolicy60, "ON: 25 a second does not escape the slow window after the grace (falls between 6 and 13 s)"); }
    // the steady 60 with the transient over is untouched on both sides
    { Cut on(true), off(false); on.Run(0.3, 40.0, 10.0); off.Run(0.3, 40.0, 10.0); check(on.ctl.Current() == kPolicy60 && off.ctl.Current() == kPolicy60, "a 0.3 s burst then steady 60: kept on both sides"); }
}

// ---- FIX-02: the climb's step priced in the frame's own clock ----------------------------------------------------------------------------------------------------------------------------
struct Climb {
    Auto ctl;
    double now = 0;
    explicit Climb(bool wall) : ctl(Cfg(wall)) {}
    static Config Cfg(bool wall) { Config c = Config::Lock60Hold(); c.holdWallSim = wall; return c; }
    // a picture at the paced 30: 2 steps of 5 ms CPU, each blocked/preempted `stall` ms more in wall time, 8 ms of the picture's own work
    void Pic(double stall, double ms = 33.37, bool heavy = false) {
        FrameIn f; std::memset(&f, 0, sizeof f);
        now += ms / 1000.0;
        const double cpuSim = 10.0, wallSim = 10.0 + 2.0 * stall, own = heavy ? 40.0 : 8.0;
        f.now = now; f.interval = ms / 1000.0; f.steps = 2; f.simMs = cpuSim; f.stepWallMs = wallSim; f.costsValid = true; f.gameCore = 0;
        f.cpuMs[0] = own + cpuSim; f.cpuMs[1] = 8.0; f.scene = 2; f.workMs = own + wallSim; f.feMs = 8.0; f.beMs = 8.0; f.gpuMinMs = 9.0;
        ctl.Frame(f);
    }
};
static void TestWallSim() {
    // heavy pictures until 60 falls to the relief (every picture 50 ms: the delivered rate is 20), then the load eases: 8 ms own work, 2 steps of 5 ms CPU + 2.5 ms stall each
    auto run = [](bool wall, double* cost, double* climbAt) {
        Climb c(wall);
        c.Pic(0.0, 16.68);   // scene start at 60
        for (int i = 0; i < 400 && c.ctl.Current() == kPolicy60; ++i) c.Pic(0.0, 50.0, true);
        const double fell = c.now;
        *climbAt = -1.0;
        for (int i = 0; i < 6000; ++i) { c.Pic(2.5); if (c.ctl.Current() == kPolicy60) { *climbAt = c.now - fell; break; } }
        *cost = c.ctl.HoldUpCost();
        return c.ctl.Current() == kPolicy60;
    };
    double costOff = 0, costOn = 0, atOff = 0, atOn = 0;
    const bool upOff = run(false, &costOff, &atOff), upOn = run(true, &costOn, &atOn);
    std::printf("     climb cost at 60 for 8 ms of work, 2 x (5 ms CPU + 2.5 ms stall) in the steps: CPU domain %.1f ms, wall domain %.1f ms; climb after %.1f s (CPU) / %.1f s (wall)\n", costOff, costOn, atOff, atOn);
    check(costOff > 0.97 * Rms && !upOff, "OFF (CPU ms in the steps, wall ms in the frame): the stall inside the steps lands in the base: cost over 97 % of a refresh, no climb");
    check(costOn > 0 && costOn < 0.97 * Rms && upOn, "ON (both in wall ms): the same picture costs 8 + 7.5 = 15.5 ms at one step and the 60 comes back");
    // with no stall in the steps the two clocks agree: the switch changes nothing
    { Climb a(false), b(true); for (int i = 0; i < 40; ++i) { a.Pic(0.0); b.Pic(0.0); } check(std::fabs(a.ctl.HoldUpCost() - b.ctl.HoldUpCost()) < 1e-9, "no stall inside the steps: both clocks give the same cost"); }
    // a wait OUTSIDE the steps (the blend lane's join, the frame-send join) stays in the cost on both sides: 8 ms of work + a 5 ms wait outside the steps
    { Climb a(false), b(true);
      a.Pic(0.0, 16.68); b.Pic(0.0, 16.68);
      for (int i = 0; i < 400 && a.ctl.Current() == kPolicy60; ++i) a.Pic(0.0, 50.0, true);
      for (int i = 0; i < 400 && b.ctl.Current() == kPolicy60; ++i) b.Pic(0.0, 50.0, true);
      a.now = b.now = std::max(a.now, b.now);
      for (int i = 0; i < 40; ++i) { FrameIn f; std::memset(&f, 0, sizeof f); a.now += 0.03337; f.now = a.now; f.interval = 0.03337; f.steps = 2; f.simMs = 10.0; f.stepWallMs = 10.0; f.costsValid = true; f.cpuMs[0] = 18.0; f.cpuMs[1] = 8.0; f.scene = 2; f.workMs = 8.0 + 5.0 + 10.0; f.feMs = 8.0; f.beMs = 8.0; f.gpuMinMs = 9.0; f.laneWaitMs = 5.0; a.ctl.Frame(f); b.now = a.now; f.now = b.now; b.ctl.Frame(f); }
      check(std::fabs(a.ctl.HoldUpCost() - 18.0) < 0.6 && std::fabs(b.ctl.HoldUpCost() - 18.0) < 0.6, "a wait outside the steps stays in the cost on both sides (8 + 5 + one 5 ms step = 18 ms: the hold60b rule)"); }
}

// ---- FIX-04: planned holds are not slips ------------------------------------------------------------------------------------------------------------------------------------------------
struct Seq {
    Fallback45 fall;
    double now = 0;
    unsigned falls = 0;
    explicit Seq(bool on) : fall(Cfg(on)) {}
    static Fallback45Config Cfg(bool on) { Fallback45Config c = Fallback45Config::Hold(); if (on) { c.rateWindow = 135; c.rateMin = 37.0; } return c; }
    // one picture: late = it missed its slot; planned = it was planned for a later slot by its game time (held 2 refreshes); the OFF side hears both as a slip, as np_fps.cpp did
    void Pic(bool on, bool late, bool planned, unsigned hold) {
        now += hold * Rms / 1000.0;
        if (fall.Picture45(on ? late : (late || planned), now, on ? hold : 0u)) ++falls;
    }
};
static void TestLateOnly() {
    // The Dump 5214: 300 pictures, 13 late, 71 planned holds (84 slips of 300 = 28 %); the delivered pictures: 13 late (2 refreshes) + 71 planned (2 refreshes) + 216 on their slot (1 refresh) = 300 pictures in 300 + 84 refreshes = 46.9 a second
    auto run = [](bool on, int late, int planned, int total) {
        Seq s(on);
        for (int i = 0; i < total; ++i) {
            const bool l = i % (total / (late ? late : 1)) == 0 && late > 0 && i / (total / late) < late;
            const bool p = !l && planned > 0 && i % (total / planned) == 1 && i / (total / planned) < planned;
            s.Pic(on, l, p, (l || p) ? 2u : 1u);
        }
        return s.falls;
    };
    check(run(false, 13, 71, 300) > 0, "OFF: 13 late + 71 planned holds in 300 pictures falls to 30 (the console's 5214)");
    check(run(true, 13, 71, 300) == 0, "ON: the same 300 pictures (46.9 a second delivered, 13 late) stay at 45");
    check(run(true, 80, 0, 300) > 0, "ON: 80 late pictures in 300 (27 %) still fall (the late window)");
    { Seq s(true); for (int i = 0; i < 90; ++i) s.Pic(true, false, false, 1u);   // (the settle)
      for (int i = 0; i < 90; ++i) { const bool l = i % 4 == 0 && i < 80; s.Pic(true, l, false, l ? 2u : 1u); }
      check(s.falls > 0, "ON: 20 late in 90 (22 %, at 49 a second delivered) falls on the late window"); }
    // planned holds alone that really slow the pictures down: every picture held 2 refreshes (30 a second) is delivered under 37: the rate rule falls
    { Seq s(true); for (int i = 0; i < 400; ++i) s.Pic(true, false, true, 2u); check(s.falls > 0, "ON: planned holds that deliver 30 a second (every picture held twice) fall on the delivered rate"); }
    // 40 a second delivered by planned holds (a 1,1,2,1,2 pattern: 5 pictures in 8 refreshes = 37.5): kept
    { Seq s(true); for (int i = 0; i < 600; ++i) s.Pic(true, false, i % 5 == 2 || i % 5 == 4, (i % 5 == 2 || i % 5 == 4) ? 2u : 1u); check(s.falls == 0, "ON: 5 pictures in 8 refreshes (37.5 a second, no late picture) is kept at 45"); }
    // the rate rule is silent without a hold figure (the callers before pace24)
    { Seq s(true); for (int i = 0; i < 400; ++i) { s.now += 2 * Rms / 1000.0; if (s.fall.Picture45(false, s.now)) ++s.falls; } check(s.falls == 0, "no hold figure: the rate rule does not speak"); }
}

// ---- X7: the spread's debt cap ---------------------------------------------------------------------------------------------------------------------------------------------------------------
struct Spread { unsigned twoStep = 0, maxSteps = 0; double debtEnd = 0, shed = 0, pct = 0, maxDebt = 0; };
static Spread RunSpread(double capMs, int lates, double lateMs = 50.0) {
    ClockConfig cc;
    GameClock gc(cc);
    Spread r;
    double acc = 0;
    for (int i = 0; i < 400; ++i) {
        const bool late = i >= 20 && i < 20 + lates;
        const double real = late ? lateMs : Rms;
        const unsigned cap = Spread60Cap(CapSteps(cc, Rms, Rms, 9.0, 5.0), gc.DebtMs(), cc, capMs);
        const double adv = gc.Frame(real, cap);
        acc += adv;
        const unsigned steps = (unsigned)std::floor(acc / 20.0 + 1e-9);
        acc -= steps * 20.0;
        if (steps >= 2) ++r.twoStep;
        if (steps > r.maxSteps) r.maxSteps = steps;
        if (gc.DebtMs() > r.maxDebt) r.maxDebt = gc.DebtMs();
    }
    r.debtEnd = gc.DebtMs(); r.shed = gc.ShedMs(); r.pct = gc.TotalPercent();
    return r;
}
static void TestSpreadCap() {
    // one 3-refresh hitch (50 ms): 30 ms owed, over the 20 of today's cap: the next picture is given two steps (the spiral of console s50); with 40 it is one step a picture
    const Spread a = RunSpread(0.0, 1), b = RunSpread(40.0, 1);
    std::printf("     one 50 ms picture at 60: cap 20: %u two-step pictures (max debt %.0f ms), cap 40: %u (max debt %.0f ms)\n", a.twoStep, a.maxDebt, b.twoStep, b.maxDebt);
    check(a.twoStep >= 1, "debt cap 20 (today): one 50 ms hitch leaves 30 ms owed and the next picture runs two steps");
    check(b.twoStep == 0 && b.maxSteps == 1, "debt cap 40: the same hitch is repaid one step a picture (no two-step picture)");
    check(b.debtEnd < 1e-6 && b.shed == 0.0 && a.shed == 0.0, "the debt is repaid to zero and nothing is shed: no slow motion, no game time lost (cap 40)");
    check(b.pct > 99.9 && b.pct < 100.1, "game time = real time over the run (cap 40)");
    // two 33 ms pictures in a row (two single repeated refreshes): 26.7 ms owed
    const Spread c = RunSpread(0.0, 2, 2.0 * Rms), d = RunSpread(40.0, 2, 2.0 * Rms);
    check(c.twoStep >= 1 && d.twoStep == 0, "two repeated refreshes in a row: two-step pictures at cap 20, none at cap 40");
    // one repeated refresh (hold60b's own case): identical
    const Spread e = RunSpread(0.0, 1, 2.0 * Rms), f = RunSpread(40.0, 1, 2.0 * Rms);
    check(e.twoStep == 0 && f.twoStep == 0 && std::fabs(e.maxDebt - f.maxDebt) < 1e-9, "one repeated refresh: the two caps are identical (the spread of hold60b)");
    // two 50 ms hitches in a row owe 60 ms: over both caps, the ordinary cap takes over, the debt stays bounded and is repaid
    const Spread g = RunSpread(40.0, 2);
    check(g.maxDebt <= ClockConfig().maxDebtMs + 1e-9 && g.twoStep > 0 && g.debtEnd < 1e-6 && g.shed == 0.0, "two 50 ms hitches in a row (60 ms owed) are over both caps: ordinary cap, debt bounded, repaid, nothing shed");
}

int main() {
    TestFastGrace();
    TestWallSim();
    TestLateOnly();
    TestSpreadCap();
    std::printf(g_fail ? "pace24 unit: %d FAILED\n" : "pace24 unit: all passed (%d failed)\n", g_fail);
    return g_fail ? 1 : 0;
}
