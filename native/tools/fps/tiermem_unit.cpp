// tiermem_unit.cpp (pace13, 22 Nov 2026): MSC_TIER_MEMORY, Smooth 60 remembers the tier a stadium settled at. The table (np_fps_tiermem.h) and, on virtual vblanks, the real controller +
// Fallback45 started from the remembered tier (Auto::SeedRelief, Fallback45::SeedFallback) against started at 60. SYNTHETIC loads, every figure an ESTIMATE.
//   c++ -std=c++11 -O1 -Wall -Inative/platform/compat native/tools/fps/tiermem_unit.cpp -o tiermem_unit && ./tiermem_unit [table]
// Mutants (-DNP_TMEM_BREAK=N, each must FAIL): 1 the settled tier is not the most-time one, 2 a 60 does not clear the entry, 3 the key ignores the mode, 4 the seeded start can never step up.
#include "np_fps_tier.h"
#include "np_fps_tiermem.h"
#include "np_fps45.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
using namespace npfps;

static int g_fail = 0;
static void check(bool ok, const char* what) { if (!ok) { std::printf("FAIL: %s\n", what); ++g_fail; } }
static const double R = 1001.0 / 60000.0, Rms = R * 1000.0;

struct Change { double t; int from, to; };
struct Sim {
    Auto ctl;
    Fallback45 fall;
    double now = 0, secs[3] = {0, 0, 0};
    std::vector<Change> ch;
    unsigned misses = 0;
    Sim() : ctl(Config::Lock60Tiered()) { fall.SetLatchSecs(240.0); }
    Tier Now() const { return TierOf(ctl.Current() == kPolicy60, fall.At45()); }
    void Seed(int tier, double hold) {   // exactly what np_fps.cpp does at the first live picture of a match
        if (tier == 45 || tier == 30) ctl.SeedRelief(now, hold);
        if (tier == 30) fall.SeedFallback(now, hold);
    }
    void Step(double cost) {
        const Tier t = Now();
        double interval = R, cpu = cost;
        bool miss = false;
        if (t == kTier60) { miss = cost > Rms; interval = miss ? 2.0 * R : R; fall.Pause(); }
        else if (t == kTier45) { cpu = cost + 0.28 * 3.4; interval = 4.0 / 3.0 * R; fall.Picture45(cpu > 4.0 / 3.0 * Rms, now); }
        else { cpu = cost + 0.84 * 3.4; interval = 2.0 * R; fall.Picture30(cost + 0.28 * 3.4 <= 20.0, now); ctl.DeferUp(now, 2.0); }
        now += interval;
        secs[t] += interval;
        if (miss) ++misses;
        FrameIn f;
        std::memset(&f, 0, sizeof f);
        f.now = now; f.interval = interval; f.steps = 1; f.simMs = 3.4; f.costsValid = true; f.gameCore = 0;
        f.cpuMs[0] = cpu; f.cpuMs[1] = cpu * 0.5; f.scene = 2; f.gpuMinMs = 9.0; f.sceneClass = 0; f.ladderRoom = false;
        ctl.Frame(f);
        const Tier after = Now();
        if (after != t) ch.push_back({now, (int)t, (int)after});
    }
    template <class F> void Run(double s, F cost) { const double end = now + s; for (unsigned i = 0; now < end; ++i) Step(cost(i, now)); }
};

int main(int argc, char** argv) {
    const bool table = argc > 1 && !std::strcmp(argv[1], "table");
    // ---- the table
    TierMem m;
    check(m.Get(1, 5) == 0, "empty: nothing remembered");
    check(!m.Settled(1, 5, 20.0, 10.0, 5.0), "a match with under 60 s of play remembers nothing");
    check(m.Settled(1, 5, 10.0, 200.0, 20.0) && m.Get(1, 5) == 45, "most time at 45: remembered 45");
    check(m.Settled(1, 5, 10.0, 20.0, 200.0) && m.Get(1, 5) == 30, "most time at 30: remembered 30");
    check(!m.Settled(1, 5, 10.0, 20.0, 200.0), "the same verdict again changes nothing (no write)");
    check(m.Settled(1, 5, 300.0, 20.0, 10.0) && m.Get(1, 5) == 0 && m.Count() == 0, "most time at 60: the entry is cleared");
    check(m.Settled(1, 7, 10.0, 100.0, 90.0) && m.Get(1, 7) == 45, "45 against 30: the larger one");
    check(m.Get(2, 7) == 0, "another picture mode does not see it");
    check(!m.Settled(1, -1, 0.0, 300.0, 0.0), "an unknown stadium is not stored");
    {
        TierMem a;
        a.Put(1, 3, 45); a.Put(1, 9, 30); a.Put(1, 11, 45);
        char buf[512];
        a.Format(buf, sizeof buf);
        TierMem b;
        b.Parse(buf);
        check(b.Count() == 3 && b.Get(1, 3) == 45 && b.Get(1, 9) == 30 && b.Get(1, 11) == 45, "file round trip");
        TierMem c;
        c.Parse("garbage\n1 4 99\n1 x y\n-1 2 45\n1 4 45\n1 4 30\n1 1500 45\n\n1 6 30");
        check(c.Count() == 2 && c.Get(1, 4) == 45 && c.Get(1, 6) == 30, "garbage, a bad tier, a duplicate and a stadium out of range are ignored");
        TierMem d;
        for (int i = 0; i < 100; ++i) d.Put(1, i, 45);
        check(d.Count() == TierMem::kMax, "the table is capped");
    }
    // ---- the dynamics (synthetic): a stadium that settles at 30, one at 45, and one that now holds 60
    // 1. The Dump-like load (23.5 ms a picture): unseeded it opens at 60 and drops through 45 to 30; seeded 30 it opens at 30
    Sim u, s;
    u.Run(60.0, [](unsigned, double) { return 23.5; });
    s.Seed(30, 60.0);
    s.Run(60.0, [](unsigned, double) { return 23.5; });
    if (table) std::printf("heavy (23.5 ms): unseeded %zu changes, %.1f s at 60, %u repeated refreshes at 60; seeded 30: %zu changes, %.1f s at 60, %u repeated refreshes at 60, %.1f s at 30 of 60\n", u.ch.size(), u.secs[0], u.misses, s.ch.size(), s.secs[0], s.misses, s.secs[2]);
    check(u.ch.size() == 2 && u.secs[0] > 0.0 && u.misses > 0, "unseeded heavy: the kick-off dip, 60 -> 45 -> 30");
    check(s.ch.empty() && s.secs[0] == 0.0 && s.misses == 0 && s.Now() == kTier30, "seeded 30: no change, no time at 60, no repeated refresh");
    // 2. a steady 55 a second: seeded 45 opens at 45 and keeps it
    Sim u2, s2;
    u2.Run(60.0, [](unsigned i, double) { return i % 12 == 5 ? 18.5 : 15.0; });
    s2.Seed(45, 60.0);
    s2.Run(60.0, [](unsigned i, double) { return i % 12 == 5 ? 18.5 : 15.0; });
    if (table) std::printf("steady 55/s: unseeded %zu changes, %u repeated refreshes at 60; seeded 45: %zu changes, %u repeated refreshes at 60, %.1f s at 45 of 60\n", u2.ch.size(), u2.misses, s2.ch.size(), s2.misses, s2.secs[1]);
    check(u2.ch.size() == 1 && u2.misses > 0 && s2.ch.empty() && s2.misses == 0 && s2.Now() == kTier45, "seeded 45 on a load that needs it: no change, no repeated refresh at 60");
    // 3. the price: a stadium that now holds 60 (11 ms), seeded 45: it must still get 60 back, on proof, after the hold (60 s here) and the 12 s proof
    Sim s3;
    s3.Seed(45, 60.0);
    s3.Run(200.0, [](unsigned, double) { return 11.0; });
    double up = -1; for (const Change& c : s3.ch) if (c.from == kTier45 && c.to == kTier60) up = c.t;
    if (table) std::printf("seeded 45, load now light (11 ms): back at 60 at %.1f s (hold 60 s + proof); %.1f s at 45 of 200\n", up, s3.secs[1]);
    check(up >= 60.0 && up <= 60.0 + 20.0 && s3.Now() == kTier60, "seeded 45 on a light load: 60 again after the hold and the proof (no sooner, not much later)");
    // 4. and the seeded 30 on the light load: 30 -> 45 -> 60 in order
    Sim s4;
    s4.Seed(30, 30.0);
    s4.Run(200.0, [](unsigned, double) { return 11.0; });
    check(s4.Now() == kTier60 && s4.ch.size() == 2 && s4.ch[0].from == kTier30 && s4.ch[0].to == kTier45 && s4.ch[1].to == kTier60, "seeded 30 on a light load: up in order 30 -> 45 -> 60");
    // 5. seeding is not a relief for the latch: three fallbacks after it are what latch, not four
    check(s.ctl.Seeds() == 1, "the controller counted the seed");
    if (g_fail) { std::printf("tiermem_unit: %d FAILED\n", g_fail); return 1; }
    std::printf("tiermem_unit: all checks passed\n");
    return 0;
}
