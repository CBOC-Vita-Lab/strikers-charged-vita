// tier_flap_unit.cpp (pace13, 22 Nov 2026): does Smooth 60's tier machine (np_fps_tier.h: the controller's Lock60Tiered rules + Fallback45) flap on a load that hovers at the edge?
// The same model as tier_unit.cpp (real Auto + real Fallback45, one picture at a time), fed SYNTHETIC game-thread costs drawn from the console's recorded 1-step figures (Vice, 17-18 Nov, s27-fin:
// 1-step p50 15.5 ms, p95 17-18 ms, [gsplit-pic]; a refresh is 16.68 ms) with a slow wander of the scene's weight. No per-picture 60 trace exists (the console has held 30 in play so far), so every
// figure is an ESTIMATE. Galaxy's governor (handoff-1121 galaxy-state-1121pm.md: engage after 10 of the last 12 s under 55, leave when predicted to hold 9 of the last 12 s, changes >= 8 s apart,
// relapse backoff) is modelled beside it for comparison (class GalaxyGov, a 2-state machine on the same pictures).
//   c++ -std=c++11 -O1 -Wall -Inative/platform/compat native/tools/fps/tier_flap_unit.cpp -o tier_flap_unit && ./tier_flap_unit [table]
// Switches under test (np_fps_ctl.h Config::tierDwell / np_fps45.h Fallback45Config::dwell, MSC_TIER_DWELL / MSC_TIER_RELAPSE): a minimum dwell after a rise, and a relapse backoff.
// Mutants (-DNP_FLAP_BREAK=N, each must FAIL): 1 dwell ignored, 2 relapse backoff never grows.
#include "np_fps_tier.h"
#include "np_fps45.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
using namespace npfps;

static int g_fail = 0;
static void check(bool ok, const char* what) { if (!ok) { std::printf("FAIL: %s\n", what); ++g_fail; } }
static const double R = 1001.0 / 60000.0, Rms = R * 1000.0;

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 2654435761ull + 12345ull) {}
    double U() { s = s * 6364136223846793005ull + 1442695040888963407ull; return (double)(s >> 11) / 9007199254740992.0; }
    double N() { double u = U() + 1e-12, v = U(); return std::sqrt(-2.0 * std::log(u)) * std::cos(6.283185307179586 * v); }
};
struct Change { double t; int from, to; };

struct Sim {
    Auto ctl;
    Fallback45 fall;
    double now = 0, secs[3] = {0, 0, 0};
    std::vector<Change> ch;
    unsigned misses = 0, pics = 0;
    explicit Sim(const Config& cfg, const Fallback45Config& fc = Fallback45Config()) : ctl(cfg), fall(fc) { fall.SetLatchSecs(240.0); }
    Tier Now() const { return TierOf(ctl.Current() == kPolicy60, fall.At45()); }
    void Step(double cost) {
        const Tier t = Now();
        double interval = R, cpu = cost;
        bool miss = false;
        if (t == kTier60) { miss = cost > Rms; interval = miss ? 2.0 * R : R; fall.Pause(); }
        else if (t == kTier45) { cpu = cost + 0.28 * 3.4; interval = 4.0 / 3.0 * R; fall.Picture45(cpu > 4.0 / 3.0 * Rms, now); }
        else { cpu = cost + 0.84 * 3.4; interval = 2.0 * R; fall.Picture30(cost + 0.28 * 3.4 <= 20.0, now); ctl.DeferUp(now, 2.0); }
        now += interval;
        secs[t] += interval;
        ++pics;
        if (miss) ++misses;
        FrameIn f;
        std::memset(&f, 0, sizeof f);
        f.now = now; f.interval = interval; f.steps = 1; f.simMs = 3.4; f.costsValid = true; f.gameCore = 0;
        f.cpuMs[0] = cpu; f.cpuMs[1] = cpu * 0.5; f.scene = 2; f.gpuMinMs = 9.0; f.sceneClass = 0; f.ladderRoom = false;
        ctl.Frame(f);
        const Tier after = Now();
        if (after != t) ch.push_back({now, (int)t, (int)after});
    }
    int MaxIn(double win) const { int m = 0; for (const Change& a : ch) { int n = 0; for (const Change& b : ch) if (b.t >= a.t && b.t < a.t + win) ++n; if (n > m) m = n; } return m; }
    // a relapse: a rise (to a better tier) followed by a fall within `win` seconds
    int Relapses(double win) const { int n = 0; for (size_t i = 1; i < ch.size(); ++i) if (ch[i].to > ch[i].from && ch[i - 1].to < ch[i - 1].from && ch[i].t - ch[i - 1].t < win) ++n; return n; }
    int Rises() const { int n = 0; for (const Change& c : ch) if (c.to < c.from) ++n; return n; }
    double MinGap() const { double m = 1e9; for (size_t i = 1; i < ch.size(); ++i) m = std::fmin(m, ch[i].t - ch[i - 1].t); return m; }
};

// The load: a picture at 60 with a step (5 in 6) costs median M (lognormal, sigma 0.075 = the console's p50 15.5 / p95 17.5), one without a step M - 6. M(t) = base + amp * sin(t / period).
struct Load {
    double base, amp, period;
    Rng rng;
    Load(double b, double a, double p, uint64_t seed) : base(b), amp(a), period(p), rng(seed) {}
    double operator()(unsigned i, double t) {
        const double M = base + amp * std::sin(6.283185307179586 * t / period);
        const bool step = (i % 6) != 5;
        return (step ? M : M - 6.0) * std::exp(0.075 * rng.N());
    }
};

// Galaxy's governor on the same pictures (a second-by-second 2-state machine): engage (step down) after 10 of the last 12 s under 55 pictures a second; leave when plain 60 is predicted to hold in
// 9 of the last 12 s; changes at least 8 s apart; a relapse (a leave followed by an engage within 12 s) doubles the next leave's hold (8 s -> 16 s ... 64 s). ESTIMATE of its rules as written in the
// notes, not its code (Galaxy's source is not on this machine).
struct GalaxyGov {
    bool engaged = false;
    double lastChange = -1e9, leaveHold = 8.0, lastLeave = -1e9;
    int under[12], holds[12], pos = 0, nsec = 0;
    unsigned picsThisSec = 0, missesThisSec = 0;
    double secStart = 0;
    unsigned changes = 0;
    double engagedSecs = 0;
    std::vector<Change> ch;
    GalaxyGov() { std::memset(under, 0, sizeof under); std::memset(holds, 0, sizeof holds); }
    // a picture shown at the current state: `fps60` is true when it was a 60 picture that met its refresh (only meaningful while not engaged); `held` whether plain 60 would have held it (the prediction)
    void Picture(double now, double interval, bool wouldHold) {
        picsThisSec += 1;
        if (!wouldHold) missesThisSec += 1;
        (void)interval;
        if (now - secStart < 1.0) return;
        // one second closed: the fps the player got while engaged is 60 minus nothing (the step down is 45/30, it does not miss); judge the PREDICTION of plain 60
        const double predFps = 60.0 * (1.0 - (double)missesThisSec / (picsThisSec ? picsThisSec : 1));
        under[pos % 12] = predFps < 55.0 ? 1 : 0;
        holds[pos % 12] = predFps >= 58.0 ? 1 : 0;   // "holds" = (nearly) all pictures make their refresh
        ++pos; if (nsec < 12) ++nsec;
        secStart = now; picsThisSec = missesThisSec = 0;
        int u = 0, h = 0; for (int i = 0; i < 12; ++i) { u += under[i]; h += holds[i]; }
        const bool full = nsec >= 12;
        if (engaged) engagedSecs += 1.0;
        if (now - lastChange < 8.0) return;
        if (!engaged && full && u >= 10) { engaged = true; lastChange = now; ++changes; ch.push_back({now, 0, 1}); if (now - lastLeave < 12.0) leaveHold *= 2.0; for (int i = 0; i < 12; ++i) holds[i] = 0; }
        else if (engaged && full && h >= 9 && now - lastChange >= leaveHold) { engaged = false; lastChange = now; lastLeave = now; ++changes; ch.push_back({now, 1, 0}); for (int i = 0; i < 12; ++i) under[i] = 0; if (leaveHold > 8.0 && now - lastLeave > 120.0) leaveHold = 8.0; }
    }
};

struct Row { double base, amp, period; };
static void RunOne(const char* name, const Config& cfg, const Fallback45Config& fc, const Row& r, uint64_t seed, double secs, bool print, Sim* keep = 0) {
    Sim s(cfg, fc);
    Load ld(r.base, r.amp, r.period, seed);
    unsigned i = 0;
    while (s.now < secs) { s.Step(ld(i, s.now)); ++i; }
    if (print) std::printf("  %-22s base %.1f amp %.1f per %3.0f s: %2zu changes (max %d in 30 s, %d rises, %d relapses <15 s, min gap %5.1f s) at 60/45/30 %.0f/%.0f/%.0f s\n", name, r.base, r.amp, r.period, s.ch.size(), s.MaxIn(30.0),
                         s.Rises(), s.Relapses(15.0), s.ch.size() > 1 ? s.MinGap() : 0.0, s.secs[0], s.secs[1], s.secs[2]);
    if (keep) *keep = s;
}

struct Variant { const char* name; double dwell; bool relapse; };
struct Totals { int changes = 0, relapses = 0, worst30 = 0, rises = 0; unsigned misses = 0, held = 0, counted = 0; double at60 = 0; int runs = 0; };
static Totals Sweep(const Variant& v, const Row* rows, size_t n, double secs, bool print) {
    Totals t;
    for (size_t k = 0; k < n; ++k) {
        for (uint64_t seed = 1; seed <= 3; ++seed) {
            Config c = Config::Lock60Tiered();
            c.tierDwell = v.dwell; c.tierRelapse = v.relapse;
            Fallback45Config fc;
            fc.relapseSecs = v.relapse ? 20.0 : 0.0;
            Sim s(c, fc);
            Load ld(rows[k].base, rows[k].amp, rows[k].period, seed);
            unsigned i = 0;
            while (s.now < secs) { s.Step(ld(i, s.now)); ++i; }
            if (print && seed == 1) std::printf("  %-14s base %.1f amp %.1f per %3.0f s: %2zu changes (max %d in 30 s, %d rises, %d relapses <15 s, min gap %5.1f s) at 60/45/30 %.0f/%.0f/%.0f s, misses at 60 %u\n", v.name, rows[k].base, rows[k].amp, rows[k].period,
                                                s.ch.size(), s.MaxIn(30.0), s.Rises(), s.Relapses(15.0), s.ch.size() > 1 ? s.MinGap() : 0.0, s.secs[0], s.secs[1], s.secs[2], s.misses);
            t.changes += (int)s.ch.size(); t.relapses += s.Relapses(15.0); t.rises += s.Rises(); t.misses += s.misses; t.at60 += s.secs[0]; ++t.runs;
            if (s.MaxIn(30.0) > t.worst30) t.worst30 = s.MaxIn(30.0);
            t.held += s.ctl.DwellHeld(); t.counted += s.ctl.Relapses() + s.fall.Relapses();
        }
    }
    return t;
}

int main(int argc, char** argv) {
    const bool table = argc > 1 && !std::strcmp(argv[1], "table");
    const double SECS = 600.0;
    // hovering loads: a steady base near the 14.5 ms budget, and slow waves of the scene's weight across it (the cases a flap needs)
    const Row rows[] = {{13.0, 0, 30}, {14.0, 0, 30}, {14.5, 0, 30}, {15.0, 0, 30}, {15.5, 0, 30}, {16.0, 0, 30},
                        {13.5, 1.5, 40}, {14.0, 2.0, 60}, {14.5, 2.0, 90}, {15.0, 2.0, 40}, {12.0, 4.0, 120}, {13.0, 3.5, 50}, {13.0, 3.0, 45}, {12.5, 3.5, 70}};
    const size_t N = sizeof rows / sizeof rows[0];
    const Variant vs[] = {{"as merged", 0.0, false}, {"RELAPSE", 0.0, true}, {"DWELL 8 s", 8.0, false}, {"DWELL 8+RELAPSE", 8.0, true}, {"DWELL 15 s", 15.0, false}};
    Totals T[5];
    for (int k = 0; k < 5; ++k) {
        T[k] = Sweep(vs[k], rows, N, SECS, table && k == 0);
        if (table) std::printf("%-16s: %2d tier changes over %d runs of %.0f s (%d rises, %d relapses <15 s, worst %d in 30 s), misses at 60 %u, time at 60 %.0f s; dwell held %u pictures, relapses counted %u\n", vs[k].name, T[k].changes, T[k].runs, SECS,
                               T[k].rises, T[k].relapses, T[k].worst30, T[k].misses, T[k].at60, T[k].held, T[k].counted);
    }
    // Galaxy-style governor on the same pictures (seed 1), for comparison only
    if (table) {
        int gch = 0;
        for (size_t k = 0; k < N; ++k) {
            GalaxyGov g;
            Load ld(rows[k].base, rows[k].amp, rows[k].period, 1);
            double now = 0; unsigned i = 0;
            while (now < SECS) { const double c = ld(i, now); g.Picture(now, R, c <= Rms); now += R; ++i; }
            gch += (int)g.changes;
        }
        std::printf("galaxy-style governor (model of its notes, 2-state, seed 1): %d changes over %zu runs of %.0f s\n", gch, N, SECS);
    }
    // the model's verdicts
    check(T[0].worst30 <= 3, "as merged: at most 3 tier changes in any 30 s on any hovering load");
    check(T[0].relapses >= 5, "as merged: the model does show relapses (a rise followed by a fall within 15 s) on hovering loads");
    check(T[0].held == 0 && T[0].counted == 0, "off means off: no counter moves");
    check(T[1].counted > 0 && T[1].relapses <= T[0].relapses, "RELAPSE: relapses are counted (> 0) and there are no more of them than as merged");
    check(T[4].held > 0 && T[4].relapses < T[0].relapses, "DWELL 15 s: the dwell held verdicts back and there are fewer relapses than as merged");
    check(T[4].misses > T[0].misses, "DWELL 15 s: the price is visible, more pictures miss at 60 (a failed try is endured longer)");
    check(T[3].changes <= T[0].changes, "DWELL + RELAPSE: no more tier changes than as merged");
    check(T[1].worst30 <= 3 && T[2].worst30 <= 3 && T[3].worst30 <= 3, "no variant puts more than 3 changes in any 30 s");
    check(T[1].at60 > 0 && T[3].at60 > 0, "the variants still reach 60");
    // the clean loads are not touched by any switch: 13 ms flat stays at 60 with no change, and a heavy steady load still leaves 60 within 4 s with the dwell on
    {
        Config c = Config::Lock60Tiered(); c.tierDwell = 8.0; c.tierRelapse = true;
        Sim a(c); Load clean(13.0, 0, 30, 5); unsigned i = 0; while (a.now < 120.0) { a.Step(clean(i, a.now)); ++i; }
        check(a.ch.empty(), "clean 13 ms: 60 kept, no change, with both switches on");
        Sim b(c); Load heavy(16.0, 0, 30, 5); i = 0; while (b.now < 30.0) { b.Step(heavy(i, b.now)); ++i; }
        check(!b.ch.empty() && b.ch[0].from == kTier60 && b.ch[0].t < 4.0, "heavy 16 ms: still leaves 60 within 4 s with both switches on (the dwell only follows a rise)");
    }
    if (g_fail) { std::printf("tier_flap_unit: %d FAILED\n", g_fail); return 1; }
    std::printf("tier_flap_unit: all checks passed\n");
    return 0;
}
