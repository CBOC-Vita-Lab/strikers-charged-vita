// flip25_unit.cpp (wave 25, 25 Nov 2026): host proofs of the two trial-of-60 rules that cut the visible 60 <-> 50 flips (np_fps_ctl.h Config::p98Defer, default ON, a bug fix; Config::p98Calm, default OFF).
//   c++ -std=c++11 -O1 -Inative/platform/compat native/tools/fps/flip25_unit.cpp native/tools/fps/gsplit_stub.cpp -o flip25_unit && ./flip25_unit
// Mutants (-DNP_FLIP_BREAK=N, each must FAIL): 1 the defer is ignored, 2 no cap on failed trials in a scene, 3 no settle after a scene change, 4 no cost gate.
// The model replays the console's trial pattern (int9 Vice USE50, jobs 6116 / 6118 / 6122 / 6123: every trial of 60 by time failed, 0 kept of 26): the relief at 50 tries 60 after 4, 8, 16, 20 s, 60 cannot hold.
#include "np_fps_ctl.h"
#include <cstdio>
#include <cstring>
#include <vector>
using namespace npfps;
static int g_fail = 0;
static void check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) ++g_fail; }
static const double R = 1001.0 / 60000.0, Rms = R * 1000.0;

struct Sim {
    Auto ctl;
    double now = 0, sceneStart = 0;
    unsigned scene = 2;
    unsigned fails = 0;
    std::vector<double> trialStarts;
    bool deferEachFrame = false;   // np_fps.cpp calls DeferUp(now, 2) every frame while the 45 / 50 is on ITS relief (the full-size 30)
    bool p98 = true;               // (false for one picture: a scene change in a LAN-like frame starts no cut trial)
    double core0 = 10.0;           // the busiest core's cost: the model's 60 cannot hold when it is over a refresh
    explicit Sim(bool calm, bool defer) : ctl(Cfg(calm, defer)) {}
    static Config Cfg(bool calm, bool defer) { Config c = Config::Lock60Hold(); c.p98 = true; c.p98Trials = true; c.p98Calm = calm; c.p98Defer = defer; return c; }
    bool At60() const { return ctl.Current() == kPolicy60; }
    Decision Pic(double ms) {
        FrameIn f; std::memset(&f, 0, sizeof f);
        now += ms / 1000.0;
        if (deferEachFrame) ctl.DeferUp(now, 2.0);
        f.now = now; f.interval = ms / 1000.0; f.steps = 1; f.simMs = 5.0; f.costsValid = true; f.gameCore = 0;
        f.cpuMs[0] = core0; f.cpuMs[1] = 6.0; f.scene = scene; f.workMs = core0; f.feMs = 8.0; f.beMs = 8.0; f.gpuMinMs = 9.0; f.p98 = p98;
        const Decision d = ctl.Frame(f);
        if (d.switched && d.policy == kPolicy60 && std::strstr(d.reason, "wait after the last fall")) trialStarts.push_back(now);
        return d;
    }
    // the relief (a 50: pictures of 1.2 refreshes); a trial at 60 meets a load 60 cannot hold: every third picture is a repeat, so it fails
    void Run(double secs) {
        const double t0 = now; int i = 0;
        while (now - t0 < secs) { if (At60()) Pic(i++ % 3 == 2 ? 2 * Rms : Rms); else Pic(1.2 * Rms); }
    }
};

int main() {
    // ---- 1. the defer: no 60 trial by time while the 50 is on its relief
    { Sim a(false, true); a.deferEachFrame = true; a.core0 = 17.5; a.Run(40.0);
      check(a.trialStarts.empty(), "DEFER ON: with DeferUp called every frame (the 50 on its relief) no 60 trial by time starts in 40 s");
      a.deferEachFrame = false; a.Run(30.0);
      check(!a.trialStarts.empty(), "DEFER ON: once the 50 is back (no DeferUp) the trial comes"); }
    { Sim a(false, false); a.deferEachFrame = true; a.core0 = 17.5; a.Run(40.0);
      check(!a.trialStarts.empty(), "DEFER OFF (=0, as before): the trial ignores DeferUp"); }
    // ---- 2. calm OFF: the old schedule (trials keep coming, 4 / 8 / 16 / 20 s)
    { Sim a(false, true); a.core0 = 10.0; a.Run(120.0);
      std::printf("     calm OFF: %zu trials in 120 s\n", a.trialStarts.size());
      check(a.trialStarts.size() >= 4, "CALM OFF: failed trials keep coming (the old schedule: 4 or more in 120 s)"); }
    // ---- 3. calm ON: at most 2 failed trials in a row in a scene
    { Sim a(true, true); a.core0 = 10.0; a.Run(120.0);
      std::printf("     calm ON: %zu trials in 120 s in one scene\n", a.trialStarts.size());
      check(a.trialStarts.size() == 2, "CALM ON: exactly 2 failed trials by time in one scene of 120 s (then none)");
      while (a.At60()) a.Pic(2 * Rms);
      a.ctl.P98NewMatch();   // (a new match: the wait is 0, so a trial is due the moment the settle allows)
      const double t0 = a.now; a.scene = 3; a.p98 = false; a.Pic(2 * Rms); a.p98 = true; const size_t n = a.trialStarts.size(); a.Run(5.0);
      check(a.trialStarts.size() == n && a.now - t0 >= 5.0, "CALM ON: no trial within 6 s of a scene change (not 0.2 s into the kick-off)");
      a.Run(30.0);
      check(a.trialStarts.size() > n, "CALM ON: a new scene allows trials again, after the settle"); }
    // ---- 4. the cost gate: a predicted cost over 90 % of a refresh skips the trial
    { Sim a(true, true); a.core0 = 17.5; a.Run(60.0);
      check(a.trialStarts.empty(), "CALM ON: a busiest core predicted over 90 % of a refresh at 60 (17.5 ms): no trial in 60 s"); }
    // ---- 5. the cut back into play still tries 60 at once (a cut hides it)
    { Sim a(true, true); a.core0 = 10.0; a.Run(120.0); while (a.At60()) a.Pic(2 * Rms);
      unsigned tr0 = a.ctl.P98Take(a.now).trials; a.scene = 5; a.Pic(2 * Rms);
      check(a.ctl.P98Take(a.now).trials == tr0 + 1 && a.At60(), "CALM ON: a cut back into play from the relief still tries 60 at once"); }
    // ---- 6. a trial that is clean is kept and the counter resets
    { Sim a(true, true); a.core0 = 10.0; a.Run(12.0);
      for (int i = 0; i < 1200 && !(a.At60() && a.ctl.P98Take(a.now).trials >= 1); ++i) a.Pic(a.At60() ? 2 * Rms : 1.2 * Rms);
      for (int i = 0; i < 900; ++i) a.Pic(Rms);
      std::printf("     at60 %d ok %u fail %u\n", a.At60() ? 1 : 0, a.ctl.P98Take(a.now).ok, a.ctl.P98Take(a.now).fail);
      check(a.At60(), "CALM ON: a clean trial still keeps 60"); }
    std::printf(g_fail ? "flip25_unit: FAILED\n" : "flip25_unit: all checks passed\n");
    return g_fail ? 1 : 0;
}
