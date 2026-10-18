#include <revolution/os/OSCache.h>

#include "Game/GL/ShaderSkinMesh.h"
#include <stdlib.h>

#include "Game/SHierarchy.h"
#include "Game/GL/glModelBuilder.h"
#include "NL/gl/glMemory.h"
#include <string.h>
#ifdef NP_NATIVE
#include "platform/assets/np_endian.h" /* GX vertex arrays stay big-endian (NPA) */
#include "np/framesend.h"
#include "np/skingpu.h"
#include "np_gx.h"
#include <stdio.h>
#endif
#include "Game/PoseAccumulator.h"
#include "NL/nlMemory.h"
#include "NL/platvmath.h"
#include "NL/nlDLRing.h"
#include "NL/nlString.h"
#include "NL/gl/glModel.h"
#include "NL/gl/glMaterialParameters.h"
#include "NL/glx/glxMatrix.h"

static const unsigned long SkinMatricesHash = nlStringLowerHash("SkinMatrices");

inline void ShaderSkinMesh::CopyMatrices(BoneMapList* node)
{
    float (*pMatrices)[3][4] = (float (*)[3][4])node->m_pMatrices;
    for (unsigned long i = 0; i < node->m_nBones; i++)
    {
        glxCopyMatrix(*pMatrices, poseMatrices[node->m_pBoneIndices[i]]);
        pMatrices++;
    }
}

ShaderSkinMesh::~ShaderSkinMesh()
{
    nlDeleteRing(&boneMaps);
    if (boneMatrices != 0)
    {
        delete[] boneMatrices;
    }
    if (packetSkinData != 0)
    {
        delete[] packetSkinData;
    }
    if (poseMatrices != 0)
    {
        delete[] poseMatrices;
    }
    if (morphData != 0)
    {
        delete[] morphData;
    }
}

void ShaderSkinMesh::SetNumMorphPackets(unsigned long count)
{
    numPackets = count;
    morphData = (MorphDeltaList*)nlMalloc(
        count * numMorphs * sizeof(MorphDeltaList), 8, false);
}

void ShaderSkinMesh::SetMorphDeltas(unsigned long packetIndex,
    unsigned long morphIndex, unsigned long count, const MorphDelta* data)
{
    MorphDeltaList* entry = morphData == 0
                               ? 0
                               : morphData + (packetIndex * numMorphs + morphIndex);

    entry->numDeltas = count;
    entry->deltas = data;
}

void ShaderSkinMesh::InitializeSkinData()
{
    packetSkinData = new (8, false)
        PacketSkinData[pModel->GetNumPackets()];

    glModelPacket* pPacket = pModel->packets;
    BoneMapList* node = nlDLRingGetStart(boneMaps);
    rigidSkin = true;

    for (int i = 0; i < (int)pModel->GetNumPackets(); i++, pPacket++)
    {
        CopyMatrices(node);

        glSetMaterialBufferParameter(pPacket, SkinMatricesHash,
            (unsigned long)node->m_pMatrices,
            node->m_nBones * sizeof(float[3][4]));
        PacketSkinData* data = &packetSkinData[i];
        if (data->numBones == 0)
        {
            BuildPacketSkinData(data, pPacket, node);
        }

        node = node->m_next;
    }
}

#ifdef NP_NATIVE
/* rlg vertex arrays are big-endian (GX reads them as they are); the CPU reads them through npa_rdf32 */
#define NPA_SKINW(pw, k) npa_rdf32(&(*(pw))[k])
#else
#define NPA_SKINW(pw, k) ((*(pw))[k])
#endif

void ShaderSkinMesh::BuildPacketSkinData(
    PacketSkinData* data, glModelPacket* pPacket,
    BoneMapList* node)
{
    unsigned long numBones;
    unsigned long numVertices;
    unsigned char* buffer;
    SkinWeight* pairs;

    numVertices = pPacket->numUniqueVertices;
    numBones = node->m_nBones;
    buffer = (unsigned char*)nlMalloc(
        numVertices * 2 * sizeof(SkinWeight), 8, false);
    pairs = (SkinWeight*)buffer;
    data->numVertices = numVertices;
    data->numBones = numBones;
    data->boneWeights = new (8, false)
        BoneSkinWeights[numBones];

    glModelStream* indexStream = glFindModelStream(pPacket, 7);
    glModelStream* weightStream = glFindModelStream(pPacket, 5);
    const unsigned char (*indices)[4] = (const unsigned char (*)[4])indexStream->address;
    const float (*weights)[4] = (const float (*)[4])weightStream->address;

    BoneSkinWeights* entry;
    const unsigned char (*pIndices)[4];
    const float (*pWeights)[4];
    SkinWeight* pPair;
    unsigned long i;
    for (i = 0; i < numBones; i++)
    {
        entry = &data->boneWeights[i];
        pIndices = indices;
        pWeights = weights;
        pPair = pairs;
        int numPairs = 0;
        for (unsigned long j = 0; j < numVertices; j++)
        {
            if (0.0f != NPA_SKINW(pWeights, 1))
            {
                rigidSkin = false;
            }
            for (unsigned long k = 0; k < 4; k++)
            {
                float weight = NPA_SKINW(pWeights, k);
                if (0.0f == weight)
                {
                    break;
                }
                if (i == (*pIndices)[k])
                {
                    pPair->vertexIndex = j;
                    pPair->vertexWeight = weight;
                    pPair++;
                    numPairs++;
                }
            }
            pIndices++;
            pWeights++;
        }
        if (numPairs > 0)
        {
            entry->numWeights = numPairs;
            entry->weights = (SkinWeight*)nlMalloc(
                numPairs * sizeof(SkinWeight), 8, false);
            memcpy(entry->weights, buffer,
                numPairs * sizeof(SkinWeight));
        }
    }
    delete[] buffer;
}

void ShaderSkinMesh::SetHierarchy(const cSHierarchy* hierarchy)
{
    hierarchySignature = hierarchy->GetHashID();
    numBones = hierarchy->GetNumNodes();

    poseMatrices = (nlMatrix4*)nlMalloc(
        numBones * sizeof(nlMatrix4), 8, false);
    for (int i = 0; i < hierarchy->GetNumNodes(); i++)
    {
        poseMatrices[i].SetIdentity();
    }

    boneMatrices = (nlMatrix4*)nlMalloc(
        numBones * sizeof(nlMatrix4), 8, false);
    for (int i = 0; i < hierarchy->GetNumNodes(); i++)
    {
        boneMatrices[i].SetIdentity();
    }
}

void ShaderSkinMesh::SetBoneMatrix(int nodeIndex, const nlMatrix4* matrix)
{
    boneMatrices[nodeIndex] = *matrix;
}

void ShaderSkinMesh::Pose(cPoseAccumulator* pPoseAccumulator)
{
    for (int i = 0; i < pPoseAccumulator->GetNumNodes(); i++)
    {
        nlMultMatrices(poseMatrices[i], boneMatrices[i],
            pPoseAccumulator->GetNodeMatrix(i));
    }

    GLSkinMesh::UpdateMorphWeights(pPoseAccumulator);
}

void ShaderSkinMesh::GetPoseMatrix(nlMatrix4* matrix, int nodeIndex)
{
    *matrix = poseMatrices[nodeIndex];
}

void ShaderSkinMesh::PrepareToRender()
{
    glModelPacket* pPacket = pModel->packets;
    BoneMapList* node = nlDLRingGetStart(boneMaps);
    for (int i = 0; i < (int)pModel->GetNumPackets(); i++, pPacket++)
    {
        CopyMatrices(node);
        glSetMaterialBufferParameter(pPacket, SkinMatricesHash,
            (unsigned long)node->m_pMatrices,
            node->m_nBones * sizeof(float[3][4]));
        node = node->m_next;
    }

    if (numActiveMorphs != 0)
    {
        unsigned long count = 0;
        unsigned long offset = 0;
        for (unsigned long i = 0; i < pModel->GetNumPackets();
             i++, offset += sizeof(PacketSkinData))
        {
            PacketSkinData* data =
                (PacketSkinData*)((unsigned char*)packetSkinData + offset);
            count = count >= data->numVertices ? count : data->numVertices;
        }
        morphBuffer = (nlVector3*)glFrameAlloc(
            count * sizeof(nlVector3), GLM_VertexData);
    }

    if (!rigidSkin && m_Unknown0C == 0)
    {
        SoftwareSkinModel(pModel);
        glModel* newModel = (glModel*)glFrameAlloc(sizeof(glModel), GLM_Header);
        softwareModel = newModel;
        memcpy(newModel, pModel, sizeof(glModel));
        unsigned long numPackets = newModel->GetNumPackets();
        glModelPacket* pPackets = (glModelPacket*)glFrameAlloc(
            numPackets * sizeof(glModelPacket), GLM_Header);
        memcpy(pPackets, newModel->packets, numPackets * sizeof(glModelPacket));
        glSetModelPackets(newModel, pPackets, numPackets);

        for (unsigned long i = 0; i < newModel->GetNumPackets(); i++)
        {
            glModelPacket* pPacket = &newModel->packets[i];
#ifdef NP_NATIVE
            if (NpSkinGpuOf(pPacket) != 0)
            {
                continue;   // W5 SKIN: skinned on the GPU, its streams stay the model's bind pose (big-endian, as on the disc)
            }
#endif
            unsigned long numStreams = pPacket->numStreams;
            glModelStream* pStreams = (glModelStream*)glFrameAlloc(
                numStreams * sizeof(glModelStream), GLM_Header);
            memcpy(pStreams, pPacket->streams, numStreams * sizeof(glModelStream));
            glSetModelStream(pStreams, pStreams->index,
                (void*)pPacket->skinnedVertices, pStreams->stride, pStreams->id);
            /* NP (NPP): the skinned arrays are host order now; the stream keeps NP_GL_STREAM_HOST_ORDER from glSetModelStream */
            if (pPacket->skinnedNormals != 0)
            {
                glModelStream* pStream = pStreams + 1;
                for (unsigned long j = 1; j < pPacket->numStreams; j++)
                {
                    if (pStream->id == 2)
                    {
                        glSetModelStream(pStream, pStream->index,
                            (void*)pPacket->skinnedNormals, sizeof(nlVector3), 2);
                        break;
                    }
                }
            }
            glSetPacketStreams(pPacket, pStreams, numStreams);
        }
    }
    else
    {
        for (unsigned long i = 0; i < pModel->GetNumPackets(); i++)
        {
            pModel->packets[i].skinnedVertices = 0;
        }
    }
}

void ShaderSkinMesh::CreateMorphBuffer(unsigned long packetIndex,
    unsigned long count)
{
    const MorphDeltaList* entry;
    const MorphDelta* pCurrentMorph;
    if (numActiveMorphs != 0)
    {
        nlZeroMemory(morphBuffer, count * sizeof(nlVector3));
        for (unsigned long morphIndex = 0; morphIndex < numMorphs; morphIndex++)
        {
            float w = GetMorphWeight(morphIndex);
            if (0.0f != w)
            {
                entry =
                    morphData == 0 ? 0
                        : morphData
                            + (packetIndex * numMorphs + morphIndex);
                for (unsigned long i = 0; i < entry->numDeltas; i++)
                {
                    pCurrentMorph = &entry->deltas[i];
                    nlVector3* dst = &morphBuffer[pCurrentMorph->index];
                    nlVec3ScaleAdd(*dst, w, pCurrentMorph->delta, *dst);
                }
            }
        }
    }
}

#ifdef NP_NATIVE
// out += weight * ((in [+ morph]) * M [+ translation]) for every (vertex, weight) pair of one bone (row-vector convention, translation
// in row 3, as the original's paired-single asm reads it).
// Byte order (NPP, was: both big-endian): `in` is a model vertex array straight from the disc (stream 1 / 2), BIG-ENDIAN, read through
// byte swaps; `out` (skinnedVertices / skinnedNormals) is written in HOST order and its stream keeps NP_GL_STREAM_HOST_ORDER (set by
// glSetModelStream, see PrepareToRender), so the accumulate needs no swaps; matrix, morph and weight are CPU data in host order.
// NEON (armhf, Vita): about 24 instructions per pair instead of about 50 (scalar, swaps on both sides).
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
// (NPP2: the morph and translate tests are template parameters: the loop has no branch but its own)
template <bool Morph, bool Translate>
static inline void NpSkinRunT(nlVector3* outArray, const nlVector3* inArray, const nlVector3* morphArray, const nlMatrix4& matrix,
    const SkinWeight* weights, unsigned long count)
{
    const float32x4_t r0 = vld1q_f32(matrix.e2[0]);
    const float32x4_t r1 = vld1q_f32(matrix.e2[1]);
    const float32x4_t r2 = vld1q_f32(matrix.e2[2]);
    const float32x4_t r3 = Translate ? vld1q_f32(matrix.e2[3]) : vdupq_n_f32(0.0f);
    for (unsigned long i = 0; i < count; ++i)
    {
        const unsigned long k = weights[i].vertexIndex;
        const unsigned char* pi = (const unsigned char*)(inArray + k);
        float* po = (float*)(outArray + k);
        float32x2_t xy = vreinterpret_f32_u8(vrev32_u8(vld1_u8(pi)));
        float32x2_t zz = vreinterpret_f32_u8(vrev32_u8(vreinterpret_u8_u32(vld1_dup_u32((const uint32_t*)(pi + 8)))));
        if (Morph)
        {
            const float* pm = (const float*)(morphArray + k);
            xy = vadd_f32(xy, vld1_f32(pm));
            zz = vadd_f32(zz, vld1_dup_f32(pm + 2));
        }
        float32x4_t t = vmlaq_lane_f32(r3, r0, xy, 0);
        t = vmlaq_lane_f32(t, r1, xy, 1);
        t = vmlaq_lane_f32(t, r2, zz, 0);
        const float32x4_t o = vcombine_f32(vld1_f32(po), vld1_dup_f32(po + 2));
        const float32x4_t r = vmlaq_n_f32(o, t, weights[i].vertexWeight);
        vst1_f32(po, vget_low_f32(r));
        vst1q_lane_f32(po + 2, r, 2);
    }
}
static void NpSkinRun(nlVector3* outArray, const nlVector3* inArray, const nlVector3* morphArray, const nlMatrix4& matrix,
    const SkinWeight* weights, unsigned long count, bool translate)
{
    if (morphArray != 0)
    {
        if (translate)
            NpSkinRunT<true, true>(outArray, inArray, morphArray, matrix, weights, count);
        else
            NpSkinRunT<true, false>(outArray, inArray, morphArray, matrix, weights, count);
    }
    else
    {
        if (translate)
            NpSkinRunT<false, true>(outArray, inArray, morphArray, matrix, weights, count);
        else
            NpSkinRunT<false, false>(outArray, inArray, morphArray, matrix, weights, count);
    }
}
// Vertices and normals of one bone in ONE pass over its weight list (NP, NPP2): the same loads of (index, weight) and the same address
// arithmetic serve both arrays, about 30 instructions per pair instead of 2 x 24. Every element still gets the same operations in the
// same order as NpSkinRun gives it (the normal's translation row is a zero vector added as before, so even the sign of a zero is the
// same): the outputs are bit-identical, which the renderer's per-frame hash checks (MSC_SKIN_FUSE=0 runs the two passes).
template <bool Morph>
static inline void NpSkinRunBothT(nlVector3* outV, const nlVector3* inV, const nlVector3* morphArray, nlVector3* outN, const nlVector3* inN,
    const nlMatrix4& matrix, const SkinWeight* weights, unsigned long count)
{
    const float32x4_t r0 = vld1q_f32(matrix.e2[0]);
    const float32x4_t r1 = vld1q_f32(matrix.e2[1]);
    const float32x4_t r2 = vld1q_f32(matrix.e2[2]);
    const float32x4_t r3 = vld1q_f32(matrix.e2[3]);
    const float32x4_t zero = vdupq_n_f32(0.0f);
    for (unsigned long i = 0; i < count; ++i)
    {
        const unsigned long k = weights[i].vertexIndex;
        const float w = weights[i].vertexWeight;
        const unsigned char* pi = (const unsigned char*)(inV + k);
        float* po = (float*)(outV + k);
        const unsigned char* pn = (const unsigned char*)(inN + k);
        float* pno = (float*)(outN + k);
        float32x2_t xy = vreinterpret_f32_u8(vrev32_u8(vld1_u8(pi)));
        float32x2_t zz = vreinterpret_f32_u8(vrev32_u8(vreinterpret_u8_u32(vld1_dup_u32((const uint32_t*)(pi + 8)))));
        const float32x2_t nxy = vreinterpret_f32_u8(vrev32_u8(vld1_u8(pn)));
        const float32x2_t nzz = vreinterpret_f32_u8(vrev32_u8(vreinterpret_u8_u32(vld1_dup_u32((const uint32_t*)(pn + 8)))));
        if (Morph)
        {
            const float* pm = (const float*)(morphArray + k);
            xy = vadd_f32(xy, vld1_f32(pm));
            zz = vadd_f32(zz, vld1_dup_f32(pm + 2));
        }
        float32x4_t t = vmlaq_lane_f32(r3, r0, xy, 0);
        t = vmlaq_lane_f32(t, r1, xy, 1);
        t = vmlaq_lane_f32(t, r2, zz, 0);
        float32x4_t tn = vmlaq_lane_f32(zero, r0, nxy, 0);
        tn = vmlaq_lane_f32(tn, r1, nxy, 1);
        tn = vmlaq_lane_f32(tn, r2, nzz, 0);
        const float32x4_t o = vcombine_f32(vld1_f32(po), vld1_dup_f32(po + 2));
        const float32x4_t on = vcombine_f32(vld1_f32(pno), vld1_dup_f32(pno + 2));
        const float32x4_t r = vmlaq_n_f32(o, t, w);
        const float32x4_t rn = vmlaq_n_f32(on, tn, w);
        vst1_f32(po, vget_low_f32(r));
        vst1q_lane_f32(po + 2, r, 2);
        vst1_f32(pno, vget_low_f32(rn));
        vst1q_lane_f32(pno + 2, rn, 2);
    }
}
static void NpSkinRunBoth(nlVector3* outV, const nlVector3* inV, const nlVector3* morphArray, nlVector3* outN, const nlVector3* inN,
    const nlMatrix4& matrix, const SkinWeight* weights, unsigned long count)
{
    if (morphArray != 0)
        NpSkinRunBothT<true>(outV, inV, morphArray, outN, inN, matrix, weights, count);
    else
        NpSkinRunBothT<false>(outV, inV, morphArray, outN, inN, matrix, weights, count);
}
#else
static void NpSkinRun(nlVector3* outArray, const nlVector3* inArray, const nlVector3* morphArray, const nlMatrix4& matrix,
    const SkinWeight* weights, unsigned long count, bool translate)
{
    const float (*m)[4] = matrix.e2;
    for (unsigned long i = 0; i < count; ++i)
    {
        const unsigned long k = weights[i].vertexIndex;
        const nlVector3& in = inArray[k];
        nlVector3& out = outArray[k];
        float x = npa_rdf32(&in.x), y = npa_rdf32(&in.y), z = npa_rdf32(&in.z);
        if (morphArray != 0)
        {
            x += morphArray[k].x;
            y += morphArray[k].y;
            z += morphArray[k].z;
        }
        float tx = m[0][0] * x + m[1][0] * y + m[2][0] * z;
        float ty = m[0][1] * x + m[1][1] * y + m[2][1] * z;
        float tz = m[0][2] * x + m[1][2] * y + m[2][2] * z;
        if (translate)
        {
            tx += m[3][0];
            ty += m[3][1];
            tz += m[3][2];
        }
        const float w = weights[i].vertexWeight;
        out.x += tx * w;
        out.y += ty * w;
        out.z += tz * w;
    }
}
static void NpSkinRunBoth(nlVector3* outV, const nlVector3* inV, const nlVector3* morphArray, nlVector3* outN, const nlVector3* inN,
    const nlMatrix4& matrix, const SkinWeight* weights, unsigned long count)
{
    NpSkinRun(outV, inV, morphArray, matrix, weights, count, true);
    NpSkinRun(outN, inN, 0, matrix, weights, count, false);
}
#endif
static bool NpSkinFuse()
{
    static int fuse = -1;
    if (fuse < 0)
    {
        const char* e = getenv("MSC_SKIN_FUSE");
        fuse = (e != 0 && *e == '0') ? 0 : 1;
    }
    return fuse != 0;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The skinning of cut-scenes and 3D menus on the helper lane (NPP2; MSC_SKIN_LANE=1, np_framesend.h NpLane*). PrepareToRender allocates the
// output arrays and copies what the math needs (matrices, morph weights) into a task, in the order the original allocates and with the same
// values; the zeroing, the morph buffer and the bone loops run on the lane while the game thread goes on with the next character; glSendFrame
// waits for the lane (NpSkinLaneWait) before the send binds the arrays, and then does the DCFlushRange calls the original made per packet
// (they only tell the GX layer's copy tracking that memory was written). The lane runs the same NEON kernels, so the arrays are bit-identical.
namespace
{
const int kSkinMaxMorphs = 48;
const int kSkinMaxTasks = 128;

struct NpSkinTask
{
    unsigned long packetIndex;
    unsigned long numVertices;
    unsigned long numBones;
    unsigned long numMorphs;
    nlVector3* outV;
    nlVector3* outN;                 // 0: no normals
    const nlVector3* inV;
    const nlVector3* inN;
    nlVector3* morph;                // the morph buffer of this PrepareToRender call; 0: no active morph
    const MorphDeltaList* morphData;
    BoneSkinWeights* boneWeights;
    float morphWeight[kSkinMaxMorphs];
    nlMatrix4 matrices[32];
};

NpSkinTask* sSkinTasks;
int sSkinCount;

bool NpSkinLaneOn()
{
    return &NpLaneEnabled != 0 && &NpLaneSubmit != 0 && &NpLaneWait != 0 && NpLaneEnabled() != 0;
}

void NpSkinTaskRun(void* arg)
{
    NpSkinTask& t = *static_cast<NpSkinTask*>(arg);
    const unsigned long size = t.numVertices * sizeof(nlVector3);
    nlZeroMemory(t.outV, size);
    if (t.outN != 0)
    {
        nlZeroMemory(t.outN, size);
    }
    if (t.morph != 0)   // ShaderSkinMesh::CreateMorphBuffer, on the weights of the call
    {
        nlZeroMemory(t.morph, t.numVertices * sizeof(nlVector3));
        for (unsigned long morphIndex = 0; morphIndex < t.numMorphs; morphIndex++)
        {
            const float w = t.morphWeight[morphIndex];
            if (0.0f != w)
            {
                const MorphDeltaList* entry = t.morphData == 0 ? 0 : t.morphData + (t.packetIndex * t.numMorphs + morphIndex);
                for (unsigned long i = 0; i < entry->numDeltas; i++)
                {
                    const MorphDelta* pCurrentMorph = &entry->deltas[i];
                    nlVector3* dst = &t.morph[pCurrentMorph->index];
                    nlVec3ScaleAdd(*dst, w, pCurrentMorph->delta, *dst);
                }
            }
        }
    }
    for (unsigned long matrixOffset = 0; matrixOffset < t.numBones; matrixOffset++)
    {
        BoneSkinWeights* curr = &t.boneWeights[matrixOffset];
        const nlMatrix4* pMatrix = &t.matrices[matrixOffset];
        if (t.outN != 0 && NpSkinFuse())
        {
            NpSkinRunBoth(t.outV, t.inV, t.morph, t.outN, t.inN, *pMatrix, curr->weights, curr->numWeights);
        }
        else
        {
            NpSkinRun(t.outV, t.inV, t.morph, *pMatrix, curr->weights, curr->numWeights, true);
            if (t.outN != 0)
            {
                NpSkinRun(t.outN, t.inN, 0, *pMatrix, curr->weights, curr->numWeights, false);
            }
        }
    }
}
} // namespace

// glSendFrame: everything submitted has finished; then the cache calls of the original, in order
extern "C" void NpSkinLaneWait(void)
{
    if (sSkinCount == 0)
    {
        return;
    }
    NpLaneWait();
    for (int i = 0; i < sSkinCount; ++i)
    {
        const unsigned long size = sSkinTasks[i].numVertices * sizeof(nlVector3);
        DCFlushRange(sSkinTasks[i].outV, size);
        if (sSkinTasks[i].outN != 0)
        {
            DCFlushRange(sSkinTasks[i].outN, size);
        }
    }
    sSkinCount = 0;
}

namespace
{
NpSkinTask* NpSkinTaskAlloc()
{
    if (sSkinTasks == 0)
    {
        sSkinTasks = static_cast<NpSkinTask*>(malloc(sizeof(NpSkinTask) * kSkinMaxTasks));
        if (sSkinTasks == 0)
        {
            return 0;
        }
    }
    if (sSkinCount == kSkinMaxTasks)
    {
        NpSkinLaneWait();   // the pool is used up: drain the lane (the tasks' memory is free again afterwards)
    }
    return &sSkinTasks[sSkinCount++];
}
} // namespace
#endif

#ifdef NP_NATIVE
// ---------------------------------------------------------------------------------------------------------------------------------
// W5 SKIN: GPU skinning (np/skingpu.h) and the per-frame skin counters (MSC_SKIN_STATS=1).
namespace
{
int sSkinGpu = -1, sSkinDiag = -1;
int EnvFlag(const char* name)
{
    const char* e = getenv(name);
    return (e != 0 && e[0] != 0 && e[0] != '0') ? 1 : 0;
}

// MSC_SKIN_STATS=1: one line per frame that skinned anything:
//   [skinstat] f=<frame> mesh=<meshes> pk=<packets> cpk=<CPU-skinned> cv=<their unique vertices> cb=<bytes written> gpk=<GPU-skinned> gv=<their vertices>
//              morph=<meshes with active morphs> mchg=<of those: weights changed since the mesh's last PrepareToRender> mpk=<packets with an active morph delta> mv=<their vertices> big=<packets over 32 bones>
struct SkinFrameStat
{
    int frame;
    unsigned long mesh, pk, cpk, cv, cb, gpk, gv, morph, mchg, mpk, mv, big;
};
SkinFrameStat sStat = {-1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
void StatFlush()
{
    if (sStat.frame >= 0 && sStat.pk != 0)
        fprintf(stderr, "[skinstat] f=%d mesh=%lu pk=%lu cpk=%lu cv=%lu cb=%lu gpk=%lu gv=%lu morph=%lu mchg=%lu mpk=%lu mv=%lu big=%lu\n", sStat.frame, sStat.mesh,
            sStat.pk, sStat.cpk, sStat.cv, sStat.cb, sStat.gpk, sStat.gv, sStat.morph, sStat.mchg, sStat.mpk, sStat.mv, sStat.big);
}
SkinFrameStat& Stat()
{
    const int f = (int)np_gx_frame_count();   // (the renderer's presented-frame number: the census lines of the same frame carry it)
    if (f != sStat.frame)
    {
        StatFlush();
        SkinFrameStat z = {f, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        sStat = z;
    }
    return sStat;
}
// the morph weights each mesh was last skinned with (a hash), to count the changes
struct MorphSeen
{
    const void* mesh;
    unsigned long hash;
};
MorphSeen sMorphSeen[256];
bool MorphChanged(const void* mesh, unsigned long hash)
{
    unsigned long h = (unsigned long)mesh;
    unsigned int i = (unsigned int)((h >> 4) ^ (h >> 12)) & 255u;
    for (int n = 0; n < 256; ++n, i = (i + 1) & 255u)
    {
        if (sMorphSeen[i].mesh == mesh)
        {
            const bool changed = sMorphSeen[i].hash != hash;
            sMorphSeen[i].hash = hash;
            return changed;
        }
        if (sMorphSeen[i].mesh == 0)
        {
            sMorphSeen[i].mesh = mesh;
            sMorphSeen[i].hash = hash;
            return true;
        }
    }
    return true;
}
} // namespace

extern "C" int NpSkinGpuOn(void)
{
    if (sSkinGpu < 0)
        sSkinGpu = EnvFlag("MSC_SKIN_GPU");
    return sSkinGpu;
}
extern "C" int NpSkinDiagOn(void)
{
    if (sSkinDiag < 0)
        sSkinDiag = EnvFlag("MSC_SKIN_STATS");
    return sSkinDiag;
}

// Does an active morph target move vertices of this packet this frame? (CreateMorphBuffer adds the deltas of every morph whose weight
// is not 0; a packet without such deltas gets a zero buffer, which changes no position.)
bool ShaderSkinMesh::PacketHasMorph(unsigned long packetIndex) const
{
    if (numActiveMorphs == 0 || morphData == 0)
        return false;
    for (unsigned long m = 0; m < numMorphs; ++m)
    {
        if (0.0f != GetMorphWeight((int)m) && morphData[packetIndex * numMorphs + m].numDeltas != 0)
            return true;
    }
    return false;
}

// The packet is not skinned here: its streams stay the bind pose, and the record the frame send hands to the renderer is made. The matrices are the
// ones SoftwareSkinModel would read now (the SkinMatrices parameter that PrepareToRender has just filled), copied: the mesh may be prepared again
// before the frame is sent, and the next build rewrites them while the two-stage pipeline sends this frame.
bool ShaderSkinMesh::GpuSkinPacket(unsigned long packetIndex, glModelPacket* pPacket, PacketSkinData* data, BoneMapList* node)
{
    const unsigned long nb = data->numBones;
    if (nb == 0 || nb > 32 || node->m_nBones != nb || PacketHasMorph(packetIndex))
        return false;
    glModelStream* posStream = glFindModelStream(pPacket, 1);
    glModelStream* weightStream = glFindModelStream(pPacket, 5);
    glModelStream* indexStream = glFindModelStream(pPacket, 7);
    if (posStream == 0 || weightStream == 0 || indexStream == 0 || pPacket->numStreams == 0 || pPacket->streams[0].id != 1)
        return false;
    const float (*pMatrices)[3][4] = *(const float (**)[3][4])glGetMaterialParameterData(pPacket, SkinMatricesHash);
    if (pMatrices == 0)
        return false;
    NpSkinGpuRec* r = (NpSkinGpuRec*)glFrameAlloc(sizeof(NpSkinGpuRec) + (nb - 1) * sizeof(float[12]), GLM_Header);
    if (r == 0)
        return false;
    r->magic = NP_SKIN_GPU_MAGIC;
    r->self = r;
    r->nBones = nb;
    r->nVerts = data->numVertices;
    r->weights = weightStream->address;
    r->indices = indexStream->address;
    memcpy(r->mats, pMatrices, nb * sizeof(float[12]));
    pPacket->skinnedVertices = (unsigned long)r;
    glModelStream* nrmStream = glFindModelStream(pPacket, 2);
    pPacket->skinnedNormals = nrmStream != 0 ? (unsigned long)nrmStream->address : 0;
    return true;
}
#endif

void ShaderSkinMesh::SoftwareSkinModel(glModel* model)
{
    nlMatrix4 tempMatrices[32];
    unsigned long size;
    BoneMapList* node = nlDLRingGetStart(boneMaps);
    glModelPacket* pPacket = model->packets;
#ifdef NP_NATIVE
    const bool laneOn = NpSkinLaneOn() && numMorphs <= (unsigned long)kSkinMaxMorphs;
    const bool gpuOn = NpSkinGpuOn() != 0;
    const bool diag = NpSkinDiagOn() != 0;
    if (diag)
    {
        SkinFrameStat& st = Stat();
        ++st.mesh;
        if (numActiveMorphs != 0)
        {
            ++st.morph;
            unsigned long h = 2166136261u;
            for (unsigned long m = 0; m < numMorphs; ++m)
            {
                float w = GetMorphWeight((int)m);
                unsigned long bits;
                memcpy(&bits, &w, 4);
                h = (h ^ bits) * 16777619u;
            }
            if (MorphChanged(this, h))
                ++st.mchg;
        }
    }
#endif

    for (unsigned long packetIndex = 0; packetIndex < model->GetNumPackets();
         packetIndex++, pPacket++)
    {
        PacketSkinData* data = &packetSkinData[packetIndex];
        unsigned long numVertices = data->numVertices;
        size = numVertices * sizeof(nlVector3);
#ifdef NP_NATIVE
        if (diag)
        {
            SkinFrameStat& st = Stat();
            ++st.pk;
            if (data->numBones > 32)
                ++st.big;
            if (PacketHasMorph(packetIndex))
            {
                ++st.mpk;
                st.mv += numVertices;
            }
        }
        if (gpuOn && GpuSkinPacket(packetIndex, pPacket, data, node))
        {
            if (diag)
            {
                ++Stat().gpk;
                Stat().gv += numVertices;
            }
            node = node->m_next;
            continue;
        }
        if (diag)
        {
            SkinFrameStat& st = Stat();
            ++st.cpk;
            st.cv += numVertices;
            st.cb += numVertices * sizeof(nlVector3) * (glFindModelStream(pPacket, 2) != 0 ? 2 : 1);
        }
        NpSkinTask* laneTask = (laneOn && data->numBones <= 32) ? NpSkinTaskAlloc() : 0;   // 0: this packet is skinned here
#endif
        nlVector3* outVertices = (nlVector3*)glFrameAlloc(size, GLM_VertexData);
#ifdef NP_NATIVE
        if (laneTask == 0)
#endif
        nlZeroMemory(outVertices, size);
        pPacket->skinnedVertices = (unsigned long)outVertices;

        nlVector3* outNormals = 0;
        glModelStream* pStream = glFindModelStream(pPacket, 2);
        const nlVector3* inNormals = 0;
        if (pStream != 0)
        {
            outNormals = (nlVector3*)glFrameAlloc(size, GLM_VertexData);
#ifdef NP_NATIVE
            if (laneTask == 0)
#endif
            nlZeroMemory(outNormals, size);
            inNormals = (const nlVector3*)pStream->address;
            pPacket->skinnedNormals = (unsigned long)outNormals;
        }

#ifdef NP_NATIVE
        if (laneTask == 0)
#endif
        CreateMorphBuffer(packetIndex, numVertices);
        const nlVector3* inVertices = (const nlVector3*)glFindModelStream(pPacket, 1)->address;
        const float (*pMatrices)[3][4] =
            *(const float (**)[3][4])glGetMaterialParameterData(pPacket, SkinMatricesHash);
#ifdef NP_NATIVE
        if (laneTask != 0)
        {
            nlMatrix4* pLaneMatrix = laneTask->matrices;
            for (unsigned long i = 0; i < node->m_nBones; i++)
            {
                glxCopyMatrix(*pLaneMatrix, *pMatrices);
                pMatrices++;
                pLaneMatrix++;
            }
            laneTask->packetIndex = packetIndex;
            laneTask->numVertices = numVertices;
            laneTask->numBones = data->numBones;
            laneTask->numMorphs = numMorphs;
            laneTask->outV = outVertices;
            laneTask->outN = outNormals;
            laneTask->inV = inVertices;
            laneTask->inN = inNormals;
            laneTask->morph = numActiveMorphs != 0 ? morphBuffer : 0;
            laneTask->morphData = morphData;
            laneTask->boneWeights = data->boneWeights;
            if (laneTask->morph != 0)
            {
                for (unsigned long i = 0; i < numMorphs; i++)
                {
                    laneTask->morphWeight[i] = GetMorphWeight(i);
                }
            }
            NpLaneSubmit(NpSkinTaskRun, laneTask);
            node = node->m_next;
            continue;
        }
#endif
        nlMatrix4* pTempMatrix = tempMatrices;
        for (unsigned long i = 0; i < node->m_nBones; i++)
        {
            glxCopyMatrix(*pTempMatrix, *pMatrices);
            pMatrices++;
            pTempMatrix++;
        }

        for (unsigned long matrixOffset = 0; matrixOffset < data->numBones; matrixOffset++)
        {
            BoneSkinWeights* curr = &data->boneWeights[matrixOffset];
            register const nlMatrix4* pMatrix = &tempMatrices[matrixOffset];
#ifndef NP_NATIVE
            // clang-format off
            asm {
                psq_l f2, 0x0(pMatrix), 0, 0
                psq_l f3, 0x8(pMatrix), 0, 0
                psq_l f4, 0x10(pMatrix), 0, 0
                psq_l f5, 0x18(pMatrix), 0, 0
                psq_l f6, 0x20(pMatrix), 0, 0
                psq_l f7, 0x28(pMatrix), 0, 0
                psq_l f8, 0x30(pMatrix), 0, 0
                psq_l f9, 0x38(pMatrix), 0, 0
            }
            // clang-format on
#endif // NP_NATIVE: the matrix is read by the helpers below

#ifdef NP_NATIVE
            if (outNormals != 0 && NpSkinFuse())
            {
                NpSkinRunBoth(outVertices, inVertices, numActiveMorphs != 0 ? morphBuffer : 0, outNormals, inNormals, *pMatrix, curr->weights,
                    curr->numWeights);
            }
            else
            {
                NpSkinRun(outVertices, inVertices, numActiveMorphs != 0 ? morphBuffer : 0, *pMatrix, curr->weights, curr->numWeights, true);
                if (outNormals != 0)
                    NpSkinRun(outNormals, inNormals, 0, *pMatrix, curr->weights, curr->numWeights, false);
            }
#else
            if (numActiveMorphs != 0)
            {
                for (unsigned long i = 0; i < curr->numWeights; i++)
                {
                    const SkinWeight& pair = curr->weights[i];
                    float vertexWeight = pair.vertexWeight;
                    unsigned long index = pair.vertexIndex;
                    register const nlVector3& inVertex = inVertices[index];
                    register const nlVector3& morph = morphBuffer[index];
                    register nlVector3& outVertex = outVertices[index];
#ifdef NP_NATIVE
                    NpSkinAccumulate(outVertex, inVertex, &morph, *pMatrix, vertexWeight, true);
#else
                    // clang-format off
                    asm {
                        lfs f12, vertexWeight
                        psq_l f0, 0x0(inVertex), 0, 0
                        psq_l f10, 0x0(morph), 0, 0
                        psq_l f1, 0x8(inVertex), 1, 0
                        ps_add f0, f0, f10
                        psq_l f11, 0x8(morph), 1, 0
                        ps_add f1, f1, f11
                        ps_muls0 f10, f2, f0
                        ps_muls0 f11, f3, f0
                        ps_madds1 f10, f4, f0, f10
                        ps_madds1 f11, f5, f0, f11
                        psq_l f0, 0x0(outVertex), 0, 0
                        ps_madds0 f10, f6, f1, f10
                        ps_madds0 f11, f7, f1, f11
                        psq_l f1, 0x8(outVertex), 1, 0
                        ps_add f10, f8, f10
                        ps_add f11, f9, f11
                        ps_madds0 f10, f10, f12, f0
                        ps_madds0 f11, f11, f12, f1
                        psq_st f10, 0x0(outVertex), 0, 0
                        psq_st f11, 0x8(outVertex), 1, 0
                    }
                    // clang-format on
#endif
                }
            }
            else
            {
                for (unsigned long i = 0; i < curr->numWeights; i++)
                {
                    const SkinWeight& pair = curr->weights[i];
                    float vertexWeight = pair.vertexWeight;
                    unsigned long index = pair.vertexIndex;
                    register const nlVector3& inVertex = inVertices[index];
                    register nlVector3& outVertex = outVertices[index];
#ifdef NP_NATIVE
                    NpSkinAccumulate(outVertex, inVertex, 0, *pMatrix, vertexWeight, true);
#else
                    // clang-format off
                    asm {
                        lfs f12, vertexWeight
                        psq_l f0, 0x0(inVertex), 0, 0
                        psq_l f1, 0x8(inVertex), 1, 0
                        ps_muls0 f10, f2, f0
                        ps_muls0 f11, f3, f0
                        ps_madds1 f10, f4, f0, f10
                        ps_madds1 f11, f5, f0, f11
                        psq_l f0, 0x0(outVertex), 0, 0
                        ps_madds0 f10, f6, f1, f10
                        ps_madds0 f11, f7, f1, f11
                        psq_l f1, 0x8(outVertex), 1, 0
                        ps_add f10, f8, f10
                        ps_add f11, f9, f11
                        ps_madds0 f10, f10, f12, f0
                        ps_madds0 f11, f11, f12, f1
                        psq_st f10, 0x0(outVertex), 0, 0
                        psq_st f11, 0x8(outVertex), 1, 0
                    }
                    // clang-format on
#endif
                }
            }

            if (outNormals != 0)
            {
                for (unsigned long i = 0; i < curr->numWeights; i++)
                {
                    const SkinWeight& pair = curr->weights[i];
                    float vertexWeight = pair.vertexWeight;
                    unsigned long index = pair.vertexIndex;
                    register const nlVector3& inNormal = inNormals[index];
                    register nlVector3& outNormal = outNormals[index];
#ifdef NP_NATIVE
                    NpSkinAccumulate(outNormal, inNormal, 0, *pMatrix, vertexWeight, false);
#else
                    // clang-format off
                    asm {
                        lfs f12, vertexWeight
                        psq_l f0, 0x0(inNormal), 0, 0
                        psq_l f1, 0x8(inNormal), 1, 0
                        ps_muls0 f10, f2, f0
                        ps_muls0 f11, f3, f0
                        ps_madds1 f10, f4, f0, f10
                        ps_madds1 f11, f5, f0, f11
                        psq_l f0, 0x0(outNormal), 0, 0
                        ps_madds0 f10, f6, f1, f10
                        ps_madds0 f11, f7, f1, f11
                        psq_l f1, 0x8(outNormal), 1, 0
                        ps_madds0 f10, f10, f12, f0
                        ps_madds0 f11, f11, f12, f1
                        psq_st f10, 0x0(outNormal), 0, 0
                        psq_st f11, 0x8(outNormal), 1, 0
                    }
                    // clang-format on
#endif
                }
            }
#endif // NP_NATIVE
        }

        DCFlushRange(outVertices, size);
        if (outNormals != 0)
        {
            DCFlushRange(outNormals, size);
        }
        node = node->m_next;
    }
}
