#ifndef GAME_BIT_PACKER_H
#define GAME_BIT_PACKER_H

#include "Game/AI/AiUtil.h"
#include "Game/MathHelpers.h"
#include <string.h>

class BitPacker
{
public:
    BitPacker()
        : mBitCount(0)
    {
        memset(&mBits, 0, sizeof(mBits));
    }

    void Pack(int value, int min, int max)
    {
        value = value >= min ? value : min;
        value = value <= max ? value : max;
        unsigned long range = max - min;
        unsigned int i;
        unsigned int bits = 0;
#ifdef NP_NATIVE
        // NP (NPP): the original counts to 64 and relies on the PowerPC `slw` giving 0 for shift counts 32..63, so those iterations add
        // nothing. In C++ `1 << i` with i >= 32 is undefined: clang -O2 unrolled the loop, saw the shifts by constants >= 32, and cut
        // the rest of cCharacter::GetJointPositionFuture off (it fell into the next function: SIGSEGV loading the goalie of a match).
        for (i = 0; i < 32; ++i)
#else
        for (i = 0; i < 64; ++i)
#endif
        {
#ifdef NP_NATIVE
            // NP: Metrowerks' `slw` gives 0 for a shift count of 32..63, C++ makes it undefined: clang removed everything after the
            // loop (cCharacter::GetJointPositionFuture ended in its last call and ran into the next function), x86 masks the count.
            if (i < 32 && ((1u << i) & range))
#else
            if ((1 << i) & range)
#endif
                bits = i + 1;
        }
        if (bits + mBitCount < 32)
        {
            mBitCount += bits;
            mBits = (mBits << bits) | (value - min);
        }
    }

    void Pack(float value, float min, float max, float precision)
    {
        value = nlMaxEquals(value, min);
        value = nlMinEquals(value, max);
        float scale = 1.0f / precision;
        int unidentifiedValue = (int)(value * scale + 0.5f * AIsgn(value));
        int unidentifiedMin = (int)(min * scale + 0.5f * AIsgn(min));
        int unidentifiedMax = (int)(max * scale + 0.5f * AIsgn(max));
        Pack(unidentifiedValue, unidentifiedMin, unidentifiedMax);
    }

    unsigned long GetBits() const
    {
        return mBits;
    }

    unsigned int mBitCount;
    unsigned long mBits;
};

#endif
