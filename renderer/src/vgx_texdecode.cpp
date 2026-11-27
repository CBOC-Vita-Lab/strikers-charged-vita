#include "vgx_texdecode.h"
#include "vgx_cmd.h"
#include "vgx_common.h"
#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && !defined(VGX_NO_NEON)
#include <arm_neon.h>
#define VGX_TEXDEC_NEON 1
#endif

namespace vgx {

namespace {
struct TileInfo { uint32_t tw, th, bytes, sw, sh; }; // tile width/height in texels, bytes per tile, and the two log2 (cpu60-25: the level size shifts instead of dividing; the console's A9 has no integer divide)
TileInfo Tile(uint32_t fmt) {
    switch (fmt) {
    case 0x0: case 0x8: return {8, 8, 32, 3, 3};  // I4, C4
    case 0x1: case 0x2: case 0x9: return {8, 4, 32, 3, 2}; // I8, IA4, C8
    case 0x3: case 0x4: case 0x5: case 0xA: return {4, 4, 32, 2, 2}; // IA8, RGB565, RGB5A3, C14X2
    case 0x6: return {4, 4, 64, 2, 2};  // RGBA8
    case 0xE: return {8, 8, 32, 3, 3};  // CMPR
    default: return {4, 4, 32, 2, 2};
    }
}
inline void Put(uint8_t* d, uint32_t w, uint32_t h, uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (x >= w || y >= h) return;
    uint8_t* p = d + (static_cast<size_t>(y) * w + x) * 4;
    p[0] = r; p[1] = g; p[2] = b; p[3] = a;
}
inline void Dec565(uint16_t v, uint8_t* o) {
    const int r = (v >> 11) & 0x1F, g = (v >> 5) & 0x3F, b = v & 0x1F;
    o[0] = static_cast<uint8_t>((r << 3) | (r >> 2)); o[1] = static_cast<uint8_t>((g << 2) | (g >> 4));
    o[2] = static_cast<uint8_t>((b << 3) | (b >> 2)); o[3] = 255;
}
inline void Dec5A3(uint16_t v, uint8_t* o) {
    if (v & 0x8000) {
        const int r = (v >> 10) & 0x1F, g = (v >> 5) & 0x1F, b = v & 0x1F;
        o[0] = static_cast<uint8_t>((r << 3) | (r >> 2)); o[1] = static_cast<uint8_t>((g << 3) | (g >> 2));
        o[2] = static_cast<uint8_t>((b << 3) | (b >> 2)); o[3] = 255;
    } else {
        const int a = (v >> 12) & 0x7, r = (v >> 8) & 0xF, g = (v >> 4) & 0xF, b = v & 0xF;
        o[0] = static_cast<uint8_t>(r * 17); o[1] = static_cast<uint8_t>(g * 17); o[2] = static_cast<uint8_t>(b * 17);
        o[3] = static_cast<uint8_t>((a << 5) | (a << 2) | (a >> 1));
    }
}
inline void DecTlut(const uint8_t* tlut, uint32_t fmt, uint32_t entries, uint32_t idx, uint8_t* o) {
    if (!tlut || idx >= entries) { o[0] = o[1] = o[2] = 0; o[3] = 0; return; }
    const uint16_t v = be16(tlut + idx * 2);
    switch (fmt) {
    case 0: o[0] = o[1] = o[2] = static_cast<uint8_t>(v & 0xFF); o[3] = static_cast<uint8_t>(v >> 8); break; // IA8
    case 1: Dec565(v, o); break;
    default: Dec5A3(v, o); break;
    }
}
} // namespace

bool TexFormatIsCI(uint32_t fmt) { return fmt == 0x8 || fmt == 0x9 || fmt == 0xA; }

uint32_t TexLevelSize(uint32_t w, uint32_t h, uint32_t fmt) {
    const TileInfo t = Tile(fmt);
    return ((w + t.tw - 1) / t.tw) * ((h + t.th - 1) / t.th) * t.bytes;
}
// The size the plain way (the division in TexLevelSize), kept as the reference: MSC_VGX_TEXSIZE_MEMO=0 and the verify mode 2 use it.
uint32_t TexDataSizeRef(uint32_t w, uint32_t h, uint32_t fmt, uint32_t levels) {
    uint32_t total = 0;
    for (uint32_t l = 0; l < levels; ++l) {
        total += TexLevelSize(w, h, fmt);
        if (w == 1 && h == 1) break;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    return total;
}

namespace {
// cpu60-25 (MSC_VGX_TEXSIZE_MEMO, default 1; 0 the plain loop, 2 verify): the player asks for the data size of a texture object at every R_TEX that is not a repeat
// (TexSameGen: ~460 a picture at The Vice's tour, 418 host instructions each, 2 integer divisions a mip level, which the console's Cortex-A9 does in software), and the
// answer is a pure function of (width, height, format, levels). A 64-entry table of the last answers (one 64-bit word each: the key above, the size below, written and
// read whole, so a thread that finds another's entry finds a whole one) gives it back for a handful of instructions; a miss computes it with shifts instead of divisions
// (the tile sizes are powers of two, so (w + t - 1) / t is (w + t - 1) >> log2 t, the same unsigned result for every input). Sizes outside the key's range (width or height
// over 1024 or 0, a format or level count over 15) take the plain function.
std::atomic<uint64_t> g_tdsMemo[64];
std::atomic<uint64_t> g_tdsWrong{0}, g_tdsChecked{0};
inline uint32_t TexDataSizeFast(uint32_t w, uint32_t h, uint32_t fmt, uint32_t levels) {
    const TileInfo t = Tile(fmt);
    uint32_t total = 0;
    for (uint32_t l = 0; l < levels; ++l) {
#ifdef VGX_TEXSIZE_MUTANT   // (unit test only: a size rounded down, the test must fail)
        total += (((w + t.tw - 1) >> t.sw) * ((h + t.th - 2) >> t.sh)) * t.bytes;
#else
        total += (((w + t.tw - 1) >> t.sw) * ((h + t.th - 1) >> t.sh)) * t.bytes;
#endif
        if (w == 1 && h == 1) break;
        w = w > 1 ? w >> 1 : 1;
        h = h > 1 ? h >> 1 : 1;
    }
    return total;
}
} // namespace

uint32_t TexDataSize(uint32_t w, uint32_t h, uint32_t fmt, uint32_t levels) {
    const int mode = static_cast<int>(VGX_KNOB(EnvInt("MSC_VGX_TEXSIZE_MEMO", 1)));
    if (mode == 0) return TexDataSizeRef(w, h, fmt, levels);
    if (w - 1u >= 1024u || h - 1u >= 1024u || fmt >= 16u || levels >= 16u) return mode == 2 ? TexDataSizeRef(w, h, fmt, levels) : TexDataSizeFast(w, h, fmt, levels);
    const uint32_t key = 0x80000000u | (w - 1u) | ((h - 1u) << 10) | (fmt << 20) | (levels << 24);
    std::atomic<uint64_t>& slot = g_tdsMemo[(key * 0x9E3779B1u) >> 26];
    const uint64_t e = slot.load(std::memory_order_relaxed);
    uint32_t size;
    if (static_cast<uint32_t>(e >> 32) == key) size = static_cast<uint32_t>(e);
    else {
        size = TexDataSizeFast(w, h, fmt, levels);
        slot.store((static_cast<uint64_t>(key) << 32) | size, std::memory_order_relaxed);
    }
    if (mode == 2) {
        const uint32_t ref = TexDataSizeRef(w, h, fmt, levels);
        const uint64_t n = g_tdsChecked.fetch_add(1, std::memory_order_relaxed) + 1;
        if (ref != size && g_tdsWrong.fetch_add(1, std::memory_order_relaxed) < 16)
            std::fprintf(stderr, "[texsize] MISMATCH %ux%u fmt %u levels %u: memo %u plain %u\n", w, h, fmt, levels, size, ref);
        if ((n & 0xFFFFF) == 0) std::fprintf(stderr, "[texsize] verify: %llu checked, %llu wrong\n", static_cast<unsigned long long>(n), static_cast<unsigned long long>(g_tdsWrong.load(std::memory_order_relaxed)));
        return ref;
    }
    return size;
}

// I8 -> RGBA8 (all four channels the intensity) for a size made of whole 8x4 tiles. Same bytes as the general loop below.
static void DecodeI8Tiles(const uint8_t* s, uint32_t w, uint32_t h, uint8_t* d) {
    const size_t rowBytes = static_cast<size_t>(w) * 4;
    for (uint32_t ty = 0; ty < h; ty += 4)
        for (uint32_t tx = 0; tx < w; tx += 8, s += 32) {
            uint8_t* o = d + static_cast<size_t>(ty) * rowBytes + static_cast<size_t>(tx) * 4;
            for (uint32_t y = 0; y < 4; ++y, o += rowBytes) {
#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && !defined(VGX_NO_NEON)
                const uint8x8_t v = vld1_u8(s + y * 8);
                const uint8x8x2_t z = vzip_u8(v, v); // y0 y0 y1 y1 ... / y4 y4 ...
                const uint16x4_t a = vreinterpret_u16_u8(z.val[0]), b = vreinterpret_u16_u8(z.val[1]);
                const uint16x4x2_t qa = vzip_u16(a, a), qb = vzip_u16(b, b);
                vst1q_u8(o, vcombine_u8(vreinterpret_u8_u16(qa.val[0]), vreinterpret_u8_u16(qa.val[1])));
                vst1q_u8(o + 16, vcombine_u8(vreinterpret_u8_u16(qb.val[0]), vreinterpret_u8_u16(qb.val[1])));
#else
                for (uint32_t x = 0; x < 8; ++x) {
                    const uint32_t px = s[y * 8 + x] * 0x01010101u;
                    std::memcpy(o + x * 4, &px, 4);
                }
#endif
            }
        }
}

void DecodeTexture(const uint8_t* s, uint32_t w, uint32_t h, uint32_t fmt, const uint8_t* tlut, uint32_t tlutFmt,
                   uint32_t tlutEntries, uint8_t* d) {
    uint8_t c[4];
    switch (fmt) {
    case 0x0: // I4
        for (uint32_t ty = 0; ty < h; ty += 8)
            for (uint32_t tx = 0; tx < w; tx += 8)
                for (uint32_t y = 0; y < 8; ++y)
                    for (uint32_t x = 0; x < 8; x += 2, ++s) {
                        const uint8_t a = static_cast<uint8_t>((*s >> 4) * 17), b = static_cast<uint8_t>((*s & 0xF) * 17);
                        Put(d, w, h, tx + x, ty + y, a, a, a, a);
                        Put(d, w, h, tx + x + 1, ty + y, b, b, b, b);
                    }
        break;
    case 0x1: // I8
        if (Gxd(GXD_SMALL) && (w & 7u) == 0 && (h & 3u) == 0) { // (movie planes and most UI textures: whole 8x4 tiles, no bounds test, one store per 4 texels)
            DecodeI8Tiles(s, w, h, d);
            break;
        }
        for (uint32_t ty = 0; ty < h; ty += 4)
            for (uint32_t tx = 0; tx < w; tx += 8)
                for (uint32_t y = 0; y < 4; ++y)
                    for (uint32_t x = 0; x < 8; ++x, ++s) Put(d, w, h, tx + x, ty + y, *s, *s, *s, *s);
        break;
    case 0x2: // IA4
        for (uint32_t ty = 0; ty < h; ty += 4)
            for (uint32_t tx = 0; tx < w; tx += 8)
                for (uint32_t y = 0; y < 4; ++y)
                    for (uint32_t x = 0; x < 8; ++x, ++s) {
                        const uint8_t i = static_cast<uint8_t>((*s & 0xF) * 17), a = static_cast<uint8_t>((*s >> 4) * 17);
                        Put(d, w, h, tx + x, ty + y, i, i, i, a);
                    }
        break;
    case 0x3: // IA8
        for (uint32_t ty = 0; ty < h; ty += 4)
            for (uint32_t tx = 0; tx < w; tx += 4)
                for (uint32_t y = 0; y < 4; ++y)
                    for (uint32_t x = 0; x < 4; ++x, s += 2) Put(d, w, h, tx + x, ty + y, s[1], s[1], s[1], s[0]);
        break;
    case 0x4: // RGB565
        for (uint32_t ty = 0; ty < h; ty += 4)
            for (uint32_t tx = 0; tx < w; tx += 4)
                for (uint32_t y = 0; y < 4; ++y)
                    for (uint32_t x = 0; x < 4; ++x, s += 2) {
                        Dec565(be16(s), c);
                        Put(d, w, h, tx + x, ty + y, c[0], c[1], c[2], 255);
                    }
        break;
    case 0x5: // RGB5A3
        for (uint32_t ty = 0; ty < h; ty += 4)
            for (uint32_t tx = 0; tx < w; tx += 4)
                for (uint32_t y = 0; y < 4; ++y)
                    for (uint32_t x = 0; x < 4; ++x, s += 2) {
                        Dec5A3(be16(s), c);
                        Put(d, w, h, tx + x, ty + y, c[0], c[1], c[2], c[3]);
                    }
        break;
    case 0x6: // RGBA8: 4x4 tiles, 32 bytes of AR pairs then 32 bytes of GB pairs
        for (uint32_t ty = 0; ty < h; ty += 4)
            for (uint32_t tx = 0; tx < w; tx += 4, s += 64)
                for (uint32_t y = 0; y < 4; ++y)
                    for (uint32_t x = 0; x < 4; ++x) {
                        const uint8_t* ar = s + (y * 4 + x) * 2;
                        const uint8_t* gb = ar + 32;
                        Put(d, w, h, tx + x, ty + y, ar[1], gb[0], gb[1], ar[0]);
                    }
        break;
    case 0x8: // C4
        for (uint32_t ty = 0; ty < h; ty += 8)
            for (uint32_t tx = 0; tx < w; tx += 8)
                for (uint32_t y = 0; y < 8; ++y)
                    for (uint32_t x = 0; x < 8; x += 2, ++s) {
                        DecTlut(tlut, tlutFmt, tlutEntries, *s >> 4, c);
                        Put(d, w, h, tx + x, ty + y, c[0], c[1], c[2], c[3]);
                        DecTlut(tlut, tlutFmt, tlutEntries, *s & 0xF, c);
                        Put(d, w, h, tx + x + 1, ty + y, c[0], c[1], c[2], c[3]);
                    }
        break;
    case 0x9: // C8
        for (uint32_t ty = 0; ty < h; ty += 4)
            for (uint32_t tx = 0; tx < w; tx += 8)
                for (uint32_t y = 0; y < 4; ++y)
                    for (uint32_t x = 0; x < 8; ++x, ++s) {
                        DecTlut(tlut, tlutFmt, tlutEntries, *s, c);
                        Put(d, w, h, tx + x, ty + y, c[0], c[1], c[2], c[3]);
                    }
        break;
    case 0xA: // C14X2
        for (uint32_t ty = 0; ty < h; ty += 4)
            for (uint32_t tx = 0; tx < w; tx += 4)
                for (uint32_t y = 0; y < 4; ++y)
                    for (uint32_t x = 0; x < 4; ++x, s += 2) {
                        DecTlut(tlut, tlutFmt, tlutEntries, be16(s) & 0x3FFF, c);
                        Put(d, w, h, tx + x, ty + y, c[0], c[1], c[2], c[3]);
                    }
        break;
    case 0xE: // CMPR: 8x8 tiles of four DXT1 blocks
        for (uint32_t ty = 0; ty < h; ty += 8)
            for (uint32_t tx = 0; tx < w; tx += 8)
                for (uint32_t sub = 0; sub < 4; ++sub, s += 8) {
                    const uint32_t bx = tx + (sub & 1) * 4, by = ty + (sub >> 1) * 4;
                    const uint16_t c0 = be16(s), c1 = be16(s + 2);
                    uint8_t pal[4][4];
                    Dec565(c0, pal[0]);
                    Dec565(c1, pal[1]);
                    if (c0 > c1) {
                        for (int k = 0; k < 3; ++k) {
                            pal[2][k] = static_cast<uint8_t>((pal[0][k] * 5 + pal[1][k] * 3) >> 3);
                            pal[3][k] = static_cast<uint8_t>((pal[1][k] * 5 + pal[0][k] * 3) >> 3);
                        }
                        pal[2][3] = pal[3][3] = 255;
                    } else {
                        for (int k = 0; k < 3; ++k) pal[2][k] = pal[3][k] = static_cast<uint8_t>((pal[0][k] + pal[1][k]) >> 1);
                        pal[2][3] = 255;
                        pal[3][3] = 0;
                    }
                    for (uint32_t y = 0; y < 4; ++y) {
                        const uint8_t rowBits = s[4 + y];
                        for (uint32_t x = 0; x < 4; ++x) {
                            const uint8_t* q = pal[(rowBits >> (6 - x * 2)) & 3];
                            Put(d, w, h, bx + x, by + y, q[0], q[1], q[2], q[3]);
                        }
                    }
                }
        break;
    default:
        for (uint32_t i = 0; i < w * h; ++i) { d[i * 4] = 255; d[i * 4 + 1] = 0; d[i * 4 + 2] = 255; d[i * 4 + 3] = 255; }
        break;
    }
}

#ifdef VGX_TEXDEC_NEON
namespace {
// Eight grey texels as luminance-alpha (the same value twice).
inline void StoreGray2(uint8_t* d, uint8x8_t v) {
    uint8x8x2_t q;
    q.val[0] = q.val[1] = v;
    vst2_u8(d, q);
}
// GXD_TEXNEON (Galaxy's LA88 tile decoders): a size made of whole tiles, one vector store per row of a tile. The same bytes as the loops below.
template <uint32_t fmt>
bool DecodeLA88TilesNeon(const uint8_t* s, uint32_t w, uint32_t h, uint8_t* d) {
    const TileInfo t = Tile(fmt);
    if ((w & (t.tw - 1)) || (h & (t.th - 1))) return false;
    const size_t stride = static_cast<size_t>(w) * 2;
    for (uint32_t ty = 0; ty < h; ty += t.th)
        for (uint32_t tx = 0; tx < w; tx += t.tw, s += t.bytes) {
            uint8_t* row = d + static_cast<size_t>(ty) * stride + tx * 2;
            if constexpr (fmt == 0) { // I4: two rows of eight texels from 8 bytes (high nibble first), L = A = n * 17
                for (uint32_t y = 0; y < 8; y += 2) {
                    const uint8x8_t packed = vld1_u8(s + y * 4);
                    uint8x8_t hi = vshr_n_u8(packed, 4), lo = vand_u8(packed, vdup_n_u8(15));
                    hi = vorr_u8(hi, vshl_n_u8(hi, 4));
                    lo = vorr_u8(lo, vshl_n_u8(lo, 4));
                    const uint8x8x2_t pixels = vzip_u8(hi, lo);
                    StoreGray2(row + y * stride, pixels.val[0]);
                    StoreGray2(row + (y + 1) * stride, pixels.val[1]);
                }
            } else if constexpr (fmt == 1) { // I8: L = A = the byte
                for (uint32_t y = 0; y < 4; ++y) StoreGray2(row + y * stride, vld1_u8(s + y * 8));
            } else if constexpr (fmt == 2) { // IA4: L = low nibble * 17, A = high nibble * 17
                for (uint32_t y = 0; y < 4; ++y) {
                    const uint8x8_t packed = vld1_u8(s + y * 8);
                    uint8x8x2_t pixels;
                    const uint8x8_t l = vand_u8(packed, vdup_n_u8(15)), a = vshr_n_u8(packed, 4);
                    pixels.val[0] = vorr_u8(l, vshl_n_u8(l, 4));
                    pixels.val[1] = vorr_u8(a, vshl_n_u8(a, 4));
                    vst2_u8(row + y * stride, pixels);
                }
            } else { // IA8: the stored pair is (alpha, luminance), the output (luminance, alpha)
                for (uint32_t y = 0; y < 4; ++y) vst1_u8(row + y * stride, vrev16_u8(vld1_u8(s + y * 8)));
            }
        }
    return true;
}
} // namespace
#endif

bool DecodeTextureLA88(const uint8_t* s, uint32_t w, uint32_t h, uint32_t fmt, uint8_t* d) {
    if (fmt > 3 || !w || !h || !s || !d) return false;
#ifdef VGX_TEXDEC_NEON
    if (Gxd(GXD_TEXNEON)) {
        bool done = false;
        switch (fmt) {
        case 0: done = DecodeLA88TilesNeon<0>(s, w, h, d); break;
        case 1: done = DecodeLA88TilesNeon<1>(s, w, h, d); break;
        case 2: done = DecodeLA88TilesNeon<2>(s, w, h, d); break;
        default: done = DecodeLA88TilesNeon<3>(s, w, h, d); break;
        }
        if (done) return true;
    }
#endif
    const TileInfo t = Tile(fmt);
    for (uint32_t ty = 0; ty < h; ty += t.th)
        for (uint32_t tx = 0; tx < w; tx += t.tw, s += t.bytes)
            for (uint32_t y = 0; y < t.th && ty + y < h; ++y)
                for (uint32_t x = 0; x < t.tw && tx + x < w; ++x) {
                    const uint32_t p = y * t.tw + x;
                    uint8_t l, a;
                    switch (fmt) {
                    case 0: l = static_cast<uint8_t>(((s[p / 2] >> ((p & 1) ? 0 : 4)) & 15) * 17); a = l; break;
                    case 1: l = a = s[p]; break;
                    case 2: l = static_cast<uint8_t>((s[p] & 15) * 17); a = static_cast<uint8_t>((s[p] >> 4) * 17); break;
                    default: l = s[p * 2 + 1]; a = s[p * 2]; break;
                    }
                    uint8_t* out = d + (static_cast<size_t>(ty + y) * w + tx + x) * 2;
                    out[0] = l; out[1] = a;
                }
    return true;
}

bool DecodeTextureI8(const uint8_t* s, uint32_t w, uint32_t h, uint8_t* d) {
    if (!s || !d || !w || !h || (w & 7u) || (h & 3u)) return false;
    for (uint32_t ty = 0; ty < h; ty += 4)
        for (uint32_t tx = 0; tx < w; tx += 8, s += 32) {
            uint8_t* row = d + static_cast<size_t>(ty) * w + tx;
            for (uint32_t y = 0; y < 4; ++y) std::memcpy(row + static_cast<size_t>(y) * w, s + y * 8, 8);
        }
    return true;
}

bool CmprToDxt1(const uint8_t* s, uint32_t w, uint32_t h, uint8_t* d, bool allowTransparent) {
    const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4; // DXT1 blocks
    const uint32_t tw = (w + 7) / 8;                   // CMPR tiles per row
    for (uint32_t by = 0; by < bh; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx, d += 8) {
            const uint8_t* b = s + ((static_cast<size_t>(by >> 1) * tw + (bx >> 1)) * 4 + (by & 1) * 2 + (bx & 1)) * 8;
            const uint32_t c0 = be16(b), c1 = be16(b + 2);
            d[0] = b[1]; d[1] = b[0]; d[2] = b[3]; d[3] = b[2];
            for (int y = 0; y < 4; ++y) {
                // GX puts the leftmost texel in the top two bits, DXT1 in the bottom two.
                const uint32_t r = b[4 + y];
                if (!allowTransparent && c0 <= c1 && ((r & (r >> 1)) & 0x55u)) return false; // entry 3 of a three-colour block
                d[4 + y] = static_cast<uint8_t>(((r & 0x03u) << 6) | ((r & 0x0Cu) << 2) | ((r & 0x30u) >> 2) | ((r & 0xC0u) >> 6));
            }
        }
    return true;
}

void CmprToDxt5(const uint8_t* s, uint32_t w, uint32_t h, uint8_t* d) {
    const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4; // blocks
    const uint32_t tw = (w + 7) / 8;                   // CMPR tiles per row
    for (uint32_t by = 0; by < bh; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx, d += 16) {
            const uint8_t* b = s + ((static_cast<size_t>(by >> 1) * tw + (bx >> 1)) * 4 + (by & 1) * 2 + (bx & 1)) * 8;
            const uint32_t c0 = be16(b), c1 = be16(b + 2);
            const bool three = c0 <= c1;
            // The colour block of DXT5 always has four colours, and the larger endpoint goes first so that no decoder can take it for a three-colour block.
            const uint32_t e0 = three ? c1 : c0, e1 = three ? c0 : c1;
            d[8] = static_cast<uint8_t>(e0); d[9] = static_cast<uint8_t>(e0 >> 8);
            d[10] = static_cast<uint8_t>(e1); d[11] = static_cast<uint8_t>(e1 >> 8);
            uint64_t alpha = 0; // 3-bit indices, first texel lowest: 0 = 255, 1 = 0
            for (uint32_t y = 0; y < 4; ++y) {
                const uint32_t r = b[4 + y];
                uint32_t row = 0;
                for (uint32_t x = 0; x < 4; ++x) {
                    uint32_t i = (r >> (6 - x * 2)) & 3u; // GX: the leftmost texel in the top two bits
                    if (three) {
                        if (i == 3) alpha |= 1ull << ((y * 4 + x) * 3);
                        if (c0 == c1) i = 0; // (one colour: no decoder can get this block wrong)
                        else i = i < 2 ? (i ^ 1u) : (((bx * 4 + x + by * 4 + y) & 1u) ? 3u : 2u); // (the endpoints changed places; the middle colour: see the header)
                    }
                    row |= i << (x * 2);
                }
                d[12 + y] = static_cast<uint8_t>(row);
            }
            d[0] = 255; d[1] = 0;
            for (int k = 0; k < 6; ++k) d[2 + k] = static_cast<uint8_t>(alpha >> (k * 8));
        }
}

uint32_t CmprDxt5MaxError(const uint8_t* s, uint32_t w, uint32_t h) {
    const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, tw = (w + 7) / 8;
    uint32_t worst = 0;
    for (uint32_t by = 0; by < bh; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx) {
            const uint8_t* b = s + ((static_cast<size_t>(by >> 1) * tw + (bx >> 1)) * 4 + (by & 1) * 2 + (bx & 1)) * 8;
            const uint32_t c0 = be16(b), c1 = be16(b + 2);
            if (c0 >= c1) continue; // four colours, or one: CmprToDxt5 keeps every colour of these
            uint8_t p0[4], p1[4];
            Dec565(static_cast<uint16_t>(c0), p0);
            Dec565(static_cast<uint16_t>(c1), p1);
            for (uint32_t y = 0; y < 4; ++y)
                for (uint32_t x = 0; x < 4; ++x) {
                    if (((b[4 + y] >> (6 - x * 2)) & 3u) < 2) continue; // an end colour, exact
                    const bool odd = ((bx * 4 + x + by * 4 + y) & 1u) != 0;
                    for (int k = 0; k < 3; ++k) {
                        const int mid = (p0[k] + p1[k]) / 2;
                        const int got = odd ? (p1[k] + 2 * p0[k]) / 3 : (2 * p1[k] + p0[k]) / 3;
                        const uint32_t e = static_cast<uint32_t>(mid > got ? mid - got : got - mid);
                        if (e > worst) worst = e;
                    }
                }
        }
    return worst;
}

uint8_t Rgba16Format(const std::vector<std::vector<uint8_t>>& levels, bool allow5551) {
    enum : uint32_t { F565 = 1, F5551 = 2, F4444 = 4 };
    const auto rep5 = [](uint32_t v) { return ((v >> 3) << 3 | v >> 5) == v; };
    const auto rep6 = [](uint32_t v) { return ((v >> 2) << 2 | v >> 6) == v; };
    const auto rep4 = [](uint32_t v) { return (v >> 4) == (v & 15u); };
    uint32_t can = F565 | (allow5551 ? F5551 : 0u) | F4444;
    for (const std::vector<uint8_t>& lv : levels) {
        for (size_t i = 0; can && i + 4 <= lv.size(); i += 4) {
            const uint32_t r = lv[i], g = lv[i + 1], b = lv[i + 2], a = lv[i + 3];
            const bool c5 = rep5(r) && rep5(b);
            if (!(c5 && rep6(g) && a == 255)) can &= ~static_cast<uint32_t>(F565);
            if (!(c5 && rep5(g) && (a == 0 || a == 255))) can &= ~static_cast<uint32_t>(F5551);
            if (!(rep4(r) && rep4(g) && rep4(b) && rep4(a))) can &= ~static_cast<uint32_t>(F4444);
        }
        if (!can) return 0;
    }
    return (can & F565) ? TF_RGB565 : (can & F5551) ? TF_RGBA5551 : (can & F4444) ? TF_RGBA4444 : 0;
}

void PackRgba16(std::vector<uint8_t>& lv, uint8_t format) {
    const size_t n = lv.size() / 4;
    for (size_t i = 0; i < n; ++i) {
        const uint32_t r = lv[i * 4], g = lv[i * 4 + 1], b = lv[i * 4 + 2], a = lv[i * 4 + 3];
        uint32_t v;
        if (format == TF_RGB565) v = (r >> 3) << 11 | (g >> 2) << 5 | b >> 3;
        else if (format == TF_RGBA5551) v = (r >> 3) << 11 | (g >> 3) << 6 | (b >> 3) << 1 | a >> 7;
        else v = (r >> 4) << 12 | (g >> 4) << 8 | (b >> 4) << 4 | a >> 4;
        lv[i * 2] = static_cast<uint8_t>(v);
        lv[i * 2 + 1] = static_cast<uint8_t>(v >> 8);
    }
    lv.resize(n * 2);
}

void EncodeDxt1(const uint8_t* rgba, uint32_t w, uint32_t h, uint8_t* d) {
    const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    for (uint32_t by = 0; by < bh; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx, d += 8) {
            uint8_t px[16][3];
            int lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
            for (uint32_t y = 0; y < 4; ++y)
                for (uint32_t x = 0; x < 4; ++x) {
                    const uint32_t sx = bx * 4 + x < w ? bx * 4 + x : w - 1, sy = by * 4 + y < h ? by * 4 + y : h - 1;
                    const uint8_t* p = rgba + (static_cast<size_t>(sy) * w + sx) * 4;
                    for (int k = 0; k < 3; ++k) {
                        px[y * 4 + x][k] = p[k];
                        if (p[k] < lo[k]) lo[k] = p[k];
                        if (p[k] > hi[k]) hi[k] = p[k];
                    }
                }
            const auto to565 = [](const int* c) { return static_cast<uint32_t>(((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3)); };
            uint32_t c0 = to565(hi), c1 = to565(lo);
            if (c0 < c1) { const uint32_t t = c0; c0 = c1; c1 = t; }
            d[0] = static_cast<uint8_t>(c0); d[1] = static_cast<uint8_t>(c0 >> 8);
            d[2] = static_cast<uint8_t>(c1); d[3] = static_cast<uint8_t>(c1 >> 8);
            if (c0 == c1) { d[4] = d[5] = d[6] = d[7] = 0; continue; } // (entry 0 of a three-colour block: opaque)
            uint8_t pal[4][4];
            Dec565(static_cast<uint16_t>(c0), pal[0]);
            Dec565(static_cast<uint16_t>(c1), pal[1]);
            for (int k = 0; k < 3; ++k) {
                pal[2][k] = static_cast<uint8_t>((2 * pal[0][k] + pal[1][k] + 1) / 3);
                pal[3][k] = static_cast<uint8_t>((pal[0][k] + 2 * pal[1][k] + 1) / 3);
            }
            for (uint32_t y = 0; y < 4; ++y) {
                uint32_t row = 0;
                for (uint32_t x = 0; x < 4; ++x) {
                    int best = 0, bestD = 1 << 30;
                    for (int e = 0; e < 4; ++e) {
                        int dist = 0;
                        for (int k = 0; k < 3; ++k) { const int df = px[y * 4 + x][k] - pal[e][k]; dist += df * df; }
                        if (dist < bestD) { bestD = dist; best = e; }
                    }
                    row |= static_cast<uint32_t>(best) << (x * 2);
                }
                d[4 + y] = static_cast<uint8_t>(row);
            }
        }
}

void Downsample(const uint8_t* src, uint32_t w, uint32_t h, std::vector<uint8_t>& dst, uint32_t& dw, uint32_t& dh) {
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

} // namespace vgx
