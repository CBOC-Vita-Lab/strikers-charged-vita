// MSC_VGX_CULL: is a box of stored positions wholly outside a draw's scissor rectangle? (header-only so a test can run it on its own)
#pragma once

namespace vgx {

// The four sides of the scissor, as a running "every corner so far is outside this side" state. Several position matrices (the rows of a matrix palette a draw's
// vertices may name) are tested into the same state: the box is outside only when every corner under every matrix is outside the same side.
struct CullSides {
    bool left = true, right = true, top = true, bottom = true;
    bool Any() const { return left | right | top | bottom; }
};

// box: min xyz, max xyz in the space the position matrix takes. mtx: the position matrix as GX holds it (3 rows of 4). proj: the six projection
// parameters of XF registers 0x20..0x25; ortho: register 0x26 bit 0. vp*: the viewport as BuildRenderState gives it (x, y of its top-left corner, width, height),
// sc*: the scissor rectangle (x, y, width, height), both in the pixels of the EFB. margin: pixels added to the scissor on every side.
// A box is wholly outside when no point of any triangle with a vertex in the box can lie inside the widened scissor, whatever the triangles are. Proof: a pixel of a
// draw is inside the scissor, so the point it is drawn from has its screen position (cx + hs * x/w, cy - vh * y/w) there with w > 0 (a point with w <= 0 is
// clipped away). That is the half-space `hs*x + (cx - Left)*w >= 0` (and its three siblings) of the clip coordinates (x, y, w), which are affine in the
// stored position, so the set of points that satisfy it is convex and its complement (what the test needs all 8 corners of the box to lie in) contains the
// box's hull when it contains the corners. NaN fails every comparison, so a box whose numbers are not finite is kept.
// Returns false as soon as no side is left (the box is not outside).
inline bool CullBoxSides(CullSides& sd, const float* box, const float* mtx, const float* proj, bool ortho, float vpX, float vpY, float vpW, float vpH, float scX,
                         float scY, float scW, float scH, float margin) {
    const float* m0 = mtx;
    const float* m1 = mtx + 4;
    const float* m2 = mtx + 8;
    const float hs = vpW * 0.5f, vh = vpH * 0.5f;
    const float cxp = vpX + hs, cyp = vpY + vh;
    const float L = scX - margin, R = scX + scW + margin;
    const float T = scY - margin, B = scY + scH + margin;
    for (int c = 0; c < 8; ++c) {
        const float x = box[(c & 1) ? 3 : 0], y = box[(c & 2) ? 4 : 1], z = box[(c & 4) ? 5 : 2];
        const float xe = m0[0] * x + m0[1] * y + m0[2] * z + m0[3];
        const float ye = m1[0] * x + m1[1] * y + m1[2] * z + m1[3];
        const float ze = m2[0] * x + m2[1] * y + m2[2] * z + m2[3];
        float xc, yc, w;
        if (!ortho) { xc = proj[0] * xe + proj[1] * ze; yc = proj[2] * ye + proj[3] * ze; w = -ze; }
        else { xc = proj[0] * xe + proj[1]; yc = proj[2] * ye + proj[3]; w = 1.0f; }
        const float fx = hs * xc, fy = -vh * yc;
        sd.left = sd.left && (fx + (cxp - L) * w < 0.0f);
        sd.right = sd.right && (fx + (cxp - R) * w > 0.0f);
        sd.top = sd.top && (fy + (cyp - T) * w < 0.0f);
        sd.bottom = sd.bottom && (fy + (cyp - B) * w > 0.0f);
        if (!sd.Any()) return false;
    }
    return true;
}

// One position matrix (a draw whose vertices name none).
inline bool CullBoxOutsideScissor(const float* box, const float* mtx, const float* proj, bool ortho, float vpX, float vpY, float vpW, float vpH, float scX, float scY,
                                  float scW, float scH, float margin) {
    CullSides sd;
    return CullBoxSides(sd, box, mtx, proj, ortho, vpX, vpY, vpW, vpH, scX, scY, scW, scH, margin);
}

} // namespace vgx
