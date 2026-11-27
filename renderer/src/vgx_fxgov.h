// MSC_FX_GOV: when a frame nears its budget, particle sprites thin out before the picture gets smaller (header-only so a test can run it on its own).
#pragma once

namespace vgx {

// Blended fill is the largest GPU cost and a tile GPU cannot reject it, so the cheapest thing to shed is large sprites: thinning additive haze to one in two with twice the alpha
// looks much the same, a smaller picture is seen everywhere. Level: 0 none, 256 one sprite in two, 512 one in four (np/scenecut.h NpSceneGovWeight, chosen by pool slot, no
// random numbers). The value moves `ramp` a frame so sprites fade rather than pop. One measured frame at a time, on the frame's GPU time, render only. It only acts when the
// sprites it could thin came to at least `minFill` screens in the last frame; otherwise it stands aside and the ladder of sizes works as ever. Dynamic resolution is held off
// while a step is left (Holding), and steps back up only when the sprites are all back.
struct FxGovernor {
    bool on = false;
    float enter = 0.85f, leave = 0.70f, minFill = 1.0f;   // of the budget; screens
    int settle = 10, calmFrames = 90, ramp = 32, maxLevel = 512;
    int target = 0, level = 0, wait = 0, calm = 0;
    bool over = false;
    // 1.3 MSC_FX_COMPOSITE: before any sprite is thinned, the effects compositor's accumulation target gets one step smaller (its divisor doubles; vgx_fxcomp.h Div).
    // Render only and cheap to look at, so it is the first step of an overload and the last to come back.
    bool accumOn = false;
    int accum = 0;
    // scenes30 MSC_FX_GOV_HOLD (frames, 0 = as before): a frame whose large sprites came to less than minFill no longer drops the thinning at once; it is kept for this many frames in a row
    // without sprites, and only then let go. The console (23 Nov) showed the governor flap 0 -> 256 -> 512 -> 0 within 4-15 frames in every walk-out, celebration and replay while the GPU stayed
    // at 40-50 ms: a burst of sprites (fireworks, smoke, confetti) ends, one frame reads "no sprites", everything is let go, the next burst starts unthinned and the climb starts again.
    // While no sprites are there a kept level thins nothing (it acts on large sprites only), so keeping it costs nothing and the next burst is thinned from its first frame.
    int absentHold = 0;
    int absent = 0;
    // ema, ms: the GPU time, mean and this frame's; fill: last frame's sprites it could thin, in screens; blocked: a Mega Strike (its own thinning) or the like.
    void Frame(float ema, float ms, float budget, float fill, bool blocked) {
        if (!on) return;
        over = ema > enter * budget || ms > budget;
        if (wait > 0) --wait;
        if (!blocked && fill < minFill && absentHold > 0 && ++absent <= absentHold) {
            calm = 0;   // (no sprites now: nothing to thin and nothing to give back; the level stands)
        } else if (blocked || fill < minFill) {
            target = 0;
            accum = 0;
            calm = 0;
        } else if (over) {
            absent = 0;
            calm = 0;
            if (accumOn && accum == 0 && wait == 0) { accum = 1; wait = settle; }
            else if (target < maxLevel && wait == 0) { target += 256; wait = settle; }
        } else if ((absent = 0), ema < leave * budget) {
            if (++calm >= calmFrames && (target > 0 || accum > 0)) {
                if (target > 0) target -= 256;
                else accum = 0;
                calm = 0;
            }
        } else {
            absent = 0;
            calm = 0;
        }
        if (level < target) level = level + ramp > target ? target : level + ramp;
        else if (level > target) level = level - ramp < target ? target : level - ramp;
    }
    // The ladder of picture sizes waits: thinning has a step left for this overload, or the sprites are not all back yet and there is no overload.
    bool Holding(float fill, bool blocked) const {
        if (!on || blocked) return false;
        if (over) return fill >= minFill && ((accumOn && accum == 0) || target < maxLevel || level < target);
        return target > 0 || level > 0 || accum > 0;
    }
};

// 1.3 FX MSC_FX_LOWRES_MAXH: the size of the effects pass's target for a render target of rtW x rtH, a side divisor div (2 or 4) and a cap on its lines (0: none).
// Uncapped: each side divided, rounded up (as since 23 Oct). Capped: cap lines, the width in proportion. effDiv: the divisor the game is told (VgxFxPassDiv), rounded up.
inline void FxTargetDims(int rtW, int rtH, int div, int cap, int& w, int& h, int& effDiv) {
    if (div < 1) div = 1;
    w = (rtW + div - 1) / div;
    h = (rtH + div - 1) / div;
    if (cap > 0 && h > cap) {
        w = static_cast<int>((static_cast<long long>(w) * cap * 2 + h) / (2LL * h));   // (rounded to nearest)
        if (w < 1) w = 1;
        h = cap;
    }
    effDiv = h > 0 ? (rtH + h - 1) / h : div;
    if (effDiv < div) effDiv = div;
}

} // namespace vgx
