// hold60_unit.cpp (hold60, 23 Nov 2026): MSC_LOCK60_HOLD, Smooth 60 judged on the delivered rate and climbing back on measured work. The REAL controller (np_fps_ctl.h Config::Lock60Tiered
// against Config::Lock60Hold), the REAL 45 fallback (np_fps45.h Fallback45Config() + SetLatchSecs(240) against Fallback45Config::Hold()) and their composition (np_fps_tier.h), fed one picture
// at a time by a MODEL of the console's pipeline. Every figure is an ESTIMATE; the console's come from the [tier] / [hold60] lines.
//   c++ -std=c++11 -O2 -Wall -Inative/platform/compat native/tools/fps/hold60_unit.cpp -o hold60_unit && ./hold60_unit [table] [native/tools/fps/hold60_traces.txt]
// Mutants (-DNP_HOLD_BREAK=N, each must FAIL): 1 never falls, 2 falls on a spike, 3 flaps, 4 never climbs, 5 climbs on the frame length, 6 the 45 keeps the 8 % fast window, 7 the 30 never climbs,
// 8 (hold60b) every picture priced with a full step, 9 (hold60b) the climb leaves out the blend lane's wait; -DNP_SPREAD_BREAK=1 (hold60b) the 60's catch-up is not spread.
// hold60b (24 Nov): the game thread's blocked wait on the helper lane (the async snapshot blend's join; [gsplit] "other") is part of a picture's cost and depends on the cadence (console s49
// Vice 0.6-0.9 ms at the paced 30, 2.4-5.8 at 45, 1.2-2.6 at 60 in the Mega Strike); the traces carry it, and the replays price it at 60 as at 45 (the console) or as at the paced 30 (what if).
//
// The model, per picture (one game-thread frame and its picture):
//   game time   each picture advances the game by the refreshes its predecessor was held (60: 1, or 2+ after a repeated refresh; 45: 1, 1, 2; 30: 2), so the 20 ms steps a picture runs follow
//               the shown rate (the show-time clock): after a repeated refresh at 60 the next picture carries 1 or 2 steps, which is what can keep a miss going (the "spiral")
//   work        game thread = base x exp(sd N) (+ a spike with probability spikeP) + steps x step ms; the lanes front end / back end (CPU) and the GPU (at the tier's picture size) beside it
//   60          coupled (the console's cut-in behaviour: a picture that misses its vblank holds the game thread to the next one): held ceil(cost / refresh) refreshes; decoupled (back-
//               pressure depth): up to one refresh of slack is banked by early pictures and a late one costs exactly one repeated refresh
//   45          a picture is off its slot when its cost runs over the 45 budget (22.24 ms) beyond one refresh of banked slack; 30: over 33.4 ms
//   inputs      the controller hears what np_fps.cpp gives it: the frame length, steps, sim ms, the per-core busy (game core = work, the render core = the larger lane), the GPU at the floor;
//               with the hold the work (frame length less waits) and the lanes' means. The 30's proof for 45 uses np_fps.cpp's test (game with 1.11 steps, GPU at the floor, lanes <= 20 ms).
#include "np_fps_tier.h"
#include "np_fps45.h"
#include "np_show_clock.h"
#include "np_game_clock.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
using namespace npfps;

static int g_fail = 0;
static void check(bool ok, const char* what) { if (!ok) { std::printf("FAIL: %s\n", what); ++g_fail; } }
static const double R = 1001.0 / 60000.0, Rms = R * 1000.0;

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 2654435761ull + 1) {}
    double U() { s = s * 6364136223846793005ull + 1442695040888963407ull; return ((double)(s >> 11) + 0.5) / 9007199254740992.0; }
    double N() { return std::sqrt(-2.0 * std::log(U())) * std::cos(6.283185307179586 * U()); }
};

struct Load {
    double base = 9.1, sd = 0.08, spikeP = 0.0, spikeMs = 0.0, step = 5.1;   // game thread: the picture's own work (median), its spread, spikes, ms a step
    double fe = 11.0, be = 10.0;          // lanes, CPU ms a picture (means; 8 % spread)
    double gpu60 = 10.5, gpu45 = 12.5, gpu30 = 14.5;   // GPU ms a picture at the tier's size (60: the floor)
    double nfit = 0.0;                    // the console's unexplained share of pictures whose 45-fit test failed at the 30 (H2: in the game term, seen by both tests; 0: none)
    double nfitLane = 0.0;                // ... H1: in the lanes' largest-of-three (only the old test sees it)
    double lane30 = 0.0, lane45 = 0.0, lane60 = 0.0;   // hold60b: the game thread's blocked wait on the blend lane, mean ms a picture at each tier (gamma 2 around it; 0: none, nothing drawn)
};

struct Sim {
    Auto ctl;
    Fallback45 fall;
    TierStats st;
    bool hold, coupled, spread;
    Rng rng;
    double now = 0, acc = 0, slack = 0, feMean = 0, beMean = 0, debt = 0;
    unsigned twoStep60 = 0, repeats60 = 0;   // pictures at 60 that ran two steps or more / that were held 2+ refreshes
    int prevHold = 1;
    unsigned changes = 0, pictures = 0, scene = 2;
    std::vector<double> changeT;
    std::vector<int> changeTo;
    Sim(bool h, bool c, uint64_t seed) : ctl(h ? Config::Lock60Hold() : Config::Lock60Tiered()), fall(h ? Fallback45Config::Hold() : Fallback45Config()), hold(h), coupled(c), spread(h), rng(seed) {
        if (!h) fall.SetLatchSecs(240.0);
    }
    Tier Now() const { return TierOf(ctl.Current() == kPolicy60, fall.At45()); }
    std::vector<int> changeScene;
    unsigned cuts = 0;
    bool justCut = false;
    std::vector<char> changeAtCut;
    void Cut() { scene = scene == 2 ? 3 : 2; ++cuts; justCut = true; }   // a cut into play (kick-off, play after a goal): the controller sees a new gameplay scene
    void Picture(const Load& L) {
        const Tier t = Now();
        // game time and steps (the show clock: the predecessor's hold)
        double adv = t == kTier60 ? prevHold * Rms : (t == kTier45 ? (prevHold >= 2 ? 2.0 : 1.0) * Rms : 2.0 * Rms);
        if (spread) {   // hold60b: the debt clock with the 60 tier's spread catch-up (np_game_clock.h Spread60Cap: at most one step a picture while the debt is at most a step)
            const double want = adv + debt;
            const double lim = t == kTier60 && Spread60Cap(5u, debt, ClockConfig()) == 1u ? 20.0 : 1e9;
            adv = want < lim ? want : lim;
            debt = want - adv;
        }
        acc += adv;
        int steps = (int)std::floor(acc / 20.0);
        acc -= steps * 20.0;
        double base = L.base * std::exp(L.sd * rng.N());
        if (L.spikeP > 0 && rng.U() < L.spikeP) base += L.spikeMs;
        const double lm = t == kTier60 ? L.lane60 : (t == kTier45 ? L.lane45 : L.lane30);
        const double lw = lm > 0 ? -0.5 * lm * (std::log(rng.U()) + std::log(rng.U())) : 0.0;   // (drawn only with a lane wait: the other scenarios keep their random sequence)
        const double work = base + steps * L.step + lw;
        const double fe = L.fe * (1.0 + 0.08 * rng.N()), be = L.be * (1.0 + 0.08 * rng.N());
        const double gpu = (t == kTier60 ? L.gpu60 : (t == kTier45 ? L.gpu45 : L.gpu30)) * (1.0 + 0.05 * rng.N());
        double cost = work;
        if (fe > cost) cost = fe;
        if (be > cost) cost = be;
        if (gpu > cost) cost = gpu;
        feMean = pictures ? feMean + (fe - feMean) / 16.0 : fe;
        beMean = pictures ? beMean + (be - beMean) / 16.0 : be;
        int holdR = 1;
        bool late = false;
        double interval;
        if (t == kTier60) {
            if (coupled) { holdR = cost <= Rms ? 1 : (int)std::ceil(cost / Rms); slack = 0; }
            else { slack += cost - Rms; if (slack < -Rms) slack = -Rms; while (slack > 0) { ++holdR; slack -= Rms; } }
            interval = holdR * Rms;
            fall.Pause();
        } else if (t == kTier45) {
            const double budget = 4.0 / 3.0 * Rms;
            slack += cost - budget; if (slack < -Rms) slack = -Rms;
            if (slack > 0) { late = true; slack -= Rms; holdR = 2; }
            interval = budget + (late ? Rms : 0.0);
            fall.Picture45(late, now + interval / 1000.0);
        } else {
            slack = 0;
            holdR = cost <= 2.0 * Rms ? 2 : 3;
            interval = holdR * Rms;
            // np_fps.cpp's 45-fit test at the fallback 30
            const double est = work - steps * L.step + L.step * (50.0 * 4.0 / 3.0 * R);
            double lanes = hold ? (feMean > beMean ? feMean : beMean) : (fe > be ? fe : be) * 1.08;   // (old: the largest of the last three)
            bool fits = est <= 20.0 && L.gpu60 <= 20.0 && lanes <= 20.0;
            if (L.nfit > 0 && rng.U() < L.nfit) fits = false;
            if (!hold && L.nfitLane > 0 && rng.U() < L.nfitLane) fits = false;
            fall.Picture30(fits, now + interval / 1000.0);
            ctl.DeferUp(now + interval / 1000.0, 2.0);
        }
        prevHold = t == kTier60 ? holdR : (t == kTier45 ? (late ? 2 : 1) : 2);
        if (t == kTier60) { if (steps >= 2) ++twoStep60; if (holdR >= 2) ++repeats60; }
        now += interval / 1000.0;
        FrameIn f;
        std::memset(&f, 0, sizeof f);
        f.now = now; f.interval = interval / 1000.0; f.steps = steps; f.simMs = steps * L.step; f.costsValid = true; f.gameCore = 0;
        f.cpuMs[0] = work; f.cpuMs[1] = (fe > be ? fe : be) + 0.25 * interval; f.timeMs[1] = 0.25 * interval;   // (the render core also holds time-following helpers)
        f.scene = scene; f.gpuMinMs = L.gpu60; f.sceneClass = 0; f.ladderRoom = false;
        f.workMs = work; f.feMs = feMean; f.beMs = beMean; f.laneWaitMs = lw;
        ctl.Frame(f);
        ++pictures;
        st.Picture(t, interval / 1000.0, t == kTier60 && holdR > 1);
        const Tier after = Now();
        if (after != t) { ++changes; changeT.push_back(now); changeTo.push_back((int)after); changeScene.push_back((int)cuts); changeAtCut.push_back(justCut ? 1 : 0); }
        justCut = false;
    }
    void Run(double secs, const Load& L) { const double end = now + secs; while (now < end) Picture(L); }
    double Secs() const { return st.secs[0] + st.secs[1] + st.secs[2]; }
    double Share(int k) const { const double s = Secs(); return s > 0 ? st.secs[k] / s : 0; }
    int MaxIn(double win) const { int m = 0; for (size_t a = 0; a < changeT.size(); ++a) { int n = 0; for (size_t b = a; b < changeT.size() && changeT[b] < changeT[a] + win; ++b) ++n; if (n > m) m = n; } return m; }
    // the closest two tier changes inside one stretch of play (a cut into play is a scene of its own on the screen: a replay or a celebration sits between; the change it makes is the cut's)
    double MinGap() const {
        double m = 1e9;
        for (size_t a = 1; a < changeT.size(); ++a)
            if (changeScene[a] == changeScene[a - 1] && !changeAtCut[a - 1] && changeT[a] - changeT[a - 1] < m) m = changeT[a] - changeT[a - 1];
        return m;
    }
    double PerMin() const { const double s = Secs(); return s > 0 ? changes * 60.0 / s : 0; }
    double Fps60() const { return st.secs[0] > 0 ? (double)(st.pictures) : 0; }
};

static void Row(const char* name, const Sim& o, const Sim& n) {
    std::printf("%-44s old: 60/45/30 %4.0f/%4.0f/%4.0f %%, %4.2f changes/min | new: 60/45/30 %4.0f/%4.0f/%4.0f %%, %4.2f changes/min, closest two %.0f s\n", name, 100 * o.Share(0), 100 * o.Share(1),
                100 * o.Share(2), o.PerMin(), 100 * n.Share(0), 100 * n.Share(1), 100 * n.Share(2), n.PerMin(), n.changeT.size() > 1 ? n.MinGap() : 0.0);
}

// delivered pictures a second while at 60 (from the TierStats: pictures at 60 / seconds at 60 is not kept, so count in the run)
struct Rate60 { double pics = 0, secs = 0; };

static Load Vice() { Load L; L.base = 9.1; L.sd = 0.08; L.spikeP = 0.05; L.spikeMs = 7.0; L.step = 5.1; L.fe = 10.5; L.be = 9.8; L.gpu60 = 10.5; L.gpu45 = 12.0; L.gpu30 = 14.0; return L; }

struct Seg { std::string log; int stadium; double secs, p50, p95, step, fe, be, gpu, at30, lane = 0, at45 = 0; };
struct Total { std::string log; int stadium; double s60, s45, s30; };
struct Entry { std::string log; int n; double secs; };

static Load FromSeg(const Seg& s) {
    Load L;
    L.step = s.step;
    L.base = s.p50 - s.step; if (L.base < 2) L.base = 2;
    L.sd = 0.08;
    const double tail = s.p95 - s.p50;
    L.spikeP = 0.06; L.spikeMs = tail > 1.0 ? tail * 1.15 : 0.0;   // (the 1-step p95 sits inside the spike mass: the slowest 5 % are end-frame spikes, [gsplit-p95])
    L.fe = s.fe * 0.9; L.be = s.be * 0.9;   // ([late] means the largest of the last three)
    if (s.at30 >= 0.5) { L.gpu30 = s.gpu; L.gpu45 = s.gpu * 0.88; L.gpu60 = s.gpu * 0.75; }   // (728x512 floor at 60 against 960x544: 0.71 of the pixels, plus fixed work)
    else { L.gpu60 = s.gpu; L.gpu45 = s.gpu * 1.17; L.gpu30 = s.gpu * 1.33; }
    return L;
}


// ---- motion at the 60 tier (owner 23 Nov 17:57: "make sure that 60 looks smooth regardless"): the show-time clock (Clock45 on the every-vblank pattern + the display gate Gate45), the
// pipeline of tier_unit.cpp's Clock60 (start on the vblank after the frame before ended, back-pressure depth 2, the game thread's start jitter, the GPU after it), with a repeat pattern.
// Per picture: the game-time advance against the shown advance (refreshes). 0 = the picture shows the world of the vblank it is on. "own": each frame's start-to-start time (the time
// without the show clock). q: the quantile of start-to-ready refreshes the clock plans with (0.8 = today's; hold60 plans with 0.95 at 60).
struct Motion { unsigned pictures = 0, off = 0, repeats = 0; double absErr = 0, ownAbs = 0, p95 = 0, ownP95 = 0, maxErr = 0, lat = 0; };
static Motion Motion60(double q, int pattern, unsigned seed) {
    Gate45 gate(Gate45Config(), true);
    Clock45 clk;
    LatencyLearner learn;
    Motion r;
    const double V = Rms;
    Rng rng(seed);
    const int N = 6000;
    std::vector<double> rep(N + 1, 0), T(N + 1, 0), Town(N + 1, 0), start(N + 1, 0);
    std::vector<uint32_t> shown(N + 1, 0);
    double end = 0, gpuDone = 0, cbFree = 0;
    int fb = 0;
    std::vector<double> errs, own;
    for (int k = 1; k <= N; ++k) {
        double t = (std::floor(end / V) + 1.0) * V + 0.1;
        while (k > 3 && rep[k - 3] > t) t += V;
        t += 5.0 * rng.U();
        start[k] = t;
        const uint32_t stamp = (uint32_t)std::floor(t / V);
        while (fb + 1 < k && rep[fb + 1] <= t) ++fb;
        const int L = learn.Ready() ? learn.Quantile(q) : 2;
        const int adv = clk.Begin(true, stamp, L, fb > 0, (uint64_t)fb, fb > 0 ? shown[fb] : 0u, fb > 0 ? (int)Pattern45::kEvery : -1);
        uint32_t g = 0;
        const bool hasT = clk.NewestTarget(&g);
        const double ownA = k > 1 ? (start[k] - start[k - 1]) / V : 0.0;
        T[k] = T[k - 1] + (adv >= 0 ? adv : ownA);
        Town[k] = Town[k - 1] + ownA;
        double cost = 11.0 + 1.5 * (rng.U() - 0.5);
        if (pattern == 1 && k % 10 == 5) cost += 9.0;                         // one repeated refresh in ten pictures (54 a second)
        if (pattern == 2 && k % 180 < 6) cost += 9.0;                         // a burst of six every three seconds
        if (pattern == 3 && rng.U() < 0.06) cost += 4.0 + 8.0 * rng.U();     // 6 % random spikes of 4-12 ms (the console's end-frame spikes)
        end = t + cost;
        clk.Handed((uint64_t)k);
        gpuDone = std::max(end + 5.0, gpuDone) + 9.0 + 1.0 * (rng.U() - 0.5);
        const double cb = std::max(gpuDone, cbFree);
        const uint32_t earliest = (uint32_t)std::floor(cb / V) + 1u;
        learn.Add(earliest - stamp);
        const uint32_t due = gate.Due(earliest, hasT, g, false, 0);
        gate.Shown(due);
        shown[k] = due;
        cbFree = (due - 1) * V;
        rep[k] = std::max(cb, (due - 1) * V) + 0.5;
        if (k >= 300) r.lat += (double)(due - stamp);
    }
    for (int k = 300; k <= N; ++k) {
        ++r.pictures;
        const double dS = (double)(shown[k] - shown[k - 1]);
        if (dS >= 2) ++r.repeats;
        const double e = std::fabs((T[k] - T[k - 1]) - dS), o = std::fabs((Town[k] - Town[k - 1]) - dS);
        if (e > 1e-6) ++r.off;
        r.absErr += e; r.ownAbs += o; errs.push_back(e); own.push_back(o);
        if (e > r.maxErr) r.maxErr = e;
    }
    std::sort(errs.begin(), errs.end()); std::sort(own.begin(), own.end());
    r.p95 = errs[(size_t)(0.95 * errs.size())]; r.ownP95 = own[(size_t)(0.95 * own.size())];
    r.lat /= r.pictures;
    return r;
}

int main(int argc, char** argv) {
    const bool table = argc > 1 && !std::strcmp(argv[1], "table");
    const char* traces = argc > 2 ? argv[2] : "native/tools/fps/hold60_traces.txt";

    // ---- 1. Vice-like play (the console's 1-step work: p50 14.2 ms, 5 % end-frame spikes of +7 ms; lanes 10-11 ms; GPU 10.5 at the floor), 300 s from a cut into play ----
    for (int cpl = 0; cpl < 2; ++cpl) {
        Sim o(false, cpl, 11), n(true, cpl, 11);
        // the cut-in seen at every entry into play on the console: 20 pictures in 0.65-0.85 s at 60 (every judged one missed), the 45 then at its edge for ~3 s
        Load k1 = Vice(); k1.fe = 36.0; Load k2 = Vice(); k2.fe = 23.9;
        Load V = Vice(); V.nfitLane = 0.4;   // (H1: the console's 35-45 % of the 30's pictures failing the 45-fit test, in the lanes' largest-of-three)
        Sim* both[2] = {&o, &n};
        for (Sim* s : both) { s->Run(0.75, k1); s->Run(3.5, k2); s->Run(296.0, V); }
        char nm[64]; std::snprintf(nm, sizeof nm, "Vice-like 300 s from a cut-in (%s)", cpl ? "coupled" : "decoupled");
        if (table) Row(nm, o, n);
        check(n.Share(0) >= 0.85, "Vice-like: the hold keeps 60 at least 85 % of the time");
        check(o.Share(0) <= 0.10 && o.Share(2) >= 0.8, "Vice-like: the old tiers are at 60 at most 10 % and at 30 most of the time (the console's finding reproduced)");
        check(n.PerMin() <= 1.0 && n.MaxIn(60.0) <= 3, "Vice-like: no flapping with the hold (at most 1 change a minute, 3 in any 60 s)");
    }
    // ---- 2. a steady 54 pictures a second at 60 (11 % of the pictures 3 ms over a refresh): kept at 60 ----
    {
        Sim n(true, true, 3), o(false, true, 3);
        Load L = Vice(); L.base = 13.0; L.sd = 0.0; L.spikeP = 0.11; L.spikeMs = 7.0; L.step = 0.0;   // (coupled, no steps: 11 % repeated refreshes, 54 a second)
        n.Run(120.0, L); o.Run(120.0, L);
        if (table) Row("steady ~54 a second", o, n);
        check(n.changes == 0 && n.Now() == kTier60, "steady 54: the hold keeps 60 (no change)");
        check(o.changes >= 1, "steady 54: the old tiers leave 60");
    }
    // ---- 3. spikes: bursts of 10 heavy pictures every 8 s on a light load: never falls ----
    {
        Sim n(true, true, 5);
        Load L = Vice(); L.spikeP = 0.0;
        Load B = L; B.base = 22.0;
        for (int i = 0; i < 25; ++i) { n.Run(7.6, L); for (int k = 0; k < 10; ++k) n.Picture(B); }
        if (table) std::printf("bursts of 10 heavy pictures every 8 s (coupled): hold changes %u, at 60 %.0f %%\n", n.changes, 100 * n.Share(0));
        check(n.changes == 0, "bursts: the hold keeps 60 through bursts of 10 heavy pictures");
    }
    // ---- 4. sustained 40 fps load (25 ms pictures): 60 -> 45 within 3 s; 45 -> 30 (25 ms does not fit 22.2) and stays ----
    {
        Sim n(true, false, 7);
        Load L = Vice(); L.base = 25.0 - 5.1 * 0.834; L.spikeP = 0;
        n.Run(120.0, L);
        if (table) Row("sustained 40 a second (25 ms)", n, n);
        check(!n.changeT.empty() && n.changeTo[0] == kTier45 && n.changeT[0] < 3.0, "40 fps load: 60 -> 45 within 3 s");
        check(n.Now() == kTier30 && n.Share(0) < 0.05, "40 fps load: ends at the 30, under 5 % at 60");
        check(n.PerMin() <= 1.5, "40 fps load: no flapping");
    }
    // ---- 5. a GPU-bound load (GPU 21 ms at the floor: 47 a second at 60 even with the pipeline's slack; 21.5 at 45's size, 26 full size): 60 must go and not come back ----
    {
        Sim n(true, false, 9);
        Load L = Vice(); L.gpu60 = 21.0; L.gpu45 = 21.5; L.gpu30 = 26.0; L.base = 10.0;
        n.Run(300.0, L);
        if (table) Row("GPU-bound Dump-like", n, n);
        check(n.Share(0) < 0.05, "Dump-like: under 5 % at 60");
        check(n.changes <= 3, "Dump-like: at most 3 tier changes in 300 s");
    }
    // ---- 5b. GPU at 18.3 ms at the floor, decoupled (the pipeline's slack): ~55 a second at 60 is the owner's "close to 60" case: kept ----
    {
        Sim n(true, false, 19);
        Load L = Vice(); L.gpu60 = 18.3; L.gpu45 = 21.0; L.gpu30 = 25.0;
        n.Run(120.0, L);
        if (table) Row("GPU 18.3 ms at the floor (~55 a second)", n, n);
        check(n.Share(0) >= 0.95, "GPU at 55 a second: the hold keeps 60");
    }
    // ---- 6. recovery: too heavy for 30 s, then Vice-like for 150 s: climbs 30 -> 45 -> 60 in order, within 60 s, and stays ----
    {
        Sim n(true, false, 13), o(false, false, 13);
        Load H = Vice(); H.base = 26.0; Load V = Vice(); V.nfitLane = 0.4;
        n.Run(30.0, H); n.Run(150.0, V); o.Run(30.0, H); o.Run(150.0, V);
        if (table) { Row("recovery (heavy 30 s, then Vice-like)", o, n); for (size_t i = 0; i < n.changeT.size(); ++i) std::printf("   new %.1f s -> %d\n", n.changeT[i], n.changeTo[i] == 0 ? 60 : (n.changeTo[i] == 1 ? 45 : 30)); }
        bool order = true; for (size_t i = 1; i < n.changeTo.size(); ++i) if ((n.changeTo[i - 1] == kTier30 && n.changeTo[i] == kTier60)) order = false;
        double back60 = -1; for (size_t i = 0; i < n.changeT.size(); ++i) if (n.changeTo[i] == kTier60) back60 = n.changeT[i];
        check(n.Now() == kTier60 && order && back60 > 30.0 && back60 < 90.0, "recovery: back at 60 within 60 s of the load easing, in order 30 -> 45 -> 60");
        check(o.Now() != kTier60, "recovery: the old tiers stay off 60 (the console's 75 % proof and the lanes' largest-of-three)");
    }
    // ---- 7. a load hovering at the edge (alternating 10 s at 52 and 10 s at 46 a second, decoupled): bounded changes ----
    {
        Sim n(true, true, 17);
        Load A = Vice(); A.base = 12.0; A.sd = 0; A.step = 0; A.spikeP = 0.15; A.spikeMs = 6.0;
        Load B = A; B.spikeP = 0.30;
        for (int i = 0; i < 30; ++i) { n.Run(10.0, A); n.Run(10.0, B); }
        if (table) Row("edge: 10 s at ~52 / 10 s at ~46, 600 s", n, n);
        check(n.PerMin() <= 1.0 && n.MaxIn(60.0) <= 3 && (n.changeT.size() < 2 || n.MinGap() >= 8.0), "edge: at most 1 change a minute, 3 in any 60 s, none within 8 s of another");
    }


    // ---- 9. motion at 60 with repeated refreshes: every picture shows the world of the vblank it is on (show-time clock), the plan's quantile 0.8 (today) against 0.95 (hold60) ----
    {
        const char* pname[4] = {"none", "1 repeat in 10", "bursts of 6 every 3 s", "6 % random spikes"};
        for (int pat = 0; pat < 4; ++pat) {
            const Motion a = Motion60(0.8, pat, 21), b = Motion60(0.95, pat, 21);
            if (table) std::printf("motion at 60, %-22s repeats %4.1f %%: q0.8 off %5.2f %% mean %.3f p95 %.0f max %.0f lat %.2f | q0.95 off %5.2f %% mean %.3f p95 %.0f max %.0f lat %.2f | own time mean %.3f p95 %.0f\n",
                                   pname[pat], 100.0 * b.repeats / b.pictures, 100.0 * a.off / a.pictures, a.absErr / a.pictures, a.p95, a.maxErr, a.lat, 100.0 * b.off / b.pictures, b.absErr / b.pictures, b.p95, b.maxErr, b.lat,
                                   b.ownAbs / b.pictures, b.ownP95);
            check(b.maxErr <= 1.0 + 1e-9, "motion: no picture more than one refresh off its show time (0.95)");
            check(b.absErr <= a.absErr + 1e-9, "motion: the 0.95 plan is never worse than 0.8");
            check(b.absErr * 2.0 <= b.ownAbs + 1e-9, "motion: the show clock has at most half the own-time error");
            if (pat > 0) check(b.off <= b.repeats + 2, "motion (0.95): at most one picture off its show time per repeated refresh (the one that overran: drawn for the vblank it missed)");
            if (pat == 1) check(a.off * 10 >= 18 * b.repeats && b.off == 0, "motion: 1 repeat in 10: about two pictures off per repeat at 0.8, none at 0.95");
        }
    }

    // ---- 10. hold60b: the climb's proof prices a picture as the 60 runs it (a step on R / 20 ms = 83 % of the pictures): a picture over only with its step counts 0.83 ----
    {
        Auto c(Config::Lock60Hold());
        FrameIn f;
        std::memset(&f, 0, sizeof f);
        f.costsValid = true; f.scene = 2; f.steps = 1; f.simMs = 5.0; f.cpuMs[0] = 17.5; f.interval = 1.0 / 45.0; f.feMs = 9.0; f.beMs = 9.0; f.gpuMinMs = 10.0;
        double t = 0;
        f.now = t; c.Frame(f);
        c.SeedRelief(t, 1000.0);   // (at the relief, the way up shut: only the proof's count is looked at)
        for (int k = 0; k < 300; ++k) { t += f.interval; f.now = t; f.workMs = 17.5; f.laneWaitMs = 3.0; c.Frame(f); }   // with the step 17.5 (over 16.2), without it 12.5
        const double over1 = c.HoldProofOver();
        for (int k = 0; k < 300; ++k) { t += f.interval; f.now = t; f.workMs = 22.0; c.Frame(f); }                      // over with and without the step
        const double over2 = c.HoldProofOver();
        const Auto::HoldParts hp = c.HoldTakeParts();
        if (table) std::printf("step share: %d pictures judged, over %.1f of 240 (with the step only) then %.1f (both); lane wait mean %.1f\n", c.HoldProofN(), over1, over2, hp.n ? hp.lane / hp.n : 0.0);
        check(c.HoldProofN() == 240 && over1 > 190.0 && over1 < 210.0, "step share: a picture over only with its step counts as 0.83 of a picture (the 60's steps)");
        check(over2 > 239.0, "step share: a picture over without its step counts whole");
        check(hp.n == 600 && hp.by[0] + hp.by[1] == 600 && hp.lane > 0, "step share: the parts count every judged picture and its lane wait");
    }
    // ---- 11. hold60b: Vice as the console has it (s49: [gsplit-pic] 1-step work p50 13.3-14.2 ms, the blend lane's wait 3.4 a picture at 45, 0.8 at the paced 30): the 60 does not fit, the hold
    // stays at 45 and never climbs into a 60 that falls; with the lane's wait as at the paced 30 (what if) it climbs and stays ----
    {
        for (int what = 0; what < 2; ++what) {
            Sim n(true, true, 31);
            Load k1 = Vice(); k1.fe = 36.0; Load k2 = Vice(); k2.fe = 23.9;
            Load V = Vice(); V.base = 8.7; V.lane30 = 0.8; V.lane45 = what ? 0.8 : 3.4; V.lane60 = V.lane45;
            k1.lane30 = k2.lane30 = V.lane30; k1.lane45 = k2.lane45 = V.lane45; k1.lane60 = k2.lane60 = V.lane60;
            n.Run(0.75, k1); n.Run(3.5, k2); n.Run(296.0, V);
            if (table) { char nm[96]; std::snprintf(nm, sizeof nm, "Vice as the console, lane wait %.1f at 45/60", V.lane45); Row(nm, n, n); std::printf("   failed tries %u\n", n.ctl.HoldFails()); }
            check(n.ctl.HoldFails() <= (what ? 1u : 0u), "Vice as the console: no climb into a 60 that falls within 20 s (the what-if at its edge: at most one)");
            check(n.changeT.size() < 2 || n.MinGap() >= 8.0, "Vice as the console: no two tier changes within 8 s");
            if (!what) check(n.Share(0) <= 0.05 && n.Share(1) >= 0.9, "Vice as the console: the 60 does not fit with the lane's wait: at 45 (the console's finding)");
            else check(n.Share(0) >= 0.8, "Vice, the lane's wait as at the paced 30: the hold climbs and keeps 60");
        }
    }

    // ---- 12. hold60b, a late picture at 60 and its catch-up (coordinator, console s50 forced 60). A load whose one-step picture fits a refresh (9.8 ms + a 4.6 ms step = 14.4) and whose two-step
    // picture does not (19.0): twenty single late pictures, one a second. Without the spread the picture after each late one is given two refreshes of game time, runs two steps when the step
    // accumulator holds the remainder (two times in three), misses in turn: a second repeated refresh. With it every late picture costs exactly one repeated refresh and no picture runs two steps.
    // (The model's coupled 60: a picture over a refresh holds the next start to the vblank after.) ----
    {
        Load L = Vice(); L.base = 9.8; L.step = 4.6; L.sd = 0.03; L.spikeP = 0.0;
        Load hit = L; hit.base = L.base + 9.0;   // (one picture 9 ms heavier: it misses its refresh with or without a step)
        unsigned two[2] = {0, 0}, reps[2] = {0, 0};
        bool stay[2] = {false, false};
        for (int sp = 0; sp < 2; ++sp) {
            Sim n(true, true, 41);
            n.spread = sp == 1;
            n.Run(8.0, L);
            const unsigned two0 = n.twoStep60, rep0 = n.repeats60;
            for (int k = 0; k < 20; ++k) { n.Picture(hit); n.Run(1.0, L); }
            two[sp] = n.twoStep60 - two0; reps[sp] = n.repeats60 - rep0; stay[sp] = n.Now() == kTier60 && n.changes == 0;
            if (table) std::printf("20 single late pictures at 60 (1 step 14.4 ms, 2 steps 19.0), spread %s: %u pictures with two steps, %u held 2+ refreshes, 60 kept %s\n", sp ? "on " : "off",
                                   two[sp], reps[sp], stay[sp] ? "yes" : "no");
        }
        check(stay[1] && two[1] == 0 && reps[1] == 20, "spread: each late picture at 60 costs one repeated refresh, no picture runs two steps, 60 stays");
        check(two[0] >= 5 && reps[0] >= 25, "spread off: the catch-up's two-step pictures miss in turn (the model reproduces the second repeat)");
        // console s50 forced 60 at Vice: the one-step picture itself is 13.4 ms of work + 6.4-7.3 ms of the blend lane's wait ([gsplit] "other") = ~20 ms: no catch-up rule makes that fit; the
        // spread changes nothing there and the hold falls to 45 (the console's 30 a second at a forced 60 is this, not a catch-up spiral)
        Load F = L; F.base = 8.8; F.lane60 = 6.8; F.lane45 = 3.4; F.lane30 = 0.8;
        Sim n(true, true, 43);
        n.Run(30.0, F);
        if (table) std::printf("Vice at 60 as the console's forced 60 (1 step 13.4 + lane wait 6.8 ms), spread on: first change at %.1f s to %d, at 60 %.0f %%\n", n.changeT.empty() ? -1.0 : n.changeT[0],
                               n.changeTo.empty() ? 0 : (n.changeTo[0] == 1 ? 45 : 30), 100 * n.Share(0));
        check(!n.changeT.empty() && n.changeTo[0] == kTier45 && n.changeT[0] < 3.0 && n.Share(0) < 0.15, "forced-60 load: the spread does not hide a one-step picture over a refresh: 60 -> 45 within 3 s");
    }

    // ---- 8. console traces (hold60_extract.py: w8/console-1123 and the s49 hold60 runs 5141-5151): each run replayed from its first cut into play, with a cut into play (a goal) every 60 s.
    // hold60b: the blend lane's wait is in the work; at 60 it is priced as the run measured it at 45 (h 0, the console: the 60's game thread is blocked as the 45's) or as at the paced 30 (h 1,
    // what if the join did not wait at the 60). Coupled 60 pipeline (the console's: a picture over a refresh holds the next start to the vblank after) and decoupled. ----
    std::vector<Seg> segs;
    std::vector<Entry> ents;
    std::vector<Total> tots;
    if (FILE* fp = std::fopen(traces, "r")) {
        char line[512];
        while (std::fgets(line, sizeof line, fp)) {
            char kind[8], log[96];
            Seg s;
            if (std::sscanf(line, "%7s %95s", kind, log) != 2 || kind[0] == '#') continue;
            if (!std::strcmp(kind, "seg") && std::sscanf(line, "seg %95s %d %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf", log, &s.stadium, &s.secs, &s.p50, &s.p95, &s.step, &s.fe, &s.be, &s.gpu, &s.at30, &s.lane, &s.at45) >= 10) { s.log = log; segs.push_back(s); }
            Entry e;
            if (!std::strcmp(kind, "entry") && std::sscanf(line, "entry %95s %*d %d %lf", log, &e.n, &e.secs) == 3) { e.log = log; ents.push_back(e); }
            Total o;
            if (!std::strcmp(kind, "total") && std::sscanf(line, "total %95s %d %lf %lf %lf", log, &o.stadium, &o.s60, &o.s45, &o.s30) == 5) { o.log = log; tots.push_back(o); }
        }
        std::fclose(fp);
    }
    check(segs.size() >= 80, "traces: the console segments are there");
    // the lane's wait by tier, per run (the windows mostly at 45 / mostly at the 30); a run without one takes its stadium's mean
    struct LaneMean { double s[2] = {0, 0}; int n[2] = {0, 0}; };
    auto laneOf = [&](const std::string& log, int stadium, int k) {
        double a = 0, sa = 0; int na = 0, nb = 0;
        for (const Seg& g : segs) {
            const int kk = g.at45 >= 0.5 ? 1 : (g.at30 >= 0.5 ? 0 : -1);
            if (kk != k || g.lane <= 0) continue;
            if (g.log == log) { a += g.lane; ++na; }
            if (g.stadium == stadium) { sa += g.lane; ++nb; }
        }
        return na ? a / na : (nb ? sa / nb : 0.0);
    };
    double sum[2][2][2][3] = {}, chg[2][2][2] = {}, vice60[2][2] = {}, viceAll[2][2] = {}, vice60o[2][2] = {};   // [lane hypothesis][pipeline: coupled / decoupled][old/new][tier]
    unsigned fails[2][2] = {};
    double cmp60[2] = {0, 0}; int cmpN = 0;
    for (int h = 0; h < 2; ++h)
    for (int hyp = 0; hyp < 2; ++hyp) {
        std::string cur;
        for (size_t i = 0; i < segs.size();) {
            cur = segs[i].log;
            size_t j = i;
            while (j < segs.size() && segs[j].log == cur) ++j;
            const bool dump = segs[i].stadium == 4;
            const double l30 = laneOf(cur, segs[i].stadium, 0), l45 = laneOf(cur, segs[i].stadium, 1);
            // the 30's 45-fit test failures the console logged ("fallback proof N fit / M not"): Vice 35-45 %, The Dump 85-93 %. H1: in the lanes' largest-of-three (the hold's lane mean does
            // not see it). At the Dump the GPU makes most of it real: the hold sees the model's own GPU there.
            const double nf = dump ? 0.0 : 0.40;
            Sim o(false, hyp == 0, 100 + i), n(true, hyp == 0, 100 + i);
            Sim* both[2] = {&o, &n};
            auto withLane = [&](Load L, const Seg& g) {
                L.lane30 = l30; L.lane45 = h ? l30 : l45; L.lane60 = L.lane45;   // (what if: the join waits at 45 and 60 as little as at the paced 30)
                if (g.at45 >= 0.5) { if (!h) L.lane45 = L.lane60 = g.lane; } else if (g.at30 >= 0.5) L.lane30 = g.lane; else L.lane60 = g.lane;
                return L;
            };
            for (Sim* s : both) {
                const Seg& g0 = segs[i + 1 < j ? i + 1 : i];
                Load k1 = withLane(FromSeg(g0), g0); k1.fe = 36.0;
                Load k2 = withLane(FromSeg(g0), g0); k2.fe = 23.9;
                s->Run(0.75, k1); s->Run(3.5, k2);
                double t = 4.25, nextCut = 60.0;
                for (size_t k = i + 1; k < j; ++k) {
                    Load L = withLane(FromSeg(segs[k]), segs[k]);
                    L.nfitLane = nf;
                    double left = segs[k].secs;
                    // a window the console made mostly at 60 is a scene of its own (a Mega Strike, or a cut-in's 2 s): entered by a cut (LOCKED 60 starts it at 60), left by a cut into play
                    if (segs[k].at30 + segs[k].at45 < 0.5) {
                        const bool first = k == i + 1 || segs[k - 1].at30 + segs[k - 1].at45 >= 0.5, last = k + 1 == j || segs[k + 1].at30 + segs[k + 1].at45 >= 0.5;
                        if (first) s->Cut();
                        s->Run(left, L); t += left;
                        if (last) { s->Cut(); s->Run(0.75, k1); s->Run(3.5, k2); t += 4.25; nextCut = t + 60.0; }
                        continue;
                    }
                    while (left > 0) {
                        const double d = left < nextCut - t ? left : nextCut - t;
                        s->Run(d, L); t += d; left -= d;
                        if (t >= nextCut - 1e-9) { s->Cut(); s->Run(0.75, k1); s->Run(3.5, k2); t += 4.25; nextCut += 60.0; }
                    }
                }
            }
            for (int k = 0; k < 3; ++k) { sum[h][hyp][0][k] += o.st.secs[k]; sum[h][hyp][1][k] += n.st.secs[k]; }
            chg[h][hyp][0] += o.changes; chg[h][hyp][1] += n.changes;
            fails[h][hyp] += n.ctl.HoldFails();
            if (table) {
                char nm[128]; std::snprintf(nm, sizeof nm, "%s %s (%s, lane at 60 %.1f)", cur.c_str(), dump ? "Dump" : "Vice", hyp ? "decoupled" : "coupled", h ? l30 : l45); Row(nm, o, n);
                if (h == 0 && hyp == 0) for (const Total& q : tots) if (q.log == cur) std::printf("   the console (its own match, cut-ins and Mega Strikes included): 60/45/30 %.0f/%.0f/%.0f %% of %.0f s\n", 100 * q.s60 / (q.s60 + q.s45 + q.s30),
                                                                                        100 * q.s45 / (q.s60 + q.s45 + q.s30), 100 * q.s30 / (q.s60 + q.s45 + q.s30), q.s60 + q.s45 + q.s30);
            }
            if (getenv("HOLD60_CHANGES") && cur == getenv("HOLD60_CHANGES")) for (size_t q = 0; q < n.changeT.size(); ++q) std::printf("   new %.1f s -> %d (scene %d%s)\n", n.changeT[q], n.changeTo[q] == 0 ? 60 : (n.changeTo[q] == 1 ? 45 : 30), n.changeScene[q], n.changeAtCut[q] ? ", at the cut" : "");
            check(n.changeT.size() < 2 || n.MinGap() >= 8.0, "traces: no two tier changes of the hold within 8 s");
            // (hold60b: the change rate is printed, not checked: with the console's Mega Strikes as scenes of their own and a cut into play every 60 s, most changes are the cuts' (LOCKED 60
            //  starting each scene at 60, the cut-in's fall 2 s later); the flapping checks are the 8 s gap and the failed climbs)
            if (h == 0) check(n.ctl.HoldFails() == 0, "traces (the console's lane wait): no climb into a 60 that falls within 20 s");
            else check(n.ctl.HoldFails() <= 1, "traces (the lane's wait as at the paced 30): at most one climb into a 60 that falls within 20 s");
            if (!dump) { vice60[h][hyp] += n.st.secs[0]; viceAll[h][hyp] += n.Secs(); vice60o[h][hyp] += o.st.secs[0]; }
            if (h == 0 && hyp == 0) for (const Total& q : tots) if (q.log == cur && q.log >= std::string("514")) {   // the s49 hold60 runs: the model against the console's own time at 60
                const double c = q.s60 / (q.s60 + q.s45 + q.s30);
                check(n.Share(0) <= c + 0.10, "traces (s49 hold60 runs, the console's lane wait): the model's 60 is not above the console's own by more than 10 points");
                cmp60[0] += n.Share(0); cmp60[1] += c; ++cmpN;
            }
            i = j;
        }
        const double so = sum[h][hyp][0][0] + sum[h][hyp][0][1] + sum[h][hyp][0][2], sn = sum[h][hyp][1][0] + sum[h][hyp][1][1] + sum[h][hyp][1][2];
        std::printf("traces, lane wait at 60 %s, %s (all runs, %.0f s): old 60/45/30 %.0f/%.0f/%.0f %%, %.2f changes/min; new %.0f/%.0f/%.0f %%, %.2f changes/min, failed climbs %u\n",
                    h ? "as at the paced 30 (what if)" : "as at 45 (the console)", hyp ? "decoupled" : "coupled", so, 100 * sum[h][hyp][0][0] / so, 100 * sum[h][hyp][0][1] / so, 100 * sum[h][hyp][0][2] / so,
                    chg[h][hyp][0] * 60.0 / so, 100 * sum[h][hyp][1][0] / sn, 100 * sum[h][hyp][1][1] / sn, 100 * sum[h][hyp][1][2] / sn, chg[h][hyp][1] * 60.0 / sn, fails[h][hyp]);
        std::printf("   Vice runs only: at 60 old %.0f %%, new %.0f %%\n", viceAll[h][hyp] > 0 ? 100 * vice60o[h][hyp] / viceAll[h][hyp] : 0.0, viceAll[h][hyp] > 0 ? 100 * vice60[h][hyp] / viceAll[h][hyp] : 0.0);
    }
    {   // the console's own time at 60 in the s49 hold60 runs (cut-ins and Mega Strikes: the only 60 it reached)
        double c60 = 0, call = 0;
        for (const Total& q : tots) if (q.stadium != 4 && q.log >= std::string("514")) { c60 += q.s60; call += q.s60 + q.s45 + q.s30; }
        if (call > 0) std::printf("the console, s49 hold60 Vice runs: at 60 %.0f %% of %.0f s of live play (Mega Strikes and the 2 s after each cut into play)\n", 100 * c60 / call, call);
        if (cmpN) std::printf("the model with the console's lane wait (coupled) against the console, s49 hold60 runs: at 60 %.0f %% against %.0f %% (mean of %d runs)\n", 100 * cmp60[0] / cmpN, 100 * cmp60[1] / cmpN, cmpN);
    }
    check(viceAll[0][0] > 0 && vice60[0][0] / viceAll[0][0] <= 0.25, "traces (Vice, the console's lane wait, coupled): the hold is at 60 at most 25 % (the console: Mega Strikes and cut-ins)");
    check(viceAll[1][0] > 0 && vice60[1][0] / viceAll[1][0] >= 0.2 && vice60[1][0] / viceAll[1][0] >= 2.0 * vice60[0][0] / viceAll[0][0],
          "traces (Vice, the lane's wait as at the paced 30): the hold is at 60 at least 20 % and twice as much as with the console's lane wait");
    if (g_fail) { std::printf("hold60_unit: %d FAILED\n", g_fail); return 1; }
    std::printf("hold60_unit: all checks passed (60 kept down to ~51 a second, falls on a sustained shortfall and on a GPU limit, climbs back on measured work with the blend lane's wait and the 60's step share, no flapping, console traces)\n");
    return 0;
}
