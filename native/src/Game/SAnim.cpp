#include "Game/SAnim.h"
#include "Game/SAnimDecode.h"

#include "Game/PoseAccumulator.h"
#include "Game/SHierarchy.h"
#include "NL/nlSlotPool.h"
#include "port/chunk_endian.h"

SlotPool<cSAnimCallback> lbl_805840D8(16, 16);

// NP: the key payloads of the per-node chunks, converted in place from big-endian once the node
// properties (which say how each rotation track is packed) have been converted themselves.
// Rotation: u16 angles (props & 1) or four s16 per key (props & 0x10) are 16-bit data; the 12-bit
// and 8-bit forms are byte streams the decoders assemble themselves, so they stay as they are.
// Translation: three floats per key. Scale: three u16 per key. Weight keys are bytes.
static void SwapSAnimNodeKeys(const cSAnim* pAnim, nlChunk* nodeChunk, nlChunk* end)
{
    unsigned int nodeIndex = 0;
    while (nodeChunk != end && nodeChunk->GetID() == 0x80017100)
    {
        const unsigned int props = nodeIndex < pAnim->m_nNumNodes ? pAnim->m_pNodeProperties[nodeIndex] : 0;
        nlChunk* subEnd = nodeChunk->GetLastChunk();
        for (nlChunk* sub = nodeChunk->GetFirstChunk(); sub != subEnd; sub = sub->GetNextChunk())
        {
            const unsigned long bytes = sub->GetDataSize();
            void* data = sub->GetData();
            switch (sub->GetID())
            {
            case 0x17101:
                if (props & 0x11)
                    port_disc16_array(data, bytes / 2);
                break;
            case 0x17102:
                port_disc32_array(data, bytes / 4);
                break;
            case 0x17103:
                port_disc16_array(data, bytes / 2);
                break;
            default:
                break;
            }
        }
        nodeChunk = nodeChunk->GetNextChunk();
        nodeIndex++;
    }
}

cSAnim* cSAnim::Initialize(nlChunk* pChunk)
{
    // NP: big-endian on disc. Convert this animation's chunk headers before anything walks them; a
    // tree whose sizes do not fit is refused instead of being walked out of bounds.
    if (port_disc_chunk_swap_tree(pChunk, port_disc32(&pChunk->m_Size) + sizeof(nlChunk)) == 0)
    {
        return 0;
    }

    cSAnim* pRetval;
    nlChunk* chunkA;
    nlChunk* end = pChunk->GetLastChunk();
    chunkA = pChunk->GetFirstChunk();
    nlChunk* chunkB;

    pRetval = (cSAnim*)chunkA->GetData();
    pRetval->m_pCallbackList = 0;
    // NP: the words of the record that come from the file: hash, key/node/morph-channel counts (four
    // consecutive words from +4), the root key count and the hierarchy signature. The rest is pointers
    // this function fills in.
    port_disc32_array(&pRetval->m_uHashID, 4);
    port_disc32_array(&pRetval->m_nNumRootKeys, 1);
    port_disc32_array(&pRetval->m_nHierarchySignature, 1);

    chunkB = chunkA->GetNextChunk();
    pRetval->m_szName = (const char*)chunkB->GetData();

    chunkA = chunkB->GetNextChunk();
    pRetval->m_pNumWeightKeys = (const unsigned int*)chunkA->GetData();
    port_disc32_array((void*)pRetval->m_pNumWeightKeys, chunkA->GetDataSize() / 4);

    chunkB = chunkA->GetNextChunk();
    pRetval->m_Unknown1C = (const unsigned int*)chunkB->GetData();
    port_disc32_array((void*)pRetval->m_Unknown1C, chunkB->GetDataSize() / 4);

    chunkA = chunkB->GetNextChunk();
    pRetval->m_pRotKeys = (void**)chunkA->GetData();

    chunkB = chunkA->GetNextChunk();
    pRetval->m_pTransKeys = (PackedTrans**)chunkB->GetData();

    chunkA = chunkB->GetNextChunk();
    pRetval->m_pScaleKeys = (PackedScale**)chunkA->GetData();

    chunkB = chunkA->GetNextChunk();
    pRetval->m_pWeightKeys = (unsigned char**)chunkB->GetData();

    chunkA = chunkB->GetNextChunk();
    pRetval->m_Unknown30 = (void**)chunkA->GetData();

    chunkB = chunkA->GetNextChunk();
    pRetval->m_pRootRot = (unsigned short*)chunkB->GetData();
    port_disc16_array(pRetval->m_pRootRot, chunkB->GetDataSize() / 2);

    chunkA = chunkB->GetNextChunk();
    pRetval->m_pRootTrans = (nlVector3*)chunkA->GetData();
    port_disc32_array(pRetval->m_pRootTrans, chunkA->GetDataSize() / 4);

    unsigned int nodeIndex = 0;
    unsigned int type;
    nlChunk* nodeChunk = chunkA->GetNextChunk();
    nlChunk* firstNodeChunk = nodeChunk;
    while (nodeChunk != end
           && (type = nodeChunk->GetID()) == 0x80017100)
    {
        if (type == 0x80017100)
        {
            pRetval->InitializeNodeKeys(nodeChunk, nodeIndex);
            nodeIndex++;
        }
        nodeChunk = nodeChunk->GetNextChunk();
    }

    nlVector3* rootTrans = pRetval->m_pRootTrans;
    nlVector3 v3PosStart;
    nlVector3 v3PosEnd;

    if (rootTrans != 0)
    {
        pRetval->GetRootTrans(0.0f, &v3PosStart);
        pRetval->GetRootTrans(1.0f, &v3PosEnd);

        nlVector3 v3Delta;
        nlVec3Sub(v3Delta, v3PosEnd, v3PosStart);
        pRetval->m_fLinearSpeed = nlSqrt(nlVec3LengthSquared(v3Delta), true)
            / pRetval->GetDuration();
    }
    else
    {
        pRetval->m_fLinearSpeed = 0.0f;
    }

    pRetval->m_pNumMorphKeys
        = (const unsigned int*)nodeChunk->GetData();
    port_disc32_array((void*)pRetval->m_pNumMorphKeys, nodeChunk->GetDataSize() / 4);

    nodeChunk = nodeChunk->GetNextChunk();
    pRetval->m_nMorphIds = (unsigned long*)nodeChunk->GetData();
    port_disc32_array(pRetval->m_nMorphIds, nodeChunk->GetDataSize() / 4);

    nodeChunk = nodeChunk->GetNextChunk();
    pRetval->m_pMorphKeys = (unsigned char*)nodeChunk->GetData();

    nodeChunk = nodeChunk->GetNextChunk();
    pRetval->m_pNodeProperties
        = (const unsigned int*)nodeChunk->GetData();
    port_disc32_array((void*)pRetval->m_pNodeProperties, nodeChunk->GetDataSize() / 4);

    SwapSAnimNodeKeys(pRetval, firstNodeChunk, end);

    return pRetval;
}

void cSAnim::InitializeNodeKeys(nlChunk* nodeChunk, int nodeIndex)
{
    nlChunk* subEnd;
    nlChunk* subChunk;
    subEnd = nodeChunk->GetLastChunk();
    subChunk = nodeChunk->GetFirstChunk();

    while (subChunk != subEnd)
    {
        unsigned int type = subChunk->GetID();
        if (type == 0x17101)
        {
            m_pRotKeys[nodeIndex] = subChunk->GetData();
        }
        else if (type == 0x17102)
        {
            m_pTransKeys[nodeIndex] = (PackedTrans*)subChunk->GetData();
        }
        else if (type == 0x17103)
        {
            m_pScaleKeys[nodeIndex] = (PackedScale*)subChunk->GetData();
        }
        else if (type == 0x17112)
        {
            m_pWeightKeys[nodeIndex] = (unsigned char*)subChunk->GetData();
        }
        else if (type == 0x17115)
        {
            m_Unknown30[nodeIndex] = subChunk->GetData();
        }

        subChunk = subChunk->GetNextChunk();
    }
}


void cSAnim::BlendRot(int nodeIndex, int remappedNodeIndex, float tNorm,
    float weight, cPoseAccumulator* acc, bool additive) const
{
    void* pRawKeys = m_pRotKeys[remappedNodeIndex];
    if (pRawKeys != 0
        && (unsigned int)remappedNodeIndex < m_nNumNodes)
    {
        unsigned int props = m_pNodeProperties[remappedNodeIndex];

        if (props & 0x2)
        {
            if (props & 0x1)
            {
                acc->BlendRotAroundZ(
                    nodeIndex, ((unsigned short*)pRawKeys)[0], weight);
                return;
            }

            nlQuaternion q;
            if (props & 0x10)
            {
                SAnimDecodeRot16(&q, pRawKeys);
            }
            else if (props & 0x20)
            {
                SAnimDecodeRot12(&q, pRawKeys);
            }
            else
            {
                SAnimDecodeRot8(&q, pRawKeys);
            }
            acc->BlendRot(nodeIndex, &q, weight, additive);
            return;
        }

        if (1.0f == tNorm)
        {
            int lastIndex = m_nNumKeys - 1;

            if (props & 0x1)
            {
                acc->BlendRotAroundZ(nodeIndex,
                    ((unsigned short*)pRawKeys)[lastIndex],
                    weight);
                return;
            }

            nlQuaternion q;
            if (props & 0x10)
            {
                SAnimDecodeRot16(
                    &q, (unsigned char*)pRawKeys + lastIndex * 8);
            }
            else if (props & 0x20)
            {
                SAnimDecodeRot12(
                    &q, (unsigned char*)pRawKeys + lastIndex * 6);
            }
            else
            {
                SAnimDecodeRot8(
                    &q, (unsigned char*)pRawKeys + lastIndex * 4);
            }
            acc->BlendRot(nodeIndex, &q, weight, additive);
            return;
        }

        float fRealIndex = tNorm * (m_nNumKeys - 1);
        int nKeyIndex = (int)fRealIndex;
        float fFrac = fRealIndex - nKeyIndex;
        float fWeight2 = weight * fFrac;
        float fWeight1 = weight - fWeight2;

        if (props & 0x1)
        {
            unsigned short* pKeys = (unsigned short*)pRawKeys;
            acc->BlendRotAroundZ(
                nodeIndex, pKeys[nKeyIndex], fWeight1);
        }
        else
        {
            nlQuaternion q1;
            if (props & 0x10)
            {
                SAnimDecodeRot16(
                    &q1, (unsigned char*)pRawKeys + nKeyIndex * 8);
            }
            else if (props & 0x20)
            {
                SAnimDecodeRot12(
                    &q1, (unsigned char*)pRawKeys + nKeyIndex * 6);
            }
            else
            {
                SAnimDecodeRot8(
                    &q1, (unsigned char*)pRawKeys + nKeyIndex * 4);
            }
            acc->BlendRot(nodeIndex, &q1, fWeight1, additive);
        }

        props = m_pNodeProperties[remappedNodeIndex];
        if (props & 0x1)
        {
            unsigned short* pKeys
                = (unsigned short*)m_pRotKeys[remappedNodeIndex];
            acc->BlendRotAroundZ(
                nodeIndex, pKeys[nKeyIndex + 1], fWeight2);
            return;
        }

        nlQuaternion q2;
        if (props & 0x10)
        {
            SAnimDecodeRot16(&q2,
                (unsigned char*)m_pRotKeys[remappedNodeIndex]
                    + (nKeyIndex + 1) * 8);
        }
        else if (props & 0x20)
        {
            SAnimDecodeRot12(&q2,
                (unsigned char*)m_pRotKeys[remappedNodeIndex]
                    + (nKeyIndex + 1) * 6);
        }
        else
        {
            SAnimDecodeRot8(&q2,
                (unsigned char*)m_pRotKeys[remappedNodeIndex]
                    + (nKeyIndex + 1) * 4);
        }
        acc->BlendRot(nodeIndex, &q2, fWeight2, additive);
        return;
    }

    acc->BlendRotIdentity(nodeIndex, weight);
}


void cSAnim::BlendScale(int nodeIndex, int remappedNodeIndex, float tNorm,
    float weight, cPoseAccumulator* acc, bool additive) const
{
    PackedScale* pKeys = m_pScaleKeys[remappedNodeIndex];
    if (pKeys != 0
        && (unsigned int)remappedNodeIndex < m_nNumNodes)
    {
        if (m_pNodeProperties[remappedNodeIndex] & 0x8)
        {
            nlVector3 v;
            SAnimDecodeScale(&v, &pKeys[0]);
            acc->BlendScale(nodeIndex, &v, weight, additive);
            return;
        }

        if (1.0f == tNorm)
        {
            nlVector3 v;
            SAnimDecodeScale(&v, &pKeys[m_nNumKeys - 1]);
            acc->BlendScale(nodeIndex, &v, weight, additive);
            return;
        }

        float fRealIndex = tNorm * (m_nNumKeys - 1);
        int nKeyIndex = (int)fRealIndex;
        float fFrac = fRealIndex - nKeyIndex;
        float fWeight2 = weight * fFrac;
        float fWeight1 = weight - fWeight2;

        nlVector3 v1;
        SAnimDecodeScale(&v1, &pKeys[nKeyIndex]);
        acc->BlendScale(nodeIndex, &v1, fWeight1, additive);

        nlVector3 v2;
        SAnimDecodeScale(
            &v2, &m_pScaleKeys[remappedNodeIndex][nKeyIndex + 1]);
        acc->BlendScale(nodeIndex, &v2, fWeight2, additive);
    }
    else
    {
        acc->BlendScaleIdentity(nodeIndex, weight);
    }
}


void cSAnim::BlendScaleMultiply(int accumulatorNode, int animNode, float time,
    float weight, cPoseAccumulator* accumulator) const
{
    PackedScale* pKeys = m_pScaleKeys[animNode];
    if (pKeys != 0 && (unsigned int)animNode < m_nNumNodes)
    {
        nlVector3 scale;
        if ((m_pNodeProperties[animNode] & 0x8) || time <= 0.0f)
        {
            SAnimDecodeScale(&scale, &pKeys[0]);
        }
        else if (time >= 1.0f)
        {
            SAnimDecodeScale(&scale, &pKeys[m_nNumKeys - 1]);
        }
        else
        {
            float fRealIndex = time * (m_nNumKeys - 1);
            int nKeyIndex = (int)fRealIndex;
            float fFrac = fRealIndex - nKeyIndex;

            nlVector3 scale1;
            nlVector3 scale2;
            SAnimDecodeScale(&scale1, &pKeys[nKeyIndex]);
            SAnimDecodeScale(
                &scale2, &m_pScaleKeys[animNode][nKeyIndex + 1]);

            scale.x = (1.0f - fFrac) * scale1.x + fFrac * scale2.x;
            scale.y = (1.0f - fFrac) * scale1.y + fFrac * scale2.y;
            scale.z = (1.0f - fFrac) * scale1.z + fFrac * scale2.z;
        }

        accumulator->MultiplyScale(accumulatorNode, &scale, weight);
    }
}


void cSAnim::BlendTrans(int nAccumulatorNode, int nSAnimNode, float fTime,
    float fWeight, cPoseAccumulator* pAccumulator, bool bMirror) const
{
    if (pAccumulator->m_BaseSHierarchy->PreserveBoneLength(nAccumulatorNode))
    {
        return;
    }

    PackedTrans* pKeys = m_pTransKeys[nSAnimNode];
    if (pKeys != 0 && (unsigned int)nSAnimNode < m_nNumNodes)
    {
        if (m_pNodeProperties[nSAnimNode] & 0x4)
        {
            pAccumulator->BlendTrans(nAccumulatorNode,
                (const nlVector3*)&pKeys[0],
                fWeight,
                bMirror);
            return;
        }

        if (1.0f == fTime)
        {
            pAccumulator->BlendTrans(nAccumulatorNode,
                (const nlVector3*)&pKeys[m_nNumKeys - 1],
                fWeight,
                bMirror);
            return;
        }

        float fRealIndex = fTime * (float)(m_nNumKeys - 1);
        int nKeyIndex = (int)fRealIndex;
        float fFrac = fRealIndex - (float)nKeyIndex;
        float fWeight2 = fWeight * fFrac;
        float fWeight1 = fWeight - fWeight2;

        pAccumulator->BlendTrans(nAccumulatorNode,
            (const nlVector3*)&pKeys[nKeyIndex],
            fWeight1,
            bMirror);
        pAccumulator->BlendTrans(nAccumulatorNode,
            (const nlVector3*)&m_pTransKeys[nSAnimNode][nKeyIndex + 1],
            fWeight2,
            bMirror);
    }
    else
    {
        pAccumulator->BlendTransIdentity(nAccumulatorNode, fWeight);
    }
}


void cSAnim::Destroy()
{
    cSAnimCallback* pNext;
    cSAnimCallback* pCallback;
    while ((pCallback = m_pCallbackList) != 0)
    {
        pNext = pCallback->next;
        lbl_805840D8.Free(pCallback);
        m_pCallbackList = pNext;
    }
    m_pCallbackList = 0;
}


void cSAnim::GetRootRot(float fTime, unsigned short* pRootRot) const
{
    if (m_nNumRootKeys != 0)
    {
        if (fTime == 1.0f || m_nNumRootKeys == 1)
        {
            *pRootRot = m_pRootRot[m_nNumRootKeys - 1];
            return;
        }

        float fRealIndex = fTime * (m_nNumRootKeys - 1);
        int nIndex = (int)fRealIndex;
        float fIndex = (float)nIndex;
        unsigned short* pRoots = m_pRootRot;
        unsigned short val0 = pRoots[nIndex];
        signed short diff = (signed short)(pRoots[nIndex + 1] - val0);
        float fFrac = fRealIndex - fIndex;
        float fDiff = (float)diff;
        *pRootRot = val0 + (int)(fFrac * fDiff);
        return;
    }
    *pRootRot = 0;
}


void cSAnim::GetRootTrans(float t, nlVector3* out) const
{
    if (m_nNumRootKeys != 0)
    {
        if (t == 1.0f || m_nNumRootKeys == 1)
        {
            *out = m_pRootTrans[m_nNumRootKeys - 1];
            return;
        }

        float fRealIndex = t * (m_nNumRootKeys - 1);
        int nIndex = (int)fRealIndex;
        float fWeight = fRealIndex - nIndex;
        nlVec3WeightedSum(*out, 1.0f - fWeight, m_pRootTrans[nIndex], fWeight, m_pRootTrans[nIndex + 1]);
        return;
    }
    out->x = 0.0f;
    out->y = 0.0f;
    out->z = 0.0f;
}


bool cSAnim::GetChannelWeight(int channel, float time, float* weight) const
{
    if (m_pWeightKeys == 0 || m_pWeightKeys[channel] == 0)
    {
        *weight = 1.0f;
        return false;
    }

    unsigned char* keys = m_pWeightKeys[channel];
    float fRealIndex = time * (float)(m_pNumWeightKeys[channel] - 1);
    int nIndex = (int)fRealIndex;
    float fWeightB = fRealIndex - (float)nIndex;
    float fWeightA = 1.0f - fWeightB;
    float weightA;
    float weightB;
    SAnimDecodeWeight(&weightA, &keys[nIndex]);
    SAnimDecodeWeight(&weightB, m_pWeightKeys[channel] + nIndex + 1);
    *weight = fWeightA * weightA + fWeightB * weightB;
    return true;
}


void cSAnim::CreateCallback(float fTime, unsigned int nParam1,
    void (*funcCallback)(cSAnim*, unsigned int))
{
    cSAnimCallback* pCallback = 0;
    lbl_805840D8.Allocate(pCallback);

    if (pCallback != 0)
    {
        pCallback->m_fTime = fTime;
        pCallback->m_nParam1 = nParam1;
        pCallback->m_funcCallback = funcCallback;
    }

    pCallback->next = m_pCallbackList;
    m_pCallbackList = pCallback;
}


float cSAnim::GetMorphWeight(int channel, float fTime) const
{
    const unsigned char* keys = m_pMorphKeys;
    int numKeys = m_pNumMorphKeys[channel];
    int i = 0;

    for (i = 0; i < channel; i++)
    {
        keys += m_pNumMorphKeys[channel];
    }

    float weight = 0.0f;
    if (numKeys == 1 || fTime == 1.0f)
    {
        SAnimDecodeMorphWeight(&weight, &keys[numKeys - 1]);
    }
    else
    {
        float fRealIndex = fTime * (float)(numKeys - 1);
        int nIndex = (int)fRealIndex;
        float fWeightB = fRealIndex - (float)nIndex;
        float fWeightA = 1.0f - fWeightB;
        float weightA;
        float weightB;
        const unsigned char* pKey = &keys[nIndex];
        SAnimDecodeMorphWeight(&weightA, pKey);
        SAnimDecodeMorphWeight(&weightB, pKey + 1);
        weight = fWeightA * weightA + fWeightB * weightB;
    }
    return weight;
}
