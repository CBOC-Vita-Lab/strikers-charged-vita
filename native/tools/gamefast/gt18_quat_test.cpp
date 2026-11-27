// MSC_GT18_QUAT: nlQuatNLerp and nlQuatRenormalize of the real math.cpp / platqmath.cpp / nlMath.cpp / mtx_native.c, switch 0 (the original code) against switch 1 (no calls) and
// against switch 2 (verify: its own answer is the original's, its counter must stay 0), bit for bit, on random and awkward inputs (zeros, -0, denormals, huge, infinities, NaN),
// both signs of the dot, t from 0 to 1 and outside, with FPSCR.FZ clear and set on ARM. Run: run_gt18_quat_test.py (x86 -m32 -msse2 as the host build, or armhf under qemu-arm).
// MSC_GT18_BREAK=1 in the environment is the proof mutant: the test must then FAIL.
#include "NL/nlMath.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" const unsigned int __float_huge[1] = { 0x7F800000 };
extern "C" const unsigned int __float_nan[1] = { 0x7FC00000 };
// referenced by the linked sources, not used by this test
void nlMultPosVectorMatrix(nlVector3&, const nlVector3&, const nlMatrix4&) {}
nlMatrix4& nlTransposeMatrix(nlMatrix4& o, const nlMatrix4&) { return o; }
extern int gNpGt18Quat;
extern int NpGt18QuatInit();

static uint32_t rngState = 0x2545F491u;
static uint32_t Rng()
{
    rngState ^= rngState << 13;
    rngState ^= rngState >> 17;
    rngState ^= rngState << 5;
    return rngState;
}

static float Awkward()
{
    static const uint32_t v[] = { 0, 0x80000000, 1, 0x80000001, 0x007FFFFF, 0x807FFFFF, 0x00800000, 0x80800000, 0x3F800000, 0xBF800000, 0x7F000000, 0x7EFFFFFF, 0xFEFFFFFF,
        0x7F7FFFFF, 0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC00001, 0x7F800001, 0x3F000000, 0xBF000000, 0x3F3504F3, 0x20000000, 0x1E000000 };
    uint32_t u = v[Rng() % (sizeof(v) / sizeof(v[0]))];
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static void Fill(nlQuaternion& q, int kind)
{
    float* c = &q.x;
    switch (kind & 7)
    {
    case 0:   // roughly unit
        for (int k = 0; k < 4; k++)
            c[k] = (float)(int)(Rng() & 0xFFFF) / 32768.0f - 1.0f;
        break;
    case 1:   // decoded keys, zeros included
        for (int k = 0; k < 4; k++)
            c[k] = (Rng() & 3) ? (int16_t)(Rng() & 0xFFF0) * (1.0f / 32768.0f) : 0.0f;
        break;
    case 2:   // any bit pattern
        for (int k = 0; k < 4; k++)
        {
            uint32_t u = Rng();
            memcpy(&c[k], &u, 4);
        }
        break;
    case 3:   // awkward values
        for (int k = 0; k < 4; k++)
            c[k] = (Rng() & 1) ? Awkward() : (float)(int)(Rng() & 0xFF) / 128.0f - 1.0f;
        break;
    case 4:   // tiny and huge magnitudes
        for (int k = 0; k < 4; k++)
            c[k] = ((float)(int)(Rng() & 0xFFFF) / 32768.0f - 1.0f) * ((Rng() & 1) ? 1.0e-19f : 1.0e19f);
        break;
    default:
        for (int k = 0; k < 4; k++)
            c[k] = (float)(int)(Rng() & 0xFFFF) / 16384.0f - 2.0f;
        break;
    }
}

static uint32_t SetFz(int on)
{
#ifdef __arm__
    uint32_t v;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(v));
    v = on ? (v | (1u << 24)) : (v & ~(1u << 24));
    __asm__ volatile("vmsr fpscr, %0" : : "r"(v));
    return v;
#else
    (void)on;
    return 0;
#endif
}

// bit equality, except that two NaNs match whatever their payload or sign (which operand's payload a NaN result keeps is the compiler's choice, not an arithmetic result)
static bool Same(const nlQuaternion& a, const nlQuaternion& b)
{
    const uint32_t* x = (const uint32_t*)&a;
    const uint32_t* y = (const uint32_t*)&b;
    for (int k = 0; k < 4; k++)
    {
        const bool nx = (x[k] & 0x7FFFFFFFu) > 0x7F800000u, ny = (y[k] & 0x7FFFFFFFu) > 0x7F800000u;
        if (nx != ny || (!nx && x[k] != y[k]))
            return false;
    }
    return true;
}

static long fails;
static void Fail(const char* what, const nlQuaternion& a, const nlQuaternion& b)
{
    if (fails++ < 5)
    {
        const uint32_t* x = (const uint32_t*)&a;
        const uint32_t* y = (const uint32_t*)&b;
        printf("FAIL %s: %08x %08x %08x %08x / %08x %08x %08x %08x\n", what, x[0], x[1], x[2], x[3], y[0], y[1], y[2], y[3]);
    }
}

int main()
{
    setvbuf(stdout, 0, _IONBF, 0);
    NpGt18QuatInit();   // reads MSC_GT18_BREAK
    long nlerps = 0, renorms = 0, renormDeclined = 0;
    for (int fz = 0; fz < 2; fz++)
    {
        SetFz(fz);
        for (int i = 0; i < 4000000; i++)
        {
            nlQuaternion a, b, r0, r1, r2;
            Fill(a, i);
            Fill(b, (i >> 3) + (i & 1));
            float t = (Rng() & 7) == 0 ? Awkward() : (float)(Rng() & 0xFFFF) / 65535.0f * ((Rng() & 15) == 0 ? 3.0f : 1.0f);
            gNpGt18Quat = 0;
            nlQuatNLerp(r0, a, b, t);
            gNpGt18Quat = 1;
            nlQuatNLerp(r1, a, b, t);
            gNpGt18Quat = 2;
            nlQuatNLerp(r2, a, b, t);
            nlerps++;
            if (!Same(r0, r1))
                Fail("nlerp 1", r0, r1);
            if (!Same(r0, r2))
                Fail("nlerp 2", r0, r2);
            // aliasing as the game calls it: out = q1
            nlQuaternion c0 = a, c1 = a;
            gNpGt18Quat = 0;
            nlQuatNLerp(c0, c0, b, t);
            gNpGt18Quat = 1;
            nlQuatNLerp(c1, c1, b, t);
            if (!Same(c0, c1))
                Fail("nlerp aliased", c0, c1);
            // renormalize
            nlQuaternion q0 = a, q1 = a, q2 = a;
            gNpGt18Quat = 0;
            const bool d0 = nlQuatRenormalize(q0);
            gNpGt18Quat = 1;
            const bool d1 = nlQuatRenormalize(q1);
            gNpGt18Quat = 2;
            const bool d2 = nlQuatRenormalize(q2);
            if (d0 != d1 || d0 != d2)
                Fail("renormalize declined", a, a);
            if (d0)
            {
                renorms++;
                if (!Same(q0, q1))
                    Fail("renormalize 1", q0, q1);
                if (!Same(q0, q2))
                    Fail("renormalize 2", q0, q2);
            }
            else
                renormDeclined++;
        }
        printf("FZ=%d done\n", fz);
    }
    SetFz(0);
    printf("nlerp compared %ld, renormalize compared %ld declined %ld\n", nlerps, renorms, renormDeclined);
    printf(fails ? "FAILED %ld\n" : "ok\n", fails);
    return fails != 0;
}
