#include "NL/nlMath.h"
#include "np/fpbits.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

nlMatrix4& nlMakeRotationMatrixAxisAngle(
    nlMatrix4& out, const nlVector3& v3RotationAxis, float ang_rad)
{
    nlMatrix4 result;
    nlVector3 axis = v3RotationAxis;
    float fSin;
    float fCos;
    nlSinCos(&fSin, &fCos, (u16)(s32)(10430.378f * ang_rad));

    float oneMinusCos = 1.0f - fCos;
    result.m11 = oneMinusCos * (axis.x * axis.x) + fCos;
    result.m21 = oneMinusCos * (axis.x * axis.y) - axis.z * fSin;
    result.m31 = oneMinusCos * (axis.x * axis.z) + axis.y * fSin;
    result.m41 = 0.0f;
    result.m12 = oneMinusCos * (axis.x * axis.y) + axis.z * fSin;
    result.m22 = oneMinusCos * (axis.y * axis.y) + fCos;
    result.m32 = oneMinusCos * (axis.y * axis.z) - axis.x * fSin;
    result.m42 = 0.0f;
    result.m13 = oneMinusCos * (axis.x * axis.z) - axis.y * fSin;
    result.m23 = oneMinusCos * (axis.y * axis.z) + axis.x * fSin;
    result.m33 = oneMinusCos * (axis.z * axis.z) + fCos;
    result.m43 = 0.0f;
    result.m14 = 0.0f;
    result.m24 = 0.0f;
    result.m34 = 0.0f;
    result.m44 = 1.0f;
    out = result;
    return out;
}

extern "C" void fn_802B5370(
    nlQuaternion& out, const nlVector3& v3RotationAxis, unsigned short angle)
{
    float fSin;
    float fCos;
    nlSinCos(&fSin, &fCos, angle >> 1);
    out.x = v3RotationAxis.x * fSin;
    out.y = v3RotationAxis.y * fSin;
    out.z = v3RotationAxis.z * fSin;
    out.w = fCos;
}

extern "C" void fn_802B53EC(nlQuaternion& out, unsigned short angle)
{
    float fSin;
    float fCos;
    nlSinCos(&fSin, &fCos, (u16)((u32)angle >> 1));
    nlVec4Set(*(nlVector4*)&out, fSin, 0.0f, 0.0f, fCos);
}

void nlMakeQuatY(nlQuaternion& out, unsigned short angle)
{
    float fSin;
    float fCos;
    nlSinCos(&fSin, &fCos, (u16)((u32)angle >> 1));
    nlVec4Set(*(nlVector4*)&out, 0.0f, fSin, 0.0f, fCos);
}

void fn_802B549C(nlQuaternion& out, unsigned short angle)
{
    float fSin;
    float fCos;
    nlSinCos(&fSin, &fCos, (u16)((u32)angle >> 1));
    nlVec4Set(*(nlVector4*)&out, 0.0f, 0.0f, fSin, fCos);
}

void nlQuatNormalize(nlQuaternion& out, const nlQuaternion& in)
{
    float fLenSquared = nlQuatDot(in, in);
    float fOneOverSqrt = nlRecipSqrt(fLenSquared, true);
    nlQuatScale(out, in, fOneOverSqrt);
}

// MSC_GT18_QUAT (gt18): 0 = the original code below (NpQuatNLerpOld, the Renormalize body), 1 = the same arithmetic with no calls, 2 = both, the original's answer is used and a
// difference counted. nlQuatNLerp is called 120 k times per 60 pictures of play (Vice, host callgrind) and made three out-of-line calls of its own (two PSQUATDotProduct through
// nlQuatDot, one PSQUATScale through nlQuatScale: 29 + 29 + 29 host instructions, the SDK functions are not inlinable on the Vita), nlQuatRenormalize two more. The new bodies
// are the SDK's expressions on the same operands in the same order (mtx_native.c PSQUATDotProduct: (x*x' + z*z') + (y*y' + w*w'); PSQUATScale: each component times the scale),
// so every float is the same bit for bit; contraction is switched off so that no target can fuse a multiply into an add. MSC_GT18_BREAK=1: proof mutant (the new dot sums
// (x + y) + (z + w): different rounding). The counters of the verify mode are plain (approximate beside the pose lanes).
int gNpGt18Quat = -1;
#if defined(__vita__)
static const int sNpGt18Break = 0;   // the proof mutant is compiled out of the console build
#else
static int sNpGt18Break = 0;
#endif
static unsigned long long sNpGt18Checked, sNpGt18Wrong;
static void NpGt18Report()
{
    fprintf(stderr, "[gt18] QUAT verify: checked %llu, mismatches %llu\n", sNpGt18Checked, sNpGt18Wrong);
}
int NpGt18QuatInit()
{
    const char* e = getenv("MSC_GT18_QUAT");
    int m = (e && (e[0] == '1' || e[0] == '2')) ? e[0] - '0' : 0;
#if !defined(__vita__)
    const char* b = getenv("MSC_GT18_BREAK");
    sNpGt18Break = (b && b[0] == '1') ? 1 : 0;
#endif
    gNpGt18Quat = m;
    if (m)
    {
        fprintf(stderr, "[gt18] MSC_GT18_QUAT=%d%s\n", m, sNpGt18Break ? " (proof mutant BREAK=1)" : "");
        if (m == 2)
            atexit(NpGt18Report);
    }
    return m;
}

// MSC_AB (renderer/src/vgx_ab.h, kCall): weak, 0 unless MSC_AB is set. Once a picture (GameRenderTask::Run) the switch follows the in-run A/B harness; both arms give the same bits,
// so a lane that reads the old value for the rest of the picture is harmless.
extern "C" {
extern int VgxAbOn __attribute__((weak));
long VgxAbOverride(const char* key, long dflt) __attribute__((weak));
}
void NpGt18Frame()
{
    if (&VgxAbOn && VgxAbOn && VgxAbOverride)
    {
        const long o = VgxAbOverride("MSC_GT18_QUAT", -1);
        if (o >= 0)
            gNpGt18Quat = o > 2 ? 2 : (int)o;
    }
}

#if defined(__clang__)
#pragma clang fp contract(off)
#endif
static inline float NpGt18Dot(const nlQuaternion& p, const nlQuaternion& q)
{
    const float xy = p.x * q.x;
    const float zw = p.z * q.z;
    const float yw = p.y * q.y;
    const float ww = p.w * q.w;
    if (sNpGt18Break == 1)
        return (xy + yw) + (zw + ww);   // proof mutant
    return (xy + zw) + (yw + ww);
}

static inline void NpGt18NLerp(nlQuaternion& out, const nlQuaternion& q1, const nlQuaternion& q2, float t)
{
    const float dot = NpGt18Dot(q1, q2);
    float x, y, z, w;
    if (NpFpBitsOn() ? np_fp::IsPos(dot) : dot > 0.0f)
    {
        x = t * (q2.x - q1.x) + q1.x;
        y = t * (q2.y - q1.y) + q1.y;
        z = t * (q2.z - q1.z) + q1.z;
        w = t * (q2.w - q1.w) + q1.w;
    }
    else
    {
        x = t * (-q2.x - q1.x) + q1.x;
        y = t * (-q2.y - q1.y) + q1.y;
        z = t * (-q2.z - q1.z) + q1.z;
        w = t * (-q2.w - q1.w) + q1.w;
    }
    const float xy = x * x;
    const float zw = z * z;
    const float yw = y * y;
    const float ww = w * w;
    const float inv = nlRecipSqrt((sNpGt18Break == 1) ? ((xy + yw) + (zw + ww)) : ((xy + zw) + (yw + ww)), true);
    out.x = x * inv;
    out.y = y * inv;
    out.z = z * inv;
    out.w = w * inv;
}
static inline void NpGt18Renorm(nlQuaternion& r, const nlQuaternion& q)
{
    const float inv = nlRecipSqrt(NpGt18Dot(q, q), true);
    r.x = q.x * inv;
    r.y = q.y * inv;
    r.z = q.z * inv;
    r.w = q.w * inv;
}
#if defined(__clang__)
#pragma clang fp contract(on)
#endif

static void NpQuatNLerpOld(
    nlQuaternion& out, const nlQuaternion& q1, const nlQuaternion& q2, float t)
{
    float dot = nlQuatDot(q1, q2);
    if (NpFpBitsOn() ? np_fp::IsPos(dot) : dot > 0.0f)   // MSC_FPBITS: the same answer from the bits (np/fpbits.h)
    {
        out.x = t * (q2.x - q1.x) + q1.x;
        out.y = t * (q2.y - q1.y) + q1.y;
        out.z = t * (q2.z - q1.z) + q1.z;
        out.w = t * (q2.w - q1.w) + q1.w;
    }
    else
    {
        out.x = t * (-q2.x - q1.x) + q1.x;
        out.y = t * (-q2.y - q1.y) + q1.y;
        out.z = t * (-q2.z - q1.z) + q1.z;
        out.w = t * (-q2.w - q1.w) + q1.w;
    }
    float fOneOverSqrt = nlRecipSqrt(nlQuatDot(out, out), true);
    nlQuatScale(out, out, fOneOverSqrt);
}


void nlQuatNLerp(
    nlQuaternion& out, const nlQuaternion& q1, const nlQuaternion& q2, float t)
{
    const int mode = gNpGt18Quat >= 0 ? gNpGt18Quat : NpGt18QuatInit();
    if (mode == 0)
    {
        NpQuatNLerpOld(out, q1, q2, t);
        return;
    }
    if (mode == 1)
    {
        NpGt18NLerp(out, q1, q2, t);
        return;
    }
    nlQuaternion a, b;
    NpQuatNLerpOld(a, q1, q2, t);
    NpGt18NLerp(b, q1, q2, t);
    if ((++sNpGt18Checked & 0x3FFFF) == 0)
        NpGt18Report();   // the game leaves without running atexit handlers
    if (memcmp(&a, &b, sizeof(a)) != 0)
        sNpGt18Wrong++;
    out = a;
}

// MSC_POSE_FAST: 1 (default) or 0. The shortcuts below and in SAnimDecode.cpp give the same bits as the plain code (native/tools/gamefast/pose_fast_test.cpp).
int gNpPoseFast = -1;

int gNpPrefetch = -1;   // MSC_PREFETCH bit mask, nlMath.h
int NpPrefetchInit()
{
    const char* e = getenv("MSC_PREFETCH");
    gNpPrefetch = (e && e[0]) ? static_cast<int>(strtol(e, 0, 0)) : 0;
    return gNpPrefetch;
}

// MSC_RENDER_NEON (np/render_neon.h): 0 (default), 1 NEON kernels for render poses, 2 verify. The boot self-test runs the NEON kernels against the game's scalar functions
// on 10,000 random inputs each and prints the result; a difference on ordinary values (no subnormal operand or result) turns the switch off for the run.
int gNpRenderNeon = -1;
unsigned gNpRenderNeonCalls = 0;
unsigned gNpRenderNeonMismatch = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include "np/render_neon.h"
#include <stdio.h>
namespace
{
unsigned gRnState = 0x2545F491u;
unsigned RnRand()
{
    gRnState ^= gRnState << 13;
    gRnState ^= gRnState >> 17;
    gRnState ^= gRnState << 5;
    return gRnState;
}
float RnFloat()
{
    const unsigned r = RnRand();
    const unsigned kind = r & 31u;
    if (kind == 0)
        return 0.0f;
    if (kind == 1)
        return -0.0f;
    float v = ((float)(RnRand() >> 8) / 8388608.0f - 1.0f) * 2.0f;   // -2 .. 2
    if (kind == 2 || kind == 3)
        v *= 1.0e-19f;   // products of two of these are subnormal
    return v;
}
bool RnSub(const float* p, int n)
{
    for (int i = 0; i < n; i++)
    {
        unsigned b;
        memcpy(&b, p + i, 4);
        if ((b & 0x7F800000u) == 0 && (b & 0x007FFFFFu) != 0)
            return true;
        if ((b & 0x7FFFFFFFu) != 0 && (b & 0x7FFFFFFFu) < 0x219392EFu)   // below 1e-18: a square of it is subnormal or nearly
            return true;
    }
    return false;
}
struct RnTally
{
    unsigned n, ordinaryDiff, subnormalCases, subnormalDiff;
    void Add(const void* neon, const void* ref, int words, bool sub)
    {
        n++;
        const bool diff = memcmp(neon, ref, words * 4) != 0;
        if (sub)
        {
            subnormalCases++;
            subnormalDiff += diff;
        }
        else
            ordinaryDiff += diff;
    }
};
}   // namespace

static unsigned NpRenderNeonSelfTest()
{
    RnTally tq = {}, tn = {}, tm = {}, tl = {};
    for (int i = 0; i < 10000; i++)
    {
        nlQuaternion a, b, r, n;
        float* fa = &a.x;
        float* fb = &b.x;
        for (int k = 0; k < 4; k++)
        {
            fa[k] = RnFloat();
            fb[k] = RnFloat();
        }
        const float t = (float)(RnRand() >> 8) / 16777216.0f;
        nlMultQuat(r, a, b);
        NpNeonQuatMult(n, a, b);
        tq.Add(&n, &r, 4, RnSub(fa, 4) || RnSub(fb, 4) || RnSub(&r.x, 4));
        // a unit-ish pair for the nlerp (its normalisation makes zero quaternions meaningless: those compare as they come)
        nlQuatNLerp(r, a, b, t);
        NpNeonQuatNLerp(n, a, b, t);
        tn.Add(&n, &r, 4, RnSub(fa, 4) || RnSub(fb, 4) || RnSub(&r.x, 4));
        nlMatrix4 m;
        float* fm = &m.e2[0][0];
        for (int k = 0; k < 16; k++)
            fm[k] = RnFloat();
        nlVector3 p, pr, pn;
        p.x = RnFloat();
        p.y = RnFloat();
        p.z = RnFloat();
        nlMultPosVectorMatrix(pr, p, m);
        NpNeonMultPosVectorMatrix(pn, p, m);
        tm.Add(&pn, &pr, 3, RnSub(fm, 16) || RnSub(&p.x, 3) || RnSub(&pr.x, 3));
        nlVector3 la, lb, lr, ln;
        la.x = RnFloat(); la.y = RnFloat(); la.z = RnFloat();
        lb.x = RnFloat(); lb.y = RnFloat(); lb.z = RnFloat();
        nlVecLerp(lr, la, lb, t);
        NpNeonVecLerp(ln, la, lb, t);
        tl.Add(&ln, &lr, 3, RnSub(&la.x, 3) || RnSub(&lb.x, 3) || RnSub(&lr.x, 3));
    }
    fprintf(stderr, "[renderneon] self-test 10000 inputs each (ordinary diff / subnormal cases / subnormal diff): quatmult %u/%u/%u, nlerp %u/%u/%u, posmat %u/%u/%u, veclerp %u/%u/%u\n",
        tq.ordinaryDiff, tq.subnormalCases, tq.subnormalDiff, tn.ordinaryDiff, tn.subnormalCases, tn.subnormalDiff, tm.ordinaryDiff, tm.subnormalCases, tm.subnormalDiff,
        tl.ordinaryDiff, tl.subnormalCases, tl.subnormalDiff);
    return tq.ordinaryDiff + tn.ordinaryDiff + tm.ordinaryDiff + tl.ordinaryDiff;
}
#endif

int NpRenderNeonInit()
{
    const char* e = getenv("MSC_RENDER_NEON");
    int m = (e && e[0]) ? atoi(e) : 0;
    if (m < 0 || m > 2)
        m = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    if (m != 0)
    {
        const unsigned bad = NpRenderNeonSelfTest();
        if (bad != 0)
        {
            fprintf(stderr, "[renderneon] self-test FAILED on %u ordinary inputs: switch is OFF for this run\n", bad);
            m = 0;
        }
        else
            fprintf(stderr, "[renderneon] self-test passed, mode %d\n", m);
    }
#else
    if (m != 0)
        m = 0;   // no NEON on this build
#endif
    gNpRenderNeon = m;
    return m;
}

int gNpFpBits = -1;   // MSC_FPBITS, np/fpbits.h
int NpFpBitsInit()
{
    const char* e = getenv("MSC_FPBITS");
    gNpFpBits = (e && e[0] == '0') ? 0 : 1;
    return gNpFpBits;
}

int NpPoseFastInit()
{
    const char* e = getenv("MSC_POSE_FAST");
    gNpPoseFast = (e && e[0] == '0') ? 0 : 1;
    return gNpPoseFast;
}

// nlQuatNLerp(q, q, z, +-0) for a finite z that need not be computed. Each component is then t * (z - q) + q, with t * (z - q) a zero (the difference is finite while |q| < 2^127).
// A zero added to a normal number is that number, and either zero added to +0 is +0, whatever FPSCR.FZ says; so the blend returns q and only the normalisation is left to do.
// -0, denormals (FZ), infinities and NaN are not handled: false, and the caller takes the plain blend.
bool nlQuatRenormalize(nlQuaternion& q)
{
    unsigned bits[4];
    memcpy(bits, &q, sizeof(bits));
    for (int i = 0; i < 4; i++)
    {
        if (bits[i] != 0 && (bits[i] & 0x7FFFFFFFu) - 0x00800000u >= 0x7E800000u)
            return false;
    }
    const int mode = gNpGt18Quat >= 0 ? gNpGt18Quat : NpGt18QuatInit();
    if (mode != 0)
    {
        // the same two statements without the calls (see MSC_GT18_QUAT above)
        nlQuaternion r;
        NpGt18Renorm(r, q);
        if (mode == 2)
        {
            nlQuaternion o = q;
            float fOneOverSqrt = nlRecipSqrt(nlQuatDot(o, o), true);
            nlQuatScale(o, o, fOneOverSqrt);
            sNpGt18Checked++;
            if (memcmp(&o, &r, sizeof(o)) != 0)
                sNpGt18Wrong++;
            q = o;
            return true;
        }
        q = r;
        return true;
    }
    float fOneOverSqrt = nlRecipSqrt(nlQuatDot(q, q), true);
    nlQuatScale(q, q, fOneOverSqrt);
    return true;
}

void GetRotationBetweenVectors(
    nlQuaternion& quat, const nlVector3& v3Vec1, const nlVector3& v3Vec2)
{
    float fInvR1R2 =
        nlRecipSqrt(v3Vec1.GetLengthSq3D() * v3Vec2.GetLengthSq3D(), true);
    float fCosAngle = fInvR1R2 * nlVec3DotProduct(v3Vec1, v3Vec2);

    if (fCosAngle > 0.99999f)
    {
        nlQuatIdentity(quat);
    }
    else if (fCosAngle < -0.99999f)
    {
        nlVector3 axis;
        if (v3Vec1.x > v3Vec1.z || v3Vec1.y > v3Vec1.z)
        {
            nlVec3Set(axis, 0.0f, 0.0f, 1.0f);
        }
        else
        {
            nlVec3Set(axis, 1.0f, 0.0f, 0.0f);
        }

        nlVec3CrossProduct(axis, axis, v3Vec1);
        nlVec3Normalize(*(nlVector3*)&quat, axis);
        quat.w = 0.0f;
    }
    else
    {
        float fMagic = nlSqrt(2.0f * (1.0f + fCosAngle), true);
        float fMultiplier = fInvR1R2 / fMagic;

        nlVector3 cross;
        nlVec3CrossProduct(cross, v3Vec1, v3Vec2);
        quat.w = 0.5f * fMagic;
        quat.x = cross.x * fMultiplier;
        quat.y = cross.y * fMultiplier;
        quat.z = cross.z * fMultiplier;
    }
}

void RotateVector(nlVector3& result, const nlVector3& v, nlQuaternion& q)
{
    float xx = q.x * q.x;
    float yy = q.y * q.y;
    float zz = q.z * q.z;
    float ww = q.w * q.w;
    float x2 = 2.0f * q.x;
    float xy2 = x2 * q.y;
    float xz2 = x2 * q.z;
    float xw2 = x2 * q.w;
    float yz2 = 2.0f * q.y * q.z;
    float yw2 = 2.0f * q.y * q.w;
    float zw2 = 2.0f * q.z * q.w;
    float vx = v.x;
    float vy = v.y;
    float vz = v.z;

    result.x = vx * (xx + ww - yy - zz) + vy * (xy2 - zw2) + vz * (xz2 + yw2);
    result.y = vx * (zw2 + xy2) + vy * (yy + (ww - xx) - zz) + vz * (yz2 - xw2);
    result.z = vx * (xz2 - yw2) + vy * (xw2 + yz2) + vz * (zz + (ww - xx - yy));
}

nlMatrix4& nlInvertRotTransMatrix(nlMatrix4& out, const nlMatrix4& in)
{
    nlVector3 negResult;
    nlVector3 translation;
    ((u32*)&translation)[0] = *(u32*)&in.e2[3][0];
    ((u32*)&translation)[1] = *(u32*)&in.e2[3][1];
    ((u32*)&translation)[2] = *(u32*)&in.e2[3][2];

    nlTransposeMatrix(out, in);

    out.e2[2][3] = 0.0f;
    out.e2[1][3] = 0.0f;
    out.e2[0][3] = 0.0f;

    nlMultPosVectorMatrix(negResult, translation, out);

    nlVec3Scale(negResult, -1.0f);

    out.e2[3][0] = negResult.x;
    out.e2[3][1] = negResult.y;
    out.e2[3][2] = negResult.z;
    out.e2[3][3] = 1.0f;
    return out;
}

nlMatrix4& nlMakeRotTransMatrix(
    nlMatrix4& out,
    const nlVector3& v3ForwardVector,
    const nlVector3& v3UpVector,
    const nlVector3& v3AlternateUpVector,
    const nlVector3& v3Translation)
{
    nlVector3 v3Right;
    nlVector3 v3Forward;
    nlVector3 v3Up;

    nlVec3Normalize(v3Up, v3UpVector);
    nlVec3Normalize(v3Forward, v3ForwardVector);
    nlVec3CrossProduct(v3Right, v3Up, v3Forward);

    if (nlVec3LengthSquared(v3Right) < 0.1f)
    {
        nlVec3Normalize(v3Up, v3AlternateUpVector);
        nlVec3CrossProduct(v3Right, v3Up, v3Forward);
    }

    nlVec3Normalize(v3Right, v3Right);
    nlVec3CrossProduct(v3Up, v3Forward, v3Right);

    out.SetRow_(0, v3Forward);
    out.e2[0][3] = 0.0f;
    out.SetRow_(1, v3Right);
    out.e2[1][3] = 0.0f;
    out.SetRow_(2, v3Up);
    out.e2[2][3] = 0.0f;
    out.SetTranslation(v3Translation);
    return out;
}
