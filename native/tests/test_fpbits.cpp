// Host test of np/fpbits.h (MSC_FPBITS): every helper against the float expression it replaces, for ALL 2^32 float bit patterns (IEEE mode: denormals, NaNs, infinities, both zeros), for the
// constants the game sites use (0.001f, 0.0001f) and the neighbours of each (the bit pattern +-1, +-2 as constants), then the same with denormals-are-zero set (x86 DAZ/FTZ, which models the
// Vita's FZ=1 for an operand that came from memory): the differences there are counted and printed (they are only denormal operands). Mutant: -DNP_FPBITS_BREAK.
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <xmmintrin.h>
#include "../include/np/fpbits.h"
int gNpFpBits = 1;
int NpFpBitsInit() { return 1; }

static float F(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }

template <uint32_t CB>
static unsigned long long Run(unsigned long long* denormDiff) {
    const float c = F(CB);
    unsigned long long bad = 0, dd = 0;
    for (uint64_t i = 0; i <= 0xFFFFFFFFull; ++i) {
        const uint32_t b = (uint32_t)i;
        volatile float vx = F(b);   // (volatile: the compiler must not fold the expressions)
        const float x = vx;
        const bool den = (b & 0x7F800000u) == 0 && (b & 0x007FFFFFu) != 0;
        const bool a1 = np_fp::AbsLt(x, CB) != (fabsf(x) < c);
        const bool a2 = np_fp::AbsGt(x, CB) != (fabsf(x) > c);
        const bool a3 = np_fp::IsPos(x) != (x > 0.0f);
        const bool a4 = np_fp::IsZero(x) != (x == 0.0f);
        const bool a5 = np_fp::NotZero(x) != (x != 0.0f);
        const unsigned n = a1 + a2 + a3 + a4 + a5;
        if (den) dd += n; else bad += n;
    }
    *denormDiff = dd;
    return bad;
}

int main() {
    unsigned long long total = 0, ddTotal = 0;
    const uint32_t k1 = NP_FPBITS_OF(0.001f), k2 = NP_FPBITS_OF(0.0001f);
    printf("bits(0.001f)=%08x bits(0.0001f)=%08x\n", k1, k2);
    unsigned long long dd;
    total += Run<0x3A83126Fu>(&dd); ddTotal += dd;           // 0.001f
    total += Run<0x38D1B717u>(&dd); ddTotal += dd;           // 0.0001f
    total += Run<0x3A83126Eu>(&dd); ddTotal += dd;           // neighbours
    total += Run<0x3A831270u>(&dd); ddTotal += dd;
    total += Run<0x00800000u>(&dd); ddTotal += dd;           // the smallest normal
    total += Run<0x7F7FFFFFu>(&dd); ddTotal += dd;           // the largest finite
    printf("fpbits IEEE mode: all 2^32 patterns x 6 constants x 5 helpers: %llu mismatches (denormal operands included: %llu)\n", total, ddTotal);
    // FZ/DAZ model: how the float expressions behave when a denormal operand is flushed (the integer forms do not flush)
    _mm_setcsr(_mm_getcsr() | 0x8040);   // FTZ + DAZ
    unsigned long long dazDiff = 0, dazNormalDiff = 0;
    for (uint64_t i = 0; i <= 0xFFFFFFFFull; ++i) {
        const uint32_t b = (uint32_t)i;
        volatile float vx = F(b);
        const float x = vx;
        const bool den = (b & 0x7F800000u) == 0 && (b & 0x007FFFFFu) != 0;
        const unsigned n = (np_fp::IsPos(x) != (x > 0.0f)) + (np_fp::IsZero(x) != (x == 0.0f)) + (np_fp::NotZero(x) != (x != 0.0f)) + (np_fp::AbsLt(x, k1) != (fabsf(x) < F(k1)));
        if (den) dazDiff += n; else dazNormalDiff += n;
    }
    printf("fpbits DAZ model: differences only on denormal operands: %llu (non-denormal operands: %llu)\n", dazDiff, dazNormalDiff);
    return (total == 0 && dazNormalDiff == 0) ? 0 : 1;
}
