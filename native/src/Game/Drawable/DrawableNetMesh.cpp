#ifdef NP_NATIVE
#include "np/framesend.h"
#endif
#include "Game/Drawable/DrawableNetMesh.h"
#include "Game/GL/MeshWriter.h"
#include "np/gamefixes.h"
#include "NL/glx/GXConstantColourMaterialProgram.h"
#include "Game/Replay.h"
#include "Game/Field.h"
#include "Game/Net.h"
#include "Game/Render/NetMesh.h"
#include "Game/Render/RLView.h"
#include "Game/Render/ShootToScoreArrow.h"
#include "Game/Task/GameRenderTask.h"
#include "NL/gl/glDraw3.h"
#include "NL/gl/glMatrix.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glState.h"
#include "NL/gl/glView.h"
#include "NL/nlDebug.h"
#include "NL/nlMemory.h"
#include "NL/nlMath.h"

#include <string.h>
#include "Game/SharedStaticStorage.h"
#ifdef NP_NATIVE
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
extern unsigned gNpNetStaticEpoch;   // MSC_SIM26_NETGRAB (Render/NetMesh.cpp): also bumped when this file (re)makes or frees the shared buffers
extern unsigned gNpNetDirtyGen[2];   // int28: bumped by NetMesh::Reset() / active updates so NP_FIX_NET_REDRAW only rebuilds on real state changes
static unsigned sNetBuiltGen[2] = { 0, 0 };
static bool sNetBuiltAtRest[2] = { false, false };
static float sNetBuiltDarkness[2] = { -1.0f, -1.0f };
static u32 sNetBuiltTex[2] = { 0, 0 };
static const DrawableNetMesh* sNetBlendDst[2] = { 0, 0 };
static unsigned sNetBlendGen[2] = { 0, 0 };
static bool sNetBlendAtRest[2] = { false, false };
// MSC_NETCENSUS=1 (diagnostic, changes nothing): every 600 Render calls (the two nets, so 300 frames) a line with the mean vertices rebuilt per call, the share of
// calls that rebuilt the mesh and the mean microseconds of a rebuild (the write loop and End(); the host's clock, a console's cost is a few times that).
namespace {
struct NetCensus { unsigned calls, rebuilds; unsigned long long verts, ns; };
NetCensus gNetCensus;
bool NetCensusOn()
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("MSC_NETCENSUS"); on = e && atoi(e) ? 1 : 0; }
    return on != 0;
}
unsigned long long NetCensusNow()
{
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000000000ull + (unsigned long long)t.tv_nsec;
}
void NetCensusNote(unsigned verts, unsigned long long ns)
{
    NetCensus& c = gNetCensus;
    ++c.rebuilds;
    c.verts += verts;
    c.ns += ns;
}
void NetCensusCall()
{
    NetCensus& c = gNetCensus;
    if (++c.calls < 600) return;
    fprintf(stderr, "[netcensus] calls=%u rebuilds=%u verts/rebuild=%.0f us/rebuild=%.1f\n", c.calls, c.rebuilds, c.rebuilds ? (double)c.verts / c.rebuilds : 0.0,
            c.rebuilds ? (double)c.ns / 1000.0 / c.rebuilds : 0.0);
    c = NetCensus();
}
}
#endif

DECL_WEAK char LightTextureName[] = "global/lightramp";
DECL_WEAK char BlackTextureName[] = "global/black";
DECL_WEAK char WhiteTextureName[] = "global/white";
DECL_WEAK char NetMeshTextureName[] = "global/netmesh";
DECL_WEAK char CheckerTextureName[] = "global/checkers";

static u32 LightTexture = glGetTexture(LightTextureName);
static u32 BlackTexture = glGetTexture(BlackTextureName);
static u32 WhiteTexture = glGetTexture(WhiteTextureName);

GLResourcePool* gNetMeshResourcePools[2][2];
GLResourceMarker* gNetMeshResourceMarkers[2][2];

bool sbResourcePoolSelected[2] = { true, true };
u8 sbRenderAnimatedNetMesh = 1;
char gNetMeshResourcePoolName[8] = "NetMesh";

GLView* g_pNetMeshView;
u32 g_NetMeshInvisiblePlaneView;
shortVector2* spTexcoord[2];
u32* spColour[2];
u16* spTriIndices[2];
bool sbStaticInitialized[2];
int sNumVertices[2];
glModel* spNetModel[2];
bool sbNetModelValid[2];
int siResourcePoolIndex[2];
u8 sbUseCheckerTexture;
int siInvisiblePlaneAlpha;

static u32 NetMeshTexture = glGetTexture(NetMeshTextureName);
static u32 CheckerTexture = glGetTexture(CheckerTextureName);

static inline void MarkMeshUploaded(
    glModel* model, bool uploaded, const DrawableNetMesh* mesh)
{
    ((glModel* volatile*)spNetModel)
        [((const volatile DrawableNetMesh*)mesh)->mNetIndex] = model;
    sbNetModelValid[((const volatile DrawableNetMesh*)mesh)->mNetIndex] = uploaded;
}

DrawableNetMesh::DrawableNetMesh(bool isPositiveXNet)
    : mNetIndex(isPositiveXNet ? 0 : 1)
    , mNetMesh(0)
    , mInitialized(false)
    , mVisible(false)
{
    g_pNetMeshView = GetLayerView(eCLV_UnsortedPerspective);
    g_NetMeshInvisiblePlaneView = (u32)GetLayerView(eCLV_InvisiblePlane);
}

DrawableNetMesh::~DrawableNetMesh()
{
    Destroy();
}

void DrawableNetMesh::RenderInvisiblePlanes() const
{
    float goalLineX = cField::GetGoalLineX(1U);
    float netHeight = cNet::m_fNetHeight;
    float netWidth = cNet::m_fNetWidth;

    glSetDefaultState(true);
    glSetRasterState((eGLState)1, 1);
    glSetRasterState((eGLState)5, 1);
    glSetRasterState((eGLState)6, 0);
    glSetCurrentRasterState(glHandleizeRasterState());
    glSetCurrentTexture(WhiteTexture, (eGLTextureType)0);
    glSetTextureState((eGLTextureState)0, 0);
    glSetCurrentTextureState(glHandleizeTextureState());

    nlMatrix4 matrix;
    nlMakeRotationMatrixY(matrix, 1.5707964f);

    nlColour colour = { 0xFF, 0xFF, 0x00, 0x00 };
    colour.c[3] = (u8)siInvisiblePlaneAlpha;
    glQuad3 quad;

    matrix.e2[3][0] = goalLineX - 0.05f;
    matrix.e2[3][1] = 0.0f;
    matrix.e2[3][2] = 0.5f * netHeight;
    matrix.e2[3][3] = 1.0f;
    quad.SetupRotatedRectangle(netHeight, netWidth, matrix, false, false);
    quad.SetColour(colour);
    glAttachQuad3((eGLView)(u32)g_NetMeshInvisiblePlaneView, 1, &quad);

    matrix.e2[3][0] = goalLineX + 0.05f;
    matrix.e2[3][1] = 0.0f;
    matrix.e2[3][2] = 0.5f * netHeight;
    matrix.e2[3][3] = 1.0f;
    quad.SetupRotatedRectangle(netHeight, netWidth, matrix, false, false);
    quad.SetColour(colour);
    glAttachQuad3((eGLView)(u32)g_NetMeshInvisiblePlaneView, 1, &quad);

    matrix.e2[3][0] = -goalLineX - 0.05f;
    matrix.e2[3][1] = 0.0f;
    matrix.e2[3][2] = 0.5f * netHeight;
    matrix.e2[3][3] = 1.0f;
    quad.SetupRotatedRectangle(netHeight, netWidth, matrix, false, false);
    quad.SetColour(colour);
    glAttachQuad3((eGLView)(u32)g_NetMeshInvisiblePlaneView, 1, &quad);

    matrix.e2[3][0] = 0.05f + -goalLineX;
    matrix.e2[3][1] = 0.0f;
    matrix.e2[3][2] = 0.5f * netHeight;
    matrix.e2[3][3] = 1.0f;
    quad.SetupRotatedRectangle(netHeight, netWidth, matrix, false, false);
    quad.SetColour(colour);
    glAttachQuad3((eGLView)(u32)g_NetMeshInvisiblePlaneView, 1, &quad);
}

void DrawableNetMesh::Render() const
{
    if (!sbRenderAnimatedNetMesh || !mInitialized || !NetMesh::s_bAnimatedNetMeshEnabled)
    {
        return;
    }

    if (!g_bRenderWorldEffects)
    {
        return;
    }

#ifdef NP_NATIVE
    const bool netCensus = NetCensusOn();
    if (netCensus) NetCensusCall();
    unsigned long long netT0 = 0;
#endif
    MeshWriter writer;
    nlVector3* sourcePositions = mPositions;
    shortVector2* sourceTexcoords = spTexcoord[mNetIndex];

    glSetDefaultState(true);
    glSetRasterState((eGLState)6, 0);
    glSetRasterState((eGLState)5, 1);
    glSetRasterState((eGLState)3, 1);
    glSetRasterState((eGLState)0, 1);
    glSetTextureState((eGLTextureState)0, 0);
    glSetRasterState((eGLState)1, 1);
    glSetCurrentRasterState(glHandleizeRasterState());
    glSetCurrentMatrix(glGetIdentityMatrix());

    u32 texture = NetMesh::sNetTextureHandle;
    if (sbUseCheckerTexture)
    {
        texture = CheckerTexture;
    }
    glSetCurrentTexture(texture, (eGLTextureType)0);

    u16* indices = spTriIndices[mNetIndex];
    // MSP game fix "net" (np/gamefixes.h, issue #19) + int28 exact dirty-tracking (Carbon C3 fix):
    // rebuild when invalid, active, interpolated across snapshots (mSnapGen==0), or any input changed (Reset/Update/WorldDarkening/texture).
    const float curDarkness = 1.0f - WorldDarkening::Instance().mPos;
    const bool atRest = !mVisible && mSnapGen != 0u;
    const bool fixDirty = NpGameFix(NP_FIX_NET_REDRAW)
        && ((unsigned)mNetIndex < 2u)
        && (!atRest
            || sNetBuiltGen[mNetIndex] != mSnapGen
            || !sNetBuiltAtRest[mNetIndex]
            || sNetBuiltDarkness[mNetIndex] != curDarkness
            || sNetBuiltTex[mNetIndex] != texture);
    if ((!sbNetModelValid[mNetIndex] || mVisible == true || fixDirty)
        && writer.Begin(mNumTriIndices, 1, gNetMeshResourcePools[mNetIndex][siResourcePoolIndex[mNetIndex]]))
    {
        if (!sbResourcePoolSelected[mNetIndex])
        {
            volatile int& bufferIndex = siResourcePoolIndex[mNetIndex];
            bufferIndex = (bufferIndex + 1) % 2;
            const volatile DrawableNetMesh* volatileThis = this;
            {
                int index = volatileThis->mNetIndex;
                gNetMeshResourcePools[index][siResourcePoolIndex[index]]->ReleaseResource(
                    (unsigned long)gNetMeshResourceMarkers[index][siResourcePoolIndex[index]]);
            }
            ((volatile u8*)sbResourcePoolSelected)[volatileThis->mNetIndex] = true;
            {
                int index = volatileThis->mNetIndex;
                GLResourceMarker* handle = (GLResourceMarker*)gNetMeshResourcePools[index][siResourcePoolIndex[index]]->MarkResource();
                ((GLResourceMarker* volatile*)gNetMeshResourceMarkers[(unsigned int)index])
                    [siResourcePoolIndex[(unsigned int)index]] = handle;
            }
            {
                int index = volatileThis->mNetIndex;
                if (gNetMeshResourceMarkers[index][siResourcePoolIndex[index]]->mUsedMemory[0]
                    || gNetMeshResourceMarkers[index][siResourcePoolIndex[index]]->mUsedMemory[1])
                {
                    nlBreak();
                }
            }
        }
        else
        {
            gNetMeshResourcePools[mNetIndex][siResourcePoolIndex[mNetIndex]]->ReleaseResource(
                (unsigned long)gNetMeshResourceMarkers[mNetIndex][siResourcePoolIndex[mNetIndex]]);
            const volatile DrawableNetMesh* volatileThis = this;
            int index = volatileThis->mNetIndex;
            GLResourceMarker* handle = (GLResourceMarker*)gNetMeshResourcePools[index][siResourcePoolIndex[index]]->MarkResource();
            gNetMeshResourceMarkers[(unsigned int)index][siResourcePoolIndex[(unsigned int)index]] = handle;
        }

#ifdef NP_NATIVE
        if (netCensus) netT0 = NetCensusNow();
#endif
        float darkness = 1.0f - WorldDarkening::Instance().mPos;
        float colour[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        colour[0] = darkness;
        colour[1] = darkness;
        colour[2] = darkness;

        int i;
        for (i = 0; i < mNumTriIndices; indices++, i++)
        {
            u16 index = *indices;
            writer.Texcoord(
                0.0009765625f * sourceTexcoords[index].x,
                0.0009765625f * sourceTexcoords[index].y);
            writer.Vertex(sourcePositions[index]);
        }

        memcpy(&((GXConstantColourParameters*)writer.model->packets->materialParameters)->constantColour,
            colour, sizeof(colour));
        u32 texture = glGetCurrentTexture((eGLTextureType)0);
        glTextureBinding* binding
            = &((GXConstantColourParameters*)writer.model->packets->materialParameters)->diffuseTexture;
        binding->texture = texture;
        binding->textureIndex = 0xFFFF;
        binding->SetWrapS(0);
        binding->SetWrapT(0);
        binding->unknown07 = 0;

        if (!writer.End())
        {
            return;
        }

        MarkMeshUploaded(writer.GetModel(), true, this);
#ifdef NP_NATIVE
        if ((unsigned)mNetIndex < 2u)
        {
            sNetBuiltGen[mNetIndex] = mSnapGen;
            sNetBuiltAtRest[mNetIndex] = atRest;
            sNetBuiltDarkness[mNetIndex] = darkness;
            sNetBuiltTex[mNetIndex] = texture;
        }
        if (netCensus) NetCensusNote((unsigned)mNumTriIndices, NetCensusNow() - netT0);
        // NP (two-stage pipeline): the net is rebuilt in the same pool every frame; the send of the last frame still reads that pool. The
        // second pool is there (the Wii's GPU lagged the CPU as well): take it for the next rebuild.
        if (NpFsPipe())
            sbResourcePoolSelected[mNetIndex] = false;
#endif
    }

    if (sbNetModelValid[mNetIndex] && spNetModel[mNetIndex])
    {
        g_pNetMeshView->AttachModel(spNetModel[mNetIndex], false);
    }

    RenderInvisiblePlanes();
}

void DrawableNetMesh::Reset()
{
    sbResourcePoolSelected[0] = false;
    sbResourcePoolSelected[1] = false;
}

void DrawableNetMesh::Initialize(int numVertices, int numTriIndices)
{
    mPositions = (nlVector3*)nlMalloc(numVertices * sizeof(nlVector3), 8, false);

    if (!sbStaticInitialized[mNetIndex])
    {
        gNpNetStaticEpoch++;
        spTriIndices[mNetIndex] = (u16*)nlMalloc(numTriIndices * sizeof(u16), 8, false);

        int allocationSize = numVertices * 4;
        spTexcoord[mNetIndex] = (shortVector2*)nlMalloc(allocationSize, 8, false);
        spColour[mNetIndex] = (u32*)nlMalloc(allocationSize, 8, false);
        memset(spColour[mNetIndex], 0xFF, allocationSize);

        sbStaticInitialized[mNetIndex] = true;
        sNumVertices[mNetIndex] = numVertices;

        GLMemoryRequirement requirements[2] = {
            { GLM_Header, 0x100 },
            { GLM_VertexData, 0x2EF8 },
        };
        siResourcePoolIndex[mNetIndex] = 0;
        sbResourcePoolSelected[mNetIndex] = true;
        sbNetModelValid[mNetIndex] = false;

        for (int i = 0; i < 2; ++i)
        {
            gNetMeshResourcePools[mNetIndex][i] =
                glCreateResourcePool(requirements, 2, gNetMeshResourcePoolName);
            gNetMeshResourceMarkers[mNetIndex][i] = (GLResourceMarker*)gNetMeshResourcePools[mNetIndex][i]->MarkResource();
        }
    }
}

void DrawableNetMesh::Destroy()
{
    if (mInitialized)
    {
        operator delete[](mPositions);
    }

    if (sbStaticInitialized[mNetIndex])
    {
        gNpNetStaticEpoch++;
        operator delete[](spTexcoord[mNetIndex]);
        operator delete[](spTriIndices[mNetIndex]);
        operator delete[](spColour[mNetIndex]);
        sbStaticInitialized[mNetIndex] = false;

        for (int i = 0; i < 2; ++i)
        {
            glDestroyResourcePool(
                gNetMeshResourcePools[mNetIndex][i]);
            gNetMeshResourcePools[mNetIndex][i] = 0;
        }

        sbNetModelValid[mNetIndex] = false;
        sbResourcePoolSelected[mNetIndex] = false;
    }

#ifdef NP_NATIVE
    mSnapGen = 0u;
    if ((unsigned)mNetIndex < 2u)
    {
        sNetBlendDst[mNetIndex] = 0;
        sNetBlendGen[mNetIndex] = 0;
        sNetBlendAtRest[mNetIndex] = false;
        sNetBuiltGen[mNetIndex] = 0;
        sNetBuiltAtRest[mNetIndex] = false;
        sNetBuiltDarkness[mNetIndex] = -1.0f;
        sNetBuiltTex[mNetIndex] = 0;
    }
#endif

    mInitialized = false;
}

namespace
{
struct NpNetStaticKey
{
    unsigned epoch;
    const NetMesh* mesh;
    const u16* srcIdx;
    const shortVector2* srcTex;
    int numIdx;
    int numTex;
    u16* dstIdx;
    shortVector2* dstTex;
};
NpNetStaticKey sNpNetKey[2];
bool sNpNetKeyValid[2];
unsigned long long sNpNetCopied, sNpNetSkipped, sNpNetChecked, sNpNetWrong;
bool NpNetKeyEqual(const NpNetStaticKey& a, const NpNetStaticKey& b)
{
    return a.epoch == b.epoch && a.mesh == b.mesh && a.srcIdx == b.srcIdx && a.srcTex == b.srcTex && a.numIdx == b.numIdx && a.numTex == b.numTex && a.dstIdx == b.dstIdx && a.dstTex == b.dstTex;
}
int NpSim26NetGrabBreak()
{
    static int brk = -1;
    if (brk < 0)
    {
        const char* e = getenv("MSC_SIM26_NETGRAB_BREAK");
        brk = (e && e[0] == '1') ? 1 : 0;
    }
    return brk;
}
int NpSim26NetGrabMode()
{
    static int mode = -1;   // first read on the game thread (RenderSnapshot::Grab runs the nets there)
    if (mode < 0)
    {
        const char* e = getenv("MSC_SIM26_NETGRAB");
        mode = (e && (e[0] == '1' || e[0] == '2')) ? e[0] - '0' : 0;
    }
    return mode;
}
} // namespace

void DrawableNetMesh::Grab(NetMesh& netMesh)
{
    mNetMesh = &netMesh;
    mVisible = false;

    if (!netMesh.mbInitialized)
    {
        return;
    }

    if (!mInitialized)
    {
        int numTriIndices = netMesh.m_NumTriStripIndices;
        int numVertices = netMesh.m_NumParticles;
        mNumVertices = numVertices;
        mNumTriIndices = numTriIndices;
        Initialize(numVertices, numTriIndices);
        mInitialized = true;
        mJoltCache = 0.0f;
    }

    shortVector2* texcoords = spTexcoord[mNetIndex];
    u16* triIndices = spTriIndices[mNetIndex];
    // MSC_SIM26_NETGRAB=1 (sim26): the tri-strip indices and texture coordinates go to buffers shared by every snapshot of this net, and their source is
    // written only while a net loads (gNpNetStaticEpoch counts those writes); the copy is skipped when the same source was already copied into the same
    // buffers since the last such write (same contents: same bits in the buffers). The positions are still copied every step. =2 verify: the buffers are
    // compared with the source first ([sim26net] counts, wrong must be 0), then copied as before.
    const int netMode = NpSim26NetGrabMode();
    NpNetStaticKey key = { gNpNetStaticEpoch, &netMesh, netMesh.m_TriStripIndices, netMesh.m_v2TextureCoords, netMesh.m_NumTriStripIndices, netMesh.m_NumParticles, triIndices, texcoords };
    const bool same = netMode != 0 && (unsigned)mNetIndex < 2u && sNpNetKeyValid[mNetIndex] && NpNetKeyEqual(sNpNetKey[mNetIndex], key);
    if (same && netMode == 2)
    {
        sNpNetChecked++;
        if (memcmp(triIndices, netMesh.m_TriStripIndices, netMesh.m_NumTriStripIndices * sizeof(u16)) != 0
            || memcmp(texcoords, netMesh.m_v2TextureCoords, netMesh.m_NumParticles * sizeof(shortVector2)) != 0)
            sNpNetWrong++;
    }
    if (same && netMode == 1)
    {
        sNpNetSkipped++;
        if (NpSim26NetGrabBreak() && netMesh.m_NumParticles > 0)
            texcoords[0].x ^= 0x40;   // proof mutant (MSC_SIM26_NETGRAB_BREAK=1): the skipped buffer is spoiled, the pictures must change
        for (int i = 0; i < netMesh.m_NumParticles; ++i)
        {
            mPositions[i] = netMesh.m_v3Position[i];
        }
    }
    else
    {
    for (int i = 0; i < netMesh.m_NumTriStripIndices; ++i)
    {
        *triIndices++ = netMesh.m_TriStripIndices[i];
    }

    for (int i = 0; i < netMesh.m_NumParticles; ++i)
    {
        mPositions[i] = netMesh.m_v3Position[i];
        *texcoords++ = netMesh.m_v2TextureCoords[i];
    }
    if (netMode != 0 && (unsigned)mNetIndex < 2u)
    {
        sNpNetKey[mNetIndex] = key;
        sNpNetKeyValid[mNetIndex] = true;
        sNpNetCopied++;
    }
    }
    if (netMode != 0 && ((sNpNetSkipped + sNpNetCopied + sNpNetChecked) % 3000) == 1)
    {
        printf("[sim26net] mode=%d grabs copied %llu, index/texcoord copies skipped %llu, verify %llu wrong %llu\n", netMode, sNpNetCopied, sNpNetSkipped, sNpNetChecked, sNpNetWrong);
        fflush(stdout);
    }

    mVisible = netMesh.mbIsActive;
#ifdef NP_NATIVE
    if ((unsigned)mNetIndex < 2u)
    {
        if (mVisible || mSnapGen == 0u)
            ++gNpNetDirtyGen[mNetIndex];
        mSnapGen = gNpNetDirtyGen[mNetIndex];
    }
    else
    {
        mSnapGen = 0u;
    }
#endif
}

void DrawableNetMesh::Blend(
    float blendFactor, const DrawableNetMesh& lhs, const DrawableNetMesh& rhs)
{
    if (!lhs.mInitialized || !rhs.mInitialized)
    {
        return;
    }

    if (!mInitialized)
    {
        int numTriIndices = lhs.mNumTriIndices;
        int numVertices = lhs.mNumVertices;
        mNumTriIndices = numTriIndices;
        mNumVertices = numVertices;
        Initialize(numVertices, numTriIndices);
        mInitialized = true;
        mJoltCache = 0.0f;
#ifdef NP_NATIVE
        if ((unsigned)mNetIndex < 2u)
        {
            sNetBlendAtRest[mNetIndex] = false;
            sNetBuiltAtRest[mNetIndex] = false;
        }
#endif
    }

#ifdef NP_NATIVE
    const bool bothAtRest = !lhs.mVisible && !rhs.mVisible;
    const bool sameStatic = bothAtRest && lhs.mSnapGen == rhs.mSnapGen && lhs.mSnapGen != 0u;
    if (NpGameFix(NP_FIX_NET_REDRAW) && sameStatic && (unsigned)mNetIndex < 2u
        && sNetBlendDst[mNetIndex] == this
        && sNetBlendAtRest[mNetIndex]
        && sNetBlendGen[mNetIndex] == lhs.mSnapGen)
    {
        mSnapGen = lhs.mSnapGen;
        mVisible = false;
        return;
    }
    const float oneMinusBlend = 1.0f - blendFactor;
    nlVector3* const dst = mPositions;
    const nlVector3* const srcL = lhs.mPositions;
    const nlVector3* const srcR = rhs.mPositions;
    for (int i = 0; i < mNumVertices; ++i)
    {
        nlVector3 tmp;
        nlVec3Scale(tmp, srcL[i], oneMinusBlend);
        nlVec3ScaleAdd(dst[i], blendFactor, srcR[i], tmp);
    }
    mSnapGen = sameStatic ? lhs.mSnapGen : 0u;
    if ((unsigned)mNetIndex < 2u)
    {
        sNetBlendDst[mNetIndex] = this;
        sNetBlendGen[mNetIndex] = lhs.mSnapGen;
        sNetBlendAtRest[mNetIndex] = sameStatic;
        if (!sameStatic)
            sNetBuiltAtRest[mNetIndex] = false;
    }
#else
    nlVector3* destination;
    nlVector3* source;
    float oneMinusBlend = 1.0f - blendFactor;

    int offset;
    int i;
    for (i = 0, offset = 0; i < mNumVertices; ++i, offset += sizeof(nlVector3))
    {
        source = (nlVector3*)((char*)((const volatile DrawableNetMesh*)&lhs)->mPositions + offset);
        destination = (nlVector3*)((char*)((volatile DrawableNetMesh*)this)->mPositions + offset);
        nlVec3Scale(*destination, *source, oneMinusBlend);
    }

    for (int i = 0; i < mNumVertices; ++i)
    {
        destination = &((volatile DrawableNetMesh*)this)->mPositions[i];
        source = (nlVector3*)&((const volatile DrawableNetMesh*)&rhs)->mPositions[i];
        nlVec3ScaleAdd(*destination, blendFactor, *source, *destination);
    }
#endif

    mVisible = lhs.mVisible;
}

void DrawableNetMesh::Replay(LoadFrame& frame)
{
    float joltValue = 0.0f;
    Replayable<0>(frame, joltValue);

    if (joltValue != mJoltCache)
    {
        mJoltCache = joltValue;
        if (mNetMesh != 0 && mJoltCache > 0.0f)
        {
            mNetMesh->JoltNet();
        }
    }

    bool visible = true;
    Replayable<0>(frame, visible);
    if (mVisible != visible)
    {
        sbNetModelValid[mNetIndex] = false;
        mVisible = visible;
    }
#ifdef NP_NATIVE
    mSnapGen = 0u;
    if ((unsigned)mNetIndex < 2u)
    {
        ++gNpNetDirtyGen[mNetIndex];
        sNetBlendAtRest[mNetIndex] = false;
        sNetBuiltAtRest[mNetIndex] = false;
    }
#endif
}

void DrawableNetMesh::Replay(SaveFrame& frame)
{
    mJoltCache = mNetMesh->mJolt;
    Replayable<0>(frame, mJoltCache);
    Replayable<0>(frame, mVisible);
}
