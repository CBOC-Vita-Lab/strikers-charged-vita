#include <revolution/gx/GXPixel.h>

#include "Game/TweakValueFloat.h"
#include "Game/TweakValueInt.h"
#include "Game/SharedStaticStorage.h"
#include "NL/glx/glxSend.h"
#ifdef NP_NATIVE
#include "np/framesend.h"
#endif

static float glx_FogNear = 0.25f;
static float glx_FogFar = 130.0f;

static TweakValueFloat glx_FogStart(
    "sfFogStart", "/Render/Fog", 5.0f);
static TweakValueFloat glx_FogEnd(
    "sfFogEnd", "/Render/Fog", 160.0f);
static TweakValueInt glx_FogRed(
    "siFogRed", gLastTweakCategory, 255);
static TweakValueInt glx_FogGreen(
    "siFogGreen", gLastTweakCategory, 255);
static TweakValueInt glx_FogBlue(
    "siFogBlue", gLastTweakCategory, 255);
static TweakValueFloat glx_FogIntensity(
    "sfFogIntensity", gLastTweakCategory, 1.0f);
static TweakValueBool glx_bFog(
    "sbFogEnabled", gLastTweakCategory, false);
static TweakValueInt glx_FogType(
    "siFogType", gLastTweakCategory, 0);

static GXFogType fogtype[] = {
    GX_FOG_PERSP_LIN,
    GX_FOG_PERSP_EXP,
    GX_FOG_PERSP_EXP2,
    GX_FOG_PERSP_REVEXP,
    GX_FOG_PERSP_REVEXP2,
};

void glx_SetFogClipPlanes(float nearPlane, float farPlane)
{
    glx_FogNear = nearPlane;
    glx_FogFar = farPlane;
}

float glx_GetFogStart()
{
    return glx_FogStart.value;
}

void glx_SetFogStart(float value)
{
    glx_FogStart.value = value;
}

float glx_GetFogEnd()
{
    return glx_FogEnd.value;
}

void glx_SetFogEnd(float value)
{
    glx_FogEnd.value = value;
}

#ifdef NP_NATIVE
// NP (two-stage pipeline): the tasks set the fog while the send of the last frame may still run; the call (plain values) is made after the join
struct GlxFogCall
{
    GXFogType type;
    GXColor colour;
    float start;
    float end;
    float nearz;
    float farz;
};

static void glx_ApplyFog(const void* data)
{
    const GlxFogCall* c = static_cast<const GlxFogCall*>(data);
    GXSetFog(c->type, c->colour, c->start, c->end, c->nearz, c->farz);
}

static void glx_SetFog(GXFogType type, GXColor colour, float start, float end, float nearz, float farz)
{
    GlxFogCall call;
    call.type = type;
    call.colour = colour;
    call.start = start;
    call.end = end;
    call.nearz = nearz;
    call.farz = farz;
    NpFsDefer(glx_ApplyFog, &call, sizeof(call));
}
#else
#define glx_SetFog GXSetFog
#endif

#ifdef NP_NATIVE
// NP (two-stage pipeline): the send (RLView::BeginRender / BeginPacket call glx_Fog) reads the fog parameters of the end of its frame, the build of the
// next frame (NisPlayer's start and end, the layer set-up) changes them while the job runs: the job works on the copy made at the fork.
static struct GlxFogFork
{
    float nearz, farz, start, end, intensity;
    s32 red, green, blue, type;
    bool enabled;
} sFogFork;

void glx_FogAtFork()
{
    sFogFork.nearz = glx_FogNear;
    sFogFork.farz = glx_FogFar;
    sFogFork.start = glx_FogStart.value;
    sFogFork.end = glx_FogEnd.value;
    sFogFork.intensity = glx_FogIntensity.value;
    sFogFork.red = glx_FogRed.mValue;
    sFogFork.green = glx_FogGreen.mValue;
    sFogFork.blue = glx_FogBlue.mValue;
    sFogFork.type = glx_FogType.mValue;
    sFogFork.enabled = glx_bFog.mValue;
}
#endif

void glx_Fog(bool enable)
{
#ifdef NP_NATIVE
    if (&NpFsOnWorker != 0 && NpFsOnWorker() != 0)
    {
        if (enable && sFogFork.enabled)
        {
            GXColor fogColour;
            fogColour.r = (s32)((float)sFogFork.red * sFogFork.intensity);
            fogColour.g = (s32)((float)sFogFork.green * sFogFork.intensity);
            fogColour.b = (s32)((float)sFogFork.blue * sFogFork.intensity);
            fogColour.a = 0xFF;
            glx_SetFog(fogtype[sFogFork.type], fogColour, sFogFork.start, sFogFork.end, sFogFork.nearz, sFogFork.farz);
        }
        else
        {
            GXColor fogColour;
            fogColour.r = 0xFF;
            fogColour.g = 0xFF;
            fogColour.b = 0xFF;
            fogColour.a = 0xFF;
            glx_SetFog(GX_FOG_NONE, fogColour, 0.0f, 0.0f, 0.0f, 0.0f);
        }
        return;
    }
#endif
    if (enable && glx_bFog)
    {
        GXColor fogColour;
        float r = glx_FogRed.mValue;
        float rScaled = r * glx_FogIntensity.value;
        fogColour.r = (s32)rScaled;
        float g = glx_FogGreen.mValue;
        float gScaled = g * glx_FogIntensity.value;
        fogColour.g = (s32)gScaled;
        float b = glx_FogBlue.mValue;
        float bScaled = b * glx_FogIntensity.value;
        fogColour.b = (s32)bScaled;
        fogColour.a = 0xFF;
        glx_SetFog(fogtype[glx_FogType.mValue], fogColour, glx_GetFogStart(), glx_GetFogEnd(), glx_FogNear, glx_FogFar);
    }
    else
    {
        GXColor fogColour;
        fogColour.r = 0xFF;
        fogColour.g = 0xFF;
        fogColour.b = 0xFF;
        fogColour.a = 0xFF;
        glx_SetFog(GX_FOG_NONE, fogColour, 0.0f, 0.0f, 0.0f, 0.0f);
    }
}
