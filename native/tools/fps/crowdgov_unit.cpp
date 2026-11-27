// crowd25 (MSC_CROWD_GOV, platform/compat/np_crowdgov.h): the crowd-cost governor. Synthetic loads, no game data.
//   clang++ -std=c++11 -O1 -Iplatform/compat tools/fps/crowdgov_unit.cpp -o /tmp/crowdgov_unit && /tmp/crowdgov_unit
// Mutants (-DNP_CROWDGOV_BREAK=N, the run must FAIL): 1 lanes ignored, 2 no failure hold, 3 give-back ignores its cost, 4 no minimum dwell, 5 the 60 floor holds at 30, 6 (nxr4a) Idle keeps the pending measurement, 7 (nxr4a) a period change keeps it. Every figure is synthetic (an estimate).
#include "np_crowdgov.h"
#include <stdio.h>
#include <stdlib.h>
#include <vector>
using namespace npcg;
namespace {
int g_fail = 0, g_pass = 0;
void check(bool ok, const char* what, const char* d = "") { if (ok) { ++g_pass; printf("ok   %s %s\n", what, d); } else { ++g_fail; printf("FAIL %s %s\n", what, d); } }
const double kSave[kMaxLevel + 1] = {0, 0.15, 0.29, 0.43, 0.48, 0.63};   // REAL cumulative ms saved at each level
struct Sim {
    Gov g; double now = 0, period; unsigned rnd = 12345; std::vector<double> mv; double noise = 1.0;
    explicit Sim(double p, const Config& c = Config()) : g(c), period(p) {}
    double R() { rnd = rnd * 1664525u + 1013904223u; return ((rnd >> 8) & 0xffff) / 65535.0 - 0.5; }
    void Run(double secs, double base, double lane, double gate = 0) {
        const double end = now + secs;
        while (now < end) {
            const double busy = base - kSave[g.Level()] + noise * R() * 2.0;
            if (g.Picture(now, busy, period, lane)) mv.push_back(now);
            now += period * 1e-3;
        }
    }
    int In(double a, double b) const { int n = 0; for (double t : mv) if (t >= a && t < b) ++n; return n; }
    int MaxIn(double w, double from) const { int m = 0; for (double t : mv) if (t >= from) { int n = In(t, t + w); if (n > m) m = n; } return m; }
};
}
int main() {
    // 1. CPU-bound at 60 (game 15.0 of a 14.5 budget, lanes 10): steps down, settles where it fits, bounded
    { Sim s(16.667); s.noise = 0.0; s.Run(40, 14.9, 10.0); char d[100]; snprintf(d, sizeof d, "(level %d, %zu moves)", s.g.Level(), s.mv.size());
      check(s.g.Level() >= 3 && s.g.Level() <= 4 && s.mv.size() <= 6 && s.mv.front() < 4.0, "CPU-bound load at 60 steps down within 4 s and settles", d); }
    // 2. GPU-bound: the game thread is over but a lane is longer: the crowd is not touched
    { Sim s(16.667); s.Run(60, 16.0, 24.0); char d[100]; snprintf(d, sizeof d, "(level %d, %u over by another lane)", s.g.Level(), s.g.St().gpuBound);
      check(s.g.Level() == 0 && s.mv.empty(), "GPU-bound load does not touch the crowd", d); }
    // 3. only a sustained overload reaches the bare minimum (level 5): a load that needs 0.6 ms
    { Sim s(16.667); s.noise = 0.0; s.Run(40, 14.9, 10.0); const int a = s.g.Level(); Sim t(16.667); t.noise = 0.0; t.Run(60, 15.0, 10.0);
      char d[100]; snprintf(d, sizeof d, "(14.9 ms: level %d; 15.0 ms: level %d)", a, t.g.Level());
      check(a == 4 || a == 3, "a load that level 4 or less fixes never reaches the bare minimum", d); check(t.g.Level() == 5, "a load only the bare minimum fixes reaches level 5 (after 5 steps)", d); }
    // 4. room: after the load drops the crowd comes back fast, all the way
    { Sim s(16.667); s.Run(15, 15.5, 10.0); const double t0 = s.now; s.Run(30, 9.0, 8.0); double tb = -1; for (double t : s.mv) if (t > t0) tb = t;
      char d[100]; snprintf(d, sizeof d, "(level %d, last move %.1f s after the drop)", s.g.Level(), tb - t0);
      check(s.g.Level() == 0 && tb - t0 <= 8.0, "when room returns the full crowd is back within 8 s", d); }
    // 5. a flapping load: 5 s over / 5 s light for 200 s, with the prior right, too small, and far too large
    for (int v = 0; v < 3; ++v) { Config c; for (int k = 1; k <= kMaxLevel; ++k) c.prior[k] = v == 0 ? 0.15 : (v == 1 ? 0.01 : 3.0);
      Sim s(16.667, c); for (int i = 0; i < 20; ++i) { s.Run(5, 14.95, 10.0); s.Run(5, 9.5, 8.0); }
      char d[140]; snprintf(d, sizeof d, "(prior %d: %zu moves in 200 s, %d in the worst 10 s after 40 s, %d in [40,200])", v, s.mv.size(), s.g.St().fails ? s.MaxIn(10, 40) : s.MaxIn(10, 40), s.In(40, 200));
      check(s.MaxIn(10, 40) <= 2 && s.In(40, 200) <= 10, "a flapping load moves about once per 10 s at most (wrong cost guesses included)", d); }
    // 6. steady load exactly at the edge: the give-back must price what it gives back (no flap)
    { Sim s(16.667); s.noise = 0.0; s.Run(20, 14.9, 10.0); const size_t m = s.mv.size(); s.Run(120, 14.9, 10.0); char d[100]; snprintf(d, sizeof d, "(%zu moves in the 120 s after settling, level %d)", s.mv.size() - m, s.g.Level());
      check(s.mv.size() - m <= 2, "a load that just fits at its level is left alone", d); }
    // 7. at the held 30 (budget 29 ms) a 20 ms game thread is left alone, a 33 ms one steps down
    { Sim s(33.333); s.Run(30, 20.0, 15.0); Sim t(33.333); t.Run(30, 33.0, 20.0); check(s.g.Level() == 0 && t.g.Level() >= 1, "at 30 the budget is 29 ms: 20 ms untouched, 33 ms steps down"); }
    // 8. no live pictures for a while: the window restarts, the level is kept
    { Sim s(16.667); s.noise = 0.0; s.Run(12, 15.5, 10.0); const int l = s.g.Level(); s.g.Idle(); s.now += 20; s.Run(0.5, 9.0, 8.0); check(s.g.Level() == l && l > 0, "a menu or replay in between keeps the level and the window starts over"); }
    // 9. moves are never closer than the dwell (2 s) unless half the pictures are over
    { Sim s(16.667); s.Run(30, 14.4, 10.0); double gap = 1e9; for (size_t i = 1; i < s.mv.size(); ++i) if (s.mv[i] - s.mv[i - 1] < gap) gap = s.mv[i] - s.mv[i - 1];
      char d[100]; snprintf(d, sizeof d, "(%zu moves, smallest gap %.1f s)", s.mv.size(), gap); check(s.mv.size() < 2 || gap >= 1.9, "a moderate overload moves no faster than the 2 s dwell", d); }
    // 10. why60 (MSC_CROWD_SEED60): Seed() jumps up at once, never down, learns nothing, and a seeded 60 does not walk down a level a second
    { Sim a(16.667); a.noise = 0.0; a.Run(10, 14.9, 10.0);   // walks down to its level (3 or 4)
      const int need = a.g.Level();
      Sim b(16.667); b.noise = 0.0; b.g.Seed(0.0, need); b.Run(10, 14.9, 10.0);
      const bool up = b.g.Seed(b.now, need - 1) == false && b.g.Level() == need && b.g.Seed(b.now, 5) && b.g.Level() == 5;
      Sim c(16.667); c.noise = 0.0; c.g.Seed(0.0, need); c.Run(10, 14.9, 10.0);
      char d[120]; snprintf(d, sizeof d, "(unseeded: level %d with %zu moves in 10 s; seeded: level %d, %zu moves)", need, a.mv.size(), c.g.Level(), c.mv.size());
      check(up && a.mv.size() >= 3 && c.mv.empty() && c.g.Level() == need && c.g.St().fails == 0, "Seed raises at once, never lowers, and the seeded 60 makes no walk-down moves", d); }
    // 11. core60 (MSC_CROWD_FLOOR60): at 60 live the level handed to the renderer is at least the floor, never lower than the governor's own; at 30 / outside live play it is the governor's level; floor 0 is off
    { bool ok = true;
      for (int lv = 0; lv <= 5; ++lv) for (int fl = 0; fl <= 7; ++fl) {
          const int e60 = fl > lv ? (fl > 5 ? 5 : fl) : lv;
          ok = ok && npcg::FloorAt60(lv, fl, true) == (fl > 5 && lv < 5 ? 5 : e60) && npcg::FloorAt60(lv, fl, false) == lv && npcg::FloorAt60(lv, 0, true) == lv;
      }
      check(ok, "FloorAt60: at 60 live max(governor, floor) capped at 5; at 30, in replays and with floor 0 the governor's level"); }
    // 12. nxr4a (MSC_CROWD_PEND_CLEAR): the saving of a move is learned from the first full window AFTER it against the mean BEFORE it. A replay / cut-scene (Idle) or a 60 <-> 30 change in between makes that
    //     two different scenes: save_[] must keep its prior (0.15 ms for level 1). The same loads without the interruption DO teach it (the check below proves the probe can see learning).
    {
        auto firstMove = [](Sim& s) { for (int i = 0; i < 100000 && s.mv.empty(); ++i) { const double busy = 14.9 - kSave[s.g.Level()]; if (s.g.Picture(s.now, busy, s.period, 10.0)) s.mv.push_back(s.now); s.now += s.period * 1e-3; } };
        Sim base(16.667); base.noise = 0.0; firstMove(base); base.Run(2.5, 12.5, 10.0);   // no interruption: the move is measured
        const double learned = base.g.Save(1);
        Sim a(16.667); a.noise = 0.0; firstMove(a); a.g.Idle(); a.now += 20; a.Run(2.5, 12.5, 10.0);
        Sim b(16.667); b.noise = 0.0; firstMove(b); b.period = 33.333; b.Run(2.5, 12.5, 10.0);
        char d[140]; snprintf(d, sizeof d, "(save[1]: uninterrupted %.3f, after Idle %.3f, after 60->30 %.3f, prior 0.150)", learned, a.g.Save(1), b.g.Save(1));
        auto near = [](double v) { return v > 0.14 && v < 0.16; };
        check(learned > 0.5 && near(a.g.Save(1)) && near(b.g.Save(1)), "a move's saving is not learned across a replay / cut-scene or a change of period", d);
        Config off; off.clearPend = false; Sim c(16.667, off); c.noise = 0.0; firstMove(c); c.g.Idle(); c.now += 20; c.Run(2.5, 12.5, 10.0);
        check(c.g.Save(1) > 0.5, "MSC_CROWD_PEND_CLEAR=0 is the old behaviour (it learns across the gap)");
    }
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
