// np/fpbits.h (1.3 cpuexact13, MSC_FPBITS, default 1): float comparisons as integer tests on the bit pattern, for the hottest compare-and-branch sites of the game thread.
//
// On the Cortex-A9 a float compare that decides a branch is vcmp + vmrs APSR_nzcv: the in-order core waits for the VFP pipeline to deliver the flags. The same answer from the bits needs one
// vmov core<-VFP (no wait for the flags) and integer ALU work. Every helper below returns exactly what the float expression in its comment returns, for EVERY one of the 2^32 bit
// patterns (tests/test_fpbits.cpp runs them all against the original expressions; the denormal cases are exact too, in the host's IEEE mode):
//   IsPos(x)         x > 0.0f          (-0, NaN, negatives false; +inf true)
//   IsZero(x)        x == 0.0f         (+-0 true, NaN false)
//   NotZero(x)       x != 0.0f         (NaN true)
//   AbsLt(x, c)      fabsf(x) < c      (c = bits of a positive finite constant; NaN false; inf false)
//   AbsGt(x, c)      fabsf(x) > c      (NaN false; inf true)
// The Vita runs with FPSCR.FZ=1 (console [fpscr] line: 0x63000010): a VFP result is never a denormal there, and every operand these sites compare is a computed value (a sum, a dot product,
// a difference) or a weight whose compare outcome is the same for a denormal and for zero (AbsLt). MSC_FPBITS=0 restores the float compares everywhere.
#pragma once
#include <stdint.h>
#include <string.h>

namespace np_fp {

inline uint32_t Bits(float x)
{
    uint32_t b;
    memcpy(&b, &x, sizeof b);
    return b;
}
#ifdef NP_FPBITS_BREAK   // (test mutant)
inline bool IsPos(float x) { return (uint32_t)(Bits(x) - 1u) <= 0x7F800000u; }
#else
inline bool IsPos(float x) { return (uint32_t)(Bits(x) - 1u) < 0x7F800000u; }
#endif
inline bool IsZero(float x) { return (Bits(x) << 1) == 0u; }
inline bool NotZero(float x) { return (Bits(x) << 1) != 0u; }
inline bool AbsLt(float x, uint32_t cbits) { return (Bits(x) & 0x7FFFFFFFu) < cbits; }
inline bool AbsGt(float x, uint32_t cbits) { return (uint32_t)((Bits(x) & 0x7FFFFFFFu) - cbits - 1u) < 0x7F800000u - cbits; }

}  // namespace np_fp

extern int gNpFpBits;   // MSC_FPBITS, NL/math.cpp
int NpFpBitsInit();
inline bool NpFpBitsOn() { return (gNpFpBits < 0 ? NpFpBitsInit() : gNpFpBits) != 0; }

// the bit patterns of the constants the sites compare against (c must be a positive finite float)
#define NP_FPBITS_OF(c) (__builtin_bit_cast(uint32_t, (float)(c)))
// the call-site forms: the bit test when MSC_FPBITS is on, the original float expression otherwise
#define NP_FP_ABSLT(x, c) (NpFpBitsOn() ? np_fp::AbsLt((x), NP_FPBITS_OF(c)) : fabsf(x) < (c))
#define NP_FP_ABSGT(x, c) (NpFpBitsOn() ? np_fp::AbsGt((x), NP_FPBITS_OF(c)) : fabsf(x) > (c))
#define NP_FP_ISZERO(x) (NpFpBitsOn() ? np_fp::IsZero(x) : (x) == 0.0f)
#define NP_FP_NOTZERO(x) (NpFpBitsOn() ? np_fp::NotZero(x) : (x) != 0.0f)
