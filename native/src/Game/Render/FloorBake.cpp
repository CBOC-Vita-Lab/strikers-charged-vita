// MSC_FLOOR_BAKE (np/floorbake.h): the reflection of the two reflective floor materials, averaged into the diffuse texture at model load. Called from their Prepare, once per packet of a
// loading model, so before anything has drawn with the texture. It changes pictures only: nothing the simulation reads comes from these textures (the lighting lookup that does is another
// texture), and peers may differ in it.
#ifdef NP_NATIVE
#include <revolution/gx.h>
#endif
#include "np/floorbake.h"

#ifdef NP_NATIVE
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>

#include "Game/GameInfo.h"
#include "Game/GL/GLTextureAnim.h"
#include "NL/gl/glPlat.h"
#include "NL/glx/GXMaterialProgramInternal.h"
#include "NL/glx/glxDisplayList.h"
#include "NL/glx/glxGX.h"
#include "NL/nlMath.h"
#include "NL/gl/glModel.h"
#include "NL/glx/glxTexture.h"

static int sPackets, sTextures, sSkipped, sVertex, sLookup;

// MSC_FLOOR_SHEEN: what a baked packet carries. The index goes in the binding of its mask, which a baked packet never binds. A load's records live until the next load has finished.
enum { kSheenMax = 256 };
struct SheenSlot
{
    NpFloorSheen* sheen;
    int generation;
};
static SheenSlot sSheen[kSheenMax];
static int sSheenNext, sGeneration;
struct ReflectionTable
{
    unsigned long texture;
    uint8_t grid[NP_FB_GRID * NP_FB_GRID * 3];
    unsigned gw, gh;
    int mean[3];
    int generation;
    ReflectionTable* next;
};
static ReflectionTable* sTables;

static int FloorSheenWanted()
{
    static int v = -1;
    if (v < 0) v = NpFloorSheenParse(getenv("MSC_FLOOR_SHEEN"));
    return v;
}

static const ReflectionTable* TableOf(unsigned long handle, const PlatTexture* o)
{
    for (ReflectionTable* t = sTables; t; t = t->next)
        if (t->texture == handle && t->generation == sGeneration) return t;
    ReflectionTable* t = (ReflectionTable*)malloc(sizeof(ReflectionTable));
    uint32_t* sums = (uint32_t*)malloc(NP_FB_GRID * NP_FB_GRID * 16);
    if (t && sums && NpFbReflectionGrid(o->m_Format, (const uint8_t*)o->m_SwizzledData, o->m_Width, o->m_Height, t->grid, sums, &t->gw, &t->gh, t->mean))
    {
        free(sums);
        t->texture = handle;
        t->generation = sGeneration;
        t->next = sTables;
        sTables = t;
        return t;
    }
    free(sums);
    free(t);
    return 0;
}

static float ReadFloat(const uint8_t* p, int hostOrder)
{
    uint32_t v = hostOrder ? ((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24)) : (((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

static void FreeSheen(NpFloorSheen* t)
{
    free(t->xy);
    free(t->weight);
    free(t->colours);
    free(t);
}

// The record of a packet just baked. The mask's coordinates are in hand; the positions are the texture generation's input, the object's own, as the hardware reads them.
static NpFloorSheen* MakeSheen(const glModelPacket* packet, float scale, float scroll, unsigned amount, const ReflectionTable* table, const int16_t* maskUv, const uint8_t* maskGrid, unsigned mgw, unsigned mgh)
{
    const glModelStream& position = packet->streams[0];
    const unsigned n = packet->numUniqueVertices;
    if (position.address == 0 || position.stride != 12 || packet->indexBuffer == 0 || n == 0) return 0;
    NpFloorSheen* t = (NpFloorSheen*)malloc(sizeof(NpFloorSheen));
    if (t == 0) return 0;
    memset(t, 0, sizeof(*t));
    t->xy = (float*)malloc((size_t)n * 8u);
    t->weight = (uint8_t*)malloc((size_t)n * 3u);
    t->colours = (uint32_t*)malloc((size_t)n * 4u);
    if (!t->xy || !t->weight || !t->colours)
    {
        FreeSheen(t);
        return 0;
    }
    t->count = n;
    t->scale = scale;
    t->scroll = scroll;
    t->grid = table->grid;
    t->gw = table->gw;
    t->gh = table->gh;
    memcpy(t->mean, table->mean, sizeof(t->mean));
    const uint8_t* p = (const uint8_t*)position.address;
    const int r = (int)(mgw / 32u);
    unsigned total[3] = {0, 0, 0};
    for (unsigned i = 0; i < n; ++i, p += 12)
    {
        t->xy[i * 2u] = ReadFloat(p, position.unknown07 & 1);
        t->xy[i * 2u + 1u] = ReadFloat(p + 4, position.unknown07 & 1);
        unsigned m[3];
        NpFbMaskAround(maskGrid, mgw, mgh, (float)maskUv[i * 2u] / 1024.0f, (float)maskUv[i * 2u + 1u] / 1024.0f, r, m);
        for (int k = 0; k < 3; ++k)
        {
            const unsigned w = m[k] * amount / 255u;
            t->weight[i * 3u + (unsigned)k] = (uint8_t)w;
            total[k] += w;
        }
    }
    float longest = 0.0f;
    const u16* index = packet->indexBuffer;
    for (unsigned i = 0; i + 2u < packet->numVertices; ++i)
    {
        const unsigned a = index[i], b = index[i + 1u], c = index[i + 2u];
        if (a == b || b == c || a == c || a >= n || b >= n || c >= n) continue;
        const unsigned e[3][2] = {{a, b}, {b, c}, {c, a}};
        for (int k = 0; k < 3; ++k)
        {
            const float dx = t->xy[e[k][0] * 2u] - t->xy[e[k][1] * 2u], dy = t->xy[e[k][0] * 2u + 1u] - t->xy[e[k][1] * 2u + 1u];
            const float d = dx * dx + dy * dy;
            if (d > longest) longest = d;
        }
    }
    t->mode = NpFloorSheenMode(FloorSheenWanted(), sqrtf(longest), scale);
    for (int k = 0; k < 3; ++k)
    {
        const unsigned v = 2u * total[k] / n;
        t->key[k] = (uint8_t)(v > 255u ? 255u : v);
    }
    t->key[3] = 0xFF;
    t->lookupData = (uint8_t*)(((uintptr_t)t->lookup + 31u) & ~(uintptr_t)31u);
    if (t->mode == NP_FB_SHEEN_LOOKUP)
    {
        NpFbLookupTexture(t->grid, t->gw, t->gh, t->mean, t->lookupData);
        GXInitTexObj((GXTexObj*)t->lookupObject, t->lookupData, NP_FB_LUT, NP_FB_LUT, GX_TF_RGBA8, GX_REPEAT, GX_REPEAT, GX_FALSE);
        GXInitTexObjLOD((GXTexObj*)t->lookupObject, GX_LINEAR, GX_LINEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
        DCStoreRange(t->lookupData, NP_FB_LUT_BYTES);
    }
    return t;
}

NpFloorSheen* NpFloorSheenOf(const glTextureBinding* maskBinding)
{
    const unsigned long i = maskBinding->texture;
    return i < kSheenMax ? sSheen[i].sheen : 0;
}

void NpFloorTevEnter(int mode, NpFloorSheen*)
{
    gxSetNumTevStages(mode == NP_FB_BAKED ? 1 : 2);
    gxSetNumTexGens(mode == NP_FB_SHEEN_LOOKUP ? 2 : 1);
    gxSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    gxSetTevColourIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_RASC, GX_CC_TEXC, GX_CC_ZERO);
    gxSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
    if (mode == NP_FB_BAKED) return;
    if (mode == NP_FB_SHEEN_VERTEX)
    {
        // prev + COLOR1 - 0.5
        gxSetNumChans(2);
        GXSetChanCtrl(GX_COLOR1A1, GX_FALSE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
        gxSetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR1A1);
        gxSetTevColourIn(GX_TEVSTAGE1, GX_CC_RASC, GX_CC_ZERO, GX_CC_ZERO, GX_CC_CPREV);
    }
    else
    {
        // prev + 0.5 + (lookup - 0.5) * key - 0.5
        gxSetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_POS, GX_TEXMTX1);   // (the end of a shadow stage on coordinate 1 changes it)
        gxSetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD1, GX_TEXMAP1, GX_COLOR_NULL);
        gxSetTevKColourSel(GX_TEVSTAGE1, GX_TEV_KCSEL_K0);
        gxSetTevColourIn(GX_TEVSTAGE1, GX_CC_HALF, GX_CC_TEXC, GX_CC_KONST, GX_CC_CPREV);
    }
    gxSetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
    gxSetTevColourOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_SUBHALF, GX_CS_SCALE_1, true, GX_TEVPREV);
}

void NpFloorTevLeave(int mode)
{
    if (mode == NP_FB_SHEEN_VERTEX) gxSetNumChans(1);
    if (mode >= NP_FB_SHEEN_VERTEX) gxSetTevColourOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, true, GX_TEVPREV);
}

void NpFloorKey(const NpFloorSheen* sheen)
{
    GXLoadTexObj((GXTexObj*)sheen->lookupObject, GX_TEXMAP1);
    GXColor key = {sheen->key[0], sheen->key[1], sheen->key[2], sheen->key[3]};
    GXSetTevKColor(GX_KCOLOR0, key);
}

void NpFloorDrawSheen(const glModelPacket* packet, NpFloorSheen* sheen, int diffuseAttribute)
{
    const nlVector3& camera = glxCameraPosition();
    NpFbSheenColours(sheen, camera.x, camera.y);
    GXClearVtxDesc();
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR1, GX_CLR_RGBA, GX_RGBA8, 0);
    GXSetVtxDesc(GX_VA_POS, GX_INDEX16);
    GXSetVtxDesc(GX_VA_NRM, GX_INDEX16);
    GXSetVtxDesc(GX_VA_CLR0, GX_INDEX16);
    GXSetVtxDesc(GX_VA_CLR1, GX_DIRECT);
    GXSetVtxDesc((GXAttr)diffuseAttribute, GX_INDEX16);
    const unsigned short* index = packet->indexBuffer;
    const unsigned short* const end = index + packet->numVertices;
    GXBegin(glxGetPrimitiveType((unsigned char)packet->primType), GX_VTXFMT0, (unsigned short)packet->numVertices);
    while (index < end)
    {
        const unsigned short i = *index++;
        WGPIPE.us = i;
        WGPIPE.us = i;
        WGPIPE.us = i;
        WGPIPE.ui = sheen->colours[i];
        WGPIPE.us = i;
    }
}

static double sSeconds;

extern bool gSkipGameplayModels;   // (StadiumLoading.cpp: the front end's world, which loads before any game mode is set; GetStadium() would read through a null pointer)

static int FloorBakeWanted()
{
    if (gSkipGameplayModels) return 0;
    return NpFloorBakeHas(getenv("MSC_FLOOR_BAKE"), GameInfoManager::Instance()->GetStadium());
}

int NpFloorBakeOn()
{
    static int on = -1;
    if (on < 0)
    {
        const char* e = getenv("MSC_FLOOR_BAKE");
        on = e && *e ? 1 : 0;
    }
    return on;
}

// A texture a packet can read as plain data: not animated, known, at least a level
static PlatTexture* PlainTexture(unsigned long handle)
{
    if (handle == 0xFFFFFFFFu || glGetTextureAnim(handle) != 0) return 0;
    PlatTexture* t = glx_GetTex(handle);
    return t && t->m_SwizzledData && !t->m_bMissingTexture ? t : 0;
}

// The coordinates of a stream in host order (n pairs). 0: not 4-byte pairs, or no memory.
static int16_t* CopyCoordinates(const glModelStream& s, unsigned n)
{
    if (s.address == 0 || s.stride != 4) return 0;
    int16_t* out = (int16_t*)malloc((size_t)n * 4u);
    if (out == 0) return 0;
    const uint8_t* p = (const uint8_t*)s.address;
    for (unsigned i = 0; i < n * 2u; ++i, p += 2)
        out[i] = (s.unknown07 & 1) ? (int16_t)(p[0] | (p[1] << 8)) : (int16_t)((p[0] << 8) | p[1]);
    return out;
}

static double Now()
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

// The packet's diffuse takes the average of the term, and *overlayAmount becomes 0: the program then draws it with one texture (its Draw).
void NpFloorBakePacket(glModelPacket* packet, const glTextureBinding* diffuse, const glTextureBinding* overlay, glTextureBinding* mask, float overlayScale, float cameraScroll, float* overlayAmount, int diffuseStream, int maskStream)
{
    if (!NpFloorBakeOn() || *overlayAmount == 0.0f || !FloorBakeWanted()) return;
    ++sSkipped;
    if (cameraScroll == 0.0f) return;   // a reflection that does not follow the camera: the animated generators of Storm Ship
    PlatTexture* d = PlainTexture(diffuse->texture);
    PlatTexture* o = PlainTexture(overlay->texture);
    PlatTexture* m = PlainTexture(mask->texture);
    if (!d || !o || !m || d->m_Format != GXTex_CMPR || m->m_Format != GXTex_CMPR || packet->numStreams <= diffuseStream || packet->numStreams <= maskStream) return;
    const unsigned n = packet->numUniqueVertices;
    int16_t* du = CopyCoordinates(packet->streams[diffuseStream], n);
    int16_t* mu = CopyCoordinates(packet->streams[maskStream], n);
    NpFbMap map;
    unsigned mean[3];
    const int fits = du && mu && NpFbFitMap(du, mu, n, 4.0f / 1024.0f, &map);
    free(du);
    if (!fits || !NpFbMeanOverlay(o->m_Format, (const uint8_t*)o->m_SwizzledData, o->m_Width, o->m_Height, mean))
    {
        free(mu);
        return;
    }
    // A mask sharper than the diffuse would lose its shape in blocks of the diffuse
    const float sharp = (map.su < 0.0f ? -map.su : map.su) * (float)m->m_Width;
    if (sharp > (float)d->m_Width * 1.01f)
    {
        free(mu);
        return;
    }
    const double t0 = Now();
    const unsigned gw = (m->m_Width + 3u) / 4u, gh = (m->m_Height + 3u) / 4u;
    uint8_t* grid = (uint8_t*)malloc((size_t)gw * gh * 3u);
    if (grid == 0)
    {
        free(mu);
        return;
    }
    NpFbMaskGrid((const uint8_t*)m->m_SwizzledData, m->m_Width, m->m_Height, grid);
    float a = *overlayAmount * 255.0f + 0.5f;
    const unsigned amount = a > 255.0f ? 255u : (unsigned)a;
    if (!d->m_NpFloorBaked)
    {
        NpFbBakeLevels((uint8_t*)d->m_SwizzledData, d->m_Width, d->m_Height, d->m_Levels, grid, gw, gh, &map, mean, amount);
        d->m_NpFloorBaked = 1;
        ++sTextures;
    }
    unsigned long slot = 0xFFFFFFFFu;
    if (FloorSheenWanted() && sSheenNext < kSheenMax && mu != 0)
    {
        const ReflectionTable* table = TableOf(overlay->texture, o);
        NpFloorSheen* sheen = table ? MakeSheen(packet, overlayScale, cameraScroll, amount, table, mu, grid, gw, gh) : 0;
        if (sheen)
        {
            for (int i = 0; i < kSheenMax; ++i, sSheenNext = (sSheenNext + 1) % kSheenMax)
            {
                if (sSheen[sSheenNext].sheen == 0)
                {
                    sSheen[sSheenNext].sheen = sheen;
                    sSheen[sSheenNext].generation = sGeneration;
                    slot = (unsigned long)sSheenNext;
                    sheen = 0;
                    break;
                }
            }
            if (sheen) FreeSheen(sheen);
            else if (sSheen[slot].sheen->mode == NP_FB_SHEEN_LOOKUP) ++sLookup;
            else ++sVertex;
        }
    }
    mask->texture = slot;
    free(mu);
    free(grid);
    sSeconds += Now() - t0;
    *overlayAmount = 0.0f;
    --sSkipped;
    ++sPackets;
}

void NpFloorBakeReport(int stadium)
{
    if (!NpFloorBakeOn()) return;
    fprintf(stderr, "[floorbake] stadium %d: %d packets to one texture, %d textures changed in %.1f ms, %d packets left with three textures; sheen: %d per vertex, %d by lookup\n", stadium, sPackets, sTextures, sSeconds * 1000.0, sSkipped, sVertex, sLookup);
    sPackets = sTextures = sSkipped = sVertex = sLookup = 0;
    sSeconds = 0.0;
    // The records of the world before this one
    for (int i = 0; i < kSheenMax; ++i)
    {
        if (sSheen[i].sheen && sSheen[i].generation != sGeneration)
        {
            FreeSheen(sSheen[i].sheen);
            sSheen[i].sheen = 0;
        }
    }
    for (ReflectionTable** p = &sTables; *p;)
    {
        ReflectionTable* t = *p;
        if (t->generation != sGeneration)
        {
            *p = t->next;
            free(t);
        }
        else
            p = &t->next;
    }
    ++sGeneration;
}

#endif // NP_NATIVE
