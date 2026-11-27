/* np/floorbake.h: MSC_FLOOR_BAKE (1.3), default off, render-only.
 *
 *   MSC_FLOOR_BAKE=<ids>|all   empty (default). Stadium numbers by comma (as MSC_STADIUM_HEAVY), or all. The two reflective pitch materials (CameraScrolledOverlay,
 *                              ScrollingCameraOverlay) draw  vertex colour * diffuse + reflection * reflection alpha * amount * mask  with three textures, plus the stadium's
 *                              shadow lookup. The reflection moves with the camera, so that term cannot go into a texture as it is. Its average can: at model load the
 *                              average of the reflection times the amount times the mask is added to the end colours of each CMPR block of the diffuse texture, in place, at
 *                              every mip level, and the amount is set to 0. Such a packet draws with one TEV stage and one texture, plus the lookup. Lost: the moving sheen.
 *                              Kept: the mask's shape down to one block (4 texels), the vertex colours, the lookup, scrolling of the diffuse. No new texture, no render
 *                              target, no extra memory. A camera-still overlay (Storm Ship's animated generators), an animated or non-CMPR mask or diffuse, and a mask sharper
 *                              than the diffuse stay as they were. FloorBake.cpp.
 *
 *   MSC_FLOOR_SHEEN=vertex     empty (default). With MSC_FLOOR_BAKE: a baked packet gets back the moving part of the reflection, without its textures. Each frame, per vertex, on the
 *                              CPU: (reflection at the vertex's scrolled coordinates - its mean) * amount * the mask around the vertex, from a table of at most 32 x 32 made of the
 *                              reflection at load. It reaches the fragment stage as COLOR1, and a second TEV stage adds it, signed: no texture read. A mesh too sparse for that
 *                              (an edge over 2/16 of the reflection's scale: Wastelands) takes a 16 x 16 lookup instead, the one extra read: the same term at the camera's
 *                              coordinates, with one mask value for the packet. lookup: that for every packet, for comparison. FloorBake.cpp.
 *
 * The block arithmetic is here, without game headers, so that a host test can compile it (native/tools/scenecut). Nothing here reads or writes simulation state.
 */
#ifndef NP_FLOORBAKE_H
#define NP_FLOORBAKE_H

#include <stdint.h>
#include <string.h>

#include "np/scenecut.h"

// Is this stadium's floor to be baked? (empty: no; "all": every stadium; otherwise a list as for MSC_STADIUM_HEAVY)
static inline int NpFloorBakeHas(const char* list, int stadium)
{
    if (!list || !*list || stadium < 0) return 0;
    if (strcmp(list, "all") == 0) return 1;
    return NpSceneHeavyHas(list, stadium);
}

static inline unsigned NpFbBe16(const uint8_t* p) { return ((unsigned)p[0] << 8) | p[1]; }

static inline void NpFbColour(unsigned c, unsigned* rgb)
{
    rgb[0] = ((c >> 11) & 31u) * 255u / 31u;
    rgb[1] = ((c >> 5) & 63u) * 255u / 63u;
    rgb[2] = (c & 31u) * 255u / 31u;
}

// The mean colour of one CMPR block (GX byte order), as the hardware's palette gives it, and how many of its 16 texels are opaque
static inline unsigned NpFbBlockSum(const uint8_t* b, unsigned* sum)
{
    const unsigned c0 = NpFbBe16(b), c1 = NpFbBe16(b + 2);
    unsigned p[4][3];
    NpFbColour(c0, p[0]);
    NpFbColour(c1, p[1]);
    for (int i = 0; i < 3; ++i)
    {
        if (c0 > c1)
        {
            p[2][i] = (2u * p[0][i] + p[1][i]) / 3u;
            p[3][i] = (p[0][i] + 2u * p[1][i]) / 3u;
        }
        else
        {
            p[2][i] = (p[0][i] + p[1][i]) / 2u;
            p[3][i] = 0u;
        }
    }
    unsigned solid = 0;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
        {
            const unsigned v = (b[4 + y] >> (6 - 2 * x)) & 3u;
            if (c0 <= c1 && v == 3u) continue;
            ++solid;
            for (int i = 0; i < 3; ++i) sum[i] += p[v][i];
        }
    return solid;
}

// The block (x, y, in units of 4 texels) of a CMPR level whose width is w: blocks go by two in tiles of 8 x 8 texels
static inline size_t NpFbBlockOffset(unsigned w, unsigned x, unsigned y)
{
    const size_t tw = (w + 7u) / 8u;
    return (((size_t)(y >> 1) * tw + (x >> 1)) * 4u + (y & 1u) * 2u + (x & 1u)) * 8u;
}

static inline size_t NpFbLevelBytes(unsigned w, unsigned h)
{
    return (size_t)((w + 7u) / 8u) * ((h + 7u) / 8u) * 32u;
}

// Adds a colour (0..255 a channel) to the two end colours of a block, keeping what the block means: whether it has four colours or three and a hole. Rounds to the nearest step of the
// 5:6:5 end colours, so an add under half a step (4 of 255 in red and blue) does nothing.
static inline void NpFbShiftBlock(uint8_t* b, unsigned ar, unsigned ag, unsigned ab)
{
    const unsigned c0 = NpFbBe16(b), c1 = NpFbBe16(b + 2);
    const unsigned dr = (ar * 31u + 127u) / 255u, dg = (ag * 63u + 127u) / 255u, db = (ab * 31u + 127u) / 255u;
    if (!(dr | dg | db)) return;
    const auto shift = [&](unsigned c) {
        unsigned r = ((c >> 11) & 31u) + dr, g = ((c >> 5) & 63u) + dg, bl = (c & 31u) + db;
        if (r > 31u) r = 31u;
        if (g > 63u) g = 63u;
        if (bl > 31u) bl = 31u;
        return (r << 11) | (g << 5) | bl;
    };
    unsigned n0 = shift(c0), n1 = shift(c1);
    const int four = c0 > c1;
    if (four && n0 < n1)
    {
        const unsigned t = n0; n0 = n1; n1 = t;
        for (int y = 0; y < 4; ++y) b[4 + y] ^= 0x55u;              // 0<->1, 2<->3: the same texels
    }
    else if (four && n0 == n1)
    {
        for (int y = 0; y < 4; ++y) b[4 + y] = 0u;                  // one colour now: index 3 would be the hole of a three-colour block
    }
    else if (!four && n0 > n1)
    {
        const unsigned t = n0; n0 = n1; n1 = t;
        for (int y = 0; y < 4; ++y)
        {
            const unsigned r = b[4 + y];
            b[4 + y] = (uint8_t)(r ^ ((~r >> 1) & 0x55u));          // 0<->1 where the top bit is clear; 2 and 3 stay
        }
    }
    b[0] = (uint8_t)(n0 >> 8); b[1] = (uint8_t)n0;
    b[2] = (uint8_t)(n1 >> 8); b[3] = (uint8_t)n1;
}

// Mean colour times alpha of the top level of a texture (GX formats: 0 RGB565, 1 RGB5A3, 2 CMPR, 3 RGBA8). 0: another format.
static inline int NpFbMeanOverlay(int format, const uint8_t* s, unsigned w, unsigned h, unsigned* rgb)
{
    uint64_t sum[3] = {0, 0, 0};
    uint64_t n = 0;
    if (format == 2)
    {
        const unsigned bw = (w + 3u) / 4u, bh = (h + 3u) / 4u;
        for (unsigned y = 0; y < bh; ++y)
            for (unsigned x = 0; x < bw; ++x)
            {
                unsigned b[3] = {0, 0, 0};
                NpFbBlockSum(s + NpFbBlockOffset(w, x, y), b);   // (a hole counts as black, as the reflection term sees it)
                for (int i = 0; i < 3; ++i) sum[i] += b[i];
                n += 16u;
            }
    }
    else if (format == 0 || format == 1)
    {
        const size_t count = (size_t)((w + 3u) / 4u) * ((h + 3u) / 4u) * 16u;
        for (size_t i = 0; i < count; ++i, ++n)
        {
            const unsigned c = NpFbBe16(s + i * 2u);
            unsigned p[3], a = 255u;
            if (format == 0) NpFbColour(c, p);
            else if (c & 0x8000u) { p[0] = ((c >> 10) & 31u) * 255u / 31u; p[1] = ((c >> 5) & 31u) * 255u / 31u; p[2] = (c & 31u) * 255u / 31u; }
            else { p[0] = ((c >> 8) & 15u) * 17u; p[1] = ((c >> 4) & 15u) * 17u; p[2] = (c & 15u) * 17u; a = ((c >> 12) & 7u) * 255u / 7u; }
            for (int k = 0; k < 3; ++k) sum[k] += p[k] * a / 255u;
        }
    }
    else if (format == 3)
    {
        const unsigned tiles = ((w + 3u) / 4u) * ((h + 3u) / 4u);
        for (unsigned t = 0; t < tiles; ++t, s += 64)
            for (int i = 0; i < 16; ++i, ++n)
            {
                const unsigned a = s[i * 2], r = s[i * 2 + 1], g = s[32 + i * 2], b = s[32 + i * 2 + 1];
                sum[0] += r * a / 255u; sum[1] += g * a / 255u; sum[2] += b * a / 255u;
            }
    }
    else
        return 0;
    if (!n) return 0;
    for (int i = 0; i < 3; ++i) rgb[i] = (unsigned)(sum[i] / n);
    return 1;
}

// How a mask's coordinates follow the diffuse's: mask.u = su * u + ou, mask.v = sv * v + ov
struct NpFbMap
{
    float su, ou, sv, ov;
};

// The map from a packet's two coordinate arrays (n pairs of 16-bit values with 10 fractional bits, host order). 0: not such a map (within tol, in texture widths).
static inline int NpFbFitMap(const int16_t* d, const int16_t* m, unsigned n, float tol, NpFbMap* out)
{
    if (n == 0) return 0;
    for (int axis = 0; axis < 2; ++axis)
    {
        unsigned lo = 0, hi = 0;
        for (unsigned i = 1; i < n; ++i)
        {
            if (d[i * 2 + axis] < d[lo * 2 + axis]) lo = i;
            if (d[i * 2 + axis] > d[hi * 2 + axis]) hi = i;
        }
        const float span = (float)(d[hi * 2 + axis] - d[lo * 2 + axis]);
        const float s = span > 0.0f ? (float)(m[hi * 2 + axis] - m[lo * 2 + axis]) / span : 1.0f;
        const float o = ((float)m[lo * 2 + axis] - s * (float)d[lo * 2 + axis]) / 1024.0f;
        for (unsigned i = 0; i < n; ++i)
        {
            const float e = s * (float)d[i * 2 + axis] / 1024.0f + o - (float)m[i * 2 + axis] / 1024.0f;
            if (e > tol || e < -tol) return 0;
        }
        if (axis == 0) { out->su = s; out->ou = o; }
        else { out->sv = s; out->ov = o; }
    }
    return 1;
}

// A mask's mean colour per block (3 bytes each, (w + 3) / 4 x (h + 3) / 4 of them) from its top level
static inline void NpFbMaskGrid(const uint8_t* s, unsigned w, unsigned h, uint8_t* grid)
{
    const unsigned bw = (w + 3u) / 4u, bh = (h + 3u) / 4u;
    for (unsigned y = 0; y < bh; ++y)
        for (unsigned x = 0; x < bw; ++x, grid += 3)
        {
            unsigned b[3] = {0, 0, 0};
            NpFbBlockSum(s + NpFbBlockOffset(w, x, y), b);
            for (int i = 0; i < 3; ++i) grid[i] = (uint8_t)(b[i] / 16u);
        }
}

static inline unsigned NpFbWrap(int v, unsigned n)
{
    if (n == 0) return 0;   // (no cells: no remainder by zero)
    const int m = v % (int)n;
    return (unsigned)(m < 0 ? m + (int)n : m);
}

// Adds overlay (mean colour times alpha, 0..255) * amount (0..255) * mask to every block of every level of a CMPR texture, in place. A block takes the mean of the grid cells it covers.
static inline void NpFbBakeLevels(uint8_t* data, unsigned w, unsigned h, unsigned levels, const uint8_t* grid, unsigned gw, unsigned gh, const NpFbMap* map, const unsigned* overlay, unsigned amount)
{
    for (unsigned l = 0; l < levels; ++l)
    {
        const unsigned bw = (w + 3u) / 4u, bh = (h + 3u) / 4u;
        for (unsigned y = 0; y < bh; ++y)
            for (unsigned x = 0; x < bw; ++x)
            {
                const float u0 = map->su * (float)x / (float)bw + map->ou, u1 = map->su * (float)(x + 1u) / (float)bw + map->ou;
                const float v0 = map->sv * (float)y / (float)bh + map->ov, v1 = map->sv * (float)(y + 1u) / (float)bh + map->ov;
                const float xa = (u0 < u1 ? u0 : u1) * (float)gw, xb = (u0 < u1 ? u1 : u0) * (float)gw;
                const float ya = (v0 < v1 ? v0 : v1) * (float)gh, yb = (v0 < v1 ? v1 : v0) * (float)gh;
                const int cx0 = xa < 0.0f ? (int)xa - 1 : (int)xa, cy0 = ya < 0.0f ? (int)ya - 1 : (int)ya;
                int cx1 = xb < 0.0f ? (int)xb - 1 : (int)xb, cy1 = yb < 0.0f ? (int)yb - 1 : (int)yb;
                if (cx1 > cx0 && (float)cx1 == xb) --cx1;
                if (cy1 > cy0 && (float)cy1 == yb) --cy1;
                if (cx1 > cx0 + 63) cx1 = cx0 + 63;
                if (cy1 > cy0 + 63) cy1 = cy0 + 63;
                unsigned sum[3] = {0, 0, 0}, n = 0;
                for (int cy = cy0; cy <= cy1; ++cy)
                    for (int cx = cx0; cx <= cx1; ++cx, ++n)
                    {
                        const uint8_t* c = grid + (size_t)(NpFbWrap(cy, gh) * gw + NpFbWrap(cx, gw)) * 3u;
                        for (int i = 0; i < 3; ++i) sum[i] += c[i];
                    }
                const unsigned a0 = (overlay[0] * amount / 255u) * (sum[0] / n) / 255u;
                const unsigned a1 = (overlay[1] * amount / 255u) * (sum[1] / n) / 255u;
                const unsigned a2 = (overlay[2] * amount / 255u) * (sum[2] / n) / 255u;
                NpFbShiftBlock(data + NpFbBlockOffset(w, x, y), a0, a1, a2);
            }
        data += NpFbLevelBytes(w, h);
        w = w > 1u ? w >> 1 : 1u;
        h = h > 1u ? h >> 1 : 1u;
    }
}

// ---- MSC_FLOOR_SHEEN

enum
{
    NP_FB_FULL = 0,          // the game's material
    NP_FB_BAKED = 1,         // one texture
    NP_FB_SHEEN_VERTEX = 2,  // one texture, COLOR1 added
    NP_FB_SHEEN_LOOKUP = 3,  // two textures: a 16 x 16 lookup added
};

static inline int NpFloorSheenParse(const char* e)
{
    if (!e) return 0;
    if (strcmp(e, "vertex") == 0) return NP_FB_SHEEN_VERTEX;
    if (strcmp(e, "lookup") == 0) return NP_FB_SHEEN_LOOKUP;
    return 0;
}

// Which sheen a packet takes: the reflection table's cells are about scale / 16 at most; a triangle two of them across cannot carry it
static inline int NpFloorSheenMode(int wanted, float longestEdge, float scale)
{
    if (wanted == NP_FB_SHEEN_VERTEX && longestEdge > 2.0f * (scale < 0.0f ? -scale : scale) / 16.0f) return NP_FB_SHEEN_LOOKUP;
    return wanted;
}

enum { NP_FB_GRID = 32, NP_FB_LUT = 16, NP_FB_LUT_BYTES = NP_FB_LUT * NP_FB_LUT / 16 * 64 };

// A reflection's colour times alpha at most NP_FB_GRID cells a side (box filter), and its mean. *gw, *gh: the cells. 0: another format. sums: 4 * 4 * NP_FB_GRID * NP_FB_GRID bytes of room.
static inline void NpFbGridAdd(uint32_t* sums, unsigned gw, unsigned gh, unsigned w, unsigned h, unsigned x, unsigned y, unsigned r, unsigned g, unsigned b)
{
    if (x >= w || y >= h) return;
    uint32_t* c = sums + (size_t)(y * gh / h * gw + x * gw / w) * 4u;
    c[0] += r; c[1] += g; c[2] += b; c[3] += 1u;
}

static inline int NpFbReflectionGrid(int format, const uint8_t* s, unsigned w, unsigned h, uint8_t* grid, uint32_t* sums, unsigned* gw, unsigned* gh, int* mean)
{
    const unsigned cw = w < (unsigned)NP_FB_GRID ? w : (unsigned)NP_FB_GRID, ch = h < (unsigned)NP_FB_GRID ? h : (unsigned)NP_FB_GRID;
    memset(sums, 0, (size_t)cw * ch * 16u);
    if (format == 2)
    {
        for (unsigned y = 0; y < (h + 3u) / 4u; ++y)
            for (unsigned x = 0; x < (w + 3u) / 4u; ++x)
            {
                const uint8_t* b = s + NpFbBlockOffset(w, x, y);
                const unsigned c0 = NpFbBe16(b), c1 = NpFbBe16(b + 2);
                unsigned p[4][3];
                NpFbColour(c0, p[0]);
                NpFbColour(c1, p[1]);
                for (int i = 0; i < 3; ++i)
                {
                    p[2][i] = c0 > c1 ? (2u * p[0][i] + p[1][i]) / 3u : (p[0][i] + p[1][i]) / 2u;
                    p[3][i] = c0 > c1 ? (p[0][i] + 2u * p[1][i]) / 3u : 0u;
                }
                for (unsigned j = 0; j < 4u; ++j)
                    for (unsigned i = 0; i < 4u; ++i)
                    {
                        const unsigned v = (b[4 + j] >> (6u - 2u * i)) & 3u;
                        NpFbGridAdd(sums, cw, ch, w, h, x * 4u + i, y * 4u + j, p[v][0], p[v][1], p[v][2]);
                    }
            }
    }
    else if (format == 0 || format == 1 || format == 3)
    {
        for (unsigned y = 0; y < (h + 3u) / 4u; ++y)
            for (unsigned x = 0; x < (w + 3u) / 4u; ++x)
            {
                for (unsigned j = 0; j < 4u; ++j)
                    for (unsigned i = 0; i < 4u; ++i)
                    {
                        unsigned p[3], a = 255u;
                        if (format == 3)
                        {
                            const unsigned k = j * 4u + i;
                            a = s[k * 2u]; p[0] = s[k * 2u + 1u]; p[1] = s[32u + k * 2u]; p[2] = s[32u + k * 2u + 1u];
                        }
                        else
                        {
                            const unsigned c = NpFbBe16(s + (j * 4u + i) * 2u);
                            if (format == 0) NpFbColour(c, p);
                            else if (c & 0x8000u) { p[0] = ((c >> 10) & 31u) * 255u / 31u; p[1] = ((c >> 5) & 31u) * 255u / 31u; p[2] = (c & 31u) * 255u / 31u; }
                            else { p[0] = ((c >> 8) & 15u) * 17u; p[1] = ((c >> 4) & 15u) * 17u; p[2] = (c & 15u) * 17u; a = ((c >> 12) & 7u) * 255u / 7u; }
                        }
                        NpFbGridAdd(sums, cw, ch, w, h, x * 4u + i, y * 4u + j, p[0] * a / 255u, p[1] * a / 255u, p[2] * a / 255u);
                    }
                s += format == 3 ? 64u : 32u;
            }
    }
    else
        return 0;
    uint64_t total[3] = {0, 0, 0};
    uint64_t n = 0;
    for (unsigned i = 0; i < cw * ch; ++i)
    {
        const uint32_t* c = sums + (size_t)i * 4u;
        for (int k = 0; k < 3; ++k)
        {
            grid[i * 3u + (unsigned)k] = (uint8_t)(c[3] ? c[k] / c[3] : 0u);
            total[k] += c[k];
        }
        n += c[3];
    }
    if (!n) return 0;
    for (int k = 0; k < 3; ++k) mean[k] = (int)(total[k] / n);
    *gw = cw;
    *gh = ch;
    return 1;
}

// A bilinear read of the table at (u, v) with repeat, 0..255 a channel, as the hardware reads the reflection
static inline void NpFbGridSample(const uint8_t* grid, unsigned gw, unsigned gh, float u, float v, int* out)
{
    if (!gw || !gh) { out[0] = out[1] = out[2] = 128; return; }   // (no table: the neutral grey; and no remainder by zero below)
    const float x = (u - (float)(int)u) * (float)gw + 2.0f * (float)gw - 0.5f, y = (v - (float)(int)v) * (float)gh + 2.0f * (float)gh - 0.5f;   // (never negative: the fraction is over -1)
    const unsigned x0 = (unsigned)x, y0 = (unsigned)y;
    const int fx = (int)((x - (float)x0) * 256.0f), fy = (int)((y - (float)y0) * 256.0f);
    const uint8_t* a = grid + (size_t)((y0 % gh) * gw + x0 % gw) * 3u;
    const uint8_t* b = grid + (size_t)((y0 % gh) * gw + (x0 + 1u) % gw) * 3u;
    const uint8_t* c = grid + (size_t)(((y0 + 1u) % gh) * gw + x0 % gw) * 3u;
    const uint8_t* d = grid + (size_t)(((y0 + 1u) % gh) * gw + (x0 + 1u) % gw) * 3u;
    for (int i = 0; i < 3; ++i)
    {
        const int top = a[i] * (256 - fx) + b[i] * fx, low = c[i] * (256 - fx) + d[i] * fx;
        out[i] = (top * (256 - fy) + low * fy) >> 16;
    }
}

// What a packet needs each frame
struct NpFloorSheen
{
    int mode;                 // NP_FB_SHEEN_VERTEX or NP_FB_SHEEN_LOOKUP
    unsigned count;           // vertices
    float scale, scroll;      // of the material: its texture matrix
    float* xy;                // the position stream's x, y, host order, count pairs
    uint8_t* weight;          // amount * mask around the vertex, 3 a vertex
    uint32_t* colours;        // COLOR1 of this frame, count of them, 0xRRGGBBAA, 0x80 is nothing
    const uint8_t* grid;      // the reflection (shared by the packets of a texture)
    unsigned gw, gh;
    int mean[3];
    uint8_t lookup[NP_FB_LUT_BYTES + 32];   // (NP_FB_SHEEN_LOOKUP)
    uint8_t* lookupData;      // the same, 32-aligned
    unsigned long lookupObject[8];
    uint8_t key[4];           // (NP_FB_SHEEN_LOOKUP) amount * mean mask * 2, as GXSetTevKColor takes it
};

// The material's coordinates: uv = position / scale + 0.5 - scroll * camera / scale
static inline void NpFbSheenColours(NpFloorSheen* t, float cameraX, float cameraY)
{
    const float inverse = 1.0f / t->scale;
    const float ou = 0.5f - t->scroll * cameraX * inverse, ov = 0.5f - t->scroll * cameraY * inverse;
    for (unsigned i = 0; i < t->count; ++i)
    {
        int r[3];
        const float u = t->xy[i * 2u] * inverse + ou, v = t->xy[i * 2u + 1u] * inverse + ov;
        NpFbGridSample(t->grid, t->gw, t->gh, u - (float)(int)u, v - (float)(int)v, r);
        uint32_t c = 0xFFu;
        for (int k = 0; k < 3; ++k)
        {
            int d = 128 + (r[k] - t->mean[k]) * (int)t->weight[i * 3u + (unsigned)k] / 255;
            d = d < 0 ? 0 : (d > 255 ? 255 : d);
            c |= (uint32_t)d << (24 - 8 * k);
        }
        t->colours[i] = c;
    }
}

// The lookup: 16 x 16 RGBA8 in GX's tiles, 0.5 + (reflection - mean) / 2 (the TEV stage scales it by key). The cells of the table are boxed down.
static inline void NpFbLookupTexture(const uint8_t* grid, unsigned gw, unsigned gh, const int* mean, uint8_t* out)
{
    for (unsigned ty = 0; ty < NP_FB_LUT / 4u; ++ty)
        for (unsigned tx = 0; tx < NP_FB_LUT / 4u; ++tx, out += 64)
            for (unsigned j = 0; j < 4u; ++j)
                for (unsigned i = 0; i < 4u; ++i)
                {
                    const unsigned x = tx * 4u + i, y = ty * 4u + j;
                    const unsigned x0 = x * gw / NP_FB_LUT, x1 = (x + 1u) * gw / NP_FB_LUT, y0 = y * gh / NP_FB_LUT, y1 = (y + 1u) * gh / NP_FB_LUT;
                    int sum[3] = {0, 0, 0}, n = 0;
                    for (unsigned cy = y0; cy < (y1 > y0 ? y1 : y0 + 1u); ++cy)
                        for (unsigned cx = x0; cx < (x1 > x0 ? x1 : x0 + 1u); ++cx, ++n)
                            for (int k = 0; k < 3; ++k) sum[k] += grid[(size_t)((cy % gh) * gw + cx % gw) * 3u + (unsigned)k];
                    const unsigned at = (j * 4u + i) * 2u;
                    int c[3];
                    for (int k = 0; k < 3; ++k)
                    {
                        c[k] = 128 + (sum[k] / n - mean[k]) / 2;
                        c[k] = c[k] < 0 ? 0 : (c[k] > 255 ? 255 : c[k]);
                    }
                    out[at] = 0xFF;
                    out[at + 1u] = (uint8_t)c[0];
                    out[32u + at] = (uint8_t)c[1];
                    out[32u + at + 1u] = (uint8_t)c[2];
                }
}

// The mean of a mask's block grid in a square of 2r + 1 cells around (u, v), repeating
static inline void NpFbMaskAround(const uint8_t* grid, unsigned gw, unsigned gh, float u, float v, int r, unsigned* out)
{
    const int cx = (int)((u - (float)(int)u + 2.0f) * (float)gw), cy = (int)((v - (float)(int)v + 2.0f) * (float)gh);
    unsigned sum[3] = {0, 0, 0}, n = 0;
    for (int y = cy - r; y <= cy + r; ++y)
        for (int x = cx - r; x <= cx + r; ++x, ++n)
        {
            const uint8_t* c = grid + (size_t)(NpFbWrap(y, gh) * gw + NpFbWrap(x, gw)) * 3u;
            for (int i = 0; i < 3; ++i) sum[i] += c[i];
        }
    for (int i = 0; i < 3; ++i) out[i] = sum[i] / n;
}

#ifdef NP_NATIVE
#ifdef __cplusplus
struct glModelPacket;
struct glTextureBinding;
int NpFloorBakeOn();   // MSC_FLOOR_BAKE names anything at all: the two programs look at the amount of their packets
void NpFloorBakePacket(glModelPacket* packet, const glTextureBinding* diffuse, const glTextureBinding* overlay, glTextureBinding* mask, float overlayScale, float cameraScroll, float* overlayAmount, int diffuseStream, int maskStream);
void NpFloorBakeReport(int stadium);
// A baked packet's way of drawing: NP_FB_BAKED, or a sheen (maskBinding is what NpFloorBakePacket left in the mask's binding). The two programs' Draw.
NpFloorSheen* NpFloorSheenOf(const glTextureBinding* maskBinding);
void NpFloorTevEnter(int mode, NpFloorSheen* sheen);     // TEV and channels for the mode; texture coordinate 0 (and 1, for the lookup) stay as the program configured them
void NpFloorTevLeave(int mode);                          // what only those modes changed
void NpFloorDrawSheen(const glModelPacket* packet, NpFloorSheen* sheen, int diffuseAttribute);   // the packet, with COLOR1 per vertex
void NpFloorKey(const NpFloorSheen* sheen);              // (NP_FB_SHEEN_LOOKUP) the stage's scale
#endif
#endif

#endif // NP_FLOORBAKE_H
