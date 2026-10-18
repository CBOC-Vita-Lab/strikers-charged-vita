// GX Z-freeze (GXSetCoPlanar, genMode bit 19): the plane arithmetic. Pure functions, no GL, no state.
//
// What the hardware does (as the fork's own desktop renderer, port/aurora-main/lib/gx/command_processor.cpp:2217, and
// Dolphin describe it): a triangle set up with Z-freeze off leaves its depth plane (z = a x + b y + c in window
// coordinates) in the rasteriser, even when it is culled, which is how games hand it an invisible reference; triangles drawn
// with Z-freeze on do not compute a depth plane of their own, every pixel of them takes its depth from that reference
// plane. All frozen triangles then have bit-identical depth wherever they overlap. Strikers uses it for its planar player
// shadows and ground decals (eCLV_CoPlanar layer: the game puts a culled quad at the "anti-flimmer" height, then draws the
// flattened shadow mesh frozen with a strict LESS depth test and blending, so that overlapping triangles of the mesh draw
// each pixel once instead of darkening it again; RLView.cpp BeginPacket, RenderShadow.cpp DrawPlanarShadow / fn_8018680C).
// This renderer used to ignore the bit: the overlapping triangles then get depths that agree only to rounding, and the
// strict test lets some pixels through twice, a different set from frame to frame.
#pragma once
#include <cmath>

namespace vgx::zfreeze {

// The viewport of a draw as the back end sets it (EFB pixels, y as the GL window sees it: this renderer flips Y in its
// projection, so GL window y is the EFB row counted from the top) and its depth range.
struct Viewport {
    float x = 0, y = 0, w = 1, h = 1, zNear = 0, zFar = 1;
};
// z_window = a * x_window + b * y_window + c, window coordinates in EFB pixels, z in the depth range [zNear, zFar].
struct Plane {
    float a = 0, b = 0, c = 0;
};

// clip = proj * (mv * (pos, 1), 1), exactly the vertex shader's arithmetic: mv is three rows of four (row-major, as
// uploaded), proj is column-major 4x4 (UB_PROJ).
inline void ModelToClip(const float pos[3], const float mv[12], const float proj[16], float clip[4]) {
    float e[4];
    for (int r = 0; r < 3; ++r) e[r] = mv[r * 4] * pos[0] + mv[r * 4 + 1] * pos[1] + mv[r * 4 + 2] * pos[2] + mv[r * 4 + 3];
    e[3] = 1.0f;
    for (int r = 0; r < 4; ++r) clip[r] = proj[r] * e[0] + proj[4 + r] * e[1] + proj[8 + r] * e[2] + proj[12 + r] * e[3];
}

inline void ClipToWindow(const float clip[4], const Viewport& vp, float win[3]) {
    const float iw = 1.0f / clip[3];
    win[0] = vp.x + (clip[0] * iw * 0.5f + 0.5f) * vp.w;
    win[1] = vp.y + (clip[1] * iw * 0.5f + 0.5f) * vp.h;
    win[2] = vp.zNear + (clip[2] * iw * 0.5f + 0.5f) * (vp.zFar - vp.zNear);
}

// The plane through three clip-space vertices. False when a vertex is not in front of the eye or the triangle is edge-on
// (the hardware would get no usable slope either): the caller keeps its previous plane.
inline bool PlaneFromClip(const float clip[3][4], const Viewport& vp, Plane& out) {
    float win[3][3];
    for (int i = 0; i < 3; ++i) {
        if (!(clip[i][3] > 1e-6f)) return false;
        ClipToWindow(clip[i], vp, win[i]);
    }
    const float e1[3] = {win[1][0] - win[0][0], win[1][1] - win[0][1], win[1][2] - win[0][2]};
    const float e2[3] = {win[2][0] - win[0][0], win[2][1] - win[0][1], win[2][2] - win[0][2]};
    const float nx = e1[1] * e2[2] - e1[2] * e2[1];
    const float ny = e1[2] * e2[0] - e1[0] * e2[2];
    const float nz = e1[0] * e2[1] - e1[1] * e2[0];
    if (std::fabs(nz) < 1e-6f) return false;
    const float a = -nx / nz, b = -ny / nz;
    const float c = win[0][2] - a * win[0][0] - b * win[0][1];
    if (!std::isfinite(a) || !std::isfinite(b) || !std::isfinite(c)) return false;
    out = Plane{a, b, c};
    return true;
}

inline float Eval(const Plane& p, float x, float y) { return p.a * x + p.b * y + p.c; }

} // namespace vgx::zfreeze
