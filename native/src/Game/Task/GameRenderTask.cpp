#include "Game/Task/GameTaskState.h"
#include "np/gsplit.h"
#include "NL/nlDLListContainer.inl"
#include "Game/HBMManager.h"
#include "Game/Task/GameRenderTask.h"
#include "np/perfmode.h"
#include "np/fx.h"
#include "np/overlap.h"
#include "np/crowdgov.h"
#ifdef NP_NATIVE
#include "np/cpu22.h"
#include "np/cpu24.h"
#endif
#include "np/scenecut.h"
#include "np/stadiumcut.h"
#include "np/crowdcut.h"
#include "np/simorder17.h"
#include "Game/Camera/GameplayCam.h"

#include "types.h"

#include "Game/BasicStadium.h"
#include "Game/Render/ChainChomp.h"
#include "Game/Camera/CameraMan.h"
#include "Game/Character.h"
#include "Game/CharacterTemplate.h"
#include "Game/Debug/ShapeRender.h"
#include "Game/Drawable/DrawableCharacter.h"
#include "Game/Drawable/DrawableNetMesh.h"
#include "Game/Effects/EmissionManager.h"
#include "Game/Effects/PhotoFlashEffect.h"
#include "Game/FE/feRender.h"
#include "Game/GL/GLMovieMeshWriter.h"
#include "NL/glx/GXMovieMaterialProgram.h"
#include "Game/Game.h"
#include "Game/GameObjectLighting.h"
#include "Game/NisPlayer.h"
#include "Game/ObjectBlur.h"
#include "Game/Render/AttackSideIndicators.h"
#include "Game/Render/CrowdManager.h"
#include "Game/Render/ElectricFence.h"
#include "Game/Render/FlareHandler.h"
#include "Game/Render/ImpostorManager.h"
#include "Game/Render/Indicators.h"
#include "Game/Render/Jumbotron.h"
#include "Game/Render/Nis.h"
#include "Game/Render/Presentation.h"
#include <stdio.h>
#include "Game/Render/Wiper.h"
#include "Game/FE/feManager.h"
#include "Game/Render/RLView.h"
#include "Game/Render/Amortize.h"
#include "Game/Render/RenderShadow.h"
#include "np/amortize.h"
#include "Game/Render/ShootToScoreArrow.h"
#include "Game/Render/ShootToScoreMeter.h"
#include "Game/Render/depthoffield.h"
#include "Game/Render/CrowdImpostorManager.h"
#include "Game/Render/WorldNPC.h"
#include "Game/ReplayManager.h"
#include "Game/Task/FixedUpdateTask.h"
#include "Game/SharedStaticStorage.h"
#include "NL/MemAlloc.h"
#include "NL/gl/gl.h"
#include "NL/gl/glDraw2.h"
#include "NL/gl/glFont.h"
#include "NL/gl/glModel.h"
#include "NL/gl/glState.h"
#include "NL/gl/glTexture.h"
#include "NL/gl/glView.h"
#include "NL/glx/glxMemory.h"
#include "NL/glx/glxSend.h"
#include "NL/nlColour.h"
#include "NL/nlMemory.h"
#include "NL/nlString.h"
#include "NL/nlTask.h"
#include "NL/nlTicker.h"
#include "Game/Render/TimedObject.h"
#include "Game/Render/CrowdImpostors.h"
#include "Game/Render/MegastrikeBackgroundOverlay.h"
#include "Game/Render/MegaBallIndicators.h"
#include "Game/Render/PeachPhoto.h"
#include "Game/Render/NumberDisplay.h"
#include "Game/Render/Warble.h"
#include "NL/nlDebugViews.h"
#include "Game/Render/ShadowVolume.h"
#include "np/bg30.h"
#include "np/fps.h"

#include <string.h>

extern "C" void VgxMegaSceneHint(int on) __attribute__((weak)); // renderer (vgx_backend_gl.cpp)
extern "C" int VgxMegaSharp(void) __attribute__((weak));        // renderer (MSC_MEGA_SHARP): the level 0..2
extern "C" int VgxPpaaMode(void) __attribute__((weak));          // renderer (MSC_VGX_PPAA): 0..2
extern "C" void VgxPpaaCutHint(int on) __attribute__((weak));   // renderer: the Mega Strike cut-scene is drawn (the edge filter takes it as a Mega scene)
extern "C" void VgxMegaCutHint(int on) __attribute__((weak));   // renderer: the Mega Strike's launch / result cut-scene is drawn
extern "C" int VgxFgCrowdThrottle(void) __attribute__((weak));   // renderer (W12 FG): 2 while its crowd step is on

const nlVector3 gRenderUpVector = { 0.0f, 0.0f, 1.0f };

GameRenderTask* GameRenderTask::sInstance;

u8 g_bRenderWorldEffects = 1;
static u8 g_bTexelDensity;
static u8 g_bRestartWarble;
static u8 g_bWarblePreview;
static u8 g_bWarbleShow;
static float g_fWarbleBlobScale;
static float g_fWarbleDuration = 0.375f;
static float sWarbleTestParameter0 = 3.0f;
static float sWarbleTestParameter1 = 12.0f;
static float sWarbleTestParameter2 = 32.0f;
static float sWarbleTestParameter3 = 32.0f;
static float sWarbleTestParameter4 = 0.8125f;
static float sWarbleTestParameter5;
bool g_bRenderWorld = true;
static bool sWarbleParticleRenderingEnabled = true;
static bool g_bUpdateImpostorSprites = true;
static u8 g_bMemoryOnScreen;
static u8 g_bShowEmissionLights;

static void WarbleTest(float fDeltaT)
{
    glPoly2 p;

    if (g_bRestartWarble)
    {
        g_bRestartWarble = 0;
        ResetWarble(&gWarble);

        WarbleConfiguration config;
        nlZeroMemory(&config, sizeof(config));
        config.view = GetLayerView(eCLV_Unshadowed);
        config.position = g_pCharacters[0]->m_DetChar.m_v3Position;
        config.position.z = (1.0f / 128.0f)
            + BasicStadium::GetCurrentStadium()->m_shadowHeight;
        config.blobScale = g_fWarbleBlobScale;
        config.duration = g_fWarbleDuration;
        config.parameters[0] = sWarbleTestParameter0;
        config.parameters[1] = sWarbleTestParameter1;
        config.parameters[2] = sWarbleTestParameter2;
        config.parameters[3] = sWarbleTestParameter3;
        config.parameters[4] = sWarbleTestParameter4;
        config.parameters[5] = sWarbleTestParameter5;
        SetWarbleInstance(&gWarble, new (8, false) WarbleInstance(config));
    }

    nlColour white = { 0xFF, 0xFF, 0xFF, 0xFF };
    glSetDefaultState(false);
    glSetCurrentTexture(glGetTexture("target/warble"), GLTT_Diffuse);

    p.SetupRectangle(512.0f, 64.0f, 100.0f, 86.0f, 0.0f);
    p.SetColour(white);

    if (g_bWarblePreview)
    {
        p.Attach(GetLayerView(eCLV_FrontEnd), 0, 0);
    }

    u32 showTex = glGetTexture(g_bWarbleShow ? "target/offset" : "target/colour");
    glSetCurrentTexture(showTex, GLTT_Diffuse);
    glSetTextureState(GLTS_DiffuseWrap, 3);
    glSetCurrentTextureState(glHandleizeTextureState());

    p.FullCoverage(white, 0.0f);
    p.Attach(GetLayerView(eCLV_WarbleBlend), 0, 0);
}

static int sTriStripIndices[4] = { 3, 0, 2, 1 };
static int sQuadIndices[4] = { 0, 1, 2, 3 };

static void RenderImageQuad(GLView* view, unsigned long texture,
    const nlFloatColour& colour, const nlVector2* positions,
    const nlVector2* texcoords)
{
    GLMovieMeshWriter writer;
    int* indices;
    int primitive;

    if (glHasQuads())
    {
        indices = sQuadIndices;
        primitive = 3;
    }
    else
    {
        indices = sTriStripIndices;
        primitive = 1;
    }

    if (writer.Begin(4, primitive, 0))
    {
        memcpy(&((GXMovieParameters*)writer.model->packets->materialParameters)->tint, &colour,
            sizeof(nlFloatColour));

        glTextureBinding* state
            = &((GXMovieParameters*)writer.model->packets->materialParameters)->texture;
        state->texture = texture;
        state->textureIndex = 0xFFFF;
        state->SetWrapS(1);
        state->SetWrapT(1);
        state->unknown07 = 0;

        for (int n = 0; n < 2; n++)
        {
            int i0 = indices[0];
            writer.Texcoord(texcoords[i0]);
            writer.Vertex(positions[i0].x, positions[i0].y, 0.0f);
            int i1 = indices[1];
            writer.Texcoord(texcoords[i1]);
            writer.Vertex(positions[i1].x, positions[i1].y, 0.0f);
            indices += 2;
        }

        if (writer.End())
        {
            view->AttachModel(writer.model, 0);
        }
    }
}

void InstallImageRenderCallback()
{
    g_pfnRenderImage = RenderImageQuad;
}

void NpGt18Frame();                          // gt18 (src/NL/math.cpp): MSC_GT18_QUAT follows MSC_AB
extern "C" void NpBlendAsyncKick(int pip);   // gt27 (src/Game/ReplayManager.cpp)
extern "C" void NpBlendAsyncDrain();         // gt27 (src/Game/RenderSnapshot.cpp)
void GameRenderTask::Run(float fDeltaT)
{
    mElapsedTime += fDeltaT;
    NpOverlapFrame();
#ifdef NP_NATIVE
    NpCpu22Frame();
    NpCpu24Frame();
    NpGt18Frame();
#endif
    NpBg30Begin();   // (1.3 MSC_BG30: ordinary until RenderFrame decides)

    if (gpHBMManager != 0 && gpHBMManager->mActive && gpHBMManager->mReady)
    {
        return;
    }

    glx_Fog(true);
    ImpostorManager::GetInstance()->InvalidateCapture();
    DrawableNetMesh::Reset();

    if (TimedObjectManager::Instance() != 0)
    {
        TimedObjectManager::Instance()->Update(fDeltaT);
    }

    if (NisPlayer::Instance()->HasSecondaryNis())
    {
        RLView* save14F8;
        GLView* save19D8;
        int saveView;
        GLView* saveStadiumOpaqueView;
        GLView* saveStadiumAlphaView;
        GLView* saveShapeView;
        GLView* save132C;
        u32 save1330;
        RLView* pipView;
        RLView* pipAlphaView;
        void* saveContext;
        RLView* saveShadowView;

        pipView = GetLayerView(eCLV_PictureInPicture);
        pipAlphaView = GetLayerView(eCLV_PictureInPictureAlpha);
        saveStadiumOpaqueView = BasicStadium::GetCurrentStadium()->m_pOpaqueView;
        saveStadiumAlphaView = BasicStadium::GetCurrentStadium()->m_pAlphaView;
        save14F8 = gpChainChompShadowView;
        gpChainChompShadowView = pipView;
        saveShapeView = g_ShapeRenderer.m_eView;
        g_ShapeRenderer.m_eView = pipView;
        save132C = g_pNetMeshView;
        save1330 = g_NetMeshInvisiblePlaneView;
        BasicStadium::GetCurrentStadium()->m_pOpaqueView = pipView;
        BasicStadium::GetCurrentStadium()->m_pAlphaView = pipAlphaView;
        save19D8 = g_pNisRenderView;
        saveView = g_nCharacterView;
        g_pNetMeshView = pipAlphaView;
        g_NetMeshInvisiblePlaneView = save1330;
        g_pNisRenderView = pipView;
        g_nCharacterView = eCLV_PictureInPicture;
        saveShadowView = SetCharacterShadowView(pipView);

        NisPlayer::Instance()->Render(0);

        if (NisPlayer::Instance()->mOverlayMode == NIS_OVERLAY_HOLOTRON)
        {
            ReplayManager::Instance()->RenderSnapshotAt(0.0f);
        }
        else
        {
            saveContext = EmissionManager::Instance()->mContext;
            EmissionManager::Instance()->SetContext(pipAlphaView);
            EmissionManager::Instance()->Render();
            EmissionManager::Instance()->SetContext(saveContext);
            RenderFrame(fDeltaT, true);
        }

        g_pNisRenderView = save19D8;
        gpChainChompShadowView = save14F8;
        SetCharacterShadowView(saveShadowView);
        g_nCharacterView = saveView;
        g_pNetMeshView = save132C;
        g_NetMeshInvisiblePlaneView = save1330;
        g_ShapeRenderer.m_eView = saveShapeView;
        BasicStadium::GetCurrentStadium()->m_pOpaqueView = saveStadiumOpaqueView;
        BasicStadium::GetCurrentStadium()->m_pAlphaView = saveStadiumAlphaView;

        NisPlayer::Instance()->Render(1);
        RenderFrame(0.0f, false);
        NpBlendAsyncDrain();
    }
    else
    {
        NisPlayer::Instance()->Render(0);
        RenderFrame(fDeltaT, false);
        NpBlendAsyncDrain();   // gt27 (MSC_BLEND_ASYNC): a kicked blend that the drawing did not join ends here (no-op otherwise)
        NP_GS(NPGS_R_NUMBERS);
        gpNumberDisplay->Render();
    }
}

static inline void MemoryOnScreen()
{
    float x0;
    float x1;
    float y0;
    float y1;

    nlColour bg = { 0x3A, 0x6E, 0xA5, 0xFF };
    nlColour fg = { 0x00, 0x00, 0x00, 0xFF };

    int savedFont = glFontSetFont(0);

    glFontVirtualPosToScreenCoordPos(0.0f, -2.0f, x0, y0);
    glFontVirtualPosToScreenCoordPos(0.0f, 1.0f, x1, y1);

    g_ShapeRenderer.DrawRectangle2D(0.0f, y0 - 4.0f, 640.0f, 8.0f + (y1 - y0), -0.5f, bg, -1);

    glFontBegin(false);
    glFontPrintf(GetDebugFontView(), -1, -2, fg, "main : %u free, %u block",
        VirtualAllocator.TotalFreeMemory() >> 10, VirtualAllocator.LargestFreeBlock() >> 10);
    glFontPrintf(GetDebugFontView(), -1, -1, fg, "rc : %u free", static_cast<unsigned>(glx_GetFreeMemory() >> 10));
    glFontPrintf(GetDebugFontView(), -1, 0, fg, "intl : %u free, %u block",
        StandardAllocator.TotalFreeMemory() >> 10, StandardAllocator.LargestFreeBlock() >> 10);
    glFontEnd();

    glFontSetFont(savedFont);
}


// ---- mapi17 (MSC_SIMT_ORDER, np/simorder17.h): the draw window. World NPC draw (stage 0) and the crowd block (stages 1..6) used to sit inline in RenderFrame; they are the pieces of
// drawing that read nothing the simulation step writes (plan-simthread17 MAP B rows 10, 11), so they can run before the step (order B) or between its phases (the
// interleave test). The code of every stage is the old code unchanged; NpSimOrder17Flush (src/Game/NpSimOrder17.cpp) decides when each stage runs. Default order A: stage 0
// at the old NPC place, stages 1..6 at the old crowd place, i.e. exactly the old sequence.
namespace
{
struct CrowdWin17
{
    bool active;        // stage 1 found the crowd block wanted this picture
    bool pip;
    bool crowdSized;
    bool updateClusters;
};
CrowdWin17 sW17;
}

extern "C" int NpWin17Wanted(int stage, int pip, int captain)   // the pre-step form of each stage's own condition (only what the step cannot change): used by order B/I to decide whether to run early
{
    if (stage == 0)
    {
        return !captain;
    }
    if (stage == 1)
    {
        return g_bRenderWorldEffects != 0;   // the step only ever sets it to 1 (Goalie.cpp:3703, 3738): a stage skipped here is simply run at the old place, which sees the new value
    }
    return 1;
}

extern "C" void NpWin17Stage(int stage, int pip, int captain)
{
    const bool bPictureInPicture = pip != 0;
    switch (stage)
    {
    case 0:
        if (!captain)
        {
            WorldNPCManager* worldNPCs = gpWorldNPCManager;
            if (!NpPerformanceMode() && !NpStadiumHeavyCrowd() && !NpStadiumCutHas(NP_CUT_WORLDNPC))   // MSP Performance Mode (np/perfmode.h): the decorative NPCs are not drawn, they still update
            { NP_GS(NPGS_R_NPCS); worldNPCs->Render(GetLayerView(eCLV_MoreCharacters)); }
            else if (!NpPerformanceMode() && !NpStadiumHeavyCrowd()) NpStadiumCutCount(NP_CUT_WORLDNPC, 1);   // (MSC_STADIUM_CUT bit 16)
        }
        break;
    case 1:
        sW17.active = false;
        sW17.pip = bPictureInPicture;
        {
    // 1.3 MSC_BG30: a light picture does not draw the crowd (the renderer re-projects it with the rest of the kept background). Its positions and
    // sprites still update here: their refresh schedule and their draws of the shared random generator stay those of every picture (the host pictures
    // showed crowd sprites missing in the next full picture when the whole block was skipped). MSC_BG30_CROWDSKIP=1 skips the whole block (test).
    static const int sBg30CrowdSkip = [] { const char* e = getenv("MSC_BG30_CROWDSKIP"); return e && *e == '1' ? 1 : 0; }();
    if (g_bRenderWorldEffects && NpBg30Light() && sBg30CrowdSkip)
    {
        NpBg30NoteCrowdSkip();
    }
    else if (g_bRenderWorldEffects)
    {
        if (ImpostorManager::GetInstance()->mEnabled)
        {
            sW17.active = true;
            // MSC_CROWD_SIZED (np/crowdcut.h): under a camera that is not the gameplay one, the sprites are chosen by the size of the figures that show them (below)
            // (not on mHasClusters: AddCharacter runs in ImpostorCharacter's constructor, before ImpostorCluster's sets mIsCluster, so that flag is never true in this build, and MSC_CROWD_REPLAY's floor never applies)
            const bool crowdSized = NpCrowdSized() != 0 && ImpostorManager::GetInstance()->mUpdateClusters != 0;
            sW17.crowdSized = crowdSized;
            ImpostorManager::GetInstance()->SetSizedMode(crowdSized);
            NpCrowdSeedProbe();
            {
            NP_GS(NPGS_R_CROWDPOS);
            UpdateImpostorPositions();
            }
        }
    }
        }
        break;
    case 2:
        if (sW17.active)
        {
            NP_GS(NPGS_R_CROWDPOS);
            (GetCrowdImpostorManager())->UpdateCrowdVisibility(GetLayerView(sW17.pip ? eCLV_PictureInPicture : eCLV_ImpostorOut));
            if (sW17.crowdSized)
            {
                ImpostorManager::GetInstance()->MeasureSizes(GetLayerView(sW17.pip ? eCLV_PictureInPicture : eCLV_ImpostorOut));
            }
        }
        break;
    case 3:
        if (sW17.active)
        {
            // MSC_CROWD_CLUSTER_LAZY (np/scenecut.h): in a stadium with no reader of the cluster's picture (CrowdImpostors.cpp) the steps that exist for it stay out: the release
            // below only queues the sprites the cluster draws, the second visibility pass only rebuilds what the cluster's Render cleared. The picture is the same.
            const bool updateClusters = NpSceneClusterWanted(ImpostorManager::GetInstance()->mUpdateClusters, NpCrowdClusterLazy(), CrowdClusterReaders()) != 0;
            sW17.updateClusters = updateClusters;
            ImpostorManager::GetInstance()->SetSkipClusters(!updateClusters);
            NpCrowdNote(CrowdClusterReaders(), ImpostorManager::GetInstance()->mHasClusters, ImpostorManager::GetInstance()->mUpdateClusters, updateClusters, NpStadiumHeavyCrowd());
            if (updateClusters)
            {
                GetCrowdImpostorManager()->ReleaseCrowdImpostors();
            }
        }
        break;
    case 4:
        if (sW17.active)
        {
            const bool crowdSized = sW17.crowdSized;
            bool paused = nlTaskManager::m_pInstance->mCurrentState == TASK_NIS;
            ImpostorManager::GetInstance()->SetUpdateThrottle(VgxFgCrowdThrottle ? VgxFgCrowdThrottle() : 1);
            ImpostorManager::GetInstance()->mUseRenderCache = g_bUpdateImpostorSprites && !paused;
            // MSC_CROWD_REFRESH (np/overlap.h): under the gameplay camera, where UpdateSprites stops at the first cluster.
            // MSC_CROWD_REPLAY (np/scenecut.h): the other cameras, default the game's rate. A skipped capture is visual only; what it would have run, from the top:
            //   ResumeCapture/SuspendCapture: the sprite's view target. EvaluatePose: the model's own accumulator; cPN_SAnimController::Evaluate also writes m_fWeight, which nothing reads.
            //   ImpostorModel::Render: matrices of its own skin mesh, a frame copy of the model, packets into the sprite's view. mModelCallback is null: it is set on NIS and world NPC models only.
            //   ImpostorCluster::Render: ResetSpriteSlots and Release queue slots (Impostor::Release only appends to the sprite's mRenderSlots), and the second visibility pass below
            //   rebuilds those whether or not the cluster ran. ImpostorManager::Render(skipCapture): GL state, a scratch mesh, mFrameCount and sNumImpostorsRendered, which nothing reads.
            //   Random numbers: none on the crowd's path. The only one on a capture path is FEImpostorCharacter::Render, in the front end's list, which has no cluster and so no floor.
            //   Animation time and its events are in UpdateAnimations, which does not depend on this. And the renderer's crowd step (VgxFgCrowdThrottle) already reaches this path at 2.
            NpGsScope npGsSpr(NPGS_R_CROWDSPR);
            if (!ImpostorManager::GetInstance()->mUpdateClusters)
            {
                // wave 10 (MSC_BG30_BALANCE bit 1): the pair's crowd pose update runs in the LIGHT picture, once, at the game's own 30 floor (the same sprites a second as two updates at the 60 floor); the full
                // picture after it only draws. The per-stadium cut table can ask for the cheaper refresh floor (MSC_BG30_CUTS).
                const int crowdSlot = NpBg30CrowdSlot();
                if (crowdSlot != NPBG30_SLOT_SKIP)
                {
                    ImpostorManager::GetInstance()->SetCrowdGov(NpCrowdGovLevel(), GetLayerView(sW17.pip ? eCLV_PictureInPicture : eCLV_ImpostorOut));   // MSC_CROWD_GOV (np/crowdgov.h)
                    ImpostorManager::GetInstance()->UpdateSpritesWithFloor(NpCrowdAt60(NpStadiumCutCrowdFloor(NpCrowdRefresh(NpPerformanceMode() || NpStadiumHeavyCrowd() || NpBg30CutCrowdLow())), AmortizeCrowdSixty() && crowdSlot != NPBG30_SLOT_DO_PAIR));   // MSC_AMORT_60, MSC_CROWD_LIVE
                }
            }
            else if (crowdSized)
            {
                ImpostorManager::GetInstance()->UpdateSpritesSized();
            }
            else
            {
                const int crowdFloor = NpHeldGovCrowd(NpSceneCrowdFloor(1, ImpostorManager::GetInstance()->mHasClusters, NpCrowdReplay()), NpHeldGovLevel());   // (scenes30 MSC_HELD_GOV level 1: one picture more, held scenes over budget only)
                if (crowdFloor > 1)
                {
                    ImpostorManager::GetInstance()->UpdateSpritesWithFloor(crowdFloor);
                }
                else
                {
                    ImpostorManager::GetInstance()->UpdateSprites();
                }
            }
        }
        break;
    case 5:
        if (sW17.active && sW17.updateClusters)
        {
            NP_GS(NPGS_R_CROWDPOS);
            (GetCrowdImpostorManager())->UpdateCrowdVisibility(GetLayerView(sW17.pip ? eCLV_PictureInPicture : eCLV_ImpostorOut));
        }
        break;
    case 6:
        if (sW17.active)
        {
            NP_GS(NPGS_R_CROWDDRAW);
            if (NpBg30Light()) NpBg30NoteCrowdSkip();   // (1.3 MSC_BG30: a light picture: the crowd quads are not drawn)
            else if (!NpPerformanceMode() && !NpStadiumHeavyCrowd())   // MSP Performance Mode (np/perfmode.h): the crowd impostor pass is skipped, the sprites still update
            ImpostorManager::GetInstance()->Render(
                GetLayerView(sW17.pip ? eCLV_PictureInPicture : eCLV_ImpostorOut), false);
            sW17.active = false;
        }
        break;
    }
}

extern "C" int NpFrameLockStepOn();   // src/Game/NpSimThread.cpp
extern "C" void NpFrameLockCount(int moved);

void GameRenderTask::RenderFrame(float fDeltaT, bool bPictureInPicture)
{
    static unsigned int lastTick;
    static s8 init;
    static float prevTime;
    NP_GS(NPGS_R_PRE);

    glPoly2 p;
    float w;
    float h;

    bool bCaptainShot = false;
    if (nlTaskManager::m_pInstance->mCurrentState == TASK_GAMEPLAY && g_pGame->mbCaptainShotToScoreOn)
    {
        bCaptainShot = true;
    }

    if (!bPictureInPicture)
    {
        switch (nlTaskManager::m_pInstance->mCurrentState)
        {
        case TASK_REPLAY:
        case TASK_NIS:
            if (gPeachPhotoState.state == PEACH_PHOTO_ACTIVE)
            {
                EndPeachPhoto(&gPeachPhotoState, true);
            }
            break;
        default:
            RenderPeachPhoto(&gPeachPhotoState);
            break;
        }
    }

    NisPlayer::Instance()->fn_8027E5D0();

    if (g_bShowEmissionLights)
    {
        for (int lightIndex = 0; lightIndex < GetEmissionManager()->GetNumLights(); lightIndex++)
        {
            EffectsLight* light = GetEmissionManager()->GetLight(lightIndex);
            nlColour lightColour;
            lightColour.c[0] = light->m_Colour.c[0];
            lightColour.c[1] = light->m_Colour.c[1];
            lightColour.c[2] = light->m_Colour.c[2];
            lightColour.c[3] = 0x40;
            g_ShapeRenderer.DrawSphere(light->m_v3Position, lightColour, light->m_fRadius);
        }
    }

    if (g_bMemoryOnScreen)
    {
        MemoryOnScreen();
    }

    UpdateGameObjectLighting();
    ClearCharacterShadowsUpdated();

    if (!g_bRenderWorld)
    {
        return;
    }

    if (NpBg30Mode() != 0)
    {
        // 1.3 MSC_BG30 (np/bg30.h): live play under the gameplay camera only; reads only
        NpBg30In in;
        in.eligible = 0;
        in.why = 0;
        cBaseCamera* top = cCameraManager::PeekCamera();
        if (bPictureInPicture || NisPlayer::Instance()->HasSecondaryNis()) in.why = NPBG30_WHY_PIP;
        else if (nlTaskManager::m_pInstance->mCurrentState != TASK_GAMEPLAY) in.why = NPBG30_WHY_STATE;
        else if (top == 0 || top->GetType() != eCameraType_Gameplay) in.why = NPBG30_WHY_CAMERA;
        else if ((int)cCameraManager::m_transition != 0) in.why = NPBG30_WHY_TRANSITION;
        else if (gGameplayCameraInReplay) in.why = NPBG30_WHY_REPLAYCAM;
        else if (gMegastrikeBackgroundOverlay.mActive) in.why = NPBG30_WHY_MEGA;
        else if (bCaptainShot) in.why = NPBG30_WHY_CAPTAIN;
        else in.eligible = 1;
        {   // lessons17 (Galaxy's Game Over strip, MSC_BG30_SCREENS=1, default 0): a picture made while the state is about to change, a wipe is running or the match is over is never a light one (the game-over wipe and the
            // frames before the state switch run under TASK_GAMEPLAY with the gameplay camera on top). Counted either way as "would exclude" so the console log shows whether they occur.
            const bool screen = FrontEnd::m_bGameOver || nlTaskManager::m_pInstance->mPendingState != nlTaskManager::m_pInstance->mCurrentState || Wiper::Instance().WipeInProgress();
            if (screen && in.eligible)
            {
                static unsigned long sWould = 0;
                if (++sWould == 1 || sWould % 600 == 0) fprintf(stderr, "[bg30] %lu eligible pictures so far were made on a game-over / state-change / wipe screen (MSC_BG30_SCREENS=%d: %s)\n", sWould, NpBg30ScreensStrict(), NpBg30ScreensStrict() ? "made ordinary" : "left as they were");
                if (NpBg30ScreensStrict()) { in.eligible = 0; in.why = NPBG30_WHY_SCREEN; }
            }
        }
        memcpy(in.view, &cCameraManager::m_matView, sizeof in.view);
        in.fovDeg = cCameraManager::m_fFOV;
        in.rate60 = NpFpsPairControlled() ? NpFpsPairWanted() != 0 : NpFpsDisplayInterval() != 2;   // (a held 30: every picture full; with MSC_PAIR_RATE=1 the pair controller says when)
        NpBg30Decide(&in);
    }
    bool bFrameLocked = GetFixedUpdateTask()->mfFrameLockTime > 0.0f;
#ifdef NP_NATIVE
    if (bFrameLocked && NpFrameLockStepOn())   // wave 10 (MSC_FRAMELOCK_STEP): the simulation task counts the lock down (FixedUpdateTask::Run)
    {
        NpFrameLockCount(0);
        bFrameLocked = false;
    }
#endif
    if (bFrameLocked)
    {
        FixedUpdateTask::DecrementFrameLock(fDeltaT);
    }

    NpBlendAsyncKick(bPictureInPicture ? 1 : 0);   // gt27 (MSC_BLEND_ASYNC, default off): the snapshot blend starts on the helper lane, joined in RenderSnapshot::Blend
    NpSimOrder17Flush(0, bPictureInPicture, bCaptainShot);   // mapi17: the window's world NPC stage (stage 0), here unless MSC_SIMT_ORDER=B/I already ran it before the step

    NpStadiumCutFrame();   // (MSC_STADIUM_CUT periodic report)

    u32 whiteTexture = glGetTexture("global/white");

    float dt = (nlTaskManager::m_pInstance->mCurrentState == TASK_PAUSED) ? 0.0f : fDeltaT;

    {
    NP_GS(NPGS_R_STAGE);
    Jumbotron::instance.Update(dt);
    CrowdManager::instance.Update(dt);
    FlareHandler::instance.Render();
    UpdateWarble(&gWarble, dt);
    RenderWarble(&gWarble);
    UpdateAttackSideIndicators();
    }

    for (int charIndex = 0; charIndex < 10; charIndex++)
    {
        if (g_pCharacters[charIndex] != 0)
        {
            g_pCharacters[charIndex]->UpdateBlinking(dt);
        }
    }

    if (g_bTexelDensity)
    {
        if (glTextureLoad(whiteTexture))
        {
            w = (float)(s32)glTextureGetWidth();
            h = (float)(s32)glTextureGetHeight();
            p.SetupRectangle(24.0f, 24.0f, w, h, 0.0f);

            WorldDarkening::SetPolyColour(p, 0xFF, 0xFF, 0xFF, 0xFF);

            glSetDefaultState(false);
            glSetCurrentTexture(whiteTexture, GLTT_Diffuse);
            glSetTextureState(GLTS_DiffuseWrap, 3);
            glSetCurrentTextureState(glHandleizeTextureState());
            p.Attach(GetLayerView(eCLV_FrontEnd), 0, 0);
        }
    }

    if (g_bRenderWorldEffects)
    {
        NP_GS(NPGS_R_NPCS);
        RenderWorldNPCs();
    }
    UpdateHighRange();

    if (nlTaskManager::m_pInstance->mCurrentState != TASK_FRONTEND)
    {
        NP_GS(NPGS_R_REPLAYSNAP);
        ReplayManager::Instance()->RenderSnapshotAt(fDeltaT);
    }
    else
    {
        nlVector3 up = gRenderUpVector;
        cCameraManager::m_UpVectorStack[cCameraManager::m_UpVectorStackSize] = up;
    }

    u32 currState = nlTaskManager::m_pInstance->mCurrentState;
    {
        // (Vita port, W12 OVERDRAW) the renderer's census and its per-scene choices want to know what kind of scene this frame is
        const bool paused = currState == TASK_PAUSED;
        const u32 st = paused ? nlTaskManager::m_pInstance->mPreviousState : currState;
        if ((currState & (TASK_FE_TO_GAME | TASK_BOOT_TO_GAME)) == 0) NpTexPretouchStep(currState == TASK_FRONTEND ? 0 : ((currState & (TASK_FE_TO_GAME | TASK_BOOT_TO_GAME)) != 0 ? 1 : (currState == TASK_GAMEPLAY ? 2 : 3)), currState);   // (MSC_TEX_PRETOUCH: textures shown to the renderer ahead of their first draw)
        NpFxSetScene(st == TASK_REPLAY || (st == TASK_GAMEPLAY && gGameplayCameraInReplay) ? 2 : (st == TASK_GAMEPLAY ? 1 : (st == TASK_NIS ? 3 : 0)));
        if (!bPictureInPicture) NpBg30Emit();   // (1.3 MSC_BG30: the picture's kind and camera, in stream order)
    }
    if (currState == TASK_GAMEPLAY
        || (currState == TASK_PAUSED && nlTaskManager::m_pInstance->mPreviousState == TASK_GAMEPLAY))
    {
        NP_GS(NPGS_R_HUD);
        if (g_pGame->mbCaptainShotToScoreOn)
        {
            float indicatorDt;
            if (currState == TASK_GAMEPLAY)
            {
                indicatorDt = fDeltaT;
            }
            else
            {
                indicatorDt = 0.0f;
            }
            UpdateAndRenderMegaBallIndicators(indicatorDt);
        }
        else
        {
            float indicatorDt;
            if (currState == TASK_GAMEPLAY)
            {
                indicatorDt = fDeltaT;
            }
            else
            {
                indicatorDt = 0.0f;
            }
            UpdateAndRenderIndicators(indicatorDt);
        }

        ShootToScoreMeter::instance.UpdateAndRender(fDeltaT);
        gMegastrikeBackgroundOverlay.UpdateAndRender(fDeltaT);
        // (Vita port) the renderer draws Mega Strike scenes at a smaller internal size while this is reported
        if (gMegastrikeBackgroundOverlay.mActive && VgxMegaSceneHint) VgxMegaSceneHint(1);
    }
    else if (currState == TASK_NIS)
    {
        if (!init)
        {
            lastTick = nlGetTicker();
            init = 1;
        }

        u32 tick = nlGetTicker();
        nlGetTickerDifference(lastTick, tick);
        lastTick = tick;
    }

    {
    NP_GS(NPGS_R_SHADOW);
    WorldDarkening::Instance().Update(fDeltaT);
    WorldDarkening::Instance().Render();
    RenderShadowVolumeBlend(GetLayerView(eCLV_ShadowVolumeBlend));
    }
    // (Vita port, MSC_MEGA_SHARP) the Mega Strike's own cut-scenes (the captain's launch, the result: the presentation functions "MegastrikeIntro" and
    // "MegastrikeEnd"). Level 1: the renderer keeps the ball's edge whole there (cut hint: the nearest-depth copy of the effects pass only, the scene stays a
    // cut-scene). Level 2: they also count as Mega scenes (full held picture, one sample). Draw only: the game reads nothing back, the simulation is the same.
    if (VgxMegaSharp)
    {
        const int sharp = VgxMegaSharp();
        if (sharp >= 1)
        {
            Presentation* presentation = GetPresentation();
            if (presentation != NULL && strncmp(presentation->mCurrentFunction, "Megastrike", 10) == 0)
            {
                if (VgxMegaCutHint) VgxMegaCutHint(1);
                if (sharp >= 2 && VgxMegaSceneHint) VgxMegaSceneHint(1);
            }
        }
    }
    // (Vita port, MSC_VGX_PPAA, aa24) the same cut-scenes for the edge filter of the picture pass. Draw only: the game reads nothing back.
    if (VgxPpaaMode && VgxPpaaCutHint && VgxPpaaMode() >= 1)
    {
        Presentation* presentation = GetPresentation();
        if (presentation != NULL && strncmp(presentation->mCurrentFunction, "Megastrike", 10) == 0) VgxPpaaCutHint(1);
    }
    NP_GS(NPGS_R_FX);
    DepthOfFieldManager::instance.Update();

    if ((nlTaskManager::m_pInstance->mCurrentState & 0x00020008) == 0)
    {
        BlurManager::Update(fDeltaT);
    }

    if (glTextureLoad(glGetTexture("target/warble")))
    {
        WarbleTest(fDeltaT);
    }

    if (g_bRenderWorldEffects)
    {
        PhotoFlash::Render(fDeltaT);
        UpdateElectricFence(fDeltaT);
    }

    NpSimOrder17Flush(1, bPictureInPicture, bCaptainShot);   // mapi17: the window's crowd stages (1..6), here unless already run before the step
}

void SetRenderWorldEffects(unsigned int enabled)
{
    if (enabled == g_bRenderWorldEffects)
    {
        return;
    }
    g_bRenderWorldEffects = enabled;
    if (enabled == 0)
    {
        EmissionManager* manager = EmissionManager::Instance();
        manager->mRenderPersistentOnly = 1;
    }
    else
    {
        EmissionManager* manager = EmissionManager::Instance();
        manager->mRenderPersistentOnly = 0;
    }
}

bool IsWarbleParticleRenderingEnabled()
{
    return sWarbleParticleRenderingEnabled;
}

