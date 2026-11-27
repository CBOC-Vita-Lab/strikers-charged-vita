// trap24 (wave 24): the 45 -> fallback 30 that never ends (console jobs 6024, 6023, 6016: "fallback 300" in every [f45] window to the end of the match). Fallback45 (np_fps45.h) with the Hold() config.
// The 6024 sequence: 45 almost clean (1-6 late of 300), one burst of 20 slips (19 of 360: the slow window) -> FALLBACK; at the 30 the proof (np_fps.cpp: est <= 20.0 ms per picture, at most 10 % of the last
// 120 pictures not fitting) read "208 fit / 92 not (game 92)", "120 pictures, 30..48 not fitting": a third of the pictures over 20 ms, so it never passed. MSC_F45_RETRY (relaxAfter) judges the MEAN cost instead.
#include "np_fps45.h"
#include <cstdio>
#include <cmath>
using namespace npfps;
static int g_fail = 0;
static void check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) ++g_fail; }
static const double Rms = 1000.0 / 59.94;

struct Match {
    Fallback45 fall;
    double now = 1000.0;
    unsigned falls0 = 0, backs = 0;
    double fellAt = -1, backAt = -1;
    explicit Match(bool retry, double secs = 20.0) : fall(Cfg(retry, secs)) {}
    static Fallback45Config Cfg(bool retry, double secs) { Fallback45Config c = Fallback45Config::Hold(); if (retry) { c.relaxAfter = secs; c.relaxMean = 18.0; } return c; }
    bool Pic45(bool late) { now += (2.0 * Rms / 1000.0) * 2.0 / 3.0; if (fall.Picture45(late, now)) { fellAt = now; ++falls0; return true; } return false; }
    // the 6024 prefix: 45 for ~50 s with 6 late of 300, then a burst of 20 slips in 40 pictures
    void Prefix() {
        for (int i = 0; i < 1500 && fall.At45(); ++i) Pic45(i % 50 == 7);
        for (int i = 0; i < 40 && fall.At45(); ++i) Pic45(i % 2 == 0);
    }
    // one picture at the fallback 30 (33.3 ms): est = the game thread's cost at 45; hw = the GPU / lanes fit
    bool Pic30(double est, bool hw = true, double fitMs = 20.0) {
        now += 2.0 * Rms / 1000.0;
        const bool fits = est <= fitMs && hw;
        if (fall.Picture30(fits, now, est, hw)) { backAt = now; ++backs; return true; }
        return false;
    }
    // 6024's cost: two pictures of three at 14 ms, one at 20.5 (mean 16.2, a third over 20.0)
    void Run30(double secs, double lo, double hi, bool hw = true) {
        const double end = now + secs;
        for (int i = 0; now < end && !fall.At45(); ++i) Pic30(i % 3 == 2 ? hi : lo, hw);
    }
};

int main() {
    { Match m(false); m.Prefix(); check(!m.fall.At45() && m.falls0 == 1, "6024: the burst of 20 slips falls to the 30 (once)");
      m.Run30(150.0, 14.0, 20.5);
      check(!m.fall.At45() && m.backs == 0, "6024 OFF: a third of the pictures over 20 ms (mean 16.2) and the 30 never ends in 150 s (the console: 100 s at 30, 0 returns)"); }
    { Match m(true); m.Prefix(); const double t0 = m.now; m.Run30(150.0, 14.0, 20.5);
      check(m.fall.At45() && m.backs == 1 && m.fall.Relaxed() == 1, "6024 ON: the same pictures come back to 45 on the mean proof");
      check(m.backAt - t0 >= 10.0 + 20.0 - 1.0 && m.backAt - t0 <= 10.0 + 20.0 + 12.0, "ON: not before the hold (10 s) + the retry wait (20 s), and within 12 s after"); }
    { Match m(true); m.Prefix(); m.Run30(150.0, 14.0, 20.5, false);
      check(!m.fall.At45(), "ON: the GPU / lanes over budget (The Dump's 22 ms GPU) never retry: still judged per picture"); }
    { Match m(true); m.Prefix(); m.Run30(150.0, 18.0, 27.0);   // mean 21: the 45 cannot hold this
      check(!m.fall.At45(), "ON: a mean cost of 21 ms (over the 22.2 slot average with the margin) stays at the 30"); }
    { Match m(true); m.Prefix(); m.Run30(150.0, 20.0, 23.0);
      check(!m.fall.At45(), "ON: a mean of 21 ms with no picture under 20 stays at the 30"); }
    { Match m(true); m.Prefix(); m.Run30(25.0, 14.0, 20.5);
      check(!m.fall.At45(), "ON: before the hold plus the retry wait nothing returns (25 s)"); }
    // a retry that fails (the burst comes back) falls again and waits the doubled hold: 10 -> 20 s, then the retry wait again
    { Match m(true); m.Prefix(); m.Run30(150.0, 14.0, 20.5);
      const double back1 = m.backAt;
      for (int i = 0; i < 1000 && m.fall.At45(); ++i) m.Pic45(i % 2 == 0);
      check(!m.fall.At45() && m.falls0 == 2, "ON: a retry whose burst returns falls again");
      const double fell2 = m.fellAt; m.Run30(150.0, 14.0, 20.5);
      check(m.fall.At45() && m.backAt - fell2 >= 20.0 + 20.0 - 1.0, "ON: the second fall waits the doubled hold (20 s) plus the retry wait");
      (void)back1; }
    // an estimate that is not known never relaxes
    { Match m(true); m.Prefix(); const double end = m.now + 150.0; for (int i = 0; m.now < end && !m.fall.At45(); ++i) { m.now += 2.0 * Rms / 1000.0; m.fall.Picture30(i % 3 != 2, m.now, -1.0, true); }
      check(!m.fall.At45(), "ON: no estimate (-1): the relaxed proof stays silent"); }
    // the per-picture proof is unchanged: pictures that all fit return at the hold + the proof, with or without the switch
    { Match a(false), b(true); a.Prefix(); b.Prefix(); const double ta = a.now, tb = b.now; a.Run30(100.0, 12.0, 12.0); b.Run30(100.0, 12.0, 12.0);
      check(a.fall.At45() && b.fall.At45() && std::fabs((a.backAt - ta) - (b.backAt - tb)) < 0.2, "all pictures fit: OFF and ON return at the same time (the per-picture proof is untouched)"); }
    std::printf(g_fail ? "trap24 unit: %d FAILED\n" : "trap24 unit: all passed (%d failed)\n", g_fail);
    return g_fail ? 1 : 0;
}
