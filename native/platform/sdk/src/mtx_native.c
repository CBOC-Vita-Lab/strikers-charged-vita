// MTX / VEC / QUAT: the SDK's math routines in portable C.
//
// The console's versions are hand written paired-single assembly (PSMTX*, PSVEC*, PSQUAT*) or C
// (C_*). Every function the headers declare is here, each doing the single-precision operations
// in the order the originals do (the recompilation's fast_sdk_mtx.cpp documents the order for
// the ones it had to reproduce bit for bit; the port needs the same results to the last bit only
// where a replay is compared, which the native build is not).
#include <math.h>
#include <revolution/mtx.h>

typedef Mtx44 NpMtx44;

// ---- vectors ---------------------------------------------------------------------------------
void PSVECAdd(const Vec* a, const Vec* b, Vec* sum) {
    Vec r;
    r.x = a->x + b->x;
    r.y = a->y + b->y;
    r.z = a->z + b->z;
    *sum = r;
}

void PSVECScale(const Vec* in, Vec* out, f32 scale) {
    Vec r;
    r.x = in->x * scale;
    r.y = in->y * scale;
    r.z = in->z * scale;
    *out = r;
}

f32 PSVECDotProduct(const Vec* a, const Vec* b) { return a->x * b->x + a->y * b->y + a->z * b->z; }

f32 PSVECMag(const Vec* v) { return sqrtf(v->x * v->x + v->y * v->y + v->z * v->z); }

void PSVECNormalize(const Vec* in, Vec* out) {
    f32 m = in->x * in->x + in->y * in->y + in->z * in->z;
    Vec r;
    if (m > 0.0f) {
        f32 s = 1.0f / sqrtf(m);
        r.x = in->x * s;
        r.y = in->y * s;
        r.z = in->z * s;
    } else {
        r = *in;
    }
    *out = r;
}

void PSVECCrossProduct(const Vec* a, const Vec* b, Vec* prod) {
    Vec r;
    r.x = a->y * b->z - a->z * b->y;
    r.y = a->z * b->x - a->x * b->z;
    r.z = a->x * b->y - a->y * b->x;
    *prod = r;
}

f32 PSVECSquareDistance(const Vec* a, const Vec* b) {
    f32 dx = a->x - b->x, dy = a->y - b->y, dz = a->z - b->z;
    return dx * dx + dy * dy + dz * dz;
}

void C_VECHalfAngle(const Vec* a, const Vec* b, Vec* half) {
    Vec aT, bT, hT;
    aT.x = -a->x;
    aT.y = -a->y;
    aT.z = -a->z;
    bT.x = -b->x;
    bT.y = -b->y;
    bT.z = -b->z;
    PSVECNormalize(&aT, &aT);
    PSVECNormalize(&bT, &bT);
    PSVECAdd(&aT, &bT, &hT);
    if (PSVECDotProduct(&hT, &hT) > 0.0f) {
        PSVECNormalize(&hT, half);
    } else {
        *half = hT;
    }
}

// ---- 3x4 matrices ----------------------------------------------------------------------------
void C_MTXIdentity(Mtx m) { PSMTXIdentity(m); }

void PSMTXIdentity(Mtx m) {
    int i, j;
    for (i = 0; i < 3; ++i)
        for (j = 0; j < 4; ++j) m[i][j] = (i == j) ? 1.0f : 0.0f;
}

void PSMTXCopy(const Mtx src, Mtx dst) {
    if (src == dst) return;
    int i, j;
    for (i = 0; i < 3; ++i)
        for (j = 0; j < 4; ++j) dst[i][j] = src[i][j];
}

// NPP: NEON forms of the two matrix products (the hottest SDK math of the game: PSMTXConcat / PSMTX44Concat were 0.4 M instructions
// per presented frame in the recompiled build). Same operation order as the C code below (b0*a0, then +b1*a1, +b2*a2 [, +b3*a3];
// vmlaq is a multiply and an add on the Cortex-A9, not a fused one), so the results are identical to it except for the sign of a zero.
// Unaligned loads are fine for vld1. NP_NO_NEON=1 selects the C code.
#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && !defined(NP_NO_NEON)
#include <arm_neon.h>
// ab = a * b (the fourth row of both is (0, 0, 0, 1)); ab may alias a or b.
void PSMTXConcat(const Mtx a, const Mtx b, Mtx ab) {
    const float32x4_t b0 = vld1q_f32(b[0]), b1 = vld1q_f32(b[1]), b2 = vld1q_f32(b[2]);
    const float32x4_t a0 = vld1q_f32(a[0]), a1 = vld1q_f32(a[1]), a2 = vld1q_f32(a[2]);
    const float32x4_t zero = vdupq_n_f32(0.0f);
    float32x4_t r0 = vmulq_lane_f32(b0, vget_low_f32(a0), 0);
    float32x4_t r1 = vmulq_lane_f32(b0, vget_low_f32(a1), 0);
    float32x4_t r2 = vmulq_lane_f32(b0, vget_low_f32(a2), 0);
    r0 = vmlaq_lane_f32(r0, b1, vget_low_f32(a0), 1);
    r1 = vmlaq_lane_f32(r1, b1, vget_low_f32(a1), 1);
    r2 = vmlaq_lane_f32(r2, b1, vget_low_f32(a2), 1);
    r0 = vmlaq_lane_f32(r0, b2, vget_high_f32(a0), 0);
    r1 = vmlaq_lane_f32(r1, b2, vget_high_f32(a1), 0);
    r2 = vmlaq_lane_f32(r2, b2, vget_high_f32(a2), 0);
    r0 = vaddq_f32(r0, vsetq_lane_f32(vgetq_lane_f32(a0, 3), zero, 3));
    r1 = vaddq_f32(r1, vsetq_lane_f32(vgetq_lane_f32(a1, 3), zero, 3));
    r2 = vaddq_f32(r2, vsetq_lane_f32(vgetq_lane_f32(a2, 3), zero, 3));
    vst1q_f32(ab[0], r0);
    vst1q_f32(ab[1], r1);
    vst1q_f32(ab[2], r2);
}
#else
// ab = a * b (the fourth row of both is (0, 0, 0, 1)); ab may alias a or b.
void PSMTXConcat(const Mtx a, const Mtx b, Mtx ab) {
    Mtx t;
    int i, j;
    for (i = 0; i < 3; ++i) {
        for (j = 0; j < 4; ++j) {
            f32 v = b[0][j] * a[i][0];
            v = b[1][j] * a[i][1] + v;
            v = b[2][j] * a[i][2] + v;
            if (j == 3) v = v + a[i][3];
            t[i][j] = v;
        }
    }
    PSMTXCopy(t, ab);
}
#endif

void PSMTXConcatArray(const Mtx a, const Mtx srcBase, Mtx dstBase, u32 count) {
    const f32(*src)[4] = (const f32(*)[4])srcBase;
    f32(*dst)[4] = (f32(*)[4])dstBase;
    u32 i;
    for (i = 0; i < count; ++i) PSMTXConcat(a, (const f32(*)[4])(src + 3 * i), (f32(*)[4])(dst + 3 * i));
}

void PSMTXTranspose(const Mtx src, Mtx xPose) {
    Mtx t;
    int i, j;
    for (i = 0; i < 3; ++i) {
        for (j = 0; j < 3; ++j) t[i][j] = src[j][i];
        t[i][3] = 0.0f;
    }
    PSMTXCopy(t, xPose);
}

u32 PSMTXInverse(const Mtx src, Mtx inv) {
    f32 m00 = src[0][0], m01 = src[0][1], m02 = src[0][2], m03 = src[0][3];
    f32 m10 = src[1][0], m11 = src[1][1], m12 = src[1][2], m13 = src[1][3];
    f32 m20 = src[2][0], m21 = src[2][1], m22 = src[2][2], m23 = src[2][3];
    f32 det = m00 * m11 * m22 + m01 * m12 * m20 + m02 * m10 * m21 - m20 * m11 * m02 - m10 * m01 * m22 - m00 * m21 * m12;
    if (det == 0.0f) return 0;
    det = 1.0f / det;
    Mtx t;
    t[0][0] = (m11 * m22 - m21 * m12) * det;
    t[0][1] = -(m01 * m22 - m21 * m02) * det;
    t[0][2] = (m01 * m12 - m11 * m02) * det;
    t[1][0] = -(m10 * m22 - m20 * m12) * det;
    t[1][1] = (m00 * m22 - m20 * m02) * det;
    t[1][2] = -(m00 * m12 - m10 * m02) * det;
    t[2][0] = (m10 * m21 - m20 * m11) * det;
    t[2][1] = -(m00 * m21 - m20 * m01) * det;
    t[2][2] = (m00 * m11 - m10 * m01) * det;
    t[0][3] = -t[0][0] * m03 - t[0][1] * m13 - t[0][2] * m23;
    t[1][3] = -t[1][0] * m03 - t[1][1] * m13 - t[1][2] * m23;
    t[2][3] = -t[2][0] * m03 - t[2][1] * m13 - t[2][2] * m23;
    PSMTXCopy(t, inv);
    return 1;
}

// Inverse transpose of the upper 3x3 (the normal matrix); the translation column is zero.
u32 PSMTXInvXpose(const Mtx src, Mtx invX) {
    f32 m00 = src[0][0], m01 = src[0][1], m02 = src[0][2];
    f32 m10 = src[1][0], m11 = src[1][1], m12 = src[1][2];
    f32 m20 = src[2][0], m21 = src[2][1], m22 = src[2][2];
    f32 c00 = m11 * m22 - m21 * m12;
    f32 c01 = m12 * m20 - m22 * m10;
    f32 c02 = m10 * m21 - m11 * m20;
    f32 c10 = m21 * m02 - m01 * m22;
    f32 c11 = m22 * m00 - m02 * m20;
    f32 c12 = m01 * m20 - m00 * m21;
    f32 c20 = m01 * m12 - m11 * m02;
    f32 c21 = m02 * m10 - m12 * m00;
    f32 c22 = m00 * m11 - m01 * m10;
    f32 det = m00 * c00;
    det = m10 * c10 + det;
    det = m20 * c20 + det;
    if (det == 0.0f) {
        // The Gekko routine (ref/RVL_SDK/mtx/mtx.c) returns 0 here and leaves invX alone, and its callers (glxSend, glxSkinMatrix, the material programs) ignore the
        // result and load whatever the output matrix held: stale stack words, which differ from run to run on this port (a spinning flat object whose 3x3 is singular in
        // exact arithmetic: float rounding makes det exactly 0 in some frames; the neighbouring frames get a finite matrix of about 1e7). Write the cofactor matrix (the inverse
        // transpose without the 1/det that is infinite here: it points along the normal of the flattened surface) scaled by 2^25, which is what the reciprocal of a rounding-noise
        // determinant of a unit matrix (2^-25) gives in those neighbouring frames, so the result is deterministic and of the same size. The return value stays 0.
        const f32 s = 33554432.0f;
        invX[0][0] = c00 * s; invX[0][1] = c01 * s; invX[0][2] = c02 * s; invX[0][3] = 0.0f;
        invX[1][0] = c10 * s; invX[1][1] = c11 * s; invX[1][2] = c12 * s; invX[1][3] = 0.0f;
        invX[2][0] = c20 * s; invX[2][1] = c21 * s; invX[2][2] = c22 * s; invX[2][3] = 0.0f;
        return 0;
    }
    f32 r = 1.0f / det;
    invX[0][0] = c00 * r;
    invX[0][1] = c01 * r;
    invX[0][2] = c02 * r;
    invX[0][3] = 0.0f;
    invX[1][0] = c10 * r;
    invX[1][1] = c11 * r;
    invX[1][2] = c12 * r;
    invX[1][3] = 0.0f;
    invX[2][0] = c20 * r;
    invX[2][1] = c21 * r;
    invX[2][2] = c22 * r;
    invX[2][3] = 0.0f;
    return 1;
}

void PSMTXRotTrig(Mtx m, char axis, f32 sinA, f32 cosA) {
    switch (axis | 0x20) {   // 'x' / 'X' ...
    case 'x':
        m[0][0] = 1.0f; m[0][1] = 0.0f; m[0][2] = 0.0f; m[0][3] = 0.0f;
        m[1][0] = 0.0f; m[1][1] = cosA; m[1][2] = -sinA; m[1][3] = 0.0f;
        m[2][0] = 0.0f; m[2][1] = sinA; m[2][2] = cosA; m[2][3] = 0.0f;
        break;
    case 'y':
        m[0][0] = cosA; m[0][1] = 0.0f; m[0][2] = sinA; m[0][3] = 0.0f;
        m[1][0] = 0.0f; m[1][1] = 1.0f; m[1][2] = 0.0f; m[1][3] = 0.0f;
        m[2][0] = -sinA; m[2][1] = 0.0f; m[2][2] = cosA; m[2][3] = 0.0f;
        break;
    case 'z':
        m[0][0] = cosA; m[0][1] = -sinA; m[0][2] = 0.0f; m[0][3] = 0.0f;
        m[1][0] = sinA; m[1][1] = cosA; m[1][2] = 0.0f; m[1][3] = 0.0f;
        m[2][0] = 0.0f; m[2][1] = 0.0f; m[2][2] = 1.0f; m[2][3] = 0.0f;
        break;
    default:
        break;
    }
}

void PSMTXRotRad(Mtx m, char axis, f32 rad) { PSMTXRotTrig(m, axis, sinf(rad), cosf(rad)); }

void PSMTXRotAxisRad(Mtx m, const Vec* axis, f32 rad) {
    Vec n;
    f32 s = sinf(rad), c = cosf(rad), t = 1.0f - c;
    f32 x, y, z;
    PSVECNormalize(axis, &n);
    x = n.x;
    y = n.y;
    z = n.z;
    m[0][0] = t * x * x + c;
    m[0][1] = t * x * y - s * z;
    m[0][2] = t * x * z + s * y;
    m[0][3] = 0.0f;
    m[1][0] = t * x * y + s * z;
    m[1][1] = t * y * y + c;
    m[1][2] = t * y * z - s * x;
    m[1][3] = 0.0f;
    m[2][0] = t * x * z - s * y;
    m[2][1] = t * y * z + s * x;
    m[2][2] = t * z * z + c;
    m[2][3] = 0.0f;
}

void PSMTXTrans(Mtx m, f32 xT, f32 yT, f32 zT) {
    m[0][0] = 1.0f; m[0][1] = 0.0f; m[0][2] = 0.0f; m[0][3] = xT;
    m[1][0] = 0.0f; m[1][1] = 1.0f; m[1][2] = 0.0f; m[1][3] = yT;
    m[2][0] = 0.0f; m[2][1] = 0.0f; m[2][2] = 1.0f; m[2][3] = zT;
}

void PSMTXTransApply(const Mtx src, Mtx dst, f32 xT, f32 yT, f32 zT) {
    if (src != dst) {
        int i, j;
        for (i = 0; i < 3; ++i)
            for (j = 0; j < 3; ++j) dst[i][j] = src[i][j];
    }
    dst[0][3] = src[0][3] + xT;
    dst[1][3] = src[1][3] + yT;
    dst[2][3] = src[2][3] + zT;
}

void PSMTXScale(Mtx m, f32 xS, f32 yS, f32 zS) {
    m[0][0] = xS; m[0][1] = 0.0f; m[0][2] = 0.0f; m[0][3] = 0.0f;
    m[1][0] = 0.0f; m[1][1] = yS; m[1][2] = 0.0f; m[1][3] = 0.0f;
    m[2][0] = 0.0f; m[2][1] = 0.0f; m[2][2] = zS; m[2][3] = 0.0f;
}

void PSMTXScaleApply(const Mtx src, Mtx dst, f32 xS, f32 yS, f32 zS) {
    int j;
    for (j = 0; j < 4; ++j) {
        dst[0][j] = src[0][j] * xS;
        dst[1][j] = src[1][j] * yS;
        dst[2][j] = src[2][j] * zS;
    }
}

void PSMTXMultVec(const Mtx m, const Vec* src, Vec* dst) {
    f32 x = src->x, y = src->y, z = src->z;
    Vec r;
    r.x = (m[0][0] * x + m[0][1] * y) + m[0][2] * z + m[0][3];
    r.y = (m[1][0] * x + m[1][1] * y) + m[1][2] * z + m[1][3];
    r.z = (m[2][0] * x + m[2][1] * y) + m[2][2] * z + m[2][3];
    *dst = r;
}

void PSMTXQuat(Mtx m, const Quaternion* q) {
    f32 s = 2.0f / (q->x * q->x + q->y * q->y + q->z * q->z + q->w * q->w);
    f32 xs = q->x * s, ys = q->y * s, zs = q->z * s;
    f32 wx = q->w * xs, wy = q->w * ys, wz = q->w * zs;
    f32 xx = q->x * xs, xy = q->x * ys, xz = q->x * zs;
    f32 yy = q->y * ys, yz = q->y * zs, zz = q->z * zs;
    m[0][0] = 1.0f - (yy + zz);
    m[0][1] = xy - wz;
    m[0][2] = xz + wy;
    m[0][3] = 0.0f;
    m[1][0] = xy + wz;
    m[1][1] = 1.0f - (xx + zz);
    m[1][2] = yz - wx;
    m[1][3] = 0.0f;
    m[2][0] = xz - wy;
    m[2][1] = yz + wx;
    m[2][2] = 1.0f - (xx + yy);
    m[2][3] = 0.0f;
}

void C_MTXLookAt(Mtx m, const Point3d* camPos, const Vec* camUp, const Point3d* target) {
    Vec vLook, vRight, vUp;
    vLook.x = camPos->x - target->x;
    vLook.y = camPos->y - target->y;
    vLook.z = camPos->z - target->z;
    PSVECNormalize(&vLook, &vLook);
    PSVECCrossProduct(camUp, &vLook, &vRight);
    PSVECNormalize(&vRight, &vRight);
    PSVECCrossProduct(&vLook, &vRight, &vUp);
    m[0][0] = vRight.x;
    m[0][1] = vRight.y;
    m[0][2] = vRight.z;
    m[0][3] = -(camPos->x * vRight.x + camPos->y * vRight.y + camPos->z * vRight.z);
    m[1][0] = vUp.x;
    m[1][1] = vUp.y;
    m[1][2] = vUp.z;
    m[1][3] = -(camPos->x * vUp.x + camPos->y * vUp.y + camPos->z * vUp.z);
    m[2][0] = vLook.x;
    m[2][1] = vLook.y;
    m[2][2] = vLook.z;
    m[2][3] = -(camPos->x * vLook.x + camPos->y * vLook.y + camPos->z * vLook.z);
}

// Texture projection matrices for lights (GXLoadTexMtx of a spot light's shadow, projected texture).
void C_MTXLightFrustum(Mtx m, f32 t, f32 b, f32 l, f32 r, f32 n, f32 scaleS, f32 scaleT, f32 transS, f32 transT) {
    f32 tmp = 1.0f / (r - l);
    m[0][0] = ((2.0f * n) * tmp) * scaleS;
    m[0][1] = 0.0f;
    m[0][2] = (((r + l) * tmp) * scaleS) - transS;
    m[0][3] = 0.0f;
    tmp = 1.0f / (t - b);
    m[1][0] = 0.0f;
    m[1][1] = ((2.0f * n) * tmp) * scaleT;
    m[1][2] = (((t + b) * tmp) * scaleT) - transT;
    m[1][3] = 0.0f;
    m[2][0] = 0.0f;
    m[2][1] = 0.0f;
    m[2][2] = -1.0f;
    m[2][3] = 0.0f;
}

void C_MTXLightPerspective(Mtx m, f32 fovY, f32 aspect, f32 scaleS, f32 scaleT, f32 transS, f32 transT) {
    f32 angle = MTXDegToRad(fovY * 0.5f);
    f32 cot = 1.0f / (f32)tan(angle);
    m[0][0] = (cot / aspect) * scaleS;
    m[0][1] = 0.0f;
    m[0][2] = -transS;
    m[0][3] = 0.0f;
    m[1][0] = 0.0f;
    m[1][1] = cot * scaleT;
    m[1][2] = -transT;
    m[1][3] = 0.0f;
    m[2][0] = 0.0f;
    m[2][1] = 0.0f;
    m[2][2] = -1.0f;
    m[2][3] = 0.0f;
}

void C_MTXLightOrtho(Mtx m, f32 t, f32 b, f32 l, f32 r, f32 scaleS, f32 scaleT, f32 transS, f32 transT) {
    f32 tmp = 1.0f / (r - l);
    m[0][0] = (2.0f * tmp * scaleS);
    m[0][1] = 0.0f;
    m[0][2] = 0.0f;
    m[0][3] = ((-(r + l) * tmp) * scaleS) + transS;
    tmp = 1.0f / (t - b);
    m[1][0] = 0.0f;
    m[1][1] = (2.0f * tmp) * scaleT;
    m[1][2] = 0.0f;
    m[1][3] = ((-(t + b) * tmp) * scaleT) + transT;
    m[2][0] = 0.0f;
    m[2][1] = 0.0f;
    m[2][2] = 0.0f;
    m[2][3] = 1.0f;
}

// ---- 4x4 matrices ----------------------------------------------------------------------------
void C_MTXFrustum(Mtx44 m, f32 t, f32 b, f32 l, f32 r, f32 n, f32 f) {
    f32 tmp = 1.0f / (r - l);
    m[0][0] = 2.0f * n * tmp;
    m[0][1] = 0.0f;
    m[0][2] = tmp * (r + l);
    m[0][3] = 0.0f;
    tmp = 1.0f / (t - b);
    m[1][0] = 0.0f;
    m[1][1] = 2.0f * n * tmp;
    m[1][2] = tmp * (t + b);
    m[1][3] = 0.0f;
    m[2][0] = 0.0f;
    m[2][1] = 0.0f;
    tmp = 1.0f / (f - n);
    m[2][2] = -n * tmp;
    m[2][3] = tmp * -(f * n);
    m[3][0] = 0.0f;
    m[3][1] = 0.0f;
    m[3][2] = -1.0f;
    m[3][3] = 0.0f;
}

void C_MTXPerspective(Mtx44 m, f32 fovY, f32 aspect, f32 n, f32 f) {
    f32 angle = MTXDegToRad(fovY * 0.5f);
    f32 cot = 1.0f / (f32)tan(angle);
    f32 tmp;
    m[0][0] = cot / aspect;
    m[0][1] = 0.0f;
    m[0][2] = 0.0f;
    m[0][3] = 0.0f;
    m[1][0] = 0.0f;
    m[1][1] = cot;
    m[1][2] = 0.0f;
    m[1][3] = 0.0f;
    m[2][0] = 0.0f;
    m[2][1] = 0.0f;
    tmp = 1.0f / (f - n);
    m[2][2] = -n * tmp;
    m[2][3] = tmp * -(f * n);
    m[3][0] = 0.0f;
    m[3][1] = 0.0f;
    m[3][2] = -1.0f;
    m[3][3] = 0.0f;
}

void C_MTXOrtho(Mtx44 m, f32 t, f32 b, f32 l, f32 r, f32 n, f32 f) {
    f32 tmp = 1.0f / (r - l);
    m[0][0] = 2.0f * tmp;
    m[0][1] = 0.0f;
    m[0][2] = 0.0f;
    m[0][3] = tmp * -(r + l);
    tmp = 1.0f / (t - b);
    m[1][0] = 0.0f;
    m[1][1] = 2.0f * tmp;
    m[1][2] = 0.0f;
    m[1][3] = tmp * -(t + b);
    tmp = 1.0f / (f - n);
    m[2][0] = 0.0f;
    m[2][1] = 0.0f;
    m[2][2] = -1.0f * tmp;
    m[2][3] = -f * tmp;
    m[3][0] = 0.0f;
    m[3][1] = 0.0f;
    m[3][2] = 0.0f;
    m[3][3] = 1.0f;
}

void PSMTX44Identity(Mtx44 m) {
    int i, j;
    for (i = 0; i < 4; ++i)
        for (j = 0; j < 4; ++j) m[i][j] = (i == j) ? 1.0f : 0.0f;
}

void PSMTX44Copy(const Mtx44 src, Mtx44 dst) {
    int i, j;
    if (src == dst) return;
    for (i = 0; i < 4; ++i)
        for (j = 0; j < 4; ++j) dst[i][j] = src[i][j];
}

#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && !defined(NP_NO_NEON)
void PSMTX44Concat(const Mtx44 a, const Mtx44 b, Mtx44 ab) {
    const float32x4_t b0 = vld1q_f32(b[0]), b1 = vld1q_f32(b[1]), b2 = vld1q_f32(b[2]), b3 = vld1q_f32(b[3]);
    float32x4_t r[4];
    int i;
    for (i = 0; i < 4; ++i) {
        const float32x4_t ai = vld1q_f32(a[i]);
        float32x4_t v = vmulq_lane_f32(b0, vget_low_f32(ai), 0);
        v = vmlaq_lane_f32(v, b1, vget_low_f32(ai), 1);
        v = vmlaq_lane_f32(v, b2, vget_high_f32(ai), 0);
        v = vmlaq_lane_f32(v, b3, vget_high_f32(ai), 1);
        r[i] = v;
    }
    vst1q_f32(ab[0], r[0]);
    vst1q_f32(ab[1], r[1]);
    vst1q_f32(ab[2], r[2]);
    vst1q_f32(ab[3], r[3]);
}
#else
void PSMTX44Concat(const Mtx44 a, const Mtx44 b, Mtx44 ab) {
    Mtx44 t;
    int i, j;
    for (i = 0; i < 4; ++i) {
        for (j = 0; j < 4; ++j) {
            f32 v = b[0][j] * a[i][0];
            v = b[1][j] * a[i][1] + v;
            v = b[2][j] * a[i][2] + v;
            v = b[3][j] * a[i][3] + v;
            t[i][j] = v;
        }
    }
    PSMTX44Copy(t, ab);
}
#endif

void PSMTX44Transpose(const Mtx44 src, Mtx44 xPose) {
    Mtx44 t;
    int i, j;
    for (i = 0; i < 4; ++i)
        for (j = 0; j < 4; ++j) t[i][j] = src[j][i];
    PSMTX44Copy(t, xPose);
}

u32 C_MTX44Inverse(const Mtx44 src, Mtx44 inv) {
    Mtx44 gjm;
    int i, j, k, swp;
    f32 w, max, ftmp;
    PSMTX44Copy(src, gjm);
    PSMTX44Identity(inv);
    for (i = 0; i < 4; ++i) {
        max = 0.0f;
        swp = i;
        for (k = i; k < 4; ++k) {
            ftmp = fabsf(gjm[k][i]);
            if (ftmp > max) {
                max = ftmp;
                swp = k;
            }
        }
        if (max == 0.0f) return 0;
        if (swp != i) {
            for (k = 0; k < 4; ++k) {
                ftmp = gjm[i][k];
                gjm[i][k] = gjm[swp][k];
                gjm[swp][k] = ftmp;
                ftmp = inv[i][k];
                inv[i][k] = inv[swp][k];
                inv[swp][k] = ftmp;
            }
        }
        w = 1.0f / gjm[i][i];
        for (j = 0; j < 4; ++j) {
            gjm[i][j] *= w;
            inv[i][j] *= w;
        }
        for (k = 0; k < 4; ++k) {
            if (k != i) {
                w = gjm[k][i];
                for (j = 0; j < 4; ++j) {
                    gjm[k][j] -= gjm[i][j] * w;
                    inv[k][j] -= inv[i][j] * w;
                }
            }
        }
    }
    return 1;
}

void PSMTX44Scale(Mtx44 m, f32 xS, f32 yS, f32 zS) {
    int i, j;
    for (i = 0; i < 4; ++i)
        for (j = 0; j < 4; ++j) m[i][j] = 0.0f;
    m[0][0] = xS;
    m[1][1] = yS;
    m[2][2] = zS;
    m[3][3] = 1.0f;
}

// ---- quaternions -----------------------------------------------------------------------------
void PSQUATMultiply(const Quaternion* a, const Quaternion* b, Quaternion* prod) {
    Quaternion r;
    r.w = a->w * b->w - a->x * b->x - a->y * b->y - a->z * b->z;
    r.x = a->w * b->x + a->x * b->w + a->y * b->z - a->z * b->y;
    r.y = a->w * b->y + a->y * b->w + a->z * b->x - a->x * b->z;
    r.z = a->w * b->z + a->z * b->w + a->x * b->y - a->y * b->x;
    *prod = r;
}

void PSQUATScale(const Quaternion* q, Quaternion* r, f32 scale) {
    r->x = q->x * scale;
    r->y = q->y * scale;
    r->z = q->z * scale;
    r->w = q->w * scale;
}

f32 PSQUATDotProduct(const Quaternion* p, const Quaternion* q) {
    f32 xy = p->x * q->x;
    f32 zw = p->z * q->z;
    f32 yw = p->y * q->y;
    f32 ww = p->w * q->w;
    return (xy + zw) + (yw + ww);
}

void PSQUATNormalize(const Quaternion* in, Quaternion* out) {
    f32 mag = in->x * in->x + in->y * in->y + in->z * in->z + in->w * in->w;
    Quaternion r;
    if (mag >= 0.00001f) {
        f32 s = 1.0f / sqrtf(mag);
        r.x = in->x * s;
        r.y = in->y * s;
        r.z = in->z * s;
        r.w = in->w * s;
    } else {
        r.x = r.y = r.z = r.w = 0.0f;
    }
    *out = r;
}

void C_QUATMtx(Quaternion* quat, const Mtx m) {
    f32 trace = m[0][0] + m[1][1] + m[2][2];
    f32 s;
    f32 q[4];   // x y z w
    if (trace > 0.0f) {
        s = sqrtf(trace + 1.0f);
        q[3] = 0.5f * s;
        s = 0.5f / s;
        q[0] = (m[2][1] - m[1][2]) * s;
        q[1] = (m[0][2] - m[2][0]) * s;
        q[2] = (m[1][0] - m[0][1]) * s;
    } else {
        int i = 0, j, k;
        if (m[1][1] > m[0][0]) i = 1;
        if (m[2][2] > m[i][i]) i = 2;
        j = (i + 1) % 3;
        k = (j + 1) % 3;
        s = sqrtf((m[i][i] - (m[j][j] + m[k][k])) + 1.0f);
        q[i] = s * 0.5f;
        if (s != 0.0f) s = 0.5f / s;
        q[3] = (m[k][j] - m[j][k]) * s;
        q[j] = (m[i][j] + m[j][i]) * s;
        q[k] = (m[i][k] + m[k][i]) * s;
    }
    quat->x = q[0];
    quat->y = q[1];
    quat->z = q[2];
    quat->w = q[3];
}

void C_QUATSlerp(const Quaternion* p, const Quaternion* q, Quaternion* r, f32 t) {
    f32 theta, sin_th, cos_th, tp, tq;
    cos_th = p->x * q->x + p->y * q->y + p->z * q->z + p->w * q->w;
    tq = 1.0f;
    if (cos_th < 0.0f) {
        cos_th = -cos_th;
        tq = -tq;
    }
    if (cos_th <= 0.99999f) {
        theta = acosf(cos_th);
        sin_th = sinf(theta);
        tp = sinf((1.0f - t) * theta) / sin_th;
        tq *= sinf(t * theta) / sin_th;
    } else {
        tp = 1.0f - t;
        tq *= t;
    }
    r->x = (tp * p->x) + (tq * q->x);
    r->y = (tp * p->y) + (tq * q->y);
    r->z = (tp * p->z) + (tq * q->z);
    r->w = (tp * p->w) + (tq * q->w);
}
