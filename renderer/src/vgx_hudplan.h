// MSC_VGX_HUDNATIVE: which commands of a frame can be drawn on the display at its own size, on top of the upscaled scene, instead of through the render target.
// Kept apart from GL so that a host test (native/tools/render/run_hudplan_test.sh) can check the rules.
//
// The run is the longest tail of the frame before its display copy in which every command is either a draw of a 2D layer (the game's eCLV order: FrontEnd and after) or something
// that does not depend on the target. It stops at anything that needs the render target to hold the HUD (a copy that reads the EFB, a pass, a new target size), at a draw of a 3D layer, at
// an effect-pass draw (FX runs read the EFB's depth), at a draw that blends with the destination's alpha (the display's alpha is 1 under the picture) and at shader
// work (a warm-up may change the target). A depth-only clear of the whole EFB stays in the run: it is how a transition wipe gets rid of the depth the HUD wrote.
#pragma once
#include <cstddef>
#include "vgx_cmd.h"

namespace vgx::backend {

constexpr int kHudFirstLayer = 37; // eCLV_FrontEnd

struct HudPlan {
    int start = -1;      // the first command of the run (a draw); -1: none
    int disp = -1;       // the display copy
    uint32_t draws = 0;
    uint32_t depthDraws = 0; // draws of the run that test depth: the run needs its own depth buffer cleared
};
inline bool HudDrawOk(const DrawRec& d, int firstLayer) {
    if (d.layer < firstLayer || d.fx) return false;
    const RenderState& r = d.rs;
    const auto dstA = [](uint8_t f) { return f == BF_DST_ALPHA || f == BF_INV_DST_ALPHA; };
    return !(r.blend && (dstA(r.blendSrc) || dstA(r.blendDst)));
}
inline bool HudClearOk(const CopyTexCmd& t, int efbW, int efbH) {
    return t.skip && !t.fromPass && !t.srcTex && t.clear.enable && !t.clear.color && !t.clear.alpha && t.src.x <= 0 && t.src.y <= 0 && t.src.x + t.src.w >= efbW && t.src.y + t.src.h >= efbH;
}
inline HudPlan PlanHudRun(const Cmd* cmds, size_t n, const DrawRec* draws, const CopyTexCmd* copies, int efbW, int efbH, int firstLayer = kHudFirstLayer) {
    HudPlan p;
    int d = -1;
    for (size_t i = n; i-- > 0;)
        if (cmds[i].type == CMD_COPY_DISP) { d = static_cast<int>(i); break; }
    if (d < 0) return p;
    p.disp = d;
    uint32_t nDraws = 0, nDepth = 0;
    for (int i = d - 1; i >= 0; --i) {
        const Cmd& c = cmds[i];
        if (c.type == CMD_DRAW) {
            const DrawRec& r = draws[c.arg];
            if (!HudDrawOk(r, firstLayer)) break;
            p.start = i;
            ++nDraws;
            if (r.rs.depthTest) ++nDepth;
            p.draws = nDraws;
            p.depthDraws = nDepth;
        } else if (c.type == CMD_COPY_TEX) {
            if (!HudClearOk(copies[c.arg], efbW, efbH)) break;
        } else if (c.type != CMD_TEX_UPLOAD && c.type != CMD_TEX_FREE && c.type != CMD_BUF_UPLOAD && c.type != CMD_BUF_FREE) {
            break;
        }
    }
    return p;
}

} // namespace vgx::backend
