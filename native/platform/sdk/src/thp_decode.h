#ifndef NP_THP_DECODE_H
#define NP_THP_DECODE_H
// Workstream F: native THP video frame decoder (core, host pointers only).
//
// A THP video frame is a baseline JPEG (YCbCr 4:2:0, three components, 8-bit) whose entropy-coded
// data has no 0xFF byte stuffing. The SDK decoder (THPDec.c: THPVideoDecode and the routines
// below it) is hand-written paired-single assembly: Huffman decoding into 16-bit coefficient
// blocks, a floating-point AAN inverse DCT (row pass on adjacent coefficient pairs with shortcut
// paths for rows whose upper coefficients are zero, column pass two columns at a time), and
// quantised stores of the samples straight into GX I8 tile layout (8x4 texel tiles, 32 bytes each).
//
// This decoder produces the same bytes. The inverse DCT repeats the original's floating-point
// operations in the original order, including the per-row choice between the regular, "half",
// "quarter" and DC-only paths (they round differently) and the multiply-adds that the translated
// code's ISA model fuses on hosts with FMA (psmadd/psmsub of fast_common.h follow that model),
// so the output is expected to be bit-identical to the translation on every host; the
// differential test (fast_thp.cpp) compares the three planes byte for byte. The Huffman stage is
// an ordinary table-driven JPEG decoder: for a valid stream any correct decoder yields the same
// coefficients. On corrupt streams this decoder and the original both produce garbage, not
// necessarily the same garbage.
//
// No third-party code is used.
//
// Port notes (platform/sdk): taken from the recompilation's fast_thp.h with its dependencies cut (host
// pointers only). The multiply-adds are not fused here: the recompilation fused them to match the
// console's paired-single unit bit for bit on x86-64; the native port has no replay to match, and an
// unfused a*c+b is what the Cortex-A9 and the 32-bit host do natively, so samples may differ from the
// console's by one level in places.
#include <stdint.h>
#include <string.h>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#elif defined(__SSE2__)
#include <emmintrin.h>
#endif

typedef float v4f __attribute__((vector_size(16)));

namespace np_thp {
inline v4f Splat(float s) { return (v4f){s, s, s, s}; }
inline float psmadd(float a, float c, float b) { return a * c + b; }
inline float psmsub(float a, float c, float b) { return a * c - b; }
inline v4f MAdd4(v4f a, v4f c, v4f b) { return a * c + b; }
inline v4f MSub4(v4f a, v4f c, v4f b) { return a * c - b; }
}  // namespace np_thp



namespace np_thp {

// Tables of the game's copy of the SDK decoder (read from guest memory by the caller).
struct Tables {
    const uint8_t* naturalOrder; // __THPJpegNaturalOrder: 64 zig-zag entries + 16 x 63 padding
    double aan[8];               // __THPAANScaleFactor
};

// Result of the header pass.
struct Frame {
    uint32_t width = 0, height = 0;
};

namespace detail {

constexpr float kCC4 = 1.414213562F;
constexpr float kCC2 = 1.847759065F;
constexpr float kCC2C6S = 1.082392200F;
constexpr float kCC2C6A = -2.613125930F;
constexpr float kBias = 1024.0F;

struct HuffTab {
    uint16_t look[512];  // next 9 bits -> (code length << 8) | symbol; 0: code longer than 9 bits
    int32_t maxCode[18]; // largest code of each length, -1 if there is none
    int32_t valPtr[17];
    const uint8_t* vij;  // symbols, in the stream
};

struct Component {
    uint8_t quantSel = 0, dcSel = 0, acSel = 0;
    int16_t predDC = 0;
};

// MSB-first bit reader over a byte stream (no stuffing). `acc` holds `count` valid bits at its
// top; the bits below them are zero or copies of bits that will be read again.
struct Bits {
    const uint8_t* p;
    uint32_t acc = 0;
    uint32_t count = 0;
    inline void Refill() // afterwards 24 <= count <= 31
    {
        uint32_t w;
        memcpy(&w, p, 4);
        acc |= __builtin_bswap32(w) >> count;
        p += (31u - count) >> 3;
        count |= 24u;
    }
    inline void Drop(uint32_t n) { acc <<= n; count -= n; }
    // The stream position is a whole number of bytes ahead of the consumed bits, so the bits
    // left to the next byte boundary are count mod 8.
    inline void AlignToByte() { Drop(count & 7u); }
};

inline int32_t Extend(uint32_t v, uint32_t n)
{
    // if (__cntlzw(v) > 32 - n) v += (0xFFFFFFFF << n) + 1
    return v < (1u << (n - 1u)) ? static_cast<int32_t>(v + (0xFFFFFFFFu << n) + 1u) : static_cast<int32_t>(v);
}

// Slow path: a code longer than 9 bits. Needs at least 16 valid bits.
inline uint32_t DecodeLong(Bits& b, const HuffTab& h)
{
    uint32_t len = 10;
    uint32_t code = b.acc >> 22;
    while (len <= 16 && static_cast<int32_t>(code) > h.maxCode[len]) {
        ++len;
        code = b.acc >> (32u - len);
    }
    if (len > 16) { // not a code of this table (corrupt stream)
        b.Drop(16);
        return 0;
    }
    b.Drop(len);
    return h.vij[static_cast<int32_t>(code) + h.valPtr[len]];
}

inline uint32_t DecodeSymbol(Bits& b, const HuffTab& h)
{
    b.Refill();
    const uint32_t e = h.look[b.acc >> 23];
    if (__builtin_expect(e != 0, 1)) {
        b.Drop(e >> 8);
        return e & 0xFFu;
    }
    return DecodeLong(b, h);
}

inline int32_t Receive(Bits& b, uint32_t n) // 1 <= n <= 16
{
    b.Refill();
    const uint32_t v = b.acc >> (32u - n);
    b.Drop(n);
    return Extend(v, n);
}

// One 8x8 block: __THPHuffDecodeDCTCompY/U/V. Returns true when every AC coefficient is zero.
inline bool DecodeBlock(Bits& b, const HuffTab& dc, const HuffTab& ac, Component& comp, const uint8_t* natural, int16_t* block)
{
    memset(block, 0, 64 * sizeof(int16_t));
    uint32_t t = DecodeSymbol(b, dc);
    int32_t diff = 0;
    if (t) {
        if (t > 16) t = 16; // corrupt stream
        diff = Receive(b, t);
    }
    comp.predDC = static_cast<int16_t>(comp.predDC + diff);
    block[0] = comp.predDC;
    bool dcOnly = true;
    for (uint32_t k = 1; k < 64; ++k) {
        b.Refill();
        uint32_t sym;
        const uint32_t e = ac.look[b.acc >> 23];
        if (__builtin_expect(e != 0, 1)) {
            b.Drop(e >> 8);
            sym = e & 0xFFu;
        } else {
            sym = DecodeLong(b, ac);
            b.Refill();
        }
        // Here at least 15 valid bits remain: 24 - 9 after a short code, a refill after a long one.
        const uint32_t size = sym & 15u;
        const uint32_t run = sym >> 4;
        if (size == 0) {
            if (run != 15) break; // end of block
            k += 15;
            continue;
        }
        k += run;
        const uint32_t v = b.acc >> (32u - size);
        b.Drop(size);
        block[natural[k]] = static_cast<int16_t>(Extend(v, size)); // k <= 78: the table is padded to 80
        dcOnly = false;
    }
    return dcOnly;
}

// ---- inverse DCT, row pass (first loop of __THPInverseDCTNoYPos / __THPInverseDCTY8) ----------
// Notation follows the derivation in NOTES.md: d_k = in[k] * q[k].
inline void RowPass(const int16_t* in, const float* q, float* ws)
{
    for (int r = 0; r < 8; ++r, in += 8, q += 8, ws += 8) {
        const float d0 = static_cast<float>(in[0]) * q[0];
        uint32_t w1, w2, w3;
        memcpy(&w1, in + 2, 4);
        memcpy(&w2, in + 4, 4);
        memcpy(&w3, in + 6, 4);
        if ((w2 | w3) != 0) { // _regularIDCT
            const float d1 = static_cast<float>(in[1]) * q[1];
            const float d2 = static_cast<float>(in[2]) * q[2];
            const float d3 = static_cast<float>(in[3]) * q[3];
            const float x4 = static_cast<float>(in[4]), x5 = static_cast<float>(in[5]);
            const float x6 = static_cast<float>(in[6]), x7 = static_cast<float>(in[7]);
            const float a = psmadd(x4, q[4], d0), b = -psmsub(x4, q[4], d0);
            const float c = psmadd(x6, q[6], d2), d = -psmsub(x6, q[6], d2);
            const float p = psmadd(x5, q[5], d3), qq = -psmsub(x5, q[5], d3);
            const float rr = psmadd(x7, q[7], d1), s = -psmsub(x7, q[7], d1);
            const float e0 = a + c, e3 = a - c;
            const float m = psmsub(d, kCC4, c);
            const float e1 = b + m, e2 = b - m;
            const float o7 = p + rr, u = p - rr;
            const float z5 = (s - qq) * kCC2;
            const float t6 = -psmsub(qq, kCC2C6A, z5) - o7;
            const float t5 = psmadd(u, kCC4, t6);
            const float t4 = psmsub(s, kCC2C6S, z5) - t5;
            ws[0] = e0 + o7; ws[1] = e1 + t6; ws[2] = e2 - t5; ws[3] = e3 - t4;
            ws[4] = e3 + t4; ws[5] = e2 + t5; ws[6] = e1 - t6; ws[7] = e0 - o7;
        } else if (w1 != 0) { // _halfIDCT
            const float d1 = static_cast<float>(in[1]) * q[1];
            const float d2 = static_cast<float>(in[2]) * q[2];
            const float d3 = static_cast<float>(in[3]) * q[3];
            const float e0 = d0 + d2, f0 = d0 - d2;
            const float os = d1 + d3, od = d1 - d3;
            const float g = psmadd(d2, kCC4, f0);
            const float h = -psmsub(d2, kCC4, e0);
            const float z = od * kCC2;
            const float t6 = -psmsub(d3, kCC2C6A, z) - os;
            const float t8 = psmsub(od, kCC4, t6);
            const float t7 = -psmsub(d1, kCC2C6S, z) - t8;
            ws[0] = e0 + os; ws[1] = g + t6; ws[2] = h + t8; ws[3] = f0 + t7;
            ws[4] = f0 - t7; ws[5] = h - t8; ws[6] = g - t6; ws[7] = e0 - os;
        } else if (in[1] != 0) { // _quarterIDCT
            const float d1 = static_cast<float>(in[1]) * q[1];
            const float k = kCC2 - kCC2C6S;
            const float a = psmsub(d1, kCC2, d1);
            const float b = psmsub(d1, kCC4, a);
            const float c = -psmsub(d1, k, b);
            ws[0] = d0 + d1; ws[1] = d0 + a; ws[2] = d0 + b; ws[3] = d0 + c;
            ws[4] = d0 - c; ws[5] = d0 - b; ws[6] = d0 - a; ws[7] = d0 - d1;
        } else {
            ws[0] = ws[1] = ws[2] = ws[3] = ws[4] = ws[5] = ws[6] = ws[7] = d0;
        }
    }
}

// psq_st with GQR type u8, scale 2^-3 (PpcQuantizePsqU8Scale61Inline): v * 0.125, not-greater-
// than-zero (NaN included) -> 0, >= 255 -> 255, otherwise truncated.
inline void StoreSamples8(uint8_t* dst, v4f lo, v4f hi)
{
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    const float32x4_t s = vdupq_n_f32(0.125f);
    const uint16x4_t a = vqmovn_u32(vcvtq_u32_f32(vmulq_f32((float32x4_t)lo, s))); // saturating, truncating, NaN -> 0
    const uint16x4_t b = vqmovn_u32(vcvtq_u32_f32(vmulq_f32((float32x4_t)hi, s)));
    vst1_u8(dst, vqmovn_u16(vcombine_u16(a, b)));
#elif defined(__SSE2__)
    const __m128 s = _mm_set1_ps(0.125f);
    const __m128 z = _mm_setzero_ps(), m = _mm_set1_ps(255.0f);
    const __m128i a = _mm_cvttps_epi32(_mm_min_ps(_mm_max_ps(_mm_mul_ps((__m128)lo, s), z), m)); // max(NaN, 0) = 0
    const __m128i b = _mm_cvttps_epi32(_mm_min_ps(_mm_max_ps(_mm_mul_ps((__m128)hi, s), z), m));
    const __m128i w = _mm_packs_epi32(a, b);
    _mm_storel_epi64((__m128i*)dst, _mm_packus_epi16(w, w));
#else
    for (int i = 0; i < 8; ++i) {
        const float v = (i < 4 ? lo[i] : hi[i - 4]) * 0.125f;
        dst[i] = !(v > 0.0f) ? 0 : (v >= 255.0f ? 255 : static_cast<uint8_t>(v));
    }
#endif
}

inline uint8_t QuantizeSample(float value)
{
    const float v = value * 0.125f;
    if (!(v > 0.0f)) return 0;
    if (v >= 255.0f) return 255;
    return static_cast<uint8_t>(v);
}

// ---- inverse DCT, column pass and stores (second half of the two routines) --------------------
// out0: rows 0-3 of the block (one 8x4 tile, rows 8 bytes apart), out1: rows 4-7 (the tile below).
inline void ColumnPass(const float* ws, uint8_t* out0, uint8_t* out1)
{
    const v4f cc4 = Splat(kCC4), cc2 = Splat(kCC2), cc2c6s = Splat(kCC2C6S), cc2c6a = Splat(kCC2C6A), bias = Splat(kBias);
    v4f r[8][2];
    for (int h = 0; h < 2; ++h) {
        v4f w[8];
        for (int i = 0; i < 8; ++i) memcpy(&w[i], ws + 8 * i + 4 * h, 16);
        const v4f a = (w[0] + w[4]) + bias;
        const v4f b = (w[0] - w[4]) + bias;
        const v4f s26 = w[2] + w[6];
        const v4f d26 = w[2] - w[6];
        const v4f t0 = a + s26;
        const v4f m = MSub4(d26, cc4, s26);
        const v4f t3 = a - s26;
        const v4f t1 = b + m;
        const v4f t2 = b - m;
        const v4f z53 = w[5] + w[3];
        const v4f d53 = w[5] - w[3];
        const v4f z17 = w[1] + w[7];
        const v4f d17 = w[1] - w[7];
        const v4f o7 = z17 + z53;
        const v4f o5 = z17 - z53;
        const v4f z = (d53 + d17) * cc2;
        const v4f o6 = MAdd4(d53, cc2c6a, z) - o7;
        const v4f o4 = MSub4(d17, cc2c6s, z);
        const v4f o5b = MSub4(o5, cc4, o6);
        const v4f o4b = o4 + o5b;
        r[0][h] = t0 + o7; r[7][h] = t0 - o7;
        r[1][h] = t1 + o6; r[6][h] = t1 - o6;
        r[2][h] = t2 + o5b; r[5][h] = t2 - o5b;
        r[3][h] = t3 - o4b; r[4][h] = t3 + o4b;
    }
    StoreSamples8(out0, r[0][0], r[0][1]);
    StoreSamples8(out0 + 8, r[1][0], r[1][1]);
    StoreSamples8(out0 + 16, r[2][0], r[2][1]);
    StoreSamples8(out0 + 24, r[3][0], r[3][1]);
    StoreSamples8(out1, r[4][0], r[4][1]);
    StoreSamples8(out1 + 8, r[5][0], r[5][1]);
    StoreSamples8(out1 + 16, r[6][0], r[6][1]);
    StoreSamples8(out1 + 24, r[7][0], r[7][1]);
}

// A block whose AC coefficients are all zero: every row takes the DC-only path, rows 1-7 are
// zero, and the column pass then gives (d0 + bias) plus or minus exact zeros in all 64 places.
inline void FlatBlock(int16_t dc, float q0, uint8_t* out0, uint8_t* out1)
{
    const uint8_t s = QuantizeSample(static_cast<float>(dc) * q0 + kBias);
    const uint64_t v = s * 0x0101010101010101ull;
    memcpy(out0, &v, 8); memcpy(out0 + 8, &v, 8); memcpy(out0 + 16, &v, 8); memcpy(out0 + 24, &v, 8);
    memcpy(out1, &v, 8); memcpy(out1 + 8, &v, 8); memcpy(out1 + 16, &v, 8); memcpy(out1 + 24, &v, 8);
}

inline void InverseBlock(const int16_t* block, bool dcOnly, const float* q, uint8_t* out0, uint8_t* out1)
{
    if (dcOnly) {
        FlatBlock(block[0], q[0], out0, out1);
        return;
    }
    alignas(16) float ws[64];
    RowPass(block, q, ws);
    ColumnPass(ws, out0, out1);
}

inline uint32_t Be16(const uint8_t* c) { return (static_cast<uint32_t>(c[0]) << 8) | c[1]; }

} // namespace detail

// Marker errors of the original (THPVideoDecode's return values).
enum : int { kOk = 0, kBadSyntax = 3, kBadPrecision = 10, kUnsupportedMarker = 11, kBadComponents = 12, kBadTable = 15, kBadSampling = 19 };

// Reads the frame size without decoding (for the differential test and the size check).
// Returns false if no frame header is found where the original would look for one.
inline bool PeekFrame(const uint8_t* c, Frame& frame, uint32_t maxBytes = 4096)
{
    const uint8_t* const end = c + maxBytes;
    while (c + 4 < end) {
        if (*c++ != 0xFF) return false;
        while (*c == 0xFF && c < end) ++c;
        const uint8_t m = *c++;
        if (m == 0xC0) {
            frame.height = detail::Be16(c + 3);
            frame.width = detail::Be16(c + 5);
            return true;
        }
        if (m == 0xD8) continue;
        if (m == 0xDA) return false;
        if (m == 0xC4 || m == 0xDB || m == 0xDD || (m >= 0xE0 && m <= 0xEF) || m == 0xFE) {
            c += detail::Be16(c);
            continue;
        }
        return false;
    }
    return false;
}

// Decodes one frame into the three tile planes. `supported` is cleared (and nothing is written)
// when the frame needs a path this decoder does not have (a width that is not a multiple of 16,
// table selectors outside the original's arrays); the caller then runs the translated original.
// Otherwise the return value is THPVideoDecode's.
inline int DecodeFrame(const uint8_t* file, uint8_t* tileY, uint8_t* tileU, uint8_t* tileV, const Tables& tables, Frame& frame,
                       bool& supported)
{
    using namespace detail;
    supported = true;
    float quant[3][64];
    HuffTab huff[4];
    Component comp[3];
    uint32_t validHuffman = 0;
    uint32_t width = 0, height = 0;
    bool restart = false;
    uint32_t restartInterval = 0;
    const uint8_t* c = file;

    for (bool allDone = false; !allDone;) {
        if (*c++ != 0xFF) return kBadSyntax;
        while (*c == 0xFF) ++c;
        const uint8_t marker = *c++;
        if (marker <= 0xD7) {
            if (marker == 0xC4) { // __THPReadHuffmanTableSpecification
                int32_t length = static_cast<int32_t>(Be16(c)) - 2;
                c += 2;
                for (;;) {
                    const uint8_t b0 = *c++;
                    const uint32_t tabIndex = ((b0 & 15u) << 1) + (b0 >> 4);
                    if (tabIndex >= 4) { supported = false; return 0; }
                    const uint8_t* bits = c;
                    uint32_t count = 0;
                    for (int i = 0; i < 16; ++i) count += *c++;
                    if (count > 256) { supported = false; return 0; }
                    HuffTab& h = huff[tabIndex];
                    h.vij = c;
                    c += count;
                    // __THPHuffGenerateSizeTable / CodeTable / DecoderTables
                    uint8_t size[257];
                    uint16_t code[257];
                    uint32_t p = 0;
                    for (uint32_t l = 1; l <= 16; ++l)
                        for (uint32_t i = bits[l - 1]; i != 0; --i) size[p++] = static_cast<uint8_t>(l);
                    size[p] = 0;
                    {
                        uint32_t k = 0;
                        uint16_t next = 0;
                        uint32_t si = size[0];
                        while (size[k]) {
                            while (size[k] == si) code[k++] = next++;
                            next = static_cast<uint16_t>(next << 1);
                            ++si;
                            if (si > 17) break; // unreachable for a size table built above
                        }
                    }
                    p = 0;
                    for (uint32_t l = 1; l <= 16; ++l) {
                        if (bits[l - 1]) {
                            h.valPtr[l] = static_cast<int32_t>(p) - code[p];
                            p += bits[l - 1];
                            h.maxCode[l] = code[p - 1];
                        } else {
                            h.maxCode[l] = -1;
                            h.valPtr[l] = -1;
                        }
                    }
                    h.maxCode[17] = 0xFFFFF;
                    memset(h.look, 0, sizeof(h.look));
                    for (uint32_t k = 0; k < count; ++k) {
                        const uint32_t l = size[k];
                        if (l > 9) break;
                        const uint32_t first = static_cast<uint32_t>(code[k]) << (9u - l);
                        const uint16_t entry = static_cast<uint16_t>((l << 8) | h.vij[k]);
                        for (uint32_t i = 0; i < (1u << (9u - l)) && first + i < 512u; ++i) h.look[first + i] = entry;
                    }
                    validHuffman |= 1u << tabIndex;
                    length -= 17 + static_cast<int32_t>(count);
                    if (length == 0) break;
                    if (length < 0) return kBadSyntax; // the original would run on; a valid stream never gets here
                }
            } else if (marker == 0xC0) { // __THPReadFrameHeader
                c += 2;
                if (*c++ != 8) return kBadPrecision;
                height = Be16(c);
                c += 2;
                width = Be16(c);
                c += 2;
                if (*c++ != 3) return kBadComponents;
                for (int i = 0; i < 3; ++i) {
                    ++c;
                    const uint8_t sampling = *c++;
                    if ((i == 0 && sampling != 0x22) || (i > 0 && sampling != 0x11)) return kBadSampling;
                    comp[i].quantSel = *c++;
                }
            } else {
                return kUnsupportedMarker;
            }
        } else if (marker <= 0xDF) {
            if (marker == 0xDD) { // __THPRestartDefinition
                restart = true;
                c += 2;
                restartInterval = Be16(c);
                c += 2;
            } else if (marker == 0xDB) { // __THPReadQuantizationTable
                int32_t length = static_cast<int32_t>(Be16(c)) - 2;
                c += 2;
                for (;;) {
                    const uint32_t id = *c++;
                    if (id >= 3) { supported = false; return 0; }
                    float temp[64];
                    for (int i = 0; i < 64; ++i) temp[tables.naturalOrder[i]] = static_cast<float>(*c++);
                    int i = 0;
                    for (int row = 0; row < 8; ++row)
                        for (int col = 0; col < 8; ++col, ++i)
                            quant[id][i] = static_cast<float>(static_cast<double>(temp[i]) * tables.aan[row] * tables.aan[col]);
                    length -= 65;
                    if (length == 0) break;
                    if (length < 0) return kBadSyntax;
                }
            } else if (marker == 0xDA) { // __THPReadScaneHeader
                c += 2;
                if (*c++ != 3) return kBadComponents;
                for (int i = 0; i < 3; ++i) {
                    ++c;
                    const uint8_t t = *c++;
                    comp[i].dcSel = static_cast<uint8_t>(t >> 4);
                    comp[i].acSel = static_cast<uint8_t>(t & 15);
                    if ((validHuffman & (1u << (t >> 4))) == 0) return kBadTable;
                    if ((validHuffman & (1u << ((t & 15) + 1))) == 0) return kBadTable;
                }
                c += 3;
                allDone = true;
            } else if (marker == 0xD8) {
                // start of image
            } else {
                return kUnsupportedMarker;
            }
        } else {
            if (marker <= 0xEF || marker == 0xFE) c += Be16(c);
            else return kUnsupportedMarker;
        }
    }

    frame.width = width;
    frame.height = height;
    // What this decoder covers; anything else goes to the translated original.
    if (width == 0 || (width & 15u) != 0 || width > 1024 || height > 1024) { supported = false; return 0; }
    for (int i = 0; i < 3; ++i) {
        if (comp[i].quantSel >= 3 || comp[i].dcSel >= 2 || comp[i].acSel >= 2) { supported = false; return 0; }
        if ((validHuffman & (1u << (comp[i].dcSel << 1))) == 0 || (validHuffman & (1u << ((comp[i].acSel << 1) + 1))) == 0) {
            supported = false; // the original's check lets this through and decodes with an unset table
            return 0;
        }
    }

    const HuffTab& ydc = huff[comp[0].dcSel << 1];
    const HuffTab& yac = huff[(comp[0].acSel << 1) + 1];
    const HuffTab& udc = huff[comp[1].dcSel << 1];
    const HuffTab& uac = huff[(comp[1].acSel << 1) + 1];
    const HuffTab& vdc = huff[comp[2].dcSel << 1];
    const HuffTab& vac = huff[(comp[2].acSel << 1) + 1];
    const float* qy = quant[comp[0].quantSel];
    const float* qu = quant[comp[1].quantSel];
    const float* qv = quant[comp[2].quantSel];
    const uint8_t* natural = tables.naturalOrder;

    Bits bits;
    bits.p = c;
    const uint32_t mcusPerRow = width / 16u;
    const uint32_t cw = width / 2u;
    uint32_t mcusLeft = restartInterval & 0xFFFFu;
    alignas(16) int16_t block[6][64];
    for (uint32_t y = 0; y < height; y += 16) {
        for (uint32_t mcu = 0; mcu < mcusPerRow; ++mcu) {
            const bool f0 = DecodeBlock(bits, ydc, yac, comp[0], natural, block[0]);
            const bool f1 = DecodeBlock(bits, ydc, yac, comp[0], natural, block[1]);
            const bool f2 = DecodeBlock(bits, ydc, yac, comp[0], natural, block[2]);
            const bool f3 = DecodeBlock(bits, ydc, yac, comp[0], natural, block[3]);
            const bool f4 = DecodeBlock(bits, udc, uac, comp[1], natural, block[4]);
            const bool f5 = DecodeBlock(bits, vdc, vac, comp[2], natural, block[5]);
            // Luma: four tile rows of width/8 tiles per MCU row; a block's upper half is one tile,
            // its lower half the tile below (width * 4 bytes further).
            uint8_t* const y0 = tileY + mcu * 64u;
            InverseBlock(block[0], f0, qy, y0, y0 + width * 4u);
            InverseBlock(block[1], f1, qy, y0 + 32, y0 + 32 + width * 4u);
            InverseBlock(block[2], f2, qy, y0 + width * 8u, y0 + width * 12u);
            InverseBlock(block[3], f3, qy, y0 + 32 + width * 8u, y0 + 32 + width * 12u);
            uint8_t* const u0 = tileU + mcu * 32u;
            InverseBlock(block[4], f4, qu, u0, u0 + cw * 4u);
            uint8_t* const v0 = tileV + mcu * 32u;
            InverseBlock(block[5], f5, qv, v0, v0 + cw * 4u);
            if (restart) {
                mcusLeft = (mcusLeft - 1u) & 0xFFFFu;
                if (mcusLeft == 0) {
                    mcusLeft = restartInterval & 0xFFFFu;
                    bits.AlignToByte();
                    comp[0].predDC = comp[1].predDC = comp[2].predDC = 0;
                }
            }
        }
        tileY += width * 16u;
        tileU += cw * 8u;
        tileV += cw * 8u;
    }
    return kOk;
}

} // namespace np_thp

#endif
