// np/fxtight.h: tight particle sprites (MSC_FX_TIGHT, Vita port). Every texel of a sprite costs fill on a tile GPU, the fully transparent ones too,
// because a blended fragment cannot be rejected before it is shaded. The first time a particle texture is drawn, this works out for each of its
// atlas frames a convex polygon (at most 8 corners) outside which no fragment can change anything, and the sprite is drawn as that polygon.
//
// Why the pictures stay the same. The texture coordinates are an affine map of the sprite's plane, so a polygon with the quad's map gives each
// pixel inside it the same coordinates as the quad does. Corners lie on the writer's own lattice (1/1024 of the texture, s16), so the map is the
// quad's exactly, not a rounding of it. Blend and alpha test are source alpha (ParticleSystem::RenderAllParticles), the combiner multiplies vertex
// and texture alpha (GXVertexColourTextureMaterialProgram, GXShadowedDiffuseMaterialProgram): a sample of alpha 0 changes nothing.
//
// What "alpha 0 everywhere outside" takes. The sampler filters: between four texel centres a sample is zero only if all four are, and that square
// is what the polygon must not cut (NpTightCover). The sampler also reads mip levels, trilinear, and on the Vita the renderer does not upload the
// disc's levels: vitaGL generates them from level 0. So a level is the disc's level OR a box reduction of level 0 (NpTightOrDown): a texel
// that could be nonzero in either counts. Low levels smear: all levels together leave almost nothing to trim (0.98 of the quad over the disc's
// 136 effect textures). So there are polygons by the deepest level a sprite can reach (class 0, 1, 2), and a sprite is given its class from a
// lower bound of its size on the screen (NpTightClass); deeper than class 2 it stays a quad. Fill is in large sprites, which read level 0.
//
// Pure: no game headers, for the host test (native/tools/fxtight). Texels are as on the disc: tiled, big-endian.
#ifndef NP_FXTIGHT_H
#define NP_FXTIGHT_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { kNpTightMaxVerts = 8, kNpTightClasses = 3, kNpTightMaxSide = 256, kNpTightLattice = 1024 };
enum { kNpTightFmtRGB5A3 = 1, kNpTightFmtCMPR = 2 };   // eGXTextureFormat: the only two formats of the disc's effect textures; others stay quads

struct NpTightPoly {
    uint8_t n;                                 // corners; 0: nothing in this frame can show; kNpTightQuad: draw the quad (nothing to gain)
    uint8_t peak;                              // (texture) largest alpha x brightness / 255 of any level: see NpTightInvisible
    uint8_t peakA;                             // (texture) largest alpha of any level
    uint8_t pad;
    int16_t uv[kNpTightMaxVerts][2];           // lattice coordinates, positive area in (u, v)
};
enum { kNpTightQuad = 255 };

struct NpTightRect { int u0, v0, u1, v1; };    // a frame in lattice units: the writer's (short)(u * 1024), so the quad's own corners

// ---- texels
static inline void NpTightRgb5a3(uint16_t v, uint8_t* a, uint8_t* c)
{
    if (v & 0x8000) {
        const int r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
        const int m = r > g ? (r > b ? r : b) : (g > b ? g : b);
        *a = 255;
        *c = (uint8_t)((m << 3) | (m >> 2));
    } else {
        const int r = (v >> 8) & 15, g = (v >> 4) & 15, b = v & 15;
        const int m = r > g ? (r > b ? r : b) : (g > b ? g : b);
        const int a3 = (v >> 12) & 7;
        *a = (uint8_t)((a3 << 5) | (a3 << 2) | (a3 >> 1));
        *c = (uint8_t)((m << 4) | m);
    }
}

static inline int NpTightLevelBytes(int fmt, int w, int h)
{
    if (fmt == kNpTightFmtCMPR) return ((w + 7) / 8) * ((h + 7) / 8) * 32;
    return ((w + 3) / 4) * ((h + 3) / 4) * 32;
}

// One level to two bytes a texel (alpha; largest of r, g, b): out[(y * w + x) * 2 + 0, 1]. Same reading as renderer/src/vgx_texdecode.cpp.
static inline void NpTightDecode(int fmt, int w, int h, const uint8_t* d, uint8_t* out)
{
    if (fmt == kNpTightFmtRGB5A3) {
        for (int ty = 0; ty < h; ty += 4)
            for (int tx = 0; tx < w; tx += 4)
                for (int y = 0; y < 4; ++y)
                    for (int x = 0; x < 4; ++x, d += 2)
                        if (tx + x < w && ty + y < h) {
                            uint8_t* o = out + ((ty + y) * w + tx + x) * 2;
                            NpTightRgb5a3((uint16_t)((d[0] << 8) | d[1]), &o[0], &o[1]);
                        }
        return;
    }
    for (int ty = 0; ty < h; ty += 8)
        for (int tx = 0; tx < w; tx += 8)
            for (int sy = 0; sy < 8; sy += 4)
                for (int sx = 0; sx < 8; sx += 4, d += 8) {
                    const unsigned c0 = (unsigned)(d[0] << 8) | d[1], c1 = (unsigned)(d[2] << 8) | d[3];
                    const bool punch = c0 <= c1;
                    const int m0 = ((c0 >> 11) & 31) * 8 > ((c0 >> 5) & 63) * 4 ? ((c0 >> 11) & 31) * 8 : ((c0 >> 5) & 63) * 4;
                    const int m0b = (int)(c0 & 31) * 8 > m0 ? (int)(c0 & 31) * 8 : m0;
                    const int m1 = ((c1 >> 11) & 31) * 8 > ((c1 >> 5) & 63) * 4 ? ((c1 >> 11) & 31) * 8 : ((c1 >> 5) & 63) * 4;
                    const int m1b = (int)(c1 & 31) * 8 > m1 ? (int)(c1 & 31) * 8 : m1;
                    const int hi = (m0b > m1b ? m0b : m1b) + 7;   // (the brightest any index gives, each channel's own largest: an upper bound, zero only for black)
                    for (int y = 0; y < 4; ++y)
                        for (int x = 0; x < 4; ++x) {
                            const int idx = (d[4 + y] >> (6 - x * 2)) & 3;
                            const int px = tx + sx + x, py = ty + sy + y;
                            if (px >= w || py >= h) continue;
                            uint8_t* o = out + (py * w + px) * 2;
                            const bool hole = punch && idx == 3;
                            o[0] = hole ? 0 : 255;
                            const bool black = hole || (idx == 0 && c0 == 0) || (idx == 1 && c1 == 0) || (c0 == 0 && c1 == 0);
                            o[1] = black ? 0 : (uint8_t)(hi > 255 ? 255 : hi);
                        }
                }
}

// ---- polygons (lattice integers; float only to place a corner, every result is checked in integers)
struct NpTightPt { int x, y; };

static inline long NpTightCross(const NpTightPt& o, const NpTightPt& a, const NpTightPt& b)
{
    return (long)(a.x - o.x) * (b.y - o.y) - (long)(a.y - o.y) * (b.x - o.x);
}

// Convex hull, positive area, of points sorted by x then y (Andrew). h has room for 2 * n. Returns the corners.
static inline int NpTightHullSorted(const NpTightPt* p, int n, NpTightPt* h)
{
    if (n < 3) { for (int i = 0; i < n; ++i) h[i] = p[i]; return n; }
    int k = 0;
    for (int i = 0; i < n; ++i) {
        while (k >= 2 && NpTightCross(h[k - 2], h[k - 1], p[i]) <= 0) --k;
        h[k++] = p[i];
    }
    for (int i = n - 2, t = k + 1; i >= 0; --i) {
        while (k >= t && NpTightCross(h[k - 2], h[k - 1], p[i]) <= 0) --k;
        h[k++] = p[i];
    }
    return k - 1;
}

static inline void NpTightSort(NpTightPt* p, int n)
{
    for (int gap = n / 2; gap > 0; gap /= 2)   // (shell: a few thousand points at most, no recursion, no allocation)
        for (int i = gap; i < n; ++i) {
            const NpTightPt t = p[i];
            int j = i;
            while (j >= gap && (p[j - gap].x > t.x || (p[j - gap].x == t.x && p[j - gap].y > t.y))) { p[j] = p[j - gap]; j -= gap; }
            p[j] = t;
        }
}

static inline long NpTightArea2(const NpTightPt* p, int n)
{
    long a = 0;
    for (int i = 0; i < n; ++i) {
        const NpTightPt& u = p[i];
        const NpTightPt& v = p[i + 1 == n ? 0 : i + 1];
        a += (long)u.x * v.y - (long)v.x * u.y;
    }
    return a;
}

// Is everything in `in` inside the convex polygon `p`, and is p convex with positive area? The proof of a result, in integers.
static inline bool NpTightContains(const NpTightPt* p, int n, const NpTightPt* in, int m)
{
    if (n < 3) return false;
    for (int i = 0; i < n; ++i) {
        const NpTightPt& a = p[i];
        const NpTightPt& b = p[i + 1 == n ? 0 : i + 1];
        if (NpTightCross(a, b, p[i + 2 >= n ? i + 2 - n : i + 2]) < 0) return false;
        for (int j = 0; j < m; ++j)
            if (NpTightCross(a, b, in[j]) < 0) return false;
    }
    return NpTightArea2(p, n) > 0;
}

struct NpTightPtF { double x, y; };

// Removes edges of a convex polygon, each time the one whose removal (extending its two neighbours to where they meet) adds the least area, down to k corners (Aggarwal's idea,
// as in Persson's particle trimmer, greedy). false: no edge can go.
static inline bool NpTightShrink(NpTightPtF* p, int* n, int k)
{
    while (*n > k) {
        int bestI = -1;
        double bestA = 0, bx = 0, by = 0;
        const int c = *n;
        for (int i = 0; i < c; ++i) {
            const NpTightPtF& a = p[(i + c - 1) % c];
            const NpTightPtF& b = p[i];
            const NpTightPtF& d = p[(i + 1) % c];
            const NpTightPtF& e = p[(i + 2) % c];
            const double dx1 = b.x - a.x, dy1 = b.y - a.y, dx2 = d.x - e.x, dy2 = d.y - e.y;
            const double den = dx1 * dy2 - dy1 * dx2;
            if (den > -1e-9 && den < 1e-9) continue;
            const double t = ((d.x - a.x) * dy2 - (d.y - a.y) * dx2) / den;
            const double qx = a.x + t * dx1, qy = a.y + t * dy1;
            if ((qx - b.x) * dx1 + (qy - b.y) * dy1 < 0) continue;
            if ((qx - d.x) * dx2 + (qy - d.y) * dy2 < 0) continue;
            const double add = (qx - b.x) * (d.y - b.y) - (qy - b.y) * (d.x - b.x);
            const double aa = add < 0 ? -add : add;
            if (bestI < 0 || aa < bestA) { bestI = i; bestA = aa; bx = qx; by = qy; }
        }
        if (bestI < 0) return false;
        // edge (bestI, bestI + 1) goes: its two ends become one point
        p[bestI].x = bx;
        p[bestI].y = by;
        const int gone = (bestI + 1) % c;
        for (int i = gone; i + 1 < c; ++i) p[i] = p[i + 1];
        *n = c - 1;
    }
    return true;
}

// Sutherland-Hodgman against the frame. o has room for n + 4.
static inline int NpTightClip(const NpTightPtF* p, int n, const NpTightRect& r, NpTightPtF* o)
{
    NpTightPtF a[kNpTightMaxVerts + 6], b[kNpTightMaxVerts + 6];
    int c = n;
    for (int i = 0; i < n; ++i) a[i] = p[i];
    for (int e = 0; e < 4; ++e) {
        const double lim = e == 0 ? r.u0 : e == 1 ? r.u1 : e == 2 ? r.v0 : r.v1;
        const double s = (e & 1) ? -1.0 : 1.0;
        int m = 0;
        for (int i = 0; i < c; ++i) {
            const NpTightPtF& u = a[i];
            const NpTightPtF& v = a[i + 1 == c ? 0 : i + 1];
            const double pu = ((e < 2 ? u.x : u.y) - lim) * s, pv = ((e < 2 ? v.x : v.y) - lim) * s;
            if (pu >= 0) b[m++] = u;
            if ((pu >= 0) != (pv >= 0)) {
                const double t = pu / (pu - pv);
                b[m].x = u.x + t * (v.x - u.x);
                b[m].y = u.y + t * (v.y - u.y);
                ++m;
            }
        }
        c = m;
        for (int i = 0; i < c; ++i) a[i] = b[i];
    }
    for (int i = 0; i < c; ++i) o[i] = a[i];
    return c;
}

// Lattice polygon of at most k corners around hull h (positive area), inside the frame, proved to contain h. At the end there is always the bounding box.
static inline int NpTightFit(const NpTightPt* h, int m, int k, const NpTightRect& r, NpTightPt* out)
{
    int x0 = h[0].x, x1 = x0, y0 = h[0].y, y1 = y0;
    for (int i = 0; i < m; ++i) {
        if (h[i].x < x0) x0 = h[i].x;
        if (h[i].x > x1) x1 = h[i].x;
        if (h[i].y < y0) y0 = h[i].y;
        if (h[i].y > y1) y1 = h[i].y;
    }
    out[0].x = x0; out[0].y = y0; out[1].x = x1; out[1].y = y0; out[2].x = x1; out[2].y = y1; out[3].x = x0; out[3].y = y1;
    if (x1 == x0 || y1 == y0) return 4;   // (a line of texels: the box, degenerate; NpTightSolveFrame widens it first)
    int best = 4;
    long bestA = NpTightArea2(out, 4);
    if (m <= k && NpTightContains(h, m, h, m) && NpTightArea2(h, m) < bestA) {
        for (int i = 0; i < m; ++i) out[i] = h[i];
        return m;
    }
    enum { kWork = 96 };
    if (m > kWork) return best;
    // The hull is first widened by `d` lattice steps on every side, so that corners can then be rounded to the nearest point and still hold it (rounding moves one by less than a
    // step); the check below is what counts, and a wider d is tried if it fails.
    for (int d = 2; d <= 4; d += 2) {
        NpTightPt wide[kWork * 4], wh[kWork * 8];
        for (int i = 0; i < m; ++i)
            for (int s = 0; s < 4; ++s) {
                wide[i * 4 + s].x = h[i].x + ((s & 1) ? d : -d);
                wide[i * 4 + s].y = h[i].y + ((s & 2) ? d : -d);
            }
        NpTightSort(wide, m * 4);
        const int wn = NpTightHullSorted(wide, m * 4, wh);
        if (wn > kWork) break;
        bool found = false;
        for (int t = k; t >= 3; --t) {
            NpTightPtF f[kWork], c[kNpTightMaxVerts + 6];
            for (int i = 0; i < wn; ++i) { f[i].x = wh[i].x; f[i].y = wh[i].y; }
            int n = wn;
            if (!NpTightShrink(f, &n, t)) continue;
            const int cn = NpTightClip(f, n, r, c);
            if (cn < 3 || cn > k) continue;
            NpTightPt q[kNpTightMaxVerts + 6], conv[(kNpTightMaxVerts + 6) * 2];
            for (int i = 0; i < cn; ++i) {
                const int qx = (int)(c[i].x + 0.5), qy = (int)(c[i].y + 0.5);
                q[i].x = qx < r.u0 ? r.u0 : qx > r.u1 ? r.u1 : qx;
                q[i].y = qy < r.v0 ? r.v0 : qy > r.v1 ? r.v1 : qy;
            }
            // (rounding can leave a corner a hair inside its neighbours, clamping can leave two the same: the hull of the rounded corners is convex and holds them all)
            NpTightSort(q, cn);
            const int w = NpTightHullSorted(q, cn, conv);
            if (w > k || !NpTightContains(conv, w, h, m)) continue;
            const long a = NpTightArea2(conv, w);
            found = true;
            if (a < bestA) {
                bestA = a;
                best = w;
                for (int i = 0; i < w; ++i) out[i] = conv[i];
            }
        }
        if (found) break;
    }
    return best;
}

// ---- a texture
// The cover of a level: squares between texel centres. Square (i, j), 0 <= i <= w, of a w x h level has the corners (i - 0.5, j - 0.5) .. (i + 0.5, j + 0.5) in texels; a clamped
// sampler reads the edge texel beyond the edge. Its four texels decide: bit 0: some alpha is nonzero there; bit 1: some brightness is. Painted onto the same squares of level 0
// (all conservative: a level 0 square that touches the level's square), `cov` (W + 1) x (H + 1) collects the levels so far: alpha and brightness may come from different levels of
// a trilinear read, so `black` asks for both bits only after all levels are in.
static inline void NpTightCover(const uint8_t* lvl, int w, int h, int W, int H, uint8_t* cov)
{
    const int sx = W / w, sy = H / h;
    for (int j = 0; j <= h; ++j) {
        const int j0 = j > 0 ? j - 1 : 0, j1 = j < h ? j : h - 1;
        const int by0 = j * sy - sy / 2, by1 = j * sy + sy / 2;
        for (int i = 0; i <= w; ++i) {
            const int i0 = i > 0 ? i - 1 : 0, i1 = i < w ? i : w - 1;
            const uint8_t* a = lvl + j0 * w * 2, *b = lvl + j1 * w * 2;
            const int aa = a[i0 * 2] | a[i1 * 2] | b[i0 * 2] | b[i1 * 2];
            const int cc = a[i0 * 2 + 1] | a[i1 * 2 + 1] | b[i0 * 2 + 1] | b[i1 * 2 + 1];
            const int bits = (aa ? 1 : 0) | (cc ? 2 : 0);
            if (!bits) continue;
            const int bx0 = i * sx - sx / 2, bx1 = i * sx + sx / 2;
            for (int y = by0 < 0 ? 0 : by0; y <= by1 && y <= H; ++y) {
                uint8_t* row = cov + y * (W + 1);
                for (int x = bx0 < 0 ? 0 : bx0; x <= bx1 && x <= W; ++x) row[x] |= (uint8_t)bits;
            }
        }
    }
}

// 2 x 2 box reduction that keeps "nonzero": (w, h) -> (w / 2, h / 2) in place of nothing: dst has its own room.
static inline void NpTightOrDown(const uint8_t* src, int w, int h, uint8_t* dst, int dw, int dh)
{
    for (int y = 0; y < dh; ++y)
        for (int x = 0; x < dw; ++x) {
            const int x1 = (w > 1 ? x * 2 + 1 : 0), y1 = (h > 1 ? y * 2 + 1 : 0), x0 = w > 1 ? x * 2 : 0, y0 = h > 1 ? y * 2 : 0;
            for (int c = 0; c < 2; ++c) {
                const int m0 = src[(y0 * w + x0) * 2 + c], m1 = src[(y0 * w + x1) * 2 + c], m2 = src[(y1 * w + x0) * 2 + c], m3 = src[(y1 * w + x1) * 2 + c];
                int m = m0 > m1 ? m0 : m1;
                if (m2 > m) m = m2;
                if (m3 > m) m = m3;
                dst[(y * dw + x) * 2 + c] = (uint8_t)m;
            }
        }
}

// One frame, one class, from the cover. `pts` has room for 8 * (H + 1) points, `hull` for twice that.
static inline void NpTightSolveFrame(const uint8_t* cov, int W, int H, const NpTightRect& r, int k, bool black, NpTightPt* pts, NpTightPt* hull, NpTightPoly* o)
{
    const int want = black ? 3 : 1;
    const int sx = 512 / W, sy = 512 / H;   // square i is (2i - 1) * sx .. (2i + 1) * sx on the lattice (sides are powers of two up to 256), and one step of margin
    int n = 0;
    for (int y = 0; y <= H; ++y) {
        const int yLo = (2 * y - 1) * sy - 1, yHi = (2 * y + 1) * sy + 1;
        if (yHi < r.v0 || yLo > r.v1) continue;
        const uint8_t* row = cov + y * (W + 1);
        int a = -1, b = -1;
        for (int x = (r.u0 / sx - 2) / 2 < 0 ? 0 : (r.u0 / sx - 2) / 2; x <= W; ++x) {
            if ((2 * x - 1) * sx - 1 > r.u1) break;
            if ((row[x] & want) != want || (2 * x + 1) * sx + 1 < r.u0) continue;
            if (a < 0) a = x;
            b = x;
        }
        if (a < 0) continue;
        const int xLo = (2 * a - 1) * sx - 1, xHi = (2 * b + 1) * sx + 1;
        const int u0 = xLo < r.u0 ? r.u0 : xLo, u1 = xHi > r.u1 ? r.u1 : xHi, v0 = yLo < r.v0 ? r.v0 : yLo, v1 = yHi > r.v1 ? r.v1 : yHi;
        pts[n].x = u0; pts[n++].y = v0;
        pts[n].x = u1; pts[n++].y = v0;
        pts[n].x = u0; pts[n++].y = v1;
        pts[n].x = u1; pts[n++].y = v1;
    }
    if (n == 0) { o->n = 0; return; }
    NpTightSort(pts, n);
    const int m = NpTightHullSorted(pts, n, hull);
    NpTightPt p[kNpTightMaxVerts + 6];
    const int c = m >= 3 ? NpTightFit(hull, m, k, r, p) : 0;
    const long full = (long)(r.u1 - r.u0) * (r.v1 - r.v0) * 2, area = c >= 3 ? NpTightArea2(p, c) : full;
    if (c < 3 || area * 100 >= full * 97) { o->n = kNpTightQuad; return; }   // (3 % or less: the quad's own numbers, and its corners on the very same floats)
    o->n = (uint8_t)c;
    for (int i = 0; i < c; ++i) { o->uv[i][0] = (int16_t)p[i].x; o->uv[i][1] = (int16_t)p[i].y; }
}

// polys[frame * kNpTightClasses + class]. levels: from the texture; data: level 0, the other levels follow it. k: 4..8. black: additive blend, a black texel adds nothing either
// (alpha is still written: MSC_FX_TIGHT_BLACK). lose: above 0 the polygon may leave out texels of alpha x brightness / 255 under this (additive sprites, MSC_FX_TIGHT_FLOOR: not
// the same picture). false: not a texture for this (format, size, no memory).
static inline bool NpTightSolve(int fmt, int W, int H, int levels, const uint8_t* data, const NpTightRect* frames, int nFrames, int k, bool black, NpTightPoly* polys, int lose = 0)
{
    if ((fmt != kNpTightFmtRGB5A3 && fmt != kNpTightFmtCMPR) || W < 2 || H < 2 || W > kNpTightMaxSide || H > kNpTightMaxSide || (W & (W - 1)) || (H & (H - 1))) return false;
    const size_t cells = (size_t)W * H * 2, covN = (size_t)(W + 1) * (H + 1);
    uint8_t* mem = (uint8_t*)malloc(cells * 3 + covN + 8 + (size_t)(12 * (H + 1)) * sizeof(NpTightPt));
    if (!mem) return false;
    uint8_t* orp = mem, *lvl = mem + cells, *tmp = mem + cells * 2, *cov = mem + cells * 3;
    NpTightPt* pts = (NpTightPt*)(void*)(((uintptr_t)(cov + covN) + 7u) & ~(uintptr_t)7u);
    NpTightPt* hull = pts + 4 * (H + 1);   // (4 points a row; the hull's work area takes twice that)
    memset(cov, 0, covN);
    memset(orp, 0, cells);
    int w = W, h = H;
    const uint8_t* d = data;
    int peak = 0, peakA = 0;
    for (int f = 0; f < nFrames * kNpTightClasses; ++f) polys[f].n = kNpTightQuad;
    for (int f = 0; f < nFrames * kNpTightClasses; ++f) polys[f].peak = polys[f].peakA = 255;
    for (int L = 0; L < kNpTightClasses; ++L) {
        if (L < levels) {
            NpTightDecode(fmt, w, h, d, lvl);
            d += NpTightLevelBytes(fmt, w, h);
        } else {
            memset(lvl, 0, (size_t)w * h * 2);
        }
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const int a = lvl[(y * w + x) * 2], c = lvl[(y * w + x) * 2 + 1], ac = (a * c + 254) / 255;
                if (ac > peak) peak = ac;
                if (a > peakA) peakA = a;
                if (lose > 0 && a * c < lose * 255) lvl[(y * w + x) * 2] = 0;
            }
        if (L == 0) memcpy(orp, lvl, (size_t)w * h * 2);
        else for (size_t i = 0; i < (size_t)w * h * 2; ++i) if (orp[i] > lvl[i]) lvl[i] = orp[i];
        NpTightCover(lvl, w, h, W, H, cov);
        for (int f = 0; f < nFrames; ++f)
            NpTightSolveFrame(cov, W, H, frames[f], k, black, pts, hull, &polys[f * kNpTightClasses + L]);
        if (w == 1 && h == 1) break;
        const int dw = w > 1 ? w / 2 : 1, dh = h > 1 ? h / 2 : 1;
        NpTightOrDown(orp, w, h, tmp, dw, dh);
        memcpy(orp, tmp, (size_t)dw * dh * 2);
        w = dw;
        h = dh;
    }
    // Levels below the deepest class hold box averages (or the disc's own): none exceeds what the texture's top holds in a generated chain, but a disc level could. Read them.
    for (int L = kNpTightClasses; L < levels; ++L) {
        NpTightDecode(fmt, w, h, d, lvl);
        d += NpTightLevelBytes(fmt, w, h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const int a = lvl[(y * w + x) * 2], c = lvl[(y * w + x) * 2 + 1], ac = (a * c + 254) / 255;
                if (ac > peak) peak = ac;
                if (a > peakA) peakA = a;
            }
        if (w == 1 && h == 1) break;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    for (int f = 0; f < nFrames * kNpTightClasses; ++f) { polys[f].peak = (uint8_t)peak; polys[f].peakA = (uint8_t)peakA; }
    free(mem);
    return true;
}

// The deepest mip level a sprite can reach. edge2: squared length of its shorter side in world units; z: distance from the camera plane; k2: (pixels of the smallest target it can be drawn to,
// per world unit at z = 1) squared; tex2: texels across the frame, squared. LOD = log2(texels / pixels); half a level of margin: lod + 0.5 <= c. -1: deeper than the classes, a quad.
static inline int NpTightClass(float edge2, float z, float k2, float tex2)
{
    if (z <= 0.0f) return -1;   // (behind the camera plane or on it: no statement, a quad)
    const float px2 = edge2 * k2 / (z * z);
    float need = tex2 * 2.0f;   // 2^(2 * 0.5)
    for (int c = 0; c < kNpTightClasses; ++c, need *= 0.25f)
        if (px2 >= need) return c;
    return -1;
}

// Rounding: a contribution under a quarter of one 8-bit step cannot reach the picture (two roundings at most, the fragment's and the blend's). The bound is the texture's largest
// value, so no pixel of the sprite passes it. va: vertex alpha, vc: largest of the vertex r, g, b.
static inline bool NpTightInvisible(unsigned peak, unsigned peakA, unsigned va, unsigned vc, bool additive)
{
    const uint64_t a = (uint64_t)peakA * va;   // final alpha x 255
    if (additive)   // colour: peak x va x vc / 255^2 < 1/4 step; destination alpha, written with the same factors: (a / 255)^2 / 255 < 1/4
        return (uint64_t)peak * va * vc * 4u < 65025u && a * a * 4u < 16581375ull * 1u;
    return a * 4u < 255u;   // normal blend moves a pixel by at most alpha
}

#endif
