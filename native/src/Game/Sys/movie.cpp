#include "Game/Sys/movie.h"
#include "Game/Sys/debug.h"

#include "RVL_SDK/thp/THPSimple.h"

#include "NL/gl/glMemory.h"
#include "NL/gl/glTexture.h"
#include "NL/gc/gcSwizzler.h"
#include "NL/gl/glState.h"
#include "NL/glx/glxSwap.h"
#include "NL/glx/glxTexture.h"
#include "NL/nlFileGC.h"
#include "NL/nlMemory.h"
#include "NL/nlPrint.h"
#include "NL/nlString.h"

#include <revolution/gx/GXMisc.h>
#include <revolution/gx/GXTexture.h>
#include <revolution/os/OSThread.h>

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "NL/nlstring_tmpl.h"
#include "NL/gl/glTexture.h"
#ifdef NP_NATIVE
#include "np/framesend.h"
#include "np/fps.h"
#endif

static THPVideoInfo videoInfo;
static PlatTexture* pTex[4];

static unsigned int g_uLastDecodeRenderTick = -1;
static bool g_bSyncedDecode = true;

static bool g_bActive;
static bool lbl_806E2419;
static bool g_bTHPInitialized;
static unsigned char* buffer;
static bool start;
static unsigned int g_uCurrentFrame;
static unsigned int g_uNextDecodeFrame;
static unsigned int g_uDecodeCount;
static unsigned int g_uRenderTickCount;
static bool g_bFinished;
static unsigned int g_uStartFrame;
static bool g_bMovieMustStop;
static unsigned long resourceMarker;
static unsigned int g_uLastPlayFrame;
#ifdef NP_NATIVE
static unsigned int g_uMoviePictures;
static unsigned int g_uMovieSkips;
static unsigned int g_uMovieAudioFull;
static unsigned int g_uMovieReadWaits;
#endif

bool MovieInit()
{
    if (g_bTHPInitialized)
    {
        return true;
    }

    THPSimpleInit(1);
    g_bTHPInitialized = true;
    return true;
}

bool MovieQuit()
{
    THPSimpleQuit();
    g_bTHPInitialized = false;
    return true;
}

void SetSyncedDecode(bool value)
{
    g_bSyncedDecode = value;
}

bool MovieStart(
    const char* szFilename, bool bSound, bool bLoopMovie, bool bMono)
{
    if (g_bActive)
    {
        return false;
    }

    // NP (frame-send pipeline): the movie's textures are registered in the tables the send reads and prepared with GX calls on this thread
    NpFsGate("moviestart");

    g_bMovieMustStop = false;

    char fileName[256];
    nlStrNCpy(fileName, szFilename, 256);
    nlToLower(fileName);

    if (!THPSimpleOpen(fileName))
    {
        return false;
    }

    THPSimpleGetVideoInfo(&videoInfo);
    GLResourcePool* resourceInterface = glGetCurrentResourcePool();
    MemoryAllocator* allocator = (MemoryAllocator*)resourceInterface;
    resourceMarker = resourceInterface->MarkResource();

    pTex[0] = glx_CreatePlatTexture(allocator);
    pTex[0]->Create(videoInfo.xSize, videoInfo.ySize, GXTex_I8,
        allocator, 1, false, false);
    glRegisterTexture(glGetTexture("movie"), pTex[0], allocator);

    pTex[1] = glx_CreatePlatTexture(allocator);
    pTex[1]->Create(videoInfo.xSize / 2, videoInfo.ySize / 2,
        GXTex_I8, allocator, 1, false, false);
    glRegisterTexture(glGetTexture("movie_u"), pTex[1], allocator);

    pTex[2] = glx_CreatePlatTexture(allocator);
    pTex[2]->Create(videoInfo.xSize / 2, videoInfo.ySize / 2,
        GXTex_I8, allocator, 1, false, false);
    glRegisterTexture(glGetTexture("movie_v"), pTex[2], allocator);

    unsigned long texSize = GCTextureSize(pTex[0]->m_Format,
        pTex[0]->m_Width, pTex[0]->m_Height, pTex[0]->m_Levels,
        (unsigned long)-1);
    memset(pTex[0]->m_SwizzledData, 0x10, texSize);

    texSize = GCTextureSize(pTex[1]->m_Format, pTex[1]->m_Width,
        pTex[1]->m_Height, pTex[1]->m_Levels, (unsigned long)-1);
    memset(pTex[1]->m_SwizzledData, 0x80, texSize);

    texSize = GCTextureSize(pTex[2]->m_Format, pTex[2]->m_Width,
        pTex[2]->m_Height, pTex[2]->m_Levels, (unsigned long)-1);
    memset(pTex[2]->m_SwizzledData, 0x80, texSize);

    pTex[0]->Prepare();
    pTex[1]->Prepare();
    pTex[2]->Prepare();
    GXInvalidateTexAll();

    buffer = (unsigned char*)nlMalloc(
        THPSimpleCalcNeedMemory(), 32, false);
    THPSimpleSetBuffer(buffer);

    unsigned int frame = glxGetFrameCount();
    g_uNextDecodeFrame = frame;
    g_uStartFrame = frame;
    g_uDecodeCount = 0;

    if (!THPSimplePreLoad(bLoopMovie != false))
    {
        g_bActive = true;
        MovieStop();
        return false;
    }

    fn_80372970(bMono);
    g_uNextDecodeFrame = glxGetFrameCount();
    g_uLastDecodeRenderTick = g_uRenderTickCount - 1;
#ifdef NP_NATIVE
    g_uMoviePictures = g_uMovieSkips = g_uMovieAudioFull = g_uMovieReadWaits = 0;
    fprintf(stderr, "[movie-fix] start %s %ux%u; clock=%s\n", fileName,
        videoInfo.xSize, videoInfo.ySize, getenv("MSC_MOVIE_CATCHUP") ? getenv("MSC_MOVIE_CATCHUP") : "bounded");
#endif
    g_bFinished = false;
    start = true;
    g_bActive = true;
    g_uCurrentFrame = 0;
    return true;
}

bool MovieStop()
{
    if (!g_bActive)
    {
        return false;
    }

    // NP (frame-send pipeline): the textures and the decoder buffer go away; the send may still read them
    NpFsGate("moviestop");

    g_bActive = false;
    lbl_806E2419 = false;

#ifdef NP_NATIVE
    // FPS (np/fps.h): the movie's speed in retraces (decodes follow the retrace count, not the drawn frames): a line for the proof runs
    NpFpsMovieEnd(g_uDecodeCount, glxGetFrameCount() - g_uStartFrame);
    fprintf(stderr, "[movie-fix] consumed=%u pictures=%u skipped=%u audio_full=%u read_waits=%u retraces=%u\n",
        g_uDecodeCount, g_uMoviePictures, g_uMovieSkips, g_uMovieAudioFull, g_uMovieReadWaits, glxGetFrameCount() - g_uStartFrame);
#endif
    int lastFrame = THPSimpleGetTotalFrame() - 1;
    int currentFrame = (int)(unsigned int)g_uCurrentFrame;
    if ((unsigned int)currentFrame != (unsigned int)lastFrame)
    {
        tDebugPrintManager::Print(DC_RENDER, "MOVIE did not finish playback.\n");
    }

    THPSimpleAudioStop();
    THPSimpleLoadStop();
    THPSimpleClose();

    if (buffer != 0)
    {
        nlFree(buffer);
    }

    buffer = 0;
    pTex[0] = 0;
    pTex[1] = 0;
    pTex[2] = 0;
    pTex[3] = 0;

    glGetCurrentResourcePool()->ReleaseResource(resourceMarker);
    return true;
}

void MovieRenderTick()
{
    ++g_uRenderTickCount;
}

bool MoviePlay()
{
    if (!g_bActive)
    {
        return false;
    }

    // NP (frame-send worker): the decoder writes the planes the send's movie material binds, and calls GX
    NpFsGate("movie");

    if (g_bMovieMustStop)
    {
        g_bFinished = true;
        return false;
    }

    unsigned int frame = glxGetFrameCount();
    if (g_uLastPlayFrame != frame)
    {
        g_uLastPlayFrame = frame;
    }

    bool decode = false;
#ifdef NP_NATIVE
    unsigned int catchUp = 0;
    static const bool movieClock = [] { const char* e = getenv("MSC_MOVIE_CATCHUP"); return !(e && e[0] == '0'); }();
    if (movieClock)
    {
        // MoviePlay is called after EVERY task. Bound work by render ticks, not
        // retraces: a slow decode must never monopolize the rest of the task loop.
        // The same clock applies to stadium previews (SetSyncedDecode(false)).
        if ((int)(frame - g_uNextDecodeFrame) >= 0 && g_uRenderTickCount != g_uLastDecodeRenderTick)
        {
            decode = true;
            g_uLastDecodeRenderTick = g_uRenderTickCount;
            catchUp = (frame - g_uNextDecodeFrame) / 2;
            if (catchUp > 2) catchUp = 2;
        }
    }
    else
#endif
    if (g_bSyncedDecode)
    {
        if (frame >= g_uNextDecodeFrame)
        {
            decode = true;
        }
    }
    else if (g_uRenderTickCount != g_uLastDecodeRenderTick)
    {
        decode = true;
        g_uLastDecodeRenderTick = g_uRenderTickCount;
    }

    if (decode)
    {
        if (g_bSyncedDecode)
        {
            GXSetDrawDone();
            GXFlush();
            nlServiceFileSystem();
            OSYieldThread();
            GXWaitDrawDone();
        }
        else
        {
            nlServiceFileSystem();
        }

#ifdef NP_NATIVE
        unsigned int packets = movieClock ? catchUp + 1 : 1;
        // Always display the last packet we can actually accept. Otherwise a full
        // audio ring or pending disk read could leave only skipped pictures.
        const unsigned int capacity = THPSimpleGetDecodeCapacity();
        // A callback can free a slot after the snapshot. Probe it with a picture.
        if (!capacity) packets = 1;
        else if (packets > capacity) packets = capacity;
#else
        const unsigned int packets = 1;
#endif
        bool picture = false;
        for (unsigned int packet = 0; packet < packets; ++packet)
        {
            const bool show = packet + 1 == packets;
            const int error = THPSimpleDecodeVideo(0, show);
            if (error == 1 || error == 2 || error == 4)
            {
#ifdef NP_NATIVE
                fprintf(stderr, "[movie-fix] stopped code=%d at packet=%u\n", error, g_uCurrentFrame);
#endif
                g_bFinished = true;
                return false;
            }
            if (error != 0)
            {
#ifdef NP_NATIVE
                if (error == 3) ++g_uMovieAudioFull;
                if (error == 5) ++g_uMovieReadWaits;
#endif
                break;  // backpressure: this packet and its deadline remain pending
            }
            ++g_uDecodeCount;
            ++g_uCurrentFrame;
#ifdef NP_NATIVE
            if (show) ++g_uMoviePictures; else ++g_uMovieSkips;
            g_uNextDecodeFrame = movieClock ? g_uNextDecodeFrame + 2 : frame + 2;
#else
            g_uNextDecodeFrame = frame + 2;
#endif
            if (start)
            {
                THPSimpleAudioStart();
                start = false;
            }
            picture = show;
        }
        if (picture)
        {
            GXInvalidateTexAll();
            pTex[0]->Prepare();
            pTex[1]->Prepare();
            pTex[2]->Prepare();
        }
    }
    else
    {
        nlServiceFileSystem();
    }

    return true;
}

bool IsMovieActive()
{
    return g_bActive;
}

bool IsMovieFinished()
{
    return g_bFinished;
}

void ClearMovieFinished()
{
    g_bFinished = false;
}

unsigned int GetMovieFrame()
{
    return g_uCurrentFrame;
}
