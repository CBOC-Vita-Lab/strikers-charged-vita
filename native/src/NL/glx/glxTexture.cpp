#include "NL/glx/glxTexture.h"
#include "Game/Sys/debug.h"
#include "Game/TweakValue.h"
#include "Game/SharedStaticStorage.h"

#include "NL/gc/gcSwizzler.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glModel.h"
#include "NL/gl/glState.h"
#include "NL/gl/glTexture.h"
#include "NL/gl/glTextureManager.h"
#include "NL/nlFile.h"
#include "NL/nlList.h"
#include "NL/nlMemory.h"
#include "NL/nlPrint.h"

#include <revolution/os/OSCache_fwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#ifdef NP_NATIVE
#include "np/framesend.h"
// NP (W5 CTL): the pictures of Wii buttons drawn as the Vita's (platform/sdk/input/np_prompts.cpp, np_prompt_art.h)
extern "C" const void* NpCtlTextureOverride(unsigned long hash, const void* header) __attribute__((weak));
extern "C" void np_gx_touch_tex(const void* gxTexObj, unsigned int mode) __attribute__((weak));   // platform/gx (np_gx.h): renderer extension record
#endif

enum GXTexWrapMode
{
    GX_CLAMP,
    GX_REPEAT,
    GX_MIRROR,
    GX_MAX_TEXWRAPMODE
};

enum eGLTextureMode
{
    GLTM_WrapWrap = 0,
    GLTM_WrapClamp = 1,
    GLTM_ClampWrap = 2,
    GLTM_ClampClamp = 3,
    GLTM_Num = 4
};

extern "C"
{
    void GXInitTlutObj(
        void* object, void* palette, int format, unsigned short entries);
    void GXInitTexObjCI(void* object, void* image, unsigned short width,
        unsigned short height, int format, int wrapS, int wrapT,
        unsigned char mipmap, unsigned long tlutName);
    void GXInitTexObj(void* object, void* image, unsigned short width,
        unsigned short height, int format, int wrapS, int wrapT,
        unsigned char mipmap);
    void GXInitTexObjLOD(void* object, int minFilter, int magFilter,
        float minLod, float maxLod, float lodBias, unsigned char biasClamp,
        unsigned char edgeLod, int maxAnisotropy);
    void GXInitTexObjWrapMode(
        void* object, GXTexWrapMode wrapS, GXTexWrapMode wrapT);
    void GXInitTexObjTlut(void* object, unsigned long tlutName);
    void GXLoadTlut(void* object, unsigned long tlutName);
    void GXLoadTexObj(void* object, unsigned long textureMap);
    void GXInvalidateTexAll();
}

TweakValueBool gbReduceTextures("gbReduceTextures", "/Rendering/Engine", false);

static glxTextureLoadCallback_t glxTextureLoad_cb;
static unsigned long nGridMemory;
static bool glx_bGridMode;

static nlListContainer<PlatTexture*> gridTextures;
static PlatTexture texobj;

glxTextureLoadCallback_t glx_SetLoadCallback(
    glxTextureLoadCallback_t callback)
{
    glxTextureLoadCallback_t previous = glxTextureLoad_cb;
    glxTextureLoad_cb = callback;
    return previous;
}

static PlatTexture* glx_MakeGridTexture(int width, int height)
{
    printf("Creating grid texture %d x %d\n", width, height);

    unsigned char bits[4] = { 5, 6, 5, 0 };
    PlatTexture* texture = new (nlMalloc(sizeof(PlatTexture), 8, false)) PlatTexture();
    texture->Create(width, height, GXTex_RGB565,
        (MemoryAllocator*)glGetCurrentResourcePool(), 1, true, true);
    memcpy(texture->m_Bits, bits, sizeof(texture->m_Bits));

    unsigned short gridColor = 0xFFFF;
    int x;
    int y;
    unsigned short* data = (unsigned short*)texture->m_LinearData;
    for (y = 0; y < height; ++y)
    {
        for (x = 0; x < width; ++x)
        {
            int gridX = x / 8;
            if ((y / 8) & 1)
            {
                data[y * width + x] = (gridX & 1) == 0 ? 0 : gridColor;
            }
            else
            {
                data[y * width + x] = (gridX & 1) ? 0 : gridColor;
            }
        }
    }

    nGridMemory += height * (width << 1) + sizeof(PlatTexture);
    tDebugPrintManager::Print(DC_GL, "grid [%d %d] now using %uKB\n", width, height, nGridMemory / 1024);

    texture->Swizzle(true);
    texture->Prepare();
    return texture;
}

static PlatTexture* glx_GetGridTexture(int width, int height)
{
    width /= 2;
    height /= 2;

    if ((width & (width - 1)) != 0 || (height & (height - 1)) != 0)
    {
        return 0;
    }
    if (width < 16 || height < 16)
    {
        return 0;
    }

    ListEntry<PlatTexture*>* current = gridTextures.m_Head;
    while (current != 0)
    {
        PlatTexture* texture = current->entry;
        if (width == texture->m_Width && height == texture->m_Height)
        {
            return texture;
        }
        current = current->next;
    }

    PlatTexture* texture = glx_MakeGridTexture(width, height);
    ListEntry<PlatTexture*>* entry = new (nlMalloc(sizeof(ListEntry<PlatTexture*>), 8, false))
        ListEntry<PlatTexture*>(texture);
    nlListAddStart(&gridTextures.m_Head, entry, &gridTextures.m_Tail);
    return gridTextures.m_Head->entry;
}

PlatTexture* glx_MakeTexture(GXTextureHeader* header,
    void* allocator, unsigned long texhandle)
{
    PlatTexture* pTex;
    unsigned char* textureData;
    int width;
    int height;
    eGXTextureFormat format;
    int numLevels;
    unsigned long numEntries;
    int textureSize;

#ifdef NP_NATIVE
    if (NpCtlTextureOverride)
    {
        const void* replacement = NpCtlTextureOverride(texhandle, header);
        if (replacement)
        {
            header = (GXTextureHeader*)replacement;
        }
    }
#endif
    width = header->width;
    height = header->height;
    numLevels = header->numLevels;
    textureData = (unsigned char*)(header + 1);

    if (gbReduceTextures.mValue)
    {
        if (numLevels > 2)
        {
            textureData += GCTextureSize(
                header->format, width, height, 2, -1);
            width /= 4;
            height /= 4;
            numLevels -= 2;
        }
        else if (numLevels > 1)
        {
            textureData += GCTextureSize(
                header->format, width, height, 1, -1);
            width /= 2;
            height /= 2;
            numLevels -= 1;
        }
    }

    textureSize = GCTextureSize(
        header->format, width, height, numLevels, texhandle);
    pTex = new (glResourceAlloc(sizeof(PlatTexture), GLM_Header, allocator)) PlatTexture();

    format = header->format;
    pTex->Create(width, height, format, allocator, numLevels, false, false);
    memcpy(pTex->m_Bits, header->numBits, sizeof(pTex->m_Bits));
    pTex->m_bMissingTexture = header->missingTexture ? true : false;

    numEntries = header->numEntries;
    if (numEntries != 0)
    {
        pTex->m_PaletteData =
            (u16*)glResourceAlloc(numEntries * 2, GLM_TextureData, allocator);
        pTex->m_nPaletteEntries = numEntries;
        memcpy(pTex->m_PaletteData, textureData + textureSize,
            header->numEntries * 2);
    }

    memcpy(pTex->m_SwizzledData, textureData, textureSize);
    pTex->Prepare();
    return pTex;
}

bool glplatBeginLoadTextureBundle(const char* filename,
    void (*callback)(void*, unsigned long, void*), void* param)
{
    return nlLoadEntireFileAsync(filename, callback, param, 32, AllocateEnd, 0, 0, 0)
        != 0;
}

bool glplatLoadTextureBundle(
    const char* filename, void* allocator)
{
    bool result;
    void* data;
    unsigned long size;
    data = nlLoadEntireFile(filename, &size, 32, AllocateStart, 0, 0, 0);
    result = glEndLoadTextureBundle(data, size, allocator, 0);
    delete[] (unsigned char*)data;
    return result;
}

bool glplatTextureLoad(PlatTexture* texture)
{
    if (texture == 0)
    {
        memset(&texobj, 0, sizeof(texobj));
        return false;
    }

    memcpy(&texobj, texture, sizeof(texobj));
    return true;
}

u32 glplatTextureGetWidth()
{
    return texobj.m_Width;
}

u32 glplatTextureGetHeight()
{
    return texobj.m_Height;
}

int glplatTextureGetNumBits(int component)
{
    if (texobj.m_Bits[component] == 0xFF)
    {
        unsigned long format = texobj.m_Format;
        unsigned char bits[4] = { 0, 0, 0, 0 };
        switch (format)
        {
        case GXTex_RGB565:
            bits[0] = 5;
            bits[1] = 6;
            bits[2] = 5;
            bits[3] = 0;
            break;
        case GXTex_RGB5A3:
            bits[0] = 5;
            bits[1] = 5;
            bits[2] = 5;
            bits[3] = 3;
            break;
        case GXTex_CMPR:
            break;
        case GXTex_RGBA8:
            bits[0] = 8;
            bits[1] = 8;
            bits[2] = 8;
            bits[3] = 8;
            break;
        case GXTex_I8:
            bits[0] = 8;
            bits[1] = 0;
            bits[2] = 0;
            bits[3] = 0;
            break;
        case GXTex_I4:
            bits[0] = 4;
            bits[1] = 0;
            bits[2] = 0;
            bits[3] = 0;
            break;
        case GXTex_A8:
            break;
        case GXTex_IA8:
            bits[0] = 8;
            bits[1] = 0;
            bits[2] = 0;
            bits[3] = 8;
            break;
        }
        return bits[component];
    }

    return texobj.m_Bits[component];
}

PlatTexture* glx_CreatePlatTexture(void* allocator)
{
    return new (glResourceAlloc(sizeof(PlatTexture), GLM_Header, allocator)) PlatTexture();
}

void PlatTexture::ClearData()
{
    if (this != 0 && m_LinearData != 0)
    {
        nlFree(m_LinearData);
        m_LinearData = 0;
    }
}

PlatTexture::~PlatTexture()
{
    if (m_LinearData != 0)
    {
        nlFree(m_LinearData);
        m_LinearData = 0;
    }
}

void PlatTexture::CreateWithMemory(int width, int height,
    eGXTextureFormat format, int numLevels, const void* textureData)
{
    m_Width = width;
    m_Height = height;
    m_Levels = numLevels;
    m_MaxLevel = numLevels;
    m_Format = format;
    m_SwizzledData = (void*)textureData;
    m_LinearData = 0;
    m_NpNoTouch = 1;
}

void PlatTexture::Create(int width, int height, eGXTextureFormat format,
    void* allocator, int numLevels, bool linearData,
    bool newResourceMemory)
{
    if (m_LinearData != 0)
    {
        nlFree(m_LinearData);
        m_LinearData = 0;
    }

    m_Width = width;
    m_Height = height;
    m_Levels = numLevels;
    m_MaxLevel = numLevels;
    m_Format = format;

    unsigned long textureSize = GCTextureSize(format, width, height, numLevels, -1);
    if (newResourceMemory)
    {
        m_SwizzledData = nlMalloc(textureSize, 32, false);
    }
    else
    {
        m_SwizzledData = glResourceAlloc(textureSize, GLM_TextureData, allocator);
    }

    if (linearData)
    {
        m_LinearData = nlMalloc(textureSize, 32, false);
    }
    else
    {
        m_LinearData = 0;
    }
}

void PlatTexture::Swizzle(bool deleteLinear)
{
#ifdef NP_NATIVE
    // Textures generated in the game (grid, font, light ramps) hold host-order 16-bit texels; GX reads them big-endian.
    GCSwizzle(m_SwizzledData, m_LinearData, m_Width, m_Height, m_Format, true);
#else
    GCSwizzle(m_SwizzledData, m_LinearData, m_Width, m_Height, m_Format, false);
#endif
    if (deleteLinear)
    {
        nlFree(m_LinearData);
        m_LinearData = 0;
    }
}

static inline int* glx_GetGXFormatTable()
{
    static int gxFormat[9] = { 4, 5, 14, 6, 1, 0, 1, 3, 9 };
    return gxFormat;
}

void PlatTexture::Prepare()
{
    DCStoreRange(m_SwizzledData,
        GCTextureSize(m_Format, m_Width, m_Height, m_Levels, -1));
    if (m_nPaletteEntries > 0)
    {
        DCStoreRange(m_PaletteData, m_nPaletteEntries * 2);
        GXInitTlutObj(
            m_TlutObj, m_PaletteData, 2, m_nPaletteEntries);
    }

    if (m_Format == GXTex_CI8)
    {
        GXInitTexObjCI(m_TexObj, m_SwizzledData, m_Width, m_Height, glx_GetGXFormatTable()[m_Format], 0, 0, m_Levels > 1, 0);
        GXInitTexObjLOD(m_TexObj, m_Levels == 1 ? 1 : 3, 1, 0.0f, (float)(m_MaxLevel - 1), 0.0f, false, false, 0);
        return;
    }

    GXInitTexObj(m_TexObj, m_SwizzledData, m_Width, m_Height, glx_GetGXFormatTable()[m_Format], 0, 0, m_Levels > 1);
    GXInitTexObjLOD(m_TexObj, m_Levels == 1 ? 1 : 5, 1, 0.0f, (float)(m_MaxLevel - 1), 0.0f, false, false, 0);
}

PlatTexture* glplatTextureAddFromBundle(glTexBundleDict* entry,
    GXTextureHeader* header, void* allocator)
{
    return glx_MakeTexture(header, allocator, entry->hash);
}

void glplatBeginTextureBundle(void*, void*)
{
}

#ifdef NP_NATIVE
static void glx_InvalidateTexAllCall(const void*)
{
    GXInvalidateTexAll();
}
#endif

void glplatEndTextureBundle(void*)
{
#ifdef NP_NATIVE
    // NP (two-stage pipeline): a texture bundle loads while the send of the last frame may run; the recorder call waits for its join
    NpFsDefer(glx_InvalidateTexAllCall, 0, 0);
#else
    GXInvalidateTexAll();
#endif
}

PlatTexture* glplatTextureAdd(unsigned long handle,
    const void* textureData, unsigned long, void* allocator)
{
    return glx_MakeTexture((GXTextureHeader*)textureData, allocator, handle);
}

void glplatTextureReplace(PlatTexture* pTex, const void* textureData,
    unsigned long size)
{
    const GXTextureHeader* pHeader = (GXTextureHeader*)textureData;
    const unsigned char* src =
        (const unsigned char*)textureData + sizeof(GXTextureHeader);
    int width = pHeader->width;
    int height = pHeader->height;
    int numLevels = pHeader->numLevels;

    if (gbReduceTextures.mValue)
    {
        if (numLevels > 2)
        {
            src += GCTextureSize(
                pHeader->format, width, height, 2, -1);
        }
        else if (numLevels > 1)
        {
            src += GCTextureSize(
                pHeader->format, width, height, 1, -1);
        }
    }

    memcpy(pTex->m_SwizzledData, src,
        GCTextureSize(pTex->m_Format, pTex->m_Width, pTex->m_Height,
            pTex->m_Levels, -1));

    if (pHeader->numEntries != 0)
    {
        int paletteOffset = GCTextureSize(pTex->m_Format, pTex->m_Width,
            pTex->m_Height, pTex->m_Levels, -1);
        memcpy(pTex->m_PaletteData, src + paletteOffset,
            pHeader->numEntries * 2);
    }

    pTex->Prepare();
}

void glxInitTex()
{
}

void glx_BindTexture(
    int textureMap, glTextureBinding* textureState)
{
    GXTexWrapMode wrapS;
    GXTexWrapMode wrapT;
    glTextureManager* textureManager;
    PlatTexture* pTex;
    eGLTextureMode tmode;
    // 1.3 armcpu9: this was `static unsigned long missingTexture = glGetTexture(...)`, a guarded static: every call (1100+ a picture) loaded the guard
    // byte and followed it with `dmb ish`, the acquire the guard needs (a barrier waits for every store before it; about 30 cycles on the Cortex-A9).
    // The value is now kept in one atomic word, +1 so that 0 means "not worked out yet", read relaxed; the first call still goes through the guarded
    // static (so glGetTexture runs once, at the same first call, on whichever thread gets there first).
    static unsigned long sMissingTexturePlus1 = 0;
    unsigned long missingTexture;
    {
        unsigned long known = __atomic_load_n(&sMissingTexturePlus1, __ATOMIC_RELAXED);
        if (__builtin_expect(known == 0, 0))
        {
            static unsigned long once = glGetTexture("font/fixedWidthMedium");
            known = once + 1;   // (a handle of 0xFFFFFFFF would stay "unknown" and take this path every time: still right)
            __atomic_store_n(&sMissingTexturePlus1, known, __ATOMIC_RELAXED);
        }
        missingTexture = known - 1;
    }

    textureManager = glGetTextureManager();
    pTex = textureManager->GetTexture(textureState);
    if (pTex == 0)
    {
        unsigned long texture =
            textureManager->GetTextureIndex(missingTexture);
        pTex = textureManager->GetTextureAtIndex(&texture);
    }

    if (glx_bGridMode)
    {
        PlatTexture* gridTexture =
            glx_GetGridTexture(pTex->m_Width, pTex->m_Height);
        if (gridTexture != 0)
        {
            pTex = gridTexture;
        }
    }

    tmode = (eGLTextureMode)(textureState->flags & 3);
    if ((tmode & 1) == GX_REPEAT)
    {
        wrapS = GX_CLAMP;
    }
    else
    {
        wrapS = GX_REPEAT;
    }
    if ((tmode >> 1) == GX_REPEAT)
    {
        wrapT = GX_CLAMP;
    }
    else
    {
        wrapT = GX_REPEAT;
    }

    unsigned long textureObject[8];
    memcpy(textureObject, pTex->m_TexObj, sizeof(textureObject));
    GXInitTexObjWrapMode(textureObject, wrapS, wrapT);

    if (pTex->m_nPaletteEntries != 0)
    {
        unsigned long tlutObject[3];
        memcpy(tlutObject, pTex->m_TlutObj, sizeof(tlutObject));
        GXInitTexObjTlut(textureObject, textureMap);
        GXLoadTlut(tlutObject, textureMap);
    }

    GXLoadTexObj(textureObject, textureMap);
}


#ifdef NP_NATIVE
// 1.3 MSC_TEX_PRETOUCH ("no loading in play"): a texture is uploaded by the renderer at its first draw, and the textures of a goal, a Mega Strike or a replay (effects, the cut-scene
// files the game loads while it plays) are first drawn in play: a hitch there. Each rendered frame outside the front end, a few textures that are registered with the texture
// manager and not yet shown to the renderer are shown to it with np_gx_touch_tex, which uploads them without a draw and without changing any binding. The ones a match has loaded
// are done over its loading frames; a texture registered later (a cut-scene's, as its file is read) is done in the frames after, a few at a time, ahead of its first draw.
//   MSC_TEX_PRETOUCH=0 off (the code default), 1 upload, 2 upload and keep the textures against the renderer's budget until the front end is back (the Vita plan sets 2)
//   MSC_TEX_PRETOUCH_N=<n>    (retired: the loading scene follows the real frame time, live play 1 a frame, a held scene 2);  MSC_TEX_PRETOUCH_KIB=<k> at most k KiB of RGBA8 a frame (default 0 = by count only: a console upload costs 0.35-0.5 ms a texture whatever its size)
//   MSC_TEX_PRETOUCH_MIB=<m>  stop after m MiB of RGBA8 in all (default 0 = no limit);  MSC_TEX_PRETOUCH_LOG=1 a [pretouch] line (here and in the renderer)
struct NpPretouchCall
{
    u32 obj[8];
    u32 mode;
};
static void NpPretouchDo(const void* p)
{
    const NpPretouchCall* c = (const NpPretouchCall*)p;
    if (&np_gx_touch_tex != 0)
    {
        np_gx_touch_tex(c->mode == 3 ? (const void*)0 : (const void*)c->obj, c->mode == 3 ? 2u : c->mode);
    }
}
static int NpPretouchEnv(const char* name, int def)
{
    const char* e = getenv(name);
    return (e != 0 && *e != 0) ? atoi(e) : def;
}
static unsigned long long NpPretouchMs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)(ts.tv_nsec / 1000000);
}
// What the loading scene is waiting for (LoadingTask): nonzero while the match's textures are still being shown to the renderer, bounded by 3 s.
static int sPretouchPending = 0;
static unsigned sPretouchState = 0;
static unsigned long long sPretouchLoadStartMs = 0, sPretouchHeldMs = 0;
static unsigned sPretouchLoadN = 0;
extern "C" int NpTexPretouchHold(void)
{
    if (!sPretouchPending) return 0;
    const unsigned long long now = NpPretouchMs();
    if (sPretouchHeldMs == 0) sPretouchHeldMs = now;
    if (now - sPretouchHeldMs > 3000ull)
    {
        fprintf(stderr, "[pretouch] loading hold timed out after %llu ms with textures still to show; they trickle in held scenes\n", now - sPretouchHeldMs);
        sPretouchPending = 0;
        return 0;
    }
    return 1;
}
// scene: 0 front end, 1 the match's loading scene, 2 live play, 3 a held scene (cut-scene, replay, pause)
extern "C" void NpTexPretouchStep(int scene, unsigned state)
{
    sPretouchState = state;
    static int mode = -1, perFrame = 4, kibPerFrame = 1024, maxMib = 0, logOn = 0;
    static PlatTexture** touched = 0;
    static PlatTexture** remainder = 0;
    static unsigned cap = 0, cursor = 0;
    static bool inMatch = false, wasLoading = false;
    static unsigned long long totalKib = 0, touchedN = 0, lastMs = 0;
    static unsigned loadN = 16, tiny = 0;
    if (mode < 0)
    {
        const char* modeEnv = getenv("MSC_TEX_PRETOUCH");
        mode = (modeEnv != 0 && *modeEnv != 0) ? atoi(modeEnv) : 0;
        perFrame = NpPretouchEnv("MSC_TEX_PRETOUCH_N", 4);
        kibPerFrame = NpPretouchEnv("MSC_TEX_PRETOUCH_KIB", 0);
        maxMib = NpPretouchEnv("MSC_TEX_PRETOUCH_MIB", 0);
        logOn = NpPretouchEnv("MSC_TEX_PRETOUCH_LOG", 0);
        if (mode > 2) mode = 2;
        if (mode > 0 && &np_gx_touch_tex == 0) mode = 0;
    }
    if (mode == 0 || gTextureManager == 0 || gTextureManager->mTextures == 0)
    {
        return;
    }
    {
        static int lastScene = -1;
        if (scene != lastScene && NpPretouchEnv("MSC_TEX_PRETOUCH_LOG", 0))
            fprintf(stderr, "[pretouch] scene %d -> %d (state 0x%x)\n", lastScene, scene, (unsigned)sPretouchState);
        lastScene = scene;
    }
    if (scene == 0)
    {
        if (inMatch)
        {
            // the match is over: every pin is let go; what was shown to the renderer is forgotten (the next match shows its own)
            inMatch = false;
            NpPretouchCall c;
            memset(&c, 0, sizeof c);
            c.mode = 3;
            if (mode == 2) NpFsDefer(NpPretouchDo, &c, sizeof c);
            if (touched != 0) memset(touched, 0, cap * sizeof(PlatTexture*));
            if (remainder != 0) memset(remainder, 0, cap * sizeof(PlatTexture*));
            cursor = 0;
            if (logOn) fprintf(stderr, "[pretouch] match over: %llu textures shown to the renderer, %llu MiB of RGBA8, %u tiny (up to 32x32)\n", touchedN, totalKib / 1024, tiny);
            touchedN = 0;
            totalKib = 0;
            tiny = 0;
            sPretouchPending = 0;
            sPretouchHeldMs = 0;
            sPretouchLoadStartMs = 0;
            wasLoading = false;
            loadN = 16;
        }
        return;
    }
    inMatch = true;
    const unsigned n = gTextureManager->mCapacity;
    if (touched == 0 || cap != n)
    {
        touched = (PlatTexture**)calloc(n, sizeof(PlatTexture*));
        remainder = (PlatTexture**)calloc(n, sizeof(PlatTexture*));
        cap = n;
        cursor = 0;
        if (touched == 0 || remainder == 0) { cap = 0; return; }
    }
    const unsigned long long nowMs = NpPretouchMs();
    int budget;
    if (scene == 1)
    {
        // the loading scene: as many as the loading frames bear (a console upload costs 0.35-0.5 ms: about 20 ms of uploads is 40-55 textures): the count follows the real time of the last frame
        if (sPretouchLoadStartMs == 0) sPretouchLoadStartMs = nowMs;
        if (lastMs != 0)
        {
            const unsigned long long dt = nowMs - lastMs;
            if (dt < 36) loadN = loadN + 4 > 64 ? 64 : loadN + 4;
            else if (dt > 50) loadN = loadN < 12 ? 8 : loadN - 4;
        }
        lastMs = nowMs;
        budget = (int)loadN;
        wasLoading = true;
    }
    else
    {
        lastMs = 0;
        if (wasLoading)
        {
            // the loading scene is over: what it did not reach is the remainder, shown only in held scenes
            wasLoading = false;
            unsigned left = 0;
            for (unsigned k = 0; k < n; ++k)
            {
                PlatTexture* t = gTextureManager->mTextures[k];
                if (t != 0 && touched[k] != t) { remainder[k] = t; ++left; }
            }
            sPretouchPending = 0;
            fprintf(stderr, "[pretouch] %llu textures in %llu ms, loading held %llu ms, %u left for held scenes\n", touchedN, sPretouchLoadStartMs ? nowMs - sPretouchLoadStartMs : 0ull, sPretouchHeldMs ? nowMs - sPretouchHeldMs : 0ull, left);
        }
        // live play: one a frame, only of the textures registered after the loading scene (a cut-scene's, as its file is read); a held scene: two
        budget = scene == 3 ? 2 : 1;
        (void)perFrame;
    }
    if (maxMib > 0 && totalKib >= (unsigned long long)maxMib * 1024ull) return;
    int done = 0;
    unsigned kib = 0;
    unsigned scanned = 0;
    for (; scanned < n && done < budget; ++scanned)
    {
        PlatTexture* t = gTextureManager->mTextures[cursor];
        const unsigned at = cursor;
        cursor = (cursor + 1 == n) ? 0 : cursor + 1;
        if (t == 0 || touched[at] == t) continue;
        if (t->m_NpNoTouch || t->m_bMissingTexture || t->m_SwizzledData == 0 || t->m_Width == 0 || t->m_Height == 0 || t->m_nPaletteEntries != 0) { touched[at] = t; continue; }
        if (scene == 2 && remainder[at] == t) continue;   // (the loading scene's remainder waits for a held scene)
        const unsigned size = ((unsigned)t->m_Width * t->m_Height * 4u) >> 10;
        if (kibPerFrame > 0 && done > 0 && kib + size > (unsigned)kibPerFrame) { cursor = at; break; }   // (next frame)
        NpPretouchCall c;
        memcpy(c.obj, t->m_TexObj, sizeof c.obj);
        c.mode = (unsigned)mode == 2 ? 1u : 0u;
        NpFsDefer(NpPretouchDo, &c, sizeof c);
        touched[at] = t;
        remainder[at] = 0;
        kib += size;
        totalKib += size;
        ++touchedN;
        if (t->m_Width <= 32 && t->m_Height <= 32) ++tiny;
        ++done;
    }
    if (scene == 1) sPretouchPending = (scanned >= n && done < budget) ? 0 : 1;   // (the whole list was seen with nothing left to show: the scene may end)
}
#endif
