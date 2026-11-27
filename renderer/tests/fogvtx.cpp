// MSC_VGX_FOGVTX: the per-vertex (eye-depth) form of GX fog against the per-pixel form the shader had, on random GX fog parameters and triangles.
// The real header (vgx_fogvtx.h) decides which draws take the linear form; the test evaluates both forms for pixels of random triangles:
//   old:  clamp(a' / max(b' - z_w, 1e-6) - c, 0, 1)   (orthographic: clamp(a z_w - c, 0, 1)),  z_w = window depth, linear in screen space (what gl_FragCoord.z holds)
//   new:  clamp(fa * ze + fb, 0, 1),  ze = the eye-space depth interpolated perspective-correctly (a varying)
// with the projection matrix BuildProj builds and the viewport depth range BuildRenderState gives. Everything in double, then both again with float arithmetic as the shader does it.
// Run by run_fogvtx_test.sh. `fogvtx mutate` breaks the coefficient: the test must then fail (the deliberate break).
#include "../src/vgx_fogvtx.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

using namespace vgx;
using namespace vgx::fogvtx;
static std::mt19937_64 rng(20261106);
static double U(double a, double b) { return a + (b - a) * std::uniform_real_distribution<double>(0, 1)(rng); }
static uint32_t F2U(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
// GX's 20-bit float (sign 1, exponent 8, mantissa 11) of BP 0xEE / 0xF1: the mantissa rounded to 11 bits
static uint32_t EncodeFog(double v) {
    const uint32_t u = F2U(static_cast<float>(v));
    const uint32_t m = ((u & 0x7FFFFFu) + 0x800u) >> 12; // round to nearest at bit 12
    uint32_t e = (u >> 23) & 0xFF, sign = u >> 31;
    uint32_t mm = m;
    if (mm >> 11) { mm = 0; ++e; }
    return (sign << 19) | (e << 11) | (mm & 0x7FF);
}
static bool g_mutate = false;
static int g_mode = 1;

struct Case {
    uint32_t xf[0x58] = {};
    uint32_t bp[256] = {};
    uint32_t fogBits = 0;
    bool ortho = false;
    double n = 0, f = 0;
    float zn = 0, zf = 1;
    double m[16] = {};
};
// vgxapi GXSetFog's arithmetic (vgx_api.cpp, libogc's: B scaled with 8388638, the 20-bit floats cut at the 11th mantissa bit)
static uint32_t Cut(float v) { return F2U(v) >> 12; }
static void SetFog(Case& c, double startz, double endz, double nearz, double farz, bool ortho, int fsel, bool exactB = false) {
    c.ortho = ortho;
    c.fogBits = static_cast<uint32_t>(fsel) | (ortho ? 8u : 0u);
    const float startf = static_cast<float>(startz), endf = static_cast<float>(endz), nearf = static_cast<float>(nearz), farf = static_cast<float>(farz);
    float a, cc;
    uint32_t bMant = 0, bShift = 0;
    if (ortho) {
        const float A = 1.0f / (endf - startf);
        a = A * (farf - nearf);
        cc = A * (startf - nearf);
    } else {
        const float A = (farf * nearf) / ((farf - nearf) * (endf - startf)), B = farf / (farf - nearf), C = startf / (endf - startf);
        float bm = B;
        int expn = 0;
        while (bm > 1.0f) { bm *= 0.5f; ++expn; }
        while (bm > 0.0f && bm < 0.5f) { bm *= 2.0f; --expn; }
        a = A / static_cast<float>(1 << (expn + 1));
        bMant = static_cast<uint32_t>(bm * (exactB ? 8388608.0f : 8388638.0f));
        bShift = static_cast<uint32_t>(expn + 1);
        cc = C;
    }
    c.bp[0xEE] = Cut(a);
    c.bp[0xEF] = bMant & 0xFFFFFF;
    c.bp[0xF0] = bShift & 31;
    c.bp[0xF1] = Cut(cc) | ((ortho ? 1u : 0u) << 20) | (static_cast<uint32_t>(fsel) << 21);
}
static void SetProj(Case& c, double n, double f, bool ortho, double zn, double zf) {
    c.ortho = ortho;
    c.n = n; c.f = f;
    const double tmp = 1.0 / (f - n);
    auto put = [&](int i, double v) { c.xf[0x20 + i] = F2U(static_cast<float>(v)); };
    if (!ortho) {
        put(0, U(0.7, 2.0)); put(1, U(-0.1, 0.1)); put(2, U(0.9, 2.5)); put(3, U(-0.1, 0.1));
        put(4, -n * tmp); put(5, -f * n * tmp);
        c.xf[0x26] = 0;
    } else {
        put(0, U(0.001, 0.01)); put(1, U(-1, 1)); put(2, U(0.001, 0.01)); put(3, U(-1, 1));
        put(4, -tmp); put(5, -f * tmp);
        c.xf[0x26] = 1;
    }
    // viewport depth: oz = zf * 16777215, sz = (zf - zn) * 16777215 (GXSetViewport's near/far)
    c.xf[0x1F] = F2U(static_cast<float>(zf * 16777215.0));
    c.xf[0x1C] = F2U(static_cast<float>((zf - zn) * 16777215.0));
    // BuildProj's matrix (column-major) and BuildRenderState's depth range
    const float p[6] = {u2f(c.xf[0x20]), u2f(c.xf[0x21]), u2f(c.xf[0x22]), u2f(c.xf[0x23]), u2f(c.xf[0x24]), u2f(c.xf[0x25])};
    std::memset(c.m, 0, sizeof c.m);
    if (!ortho) { c.m[0] = p[0]; c.m[8] = p[1]; c.m[5] = -p[2]; c.m[9] = -p[3]; c.m[10] = 2.0f * p[4] - 1.0f; c.m[14] = 2.0f * p[5]; c.m[11] = -1.0f; }
    else { c.m[0] = p[0]; c.m[12] = p[1]; c.m[5] = -p[2]; c.m[13] = -p[3]; c.m[10] = 2.0f * p[4]; c.m[14] = 2.0f * p[5] + 1.0f; c.m[15] = 1.0f; }
    const float oz = u2f(c.xf[0x1F]), sz = u2f(c.xf[0x1C]);
    c.zf = oz / 16777215.0f; c.zn = (oz - sz) / 16777215.0f;
    if (c.zn < 0) c.zn = 0;
    if (c.zf > 1) c.zf = 1;
}
static void Clip(const Case& c, const double e[3], double out[4]) {
    for (int r = 0; r < 4; ++r) out[r] = c.m[r] * e[0] + c.m[4 + r] * e[1] + c.m[8 + r] * e[2] + c.m[12 + r];
}

struct Stat { double maxD = 0, maxF = 0, maxOldF = 0; long px = 0, flips = 0, onCases = 0, offCases = 0, edge = 0; };

// one case, many pixels. Returns false on a violation.
static bool Run(const Case& c, Stat& st, double* worstBound, bool verbose) {
    double err = 0;
    Linear L = LinearOf(c.fogBits, c.xf, c.bp, g_mode, &err);
    if (!L.on) { ++st.offCases; return true; }
    ++st.onCases;
    if (g_mutate) L.fa = -L.fa;
    // the parameters as the shader's uniform holds them
    const float sh = std::ldexp(1.0f, static_cast<int>(c.bp[0xF0] & 31));
    const float ap = Float(c.bp[0xEE]) * sh, bq = static_cast<float>(c.bp[0xEF]) * sh / 16777216.0f, cc = Float(c.bp[0xF1]);
    const double a = Float(c.bp[0xEE]);
    bool ok = true;
    const double tolRamp = c.ortho ? 0.0 : 1.25 * (g_mode >= 2 ? kTolBound : kTolExact); // (the measured ramp bound, plus the pixels between its samples)
    for (int t = 0; t < 40; ++t) {
        double e[3][3], cl[3][4], zw[3], xs[3], ys[3];
        bool visible = true;
        for (int i = 0; i < 3; ++i) {
            const double depth = c.ortho ? U(c.n * 1.0001, c.f * 0.9999) : std::exp(U(std::log(c.n * 1.001), std::log(c.f * 0.999)));
            e[i][2] = -depth;
            e[i][0] = U(-0.4, 0.4) * (c.ortho ? 100.0 : depth);
            e[i][1] = U(-0.3, 0.3) * (c.ortho ? 100.0 : depth);
            Clip(c, e[i], cl[i]);
            if (!(cl[i][3] > 0)) visible = false;
            const double ndc = cl[i][2] / cl[i][3];
            zw[i] = c.zn + (ndc * 0.5 + 0.5) * (static_cast<double>(c.zf) - c.zn);
            xs[i] = cl[i][0] / cl[i][3]; ys[i] = cl[i][1] / cl[i][3];
        }
        if (!visible) continue;
        (void)xs; (void)ys;
        for (int s = 0; s < 40; ++s) {
            double l[3] = {U(0, 1), U(0, 1), U(0, 1)};
            if (s % 8 == 0) { l[0] = 1; l[1] = l[2] = 0; } // a vertex
            const double sum = l[0] + l[1] + l[2];
            for (double& v : l) v /= sum;
            const double zwp = l[0] * zw[0] + l[1] * zw[1] + l[2] * zw[2];              // screen-linear: gl_FragCoord.z
            double num = 0, den = 0;
            for (int i = 0; i < 3; ++i) { num += l[i] * e[i][2] / cl[i][3]; den += l[i] / cl[i][3]; }
            const double ze = num / den;                                                // perspective-correct: the varying
            auto clamp01 = [](double v) { return v < 0 ? 0.0 : (v > 1 ? 1.0 : v); };
            double oldD;
            if (c.ortho) oldD = clamp01(a * zwp - Float(c.bp[0xF1]));
            else oldD = clamp01(static_cast<double>(ap) / std::fmax(static_cast<double>(bq) - zwp, 1e-6) - cc);
            const double newD = clamp01(static_cast<double>(L.fa) * ze + L.fb);
            // float arithmetic as the shader: gl_FragCoord.z and the varying are floats
            const float zwf = static_cast<float>(zwp), zef = static_cast<float>(ze);
            float oldF;
            if (c.ortho) oldF = std::fmin(std::fmax(static_cast<float>(a) * zwf - cc, 0.0f), 1.0f);
            else oldF = std::fmin(std::fmax(ap / std::fmax(bq - zwf, 0.000001f) - cc, 0.0f), 1.0f);
            const float newF = std::fmin(std::fmax(L.fa * zef + L.fb, 0.0f), 1.0f);
            const double dD = std::fabs(oldD - newD), dF = std::fabs(static_cast<double>(newF) - oldD), dOldF = std::fabs(static_cast<double>(oldF) - oldD);
            ++st.px;
            if (oldD > 0 && oldD < 1) ++st.edge;
            if (dD > st.maxD) st.maxD = dD;
            if (dF > st.maxF) st.maxF = dF;
            if (dOldF > st.maxOldF) st.maxOldF = dOldF;
            // an 8-bit mix step: does the colour mix(x, fogcolour, fog) rounded to 8 bits differ? (colour 0 -> 255: 255 * fog)
            if (std::floor(oldD * 255.0 + 0.5) != std::floor(static_cast<double>(newF) * 255.0 + 0.5)) ++st.flips;
            const double bound = tolRamp + 4.0 * 5.96e-8 * (std::fabs(static_cast<double>(L.fa) * ze) + std::fabs(static_cast<double>(L.fb)) + 1.0); // (+ the float rounding of fa * ze + fb)
            if (dD > bound) {
                static int shown = 0; if (verbose && shown++ < 6) std::fprintf(stderr, "violation: %s old %.9f new %.9f diff %.3g (bound %.3g, err bound %.3g, ze %.4f)\n", c.ortho ? "ortho" : "persp", oldD, newD, dD, bound, err, ze);
                ok = false;
            }
        }
    }
    if (worstBound && err > *worstBound) *worstBound = err;
    return ok;
}

int main(int argc, char** argv) {
    g_mutate = argc > 1 && !std::strcmp(argv[1], "mutate");
    g_mode = argc > 2 ? std::atoi(argv[2]) : (argc > 1 && argv[1][0] >= '1' && argv[1][0] <= '2' ? std::atoi(argv[1]) : 1);
    Stat st, stGame, stPert, stOrtho;
    int bad = 0;
    double worst = 0;
    const int kinds[] = {2, 2, 2, 4, 5, 6, 7};
    for (int iter = 0; iter < 6000; ++iter) {
        Case c;
        const bool ortho = iter % 5 == 4;
        const int mode = iter % 5; // 0..2 game-like, 3 perturbed, 4 orthographic
        double n = std::exp(U(std::log(0.05), std::log(5))), f = n * std::exp(U(std::log(20), std::log(5000)));
        double zn = 0, zf = 1;
        if (mode == 3 && (iter & 1)) { zn = U(0, 0.3); zf = U(0.5, 1); } // another depth range than the 0..1 the fog assumes
        if (iter % 29 == 0) { zn = 0; zf = 1.0; }
        if (ortho) { n = U(0.1, 10); f = n + U(10, 5000); }
        SetProj(c, n, f, ortho, zn, zf);
        double start = U(n, f * 0.5), end = start + U(0.01, 1) * (f - start);
        if (iter % 7 == 0) { start = n * 0.5; end = f * 1.5; }                       // a fog that starts before the near plane / ends past the far one
        const int fsel = kinds[iter % 7];
        double fogN = n, fogF = f;
        if (mode == 3) { fogN = n * U(0.5, 2.0); fogF = f * U(0.5, 2.0); if (fogF <= fogN) fogF = fogN * 2; } // the game's fog near/far not the projection's
        SetFog(c, start, end, fogN, fogF, ortho, fsel, mode == 2);
        Stat one;
        double w = 0;
        if (!Run(c, one, &w, bad < 8)) ++bad;
        Stat& dst = ortho ? stOrtho : (mode == 3 ? stPert : stGame);
        dst.px += one.px; dst.onCases += one.onCases; dst.offCases += one.offCases; dst.flips += one.flips; dst.edge += one.edge;
        dst.maxD = std::fmax(dst.maxD, one.maxD); dst.maxF = std::fmax(dst.maxF, one.maxF); dst.maxOldF = std::fmax(dst.maxOldF, one.maxOldF);
        if (w > worst) worst = w;
    }
    auto line = [](const char* n, const Stat& s) {
        std::printf("%-22s cases linear %5ld, kept old %5ld; %7ld pixels (%ld with 0<fog<1); max |old-new| double %.3g, float %.3g (the old form's own float error: %.3g); 8-bit mix steps that differ: %ld\n",
                    n, s.onCases, s.offCases, s.px, s.edge, s.maxD, s.maxF, s.maxOldF, s.flips);
    };
    line("game-like perspective", stGame);
    line("perturbed (b off)", stPert);
    line("orthographic", stOrtho);
    std::printf("largest ramp error taken: %.3g (limit %.3g)\n", worst, g_mode >= 2 ? kTolBound : kTolExact);
    // refusals that must happen
    int refusals = 0;
    {
        Case c; SetProj(c, 0.25, 130, false, 0, 1); SetFog(c, 10, 100, 0.25, 130, false, 2); // the game's defaults (glxFog.cpp) with a ramp
        if (LinearOf(c.fogBits, c.xf, c.bp, g_mode).on != (g_mode >= 2)) { std::fprintf(stderr, "FAIL: libogc's b (8388638) must be refused by mode 1 and taken by mode 2\n"); ++bad; }
        Case x; SetProj(x, 3.0, 40, false, 0, 1); SetFog(x, 5, 20, 3.0, 40, false, 2, true); // a short ramp near the camera and b scaled by 2^23: within float rounding
        if (!LinearOf(x.fogBits, x.xf, x.bp, g_mode).on) { std::fprintf(stderr, "FAIL: an exact b is refused\n"); ++bad; }
        Case o = c; o.fogBits |= 8; // fog says orthographic, the projection is perspective
        if (LinearOf(o.fogBits, o.xf, o.bp, g_mode).on) { std::fprintf(stderr, "FAIL: fog and projection kinds differ but the linear form is taken\n"); ++bad; } else ++refusals;
        Case v; SetProj(v, 0.1, 1000, false, 0.0, 0.5); SetFog(v, 5, 500, 0.1, 1000, false, 2); // viewport depth 0..0.5, fog made for 0..1
        if (LinearOf(v.fogBits, v.xf, v.bp, g_mode).on) { std::fprintf(stderr, "FAIL: a half-range viewport takes the linear form\n"); ++bad; } else ++refusals;
        Case r; SetProj(r, 0.1, 1000, false, 1.0, 0.0); SetFog(r, 5, 500, 0.1, 1000, false, 2); // reversed range
        if (LinearOf(r.fogBits, r.xf, r.bp, g_mode).on) { std::fprintf(stderr, "FAIL: a reversed viewport takes the linear form\n"); ++bad; } else ++refusals;
    }
    std::printf("kinds / depth ranges that must keep the old form: %d of 3 refused\n", refusals);
    if (bad) { std::fprintf(stderr, "fogvtx: %d FAILURES\n", bad); return 1; }
    std::printf("fogvtx: PASS\n");
    return 0;
}
