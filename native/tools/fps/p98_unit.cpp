// p98_unit.cpp (wave 24, 24 Nov 2026): host proofs of MSC_LOCK60_P98 (60 held on what the player sees) and MSC_LOCK60_TRIAL (the way back by trial). The REAL np_fps_ctl.h Auto and np_fps45.h Fallback45.
//   c++ -std=c++11 -O1 -Inative/platform/compat native/tools/fps/p98_unit.cpp native/tools/fps/gsplit_stub.cpp -o p98_unit && ./p98_unit
// Mutants (-DNP_P98_BREAK=N, each must FAIL): 1 a burst falls 60 at once (no sustain), 2 60 never falls, 3 the visible-hitch rule is ignored, 4 a trial never keeps its result, 5 the backoff never grows,
// 6 no trial at a cut, 7 a GPU-bound relief is retried, 8 USE45 is ignored (the 45 is always allowed).
#include "np_fps_ctl.h"
#include "np_fps45.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
using namespace npfps;
static int g_fail = 0;
static void check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) ++g_fail; }
static const double R = 1001.0 / 60000.0, Rms = R * 1000.0;

struct Sim {
    Auto ctl;
    double now = 0;
    unsigned scene = 2;
    double gpu = 9.0;
    bool p98 = true;
    std::vector<double> trialStarts;   // times 60 was entered by a trial
    explicit Sim(bool on, bool trials) : ctl(Cfg(on, trials)) {}
    static Config Cfg(bool on, bool trials) { Config c = Config::Lock60Hold(); c.p98 = on; c.p98Trials = on && trials; return c; }
    // one picture: ms long; busy: the game core over the budget (a spike's load); returns the decision
    Decision Pic(double ms, bool busy = false, double work = 12.0) {
        FrameIn f; std::memset(&f, 0, sizeof f);
        now += ms / 1000.0;
        f.now = now; f.interval = ms / 1000.0; f.steps = 1; f.simMs = 5.0; f.costsValid = true; f.gameCore = 0;
        f.cpuMs[0] = busy ? 17.5 : 10.0; f.cpuMs[1] = 6.0; f.scene = scene; f.workMs = work; f.feMs = 8.0; f.beMs = 8.0; f.gpuMinMs = gpu; f.p98 = p98;
        const Decision d = ctl.Frame(f);
        if (d.switched && d.policy == kPolicy60 && std::strstr(d.reason, "trial")) trialStarts.push_back(now);
        return d;
    }
    bool At60() const { return ctl.Current() == kPolicy60; }
    // steady 60 (or the relief's 30 when not at 60) for secs
    void Steady(double secs, bool busy = false) { const double t0 = now; while (now - t0 < secs) Pic(At60() ? Rms : 2 * Rms, busy, At60() ? 12.0 : 20.0); }
    // a load mix: of three pictures two take a refresh and one two (40 a second, 1 repeat in 4 refreshes); returns when 60 fell (seconds from the start), -1 when it did not
    double Mix(double secs, bool busy = true) {
        const double t0 = now; int i = 0;
        while (now - t0 < secs) { Pic(i++ % 3 == 2 ? 2 * Rms : Rms, busy); if (!At60()) return now - t0; }
        return -1.0;
    }
    // at the relief: the load is light; pictures at the held 30; stop after secs or when a trial started
    void Relief(double secs, bool stopAtTrial = true) { const double t0 = now; const size_t n = trialStarts.size(); while (now - t0 < secs && !(stopAtTrial && trialStarts.size() > n)) Pic(2 * Rms, false, 20.0); }
};

int main() {
    // ---- a 2 s spike in a steady 60
    { Sim off(false, false); off.Steady(8.0); off.Steady(20.0); const double t = off.Mix(2.0, true); off.Steady(3.0, false);
      std::printf("     OFF: spike of 2 s (40 a second, core over budget) fell 60 after %.2f s\n", t);
      check(t > 0.0 && !off.At60(), "OFF (the old rules): a 2 s spike with a core over the budget takes 60 away (the owner's complaint)"); }
    { Sim on(true, false); on.Steady(8.0); on.Steady(20.0); const double t = on.Mix(2.0, true); on.Steady(20.0);
      check(t < 0.0 && on.At60(), "ON: the same 2 s spike keeps 60 (and 20 s later it is still at 60)"); }
    // two spikes 4 s apart still keep it (the sustain timer decays between them)
    { Sim on(true, false); on.Steady(8.0); on.Steady(20.0); const double t1 = on.Mix(2.0); on.Steady(14.0); const double t2 = on.Mix(2.0);
      check(t1 < 0.0 && t2 < 0.0 && on.At60(), "ON: two 2 s spikes 14 s apart keep 60"); }
    // ---- a sustained overload falls
    { Sim on(true, false); on.Steady(8.0); on.Steady(10.0); const double t = on.Mix(20.0, true);
      std::printf("     ON: sustained overload (40 a second) fell 60 after %.2f s\n", t);
      check(t > 2.5 && t < 9.0 && !on.At60(), "ON: a sustained overload falls to the relief (45) after about the sustain span"); }
    // a 2 s spike followed by a second one 4 s later is 4 s of bad pictures in 10 s: that is a real problem and it falls
    { Sim on(true, false); on.Steady(8.0); on.Steady(20.0); on.Mix(2.0); on.Steady(4.0); double t2 = on.Mix(2.0); on.Steady(6.0);
      check(!on.At60(), "ON: two 2 s spikes 4 s apart (4 s of bad pictures in 10 s) fall"); (void)t2; }
    // a hitch (3 refreshes held) every 3 s is 1 % repeats but more than one hitch per 5 s: the hitch rule alone
    { Sim on(true, false); on.Steady(8.0); const double t0 = on.now; int n = 0; bool fell = false;
      while (on.now - t0 < 60.0 && !fell) { on.Pic(Rms); if (++n % 180 == 0) on.Pic(3 * Rms); fell = !on.At60(); }
      check(fell, "ON: a hitch (2 repeats in a row) every 3 s (1 % repeats) falls by the hitch rule alone"); }
    // ---- the hitch rule: isolated repeats under 1.5 % never fall; a burst of hitches does
    { Sim on(true, false); on.Steady(8.0); const double t0 = on.now; int n = 0; bool fell = false;
      while (on.now - t0 < 60.0 && !fell) { on.Pic(Rms); if (++n % 100 == 0) on.Pic(2 * Rms); fell = !on.At60(); }   // 1 repeat in 100 pictures: 1 %, one every 1.7 s
      check(!fell, "ON: isolated repeats at 1 % (one every 1.7 s, none in a row) keep 60 for 60 s"); }
    { Sim on(true, false); on.Steady(8.0); const double t0 = on.now; int n = 0; bool fell = false;
      while (on.now - t0 < 60.0 && !fell) { on.Pic(Rms); if (++n % 30 == 0) on.Pic(3 * Rms); fell = !on.At60(); }   // a double repeat (a visible hitch) every 0.5 s
      check(fell, "ON: a visible hitch (a picture held 3 refreshes) every 0.5 s falls (the hitch rule: once per 5 s)"); }
    // ---- the catastrophic guard stays: 14 a second falls at the fast window
    { Sim on(true, false); on.Steady(8.0); on.Steady(10.0); const double t0 = on.now; bool fell = false; while (on.now - t0 < 6.0 && !fell) { on.Pic(70.0, true); fell = !on.At60(); }
      check(fell && on.now - t0 < 3.0, "ON: a collapse (14 a second) still falls within about 2 s"); }
    // ---- trials
    { Sim on(true, true); on.Steady(8.0); on.Steady(10.0); on.Mix(20.0, true); check(!on.At60(), "(setup) fell to the relief");
      const double t0 = on.now; on.Relief(10.0);
      check(on.trialStarts.size() == 1 && on.trialStarts[0] - t0 > 3.0 && on.trialStarts[0] - t0 < 5.5, "trials ON: the relief tries 60 after about 4 s");
      for (int i = 0; i < 400 && on.At60(); ++i) on.Pic(Rms);   // a clean trial
      on.Steady(6.0);
      check(on.At60() && on.ctl.P98Take(on.now).ok == 1, "a trial that is clean keeps 60 (counted), and 6 s later it is still at 60"); }
    { Sim on(true, true); on.Steady(8.0); on.Steady(10.0); on.Mix(20.0, true);
      std::vector<double> gaps;
      for (int k = 0; k < 6; ++k) {   // every trial meets the same overload and fails; the relief waits between
          const double failAt0 = on.now; const size_t n = on.trialStarts.size();
          on.Relief(40.0);
          if (on.trialStarts.size() == n) break;
          gaps.push_back(on.trialStarts.back() - failAt0);
          const double s = on.now; int i = 0; while (on.now - s < 6.0 && on.At60()) on.Pic(i++ % 3 == 2 ? 2 * Rms : Rms, true);
      }
      std::printf("     waits before the trials:"); for (size_t i = 0; i < gaps.size(); ++i) std::printf(" %.1f", gaps[i]); std::printf(" s\n");
      check(gaps.size() >= 5 && gaps[0] < 4.5 && gaps[1] > 7.0 && gaps[1] < 9.5 && gaps[2] > 14.0 && gaps[2] < 17.5 && gaps[3] > 18.0 && gaps[3] <= 21.0 && gaps[4] <= 21.0, "failed trials: the wait is 4, then doubles 8, 16 and stops at 20 s");
      check(on.ctl.P98Take(on.now).fail >= 5, "failed trials are counted"); }
    // a cut triggers a trial at once
    { Sim on(true, true); on.Steady(8.0); on.Steady(10.0); on.Mix(20.0, true); on.Relief(1.0, false);
      const unsigned tr0 = on.ctl.P98Take(on.now).trials; on.scene = 3; on.Pic(2 * Rms);
      check(on.At60() && on.ctl.P98Take(on.now).trials == tr0 + 1, "a cut back into play from the relief tries 60 at once (not after 4 s)"); }
    // the same without trials: the cut goes to 60 as it always did only when not stuck; with trials it does at once while stuck
    // GPU-bound: the GPU's smallest picture over the budget: no trial, no 60 at a cut, in 120 s
    { Sim on(true, true); on.Steady(8.0); on.Steady(10.0); on.Mix(20.0, true); on.gpu = 22.0; const size_t n = on.trialStarts.size(); unsigned t = on.ctl.P98Take(on.now).trials;
      for (int c = 0; c < 6; ++c) { on.Relief(20.0, false); on.scene = 10 + c; on.Pic(2 * Rms); }
      check(on.trialStarts.size() == n && on.ctl.P98Take(on.now).trials == t && !on.At60(), "GPU-bound (22 ms at the smallest picture): no trial in 120 s and none at six cuts, the relief stays"); }
    // LAN / online (FrameIn::p98 false): no trial, the old rules
    { Sim on(true, true); on.Steady(8.0); on.Steady(10.0); on.Mix(20.0, true); on.p98 = false; const size_t n = on.trialStarts.size(); on.Relief(30.0, false);
      check(on.trialStarts.size() == n, "LAN / online pictures (p98 off in the frame): no trial"); }

    // ---- Fallback45: the 30 -> 45 trial
    {
        auto cfg = [](bool trial) { Fallback45Config c = Fallback45Config::Hold(); if (trial) { c.trialAfter = 4.0; c.trialSecs = 2.0; c.trialMax = 20.0; } return c; };
        auto fallDown = [](Fallback45& f, double& now) { for (int i = 0; i < 2000 && f.At45(); ++i) { now += 0.0222; f.Picture45(i % 2 == 0, now); } };
        // OFF: the 30 stays (hold 10 s + a proof that a loaded scene never passes); ON: a trial after 4 s
        { Fallback45 f(cfg(true)); double now = 1000; fallDown(f, now); check(!f.At45(), "(setup) the 45 fell to the 30");
          const double t0 = now; double back = -1;
          for (int i = 0; i < 1500 && back < 0; ++i) { now += 0.0333; if (f.Picture30(false, now, 25.0, true)) back = now - t0; }
          check(back > 3.5 && back < 5.0 && f.TrialOn(), "30 -> 45: a trial starts after about 4 s (the proof never passes at 25 ms a picture)");
          for (int i = 0; i < 200 && f.TrialOn(); ++i) { now += 0.0222; f.Picture45(false, now); }
          check(f.At45() && f.TrialsOk() == 1 && f.TrialsFailed() == 0, "a clean trial keeps the 45"); }
        { Fallback45 f(cfg(true)); double now = 1000; fallDown(f, now);
          std::vector<double> gaps;
          for (int k = 0; k < 5; ++k) {
              const double t0 = now; double back = -1;
              for (int i = 0; i < 2000 && back < 0; ++i) { now += 0.0333; if (f.Picture30(false, now, 25.0, true)) back = now - t0; }
              if (back < 0) break;
              gaps.push_back(back);
              for (int i = 0; i < 400 && f.TrialOn(); ++i) { now += 0.0222; f.Picture45(i % 2 == 0, now); }   // a failing trial
          }
          std::printf("     30 -> 45 waits:"); for (size_t i = 0; i < gaps.size(); ++i) std::printf(" %.1f", gaps[i]); std::printf(" s\n");
          check(gaps.size() >= 4 && gaps[0] < 4.5 && gaps[1] > 7.0 && gaps[1] < 9.5 && gaps[2] > 14.0 && gaps[2] < 17.5 && gaps[3] > 18.0 && gaps[3] <= 21.0 && f.TrialsFailed() >= 4, "30 -> 45 failed trials: the wait doubles 4, 8, 16 and stops at 20 s"); }
        // GPU-bound: hwOk false: no trial, ever
        { Fallback45 f(cfg(true)); double now = 1000; fallDown(f, now); bool any = false;
          for (int i = 0; i < 5000 && !any; ++i) { now += 0.0333; any = f.Picture30(false, now, 25.0, false); if (i % 600 == 599) f.Pause(); }
          check(!any && f.Trials() == 0, "30 -> 45 GPU-bound (hwOk false): no trial in 160 s, none at cuts"); }
        // a cut triggers one at once
        { Fallback45 f(cfg(true)); double now = 1000; fallDown(f, now); for (int i = 0; i < 20; ++i) { now += 0.0333; f.Picture30(false, now, 25.0, true); }
          f.Pause(); now += 1.0; const bool back = f.Picture30(false, now, 25.0, true); check(back && f.TrialOn() && f.Trials() == 1, "30 -> 45: a cut back into play tries the 45 at once"); }
        // OFF: nothing changes
        { Fallback45 f(cfg(false)); double now = 1000; fallDown(f, now); bool any = false; for (int i = 0; i < 3000 && !any; ++i) { now += 0.0333; any = f.Picture30(false, now, 25.0, true); }
          check(!any && f.Trials() == 0, "OFF: the 30 with a loaded proof never returns (as the console showed)"); }
    }

    // ---- MSC_LOCK60_USE45: the ladder is 60 <-> 30 only unless the flag is on
    {
        // today's rule (before p98): live = livePlay && At45; 45 allowed by the tiers
        bool same = true, never = true; int enters = 0;
        for (int lp = 0; lp < 2; ++lp) for (int a = 0; a < 2; ++a) for (int m = 0; m < 2; ++m) for (int u = 0; u < 2; ++u) {
            const bool allowed = Tier45Allowed(m != 0, u != 0), live = Tier45Live(lp != 0, allowed, a != 0);
            if (u == 1 || m == 1) { if (live != (lp && a)) same = false; }
            else { if (live) never = false; }
            if (m == 0 && u == 0 && live) ++enters;
        }
        check(never && enters == 0, "USE45=0: the 45 is never entered, whatever the fallback state and play (60 <-> 30 only)");
        check(same, "USE45=1 (and MSC_FPS=45): live = live play and not on the fallback 30, exactly today's rule");
        // a whole simulated session at USE45=0 with the fallback at 45 all the time: no picture at 45
        Fallback45 f(Fallback45Config::Hold()); int at45 = 0; for (int i = 0; i < 5000; ++i) if (Tier45Live(true, Tier45Allowed(false, false), f.At45())) ++at45;
        check(at45 == 0, "USE45=0: 5000 live pictures with the fallback state at 45 stay out of the 45");
    }
    std::printf(g_fail ? "p98: %d FAILED\n" : "p98: all ok\n", g_fail);
    return g_fail ? 1 : 0;
}
