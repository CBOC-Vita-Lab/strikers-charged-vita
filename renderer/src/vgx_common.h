// vgx - GX renderer for the Mario Strikers Charged Vita port.
// Shared small helpers. No GL and no runtime (HLE) headers in here.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

namespace vgx {

// Comparing and copying short runs of 4-byte words (uniform blocks, matrices, shader keys)
// without going through the C library: on the console's processor a memcmp / memcpy call
// for 48 bytes costs more than the 48 bytes, and there are thousands per frame.
// SameWords(a, b, n) is memcmp(a, b, 4 * n) == 0; CopyWords(d, s, n) is memcpy(d, s, 4 * n).
// Any alignment. -DVGX_NO_NEON builds the library version on ARM too (to compare the two:
// frame hashes of an ARM run with and without it must be equal).
#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && !defined(VGX_NO_NEON)
inline bool SameWords(const void* pa, const void* pb, uint32_t n) {
    const uint8_t* a = static_cast<const uint8_t*>(pa);
    const uint8_t* b = static_cast<const uint8_t*>(pb);
    for (; n >= 16; n -= 16, a += 64, b += 64) {
        const uint8x16_t d0 = veorq_u8(vld1q_u8(a), vld1q_u8(b)), d1 = veorq_u8(vld1q_u8(a + 16), vld1q_u8(b + 16));
        const uint8x16_t d2 = veorq_u8(vld1q_u8(a + 32), vld1q_u8(b + 32)), d3 = veorq_u8(vld1q_u8(a + 48), vld1q_u8(b + 48));
        const uint32x4_t d = vreinterpretq_u32_u8(vorrq_u8(vorrq_u8(d0, d1), vorrq_u8(d2, d3)));
        const uint32x2_t h = vorr_u32(vget_low_u32(d), vget_high_u32(d));
        if ((vget_lane_u32(h, 0) | vget_lane_u32(h, 1)) != 0) return false;
    }
    for (; n >= 4; n -= 4, a += 16, b += 16) {
        const uint32x4_t d = vreinterpretq_u32_u8(veorq_u8(vld1q_u8(a), vld1q_u8(b)));
        const uint32x2_t h = vorr_u32(vget_low_u32(d), vget_high_u32(d));
        if ((vget_lane_u32(h, 0) | vget_lane_u32(h, 1)) != 0) return false;
    }
    uint32_t x = 0, y = 0, z = 0;
    switch (n) { // (no loop here: a compiler turns a word loop back into a library call)
    case 3: std::memcpy(&x, a + 8, 4); std::memcpy(&y, b + 8, 4); z |= x ^ y; [[fallthrough]];
    case 2: std::memcpy(&x, a + 4, 4); std::memcpy(&y, b + 4, 4); z |= x ^ y; [[fallthrough]];
    case 1: std::memcpy(&x, a, 4); std::memcpy(&y, b, 4); z |= x ^ y; [[fallthrough]];
    default: break;
    }
    return z == 0;
}
inline void CopyWords(void* pd, const void* ps, uint32_t n) {
    uint8_t* d = static_cast<uint8_t*>(pd);
    const uint8_t* s = static_cast<const uint8_t*>(ps);
    for (; n >= 4; n -= 4, d += 16, s += 16) vst1q_u8(d, vld1q_u8(s));
    switch (n) {
    case 3: std::memcpy(d + 8, s + 8, 4); [[fallthrough]];
    case 2: std::memcpy(d + 4, s + 4, 4); [[fallthrough]];
    case 1: std::memcpy(d, s, 4); [[fallthrough]];
    default: break;
    }
}
inline void ZeroWords(void* pd, uint32_t n) {
    uint8_t* d = static_cast<uint8_t*>(pd);
    const uint8x16_t z = vdupq_n_u8(0);
    for (; n >= 4; n -= 4, d += 16) vst1q_u8(d, z);
    switch (n) {
    case 3: std::memset(d + 8, 0, 4); [[fallthrough]];
    case 2: std::memset(d + 4, 0, 4); [[fallthrough]];
    case 1: std::memset(d, 0, 4); [[fallthrough]];
    default: break;
    }
}
// GXD_XFREC: matrix memory loads. Compare and copy in one pass, true when the words differed (and were copied): 12 words (position / texture matrix) and
// 9 words (normal matrix). Any alignment.
inline bool UpdateWords12(void* pd, const void* ps) {
    uint8_t* d = static_cast<uint8_t*>(pd);
    const uint8_t* s = static_cast<const uint8_t*>(ps);
    const uint8x16_t s0 = vld1q_u8(s), s1 = vld1q_u8(s + 16), s2 = vld1q_u8(s + 32);
    const uint8x16_t x = vorrq_u8(vorrq_u8(veorq_u8(s0, vld1q_u8(d)), veorq_u8(s1, vld1q_u8(d + 16))), veorq_u8(s2, vld1q_u8(d + 32)));
    const uint32x4_t xr = vreinterpretq_u32_u8(x);
    const uint32x2_t h = vorr_u32(vget_low_u32(xr), vget_high_u32(xr));
    if ((vget_lane_u32(h, 0) | vget_lane_u32(h, 1)) == 0) return false;
    vst1q_u8(d, s0);
    vst1q_u8(d + 16, s1);
    vst1q_u8(d + 32, s2);
    return true;
}
inline bool UpdateWords9(void* pd, const void* ps) {
    uint8_t* d = static_cast<uint8_t*>(pd);
    const uint8_t* s = static_cast<const uint8_t*>(ps);
    const uint8x16_t s0 = vld1q_u8(s), s1 = vld1q_u8(s + 16);
    uint32_t w, v;
    std::memcpy(&w, s + 32, 4);
    std::memcpy(&v, d + 32, 4);
    const uint8x16_t x = vorrq_u8(veorq_u8(s0, vld1q_u8(d)), veorq_u8(s1, vld1q_u8(d + 16)));
    const uint32x4_t xr = vreinterpretq_u32_u8(x);
    const uint32x2_t h = vorr_u32(vget_low_u32(xr), vget_high_u32(xr));
    if (((vget_lane_u32(h, 0) | vget_lane_u32(h, 1)) | (w ^ v)) == 0) return false;
    vst1q_u8(d, s0);
    vst1q_u8(d + 16, s1);
    std::memcpy(d + 32, &w, 4);
    return true;
}
#else
inline bool UpdateWords12(void* d, const void* s) {
    if (std::memcmp(d, s, 48) == 0) return false;
    std::memcpy(d, s, 48);
    return true;
}
inline bool UpdateWords9(void* d, const void* s) {
    if (std::memcmp(d, s, 36) == 0) return false;
    std::memcpy(d, s, 36);
    return true;
}
inline void ZeroWords(void* d, uint32_t n) { std::memset(d, 0, static_cast<size_t>(n) * 4); }
inline bool SameWords(const void* a, const void* b, uint32_t n) { return std::memcmp(a, b, static_cast<size_t>(n) * 4) == 0; }
inline void CopyWords(void* d, const void* s, uint32_t n) { std::memcpy(d, s, static_cast<size_t>(n) * 4); }
#endif
// For structs: the word version when the size allows it.
template <typename T> inline bool SameStruct(const T& a, const T& b) {
    if (sizeof(T) % 4 == 0) return SameWords(&a, &b, static_cast<uint32_t>(sizeof(T) / 4));
    return std::memcmp(&a, &b, sizeof(T)) == 0;
}

inline uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
inline uint32_t be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}
inline uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
inline float bef32(const uint8_t* p) {
    uint32_t v = be32(p);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}
inline uint32_t f2u(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
inline float u2f(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }

inline uint32_t bits(uint32_t v, int shift, int count) { return (v >> shift) & ((1u << count) - 1u); }

// MSC_VGX_LOG=1 turns on the chatty one-time diagnostics; errors always print.
bool LogEnabled();
#define VGX_LOG(...) do { if (::vgx::LogEnabled()) { std::fprintf(stderr, "[vgx] " __VA_ARGS__); } } while (0)
#define VGX_ERR(...) do { std::fprintf(stderr, "[vgx] ERROR: " __VA_ARGS__); } while (0)
// Logs a message the first `n` times this line is reached.
#define VGX_LOG_N(n, ...) do { static int vgx_cnt_ = 0; if (vgx_cnt_ < (n)) { ++vgx_cnt_; std::fprintf(stderr, "[vgx] " __VA_ARGS__); } } while (0)

inline long EnvInt(const char* name, long dflt) {
    const char* v = std::getenv(name);
    return (v && *v) ? std::strtol(v, nullptr, 0) : dflt;
}

// Wave 5 (GXD) front-end savings, each behind a bit of MSC_VGX_GXD=<mask> (default: all on; 0: the wave-4 code paths, for
// A/B runs of one binary; the frame hash of every combination must be equal unless the bit's comment says otherwise).
// The mask is read by GxdInit() (called by rec::Reset / rec::InitMode, i.e. after the environment file has been applied);
// until then every bit is on.
enum : uint32_t {
    GXD_SOFT_NEON = 1u << 0, // SoftTransform (CPU transform of small meshes) with NEON on ARM
    GXD_BP_FAST = 1u << 1,   // the player drops register / XF records that change nothing before it calls the writer; TEV colour writes compare words
    GXD_DIV = 1u << 2,       // BeginVertices: reciprocal multiplication instead of the library division
    GXD_WORDCMP = 1u << 3,   // word compares (SameWords) instead of memcmp in the per-draw tests (merge test, display-list state)
    GXD_STATS = 1u << 4,     // vertex-format statistics: only a combination not seen before is formatted
    GXD_DEC_NEON = 1u << 5,  // vertex decode: NEON kernels for 16-bit-indexed 16-bit-fixed-point attributes, the index range scan and the matrix-index column with NEON
    GXD_KEYMEMO = 1u << 6,   // ResolveState: the shader keys of a register state seen before are taken from a small memo instead of being built again
    GXD_TEXMEMO = 1u << 7,   // a texture bound again exactly as it was (same object, nothing written since) is not resolved again
    GXD_SMALL = 1u << 8,     // small per-draw savings (uniform blocks built without clearing what they do not use, ...)
    GXD_XFREC = 1u << 9,     // player: matrix loads of 12 / 9 words (position, texture, normal, post-transform matrices) compared and copied in one pass, inline
    GXD_DLFAST = 1u << 10,   // display-list cache: a hit whose write generations were read after the last write notification does not read them again
    GXD_LOOPS_NEON = 1u << 11, // per-vertex loops outside the decoder (atlas redirect: coordinate range check and remap) with NEON
    GXD_RECFAST = 1u << 12,  // recorder: single-word XF register writes encoded inline, matrix loads (position / texture / normal) copied once, from the caller's matrix into the record
    GXD_VERIFY = 1u << 30,   // diagnostic: every memo / shortcut also computes the long way and reports a difference (never on by default)
};
// -DVGX_PROFNOINLINE (profile builds, tools/gxd/build.sh ... EXTRA=-DVGX_PROFNOINLINE): the stages of ResolveState and the like stay functions of their
// own so that a qemu instruction profile can tell them apart. No effect on what they compute.
#ifdef VGX_PROFNOINLINE
#define GXD_NI __attribute__((noinline))
#else
#define GXD_NI
#endif
extern uint32_t g_gxd;
void GxdInit();
inline bool Gxd(uint32_t bit) { return (g_gxd & bit) != 0; }

} // namespace vgx
