// f45_unit.cpp (helper f45, 18 Nov 2026; Fable's review the same day: the lead, the front-end lane, the slow fallback window): the 45 FPS cadence (native/platform/compat/np_fps45.h) on
// virtual vblanks. A MODEL of the Vita pipeline, not the game: the game thread (cost per picture plus a cost per 20 ms step, started by np_fps.cpp's F45Gate: `lead` refreshes before the
// slot its picture is planned for, Lead45 learning the lead), the front end, the back end, the GPU, and the display callback that holds each picture to its vblank with Gate45; the game
// time of every frame from Clock45. Console figures come from the [f45] and [present] lines, never from here.
//   c++ -std=c++11 -O1 -Wall -Inative/platform/compat native/tools/fps/f45_unit.cpp -o f45_unit && ./f45_unit [table]
// Checks: the pattern 1, 1, 2 is held (every picture on a slot of the one phase, holds 1 and 2 only in a clean scene); the phase is kept across hitches and taken again only after a long
// stall (whose picture goes up at once); every picture shows the world at the vblank it is shown on (the game time between two shown pictures equals the refreshes between them, outside
// the pictures in flight behind a late one); the game runs at real time; the fallback to 30 has hysteresis (no flapping, against cost guesses that are wrong on purpose).
// NP_F45_BREAK=1..10 (np_fps45.h) must each make it fail: run_pace_tests.sh builds them.
#include "np_fps45.h"
#include "np_show_clock.h"   // LatencyLearner
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
using namespace npfps;

// (f50) the same model and checks for the 50: -DNP_UNIT_N=5 -DNP_UNIT_M=6 (default 3 / 4: the 45 as before). run_pace_tests.sh builds both.
#ifndef NP_UNIT_N
#define NP_UNIT_N 3
#define NP_UNIT_M 4
#endif
static const Pattern45 g_pat(NP_UNIT_N, NP_UNIT_M);
static const double kNominal = NP_UNIT_N == 3 ? 45.0 : 50.0;   // (the loops' picture rate; the pattern's own is g_pat.Fps())

static int g_fail = 0;
static void check(bool ok, const char* what) { if (!ok) { std::printf("FAIL: %s\n", what); ++g_fail; } }

static const double kV = 1001.0 / 60.0;   // ms a refresh

struct Rng { uint64_t s; explicit Rng(uint64_t v) : s(v) {} double U() { s = s * 6364136223846793005ull + 1442695040888963407ull; return (double)(s >> 11) / 9007199254740992.0; }
             double N() { double a = U(), b = U(); if (a < 1e-12) a = 1e-12; return std::sqrt(-2 * std::log(a)) * std::cos(6.283185307 * b); } };

struct Case {
    const char* name;
    double perPic, step, noise, be, gpu, gpuNoise;   // game thread ms a picture without its steps, ms a step; back end; GPU
    int depth;                                       // the gate's count: at most `depth` earlier pictures unshown while no slot is known (depth + 1 with a slot)
    int hitchEvery; double hitchMs;                  // every hitchEvery-th picture the game thread takes hitchMs more
    int stallAt; double stallMs;                     // one stall
    int pictures;
    double fe;                                       // the front-end lane (decoding the frame), ms; 0: none
    int startLead;                                   // 0: Lead45's own start (5); else the lead it starts with
};

struct Result {
    unsigned pictures = 0, predicted = 0, motionErr = 0, absErr = 0, offPattern = 0, resyncs = 0, stalls = 0, late = 0, slips = 0, planned = 0, holds[4] = {0, 0, 0, 0};
    unsigned phaseChanges = 0, wrongPhase = 0, raises = 0, lowers = 0, failedLowers = 0, lateAfter300 = 0, gateCut = 0;
    int lead = 0, minLeadSeen = 99;
    double speed = 0, meanAbsErr = 0, fps = 0;
    bool stallAtOnce = true;
    std::vector<double> lat;   // frame start to shown, ms
};

static Result Run(const Case& c, uint64_t seed = 11) {
    Rng rng(seed);
    Gate45 gate(Gate45Config(), false, g_pat);
    Clock45 clk(g_pat);
    LatencyLearner learn;
    Lead45Config lc;
    if (c.startLead) lc.startLead = c.startLead;
    Lead45 lead(lc);
    Result R;
    const int N = c.pictures;
    std::vector<double> start(N + 1, 0), rep(N + 1, 0), cbFree(N + 1, 0);
    std::vector<uint32_t> shown(N + 1, 0), stamp(N + 1, 0), G(N + 1, 0);
    std::vector<int> gValid(N + 1, 0);
    std::vector<double> T(N + 1, 0);   // the game time of frame k's picture, refreshes (cumulative)
    double end = 0, feDone = 0, beDone = 0, gpuDone = 0, acc = 0;
    double firstStart = -1;
    unsigned firstPhase = 99;
    int fbIdx = 0;                      // the newest picture whose report the game thread can see
    for (int k = 1; k <= N; ++k) {
        // the gate (np_fps.cpp F45Gate): whole refreshes until the vblank `lead` before the slot the picture will be planned for, and at most depth (+ 1 with a slot) pictures unshown
        double t = end;
        int L = learn.Ready() ? learn.Quantile(0.8) : 3;
        for (int waited = 0;; ++waited) {
            while (fbIdx + 1 < k && rep[fbIdx + 1] <= t) ++fbIdx;
            uint32_t slot = 0;
            const bool haveSlot = k > 1 && clk.NextSlot(L, fbIdx > 0, (uint64_t)fbIdx, fbIdx > 0 ? shown[fbIdx] : 0u, fbIdx > 0 ? (int)gate.Phase() : -1, &slot);
            const uint32_t vc = static_cast<uint32_t>(std::floor(t / kV));
            const int32_t early = haveSlot ? static_cast<int32_t>(slot - static_cast<uint32_t>(lead.Lead()) - vc) : 0;
            const bool timeWait = early > 0 && early <= 6;
            const bool countWait = fbIdx > 0 && (k - 1) > fbIdx + c.depth + (haveSlot ? 1 : 0);
            if (!timeWait && !countWait) break;
            if (waited >= 8) { ++R.gateCut; break; }
            t = (std::floor(t / kV) + 1.0) * kV + 0.1;
        }
        start[k] = t;
        if (firstStart < 0) firstStart = t;
        stamp[k] = static_cast<uint32_t>(std::floor(t / kV));
        while (fbIdx + 1 < k && rep[fbIdx + 1] <= t) ++fbIdx;
        L = learn.Ready() ? learn.Quantile(0.8) : 3;
        const int adv = clk.Begin(true, stamp[k], L, fbIdx > 0, (uint64_t)fbIdx, fbIdx > 0 ? shown[fbIdx] : 0u, fbIdx > 0 ? (int)gate.Phase() : -1);
        uint32_t g = 0;
        gValid[k] = clk.NewestTarget(&g) ? 1 : 0;
        G[k] = g;
        const double own = k > 1 ? (start[k] - start[k - 1]) / kV : 0.0;
        T[k] = (k > 1 ? T[k - 1] : 0.0) + (adv >= 0 ? (double)adv : own);
        if (adv >= 0) ++R.predicted;
        // the steps this picture runs (fixed 20 ms steps against the game time it advances)
        acc += (adv >= 0 ? adv : own) * kV;
        int steps = 0;
        while (acc >= 20.0 && steps < 5) { acc -= 20.0; ++steps; }
        double cost = c.perPic + c.step * steps + c.noise * rng.N();
        if (cost < 3) cost = 3;
        if (c.hitchEvery > 0 && k % c.hitchEvery == 0) cost += c.hitchMs;
        if (k == c.stallAt) cost += c.stallMs;
        end = t + cost;
        clk.Handed((uint64_t)k);
        // front end, back end, GPU, display callback
        feDone = std::max(end, feDone) + c.fe;
        const double beStart = std::max(feDone, beDone);
        beDone = beStart + c.be;
        double gpu = c.gpu + c.gpuNoise * rng.N();
        if (gpu < 2) gpu = 2;
        gpuDone = std::max(beDone, gpuDone) + gpu;
        const double cb = std::max(gpuDone, k > 1 ? cbFree[k - 1] : 0.0);
        const uint32_t earliest = static_cast<uint32_t>(std::floor(cb / kV)) + 1u;
        learn.Add(earliest - stamp[k]);
        const bool wasHave = gate.Have();
        const unsigned prevHold = wasHave ? g_pat.Hold(gate.LastShown(), gate.Phase()) : 0u;
        const uint32_t resyncs0 = gate.resyncs_;
        const uint32_t due = gate.Due(earliest, gValid[k] != 0, G[k], false, 0);
        if (wasHave && gate.resyncs_ == resyncs0) {   // (np_fps.cpp NpFps45Due -> F45Frame: the lead's evidence; the model applies it at once)
            const Lead45Verdict v = Lead45Verdict::Of(due, earliest, prevHold, due * kV - cb, gate.LastLate(), gate.LastPlanned(), lc.guardMs, kV);
            lead.Picture(v);
            if (v.late && k > 300) ++R.lateAfter300;
        }
        if (k > 30 && lead.Lead() < R.minLeadSeen) R.minLeadSeen = lead.Lead();
        if (k == c.stallAt + 1 || k == c.stallAt) { if (gate.stalls_ > R.stalls) { R.stalls = gate.stalls_; if (due != earliest) R.stallAtOnce = false; } }
        shown[k] = due;
        gate.Shown(due);
        if (!wasHave) firstPhase = gate.Phase();
        if (gate.Phase() != firstPhase && gate.stalls_ == 0) ++R.wrongPhase;
        cbFree[k] = (due - 1) * kV;
        rep[k] = std::max(cb, (due - 1) * kV) + 0.5;
        R.lat.push_back(due * kV - start[k]);
    }
    // statistics over the pictures after the first 30 (the start-up of the chain)
    double absSum = 0;
    int nAbs = 0;
    long base = 0;
    bool haveBase = false;
    for (int k = 31; k <= N; ++k) {
        ++R.pictures;
        if (k > 1) {
            const double dT = T[k] - T[k - 1], dS = (double)(shown[k] - shown[k - 1]);
            if (std::fabs(dT - dS) > 1e-6) ++R.motionErr;
        }
        if (gValid[k]) {
            const long e = (long)(int32_t)(shown[k] - G[k]);
            if (!haveBase) { base = 0; haveBase = true; }
            if (e != base) ++R.absErr;
            absSum += std::fabs((double)e);
            ++nAbs;
        }
    }
    R.meanAbsErr = nAbs ? absSum / nAbs : 0;
    R.offPattern = gate.offPattern_;
    R.resyncs = gate.resyncs_;
    R.late = gate.late_;
    R.slips = gate.slips_;
    R.planned = gate.planned_;
    for (int i = 0; i < 4; ++i) R.holds[i] = gate.holds_[i];
    R.phaseChanges = (unsigned)clk.phaseChanges_;
    R.raises = lead.raises_; R.lowers = lead.lowers_; R.failedLowers = lead.failed_; R.lead = lead.Lead();
    const double vblanks = (double)(shown[N] - shown[31]);
    R.speed = vblanks > 0 ? (T[N] - T[31]) / vblanks : 0;
    R.fps = vblanks > 0 ? (N - 31) / (vblanks * kV / 1000.0) : 0;
    (void)firstStart;
    return R;
}

static double Pct(std::vector<double> v, double p) { if (v.empty()) return 0; std::sort(v.begin(), v.end()); return v[(size_t)(p * (v.size() - 1))]; }

static void Print(const Case& c, const Result& r) {
    std::printf("%-22s %5u pictures %.2f fps; holds 1/2/3+ %u/%u/%u; slips %u, late %u, held to target %u; off the pattern %u; phase taken %u (stalls %u); motion errors %u (%.2f %%), "
                "show-time errors %u (mean %.3f refreshes); game speed %.2f %%; start to shown p5 %.1f p50 %.1f p95 %.1f ms; lead now %d (raised %u, lowered %u, failed %u)\n",
                c.name, r.pictures, r.fps, r.holds[1], r.holds[2], r.holds[3], r.slips, r.late, r.planned, r.offPattern, r.resyncs, r.stalls, r.motionErr,
                r.pictures ? 100.0 * r.motionErr / r.pictures : 0.0, r.absErr, r.meanAbsErr, r.speed * 100.0, Pct(r.lat, 0.05), Pct(r.lat, 0.5), Pct(r.lat, 0.95), r.lead, r.raises, r.lowers, r.failedLowers);
}

// ---- the fallback's hysteresis -------------------------------------------------------------------------------------------------------------------------------------------------------
// A scene whose share of late pictures swings around the limit (6 % / 11 % in alternating 4 s stretches) and whose estimated cost at 30 always "fits" (a guess that is wrong on purpose):
// in 10 minutes the rate may change only a few times, the 30 is held at least the minimum after a fallback, and the third fallback latches.
static void FallbackChecks(bool table) {
    Fallback45 fb;
    const double dt = 1.0 / kNominal;
    double now = 0;
    unsigned changes = 0;
    double lastFall = -1e9, minHeld = 1e9;
    Rng rng(5);
    for (int i = 0; now < 600.0; ++i) {
        const bool heavy = ((int)(now / 4.0)) % 2 == 1;
        if (fb.At45()) {
            const bool late = rng.U() < (heavy ? 0.11 : 0.06);
            if (fb.Picture45(late, now)) { ++changes; lastFall = now; }
            now += dt;
        } else {
            if (fb.Picture30(true, now)) { ++changes; if (now - lastFall < minHeld) minHeld = now - lastFall; }
            now += 1.0 / 30.0;
        }
    }
    if (table) std::printf("fallback: swinging scene, 10 min: %u rate changes (%u fallbacks, %u returns), shortest 30 hold %.1f s\n", changes, fb.Fallbacks(), fb.Returns(), minHeld);
    check(fb.Fallbacks() >= 1, "fallback: a scene with 11 % late pictures falls back to 30");
    check(changes <= 5, "fallback: no flapping (at most 5 rate changes in 10 minutes of a scene swinging around the limit)");
    check(minHeld >= 25.0 - 1e-6, "fallback: the 30 is held at least 25 s after a fallback");
    check(fb.Fallbacks() <= 3 && fb.Latched(), "fallback: the third fallback latches the 30 for the match (no fourth)");
    fb.MatchEnd();
    check(fb.At45() && !fb.Latched(), "fallback: a new match starts at 45");
    // a clean scene never falls back; one late picture in a window does not either
    {
        Fallback45 f2;
        double t = 0;
        for (int i = 0; i < (int)kNominal * 120; ++i, t += dt) f2.Picture45(i % 100 == 50, t);
        check(f2.At45() && f2.Fallbacks() == 0, "fallback: 1 % late pictures never fall back");
    }
    // (Fable) a steady 5 % of pictures off their slot (under the fast limit, about two 50 ms holds a second) goes to 30 within 20 s; 2 % never does
    {
        Fallback45 f4, f5;
        double t = 0;
        for (int i = 0; i < (int)kNominal * 20 && f4.At45(); ++i, t += dt) f4.Picture45(i % 20 == 7, t);
        check(!f4.At45(), "fallback: a steady 5 % of pictures off their slot falls back to 30 within 20 s (the slow window)");
        t = 0;
        for (int i = 0; i < (int)kNominal * 120; ++i, t += dt) f5.Picture45(i % 50 == 7, t);
        check(f5.At45(), "fallback: a steady 2 % never falls back");
    }
    // a heavy scene falls back within a few seconds, comes back only with 12 s of proof after the 25 s hold, and not while the proof fails
    {
        Fallback45 f3;
        double t = 0;
        while (f3.At45() && t < 30) { f3.Picture45(true, t); t += dt; }
        check(!f3.At45() && t < 4.0, "fallback: every picture late -> 30 within 4 s");
        const double fellAt = t;
        bool back = false;
        while (t < fellAt + 200 && !back) { back = f3.Picture30(((int)(t * 30)) % 4 != 0, t); t += 1.0 / 30.0; }   // 25 % of pictures do not fit: no proof
        check(!back, "fallback: no return while a quarter of the pictures do not fit at 45");
        double t2 = t;
        while (t2 < t + 60 && !back) { back = f3.Picture30(true, t2); t2 += 1.0 / 30.0; }
        check(back && t2 - t >= 12.0 - 0.05, "fallback: the return needs 12 s of proof");
    }
}

int main(int argc, char** argv) {
    const bool table = argc > 1 && !std::strcmp(argv[1], "table");
    //                 name                   perPic step  noise be   gpu  gpuN depth hitchE hitchMs stallAt stallMs  pictures
    const Case clean = {"clean (fits)",        10.0, 6.0, 0.5, 6.0, 13.0, 1.0, 2, 0, 0, 0, 0, 3000, 0, 0};
    const Case vice  = {"console-like play",   13.3, 6.5, 1.5, 6.0, 14.0, 1.5, 2, 0, 0, 0, 0, 6000, 0, 0};
    const Case hitch = {"hitch every 400",     12.0, 6.5, 1.0, 6.0, 14.0, 1.0, 2, 400, 45.0, 0, 0, 6000, 0, 0};
    const Case stall = {"one 600 ms stall",    12.0, 6.5, 1.0, 6.0, 14.0, 1.0, 2, 0, 0, 2000, 600.0, 4000, 0, 0};
    const Case heavy = {"too heavy for 45",    18.0, 7.0, 2.0, 6.0, 15.0, 2.0, 2, 0, 0, 0, 0, 3000, 0, 0};
    // (Fable) the lanes the console logs show at Vice (ESTIMATES from the 30 FPS [lat] / [lane] lines of 17 Nov: pad to hand-over 16.5 with one step and 20 with two, hand-over to
    // decoded 15, decoded to presented 9.5, GPU 15): about 60 ms from a frame's start to its picture being ready, 3.6 refreshes
    const Case pipe  = {"console pipeline",    13.1, 3.4, 1.6, 9.5, 15.0, 1.5, 2, 0, 0, 0, 0, 9000, 15.0, 0};
    const Case pipe3 = {"same, lead from 3",   13.1, 3.4, 1.6, 9.5, 15.0, 1.5, 2, 0, 0, 0, 0, 9000, 15.0, 3};
    const Case fast  = {"fast pipeline",        8.0, 3.0, 0.8, 3.0, 9.0, 0.8, 2, 0, 0, 0, 0, 9000, 4.0, 0};
    const Result a = Run(clean), b = Run(vice), h = Run(hitch), s = Run(stall), x = Run(heavy), pc = Run(pipe), p3 = Run(pipe3), fa = Run(fast);
    if (table) { Print(clean, a); Print(vice, b); Print(hitch, h); Print(stall, s); Print(heavy, x); Print(pipe, pc); Print(pipe3, p3); Print(fast, fa); }
    // 1. the pattern is held, exactly 1, 1, 2, in a scene that fits
    check(a.offPattern == 0 && a.slips == 0 && a.holds[3] == 0, "clean: every picture on its next slot (no slip, no hold of 3)");
    check(std::abs((int)a.holds[1] - (int)(NP_UNIT_N - 1) * (int)a.holds[2]) <= (int)NP_UNIT_N, "clean: holds of 1 and 2 in the pattern's ratio (N-1 : 1)");
    check(std::fabs(a.fps - g_pat.Fps()) < 0.05, "clean: the pattern's pictures a second (44.96 at 45, 49.95 at 50)");
    // 2. never off the pattern, whatever the load; the phase is taken once (and again only after the long stall)
    check(b.offPattern == 0 && h.offPattern == 0 && s.offPattern == 0 && x.offPattern == 0, "every picture on a slot of the pattern (console-like, hitches, stall, too heavy)");
    check(b.resyncs == 1 && h.resyncs == 1 && x.resyncs == 1 && b.wrongPhase == 0 && h.wrongPhase == 0, "the phase is kept across hitches (taken once)");
    check(h.late >= 10 && h.late < 200, "hitches: the late pictures are there (the case is what it claims)");
    check(s.stalls == 1 && s.resyncs == 2 && s.stallAtOnce, "long stall: the pattern starts again once, and the stalled picture goes up at once");
    check(h.holds[3] <= h.slips, "hitches: a picture off its next slot costs that slot only (no hold of 3 beyond the slips)");
    // 3. every picture shows the world at the vblank it is shown on
    check(a.motionErr == 0 && a.absErr == 0, "clean: the game time of every picture is its show vblank (no motion error)");
    check(b.pictures > 0 && b.motionErr * 100 <= b.pictures * 2, "console-like: motion errors only behind late pictures (at most 2 % of pictures)");
    check(b.predicted * 10 >= (unsigned)vice.pictures * 9, "console-like: at least 90 % of frames have a predicted show time (the clock is in force)");
    check(h.motionErr <= 4 * h.late + 4, "hitches: at most the pictures in flight behind each late one carry a motion error");
    // 4. real time
    check(std::fabs(a.speed - 1.0) < 0.01 && std::fabs(b.speed - 1.0) < 0.02 && std::fabs(h.speed - 1.0) < 0.02, "the game runs at real time (within 2 %)");
    // 5. a scene too heavy for 45: still on the pattern (late pictures take later slots), and the fallback sees it
    {
        Fallback45 fb;
        Gate45 g;
        (void)g;
        double t = 0;
        unsigned late = 0;
        for (unsigned i = 0; i < 200; ++i) { const bool l = (i % 5) == 0; late += l; fb.Picture45(l, t); t += 1.0 / kNominal; }   // (20 % late, the share the heavy case shows)
        check(!fb.At45(), "too heavy: a 20 % late share falls back to 30");
        check(x.slips * 100 >= (unsigned)heavy.pictures * 8, "too heavy: the model's case slips on more than 8 % of pictures (what the fallback is for)");
    }
    // 6. (Fable) the lead: every picture has the same start-to-screen time; it goes up when the pipeline needs it, comes down only on proof, and does not flap
    {   // (the last 1000 pictures: after the lead has settled)
        const std::vector<double> tail(a.lat.end() - 1000, a.lat.end()), ptail(pc.lat.end() - 1000, pc.lat.end());
        check(Pct(tail, 0.95) - Pct(tail, 0.05) < 10.0 && Pct(tail, 0.95) <= a.lead * kV + 0.5 && a.gateCut == 0, "clean: every picture's start to screen is the lead (within the game thread's own spill, under 10 ms)");
        // (f50: at 50 the game thread has 20 ms an average slot, not 22.2: this case is 16.5 ms of work, so a few percent of frames start a refresh after their lead, and the spread is allowed one refresh)
    check(Pct(ptail, 0.95) - Pct(ptail, 0.05) < (NP_UNIT_N == 3 ? 10.0 : kV + 1.0) && Pct(ptail, 0.95) <= pc.lead * kV + 0.5, "console pipeline: every picture's start to screen is the lead (within the game thread's spill)");
    }
    check(pc.late * 200 <= (unsigned)pipe.pictures && pc.failedLowers == 0 && pc.lead >= 4, "console pipeline: at most 0.5 % of pictures late, no lowering that failed");
    check(pc.offPattern == 0 && p3.offPattern == 0 && fa.offPattern == 0, "console / fast pipeline: every picture on a slot");
    check(p3.lead >= 4 && p3.raises >= 1 && p3.lateAfter300 * 200 <= (unsigned)pipe3.pictures, "a lead too short for the pipeline goes up within 300 pictures (few late pictures after them)");
    check(fa.lead == 3 && fa.late * 500 <= (unsigned)fast.pictures && Pct(fa.lat, 0.5) < 3.0 * kV + 1.0, "fast pipeline: the lead comes down to 3 refreshes (50 ms start to screen) without late pictures");
    check(fa.failedLowers == 0, "fast pipeline: no lowering that failed");
    FallbackChecks(table);
    if (g_fail) { std::printf("f45_unit: %d FAILED\n", g_fail); return 1; }
    std::printf("f45_unit: all checks passed (pattern held, phase kept, show-time game time, real time, fallback hysteresis)\n");
    return 0;
}
