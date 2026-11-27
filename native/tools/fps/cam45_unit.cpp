// cam45_unit.cpp (wave 24, helper "feel24"): the gameplay camera at the held cadences, 60 (1), 45 (1,1,2) and 30 (2) refreshes a picture, through the game's own Dampen
// (native/include/Game/Camera/CameraDamping.h) with the camera's own smooth time (0.5 s) and its own input: the aim point read from the PREVIOUS picture's blended snapshot,
// damped by this picture's dt (BeginFrameTask -> cCameraManager::Update before FixedUpdateTask; GameplayCameraZoomLevel::Update).
//   c++ -std=c++11 -O1 -Inative/include native/tools/fps/cam45_unit.cpp -o cam45_unit && ./cam45_unit
// Two different things are measured, and the console's [smooth] "camera aim" line is only the first:
//   AIM AGE  what [smooth] prints as the camera's aim: the step of the snapshot time the aim was read from, minus the refreshes the picture was shown. At 45 it is
//            |hold(k-1) - hold(k)| = 1,0,1: mean 0.667, 67 % of pictures over 0.5 (the console read 0.71-0.78, 67-69 %). It is the age of the damper's INPUT, not the camera's motion.
//   MOTION   what is on the screen: the camera's step per picture against the continuous critically damped camera at the same shown times (error in refreshes of a player's
//            8 m/s run), and the camera's speed per shown refresh from one picture to the next (the console's "camera pan |ratio-1|").
// Candidate render-side fix = aim read at THIS picture's own time (what reading this picture's snapshot would give): checked here to show what it buys.
#include "Game/Camera/CameraDamping.h"
#include <cmath>
#include <cstdio>
#include <vector>
#include <algorithm>

static int g_fail = 0;
static void check(bool ok, const char* what) { if (!ok) { std::printf("FAIL: %s\n", what); ++g_fail; } }
static const double kV = 1001.0 / 60000.0;   // seconds a refresh

struct R { double aimMean, aimOver, stepErr, stepErrWorst, pan, panWorst, lagMean; };

// player path: a run with speed changes, a stop and a reversal, smooth (up to about 10 m/s^2, a sprint turn; m, s)
static double Path(double t) { return 4.0 * t + 7.0 * std::sin(1.2 * t) + 1.5 * std::sin(3.1 * t); }

static R Run(const std::vector<int>& pattern, bool aimNow, bool steady) {
    std::vector<double> S(1, 0.0);
    for (size_t k = 0; S.size() < 6000; ++k) S.push_back(S.back() + pattern[k % pattern.size()] * kV);
#ifdef CAM45_BREAK
    const double smooth = 0.05;   // mutant: a camera with almost no damping follows the aim's irregularity (must fail the checks)
#else
    const double smooth = 0.5;
#endif
    float cam = (float)Path(0), vel = 0.0f;
    std::vector<double> camPos(S.size(), 0.0), ref(S.size(), 0.0), aimT(S.size(), 0.0);
    camPos[0] = cam; ref[0] = cam;
    float rc = (float)Path(0), rv = 0.0f; double t = 0.0;
    for (size_t k = 1; k < S.size(); ++k) {
        const float dt = (float)(S[k] - S[k - 1]);
        const double at = aimNow ? S[k] : S[k - 1];            // the aim point's time: this picture's (candidate) or the previous picture's (the game)
        aimT[k] = at;
        cam = Dampen(cam, (float)Path(at), vel, (float)smooth, dt);
        camPos[k] = cam;
        while (t < S[k] - 1e-9) { const float h = (float)std::min(1e-3, S[k] - t); rc = Dampen(rc, (float)Path(t + h), rv, (float)smooth, h); t += h; }
        ref[k] = rc;
    }
    (void)steady;
    R r = {0, 0, 0, 0, 0, 0, 0};
    unsigned n = 0, over = 0, np = 0; double lag = 0;
    for (size_t k = 400; k + 1 < S.size(); ++k) {
        const double hold = (S[k] - S[k - 1]) / kV;
        const double aimStep = (aimT[k] - aimT[k - 1]) / kV - hold;
        r.aimMean += std::fabs(aimStep); if (std::fabs(aimStep) > 0.5) ++over;
        const double e = ((camPos[k] - camPos[k - 1]) - (ref[k] - ref[k - 1])) / (8.0 * kV);   // step error in refreshes of an 8 m/s run
        r.stepErr += std::fabs(e); r.stepErrWorst = std::max(r.stepErrWorst, std::fabs(e));
        lag += camPos[k] - ref[k];
        const double h1 = (S[k + 1] - S[k]) / kV, h0 = hold;
        const double v1 = (camPos[k + 1] - camPos[k]) / h1, v0 = (camPos[k] - camPos[k - 1]) / h0;
        if (std::fabs(v0) > 1.0 * kV) { const double pr = std::fabs(v1 / v0 - 1.0); r.pan += pr; r.panWorst = std::max(r.panWorst, pr); ++np; }
        ++n;
    }
    r.aimMean /= n; r.aimOver = 100.0 * over / n; r.stepErr /= n; r.pan /= np ? np : 1; r.lagMean = lag / n;
    return r;
}

int main() {
    struct C { const char* name; std::vector<int> p; } cs[] = {{"60 (1)", {1}}, {"45 (1,1,2)", {1, 1, 2}}, {"45 (1,2,1,1,1,2)", {1, 2, 1, 1, 1, 2}}, {"30 (2)", {2}}};
    R game[4], cand[4];
    for (int i = 0; i < 4; ++i) {
        game[i] = Run(cs[i].p, false, true);
        cand[i] = Run(cs[i].p, true, true);
        std::printf("%-18s game: aim age mean %.3f over0.5 %5.1f%% | motion: step error mean %.4f worst %.3f refresh, pan |ratio-1| mean %.4f worst %.3f | candidate (aim at this picture): aim age %.3f, step error mean %.4f, pan %.4f\n",
                    cs[i].name, game[i].aimMean, game[i].aimOver, game[i].stepErr, game[i].stepErrWorst, game[i].pan, game[i].panWorst, cand[i].aimMean, cand[i].stepErr, cand[i].pan);
    }
    // 60 and 30: the aim age is exact (the console read about 0.15 and 0 there: its noise is the real clock, not the cadence)
    check(game[0].aimMean < 1e-6 && game[3].aimMean < 1e-6, "60 and 30: the aim's age is steady");
    // 45: the model reproduces the console's reading (0.71-0.78, 67-69 % over 0.5)
    check(game[1].aimMean > 0.6 && game[1].aimMean < 0.75 && game[1].aimOver > 60 && game[1].aimOver < 72, "45 (1,1,2): the aim-age figure is the console's (0.67, 67 %)");
    // ... and is not a motion error: what the damper does with it is invisible
    check(game[1].stepErr < 0.02 && game[2].stepErr < 0.02, "45: the camera's step error against the continuous camera is under 0.02 of a refresh of an 8 m/s run");
    check(game[1].pan <= 1.5 * game[0].pan && game[1].pan < game[3].pan && game[2].pan < game[3].pan, "45: the camera's speed per shown refresh changes from picture to picture by no more than 1.5x the 60 value and less than at 30 (the path itself, a turn, is most of it)");
    check(game[1].stepErr < 3.0 * game[0].stepErr + 1e-4 && game[1].stepErr < 4.0 * game[3].stepErr + 1e-4, "45 is not worse than 60 or 30 by more than the cadence itself costs");
    // the candidate fix gains nothing the eye could see
    check(cand[1].aimMean < 1e-6, "candidate: the aim age is steady at 45");
    check(std::fabs(game[1].stepErr - cand[1].stepErr) < 0.005 && std::fabs(game[1].pan - cand[1].pan) < 0.005, "candidate: the camera's step error and speed change move by under 0.005 (nothing visible to gain)");
    if (g_fail) { std::printf("cam45_unit: %d checks FAILED\n", g_fail); return 1; }
    std::printf("cam45_unit: all checks passed\n");
    return 0;
}
