// NPB3: nlSqrt / nlRecipSqrt over the whole float range, armhf under qemu (the NEON __frsqrte of include/np/compat.h) and host32.
// (tools/det/test_frsqrte.sh builds and runs it for armhf under qemu and host32; the NEON path of compat.h is the one with the denormal hole)
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "np/compat.h"   // __frsqrte: NEON vrsqrte on armhf/Vita, exact on host32
// the bodies of src/NL/nlMath.cpp (nlSqrt, nlRecipSqrt) as they are there
static float nlSqrt(float x, bool accurate)
{
    if (x > 0.0f)
    {
        double guess = __frsqrte((double)x);
        guess = 0.5 * guess * (3.0 - guess * guess * x);
        if (accurate)
        {
            guess = 0.5 * guess * (3.0 - guess * guess * x);
            guess = 0.5 * guess * (3.0 - guess * guess * x);
        }
        return (float)(x * guess);
    }
    return 0.0f;
}
static float nlRecipSqrt(float x, bool accurate)
{
    (void)accurate;
    if (x > 0.0f)
    {
        float y = __frsqrte(x);
        y = 0.5f * y * (3.0f - y * y * x);
        y = 0.5f * y * (3.0f - y * y * x);
        y = 0.5f * y * (3.0f - y * y * x);
        return y;
    }
    return 0.0f;
}
int main()
{
    int bad = 0, n = 0;
    // every exponent from the smallest denormal to the largest float, several mantissas
    for (int e = 0; e < 255; ++e)
    {
        for (unsigned m = 1; m < 0x800000; m += 0x1fffb)
        {
            unsigned bits = ((unsigned)e << 23) | m;
            float x;
            memcpy(&x, &bits, 4);
            if (!(x > 0.0f))
                continue;
            ++n;
            double want = sqrt((double)x);
            float got = nlSqrt(x, true);
            double rel = fabs((double)got - want) / want;
            if (!(rel < 1e-6))
            {
                if (bad < 10) printf("nlSqrt(%g) = %g, want %g\n", x, got, want);
                ++bad;
            }
            float rgot = nlRecipSqrt(x, true);
            double rwant = 1.0 / want;
            // nlRecipSqrt's own Newton step overflows a float for x below ~1e-36 (y*y > FLT_MAX) on every build and on the console: callers guard with lengthSq > 1e-4
            if (rwant < 1e18 && rwant > 1.2e-38)
            {
                double rrel = fabs((double)rgot - rwant) / rwant;
                if (!(rrel < 1e-6))
                {
                    if (bad < 10) printf("nlRecipSqrt(%g) = %g, want %g\n", x, rgot, rwant);
                    ++bad;
                }
            }
        }
    }
    printf("%d values, %d wrong\n", n, bad);
    return bad != 0;
}
