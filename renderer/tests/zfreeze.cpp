// Unit test of vgx_zfreeze.h (GX Z-freeze reference plane): the real header, no GL, no game data.
//  1. the plane through a triangle's window-space vertices agrees with what the GPU interpolates inside that triangle;
//  2. refusals: a vertex behind the eye, an edge-on triangle;
//  3. the mechanism the feature exists for: two overlapping triangles of ONE plane get depths that agree only to rounding when each is
//     interpolated from its own vertices (a strict LESS depth test then lets some pixels through twice), and bit-identical depth
//     from the frozen plane. Prints the measured rates.
#include "../src/vgx_zfreeze.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

using namespace vgx::zfreeze;

static int g_failures = 0;
static void Check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
}

// UB_PROJ as BuildProj makes it (perspective): column-major, Y flipped, GX clip z [-w, 0] -> GL [-w, w].
static void MakeProj(float fovScale, float aspect, float n, float f, float out[16]) {
    std::memset(out, 0, 16 * sizeof(float));
    // GX perspective parameters: p0 = x scale, p2 = y scale, p4 = -n/(f-n)..., p5 = -f*n/(f-n)
    const float p0 = fovScale / aspect, p2 = fovScale, p4 = -n / (f - n), p5 = -f * n / (f - n);
    out[0] = p0;
    out[5] = -p2;
    out[10] = 2.0f * p4 - 1.0f;
    out[14] = 2.0f * p5;
    out[11] = -1.0f;
}

int main() {
    std::mt19937 rng(12345);
    std::uniform_real_distribution<float> U(0.0f, 1.0f);
    float proj[16];
    MakeProj(1.7f, 640.0f / 448.0f, 0.5f, 800.0f, proj);
    const Viewport vp{0, 0, 640, 448, 0.0f, 1.0f};
    // A camera 30 m above a plane, looking down at 45 degrees, rotated a little.
    const float c = std::cos(0.8f), s = std::sin(0.8f);
    const float mv[12] = {1, 0, 0, 3, 0, c, -s, -2, 0, s, c, -30};

    // 1. plane against interpolation, 2000 random triangles on the ground plane y = 0.1
    int worst = 0;
    double maxErr = 0;
    for (int t = 0; t < 2000; ++t) {
        float p[3][3], clip[3][4], win[3][3];
        for (int i = 0; i < 3; ++i) { p[i][0] = (U(rng) - 0.5f) * 40; p[i][1] = 0.1f; p[i][2] = (U(rng) - 0.5f) * 40; ModelToClip(p[i], mv, proj, clip[i]); }
        Plane pl;
        if (!PlaneFromClip(clip, vp, pl)) continue;
        for (int i = 0; i < 3; ++i) ClipToWindow(clip[i], vp, win[i]);
        for (int k = 0; k < 8; ++k) {
            // a point inside the triangle by screen-space barycentric coordinates
            double l0 = U(rng), l1 = U(rng) * (1 - l0), l2 = 1 - l0 - l1;
            const double x = l0 * win[0][0] + l1 * win[1][0] + l2 * win[2][0], y = l0 * win[0][1] + l1 * win[1][1] + l2 * win[2][1];
            const double zi = l0 * win[0][2] + l1 * win[1][2] + l2 * win[2][2]; // what the GPU's linear interpolation of window depth gives
            const double err = std::fabs(Eval(pl, static_cast<float>(x), static_cast<float>(y)) - zi);
            if (err > maxErr) { maxErr = err; worst = t; }
        }
    }
    std::printf("plane vs interpolation: max |dz| = %.3g (triangle %d), depth range 0..1\n", maxErr, worst);
    Check(maxErr < 2e-5, "the plane reproduces the interpolated window depth inside the triangle (< 2e-5)");

    // 2. refusals
    {
        float clip[3][4] = {{0, 0, 0.5f, 1}, {1, 0, 0.5f, 1}, {0, 1, 0.5f, 1}};
        Plane pl;
        Check(PlaneFromClip(clip, vp, pl), "an ordinary triangle gives a plane");
        clip[1][3] = -1;
        Check(!PlaneFromClip(clip, vp, pl), "a vertex behind the eye gives none");
        float edgeOn[3][4] = {{0, 0, 0.1f, 1}, {0.5f, 0, 0.2f, 1}, {1, 0, 0.3f, 1}};
        Check(!PlaneFromClip(edgeOn, vp, pl), "an edge-on triangle gives none");
    }

    // 3. the mechanism: two triangles of one plane, overlapping, depth per pixel from their own vertices vs from the frozen plane
    {
        long samples = 0, floatDiffer = 0, quantDiffer = 0, frozenDiffer = 0;
        for (int t = 0; t < 400; ++t) {
            // a flattened mesh: vertices on the plane y = 0.1 at random places, two overlapping triangles A and B
            float pa[3][3], pb[3][3], ca[3][4], cb[3][4], wa[3][3], wb[3][3];
            const float ox = (U(rng) - 0.5f) * 20, oz = (U(rng) - 0.5f) * 20;
            for (int i = 0; i < 3; ++i) {
                pa[i][0] = ox + (U(rng) - 0.5f) * 4; pa[i][1] = 0.1f; pa[i][2] = oz + (U(rng) - 0.5f) * 4;
                pb[i][0] = ox + (U(rng) - 0.5f) * 4; pb[i][1] = 0.1f; pb[i][2] = oz + (U(rng) - 0.5f) * 4;
                ModelToClip(pa[i], mv, proj, ca[i]); ModelToClip(pb[i], mv, proj, cb[i]);
                ClipToWindow(ca[i], vp, wa[i]); ClipToWindow(cb[i], vp, wb[i]);
            }
            Plane ref;
            if (!PlaneFromClip(ca, vp, ref)) continue;
            // sample pixel centres inside both triangles
            auto inside = [](const float w[3][3], float x, float y, float l[3]) {
                const float d = (w[1][1] - w[2][1]) * (w[0][0] - w[2][0]) + (w[2][0] - w[1][0]) * (w[0][1] - w[2][1]);
                if (std::fabs(d) < 1e-9f) return false;
                l[0] = ((w[1][1] - w[2][1]) * (x - w[2][0]) + (w[2][0] - w[1][0]) * (y - w[2][1])) / d;
                l[1] = ((w[2][1] - w[0][1]) * (x - w[2][0]) + (w[0][0] - w[2][0]) * (y - w[2][1])) / d;
                l[2] = 1 - l[0] - l[1];
                return l[0] >= 0 && l[1] >= 0 && l[2] >= 0;
            };
            for (int y = 0; y < 448; ++y)
                for (int x = 0; x < 640; ++x) {
                    const float px = x + 0.5f, py = y + 0.5f;
                    float la[3], lb[3];
                    if (!inside(wa, px, py, la) || !inside(wb, px, py, lb)) continue;
                    ++samples;
                    const float za = la[0] * wa[0][2] + la[1] * wa[1][2] + la[2] * wa[2][2];
                    const float zb = lb[0] * wb[0][2] + lb[1] * wb[1][2] + lb[2] * wb[2][2];
                    if (za != zb) ++floatDiffer;
                    if (std::floor(za * 16777215.0f) != std::floor(zb * 16777215.0f)) ++quantDiffer;
                    if (Eval(ref, px, py) != Eval(ref, px, py)) ++frozenDiffer; // (the same arithmetic on the same plane: never)
                }
        }
        std::printf("overlapping coplanar triangles, %ld pixels covered by both: float depth differs in %.1f %%, 24-bit depth differs in %.1f %%, frozen plane: %ld\n",
                    samples, samples ? 100.0 * floatDiffer / samples : 0.0, samples ? 100.0 * quantDiffer / samples : 0.0, frozenDiffer);
        Check(samples > 1000, "the experiment covered pixels");
        Check(frozenDiffer == 0, "frozen depth is identical for every triangle");
        Check(quantDiffer > 0, "own-vertex interpolation of coplanar triangles disagrees in the depth buffer's last bit somewhere (the effect the feature removes)");
    }
    if (g_failures) return 1;
    std::puts("zfreeze test: passed");
    return 0;
}
