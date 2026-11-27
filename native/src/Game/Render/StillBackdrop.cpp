#include "Game/Render/StillBackdrop.h"

#include "Game/BasicStadium.h"
#include "Game/FE/feManager.h"
#include "Game/FE/feModelManager.h"
#include "Game/GameInfo.h"
#include "Game/HBMManager.h"
#include "Game/NetworkDebug.h"
#include "Game/NetworkSession.h"
#include "Game/Render/RLView.h"
#include "Game/Render/RLViewLayers.h"
#include "Game/Render/StadiumLoading.h"
#include "Game/Render/Warble.h"
#include "Game/Task/FixedUpdateTask.h"
#include "Game/Task/GameRenderTask.h"
#include "Game/Task/GameTaskState.h"
#include "NL/gl/gl.h"
#include "NL/gl/glDraw2.h"
#include "NL/gl/glState.h"
#include "NL/gl/glTarget.h"
#include "NL/glx/glxTarget.h"
#include "NL/glx/glxTexture.h"
#include "NL/nlList.h"
#include "NL/nlTask.h"
#include "np/scenecut.h"
#include "np/stillback.h"

#include <stdio.h>
#include <stdlib.h>

// MSC_MENU_STILL, MSC_PAUSE_STILL: np/stillback.h has what they do and why nothing the simulation sees can change. Here: the screens they act on and the views.
//
// The views are sent in the order of eCLV. In a menu the world is 5..8 (NoFog, Shadowed, WorldShadowed, Unshadowed); the characters' pictures (0..4) must go on, and the sprites that
// show them (ImpostorOut, 10) stand in front of what comes before. So the copy is made and later drawn in view 9, MegastrikeBackground: the empty full-screen 2D view between the
// two (a match's Mega Strike background draws its full-screen quads there). What comes after it, glow included, is drawn on the copy every frame as it was on the world.
// Behind the pause menu everything up to the 2D of the menu is frozen, so the views 0..35 are left out and the place is view 36, ScreenGrab, which nothing else uses.
//
// The texture is the warble's colour target: it is full size, it exists anyway and a copy into it needs no game memory. The warble rewrites it, and a frame with warble is
// therefore never held and starts the count again.
extern "C" void VgxStillArm(const void* dest) __attribute__((weak));                 // renderer (vgx_draw.cpp): whose copies to report
extern "C" unsigned VgxStillDone(void) __attribute__((weak));                        // renderer (vgx_backend_gl.cpp)
extern "C" unsigned VgxStillFailed(void) __attribute__((weak));
extern "C" int VgxStillRoom(void) __attribute__((weak));
extern "C" float VgxRenderScale(void) __attribute__((weak));

namespace
{
const int cMenuFirst = eCLV_NoFog, cMenuLast = eCLV_Unshadowed;
const int cPauseFirst = eCLV_ImpostorTexture, cPauseLast = eCLV_ScreenBlur2;
const int cMenuSettle = 20, cPauseSettle = 8;   // frames: long enough for what fades in with a screen

NpStillState sState;
int sAct = NPSTILL_LIVE;
int sFirst, sLast;
RLView* sView;
GLRenderPair sViewPair;
unsigned long sViewTarget;
GLViewInterface* sViewInterface;
bool sViewFog;
bool sArmed;
unsigned sCount[3];
unsigned sStampChanges, sLastStamp;
int sLastAct = -1;

int MenuStillOn()
{
    static int v = -1;
    if (v < 0) v = NpStillParse(getenv("MSC_MENU_STILL"));
    return v;
}

int PauseStillOn()
{
    static int v = -1;
    if (v < 0) v = NpStillParse(getenv("MSC_PAUSE_STILL"));
    return v;
}

int LogOn()
{
    static int v = -1;
    if (v < 0) v = NpStillParse(getenv("MSC_STILL_LOG"));
    return v;
}

int StillTolOn()
{
    static int v = -1;
    if (v < 0) v = NpStillParse(getenv("MSC_STILL_TOL"));
    return v;
}

int StillRefresh()
{
    static int v = -1;
    if (v < 0)
    {
        const char* e = getenv("MSC_STILL_REFRESH");
        v = e && *e ? atoi(e) : 0;
        if (v < 0 || v > 8) v = 0;
    }
    return v;
}

// MSC_STILL_TOL: the stamp of the camera is the number of times it has moved by more than noise (np/stillback.h NpStillCameraMoved), not the bits of its matrices.
unsigned CameraStamp()
{
    if (StillTolOn())
    {
        static nlMatrix4 refView, refProj;
        static unsigned epoch;
        static bool have;
        nlMatrix4 v, p;
        sPerspectiveCamera.GetViewMatrix(v);
        sPerspectiveCamera.GetProjectionMatrix(p);
        if (!have || NpStillCameraMoved(refView.e, v.e, refProj.e, p.e))
        {
            refView = v;
            refProj = p;
            have = true;
            ++epoch;
        }
        return NpStillHash(2166136261u, &epoch, sizeof(epoch));
    }
    unsigned h = 2166136261u;
    nlMatrix4 m;
    sPerspectiveCamera.GetViewMatrix(m);
    h = NpStillHash(h, &m, sizeof(m));
    sPerspectiveCamera.GetProjectionMatrix(m);
    return NpStillHash(h, &m, sizeof(m));
}

// MSC_STILL_DIAG=1 (wave 7, diagnostic only, reads, no renderer needed): what changes the stamp of a menu world. Per 300 frames: how often the camera matrices, the world pointer and the set of
// characters changed, and for the first changes the size of the camera's step.
void StillDiag()
{
    static int on = -1;
    if (on < 0) on = NpStillParse(getenv("MSC_STILL_DIAG"));
    if (!on) return;
    const unsigned state = nlTaskManager::m_pInstance->mCurrentState;
    if (!(state == TASK_FRONTEND && IsStadiumWorldLoaded())) return;
    static nlMatrix4 lastView, lastProj;
    static unsigned lastCam = 0, lastWorld = 0, lastModels = 0, nFrames = 0, nCam = 0, nWorld = 0, nModels = 0, nAny = 0, nShown = 0;
    static bool have = false;
    nlMatrix4 v, pr;
    sPerspectiveCamera.GetViewMatrix(v);
    sPerspectiveCamera.GetProjectionMatrix(pr);
    unsigned cam = NpStillHash(NpStillHash(2166136261u, &v, sizeof(v)), &pr, sizeof(pr));
    const void* w = BasicStadium::GetCurrentStadium();
    unsigned world = NpStillHash(2166136261u, &w, sizeof(w));
    unsigned models = FEModelManager::Instance() != 0 ? FEModelManager::Instance()->StillStamp(2166136261u) : 0;
    if (have)
    {
        const bool c = cam != lastCam, wd = world != lastWorld, m = models != lastModels;
        nCam += c; nWorld += wd; nModels += m; nAny += (c || wd || m);
        if ((c || wd || m) && nShown < 60)
        {
            float dv = 0.0f, dp = 0.0f;
            for (int i = 0; i < 16; ++i)
            {
                float a = v.e[i] - lastView.e[i], b = pr.e[i] - lastProj.e[i];
                a = a < 0 ? -a : a; b = b < 0 ? -b : b;
                if (a > dv) dv = a;
                if (b > dp) dp = b;
            }
            ++nShown;
            printf("[stilldiag] frame change: camera %d (view step %g, projection step %g) world %d characters %d\n", c, dv, dp, wd, m);
        }
    }
    have = true; lastCam = cam; lastWorld = world; lastModels = models; lastView = v; lastProj = pr;
    static nlMatrix4 refView;
    if (nFrames == 0) refView = v;
    if (nFrames % 60 == 59)
    {
        float dr = 0.0f, dt = 0.0f;
        for (int i = 0; i < 16; ++i)
        {
            float a = v.e[i] - refView.e[i];
            a = a < 0 ? -a : a;
            if (i == 12 || i == 13 || i == 14) { if (a > dt) dt = a; } else if (a > dr) dr = a;
        }
        printf("[stilldiag] %u frames after the reference: view deviation rotation part %g, translation part %g (translation %g %g %g)\n", nFrames + 1, dr, dt, v.e[12], v.e[13], v.e[14]);
    }
    if (++nFrames >= 300)
    {
        printf("[stilldiag] 300 menu frames: stamp changed %u (camera %u, world %u, characters %u)\n", nAny, nCam, nWorld, nModels);
        nFrames = nCam = nWorld = nModels = nAny = 0;
    }
}

void HideTree(GLView* view)
{
    if (view->m_Visible)
    {
        view->m_Visible = false;
        view->m_StillHidden = 1;
    }
    nlListIterator<GLView*> child = view->m_Children.Begin();
    while (child.IsValid())
    {
        HideTree(child.Current());
        child.Next();
    }
}

void ShowTree(GLView* view)
{
    if (view->m_StillHidden)
    {
        view->m_StillHidden = 0;
        view->m_Visible = true;
    }
    nlListIterator<GLView*> child = view->m_Children.Begin();
    while (child.IsValid())
    {
        ShowTree(child.Current());
        child.Next();
    }
}

void Prepare(eCLV layer)
{
    sView = GetLayerView(layer);
    sViewPair = sView->GetRenderPair();
    sViewTarget = sView->m_Target;
    sViewInterface = sView->m_Interface;
    sViewFog = sView->mFogEnabled;
    sView->m_Interface = GetOrthoCamera();
    sView->mFogEnabled = false;
}

void DrawCopy()
{
    glPoly2 poly;
    nlColour white;
    nlColourSet(white, 0xFF, 0xFF, 0xFF, 0xFF);
    glSetDefaultState(false);
    glSetRasterState(GLS_DepthTest, 0);
    glSetRasterState(GLS_DepthWrite, 0);
    glSetCurrentRasterState(glHandleizeRasterState());
    glSetCurrentTexture(glGetTargetTexture(sWarbleColourTarget), GLTT_Diffuse);
    glSetTextureState(GLTS_DiffuseWrap, 3);
    glSetCurrentTextureState(glHandleizeTextureState());
    poly.FullCoverage(white, 0.0f);
    poly.Attach(sView, 0, 0);
}
} // namespace

void StillBackdropBeforeSend()
{
    sAct = NPSTILL_LIVE;
    sView = 0;
    StillDiag();
    if (!(MenuStillOn() || PauseStillOn()) || &VgxStillArm == 0 || &VgxStillDone == 0 || &VgxStillFailed == 0 || &VgxStillRoom == 0)
    {
        return;
    }

    const unsigned state = nlTaskManager::m_pInstance->mCurrentState;
    NpStillIn in;
    in.active = 0;
    in.stamp = 0;
    eCLV place = eCLV_MegastrikeBackground;
    int settle = cMenuSettle;
    if (!gWarbleEnabled && NpMenuLod() < 3)
    {
        if (MenuStillOn() && state == TASK_FRONTEND && IsStadiumWorldLoaded())
        {
            in.active = 1;
            sFirst = cMenuFirst;
            sLast = cMenuLast;
            in.stamp = CameraStamp();
            const void* world = BasicStadium::GetCurrentStadium();
            in.stamp = NpStillHash(in.stamp, &world, sizeof(world));
            if (FEModelManager::Instance() != 0)
            {
                // the world's floor shows the shadows of the captains: another captain is another picture
                in.stamp = FEModelManager::Instance()->StillStamp(in.stamp);
            }
        }
        else if (PauseStillOn() && state == TASK_PAUSED && FrontEnd::m_bInPauseMenuState && !GameInfoManager::Instance()->mIsOnlineMode
                 && g_pNetworkSessionBase->GetNumMachines() <= 1 && GetFixedUpdateTask()->mfFrameLockTime <= 0.0f)
        {
            in.active = 1;
            sFirst = cPauseFirst;
            sLast = cPauseLast;
            in.stamp = CameraStamp();
            place = eCLV_ScreenGrab;
            settle = cPauseSettle;
        }
    }
    if (in.active && !sArmed)
    {
        sArmed = true;
        VgxStillArm(sWarbleColourTarget.target->mTexture->m_SwizzledData);
    }
    in.scale = &VgxRenderScale != 0 ? (int)(VgxRenderScale() * 4096.0f) : 0;
    in.done = VgxStillDone();
    in.failed = VgxStillFailed();
    in.room = VgxStillRoom();
    sAct = NpStillStep(sState, in, settle, StillRefresh());

    if (LogOn())
    {
        sCount[sAct]++;
        sStampChanges += in.active && in.stamp != sLastStamp;
        sLastStamp = in.stamp;
        if (sAct != sLastAct)
        {
            printf("[still] %s: %s (scale %d, copies %u, failed %u, fails %d, room %d)\n", place == eCLV_ScreenGrab ? "pause" : "menu",
                sAct == NPSTILL_STILL ? "held" : (sAct == NPSTILL_CAPTURE ? "copy" : "live"), in.scale, in.done, in.failed, sState.fails, in.room);
            sLastAct = sAct;
        }
        if (sCount[0] + sCount[1] + sCount[2] >= 300)
        {
            printf("[still] 300 frames: %u live, %u copy, %u held, stamp changed %u times\n", sCount[0], sCount[1], sCount[2], sStampChanges);
            sCount[0] = sCount[1] = sCount[2] = 0;
            sStampChanges = 0;
        }
    }

    if (sAct == NPSTILL_LIVE)
    {
        return;
    }
    Prepare(place);
    if (sAct == NPSTILL_CAPTURE)
    {
        sView->SetRenderPair(sWarbleColourTarget);
        sView->m_Target = GLViewTarget_Mode8;
        return;
    }
    DrawCopy();
    for (int i = sFirst; i <= sLast; i++)
    {
        HideTree(GetLayerView((eCLV)i));
    }
}

void StillBackdropAfterSend()
{
    if (sView == 0)
    {
        return;
    }
    if (sAct == NPSTILL_STILL)
    {
        for (int i = sFirst; i <= sLast; i++)
        {
            ShowTree(GetLayerView((eCLV)i));
        }
    }
    sView->SetRenderPair(sViewPair);
    sView->m_Target = sViewTarget;
    sView->m_Interface = sViewInterface;
    sView->mFogEnabled = sViewFog;
    sView = 0;
}
