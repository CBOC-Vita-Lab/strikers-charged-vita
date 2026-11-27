// The seam between the picture hold (vgx_hold.h, the Hold* functions of vgx_backend_gl.cpp) and the background-at-30 pair of pictures (vgx_bg30.h). No GL here.
// With the owner's decision of 13 Nov 14:04 (players at 60, everything else at 30) live play is a PAIR: a full picture, then a light one that re-uses the background. The quality
// ladder acts on the BACKGROUND only; the foreground (players, ball, shadows, nets, HUD) keeps the held size. bg30 fills the table below at its init; with no table (bg30 absent
// or MSC_BG30=0) the hold behaves as before.
#pragma once
#include <stdint.h>

namespace vgx {
namespace hold {

struct BgHooks {
    // 0: this picture is not part of a pair; 1: the FULL picture of a pair; 2: the LIGHT picture of a pair. Called once a picture from the render thread, at the hold's tick.
    int (*pairPhase)() = nullptr;
    // Set the background target's level: size level 0..5 (the hold's size levels; 0 the held size), samples 1 or 2, crowd/ambient detail 0 full / 1 reduced. false: it could not be set.
    bool (*bgApply)(int level, int samples, int crowdLow) = nullptr;
    // Is the background target of that size level made and kept (the hold never makes one in play)? level 0 is always true.
    bool (*bgKept)(int level) = nullptr;
    // The hold's verdict on the pair: false when even the bottom of the ladder does not fit 33.3 ms x margin (the game then draws full pictures at a held 30), true when it fits again. Optional.
    void (*pairFits)(bool fits) = nullptr;
    // wave 10 (bg30b): the hold sets `seq` (the present sequence number of the picture whose GPU time it is judging) before it calls pairPhase, and reports that picture's GPU ms by phase.
    uint32_t seq = 0;
    void (*noteGpu)(int phase, float ms) = nullptr;
};

inline BgHooks& Bg() { static BgHooks h; return h; }
inline bool BgReady() { const BgHooks& h = Bg(); return h.pairPhase && h.bgApply && h.bgKept; }

// A pair's GPU time, summed over its two pictures, judged against the pair's own budget: 2 x 16.7 ms times the margin (the pair must fit 33.3 ms with room for its p95).
struct PairJudge {
    float full = 0.0f;
    bool have = false;
    // One picture: the phase and its GPU ms. Returns true when a PAIR is complete; *pairMs then holds the sum. A light picture without its full one (a cut between them) gives false.
    bool Add(int phase, float ms, float* pairMs) {
        if (phase == 1) { full = ms; have = true; return false; }
        if (phase == 2 && have) { *pairMs = full + ms; have = false; return true; }
        have = false;
        return false;
    }
    static float Budget(float margin) { return 2.0f * (1001.0f / 60.0f) * margin; }
};

}  // namespace hold
}  // namespace vgx
