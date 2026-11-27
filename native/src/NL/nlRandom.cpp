#include "NL/nlMath.h"

#include "Game/Core/mtRandom.h"

unsigned int nlDefaultSeed = 0x12345678;

#ifdef NP_NATIVE
// NP (src/Game/NpSimThread.cpp, MSC_SIMT_PROBE): who draws from which random-number generator
extern "C" int gNpSimtProbe;
extern "C" void NpSimtRandom(const unsigned int* seed);
// NP (src/Game/NpSimThread.cpp, MSC_RENDER_RNG=2): while the switch is on, a draw made inside a simulation step from nlDefaultSeed / gEffectsRandomSeed is served from the
// simulation's own copy; the globals are never swapped. gNpRngRedirect is 0 unless the switch is 2 (one predictable branch per draw).
extern "C" int gNpRngRedirect;
extern "C" unsigned int* NpRngSeedFor(unsigned int* seed);
#endif

float nlRandomf(float fMin, float fMax, unsigned int* pSeed)
{
    return fMin + nlRandomf(fMax - fMin, pSeed);
}

float nlRandomf(float fMax, unsigned int* pSeed)
{
    return (1.0f / 2147483647.0f) * fMax * (float)nlRandom(0x7FFFFFFF, pSeed);
}

unsigned int nlRandom(unsigned int range, unsigned int* seed)
{
    if (range == 0)
    {
        return 0;
    }

    unsigned int next;
    unsigned int temp;
#ifdef NP_NATIVE
    if (gNpSimtProbe > 0) NpSimtRandom(seed);
    if (gNpRngRedirect != 0) seed = NpRngSeedFor(seed);
#endif

    unsigned int result = *seed % range;
    next = *seed ^ 0x1d872b41;
    temp = next ^ (next >> 5);
    *seed = temp ^ (next ^ (temp << 0x1b));
    return result;
}

void nlSetRandomSeed(unsigned int value, unsigned int* seed)
{
    unsigned int temp;
    unsigned int next;

#ifdef NP_NATIVE
    if (gNpRngRedirect != 0) seed = NpRngSeedFor(seed);
#endif
    *seed = value;

    next = *seed ^ 0x1d872b41;
    temp = next ^ (next >> 5);

    *seed = temp ^ (next ^ (temp << 0x1b));
}

void nlInitRandom()
{
    seedMT(0x1105);
}
