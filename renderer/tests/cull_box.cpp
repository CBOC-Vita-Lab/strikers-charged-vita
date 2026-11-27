// MSC_VGX_CULL: CullBoxOutsideScissor never culls a box that has a point on the screen inside the scissor (random cameras, boxes, viewports, scissors,
// perspective and orthographic projections), and does cull most boxes that are plainly elsewhere.
#include "../src/vgx_cull.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>

static std::mt19937 rng(12345);
static float U(float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); }

int main() {
    long wrong = 0, culled = 0, kept = 0, visible = 0, cases = 0;
    for (int it = 0; it < 200000; ++it) {
        // position matrix: random rotation (Rodrigues) and translation
        float ax[3] = {U(-1, 1), U(-1, 1), U(-1, 1)};
        const float an = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]) + 1e-6f;
        for (float& a : ax) a /= an;
        const float th = U(0, 6.2831853f), c = std::cos(th), s = std::sin(th);
        float m[12] = {c + ax[0] * ax[0] * (1 - c), ax[0] * ax[1] * (1 - c) - ax[2] * s, ax[0] * ax[2] * (1 - c) + ax[1] * s, U(-30, 30),
                       ax[1] * ax[0] * (1 - c) + ax[2] * s, c + ax[1] * ax[1] * (1 - c), ax[1] * ax[2] * (1 - c) - ax[0] * s, U(-30, 30),
                       ax[2] * ax[0] * (1 - c) - ax[1] * s, ax[2] * ax[1] * (1 - c) + ax[0] * s, c + ax[2] * ax[2] * (1 - c), U(-30, 30)};
        const bool ortho = (rng() & 3) == 0;
        float pr[6];
        if (!ortho) { pr[0] = U(0.5f, 3.0f); pr[1] = U(-0.3f, 0.3f); pr[2] = U(0.5f, 3.0f); pr[3] = U(-0.3f, 0.3f); }
        else { pr[0] = U(0.01f, 0.2f); pr[1] = U(-0.5f, 0.5f); pr[2] = U(0.01f, 0.2f); pr[3] = U(-0.5f, 0.5f); }
        pr[4] = U(-1, 0); pr[5] = U(-1, 0);
        const float vpW = U(200, 1280), vpH = U(150, 960), vpX = U(-100, 300), vpY = U(-100, 300);
        const float scX = U(-50, 300), scY = U(-50, 300), scW = U(20, 700), scH = U(20, 500);
        float box[6];
        const float cx = U(-40, 40), cy = U(-40, 40), cz = U(-40, 40);
        const float ex = U(0.05f, 20), ey = U(0.05f, 20), ez = U(0.05f, 20);
        box[0] = cx - ex; box[3] = cx + ex; box[1] = cy - ey; box[4] = cy + ey; box[2] = cz - ez; box[5] = cz + ez;
        const bool cull = vgx::CullBoxOutsideScissor(box, m, pr, ortho, vpX, vpY, vpW, vpH, scX, scY, scW, scH, 4.0f);
        // ground truth by sampling the box's points (and its corners)
        bool vis = false;
        for (int k = 0; k < 400 && !vis; ++k) {
            const float x = k < 8 ? box[(k & 1) ? 3 : 0] : U(box[0], box[3]);
            const float y = k < 8 ? box[(k & 2) ? 4 : 1] : U(box[1], box[4]);
            const float z = k < 8 ? box[(k & 4) ? 5 : 2] : U(box[2], box[5]);
            const float xe = m[0] * x + m[1] * y + m[2] * z + m[3], ye = m[4] * x + m[5] * y + m[6] * z + m[7], ze = m[8] * x + m[9] * y + m[10] * z + m[11];
            float xc, yc, w;
            if (!ortho) { xc = pr[0] * xe + pr[1] * ze; yc = pr[2] * ye + pr[3] * ze; w = -ze; }
            else { xc = pr[0] * xe + pr[1]; yc = pr[2] * ye + pr[3]; w = 1.0f; }
            if (!(w > 1e-5f)) continue;
            const float px = vpX + vpW * 0.5f + vpW * 0.5f * xc / w, py = vpY + vpH * 0.5f - vpH * 0.5f * yc / w;
            if (px >= scX && px <= scX + scW && py >= scY && py <= scY + scH) vis = true;
        }
        ++cases;
        if (vis) ++visible;
        if (cull && vis) ++wrong;
        if (cull) ++culled; else ++kept;
    }
    std::printf("cull box test: %ld cases, %ld with a visible point, %ld culled, %ld kept, %ld WRONG\n", cases, visible, culled, kept, wrong);
    // several matrices (a palette): outside only when outside under every one
    long wrong2 = 0, culled2 = 0, cases2 = 0;
    for (int it = 0; it < 60000; ++it) {
        const int nm = 1 + static_cast<int>(rng() % 8);
        float mm[8][12];
        for (int k = 0; k < nm; ++k)
            for (int e = 0; e < 12; ++e) mm[k][e] = (e % 5 == 0 ? 1.0f : 0.0f) + U(-0.3f, 0.3f) + ((e % 4 == 3) ? U(-25, 25) : 0.0f);
        float pr[6] = {U(0.5f, 3.0f), 0, U(0.5f, 3.0f), 0, U(-1, 0), U(-1, 0)};
        const float vpW = 640, vpH = 448, vpX = 0, vpY = 0, scX = U(0, 300), scY = U(0, 200), scW = U(50, 340), scH = U(50, 248);
        float box[6];
        const float cx = U(-60, 60), cy = U(-60, 60), cz = U(-60, 0);
        const float ex = U(0.05f, 8), ey = U(0.05f, 8), ez = U(0.05f, 8);
        box[0] = cx - ex; box[3] = cx + ex; box[1] = cy - ey; box[4] = cy + ey; box[2] = cz - ez; box[5] = cz + ez;
        vgx::CullSides sd;
        bool inside = false;
        for (int k = 0; k < nm && !inside; ++k)
            inside = !vgx::CullBoxSides(sd, box, mm[k], pr, false, vpX, vpY, vpW, vpH, scX, scY, scW, scH, 4.0f);
        const bool cull = !inside && sd.Any();
        bool vis = false;
        for (int k = 0; k < 600 && !vis; ++k) {
            const float* m = mm[rng() % nm];
            const float x = U(box[0], box[3]), y = U(box[1], box[4]), z = U(box[2], box[5]);
            const float xe = m[0] * x + m[1] * y + m[2] * z + m[3], ye = m[4] * x + m[5] * y + m[6] * z + m[7], ze = m[8] * x + m[9] * y + m[10] * z + m[11];
            const float xc = pr[0] * xe, yc = pr[2] * ye, w = -ze;
            if (!(w > 1e-5f)) continue;
            const float px = vpX + vpW * 0.5f + vpW * 0.5f * xc / w, py = vpY + vpH * 0.5f - vpH * 0.5f * yc / w;
            if (px >= scX && px <= scX + scW && py >= scY && py <= scY + scH) vis = true;
        }
        ++cases2;
        if (cull) ++culled2;
        if (cull && vis) ++wrong2;
    }
    std::printf("cull box test (palettes): %ld cases, %ld culled, %ld WRONG\n", cases2, culled2, wrong2);
    if (wrong2) return 1;
    // NaN keeps
    const float nanBox[6] = {NAN, 0, 0, 1, 1, 1};
    const float id[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    const float pr[6] = {1, 0, 1, 0, -1, -1};
    if (vgx::CullBoxOutsideScissor(nanBox, id, pr, false, 0, 0, 640, 448, 0, 0, 640, 448, 4.0f)) { std::puts("NaN box culled"); return 1; }
    return wrong || culled < cases / 10 ? 1 : 0;
}
