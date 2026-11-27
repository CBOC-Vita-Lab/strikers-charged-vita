// MSC_PICTURE_FLOORS (vgx_floors.h, vgx_hold.h, vgx_lock60.h, the shipped headers): the owner's picture floors, 17 Nov 2026. "448x320 is unacceptable."
//   Quality (MSC_PICTURE_HOLD): nothing below 864x488 in any scene; Smooth 60 (MSC_LOCK60): nothing below 800x456 (in pixels); a 60 that does not hold at its floor goes to a steady 30 at the full size.
// The limits below are WRITTEN HERE as literals, not read from the header: a mutant that lowers the header's constant (-DVGX_FLOOR_QUALITY_W=700 -DVGX_FLOOR_QUALITY_H=400, or the SMOOTH pair) must
// make this test FAIL.   floors [--break=N]   N=1: the policy ignores the floor's level count (maxSizes back to 5);  N=2: the steady 30 is entered on one picture (flap check);
//   N=3: the Smooth 60 table keeps its old rungs (1.00 .. 0.50);  N=4: the fallback rung has one sample;  N=5: the ladders without the hold use the old table (the clamp removed);
//   N=6: a fixed size from a file is not raised.   Compile mutant: -DVGX_FLOORS_TABLE_BREAK (MUTANT=table in run_floors_test.sh): the Quality table itself has the old rungs.
// Pure: no GL, no clock. Every figure is an estimate (video memory: colour + depth, 4 bytes a pixel each, one sample).
#include "vgx_floors.h"
#include "vgx_hold.h"
#include "vgx_lock60.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
int g_fail = 0, g_pass = 0, g_break = 0;
void Check(bool ok, const char* what, const char* detail = "") {
    if (ok) { ++g_pass; std::printf("ok   %s %s\n", what, detail); }
    else { ++g_fail; std::printf("FAIL %s %s\n", what, detail); }
}
constexpr long kQualityPix = 864L * 488, kSmoothPix = 800L * 456;   // the owner's numbers, literal
bool QualityOk(int w, int h) { return static_cast<long>(w) * h >= kQualityPix; }
bool SmoothOk(int w, int h) { return static_cast<long>(w) * h >= kSmoothPix; }

using namespace vgx::hold;

// Walk every step of a ladder: its size must be allowed.
int WorstStepPixels(const Policy& p, int heldW, int heldH, int* worstW, int* worstH) {
    long worst = 1L << 40;
    for (int s = 0; s <= p.maxStep; ++s) {
        const int l = p.Lvl(s);
        const int w = l ? StepDim(heldW, p.scale[l]) : heldW, h = l ? StepDim(heldH, p.scale[l]) : heldH;
        if (static_cast<long>(w) * h < worst) { worst = static_cast<long>(w) * h; *worstW = w; *worstH = h; }
    }
    return static_cast<int>(worst / 1000);
}
}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) if (!std::strncmp(argv[i], "--break=", 8)) g_break = std::atoi(argv[i] + 8);
    const int W = 960, H = 544;

    // ---- the named constants: one per mode
    Check(vgx::floors::kQualityMinW == 864 && vgx::floors::kQualityMinH == 488, "the Quality floor constant is 864x488");
    Check(vgx::floors::kSmoothMinW == 800 && vgx::floors::kSmoothMinH == 456, "the Smooth 60 floor constant is 800x456");

    // ---- Quality: the new order (the samples last), the one the Vita plan runs
    {
        Policy p;
        const float sc = 0.95f;
        p.scale[1] = sc; p.scale[2] = sc * (0.90f / 0.95f); p.scale[3] = sc * (0.8333f / 0.95f); p.scale[4] = sc * (0.7833f / 0.95f); p.scale[5] = sc * (0.7333f / 0.95f);
        const int nl = FloorLevels(vgx::floors::kQuality, W, H, p.scale, kSizeSteps);
        char d[96]; std::snprintf(d, sizeof d, "(%d levels: 912x520, 864x488)", nl);
        Check(nl == 2, "Quality: the aaLast ladder keeps two size levels (912x520, 864x488)", d);
        p.maxSizes = nl;
        if (g_break == 1) p.maxSizes = kSizeSteps;
        p.SetAa(2, true);
        p.SetOrder(true, kSizeSteps, kOldSizes);
        int ww = 0, hh = 0;
        WorstStepPixels(p, W, H, &ww, &hh);
        std::snprintf(d, sizeof d, "(the deepest step is %dx%d)", ww, hh);
        Check(QualityOk(ww, hh), "Quality: no step of the aaLast ladder is below 864x488", d);
        Check(p.maxStep <= 7, "Quality: the last step is the one-sample step at 864x488 (step 7)");
        // driven by an overload that never ends, every target ready: the policy never leaves the ladder
        Policy q = p;
        q.readyMask = 0xFF; q.aaReady = 7; q.off[kFxSize] = q.off[kCrowd] = q.off[kShadowFar] = q.off[kShadowAll] = true;
        q.Start(0, 4);
        int deepest = 0;
        for (int i = 0; i < 20000; ++i) { q.Frame(90.0f, 33.3f, false); deepest = std::max(deepest, q.step); }
        const int l = q.Lvl(deepest);
        const int w = l ? StepDim(W, q.scale[l]) : W, h = l ? StepDim(H, q.scale[l]) : H;
        std::snprintf(d, sizeof d, "(step %d, %dx%d)", deepest, w, h);
        Check(QualityOk(w, h) && deepest <= q.maxStep, "Quality: 20000 pictures of 90 ms never take a step below the floor", d);
        // the start tables (stadium class 2, the held scene's table, the Dump's bottom step)
        const int sceneStart = SceneStartStep(2, q.aa2, q.nSizes, q.firstSize);
        const int dumpStart = StartLevel(-1, 2, q.Last(), q.firstSize, q.aa2, q.nSizes);
        const int l1 = q.Lvl(sceneStart), l2 = q.Lvl(dumpStart);
        const int w1 = l1 ? StepDim(W, q.scale[l1]) : W, h1 = l1 ? StepDim(H, q.scale[l1]) : H, w2 = l2 ? StepDim(W, q.scale[l2]) : W, h2 = l2 ? StepDim(H, q.scale[l2]) : H;
        std::snprintf(d, sizeof d, "(held scene %dx%d step %d, The Dump %dx%d step %d)", w1, h1, sceneStart, w2, h2, dumpStart);
        Check(QualityOk(w1, h1) && QualityOk(w2, h2) && sceneStart <= q.maxStep && dumpStart <= q.maxStep, "Quality: the held-scene table and the Dump's bottom start are at or above 864x488", d);
        // a held scene's own policy (its SetOrder takes the live one's sizes)
        Policy S; S.scale[1] = p.scale[1]; S.scale[2] = p.scale[2]; S.scale[3] = p.scale[3]; S.scale[4] = p.scale[4]; S.scale[5] = p.scale[5];
        S.maxSizes = p.maxSizes; S.SetOrder(true, p.nSizes, p.n2x);
        WorstStepPixels(S, W, H, &ww, &hh);
        Check(QualityOk(ww, hh) && S.nSizes <= 2, "Quality: a held scene's policy has no level below the floor");
        // the old ladder's numbers: 5 sizes down to 704x400 cost this much video memory (1x colour + depth), now not made
        long cut = 0;
        for (int k = nl + 1; k <= kSizeSteps; ++k) cut += static_cast<long>(StepDim(W, p.scale[k])) * StepDim(H, p.scale[k]) * 8 / 1024;
        std::snprintf(d, sizeof d, "(%ld KiB: 800x456, 752x424, 704x400 at one sample)", cut);
        Check(cut > 0, "Quality: video memory no longer reserved for the cut levels", d);
        std::printf("INFO video memory freed, Quality aaLast ladder: %ld KiB (estimate; with the 2x target of the third level, +%ld KiB)\n", cut, static_cast<long>(StepDim(W, p.scale[3])) * StepDim(H, p.scale[3]) * 4 / 1024);
    }
    // ---- Quality: the old order (the sizes before the samples): 0.90, 0.81, 0.73
    {
        Policy p;   // scale defaults 0.90, 0.81, 0.73
        const int nl = FloorLevels(vgx::floors::kQuality, W, H, p.scale, kSizeSteps);
        char d[96]; std::snprintf(d, sizeof d, "(%d level)", nl);
        Check(nl == 1, "Quality: the old ladder keeps one size level (864x488)", d);
        p.maxSizes = g_break == 1 ? kSizeSteps : nl;
        p.SetOrder(false);
        int ww = 0, hh = 0;
        WorstStepPixels(p, W, H, &ww, &hh);
        std::snprintf(d, sizeof d, "(last step %d, %dx%d)", p.maxStep, ww, hh);
        Check(QualityOk(ww, hh) && p.maxStep <= kShedSteps + 1, "Quality: no step of the old ladder is below 864x488", d);
        const int ready = LadderMaxStep(0xF, kOldLast, p.maxSizes);
        Check(ready <= kShedSteps + 1, "Quality: the ladder made from the kept targets stops at the floor level even when every target is kept");
        Check(LadderSizes(0x3F, p.maxSizes) <= p.maxSizes, "Quality: the new order's ladder from kept targets stops at the floor level even when five are kept");
    }
    // ---- Smooth 60: the level table and the presets
    {
        float tbl[16] = {1.50f, 1.50f, 1.20f, 1.13f};
        int n = vgx::floors::kSmoothRungs;
        if (g_break == 3) { const float old[] = {1.20f, 1.12f, 1.06f, 1.00f, 1.00f, 1.00f, 0.93f, 0.86f, 0.78f, 0.70f, 0.62f, 0.56f, 0.50f}; n = 13; for (int i = 0; i < n; ++i) tbl[i] = old[i]; }
        int worstW = 9999, worstH = 9999;
        bool allOk = true;
        for (int i = 0; i < n; ++i) { int w, h; vgx::floors::LevelSize(640, 448, tbl[i], &w, &h); if (!SmoothOk(w, h)) allOk = false; if (static_cast<long>(w) * h < static_cast<long>(worstW) * worstH) { worstW = w; worstH = h; } }
        char d[96]; std::snprintf(d, sizeof d, "(%d rungs, the bottom is %dx%d)", n, worstW, worstH);
        Check(allOk, "Smooth 60: every rung of the level table has the pixels of 800x456", d);
        Check(vgx::floors::LevelsAllowed(vgx::floors::kSmooth, 640, 448, tbl, n) == n, "Smooth 60: the table in force is all allowed (LevelsAllowed)");
        // the OLD table: only its leading levels pass; the rest were below the floor
        const float old[] = {1.20f, 1.12f, 1.06f, 1.00f, 1.00f, 1.00f, 0.93f, 0.86f, 0.78f, 0.70f, 0.62f, 0.56f, 0.50f};
        const int oldOk = vgx::floors::LevelsAllowed(vgx::floors::kSmooth, 640, 448, old, 13);
        std::snprintf(d, sizeof d, "(%d of 13 old rungs would pass)", oldOk);
        Check(oldOk < 13 && oldOk <= 2, "Smooth 60: the old table's rungs below the floor are found (448x320 among them)", d);
        const float sf = vgx::floors::ScaleFloor(vgx::floors::kSmooth, 640, 448);
        std::snprintf(d, sizeof d, "(%.2f)", sf);
        Check(sf >= 1.12f && sf <= 1.14f, "Smooth 60: the floor as a scale of the game's own size is 1.13", d);
        bool presets = true;
        for (int k = 0; k < vgx::lock60::kSceneKinds; ++k) { const vgx::lock60::Preset pr = vgx::lock60::PresetForFloor(k, sf); int w, h; vgx::floors::LevelSize(640, 448, pr.floor, &w, &h); int w2, h2; vgx::floors::LevelSize(640, 448, pr.start, &w2, &h2); if (!SmoothOk(w, h) || !SmoothOk(w2, h2)) presets = false; }
        Check(presets, "Smooth 60: every scene kind's start and floor preset (play, heavy, replay, cut-scene, Mega Strike, menus) is at or above 800x456");
        bool oldPresetsBelow = false;
        for (int k = 0; k < vgx::lock60::kSceneKinds; ++k) { int w, h; vgx::floors::LevelSize(640, 448, vgx::lock60::PresetFor(k).floor, &w, &h); if (!SmoothOk(w, h)) oldPresetsBelow = true; }
        Check(oldPresetsBelow, "(control) the unclamped presets do go below the floor: the clamp is what keeps them up");
        // video memory the old Smooth 60 ladder made ahead and the new one does not: rungs 3..12 of the old table at the game's own shape, 1x, colour + depth
        long old_kib = 0;
        for (int i = 2; i < 13; ++i) { int w, h; vgx::floors::LevelSize(640, 448, old[i], &w, &h); if (!SmoothOk(w, h)) old_kib += static_cast<long>(w) * h * 8 / 1024; }
        std::printf("INFO video memory of the Smooth 60 rungs below the floor (the old table, 1x colour + depth): %ld KiB (estimate; the old tour made up to 24576 KiB of them)\n", old_kib);
    }
    // ---- Smooth 60: the fallback picture is 960x544 with edge smoothing (2x), rung 1 is native 960x544 1x
    {
        vgx::floors::SmoothRung t[vgx::floors::kSmoothRungs];
        vgx::floors::SmoothTable(2, t);
        if (g_break == 4) t[0].ms = 1;
        int w, h, w1, h1; vgx::floors::LevelSize(640, 448, t[0].s, &w, &h);
        vgx::floors::LevelSize(640, 448, t[1].s, &w1, &h1);
        char d[96]; std::snprintf(d, sizeof d, "(%dx%d, %dx samples, %ld KiB colour+depth vs %ld KiB at 1x)", w, h, t[0].ms, static_cast<long>(w) * h * 4 * (1 + t[0].ms) / 1024, static_cast<long>(w) * h * 8 / 1024);
        Check(w == 960 && h == 544 && t[0].ms == 2, "steady 30 fallback rung is 960x544 with 2x edge smoothing", d);
        Check(w1 == 960 && h1 == 544 && t[1].ms == 1, "Smooth 60 top 1x rung is full native 960x544 1x");
        vgx::floors::SmoothRung u[vgx::floors::kSmoothRungs]; vgx::floors::SmoothTable(0, u);
        Check(u[0].ms == 1, "without multisampling in the build the rung has one sample");
    }
    // ---- Smooth 60: the steady 30 at full size, with the anti-flap rules
    {
        vgx::lock60::Fallback30 f;
        if (g_break == 2) f.enter = 1;
        for (int i = 0; i < 40; ++i) f.Frame(true);
        Check(!f.full, "steady 30: 40 pictures of a held 30 do not move the picture (a short 30 moves nothing)");
        for (int i = 0; i < 80; ++i) f.Frame(true);
        Check(f.full && f.entries == 1, "steady 30: after 120 pictures of a held 30 the picture is at the full size, one entry");
        for (int i = 0; i < 200; ++i) f.Frame(false);
        Check(f.full, "steady 30: 200 pictures back at 60 keep the full size (the controller may be flapping)");
        for (int i = 0; i < 60; ++i) f.Frame(false);
        Check(!f.full && f.exits == 1, "steady 30: 260 pictures back at 60 give the range of 60 back");
        // flapping 30 / 60 every 50 pictures never moves more than once
        vgx::lock60::Fallback30 g;
        for (int c = 0; c < 40; ++c) for (int i = 0; i < 50; ++i) g.Frame(c % 2 == 0);
        Check(g.entries <= 1, "steady 30: a 30 that flaps every 50 pictures moves the picture at most once in 2000");
    }

    // ---- 18 Nov (F1, F2, F3 of fable-review25): EVERY Settings choice a player can make reaches a size at or over its mode's floor. The enumeration is the product of the rows that move the picture size
    // (RESOLUTION, FRAME RATE, ANTI-ALIASING, SMOOTH 60 MODE) and the hand-written keys the plan reacts to (DYNRES off, MSC_VITA_MSAA, MSC_PICTURE_HOLD=0 / native / full, 1.1 files with a fixed size), each
    // crossed with live play, a Mega Strike, MSC_DYNRES_SEVERE and a held scene. The renderer's DynBuildLevels takes its table from vgx::floors::LadderFor + SmoothTable / QualityTable (the functions called here);
    // a named floor of the ladder (ordinary, 60, paced, SEVERE, heavy stadium, Mega Strike start) is an index into that table, so the deepest rung is the lowest size any scene can reach.
    // Smooth 60's bottom rung is 728x512 (372,736 pixels): the same pixels as 800x456 (364,800) in the game's own shape, so it passes the pixel test; the floor is a pixel count, not a width.
    {
        struct Fixed { const char* name; int w, h; };
        const Fixed fixeds[] = {{"STANDARD (no fixed size)", 0, 0}, {"SMALLER 864X488", 864, 488}, {"1.1 PERFORMANCE file 640x362", 640, 362}, {"old SMALLEST 480x272", 480, 272}, {"held native 640x448", 640, 448}, {"hold full 960x544", 960, 544}};
        long minAll = 1L << 40; int cases = 0, bad = 0;
        long worstW = 0, worstH = 0;
        for (const Fixed& fx : fixeds)
            for (int fps30 = 0; fps30 < 2; ++fps30)
                for (int msaaTop = 0; msaaTop <= 4; msaaTop += 2)           // ANTI-ALIASING OFF (0), ON (4), the 2x-only build key (2)
                    for (int lock60 = 0; lock60 < 2; ++lock60)
                        for (int dynOn = 0; dynOn < 2; ++dynOn)
                            for (int vitaMsaa = 0; vitaMsaa < 2; ++vitaMsaa)
                                for (int scene = 0; scene < 4; ++scene) {       // 0 live play, 1 Mega Strike, 2 SEVERE, 3 held scene (replay, cut-scene, walk-out)
                                    ++cases;
                                    (void)fps30; (void)dynOn; (void)vitaMsaa; (void)scene;   // none of them picks a size on its own: the table (or the fixed size) decides, which is what is asserted
                                    long minPix = 1L << 40; int mw = 0, mh = 0;
                                    const bool fixedByPlayer = fx.w > 0;
                                    // a fixed size: the plan raises a size under the floor (ClampFixedSize); the held pictures of the hold use the held size
                                    int lad = vgx::floors::LadderFor(true, fixedByPlayer, lock60 != 0);
                                    if (g_break == 5 && !fixedByPlayer) lad = vgx::floors::kLadderLegacy;
                                    if (fixedByPlayer) {
                                        int cw = fx.w, ch = fx.h;
                                        if (g_break != 6) vgx::floors::ClampFixedSize(fx.w, fx.h, &cw, &ch);
                                        minPix = static_cast<long>(cw) * ch; mw = cw; mh = ch;
                                    } else if (lad == vgx::floors::kLadderSmooth) {
                                        vgx::floors::SmoothRung t[vgx::floors::kSmoothRungs]; vgx::floors::SmoothTable(msaaTop, t);
                                        for (int i = 0; i < vgx::floors::kSmoothRungs; ++i) { int w, h; vgx::floors::LevelSize(640, 448, t[i].s, &w, &h); if (static_cast<long>(w) * h < minPix) { minPix = static_cast<long>(w) * h; mw = w; mh = h; } }
                                    } else if (lad == vgx::floors::kLadderQuality) {
                                        vgx::floors::SmoothRung t[vgx::floors::kQualityRungs]; vgx::floors::QualityTable(msaaTop, t);
                                        for (int i = 0; i < vgx::floors::kQualityRungs; ++i) { int w, h; vgx::floors::LevelSize(640, 448, t[i].s, &w, &h); if (static_cast<long>(w) * h < minPix) { minPix = static_cast<long>(w) * h; mw = w; mh = h; } }
                                    } else {   // the old table (floors off, or the mutant): the levels of the old ladder, 1.20 .. 0.50 of 640x448
                                        static const float old[] = {1.20f, 1.12f, 1.06f, 1.00f, 1.00f, 1.00f, 0.93f, 0.86f, 0.78f, 0.70f, 0.62f, 0.56f, 0.50f};
                                        for (float sc : old) { int w, h; vgx::floors::LevelSize(640, 448, sc, &w, &h); if (static_cast<long>(w) * h < minPix) { minPix = static_cast<long>(w) * h; mw = w; mh = h; } }
                                    }
                                    const bool ok = lock60 && !fixedByPlayer ? SmoothOk(mw, mh) : QualityOk(mw, mh);
                                    if (!ok) { ++bad; worstW = mw; worstH = mh; }
                                    if (minPix < minAll) minAll = minPix;
                                }
        char d[160]; std::snprintf(d, sizeof d, "(%d configurations, smallest size reached %ld pixels; worst failing %ldx%ld)", cases, minAll, worstW, worstH);
        Check(bad == 0, "every Settings choice x frame rate x anti-aliasing x Smooth 60 x DYNRES x MSC_VITA_MSAA x live / Mega Strike / SEVERE / held reaches no size under its floor", d);
        Check(cases == 6 * 2 * 3 * 2 * 2 * 2 * 4, "the enumeration is complete");
    }
    // the Quality table itself: shape, order, the full size on top, every rung over 421,632 pixels; the floor rung is the last
    {
        vgx::floors::SmoothRung t[vgx::floors::kQualityRungs]; vgx::floors::QualityTable(4, t);
        bool allOk = true, ordered = true; long prev = 1L << 40;
        for (int i = 0; i < vgx::floors::kQualityRungs; ++i) {
            int w, h; vgx::floors::LevelSize(640, 448, t[i].s, &w, &h);
            if (!QualityOk(w, h)) allOk = false;
            if (static_cast<long>(w) * h >= prev) ordered = false;
            prev = static_cast<long>(w) * h;
        }
        int w0, h0; vgx::floors::LevelSize(640, 448, t[0].s, &w0, &h0);
        int wl, hl; vgx::floors::LevelSize(640, 448, t[vgx::floors::kQualityRungs - 1].s, &wl, &hl);
        char d[96]; std::snprintf(d, sizeof d, "(top %dx%d, bottom %dx%d = %ld pixels, floor 421632)", w0, h0, wl, hl, static_cast<long>(wl) * hl);
        Check(allOk && ordered && w0 == 960 && h0 == 544 && t[0].ms == 2, "Quality table: 960x544 with 2x smoothing on top, strictly smaller rungs, all at or over 864x488", d);
        Check(QualityOk(wl, hl) && static_cast<long>(wl) * hl < 421632L * 105 / 100, "Quality table: the bottom rung is the floor's own size (within 5% of the floor's pixels)", d);
        // the ladders DynInit makes from it: a ladder step is one rung and the old rungs 0.93 .. 0.50 never come back
        vgx::floors::SmoothRung u[vgx::floors::kQualityRungs]; vgx::floors::QualityTable(0, u);
        Check(u[0].ms == 1, "Quality table: without multisampling in the build the top rung has one sample");
    }
    // a size from a file under the floor goes up to 864x488; a size over it is kept (F2)
    {
        int w, h;
        vgx::floors::ClampFixedSize(640, 362, &w, &h);
        const bool a = g_break == 6 ? true : (w == 864 && h == 488);
        vgx::floors::ClampFixedSize(480, 272, &w, &h);
        const bool b = g_break == 6 ? true : (w == 864 && h == 488);
        vgx::floors::ClampFixedSize(912, 520, &w, &h);
        const bool c = (w == 912 && h == 520);
        vgx::floors::ClampFixedSize(960, 544, &w, &h);
        Check(a && b && c && w == 960 && h == 544, "a fixed size under 864x488 (640x362, 480x272) is raised to 864x488; 912x520 and 960x544 are kept");
    }
    // F3: a held native picture (640x448) is below the Quality floor: the plan holds 864x488; at that size the hold ladder has no step left, at 960x544 two or three
    {
        Policy p;
        const float sc = 0.95f;
        p.scale[1] = sc; p.scale[2] = sc * (0.90f / 0.95f); p.scale[3] = sc * (0.8333f / 0.95f); p.scale[4] = sc * (0.7833f / 0.95f); p.scale[5] = sc * (0.7333f / 0.95f);
        int w = 0, h = 0; vgx::floors::ClampFixedSize(640, 448, &w, &h);
        const int nl = FloorLevels(vgx::floors::kQuality, w, h, p.scale, kSizeSteps);
        char d[64]; std::snprintf(d, sizeof d, "(held %dx%d keeps %d of %d steps)", w, h, nl, kSizeSteps);
        Check(w == 864 && h == 488 && nl == 0, "MSC_PICTURE_HOLD=native: held at 864x488 with no size step below it", d);
    }

    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
