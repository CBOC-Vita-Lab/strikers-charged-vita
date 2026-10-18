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

void glSendFrame()
{
#ifdef NP_NATIVE
    // NP (helper lane): the software-skinned arrays the send binds are finished first (ShaderSkinMesh.cpp)
    if (&NpSkinLaneWait != 0)
    {
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
        glplatAbortFrame();
        gl_nDiscard -= 1;
    }
    else
    {
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
