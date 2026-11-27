#include "NL/nlDLListContainer.inl"
#include "Game/Effects/ParticleSystem.h"
#include "Game/Sys/debug.h"

#include <math.h>

#include "Game/Effects/EmissionManager.h"
#include "Game/GL/GLInventory.h"
#include "Game/GL/GLTexturedColourMeshWriter.h"
#include "Game/GL/GLVertexAnim.h"
#include "NL/gl/gl.h"
#include "NL/gl/glDraw3.h"
#include "NL/gl/glMatrix.h"
#include "NL/gl/glState.h"
#include "NL/gl/glTexture.h"
#include "NL/gl/glView.h"
#include "NL/gl/glMaterialParameters.h"
#include "Game/TweakValueFloat.h"
#include "Game/SharedStaticStorage.h"
#include "NL/nlMemory.h"
#include "NL/nlString.h"
#include "NL/platvmath.h"
#ifdef NP_NATIVE
#include "Game/Render/MegastrikeBackgroundOverlay.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#ifdef NP_NATIVE
#include "np/cpu24.h"
#include "np/fx.h"
#include "np/fxtight.h"
#include "np/vgx_rainhull.h"
#include "np/scenecut.h"
#include "np/stadiumcut.h"
#include "NL/gl/glTextureManager.h"
#include "NL/glx/glxTexture.h"
extern glTextureManager* gTextureManager;
#endif

#ifdef NP_NATIVE
extern "C" int VgxMegaStep(void) __attribute__((weak)); // renderer (vgx_backend_gl.cpp)
extern "C" int VgxFxPassDiv(void) __attribute__((weak));       // renderer: the side divisor of the effects pass while it is in use, 1 while not
extern "C" float VgxFxPixelScale(void) __attribute__((weak));  // renderer: the size of the picture against the game's own, at most 1
extern "C" int VgxFxGovLevel(void) __attribute__((weak));      // renderer (vgx_fxgov.h): how far the governor thins
extern "C" void VgxFxGovFill(float screens) __attribute__((weak)); // renderer: the screens of last frame's sprites it could thin
#endif
#endif

struct TextureFrame
{
    float u;
    float v;
    float increment;
}; // size: 0x0C

#ifdef NP_NATIVE
#include "platform/assets/np_endian.h"
#include "np/fxmodelov.h"
#ifndef NP_GL_STREAM_HOST_ORDER
#define NP_GL_STREAM_HOST_ORDER 1   /* platform/gx/include/np_gx.h: bit 0 of glModelStream::unknown07 */
#endif
// MSC_RAIN_HULL (np/vgx_rainhull.h): The Dump's rain streak model drawn as small quads around the visible dashes of its repeating texture instead of one quad that is almost all alpha 0.
// =1 on (rectangles per tile at most MSC_RAIN_HULL_RECTS, default 8), =2 on with a [rainhull] statistics line, MSC_RAIN_HULL_CLASS=0..2 the deepest class used (default 2), MSC_RAIN_HULL_LOG=1
// logs what the model and its texture are. Render only: nothing the simulation reads. Default off in code; the Vita plan turns it on.
namespace {
enum { kRainFrames = 64 };
struct RainHull {
    const GLVertexAnim* src;       // the animation this was built for (a stadium load makes a new one)
    const void* tex;               // the texture and its texel data the grids were read from (a reload makes the hulls stale: rebuilt)
    const void* texData;
    bool built;
    bool ok;
    int frames;
    unsigned char* cls[kNpTightClasses][kRainFrames]; // hull GEOMETRY by class and animation frame (a malloc block that starts with a glModelPacket holding only index buffer, streams, counts, primType), 0: that class stays the quad
    float frac[kNpTightClasses][kRainFrames];   // the hull's area as a part of the quad's
    float texU[kRainFrames], texV;              // texels across the quad's u and v extent
    float radius;                               // MSC_FX_MODEL_OFFVIEW: the farthest any vertex of any frame lies from the model's origin (0: not known, no culling)
};
// The templates that get hulls: model particles whose model is one 4-vertex quad over a REPEATING 32x32 RGB5A3 texture with mostly alpha 0 (a build refuses anything else by itself:
// RainBuild's checks). A table, one RainHull each; MSC_RAIN_HULL_HASH=0x... adds one more candidate for a test (the census of model particles in the other stadiums is MSC_RAIN_HULL_LOG=2).
enum { kRainSlots = 4 };
unsigned sRainHash[kRainSlots] = {0x8270565bu, 0, 0, 0};   // fx_dump_rain
RainHull sRainSlot[kRainSlots];
RainHull* RainSlotFor(unsigned hash)
{
    for (int i = 0; i < kRainSlots; ++i) if (sRainHash[i] == hash && hash != 0) return &sRainSlot[i];
    return 0;
}
int sRainMode = -1, sRainStrips = 1, sRainMargin = 1, sRainRects = 8, sRainMaxClass = 2, sRainLog;
static inline bool RainOn() { return sRainMode > 0 || NpStadiumCutHas(NP_CUT_RAINHULL); }   // MSC_RAIN_HULL=1|2, or the stadium cut bit 16384 (The Dump's row of MSC_STADIUM_CUT)
// MSC_FX_RAIN_FAST (default 1) and MSC_FX_MODEL_OFFVIEW (0 off, 1 cull, 2 count only; default 1), both render only, both exact: see the model branch of RenderAllParticles.
int sRainFast = -1, sModelOv = -1, sModelOvBreak;
struct ModelOvStat { unsigned long drops, culled, calls; } sMov;
void RainInit2()
{
    const char* e = getenv("MSC_FX_RAIN_FAST");
    sRainFast = e && *e ? atoi(e) : 1;
    e = getenv("MSC_FX_MODEL_OFFVIEW");
    sModelOv = e && *e ? atoi(e) : 1;
    if (sModelOv < 0 || sModelOv > 2) sModelOv = 1;
    e = getenv("MSC_FX_MODEL_OFFVIEW_BREAK");
    sModelOvBreak = e && *e && atoi(e);
}
// the GetModel frame of a drop (the two forms of the model branch's own code, the same float operations)
static inline int RainFrameOf(const Particle* pPart, const GLVertexAnim* pAnim, bool byLife)
{
    if (byLife) {
        float frameFrac = pPart->timeElapsed / pPart->lifeSpan;
        float frame = frameFrac * (float)((int)pAnim->m_nNumFrames - 1);
        return (int)frame;
    }
    float frame = pPart->FPS * pPart->timeElapsed;
    float numFrames = (float)(int)pAnim->m_nNumFrames;
    while (frame >= numFrames) frame -= numFrames;
    return (int)frame;
}
struct RainStat { unsigned drops, cls[4]; double quadPx, drawnPx; unsigned calls; } sRs;
unsigned sRainLogged;

void RainInit()
{
    const char* e = getenv("MSC_RAIN_HULL");
    sRainMode = e && *e ? atoi(e) : 0;
    e = getenv("MSC_RAIN_HULL_RECTS");
    sRainRects = e && *e ? atoi(e) : 8;
    if (sRainRects < 1) sRainRects = 1;
    if (sRainRects > kNpRainMaxRects) sRainRects = kNpRainMaxRects;
    e = getenv("MSC_RAIN_HULL_CLASS");
    sRainMaxClass = e && *e ? atoi(e) : 2;
    if (sRainMaxClass < 0) sRainMaxClass = 0;
    if (sRainMaxClass > 2) sRainMaxClass = 2;
    e = getenv("MSC_RAIN_HULL_LOG");
    sRainLog = e && *e ? atoi(e) : 0;
    e = getenv("MSC_RAIN_HULL_MARGIN");   // lattice units (1/1024 tile) of margin round every rectangle, default 1
    if (e && *e) { sRainMargin = atoi(e); if (sRainMargin < 0) sRainMargin = 0; if (sRainMargin > 64) sRainMargin = 64; }
    e = getenv("MSC_RAIN_HULL_STRIPS");   // 1 (default): full-height strips, watertight; 0: free rectangles (a few single pixels differ)
    if (e && *e) sRainStrips = atoi(e);
    e = getenv("MSC_RAIN_HULL_HASH");
    if (e && *e) sRainHash[1] = (unsigned)strtoul(e, 0, 0);
}

PlatTexture* RainTexture(const glModelPacket& k)
{
    glTextureBinding* b = (glTextureBinding*)k.materialParameters;
    if (!b) return 0;
    if (b->textureIndex == 0xFFFFu) gTextureManager->ResolveTextureIndex(b);
    unsigned long key = b->textureIndex;
    if (key == 0xFFFFu) return 0;
    PlatTexture* t = (PlatTexture*)gTextureManager->GetTextureAtIndex(&key);
    return t && !t->m_bMissingTexture && t->m_SwizzledData ? t : 0;
}

static inline float RainRdF(const unsigned char* a, bool host) { if (host) { float f; memcpy(&f, a, 4); return f; } return npa_rdf32(a); }
static inline int RainRdS16(const unsigned char* a, bool host) { return host ? (int)(short)npa_hrd16(a) : (int)(short)npa_rd16(a); }
static inline void RainWrF(unsigned char* a, float f, bool host) { if (host) memcpy(a, &f, 4); else npa_wrf32(a, f); }
static inline void RainWrS16(unsigned char* a, int v, bool host) { if (host) { unsigned short w = (unsigned short)(short)v; memcpy(a, &w, 2); } else npa_wr16(a, (unsigned short)(short)v); }

// Builds the hull models of every frame of the rain's vertex animation (each frame is a model of one 4-vertex strip with its own streams), once per animation.
bool RainBuild(RainHull& r, GLVertexAnim* an)
{
    for (int c = 0; c < kNpTightClasses; ++c) for (int f = 0; f < kRainFrames; ++f) free(r.cls[c][f]);   // (geometry blocks of an earlier build)
    r.src = an;
    r.tex = 0;
    r.texData = 0;
    r.built = true;
    r.ok = false;
    r.frames = 0;
    r.radius = 0.0f;
    float rad2 = 0.0f;
    memset(r.cls, 0, sizeof r.cls);
    for (int c = 0; c < kNpTightClasses; ++c) for (int f = 0; f < kRainFrames; ++f) r.frac[c][f] = 1.0f;
    const int nf = an ? (int)an->m_nNumFrames : 0;
    if (nf < 1 || nf > kRainFrames) { if (sRainLog) fprintf(stderr, "[rainhull] %d animation frames: no hull\n", nf); return false; }
    static uint8_t g0[64 * 64], g1[64 * 64], g2[64 * 64];
    uint8_t* const grids[3] = {g0, g1, g2};
    bool gridsDone = false;
    static float (*pos)[4][3];
    static int (*uv)[4][2];
    if (!pos) { pos = (float (*)[4][3])malloc(256 * sizeof(float[4][3])); uv = (int (*)[4][2])malloc(256 * sizeof(int[4][2])); }
    if (!pos || !uv) return false;
    NpRainTile tiles[kNpTightClasses];
    bool tileOk[kNpTightClasses] = {false, false, false};
    r.texV = 32.0f;
    for (int fi = 0; fi < nf; ++fi) {
        glModel* m = an->GetModel(fi);   // (a copy in the frame's memory; its streams point at this frame's vertices)
        if (!m || m->numPackets != 1) return false;
        const glModelPacket& k = m->packets[0];
        if (k.primType != GLP_TriStrip || k.numUniqueVertices != 4 || k.numStreams != 2 || !k.streams) { if (sRainLog) fprintf(stderr, "[rainhull] frame %d is not a 4-vertex strip with 2 streams: no hull\n", fi); return false; }
        const glModelStream* ps = glFindModelStream(const_cast<glModelPacket*>(&k), 1);
        const glModelStream* ts = glFindModelStream(const_cast<glModelPacket*>(&k), 4);
        if (!ps || !ts || ps->stride != 12 || ts->stride != 4 || !ps->address || !ts->address) { if (sRainLog) fprintf(stderr, "[rainhull] unexpected streams: no hull\n"); return false; }
        const bool hostP = (ps->unknown07 & NP_GL_STREAM_HOST_ORDER) != 0, hostT = (ts->unknown07 & NP_GL_STREAM_HOST_ORDER) != 0;
        NpRainQuad q;
        for (int i = 0; i < 4; ++i) {
            const unsigned char* a = (const unsigned char*)ps->address + i * 12;
            q.pos[i][0] = RainRdF(a, hostP); q.pos[i][1] = RainRdF(a + 4, hostP); q.pos[i][2] = RainRdF(a + 8, hostP);
            { const float d2 = q.pos[i][0] * q.pos[i][0] + q.pos[i][1] * q.pos[i][1] + q.pos[i][2] * q.pos[i][2]; if (!(d2 <= rad2)) rad2 = d2; }   // (a NaN makes the radius NaN: no culling)
            const unsigned char* t = (const unsigned char*)ts->address + i * 4;
            q.u[i] = RainRdS16(t, hostT); q.v[i] = RainRdS16(t + 2, hostT);
        }
        NpRainFrame f;
        if (!NpRainFrameOf(q, &f) || f.vhi - f.vlo != 4096) { if (sRainLog) fprintf(stderr, "[rainhull] frame %d: the quad is not an affine one tile high (u %d..%d v %d..%d): no hull for it\n", fi, q.u[0], q.u[2], q.v[0], q.v[1]); continue; }
        if (sRainLog && fi < 4) fprintf(stderr, "[rainhull] frame %d: u %d..%d v %d..%d, x %g..%g z %g..%g\n", fi, f.ulo, f.uhi, f.vlo, f.vhi, (double)q.pos[0][0], (double)q.pos[2][0], (double)q.pos[0][2], (double)q.pos[1][2]);
        if (!gridsDone) {
            PlatTexture* tex = an->m_pModel && an->m_pModel->numPackets >= 1 ? RainTexture(an->m_pModel->packets[0]) : RainTexture(k);
            if (!tex || tex->m_Format != GXTex_RGB5A3 || tex->m_Width != 32 || tex->m_Height != 32) { if (sRainLog) fprintf(stderr, "[rainhull] texture not ready or not RGB5A3 32x32: asked again later\n"); r.built = false; return false; }
            if (!NpRainHullGrid(tex->m_Format, 32, 32, tex->m_Levels, (const uint8_t*)tex->m_SwizzledData, grids)) return false;
            r.tex = tex;
            r.texData = tex->m_SwizzledData;
            for (int c = 0; c <= sRainMaxClass; ++c) tileOk[c] = NpRainRects(grids[c], 64, 64, sRainRects, &tiles[c], sRainMargin, sRainStrips != 0);
            if (const char* brk = getenv("MSC_RAIN_HULL_BREAK"))   // test mutant (the picture proof must catch it): every rectangle's right edge pulled in by 20 lattice units, just over a texel
                if (*brk && atoi(brk)) for (int c = 0; c <= sRainMaxClass; ++c) for (int i = 0; tileOk[c] && i < tiles[c].n; ++i) if (tiles[c].r[i].x1 - 20 > tiles[c].r[i].x0) tiles[c].r[i].x1 -= 20;
            gridsDone = true;
        }
        r.texU[fi] = 32.0f * (float)(f.uhi - f.ulo) / 4096.0f;
        for (int c = 0; c <= sRainMaxClass; ++c) {
            if (!tileOk[c]) continue;
            bool over = false;
            const int nq = NpRainQuads(f, tiles[c], 256, pos, uv, &over);
            if (nq <= 0 || over) continue;
            double area = 0;
            for (int i = 0; i < nq; ++i) area += (double)(uv[i][2][0] - uv[i][0][0]) * (double)(uv[i][2][1] - uv[i][0][1]);
            const float frac = (float)(area / ((double)(f.uhi - f.ulo) * (double)(f.vhi - f.vlo)));
            if (sRainLog && fi == 0) fprintf(stderr, "[rainhull] class %d: %d rectangles a tile, %d quads on the model, %.1f %% of the quad's area\n", c, tiles[c].n, nq, 100.0f * frac);
            if (frac > 0.85f) continue;   // (not worth the extra vertices)
            // one block of GEOMETRY ONLY: a packet (index buffer, streams, counts, primType: nothing the frame owns), two streams, positions, texture coordinates, indices (6 vertices a quad: two
            // triangles each, so a direct draw and an indexed one agree). The material parameters (the texture binding, the colour), the matrix and the raster state are NOT kept: they live in
            // frame memory (glModelDupNoStreams / glFrameAlloc), so a packet copied here once pointed at memory the next picture reused (the crash of data16: 0xcd fill, glx_BindTexture).
            // Every drop takes them from its own fresh copy of the source frame model (RainPatch below).
            const int nv = nq * 6;
            const size_t bytes = sizeof(glModelPacket) + 2 * sizeof(glModelStream) + (size_t)nv * 12 + (size_t)nv * 4 + (size_t)nv * 2 + 64;
            unsigned char* blk = (unsigned char*)malloc(bytes);
            if (!blk) continue;
            memset(blk, 0, bytes);
            glModelPacket* hp = (glModelPacket*)blk;
            glModelStream* hs = (glModelStream*)(blk + sizeof(glModelPacket));
            unsigned char* hpos = (unsigned char*)(hs + 2);
            unsigned char* huv = hpos + (size_t)nv * 12;
            unsigned short* hidx = (unsigned short*)(huv + (size_t)nv * 4);
            hs[0] = *ps;
            hs[1] = *ts;
            hs[0].address = hpos;
            hs[1].address = huv;
            hp->numStreams = k.numStreams;
            hp->streams = hs;
            hp->indexBuffer = hidx;
            hp->numVertices = (unsigned)nv;
            hp->numUniqueVertices = (unsigned short)nv;
            hp->primType = (char)GLP_TriList;
            static const int tri[6] = {0, 1, 2, 0, 2, 3};
            for (int i = 0; i < nq; ++i)
                for (int j = 0; j < 6; ++j) {
                    const int vi = i * 6 + j, c4 = tri[j];
                    for (int a = 0; a < 3; ++a) RainWrF(hpos + vi * 12 + a * 4, pos[i][c4][a], hostP);
                    RainWrS16(huv + vi * 4, uv[i][c4][0], hostT);
                    RainWrS16(huv + vi * 4 + 2, uv[i][c4][1], hostT);
                    hidx[vi] = (unsigned short)vi;
                }
            r.cls[c][fi] = blk;
            r.frac[c][fi] = frac;
            r.ok = true;
        }
    }
    r.frames = nf;
    r.radius = sqrtf(rad2);
    return r.ok;
}
}
#endif

static TweakValueFloat sfParticleRedScale(
    "sfParticleRedScale", "Render/Particles/Colour Scaling", 1.0f);
static TweakValueFloat sfParticleGreenScale(
    "sfParticleGreenScale", "Render/Particles/Colour Scaling", 1.0f);
static TweakValueFloat sfParticleBlueScale(
    "sfParticleBlueScale", "Render/Particles/Colour Scaling", 1.0f);

static TextureFrame* textureFrames[36];

static bool sRenderParticles = true;
float ParticleSystem::m_fAspect = 1.0f;
bool ParticleSystem::m_AllowInFront = true;

static bool sUseWhiteParticleTexture;
static bool sDisableParticleDepthTest;
int ParticleSystem::m_NumInstances;
bool (*ParticleSystem::m_Callback)(ParticleSystem*, GLView*,
    nlDLListSlotPool<Particle*>*, const nlVector3&, const nlVector3&,
    const nlMatrix4*);
glModel* (*ParticleSystem::m_LightingCallback)(glModel*);
static int MaxNumParticles;
int gNumRenderedParticles;
static unsigned short hackyFacingAngle;

ParticleSystem::ParticleSystem(EffectsTemplate* pTemplate,
    nlDLListSlotPool<Particle*>* pFreeParticles, EffectsSpec* spec,
    unsigned long resourceID)
    : m_uResourceID(resourceID)
    , m_Particles()
    , m_pFreeParticles(pFreeParticles)
{
    ++m_NumInstances;
    m_NumParticles = 0;
    m_pTemplate = pTemplate;
    m_pSpec = spec;
    m_fElapsedTime = 0.0f;
    m_fNormalizedTime = 0.0f;
    m_fNumParticlesToCreate = 0.0f;
    m_fDelay = 0.0f;
    m_uLayer = 0;
    m_fRotationOffset = 0.0f;
    nlVec3Set(m_vVelocity, 0.0f, 0.0f, 0.0f);
    nlVec3Set(m_vPosition, 0.0f, 0.0f, 0.0f);
    nlVec3Set(m_vForward, 0.0f, 1.0f, 0.0f);
    nlVec3Set(m_vSourcePosition, 0.0f, 0.0f, 0.0f);
#ifdef NP_NATIVE
    nlVec3Set(m_vDrawOffset, 0.0f, 0.0f, 0.0f);
#endif
    m_mCoordSys.SetIdentity();
    m_aFacing = 0;
    m_bAmDying = false;
    m_bVisible = false;
    m_uTextureIndex = glGetTextureIndex(m_pTemplate->m_hTexture);
}

ParticleSystem::~ParticleSystem()
{
    if (m_pTemplate->m_uEmitterDeathCode != 0)
    {
        tDebugPrintManager::Print(DC_RENDER, "OnEmitterDeath: %d\n",
            m_pTemplate->m_uEmitterDeathCode);
    }

    --m_NumInstances;
    while (m_Particles.m_Head != 0)
    {
        Particle* particle;
        m_Particles.RemoveStart(&particle);
        m_pFreeParticles->AddEnd(particle);
    }
    m_NumParticles = 0;
}

void ParticleSystem::UpdateCoordSys()
{
    UpdateCoordSys(m_mCoordSys);
}

void ParticleSystem::UpdateCoordSys(nlMatrix4& mCoordSys)
{
    nlVector3 grav;
    nlVec3Normalize(grav, m_vForward);

    nlVector3 right;
    nlVector3 ref;
    nlVec3Set(ref, 0.0f, 0.0f, 1.0f);
    if ((float)__fabs(nlVec3DotProduct(ref, grav)) > 0.99f)
    {
        nlVec3Set(ref, 0.0f, 1.0f, 0.0f);
    }

    nlVec3CrossProduct(right, grav, ref);
    nlVec3Normalize(right, right);

    nlVec3CrossProduct(ref, right, grav);
    nlVec3Normalize(ref, ref);

    nlVec3Neg(grav, grav);

    mCoordSys.SetRow_(0, right);
    mCoordSys.SetRow_(1, ref);
    mCoordSys.SetRow_(2, grav);
    mCoordSys.SetTranslation(m_vPosition);
    mCoordSys.e[11] = 0.0f;
    mCoordSys.e[7] = 0.0f;
    mCoordSys.e[3] = 0.0f;
}

typedef void (*EmitParticlePosition)(nlVector3&, nlVector3&,
    ParticleSystem*, EffectsSpec*, const nlMatrix4&);

static void EmitCircularPosition(nlVector3& pos, nlVector3& dir,
    ParticleSystem* pSystem, EffectsSpec* pSpec,
    const nlMatrix4& mLocalToWorld)
{
    EffectsTemplate* pTemplate = pSystem->m_pTemplate;
    float randomAngle = RandomizedValue(0.0f, 6.2831855f);
    float sinVal;
    float cosVal;
    nlSinCos(&sinVal, &cosVal,
        (unsigned short)(int)(10430.378f * randomAngle));

    float radius = pTemplate->EvaluateProperty(4, pSystem->m_fNormalizedTime);
    nlVector3 localPos;
    nlVec3Set(localPos, cosVal * radius, -sinVal * radius, 0.0f);

    if (pSpec != 0)
    {
        localPos.x += pSpec->m_vLocalOffset.x;
        localPos.y += pSpec->m_vLocalOffset.y;
        localPos.z += pSpec->m_vLocalOffset.z;
    }

    if (pTemplate->IsLocalSpace())
        pos = localPos;
    else
        nlMultPosVectorMatrix(pos, localPos, mLocalToWorld);
}

static void EmitDiscPosition(nlVector3& pos, nlVector3& dir,
    ParticleSystem* pSystem, EffectsSpec* pSpec,
    const nlMatrix4& mLocalToWorld)
{
    EffectsTemplate* pTemplate = pSystem->m_pTemplate;
    float randomAngle = RandomizedValue(0.0f, 6.2831855f);
    float sinVal;
    float cosVal;
    nlSinCos(&sinVal, &cosVal,
        (unsigned short)(int)(10430.378f * randomAngle));

    float radius
        = pTemplate->EvaluateProperty(4, pSystem->m_fNormalizedTime);
    radius = RandomizedValue(0.0f, radius);

    nlVector3 localPos;
    nlVec3Set(localPos, cosVal * radius, -sinVal * radius, 0.0f);

    if (pSpec != 0)
    {
        localPos.x += pSpec->m_vLocalOffset.x;
        localPos.y += pSpec->m_vLocalOffset.y;
        localPos.z += pSpec->m_vLocalOffset.z;
    }

    if (pTemplate->IsLocalSpace())
        pos = localPos;
    else
        nlMultPosVectorMatrix(pos, localPos, mLocalToWorld);
}

static void EmitSphericalPosition(nlVector3& pos, nlVector3& dir,
    ParticleSystem* pSystem, EffectsSpec* pSpec,
    const nlMatrix4& mLocalToWorld)
{
    EffectsTemplate* pTemplate = pSystem->m_pTemplate;
    float randomZ = RandomizedValue(0.0f, 2.0f);
    float randomAngleValue = RandomizedValue(6.2831855f);
    float xyRadius = nlSqrt(1.0f - randomZ * randomZ, true);

    float sinVal;
    float cosVal;
    nlSinCos(&sinVal, &cosVal,
        (unsigned short)(int)(10430.378f * randomAngleValue));

    nlVector3 localPos;
    nlVector3 localDir;
    float x = xyRadius * cosVal;
    float y = xyRadius * sinVal;
    float z = randomZ;
    float radius
        = pTemplate->EvaluateProperty(4, pSystem->m_fNormalizedTime);
    nlVec3Set(localDir, x, y, z);
    nlVec3Scale(localPos, localDir, radius);

    if (pSpec != 0)
    {
        localPos.x += pSpec->m_vLocalOffset.x;
        localPos.y += pSpec->m_vLocalOffset.y;
        localPos.z += pSpec->m_vLocalOffset.z;
    }

    if (pTemplate->IsLocalSpace())
    {
        pos = localPos;
        dir = localDir;
    }
    else
    {
        nlMultPosVectorMatrix(pos, localPos, mLocalToWorld);
        nlMultDirVectorMatrix(dir, localDir, mLocalToWorld);
    }
}

static void EmitHemisphericalPosition(nlVector3& pos, nlVector3& dir,
    ParticleSystem* pSystem, EffectsSpec* pSpec,
    const nlMatrix4& mLocalToWorld)
{
    EffectsTemplate* pTemplate = pSystem->m_pTemplate;
    float randomZ = RandomizedValue(-0.5f, 1.0f);
    float randomAngleValue = RandomizedValue(6.2831855f);
    float xyRadius = nlSqrt(1.0f - randomZ * randomZ, true);

    float sinVal;
    float cosVal;
    nlSinCos(&sinVal, &cosVal,
        (unsigned short)(int)(10430.378f * randomAngleValue));

    nlVector3 localPos;
    nlVector3 localDir;
    float x = xyRadius * cosVal;
    float y = xyRadius * sinVal;
    float z = randomZ;
    float radius
        = pTemplate->EvaluateProperty(4, pSystem->m_fNormalizedTime);
    nlVec3Set(localDir, x, y, z);
    nlVec3Scale(localPos, localDir, radius);

    if (pSpec != 0)
    {
        localPos.x += pSpec->m_vLocalOffset.x;
        localPos.y += pSpec->m_vLocalOffset.y;
        localPos.z += pSpec->m_vLocalOffset.z;
    }

    if (pTemplate->IsLocalSpace())
    {
        pos = localPos;
        dir = localDir;
    }
    else
    {
        nlMultPosVectorMatrix(pos, localPos, mLocalToWorld);
        nlMultDirVectorMatrix(dir, localDir, mLocalToWorld);
    }
}

static inline void RotateXYInPlace(nlVector3& v, float sn, float cs)
{
    float x = (v.x * cs) + (-v.y * sn);
    float y = (v.x * sn) + (v.y * cs);
    nlVec3Set(v, x, y, v.z);
}

static inline void RotateXZInPlace(nlVector3& v, float sn, float cs)
{
    float x = (v.x * cs) + (v.z * sn);
    float z = (-v.x * sn) + (v.z * cs);
    nlVec3Set(v, x, v.y, z);
}

static void EmitSpindularPosition(nlVector3& pos, nlVector3& dir,
    ParticleSystem* pSystem, EffectsSpec* pSpec,
    const nlMatrix4& mLocalToWorld)
{
    EffectsTemplate* pTemplate = pSystem->m_pTemplate;
    nlVector3 localPos;
    nlVector3 localDir;
    float sin;
    float cos;
    float randomAngle = RandomizedValue(0.0f, 6.2831855f);
    nlSinCos(&sin, &cos,
        (unsigned short)(int)(10430.378f * randomAngle));

    float radius
        = pTemplate->EvaluateProperty(4, pSystem->m_fNormalizedTime);
    nlVec3Set(localPos, cos * radius, -sin * radius, 0.0f);

    float tilt = pTemplate->EvaluateProperty(6, pSystem->m_fNormalizedTime);
    if (tilt <= -90.0f)
        tilt = -89.9f;
    else if (tilt >= 90.0f)
        tilt = 89.9f;

    localDir.z = nlTan((unsigned short)(((int)(-tilt * 65536.0f)) / 360));
    localDir.x = cos;
    localDir.y = -sin;
    float lengthSq = nlVec3LengthSquared(localDir);
    float length = nlRecipSqrt(lengthSq, false);
    nlVec3Set(localDir,
        length * localDir.x,
        length * localDir.y,
        length * localDir.z);

    float tiltRotation
        = pTemplate->EvaluateProperty(7, pSystem->m_fNormalizedTime);
    tiltRotation = -tiltRotation * 3.14159265f / 180.0f;
    if (tiltRotation != 0.0f)
    {
        nlSinCos(&sin, &cos,
            (unsigned short)(int)(10430.378f * tiltRotation));
        RotateXZInPlace(localDir, sin, cos);
        RotateXZInPlace(localPos, sin, cos);
    }

    if (pSpec != 0)
    {
        localPos.x += pSpec->m_vLocalOffset.x;
        localPos.y += pSpec->m_vLocalOffset.y;
        localPos.z += pSpec->m_vLocalOffset.z;
    }

    if (pTemplate->IsLocalSpace())
    {
        pos = localPos;
        dir = localDir;
    }
    else
    {
        nlMultDirVectorMatrix(pos, localPos, mLocalToWorld);
        nlMultDirVectorMatrix(dir, localDir, mLocalToWorld);
        if (hackyFacingAngle != 0)
        {
            nlSinCos(&sin, &cos, hackyFacingAngle);
            RotateXYInPlace(dir, sin, cos);
            RotateXYInPlace(pos, sin, cos);
        }
        nlVec3Set(pos,
            pos.x + mLocalToWorld.e2[3][0],
            pos.y + mLocalToWorld.e2[3][1],
            pos.z + mLocalToWorld.e2[3][2]);
    }
}

void ParticleSystem::CreateNewParticles(int numParticles)
{
    EmitParticlePosition emit;
    nlVector3 baseDir;
    nlVector3 dir;
    int i;
    nlMatrix4& mCoordSys = m_mCoordSys;

    if (m_pTemplate->IsLocalSpace())
        nlVec3Set(baseDir, 0.0f, 0.0f, -1.0f);
    else
        baseDir = m_vForward;

    switch (m_pTemplate->m_eEmitter)
    {
    case Emitter_Circle:
        emit = EmitCircularPosition;
        break;
    case Emitter_Disc:
        emit = EmitDiscPosition;
        break;
    case Emitter_Sphere:
        emit = EmitSphericalPosition;
        break;
    case Emitter_Spindle:
        emit = EmitSpindularPosition;
        hackyFacingAngle = m_aFacing;
        break;
    case Emitter_Hemisphere:
        emit = EmitHemisphericalPosition;
        break;
    default:
        emit = 0;
        break;
    }

    for (i = 0; i < numParticles; ++i)
    {
        Particle* removed;
        Particle* pPart = m_pFreeParticles->m_Head == 0 ? 0
            : (m_pFreeParticles->RemoveStart(&removed), removed);
        if (pPart == 0)
            break;

        if (m_pTemplate->m_uParticleCreationCode != 0)
        {
            tDebugPrintManager::Print(DC_RENDER, "OnParticleCreation: %d\n",
                m_pTemplate->m_uParticleCreationCode);
        }

        m_Particles.AddStart(pPart);
        ++m_NumParticles;

        dir = baseDir;
        pPart->pTemplate = m_pTemplate;
        emit(pPart->initialPosition, dir, this, m_pSpec, mCoordSys);
        pPart->position.x = pPart->initialPosition.x + m_vSourcePosition.x;
        pPart->position.y = pPart->initialPosition.y + m_vSourcePosition.y;
        pPart->position.z = pPart->initialPosition.z + m_vSourcePosition.z;

        pPart->lifeSpan = RandomizedValue(m_pTemplate->m_rParticleLife);
        pPart->initialRotation
            = RandomizedValue(m_pTemplate->m_rRotation);
        pPart->rot = pPart->initialRotation + m_fRotationOffset;
        pPart->dRot
            = m_pTemplate->mProperties[3]->Evaluate(0.0f);
        pPart->mass = RandomizedValue(m_pTemplate->m_rMass);
        pPart->size
            = m_pTemplate->mProperties[1]->Evaluate(0.0f);
        pPart->sizeScale
            = m_pTemplate->mProperties[2]->Evaluate(0.0f);
        pPart->flipTexcoords
            = nlRandomf(100.0f, &gEffectsRandomSeed) < m_pTemplate->m_fTexcoordFlipPercentage;

        float inheritVelocity
            = RandomizedValue(m_pTemplate->m_rInheritVelocity);
        nlVector3 velocity;
        nlVec3Scale(velocity, m_vVelocity, inheritVelocity);
        if (m_pTemplate->mProperties[5]->mUseCurve == 0)
        {
            float vel = m_pTemplate->mProperties[5]->Evaluate(0.0f);
            nlVec3ScaleAdd(velocity, vel, dir, velocity);
        }
        pPart->velocity = nlSqrt(nlVec3LengthSquared(velocity), true);
        if (pPart->velocity == 0.0f)
            pPart->velDir = dir;
        else
            nlVec3Scale(pPart->velDir, velocity,
                nlRecipSqrt(nlVec3LengthSquared(velocity), true));

        pPart->acceleration
            = RandomizedValue(m_pTemplate->m_rAcceleration);
        pPart->frame = 0.0f;
        pPart->FPS = RandomizedValue(m_pTemplate->m_rFPS);
        pPart->timeElapsed = 0.0f;
        pPart->timeFraction = 0.0f;
    }
}

void ParticleSystem::UpdateAllParticles(float dt,
    const nlMatrix4* pCoordSys)
{
    nlDLListIterator<Particle*> iterator;
    iterator = m_Particles.Begin();
    while (iterator.hasNext())
    {
        Particle* p = *iterator;
        p->timeElapsed += dt;
        p->timeFraction = p->timeElapsed / p->lifeSpan;
        if (p->timeElapsed >= p->lifeSpan)
        {
            if (m_pTemplate->m_uParticleDeathCode != 0)
            {
                tDebugPrintManager::Print(DC_RENDER, "OnParticleDeath: %d\n",
                    m_pTemplate->m_uParticleDeathCode);
            }
            m_Particles.Remove(&iterator, 0);
            --m_NumParticles;
            m_pFreeParticles->AddEnd(p);
        }
        else
        {
            iterator.Step();
            UpdateParticleMotion(p, pCoordSys);
        }
    }
}

void ParticleSystem::UpdateLight(EffectsLight* pLight, Particle* pPart,
    EffectsTemplate* pTemplate, const nlVector3& viewRight,
    const nlVector3& viewUp,
    const nlMatrix4* pCoordSys)
{
    int colourIndex = (int)(24.5f * pPart->timeFraction);
    pLight->m_Colour = pTemplate->m_cColour[colourIndex];

    float size;
    if (pPart->pTemplate->mProperties[1]->mUseCurve != 0)
        size = pPart->pTemplate->mProperties[1]->Evaluate(
            pPart->timeFraction);
    else
        size = pPart->size;

    if (pPart->pTemplate->mProperties[2]->mUseCurve != 0)
        size *= pPart->pTemplate->mProperties[2]->Evaluate(
            m_fNormalizedTime);
    else
        size *= pPart->sizeScale;
    pLight->m_fRadius = 0.5f * size;

    pLight->m_v3Position = pPart->position;
    if (pCoordSys != 0)
    {
        nlVector3 position;
        nlMultPosVectorMatrix(position, pLight->m_v3Position, *pCoordSys);
        pLight->m_v3Position = position;
    }
}

void ParticleSystem::UpdateParticleMotion(Particle* pPart,
    const nlMatrix4* pCoordSys)
{
    float velocityCurve = 0.0f;
    if (pPart->pTemplate->mProperties[5]->mUseCurve != 0)
    {
        velocityCurve
            = pPart->pTemplate->mProperties[5]->Evaluate(
                pPart->timeFraction);
    }

    float rotationDelta;
    if (pPart->pTemplate->mProperties[3]->mUseCurve != 0)
    {
        rotationDelta
            = pPart->pTemplate->mProperties[3]->Evaluate(
                pPart->timeFraction);
    }
    else
    {
        rotationDelta = pPart->dRot;
    }
    pPart->rot += m_fDeltaTime * rotationDelta;

    float velocity = pPart->velocity + velocityCurve
        + pPart->acceleration * pPart->timeElapsed;
    float distance = m_fDeltaTime * velocity;
    nlVec3ScaleAdd(
        pPart->position, distance, pPart->velDir, pPart->position);

    nlVector3 gravity = { 0.0f, 0.0f, 1.0f };
    if (pCoordSys != 0)
        pCoordSys->GetColumn_(2, gravity);

    float gravityDistance
        = pPart->mass * m_fDeltaTime * pPart->timeElapsed;
    nlVec3ScaleAdd(
        pPart->position, gravityDistance, gravity, pPart->position);
}

#ifdef NP_NATIVE
// MSC_CPU24_PART (np/cpu24.h): the same bits as EvaluateParticleColour below, made cheaper. (1) timeFraction is the stored float timeElapsed / lifeSpan of the last
// UpdateAllParticles (the only writers of timeElapsed and timeFraction are the spawn, 0 / 0, and that update; a spawn with lifeSpan <= 0 keeps the divide); (2) the keyframe
// index: floor(x) of a float x >= 0 is its truncation (a float to double to float round trip through libm's floor, a call, per particle); x < 0 or NaN keep floor();
// (3) with the three colour scales exactly 1.0f the channels are c * 1.0f = c, inside 0..255, and the clamps and the three multiplies change nothing.
static nlColour NpEvaluateParticleColourFast(const Particle* pPart, const EffectsTemplate* pTemplate, bool mutant)
{
    const float frac = pPart->lifeSpan > 0.0f ? pPart->timeFraction : pPart->timeElapsed / pPart->lifeSpan;
    float frame = 24.0f * frac;
    int first = frame >= 0.0f ? (int)frame : (int)(float)floor(frame);
    nlColour colour;
    if (first >= 24)
    {
        colour = pTemplate->m_cColour[25];
    }
    else
    {
        const nlColour* firstColour = &pTemplate->m_cColour[first];
        const nlColour* secondColour = &pTemplate->m_cColour[first + 1];
        // (second - first) is 1 and x / 1.0f is x: the divide of the original is the identity
        unsigned int fraction = (unsigned int)(65536.0f * (frame - (float)first));
        unsigned int inverseFraction = 65536 - fraction;
        for (int i = 0; i < 4; ++i)
        {
            colour.c[i] = (unsigned char)((
                fraction * secondColour->c[i]
                + inverseFraction * firstColour->c[i]) >> 16);
        }
    }
    if (mutant && (colour.c[3] & 63) == 1)
        colour.c[0] = (unsigned char)(colour.c[0] + 1);   // proof mutant (MSC_CPU24_BREAK=1)
    return colour;
}
#endif

static nlColour EvaluateParticleColour(Particle* pPart,
    const EffectsTemplate* pTemplate)
{
#ifdef NP_NATIVE
    const int c24 = NpCpu24Mode(NP_C24_PART);
    if ((c24 == 1 || c24 == 3) && sfParticleRedScale.value == 1.0f && sfParticleGreenScale.value == 1.0f && sfParticleBlueScale.value == 1.0f)
    {
        return NpEvaluateParticleColourFast(pPart, pTemplate, c24 == 3);   // (the channels are not scaled: see above)
    }
#endif
    float frame = 24.0f * (pPart->timeElapsed / pPart->lifeSpan);
    int first = (int)(float)floor(frame);
    int second = first + 1;
    nlColour colour;
    if (first >= 24)
    {
        colour = pTemplate->m_cColour[25];
    }
    else
    {
        const nlColour* firstColour = &pTemplate->m_cColour[first];
        const nlColour* secondColour = &pTemplate->m_cColour[second];
        unsigned int fraction = (unsigned int)(65536.0f
            * ((frame - (float)first) / (float)(second - first)));
        unsigned int inverseFraction = 65536 - fraction;
        for (int i = 0; i < 4; ++i)
        {
            colour.c[i] = (unsigned char)((
                fraction * secondColour->c[i]
                + inverseFraction * firstColour->c[i]) >> 16);
        }
    }
    nlColour result;
    float red = colour.c[0] * sfParticleRedScale.value;
    red = red >= 0.0f ? red : 0.0f;
    red = red <= 255.0f ? red : 255.0f;
    result.c[0] = (unsigned char)red;

    float green = colour.c[1] * sfParticleGreenScale.value;
    green = green >= 0.0f ? green : 0.0f;
    green = green <= 255.0f ? green : 255.0f;
    result.c[1] = (unsigned char)green;

    float blue = colour.c[2] * sfParticleBlueScale.value;
    blue = blue >= 0.0f ? blue : 0.0f;
    blue = blue <= 255.0f ? blue : 255.0f;
    result.c[2] = (unsigned char)blue;
    result.c[3] = colour.c[3];
#ifdef NP_NATIVE
    if (c24 == 2 && sfParticleRedScale.value == 1.0f && sfParticleGreenScale.value == 1.0f && sfParticleBlueScale.value == 1.0f)
    {
        // verify: what mode 1 would have returned against what was made
        const nlColour fast = NpEvaluateParticleColourFast(pPart, pTemplate, NpCpu24Break() == 1);
        NpCpu24Note(NP_C24_PART, 1);
        if (memcmp(fast.c, result.c, 4) != 0)
            NpCpu24Note(NP_C24_PART, 2);
    }
#endif
    return result;
}

void ParticleSystem::UpdateParticle(ParticleReturn* pReturn,
    Particle* pPart, EffectsTemplate* pTemplate,
    const nlVector3& viewRight, const nlVector3& viewUp,
    const nlMatrix4* pCoordSys)
{
    pReturn->c = EvaluateParticleColour(pPart, pTemplate);

    float rot = pPart->rot;
    float size;
    if (pPart->pTemplate->mProperties[1]->mUseCurve != 0)
        size = pPart->pTemplate->mProperties[1]->Evaluate(
            pPart->timeFraction);
    else
        size = pPart->size;
    if (pPart->pTemplate->mProperties[2]->mUseCurve != 0)
        size *= pPart->pTemplate->mProperties[2]->Evaluate(
            m_fNormalizedTime);
    else
        size *= pPart->sizeScale;

    nlVector3 position = pPart->position;
    nlVector3 a;
    nlVector3 b;
    if (pCoordSys != 0)
    {
        nlVector3 transformed;
        nlMultPosVectorMatrix(transformed, position, *pCoordSys);
        position = transformed;
    }
#ifdef NP_NATIVE
    // MSC_FX_ATTACH: draw side only. The emitter was placed from the simulated position; the object it is attached to is drawn from the blended snapshot. The newest particles follow the
    // difference (weight 1 at birth, none at NpFxAttachTaper() seconds of age), so a trail starts at the drawn object and joins the old part of it without a step.
    if (NpFxAttachOn() && (m_vDrawOffset.x != 0.0f || m_vDrawOffset.y != 0.0f || m_vDrawOffset.z != 0.0f))
    {
        const float w = 1.0f - pPart->timeElapsed / NpFxAttachTaper();
        if (w > 0.0f)
        {
            position.x += m_vDrawOffset.x * w;
            position.y += m_vDrawOffset.y * w;
            position.z += m_vDrawOffset.z * w;
        }
    }
#endif

    float s2 = 0.5f * size;
    if (pTemplate->m_uModelID != 0xFFFFFFFF)
    {
        pReturn->position[0] = position;
        pReturn->position[1].x = size;
        pReturn->position[1].y = rot;
        return;
    }

    unsigned int animFrame
        = (int)(pPart->FPS * pPart->timeElapsed + pPart->frame);
    animFrame %= pTemplate->m_nFrames;
    TextureFrame* frame
        = &textureFrames[pTemplate->m_nFrames - 1][animFrame];
    float v0;
    float u0;
    float increment;
    u0 = frame->u;
    v0 = frame->v;
    increment = frame->increment;
    if (pPart->flipTexcoords)
    {
        nlVec2Set(pReturn->texcoord[1], u0 + increment, v0);
        nlVec2Set(pReturn->texcoord[0], u0, v0);
        nlVec2Set(pReturn->texcoord[3], u0, v0 + increment);
        nlVec2Set(pReturn->texcoord[2], u0 + increment, v0 + increment);
    }
    else
    {
        nlVec2Set(pReturn->texcoord[0], u0 + increment, v0);
        nlVec2Set(pReturn->texcoord[1], u0, v0);
        nlVec2Set(pReturn->texcoord[2], u0, v0 + increment);
        nlVec2Set(pReturn->texcoord[3], u0 + increment, v0 + increment);
    }

    float sn;
    float cs;
    nlSinCos(&sn, &cs,
        (unsigned short)(((int)(65536.0f * rot)) / 360));
    sn = sn * s2;
    cs = cs * s2;
    a.x = (cs * viewRight.x) + (sn * viewUp.x);
    a.y = (cs * viewRight.y) + (sn * viewUp.y);
    a.z = (cs * viewRight.z) + (sn * viewUp.z);
    b.x = ((-sn) * viewRight.x) + (cs * viewUp.x);
    b.y = ((-sn) * viewRight.y) + (cs * viewUp.y);
    b.z = ((-sn) * viewRight.z) + (cs * viewUp.z);

    pReturn->position[0].x = (position.x + a.x) + b.x;
    pReturn->position[0].y = (position.y + a.y) + b.y;
    pReturn->position[0].z = (position.z + a.z) + b.z;
    pReturn->position[1].x = (position.x - a.x) + b.x;
    pReturn->position[1].y = (position.y - a.y) + b.y;
    pReturn->position[1].z = (position.z - a.z) + b.z;
    pReturn->position[2].x = (position.x - a.x) - b.x;
    pReturn->position[2].y = (position.y - a.y) - b.y;
    pReturn->position[2].z = (position.z - a.z) - b.z;
    pReturn->position[3].x = (position.x + a.x) - b.x;
    pReturn->position[3].y = (position.y + a.y) - b.y;
    pReturn->position[3].z = (position.z + a.z) - b.z;
}

static void RenderLightOnField(GLView* view, const EffectsLight& light)
{
    if (light.m_fRadius == 0.0f)
        return;
    float heightFrac
        = 1.0f - light.m_v3Position.z / light.m_fRadius;
    if (heightFrac <= 0.0f)
        return;
    if (heightFrac > 1.0f)
        heightFrac = 1.0f;

    glSetDefaultState(true);
    glSetCurrentTexture(glGetTexture("global/light_blob"), GLTT_Diffuse);
    glSetRasterState(GLS_AlphaBlend, 3);
    glSetRasterState(GLS_DepthWrite, 0);
    glSetCurrentRasterState(glHandleizeRasterState());

    float dim = 1.4f * ((2.0f * light.m_fRadius) * (heightFrac * heightFrac));
    nlMatrix4 mRot;
    mRot.SetIdentity();
    glQuad3 q;
    q.SetupRotatedRectangle(dim, dim, mRot, false, false);
    q.SetColour(light.m_Colour);
    for (int i = 0; i < 4; ++i)
    {
        nlVec3Add(q.m_pos[i], q.m_pos[i], light.m_v3Position);
        q.m_pos[i].z = 0.03125f;
        q.m_colour[i].c[3] /= 3;
    }
    view->AttachModel(q.GetModel(), 0);
}

void ParticleSystem::ClearParticles()
{
    nlDLListIterator<Particle*> iterator;
    iterator = m_Particles.Begin();
    while (iterator.hasNext())
    {
        Particle* pPart = *iterator;
        m_Particles.Remove(&iterator);
        --m_NumParticles;
        m_pFreeParticles->AddEnd(pPart);
    }
}

#ifdef NP_NATIVE
// NP (Mega Saves fill rate, 23 Oct): the Mega Strike save scene draws ~40 screens of overlapping blended particle quads per frame (Codex's
// render census); the largest additive layer is ~78 big quads at vertex alpha 0..11/255 and alone covers ~28 screens. The Vita GPU is
// fill-bound there. Two render-only changes, no simulation/emission/RNG change (particles are still updated and kept):
//  1. A particle whose colour alpha is 0 fails the alpha test (ref 0) on every pixel: its quad is written collapsed to one point (zero area,
//     no fragments). Lossless, everywhere.
//  2. While the Mega Strike background is up, faint additive particles (alpha <= 12) are thinned: one in four (chosen by pool slot, so
//     stable for the particle's life) is drawn with four times the alpha, the others are collapsed; from alpha 12 to 32 this fades out to the
//     original. Additive light adds up, so the expected brightness is the same; the dominant haze layer (alpha 0..11) costs about a quarter.
// Both renderers of the sprites use it: this file's mesh path and the game's own fast path (main.cpp BuildParticleQuads, the one the game
// installs, ParticleSystem::m_Callback). MSC_FX_THIN=0: never thin (MSC_FX_ZERO=0 as well restores the original submission); =1: always in
// Mega Strikes; =2: stronger (full thinning up to alpha 24, fading out by alpha 48), also what 1 becomes at the smallest Mega Strike picture.
NpFxThin NpFxThinFor(const EffectsTemplate* tmpl, bool whiteTexture)
{
    // -1 (default, since the effects are drawn at a lower resolution, FX): thinning only while the renderer has a Mega Strike at its smallest
    // picture (VgxMegaStep >= 3). It is not free to look at: the alpha test drops the faintest sprites (alpha 1..3), and four times their
    // alpha lets some of them through, so the haze comes out brighter and grainier.
    static const int kThin = [] { const char* e = getenv("MSC_FX_THIN"); return !e || !*e ? -1 : e[0] == '0' ? 0 : e[0] == '2' ? 2 : 1; }();
    static const bool kZero = [] { const char* e = getenv("MSC_FX_ZERO"); return !(e && e[0] == '0'); }();
    const bool smallest = VgxMegaStep && VgxMegaStep() >= 3;
    const int level = kThin < 0 ? (smallest ? 1 : 0) : (kThin == 1 && smallest ? 2 : kThin);
    NpFxThin t;
    t.zero = kZero;
    t.thin = level != 0 && gMegastrikeBackgroundOverlay.mActive && !whiteTexture && tmpl->m_eBlend == EfBlend_Additive;
    t.full = level == 2 ? 24u : 12u;
    t.span = level == 2 ? 24u : 20u;
    t.keep = 4u;
    // MSC_FX_HAZE (np/scenecut.h): under the heavy-stadium profile. Sizes and lives are the template's own constants (a curve reads as 0 and does not qualify).
    const bool cutHaze = NpStadiumCutHas(NP_CUT_HAZE);   // MSC_STADIUM_CUT bit 2: the same thinning, with the wide family, without the whole heavy profile
    if (!t.thin && !whiteTexture && (NpStadiumHeavy() || cutHaze) && NpSceneHazeEvery() > 1
        && (NpSceneHazeTemplate(tmpl->m_eBlend == EfBlend_Additive, tmpl->mProperties[1]->base * tmpl->mProperties[2]->base, tmpl->m_rParticleLife.base, tmpl->m_fFountainLife)
            || ((NpSceneHazeWide() || cutHaze) && NpSceneHazeTemplateWide(tmpl->m_eBlend == EfBlend_Additive, tmpl->mProperties[1]->base * tmpl->mProperties[2]->base, tmpl->m_rParticleLife.base, tmpl->m_fFountainLife))))
    {
        t.thin = true;
        t.full = 255u;
        t.span = 1u;
        t.keep = (unsigned)NpSceneHazeEvery();
        if (cutHaze) NpStadiumCutCount(NP_CUT_HAZE, 1);
    }
    return t;
}

bool NpFxThinParticle(const NpFxThin& t, const Particle* pPart, ParticleReturn* ret)
{
    bool collapse = t.zero && ret->c.c[3] == 0;
    if (!collapse && t.thin && ret->c.c[3] < t.full + t.span)
    {
        // one in four is kept (pool slot hash: stable for the particle's life); between alpha full and full + span the thinning fades out
        // linearly so nothing pops as a particle fades: kept a*(4 - 3s), others a*s, s = (a - full) / span clamped to 0..1; the expected
        // sum stays a (np/scenecut.h NpSceneThinAlpha; t.keep instead of four).
        const uint32_t slot = (uint32_t)((uintptr_t)pPart >> 4) * 2654435761u;
        const unsigned int out = NpSceneThinAlpha(ret->c.c[3], t.full, t.span, t.keep, (slot >> 28) % t.keep == 0);
        ret->c.c[3] = (unsigned char)out;
        collapse = out == 0;
    }
    return collapse;
}

// MSC_FX_TIGHT (np/fxtight.h). A table of what is known about a texture for a number of frames. Its key holds the texture's data address, so a texture loaded again is worked out
// again; when the pool is full everything goes and is worked out again as it is drawn. Drawn as quads in the meantime, which is the same picture. One texture a frame, at most.
namespace {
enum { kTightSlots = 509, kTightPool = 4096 };   // (slots: a prime; pool: polygons, 36 bytes each)
struct TightEntry {
    unsigned long hash;
    const void* data;
    short frames, verts;     // verts 0: nothing known (a format or a size not handled)
    int first;
    bool used, additive;
};
TightEntry sTight[kTightSlots];
NpTightPoly* sTightPool;
int sTightUsed;
int sTightK = -1, sTightDrop, sTightBlack, sTightFloor, sTightStat, sTightGov, sTightGovMin2;
int sTightSolves, sTightLastCount, sTightAb;
unsigned sTightFrame;
struct TightStat { unsigned sprites, cls[4], dropped, collapsed; float quad, tight, govBig; } sTs, sTsSum;

void TightInit()
{
    const char* e = getenv("MSC_FX_TIGHT");
    sTightK = e && *e ? atoi(e) : 0;
    if (sTightK != 0 && sTightK != 6 && sTightK != 8) sTightK = sTightK ? 4 : 0;
    e = getenv("MSC_FX_TIGHT_DROP");
    sTightDrop = e && *e ? atoi(e) != 0 : 0;
    e = getenv("MSC_FX_TIGHT_BLACK");
    sTightBlack = e && *e ? atoi(e) != 0 : 0;
    e = getenv("MSC_FX_TIGHT_FLOOR");
    sTightFloor = e && *e ? atoi(e) : 0;
    e = getenv("MSC_FX_TIGHT_AB");
    sTightAb = e && *e ? atoi(e) : 0;
    e = getenv("MSC_FX_TIGHT_STAT");
    sTightStat = e && *e ? atoi(e) : 0;
    e = getenv("MSC_FX_GOV");
    sTightGov = e && *e ? atoi(e) != 0 : 0;
    e = getenv("MSC_FX_GOV_MINPX");
    const int minPx = e && *e ? atoi(e) : 48;
    sTightGovMin2 = minPx * minPx;
    if (sTightK || sTightDrop) {
        sTightPool = (NpTightPoly*)malloc(kTightPool * sizeof(NpTightPoly));
        if (!sTightPool) sTightK = sTightDrop = 0;
    }
}

const TightEntry* TightLookup(const EffectsTemplate* tmpl, unsigned long textureIndex, bool additive)
{
    if (textureIndex == 0xFFFFu) return 0;
    unsigned long key = textureIndex;
    PlatTexture* tex = (PlatTexture*)gTextureManager->GetTextureAtIndex(&key);
    if (!tex || tex->m_bMissingTexture || !tex->m_SwizzledData) return 0;
    const int frames = tmpl->m_nFrames;
    if (frames < 1 || frames > 36 || !textureFrames[frames - 1]) return 0;
    const bool black = (sTightBlack || sTightFloor > 0) && additive;   // (a floor on alpha x brightness only applies to additive sprites: they get their own entry)
    const unsigned long h = tmpl->m_hTexture ^ ((unsigned long)frames << 24) ^ (black ? 0x5A5A5Au : 0u);
    unsigned i = (unsigned)(h % kTightSlots);
    for (int n = 0; n < kTightSlots; ++n, i = i + 1 == kTightSlots ? 0 : i + 1) {
        TightEntry& t = sTight[i];
        if (!t.used) break;
        if (t.hash == tmpl->m_hTexture && t.data == tex->m_SwizzledData && t.frames == frames && t.additive == black) return &t;
    }
    if (sTightSolves > 0) return 0;   // (the next frame)
    if (sTightUsed + frames * kNpTightClasses > kTightPool) {
        memset(sTight, 0, sizeof sTight);
        sTightUsed = 0;
        i = (unsigned)(h % kTightSlots);
    }
    while (sTight[i].used) i = i + 1 == kTightSlots ? 0 : i + 1;
    ++sTightSolves;
    NpTightRect rects[36];
    const TextureFrame* fr = textureFrames[frames - 1];
    for (int f = 0; f < frames; ++f) {   // (the writer's own rounding of ParticleSystem::UpdateParticle's corners)
        rects[f].u0 = (short)(fr[f].u * 1024.0f);
        rects[f].v0 = (short)(fr[f].v * 1024.0f);
        rects[f].u1 = (short)((fr[f].u + fr[f].increment) * 1024.0f);
        rects[f].v1 = (short)((fr[f].v + fr[f].increment) * 1024.0f);
    }
    const int k = sTightK ? sTightK : 4;
    NpTightPoly* polys = sTightPool + sTightUsed;
    TightEntry& t = sTight[i];
    t.hash = tmpl->m_hTexture;
    t.data = tex->m_SwizzledData;
    t.frames = (short)frames;
    t.additive = black;
    t.used = true;
    t.first = sTightUsed;
    t.verts = 0;
    if (NpTightSolve((int)tex->m_Format, tex->m_Width, tex->m_Height, tex->m_Levels, (const uint8_t*)tex->m_SwizzledData, rects, frames, k, sTightBlack && additive, polys, additive ? sTightFloor : 0)) {
        int most = 3;
        for (int p = 0; p < frames * kNpTightClasses; ++p)
            if (polys[p].n != kNpTightQuad && polys[p].n > most) most = polys[p].n;
        t.verts = (short)(most <= 4 ? 4 : 3 * (most - 2));
        sTightUsed += frames * kNpTightClasses;
    }
    return &t;
}
}

// MSC_FX_OFFVIEW (gx25, from the Galaxy port's GXV_HOTPROF_PTCLCULL): a sprite whose four corners all lie beyond the same side plane of the view's clip volume
// (clip x > w or < -w, clip y > w or < -w, with a margin of 2 % of |w| and a little more) cannot put a fragment anywhere: a convex quad entirely in one half-space of the clip
// volume is entirely outside it, whatever the signs of w (the test is on the homogeneous coordinates, no divide). Only the four side planes are tested (not near, far).
// The system's own particles are all still updated and counted (UpdateParticle, the thinning, the governor's census, NpFxTightParticle) in the same order as before; only the
// write of the sprite's vertices is left out, so nothing a later picture reads changes. Matrices: the view is stored for row vectors (translation in m41..m43), the projection for
// column vectors (np: glViewProjectPoint transposes it), so clip_r = sum_i p_i * (sum_c P[r][c] * V[i][c]) with p = (x, y, z, 1).
// 0 off (code default), 1 on, 2 count only, 3 count and compare the matrices with glViewProjectPoint (a mismatch is counted and printed).
namespace {
struct OffViewStats { unsigned long sprites, off, collapsed, kept, verified, bad, frames, mesh, meshAll; } sOv;
int sOvMode = -1;
int OvMode()
{
    if (sOvMode < 0) { const char* e = getenv("MSC_FX_OFFVIEW"); sOvMode = e && *e ? atoi(e) : 0; if (sOvMode < 0 || sOvMode > 3) sOvMode = 0; }
    return sOvMode;
}
void OffViewSetup(NpFxTight* t, ParticleSystem* source, GLView* view)
{
    t->offview = 0;
    t->offView = 0;
    const int mode = OvMode();
    if (!mode || !view || !view->m_Interface) return;
    EffectsTemplate* tmpl = source->m_pTemplate;
    if (sUseWhiteParticleTexture || tmpl->m_uModelID != 0xFFFFFFFF) return;   // (sprite meshes only: the model and rain forms draw elsewhere)
    nlMatrix4 P, V, Mm, MV;
    view->m_Interface->GetProjectionMatrix(P);
    view->m_Interface->GetViewMatrix(V);
    glGetMatrix(glGetCurrentMatrix(), Mm);   // the mesh's own matrix (glCreateModel takes the current one; glxSend draws with matrix * view)
    nlMultMatrices(MV, Mm, V);
    V = MV;
    memcpy(t->model, Mm.e, sizeof t->model);
    for (int r = 0; r < 3; ++r) {
        const int pr = r == 2 ? 3 : r;   // clip x, clip y, clip w
        for (int i = 0; i < 4; ++i)
            t->clip[r][i] = P.e[pr * 4 + 0] * V.e[i * 4 + 0] + P.e[pr * 4 + 1] * V.e[i * 4 + 1] + P.e[pr * 4 + 2] * V.e[i * 4 + 2] + P.e[pr * 4 + 3] * V.e[i * 4 + 3];
    }
    t->offview = mode;
    t->offView = view;
}
}

bool NpFxOffView(const NpFxTight& t, const ParticleReturn& ret)
{
    // int29: fix all-4-corners frustum test (bit k stays set only while EVERY corner seen so far lies beyond plane k)
    // and exit on corner 0 for visible on-screen sprites (4x fewer clip-matrix multiplies on Core 0).
    unsigned allOut = 0xFu;   // bit 0: +x, bit 1: -x, bit 2: +y, bit 3: -y
    for (int k = 0; k < 4; ++k) {
        const nlVector3& p = ret.position[k];
        const float cx = t.clip[0][0] * p.x + t.clip[0][1] * p.y + t.clip[0][2] * p.z + t.clip[0][3];
        const float cy = t.clip[1][0] * p.x + t.clip[1][1] * p.y + t.clip[1][2] * p.z + t.clip[1][3];
        const float cw = t.clip[2][0] * p.x + t.clip[2][1] * p.y + t.clip[2][2] * p.z + t.clip[2][3];
        const float m = 0.02f * fabsf(cw) + 0.001f * (fabsf(cx) + fabsf(cy)) + 1e-4f;
        if (!(cx > cw + m)) allOut &= ~1u;
        if (!(cx < -cw - m)) allOut &= ~2u;
        if (!(cy > cw + m)) allOut &= ~4u;
        if (!(cy < -cw - m)) allOut &= ~8u;
        if (!allOut) return false;
    }
    return true;
}

bool NpFxOffViewDrop(const NpFxTight& t, const ParticleReturn& ret, bool collapse)
{
    if (!t.offview) return false;
    ++sOv.sprites;
    if (collapse) { ++sOv.collapsed; return t.offview == 1; }
    const bool off = NpFxOffView(t, ret);
    if (t.offview == 3) {   // the matrices against the game's own projection of the first corner
        nlVector3 ndc, pw;
        nlMatrix4 mm;
        memcpy(mm.e, t.model, sizeof mm.e);
        nlMultPosVectorMatrix(pw, ret.position[0], mm);   // (the game's own projection takes a world point; this one is of the mesh's own space)
        glViewProjectPoint(const_cast<GLView*>(t.offView), pw, ndc);
        ndc.y = -ndc.y;   // (the game's NDC y is up the other way: only |y| matters here)
        const nlVector3& p = ret.position[0];
        const float cx = t.clip[0][0] * p.x + t.clip[0][1] * p.y + t.clip[0][2] * p.z + t.clip[0][3];
        const float cy = t.clip[1][0] * p.x + t.clip[1][1] * p.y + t.clip[1][2] * p.z + t.clip[1][3];
        const float cw = t.clip[2][0] * p.x + t.clip[2][1] * p.y + t.clip[2][2] * p.z + t.clip[2][3];
        ++sOv.verified;
        if (cw != 0.0f && fabsf(cx / cw) < 4.0f && fabsf(cy / cw) < 4.0f && (fabsf(cx / cw - ndc.x) > 1e-3f || fabsf(cy / cw - ndc.y) > 1e-3f)) {
            if (++sOv.bad <= 5) fprintf(stderr, "[fxoffview] MISMATCH mine (%.4f, %.4f) game (%.4f, %.4f)\n", cx / cw, cy / cw, ndc.x, ndc.y);
        }
    }
    if (off) ++sOv.off; else ++sOv.kept;
    return off && t.offview == 1;
}

void NpFxTrimPacket(glModelPacket* packet, int vertices)
{
    packet->numUniqueVertices = vertices;
}

void NpFxOffViewStat(void)
{
    if (!OvMode()) return;
    ++sOv.frames;
    if (sOv.frames % 600 != 0) return;
    static unsigned long long lastPic;
    const unsigned long long pic = NpStadiumCutFrameNo();
    const double n = pic > lastPic ? (double)(pic - lastPic) : 1.0;
    lastPic = pic;
    fprintf(stderr, "[fxoffview] mode %d, %.0f pictures to frame %llu, a picture: %.1f sprites, %.1f collapsed (zero alpha), %.1f wholly off the view, %.1f kept; checks %lu, mismatches %lu\n", OvMode(), n, pic,
        sOv.sprites / n, sOv.collapsed / n, sOv.off / n, sOv.kept / n, sOv.verified, sOv.bad);
    sOv.sprites = sOv.collapsed = sOv.off = sOv.kept = 0;
}

void NpFxTightFor(NpFxTight* t, ParticleSystem* source, GLView* view)
{
    if (sTightK < 0) TightInit();
    memset(t, 0, sizeof *t);
    t->verts = 4;
    t->mark = true;
    t->frames = 1;
    OffViewSetup(t, source, view);
    if (!sTightK && !sTightDrop && !sTightGov && !sTightStat) return;   // (all off: as ever, and nothing spent on it)
    t->active = true;
    if (gNumRenderedParticles <= sTightLastCount) {   // (EmissionManager clears it every frame: a new frame)
        sTightSolves = 0;
        ++sTightFrame;
        if (sTightAb > 0 && sTightFrame % (unsigned)sTightAb == 0) fprintf(stderr, "[fxtight] tick %u: polygons %s\n", sTightFrame, (sTightFrame / (unsigned)sTightAb) & 1u ? "off" : "on");
        if (sTightStat > 0) {
            sTsSum.sprites += sTs.sprites; sTsSum.dropped += sTs.dropped; sTsSum.collapsed += sTs.collapsed;
            sTsSum.quad += sTs.quad; sTsSum.tight += sTs.tight;
            for (int c = 0; c < 4; ++c) sTsSum.cls[c] += sTs.cls[c];
            if (sTightFrame % (unsigned)sTightStat == 0) {
                const float n = (float)sTightStat;
                fprintf(stderr, "[fxtight] over %d frames: %.1f sprites a frame (class 0: %u, 1: %u, 2: %u, quads: %u; faint %u, empty %u); billboards %.2f screens a frame as quads, %.2f drawn, %.1f %% saved (estimate: 640x448, no depth test)\n",
                    sTightStat, (float)sTsSum.sprites / n, sTsSum.cls[0], sTsSum.cls[1], sTsSum.cls[2], sTsSum.cls[3], sTsSum.dropped, sTsSum.collapsed,
                    sTsSum.quad / n, sTsSum.tight / n, sTsSum.quad > 0.0f ? 100.0f * (1.0f - sTsSum.tight / sTsSum.quad) : 0.0f);
                memset(&sTsSum, 0, sizeof sTsSum);
            }
        }
        if (VgxFxGovFill) VgxFxGovFill(sTs.govBig);
        memset(&sTs, 0, sizeof sTs);
    }
    sTightLastCount = gNumRenderedParticles;
    t->gov = VgxFxGovLevel ? VgxFxGovLevel() : 0;
    EffectsTemplate* tmpl = source->m_pTemplate;
    if (sUseWhiteParticleTexture || tmpl->m_uModelID != 0xFFFFFFFF) return;
    t->additive = tmpl->m_eBlend == EfBlend_Additive;
    nlMatrix4 proj;
    bool perspective = false;
    int div = 1;
    if (tmpl->m_eBillboard == EfBill_Billboard && view && view->m_Interface) {
        view->m_Interface->GetProjectionMatrix(proj);
        perspective = proj.e[11] != 0.0f && proj.e[15] == 0.0f;
    }
    if (perspective) {
        nlMatrix4 v;
        view->m_Interface->GetViewMatrix(v);
        t->vz[0] = v.e[2];
        t->vz[1] = v.e[6];
        t->vz[2] = v.e[10];
        t->vz[3] = v.e[14];
        // The picture's size now (half a level of margin in NpTightClass takes the next rung down, which the renderer may step to before this frame is drawn). An additive
        // sprite goes to the effects pass only if it is marked. While the pass is in use additive sprites stay quads, marked, as ever: the pass has few pixels to save and they
        // would reach deeper levels. While it is not, they are sized for the picture and left unmarked (t->mark), so the frame on which the renderer starts the pass draws
        // them at full size, which is what they were made for, and the pass takes them from the next frame.
        div = t->additive && VgxFxPassDiv ? VgxFxPassDiv() : 1;
        float scale = VgxFxPixelScale ? VgxFxPixelScale() : 0.5f;
        scale = scale < 0.25f ? 0.25f : scale > 1.0f ? 1.0f : scale;
        const float k = proj.e[5] * 224.0f * scale;
        t->k2 = k * k;
        t->px2 = 4.0f * proj.e[5] * 112.0f * proj.e[5] * 112.0f;
        t->classify = true;
    }
    if (!sTightK && !sTightDrop) return;
    const TightEntry* e = TightLookup(tmpl, source->m_uTextureIndex, t->additive);
    if (!e || !e->verts) return;
    unsigned long key = source->m_uTextureIndex;
    PlatTexture* tex = (PlatTexture*)gTextureManager->GetTextureAtIndex(&key);
    const int side = tex->m_Width > tex->m_Height ? tex->m_Width : tex->m_Height;
    const float texels = (float)side * textureFrames[e->frames - 1]->increment;
    t->tex2 = texels * texels;
    t->polys = sTightPool + e->first;
    t->frames = e->frames;
    t->drop = sTightDrop != 0;
    t->tight = sTightK != 0 && t->classify && div <= 1;
    if (sTightAb > 0 && (sTightFrame / (unsigned)sTightAb) & 1u) t->tight = false;   // (MSC_FX_TIGHT_AB: on a still scene, the same picture both ways or not)
    t->verts = t->tight ? e->verts : 4;
    t->mark = !t->tight || div > 1;
}

const NpTightPoly* NpFxTightParticle(const NpFxTight& t, const Particle* pPart, ParticleReturn* ret, bool* collapse)
{
    *collapse = false;
    if (!t.active) return 0;
    // MSC_CPU24_TIGHTSTAT=1: sTs.quad / sTs.tight are read only by the MSC_FX_TIGHT_STAT report (sTightStat > 0); sTs.govBig (the governor's input) and the counters stay
    const bool stat24 = NpCpu24Mode(NP_C24_TIGHTSTAT) != 1 || sTightStat > 0;
    float edge2 = 0.0f, z = 0.0f;
    if (t.classify) {
        const nlVector3& p0 = ret->position[0], & p1 = ret->position[1], & p3 = ret->position[3];
        const float ax = p1.x - p0.x, ay = p1.y - p0.y, az = p1.z - p0.z, bx = p3.x - p0.x, by = p3.y - p0.y, bz = p3.z - p0.z;
        const float a2 = ax * ax + ay * ay + az * az, b2 = bx * bx + by * by + bz * bz;
        edge2 = a2 < b2 ? a2 : b2;
        const float cx = 0.5f * (ret->position[1].x + p3.x), cy = 0.5f * (ret->position[1].y + p3.y), cz = 0.5f * (ret->position[1].z + p3.z);
        z = t.vz[0] * cx + t.vz[1] * cy + t.vz[2] * cz + t.vz[3];
        if (z < 0.0f) z = -z;
    }
    float px2 = t.classify && z > 0.0f ? edge2 * t.px2 / (z * z) : 0.0f;
    if (px2 > 640.0f * 448.0f) px2 = 640.0f * 448.0f;   // (the census: a sprite covers one screen at most; the class has its own numbers)
    if (px2 >= (float)sTightGovMin2) sTs.govBig += px2 / (640.0f * 448.0f);
    // the governor (renderer: vgx_fxgov.h): large sprites only, by pool slot (no random numbers: render only, and stable for a sprite's life)
    if (t.gov > 0 && px2 >= (float)sTightGovMin2 && ret->c.c[3] != 0) {
        const uint32_t slot = (uint32_t)((uintptr_t)pPart >> 4) * 2654435761u;
        const unsigned out = (ret->c.c[3] * NpSceneGovWeight(slot >> 24, (unsigned)t.gov) + 128u) >> 8;
        ret->c.c[3] = (unsigned char)(out > 255u ? 255u : out);
        if (ret->c.c[3] == 0) { *collapse = true; return 0; }
    }
    ++sTs.sprites;
    if (!t.polys) { sTs.cls[3] += 1; if (stat24) { sTs.quad += px2 / (640.0f * 448.0f); sTs.tight += px2 / (640.0f * 448.0f); } return 0; }
    const NpTightPoly* first = t.polys;
    if (t.drop) {
        const unsigned vc = ret->c.c[0] > ret->c.c[1] ? (ret->c.c[0] > ret->c.c[2] ? ret->c.c[0] : ret->c.c[2]) : (ret->c.c[1] > ret->c.c[2] ? ret->c.c[1] : ret->c.c[2]);
        if (NpTightInvisible(first->peak, first->peakA, ret->c.c[3], vc, t.additive)) {
            ++sTs.dropped;
            if (stat24) sTs.quad += px2 / (640.0f * 448.0f);
            *collapse = true;
            return 0;
        }
    }
    const int cls = t.tight ? NpTightClass(edge2, z, t.k2, t.tex2) : -1;
    if (stat24) sTs.quad += px2 / (640.0f * 448.0f);
    if (cls < 0) { sTs.cls[3] += 1; if (stat24) sTs.tight += px2 / (640.0f * 448.0f); return 0; }
    sTs.cls[cls] += 1;
    unsigned int f = (int)(pPart->FPS * pPart->timeElapsed + pPart->frame);
    f %= (unsigned)t.frames;
    const NpTightPoly* p = t.polys + f * kNpTightClasses + cls;
    if (p->n == kNpTightQuad) { if (stat24) sTs.tight += px2 / (640.0f * 448.0f); return 0; }
    if (p->n == 0) { ++sTs.collapsed; *collapse = true; return 0; }
    if (sTightStat > 0) {
        long a2 = 0;
        for (int i = 0; i < p->n; ++i) {
            const int j = i + 1 == p->n ? 0 : i + 1;
            a2 += (long)p->uv[i][0] * p->uv[j][1] - (long)p->uv[j][0] * p->uv[i][1];
        }
        const float span = (float)t.frames >= 2.0f ? (float)(textureFrames[t.frames - 1]->increment * 1024.0f) : 1024.0f;
        sTs.tight += px2 / (640.0f * 448.0f) * (float)a2 * 0.5f / (span * span);
    }
    return p;
}
#endif

#ifdef NP_NATIVE
// MSC_STADIUM_CUT_LOG=4 (diagnostic): one line per sprite system template, at first sight and at each doubling of its particle count.
static void NpFxWho(const ParticleSystem* s, int path)
{
    {   // MSC_STADIUM_CUT_LOG=5: per texture, systems drawn and particles, averaged over the frames 1200..2100 (printed at frame 2100)
        static const bool on5 = getenv("MSC_STADIUM_CUT_LOG") && getenv("MSC_STADIUM_CUT_LOG")[0] == '5';
        if (on5 && s->m_pTemplate)
        {
            static struct { unsigned tex; int blend; long sys, part; unsigned tmpl[6]; long tc[6]; } acc[64];
            static int na; static bool done;
            static unsigned long long lo = 1200, hi = 2100;
            static bool once; if (!once) { once = true; const char* w = getenv("MSC_STADIUM_CUT_WIN"); if (w) sscanf(w, "%llu,%llu", &lo, &hi); }
            const unsigned long long f = NpStadiumCutFrameNo();
            if (f >= lo && f < hi)
            {
                int i = 0;
                for (; i < na; ++i) if (acc[i].tex == (unsigned)s->m_pTemplate->m_hTexture && acc[i].blend == (int)s->m_pTemplate->m_eBlend) break;
                if (i == na && na < 64) { acc[na].tex = (unsigned)s->m_pTemplate->m_hTexture; acc[na].blend = (int)s->m_pTemplate->m_eBlend; ++na; }
                if (i < 64) {
                    acc[i].sys++; acc[i].part += s->m_NumParticles;
                    int k = 0; for (; k < 6; ++k) if (acc[i].tmpl[k] == (unsigned)s->m_pTemplate->m_uHashID || acc[i].tmpl[k] == 0) break;
                    if (k < 6) { acc[i].tmpl[k] = (unsigned)s->m_pTemplate->m_uHashID; acc[i].tc[k] += s->m_NumParticles; }
                }
            }
            else if (f >= hi && !done)
            {
                done = true;
                for (int i = 0; i < na; ++i)
                {
                    fprintf(stderr, "[fxtex] tex 0x%08x blend %d systems/frame %.1f particles/frame %.1f |", acc[i].tex, acc[i].blend, acc[i].sys / (double)(hi - lo) * 2.5, acc[i].part / (double)(hi - lo) * 2.5);
                    for (int k = 0; k < 6 && acc[i].tmpl[k]; ++k) fprintf(stderr, " 0x%08x:%.1f", acc[i].tmpl[k], acc[i].tc[k] / (double)(hi - lo) * 2.5);
                    fprintf(stderr, "\n");
                }
            }
        }
    }
    static const bool on = getenv("MSC_STADIUM_CUT_LOG") && getenv("MSC_STADIUM_CUT_LOG")[0] == '4';
    if (!on || !s->m_pTemplate) return;
    static struct { unsigned h; int path, max; } seen[256];
    static int n;
    const EffectsTemplate* t = s->m_pTemplate;
    int i = 0;
    for (; i < n; ++i) if (seen[i].h == (unsigned)t->m_uHashID && seen[i].path == path) break;
    if (i == n) { if (n == 256) return; seen[n].h = (unsigned)t->m_uHashID; seen[n].path = path; seen[n].max = 0; ++n; }
    if (s->m_NumParticles <= 0 || s->m_NumParticles < 2 * seen[i].max) return;
    seen[i].max = s->m_NumParticles;
    unsigned long key = s->m_uTextureIndex;
    PlatTexture* tex = s->m_uTextureIndex == 0xFFFFu ? 0 : (PlatTexture*)gTextureManager->GetTextureAtIndex(&key);
    fprintf(stderr, "[fxwho] tmpl 0x%08x blend %d tex 0x%08x %dx%d texidx %lu layer %lu res 0x%08lx particles %d life %.2f size %.1f frames %d bill %d cb %d light %d\n", (unsigned)t->m_uHashID, (int)t->m_eBlend, (unsigned)t->m_hTexture,
        tex ? tex->m_Width : -1, tex ? tex->m_Height : -1, s->m_uTextureIndex, s->m_uLayer, s->m_uResourceID, s->m_NumParticles, t->m_rParticleLife.base, t->mProperties[1]->base * t->mProperties[2]->base, (int)t->m_nFrames, (int)t->m_eBillboard, ParticleSystem::m_Callback != 0, s->m_pSpec ? (int)s->m_pSpec->m_bLight : -1);
}
#endif

int ParticleSystem::RenderAllParticles(GLView* view)
{
    static int _tris[6] = { 0, 1, 2, 0, 2, 3 };
    ParticleReturn ret;

    if (!m_bVisible)
        return 0;
    if (!sRenderParticles)
        return 0;

    int numParticles = m_NumParticles;
    if (numParticles == 0)
        return 0;

    const nlMatrix4* pCoord = &m_mCoordSys;
    EmissionManager::RecordRenderedParticles(m_uResourceID, numParticles);
    if ((unsigned int)gNumRenderedParticles
        > (unsigned int)MaxNumParticles)
        return 0;
    gNumRenderedParticles += numParticles;
#ifdef NP_NATIVE
    NpFxWho(this, 1);   // MSC_STADIUM_CUT_LOG=4
#endif

    nlVector3 viewRight;
    nlVector3 viewUp;
    nlVector3 viewForward;
    int cullBackFaces = true;
    if (m_pTemplate->m_eBillboard == EfBill_Billboard)
    {
        nlMatrix4 viewMatrix;
        view->m_Interface->GetViewMatrix(viewMatrix);
        viewMatrix.GetColumn_(0, viewRight);
        viewMatrix.GetColumn_(1, viewUp);
        viewMatrix.GetColumn_(2, viewForward);
        nlVec3Scale(viewRight, m_fAspect);
    }
    else if (m_pTemplate->m_eBillboard == EfBill_Groundboard)
    {
        nlVec3Set(viewRight, 1.0f, 0.0f, 0.0f);
        nlVec3Set(viewUp, 0.0f, 1.0f, 0.0f);
    }
    else if (m_pTemplate->m_eBillboard == EfBill_SoftwareControlled)
    {
        viewUp.x = 0.0f;
        viewUp.y = 0.0f;
        viewUp.z = 1.0f;
        viewRight.x = 1.0f;
        viewRight.y = 0.0f;
        viewRight.z = 0.0f;
        cullBackFaces = false;
        nlMatrix4 rot;
        nlMakeRotationMatrixZ(rot,
            0.0000958738f * (float)(unsigned short)(m_aFacing + 0x4000));
        nlMultDirVectorMatrix(viewRight, rot);
    }

    glSetDefaultState(true);
    glSetRasterState(GLS_DepthWrite, 0);
    glSetRasterState(GLS_Culling, cullBackFaces ? 1 : 0);
    if (sDisableParticleDepthTest)
        glSetRasterState(GLS_DepthTest, 0);
    if (m_AllowInFront
        && (m_pTemplate->IsInFront()
            || (m_pSpec != 0 && m_pSpec->m_bInFront)))
    {
        glSetRasterState(GLS_DepthTest, 0);
    }
    if (sUseWhiteParticleTexture)
    {
        glSetRasterState(GLS_AlphaBlend, 3);
    }
    else
    {
        switch (m_pTemplate->m_eBlend)
        {
        case EfBlend_Normal:
            glSetRasterState(GLS_AlphaBlend, 1);
            break;
        case EfBlend_Additive:
            glSetRasterState(GLS_AlphaBlend, 3);
            break;
        }
    }
    glSetRasterState(GLS_AlphaTest, 1);
    glSetCurrentRasterState(glHandleizeRasterState());

    static unsigned long WhiteTexture = glGetTexture("global/white");
    if (m_pSpec != 0 && m_pSpec->m_bLight)
    {
        pCoord = m_pTemplate->IsLocalSpace() ? pCoord : 0;
        nlDLListIterator<Particle*> iterator;
        iterator = m_Particles.Begin();
        while (iterator.hasNext())
        {
            Particle* pPart = *iterator;
            EffectsLight light;
            UpdateLight(&light, pPart, m_pTemplate, viewRight, viewUp,
                pCoord);
            EmissionManager::Instance()->AddEffectsLight(light);
            RenderLightOnField(view, light);
            iterator.Step();
        }
    }
    else if (m_pTemplate->m_uModelID != 0xFFFFFFFF)
    {
        glModel* pModel;
        GLVertexAnim* pAnim = gEffectsModelInventory->GetVertexAnim(
            m_pTemplate->m_uModelID);
        eEffectsBlend blendType;

        nlMatrix4 m;
        nlMatrix4 mScale;
        nlMatrix4 mRot;
        nlMatrix4 mCoord;
        mCoord = *pCoord;
        mCoord.e[2] = -mCoord.e[2];
        mCoord.e[6] = -mCoord.e[6];
        mCoord.e[10] = -mCoord.e[10];

        switch (m_pTemplate->m_eBlend)
        {
        case EfBlend_Normal:
            blendType = EfBlend_Additive;
            break;
        case EfBlend_Additive:
            blendType = (eEffectsBlend)3;
            break;
        }

        pCoord = m_pTemplate->IsLocalSpace() ? pCoord : 0;
#ifdef NP_NATIVE
        const bool dumpRain = NpStadiumCutHas(NP_CUT_DUMPFX) && (unsigned)m_pTemplate->m_uHashID == 0x8270565bu;   // fx_dump_rain
        const bool dumpRain3 = !dumpRain && NpStadiumCutHas(NP_CUT_RAIN3) && (unsigned)m_pTemplate->m_uHashID == 0x8270565bu;   // MSC_STADIUM_CUT bit 8192: the gentler form, one streak in three left out, the others at 1.5 x alpha
#endif
#ifdef NP_NATIVE
        // MSC_RAIN_HULL: the rain model's hull models (built once per model, when its texture is there) and the view's numbers for each drop's class (as NpFxTightFor)
        bool rainHull = false;
        NpModelOv ov; bool ovOn = false, ovCull = false; int pendFi = -1;   // MSC_FX_MODEL_OFFVIEW; pendFi: the frame of the last GetModel left out (see GLVertexAnim::PatchSharedStreams)
        bool rainFastOn = false, cHas = false; unsigned cOff = 0; u32 fastRaster = 0;   // MSC_FX_RAIN_FAST
        static unsigned long constantColourHashR = nlStringLowerHash("constantcolour");
        float rainVx[3] = {0, 0, 0}, rainVy[3] = {0, 0, 0}, rainVz[4] = {0, 0, 0, 0}, rainK = 0.0f, rainK1 = 0.0f;
        if (sRainMode < 0) RainInit();
        if (sRainFast < 0) RainInit2();
        RainHull* rh = RainSlotFor((unsigned)m_pTemplate->m_uHashID);
        if (sRainLog >= 2 && pAnim != 0) {   // census of model particles: one line a template (and a stadium load: the animation's address)
            static unsigned seen[16][2]; static int nseen;
            bool known = false;
            for (int i = 0; i < nseen; ++i) if (seen[i][0] == (unsigned)m_pTemplate->m_uHashID && seen[i][1] == (unsigned)(uintptr_t)pAnim) known = true;
            if (!known && nseen < 16) {
                seen[nseen][0] = (unsigned)m_pTemplate->m_uHashID; seen[nseen][1] = (unsigned)(uintptr_t)pAnim; ++nseen;
                PlatTexture* t = pAnim->m_pModel && pAnim->m_pModel->numPackets >= 1 ? RainTexture(pAnim->m_pModel->packets[0]) : 0;
                fprintf(stderr, "[rainhull] model particles: template %#x, %d frames, %u packets, texture %s %dx%d format %d, %d particles in this system, hull table %s\n", (unsigned)m_pTemplate->m_uHashID, (int)pAnim->m_nNumFrames,
                        pAnim->m_pModel ? (unsigned)pAnim->m_pModel->numPackets : 0u, t ? "found" : "not ready", t ? (int)t->m_Width : 0, t ? (int)t->m_Height : 0, t ? (int)t->m_Format : -1, (int)m_NumParticles, rh ? "yes" : "no");
            }
        }
        if (rh && pAnim != 0) {
            if (RainOn() && rh->built && rh->ok && rh->src == pAnim && pAnim->m_pModel && pAnim->m_pModel->numPackets >= 1) {
                // the binding is resolved again every picture: a texture that was reloaded (another PlatTexture or other texel data) makes the hulls stale, they are rebuilt
                PlatTexture* t = RainTexture(pAnim->m_pModel->packets[0]);
                if (t != rh->tex || (t && t->m_SwizzledData != rh->texData)) rh->built = false;
            }
            if (RainOn() && view && view->m_Interface && (!rh->built || rh->src != pAnim)) {
                RainBuild(*rh, pAnim);   // (a texture that was not there yet is asked for again at the next draw)
            }
            if (RainOn() && rh->ok && rh->src == pAnim && view && view->m_Interface) {
                nlMatrix4 proj;
                view->m_Interface->GetProjectionMatrix(proj);
                if (proj.e[11] != 0.0f && proj.e[15] == 0.0f) {
                    nlMatrix4 v;
                    view->m_Interface->GetViewMatrix(v);
                    rainVx[0] = v.e[0]; rainVx[1] = v.e[4]; rainVx[2] = v.e[8];
                    rainVy[0] = v.e[1]; rainVy[1] = v.e[5]; rainVy[2] = v.e[9];
                    rainVz[0] = v.e[2]; rainVz[1] = v.e[6]; rainVz[2] = v.e[10]; rainVz[3] = v.e[14];
                    float scale = VgxFxPixelScale ? VgxFxPixelScale() : 0.5f;   // the picture's size against the game's own, at most 1: the smallest target the drop can be drawn to
                    scale = scale < 0.25f ? 0.25f : scale > 1.0f ? 1.0f : scale;
                    rainK = proj.e[5] * 224.0f * scale;
                    rainK1 = proj.e[5] * 224.0f;
                    rainHull = true;
                    if (sModelOv > 0 && rh->radius > 0.0f && rh->radius < 1e6f) {   // (a NaN radius fails both)
                        float cf = 0.0f;
                        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) cf += mCoord.e2[i][j] * mCoord.e2[i][j];
                        ovOn = NpModelOvSetup(&ov, proj.e, v.e, sqrtf(cf), rh->radius);
                        if (ovOn && sModelOvBreak) ov.r0 = 0.0f;   // test mutant (the picture proof must catch it): the sphere is a point
                        ovCull = sModelOv == 1;
                    }
                    if (sRainFast > 0 && pAnim->m_pModel && pAnim->m_pModel->numPackets == 1) {   // the drop's copy of the source model: one packet, its colour slot and raster state hoisted
                        const glModelPacket* base = pAnim->m_pModel->packets;
                        cHas = glHasMaterialParameter(base, constantColourHashR);
                        if (cHas) cOff = (unsigned)((const unsigned char*)glGetMaterialParameterData(base, constantColourHashR) - (const unsigned char*)base->materialParameters);
                        fastRaster = base->rasterState;
                        glSetRasterState(fastRaster, GLS_Culling, 0);
                        glSetRasterState(fastRaster, GLS_AlphaBlend, blendType);
                        glSetRasterState(fastRaster, GLS_AlphaTest, 1);
                        glSetRasterState(fastRaster, GLS_AlphaTestRef, 3);
                        if (m_pTemplate->DisablesDepthWrite())
                            glSetRasterState(fastRaster, GLS_DepthWrite, 0);
                        rainFastOn = true;
                    }
                }
            }
        }
#endif
        nlDLListIterator<Particle*> iterator;
        iterator = m_Particles.Begin();
        while (iterator.hasNext())
        {
            Particle* pPart = *iterator;
#ifdef NP_NATIVE
            if ((dumpRain && (((uint32_t)((uintptr_t)pPart >> 4) * 2654435761u) >> 28) % 2u == 1u) || (dumpRain3 && (((uint32_t)((uintptr_t)pPart >> 4) * 2654435761u) >> 24) % 3u == 2u))   // MSC_STADIUM_CUT bit 64 DUMPFX: every second rain streak is not built (the others get twice the alpha below)
            {
                NpStadiumCutCount(NP_CUT_DUMPFX, 1);
                iterator.Step();
                continue;
            }
#endif
#ifdef NP_NATIVE
            if (pAnim == 0)
#else
            if (pAnim == 0)
#endif
            {
                pModel = glModelDupNoStreams(
                    gEffectsModelInventory->GetModel(m_pTemplate->m_uModelID),
                    false,
                    0);
            }
            UpdateParticle(&ret, pPart, m_pTemplate, viewRight, viewUp, pCoord);
#ifdef NP_NATIVE
            if (ovOn) {   // MSC_FX_MODEL_OFFVIEW: the drop is still updated (above), only its matrix, copy and attach are left out
                ++sMov.drops;
                if (NpModelOvOff(ov, ret.position[0].x, ret.position[0].y, ret.position[0].z, ret.position[1].x)) {
                    ++sMov.culled;
                    if (ovCull) {
                        pendFi = RainFrameOf(pPart, pAnim, (m_pTemplate->m_uFlags & 8) != 0);
                        iterator.Step();
                        continue;
                    }
                }
            }
            if (dumpRain)
                ret.c.c[3] = (unsigned char)(ret.c.c[3] > 127 ? 255 : ret.c.c[3] * 2);
            else if (dumpRain3)
                ret.c.c[3] = (unsigned char)(ret.c.c[3] * 3u > 510u ? 255 : ret.c.c[3] * 3u / 2u);
#endif
            float rotRad = 3.1415927f * ret.position[1].y / 180.0f;
            float size = ret.position[1].x;
            if (m_pTemplate->m_eBillboard == EfBill_Billboard)
            {
                float facingRot = 0.0000958738f
                    * (float)(unsigned short)(m_aFacing + 0x8000);
                rotRad += facingRot;
            }
            nlMakeRotationMatrixZ(mRot, rotRad);
            nlMakeScaleMatrix(mScale, size, size, size);
            nlMultMatrices(mScale, mRot);
            nlMultMatrices(m, mCoord, mScale);
            m.e[12] = ret.position[0].x;
            m.e[13] = ret.position[0].y;
            m.e[14] = ret.position[0].z;

            u32 hMatrix = glAllocSetMatrix(m);

            float meshRateScale = 1.0f;
#ifdef NP_NATIVE
            int rainFi = -1;
            bool fastDone = false;
#endif
            if (pAnim != 0)
            {
                if ((m_pTemplate->m_uFlags & 8) != 0)
                {
                    meshRateScale = ((float)(int)pAnim->m_nNumFrames
                        / pAnim->m_fFrameRate) / pPart->lifeSpan;
                    float frameFrac = pPart->timeElapsed / pPart->lifeSpan;
                    float frame = frameFrac * (float)((int)pAnim->m_nNumFrames - 1);
#ifdef NP_NATIVE
                    rainFi = rainHull && (int)frame < rh->frames ? (int)frame : -1;
                    if (rainFi >= 0) pModel = 0; else { pModel = pAnim->GetModel((int)frame); pendFi = -1; }
#else
                    pModel = pAnim->GetModel((int)frame);
#endif
                }
                else
                {
                    meshRateScale = pPart->FPS / pAnim->m_fFrameRate;
                    float frame = pPart->FPS * pPart->timeElapsed;
                    float numFrames = (float)(int)pAnim->m_nNumFrames;
                    while (frame >= numFrames)
                    {
                        frame -= numFrames;
                    }
#ifdef NP_NATIVE
                    rainFi = rainHull && (int)frame < rh->frames ? (int)frame : -1;
                    if (rainFi >= 0) pModel = 0; else { pModel = pAnim->GetModel((int)frame); pendFi = -1; }
#else
                    pModel = pAnim->GetModel((int)frame);
#endif
                }
            }

#ifdef NP_NATIVE
            if (rainHull && rainFi >= 0)
            {
                // the class of this drop: the deepest mip level its size on the screen can reach (np/fxtight.h NpTightClass, the sprites' own numbers); -1: the whole quad
                int cls = -1;
                const float cx = m.e[12], cy = m.e[13], cz = m.e[14];
                float z = rainVz[0] * cx + rainVz[1] * cy + rainVz[2] * cz + rainVz[3];
                if (z < 0.0f) z = -z;
                if (z > 0.0f) {
                    // the model's x axis (u) and z axis (v) in the world, 0.75 long each (m holds the scale), seen from the camera: their lengths across the picture
                    const float ux = 0.75f * m.e[0], uy = 0.75f * m.e[1], uz = 0.75f * m.e[2], wx = 0.75f * m.e[8], wy = 0.75f * m.e[9], wz = 0.75f * m.e[10];
                    const float ua = rainVx[0] * ux + rainVx[1] * uy + rainVx[2] * uz, ub = rainVy[0] * ux + rainVy[1] * uy + rainVy[2] * uz;
                    const float va = rainVx[0] * wx + rainVx[1] * wy + rainVx[2] * wz, vb = rainVy[0] * wx + rainVy[1] * wy + rainVy[2] * wz;
                    const float eu2 = ua * ua + ub * ub, ev2 = va * va + vb * vb;
                    const float k2 = rainK * rainK;
                    // The mip level a pixel reads comes from the SMALLEST singular value of the texel -> screen map, not from the two edge lengths: a drop seen at a slant has long edges that lie almost along
                    // each other, and the sampler then minifies hard across them (the first form, NpTightClass per edge, gave class 0 there and the hull cut 1-9 levels off a few dozen pixels a picture
                    // in the 640x448 test of rainhull19). m = [ua / texU, va / texV; ub / texU, vb / texV] (screen units per texel); sigma_min^2 = D^2 / sigma_max^2, sigma_max^2 = (T + sqrt(T^2 - 4 D^2)) / 2, T = |m|^2, D = det m.
                    const float tu = rh->texU[rainFi], tv = rh->texV;
                    const float m00 = ua / tu, m01 = va / tv, m10 = ub / tu, m11 = vb / tv;
                    const float T = m00 * m00 + m01 * m01 + m10 * m10 + m11 * m11, D = m00 * m11 - m01 * m10;
                    const float disc = T * T - 4.0f * D * D;
                    const float smax2 = 0.5f * (T + (disc > 0.0f ? sqrtf(disc) : 0.0f)), sig2 = smax2 > 0.0f ? D * D / smax2 : 0.0f;
                    cls = NpTightClass(sig2, z, k2, 1.0f);
                    if (cls > sRainMaxClass) cls = -1;
                    if (cls >= 0 && !rh->cls[cls][rainFi]) {   // (no hull of this class: the next coarser one covers it, a finer class's hull never does)
                        int c2 = cls + 1;
                        while (c2 <= sRainMaxClass && !rh->cls[c2][rainFi]) ++c2;
                        cls = c2 <= sRainMaxClass ? c2 : -1;
                    }
                    if (sRainMode > 1) {
                        const float k1 = rainK1;
                        sRs.quadPx += (double)(eu2 > 0 ? sqrtf(eu2) : 0) * (double)(ev2 > 0 ? sqrtf(ev2) : 0) * (double)(k1 * k1) / ((double)z * (double)z);
                        sRs.drawnPx += (double)(eu2 > 0 ? sqrtf(eu2) : 0) * (double)(ev2 > 0 ? sqrtf(ev2) : 0) * (double)(k1 * k1) / ((double)z * (double)z) * (cls >= 0 ? (double)rh->frac[cls][rainFi] : 1.0);
                    }
                }
                if (sRainMode > 1) { ++sRs.drops; ++sRs.cls[cls < 0 ? 3 : cls]; }
                // a fresh copy of the source frame model for every drop (its binding, colour slot and raster state are this picture's own, as the game's own draw), then only the
                // geometry swapped for the hull's: nothing from an earlier picture is ever handed to the draw
                if (cls >= 0 && rainFastOn)
                {
                    // MSC_FX_RAIN_FAST: the same copy without GetModel's stream patching (the hull's streams replace them just below; the shared source streams' last patch is made once after the loop,
                    // pendFi), the colour written at its hoisted offset and the raster state as one word (the same four or five field sets applied once to the same source word)
                    pModel = glModelDupNoStreams(pAnim->m_pModel, false, 0);
                    const glModelPacket* hg = (const glModelPacket*)rh->cls[cls][rainFi];
                    glModelPacket* dp = pModel->packets;
                    dp->indexBuffer = hg->indexBuffer;
                    dp->streams = hg->streams;
                    dp->numStreams = hg->numStreams;
                    dp->numVertices = hg->numVertices;
                    dp->numUniqueVertices = hg->numUniqueVertices;
                    dp->primType = hg->primType;
                    dp->displayList = 0;
                    if (cHas)
                    {
                        nlVector4 colour;
                        colour.x = (float)ret.c.c[0] * (1.0f / 255.0f);
                        colour.y = (float)ret.c.c[1] * (1.0f / 255.0f);
                        colour.z = (float)ret.c.c[2] * (1.0f / 255.0f);
                        colour.w = (float)ret.c.c[3] * (1.0f / 255.0f);
                        memcpy((unsigned char*)dp->materialParameters + cOff, &colour, sizeof colour);
                    }
                    dp->rasterState = fastRaster;
                    dp->matrix = hMatrix;   // (glModelSetMatrix below would only store this into the one packet again)
                    pendFi = rainFi;
                    fastDone = true;
                }
                else
                {
                pModel = pAnim->GetModel(rainFi);
                pendFi = -1;
                if (cls >= 0 && pModel && pModel->numPackets == 1) {
                    const glModelPacket* hg = (const glModelPacket*)rh->cls[cls][rainFi];
                    glModelPacket* dp = pModel->packets;
                    dp->indexBuffer = hg->indexBuffer;
                    dp->streams = hg->streams;
                    dp->numStreams = hg->numStreams;
                    dp->numVertices = hg->numVertices;
                    dp->numUniqueVertices = hg->numUniqueVertices;
                    dp->primType = hg->primType;
                    dp->displayList = 0;
                }
                }
            }
#endif
            static unsigned long constantColourHash
                = nlStringLowerHash("constantcolour");
#ifdef NP_NATIVE
            if (!fastDone)
#endif
            {
            glModelPacket* pPacket = pModel->packets;
            while (pPacket < pModel->packets + pModel->numPackets)
            {
                if (glHasMaterialParameter(pPacket, constantColourHash))
                {
                    nlVector4 colour;
                    colour.x = (float)ret.c.c[0] * (1.0f / 255.0f);
                    colour.y = (float)ret.c.c[1] * (1.0f / 255.0f);
                    colour.z = (float)ret.c.c[2] * (1.0f / 255.0f);
                    colour.w = (float)ret.c.c[3] * (1.0f / 255.0f);
                    glSetMaterialParameterArray(pPacket, constantColourHash,
                        &colour, 4);
                }
                glSetRasterState(pPacket->rasterState, GLS_Culling, 0);
                glSetRasterState(pPacket->rasterState, GLS_AlphaBlend,
                    blendType);
                glSetRasterState(pPacket->rasterState, GLS_AlphaTest, 1);
                glSetRasterState(pPacket->rasterState, GLS_AlphaTestRef, 3);
                if (m_pTemplate->DisablesDepthWrite())
                    glSetRasterState(
                        pPacket->rasterState, GLS_DepthWrite, 0);
                pPacket->matrix = hMatrix;
                ++pPacket;
            }
            glModelSetMatrix(pModel, hMatrix);
            }
            view->AttachModel(pModel, m_uLayer + 1);
            iterator.Step();
        }
#ifdef NP_NATIVE
        if (pendFi >= 0) pAnim->PatchSharedStreams(pendFi);   // the last GetModel that was left out: the source streams end this call as they always did
        if (ovOn && ++sMov.calls % 1200u == 0u && sMov.drops > 0) {
            fprintf(stderr, "[fxmodelov] mode %d: over %lu draws %.1f drops a draw, %.1f wholly off the view%s\n", sModelOv, sMov.calls, (double)sMov.drops / 1200.0, (double)sMov.culled / 1200.0, ovCull ? " (not drawn)" : " (counted only)");
            sMov.drops = sMov.culled = 0;
        }
#endif
#ifdef NP_NATIVE
        if (rainHull && sRainMode > 1 && ++sRs.calls % 300u == 0u && sRs.drops > 0) {
            const double pic = 640.0 * 448.0;   // estimate: the drops' projected area at the game's own size (448 px high), no depth test, no overlap
            fprintf(stderr, "[rainhull] over %u draws: %.1f drops a draw (class 0: %u, 1: %u, 2: %u, whole quad: %u); the drops' quads %.2f screens a draw, %.2f drawn as hulls (estimate, 640x448)\n",
                    sRs.calls, (double)sRs.drops / 300.0, sRs.cls[0], sRs.cls[1], sRs.cls[2], sRs.cls[3], sRs.quadPx / pic / 300.0, sRs.drawnPx / pic / 300.0);
            sRs.drops = 0; sRs.cls[0] = sRs.cls[1] = sRs.cls[2] = sRs.cls[3] = 0; sRs.quadPx = sRs.drawnPx = 0;
        }
#endif
    }
    else if (m_Callback == 0)
    {
        bool bQuads = glHasQuads();
        GLTexturedColourMeshWriter mesh;
        bool began;
        if (bQuads)
        {
            began = mesh.Begin(m_NumParticles * 4, GLP_QuadList, 0);
        }
        else
        {
            began = mesh.Begin(m_NumParticles * 6, GLP_TriList, 0);
        }
        if (began)
        {
            pCoord = m_pTemplate->IsLocalSpace() ? pCoord : 0;
#ifdef NP_NATIVE
            // NP (Mega Saves fill rate, 23 Oct): the Mega Strike save scene draws ~40 screens of overlapping blended
            // particle quads per frame (Codex's render census); the largest additive layer is ~78 big quads at vertex
            // alpha 0..11/255 and alone covers ~28 screens. The Vita GPU is fill-bound there (~15 fps, 66 ms queued per
            // frame). Two render-only changes, no simulation/emission/RNG change (particles are still updated and kept):
            //  1. A particle whose colour alpha is 0 fails the alpha test (ref 0) on every pixel: its quad is written
            //     collapsed to one point (zero area, no fragments). Lossless, everywhere.
            //  2. While the Mega Strike background is up, faint additive particles (alpha <= 12) are thinned: one in four
            //     (chosen by pool slot, so stable for the particle's life) is drawn with four times the alpha, the others
            //     are collapsed; from alpha 12 to 32 this fades out to the original. Additive light adds up, so the
            //     expected brightness is the same; the dominant haze layer (alpha 0..11) costs about a quarter of the fill.
            // MSC_FX_THIN=0 turns 2 off, MSC_FX_THIN=0 together with MSC_FX_ZERO=0 restores the original submission.
            // MSC_FX_THIN=2: stronger, for console A/B tests: full thinning up to alpha 24, fading out by alpha 48.
            const NpFxThin thin = NpFxThinFor(m_pTemplate, sUseWhiteParticleTexture);
#endif
            nlDLListIterator<Particle*> iterator;
            iterator = m_Particles.Begin();
            while (iterator.hasNext())
            {
                Particle* pPart = *iterator;
                UpdateParticle(&ret, pPart, m_pTemplate, viewRight,
                    viewUp, pCoord);
                int i;
#ifdef NP_NATIVE
                {
                    const bool collapse = NpFxThinParticle(thin, pPart, &ret);
                    if (collapse)
                    {
                        const int n = bQuads ? 4 : 6;
                        for (i = 0; i < n; i++)
                        {
                            mesh.Texcoord(ret.texcoord[0]);
                            mesh.Colour(ret.c);
                            mesh.Vertex(ret.position[0]);
                        }
                        iterator.Step();
                        continue;
                    }
                }
#endif
                if (bQuads)
                {
                    for (i = 0; i < 4; i++)
                    {
                        mesh.Texcoord(ret.texcoord[i]);
                        mesh.Colour(ret.c);
                        mesh.Vertex(ret.position[i]);
                    }
                }
                else
                {
                    for (i = 0; i < 6; i++)
                    {
                        mesh.Texcoord(ret.texcoord[_tris[i]]);
                        mesh.Colour(ret.c);
                        mesh.Vertex(ret.position[_tris[i]]);
                    }
                }
                iterator.Step();
            }

            if (sUseWhiteParticleTexture)
            {
                glTextureBinding* textureState
                    = (glTextureBinding*)mesh.GetModel()
                        ->packets->materialParameters;
                textureState->texture = WhiteTexture;
                textureState->textureIndex = 0xFFFF;
                textureState->SetWrapS(false);
                textureState->SetWrapT(false);
                textureState->unknown07 = 0;
            }
            else
            {
                glTextureBinding* textureState
                    = (glTextureBinding*)mesh.GetModel()
                        ->packets->materialParameters;
                textureState->textureIndex = m_uTextureIndex;
                textureState->SetWrapS(false);
                textureState->SetWrapT(false);
                textureState->unknown07 = 0;
            }

            if (mesh.End())
            {
#ifdef NP_NATIVE
                // (Vita port, FX: np/fx.h) an additive sprite mesh may be drawn at a lower resolution by the renderer and added to the
                // scene in one pass (additive light adds up in any order): the packet is marked for glxSend
                if (sUseWhiteParticleTexture || m_pTemplate->m_eBlend == EfBlend_Additive)
                    NpFxMarkPacket(mesh.GetModel()->packets);
#endif
                view->AttachModel(mesh.GetModel(), m_uLayer);
            }
            else
                tDebugPrintManager::Print(DC_RENDER,
                    "couldn't end mesh built by sprites\n");
        }
        else
        {
            tDebugPrintManager::Print(DC_RENDER,
                "could not begin a mesh for sprites\n");
        }
    }
    else
    {
        if (!m_Callback(this, view, &m_Particles, viewRight, viewUp,
                m_pTemplate->IsLocalSpace() ? pCoord : 0))
        {
            tDebugPrintManager::Print(DC_RENDER,
                "too many particles for the fast-path\n");
        }
    }

    return numParticles;
}

void ParticleSystem::Die()
{
    m_fDelay = 0.0f;
    m_fElapsedTime = 100000000000000000000.0f;
    m_bAmDying = true;
}

bool ParticleSystem::Update(float dt)
{
    if (m_fDelay > 0.0f)
    {
        m_fDelay -= dt;
        if (m_fDelay < 0.0f)
            m_fDelay = 0.0f;
        return true;
    }

    m_fDeltaTime = dt;
    m_fElapsedTime += dt;
    if (m_pSpec != 0 && m_pSpec->m_fLingerEnd >= 0.0f
        && !m_bAmDying && m_fElapsedTime > m_pSpec->m_fLingerEnd)
    {
        m_fElapsedTime = m_pSpec->m_fLingerStart;
    }

    if (m_pTemplate->m_fFountainLife <= 0.0f)
    {
        if (!m_bAmDying && m_Particles.m_Head == 0)
        {
            if (m_pSpec == 0 || m_pSpec->m_fLingerStart < 0.0f)
                m_bAmDying = true;
            m_fNumParticlesToCreate
                += m_pTemplate->mProperties[0]->Evaluate(0.0f);
        }
    }
    else
    {
        if (m_fElapsedTime >= m_pTemplate->m_fFountainLife)
        {
            m_bAmDying = true;
        }
        m_fNormalizedTime
            = m_fElapsedTime / m_pTemplate->m_fFountainLife;
        if (m_fNormalizedTime > 1.0f)
            m_fNormalizedTime = 1.0f;
        if (m_fElapsedTime < m_pTemplate->m_fFountainLife)
        {
            m_fNumParticlesToCreate += dt
                * m_pTemplate->mProperties[0]->Evaluate(
                    m_fNormalizedTime);
        }
    }

    int numParticles = (int)m_fNumParticlesToCreate;
    m_fNumParticlesToCreate -= (float)numParticles;
    if (m_fNumParticlesToCreate < 0.0f)
        m_fNumParticlesToCreate = 0.0f;
    if (numParticles > 0)
        CreateNewParticles(numParticles);

    UpdateAllParticles(dt,
        m_pTemplate->IsLocalSpace() ? &m_mCoordSys : 0);
    if (m_bAmDying && m_Particles.m_Head == 0)
        return false;
    return true;
}

float ParticleSystem::GetRemainingTime() const
{
    return m_pTemplate->m_fFountainLife - m_fElapsedTime;
}

static TextureFrame* BuildFrameLookup(int numFrames, float inc)
{
    TextureFrame* p = (TextureFrame*)nlMalloc(
        numFrames * sizeof(TextureFrame), 8, false);
    TextureFrame* q = p;
    float u = 0.0f;
    float v = 0.0f;
    for (int i = 0; i < numFrames; ++i, ++q)
    {
        q->u = u;
        q->v = v;
        q->increment = inc;
        u += inc;
        if (u >= 0.999f)
        {
            u = 0.0f;
            v += inc;
        }
    }
    return p;
}

bool fxParticleStartup(int maxNumParticles)
{
    textureFrames[0]
        = (TextureFrame*)nlMalloc(sizeof(TextureFrame), 8, false);
    textureFrames[0]->u = 0.0f;
    textureFrames[0]->v = 0.0f;
    textureFrames[0]->increment = 1.0f;
    textureFrames[3] = BuildFrameLookup(4, 0.5f);
    textureFrames[8] = BuildFrameLookup(9, 1.0f / 3.0f);
    textureFrames[15] = BuildFrameLookup(16, 0.25f);
    textureFrames[24] = BuildFrameLookup(25, 0.2f);
    textureFrames[35] = BuildFrameLookup(36, 1.0f / 6.0f);
    if (MaxNumParticles == 0)
        MaxNumParticles = maxNumParticles;
    return true;
}

bool fxParticleShutdown()
{
    for (int i = 0; i < 36; ++i)
    {
        if (textureFrames[i] != 0)
        {
            delete[] (unsigned char*)textureFrames[i];
            textureFrames[i] = 0;
        }
    }
    return true;
}

void fxSetMaxNumParticles(int maxNumParticles)
{
    MaxNumParticles = maxNumParticles;
}
