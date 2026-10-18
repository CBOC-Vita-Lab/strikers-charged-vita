/* np/compat.h: portability layer between the Metrowerks/PowerPC decompiled game and a native
 * ILP32 little-endian build (host32, armhf, vita). Force-included (-include np/compat.h) in every
 * translation unit by the Makefile. Keep it C and C++ compatible, header-only (the few data symbols
 * it declares are defined in platform/compat/np_compat.c). */
#ifndef NP_COMPAT_H
#define NP_COMPAT_H

#include <stddef.h>   /* NULL, size_t: MSL's headers provided them implicitly on the Wii */
#include <stdint.h>
#include <math.h>     /* host math first, so the MSL-name shims below do not clash with it */
#include <decomp.h>  /* DECL_SECTION / DECL_WEAK / ASM etc. (NP_NATIVE variants) */

#ifdef __cplusplus
extern "C" {
#endif

/* MSL math constants (bit patterns of float values), defined in platform/compat/np_compat.c.
 * The game reads them as `*(float*)&__float_nan[0]`. */
extern const unsigned int __float_nan[1];
extern const unsigned int __float_huge[1];

/* ---- Gekko / PPC intrinsics used by the game ---- */
/* glibc's math.h already declares an internal __fabs, so the name is redirected */
static inline double np_fabs(double x) { return __builtin_fabs(x); }
#define __fabs(x) np_fabs(x)

/* frsqrte is a ~12-bit estimate on the 750CL; callers refine it with Newton-Raphson, so the exact
 * reciprocal square root is a valid (and slightly better) substitute. */
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
/* NPP: on the Cortex-A9 the exact form below is vsqrt.f64 + vdiv.f64 (about 70 cycles, unpipelined); vrsqrte is the same kind of estimate as
 * the 750CL's frsqrte (8 bits here), one instruction. All three callers (nlSqrt, nlRecipSqrt, the nlACos-style polynomial) refine it with
 * Newton-Raphson steps: nlRecipSqrt and the third have three (8 -> 64 bits), nlSqrt(x, false) one (16 bits, the console's own estimate gave about 10). */
#include <arm_neon.h>
/* NPB3: vrsqrte works on a float and NEON flushes float denormals to zero: for a double x below FLT_MIN (1.18e-38; nlSqrt(velX^2 + velY^2) of a player who has
 * slowed down to 3e-20) the estimate is +inf, the Newton steps then give guess = -inf and nlSqrt returned -inf (a character's speed, then its velocity and position,
 * then the ball went inf/NaN and the match never scored again: armhf/qemu, 58 s into the match). Above FLT_MAX the float is inf and the estimate 0. Outside the normal
 * float range take the exact value (0 gives +inf like the 750CL's frsqrte; the branch is almost never taken). NaN falls through to vrsqrte (NaN). */
static inline double __frsqrte(double x)
{
    float f = (float)x;
    if (__builtin_expect(f < 1.17549435e-38f || f > 3.40282347e38f, 0)) return 1.0 / __builtin_sqrt(x);
    return (double)vget_lane_f32(vrsqrte_f32(vdup_n_f32(f)), 0);
}
#else
static inline double __frsqrte(double x) { return 1.0 / __builtin_sqrt(x); }
#endif

/* NPP: object pools carved out of `static unsigned char sXxxStorage[N * sizeof(T)]` need the alignment of T; a char array has 1 and the linker put
 * one at an odd address (LTO build: SIGBUS in `strd` in PhysicsSphere::PhysicsSphere). Metrowerks aligned these arrays on its own. */
#define NP_STORAGE_ALIGN __attribute__((aligned(16)))

static inline int _abs(int x) { return x < 0 ? -x : x; }

/* byte-reversed loads/stores (lwbrx / stwbrx): offset is in bytes */
static inline unsigned int __lwbrx(const void *base, int offset)
{
    return __builtin_bswap32(*(const unsigned int *)((const char *)base + offset));
}
static inline void __stwbrx(unsigned int value, void *base, int offset)
{
    *(unsigned int *)((char *)base + offset) = __builtin_bswap32(value);
}

/* MSL's fpclassify values: FP_NAN 1, FP_INFINITE 2, FP_ZERO 3, FP_NORMAL 4, FP_SUBNORMAL 5.
 * (glibc's __fpclassifyf has different values, so the name is redirected.) */
static inline int np_msl_fpclassifyf(float x)
{
    if (__builtin_isnan(x)) return 1;
    if (__builtin_isinf(x)) return 2;
    if (x == 0.0f) return 3;
    if (__builtin_isnormal(x)) return 4;
    return 5;
}
#define __fpclassifyf(x) np_msl_fpclassifyf(x)

#ifdef __cplusplus
}
#endif

#define __alloca(n) __builtin_alloca(n)

#endif /* NP_COMPAT_H */
