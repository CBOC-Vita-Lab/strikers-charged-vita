// MSC_VGX_FOGVTX: GX fog from the eye-space depth. Pure functions, no GL, no state (tests/fogvtx.cpp runs the real header).
//
// GX's perspective fog is a / (b - z_w) - c, z_w the window depth, clamped to 0..1. The projection (BuildProj in vgx_draw.cpp) and the viewport make
//   z_w = c0 + c1 / ze,   c0 = zn + K (1 - p4),   c1 = -K p5,   K = zf - zn
// (ze the eye-space depth, negative in front of the camera; zn, zf the viewport's depth range as BuildRenderState clamps it; p4, p5 the projection's
// third row, XF 0x24 and 0x25). With b = c0, b - z_w = -c1 / ze, and the factor is  -(a / c1) ze - c : linear in ze. A varying interpolates a linear function
// of the eye depth exactly (perspective-correct interpolation is GL's default), so the vertex shader writes ze and the fragment shader keeps only
// clamp(fa ze + fb, 0, 1). A game sets its fog up so (GXSetFog's b is the projection's far/(far-near)).
// When b is off from c0 by d the old form is  ap / (d - c1/ze) - c  = a Moebius function of ze, not a linear one. GXSetFog's own b is never exactly c0: it
// is stored as a 24-bit mantissa scaled by 8388638 / 2^23 (libogc's constant, kept by this renderer's GXSetFog), so d is about 3.6e-6 of B. The linear form is then
// fitted through the two points the ramp is made of: fa, fb are chosen so that the factor is exactly 0 at the eye depth where the old form reaches 0 and exactly 1 where it
// reaches 1 (the clamp's two edges, where an error would show as a step); in between the two differ by at most  ~ d (ze_end - ze_start) / (4 c1), which LinearOf measures on
// 64 points of the ramp. With d = 0 the fit is the tangent form above and the difference is float rounding.
//   mode 1 (MSC_VGX_FOGVTX=1) takes the linear form when the measured largest difference is at most 2^-17: float rounding (the old form's own float error at the far end of the ramp is larger),
//          which in practice means a b that is exactly c0 (a game that sets the fog up without libogc's constant);
//   mode 2 (=2) when it is at most 2^-11 (an eighth of an 8-bit step of the factor): NOT exact, a bound.
// Both need the projection perspective with c1 > 0 (b - z_w > 0 in front of the camera, so the old form's 1e-6 guard cannot act), the pole of the old form
// (ze = c1 / d) behind the far plane (ze_far = p5 / p4) with a factor of two to spare, and the ramp inside the eye depths in front of the camera.
// The orthographic fog is a * z_w with z_w = zn + K (p4 ze + p5 + 1): affine in ze, so exactly linear (both modes), when the projection is orthographic too.
// Anything else (the fog's kind is not the projection's, an empty or reversed depth range, a value that is not finite) keeps the old text.
// The fog range adjustment (BP 0xE8..0xED) is not implemented in this renderer.
#pragma once
#include "vgx_common.h"
#include <cmath>
#include <cstdint>

namespace vgx::fogvtx {

inline float Float(uint32_t r) { // sign(1) exp(8) mantissa(11) packed in the low 20 bits (BP 0xEE, 0xF1)
    return u2f((bits(r, 19, 1) << 31) | (bits(r, 11, 8) << 23) | (bits(r, 0, 11) << 12));
}
struct Linear {
    bool on = false;
    float fa = 0, fb = 0; // factor before the clamp = fa * ze + fb (the start offset c is in fb)
};
constexpr double kTolExact = 7.62939453125e-6; // 2^-17 (mode 1)
constexpr double kTolBound = 4.8828125e-4;     // 2^-11 (mode 2)

// fogBits: FsKey::fog (fsel | ortho << 3). xf: the XF register file (index = address - 0x1000), bp: the BP register file. mode: 1 or 2 (above).
// err (if given) receives the measured largest difference over the ramp for a perspective fog.
inline Linear LinearOf(uint32_t fogBits, const uint32_t* xf, const uint32_t* bp, int mode = 1, double* err = nullptr) {
    Linear out;
    const bool ortho = (fogBits & 8) != 0;
    if (ortho != ((xf[0x26] & 1) != 0)) return out;
    const float p4 = u2f(xf[0x24]), p5 = u2f(xf[0x25]);
    const float sz = u2f(xf[0x1C]), oz = u2f(xf[0x1F]);
    float zFar = oz / 16777215.0f, zNear = (oz - sz) / 16777215.0f; // (BuildRenderState's arithmetic)
    if (zNear < 0) zNear = 0;
    if (zFar > 1) zFar = 1;
    const double K = static_cast<double>(zFar) - static_cast<double>(zNear);
    const double c = Float(bp[0xF1]), a = Float(bp[0xEE]);
    if (!(K > 0) || !std::isfinite(static_cast<double>(p4)) || !std::isfinite(static_cast<double>(p5)) || !std::isfinite(a) || !std::isfinite(c)) return out;
    double fa, fb;
    if (ortho) {
        const double m10 = 2.0f * p4, m14 = 2.0f * p5 + 1.0f; // (BuildProj's float entries: the depth the GPU sees)
        fa = a * K * 0.5 * m10;
        fb = a * (zNear + K * (0.5 * m14 + 0.5)) - c;
    } else {
        const float sh = std::ldexp(1.0f, static_cast<int>(bits(bp[0xF0], 0, 5)));
        const double ap = static_cast<double>(Float(bp[0xEE]) * sh);                                       // (BuildTev's a', b')
        const double bq = static_cast<double>(static_cast<float>(bp[0xEF]) * sh / 16777216.0f);
        const double m10 = 2.0f * p4 - 1.0f, m14 = 2.0f * p5;                                             // (BuildProj's float entries: z_w = zn + K (1 - m10 / 2 ... see below)
        const double c1 = -K * 0.5 * m14, c0 = zNear + K * (0.5 - 0.5 * m10);                             // ndc = -m10 - m14 / ze,  z_w = zn + K (ndc / 2 + 1 / 2)
        const double d = bq - c0;
        if (!(c1 > 0) || !(ap > 0) || !(K * std::fabs(static_cast<double>(p4)) >= 1e-6)) return out;
        const double zeFar = std::fabs(static_cast<double>(p5) / p4);
        if (d < 0 && !(c1 / -d > 2.0 * zeFar)) return out;                                         // (the pole of the old form is in front of the camera and in view)
        // eye depths (negative) where the old form's factor is 0 and 1: ap / (d - c1/ze) = c + t  =>  ze = c1 / (d - ap / (c + t))
        auto at = [&](double t, double& ze) {
            const double T = c + t;
            if (T == 0) { ze = 0; return true; }
            if (!(T > 0)) return false;
            const double den = d - ap / T;
            if (!(den < 0)) return false;
            ze = c1 / den;
            return std::isfinite(ze);
        };
        double zs, ze1;
        if (!at(0.0, zs) || !at(1.0, ze1) || !(ze1 < zs) || !(ze1 < 0)) return out;
        fa = 1.0 / (ze1 - zs);
        fb = -fa * zs;
        // the largest difference over the ramp
        double worst = 0;
        for (int i = 0; i <= 64; ++i) {
            const double ze = zs + (ze1 - zs) * i / 64.0;
            const double oldF = ap / (d - c1 / (ze == 0 ? -1e-30 : ze)) - c, newF = fa * ze + fb;
            worst = std::fmax(worst, std::fabs(oldF - newF));
        }
        if (err) *err = worst;
        if (!(worst <= (mode >= 2 ? kTolBound : kTolExact))) return out;
        // the new form must also stay on the clamp's side outside the ramp: both are monotone there (the old one up to its pole, behind the far plane)
    }
    if (!std::isfinite(fa) || !std::isfinite(fb)) return out;
    out.on = true;
    out.fa = static_cast<float>(fa);
    out.fb = static_cast<float>(fb);
    return out;
}

} // namespace vgx::fogvtx
