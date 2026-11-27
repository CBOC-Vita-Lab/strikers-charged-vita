// tier_unit.cpp (Fable, 18 Nov 2026): MSC_LOCK60_TIERS, Smooth 60 in three tiers (60 -> the strict 45 cadence -> the full-size 30) on virtual vblanks. A MODEL of the load, not the
// game: the real controller (np_fps_ctl.h Config::Lock60Tiered), the real 45 fallback (np_fps45.h Fallback45) and their composition (np_fps_tier.h), fed one picture at a time with a
// game-thread cost per picture. Every figure is synthetic (an ESTIMATE); the console's come from the [tier] line.
//   c++ -std=c++11 -O1 -Wall -Inative/platform/compat native/tools/fps/tier_unit.cpp -o tier_unit && ./tier_unit [table]
// Mutants (-DNP_TIER_BREAK=N, each must FAIL): 1 falls on a spike (Lock60's own windows: 3 misses of 60), 2 never falls, 3 flaps (no sticky hold), 4 skips 45 (the relief is the 30).
#include "np_fps_tier.h"
#include "np_fps45.h"
#include "np_show_clock.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
using namespace npfps;

static int g_fail = 0;
static void check(bool ok, const char* what, const char* detail = "") { if (!ok) { std::printf("FAIL: %s %s\n", what, detail); ++g_fail; } }
static const double R = 1001.0 / 60000.0, Rms = R * 1000.0;

struct Change { double t; int from, to; };
struct Sim {
    Auto ctl;
    Fallback45 fall;
    TierStats st;
    double now = 0;
    std::vector<Change> ch;
    Sim() : ctl(Config::Lock60Tiered()) { fall.SetLatchSecs(240.0); }
    // one live-play picture whose game-thread work at 60 (one step or none) costs `cost` ms; at 45 a picture carries 1.11 steps and at 30 1.67 (3.4 ms a step)
    void Step(double cost) {
        const Tier t = TierOf(ctl.Current() == kPolicy60, fall.At45());
        double interval = R, cpu = cost;
        bool miss = false;
        if (t == kTier60) { miss = cost > Rms; interval = miss ? 2.0 * R : R; fall.Pause(); }
        else if (t == kTier45) { cpu = cost + 0.28 * 3.4; interval = 4.0 / 3.0 * R; fall.Picture45(cpu > 4.0 / 3.0 * Rms, now); }
        else { cpu = cost + 0.84 * 3.4; interval = 2.0 * R; fall.Picture30(cost + 0.28 * 3.4 <= 20.0, now); ctl.DeferUp(now, 2.0); }
        now += interval;
        FrameIn f;
        std::memset(&f, 0, sizeof f);
        f.now = now; f.interval = interval; f.steps = 1; f.simMs = 3.4; f.costsValid = true; f.gameCore = 0;
        f.cpuMs[0] = cpu; f.cpuMs[1] = cpu * 0.5; f.scene = 2; f.gpuMinMs = 9.0; f.sceneClass = 0; f.ladderRoom = false;
        ctl.Frame(f);
        const Tier after = TierOf(ctl.Current() == kPolicy60, fall.At45());
        if (st.Picture(t, interval, miss) && !ch.empty()) {}
        if (after != t) ch.push_back({now, (int)t, (int)after});
    }
    template <class F> void Run(double secs, F cost) { const double end = now + secs; for (unsigned i = 0; now < end; ++i) Step(cost(i, now)); }
    int MaxIn(double win) const { int m = 0; for (const Change& a : ch) { int n = 0; for (const Change& b : ch) if (b.t >= a.t && b.t < a.t + win) ++n; if (n > m) m = n; } return m; }
    Tier Now() const { return TierOf(ctl.Current() == kPolicy60, fall.At45()); }
    bool Skipped() const { for (const Change& c : ch) if ((c.from == kTier30 && c.to == kTier60) || (c.from == kTier60 && c.to == kTier30)) return true; return false; }
};
static void Print(const char* name, const Sim& s) {
    std::printf("%-34s %.0f s: at 60 %.1f s, at 45 %.1f s, at 30 %.1f s; %zu tier changes (at most %d in 30 s); misses at 60 %u, tolerated %u; now %d\n", name, s.now, s.st.secs[0], s.st.secs[1],
                s.st.secs[2], s.ch.size(), s.MaxIn(30.0), s.st.misses60, s.st.Tolerated(), s.Now() == kTier60 ? 60 : (s.Now() == kTier45 ? 45 : 30));
}

// ---- 60 with the show-time clock: a picture that lands a refresh late shows the world of the refresh it appears on ----------------------------------------------------------------
struct Clock60Result { unsigned pictures = 0, motionErr = 0, holds[4] = {0, 0, 0, 0}, off = 0; double absErr = 0, ownAbsErr = 0; };
static Clock60Result Clock60(unsigned seed) {
    Gate45 gate(Gate45Config(), true);
    Clock45 clk;
    LatencyLearner learn;
    Clock60Result r;
    const double V = Rms;
    uint64_t s = seed;
    auto U = [&s] { s = s * 6364136223846793005ull + 1442695040888963407ull; return (double)(s >> 11) / 9007199254740992.0; };
    const int N = 4000;
    std::vector<double> rep(N + 1, 0), T(N + 1, 0), Town(N + 1, 0), start(N + 1, 0);
    std::vector<uint32_t> shown(N + 1, 0);
    double end = 0, gpuDone = 0, cbFree = 0;
    int fb = 0;
    for (int k = 1; k <= N; ++k) {
        double t = (std::floor(end / V) + 1.0) * V + 0.1;                       // (the 60 swap: the frame starts on the vblank after the one before ended)
        while (k > 3 && rep[k - 3] > t) t += V;                                 // (back-pressure, depth 2)
        t += 5.0 * U();                                                         // (the game thread does not start on the vblank edge: up to 5 ms later, the console's start jitter)
        start[k] = t;
        const uint32_t stamp = (uint32_t)std::floor(t / V);
        while (fb + 1 < k && rep[fb + 1] <= t) ++fb;
        const int L = learn.Ready() ? learn.Quantile(0.8) : 2;
        const int adv = clk.Begin(true, stamp, L, fb > 0, (uint64_t)fb, fb > 0 ? shown[fb] : 0u, fb > 0 ? (int)Pattern45::kEvery : -1);
        uint32_t g = 0;
        const bool hasT = clk.NewestTarget(&g);
        const double own = k > 1 ? (start[k] - start[k - 1]) / V : 0.0;
        T[k] = T[k - 1] + (adv >= 0 ? adv : own);
        Town[k] = Town[k - 1] + own;
        double cost = 11.0 + 1.5 * (U() - 0.5);
        if (U() < 0.04) cost += 9.0;                                             // (4 % of pictures: a 2-step picture, an effect burst: one repeated refresh)
        end = t + cost;
        clk.Handed((uint64_t)k);
        gpuDone = std::max(end + 5.0, gpuDone) + 9.0 + 1.0 * (U() - 0.5);
        const double cb = std::max(gpuDone, cbFree);
        const uint32_t earliest = (uint32_t)std::floor(cb / V) + 1u;
        learn.Add(earliest - stamp);
        const uint32_t due = gate.Due(earliest, hasT, g, false, 0);
        gate.Shown(due);
        shown[k] = due;
        cbFree = (due - 1) * V;
        rep[k] = std::max(cb, (due - 1) * V) + 0.5;
    }
    for (int k = 200; k <= N; ++k) {
        ++r.pictures;
        const double dS = (double)(shown[k] - shown[k - 1]);
        if (std::fabs((T[k] - T[k - 1]) - dS) > 1e-6) ++r.motionErr;
        r.absErr += std::fabs((T[k] - T[k - 1]) - dS);
        r.ownAbsErr += std::fabs((Town[k] - Town[k - 1]) - dS);
        ++r.holds[dS >= 3 ? 3 : (int)dS];
    }
    r.off = gate.offPattern_;
    return r;
}

int main(int argc, char** argv) {
    const bool table = argc > 1 && !std::strcmp(argv[1], "table");
    // 1. spikes at 60: 14 ms a picture, and every 10 s a burst of 8 pictures at 19 ms (a goal's effects), plus a lone 21 ms picture (a 2-step picture) every 2 s: 60 is kept
    Sim a;
    a.Run(120.0, [](unsigned i, double) { const unsigned p = i % 600; return (p >= 300 && p < 308) ? 19.0 : (i % 120 == 60 ? 21.0 : 14.0); });
    if (table) Print("spikes (bursts of 8, lone misses)", a);
    check(a.ch.empty() && a.Now() == kTier60, "spikes: 60 is kept (no tier change)");
    check(a.st.misses60 >= 100 && a.st.Tolerated() == a.st.misses60, "spikes: the misses are there and all were tolerated");
    // 2. a steady 55 pictures a second (one picture in twelve misses): 60 -> 45 within 4 s, 45 holds, never the 30
    Sim b;
    b.Run(60.0, [](unsigned i, double) { return i % 12 == 5 ? 18.5 : 15.0; });
    if (table) Print("steady 55 a second", b);
    check(!b.ch.empty() && b.ch[0].from == kTier60 && b.ch[0].to == kTier45 && b.ch[0].t < 4.0, "steady 55: 60 -> 45 within 4 s");
    check(b.Now() == kTier45 && b.st.secs[2] == 0.0 && b.ch.size() == 1, "steady 55: the 45 holds (one change, no 30)");
    // 3. too heavy for 45 as well (23.5 ms a picture): 60 -> 45 -> 30 and it stays
    Sim c;
    c.Run(120.0, [](unsigned, double) { return 23.5; });
    if (table) Print("too heavy for 45", c);
    check(c.ch.size() == 2 && c.ch[0].to == kTier45 && c.ch[1].from == kTier45 && c.ch[1].to == kTier30 && c.Now() == kTier30, "too heavy: 60 -> 45 -> 30, then the 30 stays");
    check(c.ch.size() >= 2 && c.ch[1].t < 8.0, "too heavy: the 30 within 8 s");
    // 4. a bursty load (4 s at 12 ms, 6 s at 19 ms) for 300 s: no flapping
    Sim d;
    d.Run(300.0, [](unsigned, double t) { return std::fmod(t, 10.0) < 4.0 ? 12.0 : 19.0; });
    if (table) Print("bursty 12 / 19 ms", d);
    check(d.ch.size() <= 6 && d.MaxIn(30.0) <= 2, "bursty: at most 6 tier changes in 300 s and 2 in any 30 s");
    check(!d.Skipped(), "bursty: never 60 <-> 30 directly");
    // 5. recovery: too heavy for 20 s, then light for good: up again in order, 30 -> 45 -> 60, on proof (not before the holds)
    Sim e;
    e.Run(20.0, [](unsigned, double) { return 23.5; });
    e.Run(160.0, [](unsigned, double) { return 11.0; });
    if (table) { Print("recovery", e); for (const Change& x : e.ch) std::printf("   %.1f s: %d -> %d\n", x.t, x.from == 0 ? 60 : (x.from == 1 ? 45 : 30), x.to == 0 ? 60 : (x.to == 1 ? 45 : 30)); }
    check(e.Now() == kTier60 && !e.Skipped(), "recovery: back at 60, never 30 -> 60 directly");
    check(e.ch.size() == 4 && e.ch[2].from == kTier30 && e.ch[2].to == kTier45 && e.ch[2].t - e.ch[1].t >= 25.0 && e.ch[3].from == kTier45 && e.ch[3].to == kTier60 && e.ch[3].t - e.ch[2].t >= 2.0,
          "recovery: 30 -> 45 after the 25 s hold and the proof, then 45 -> 60 on the controller's proof");
    // 6. a clean 60 never leaves it
    Sim f;
    f.Run(120.0, [](unsigned, double) { return 13.0; });
    check(f.ch.empty() && f.st.misses60 == 0, "clean: 60 all the time");
    // 7. the show-time clock at 60: with 4 % of pictures missing a refresh, the game time between two shown pictures is the refreshes between them
    const Clock60Result q = Clock60(7);
    if (table) std::printf("60 with the show-time clock: %u pictures, holds 1/2/3+ %u/%u/%u, pictures off their show time %u (%.2f %%), mean error %.3f refreshes; with each frame's own start-to-start time: mean error %.3f refreshes\n", q.pictures, q.holds[1],
                           q.holds[2], q.holds[3], q.motionErr, 100.0 * q.motionErr / q.pictures, q.absErr / q.pictures, q.ownAbsErr / q.pictures);
    check(q.holds[2] > 40, "clock at 60: the misses are there");
    // (the picture that itself overran cannot be repaired: it was drawn for the refresh it missed. It and the catch-up after it are the two errors a miss costs; everything else is exact.)
    check(q.motionErr <= 2 * (q.holds[2] + q.holds[3]) + 8 && q.off == 0, "clock at 60: at most two pictures off their show time per repeated refresh, none anywhere else");
    check(q.absErr * 2.0 <= q.ownAbsErr, "clock at 60: at most half the mean motion error of each frame's own start-to-start time (start jitter is gone)");
    if (g_fail) { std::printf("tier_unit: %d FAILED\n", g_fail); return 1; }
    std::printf("tier_unit: all checks passed (spikes tolerated, sustained shortfall -> 45, 45 -> 30 only when 45 fails, no flapping, up in order on proof, show-time clock at 60)\n");
    return 0;
}
