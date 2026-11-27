// aa24 (wave 24) MSC_VGX_PPAA=0|1|2: post-process edge smoothing in the display copy (the picture pass), for ONE-SAMPLE pictures only. Render only: nothing here reads or writes game state.
//   0 off (code default). 1 the Mega Strike scenes (the minigame, and the captain's launch / result cut-scenes by the game's presentation name). 2 also every held scene (replay, cut-scene).
// Why: the Mega scene and its cut-scenes are drawn with one sample (the effects pass needs a linear depth texture, so no MSAA), and the edges of the ball and its spikes are 1 sample polygon steps.
// How: the picture program of the mode in force (COMBINED, the plan's; or SMOOTH) with an edge branch in the FXAA 3.11 manner (vgx_backend_gl.cpp PpaaEdgeBody, kPicCombinedPpaa / kPicSmoothPpaa).
// The HUD is not filtered: the pass is used only when no 2D layer draw (layer >= 37) is left in the picture, i.e. the HUD run of MSC_VGX_HUDNATIVE is drawn on the display after it, or there is no HUD.
// The GPU gate below skips the pass while the scene class is over budget. Header-only so a host test runs the shipped rules (renderer/tests/ppaa.cpp).
#pragma once
#include <algorithm>

namespace vgxppaa {

// Does the scene class use the pass? mode 1: Mega (the scene, or the launch / result cut hint). mode 2: also a held replay or cut-scene (kind 2 / 3 of vgx_lock60.h).
inline bool ClassOn(int mode, bool mega, bool megaCutHint, int lockKind) {
    if (mode <= 0) return false;
    if (mega || megaCutHint || lockKind == 4) return true;
    return mode >= 2 && (lockKind == 2 || lockKind == 3);
}

// The GPU gate. ms: the GPU time of a measured picture; budget: the picture's own budget (33.4 ms at 30). The pass is skipped while the mean is over `enter` of the budget and comes back under `leave`
// after `calm` measured pictures (it is part of what is measured, so the two thresholds are apart: its own cost, about half a millisecond, cannot make it flap).
struct Gate {
    float enter = 0.95f, leave = 0.85f;
    int calm = 20, calmN = 0;
    float ema = -1.0f;
    bool skip = false;
    unsigned moves = 0;
    void Frame(float ms, float budget) {
        if (ms <= 0.05f || budget <= 0.0f) return;
        ema = ema < 0.0f ? ms : 0.75f * ema + 0.25f * ms;
        if (!skip) {
            if (ema > enter * budget || ms > 1.15f * budget) { skip = true; calmN = 0; ++moves; }
        } else if (ema < leave * budget) {
            if (++calmN >= calm) { skip = false; calmN = 0; ++moves; }
        } else {
            calmN = 0;
        }
    }
    void Reset() { ema = -1.0f; skip = false; calmN = 0; }   // a new scene class starts with the pass on
};

// May the pass be used for this picture? one sample, the HUD out of the picture, the class on, the gate open.
inline bool Use(int mode, bool classOn, int samples, bool hudClean, bool gateSkip) { return mode > 0 && classOn && samples <= 1 && hudClean && !gateSkip; }

}  // namespace vgxppaa
