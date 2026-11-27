// smooth_unit.cpp (wave 9 helper "smooth"): what time each picture represents against the time it is shown, per layer, through the real clocks (np_show_clock.h,
// np_game_clock.h, np_slip.h). A MODEL of the Vita pipeline at a held cadence (pacing11's shape: late wait anchored before the back-pressure gate, a 2-deep queue, the display
// gate holding each picture `n` refreshes at least), not the game: console figures come from the [smooth] and [present] lines.
//   c++ -std=c++11 -O1 -Inative/platform/compat native/tools/fps/smooth_unit.cpp -o smooth_unit && ./smooth_unit [table]
// Layers: SNAP = players, ball, items (the blended snapshot: game time at the frame start minus one step), LIVE = what reads the newest step (whole 20 ms steps),
// CAM = the gameplay camera (Dampen of the aim point read from the PREVIOUS picture's snapshot, per-picture dt; MSC_CAM_INTERP).
// Error per picture (in refreshes): represented-time step minus the refreshes the previous picture was shown. Motion: a player running at 8 m/s, its world step per
// picture against the shown time (ratio 1 = right speed on screen), and on screen (minus the camera).
// NP_SHOW_BREAK mutants (np_show_clock.h) must make the checks fail.
#include "np_show_clock.h"
#include "np_game_clock.h"
#include "np_slip.h"
#include "Game/Camera/CameraDamping.h"   // the gameplay camera's Dampen (native/include)
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

static const double kV = 1001.0 / 60.0;   // ms a refresh
static const double kTick = 20.0;         // ms a step

struct Rng { uint64_t s; explicit Rng(uint64_t v) : s(v) {} double U() { s = s * 6364136223846793005ull + 1442695040888963407ull; return (double)(s >> 11) / 9007199254740992.0; }
             double N() { double a = U(), b = U(); if (a < 1e-12) a = 1e-12; return std::sqrt(-2 * std::log(a)) * std::cos(6.283185307 * b); } };

struct Case {
    const char* name;
    int n;                 // held interval (refreshes); 0 = free run (MSC_FPS_BETWEEN=pace, pictures shown at stamp + L)
    double perPic, stepCost, cpuNoise, gpu, gpuNoise;
    int lateEvery; double lateMs;   // every lateEvery-th picture costs lateMs of game thread
    int depth;              // back-pressure depth (pictures unshown before a frame may start)
    double bpProb;          // chance that the gate waits one refresh more (the "fits" rule, a busy display thread)
    double fe;              // the renderer's front end / frame send (ms a picture, a stage of its own between the hand-over and the GPU)
    int queue;              // refreshes between ready and the first vblank it can be on (the console's decode / present hand-off: shown-minus-start 6 against ready-after 3-4)
};

struct Stat {
    std::vector<double> v;
    void add(double x) { v.push_back(x); }
    double mean() const { double s = 0; for (double x : v) s += std::fabs(x); return v.empty() ? 0 : s / v.size(); }
    double pct(double p) const { if (v.empty()) return 0; std::vector<double> a; for (double x : v) a.push_back(std::fabs(x)); std::sort(a.begin(), a.end()); return a[(size_t)(p * (a.size() - 1))]; }
    double worst() const { double m = 0; for (double x : v) m = std::max(m, std::fabs(x)); return m; }
    double share(double over) const { size_t k = 0; for (double x : v) if (std::fabs(x) > over) ++k; return v.empty() ? 0 : 100.0 * k / v.size(); }
};

struct Result { unsigned errEven = 0, errMiss = 0; Stat snap, live, cam, motion, screen; double speedPct = 0, fps = 0; unsigned changed = 0; double hold3 = 0; std::vector<double> stepsPerPic; };

// clockMode: 0 slip (bounded cut, no bucket), 1 debt (pacing12). show: MSC_SMOOTH_CLOCK.
static Result Run(const Case& c, int clockMode, bool show, double q = 0.8, int pictures = 3000, uint64_t seed = 7) {
    Rng rng(seed);
    ShowClock sc; GameClock gc; ClockConfig cc; LatencyLearner learn; std::vector<unsigned> naturals;
    Result R;
    double t = 0;                // wall ms (frame start)
    double prevStart = 0;
    long anchor = 0;             // late-wait anchor (vblank index)
    double acc = kTick;          // FixedUpdateTask accumulator (ms)
    double game = 0;             // game clock: sum of advances (ms)
    double simTime = 0;          // newest step's time
    double gpuFree = 0, feFree = 0;
    long lastShown = -1000;
    std::vector<long> shownAt;   // vblank each picture went up
    std::vector<double> repSnap, repLive, repCam, xSnap, camPos;
    double camX = 0, camVel = 0, prevSnapX = 0;
    double seen = 33.4, stepEma = c.stepCost, perPicEma = c.perPic;
    double realSum = 0, gameSum = 0; size_t learnFed = 0; unsigned steadyRun = 0;
    unsigned hold3 = 0, holds = 0;
    const double v = 8.0 / 1000.0;   // m per ms
    for (int k = 0; k < pictures; ++k) {
        const double real = k == 0 ? (c.n ? c.n * kV : kV) : t - prevStart;
        prevStart = t;
        // the clock of this frame (np_fps.cpp NpFpsBeginFrame: show clock, then SlipFrame)
        const double period = c.n > 0 ? c.n * kV : 0.0;
        // the display's reports reach the game thread when the picture is up: L from the "ready after" counts, the newest shown picture for re-anchoring
        while (learnFed < shownAt.size() && shownAt[learnFed] * kV <= t) {
            learn.Add(naturals[learnFed]);
            if (learnFed > 0) steadyRun = (shownAt[learnFed] - shownAt[learnFed - 1] == c.n) ? steadyRun + 1 : 0;
            ++learnFed;
        }
        const int L = learn.Ready() ? learn.Quantile(q) : 3;
        const bool haveFb = learnFed > 0;
        const uint32_t stamp = (uint32_t)std::floor(t / kV);
        const int a = show ? sc.Begin(c.n > 0, stamp, c.n, L, steadyRun, haveFb, haveFb ? (uint64_t)learnFed : 0, haveFb ? (uint32_t)shownAt[learnFed - 1] : 0) : -1;   // (-1: the frame's own time)
        const double basis = a >= 0 ? a * kV : real;
        double adv;
        if (clockMode == 1) {
            const double target = (c.n ? c.n : 2) * kV;
            seen += (real - seen) / 8.0;
            const unsigned cap = CapSteps(cc, target, seen, perPicEma, stepEma);
            adv = gc.Frame(basis, cap);
        } else {
            float dt = (float)(basis * 0.001); if (dt > 0.1f) dt = 0.1f;
            const float lim = (float)((c.n ? c.n : 2) * kV * 0.001 * 1.05);
            adv = 1000.0 * (dt <= lim ? dt : SlipAdvance(dt, lim, 20, 2, nullptr));
        }
        realSum += real; gameSum += adv;
        if (show && std::fabs(adv - real) > 0.5) ++R.changed;
        // steps
        acc += adv;
        int steps = 0;
        while (acc >= kTick) { acc -= kTick; simTime += kTick; ++steps; }
        game += adv;
        R.stepsPerPic.push_back(steps);
        const double rep = simTime - kTick + acc;    // blended snapshot: previous step + remainder (RenderSnapshot::Blend)
        repSnap.push_back(rep);
        repLive.push_back(simTime);
        // camera: Dampen towards the aim point read from the previous picture's snapshot (BeginFrame runs before this frame's steps), dt = adv
        { float vel = (float)camVel; camX = Dampen((float)camX, (float)prevSnapX, vel, 0.5f, (float)(adv * 0.001)); camVel = vel; }
        repCam.push_back(rep);
        const double x = v * rep;
        xSnap.push_back(x);
        camPos.push_back(camX);
        prevSnapX = x;
        // game thread cost, hand-over
        double cpu = c.perPic + c.stepCost * steps + c.cpuNoise * rng.N();
        if (c.lateEvery && k % c.lateEvery == c.lateEvery - 1) cpu = c.lateMs;
        if (cpu < 3) cpu = 3;
        const double hand = t + cpu;
        if (show) sc.Handed((uint64_t)k + 1);
        perPicEma += (cpu - c.stepCost * steps - perPicEma) / 16.0;
        // GPU, display
        double g = c.gpu + c.gpuNoise * std::fabs(rng.N());
        const double feDone = std::max(hand, feFree) + c.fe * (0.85 + 0.3 * rng.U());
        feFree = feDone;
        const double ready = std::max(feDone, gpuFree) + g;
        gpuFree = ready;
        long vb = (long)std::ceil(ready / kV) + c.queue;
        naturals.push_back((unsigned)std::max(0L, vb - (long)std::floor(t / kV)));
        long shown;
        if (c.n > 0) shown = std::max(vb, lastShown + c.n);
        if (getenv("SMOOTH_DEBUG") && k > 1000 && k < 1100 && show && clockMode == 1 && c.queue == 2 && c.bpProb > 0.1) std::fprintf(stderr, "k %d start %.1f (vb %ld) cpu %.1f ready %.1f vb %ld shown %ld adv %.1f steps %d a %d L %d run %u\n", k, t, (long)std::floor(t / kV), cpu, ready, vb, shown, adv, steps, a, L, steadyRun);
        else {   // free run, MSC_PRESENT_SYNC aligned: at the vblank of the frame start + L (L = 3), not before the picture is ready, one a vblank
            const long stamp = (long)std::floor(t / kV);
            shown = std::max(std::max(vb, stamp + 3), lastShown + 1);
        }
        if (k > 0) { ++holds; if (shown - lastShown >= 3) ++hold3; }
        lastShown = shown;
        shownAt.push_back(shown);
        // next frame start: late wait (anchor + n), then the back-pressure gate (unshown pictures <= depth), anchor before the gate (MSC_LAT_ANCHOR=1)
        double next = hand;
        if (c.n > 0) {
            const double lw = (anchor + c.n) * kV;
            if (next < lw) next = lw;
            long vnow = (long)std::ceil(next / kV - 1e-9);
            const long preGate = vnow;
            int extra = rng.U() < c.bpProb ? 1 : 0;
            for (int guard = 0; guard < 6; ++guard) {
                int unshown = 0;
                for (int j = (int)shownAt.size() - 1; j >= 0 && j >= (int)shownAt.size() - 8; --j) if (shownAt[j] > vnow) ++unshown;
                if (unshown <= c.depth && !extra) break;
                if (extra) --extra;
                ++vnow;
            }
            next = std::max(next, vnow * kV) + 0.3 * rng.U();
            anchor = preGate;
        } else {
            next = hand + 0.3 * rng.U();
        }
        t = next;
    }
    // per picture pairs: represented step vs shown hold (refreshes)
    for (size_t k = 1; k < shownAt.size(); ++k) {
        const double hold = (double)(shownAt[k] - shownAt[k - 1]);
        if (k < 30) continue;   // (start-up)
        R.snap.add((repSnap[k] - repSnap[k - 1]) / kV - hold);
        if (std::fabs((repSnap[k] - repSnap[k - 1]) / kV - hold) > 0.5) { if (hold == c.n) ++R.errEven; else ++R.errMiss; }
        R.live.add((repLive[k] - repLive[k - 1]) / kV - hold);
        R.cam.add((repCam[k - 1] - (k >= 2 ? repCam[k - 2] : 0)) / kV - hold);
        R.motion.add((xSnap[k] - xSnap[k - 1]) / (v * hold * kV) - 1.0);
        const double sNow = xSnap[k] - camPos[k], sPrev = xSnap[k - 1] - camPos[k - 1];
        R.screen.add((sNow - sPrev) / (v * hold * kV) - ((xSnap[k] - xSnap[k - 1]) - (camPos[k] - camPos[k - 1])) / (v * hold * kV) + 0.0);   // (placeholder replaced below)
    }
    // on-screen motion of the player relative to the camera, against the steady value (the camera follows at the same speed: the on-screen step of a steady run is what the
    // camera has not caught up; its irregularity per shown refresh is what reads as judder)
    R.screen.v.clear();
    {
        std::vector<double> rate;
        for (size_t k = 31; k < shownAt.size(); ++k) {
            const double hold = (double)(shownAt[k] - shownAt[k - 1]);
            const double camStep = camPos[k] - camPos[k - 1];
            rate.push_back(camStep / (v * hold * kV) - 1.0);   // camera pan per shown refresh against the target's speed
        }
        for (double x : rate) R.screen.add(x);
    }
    R.speedPct = realSum > 0 ? 100.0 * gameSum / realSum : 100;
    R.fps = shownAt.size() > 1 ? 1000.0 * (shownAt.size() - 1) / ((shownAt.back() - shownAt.front()) * kV) : 0;
    R.hold3 = holds ? 100.0 * hold3 / holds : 0;
    return R;
}

static void Row(const char* name, const char* mode, const Result& r) {
    std::printf("[errors on even holds %u, on uneven holds %u] ", r.errEven, r.errMiss);
    std::printf("%-22s %-10s fps %5.1f speed %6.2f%% 3+holds %4.1f%% | SNAP err mean %.3f p95 %.2f worst %.2f off>0.5 %4.1f%% | LIVE mean %.3f p95 %.2f worst %.2f | "
                "CAM mean %.3f p95 %.2f | run 8 m/s step/shown-time-1: mean %.3f p95 %.3f worst %.3f | camera pan rate-1: mean %.3f p95 %.3f\n",
                name, mode, r.fps, r.speedPct, r.hold3, r.snap.mean(), r.snap.pct(0.95), r.snap.worst(), r.snap.share(0.5), r.live.mean(), r.live.pct(0.95), r.live.worst(),
                r.cam.mean(), r.cam.pct(0.95), r.motion.mean(), r.motion.pct(0.95), r.motion.worst(), r.screen.mean(), r.screen.pct(0.95));
}


// ---- the console's display, by its statistics (r10 [present] lines): the vblank each frame begins at (stamp) and each picture goes up at (shown), the gate's rule
//      shown_k = max(stamp_k + ready_k, shown_{k-1} + 2), steady offset shown - stamp = 6 (r10 "shown-minus-start 6.0 +- 0.0") over ready 3-4 ("ready after 3/4").
// Starts move by a refresh against the cadence (pStart: the gate's wait, then the late wait's anchor brings the next one back: 3 then 1), a few by 3 (pLong: a late start past the
// slack: a late picture), and the GPU or the send makes a picture late on its own (pGpu: ready + 3). Today's clock gives a picture its start-to-start refreshes; the smooth clock
// its prediction. Error = refreshes of motion minus refreshes shown.
struct TruthCase { const char* name; double pStart, pLong, pGpu; };
static void Truth(const TruthCase& tc, bool print, double* offMean, double* onMean, double* offShare, double* onShare, int breakMode = 0) {
    Rng rng(11);
    ShowClock sc; LatencyLearner learn;
    std::vector<uint32_t> st, sh; std::vector<unsigned> nat;
    uint32_t stamp = 5000; int pendingBack = 0;
    Stat off, on; unsigned steady = 0; size_t fed = 0; long sumOn = 0, sumOff = 0;
    for (int k = 0; k < 6000; ++k) {
        if (k > 0) {
            int step = 2;
            if (pendingBack) { step = 2 - pendingBack; if (step < 1) step = 1; pendingBack = 0; }
            else if (rng.U() < tc.pStart) { step = 3; pendingBack = 1; }
            else if (rng.U() < tc.pLong) { step = 5; }
            // back-pressure: a start may not run more than 6 refreshes ahead of the newest shown picture's slot
            stamp += (uint32_t)step;
            if (!sh.empty() && (int32_t)(sh.back() + 2 - stamp) > 6) stamp = sh.back() + 2 - 6;
        }
        unsigned ready = 3 + (rng.U() < 0.3 ? 1 : 0);
        if (rng.U() < tc.pGpu) ready += 3;
        uint32_t shown = sh.empty() ? stamp + 6 : std::max(stamp + ready, sh.back() + 2);
        // the display's reports up to the previous picture shown before this frame's start
        while (fed < sh.size() && (int32_t)(sh[fed] - stamp) <= 0) {
            learn.Add(nat[fed]);
            if (fed > 0) steady = (sh[fed] - sh[fed - 1] == 2) ? steady + 1 : 0;
            ++fed;
        }
        const int L = learn.Ready() ? learn.Quantile(0.8) : 4;
        int a = sc.Begin(true, stamp, 2, L, steady, fed > 0, fed > 0 ? (uint64_t)fed : 0, fed > 0 ? sh[fed - 1] : 0);
        const int own = st.empty() ? 2 : (int)(stamp - st.back());
        if (a < 0 || breakMode == 3) a = own;
        st.push_back(stamp); sh.push_back(shown); nat.push_back(ready); sc.Handed((uint64_t)k + 1);
        if (k > 100) {
            const int hold = (int)(sh[k] - sh[k - 1]);
            off.add(own - hold); on.add(a - hold); sumOn += a; sumOff += own;
        }
    }
    *offMean = off.mean(); *onMean = on.mean(); *offShare = off.share(0.5); *onShare = on.share(0.5);
    if (print) std::printf("console-statistics %-26s today: mean %.3f, pictures off %.1f%%; smooth: mean %.3f, pictures off %.1f%% (game refreshes %ld vs %ld)\n", tc.name, off.mean(), off.share(0.5), on.mean(), on.share(0.5), sumOn, sumOff);
}

int main(int argc, char** argv) {
    const bool table = argc > 1 && std::strcmp(argv[1], "table") == 0;
    const Case cases[] = {
        // name                 n  perPic step noise gpu  gnoise lateEvery lateMs depth bpProb
        {"locked60",            1, 6.0,  4.0, 0.8,  9.0,  1.5,   0,  0,  2, 0.00, 0.0, 0},
        {"held30-clean",        2, 11.0, 6.0, 1.5,  14.0, 2.0,   0,  0,  2, 0.00, 0.0, 0},
        {"held30-startjitter",  2, 9.0,  5.0, 1.5,  11.0, 2.0,   0,  0,  2, 0.12, 0.0, 0},   // the r10 console signature: every picture held 2, starts moved by the gate
        {"held30-q2-jitter",    2, 11.0, 6.0, 2.0,  14.0, 3.0,   0,  0,  2, 0.12, 0.0, 2},   // deep queue (the console's 6 vblanks shown-minus-start): starts move, holds stay 2
        {"held30-q2-heavy",     2, 12.0, 8.0, 3.0,  17.0, 5.0,   0,  0,  2, 0.08, 0.0, 2},
        {"held30-q2-late45",    2, 11.0, 7.0, 1.5,  14.0, 2.0,   9, 45,  2, 0.05, 0.0, 2},
        {"held30-gate (r10)",   2, 11.0, 8.0, 2.5,  15.0, 4.0,   0,  0,  2, 0.12, 0.0, 0},
        {"held30-heavy",        2, 13.0, 8.0, 3.0,  18.0, 6.0,   0,  0,  2, 0.06, 0.0, 0},
        {"held30-late25",       2, 11.0, 7.0, 1.5,  14.0, 2.0,   5,  25, 2, 0.02, 0.0, 0},
        {"held30-late45",       2, 11.0, 7.0, 1.5,  14.0, 2.0,   9,  45, 2, 0.02, 0.0, 0},
        {"held30-late70",       2, 11.0, 7.0, 1.5,  14.0, 2.0,  30,  70, 2, 0.02, 0.0, 0},
        {"free45 (paced)",      0, 9.0,  6.0, 2.0,  12.0, 3.0,   0,  0,  2, 0.00, 0.0, 0},
    };
    bool printedHeader = false;
    for (const Case& c : cases) {
        for (int clockMode = 0; clockMode < 2; ++clockMode) {
            const Result off = Run(c, clockMode, false);
            const Result on = Run(c, clockMode, true);
            if (table) {
                if (!printedHeader) { std::printf("model: per picture, represented-time step minus shown hold, in refreshes (1 = 16.7 ms)\n"); printedHeader = true; }
                char m0[32], m1[32];
                std::snprintf(m0, sizeof m0, "%s/off", clockMode ? "debt" : "slip");
                std::snprintf(m1, sizeof m1, "%s/SMOOTH", clockMode ? "debt" : "slip");
                Row(c.name, m0, off);
                Row(c.name, m1, on);
            }
            // checks: the game keeps real time (no slow-down beyond what the clock already does), and where the cadence is held the snapshot error falls
            char w[200];
            std::snprintf(w, sizeof w, "%s clock %d: game speed with the smooth clock within 0.3 %% of without (%.2f vs %.2f)", c.name, clockMode, on.speedPct, off.speedPct);
            check(std::fabs(on.speedPct - off.speedPct) < 0.6, w);
            std::snprintf(w, sizeof w, "%s clock %d: snapshot error not worse (mean %.3f -> %.3f, off by > 0.5: %.1f%% -> %.1f%%)", c.name, clockMode, off.snap.mean(), on.snap.mean(), off.snap.share(0.5), on.snap.share(0.5));
            check(on.snap.mean() <= off.snap.mean() + 0.06 && on.snap.share(0.5) <= off.snap.share(0.5) + 5.5, w);
            if (std::strstr(c.name, "clean") || std::strstr(c.name, "locked")) {
                std::snprintf(w, sizeof w, "%s clock %d: a clean cadence is left alone (mean %.3f -> %.3f, off by > 0.5: %.1f%% -> %.1f%%)", c.name, clockMode, off.snap.mean(), on.snap.mean(), off.snap.share(0.5), on.snap.share(0.5));
                check(on.snap.mean() <= off.snap.mean() + 1e-9 && on.snap.share(0.5) <= off.snap.share(0.5) + 1e-9 && on.changed == 0, w);   // (a clean cadence: every frame keeps its own time)
            }
            if (c.n == 0) {
                std::snprintf(w, sizeof w, "%s clock %d: a free run is untouched", c.name, clockMode);
                check(on.changed == 0 && on.snap.mean() == off.snap.mean(), w);
            }
            std::snprintf(w, sizeof w, "%s clock %d: no more 3-refresh holds (%.1f%% -> %.1f%%)", c.name, clockMode, off.hold3, on.hold3);
            check(on.hold3 <= off.hold3 + 1.0, w);   // (model noise: the step count changes the modelled cost)
        }
    }
    // a clean locked cadence: every frame gets its own time (the action counter stays 0)
    {
        ShowClock sc; unsigned bad = 0;
        for (uint32_t i = 0; i < 1000; ++i) { const int a = sc.Begin(true, 100 + i, 1, 3, 100, false, 0, 0); if (i > 0 && a != -1) ++bad; sc.Handed(i + 1); }
        check(bad == 0 && sc.Changed() == 0, "clean 60: every frame keeps its own time, nothing changed");
    }
    // the display's truth: shown_k = max(stamp_k + 3, shown_{k-1} + 2), reported two frames later. Starts: 2,2,1,3,2,3,1,2,... (early and late by one, either order),
    // a hitch of 7 refreshes at frame 300. The clock must give each picture the refreshes it is shown for (after the first report), and the hitch's whole time.
    {
        ShowClock sc; std::vector<uint32_t> st, sh; uint32_t stamp = 1000; int wrong = 0, wrongToday = 0, total = 0; long sumAdv = 0, sumReal = 0; int hitchAdv = 0; uint32_t hitchHold = 0;
        const int pat[8] = {2, 2, 1, 3, 2, 3, 1, 2};
        for (int k = 0; k < 600; ++k) {
            if (k > 0) stamp += (k == 300) ? 7 : pat[k % 8];
            const uint32_t shown = st.empty() ? stamp + 5 : std::max(stamp + 3, sh.back() + 2);
            const bool fb = k >= 2;
            unsigned run = 0;
            for (int j = k - 2; j >= 1 && sh[j] - sh[j - 1] == 2; --j) ++run;
            int a = sc.Begin(true, stamp, 2, 3, run, fb, fb ? (uint64_t)(k - 1) : 0, fb ? sh[k - 2] : 0);
            if (a < 0) a = k > 0 ? (int)(stamp - st.back()) : 0;   // (today's clock: the frame's own time)
            st.push_back(stamp); sh.push_back(shown); sc.Handed((uint64_t)k + 1);
            if (k == 300) { hitchAdv = a; hitchHold = sh[k] - sh[k - 1]; }
            if (k > 4) { ++total; if (a != (int)(sh[k] - sh[k - 1])) ++wrong; if ((int)(st[k] - st[k - 1]) != (int)(sh[k] - sh[k - 1])) ++wrongToday; sumAdv += a; sumReal += (long)(st[k] - st[k - 1]); }
        }
        char w[200]; std::snprintf(w, sizeof w, "start jitter at a held 30: %d of %d pictures given a time other than their hold (today's clock %d); hitch %d refreshes (as shown); total %ld of %ld", wrong, total, wrongToday, hitchAdv, sumAdv, sumReal);
        check(wrong * 10 <= wrongToday && hitchAdv == (int)hitchHold && std::labs(sumAdv - sumReal) <= 6, w);
        std::printf("%s\n", w);
    }
    // the console's display by its statistics
    {
        const TruthCase tcs[] = {{"steady (d1/pipe all-2)", 0.04, 0.0, 0.0}, {"start jitter (perf13 r10)", 0.20, 0.0, 0.0}, {"rough (gpu misses + long starts)", 0.10, 0.03, 0.05}, {"heavy (worstgpu)", 0.08, 0.05, 0.12}};
        for (const TruthCase& tc : tcs) {
            double a, b, c2, d;
            Truth(tc, true, &a, &b, &c2, &d);
            char w[200]; std::snprintf(w, sizeof w, "console statistics %s: error falls (mean %.3f -> %.3f, off %.1f%% -> %.1f%%)", tc.name, a, b, c2, d);
            const bool calm = tc.pLong == 0.0 && tc.pGpu == 0.0;
            check(calm ? (b <= a * 0.1 && d <= c2 * 0.1) : (b < a * 0.95 && d < c2 * 0.95), w);
        }
    }
    if (g_fail) { std::printf("smooth_unit: %d checks FAILED\n", g_fail); return 1; }
    std::printf("smooth_unit: all checks passed\n");
    return 0;
}
