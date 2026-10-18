#pragma once

// Workstream AUD (lever L6): the cheap form of the AX/DSP mix.
//
// ax_mix.cpp keeps the mixer it had (the "reference" path: one Accelerator::ReadSample call per
// input sample, 64-bit arithmetic in the resampler and the output stage, a full parameter-block
// read and write for every voice in the chain, stopped ones included). This header holds the
// pieces of the "fast" path that replaces it by default (MSC_AX_FAST, see ax_mix.cpp):
//
//   - block kernels over one AX frame of 96 samples: volume envelope, mix into a bus, aux return,
//     the final ramp + clamp + interleave, byte-order conversion, the 4-tap polyphase resampler.
//     Each exists as plain C++ ("...C": the same arithmetic as the reference loop it stands for)
//     and, on ARM with NEON, as intrinsics ("...Neon"). The Vita's Cortex-A9 has NEON and VFPv3
//     but no fused multiply-add and no integer divide: nothing here uses either.
//   - a batch ADPCM decoder that produces the same samples and the same parameter-block state as
//     the reference's per-sample accelerator read.
//
// EVERY kernel is bit-exact against the reference arithmetic: there is no place where a NEON
// rounding differs, not even by one least-significant bit. The reasoning is next to each kernel;
// the checks are scripts/aud/kernel_test.cpp (kernels against the reference loops, edge values
// and random data) and MSC_AX_VERIFY=1 (the whole mix both ways on the game's own data, every
// bus, every parameter block and every guest byte compared; ax_mix.cpp).
//
// MSC_AX_FAST=2 selects the fast path with the plain C++ kernels on an ARM build too.

#include "ax_internal.h"
#include "ax_mix_kernels.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define MKW_AX_MIX_NEON 1
#else
#define MKW_AX_MIX_NEON 0
#endif

namespace AxFast {

using AxDspHle::AramWindow;
using AxDspHle::AXPBWii;
using AxDspHle::ClampAdpcmS16;
using AxDspHle::ClampS16;
using AxDspHle::Hilo;
using AxDspHle::ReadAramByteSlow;
using AxDspHle::ResolveAramWindow;

constexpr uint32_t kFrame = 96; // samples of one AX frame; every kernel below is for exactly this many

// Set once at AX initialisation from MSC_AX_FAST (ax_mix.cpp); read by the kernel dispatchers.
extern bool g_useNeon;

// ---------------------------------------------------------------------------------------------
// Byte order
// ---------------------------------------------------------------------------------------------

// Copies `bytes` (even) bytes, swapping the two bytes of every halfword: guest big-endian
// halfwords <-> host halfwords, either direction. Neither pointer needs alignment.
inline void SwapCopy16C(uint8_t* dst, const uint8_t* src, size_t bytes) {
    // native port: both sides are host-endian, a plain copy (ax_native_endian.h)
    std::memcpy(dst, src, bytes);
}

#if MKW_AX_MIX_NEON
inline void SwapCopy16Neon(uint8_t* dst, const uint8_t* src, size_t bytes) {
    size_t i = 0;
    for (; i + 16 <= bytes; i += 16) {
        vst1q_u8(dst + i, vld1q_u8(src + i));
    }
    if (i + 8 <= bytes) {
        vst1_u8(dst + i, vld1_u8(src + i));
        i += 8;
    }
    SwapCopy16C(dst + i, src + i, bytes - i);
}
#endif

inline void SwapCopy16(uint8_t* dst, const uint8_t* src, size_t bytes) {
#if MKW_AX_MIX_NEON
    if (g_useNeon) {
        SwapCopy16Neon(dst, src, bytes);
        return;
    }
#endif
    SwapCopy16C(dst, src, bytes);
}

// Host int32 words <-> guest big-endian words (the mix buses that travel through guest memory).
#if MKW_AX_MIX_NEON
inline void StoreBigEndian32Neon(uint8_t* dst, const int32_t* src, size_t count) {
    size_t i = 0;
    for (; i + 4 <= count; i += 4) {
        vst1q_u8(dst + i * 4, vreinterpretq_u8_s32(vld1q_s32(src + i)));
    }
    AxMixKernels::StoreBigEndian32Scalar(dst + i * 4, src + i, count - i);
}

inline void LoadBigEndian32Neon(int32_t* dst, const uint8_t* src, size_t count) {
    size_t i = 0;
    for (; i + 4 <= count; i += 4) {
        vst1q_s32(dst + i, vreinterpretq_s32_u8(vld1q_u8(src + i * 4)));
    }
    AxMixKernels::LoadBigEndian32Scalar(dst + i, src + i * 4, count - i);
}
#endif

inline void StoreBigEndian32(uint8_t* dst, const int32_t* src, size_t count) {
#if MKW_AX_MIX_NEON
    if (g_useNeon) {
        StoreBigEndian32Neon(dst, src, count);
        return;
    }
#endif
    AxMixKernels::StoreBigEndian32(dst, src, count);
}

inline void LoadBigEndian32(int32_t* dst, const uint8_t* src, size_t count) {
#if MKW_AX_MIX_NEON
    if (g_useNeon) {
        LoadBigEndian32Neon(dst, src, count);
        return;
    }
#endif
    AxMixKernels::LoadBigEndian32(dst, src, count);
}

// ---------------------------------------------------------------------------------------------
// Q15 volume: (sample * volume) >> 15, clamped to int16, volume an unsigned 16-bit value
// ---------------------------------------------------------------------------------------------
// The reference computes the int32 product of an int16 sample and a uint16 volume (it always
// fits), shifts right arithmetically by 15 (floor) and clamps to [-0x8000, 0x7fff].
//
// NEON form, 8 samples per instruction, all in 16-bit lanes: write volume = hi * 0x8000 + lo with
// lo = volume & 0x7fff. Then sample * volume = sample * lo + hi * sample * 0x8000, and because
// the second term is a multiple of 0x8000
//     floor(sample * volume / 0x8000) = floor(sample * lo / 0x8000) + (hi ? sample : 0)
// exactly. VQDMULH(sample, lo) is floor(2 * sample * lo / 0x10000) = floor(sample * lo / 0x8000)
// (it can only saturate for -0x8000 * -0x8000, and lo is never negative), and the saturating
// 16-bit add VQADD of two int16 values is the clamp of their exact sum. So the result equals the
// reference's for every sample and every volume.

inline int16_t ScaleQ15C(int16_t sample, uint16_t volume) {
    return AxMixKernels::ClampToS16((static_cast<int32_t>(sample) * static_cast<int32_t>(volume)) >> 15);
}

#if MKW_AX_MIX_NEON
inline int16x8_t ScaleQ15Neon(int16x8_t s, uint16x8_t v) {
    const int16x8_t lo = vreinterpretq_s16_u16(vbicq_u16(v, vdupq_n_u16(0x8000)));
    const int16x8_t hi = vshrq_n_s16(vreinterpretq_s16_u16(v), 15); // all ones where v >= 0x8000
    return vqaddq_s16(vqdmulhq_s16(s, lo), vandq_s16(s, hi));
}

// volume + lane * delta for lanes 0..7, in wrapping 16-bit arithmetic like the reference's
// `volume = uint16_t(volume + delta)` per sample.
inline uint16x8_t RampStartNeon(uint16_t volume, uint16_t delta) {
    static const uint16_t kLanes[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    return vmlaq_u16(vdupq_n_u16(volume), vdupq_n_u16(delta), vld1q_u16(kLanes));
}
#endif

// Voice volume envelope over one frame, in place. Returns the ramp after 96 steps.
// (= AxMixKernels::ScaleRampScalar(samples, 96, volume, delta).)
inline uint16_t ScaleEnv96C(int16_t* samples, uint16_t volume, uint16_t delta) {
    return AxMixKernels::ScaleRampScalar(samples, kFrame, volume, delta);
}

#if MKW_AX_MIX_NEON
inline uint16_t ScaleEnv96Neon(int16_t* samples, uint16_t volume, uint16_t delta) {
    uint16x8_t v = RampStartNeon(volume, delta);
    const uint16x8_t step = vdupq_n_u16(static_cast<uint16_t>(delta * 8u));
    for (uint32_t i = 0; i < kFrame; i += 8) {
        vst1q_s16(samples + i, ScaleQ15Neon(vld1q_s16(samples + i), v));
        v = vaddq_u16(v, step);
    }
    return static_cast<uint16_t>(volume + kFrame * delta);
}
#endif

inline uint16_t ScaleEnv96(int16_t* samples, uint16_t volume, uint16_t delta) {
#if MKW_AX_MIX_NEON
    if (g_useNeon) {
        return ScaleEnv96Neon(samples, volume, delta);
    }
#endif
    return ScaleEnv96C(samples, volume, delta);
}

// Mix one frame of a voice into a 32-bit bus at a constant volume (the reference's MixAdd with
// delta 0, minus the de-pop bookkeeping, which the caller does from the last sample).
inline void MixAddConst96C(int32_t* out, const int16_t* in, uint16_t volume) {
    for (uint32_t i = 0; i < kFrame; ++i) {
        out[i] += ScaleQ15C(in[i], volume);
    }
}

#if MKW_AX_MIX_NEON
inline void MixAddConst96Neon(int32_t* out, const int16_t* in, uint16_t volume) {
    const int16x8_t lo = vdupq_n_s16(static_cast<int16_t>(volume & 0x7fffu));
    if (volume & 0x8000u) {
        for (uint32_t i = 0; i < kFrame; i += 8) {
            const int16x8_t s = vld1q_s16(in + i);
            const int16x8_t r = vqaddq_s16(vqdmulhq_s16(s, lo), s);
            vst1q_s32(out + i, vaddw_s16(vld1q_s32(out + i), vget_low_s16(r)));
            vst1q_s32(out + i + 4, vaddw_s16(vld1q_s32(out + i + 4), vget_high_s16(r)));
        }
    } else {
        for (uint32_t i = 0; i < kFrame; i += 8) {
            const int16x8_t r = vqdmulhq_s16(vld1q_s16(in + i), lo);
            vst1q_s32(out + i, vaddw_s16(vld1q_s32(out + i), vget_low_s16(r)));
            vst1q_s32(out + i + 4, vaddw_s16(vld1q_s32(out + i + 4), vget_high_s16(r)));
        }
    }
}
#endif

inline void MixAddConst96(int32_t* out, const int16_t* in, uint16_t volume) {
#if MKW_AX_MIX_NEON
    if (g_useNeon) {
        MixAddConst96Neon(out, in, volume);
        return;
    }
#endif
    MixAddConst96C(out, in, volume);
}

// The same through a 16-bit volume ramp. Returns the ramp after 96 steps.
inline uint16_t MixAddRamp96C(int32_t* out, const int16_t* in, uint16_t volume, uint16_t delta) {
    for (uint32_t i = 0; i < kFrame; ++i) {
        out[i] += ScaleQ15C(in[i], volume);
        volume = static_cast<uint16_t>(volume + delta);
    }
    return volume;
}

#if MKW_AX_MIX_NEON
inline uint16_t MixAddRamp96Neon(int32_t* out, const int16_t* in, uint16_t volume, uint16_t delta) {
    uint16x8_t v = RampStartNeon(volume, delta);
    const uint16x8_t step = vdupq_n_u16(static_cast<uint16_t>(delta * 8u));
    for (uint32_t i = 0; i < kFrame; i += 8) {
        const int16x8_t r = ScaleQ15Neon(vld1q_s16(in + i), v);
        vst1q_s32(out + i, vaddw_s16(vld1q_s32(out + i), vget_low_s16(r)));
        vst1q_s32(out + i + 4, vaddw_s16(vld1q_s32(out + i + 4), vget_high_s16(r)));
        v = vaddq_u16(v, step);
    }
    return static_cast<uint16_t>(volume + kFrame * delta);
}
#endif

inline uint16_t MixAddRamp96(int32_t* out, const int16_t* in, uint16_t volume, uint16_t delta) {
#if MKW_AX_MIX_NEON
    if (g_useNeon) {
        return MixAddRamp96Neon(out, in, volume, delta);
    }
#endif
    return MixAddRamp96C(out, in, volume, delta);
}

// ---------------------------------------------------------------------------------------------
// 32-bit buses through a 16-bit gain: aux return and the output stage
// ---------------------------------------------------------------------------------------------
// The reference widens to 64 bits: (int64(bus) * gain) >> 15, then keeps the low 32 bits (aux
// return) or clamps to int16 (output). NEON has the 32x32->64 multiply for two lanes (VMULL.S32);
// VSHRN #15 of the 64-bit lanes keeps bits 15..46, which are the low 32 bits of the shifted
// product (aux return), and VQSHRN #15 followed by VQMOVN is clamp-to-int32 followed by
// clamp-to-int16 of the shifted product, which is the clamp to int16 (output). A gain of exactly
// 0x8000 makes the product bus << 15, so the result is the bus value itself (aux return) or its
// clamp (output): no multiply is needed at all, and that is the usual case (a ramp only differs
// from a constant in the frames in which the game changes a volume).

// dst += (src * gain) >> 15 for one frame, gain constant.
inline void AccumGain96C(int32_t* dst, const int32_t* src, uint16_t gain) {
    for (uint32_t i = 0; i < kFrame; ++i) {
        dst[i] += static_cast<int32_t>((static_cast<int64_t>(src[i]) * static_cast<int64_t>(gain)) >> 15);
    }
}

#if MKW_AX_MIX_NEON
inline void AccumGain96Neon(int32_t* dst, const int32_t* src, uint16_t gain) {
    if (gain == 0x8000u) {
        for (uint32_t i = 0; i < kFrame; i += 4) {
            vst1q_s32(dst + i, vaddq_s32(vld1q_s32(dst + i), vld1q_s32(src + i)));
        }
        return;
    }
    const int32x2_t g = vdup_n_s32(static_cast<int32_t>(gain));
    for (uint32_t i = 0; i < kFrame; i += 4) {
        const int32x4_t s = vld1q_s32(src + i);
        const int32x4_t r = vcombine_s32(vshrn_n_s64(vmull_s32(vget_low_s32(s), g), 15),
                                         vshrn_n_s64(vmull_s32(vget_high_s32(s), g), 15));
        vst1q_s32(dst + i, vaddq_s32(vld1q_s32(dst + i), r));
    }
}

// The same with a per-sample gain table (a frame in which the volume moves).
inline void AccumRamp96Neon(int32_t* dst, const int32_t* src, const uint16_t* ramp) {
    for (uint32_t i = 0; i < kFrame; i += 4) {
        const int32x4_t s = vld1q_s32(src + i);
        const int32x4_t g = vreinterpretq_s32_u32(vmovl_u16(vld1_u16(ramp + i)));
        const int32x4_t r =
            vcombine_s32(vshrn_n_s64(vmull_s32(vget_low_s32(s), vget_low_s32(g)), 15),
                         vshrn_n_s64(vmull_s32(vget_high_s32(s), vget_high_s32(g)), 15));
        vst1q_s32(dst + i, vaddq_s32(vld1q_s32(dst + i), r));
    }
}
#endif

inline void AccumGain96(int32_t* dst, const int32_t* src, uint16_t gain) {
#if MKW_AX_MIX_NEON
    if (g_useNeon) {
        AccumGain96Neon(dst, src, gain);
        return;
    }
#endif
    AccumGain96C(dst, src, gain);
}

inline void AccumRamp96(int32_t* dst, const int32_t* src, const uint16_t* ramp) {
#if MKW_AX_MIX_NEON
    if (g_useNeon) {
        AccumRamp96Neon(dst, src, ramp);
        return;
    }
#endif
    AxMixKernels::MixAccumRamp32Scalar(dst, src, ramp, kFrame);
}

// ADD_TO_LR / SUB_TO_LR: a 32-bit frame added to (or subtracted from) both main buses. The
// reference negates and adds; modulo 2^32 that is the subtraction.
inline void AddToTwo96C(int32_t* a, int32_t* b, const int32_t* v, bool negate) {
    for (uint32_t i = 0; i < kFrame; ++i) {
        const uint32_t value = negate ? 0u - static_cast<uint32_t>(v[i]) : static_cast<uint32_t>(v[i]);
        a[i] = static_cast<int32_t>(static_cast<uint32_t>(a[i]) + value);
        b[i] = static_cast<int32_t>(static_cast<uint32_t>(b[i]) + value);
    }
}

#if MKW_AX_MIX_NEON
inline void AddToTwo96Neon(int32_t* a, int32_t* b, const int32_t* v, bool negate) {
    for (uint32_t i = 0; i < kFrame; i += 4) {
        const int32x4_t value = vld1q_s32(v + i);
        if (negate) {
            vst1q_s32(a + i, vsubq_s32(vld1q_s32(a + i), value));
            vst1q_s32(b + i, vsubq_s32(vld1q_s32(b + i), value));
        } else {
            vst1q_s32(a + i, vaddq_s32(vld1q_s32(a + i), value));
            vst1q_s32(b + i, vaddq_s32(vld1q_s32(b + i), value));
        }
    }
}
#endif

inline void AddToTwo96(int32_t* a, int32_t* b, const int32_t* v, bool negate) {
#if MKW_AX_MIX_NEON
    if (g_useNeon) {
        AddToTwo96Neon(a, b, v, negate);
        return;
    }
#endif
    AddToTwo96C(a, b, v, negate);
}

// The compressor's trigger: is any sample of either main bus above the threshold in magnitude?
// The reference compares the 64-bit magnitude; |INT32_MIN| = 2^31 is above every 16-bit
// threshold, and VABS leaves INT32_MIN as 0x80000000, which the UNSIGNED compare also puts above.
inline bool AnyAbove96C(const int32_t* left, const int32_t* right, uint16_t threshold) {
    for (uint32_t i = 0; i < kFrame; ++i) {
        const int64_t l = left[i];
        const int64_t r = right[i];
        if ((l < 0 ? -l : l) > threshold || (r < 0 ? -r : r) > threshold) {
            return true;
        }
    }
    return false;
}

#if MKW_AX_MIX_NEON
inline bool AnyAbove96Neon(const int32_t* left, const int32_t* right, uint16_t threshold) {
    const uint32x4_t limit = vdupq_n_u32(threshold);
    uint32x4_t any = vdupq_n_u32(0);
    for (uint32_t i = 0; i < kFrame; i += 4) {
        any = vorrq_u32(any, vcgtq_u32(vreinterpretq_u32_s32(vabsq_s32(vld1q_s32(left + i))), limit));
        any = vorrq_u32(any, vcgtq_u32(vreinterpretq_u32_s32(vabsq_s32(vld1q_s32(right + i))), limit));
    }
    const uint32x2_t folded = vorr_u32(vget_low_u32(any), vget_high_u32(any));
    return (vget_lane_u32(folded, 0) | vget_lane_u32(folded, 1)) != 0;
}
#endif

inline bool AnyAbove96(const int32_t* left, const int32_t* right, uint16_t threshold) {
#if MKW_AX_MIX_NEON
    if (g_useNeon) {
        return AnyAbove96Neon(left, right, threshold);
    }
#endif
    return AnyAbove96C(left, right, threshold);
}

// The output stage of one frame: left and right main bus through the output gain, clamped to
// int16, written back into the buses (the DSP's OUTPUT is destructive) and into `pcm` as the AI
// frame: 96 pairs right, left, each a big-endian halfword (384 bytes; no alignment needed).
// `ramp` is the per-sample gain table, or nullptr for the constant `gain`.
inline void Output96C(int32_t* busL, int32_t* busR, const uint16_t* ramp, uint16_t gain, uint8_t* pcm) {
    for (uint32_t i = 0; i < kFrame; ++i) {
        const int64_t g = ramp ? ramp[i] : gain;
        const int16_t left = ClampS16((static_cast<int64_t>(busL[i]) * g) >> 15);
        const int16_t right = ClampS16((static_cast<int64_t>(busR[i]) * g) >> 15);
        busL[i] = left;
        busR[i] = right;
        BigEndian::Write16(pcm + i * 4, static_cast<uint16_t>(right));
        BigEndian::Write16(pcm + i * 4 + 2, static_cast<uint16_t>(left));
    }
}

#if MKW_AX_MIX_NEON
inline int16x4_t OutputLaneNeon(int32x4_t bus, int32x4_t g) {
    return vqmovn_s32(vcombine_s32(vqshrn_n_s64(vmull_s32(vget_low_s32(bus), vget_low_s32(g)), 15),
                                   vqshrn_n_s64(vmull_s32(vget_high_s32(bus), vget_high_s32(g)), 15)));
}

inline void Output96Neon(int32_t* busL, int32_t* busR, const uint16_t* ramp, uint16_t gain, uint8_t* pcm) {
    const bool unity = ramp == nullptr && gain == 0x8000u;
    int32x4_t g = vdupq_n_s32(static_cast<int32_t>(gain));
    for (uint32_t i = 0; i < kFrame; i += 4) {
        const int32x4_t l = vld1q_s32(busL + i);
        const int32x4_t r = vld1q_s32(busR + i);
        int16x4_t l16;
        int16x4_t r16;
        if (unity) {
            l16 = vqmovn_s32(l);
            r16 = vqmovn_s32(r);
        } else {
            if (ramp) {
                g = vreinterpretq_s32_u32(vmovl_u16(vld1_u16(ramp + i)));
            }
            l16 = OutputLaneNeon(l, g);
            r16 = OutputLaneNeon(r, g);
        }
        vst1q_s32(busL + i, vmovl_s16(l16));
        vst1q_s32(busR + i, vmovl_s16(r16));
        uint8x8x2_t frame;
        frame.val[0] = vreinterpret_u8_s16(r16);
        frame.val[1] = vreinterpret_u8_s16(l16);
        // Interleave as halfwords: r0 l0 r1 l1 r2 l2 r3 l3, each already byte-swapped.
        const uint16x4x2_t pairs = vzip_u16(vreinterpret_u16_u8(frame.val[0]), vreinterpret_u16_u8(frame.val[1]));
        vst1_u8(pcm + i * 4, vreinterpret_u8_u16(pairs.val[0]));
        vst1_u8(pcm + i * 4 + 8, vreinterpret_u8_u16(pairs.val[1]));
    }
}
#endif

inline void Output96(int32_t* busL, int32_t* busR, const uint16_t* ramp, uint16_t gain, uint8_t* pcm) {
#if MKW_AX_MIX_NEON
    if (g_useNeon) {
        Output96Neon(busL, busR, ramp, gain, pcm);
        return;
    }
#endif
    Output96C(busL, busR, ramp, gain, pcm);
}

// ---------------------------------------------------------------------------------------------
// Sample-rate conversion from a linear history buffer
// ---------------------------------------------------------------------------------------------
// The reference keeps the last four input samples in a ring and pulls input samples one at a
// time while it produces output. How many input samples output i needs depends only on the
// position and the ratio, never on the data, so the fast path decodes all of a frame's input
// first into `hist` (hist[0..3] = the four samples carried over from the previous frame, oldest
// first, hist[4..] = this frame's input in order) and converts from that buffer: after k input
// samples the reference's ring holds hist[k..k+3], oldest first.

// Number of input samples one frame consumes (polyphase and linear conversion).
inline uint32_t SrcInputCount(uint32_t pos, uint32_t ratio) {
    return (pos + kFrame * ratio) >> 16;
}

// 4-tap polyphase conversion (SRCTYPE_POLYPHASE with the DSP ROM table). Returns the new
// fractional position. The reference sums the four int16 x int16 products in 64 bits; with the
// ROM's coefficients the sum of the four magnitudes is at most 38,635 (bank 2), so the sum of
// products is below 2^31 in magnitude and 32-bit arithmetic gives the same value (ax_mix.cpp
// checks the bound when it loads the table and falls back to the reference if it ever fails).
inline uint32_t ResamplePoly96C(int16_t* out, const int16_t* hist, uint32_t pos, uint32_t ratio,
                                const int16_t* coeffs) {
    uint32_t k = 0;
    for (uint32_t i = 0; i < kFrame; ++i) {
        pos += ratio;
        k += pos >> 16;
        pos &= 0xffffu;
        const int16_t* c = coeffs + ((pos >> 9) << 2);
        const int16_t* h = hist + k;
        const int32_t sum = h[0] * c[0] + h[1] * c[1] + h[2] * c[2] + h[3] * c[3];
        out[i] = AxMixKernels::ClampToS16(sum >> 15);
    }
    return pos;
}

#if MKW_AX_MIX_NEON
inline uint32_t ResamplePoly96Neon(int16_t* out, const int16_t* hist, uint32_t pos, uint32_t ratio,
                                   const int16_t* coeffs) {
    const int16_t* h = hist;
    for (uint32_t i = 0; i < kFrame; i += 4) {
        // Four outputs: the positions in plain integer code, the taps in NEON. The 32-bit lane
        // adds wrap, and the exact total fits (see above), so the order of the adds is free.
        int32x2_t pair[2];
        for (uint32_t half = 0; half < 2; ++half) {
            pos += ratio;
            h += pos >> 16;
            pos &= 0xffffu;
            const int32x4_t p0 = vmull_s16(vld1_s16(h), vld1_s16(coeffs + ((pos >> 9) << 2)));
            pos += ratio;
            h += pos >> 16;
            pos &= 0xffffu;
            const int32x4_t p1 = vmull_s16(vld1_s16(h), vld1_s16(coeffs + ((pos >> 9) << 2)));
            pair[half] = vpadd_s32(vadd_s32(vget_low_s32(p0), vget_high_s32(p0)),
                                   vadd_s32(vget_low_s32(p1), vget_high_s32(p1)));
        }
        vst1_s16(out + i, vqmovn_s32(vshrq_n_s32(vcombine_s32(pair[0], pair[1]), 15)));
    }
    return pos;
}
#endif

inline uint32_t ResamplePoly96(int16_t* out, const int16_t* hist, uint32_t pos, uint32_t ratio,
                               const int16_t* coeffs) {
#if MKW_AX_MIX_NEON
    if (g_useNeon) {
        return ResamplePoly96Neon(out, hist, pos, ratio, coeffs);
    }
#endif
    return ResamplePoly96C(out, hist, pos, ratio, coeffs);
}

// Linear conversion (SRCTYPE_LINEAR, or polyphase without a table), the reference's arithmetic
// on the history buffer: it interpolates between the two OLDEST samples of the ring.
// The reference special-cases a fraction of 0 (it returns the older sample) because its weight
// uint16(-frac) would be 0 there instead of 0x10000. With the weight written as 0x10000 - frac
// in 32 bits the one expression covers both: for frac == 0 it is s0 * 0x10000 >> 16 == s0, and
// s0 * (0x10000 - frac) + s1 * frac never leaves int32 (it lies between -0x8000 * 0x10000 and
// 0x7fff * 0x10000).
inline uint32_t ResampleLinear96(int16_t* out, const int16_t* hist, uint32_t pos, uint32_t ratio) {
    const int16_t* h = hist;
    for (uint32_t i = 0; i < kFrame; ++i) {
        pos += ratio;
        h += pos >> 16;
        pos &= 0xffffu;
        const int32_t frac = static_cast<int32_t>(pos);
        out[i] = static_cast<int16_t>((h[0] * (0x10000 - frac) + h[1] * frac) >> 16);
    }
    return pos;
}

// ---------------------------------------------------------------------------------------------
// ADPCM: a frame's input samples in one go
// ---------------------------------------------------------------------------------------------
// Produces what `count` consecutive calls of Accelerator::ReadSample produce for a voice whose
// sample format is DSP ADPCM (sample_format & 0xF == 0: 4-bit samples, predictor decode), and
// leaves the parameter block as those calls leave it (current address, yn1, yn2, pred_scale,
// running). The work per sample is the reference's own arithmetic; what goes away is the
// per-sample call, the reload of every field, the four stores into the parameter block and the
// thread-local sample-window check.
//
// Structure: within one 16-nibble ADPCM frame (2 header nibbles + 14 samples) and away from the
// voice's end address nothing but "decode the next nibble" can happen, so such a run is decoded
// in a tight loop; every sample at which the reference could wrap to the loop start, stop the
// voice or finish early takes the general step, which is the reference's code on local state.
inline void DecodeAdpcm(AXPBWii& pb, int16_t* out, uint32_t count) {
    if (count == 0) {
        return; // the reference does not touch the parameter block either
    }
    const uint32_t start = Hilo(pb.audio_addr.loop_addr_hi, pb.audio_addr.loop_addr_lo) & 0x3fffffffu;
    const uint32_t end = Hilo(pb.audio_addr.end_addr_hi, pb.audio_addr.end_addr_lo) & 0x3fffffffu;
    uint32_t cur = Hilo(pb.audio_addr.cur_addr_hi, pb.audio_addr.cur_addr_lo) & 0xbfffffffu;
    uint16_t predScale = pb.adpcm.pred_scale & 0x7f;
    int32_t yn1 = pb.adpcm.yn1;
    int32_t yn2 = pb.adpcm.yn2;

    // The page of guest memory the samples are read from, resolved like ReadAramByte resolves
    // it (same aliases, same order); a local copy instead of the thread-local one.
    AramWindow window{};
    auto byteAt = [&window](uint32_t addr) -> uint8_t {
        if (addr >= window.begin && addr < window.end) {
            return window.host[addr - window.begin];
        }
        if (ResolveAramWindow(addr, window)) {
            return window.host[addr - window.begin];
        }
        return ReadAramByteSlow(addr);
    };

    uint32_t i = 0;
    while (i < count) {
        // A clean run: the rest of the current ADPCM frame (or of the request), provided the
        // reference's three end-address tests (cur == end, end - 1, end + 1 after the
        // increment) cannot fire inside it and the address cannot carry into bit 30.
        const uint32_t room = 16u - (cur & 15u);
        const uint32_t run = count - i < room ? count - i : room;
        const bool nearEnd = cur <= end && end <= cur + run + 1u;
        const bool carry = (cur & 0x3fffffffu) + 18u >= 0x40000000u;
        const uint8_t* frame = nullptr;
        if (!nearEnd && !carry) {
            const uint32_t frameAddr = (cur & ~15u) >> 1;
            if (!(frameAddr >= window.begin && frameAddr < window.end)) {
                ResolveAramWindow(frameAddr, window);
            }
            if (frameAddr >= window.begin && frameAddr < window.end) {
                frame = window.host + (frameAddr - window.begin); // 8 bytes, inside the 4 KiB window
            }
        }
        if (frame != nullptr) {
            const int coefIdx = (predScale >> 4) & 0x7;
            const int32_t coef1 = pb.adpcm.coefs[coefIdx * 2 + 0];
            const int32_t coef2 = pb.adpcm.coefs[coefIdx * 2 + 1];
            const int32_t scale = 1 << (predScale & 0xf);
            // The run, a byte (two samples) at a time. `step` is the reference's arithmetic for
            // one sample; the two history values swap roles instead of being moved.
            const uint8_t* p = frame + ((cur >> 1) & 7u);
            int16_t* o = out + i;
            uint32_t left = run;
            int32_t a = yn1; // newest
            int32_t b = yn2; // the one before
            auto step = [coef1, coef2, scale](int32_t nibble, int32_t newest, int32_t older) -> int32_t {
                const int32_t value = scale * nibble + ((0x400 + coef1 * newest + coef2 * older) >> 11);
                return value > 0x7fff ? 0x7fff : (value < -0x7fff ? -0x7fff : value);
            };
            if (cur & 1u) {
                const int32_t value = step(static_cast<int32_t>((*p++ & 0xfu) ^ 8u) - 8, a, b);
                b = a;
                a = value;
                *o++ = static_cast<int16_t>(value);
                --left;
            }
            for (; left >= 2; left -= 2) {
                const int32_t byte = static_cast<int8_t>(*p++);
                b = step(byte >> 4, a, b);                                      // high nibble, sign-extended
                o[0] = static_cast<int16_t>(b);
                a = step(static_cast<int32_t>((byte & 0xf) ^ 8) - 8, b, a);    // low nibble
                o[1] = static_cast<int16_t>(a);
                o += 2;
            }
            if (left != 0) {
                const int32_t value = step(static_cast<int32_t>(static_cast<int8_t>(*p)) >> 4, a, b);
                b = a;
                a = value;
                *o++ = static_cast<int16_t>(value);
            }
            yn1 = a;
            yn2 = b;
            cur += run;
            i += run;
            if ((cur & 15u) == 0) {
                predScale = byteAt(cur >> 1) & 0x7f;
                cur += 2;
            }
            continue;
        }

        // The general step: Accelerator::ReadSample for this format, on the local state.
        {
            const uint8_t byte = byteAt(cur >> 1);
            const uint16_t raw = (cur & 1u) ? (byte & 0xf) : (byte >> 4);
            const int32_t nibble = raw >= 8 ? static_cast<int32_t>(raw) - 16 : static_cast<int32_t>(raw);
            const int coefIdx = (predScale >> 4) & 0x7;
            const int32_t coef1 = pb.adpcm.coefs[coefIdx * 2 + 0];
            const int32_t coef2 = pb.adpcm.coefs[coefIdx * 2 + 1];
            const int32_t scale = 1 << (predScale & 0xf);
            const int32_t decoded = scale * nibble + ((0x400 + coef1 * yn1 + coef2 * yn2) >> 11);
            const int16_t value = ClampAdpcmS16(decoded);
            yn2 = yn1;
            yn1 = value;
            ++cur;
            uint32_t step = 2;
            if ((end & 0xf) == 0 && cur == end) {
                cur = start + 1;
            } else if ((end & 0xf) == 1 && cur == end - 1) {
                cur = start;
            } else if ((cur & 15) == 0) {
                predScale = byteAt((cur & ~15u) >> 1) & 0x7f;
                cur += 2;
                step += 2;
            }
            bool stopped = false;
            if (cur == end + step - 1) {
                cur = start;
                if (pb.audio_addr.looping) {
                    predScale = pb.adpcm_loop_info.pred_scale & 0x7f;
                    if (pb.is_stream != 1) {
                        yn1 = static_cast<int16_t>(pb.adpcm_loop_info.yn1);
                        yn2 = static_cast<int16_t>(pb.adpcm_loop_info.yn2);
                    }
                } else {
                    pb.running = 0;
                    stopped = true;
                }
            }
            cur &= 0xbfffffffu;
            out[i++] = value;
            if (stopped) {
                // The reference returns 0 for every further read of this frame and leaves the
                // parameter block as the stopping read wrote it.
                std::memset(out + i, 0, (count - i) * sizeof(int16_t));
                break;
            }
        }
    }

    pb.audio_addr.cur_addr_hi = static_cast<uint16_t>(cur >> 16);
    pb.audio_addr.cur_addr_lo = static_cast<uint16_t>(cur);
    pb.adpcm.yn1 = static_cast<int16_t>(yn1);
    pb.adpcm.yn2 = static_cast<int16_t>(yn2);
    pb.adpcm.pred_scale = predScale;
}

} // namespace AxFast
