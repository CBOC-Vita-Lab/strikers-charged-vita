#include "Game/Render/Amortize.h"

#include "Game/Effects/EffectsTemplate.h"
#include "Game/Effects/EmissionManager.h"
#include "Game/Effects/ParticleSystem.h"
#include "Game/Game.h"
#include "Game/GameInfo.h"
#include "Game/Render/MegastrikeBackgroundOverlay.h"
#include "Game/Render/RLViewLayers.h"
#include "Game/Render/Warble.h"
#include "Game/Task/GameTaskState.h"
#include "NL/gl/gl.h"
#include "NL/gl/glDraw2.h"
#include "NL/gl/glPlat.h"
#include "NL/gl/glState.h"
#include "NL/gl/glTarget.h"
#include "NL/nlMath.h"
#include "NL/nlTask.h"
#include "np/amortize.h"
#include "np/fps.h"
#include "np/livetrades.h"
#include "np/scenecut.h"

#include <stdio.h>
#include <stdlib.h>

extern unsigned gAmortShadow[2];   // RenderShadow.cpp

// np/amortize.h has the switches.
//
// The haze's target. The view is eCLV_GrabTexture: nothing in the game draws there, it stands before the world, so a refresh is a small scene of its own and does not break the frame's.
// The size is the warble's: a 320x224 viewport, copied at half size to 160x112, a sixteenth of the game's pixels. RGBA8: the haze is hundreds of faint sprites, and RGB565's steps of 8
// would round most of them away. It is added in eCLV_WarbleBlend, an empty ortho view after the particles and before the 2D (a frame with warble is live), with blend ONE, ONE: in the
// frame's own scene, no scene change, one quad over the screen.
namespace
{
// (a view matrix is for row vectors, a projection from C_MTXPerspective for column vectors: GLView transposes it as well)
void ViewProjection(nlMatrix4& vp, nlMatrix4& inverse)
{
    nlMatrix4 view, proj, transposed;
    sPerspectiveCamera.GetViewMatrix(view);
    sPerspectiveCamera.GetProjectionMatrix(proj);
    nlTransposeMatrix(transposed, proj);
    nlMultMatrices(vp, view, transposed);
    if (!NpAmortInvert(inverse.e, vp.e))
    {
        inverse = vp;   // (cannot happen with a camera; the plane test below then says no)
    }
}

GLRenderPair sHazeTarget;

int HazeEvery()
{
    static int v = -1;
    if (v < 0) v = NpHazeCacheParse(NpAmortEnvInt("MSC_FX_HAZE_CACHE", 0));
    return v;
}

float HazeHeight()
{
    static float z = -1.0f;
    if (z < 0.0f)
    {
        const char* e = getenv("MSC_FX_HAZE_CACHE_Z");
        z = e && *e ? (float)atof(e) : 5.0f;   // under the camera: open play's stands at 10 to 21
    }
    return z;
}

const float cJumpPixels = 96.0f;   // of 960x544, in a frame: a cut; open play's fastest pan on the Pipeline route is 57 (MSC_AMORT_LOG)

NpHazeState sHaze;
int sHazeAct = NPHAZE_LIVE;
bool sHazeTargetMade;
float sHazeThen[16];
bool sHazeFog;
unsigned sHazeCount[3], sHazeSprites;

int LogOn()
{
    static int v = -1;
    if (v < 0)
    {
        const char* e = getenv("MSC_AMORT_LOG");
        v = e && e[0] == '1';
    }
    return v;
}

// MSC_AMORT_LOG: pixels of 960x544 that the picture of the plane of MSC_FX_HAZE_CACHE_Z moves, by frames held (1, 2, 3)
struct MoveStat
{
    unsigned frames, cuts;
    unsigned hist[3][6];   // < 0.5, 1, 2, 4, 8, more
    float sum[3], top[3];
    float vp[3][16];       // 1, 2, 3 frames ago
    int have;
};
MoveStat sMove;

void Move()
{
    if (nlTaskManager::m_pInstance->mCurrentState != TASK_GAMEPLAY)
    {
        sMove.have = 0;
        return;
    }
    nlMatrix4 vp, inverse;
    ViewProjection(vp, inverse);
    sMove.frames++;
    for (int back = 0; back < sMove.have && back < 3; back++)
    {
        float shift[2];
        if (!NpAmortShift(inverse.e, sMove.vp[back], HazeHeight(), shift))
        {
            sMove.cuts++;
            continue;
        }
        const float dx = shift[0] * 480.0f, dy = shift[1] * 272.0f;
        const float px = nlSqrt(dx * dx + dy * dy, false);
        sMove.sum[back] += px;
        if (px > sMove.top[back]) sMove.top[back] = px;
        sMove.hist[back][px < 0.5f ? 0 : px < 1.0f ? 1 : px < 2.0f ? 2 : px < 4.0f ? 3 : px < 8.0f ? 4 : 5]++;
    }
    __builtin_memmove(sMove.vp[1], sMove.vp[0], sizeof(float) * 32);
    __builtin_memcpy(sMove.vp[0], vp.e, sizeof(float) * 16);
    if (sMove.have < 3) sMove.have++;
    if (sMove.frames >= 300)
    {
        for (int back = 0; back < 3; back++)
        {
            printf("[amort] camera, picture held %d: pixels moved: mean %.2f max %.1f; frames under 0.5/1/2/4/8/more: %u/%u/%u/%u/%u/%u; cuts %u\n", back + 1,
                sMove.sum[back] / (float)sMove.frames, sMove.top[back], sMove.hist[back][0], sMove.hist[back][1], sMove.hist[back][2], sMove.hist[back][3], sMove.hist[back][4],
                sMove.hist[back][5], sMove.cuts);
        }
        printf("[amort] shadow pictures per frame: %.2f in view, %.2f out of view\n", (float)gAmortShadow[0] / 300.0f, (float)gAmortShadow[1] / 300.0f);
        gAmortShadow[0] = gAmortShadow[1] = 0;
        const int have = sMove.have;
        float keep[3][16];
        __builtin_memcpy(keep, sMove.vp, sizeof(keep));
        __builtin_memset(&sMove, 0, sizeof(sMove));
        sMove.have = have;
        __builtin_memcpy(sMove.vp, keep, sizeof(keep));
    }
}
} // namespace

void AmortizeCreateTargets()
{
    if (!HazeEvery())
    {
        return;
    }
    GLTargetInfo info;
    nlZeroMemory(&info, sizeof(GLTargetInfo));
    info.width = glplatGetDefaultTargetWidth() >> 2;
    info.height = glplatGetDefaultTargetHeight() >> 2;
    info.format = GLTargetFormat_RGBA8;
    info.clearFlags = 1;
    sHazeTarget = glCreateTarget("hazecache", &info);
    GLView* view = GetLayerView(eCLV_GrabTexture);
    view->SetRenderPair(sHazeTarget);
    view->SetViewport(0, 0, glplatGetDefaultTargetWidth() >> 1, glplatGetDefaultTargetHeight() >> 1);
    view->m_Target = GLViewTarget_None;
    sHazeTargetMade = true;
}

void AmortizeHazeBeginFrame()
{
    const int every = HazeEvery();
    if (!every)
    {
        return;
    }
    const bool active = sHazeTargetMade && nlTaskManager::m_pInstance->mCurrentState == TASK_GAMEPLAY && !gWarbleEnabled && !gMegastrikeBackgroundOverlay.mActive
        && !g_pGame->mbCaptainShotToScoreOn;
    bool jumped = false;
    if (active && sHaze.valid)
    {
        nlMatrix4 vp, inverse;
        float uv[4][2], shift[2];
        ViewProjection(vp, inverse);
        if (!NpHazeCorners(inverse.e, sHazeThen, HazeHeight(), uv) || !NpAmortShift(inverse.e, sHazeThen, HazeHeight(), shift))
        {
            jumped = true;
        }
        else
        {
            const float dx = shift[0] * 480.0f, dy = shift[1] * 272.0f;
            jumped = dx * dx + dy * dy > cJumpPixels * cJumpPixels * (float)(sHaze.age + 1);
        }
    }
    sHazeAct = NpHazeStep(&sHaze, every, active, jumped);
    sHazeCount[sHazeAct]++;
}

bool AmortizeHazeRoute(const ParticleSystem* system, GLView** view)
{
    if (sHazeAct == NPHAZE_LIVE || *view != GetLayerView(eCLV_Particles))
    {
        return true;
    }
    const EffectsTemplate* t = system->m_pTemplate;
    if (t->m_uModelID != 0xFFFFFFFF || (system->m_pSpec != 0 && system->m_pSpec->m_bLight))
    {
        return true;
    }
    const float size = t->mProperties[1]->base * t->mProperties[2]->base;
    const int additive = t->m_eBlend == EfBlend_Additive;
    if (!NpSceneHazeTemplate(additive, size, t->m_rParticleLife.base, t->m_fFountainLife)
        && !(NpSceneHazeWide() && NpSceneHazeTemplateWide(additive, size, t->m_rParticleLife.base, t->m_fFountainLife)))
    {
        return true;
    }
    sHazeSprites += (unsigned)system->m_NumParticles;
    if (sHazeAct == NPHAZE_HOLD)
    {
        return false;
    }
    *view = GetLayerView(eCLV_GrabTexture);
    return true;
}

extern "C" int NpCrowdLiveRelief(void) __attribute__((weak));

static int Amortize60Enabled()
{
    static int v = -1;
    if (v < 0) v = NpAmortEnvInt("MSC_AMORT_60", 0) == 1;
    return v;
}

int AmortizeSixty()
{
    return Amortize60Enabled() && NpFpsPolicy() == 2;
}

int AmortizeCrowdSixty()
{
    return AmortizeSixty() || (NpCrowdLive30() && NpFpsPolicy() == 2) || (Amortize60Enabled() && &NpCrowdLiveRelief != 0 && NpCrowdLiveRelief() != 0);   // MSC_AMORT_60, MSC_CROWD_LIVE, int33 Smooth-60 live relief
}

void AmortizeBeforeSend()
{
    if (LogOn())
    {
        Move();
    }
    if (!HazeEvery())
    {
        return;
    }
    GLView* target = GetLayerView(eCLV_GrabTexture);
    target->m_Target = sHazeAct == NPHAZE_REFRESH ? GLViewTarget_Mode8 : GLViewTarget_None;
    target->m_ClearColour = sHazeAct == NPHAZE_REFRESH;
    target->m_ClearDepth = sHazeAct == NPHAZE_REFRESH;
    if (LogOn() && sHazeCount[0] + sHazeCount[1] + sHazeCount[2] >= 300)
    {
        printf("[amort] haze, 300 frames: %u live, %u drawn, %u held; sprites per frame not live %.1f\n", sHazeCount[0], sHazeCount[1], sHazeCount[2],
            (float)sHazeSprites / (float)(sHazeCount[1] + sHazeCount[2] + 1));
        sHazeCount[0] = sHazeCount[1] = sHazeCount[2] = sHazeSprites = 0;
    }
    if (sHazeAct == NPHAZE_LIVE)
    {
        return;
    }

    nlMatrix4 vp, inverse;
    ViewProjection(vp, inverse);
    if (sHazeAct == NPHAZE_REFRESH)
    {
        __builtin_memcpy(sHazeThen, vp.e, sizeof(sHazeThen));
    }

    glPoly2 poly;
    nlColour white;
    nlColourSet(white, 0xFF, 0xFF, 0xFF, 0xFF);
    glSetDefaultState(false);
    glSetRasterState(GLS_DepthTest, 0);
    glSetRasterState(GLS_DepthWrite, 0);
    glSetRasterState(GLS_AlphaBlend, 2);   // ONE, ONE (glxSend.cpp)
    glSetCurrentRasterState(glHandleizeRasterState());
    glSetCurrentTexture(glGetTargetTexture(sHazeTarget), GLTT_Diffuse);
    glSetTextureState(GLTS_DiffuseWrap, 3);
    glSetCurrentTextureState(glHandleizeTextureState());
    poly.FullCoverage(white, 0.0f);
    float uv[4][2];
    if (sHazeAct == NPHAZE_HOLD && NpHazeCorners(inverse.e, sHazeThen, HazeHeight(), uv))
    {
        for (int i = 0; i < 4; i++)
        {
            nlVec2Set(poly.m_uv[i], uv[i][0], uv[i][1]);
        }
    }
    RLView* blend = GetLayerView(eCLV_WarbleBlend);
    sHazeFog = blend->mFogEnabled;
    blend->mFogEnabled = false;
    poly.Attach(blend, 0, 0);
}

void AmortizeAfterSend()
{
    if (sHazeAct != NPHAZE_LIVE)
    {
        GetLayerView(eCLV_WarbleBlend)->mFogEnabled = sHazeFog;
        sHazeAct = NPHAZE_LIVE;
    }
}
