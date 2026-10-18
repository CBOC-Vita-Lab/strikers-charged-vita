/* np_neon.h: NEON kernels for the math that dominates the game thread's profile, with scalar twins.
 *
 * Which functions and why: the recompiled build's hand-written natives list
 * (one/port/runtime/include/fast_native_table.inc, 57 functions that carried 5.41 M instructions per
 * presented frame) names the hot spots of this game: the SDK's matrix concatenation (PSMTXConcat,
 * PSMTX44Concat), quaternion maths (nlQuatNLerp, nlMultQuat, nlQuatToMatrix), position transforms
 * (nlMultPosVectorMatrix), the pose accumulator (PA_Blend*, PA_BuildNodeMatrices) and frustum tests
 * (ClassifySphereInFrustum). They are plain C++ in the decomp; compiled for the Cortex-A9 each of
 * them is a few dozen scalar VFP instructions where NEON needs a handful.
 *
 * Contract (np/docs/vita.md, "NEON"): row-major float matrices as the game stores them (nlMatrix4
 * e2[4][4], RVL Mtx = 3x4, row vectors: v' = v * M); every kernel has a scalar twin `ref_*` with the
 * same arguments; tests/test_neon.cpp compares them (tolerance: a few ulp, the kernels use fused
 * multiply-add where the scalar code rounds twice, and vrsqrte + Newton for 1/sqrt) and counts
 * instructions under qemu. Not bit-exact with the Wii and not bit-exact with the scalar twin:
 * np/docs/vita.md "Determinism" says where that matters (nothing that is checked against the
 * recompiled oracle bit for bit; matches stay the same game to a player).
 * Aliasing: out may equal an input in every kernel.
 */
#ifndef NP_NEON_H
#define NP_NEON_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define NP_HAVE_NEON 1
#else
#define NP_HAVE_NEON 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- scalar twins (the semantics, as the decomp's C++ computes them) ------------------------- */

/* PSMTXConcat: 3x4 * 3x4 (the implied fourth row is 0 0 0 1). */
static inline void ref_mtx34_concat(const float a[3][4], const float b[3][4], float ab[3][4]) {
    float t[3][4];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j)
            t[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + (j == 3 ? a[i][3] : 0.0f);
    memcpy(ab, t, sizeof(t));
}
/* PSMTX44Concat */
static inline void ref_mtx44_concat(const float a[4][4], const float b[4][4], float ab[4][4]) {
    float t[4][4];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            t[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
    memcpy(ab, t, sizeof(t));
}
/* nlMultPosVectorMatrix: row vector (x y z 1) times a 4x4 (only the first three columns are used). */
static inline void ref_pos_mtx44(float out[3], const float p[3], const float m[4][4]) {
    const float x = p[0], y = p[1], z = p[2];
    out[0] = m[0][0] * x + m[1][0] * y + m[2][0] * z + m[3][0];
    out[1] = m[0][1] * x + m[1][1] * y + m[2][1] * z + m[3][1];
    out[2] = m[0][2] * x + m[1][2] * y + m[2][2] * z + m[3][2];
}
/* nlMultQuat (x y z w order in memory) */
static inline void ref_quat_mul(float out[4], const float q1[4], const float q2[4]) {
    const float x1 = q1[0], y1 = q1[1], z1 = q1[2], w1 = q1[3], x2 = q2[0], y2 = q2[1], z2 = q2[2], w2 = q2[3];
    float t[4];
    t[3] = w1 * w2 - x1 * x2 - y1 * y2 - z1 * z2;
    t[0] = w1 * x2 + x1 * w2 + y1 * z2 - z1 * y2;
    t[1] = w1 * y2 - x1 * z2 + y1 * w2 + z1 * x2;
    t[2] = w1 * z2 + x1 * y2 - y1 * x2 + z1 * w2;
    memcpy(out, t, sizeof(t));
}
/* nlQuatNLerp: shortest-path linear blend, then normalised. */
static inline void ref_quat_nlerp(float out[4], const float q1[4], const float q2[4], float t) {
    const float dot = q1[0] * q2[0] + q1[1] * q2[1] + q1[2] * q2[2] + q1[3] * q2[3];
    const float s = dot > 0.0f ? 1.0f : -1.0f;
    float r[4];
    for (int i = 0; i < 4; ++i) r[i] = t * (s * q2[i] - q1[i]) + q1[i];
    const float inv = 1.0f / sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2] + r[3] * r[3]);
    for (int i = 0; i < 4; ++i) out[i] = r[i] * inv;
}
/* nlQuatToMatrix with setFourthColumn = true (rows of the matrix as the game's e2 holds them). */
static inline void ref_quat_to_mtx44(float m[4][4], const float q[4]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float xx = 2 * x * x, yy = 2 * y * y, zz = 2 * z * z, xy = 2 * x * y, xz = 2 * x * z, yz = 2 * y * z, wx = 2 * x * w, wy = 2 * y * w, wz = 2 * z * w;
    m[0][0] = 1 - yy - zz; m[1][0] = xy - wz;     m[2][0] = xz + wy;     m[3][0] = 0;
    m[0][1] = xy + wz;     m[1][1] = 1 - xx - zz; m[2][1] = yz - wx;     m[3][1] = 0;
    m[0][2] = xz - wy;     m[1][2] = yz + wx;     m[2][2] = 1 - xx - yy; m[3][2] = 0;
    m[0][3] = 0; m[1][3] = 0; m[2][3] = 0; m[3][3] = 1;
}
/* ClassifySphereInFrustum: 0 outside, 1 intersecting, 2 inside (the decomp's FrustumResult order is
 * its own enum: callers compare with FRUSTUM_*; the kernel returns this triple). Planes: 6 x (a b c d). */
static inline int ref_sphere_frustum(const float planes[6][4], const float pos[3], float radius) {
    int result = 2;
    for (int i = 0; i < 6; ++i) {
        const float d = pos[0] * planes[i][0] + pos[1] * planes[i][1] + pos[2] * planes[i][2] + planes[i][3];
        if (d < -radius) return 0;
        if (d < radius) result = 1;
    }
    return result;
}

/* ShaderSkinMesh::SoftwareSkinModel's inner loop (src/Game/GL/ShaderSkinMesh.cpp; the decomp's NpSkinAccumulate):
 * for every (vertexIndex, weight) pair of one bone:  out[idx] += weight * ((in[idx] [+ morph[idx]]) * M + M.row3).
 * M is the bone's matrix as the game stores it (4 rows of 4 floats, row-vector convention, translation in
 * row 3); `in`, `out`, `morph` are arrays of 3 floats per vertex. This loop is the CPU skinning of every
 * visible character, every frame, in the frame-send part of the frame. */
typedef struct NpSkinWeight { uint32_t vertexIndex; float vertexWeight; } NpSkinWeight;
static inline void ref_skin_accumulate(float* out, const float* in, const float* morph, const float M[4][4],
                                       const NpSkinWeight* w, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t k = w[i].vertexIndex;
        float x = in[3 * k], y = in[3 * k + 1], z = in[3 * k + 2];
        if (morph) { x += morph[3 * k]; y += morph[3 * k + 1]; z += morph[3 * k + 2]; }
        const float tx = M[0][0] * x + M[1][0] * y + M[2][0] * z + M[3][0];
        const float ty = M[0][1] * x + M[1][1] * y + M[2][1] * z + M[3][1];
        const float tz = M[0][2] * x + M[1][2] * y + M[2][2] * z + M[3][2];
        out[3 * k] += tx * w[i].vertexWeight;
        out[3 * k + 1] += ty * w[i].vertexWeight;
        out[3 * k + 2] += tz * w[i].vertexWeight;
    }
}

#if NP_HAVE_NEON
/* ---- NEON ---------------------------------------------------------------------------------- */
static inline void np_mtx44_concat(const float a[4][4], const float b[4][4], float ab[4][4]) {
    const float32x4_t b0 = vld1q_f32(b[0]), b1 = vld1q_f32(b[1]), b2 = vld1q_f32(b[2]), b3 = vld1q_f32(b[3]);
    float32x4_t r[4];
    for (int i = 0; i < 4; ++i) {
        const float32x4_t ai = vld1q_f32(a[i]);
        float32x4_t v = vmulq_lane_f32(b0, vget_low_f32(ai), 0);
        v = vmlaq_lane_f32(v, b1, vget_low_f32(ai), 1);
        v = vmlaq_lane_f32(v, b2, vget_high_f32(ai), 0);
        v = vmlaq_lane_f32(v, b3, vget_high_f32(ai), 1);
        r[i] = v;
    }
    vst1q_f32(ab[0], r[0]); vst1q_f32(ab[1], r[1]); vst1q_f32(ab[2], r[2]); vst1q_f32(ab[3], r[3]);
}
static inline void np_mtx34_concat(const float a[3][4], const float b[3][4], float ab[3][4]) {
    const float32x4_t b0 = vld1q_f32(b[0]), b1 = vld1q_f32(b[1]), b2 = vld1q_f32(b[2]);
    float32x4_t r[3];
    for (int i = 0; i < 3; ++i) {
        const float32x4_t ai = vld1q_f32(a[i]);
        float32x4_t v = vmulq_lane_f32(b0, vget_low_f32(ai), 0);
        v = vmlaq_lane_f32(v, b1, vget_low_f32(ai), 1);
        v = vmlaq_lane_f32(v, b2, vget_high_f32(ai), 0);
        /* the implied row 0 0 0 1 adds a[i][3] to column 3 only */
        const float32x4_t tail = vsetq_lane_f32(vgetq_lane_f32(ai, 3), vdupq_n_f32(0.0f), 3);
        r[i] = vaddq_f32(v, tail);
    }
    vst1q_f32(ab[0], r[0]); vst1q_f32(ab[1], r[1]); vst1q_f32(ab[2], r[2]);
}
static inline void np_pos_mtx44(float out[3], const float p[3], const float m[4][4]) {
    const float32x4_t m0 = vld1q_f32(m[0]), m1 = vld1q_f32(m[1]), m2 = vld1q_f32(m[2]), m3 = vld1q_f32(m[3]);
    float32x4_t v = vmlaq_n_f32(m3, m0, p[0]);
    v = vmlaq_n_f32(v, m1, p[1]);
    v = vmlaq_n_f32(v, m2, p[2]);
    out[0] = vgetq_lane_f32(v, 0); out[1] = vgetq_lane_f32(v, 1); out[2] = vgetq_lane_f32(v, 2);
}
/* n positions, 3 floats each, through the same matrix (a skinned mesh's vertices, a crowd's positions) */
static inline void np_pos_mtx44_array(float* out, const float* in, uint32_t n, const float m[4][4]) {
    const float32x4_t m0 = vld1q_f32(m[0]), m1 = vld1q_f32(m[1]), m2 = vld1q_f32(m[2]), m3 = vld1q_f32(m[3]);
    for (uint32_t i = 0; i < n; ++i) {
        const float32x2_t xy = vld1_f32(in + 3 * i);
        const float z = in[3 * i + 2];
        float32x4_t v = vmlaq_lane_f32(m3, m0, xy, 0);
        v = vmlaq_lane_f32(v, m1, xy, 1);
        v = vmlaq_n_f32(v, m2, z);
        vst1_f32(out + 3 * i, vget_low_f32(v));
        out[3 * i + 2] = vgetq_lane_f32(v, 2);
    }
}
/* The same loop in NEON: one vertex per iteration, the four matrix rows stay in registers. */
static inline void np_skin_accumulate(float* out, const float* in, const float* morph, const float M[4][4],
                                      const NpSkinWeight* w, uint32_t n) {
    const float32x4_t r0 = vld1q_f32(M[0]), r1 = vld1q_f32(M[1]), r2 = vld1q_f32(M[2]), r3 = vld1q_f32(M[3]);
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t k = w[i].vertexIndex;
        const float* pi = in + 3 * k;
        float* po = out + 3 * k;
        float32x2_t xy = vld1_f32(pi);
        float32x2_t zz = vld1_dup_f32(pi + 2);
        if (morph) {
            const float* pm = morph + 3 * k;
            xy = vadd_f32(xy, vld1_f32(pm));
            zz = vadd_f32(zz, vld1_dup_f32(pm + 2));
        }
        float32x4_t t = vmlaq_lane_f32(r3, r0, xy, 0);
        t = vmlaq_lane_f32(t, r1, xy, 1);
        t = vmlaq_lane_f32(t, r2, zz, 0);
        const float32x4_t o = vcombine_f32(vld1_f32(po), vld1_dup_f32(po + 2));
        const float32x4_t r = vmlaq_n_f32(o, t, w[i].vertexWeight);
        vst1_f32(po, vget_low_f32(r));
        vst1q_lane_f32(po + 2, r, 2);
    }
}
/* np_quat_mul: no NEON form. A NEON formulation (shuffled copies of both operands for the cross product)
 * measured 56 instructions against the scalar twin's 34 under qemu with GCC 13 (tests/test_neon.cpp), so it
 * is the scalar code. */
#define np_quat_mul ref_quat_mul
static inline void np_quat_nlerp(float out[4], const float q1[4], const float q2[4], float t) {
    const float32x4_t a = vld1q_f32(q1), b = vld1q_f32(q2);
    const float32x4_t p = vmulq_f32(a, b);
    const float32x2_t s2 = vadd_f32(vget_low_f32(p), vget_high_f32(p));
    const float dot = vget_lane_f32(vpadd_f32(s2, s2), 0);
    const float32x4_t sb = dot > 0.0f ? b : vnegq_f32(b);
    float32x4_t r = vmlaq_n_f32(a, vsubq_f32(sb, a), t);              /* t * (s*q2 - q1) + q1 */
    const float32x4_t sq = vmulq_f32(r, r);
    const float32x2_t n2 = vadd_f32(vget_low_f32(sq), vget_high_f32(sq));
    float32x2_t n = vpadd_f32(n2, n2);
    float32x2_t inv = vrsqrte_f32(n);                                   /* 1/sqrt estimate + two Newton steps */
    inv = vmul_f32(inv, vrsqrts_f32(vmul_f32(n, inv), inv));
    inv = vmul_f32(inv, vrsqrts_f32(vmul_f32(n, inv), inv));
    vst1q_f32(out, vmulq_lane_f32(r, inv, 0));
}
/* Six planes at once: the dot products of all planes with the position, then two comparisons. */
static inline int np_sphere_frustum(const float planes[6][4], const float pos[3], float radius) {
    const float32x4_t p = {pos[0], pos[1], pos[2], 1.0f};
    float d[6];
    for (int i = 0; i < 6; ++i) {
        const float32x4_t m = vmulq_f32(vld1q_f32(planes[i]), p);
        const float32x2_t s = vadd_f32(vget_low_f32(m), vget_high_f32(m));
        d[i] = vget_lane_f32(vpadd_f32(s, s), 0);
    }
    int result = 2;
    for (int i = 0; i < 6; ++i) {
        if (d[i] < -radius) return 0;
        if (d[i] < radius) result = 1;
    }
    return result;
}
/* Spheres in a batch: `spheres` is x y z r per sphere (16 bytes), out[i] = 0 outside, 1 intersecting,
 * 2 inside. Four spheres per iteration: vld4 turns four of them into x, y, z, r vectors, every plane is
 * three multiply-accumulates and two compares on all four. Culling thousands of crowd members or the
 * snapshot's drawables is this loop. */
static inline void np_sphere_frustum_batch(const float planes[6][4], const float* spheres, uint32_t n, uint8_t* out) {
    uint32_t i = 0;
    for (; i + 4 <= n; i += 4) {
        const float32x4x4_t s = vld4q_f32(spheres + 4 * i);   /* .val[0]=x .val[1]=y .val[2]=z .val[3]=r */
        /* The sign bit of (d + r) says d < -r (outside), the sign bit of (d - r) says d < r (not fully inside).
         * Plain float compares (vcltq_f32) are scalarised by GCC 13 without -ffast-math (measured: 120 instructions
         * per sphere against 87 for the scalar code), the sign-bit form stays on the vector unit. Differs from the
         * comparison only where d + r rounds across zero (boundary cases). */
        uint32x4_t outside = vdupq_n_u32(0), inter = vdupq_n_u32(0);
        for (int p = 0; p < 6; ++p) {
            const float32x4_t row = vld1q_f32(planes[p]);
            const float32x2_t lo = vget_low_f32(row), hi = vget_high_f32(row);
            float32x4_t d = vmlaq_lane_f32(vdupq_lane_f32(hi, 1), s.val[0], lo, 0);
            d = vmlaq_lane_f32(d, s.val[1], lo, 1);
            d = vmlaq_lane_f32(d, s.val[2], hi, 0);
            outside = vorrq_u32(outside, vreinterpretq_u32_f32(vaddq_f32(d, s.val[3])));
            inter = vorrq_u32(inter, vreinterpretq_u32_f32(vsubq_f32(d, s.val[3])));
        }
        /* inside = 2, intersecting = 1, outside = 0 */
        const uint32x4_t outMask = vreinterpretq_u32_s32(vshrq_n_s32(vreinterpretq_s32_u32(outside), 31));
        uint32x4_t res = vsubq_u32(vdupq_n_u32(2), vshrq_n_u32(inter, 31));
        res = vbicq_u32(res, outMask);
        const uint16x4_t r16 = vmovn_u32(res);
        const uint8x8_t r8 = vmovn_u16(vcombine_u16(r16, r16));
        vst1_lane_u32((uint32_t*)(void*)(out + i), vreinterpret_u32_u8(r8), 0);
    }
    for (; i < n; ++i) out[i] = (uint8_t)np_sphere_frustum(planes, spheres + 4 * i, spheres[4 * i + 3]);
}
#else
/* no NEON (x86 tests, a scalar fallback build): the twins stand in */
#define np_mtx44_concat ref_mtx44_concat
#define np_mtx34_concat ref_mtx34_concat
#define np_pos_mtx44 ref_pos_mtx44
static inline void np_pos_mtx44_array(float* out, const float* in, uint32_t n, const float m[4][4]) { for (uint32_t i = 0; i < n; ++i) ref_pos_mtx44(out + 3 * i, in + 3 * i, m); }
#define np_quat_mul ref_quat_mul
#define np_quat_nlerp ref_quat_nlerp
#define np_sphere_frustum ref_sphere_frustum
#define np_skin_accumulate ref_skin_accumulate
static inline void np_sphere_frustum_batch(const float planes[6][4], const float* spheres, uint32_t n, uint8_t* out) { for (uint32_t i = 0; i < n; ++i) out[i] = (uint8_t)ref_sphere_frustum(planes, spheres + 4 * i, spheres[4 * i + 3]); }
#endif

#ifdef __cplusplus
}
#endif
#endif
