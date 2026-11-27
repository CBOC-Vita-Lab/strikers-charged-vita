#ifndef NP_RENDER_NEON_H
#define NP_RENDER_NEON_H
// MSC_RENDER_NEON (wave 11, gt16a; default 0): NEON forms of the small float kernels of RENDER poses only: the snapshot blend of the drawable characters
// (DrawableCharacter::Blend) and the node matrices built from the blended pose (cPoseAccumulator::BuildNodeMatrices with renderPose = true). The simulation's
// poses (cCharacter::m_pPoseAccumulator, GetJointPosition) never call these: they keep the scalar code, so no simulation value can depend on the switch.
//   0  off: the scalar code (the game's own functions), the default
//   1  on: the NEON kernels below, after a boot self-test (math.cpp NpRenderNeonInit: 10,000 random inputs per kernel against the scalar functions, bit for bit;
//      one mismatch among ordinary values turns the switch off for the run and says so in the log)
//   2  verify: both are computed, the scalar result is used, the number of NEON results that differ is counted and printed ([renderneon] lines)
// Every kernel does the same single-precision operations in the same order as the scalar code it replaces (vmul then vadd, never a fused multiply-add: the A9
// has none; negations are sign flips, exact), so the results are the same bits for every input except those the NEON unit flushes: a subnormal operand or result
// reads as zero in NEON, while the VFP keeps it when FPSCR.FZ is clear (MSC_VITA_FPSCR=0, the default). The self-test counts those cases apart.
// There is no NEON on the host32 build: the switch is then forced to 0 (the host proof is "off equals off"; the NEON proof is the boot self-test, the qemu-arm test
// tools/gamefast/render_neon_test.cpp and the emulator's verify counters).
#include "NL/nlMath.h"

extern int gNpRenderNeon;     // math.cpp; -1 until read
int NpRenderNeonInit();
inline int NpRenderNeon() { return gNpRenderNeon < 0 ? NpRenderNeonInit() : gNpRenderNeon; }
extern unsigned gNpRenderNeonCalls;      // verify mode: kernel calls compared
extern unsigned gNpRenderNeonMismatch;   // verify mode: results whose bits differ

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>

// out = q1 * q2, the order of nlMultQuat (platqmath.cpp): each component is ((a + b) + c) + d with the products of q1.w, q1.x, q1.y, q1.z in turn.
// out may alias q1 or q2 (both are read first).
inline void NpNeonQuatMult(nlQuaternion& out, const nlQuaternion& q1, const nlQuaternion& q2)
{
    const float32x4_t b = vld1q_f32(&q2.x);              // x y z w
    const float32x4_t r = vrev64q_f32(b);                // y x w z
    const float32x4_t p2 = vextq_f32(r, r, 2);           // w z y x
    const float32x4_t p3 = vextq_f32(b, b, 2);           // z w x y
    const uint32x4_t s2 = { 0u, 0x80000000u, 0u, 0x80000000u };   // x1 * ( w2, -z2,  y2, -x2)
    const uint32x4_t s3 = { 0u, 0u, 0x80000000u, 0x80000000u };   // y1 * ( z2,  w2, -x2, -y2)
    const uint32x4_t s4 = { 0x80000000u, 0u, 0u, 0x80000000u };   // z1 * (-y2,  x2,  w2, -z2)
    const float32x4_t t2 = vmulq_n_f32(vreinterpretq_f32_u32(veorq_u32(vreinterpretq_u32_f32(p2), s2)), q1.x);
    const float32x4_t t3 = vmulq_n_f32(vreinterpretq_f32_u32(veorq_u32(vreinterpretq_u32_f32(p3), s3)), q1.y);
    const float32x4_t t4 = vmulq_n_f32(vreinterpretq_f32_u32(veorq_u32(vreinterpretq_u32_f32(r), s4)), q1.z);
    float32x4_t acc = vmulq_n_f32(b, q1.w);
    acc = vaddq_f32(acc, t2);
    acc = vaddq_f32(acc, t3);
    acc = vaddq_f32(acc, t4);
    vst1q_f32(&out.x, acc);
}

// PSQUATDotProduct: (x*x' + z*z') + (y*y' + w*w')
inline float NpNeonQuatDot(const float32x4_t a, const float32x4_t b)
{
    const float32x4_t p = vmulq_f32(a, b);
    const float32x2_t s = vadd_f32(vget_low_f32(p), vget_high_f32(p));   // (x + z, y + w)
    return vget_lane_f32(vpadd_f32(s, s), 0);
}

// nlQuatNLerp (math.cpp): the sign of the dot picks q2 or -q2; t * (q2 - q1) + q1; then a unit scale by nlRecipSqrt (the shared scalar routine).
inline void NpNeonQuatNLerp(nlQuaternion& out, const nlQuaternion& q1, const nlQuaternion& q2, float t)
{
    const float32x4_t a = vld1q_f32(&q1.x);
    const float32x4_t b = vld1q_f32(&q2.x);
    const float dot = NpNeonQuatDot(a, b);
    const float32x4_t d = dot > 0.0f ? vsubq_f32(b, a) : vsubq_f32(vnegq_f32(b), a);
    const float32x4_t o = vaddq_f32(vmulq_n_f32(d, t), a);
    const float inv = nlRecipSqrt(NpNeonQuatDot(o, o), true);
    vst1q_f32(&out.x, vmulq_n_f32(o, inv));
}

// nlMultPosVectorMatrix (platvmath.cpp): ((m00*x + m10*y) + m20*z) + m30 for each column; rows 0..3 are read whole, only three lanes are stored.
inline void NpNeonMultPosVectorMatrix(nlVector3& result, const nlVector3& pos, const nlMatrix4& m)
{
    const float x = pos.x, y = pos.y, z = pos.z;
    float32x4_t r = vmulq_n_f32(vld1q_f32(m.e2[0]), x);
    r = vaddq_f32(r, vmulq_n_f32(vld1q_f32(m.e2[1]), y));
    r = vaddq_f32(r, vmulq_n_f32(vld1q_f32(m.e2[2]), z));
    r = vaddq_f32(r, vld1q_f32(m.e2[3]));
    vst1_f32(&result.x, vget_low_f32(r));
    vst1q_lane_f32(&result.z, r, 2);
}

// nlVecLerp: (1 - alpha) * a + alpha * b per component (the 1 - alpha is the scalar's, passed in)
inline void NpNeonVecLerp(nlVector3& result, const nlVector3& a, const nlVector3& b, float alpha)
{
    const float oma = 1.0f - alpha;
    const float32x4_t va = vcombine_f32(vld1_f32(&a.x), vld1_dup_f32(&a.z));
    const float32x4_t vb = vcombine_f32(vld1_f32(&b.x), vld1_dup_f32(&b.z));
    const float32x4_t r = vaddq_f32(vmulq_n_f32(va, oma), vmulq_n_f32(vb, alpha));
    vst1_f32(&result.x, vget_low_f32(r));
    vst1q_lane_f32(&result.z, r, 2);
}

inline bool NpBitsDiffer(const void* a, const void* b, int words)
{
    const unsigned* x = static_cast<const unsigned*>(a);
    const unsigned* y = static_cast<const unsigned*>(b);
    for (int i = 0; i < words; i++)
        if (x[i] != y[i])
            return true;
    return false;
}

// The wrappers the game code calls (mode 1: NEON; mode 2: scalar result, NEON compared).
inline void NpRQuatMult(nlQuaternion& out, const nlQuaternion& q1, const nlQuaternion& q2)
{
    const int m = NpRenderNeon();
    if (m == 1)
    {
        NpNeonQuatMult(out, q1, q2);
    }
    else if (m == 2)
    {
        nlQuaternion n;
        NpNeonQuatMult(n, q1, q2);
        nlMultQuat(out, q1, q2);
        __atomic_fetch_add(&gNpRenderNeonCalls, 1u, __ATOMIC_RELAXED);
        if (NpBitsDiffer(&n, &out, 4))
            __atomic_fetch_add(&gNpRenderNeonMismatch, 1u, __ATOMIC_RELAXED);
    }
    else
    {
        nlMultQuat(out, q1, q2);
    }
}

inline void NpRQuatNLerp(nlQuaternion& out, const nlQuaternion& q1, const nlQuaternion& q2, float t)
{
    const int m = NpRenderNeon();
    if (m == 1)
    {
        NpNeonQuatNLerp(out, q1, q2, t);
    }
    else if (m == 2)
    {
        nlQuaternion n;
        NpNeonQuatNLerp(n, q1, q2, t);
        nlQuatNLerp(out, q1, q2, t);
        __atomic_fetch_add(&gNpRenderNeonCalls, 1u, __ATOMIC_RELAXED);
        if (NpBitsDiffer(&n, &out, 4))
            __atomic_fetch_add(&gNpRenderNeonMismatch, 1u, __ATOMIC_RELAXED);
    }
    else
    {
        nlQuatNLerp(out, q1, q2, t);
    }
}

inline void NpRMultPosVectorMatrix(nlVector3& result, const nlVector3& pos, const nlMatrix4& m)
{
    const int mode = NpRenderNeon();
    if (mode == 1)
    {
        NpNeonMultPosVectorMatrix(result, pos, m);
    }
    else if (mode == 2)
    {
        nlVector3 n;
        NpNeonMultPosVectorMatrix(n, pos, m);
        nlMultPosVectorMatrix(result, pos, m);
        __atomic_fetch_add(&gNpRenderNeonCalls, 1u, __ATOMIC_RELAXED);
        if (NpBitsDiffer(&n, &result, 3))
            __atomic_fetch_add(&gNpRenderNeonMismatch, 1u, __ATOMIC_RELAXED);
    }
    else
    {
        nlMultPosVectorMatrix(result, pos, m);
    }
}

inline void NpRVecLerp(nlVector3& result, const nlVector3& a, const nlVector3& b, float alpha)
{
    const int mode = NpRenderNeon();
    if (mode == 1)
    {
        NpNeonVecLerp(result, a, b, alpha);
    }
    else if (mode == 2)
    {
        nlVector3 n;
        NpNeonVecLerp(n, a, b, alpha);
        nlVecLerp(result, a, b, alpha);
        __atomic_fetch_add(&gNpRenderNeonCalls, 1u, __ATOMIC_RELAXED);
        if (NpBitsDiffer(&n, &result, 3))
            __atomic_fetch_add(&gNpRenderNeonMismatch, 1u, __ATOMIC_RELAXED);
    }
    else
    {
        nlVecLerp(result, a, b, alpha);
    }
}
#else
// no NEON (host): the scalar functions; NpRenderNeon() is 0 there
inline void NpRQuatMult(nlQuaternion& out, const nlQuaternion& q1, const nlQuaternion& q2) { nlMultQuat(out, q1, q2); }
inline void NpRQuatNLerp(nlQuaternion& out, const nlQuaternion& q1, const nlQuaternion& q2, float t) { nlQuatNLerp(out, q1, q2, t); }
inline void NpRMultPosVectorMatrix(nlVector3& result, const nlVector3& pos, const nlMatrix4& m) { nlMultPosVectorMatrix(result, pos, m); }
inline void NpRVecLerp(nlVector3& result, const nlVector3& a, const nlVector3& b, float alpha) { nlVecLerp(result, a, b, alpha); }
#endif

#endif // NP_RENDER_NEON_H
