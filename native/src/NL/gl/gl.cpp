#include <string.h>
#include "np/gsplit.h"
#include "np/stall.h"   // MSC_STALL_TIMERS (stall10)
#include "NL/gl/gl.h"
#include "NL/gl/glFont.h"

#include "NL/gl/glMatrix.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glPlat.h"
#include "NL/gl/glStat.h"
#include "NL/gl/glState.h"
#include "NL/gl/glStruct.h"
#include "NL/gl/glTarget.h"
#include "NL/gl/glView.h"
#include "NL/glx/GXMaterialProgramRegistry.h"
#include "NL/glx/glxLoadModel.h"
#include "NL/glx/glxTexture.h"
#include "NL/nlString.h"
#ifdef NP_NATIVE
#include "np/framesend.h"
#endif

const char* szMemoryNames[6] = { "header", "matrix", "index", "vertex", "texture", "target" };

static int gl_frameCounter;
#ifdef NP_NATIVE
static int gl_sendFrameCounter;   // the frame counter as of the send (a forked send runs after the counter has moved on)
#endif
static int gl_nDiscard;
static int gl_state;

bool glStartup(void (*startupCallback)())
{
    gl_frameCounter = 0;
    gl_nDiscard = 0;
    gl_state = 0;

    glInitResourcePools();
    startupCallback();
    glInitMaterialPrograms();

    if (!glplatStartup(glGetScreenInfo()))
        return false;

    gl_StatStartup();
    gl_StateStartup();
    for (int i = 0; i < GLTT_Num; ++i)
        glSetCurrentTexture(-1, (eGLTextureType)i);

    glSetRasterStateDefaults();
    glSetCurrentRasterState(glHandleizeRasterState());
    glSetTextureStateDefaults();
    glSetCurrentTextureState(glHandleizeTextureState());
    gl_MatrixStartup();
    gl_TargetStartup();
    gl_ViewStartup();

    if (!glplatPostStartup())
        return false;

    gl_FontStartup();
    return true;
}

unsigned long glHash(const char* string)
{
    return nlStringHash(string);
}

int glGetCurrentFrame()
{
#ifdef NP_NATIVE
    // NP (frame-send worker): the send of frame N reads N whenever it runs (the counter moves to N + 1 at the fork)
    if (&NpFsOnWorker != 0 && NpFsOnWorker() != 0)
    {
        return gl_sendFrameCounter;
    }
#endif
    return gl_frameCounter;
}

bool glHasQuads()
{
    return true;
}

void glBeginFrame()
{
    glplatBeginFrame();
    gl_state = 1;
}

void glEndFrame()
{
    glplatEndFrame();
    gl_state = 2;
}

bool glIsFrameActive()
{
    return gl_state == 1;
}

#ifdef NP_NATIVE
extern "C" void NpSkinLaneWait(void) __attribute__((weak));
#endif

#ifdef NP_NATIVE
extern "C" int NpDiscardRehearseEvery(void) __attribute__((weak));   // Game/InputManager.cpp: MSC_DISCARD_REHEARSE (every Nth discarded GameBegin frame is sent hidden)
extern "C" void np_gx_hidden(unsigned int on) __attribute__((weak));   // platform/gx: the frame being recorded is executed but never presented
extern "C" int NpDiscardFastAllowed(void) __attribute__((weak));   // Game/InputManager.cpp: MSC_DISCARD_FAST and not a LAN / online / recorded game

// The runs of discarded frames (stall10): how many in a row, how long, who asked, how many were not paced. One line when a run ends (with MSC_STALL_TIMERS=1 or MSC_DISCARD_FAST=1).
namespace
{
struct DiscardRun
{
    unsigned frames, fast, runs, totalFrames, totalFast, rehearsed, totalRehearsed;
    unsigned long long startUs, totalUs;
    const char* why;
    unsigned startState;
};
DiscardRun sDiscard = {0, 0, 0, 0, 0, 0, 0, 0, 0, "other", 0};
}
static int NpDiscardFastOn()
{
    return &NpDiscardFastAllowed != 0 && NpDiscardFastAllowed() != 0;
}
static void NpDiscardWhy(const char* why)
{
    sDiscard.why = why;
}
// MSC_DISCARD_REHEARSE: is this discarded frame one that is sent hidden? Only the walk-out's wait ("GameBegin"), every Nth frame of the run, and only where np_gx_hidden exists.
static int NpDiscardRehearseNow()
{
    if (&NpDiscardRehearseEvery == 0 || &np_gx_hidden == 0) return 0;
    const int n = NpDiscardRehearseEvery();
    if (n == 0 || sDiscard.why == 0 || strncmp(sDiscard.why, "GameBegin", 9) != 0) return 0;
    if (strcmp(sDiscard.why, "GameBegin hold") == 0) return 1;   // MSC_LOADHOLD: the walk-out's own first frames
    return (sDiscard.frames % (unsigned)n) == 1 % (unsigned)n ? 1 : 0;   // (frames was counted by NpDiscardNote: 1 is the run's first)
}
static void NpDiscardNoteFast()
{
    ++sDiscard.fast;
    ++sDiscard.totalFast;
}
static void NpDiscardNote(bool discarded)
{
    if (discarded)
    {
        if (sDiscard.frames == 0)
        {
            sDiscard.startUs = np::stall::NowUs();
            sDiscard.startState = np::stall::State();
        }
        ++sDiscard.frames;
        ++sDiscard.totalFrames;
        return;
    }
    if (sDiscard.frames == 0) return;
    const unsigned long long us = np::stall::NowUs() - sDiscard.startUs;
    ++sDiscard.runs;
    sDiscard.totalUs += us;
    if (np::stall::On() || sDiscard.fast != 0 || sDiscard.rehearsed != 0)
        fprintf(stderr, "[discard] run %u: %u frames discarded in %.1f ms (%.1f ms a frame), %u of them not paced (MSC_DISCARD_FAST), %u sent hidden (MSC_DISCARD_REHEARSE), asked by \"%s\", task state 0x%x -> 0x%x; all runs so far: %u frames, %u not paced, %.1f ms\n",
                sDiscard.runs, sDiscard.frames, us / 1000.0, us / 1000.0 / sDiscard.frames, sDiscard.fast, sDiscard.rehearsed, sDiscard.why, sDiscard.startState, np::stall::State(), sDiscard.totalFrames, sDiscard.totalFast, sDiscard.totalUs / 1000.0);
    sDiscard.frames = 0;
    sDiscard.fast = 0;
    sDiscard.rehearsed = 0;
}
#endif

void glSendFrame()
{
#ifdef NP_NATIVE
    NP_STALL_ARG("glSendFrame (arg: frames to discard)", gl_nDiscard);   // MSC_STALL_TIMERS (stall10)
    // NP (helper lane): the software-skinned arrays the send binds are finished first (ShaderSkinMesh.cpp)
    if (&NpSkinLaneWait != 0)
    {
        NP_GS(NPGS_WAIT_JOIN);
        NpSkinLaneWait();
    }
#endif
#ifdef NP_NATIVE
    // NP (two-stage pipeline): the job of the last frame may still be running; the paths below that do not fork must not start beside it
    if (&NpFsJoin != 0)
    {
        NpFsJoin();
    }
#endif
    if (gl_nDiscard > 0)
    {
#ifdef NP_NATIVE
        // MSC_DISCARD_FAST (stall10): a discarded frame is thrown away, but the game paces it at one retrace each (the original game waited for the screen to catch up): the
        // loader that waits behind these frames (the front end's scenes at the end of loading, the walk-out's NIS) needs a count of frames, not of retraces. Not in a LAN / online / recorded game.
        NpDiscardNote(true);
        if (NpDiscardRehearseNow())
        {
            // MSC_DISCARD_REHEARSE (stall10): this frame goes to the renderer as a hidden one, the plain way (not forked: the hidden mark is recorded in the frame's own stream, here on the game thread)
            ++sDiscard.rehearsed;
            ++sDiscard.totalRehearsed;
            np_gx_hidden(1);
            glplatSendFrame();
            gl_nDiscard -= 1;
            gl_ViewReset();
            gl_state = 0;
            gl_frameCounter += 1;
            return;
        }
        if (NpDiscardFastOn())
        {
            glplatAbortFrameFast();
            NpDiscardNoteFast();
        }
        else
#endif
        glplatAbortFrame();
        gl_nDiscard -= 1;
    }
    else
    {
#ifdef NP_NATIVE
        NpDiscardNote(false);
#endif
#ifdef NP_NATIVE
        gl_sendFrameCounter = gl_frameCounter;
        if (glplatSendFrameForked())
        {
            // the send runs as a job on a second core and ends with gl_ViewReset(); everything else is the plain path's
            gl_state = 0;
            gl_frameCounter += 1;
            return;
        }
#endif
        glplatSendFrame();
    }

    gl_ViewReset();
    gl_state = 0;
    gl_frameCounter += 1;
}

void glDiscardFrame(int nFrames)
{
    glDiscardFrameWhy(nFrames, "other");
}

void glDiscardFrameWhy(int nFrames, const char* why)
{
#ifdef NP_NATIVE
    NpDiscardWhy(why);
#endif
    if (nFrames > gl_nDiscard)
        gl_nDiscard = nFrames;
}

void glFinish()
{
    NpFsGate("finish");   // NP (frame-send worker)
    glplatFinish();
}

void glCompact()
{
    glViewCompact();
}

unsigned long glGetNumTriangles(eGLPrimitive primitive, unsigned long count)
{
    switch (primitive)
    {
    case GLP_TriList:
        return count / 3;
    case GLP_TriStrip:
        return count - 2;
    case GLP_TriFan:
        return count - 2;
    case GLP_QuadList:
        return count / 2;
    case GLP_LineList:
        return count / 2;
    case GLP_LineStrip:
        return count - 1;
    default:
        return 0;
    }
}

glModel* glEndLoadModel(
    void* data, unsigned long size, unsigned long* pNumModels, void* context)
{
    return glplatEndLoadModel(data, size, pNumModels, context);
}

bool glBeginLoadModel(const char* filename,
    void (*callback)(void*, unsigned long, void*), void* userData,
    void* context)
{
    return glplatBeginLoadModel(filename, callback, userData);
}

bool glBeginLoadTextureBundle(const char* filename,
    void (*callback)(void*, unsigned long, void*), void* param,
    void* context)
{
    return glplatBeginLoadTextureBundle(filename, callback, param);
}

glModel* glLoadModel(
    const char* filename, unsigned long* pNumModels, void* context)
{
    return glplatLoadModel(filename, pNumModels, context);
}

bool glLoadTextureBundle(
    const char* filename, GLResourcePool* allocator)
{
    return glplatLoadTextureBundle(filename, allocator);
}

float glGetOrthographicWidth()
{
    return glplatGetOrthographicWidth();
}

float glGetOrthographicHeight()
{
    return glplatGetOrthographicHeight();
}

void glBeginResource(const char*)
{
}

void glBeginResource(unsigned long)
{
}

void glEndResource()
{
}

void glDumpResources(const char* name, GLResourcePool* resourcePool)
{
}
