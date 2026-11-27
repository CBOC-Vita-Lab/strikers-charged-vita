// np/vgx_rainhull.h: MSC_RAIN_HULL (Vita port, wave 11). The Dump's rain streak model (template fx_dump_rain, hash 0x8270565b) is one quad (a triangle strip of 4 vertices, 0.75 units
// square) whose texture coordinates run over about 2.19 widths of a 32 x 32 RGB5A3 streak texture that REPEATS (u from -0.575 to 1.614, v from 0 to 1; the binding's wrap flags
// are 0, and glx_BindTexture turns a clear flag into GX_REPEAT). A tile holds four thin vertical dashes: about 4 % of the texels are not alpha 0, and a blended fragment cannot be
// rejected before it is shaded, so each of the 144 drops a picture pays for the whole quad (about 37 % of the stadium's fill, dumpsteam15). This works out, from the texture's
// alpha, the rectangles of one tile outside which no fragment can change a pixel; the game then draws a drop as one small quad per rectangle of each tile the quad covers (two
// triangles each, the quad's own affine map, so every pixel inside keeps the very same texture coordinate).
//
// Why the pictures stay the same. A texel of level L at (i, j) can make a sample nonzero only where the bilinear footprint reaches it: sample positions from (i - 0.5) to (i + 1.5)
// texels of that level, wrapped (the sampler repeats), which is (2i - 1) 2^L to (2i + 3) 2^L in half texels of level 0. A level is the disc's OR a box reduction of level 0 (vitaGL
// generates levels from level 0, the host GL does the same), so a texel nonzero in either counts, as in np/fxtight.h. Class c = the union of levels 0..c; a drop is drawn from the
// class that covers the deepest level its size on the screen can reach (the game decides, with np/fxtight.h's NpTightClass numbers), deeper than class 2 it stays one quad. Alpha
// is the only thing read: a texel of alpha 0 writes nothing in any blend mode of this effect, and RGB5A3's alpha is 0, 36, 73, 109, 146, 182, 219 or 255, so "nonzero" is exact.
// Pure: no game headers, for the host test native/tools/rainhull.
#ifndef NP_VGX_RAINHULL_H
#define NP_VGX_RAINHULL_H

#include "np/fxtight.h"

enum { kNpRainMaxRects = 16 };   // rectangles of one tile for one class: more than this and the class stays the whole quad (the game decides by cost)
struct NpRainRect { int x0, y0, x1, y1; };   // lattice units (1/1024 of a tile) inside one tile, 0 <= x0 < x1 <= 1024: [x0, x1) x [y0, y1)
struct NpRainTile { int n; NpRainRect r[kNpRainMaxRects]; long area; };   // area in lattice units squared; n = 0 with area = 0: nothing shows

// One class: the half-texel grid of level 0 (2W x 2H cells, grid[cy * 2W + cx] != 0: a sample inside that cell can be nonzero), built from levels 0..cls.
static inline bool NpRainHullGrid(int fmt, int W, int H, int levels, const uint8_t* data, uint8_t* const grids[kNpTightClasses])
{
    if ((fmt != kNpTightFmtRGB5A3 && fmt != kNpTightFmtCMPR) || W < 4 || H < 4 || W > 128 || H > 128 || (W & (W - 1)) || (H & (H - 1))) return false;
    const size_t cells = (size_t)W * H * 2;
    uint8_t* mem = (uint8_t*)malloc(cells * 3);
    if (!mem) return false;
    uint8_t* orp = mem, *lvl = mem + cells, *tmp = mem + cells * 2;
    memset(orp, 0, cells);
    const int GW = 2 * W, GH = 2 * H;
    for (int c = 0; c < kNpTightClasses; ++c) memset(grids[c], 0, (size_t)GW * GH);
    int w = W, h = H;
    const uint8_t* d = data;
    for (int L = 0; L < kNpTightClasses; ++L) {
        if (L < levels) {
            NpTightDecode(fmt, w, h, d, lvl);
            d += NpTightLevelBytes(fmt, w, h);
        } else {
            memset(lvl, 0, (size_t)w * h * 2);
        }
        if (L == 0) memcpy(orp, lvl, cells);
        else for (size_t i = 0; i < (size_t)w * h * 2; ++i) if (orp[i] > lvl[i]) lvl[i] = orp[i];
        uint8_t* g = grids[L];
        if (L > 0) memcpy(g, grids[L - 1], (size_t)GW * GH);
        const int s = 1 << L;   // level-0 texels per texel of this level
        for (int j = 0; j < h; ++j)
            for (int i = 0; i < w; ++i) {
                if (!lvl[(j * w + i) * 2]) continue;   // alpha 0
                // sample positions (i - 0.5) .. (i + 1.5) texels of level L = half texels of level 0 from (2i - 1) s to (2i + 3) s, wrapped
                for (int cy = (2 * j - 1) * s; cy < (2 * j + 3) * s; ++cy)
                    for (int cx = (2 * i - 1) * s; cx < (2 * i + 3) * s; ++cx)
                        g[(((cy % GH) + GH) % GH) * GW + (((cx % GW) + GW) % GW)] = 1;
            }
        if (w == 1 && h == 1) { for (int c = L + 1; c < kNpTightClasses; ++c) memcpy(grids[c], g, (size_t)GW * GH); break; }
        const int dw = w > 1 ? w / 2 : 1, dh = h > 1 ? h / 2 : 1;
        NpTightOrDown(orp, w, h, tmp, dw, dh);
        memcpy(orp, tmp, (size_t)dw * dh * 2);
        w = dw;
        h = dh;
    }
    free(mem);
    return true;
}

// The grid as rectangles of one tile: each connected component (4-neighbours, NOT across the tile's edge: the tile repeats, so a dash that crosses it is two rectangles, one
// at each edge, which is right) is its bounding rectangle; while there are more than `maxRects`, the two whose joint bounding rectangle adds the least area are merged. One step
// of margin on every side (a lattice unit, so a pixel on a rectangle's edge is not decided by rounding). Returns false: more components than the work area holds (kNpRainMaxWork).
enum { kNpRainMaxWork = 64 };
static inline bool NpRainRects(const uint8_t* grid, int GW, int GH, int maxRects, NpRainTile* out, int margin = 1, bool strips = false)
{
    out->n = 0;
    out->area = 0;
    uint8_t* lab = (uint8_t*)malloc((size_t)GW * GH);
    uint16_t* stack = (uint16_t*)malloc((size_t)GW * GH * sizeof(uint16_t) * 1);
    if (!lab || !stack) { free(lab); free(stack); return false; }
    memset(lab, 0, (size_t)GW * GH);
    int bx0[kNpRainMaxWork], by0[kNpRainMaxWork], bx1[kNpRainMaxWork], by1[kNpRainMaxWork];
    int n = 0;
    bool ok = true;
    for (int y = 0; y < GH && ok; ++y)
        for (int x = 0; x < GW && ok; ++x) {
            if (!grid[y * GW + x] || lab[y * GW + x]) continue;
            if (n == kNpRainMaxWork) { ok = false; break; }
            int sp = 0;
            stack[sp++] = (uint16_t)(y * GW + x);
            lab[y * GW + x] = 1;
            int x0 = x, x1 = x, y0 = y, y1 = y;
            while (sp > 0) {
                const int p = stack[--sp], px = p % GW, py = p / GW;
                if (px < x0) x0 = px;
                if (px > x1) x1 = px;
                if (py < y0) y0 = py;
                if (py > y1) y1 = py;
                static const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
                for (int k = 0; k < 4; ++k) {
                    const int nx = px + dx[k], ny = py + dy[k];
                    if (nx < 0 || ny < 0 || nx >= GW || ny >= GH || !grid[ny * GW + nx] || lab[ny * GW + nx]) continue;
                    lab[ny * GW + nx] = 1;
                    stack[sp++] = (uint16_t)(ny * GW + nx);
                }
            }
            bx0[n] = x0; by0[n] = y0; bx1[n] = x1 + 1; by1[n] = y1 + 1; ++n;   // (cells, [x0, x1) x [y0, y1))
        }
    free(lab);
    free(stack);
    if (!ok) return false;
    while (n > maxRects && n > 1) {
        int ba = 0, bb = 1;
        long best = -1;
        for (int a = 0; a < n; ++a)
            for (int b = a + 1; b < n; ++b) {
                const int x0 = bx0[a] < bx0[b] ? bx0[a] : bx0[b], x1 = bx1[a] > bx1[b] ? bx1[a] : bx1[b], y0 = by0[a] < by0[b] ? by0[a] : by0[b], y1 = by1[a] > by1[b] ? by1[a] : by1[b];
                const long add = (long)(x1 - x0) * (y1 - y0) - (long)(bx1[a] - bx0[a]) * (by1[a] - by0[a]) - (long)(bx1[b] - bx0[b]) * (by1[b] - by0[b]);
                if (best < 0 || add < best) { best = add; ba = a; bb = b; }
            }
        bx0[ba] = bx0[ba] < bx0[bb] ? bx0[ba] : bx0[bb]; bx1[ba] = bx1[ba] > bx1[bb] ? bx1[ba] : bx1[bb];
        by0[ba] = by0[ba] < by0[bb] ? by0[ba] : by0[bb]; by1[ba] = by1[ba] > by1[bb] ? by1[ba] : by1[bb];
        bx0[bb] = bx0[n - 1]; by0[bb] = by0[n - 1]; bx1[bb] = bx1[n - 1]; by1[bb] = by1[n - 1];
        --n;
    }
    const int cu = 1024 / GW, cv = 1024 / GH;   // lattice units per cell (GW and GH divide 1024)
    // strips: every rectangle spans the tile's whole height (the dashes are vertical). Rectangles of different heights next to each other leave T-junctions along their shared edges, and a
    // rasteriser may then draw a pixel on such an edge twice or not at all (rainhull19, 960x544 test: single pixels 6-16 levels off; with one rectangle a tile the pictures are bit-identical).
    // Full-height strips meet the tile above and below, and the strip at a tile's side the one at the next tile's other side, with the very same end points: nothing but whole edges.
    if (strips) for (int i = 0; i < n; ++i) { by0[i] = 0; by1[i] = GH; }
    // The rectangles must not overlap once their margin is on: the effect blends ADDITIVELY, so a texel in the overlap of two quads would be added twice (found by the 640x448 picture test of
    // rainhull19: single pixels 10-16 levels too bright where two merged rectangles overlapped). Rectangles whose margin rectangles overlap are merged (touching is fine: the fill rule draws a
    // shared edge once); repeated until none does.
    for (bool again = true; again && n > 1;) {
        again = false;
        for (int a = 0; a < n && !again; ++a)
            for (int b = a + 1; b < n && !again; ++b) {
                const int ax0 = bx0[a] * cu - margin, ax1 = bx1[a] * cu + margin, ay0 = by0[a] * cv - margin, ay1 = by1[a] * cv + margin;
                const int cx0 = bx0[b] * cu - margin, cx1 = bx1[b] * cu + margin, cy0 = by0[b] * cv - margin, cy1 = by1[b] * cv + margin;
                if (!(ax0 < cx1 && cx0 < ax1 && ay0 < cy1 && cy0 < ay1)) continue;
                bx0[a] = bx0[a] < bx0[b] ? bx0[a] : bx0[b]; bx1[a] = bx1[a] > bx1[b] ? bx1[a] : bx1[b];
                by0[a] = by0[a] < by0[b] ? by0[a] : by0[b]; by1[a] = by1[a] > by1[b] ? by1[a] : by1[b];
                bx0[b] = bx0[n - 1]; by0[b] = by0[n - 1]; bx1[b] = bx1[n - 1]; by1[b] = by1[n - 1];
                --n;
                again = true;
            }
    }
    for (int i = 0; i < n; ++i) {
        NpRainRect& r = out->r[out->n];
        r.x0 = bx0[i] * cu - margin; r.x1 = bx1[i] * cu + margin; r.y0 = by0[i] * cv - margin; r.y1 = by1[i] * cv + margin;
        if (r.x0 < 0) r.x0 = 0;
        if (r.y0 < 0) r.y0 = 0;
        if (r.x1 > 1024) r.x1 = 1024;
        if (r.y1 > 1024) r.y1 = 1024;
        out->area += (long)(r.x1 - r.x0) * (r.y1 - r.y0);
        ++out->n;
    }
    return true;
}

// The quad: four corners with texture coordinates in 1/4096 of a tile (s16 with 12 fraction bits: 1.0 = 4096, as the model's stream holds them), and positions.
struct NpRainQuad {
    float pos[4][3];
    int u[4], v[4];
};
// the quad is a parallelogram that maps (u, v) affinely, and its four corners are the four combinations of two u and two v values. Fills the affine frame.
struct NpRainFrame { int ulo, uhi, vlo, vhi; float o[3], du[3], dv[3]; };   // position(u, v) = o + (u - ulo) / (uhi - ulo) * du + (v - vlo) / (vhi - vlo) * dv
static inline bool NpRainFrameOf(const NpRainQuad& q, NpRainFrame* f)
{
    int ulo = q.u[0], uhi = q.u[0], vlo = q.v[0], vhi = q.v[0];
    for (int i = 1; i < 4; ++i) {
        if (q.u[i] < ulo) ulo = q.u[i];
        if (q.u[i] > uhi) uhi = q.u[i];
        if (q.v[i] < vlo) vlo = q.v[i];
        if (q.v[i] > vhi) vhi = q.v[i];
    }
    if (ulo == uhi || vlo == vhi) return false;
    int found = 0;
    const float* p[4] = {0, 0, 0, 0};
    for (int i = 0; i < 4; ++i) {
        if ((q.u[i] != ulo && q.u[i] != uhi) || (q.v[i] != vlo && q.v[i] != vhi)) return false;
        const int k = (q.u[i] == uhi ? 2 : 0) | (q.v[i] == vhi ? 1 : 0);
        found |= 1 << k;
        p[k] = q.pos[i];
    }
    if (found != 0xF) return false;
    for (int c = 0; c < 3; ++c) {
        const float e = p[0][c] + (p[2][c] - p[0][c]) + (p[1][c] - p[0][c]) - p[3][c];
        const float s = (p[2][c] > p[0][c] ? p[2][c] - p[0][c] : p[0][c] - p[2][c]) + (p[1][c] > p[0][c] ? p[1][c] - p[0][c] : p[0][c] - p[1][c]) + 1e-6f;
        if ((e < 0 ? -e : e) > 1e-4f * s) return false;
    }
    f->ulo = ulo; f->uhi = uhi; f->vlo = vlo; f->vhi = vhi;
    for (int c = 0; c < 3; ++c) { f->o[c] = p[0][c]; f->du[c] = p[2][c] - p[0][c]; f->dv[c] = p[1][c] - p[0][c]; }
    return true;
}

// Every rectangle of every tile the quad covers, clipped to the quad, as small quads: for each one, 4 corners (lo/lo, hi/lo, hi/hi, lo/hi) of position and (u, v) in 1/4096. Returns the
// number of quads written (at most maxQuads; 0 with `*overflow` set when they do not fit). A tile is 4096 in u and v; a rectangle's lattice unit is 4.
static inline int NpRainQuads(const NpRainFrame& f, const NpRainTile& t, int maxQuads, float (*pos)[4][3], int (*uv)[4][2], bool* overflow)
{
    *overflow = false;
    int nq = 0;
    const int k0u = f.ulo >= 0 ? f.ulo / 4096 : -((-f.ulo + 4095) / 4096), k1u = (f.uhi - 1) >= 0 ? (f.uhi - 1) / 4096 : -((-(f.uhi - 1) + 4095) / 4096);
    const int k0v = f.vlo >= 0 ? f.vlo / 4096 : -((-f.vlo + 4095) / 4096), k1v = (f.vhi - 1) >= 0 ? (f.vhi - 1) / 4096 : -((-(f.vhi - 1) + 4095) / 4096);
    for (int ky = k0v; ky <= k1v; ++ky)
        for (int kx = k0u; kx <= k1u; ++kx)
            for (int i = 0; i < t.n; ++i) {
                int u0 = kx * 4096 + t.r[i].x0 * 4, u1 = kx * 4096 + t.r[i].x1 * 4, v0 = ky * 4096 + t.r[i].y0 * 4, v1 = ky * 4096 + t.r[i].y1 * 4;
                if (u0 < f.ulo) u0 = f.ulo;
                if (u1 > f.uhi) u1 = f.uhi;
                if (v0 < f.vlo) v0 = f.vlo;
                if (v1 > f.vhi) v1 = f.vhi;
                if (u1 <= u0 || v1 <= v0) continue;
                if (nq == maxQuads) { *overflow = true; return 0; }
                const int cu[4] = {u0, u1, u1, u0}, cv[4] = {v0, v0, v1, v1};
                for (int k = 0; k < 4; ++k) {
                    const double fu = (double)(cu[k] - f.ulo) / (double)(f.uhi - f.ulo), fv = (double)(cv[k] - f.vlo) / (double)(f.vhi - f.vlo);
                    for (int c = 0; c < 3; ++c) pos[nq][k][c] = (float)((double)f.o[c] + fu * (double)f.du[c] + fv * (double)f.dv[c]);
                    uv[nq][k][0] = cu[k];
                    uv[nq][k][1] = cv[k];
                }
                ++nq;
            }
    return nq;
}

#endif
