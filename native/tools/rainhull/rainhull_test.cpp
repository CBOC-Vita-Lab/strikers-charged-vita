// Host test of np/vgx_rainhull.h: the rectangles and quads must cover every point where a repeating, bilinearly filtered texture can be nonzero (an invented streak texture:
// no game data). RAINHULL_BREAK=1 shrinks every rectangle by one grid cell and must make the test fail (a live mutant).
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include "np/vgx_rainhull.h"

static int fails;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

// RGB5A3 level, 4x4 tiles of 16 bit big-endian: alpha 7 (opaque-ish) texels where mask says so
static void MakeLevel(int w, int h, const unsigned char* a, unsigned char* out)
{
    for (int ty = 0; ty < h; ty += 4) for (int tx = 0; tx < w; tx += 4) for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x, out += 2) {
        const int on = a[(ty + y) * w + tx + x];
        const unsigned v = on ? (0x7000u | 0xFFFu) : 0u;   // 3-bit alpha 7, rgb 4:4:4 white
        out[0] = (unsigned char)(v >> 8); out[1] = (unsigned char)v;
    }
}

static double Bilinear(const unsigned char* a, int w, int h, double s, double t)   // sample position in texels, repeat; returns the largest weight*alpha (0 or positive)
{
    const double fs = s - 0.5, ft = t - 0.5;
    const int i0 = (int)std::floor(fs), j0 = (int)std::floor(ft);
    const double wx = fs - i0, wy = ft - j0;
    double r = 0;
    for (int dj = 0; dj < 2; ++dj) for (int di = 0; di < 2; ++di) {
        const int i = (((i0 + di) % w) + w) % w, j = (((j0 + dj) % h) + h) % h;
        r += a[j * w + i] * (di ? wx : 1 - wx) * (dj ? wy : 1 - wy);
    }
    return r;
}

int main()
{
    const bool brk = std::getenv("RAINHULL_BREAK") != 0;
    // 1. one texel: its support is (i - 0.5 .. i + 1.5) texels = half texels 2i - 1 .. 2i + 3
    {
        unsigned char a[32 * 32] = {0};
        a[7 * 32 + 5] = 1;
        unsigned char tex[2048];
        MakeLevel(32, 32, a, tex);
        static uint8_t g0[4096], g1[4096], g2[4096];
        uint8_t* const grids[3] = {g0, g1, g2};
        CHECK(NpRainHullGrid(kNpTightFmtRGB5A3, 32, 32, 1, tex, grids));
        int n = 0, x0 = 99, x1 = -1, y0 = 99, y1 = -1;
        for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) if (g0[y * 64 + x]) { ++n; if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
        CHECK(n == 16 && x0 == 9 && x1 == 12 && y0 == 13 && y1 == 16);
        int n1 = 0; for (int i = 0; i < 4096; ++i) n1 += g1[i];
        CHECK(n1 > n);   // a coarser class covers at least as much, and here more
        for (int i = 0; i < 4096; ++i) CHECK(!g0[i] || g1[i]);
    }
    // 2. a texel in the corner wraps to the far edges
    {
        unsigned char a[32 * 32] = {0};
        a[0] = 1;
        unsigned char tex[2048];
        MakeLevel(32, 32, a, tex);
        static uint8_t g0[4096], g1[4096], g2[4096];
        uint8_t* const grids[3] = {g0, g1, g2};
        CHECK(NpRainHullGrid(kNpTightFmtRGB5A3, 32, 32, 1, tex, grids));
        CHECK(g0[63 * 64 + 63] && g0[0] && g0[2 * 64 + 2] && !g0[3 * 64 + 3] && !g0[62 * 64 + 62]);
        NpRainTile t;
        CHECK(NpRainRects(g0, 64, 64, 8, &t));
        CHECK(t.n == 4);   // the repeat is not joined across the tile edge: four corners of the tile
    }
    // 3. four dashes (invented): rectangles, the quad of the model (u -0.575..1.614, v 0..1), coverage by brute force
    {
        unsigned char a[32 * 32] = {0};
        for (int y = 3; y < 14; ++y) a[y * 32 + 2] = 1;
        for (int y = 12; y < 24; ++y) a[y * 32 + 6] = 1;
        for (int y = 0; y < 22; ++y) a[y * 32 + 16] = 1;
        for (int y = 14; y < 31; ++y) a[y * 32 + 26] = 1;
        unsigned char tex[2048];
        MakeLevel(32, 32, a, tex);
        static uint8_t g0[4096], g1[4096], g2[4096];
        uint8_t* const grids[3] = {g0, g1, g2};
        CHECK(NpRainHullGrid(kNpTightFmtRGB5A3, 32, 32, 1, tex, grids));
        NpRainTile t;
        CHECK(NpRainRects(g0, 64, 64, 8, &t));
        CHECK(t.n >= 4 && t.n <= 8);
        CHECK(t.area < 1024L * 1024L / 4);   // well under a quarter of a tile
        NpRainQuad q = {{{-0.375f, 0.0f, 0.375f}, {-0.375f, 0.0f, -0.375f}, {0.375f, 0.0f, 0.375f}, {0.375f, 0.0f, -0.375f}}, {-2357, -2357, 6612, 6612}, {0, 4096, 0, 4096}};
        NpRainFrame f;
        CHECK(NpRainFrameOf(q, &f));
        static float pos[256][4][3];
        static int uv[256][4][2];
        bool over = false;
        if (brk) for (int i = 0; i < t.n; ++i) { t.r[i].x1 -= 20; }   // the mutant: the right edge of each rectangle pulled in by 20 lattice units (just over a texel)
        const int nq = NpRainQuads(f, t, 256, pos, uv, &over);
        CHECK(nq > 0 && !over);
        // the position of a corner is the affine one: the u = ulo, v = vlo corner is the model's first vertex
        CHECK(std::fabs(pos[0][0][0] - (-0.375f + 0.75f * (uv[0][0][0] - -2357) / (6612 + 2357))) < 1e-5f);
        // coverage: every sample position of the quad where the filtered alpha is nonzero lies inside a quad (u, v in 1/4096 tile)
        int missed = 0, tested = 0;
        for (int iu = 0; iu < 1400; ++iu) for (int iv = 0; iv < 640; ++iv) {
            const double u = -2357.0 + (6612.0 + 2357.0) * (iu + 0.5) / 1400.0, v = 4096.0 * (iv + 0.5) / 640.0;
            const double s = u / 4096.0 * 32.0, tt = v / 4096.0 * 32.0;
            if (Bilinear(a, 32, 32, s, tt) <= 0) continue;
            ++tested;
            bool in = false;
            for (int i = 0; i < nq && !in; ++i) in = u >= uv[i][0][0] && u <= uv[i][2][0] && v >= uv[i][0][1] && v <= uv[i][2][1];
            if (!in) ++missed;
        }
        CHECK(tested > 1000);
        CHECK(missed == 0);
        // the area of the quads against the quad: (sum of the rectangles' uv areas) is small
        double area = 0; for (int i = 0; i < nq; ++i) area += (double)(uv[i][2][0] - uv[i][0][0]) * (uv[i][2][1] - uv[i][0][1]);
        const double frac = area / ((6612.0 + 2357.0) * 4096.0);
        std::printf("dash texture: %d rectangles a tile, %d quads, %.1f %% of the quad\n", t.n, nq, 100.0 * frac);
        CHECK(frac < 0.4);
        // too many quads for the room: refused, not truncated
        CHECK(NpRainQuads(f, t, 2, pos, uv, &over) == 0 && over);
    }
    // 3b. rectangles never overlap (the effect blends additively: an overlap would add a texel twice). An L shape with a dot inside its bounding box: separate components whose boxes overlap.
    {
        unsigned char a[32 * 32] = {0};
        for (int y = 2; y < 21; ++y) a[y * 32 + 2] = 1;
        for (int x = 2; x < 21; ++x) a[20 * 32 + x] = 1;
        a[8 * 32 + 12] = 1;
        unsigned char tex[2048];
        MakeLevel(32, 32, a, tex);
        static uint8_t g0[4096], g1[4096], g2[4096];
        uint8_t* const grids[3] = {g0, g1, g2};
        CHECK(NpRainHullGrid(kNpTightFmtRGB5A3, 32, 32, 1, tex, grids));
        for (int mr = 1; mr <= 8; ++mr) {
            NpRainTile t;
            CHECK(NpRainRects(g0, 64, 64, mr, &t));
            NpRainTile ts;
            CHECK(NpRainRects(g0, 64, 64, mr, &ts, 1, true));
            for (int i = 0; i < ts.n; ++i) CHECK(ts.r[i].y0 == 0 && ts.r[i].y1 == 1024);
            for (int i = 0; i < ts.n; ++i) for (int j = i + 1; j < ts.n; ++j) CHECK(!(ts.r[i].x0 < ts.r[j].x1 && ts.r[j].x0 < ts.r[i].x1));
            for (int i = 0; i < t.n; ++i) for (int j = i + 1; j < t.n; ++j) {
                const NpRainRect& p = t.r[i], &q = t.r[j];
                const bool overlap = p.x0 < q.x1 && q.x0 < p.x1 && p.y0 < q.y1 && q.y0 < p.y1;
                if (overlap) std::printf("overlap at maxRects %d: %d and %d\n", mr, i, j);
                CHECK(!overlap);
            }
        }
    }
    // 4. a quad that is not an affine one tile frame is refused
    {
        NpRainQuad q = {{{0, 0, 0}, {0, 0, 1}, {1, 0, 0}, {1, 0.5f, 1}}, {0, 0, 4096, 4096}, {0, 4096, 0, 4096}};
        NpRainFrame f;
        CHECK(!NpRainFrameOf(q, &f));
    }
    std::printf(fails ? "FAILED %d\n" : "rainhull: ok\n", fails);
    return fails ? 1 : 0;
}
