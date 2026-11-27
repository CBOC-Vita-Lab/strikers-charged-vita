// be25 MSC_VGX_FASTCMP=0|1|2 (default 0 in the code; the Vita plan sets 1): the equality test of the uniform blocks (BlockDiffers, the skin shadow) without the C library's memcmp.
//
// Why: the first console run of the goal cut-scene (17 Nov, bephbase) put 18 us a draw in the "uniforms" phase (UseProgram and the uniform blocks), 6 ms a picture at 340 draws. The
// back end compares every block the front end names with the copy the program holds (MSC_VGX_UNIDUP=2) and most of them are equal, so the whole block is read: the host profile of the same
// cut-scene shows ~570 memcmp calls of some 900 bytes a picture. The console's newlib memcmp is a 10-instruction loop for every 4 bytes (arm-vita-eabi disassembly of the 17 Nov ELF:
// ldr, ldr, cmp, bne, subs, cmp, bhi ... per word), about 2 cycles a byte: ~1.5 M cycles (3 ms) a picture. This compares 128 bytes per test with NEON (veor / vorr), or 64 bytes with plain
// 64-bit words where NEON is not there (the host), and gives the same answer as memcmp(a, b, bytes) == 0 for any contents.
//   0 memcmp as before;  1 this;  2 this, and memcmp beside it, every disagreement counted (fastCmpBad; the host proof: it must stay 0);
//   MSC_VGX_FASTCMP_BREAK=1 (test only): only the first half of each block is looked at, so the proof's counter must move (the negative control).
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif
namespace vgx {
namespace fastcmp {
// bytes: a multiple of 4 (uniform blocks are floats); a and b: 4-byte aligned is enough.
inline bool Equal4(const void* a, const void* b, size_t bytes, bool skipLast = false) {
    const uint32_t* x = static_cast<const uint32_t*>(a);
    const uint32_t* y = static_cast<const uint32_t*>(b);
    size_t n = bytes >> 2;
    if (skipLast) n >>= 1;   // (the negative control: only the first half is looked at)
#if defined(__ARM_NEON)
    while (n >= 32) {   // 128 bytes
        uint32x4_t d0 = veorq_u32(vld1q_u32(x), vld1q_u32(y));
        uint32x4_t d1 = veorq_u32(vld1q_u32(x + 4), vld1q_u32(y + 4));
        uint32x4_t d2 = veorq_u32(vld1q_u32(x + 8), vld1q_u32(y + 8));
        uint32x4_t d3 = veorq_u32(vld1q_u32(x + 12), vld1q_u32(y + 12));
        uint32x4_t d4 = veorq_u32(vld1q_u32(x + 16), vld1q_u32(y + 16));
        uint32x4_t d5 = veorq_u32(vld1q_u32(x + 20), vld1q_u32(y + 20));
        uint32x4_t d6 = veorq_u32(vld1q_u32(x + 24), vld1q_u32(y + 24));
        uint32x4_t d7 = veorq_u32(vld1q_u32(x + 28), vld1q_u32(y + 28));
        const uint32x4_t o = vorrq_u32(vorrq_u32(vorrq_u32(d0, d1), vorrq_u32(d2, d3)), vorrq_u32(vorrq_u32(d4, d5), vorrq_u32(d6, d7)));
        const uint32x2_t h = vorr_u32(vget_low_u32(o), vget_high_u32(o));
        if (vget_lane_u32(h, 0) | vget_lane_u32(h, 1)) return false;
        x += 32; y += 32; n -= 32;
    }
    while (n >= 4) {   // 16 bytes
        const uint32x4_t o = veorq_u32(vld1q_u32(x), vld1q_u32(y));
        const uint32x2_t h = vorr_u32(vget_low_u32(o), vget_high_u32(o));
        if (vget_lane_u32(h, 0) | vget_lane_u32(h, 1)) return false;
        x += 4; y += 4; n -= 4;
    }
#else
    while (n >= 16) {   // 64 bytes
        uint64_t p[8], q[8];
        std::memcpy(p, x, 64);
        std::memcpy(q, y, 64);
        const uint64_t o = ((p[0] ^ q[0]) | (p[1] ^ q[1]) | (p[2] ^ q[2]) | (p[3] ^ q[3])) | ((p[4] ^ q[4]) | (p[5] ^ q[5]) | (p[6] ^ q[6]) | (p[7] ^ q[7]));
        if (o) return false;
        x += 16; y += 16; n -= 16;
    }
#endif
    for (; n; --n, ++x, ++y)
        if (*x != *y) return false;
    return true;
}
}   // namespace fastcmp
}   // namespace vgx
