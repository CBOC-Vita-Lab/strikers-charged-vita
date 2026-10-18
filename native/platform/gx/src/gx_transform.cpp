// GX glue, part 6: viewport and projection (with the dynamic-aspect correction), getters, GXProject.
//
// The game's perspective projections are written for a 16:9 picture (the anamorphic EFB of the widescreen mode). The
// recompiled build widens full-screen perspective projections to the shape of the output surface (port/runtime/src/
// dynamic_aspect.cpp, hle/gx/gx_transform.cpp: Hor+ above 16:9, Vert+ below it), so that the 3D fills any window without
// bars and without squeezing; this file does the same, by the same rules, for the native game:
//  * only perspective projections, only when the game runs in its widescreen mode (SCGetAspectRatio) and the correction is
//    not switched off (np_gx_set_dynamic_aspect);
//  * only while the viewport is full screen (width >= 600 and height >= 400), or when the projection is the very matrix of
//    the last full-screen one (the heat haze draws its distortion at half size and must line up with the scene);
//  * row 0 of the matrix is divided by the horizontal expansion max(k, 1), row 1 by the vertical one max(1/k, 1), where
//    k = (surface width / height) / (16 / 9), the aspect clamped from below at (16/9)/6;
//  * the game reads back its OWN matrix (GXGetProjectionv) and viewport (GXGetViewportv); a projection set before its
//    viewport is issued again when the viewport says which pass it belongs to.
// The surface is what the renderer reports (vgxn_surface_size: np_gx_config, 640 x 480 by default, 960 x 544 on the Vita).
#include <revolution/gx.h>

#include "np_gx.h"
#include "vgx_native.h"

// The SDK layer's SCGetAspectRatio (weak: absent in the standalone tests): 1 = the game runs in 16:9.
extern "C" unsigned char SCGetAspectRatio(void) __attribute__((weak));

namespace {
int g_dynAspectMode = -1;            // -1: follow SCGetAspectRatio (no SDK layer: on), 0: off, 1: on
float g_projVec[7] = {0.0f, 1.0f, 0.0f, 1.0f, 0.0f, -1.0f, 0.0f};   // the six coefficients + type as the game set them
float g_viewport[6] = {0.0f, 0.0f, 640.0f, 480.0f, 0.0f, 1.0f};   // as the game set it
float g_lastPersp[16];
bool g_lastWasPersp = false, g_lastPerspAdjusted = false;
float g_sceneProj[16];
bool g_haveScene = false;

inline bool DynAspectOn() {
    if (g_dynAspectMode >= 0) return g_dynAspectMode != 0;
    return SCGetAspectRatio ? SCGetAspectRatio() != 0 : true;
}
inline bool ViewportIsFullScreen() { return g_viewport[2] >= 600.0f && g_viewport[3] >= 400.0f; }
bool SameMatrix(const float* a, const float* b) {
    for (int i = 0; i < 16; ++i)
        if (!(a[i] == b[i])) return false;   // (a NaN never matches, as memcmp of NaN payloads rarely would)
    return true;
}
bool WidensProjection(const float raw[16]) {
    if (ViewportIsFullScreen()) return true;
    return g_haveScene && SameMatrix(raw, g_sceneProj);
}
// Divides the two rows by the expansions; false when nothing changes.
bool AdjustPerspective(float m[16]) {
    if (!DynAspectOn()) return false;
    unsigned int w = 0, h = 0;
    vgxn_surface_size(&w, &h);
    float aspect = 16.0f / 9.0f;
    if (w != 0 && h != 0) {
        aspect = static_cast<float>(w) / static_cast<float>(h);
        const float kMin = (16.0f / 9.0f) / 6.0f;
        if (!(aspect > 0.0f) || !(aspect < 1e30f)) aspect = 1.0f;   // (not finite)
        if (aspect < kMin) aspect = kMin;
    }
    const float k = aspect / (16.0f / 9.0f);
    const float horizontal = k > 1.0f ? k : 1.0f;
    float vertical = 1.0f;
    if (k > 0.0f) {
        vertical = 1.0f / k;
        if (vertical < 1.0f) vertical = 1.0f;
        if (vertical > 6.0f) vertical = 6.0f;
    }
    if (horizontal == 1.0f && vertical == 1.0f) return false;
    for (int i = 0; i < 4; ++i) {
        m[i] /= horizontal;
        m[4 + i] /= vertical;
    }
    return true;
}

void SubmitProjection(const float raw[16], GXProjectionType type) {
    g_lastWasPersp = type == GX_PERSPECTIVE;
    g_lastPerspAdjusted = false;
    if (g_lastWasPersp) {
        for (int i = 0; i < 16; ++i) g_lastPersp[i] = raw[i];
        float adjusted[16];
        for (int i = 0; i < 16; ++i) adjusted[i] = raw[i];
        if (WidensProjection(raw) && AdjustPerspective(adjusted)) {
            if (ViewportIsFullScreen()) {
                for (int i = 0; i < 16; ++i) g_sceneProj[i] = raw[i];
                g_haveScene = true;
            }
            g_lastPerspAdjusted = true;
            vgxapi_GXSetProjection(reinterpret_cast<const float(*)[4]>(adjusted), static_cast<unsigned int>(type));
            return;
        }
    }
    vgxapi_GXSetProjection(reinterpret_cast<const float(*)[4]>(raw), static_cast<unsigned int>(type));
}
} // namespace

extern "C" void np_gx_set_dynamic_aspect(int mode) { g_dynAspectMode = mode; }

extern "C" {

void GXSetProjection(const f32 mtx[4][4], GXProjectionType type) {
    const f32* m = &mtx[0][0];
    SubmitProjection(m, type);
    // the coefficients the game would read back (the six values GXSetProjection writes to the hardware)
    g_projVec[0] = type == GX_PERSPECTIVE ? 0.0f : 1.0f;
    g_projVec[1] = m[0];
    g_projVec[3] = m[5];
    g_projVec[5] = m[10];
    g_projVec[6] = m[11];
    if (type == GX_PERSPECTIVE) {
        g_projVec[2] = m[2];
        g_projVec[4] = m[6];
    } else {
        g_projVec[2] = m[3];
        g_projVec[4] = m[7];
    }
}

// proj[0]: 0 perspective / 1 orthographic; proj[1..6]: the six projection coefficients as the hardware holds them.
void GXGetProjectionv(f32 proj[GX_PROJECTION_SZ]) {
    for (int i = 0; i < 7; ++i) proj[i] = g_projVec[i];
}

void GXSetProjectionv(const f32 proj[GX_PROJECTION_SZ]) {
    // Rebuilt as the matrix GXSetProjection takes (the six coefficients and the type are written from it unchanged).
    f32 m[4][4];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) m[r][c] = 0.0f;
    m[0][0] = proj[1];
    m[1][1] = proj[3];
    m[2][2] = proj[5];
    m[2][3] = proj[6];
    const GXProjectionType type = proj[0] == 0.0f ? GX_PERSPECTIVE : GX_ORTHOGRAPHIC;
    if (type == GX_PERSPECTIVE) {
        m[0][2] = proj[2];
        m[1][2] = proj[4];
        m[3][2] = -1.0f;
    } else {
        m[0][3] = proj[2];
        m[1][3] = proj[4];
        m[3][3] = 1.0f;
    }
    SubmitProjection(&m[0][0], type);
    for (int i = 0; i < 7; ++i) g_projVec[i] = proj[i];
}

void GXSetViewport(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz) {
    g_viewport[0] = left;
    g_viewport[1] = top;
    g_viewport[2] = wd;
    g_viewport[3] = ht;
    g_viewport[4] = nearz;
    g_viewport[5] = farz;
    vgxapi_GXSetViewport(left, top, wd, ht, nearz, farz);
    // A projection set before its viewport: issue it again now that the viewport says which pass it is.
    if (g_lastWasPersp && g_lastPerspAdjusted != WidensProjection(g_lastPersp)) {
        float raw[16];
        for (int i = 0; i < 16; ++i) raw[i] = g_lastPersp[i];
        SubmitProjection(raw, GX_PERSPECTIVE);
    }
}

// The viewport as GXSetViewport was given it: left, top, width, height, near, far.
void GXGetViewportv(f32 view[GX_VIEWPORT_SZ]) {
    for (int i = 0; i < 6; ++i) view[i] = g_viewport[i];
}

void GXProject(f32 x, f32 y, f32 z, const f32 mtx[3][4], const f32* pm, const f32* vp, f32* sx, f32* sy, f32* sz) {
    const f32 px = mtx[0][0] * x + mtx[0][1] * y + mtx[0][2] * z + mtx[0][3];
    const f32 py = mtx[1][0] * x + mtx[1][1] * y + mtx[1][2] * z + mtx[1][3];
    const f32 pz = mtx[2][0] * x + mtx[2][1] * y + mtx[2][2] * z + mtx[2][3];
    f32 xc, yc, zc, wc;
    if (pm[0] == static_cast<f32>(GX_PERSPECTIVE)) {
        xc = px * pm[1] + pz * pm[2];
        yc = py * pm[3] + pz * pm[4];
        zc = pz * pm[5] + pm[6];
        wc = 1.0f / -pz;
    } else {
        xc = px * pm[1] + pm[2];
        yc = py * pm[3] + pm[4];
        zc = pz * pm[5] + pm[6];
        wc = 1.0f;
    }
    *sx = xc * vp[2] / 2 * wc + vp[0] + vp[2] / 2;
    *sy = -yc * vp[3] / 2 * wc + vp[1] + vp[3] / 2;
    *sz = zc * (vp[5] - vp[4]) * wc + vp[5];
}

} // extern "C"
