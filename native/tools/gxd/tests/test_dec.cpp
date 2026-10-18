// GXD_DEC_NEON: every NEON decode kernel of vgx_state.cpp (16-bit-indexed S16 and F32 attributes, 2 and 3 components, big- and little-endian array elements)
// against the scalar kernel DecodeAttr<2, type, N, false, LE> on random arrays, indices (including out of range ones), counts, strides and misaligned
// array / stream / output addresses; the 'out of range' flag and every output byte must be equal. Also IndexRange16 and MatrixIndexColumn against plain loops.
// The kernels are cut out of the source file (no copy is kept here); ARM only (NEON), run under qemu-arm:
//   V=<vgx dir>; sed -n '/^template <int IDX>$/,/^\/\/ MSC_VGX_GXD with GXD_VERIFY/p' $V/src/vgx_state.cpp > dec_snip.inc
//   arm-linux-gnueabihf-g++ -std=c++20 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -I$V/src -I. test_dec.cpp -o t_dec
//   qemu-arm -cpu cortex-a9 -L /usr/arm-linux-gnueabihf ./t_dec        (result of 38942a0: 8 kernels x 3000 arrays, IndexRange16 4000, MatrixIndexColumn 4000: 0 bad;
//   a deliberately broken big-endian F32 kernel gives 5663 bad, so the test sees errors)
#include "vgx_common.h"
#include <arm_neon.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
namespace vgx {
namespace {
enum { C_U8 = 0, C_S8 = 1, C_U16 = 2, C_S16 = 3, C_F32 = 4 };
const uint8_t kZero[64] = {};
#include "dec_snip.inc"
} // namespace
} // namespace vgx
using namespace vgx;
static uint32_t rs = 12345;
static uint32_t rnd() { rs = rs * 1664525u + 1013904223u; return rs >> 8; }

template <int TYPE, int N, bool LE, typename K>
static int check(const char* name, K kernel) {
    int bad = 0;
    const uint32_t elem = TYPE == C_F32 ? 4u * N : 2u * N;
    for (int it = 0; it < 3000; ++it) {
        const uint32_t count = rnd() % 41;                       // 0 .. 40 vertices (odd counts exercise the scalar tail)
        const uint32_t ss = 2 + (rnd() % 3) * 2 + (rnd() % 2 ? 2 : 0); // 2 / 4 / 6 / 8 / 10 bytes of stream per vertex
        const uint32_t astride = elem + (rnd() % 4) * 2;          // element stride in the array (>= element size)
        const uint32_t nel = 1 + rnd() % 64;                      // elements in the array
        const uint32_t os = 12 + (rnd() % 3) * 4;                 // output stride
        const uint32_t asize = nel * astride;
        std::vector<uint8_t> arr(asize + 16), stream(count * ss + 16), o1(count * os + 32, 0xA5), o2(count * os + 32, 0xA5);
        for (auto& x : arr) x = static_cast<uint8_t>(rnd());
        if (TYPE == C_F32 && (it & 3) == 0)                      // NaN / denormal patterns must survive
            for (size_t i = 0; i + 4 <= arr.size(); i += 4) if (rnd() % 3 == 0) { arr[i + (LE ? 3 : 0)] = 0x7F; arr[i + (LE ? 2 : 1)] |= 0x80; }
        for (uint32_t v = 0; v < count; ++v) {
            uint32_t idx = rnd() % (nel + (it % 5 == 0 ? 4 : 0));      // some runs read past the array: the element then reads as zeros and the flag is set
            stream[v * ss] = static_cast<uint8_t>(idx >> 8);
            stream[v * ss + 1] = static_cast<uint8_t>(idx);
            for (uint32_t b = 2; b < ss; ++b) stream[v * ss + b] = static_cast<uint8_t>(rnd());
        }
        const uint32_t misA = rnd() % 2, misS = 0, misO = (rnd() % 2) * 4;   // array 2-aligned (as in the game's data), output 4-aligned
        const uint8_t* adata = arr.data() + misA * 2;
        const uint32_t maxOff = asize - elem;                      // as DecodeVertices: highest offset at which a whole element still fits
        const float scale = 1.0f / static_cast<float>(1u << (rnd() % 12));
        const uint32_t b1 = kernel(stream.data() + misS, o1.data() + misO, count, ss, os, adata, astride, maxOff, scale);
        const uint32_t b2 = DecodeAttr<2, TYPE, N, false, LE>(stream.data() + misS, o2.data() + misO, count, ss, os, adata, astride, maxOff, TYPE == C_F32 ? 1.0f : scale);
        if (b1 != b2 || std::memcmp(o1.data(), o2.data(), o1.size()) != 0) {
            if (++bad <= 3) std::printf("%s: iteration %d differs (count %u ss %u astride %u os %u flags %u %u)\n", name, it, count, ss, astride, os, b1, b2);
        }
    }
    return bad;
}

int main() {
    int bad = 0, kernels = 0;
#define K(T, N, LE, FN) bad += check<T, N, LE>(#FN "<" #N "," #LE ">", &FN<N, LE>); ++kernels;
    K(C_S16, 2, true, DecodeS16Idx16Neon) K(C_S16, 2, false, DecodeS16Idx16Neon) K(C_S16, 3, true, DecodeS16Idx16Neon) K(C_S16, 3, false, DecodeS16Idx16Neon)
    K(C_F32, 2, true, DecodeF32Idx16Neon) K(C_F32, 2, false, DecodeF32Idx16Neon) K(C_F32, 3, true, DecodeF32Idx16Neon) K(C_F32, 3, false, DecodeF32Idx16Neon)
    // IndexRange16 against the plain loop
    int rbad = 0;
    for (int it = 0; it < 4000; ++it) {
        const uint32_t count = 1 + rnd() % 70, ss = 2 + (rnd() % 4) * 2;
        std::vector<uint8_t> s(count * ss + 8);
        for (auto& x : s) x = static_cast<uint8_t>(rnd());
        if (it % 3 == 0) for (uint32_t v = 0; v < count; ++v) s[v * ss] = 0;       // small indices
        uint32_t mn, mx, lo = 0xFFFF, hi = 0;
        IndexRange16(s.data() + 1 - 1, count, ss, mn, mx);
        for (uint32_t v = 0; v < count; ++v) { const uint32_t i = be16(s.data() + v * ss); if (i < lo) lo = i; if (i > hi) hi = i; }
        if (mn != lo || mx != hi) ++rbad;
    }
    // MatrixIndexColumn against the plain loop
    int mbad = 0;
    for (int it = 0; it < 4000; ++it) {
        const uint32_t count = rnd() % 70, ss = 1 + rnd() % 12, os = 12 + (rnd() % 3) * 4;
        std::vector<uint8_t> s(count * ss + 8), o1(count * os + 16, 0x5A), o2(count * os + 16, 0x5A);
        for (auto& x : s) x = static_cast<uint8_t>(rnd());
        const uint32_t m1 = MatrixIndexColumn(s.data(), o1.data(), count, ss, os);
        uint32_t m2 = 0;
        for (uint32_t v = 0; v < count; ++v) { if (s[v * ss] > m2) m2 = s[v * ss]; const float f = static_cast<float>(s[v * ss]); std::memcpy(o2.data() + v * os, &f, 4); }
        if (m1 != m2 || std::memcmp(o1.data(), o2.data(), o1.size()) != 0) ++mbad;
    }
    std::printf("%d kernels x 3000 random arrays: %d bad; IndexRange16 4000: %d bad; MatrixIndexColumn 4000: %d bad\n", kernels, bad, rbad, mbad);
    return bad + rbad + mbad != 0;
}
