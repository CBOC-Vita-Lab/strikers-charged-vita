// Native replacement for the paired-single quantisation helpers of the Wii SDK (OSFastCast.h): the
// graphics quantisers psq_l/psq_st with the GQR types the SDK sets up (u8/u16/s8/s16, scale 0).
// psq_st converts with saturation and rounds towards zero.
#ifndef RVL_SDK_OS_FAST_CAST_H
#define RVL_SDK_OS_FAST_CAST_H

#include <revolution/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OS_GQR_TYPE_U8 4
#define OS_GQR_TYPE_U16 5
#define OS_GQR_TYPE_S8 6
#define OS_GQR_TYPE_S16 7

static inline void OSInitFastCast(void) {}

static inline f32 __OSu16tof32(const u16* arg) { return (f32)*arg; }
static inline void OSu16tof32(const u16* in, f32* out) { *out = __OSu16tof32(in); }

static inline u16 __OSf32tou16(f32 arg) {
    if (!(arg > 0.0f)) return 0;
    if (arg >= 65535.0f) return 65535;
    return (u16)arg;
}
static inline void OSf32tou16(const f32* in, u16* out) { *out = __OSf32tou16(*in); }

static inline u8 __OSf32tou8(f32 arg) {
    if (!(arg > 0.0f)) return 0;
    if (arg >= 255.0f) return 255;
    return (u8)arg;
}
static inline void OSf32tou8(f32* in, u8* out) { *out = __OSf32tou8(*in); }

static inline s16 __OSf32tos16(f32 arg) {
    if (!(arg > -32768.0f)) return (arg != arg) ? 0 : -32768;
    if (arg >= 32767.0f) return 32767;
    return (s16)arg;
}
static inline void OSf32tos16(const f32* in, s16* out) { *out = __OSf32tos16(*in); }

#ifdef __cplusplus
}
#endif

#endif
