/*
 * np_sanim_decode.h - host decoders for the quantised SAnim keys AFTER npa_convert_sanim.
 *
 * The console decodes with paired-single quantised loads (GQR6 = 0x0F070F07: s16 / 2^15, GQR7 =
 * 0x07060706: s8 / 2^7); native builds replace SAnimDecodeRot16/12/8 / Scale / Weight with these.
 * Converted layout: rot16 keys are 4 x s16 in host order (8 bytes/key), rot12 keys 6 bytes of packed
 * 12-bit values (byte oriented, untouched), rot8 keys 4 x s8, scale keys 3 x u16 host order, weights u8.
 *
 * rot12: three bytes carry two signed 12-bit values:  v0 = b0<<4 | b1>>4,  v1 = b2<<4 | (b1 & 15).
 * (Derived from the unpacking in SAnimDecodeRot12 and confirmed on every shipped animation: all decoded
 * quaternions have unit length. Note mscharged-port's sanim_decode.cpp unpacks the second value as
 * (b1 & 15)<<8 | b2, which does not.)
 */
#ifndef NP_SANIM_DECODE_H
#define NP_SANIM_DECODE_H

#include <stdint.h>
#include <string.h>

static inline int npa_sanim_s12(unsigned v) { return (v & 0x800) ? (int)v - 0x1000 : (int)v; }

static inline void npa_SAnimDecodeRot16(float out[4], const void* packed)
{
    int16_t q[4];
    memcpy(q, packed, 8);
    out[0] = q[0] * (1.0f / 32768.0f);
    out[1] = q[1] * (1.0f / 32768.0f);
    out[2] = q[2] * (1.0f / 32768.0f);
    out[3] = q[3] * (1.0f / 32768.0f);
}

static inline void npa_SAnimDecodeRot12(float out[4], const void* packed)
{
    const uint8_t* b = (const uint8_t*)packed;
    out[0] = npa_sanim_s12((unsigned)(b[0] << 4) | (b[1] >> 4)) * (1.0f / 2048.0f);
    out[1] = npa_sanim_s12((unsigned)(b[2] << 4) | (b[1] & 15)) * (1.0f / 2048.0f);
    out[2] = npa_sanim_s12((unsigned)(b[3] << 4) | (b[4] >> 4)) * (1.0f / 2048.0f);
    out[3] = npa_sanim_s12((unsigned)(b[5] << 4) | (b[4] & 15)) * (1.0f / 2048.0f);
}

static inline void npa_SAnimDecodeRot8(float out[4], const void* packed)
{
    const int8_t* b = (const int8_t*)packed;
    out[0] = b[0] * (1.0f / 128.0f);
    out[1] = b[1] * (1.0f / 128.0f);
    out[2] = b[2] * (1.0f / 128.0f);
    out[3] = b[3] * (1.0f / 128.0f);
}

static inline void npa_SAnimDecodeScale(float out[3], const void* packed)
{
    uint16_t s[3];
    memcpy(s, packed, 6);
    out[0] = 0.00048828125f * s[0];
    out[1] = 0.00048828125f * s[1];
    out[2] = 0.00048828125f * s[2];
}

static inline float npa_SAnimDecodeWeight(const void* packed) { return *(const uint8_t*)packed / 255.0f; }

#endif
