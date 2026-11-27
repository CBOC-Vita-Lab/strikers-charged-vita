// MSC_VGX_TEX_COMPRESS (wave 15, texc15; the design of Galaxy's GXV_TEX_COMPRESS, vgx_texcomp.cpp, cut down to what Strikers needs): big uncompressed colour textures become DXT1 / DXT5
// on the console, once, and the result is kept on the memory card. Pure: standard library only (stdio for the card files), no GL, no vitaGL; tests/texcomp.cpp builds it on the host.
//
// WHY. The Dump is GPU-bound and fetch-heavy (3.19 texture reads a fragment): of 585 live textures (29.4 MiB) 278 (23.7 MiB) are uncompressed. The SGX samples DXT1 / DXT5 natively (UBC1 / UBC3):
// 1/8 and 1/4 of the bytes of RGBA8 for every texel a cache miss fetches, and the memory comes back (CDRAM is full at kick-off).
//
// WHAT (a texture must pass all of these):
//   static   RGBA8 upload, both sides powers of two and >= minSide (16), at least minTexels (4096 = 64x64), consistent level sizes, not an HD replacement, not an EFB copy / render target
//            (those never reach Upload), its id never re-uploaded with other contents (an animated texture).
//   usage    the back end has seen it drawn >= readyPictures pictures (default 8) with a PERSPECTIVE projection and NEVER with an orthographic one, never from the HUD class / a front-end layer, never by a
//            shader with indirect stages or a Z-texture (their texel bits are coordinates): UseVerdict. A texture seen in 2D is VETOED on the card for good (the next run never touches it).
//   quality  the encoded level 0 is within the floor: RGB PSNR >= psnrRgb (32 dB); alpha: binary alpha (only 0 and 255) must stay EXACT, smooth alpha needs PSNR >= psnrA (36 dB). A texture that fails
//            keeps its RGBA8 bytes and the card remembers the verdict. Opaque -> DXT1, anything else -> DXT5.
//   The texture is uploaded as before and replaced (same id, same sampler state) a few pictures later, when the verdict is in: the first pictures look exactly as before.
// COST. One conversion is a sliced job (blocksPerPicture 4x4 blocks a picture, on the render thread, at the picture's end): no worker thread. After the first run every verdict and payload is on the card:
// a replacement is one small read. CARD: <stem>.idx (32-byte records, crc checked) + <stem>.dat (payloads, hash checked), append only, torn tail ignored, version in the header, size cap.
// Nothing is shipped: the files are made on the player's console from the player's own game data.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace vgx {
namespace tc {

constexpr uint32_t kEncoderVersion = 1;   // in every cache key: a better encoder gets a new number and the card re-learns
constexpr uint32_t kFileVersion = 1;      // the card files' layout
enum : uint8_t { kFmtNone = 0, kFmtDxt1 = 1, kFmtDxt5 = 3 };   // = vgx_cmd.h TF_DXT1 / TF_DXT5 (the back end checks it)

// ================================================================ the encoder (Galaxy vgx_texcomp.cpp's, same algorithm; tests/texcomp.cpp compares it with that file when it is present)
namespace enc {
inline int Expand5(int v) { return (v << 3) | (v >> 2); }
inline int Expand6(int v) { return (v << 2) | (v >> 4); }
inline void Unpack565(uint32_t c, int o[3]) { o[0] = Expand5((c >> 11) & 31); o[1] = Expand6((c >> 5) & 63); o[2] = Expand5(c & 31); }
inline uint32_t Pack565(int r5, int g6, int b5) { return static_cast<uint32_t>((r5 << 11) | (g6 << 5) | b5); }
inline int Q5(float v) { const int q = static_cast<int>(v * (31.0f / 255.0f) + 0.5f); return q < 0 ? 0 : q > 31 ? 31 : q; }
inline int Q6(float v) { const int q = static_cast<int>(v * (63.0f / 255.0f) + 0.5f); return q < 0 ? 0 : q > 63 ? 63 : q; }

inline void Palette4(uint32_t c0, uint32_t c1, int pal[4][3]) {
    Unpack565(c0, pal[0]);
    Unpack565(c1, pal[1]);
    for (int k = 0; k < 3; ++k) {
        pal[2][k] = (2 * pal[0][k] + pal[1][k] + 1) / 3;
        pal[3][k] = (pal[0][k] + 2 * pal[1][k] + 1) / 3;
    }
}
inline uint32_t Assign(const uint8_t* px, uint32_t c0, uint32_t c1, uint8_t idx[16]) {
    int pal[4][3];
    Palette4(c0, c1, pal);
    uint32_t total = 0;
    for (int i = 0; i < 16; ++i) {
        const uint8_t* p = px + i * 4;
        uint32_t best = ~0u;
        uint8_t bi = 0;
        for (uint8_t e = 0; e < 4; ++e) {
            const int dr = p[0] - pal[e][0], dg = p[1] - pal[e][1], db = p[2] - pal[e][2];
            const uint32_t d = static_cast<uint32_t>(dr * dr + dg * dg + db * db);
            if (d < best) { best = d; bi = e; }
        }
        idx[i] = bi;
        total += best;
    }
    return total;
}
struct OneColour { uint8_t e0, e1; };
struct OneColourTables {
    OneColour t5[256], t6[256];
    OneColourTables() {
        for (int v = 0; v < 256; ++v) {
            for (int bits = 5; bits <= 6; ++bits) {
                const int top = (1 << bits) - 1;
                int best = 1 << 30;
                OneColour bc{0, 0};
                for (int a = 0; a <= top; ++a)
                    for (int b = 0; b <= top; ++b) {
                        const int ea = bits == 5 ? Expand5(a) : Expand6(a), eb = bits == 5 ? Expand5(b) : Expand6(b);
                        const int mix = (2 * ea + eb + 1) / 3;
                        const int err = std::abs(mix - v) * 8 + (a == b ? 0 : 1);
                        if (err < best) { best = err; bc = OneColour{static_cast<uint8_t>(a), static_cast<uint8_t>(b)}; }
                    }
                (bits == 5 ? t5 : t6)[v] = bc;
            }
        }
    }
};
inline const OneColourTables& Tables() { static const OneColourTables t; return t; }
inline void WriteColour(uint8_t out[8], uint32_t c0, uint32_t c1, const uint8_t idx[16]) {
    uint8_t ix[16];
    std::memcpy(ix, idx, 16);
    if (c0 < c1) {   // four-colour mode needs c0 > c1: swap the endpoints, 0 <-> 1, 2 <-> 3
        std::swap(c0, c1);
        for (uint8_t& i : ix) i ^= 1;
    }
    if (c0 == c1) std::memset(ix, 0, 16);   // (one colour: every decoder reads index 0 as c0)
    out[0] = static_cast<uint8_t>(c0); out[1] = static_cast<uint8_t>(c0 >> 8);
    out[2] = static_cast<uint8_t>(c1); out[3] = static_cast<uint8_t>(c1 >> 8);
    for (int y = 0; y < 4; ++y) out[4 + y] = static_cast<uint8_t>(ix[y * 4] | (ix[y * 4 + 1] << 2) | (ix[y * 4 + 2] << 4) | (ix[y * 4 + 3] << 6));
}
}   // namespace enc

// One 4x4 block (16 RGBA texels, row-major). The colour half is always DXT1's four-colour mode (colour 0 > colour 1, or both equal with every index 0), so it decodes the same as a
// DXT1 block and as the colour half of a DXT5 block on any decoder.
inline void EncodeColorBlock(const uint8_t rgba[64], uint8_t out[8]) {
    using namespace enc;
    bool solid = true;
    for (int i = 1; i < 16 && solid; ++i) solid = rgba[i * 4] == rgba[0] && rgba[i * 4 + 1] == rgba[1] && rgba[i * 4 + 2] == rgba[2];
    if (solid) {
        const OneColourTables& t = Tables();
        const OneColour r = t.t5[rgba[0]], g = t.t6[rgba[1]], b = t.t5[rgba[2]];
        const uint32_t c0 = Pack565(r.e0, g.e0, b.e0), c1 = Pack565(r.e1, g.e1, b.e1);
        uint8_t ia[16];
        const uint32_t eMix = Assign(rgba, c0, c1, ia);
        const uint32_t cq = Pack565(Q5(rgba[0]), Q6(rgba[1]), Q5(rgba[2]));
        uint8_t ib[16];
        const uint32_t eQ = Assign(rgba, cq, cq, ib);
        if (eQ <= eMix) { WriteColour(out, cq, cq, ib); return; }
        WriteColour(out, c0, c1, ia);
        return;
    }
    float mean[3] = {0, 0, 0};
    for (int i = 0; i < 16; ++i) for (int k = 0; k < 3; ++k) mean[k] += rgba[i * 4 + k];
    for (float& m : mean) m *= 1.0f / 16.0f;
    float cov[6] = {0, 0, 0, 0, 0, 0};   // rr rg rb gg gb bb
    for (int i = 0; i < 16; ++i) {
        const float r = rgba[i * 4] - mean[0], g = rgba[i * 4 + 1] - mean[1], b = rgba[i * 4 + 2] - mean[2];
        cov[0] += r * r; cov[1] += r * g; cov[2] += r * b; cov[3] += g * g; cov[4] += g * b; cov[5] += b * b;
    }
    float ax[3];
    {
        const int start = cov[0] >= cov[3] && cov[0] >= cov[5] ? 0 : cov[3] >= cov[5] ? 1 : 2;
        const float row[3][3] = {{cov[0], cov[1], cov[2]}, {cov[1], cov[3], cov[4]}, {cov[2], cov[4], cov[5]}};
        ax[0] = row[start][0]; ax[1] = row[start][1]; ax[2] = row[start][2];
        for (int it = 0; it < 6; ++it) {
            const float n0 = row[0][0] * ax[0] + row[0][1] * ax[1] + row[0][2] * ax[2];
            const float n1 = row[1][0] * ax[0] + row[1][1] * ax[1] + row[1][2] * ax[2];
            const float n2 = row[2][0] * ax[0] + row[2][1] * ax[1] + row[2][2] * ax[2];
            const float m = std::max(std::fabs(n0), std::max(std::fabs(n1), std::fabs(n2)));
            if (m < 1e-6f) break;
            ax[0] = n0 / m; ax[1] = n1 / m; ax[2] = n2 / m;
        }
        if (std::fabs(ax[0]) + std::fabs(ax[1]) + std::fabs(ax[2]) < 1e-6f) { ax[0] = ax[1] = ax[2] = 1.0f; }
    }
    float lo = 1e30f, hi = -1e30f;
    int iLo = 0, iHi = 0;
    for (int i = 0; i < 16; ++i) {
        const float d = (rgba[i * 4] - mean[0]) * ax[0] + (rgba[i * 4 + 1] - mean[1]) * ax[1] + (rgba[i * 4 + 2] - mean[2]) * ax[2];
        if (d < lo) { lo = d; iLo = i; }
        if (d > hi) { hi = d; iHi = i; }
    }
    const uint8_t* pHi = rgba + iHi * 4;
    const uint8_t* pLo = rgba + iLo * 4;
    uint32_t c0 = Pack565(Q5(pHi[0]), Q6(pHi[1]), Q5(pHi[2]));
    uint32_t c1 = Pack565(Q5(pLo[0]), Q6(pLo[1]), Q5(pLo[2]));
    uint8_t idx[16], tryIdx[16];
    uint32_t err = Assign(rgba, c0, c1, idx);
    {
        int mn[3] = {255, 255, 255}, mx[3] = {0, 0, 0};
        for (int i = 0; i < 16; ++i) for (int k = 0; k < 3; ++k) { mn[k] = std::min<int>(mn[k], rgba[i * 4 + k]); mx[k] = std::max<int>(mx[k], rgba[i * 4 + k]); }
        float a[3], b[3];
        for (int k = 0; k < 3; ++k) { a[k] = ax[k] >= 0 ? static_cast<float>(mx[k]) : static_cast<float>(mn[k]); b[k] = ax[k] >= 0 ? static_cast<float>(mn[k]) : static_cast<float>(mx[k]); }
        const uint32_t d0 = Pack565(Q5(a[0]), Q6(a[1]), Q5(a[2])), d1 = Pack565(Q5(b[0]), Q6(b[1]), Q5(b[2]));
        const uint32_t e2 = Assign(rgba, d0, d1, tryIdx);
        if (e2 < err) { err = e2; c0 = d0; c1 = d1; std::memcpy(idx, tryIdx, 16); }
    }
    static const float kW0[4] = {1.0f, 0.0f, 2.0f / 3.0f, 1.0f / 3.0f};
    for (int it = 0; it < 2 && err > 0; ++it) {
        float aa = 0, ab = 0, bb = 0, ax0[3] = {0, 0, 0}, bx0[3] = {0, 0, 0};
        for (int i = 0; i < 16; ++i) {
            const float a = kW0[idx[i]], b = 1.0f - a;
            aa += a * a; ab += a * b; bb += b * b;
            for (int k = 0; k < 3; ++k) { ax0[k] += a * rgba[i * 4 + k]; bx0[k] += b * rgba[i * 4 + k]; }
        }
        const float det = aa * bb - ab * ab;
        if (std::fabs(det) < 1e-4f) break;
        const float inv = 1.0f / det;
        float e0[3], e1[3];
        for (int k = 0; k < 3; ++k) {
            e0[k] = (ax0[k] * bb - bx0[k] * ab) * inv;
            e1[k] = (bx0[k] * aa - ax0[k] * ab) * inv;
        }
        const uint32_t d0 = Pack565(Q5(e0[0]), Q6(e0[1]), Q5(e0[2])), d1 = Pack565(Q5(e1[0]), Q6(e1[1]), Q5(e1[2]));
        if (d0 == c0 && d1 == c1) break;
        const uint32_t e2 = Assign(rgba, d0, d1, tryIdx);
        if (e2 >= err) break;
        err = e2; c0 = d0; c1 = d1;
        std::memcpy(idx, tryIdx, 16);
    }
    WriteColour(out, c0, c1, idx);
}

namespace enc {
inline void AlphaPalette(int a0, int a1, int pal[8]) {
    pal[0] = a0; pal[1] = a1;
    if (a0 > a1) for (int i = 1; i < 7; ++i) pal[i + 1] = ((7 - i) * a0 + i * a1) / 7;
    else { for (int i = 1; i < 5; ++i) pal[i + 1] = ((5 - i) * a0 + i * a1) / 5; pal[6] = 0; pal[7] = 255; }
}
inline uint32_t AlphaAssign(const uint8_t* a16, int a0, int a1, uint8_t idx[16]) {
    int pal[8];
    AlphaPalette(a0, a1, pal);
    uint32_t total = 0;
    for (int i = 0; i < 16; ++i) {
        int best = 1 << 30;
        uint8_t bi = 0;
        for (uint8_t e = 0; e < 8; ++e) {
            const int d = std::abs(a16[i] - pal[e]);
            if (d < best) { best = d; bi = e; }
        }
        idx[i] = bi;
        total += static_cast<uint32_t>(best * best);
    }
    return total;
}
}   // namespace enc

// DXT5's alpha half: the 8-value mode (a0 > a1) or the 6-value mode with explicit 0 and 255, whichever is closer. Texels that are 0 or 255 stay exactly 0 or 255 (binary alpha is lossless).
inline void EncodeAlphaBlock(const uint8_t rgba[64], uint8_t out[8]) {
    using namespace enc;
    uint8_t a[16];
    int mn = 255, mx = 0, mnIn = 255, mxIn = 0;   // (In: leaving out 0 and 255)
    bool binary = true;
    for (int i = 0; i < 16; ++i) {
        a[i] = rgba[i * 4 + 3];
        mn = std::min<int>(mn, a[i]); mx = std::max<int>(mx, a[i]);
        if (a[i] != 0 && a[i] != 255) { mnIn = std::min<int>(mnIn, a[i]); mxIn = std::max<int>(mxIn, a[i]); binary = false; }
    }
    uint8_t idx[16], t[16];
    int a0, a1;
    if (mn == mx) { a0 = a1 = mn; std::memset(idx, 0, 16); }
    else if (binary) {
#if defined(VGX_TEXCOMP_BREAK) && VGX_TEXCOMP_BREAK == 6
        a0 = 255; a1 = 0; for (int i = 0; i < 16; ++i) idx[i] = a[i] ? 2 : 1;   // deliberate break: opaque texels read palette entry 2
#else
        a0 = 255; a1 = 0; for (int i = 0; i < 16; ++i) idx[i] = a[i] ? 0 : 1;
#endif
    }   // (exact: palette entries 0 and 1)
    else {
        a0 = mx; a1 = mn;   // 8-value mode
        uint32_t err = AlphaAssign(a, a0, a1, idx);
        if (err) {   // one round of nudging the endpoints by one step (they need not be texels)
            int b0Best = a0, b1Best = a1;
            for (int d0 = -1; d0 <= 1; ++d0)
                for (int d1 = -1; d1 <= 1; ++d1) {
                    const int b0 = mx + d0, b1 = mn + d1;
                    if ((d0 == 0 && d1 == 0) || b0 > 255 || b1 < 0 || b0 <= b1) continue;
                    const uint32_t e = AlphaAssign(a, b0, b1, t);
                    if (e < err) { err = e; b0Best = b0; b1Best = b1; std::memcpy(idx, t, 16); }
                }
            a0 = b0Best; a1 = b1Best;
        }
        if ((mn == 0 || mx == 255) && mnIn <= mxIn) {   // 6-value mode with explicit 0 and 255: better with 0 or 255 beside a narrow middle range
            const uint32_t e = AlphaAssign(a, mnIn, mxIn, t);
            if (e < err) { a0 = mnIn; a1 = mxIn; std::memcpy(idx, t, 16); }
        }
    }
    out[0] = static_cast<uint8_t>(a0);
    out[1] = static_cast<uint8_t>(a1);
    uint64_t bits = 0;
    for (int i = 0; i < 16; ++i) bits |= static_cast<uint64_t>(idx[i] & 7) << (i * 3);
    for (int k = 0; k < 6; ++k) out[2 + k] = static_cast<uint8_t>(bits >> (k * 8));
}

inline void GatherBlock(const uint8_t* rgba, uint32_t w, uint32_t h, uint32_t bx, uint32_t by, uint8_t px[64]) {
    for (uint32_t y = 0; y < 4; ++y)
        for (uint32_t x = 0; x < 4; ++x) {
            const uint32_t sx = std::min(bx * 4 + x, w - 1), sy = std::min(by * 4 + y, h - 1);
            std::memcpy(px + (y * 4 + x) * 4, rgba + (static_cast<size_t>(sy) * w + sx) * 4, 4);
        }
}
inline uint32_t BlockBytes(bool dxt5) { return dxt5 ? 16u : 8u; }
inline size_t LevelBytes(uint32_t w, uint32_t h, bool dxt5) { return static_cast<size_t>((w + 3) / 4) * ((h + 3) / 4) * BlockBytes(dxt5); }
// Whole levels (row-major blocks, as GL_COMPRESSED_RGBA_S3TC_DXT1/5_EXT take them). Edge blocks repeat the last row / column.
inline void EncodeDxt1(const uint8_t* rgba, uint32_t w, uint32_t h, uint8_t* dst) {
    const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    uint8_t px[64];
    for (uint32_t by = 0; by < bh; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx, dst += 8) { GatherBlock(rgba, w, h, bx, by, px); EncodeColorBlock(px, dst); }
}
inline void EncodeDxt5(const uint8_t* rgba, uint32_t w, uint32_t h, uint8_t* dst) {
    const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    uint8_t px[64];
    for (uint32_t by = 0; by < bh; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx, dst += 16) { GatherBlock(rgba, w, h, bx, by, px); EncodeAlphaBlock(px, dst); EncodeColorBlock(px, dst + 8); }
}
// The reference decode (the exact integer formulas of the S3TC spec, colour 2 = (2 c0 + c1) / 3 rounded, as vgx_backend_gl.cpp DecodeDxt1 / DecodeDxt5 do): the quality gate and the tests.
inline void DecodeDxt(const uint8_t* src, uint32_t w, uint32_t h, bool dxt5, uint8_t* rgba) {
    using namespace enc;
    const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    for (uint32_t by = 0; by < bh; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx) {
            const uint8_t* b = src + (static_cast<size_t>(by) * bw + bx) * BlockBytes(dxt5);
            const uint8_t* c = dxt5 ? b + 8 : b;
            const uint32_t c0 = c[0] | (c[1] << 8), c1 = c[2] | (c[3] << 8);
            int pal[4][4];
            Unpack565(c0, pal[0]);
            Unpack565(c1, pal[1]);
            const bool four = dxt5 || c0 > c1;
            for (int k = 0; k < 3; ++k) {
                if (four) { pal[2][k] = (2 * pal[0][k] + pal[1][k] + 1) / 3; pal[3][k] = (pal[0][k] + 2 * pal[1][k] + 1) / 3; }
                else { pal[2][k] = (pal[0][k] + pal[1][k]) / 2; pal[3][k] = 0; }
            }
            pal[0][3] = pal[1][3] = pal[2][3] = 255;
            pal[3][3] = four ? 255 : 0;
            int apal[8] = {0};
            uint64_t abits = 0;
            if (dxt5) {
                AlphaPalette(b[0], b[1], apal);
                for (int k = 0; k < 6; ++k) abits |= static_cast<uint64_t>(b[2 + k]) << (k * 8);
            }
            for (uint32_t y = 0; y < 4; ++y)
                for (uint32_t x = 0; x < 4; ++x) {
                    const uint32_t px = bx * 4 + x, py = by * 4 + y;
                    if (px >= w || py >= h) continue;
                    const int e = (c[4 + y] >> (x * 2)) & 3;
                    uint8_t* o = rgba + (static_cast<size_t>(py) * w + px) * 4;
                    o[0] = static_cast<uint8_t>(pal[e][0]); o[1] = static_cast<uint8_t>(pal[e][1]); o[2] = static_cast<uint8_t>(pal[e][2]);
                    o[3] = dxt5 ? static_cast<uint8_t>(apal[(abits >> ((y * 4 + x) * 3)) & 7]) : static_cast<uint8_t>(pal[e][3]);
                }
        }
}

struct Quality {
    double psnrRgb = 99.0, psnrA = 99.0;   // over the level (99: identical)
    uint32_t maxErrRgb = 0, maxErrA = 0;   // largest error of one channel of one texel
};
inline Quality Measure(const uint8_t* a, const uint8_t* b, size_t texels) {
    Quality q;
    double se = 0, sa = 0;
    for (size_t i = 0; i < texels; ++i) {
        for (int k = 0; k < 3; ++k) {
            const int d = std::abs(static_cast<int>(a[i * 4 + k]) - static_cast<int>(b[i * 4 + k]));
            se += d * d;
            q.maxErrRgb = std::max<uint32_t>(q.maxErrRgb, static_cast<uint32_t>(d));
        }
        const int d = std::abs(static_cast<int>(a[i * 4 + 3]) - static_cast<int>(b[i * 4 + 3]));
        sa += d * d;
        q.maxErrA = std::max<uint32_t>(q.maxErrA, static_cast<uint32_t>(d));
    }
    const auto psnr = [](double mse) { return mse <= 0.0 ? 99.0 : std::min(99.0, 10.0 * std::log10(255.0 * 255.0 / mse)); };
    q.psnrRgb = texels ? psnr(se / (3.0 * static_cast<double>(texels))) : 99.0;
    q.psnrA = texels ? psnr(sa / static_cast<double>(texels)) : 99.0;
    return q;
}
// One block (4x4 texels, 16 RGBA) decoded the way DecodeDxt does (the sliced quality measure uses it; tests/texcomp.cpp compares it with DecodeDxt and an independent decoder).
inline void DecodeBlock(const uint8_t* b, bool dxt5, uint8_t out[64]) {
    uint8_t blk[4 * 4 * 4];
    DecodeDxt(b, 4, 4, dxt5, blk);
    std::memcpy(out, blk, 64);
}
struct QualityAcc {
    double se = 0, sa = 0;
    uint32_t maxRgb = 0, maxA = 0;
    void Add(const uint8_t* a, const uint8_t* b, size_t texels) {
        for (size_t i = 0; i < texels; ++i) {
            for (int k = 0; k < 3; ++k) {
                const int d = std::abs(static_cast<int>(a[i * 4 + k]) - static_cast<int>(b[i * 4 + k]));
                se += d * d;
                maxRgb = std::max<uint32_t>(maxRgb, static_cast<uint32_t>(d));
            }
            const int d = std::abs(static_cast<int>(a[i * 4 + 3]) - static_cast<int>(b[i * 4 + 3]));
            sa += d * d;
            maxA = std::max<uint32_t>(maxA, static_cast<uint32_t>(d));
        }
    }
    Quality Finish(size_t texels) const {
        Quality q;
        q.maxErrRgb = maxRgb; q.maxErrA = maxA;
        const auto psnr = [](double mse) { return mse <= 0.0 ? 99.0 : std::min(99.0, 10.0 * std::log10(255.0 * 255.0 / mse)); };
        q.psnrRgb = texels ? psnr(se / (3.0 * static_cast<double>(texels))) : 99.0;
        q.psnrA = texels ? psnr(sa / static_cast<double>(texels)) : 99.0;
        return q;
    }
};
// 2x2 box filter (the filter of vgx_texdecode.cpp Downsample and of glGenerateMipmap): for the levels of a texture that has none of its own.
inline void Box2(const uint8_t* src, uint32_t w, uint32_t h, std::vector<uint8_t>& dst, uint32_t& dw, uint32_t& dh) {
    dw = w > 1 ? w / 2 : 1;
    dh = h > 1 ? h / 2 : 1;
    dst.resize(static_cast<size_t>(dw) * dh * 4);
    for (uint32_t y = 0; y < dh; ++y)
        for (uint32_t x = 0; x < dw; ++x) {
            const uint32_t x0 = x * 2 < w ? x * 2 : w - 1, x1 = x * 2 + 1 < w ? x * 2 + 1 : w - 1;
            const uint32_t y0 = y * 2 < h ? y * 2 : h - 1, y1 = y * 2 + 1 < h ? y * 2 + 1 : h - 1;
            for (int k = 0; k < 4; ++k) {
                const uint32_t sum = src[(static_cast<size_t>(y0) * w + x0) * 4 + k] + src[(static_cast<size_t>(y0) * w + x1) * 4 + k] +
                                     src[(static_cast<size_t>(y1) * w + x0) * 4 + k] + src[(static_cast<size_t>(y1) * w + x1) * 4 + k];
                dst[(static_cast<size_t>(y) * dw + x) * 4 + k] = static_cast<uint8_t>((sum + 2) >> 2);
            }
        }
}

// ================================================================ policy: what may be converted (pure; every rule has a test and a mutant)
struct Params {
    double psnrRgb = 32.0, psnrA = 36.0;
    uint32_t minTexels = 4096, minSide = 16;
    uint32_t readyPictures = 8;            // 3D-only pictures before the verdict is taken
    uint32_t blocksPerPicture = 600;       // 4x4 blocks encoded a picture (about 1.5-2 ms on the Vita, ESTIMATE)
    uint32_t appliesPerPicture = 2;        // replacements uploaded a picture
    uint64_t holdBytes = 12u << 20;        // level copies waiting for a verdict (first run only)
    uint32_t maxErrRgb = 40;               // largest error of one colour channel of one texel (levels 0 and 1): a smooth ramp or glow that 565 endpoints would band fails here whatever its average says
    uint32_t maxAspect = 4;                // sides in a ratio over this are left alone: their chain would end at 4 texels on the short side and lose the long side's far levels
    uint64_t heapFloor = 24u << 20;        // free C heap that must stay free after a level copy is taken (needs freeHeap)
    uint64_t (*freeHeap)() = nullptr;      // the free C heap in bytes, or null when it is not known
    uint64_t gateTag() const { return static_cast<uint64_t>(psnrRgb * 10.0 + 0.5) * 1000003ull + static_cast<uint64_t>(psnrA * 10.0 + 0.5); }
};
enum Class : uint8_t {
    kClsNone = 0,
    kClsOffered,     // static rules pass: waits for its usage verdict
    kClsFormat,      // not an RGBA8 upload (DXT already, LA88, 16-bit formats, I8)
    kClsSmall,       // under minTexels or a side under minSide
    kClsNpot,        // not a power of two
    kClsShape,       // level sizes do not fit the dimensions (never seen; refuses to guess)
    kClsHd,          // an HD replacement
    kClsDynamic,     // the id came again with other contents
    kClsVetoed,      // the card says it was drawn in 2D / indirect (this run or an earlier one)
    kClsRejected,    // the card says it failed the quality floor
    kClsCap,         // first sight, no payload on the card and the level copies are at holdBytes, or the free heap is under the floor, or the copy failed
    kClsThin,        // sides in a ratio over maxAspect
    kClsRefused,     // its replacement upload failed (no memory, or the texture was gone): left alone for the rest of the run
    kClsCount
};
inline const char* ClassName(int c) {
    static const char* const n[kClsCount] = {"none", "offered", "format", "small", "npot", "shape", "hd", "dynamic", "vetoed", "rejected", "cap", "thin", "refused"};
    return c >= 0 && c < kClsCount ? n[c] : "?";
}
struct StaticIn {
    bool rgba8 = false;
    uint32_t w = 0, h = 0;
    bool hd = false;
    size_t nLevels = 0;
    bool genMips = false;
    size_t level0Bytes = 0;
};
inline bool Pow2(uint32_t v) { return v && !(v & (v - 1)); }
inline Class StaticClass(const StaticIn& s, const Params& p) {
    if (!s.rgba8) return kClsFormat;
    if (s.hd) return kClsHd;
#if defined(VGX_TEXCOMP_BREAK) && VGX_TEXCOMP_BREAK == 3
    if (static_cast<uint64_t>(s.w) * s.h < p.minTexels) return kClsSmall;   // deliberate break: no side rule, no power-of-two rule
#else
    if (s.w < p.minSide || s.h < p.minSide || static_cast<uint64_t>(s.w) * s.h < p.minTexels) return kClsSmall;
    if (!Pow2(s.w) || !Pow2(s.h)) return kClsNpot;
#endif
#if !(defined(VGX_TEXCOMP_BREAK) && VGX_TEXCOMP_BREAK == 16)   // (break 16: any aspect)
    if (std::max(s.w, s.h) > p.maxAspect * std::min(s.w, s.h)) return kClsThin;
#endif
    if (s.nLevels == 0 || s.level0Bytes != static_cast<size_t>(s.w) * s.h * 4 || (s.genMips && s.nLevels != 1)) return kClsShape;
    return kClsOffered;
}
// How a draw used a texture (the back end's marks).
enum : uint8_t { kUse3D = 1, kUse2D = 2, kUseInd = 4 };
// One draw: orthographic projection, the HUD class, a front-end layer, a shader with indirect stages or a Z-texture, or a projection that is not there: not a perspective colour read.
inline uint8_t DrawUse(bool haveProj, bool ortho, bool hudClassOrLayer, bool indirectOrZ) {
    uint8_t u = 0;
    if (indirectOrZ) u |= kUseInd;
#if defined(VGX_TEXCOMP_BREAK) && VGX_TEXCOMP_BREAK == 2
    ortho = false;   // deliberate break: an orthographic draw counts as 3D
#endif
    if (!haveProj || ortho || hudClassOrLayer) u |= kUse2D;
    return u ? u : static_cast<uint8_t>(kUse3D);
}
inline bool OrthoProj(const float* P) {   // column-major mat4: the last row (P[3], P[7], P[11], P[15]) is (0,0,0,1) for an orthographic projection, (0,0,-1,0) for a perspective one
    return std::fabs(P[3]) + std::fabs(P[7]) + std::fabs(P[11]) < 1e-6f && P[15] != 0.0f;
}
// The quality gate (the decoded levels 0 and 1 against their sources; q1 only when there is a level 1). binaryAlpha: every alpha of the source level 0 is 0 or 255.
// RGB: PSNR over the floor AND no channel of any texel off by more than maxErrRgb (an average cannot see a banded ramp). Alpha: binary alpha exact at level 0; smooth alpha PSNR over its floor.
inline bool PassesGate(const Quality& q, bool opaque, bool binaryAlpha, const Params& p, const Quality* q1 = nullptr) {
    if (q.psnrRgb < p.psnrRgb) return false;
#if !(defined(VGX_TEXCOMP_BREAK) && VGX_TEXCOMP_BREAK == 9)   // (break 9: the largest error is not looked at)
    if (q.maxErrRgb > p.maxErrRgb) return false;
#endif
    if (q1) {
#if !(defined(VGX_TEXCOMP_BREAK) && VGX_TEXCOMP_BREAK == 10)   // (break 10: level 1 is not looked at)
        if (q1->psnrRgb < p.psnrRgb || q1->maxErrRgb > p.maxErrRgb) return false;
        if (!opaque && q1->psnrA < p.psnrA - 6.0) return false;   // (level 1's alpha is a box filter of a cut-out's edge: eight steps a block, 6 dB of room)
#endif
    }
    if (opaque) return true;
#if defined(VGX_TEXCOMP_BREAK) && VGX_TEXCOMP_BREAK == 1
    if (binaryAlpha) return true;   // deliberate break: cut-out edges may move
#endif
    if (binaryAlpha) return q.maxErrA == 0;   // cut-outs keep their exact edges
    return q.psnrA >= p.psnrA;
}

// ================================================================ hashes
inline uint64_t Mix64(uint64_t h, uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    h *= 0xFF51AFD7ED558CCDull;
    return h ^ (h >> 32);
}
inline uint64_t HashBytes(const uint8_t* p, size_t n, uint64_t seed) {
    uint64_t h = seed ^ (n * 0x9E3779B97F4A7C15ull);
    size_t i = 0;
    for (; i + 8 <= n; i += 8) { uint64_t v; std::memcpy(&v, p + i, 8); h = Mix64(h, v); }
    uint64_t t = 0;
    for (size_t k = 0; i + k < n; ++k) t |= static_cast<uint64_t>(p[i + k]) << (8 * k);
    return Mix64(h, t);
}
inline uint32_t Hash32(const uint8_t* p, size_t n) { const uint64_t h = HashBytes(p, n, 0x4D535443ull); return static_cast<uint32_t>(h ^ (h >> 32)); }
// The cache key of an upload: contents of every level given, size, level layout, encoder version and gate (a changed floor re-tests every texture).
inline uint64_t ContentKey(const std::vector<std::vector<uint8_t>>& levels, uint32_t w, uint32_t h, bool genMips, const Params& p) {
    uint64_t k = 0x5445584Full;
    for (const auto& l : levels) k = HashBytes(l.data(), l.size(), k);
    k = Mix64(k, (static_cast<uint64_t>(w) << 32) | h);
    k = Mix64(k, (static_cast<uint64_t>(levels.size()) << 1) | (genMips ? 1u : 0u));
    k = Mix64(k, (static_cast<uint64_t>(kEncoderVersion) << 32) | kFileVersion);
    k = Mix64(k, (static_cast<uint64_t>(p.maxErrRgb) << 32) | p.maxAspect);
    return Mix64(k, p.gateTag());
}

// ================================================================ the card cache
enum Verdict : uint8_t { kVNone = 0, kVOk = 1, kVRejected = 2, kVeto = 3 };
struct Entry {
    uint64_t key = 0;
    uint32_t off = 0, size = 0, phash = 0;
    uint16_t w = 0, h = 0;
    uint8_t verdict = kVNone, fmt = kFmtNone, nLevels = 0;
};
inline void Put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i)); }
inline void Put64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i)); }
inline uint32_t Get32(const uint8_t* p) { uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(p[i]) << (8 * i); return v; }
inline uint64_t Get64(const uint8_t* p) { uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i); return v; }
constexpr size_t kHeaderBytes = 16, kRecBytes = 32;
// idx header: "MSTC", file version, encoder version, 0. Record: key(8) off(4) size(4) phash(4) w(2) h(2) verdict fmt nLevels pad (4) crc(4) of the first 28 bytes.
inline void PackRec(const Entry& e, uint8_t out[kRecBytes]) {
    Put64(out, e.key); Put32(out + 8, e.off); Put32(out + 12, e.size); Put32(out + 16, e.phash);
    out[20] = static_cast<uint8_t>(e.w); out[21] = static_cast<uint8_t>(e.w >> 8); out[22] = static_cast<uint8_t>(e.h); out[23] = static_cast<uint8_t>(e.h >> 8);
    out[24] = e.verdict; out[25] = e.fmt; out[26] = e.nLevels; out[27] = 0;
    Put32(out + 28, Hash32(out, 28));
}
inline bool UnpackRec(const uint8_t in[kRecBytes], Entry& e) {
    if (Get32(in + 28) != Hash32(in, 28)) return false;
    e.key = Get64(in); e.off = Get32(in + 8); e.size = Get32(in + 12); e.phash = Get32(in + 16);
    e.w = static_cast<uint16_t>(in[20] | (in[21] << 8)); e.h = static_cast<uint16_t>(in[22] | (in[23] << 8));
    e.verdict = in[24]; e.fmt = in[25]; e.nLevels = in[26];
    return e.verdict >= kVOk && e.verdict <= kVeto;
}

class CardCache {
public:
    struct Stats { uint64_t records = 0, badRecords = 0, appended = 0, appendFail = 0, readFail = 0, capped = 0, bytes = 0, tornBytes = 0; bool files = false, versionReset = false, broken = false, realigned = false; };
    // stem empty or the files cannot be made: a memory-only cache (payloads in RAM, gone with the run). capBytes: payloads stop being added once the .dat file is that big (verdicts still are).
    bool Open(const std::string& stem, uint64_t capBytes) {
        Close();
        cap_ = capBytes;
        if (stem.empty()) return true;
        idxPath_ = stem + ".idx";
        datPath_ = stem + ".dat";
        std::vector<uint8_t> idx;
        if (FILE* f = std::fopen(idxPath_.c_str(), "rb")) {
            uint8_t buf[4096];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) idx.insert(idx.end(), buf, buf + n);
            std::fclose(f);
        }
        uint64_t datSize = 0;
        if (FILE* f = std::fopen(datPath_.c_str(), "rb")) {
            std::fseek(f, 0, SEEK_END);
            const long sz = std::ftell(f);
            if (sz > 0) datSize = static_cast<uint64_t>(sz);
            std::fclose(f);
        }
#if defined(VGX_TEXCOMP_BREAK) && VGX_TEXCOMP_BREAK == 8
        bool fresh = idx.size() < kHeaderBytes || std::memcmp(idx.data(), "MSTC", 4) != 0;   // deliberate break: another version's records are trusted
#else
        bool fresh = idx.size() < kHeaderBytes || std::memcmp(idx.data(), "MSTC", 4) != 0 || Get32(idx.data() + 4) != kFileVersion || Get32(idx.data() + 8) != kEncoderVersion;
#endif
        if (fresh && !idx.empty()) st_.versionReset = true;
        if (!fresh) {
            const size_t whole = (idx.size() - kHeaderBytes) / kRecBytes, tail = (idx.size() - kHeaderBytes) % kRecBytes;
            for (size_t k = 0; k < whole; ++k) {
                Entry e;
                const uint8_t* rec = idx.data() + kHeaderBytes + k * kRecBytes;
                if (!UnpackRec(rec, e)) { ++st_.badRecords; continue; }
                if (e.verdict == kVOk && (static_cast<uint64_t>(e.off) + e.size > datSize || e.size == 0)) { ++st_.badRecords; continue; }
                Merge(e);
            }
            dat_ = datSize;
            st_.bytes = datSize;
            if (tail) {   // a torn last record (power-off, kill, full card): cut it off, else every record appended after it is misaligned and unreadable at the next start
                st_.tornBytes = tail;
#if !(defined(VGX_TEXCOMP_BREAK) && VGX_TEXCOMP_BREAK == 11)   // (break 11: the tail stays)
                FILE* f = std::fopen(idxPath_.c_str(), "wb");
                bool ok = f && std::fwrite(idx.data(), 1, kHeaderBytes + whole * kRecBytes, f) == kHeaderBytes + whole * kRecBytes;
                if (f && std::fclose(f) != 0) ok = false;
                if (ok) st_.realigned = true; else broken_ = true;
#endif
            }
        }
        if (fresh) {   // a new card file, or another version's: start over (the old files are replaced)
            uint8_t h[kHeaderBytes] = {'M', 'S', 'T', 'C'};
            Put32(h + 4, kFileVersion); Put32(h + 8, kEncoderVersion);
            FILE* f = std::fopen(idxPath_.c_str(), "wb");
            if (!f) { idxPath_.clear(); datPath_.clear(); return true; }   // (no card folder: memory only)
            bool ok = std::fwrite(h, 1, kHeaderBytes, f) == kHeaderBytes;
            if (std::fclose(f) != 0) ok = false;
            FILE* d = std::fopen(datPath_.c_str(), "wb");
            bool dok = d != nullptr;
            if (d && std::fclose(d) != 0) dok = false;
            if (!ok || !dok) { idxPath_.clear(); datPath_.clear(); return true; }
            dat_ = 0;
            st_.bytes = 0;
        }
        st_.files = true;
        st_.broken = broken_;
        return true;
    }
    void Close() { map_.clear(); mem_.clear(); idxPath_.clear(); datPath_.clear(); st_ = Stats{}; dat_ = 0; broken_ = false; memBytes_ = 0; }
    Verdict Find(uint64_t key, Entry* out = nullptr) {
        const auto it = map_.find(key);
        if (it == map_.end()) return kVNone;
        if (out) *out = it->second;
        return static_cast<Verdict>(it->second.verdict);
    }
    // The payload of an OK entry: size and hash checked (a failed check removes the entry: it will be learned again).
    bool Read(const Entry& e, std::vector<uint8_t>& out) {
        out.assign(e.size, 0);
        if (idxPath_.empty()) {
            const auto m = mem_.find(e.key);
            if (m == mem_.end() || m->second.size() != e.size) { ++st_.readFail; map_.erase(e.key); return false; }
            out = m->second;
        } else {
            FILE* f = std::fopen(datPath_.c_str(), "rb");
            bool ok = f && std::fseek(f, static_cast<long>(e.off), SEEK_SET) == 0 && std::fread(out.data(), 1, e.size, f) == e.size;
            if (f) std::fclose(f);
            if (!ok) { ++st_.readFail; map_.erase(e.key); return false; }
        }
#if defined(VGX_TEXCOMP_BREAK) && VGX_TEXCOMP_BREAK == 4
        const bool hashOk = true;   // deliberate break: a damaged payload is used
#else
        const bool hashOk = Hash32(out.data(), out.size()) == e.phash;
#endif
        if (!hashOk) { ++st_.readFail; map_.erase(e.key); return false; }
        return true;
    }
    bool AppendOk(uint64_t key, uint16_t w, uint16_t h, uint8_t fmt, uint8_t nLevels, const std::vector<uint8_t>& payload) {
        if (broken_) return false;
        const uint64_t used = idxPath_.empty() ? memBytes_ : dat_;
        if (payload.empty() || used + payload.size() > cap_) { ++st_.capped; return false; }
        Entry e;
        e.key = key; e.size = static_cast<uint32_t>(payload.size()); e.phash = Hash32(payload.data(), payload.size()); e.w = w; e.h = h; e.verdict = kVOk; e.fmt = fmt; e.nLevels = nLevels;
        if (idxPath_.empty()) { e.off = 0; mem_[key] = payload; memBytes_ += payload.size(); }
        else {
            FILE* d = std::fopen(datPath_.c_str(), "ab");
            bool ok = d && std::fwrite(payload.data(), 1, payload.size(), d) == payload.size();
            if (d && std::fclose(d) != 0) ok = false;   // (a full card can fail here: the payload is on the card before the record that points at it, or it is not a record)
            if (!ok) { ++st_.appendFail; broken_ = st_.broken = true; return false; }
            e.off = static_cast<uint32_t>(dat_);
            dat_ += payload.size();
            st_.bytes = dat_;
        }
        if (!WriteRec(e)) return false;
        Merge(e);
        return true;
    }
    bool AppendVerdict(uint64_t key, Verdict v) {
        if (broken_) return false;
        Entry e;
        e.key = key; e.verdict = v;
        if (!WriteRec(e)) return false;
        Merge(e);
        return true;
    }
    const Stats& stats() const { return st_; }
    size_t size() const { return map_.size(); }
    bool OnCard() const { return !idxPath_.empty(); }
    bool Broken() const { return broken_; }
private:
    // A veto is for good: nothing later replaces it. Otherwise the newest record wins.
    void Merge(const Entry& e) {
        auto it = map_.find(e.key);
#if !(defined(VGX_TEXCOMP_BREAK) && VGX_TEXCOMP_BREAK == 5)   // (break 5: a later record replaces a veto)
        if (it != map_.end() && it->second.verdict == kVeto) return;
#endif
        map_[e.key] = e;
        ++st_.records;
    }
    bool WriteRec(const Entry& e) {
        if (idxPath_.empty()) { ++st_.appended; return true; }
        uint8_t rec[kRecBytes];
        PackRec(e, rec);
        FILE* f = std::fopen(idxPath_.c_str(), "ab");
        bool ok = f && std::fwrite(rec, 1, kRecBytes, f) == kRecBytes;
        if (f && std::fclose(f) != 0) ok = false;
        if (!ok) { ++st_.appendFail; broken_ = st_.broken = true; return false; }   // (after one failed write nothing more is written this run: a half record would misalign the rest)
        ++st_.appended;
        return true;
    }
    std::unordered_map<uint64_t, Entry> map_;
    std::map<uint64_t, std::vector<uint8_t>> mem_;
    std::string idxPath_, datPath_;
    uint64_t cap_ = 0, dat_ = 0, memBytes_ = 0;
    bool broken_ = false;
    Stats st_;
};

// ================================================================ a sliced conversion
// Levels to encode: the authored levels while both sides stay >= 4 (as many as the game gave), or, for a texture the back end would have mip-mapped itself (genMips), a box chain down to 4x4. (Static rules keep
// the sides within maxAspect of each other, so that "both sides >= 4" loses only levels of a few texels: a long thin texture would lose its far levels and is not offered.)
// Every step is bounded: encoding is one block each, the box levels and the alpha scan are charged by their texels, and the quality measure (levels 0 and 1) decodes one block each.
// A unit may overrun a small budget once, but the first unit of a call always runs (progress).
struct Job {
    uint16_t w = 0, h = 0;
    bool opaque = true, binaryAlpha = true, haveQ1 = false;
    std::vector<std::vector<uint8_t>> src;   // RGBA of each level to encode (the caller's copy, moved in)
    std::vector<std::vector<uint8_t>> out;   // DXT of each level
    Quality q, q1;                           // levels 0 and 1 against their sources
    bool done = false;
    // `levels` is moved from. genMips: only levels[0] is used and the chain is made here.
    bool Begin(uint16_t w0, uint16_t h0, std::vector<std::vector<uint8_t>>&& levels, bool genMips) {
        w = w0; h = h0; src.clear(); out.clear(); done = false; phase_ = kPrep; level_ = 0; block_ = 0; haveQ1 = false; scanned_ = false; genMips_ = genMips; mLevel_ = 0; mBlock_ = 0; acc_ = QualityAcc{};
        if (levels.empty() || w < 4 || h < 4) return false;
        uint32_t lw = w, lh = h;
        if (genMips) {
            if (levels[0].size() != static_cast<size_t>(w) * h * 4) return false;
            src.push_back(std::move(levels[0]));
        } else {
            for (size_t i = 0; i < levels.size() && lw >= 4 && lh >= 4; ++i) {
                if (levels[i].size() != static_cast<size_t>(lw) * lh * 4) return false;
                src.push_back(std::move(levels[i]));
                lw = lw > 1 ? lw / 2 : 1; lh = lh > 1 ? lh / 2 : 1;
            }
        }
        return !src.empty();
    }
    uint32_t TotalBlocks() const {   // (encode blocks of the levels there are; with genMips, of the chain that will be made)
        uint32_t n = 0, lw = w, lh = h;
        const size_t levels = genMips_ ? ChainLevels(w, h) : src.size();
        for (size_t i = 0; i < levels; ++i) { n += ((lw + 3) / 4) * ((lh + 3) / 4); lw = lw > 1 ? lw / 2 : 1; lh = lh > 1 ? lh / 2 : 1; }
        return n;
    }
    static size_t ChainLevels(uint32_t w, uint32_t h) { size_t n = 1; while (w / 2 >= 4 && h / 2 >= 4) { w /= 2; h /= 2; ++n; } return n; }
    // Does up to maxBlocks blocks of work; returns the blocks charged. After the last, `done` is set and q / q1 hold the quality.
    uint32_t Step(uint32_t maxBlocks) {
        uint32_t did = 0;
        auto room = [&](uint32_t cost) { return did == 0 || did + cost <= maxBlocks; };
        while (!done && did < maxBlocks) {
            if (phase_ == kPrep) {
                uint32_t lw = w, lh = h;
                for (size_t i = 1; i < src.size(); ++i) { lw = std::max<uint32_t>(1, lw / 2); lh = std::max<uint32_t>(1, lh / 2); }
                if (genMips_ && lw / 2 >= 4 && lh / 2 >= 4) {   // one more box level
                    const uint32_t cost = std::max<uint32_t>(1, lw * lh / 32);
                    if (!room(cost)) break;
                    std::vector<uint8_t> nx; uint32_t nw, nh;
                    Box2(src.back().data(), lw, lh, nx, nw, nh);
                    src.push_back(std::move(nx));
                    did += cost;
                    continue;
                }
                if (!scanned_) {
                    const uint32_t cost = std::max<uint32_t>(1, w * h / 64);
                    if (!room(cost)) break;
                    opaque = true;
                    for (const auto& l : src) for (size_t i = 3; i < l.size() && opaque; i += 4) opaque = l[i] == 255;
                    binaryAlpha = true;
                    if (!opaque) for (size_t i = 3; i < src[0].size() && binaryAlpha; i += 4) binaryAlpha = src[0][i] == 0 || src[0][i] == 255;
                    uint32_t ow = w, oh = h;
                    for (size_t i = 0; i < src.size(); ++i) { out.emplace_back(LevelBytes(ow, oh, !opaque)); ow = ow > 1 ? ow / 2 : 1; oh = oh > 1 ? oh / 2 : 1; }
                    scanned_ = true;
                    did += cost;
                    continue;
                }
                phase_ = kEncode;
            } else if (phase_ == kEncode) {
                const uint32_t lw = std::max<uint32_t>(1, w >> level_), lh = std::max<uint32_t>(1, h >> level_);
                const uint32_t bw = (lw + 3) / 4, bh = (lh + 3) / 4, nb = bw * bh;
                uint8_t px[64];
                while (block_ < nb && did < maxBlocks) {
                    GatherBlock(src[level_].data(), lw, lh, block_ % bw, block_ / bw, px);
                    uint8_t* dst = out[level_].data() + static_cast<size_t>(block_) * BlockBytes(!opaque);
                    if (opaque) EncodeColorBlock(px, dst);
                    else { EncodeAlphaBlock(px, dst); EncodeColorBlock(px, dst + 8); }
                    ++block_; ++did;
                }
                if (block_ == nb) { ++level_; block_ = 0; if (level_ == src.size()) phase_ = kMeasure; }
            } else {   // kMeasure: decode the blocks of levels 0 and 1 and compare them with their sources
                const size_t nm = std::min<size_t>(2, src.size());
                const uint32_t lw = std::max<uint32_t>(1, w >> mLevel_), lh = std::max<uint32_t>(1, h >> mLevel_);
                const uint32_t bw = (lw + 3) / 4, bh = (lh + 3) / 4, nb = bw * bh;
                while (mBlock_ < nb && did < maxBlocks) {
                    uint8_t dec[64], ref[64];
                    DecodeBlock(out[mLevel_].data() + static_cast<size_t>(mBlock_) * BlockBytes(!opaque), !opaque, dec);
                    GatherBlock(src[mLevel_].data(), lw, lh, mBlock_ % bw, mBlock_ / bw, ref);
                    acc_.Add(ref, dec, 16);
                    ++mBlock_; ++did;
                }
                if (mBlock_ == nb) {
                    const Quality qq = acc_.Finish(static_cast<size_t>(lw) * lh);
                    if (mLevel_ == 0) q = qq; else { q1 = qq; haveQ1 = true; }
                    acc_ = QualityAcc{};
                    mBlock_ = 0;
                    if (++mLevel_ >= nm) { done = true; }
                }
            }
        }
        return did;
    }
private:
    enum Phase : uint8_t { kPrep, kEncode, kMeasure };
    Phase phase_ = kPrep;
    size_t level_ = 0, mLevel_ = 0;
    uint32_t block_ = 0, mBlock_ = 0;
    bool scanned_ = false, genMips_ = false;
    QualityAcc acc_;
};
// MSC_VGX_TEX_COMPRESS=ceiling: the whole texture as one 4x4 block of its average colour (DXT1, or DXT5 with the average alpha): the picture is WRONG ON PURPOSE. avg = the average RGBA taken at upload.
inline void CeilingBlock(const uint8_t avg[4], bool opaque, std::vector<uint8_t>& out) {
    uint8_t px[64];
    for (int i = 0; i < 16; ++i) std::memcpy(px + i * 4, avg, 4);
    out.assign(BlockBytes(!opaque), 0);
    if (opaque) EncodeColorBlock(px, out.data());
    else { EncodeAlphaBlock(px, out.data()); EncodeColorBlock(px, out.data() + 8); }
}
inline void AverageRgba(const std::vector<uint8_t>& level0, uint8_t avg[4], bool& opaque) {
    uint64_t s[4] = {0, 0, 0, 0};
    const size_t n = level0.size() / 4;
    opaque = true;
    for (size_t i = 0; i < n; ++i) { for (int c = 0; c < 4; ++c) s[c] += level0[i * 4 + static_cast<size_t>(c)]; if (level0[i * 4 + 3] != 255) opaque = false; }
    for (int c = 0; c < 4; ++c) avg[c] = n ? static_cast<uint8_t>(s[c] / n) : 0;
}

// ================================================================ the manager (what the back end calls)
struct Applied {   // a finished texture, to be uploaded under `texId` as DXT (levels level 0 first, each tightly packed blocks)
    uint32_t texId = 0;
    uint16_t w = 0, h = 0;      // level 0's size (a ceiling block: 4 x 4)
    uint8_t format = kFmtNone;
    std::vector<std::vector<uint8_t>> levels;
    bool ceiling = false;
    uint32_t oldBytes = 0;      // what the RGBA8 texture held (all its levels)
};
struct Counters {
    uint64_t uploads = 0, cls[kClsCount] = {}, clsBytes[kClsCount] = {};
    uint64_t waiting = 0, applied = 0, appliedFromCard = 0, encoded = 0, rejected = 0, late2D = 0, vetoedNow = 0, readFail = 0, applyFail = 0, blocks = 0, copyFail = 0, vetoWrites = 0;
    uint64_t bytesBefore = 0, bytesAfter = 0, heldBytes = 0, ceilings = 0;
};
class Manager {
public:
    // card: not owned. ceiling: the diagnostic (no card, no gate, no copies: the average colour is taken at upload).
    void Configure(const Params& p, CardCache* card, bool ceiling) {
        P = p; card_ = card; ceiling_ = ceiling; recs_.clear(); order_.clear(); held_ = 0; C = Counters{}; job_.reset(); jobId_ = 0; pendingVetoes_.clear(); vetoedKeys_.clear();
    }
    const Params& params() const { return P; }
    const Counters& counters() const { return C; }
    // A texture upload as the game gave it (not a replacement of ours). levels: as TexUpload::levels.
    Class OnUpload(uint32_t texId, uint16_t w, uint16_t h, bool rgba8, bool hd, bool genMips, const std::vector<std::vector<uint8_t>>& levels) {
        ++C.uploads;
        StaticIn s;
        s.rgba8 = rgba8; s.w = w; s.h = h; s.hd = hd; s.nLevels = levels.size(); s.genMips = genMips; s.level0Bytes = levels.empty() ? 0 : levels[0].size();
        size_t allBytes = 0;
        for (const auto& l : levels) allBytes += l.size();
        const Class c = StaticClass(s, P);
        auto it = recs_.find(texId);
        if (c != kClsOffered) { if (it != recs_.end()) Drop(it); Count(c, allBytes); return c; }
        if (it != recs_.end() && it->second.ban) { const Class bc = static_cast<Class>(it->second.banCls); Count(bc, allBytes); return bc; }   // (no hashing of a texture that is never converted)
        const uint64_t key = ContentKey(levels, w, h, genMips, P);
        if (it != recs_.end()) {
            Rec& r = it->second;
#if !(defined(VGX_TEXCOMP_BREAK) && VGX_TEXCOMP_BREAK == 13)   // (break 13: the job stays)
            if (jobId_ == texId) { job_.reset(); jobId_ = 0; }   // (a job on this id works on contents that are gone or are about to come again)
#endif
            if (r.key != key) {   // the same id with other contents: animated in memory, never
                ReleaseCopy(r);
                r.ban = true; r.banCls = kClsDynamic; r.key = key; r.state = kIdle;
                Count(kClsDynamic, allBytes);
                return kClsDynamic;
            }
            // same contents again (the texture cache gave it up and the game sent it again): back to waiting, the usage marks stay
            ReleaseCopy(r);
            if (r.use & (kUse2D | kUseInd)) { r.state = kVetoState; r.cls = kClsVetoed; Count(kClsVetoed, allBytes); return kClsVetoed; }
            r.state = kWait; r.applied = false;
            if (!Resolve(r, levels, allBytes)) { Count(static_cast<Class>(r.cls), allBytes); return static_cast<Class>(r.cls); }
            Count(kClsOffered, allBytes);
            return kClsOffered;
        }
        Rec r;
        r.id = texId; r.key = key; r.w = w; r.h = h; r.genMips = genMips; r.oldBytes = static_cast<uint32_t>(allBytes); r.state = kWait;
        auto ins = recs_.emplace(texId, std::move(r)).first;
        order_.push_back(texId);
        if (!Resolve(ins->second, levels, allBytes)) { Count(static_cast<Class>(ins->second.cls), allBytes); return static_cast<Class>(ins->second.cls); }
        Count(kClsOffered, allBytes);
        return kClsOffered;
    }
    // The back end saw a draw sample texture texId (use = DrawUse): frame = the picture's number. Nothing is written here: a veto goes to the card at the end of the picture.
    void NoteUse(uint32_t texId, uint8_t use, uint64_t frame) {
        auto it = recs_.find(texId);
        if (it == recs_.end()) return;
        Rec& r = it->second;
        if (use & (kUse2D | kUseInd)) {
            r.use |= use;
            if (r.state == kVetoState) return;
            if (r.applied) ++C.late2D;   // replaced already: it stays DXT for this run, the card makes sure the next run never does it
            if (r.state != kIdle && !ceiling_) {
                pendingVetoes_.push_back(r.key); vetoedKeys_.insert(r.key);
#if defined(VGX_TEXCOMP_BREAK) && VGX_TEXCOMP_BREAK == 18
                FlushVetoes();   // deliberate break: the card is written from inside a draw
#endif
            }
            if (r.state != kIdle) ++C.vetoedNow;
#if !(defined(VGX_TEXCOMP_BREAK) && VGX_TEXCOMP_BREAK == 14)   // (break 14: the job stays)
            if (jobId_ == texId) { job_.reset(); jobId_ = 0; }
#endif
            ReleaseCopy(r);
            r.state = kVetoState; r.cls = kClsVetoed;
            return;
        }
        if (r.lastFrame != frame || r.pics == 0) { r.lastFrame = frame; if (r.pics < 0xFFFF) ++r.pics; }
    }
    void Forget(uint32_t texId) {   // freed, or become a render target
        auto it = recs_.find(texId);
        if (it != recs_.end()) Drop(it);
    }
    // At the end of a picture: write the queued vetoes, convert / replace what is ready, within the picture's budget. apply(Applied&) uploads one; true = the texture is there as DXT; false = it is not
    // (the upload was refused, or the texture is gone): it is left alone for the rest of the run (no retry) and counted.
    template <class Fn>
    void Frame(Fn&& apply) {
        FlushVetoes();
        uint32_t blocks = P.blocksPerPicture, applies = P.appliesPerPicture;
        C.waiting = 0;
        for (size_t k = 0; k < order_.size(); ++k) {
            auto it = recs_.find(order_[k]);
            if (it == recs_.end()) { order_[k] = order_.back(); order_.pop_back(); --k; continue; }
            Rec& r = it->second;
            if (r.state != kWait && r.state != kJob) continue;
            ++C.waiting;
#if defined(VGX_TEXCOMP_BREAK) && VGX_TEXCOMP_BREAK == 7
            if ((r.use & (kUse2D | kUseInd)) || applies == 0) continue;   // deliberate break: no wait for a 3D verdict
#else
            if (r.pics < P.readyPictures || (r.use & (kUse2D | kUseInd)) || applies == 0) continue;
#endif
            if (ceiling_) {
                Applied a;
                a.texId = r.id; a.w = 4; a.h = 4; a.ceiling = true; a.oldBytes = r.oldBytes;
                a.format = r.opaque ? kFmtDxt1 : kFmtDxt5;
                a.levels.resize(1);
                CeilingBlock(r.avg, r.opaque, a.levels[0]);
                const size_t nb = a.levels[0].size();
                Finish(r, apply(a), nb, true);
                --applies;
                continue;
            }
            Entry e;
            const Verdict v = card_ ? card_->Find(r.key, &e) : kVNone;
            if (v == kVOk) {
                std::vector<uint8_t> payload;
                if (!card_->Read(e, payload)) { ++C.readFail; r.state = kIdle; ReleaseCopy(r); continue; }   // (learned again when the game sends it next)
                Applied a;
                if (!Split(e, payload, a)) { ++C.readFail; r.state = kIdle; continue; }
                a.texId = r.id; a.oldBytes = r.oldBytes;
                const bool ok = apply(a);
                Finish(r, ok, payload.size(), false);
                if (ok) ++C.appliedFromCard;
                --applies;
                continue;
            }
            if (v == kVRejected || v == kVeto) { ReleaseCopy(r); r.state = kDone; r.cls = v == kVeto ? kClsVetoed : kClsRejected; continue; }
            if (blocks == 0 || (r.state != kJob && r.src.empty())) continue;   // (a record on its job has handed its copy to the job)
            if (!job_) {
                job_.reset(new Job);
                jobId_ = r.id;
                if (!job_->Begin(r.w, r.h, std::move(r.src), r.genMips)) { job_.reset(); jobId_ = 0; ReleaseCopy(r); r.state = kDone; r.cls = kClsRejected; continue; }
                r.src.clear();
                r.state = kJob;
            }
            if (jobId_ != r.id) continue;   // one job at a time
            const uint32_t did = job_->Step(blocks);
            C.blocks += did;
            blocks = blocks > did ? blocks - did : 0;
            if (!job_->done) continue;
            ++C.encoded;
            const bool pass = PassesGate(job_->q, job_->opaque, job_->binaryAlpha, P, job_->haveQ1 ? &job_->q1 : nullptr);
            if (!pass) {
                ++C.rejected;
                if (card_) card_->AppendVerdict(r.key, kVRejected);
                ReleaseCopy(r); r.state = kDone; r.cls = kClsRejected;
                job_.reset(); jobId_ = 0;
                continue;
            }
            Applied a;
            a.texId = r.id; a.w = r.w; a.h = r.h; a.format = job_->opaque ? kFmtDxt1 : kFmtDxt5; a.oldBytes = r.oldBytes;
            a.levels = std::move(job_->out);
            std::vector<uint8_t> payload;
            for (const auto& l : a.levels) payload.insert(payload.end(), l.begin(), l.end());
            if (card_) card_->AppendOk(r.key, r.w, r.h, a.format, static_cast<uint8_t>(a.levels.size()), payload);
            job_.reset(); jobId_ = 0;
            Finish(r, apply(a), payload.size(), false);
            --applies;
        }
    }
    uint64_t HeldBytes() const { return held_; }
    bool JobActive() const { return static_cast<bool>(job_); }
    void StateCounts(uint64_t& wait, uint64_t& done, uint64_t& vetoed, uint64_t& idle) const {
        wait = done = vetoed = idle = 0;
        for (const auto& kv : recs_) { const Rec& r = kv.second; if (r.state == kWait || r.state == kJob) ++wait; else if (r.state == kDone) ++done; else if (r.state == kVetoState) ++vetoed; else ++idle; }
    }
    Counters C;
private:
    enum State : uint8_t { kIdle = 0, kWait, kJob, kDone, kVetoState };
    struct Rec {
        uint32_t id = 0;
        uint64_t key = 0, lastFrame = 0;
        uint16_t w = 0, h = 0, pics = 0;
        bool genMips = false, ban = false, applied = false, opaque = true;
        uint8_t use = 0, state = kIdle, cls = kClsOffered, banCls = kClsDynamic, avg[4] = {0, 0, 0, 0};
        uint32_t oldBytes = 0, heldBytes = 0;
        std::vector<std::vector<uint8_t>> src;
    };
    typedef std::unordered_map<uint32_t, Rec>::iterator It;
    void Count(Class c, size_t bytes) { ++C.cls[c]; C.clsBytes[c] += bytes; }
    void ReleaseCopy(Rec& r) { held_ = held_ >= r.heldBytes ? held_ - r.heldBytes : 0; r.heldBytes = 0; C.heldBytes = held_; r.src.clear(); r.src.shrink_to_fit(); }
    void Drop(It it) { Rec& r = it->second; if (jobId_ == r.id) { job_.reset(); jobId_ = 0; } ReleaseCopy(r); recs_.erase(it); }
    void FlushVetoes() {
        if (pendingVetoes_.empty()) return;
        if (card_) for (uint64_t k : pendingVetoes_) { if (card_->AppendVerdict(k, kVeto)) ++C.vetoWrites; }
        pendingVetoes_.clear();
    }
    // The result of an upload attempt. ok false: the texture is not there as DXT (refused, or gone): no retry this run.
    void Finish(Rec& r, bool ok, size_t newBytes, bool ceiling) {
        ReleaseCopy(r);
        r.state = kDone;
        r.applied = ok;
        if (ok) { ++C.applied; if (ceiling) ++C.ceilings; C.bytesBefore += r.oldBytes; C.bytesAfter += newBytes; }
        else {
#if !(defined(VGX_TEXCOMP_BREAK) && VGX_TEXCOMP_BREAK == 12)   // (break 12: a failed replacement is tried again with the next upload)
            r.ban = true; r.banCls = kClsRefused;
#endif
            ++C.applyFail;
        }
    }
    // The card's verdict for the key and the copy of the levels when it has none. false: nothing to wait for (the class is in r.cls).
    bool Resolve(Rec& r, const std::vector<std::vector<uint8_t>>& levels, size_t allBytes) {
#if !(defined(VGX_TEXCOMP_BREAK) && VGX_TEXCOMP_BREAK == 15)   // (break 15: the ceiling holds copies like the real run)
        if (ceiling_) {   // no copy: the average and the alpha flag are all it needs
            AverageRgba(levels[0], r.avg, r.opaque);
            return true;
        }
#endif
        Entry e;
        const Verdict v = card_ ? card_->Find(r.key, &e) : kVNone;
        if (v == kVeto || vetoedKeys_.count(r.key)) { r.state = kVetoState; r.cls = kClsVetoed; return false; }
        if (v == kVRejected) { r.state = kDone; r.cls = kClsRejected; return false; }
        if (v == kVOk) return true;   // payload on the card: nothing to hold
        #if defined(VGX_TEXCOMP_BREAK) && VGX_TEXCOMP_BREAK == 17
        const bool room = held_ + allBytes <= P.holdBytes;   // deliberate break: the free heap is not looked at
#else
        const bool room = held_ + allBytes <= P.holdBytes && !(P.freeHeap && P.freeHeap() < P.heapFloor + allBytes);
#endif
        if (room) {
            try { r.src = levels; }
            catch (...) { r.src.clear(); r.src.shrink_to_fit(); ++C.copyFail; r.state = kIdle; r.cls = kClsCap; return false; }
            r.heldBytes = static_cast<uint32_t>(allBytes);
            held_ += allBytes;
            C.heldBytes = held_;
            return true;
        }
        r.state = kIdle; r.cls = kClsCap;   // (left for the byte cap or the heap floor: the next upload of the same contents tries again)
        return false;
    }
    static bool Split(const Entry& e, const std::vector<uint8_t>& payload, Applied& a) {
        if (e.fmt != kFmtDxt1 && e.fmt != kFmtDxt5) return false;
        a.format = e.fmt; a.w = e.w; a.h = e.h;
        size_t off = 0;
        uint32_t lw = e.w, lh = e.h;
        for (uint8_t i = 0; i < e.nLevels; ++i) {
            const size_t n = LevelBytes(lw, lh, e.fmt == kFmtDxt5);
            if (off + n > payload.size()) return false;
            a.levels.emplace_back(payload.begin() + static_cast<std::ptrdiff_t>(off), payload.begin() + static_cast<std::ptrdiff_t>(off + n));
            off += n;
            lw = lw > 1 ? lw / 2 : 1; lh = lh > 1 ? lh / 2 : 1;
        }
        return off == payload.size() && !a.levels.empty();
    }
    Params P;
    CardCache* card_ = nullptr;
    bool ceiling_ = false;
    std::unordered_map<uint32_t, Rec> recs_;
    std::vector<uint32_t> order_;
    std::vector<uint64_t> pendingVetoes_;
    std::unordered_set<uint64_t> vetoedKeys_;
    uint64_t held_ = 0;
    std::unique_ptr<Job> job_;
    uint32_t jobId_ = 0;
};

}   // namespace tc
}   // namespace vgx
