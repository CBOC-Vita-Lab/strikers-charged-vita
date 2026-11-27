// np_gx_seqfill.h: the bytes of a material program's unindexed draw, `count` vertices of `comps` 16-bit attribute indices that all equal the vertex's number, big-endian:
// what `for (i = 0; i < count; ++i) { WGPIPE.us = i; ... comps times }` writes, without a cursor check per index. Host test: renderer/tests/gx_seqfill.cpp (also run under qemu-arm).
#ifndef NP_GX_SEQFILL_H
#define NP_GX_SEQFILL_H

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

static inline void np_gx_seq16_fill(unsigned char* p, unsigned int count, unsigned int comps) {
    unsigned int i = 0;
#if defined(__ARM_NEON)
    if ((comps == 2 || comps == 3) && !(reinterpret_cast<__SIZE_TYPE__>(p) & 1)) {
        static const unsigned short kFirst[8] = {0, 1, 2, 3, 4, 5, 6, 7};
        uint16x8_t v = vld1q_u16(kFirst);
        const uint16x8_t step = vdupq_n_u16(8);
        for (; i + 8 <= count; i += 8) {
            const uint16x8_t be = vreinterpretq_u16_u8(vrev16q_u8(vreinterpretq_u8_u16(v)));
            if (comps == 3) {
                uint16x8x3_t t;
                t.val[0] = t.val[1] = t.val[2] = be;
                vst3q_u16(reinterpret_cast<uint16_t*>(p), t);
                p += 48;
            } else {
                uint16x8x2_t t;
                t.val[0] = t.val[1] = be;
                vst2q_u16(reinterpret_cast<uint16_t*>(p), t);
                p += 32;
            }
            v = vaddq_u16(v, step);
        }
    }
#endif
    for (; i < count; ++i)
        for (unsigned int k = 0; k < comps; ++k) {
            p[0] = static_cast<unsigned char>(i >> 8);
            p[1] = static_cast<unsigned char>(i);
            p += 2;
        }
}

#endif
