#include "Game/PoseAccumulator.h"

#include "Game/PoseNode.h"
#include "np/render_neon.h"
#include "np/fpbits.h"
#include "Game/SHierarchy.h"
#include "NL/nlMemory.h"

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// MSC_PHYS27_DIV (wave 12, phys27): the blend weight t = w / acc, acc = before + w. When nothing was accumulated before (before == 0) and w is finite,
// acc is exactly w (0 + w) and w / w is exactly 1.0f (IEEE division is correctly rounded), so the divide (VFP vdiv: not pipelined, ~15 cycles on the A9, and
// the result feeds the blend) is not made. Same bits as the division. =2 verify: the division is made as well, compared and used (counted; a [phys27div] line).
// MSC_PHYS27_DIV_BREAK=1: proof mutant (every 4th skipped divide gives 1.0009765625f). The counters are plain (racy beside the pose lanes: approximate).
extern int gNpGt18Quat;   // MSC_GT18_QUAT, NL/math.cpp
int NpGt18QuatInit();

namespace
{
int sP27DivMode = -1, sP27DivBreak = 0;
// MSC_GT18_QUAT >= 1 (gt18): the three counters below were plain globals that every pose lane (and the game thread) incremented on every call (83 k calls per 60 pictures of
// play): one cache line written by up to four cores, and the loads and stores of each call. Only the verify mode and the proof mutant read them, so with the switch on the
// fast path touches none of them (the value returned is the same).
inline bool NpGt18CntOff()
{
    return (gNpGt18Quat >= 0 ? gNpGt18Quat : NpGt18QuatInit()) != 0;   // MSC_GT18_QUAT (math.cpp)
}
unsigned long long sP27DivSkipped, sP27DivDone, sP27DivChecked, sP27DivWrong;
int NpP27DivInit()
{
    const char* e = getenv("MSC_PHYS27_DIV");
    sP27DivMode = (e && (e[0] == '1' || e[0] == '2')) ? e[0] - '0' : 0;
    const char* b = getenv("MSC_PHYS27_DIV_BREAK");
    sP27DivBreak = (b && b[0] == '1') ? 1 : 0;
    return sP27DivMode;
}
inline float NpWeightT(float before, float w, float acc)
{
    const int m = sP27DivMode >= 0 ? sP27DivMode : NpP27DivInit();
    if (m != 0 && before == 0.0f && fabsf(w) <= 3.0e38f)
    {
        if (m == 1 && sP27DivBreak == 0 && NpGt18CntOff())
            return 1.0f;
        float t = 1.0f;
        sP27DivSkipped++;
        if (sP27DivBreak == 1 && (sP27DivSkipped & 3) == 0)
            t = 1.0009765625f;   // proof mutant
        if (m == 2)
        {
            sP27DivChecked++;
            const float d = w / acc;
            if (memcmp(&d, &t, sizeof(float)) != 0)
                sP27DivWrong++;
            if ((sP27DivChecked % 400000) == 0)
                printf("[phys27div] checked=%llu wrong=%llu\n", sP27DivChecked, sP27DivWrong);
            return d;   // verify decides nothing: the division's value
        }
        return t;
    }
    if (!NpGt18CntOff())
        sP27DivDone++;
    return w / acc;
}
}

static nlQuaternion qRotIdentity = { 0.0f, 0.0f, 0.0f, 1.0f };
static nlVector3 v3ScaleIdentity = { 1.0f, 1.0f, 1.0f };
static nlVector3 v3TransIdentity = { 0.0f, 0.0f, 0.0f };

static inline void PoseAccumulatorMultiplyScale(
    nlVector3& result, const nlVector3& parent, const nlVector3& scale)
{
    result.x = parent.x * scale.x;
    result.y = parent.y * scale.y;
    result.z = parent.z * scale.z;
}

/**
 * Offset/Address/Size: 0x0 | 0x8030A9D0 | size: 0x228
 */
cPoseAccumulator::cPoseAccumulator(
    cSHierarchy* pSHierarchy, bool bStorePrevNodeMatrices)
{
    m_BaseSHierarchy = pSHierarchy;
    m_Scale = 1.0f;
    m_bUseObject = false;
    m_nNumNodes = pSHierarchy->m_nNumNodes;

    if (bStorePrevNodeMatrices)
    {
        m_PrevNodeMatrices = new (8, false) nlMatrix4[m_nNumNodes + 1];
    }
    else
    {
        m_PrevNodeMatrices = NULL;
    }

    m_NodeMatrices = new (8, false) nlMatrix4[m_nNumNodes + 1];
    m_pQuaternions = new (8, false) nlQuaternion[m_nNumNodes + 1];
    m_rot = new (8, false) RotAccum[m_nNumNodes];
    m_scale = new (8, false) ScaleAccum[m_nNumNodes];
    m_trans = new (8, false) TransAccum[m_nNumNodes];
    m_cb = new (8, false) cBuildNodeMatrixCallbackInfo[m_nNumNodes];

    for (int i = 0; i < m_BaseSHierarchy->m_nNumNodes; ++i)
    {
        if (m_PrevNodeMatrices != NULL)
        {
            m_PrevNodeMatrices[i].SetIdentity();
        }
        m_NodeMatrices[i].SetIdentity();
        m_cb[i].funcCallback = NULL;
        nlQuaternion& q = m_pQuaternions[i];
        q.x = q.y = q.z = 0.0f;
        q.w = 1.0f;

        if (m_BaseSHierarchy->PreserveBoneLength(i))
        {
            const nlVector3& t = m_BaseSHierarchy->GetTranslationOffset(i);
            m_trans[i].t = t;
            m_trans[i].fAccumulatedWeight = 1.0f;
            m_trans[i].bIdentity = false;
        }
    }
}

/**
 * Offset/Address/Size: 0x228 | 0x8030ABF8 | size: 0x11C
 */
cPoseAccumulator::cPoseAccumulator(const cPoseAccumulator& other)
{
    new (this) cPoseAccumulator(
        other.m_BaseSHierarchy, other.m_PrevNodeMatrices != NULL);

    m_BaseSHierarchy = other.m_BaseSHierarchy;
    if (m_PrevNodeMatrices != NULL && other.m_PrevNodeMatrices != NULL)
    {
        memcpy(m_PrevNodeMatrices, other.m_PrevNodeMatrices,
            other.m_nNumNodes * sizeof(nlMatrix4));
    }
    memcpy(m_NodeMatrices, other.m_NodeMatrices,
        other.m_nNumNodes * sizeof(nlMatrix4));
    memcpy(m_pQuaternions, other.m_pQuaternions,
        other.m_nNumNodes * sizeof(nlQuaternion));
    memcpy(m_rot, other.m_rot, other.m_nNumNodes * sizeof(RotAccum));
    memcpy(m_scale, other.m_scale, other.m_nNumNodes * sizeof(ScaleAccum));
    memcpy(m_trans, other.m_trans, other.m_nNumNodes * sizeof(TransAccum));
    memcpy(m_cb, other.m_cb,
        other.m_nNumNodes * sizeof(cBuildNodeMatrixCallbackInfo));
    memcpy(&m_MorphWeights, &other.m_MorphWeights,
        sizeof(MorphWeightAccum));
    m_Scale = other.m_Scale;
    m_bUseObject = other.m_bUseObject;
}

/**
 * Offset/Address/Size: 0x344 | 0x8030AD14 | size: 0x100
 */
cPoseAccumulator& cPoseAccumulator::operator=(const cPoseAccumulator& other)
{
    m_BaseSHierarchy = other.m_BaseSHierarchy;
    if (m_PrevNodeMatrices != NULL && other.m_PrevNodeMatrices != NULL)
    {
        memcpy(m_PrevNodeMatrices, other.m_PrevNodeMatrices,
            other.m_nNumNodes * sizeof(nlMatrix4));
    }
    memcpy(m_NodeMatrices, other.m_NodeMatrices,
        other.m_nNumNodes * sizeof(nlMatrix4));
    memcpy(m_pQuaternions, other.m_pQuaternions,
        other.m_nNumNodes * sizeof(nlQuaternion));
    memcpy(m_rot, other.m_rot, other.m_nNumNodes * sizeof(RotAccum));
    memcpy(m_scale, other.m_scale, other.m_nNumNodes * sizeof(ScaleAccum));
    memcpy(m_trans, other.m_trans, other.m_nNumNodes * sizeof(TransAccum));
    memcpy(m_cb, other.m_cb,
        other.m_nNumNodes * sizeof(cBuildNodeMatrixCallbackInfo));
    memcpy(&m_MorphWeights, &other.m_MorphWeights,
        sizeof(MorphWeightAccum));
    m_Scale = other.m_Scale;
    m_bUseObject = other.m_bUseObject;
    return *this;
}

// MSC_SNAP_ONECOPY (np/snap_onecopy.h): operator= without the previous node matrices (see the header); mode 2 poisons them, mode 3 nudges a quaternion (proofs)
void cPoseAccumulator::CopyForSnapshot(const cPoseAccumulator& other, int mode)
{
    m_BaseSHierarchy = other.m_BaseSHierarchy;
    if (mode == 2 && m_PrevNodeMatrices != NULL)
    {
        memset(m_PrevNodeMatrices, 0xFF, other.m_nNumNodes * sizeof(nlMatrix4));
    }
    memcpy(m_NodeMatrices, other.m_NodeMatrices, other.m_nNumNodes * sizeof(nlMatrix4));
    memcpy(m_pQuaternions, other.m_pQuaternions, other.m_nNumNodes * sizeof(nlQuaternion));
    memcpy(m_rot, other.m_rot, other.m_nNumNodes * sizeof(RotAccum));
    memcpy(m_scale, other.m_scale, other.m_nNumNodes * sizeof(ScaleAccum));
    memcpy(m_trans, other.m_trans, other.m_nNumNodes * sizeof(TransAccum));
    memcpy(m_cb, other.m_cb, other.m_nNumNodes * sizeof(cBuildNodeMatrixCallbackInfo));
    memcpy(&m_MorphWeights, &other.m_MorphWeights, sizeof(MorphWeightAccum));
    m_Scale = other.m_Scale;
    m_bUseObject = other.m_bUseObject;
    if (mode == 3 && m_nNumNodes > 2)
    {
        m_pQuaternions[2].x += 0.01f;
    }
}

/**
 * Offset/Address/Size: 0x444 | 0x8030AE14 | size: 0x9C
 */
cPoseAccumulator::~cPoseAccumulator()
{
    if (m_PrevNodeMatrices != NULL)
    {
        delete[] m_PrevNodeMatrices;
    }
    delete[] m_NodeMatrices;
    delete[] m_pQuaternions;
    delete[] m_rot;
    delete[] m_scale;
    delete[] m_trans;
    delete[] m_cb;
}

static inline void PoseAccumulatorInitNodeAccumulators(cPoseAccumulator* pose)
{
    // (the flag store was inside the loop, one byte store a node that the compiler cannot hoist past the char stores; it is made once, when a node exists)
    if (pose->m_BaseSHierarchy->m_nNumNodes > 0)
        pose->m_bUseObject = false;
    for (int i = 0; i < pose->m_BaseSHierarchy->m_nNumNodes; ++i)
    {
        RotAccum& r = pose->m_rot[i];
        r.q.x = 0.0f;
        r.q.y = 0.0f;
        r.q.z = 0.0f;
        r.q.w = 1.0f;
        r.quatAccumulatedWeight = 0.0f;
        r.rotAroundZ = 0;
        r.rotAroundZAccumulatedWeight = 0.0f;
        r.bIdentity = true;

        ScaleAccum& s = pose->m_scale[i];
        s.s.x = 1.0f;
        s.s.y = 1.0f;
        s.s.z = 1.0f;
        s.fAccumulatedWeight = 0.0f;
        s.bIdentity = true;

        if (!pose->m_BaseSHierarchy->PreserveBoneLength(i))
        {
            TransAccum& t = pose->m_trans[i];
            t.t.x = 0.0f;
            t.t.y = 0.0f;
            t.t.z = 0.0f;
            t.fAccumulatedWeight = 0.0f;
            t.bIdentity = true;
        }
    }
}

static inline void PoseAccumulatorClearMorphWeights(cPoseAccumulator* pose)
{
    for (int i = 0; i < 20; ++i)
    {
        pose->m_MorphWeights.mData[i] = 0.0f;
    }
}

/**
 * Offset/Address/Size: 0x4E0 | 0x8030AEB0 | size: 0x188
 */
void cPoseAccumulator::Pose(
    const cPoseNode& pPoseTree, const nlMatrix4& pWorldMatrix)
{
    PoseAccumulatorInitNodeAccumulators(this);
    PoseAccumulatorClearMorphWeights(this);
    pPoseTree.Evaluate(1.0f, this);
    BuildNodeMatrices(pWorldMatrix);
}

/**
 * Offset/Address/Size: 0x668 | 0x8030B038 | size: 0x188
 */
void cPoseAccumulator::Pose(
    const cPoseNode* pPoseTree, const nlMatrix4* pWorldMatrix)
{
    PoseAccumulatorInitNodeAccumulators(this);
    PoseAccumulatorClearMorphWeights(this);
    pPoseTree->Evaluate(1.0f, this);
    BuildNodeMatrices(*pWorldMatrix);
}

/**
 * Offset/Address/Size: 0x7F0 | 0x8030B1C0 | size: 0x158
 */
void cPoseAccumulator::InitAccumulators()
{
    if (m_BaseSHierarchy->m_nNumNodes > 0)
        m_bUseObject = false;
    for (int i = 0; i < m_BaseSHierarchy->m_nNumNodes; ++i)
    {
        RotAccum& r = m_rot[i];
        r.q.x = 0.0f;
        r.q.y = 0.0f;
        r.q.z = 0.0f;
        r.q.w = 1.0f;
        r.quatAccumulatedWeight = 0.0f;
        r.rotAroundZ = 0;
        r.rotAroundZAccumulatedWeight = 0.0f;
        r.bIdentity = true;

        ScaleAccum& s = m_scale[i];
        s.s.x = 1.0f;
        s.s.y = 1.0f;
        s.s.z = 1.0f;
        s.fAccumulatedWeight = 0.0f;
        s.bIdentity = true;

        if (!m_BaseSHierarchy->PreserveBoneLength(i))
        {
            TransAccum& t = m_trans[i];
            t.t.x = 0.0f;
            t.t.y = 0.0f;
            t.t.z = 0.0f;
            t.fAccumulatedWeight = 0.0f;
            t.bIdentity = true;
        }
    }

    for (int k = 0; k < 20; ++k)
    {
        m_MorphWeights.mData[k] = 0.0f;
    }
}

void cPoseAccumulator::BuildNodeMatrices(const nlMatrix4& pWorldMatrix)
{
    BuildNodeMatricesImpl(pWorldMatrix, false);
}

// link shim: the two-argument form of an intermediate wave 11 header, kept so that an object compiled against it (the shared console build root keeps objects across branches) still links
void cPoseAccumulator::BuildNodeMatrices(const nlMatrix4& pWorldMatrix, bool renderPose)
{
    BuildNodeMatricesImpl(pWorldMatrix, renderPose);
}

void cPoseAccumulator::BuildNodeMatricesRender(const nlMatrix4& pWorldMatrix)
{
    BuildNodeMatricesImpl(pWorldMatrix, true);   // MSC_RENDER_NEON: a render pose
}

/**
 * Offset/Address/Size: 0x948 | 0x8030B318 | size: 0x6B0
 */
// Preserve the shared scalar lifetimes used to remove world scale.
#pragma opt_lifetimes off
void cPoseAccumulator::BuildNodeMatricesImpl(const nlMatrix4& pWorldMatrix, bool renderPose)
{
    // MSC_RENDER_NEON: only the blended snapshot poses of the drawable characters pass renderPose; the simulation's accumulators never do
    const bool neon = renderPose && NpRenderNeon() != 0;
    float scratch0, scratch1, scratch2, scratch3, scratch4, scratch5, scratch6, scratch7, scratch8, scaledX, rightZ, rightY, rightX, forwardZ, forwardY, forwardX;
    register float fWorldScaleY, fWorldScaleX;
    float scaleUpper, scaleLower, weightEpsilon;

    if (m_PrevNodeMatrices != NULL)
    {
        nlMatrix4* pTemp = m_PrevNodeMatrices;
        m_PrevNodeMatrices = m_NodeMatrices;
        m_NodeMatrices = pTemp;
    }

    int ParentStack[32];
    bool ScaleIdentityStack[33];
    nlVector3 ScaleStack[33];
    int nStackIndex = -1;
    int nScaleIndex = 0;

    if (m_scale[0].bIdentity && NP_FP_ABSLT(m_Scale - 1.0f, 0.0001f))
    {
        ScaleIdentityStack[0] = true;
        ScaleStack[0].x = 1.0f;
        ScaleStack[0].y = 1.0f;
        ScaleStack[0].z = 1.0f;
    }
    else
    {
        ScaleIdentityStack[0] = false;
        nlVec3Scale(ScaleStack[0], m_scale[0].s, m_Scale);
    }

    const bool prefetchNodes = NpPrefetchOn(1);
    if (prefetchNodes)
    {
        // the matrices of the buffer that was last written a picture ago, the first accumulators and quaternions
        NpPrefetchLines(&m_NodeMatrices[1], 4 * sizeof(nlMatrix4), 1);
        NpPrefetchLines(&m_pQuaternions[1], 8 * sizeof(nlQuaternion), 1);
        NpPrefetchLines(&m_rot[0], 4 * sizeof(m_rot[0]), 0);
        NpPrefetchLines(&m_trans[0], 4 * sizeof(m_trans[0]), 0);
    }
    for (int i = 0; i < m_BaseSHierarchy->m_nNumNodes; ++i)
    {
        if (prefetchNodes)
        {
            NpPrefetchLines(&m_NodeMatrices[i + 5], sizeof(nlMatrix4), 1);
            NpPrefetchLines(&m_rot[i + 4], sizeof(m_rot[0]), 0);
            NpPrefetchLines(&m_trans[i + 4], sizeof(m_trans[0]), 0);
        }
        nlMatrix4* pLocalMatrix = &m_NodeMatrices[i + 1];
        nlQuaternion* pLocalQuaternion = &m_pQuaternions[i + 1];
        if (!m_rot[i].bIdentity)
        {
            if (NP_FP_ISZERO(m_rot[i].quatAccumulatedWeight))
            {
                fn_802B549C(*pLocalQuaternion, m_rot[i].rotAroundZ);
            }
            else
            {
                float fTotalWeight = m_rot[i].rotAroundZAccumulatedWeight
                                   + m_rot[i].quatAccumulatedWeight;
                weightEpsilon = 0.0001f;
                if (NP_FP_ABSGT(fTotalWeight, 0.0001f))
                {
                    // Every bone with no around-Z weight (t below is 0) blended a sine and cosine it then multiplied by 0: MSC_POSE_FAST skips them, same bits.
                    if (NP_FP_NOTZERO(m_rot[i].rotAroundZAccumulatedWeight) || !NpPoseFast() || !nlQuatRenormalize(m_rot[i].q))
                    {
                        nlQuaternion quatAroundZ;
                        fn_802B549C(quatAroundZ, m_rot[i].rotAroundZ);
                        nlQuatNLerp(m_rot[i].q, m_rot[i].q, quatAroundZ, m_rot[i].rotAroundZAccumulatedWeight / fTotalWeight);
                    }
                }
                *pLocalQuaternion = m_rot[i].q;
            }
        }
        else
        {
            pLocalQuaternion->x = pLocalQuaternion->y = pLocalQuaternion->z = 0.0f;
            pLocalQuaternion->w = 1.0f;
        }

        TransAccum& t = m_trans[i];
        if (!t.bIdentity)
        {
            pLocalMatrix->m41 = t.t.x;
            pLocalMatrix->m42 = t.t.y;
            pLocalMatrix->m43 = t.t.z;
            pLocalMatrix->m44 = 1.0f;
        }
        else
        {
            pLocalMatrix->m41 = 0.0f;
            pLocalMatrix->m42 = 0.0f;
            pLocalMatrix->m43 = 0.0f;
            pLocalMatrix->m44 = 1.0f;
        }

        nlVector3 v3Position;
        int nParentIndex = -1;
        if (i > 0)
        {
            nParentIndex = ParentStack[nStackIndex];
            nScaleIndex = nStackIndex + 1;
            if (neon)
                NpRQuatMult(m_pQuaternions[i], m_pQuaternions[nParentIndex], *pLocalQuaternion);
            else
                nlMultQuat(m_pQuaternions[i], m_pQuaternions[nParentIndex], *pLocalQuaternion);
            if (neon)
                NpRMultPosVectorMatrix(v3Position, pLocalMatrix->GetTranslation(), m_NodeMatrices[nParentIndex]);
            else
                nlMultPosVectorMatrix(v3Position, pLocalMatrix->GetTranslation(), m_NodeMatrices[nParentIndex]);

            ScaleAccum& s = m_scale[i];
            if (s.bIdentity)
            {
                ScaleIdentityStack[nScaleIndex]
                    = ScaleIdentityStack[nStackIndex];
                if (!ScaleIdentityStack[nStackIndex])
                {
                    ScaleStack[nScaleIndex] = ScaleStack[nStackIndex];
                }
            }
            else
            {
                ScaleIdentityStack[nScaleIndex] = false;
                if (ScaleIdentityStack[nStackIndex])
                {
                    ScaleStack[nScaleIndex] = s.s;
                }
                else
                {
                    PoseAccumulatorMultiplyScale(ScaleStack[nScaleIndex],
                        ScaleStack[nStackIndex],
                        s.s);
                }
            }
        }
        else
        {
            nlQuaternion qWorld;
            nlVector3 v3WorldScaleSquared;
            v3WorldScaleSquared.x = pWorldMatrix.m11 * pWorldMatrix.m11
                                  + pWorldMatrix.m12 * pWorldMatrix.m12
                                  + pWorldMatrix.m13 * pWorldMatrix.m13;
            v3WorldScaleSquared.y = pWorldMatrix.m21 * pWorldMatrix.m21
                                  + pWorldMatrix.m22 * pWorldMatrix.m22
                                  + pWorldMatrix.m23 * pWorldMatrix.m23;
            v3WorldScaleSquared.z = pWorldMatrix.m31 * pWorldMatrix.m31
                                  + pWorldMatrix.m32 * pWorldMatrix.m32
                                  + pWorldMatrix.m33 * pWorldMatrix.m33;

            scaleLower = 0.9999f;
            scaleUpper = 1.0001f;
            if (v3WorldScaleSquared.x < scaleLower || v3WorldScaleSquared.x > scaleUpper
                || v3WorldScaleSquared.y < scaleLower || v3WorldScaleSquared.y > scaleUpper
                || v3WorldScaleSquared.z < scaleLower || v3WorldScaleSquared.z > scaleUpper)
            {
                // Capture the floating-point return value before the next call.
#ifdef NP_NATIVE
                fWorldScaleX = nlSqrt(v3WorldScaleSquared.x, true);
                fWorldScaleY = nlSqrt(v3WorldScaleSquared.y, true);
#else
                nlSqrt(v3WorldScaleSquared.x, true);
                // clang-format off
                asm { fmr fWorldScaleX, f1; }
                // clang-format on
                nlSqrt(v3WorldScaleSquared.y, true);
                // clang-format off
                asm { fmr fWorldScaleY, f1; }
                // clang-format on
#endif
                float fWorldScaleZ = nlSqrt(v3WorldScaleSquared.z, true);

                nlMatrix4 mWorldNoScale;

                ScaleIdentityStack[0] = false;
                mWorldNoScale.m14 = 0.0f;
                scratch7 = 1.0f / fWorldScaleX;
                scratch5 = pWorldMatrix.m12;
                scratch0 = pWorldMatrix.m11;
                scratch6 = pWorldMatrix.m13;
                rightX = scratch7 * scratch0;
                scratch3 = pWorldMatrix.m33;
                scratch4 = 1.0f / fWorldScaleZ;
                scratch2 = pWorldMatrix.m32;
                scratch0 = pWorldMatrix.m31;
                scratch8 = ScaleStack[0].y;
                scaledX = ScaleStack[0].x;
                forwardZ = scratch4 * scratch3;
                scratch3 = pWorldMatrix.m41;
                rightY = scratch7 * scratch5;
                rightZ = scratch7 * scratch6;
                scratch6 = ScaleStack[0].z;
                forwardY = scratch4 * scratch2;
                scratch2 = pWorldMatrix.m42;
                forwardX = scratch4 * scratch0;
                scratch0 = pWorldMatrix.m43;
                scratch6 = scratch6 * fWorldScaleZ;
                mWorldNoScale.m24 = 0.0f;
                scratch7 = scratch8 * fWorldScaleY;
                mWorldNoScale.m34 = 0.0f;
                scratch5 = forwardZ * rightY;
                scaledX = scaledX * fWorldScaleX;
                ScaleStack[0].z = scratch6;
                scratch4 = forwardY * rightX;
                scratch8 = forwardY * rightZ - scratch5;
                ScaleStack[0].y = scratch7;
                scratch1 = forwardZ * rightX;
                scratch5 = forwardX * rightY - scratch4;
                ScaleStack[0].x = scaledX;
                scratch4 = -forwardX;
                mWorldNoScale.m11 = rightX;
                scratch1 = scratch4 * rightZ + scratch1;
                mWorldNoScale.m12 = rightY;
                mWorldNoScale.m13 = rightZ;
                mWorldNoScale.m31 = forwardX;
                mWorldNoScale.m32 = forwardY;
                mWorldNoScale.m33 = forwardZ;
                mWorldNoScale.m21 = scratch8;
                mWorldNoScale.m22 = scratch1;
                mWorldNoScale.m23 = scratch5;
                mWorldNoScale.m41 = scratch3;
                mWorldNoScale.m42 = scratch2;
                mWorldNoScale.m43 = scratch0;
                mWorldNoScale.m44 = 1.0f;
                nlMatrixToQuat(qWorld, mWorldNoScale);
            }
            else
            {
                nlMatrixToQuat(qWorld, pWorldMatrix);
            }
            if (neon)
                NpRQuatMult(m_pQuaternions[i], qWorld, *pLocalQuaternion);
            else
                nlMultQuat(m_pQuaternions[i], qWorld, *pLocalQuaternion);
            if (neon)
                NpRMultPosVectorMatrix(v3Position, pLocalMatrix->GetTranslation(), pWorldMatrix);
            else
                nlMultPosVectorMatrix(v3Position, pLocalMatrix->GetTranslation(), pWorldMatrix);
        }

        nlQuatToMatrix(m_NodeMatrices[i], m_pQuaternions[i], false);
        {
            nlMatrix4& mNode = m_NodeMatrices[i];
            mNode.m41 = v3Position.x;
            mNode.m42 = v3Position.y;
            mNode.m43 = v3Position.z;
            mNode.m44 = 1.0f;
        }

        int nPushPop = m_BaseSHierarchy->GetPushPop(i);
        nStackIndex += nPushPop;
        if (nPushPop > 0)
        {
            ParentStack[nStackIndex] = i;
        }

        if (!ScaleIdentityStack[nScaleIndex])
        {
            nlMatrix4& mNode = m_NodeMatrices[i];
            nlVec3Scale(*(nlVector3*)&mNode.e2[0][0], ScaleStack[nScaleIndex].x);
            nlVec3Scale(*(nlVector3*)&mNode.e2[1][0], ScaleStack[nScaleIndex].y);
            nlVec3Scale(*(nlVector3*)&mNode.e2[2][0], ScaleStack[nScaleIndex].z);
        }

        cBuildNodeMatrixCallbackInfo* pCallback = &m_cb[i];
        if (pCallback->funcCallback != NULL)
        {
            pCallback->funcCallback(pCallback->nParam1, pCallback->nParam2, this, i, nParentIndex);
        }
    }
}

#pragma opt_lifetimes reset


/**
 * Offset/Address/Size: 0xFF8 | 0x8030B9C8 | size: 0x350
 */
void cPoseAccumulator::BuildNodeMatricesFromQuaternions(
    const nlMatrix4& pWorldMatrix)
{
    bool ScaleIdentityStack[32];
    nlVector3 ScaleStack[32];
    int nStackIndex = 0;

    if (m_scale[0].bIdentity
        && NP_FP_ABSLT(m_Scale - 1.0f, 0.0001f))
    {
        ScaleIdentityStack[0] = true;
    }
    else
    {
        ScaleIdentityStack[0] = false;
        nlVec3Scale(ScaleStack[0], m_scale[0].s,
            m_Scale);
    }

    for (int i = 0; i < m_BaseSHierarchy->m_nNumNodes; ++i)
    {
        nlVector3 v3Position;
        if (!m_trans[i].bIdentity)
        {
            v3Position = m_trans[i].t;
        }
        else
        {
            v3Position.x = 0.0f;
            v3Position.y = 0.0f;
            v3Position.z = 0.0f;
        }

        if (i > 0)
        {
            int nPreviousScaleIndex = nStackIndex - 1;
            ScaleAccum& s = m_scale[i];
            if (s.bIdentity)
            {
                ScaleIdentityStack[nStackIndex]
                    = ScaleIdentityStack[nPreviousScaleIndex];
                if (!ScaleIdentityStack[nPreviousScaleIndex])
                {
                    ScaleStack[nStackIndex] = ScaleStack[nPreviousScaleIndex];
                }
            }
            else
            {
                ScaleIdentityStack[nStackIndex] = false;
                if (ScaleIdentityStack[nPreviousScaleIndex])
                {
                    ScaleStack[nStackIndex] = s.s;
                }
                else
                {
                    PoseAccumulatorMultiplyScale(ScaleStack[nStackIndex],
                        ScaleStack[nPreviousScaleIndex], s.s);
                }
            }

            int nParentIndex = m_BaseSHierarchy->GetParent(i);
            nlMultPosVectorMatrix(
                v3Position, m_NodeMatrices[nParentIndex]);
        }
        else
        {
            nlMultPosVectorMatrix(v3Position, pWorldMatrix);
        }

        nlMatrix4& mNode = m_NodeMatrices[i];
        nlQuatToMatrix(mNode, m_pQuaternions[i], false);
        mNode.m41 = v3Position.x;
        mNode.m42 = v3Position.y;
        mNode.m43 = v3Position.z;
        mNode.m44 = 1.0f;

        if (!ScaleIdentityStack[nStackIndex])
        {
            nlVec3Scale(*(nlVector3*)&mNode.e2[0][0], ScaleStack[nStackIndex].x);
            nlVec3Scale(*(nlVector3*)&mNode.e2[1][0], ScaleStack[nStackIndex].y);
            nlVec3Scale(*(nlVector3*)&mNode.e2[2][0], ScaleStack[nStackIndex].z);
        }

        nStackIndex += m_BaseSHierarchy->GetPushPop(i);
    }
}

/**
 * Offset/Address/Size: 0x1348 | 0x8030BD18 | size: 0x150
 */
void cPoseAccumulator::BlendRot(int nNode, const nlQuaternion* pRot,
    float fWeight, bool bMirror)
{
    RotAccum* e = m_rot + nNode;
    bool bNegligibleWeight = NP_FP_ABSLT(fWeight, 0.001f);
    if (bNegligibleWeight)
        return;

    nlQuaternion qtemp;
    if (bMirror)
    {
        cSHierarchy* h = m_BaseSHierarchy;
        if (nNode == h->m_nSpineNodeIndex
            || nNode == h->m_nPelvisNodeIndex)
        {
            qtemp.x = -pRot->w;
            qtemp.y = pRot->z;
            qtemp.z = pRot->y;
            qtemp.w = -pRot->x;
        }
        else if (nNode < h->m_nPelvisNodeIndex)
        {
            qtemp.x = -pRot->x;
            qtemp.y = pRot->y;
            qtemp.z = -pRot->z;
            qtemp.w = pRot->w;
        }
        else
        {
            qtemp.x = -pRot->x;
            qtemp.y = -pRot->y;
            qtemp.z = pRot->z;
            qtemp.w = pRot->w;
        }
        pRot = &qtemp;
    }

    const float before = e->quatAccumulatedWeight;
    e->quatAccumulatedWeight += fWeight;
    float t = NpWeightT(before, fWeight, e->quatAccumulatedWeight);
    nlQuaternion tmp = e->q;
    nlQuatNLerp(e->q, tmp, *pRot, t);
    e = m_rot + nNode;
    e->bIdentity = false;
}

// BlendRot (no mirror) for the blended render pose of DrawableCharacter::Blend: the same accumulation, the nlerp through the MSC_RENDER_NEON wrapper
void cPoseAccumulator::BlendRotRender(int nNode, const nlQuaternion* pRot, float fWeight)
{
    RotAccum* e = m_rot + nNode;
    if (NP_FP_ABSLT(fWeight, 0.001f))
        return;
    e->quatAccumulatedWeight += fWeight;
    float t = fWeight / e->quatAccumulatedWeight;
    nlQuaternion tmp = e->q;
    NpRQuatNLerp(e->q, tmp, *pRot, t);
    e->bIdentity = false;
}

/**
 * Offset/Address/Size: 0x1498 | 0x8030BE68 | size: 0x98
 */
void cPoseAccumulator::BlendRotAroundZ(
    int nNode, unsigned short rot, float fWeight)
{
    bool bNegligibleWeight = NP_FP_ABSLT(fWeight, 0.001f);
    if (bNegligibleWeight)
        return;

    RotAccum* e = m_rot + nNode;
    const float before = e->rotAroundZAccumulatedWeight;
    e->rotAroundZAccumulatedWeight += fWeight;
    float t = NpWeightT(before, fWeight, e->rotAroundZAccumulatedWeight);
    int delta = (short)(rot - e->rotAroundZ);
    delta = (short)(t * delta);
    e->rotAroundZ = e->rotAroundZ + delta;
    e = m_rot + nNode;
    e->bIdentity = false;
}

/**
 * Offset/Address/Size: 0x1530 | 0x8030BF00 | size: 0x88
 */
void cPoseAccumulator::BlendScale(int nNode, const nlVector3* pScale,
    float fWeight, bool bMirror)
{
    bool bNegligibleWeight = NP_FP_ABSLT(fWeight, 0.001f);
    if (bNegligibleWeight)
        return;

    ScaleAccum* e = m_scale + nNode;
    const float before = e->fAccumulatedWeight;
    e->fAccumulatedWeight += fWeight;
    float t = NpWeightT(before, fWeight, e->fAccumulatedWeight);
    float inv = 1.0f - t;
    e->s.x = inv * e->s.x + t * pScale->x;
    e->s.y = inv * e->s.y + t * pScale->y;
    e->s.z = inv * e->s.z + t * pScale->z;
    e->bIdentity = false;
}

/**
 * Offset/Address/Size: 0x15B8 | 0x8030BF88 | size: 0xFC
 */
void cPoseAccumulator::BlendTrans(int nNode, const nlVector3* pTrans,
    float fWeight, bool bMirror)
{
    bool bNegligibleWeight = NP_FP_ABSLT(fWeight, 0.001f);
    if (bNegligibleWeight)
        return;

    // vtemp is read through pTrans after the if block: it must outlive the block (a block-scope local here is a dangling pointer
    // as soon as the compiler reuses its stack slot; GCC -Wdangling-pointer)
    nlVector3 vtemp;
    if (bMirror)
    {
        cSHierarchy* h = m_BaseSHierarchy;
        if (nNode <= h->m_nPelvisNodeIndex
            || nNode == h->m_nSpineNodeIndex)
        {
            vtemp.x = pTrans->x;
            vtemp.y = -pTrans->y;
            vtemp.z = pTrans->z;
        }
        else
        {
            vtemp.x = pTrans->x;
            vtemp.y = pTrans->y;
            vtemp.z = -pTrans->z;
        }
        pTrans = &vtemp;
    }

    TransAccum* e = m_trans + nNode;
    const float before = e->fAccumulatedWeight;
    e->fAccumulatedWeight += fWeight;
    float t = NpWeightT(before, fWeight, e->fAccumulatedWeight);
    float inv = 1.0f - t;
    e->t.x = inv * e->t.x + t * pTrans->x;
    e->t.y = inv * e->t.y + t * pTrans->y;
    e->t.z = inv * e->t.z + t * pTrans->z;
    e = m_trans + nNode;
    e->bIdentity = false;
}

/**
 * Offset/Address/Size: 0x16B4 | 0x8030C084 | size: 0x94
 */
void cPoseAccumulator::BlendRotIdentity(int nNode, float fWeight)
{
    bool bNegligibleWeight = NP_FP_ABSLT(fWeight, 0.001f);
    if (bNegligibleWeight)
        return;

    RotAccum* a = &m_rot[nNode];
    const float before = a->quatAccumulatedWeight;
    a->quatAccumulatedWeight += fWeight;
    if (a->bIdentity)
        return;

    const float t = NpWeightT(before, fWeight, a->quatAccumulatedWeight);
    nlQuaternion tmp = a->q;
    nlQuatNLerp(a->q, tmp, qRotIdentity, t);
}

/**
 * Offset/Address/Size: 0x1748 | 0x8030C118 | size: 0x98
 */
void cPoseAccumulator::BlendScaleIdentity(int nNode, float fWeight)
{
    bool bNegligibleWeight = NP_FP_ABSLT(fWeight, 0.001f);
    if (bNegligibleWeight)
        return;

    ScaleAccum* e = &m_scale[nNode];
    const float before = e->fAccumulatedWeight;
    e->fAccumulatedWeight += fWeight;
    if (e->bIdentity)
        return;

    float f3 = NpWeightT(before, fWeight, e->fAccumulatedWeight);
    float f2 = 1.0f - f3;
    e->s.x = f2 * e->s.x + f3 * v3ScaleIdentity.x;
    e->s.y = f2 * e->s.y + f3 * v3ScaleIdentity.y;
    e->s.z = f2 * e->s.z + f3 * v3ScaleIdentity.z;
}

/**
 * Offset/Address/Size: 0x17E0 | 0x8030C1B0 | size: 0xA8
 */
void cPoseAccumulator::MultiplyScale(
    int nNode, const nlVector3* pScale, float fWeight)
{
    bool bNegligibleWeight = NP_FP_ABSLT(fWeight, 0.001f);
    if (bNegligibleWeight)
        return;

    nlVector3 v3Scale;
    float fInvWeight = 1.0f - fWeight;
    v3Scale.x = fInvWeight * v3ScaleIdentity.x + fWeight * pScale->x;
    v3Scale.y = fInvWeight * v3ScaleIdentity.y + fWeight * pScale->y;
    v3Scale.z = fInvWeight * v3ScaleIdentity.z + fWeight * pScale->z;

    ScaleAccum* e = &m_scale[nNode];
    e->s.x *= v3Scale.x;
    e->s.y *= v3Scale.y;
    e->s.z *= v3Scale.z;
    e->bIdentity = false;
}

/**
 * Offset/Address/Size: 0x1888 | 0x8030C258 | size: 0x98
 */
void cPoseAccumulator::BlendTransIdentity(int nNode, float fWeight)
{
    bool bNegligibleWeight = NP_FP_ABSLT(fWeight, 0.001f);
    if (bNegligibleWeight)
        return;

    TransAccum* e = &m_trans[nNode];
    const float before = e->fAccumulatedWeight;
    e->fAccumulatedWeight += fWeight;
    if (e->bIdentity)
        return;

    const float f3 = NpWeightT(before, fWeight, e->fAccumulatedWeight);
    const float f2 = 1.0f - f3;
    e->t.x = f2 * e->t.x + f3 * v3TransIdentity.x;
    e->t.y = f2 * e->t.y + f3 * v3TransIdentity.y;
    e->t.z = f2 * e->t.z + f3 * v3TransIdentity.z;
}

/**
 * Offset/Address/Size: 0x1920 | 0x8030C2F0 | size: 0x10
 */
nlMatrix4& cPoseAccumulator::GetNodeMatrix(int nNode) const
{
    return m_NodeMatrices[nNode];
}

/**
 * Offset/Address/Size: 0x1930 | 0x8030C300 | size: 0x74
 */
nlMatrix4& cPoseAccumulator::GetNodeMatrixByHashID(
    unsigned int nHashID) const
{
    int index = 0;
    while (index < m_BaseSHierarchy->m_nNumNodes)
    {
        unsigned int nodeID = m_BaseSHierarchy->GetNodeID(index);
        if (nHashID == nodeID)
        {
            break;
        }
        ++index;
    }
    return m_NodeMatrices[index];
}

/**
 * Offset/Address/Size: 0x19A4 | 0x8030C374 | size: 0xC
 */
s32 cPoseAccumulator::GetNumNodes() const
{
    return m_BaseSHierarchy->m_nNumNodes;
}

/**
 * Offset/Address/Size: 0x19B0 | 0x8030C380 | size: 0x28
 */
void cPoseAccumulator::SetBuildNodeMatrixCallback(int nNode,
    BuildNodeMatrixFn funcCallback, unsigned int nParam1,
    unsigned int nParam2)
{
    m_cb[nNode].funcCallback = funcCallback;
    m_cb[nNode].nParam1 = nParam1;
    m_cb[nNode].nParam2 = nParam2;
}
