#include "Game/SAnimDecode.h"

#include "Game/SAnim.h"
#include "NL/nlMath.h"

#ifdef NP_NATIVE
// Native replacements for the paired-single quantised loads (psq_l with GQR6/GQR7 set by
// SAnimInitGQR: GQR6 = s16 scaled by 2^-15, GQR7 = s8 scaled by 2^-7). The packed stream is
// read in HOST byte order (the asset loader converts SAnim data to little endian); the 12-bit
// decoder assembles its 16-bit values from bytes and does not depend on endianness.
void SAnimInitGQR()
{
}

// MSC_POSE_FAST (nlMath.h). Rot16 and Rot12 in NEON: a 16-bit integer times 2^-15 is exact as a float, and the fixed-point vcvt gives that same float: nothing here rounds, and
// the smallest non-zero result is 2^-15, far from anything NEON flushes. A key's bytes are read as 4 + 2, never as 8: the last key of an animation may end a page.
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#define NP_SANIM_NEON 1
#include <arm_neon.h>
#include <string.h>

static inline void NpStoreS16Q15(float* r, int16x4_t v)
{
    vst1q_f32(r, vcvtq_n_f32_s32(vmovl_s16(v), 15));
}
#endif

extern "C" void SAnimDecodeRot16(nlQuaternion* result, const void* packed)
{
#ifdef NP_SANIM_NEON
    if (NpPoseFast())
    {
        uint64_t k;
        memcpy(&k, packed, 8);
        NpStoreS16Q15((float*)result, vreinterpret_s16_u64(vcreate_u64(k)));
        return;
    }
#endif
    const short* p = (const short*)packed;
    float* r = (float*)result;
    r[0] = p[0] * (1.0f / 32768.0f);
    r[1] = p[1] * (1.0f / 32768.0f);
    r[2] = p[2] * (1.0f / 32768.0f);
    r[3] = p[3] * (1.0f / 32768.0f);
}

extern "C" void SAnimDecodeRot12(nlQuaternion* result, const void* packed)
{
#ifdef NP_SANIM_NEON
    if (NpPoseFast())
    {
        // bytes b0..b5 to the little-endian shorts b0:(b1 & F0), b2:(b1 << 4), b3:(b4 & F0), b5:(b4 << 4)
        static const uint8_t kOrder[8] = { 1, 0, 1, 2, 4, 3, 4, 5 };
        static const int8_t kShift[8] = { 0, 0, 4, 0, 0, 0, 4, 0 };
        static const uint8_t kMask[8] = { 0xF0, 0xFF, 0xF0, 0xFF, 0xF0, 0xFF, 0xF0, 0xFF };
        uint32_t lo;
        uint16_t hi;
        memcpy(&lo, packed, 4);
        memcpy(&hi, (const unsigned char*)packed + 4, 2);
        uint8x8_t t = vtbl1_u8(vreinterpret_u8_u64(vcreate_u64(lo | ((uint64_t)hi << 32))), vld1_u8(kOrder));
        t = vand_u8(vshl_u8(t, vld1_s8(kShift)), vld1_u8(kMask));
        NpStoreS16Q15((float*)result, vreinterpret_s16_u8(t));
        return;
    }
#endif
    const unsigned char* bytes = (const unsigned char*)packed;
    unsigned char unpacked[8];
    unpacked[0] = bytes[0];
    unpacked[3] = unpacked[1] = bytes[1];
    unpacked[1] &= 0xF0;
    unpacked[2] = bytes[2];
    unpacked[3] <<= 4;
    unpacked[4] = bytes[3];
    unpacked[7] = unpacked[5] = bytes[4];
    unpacked[5] &= 0xF0;
    unpacked[6] = bytes[5];
    unpacked[7] <<= 4;
    float* r = (float*)result;
    // the original feeds `unpacked` to psq_l (s16, big endian): value i = bytes 2i (high), 2i+1 (low)
    for (int i = 0; i < 4; i++)
    {
        short v = (short)((unpacked[2 * i] << 8) | unpacked[2 * i + 1]);
        r[i] = v * (1.0f / 32768.0f);
    }
}

extern "C" void SAnimDecodeRot8(nlQuaternion* result, const void* packed)
{
    const signed char* p = (const signed char*)packed;
    float* r = (float*)result;
    r[0] = p[0] * (1.0f / 128.0f);
    r[1] = p[1] * (1.0f / 128.0f);
    r[2] = p[2] * (1.0f / 128.0f);
    r[3] = p[3] * (1.0f / 128.0f);
}
#else
// clang-format off
void SAnimInitGQR()
{
    register unsigned int value = 0x0F070F07;
    asm {
        mtspr 918, value
    }
    value = 0x07060706;
    asm {
        mtspr 919, value
    }
}
// clang-format on

// clang-format off
extern "C" void SAnimDecodeRot16(register nlQuaternion* result, register const void* packed)
{
    asm {
        psq_l f0, 0(packed), 0, 6
        psq_st f0, 0(result), 0, 0
        psq_l f0, 4(packed), 0, 6
        psq_st f0, 8(result), 0, 0
    }
}
// clang-format on

// clang-format off
extern "C" void SAnimDecodeRot12(register nlQuaternion* result, const void* packed)
{
    const unsigned char* bytes = (const unsigned char*)packed;
    unsigned char unpacked[8];
    unpacked[0] = bytes[0];
    unpacked[3] = unpacked[1] = bytes[1];
    unpacked[1] &= 0xF0;
    unpacked[2] = bytes[2];
    unpacked[3] <<= 4;
    unpacked[4] = bytes[3];
    unpacked[7] = unpacked[5] = bytes[4];
    unpacked[5] &= 0xF0;
    unpacked[6] = bytes[5];
    unpacked[7] <<= 4;
    register unsigned char* values = unpacked;
    asm {
        psq_l f0, 0(values), 0, 6
        psq_st f0, 0(result), 0, 0
        psq_l f0, 4(values), 0, 6
        psq_st f0, 8(result), 0, 0
    }
}
// clang-format on

// clang-format off
extern "C" void SAnimDecodeRot8(register nlQuaternion* result, register const void* packed)
{
    asm {
        psq_l f0, 0(packed), 0, 7
        psq_st f0, 0(result), 0, 0
        psq_l f0, 2(packed), 0, 7
        psq_st f0, 8(result), 0, 0
    }
}
// clang-format on

#endif // NP_NATIVE

extern "C" void SAnimDecodeScale(nlVector3* result, const PackedScale* packed)
{
    result->x = 0.00048828125f * packed->x;
    result->y = 0.00048828125f * packed->y;
    result->z = 0.00048828125f * packed->z;
}

extern "C" void SAnimDecodeWeight(float* result, const unsigned char* packed)
{
    *result = *packed / 255.0f;
}

extern "C" void SAnimDecodeMorphWeight(float* result, const unsigned char* packed)
{
    *result = *packed / 255.0f;
}
