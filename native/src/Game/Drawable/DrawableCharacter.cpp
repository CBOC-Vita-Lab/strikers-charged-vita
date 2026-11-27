#include "np/render_neon.h"
#include "np/snap_onecopy.h"
#include "Game/Task/GameTaskState.h"
#include "NL/nlDLListContainer.inl"
#include "Game/Drawable/DrawableCharacter.h"
#include "np/overlap.h"
#include "Game/Render/StadiumLoading.h"
#include "Game/GameObjectLighting.h"
#include "Game/Character.h"
#include "Game/CharacterTemplate.h"
#include "Game/Player.h"
#include "Game/DB/CharacterInfo.h"
#include "Game/Render/PeachPhoto.h"

#include "Game/BasicStadium.h"
#include "Game/CharacterEffects.h"
#include "Game/AI/HeadTrack.h"
#include "Game/GameInfo.h"
#include "Game/GL/ShaderSkinMesh.h"
#include "Game/PoseAccumulator.h"
#include "Game/PoseNode.h"
#include "Game/Render/RLView.h"
#include "Game/Render/Amortize.h"
#include "Game/Render/RenderShadow.h"
#include "np/amortize.h"
#include "np/livetrades.h"
#include "Game/Render/SkinAnimatedNPC.h"
#include "Game/Render/WorldNPC.h"
#include "Game/SHierarchy.h"
#include "Game/Team.h"
#include "NL/gl/glModel.h"
#include "NL/gl/glState.h"

#include "NL/gl/glMaterialParameters.h"
#include "NL/glx/GXCharacterDamageMaterialProgram.h"
#include "NL/glx/glxTexture.h"
#include "NL/nlMemory.h"
#include "NL/nlString.h"
#include "NL/nlTask.h"
#include "NL/gl/glTexture.h"
#include "Game/SharedStaticStorage.h"
#include "np/scenecut.h"
#include "np/lod.h"
#include "Game/Render/MegastrikeBackgroundOverlay.h"
#include <stdio.h>
#include <stdlib.h>
#ifdef NP_NATIVE
extern "C" int VgxHoldShadowStep(void) __attribute__((weak));
extern "C" int VgxHoldShadowStepFor(int key) __attribute__((weak));   // renderer (MSC_PICTURE_HOLD): the step in force for this player; a change reaches the players one after another over about 3 s    // renderer (MSC_PICTURE_HOLD): 0, 1 or 2
extern "C" int VgxLock60ShadowStep(void) __attribute__((weak));   // renderer (MSC_LOCK60): 0, 1 or 2 frames added to a visible player's shadow interval
#endif

static int g_nOnscreenUpdate[3] = { 2, 2, 2 };
static int g_nOffscreenUpdate[3] = { 4, 4, 4 };

float g_fAlphaValueOverride = 1.0f;
u8 g_bRenderScorchPass = 1;
float g_fRightPropSpineOffsetX = 0.25f;
float g_fRightPropSpineOffsetY = -0.22f;
float g_fRightPropSpineOffsetZ = -0.16f;
float g_fRightPropSpinePitch = -98.0f;
float g_fRightPropSpineYaw = -132.0f;
float g_fRightPropSpineRoll = 60.0f;
float g_fLeftPropSpineOffsetX = 0.26f;
float g_fLeftPropSpineOffsetY = -0.26f;
float g_fLeftPropSpineOffsetZ = 0.12f;
float g_fLeftPropSpinePitch = 60.0f;
float g_fLeftPropSpineYaw = 130.0f;
float g_fLeftPropSpineRoll = 28.0f;
int g_nPacketBIndex = 8;
int g_nCharacterView = eCLV_Characters;
float g_fDetailBlendAmount = 0.25f;
static float g_fRadiusScale = 1.175f;

static unsigned long LightTexture = glGetTexture("global/lightramp");
static unsigned long BlackTexture = glGetTexture("global/black");
static unsigned long WhiteTexture = glGetTexture("global/white");
int g_nLeftPropJointIndex;
int g_nRightPropJointIndex;
int g_nSpine1JointIndex;
float g_fForceDamage1;
float g_fForceDamage2;
float g_fMegaBlendOverride;
u8 g_bForceShockModel;
u8 g_bForceSoftwareSkinning;
u8 g_bForceLeftPropOnSpine;
u8 g_bForceRightPropOnSpine;
u8 g_bIgnoreFrozenHammerTransform;
u8 g_bForceHidePacketA;
u8 g_bForceHidePacketB;
int g_nPacketAIndex;

unsigned char DrawableCharacter::sShadowRenderingDisabled;
cCharacter* DrawableCharacter::spRenderOnlyThisCharacter = 0;
bool DrawableCharacter::sbRenderOpposingGoalieToo = false;
bool DrawableCharacter::sSTSLighting = false;
bool DrawableCharacter::sCameraRelativeLighting = false;

static inline void BlendTranslationAccum(
    TransAccum& output,
    const TransAccum& lhs,
    const TransAccum& rhs,
    float weight)
{
    output.fAccumulatedWeight = 1.0f;
    if (lhs.bIdentity && rhs.bIdentity)
    {
        output.bIdentity = true;
        nlVec3Set(output.t, 0.0f, 0.0f, 0.0f);
    }
    else
    {
        output.bIdentity = false;
        NpRVecLerp(output.t, lhs.t, rhs.t, weight);
    }
}

static inline void BlendScaleAccum(
    ScaleAccum& output,
    const ScaleAccum& lhs,
    const ScaleAccum& rhs,
    float weight)
{
    output.fAccumulatedWeight = 1.0f;
    if (lhs.bIdentity && rhs.bIdentity)
    {
        output.bIdentity = true;
        nlVec3Set(output.s, 1.0f, 1.0f, 1.0f);
    }
    else
    {
        output.bIdentity = false;
        NpRVecLerp(output.s, lhs.s, rhs.s, weight);
    }
}

DrawableCharacter::DrawableCharacter()
{
    visible = true;
    useObject = false;
    leftPropAnimated = true;
    rightPropAnimated = true;
    hammerTransformFrozen = false;
    packetAVisible = true;
    packetBVisible = true;
    useShockModel = false;
    facingDirection = 0;
    headSpin = 0;
    headTilt = 0;
    poseCulled = 0;
    height = 0.0f;
    scale = 1.0f;
    blendAmount = 1.0f;
    megaBlend = 0.0f;
    shadowLevel = 1.0f;
    object = 0;
    poseAccumulator = 0;
    effectsTexturing = 0;
    character = 0;
    damage1 = 0.0f;
    damage2 = 0.0f;
    damageType = DAMAGE_TEXTURE_STADIUM;
    savedScorchTexture = 0xFFFFFFFF;
    scorchTexture = 0;
    position.x = 0.0f;
    position.y = 0.0f;
    position.z = 0.0f;
    bip01Position.x = 0.0f;
    bip01Position.y = 0.0f;
    bip01Position.z = 0.0f;
    headPosition.x = 0.0f;
    headPosition.y = 0.0f;
    headPosition.z = 0.0f;
    velocity.x = 0.0f;
    velocity.y = 0.0f;
    velocity.z = 0.0f;
    frozenHammerRotation.z = 0.0f;
    frozenHammerRotation.y = 0.0f;
    frozenHammerRotation.x = 0.0f;
    frozenHammerRotation.w = 1.0f;
    frozenHammerTranslation.x = 0.0f;
    frozenHammerTranslation.y = 0.0f;
    frozenHammerTranslation.z = -10.0f;
    frozenHammerScale = 1.0f;
}

DrawableCharacter::~DrawableCharacter()
{
    delete poseAccumulator;
}

void DrawableCharacter::Free()
{
    delete poseAccumulator;
    poseAccumulator = 0;
}

cPN_SAnimController& DrawableCharacter::GetAnimController() const
{
    return *character->m_pCurrentAnimController;
}

void DrawableCharacter::Grab(cCharacter& source)
{
    character = &source;
    position = source.m_DetChar.m_v3Position;
    bip01Position = source.GetJointPosition(source.m_nBip01JointIndex_0xA4);
    headPosition = source.GetJointPosition(source.m_nHeadJointIndex);
    height = bip01Position.z;
    scale = source.m_DetChar.m_fPlayerScale;
    leftPropAnimated = source.m_bLeftPropAnimated;
    rightPropAnimated = source.m_bRightPropAnimated;
    hammerTransformFrozen = source.m_bHammerTransformFrozen;
    if (hammerTransformFrozen)
    {
        frozenHammerTranslation = source.m_v3FrozenHammerTranslation;
        frozenHammerRotation = source.m_qFrozenHammerRotation;
        frozenHammerScale = source.m_fFrozenHammerScale;
    }
    packetAVisible = source.m_bPacketAVisible;
    packetBVisible = source.m_bPacketBVisible;
    useShockModel = source.m_ModelType == CHAR_MODEL_SHOCK;
    blendAmount = source.m_fOpacity;
    megaBlend = source.m_fMegaBlend;
    shadowLevel = source.m_bShadowVisible ? 1.0f : 0.0f;
    shadowLevel *= blendAmount;
    velocity = source.m_DetChar.m_v3Velocity;
    facingDirection = source.m_DetChar.m_aActualFacingDirection;
    headSpin = (unsigned short)source.m_pHeadTrack->m_fHeadSpin;
    headTilt = (unsigned short)source.m_pHeadTrack->m_fHeadTilt;
    visible = true;
    useObject = source.m_pPoseAccumulator->m_bUseObject;
    damage1 = source.GetDirt();
    damage2 = source.GetMinDirt();
    damageType = source.m_nDamageType;
    if (!useObject)
    {
        object = source.m_pPoseTree;
    }

    if (poseAccumulator == 0)
    {
        cPoseAccumulator* p = (cPoseAccumulator*)nlMalloc(sizeof(cPoseAccumulator), 8, false);
        p = new (p) cPoseAccumulator(*source.m_pPoseAccumulator);
        poseAccumulator = p;
    }
    else
    {
        const int oneCopy = NpSnapOneCopy();   // MSC_SNAP_ONECOPY
        if (oneCopy != 0)
            poseAccumulator->CopyForSnapshot(*source.m_pPoseAccumulator, oneCopy);
        else
            *poseAccumulator = *source.m_pPoseAccumulator;
    }

    if (g_bIgnoreFrozenHammerTransform == 0 && hammerTransformFrozen)
    {
        nlMatrix4 matrix;
        nlQuatToMatrix(matrix, frozenHammerRotation, true);
        for (int row = 0; row < 3; ++row)
        {
            for (int column = 0; column < 3; ++column)
            {
                matrix.e2[row][column] *= frozenHammerScale;
            }
        }
        matrix.SetTranslation(frozenHammerTranslation);

        poseAccumulator->m_NodeMatrices[g_nRightPropJointIndex] = matrix;
    }

    EffectsTexturing* tex = source.m_pEffectsTexturing;
    if (tex == 0)
    {
        tex = fxGetTexturing(eFXTex_Nothing);
    }
    effectsTexturing = tex;
}

static void DrawableCharacterHeadTrackCallback(unsigned int ctx, unsigned int, cPoseAccumulator* poseAccumulator, unsigned int currentNodeIndex, int)
{
    DrawableCharacter* drawableChar = (DrawableCharacter*)ctx;
    CalcHeadTrackMatrix(drawableChar->headSpin, drawableChar->headTilt, poseAccumulator, currentNodeIndex);
}

void DrawableCharacter::BuildNodeMatrices(cPoseAccumulator* accumulator)
{
    nlMatrix4 worldMatrix;
    float angle = 0.0000958738f * (float)facingDirection;
    nlMakeRotationMatrixZ(worldMatrix, angle);
    worldMatrix.SetTranslation(position);

    if (character != 0)
    {
        accumulator->SetBuildNodeMatrixCallback(character->m_nHeadJointIndex, DrawableCharacterHeadTrackCallback, (unsigned int)this, 0);
    }

    accumulator->BuildNodeMatricesRender(worldMatrix);   // a drawable character's pose is a render pose (MSC_RENDER_NEON)

    if (character != 0)
    {
        accumulator->SetBuildNodeMatrixCallback(character->m_nHeadJointIndex, 0, 0, 0);
    }
}

void DrawableCharacter::BuildNpcMatrix()
{
    nlMatrix4 worldMatrix;
    float angle = 0.0000958738f * (float)facingDirection;
    nlMakeRotationMatrixZ(worldMatrix, angle);
    worldMatrix.SetTranslation(position);
    poseAccumulator->BuildNodeMatricesFromQuaternions(worldMatrix);
}

#ifdef NP_NATIVE
// MSC_REPLAY_FARLOD (np/scenecut.h): the mesh choice of the character being drawn, made once so that the pose and the draw agree
static bool sDrawLowPoly;
static bool sDrawFlatShadow;   // MSC_SHADOW_DEADMESH (from the plan)
static int sDrawLevel;      // MSC_LOD_SIZE: 0 full, 1 lower mesh, 2 built L2
static int sDrawScene;

// MSC_LOD_SIZE (np/lod.h): the level of a character by the projected size of its bounding sphere in the view it is drawn in. Render thread only; reads the camera
// matrices and the character's position, writes nothing the simulation reads. Returns false when the character is too small to draw.
static bool ChooseLodLevel(const cCharacter& source, const nlVector3& position, bool* lowPoly)
{
    sDrawLevel = 0;
    sDrawScene = NpLodSceneOf(nlTaskManager::m_pInstance->mCurrentState);
    if (NpLodMode() == 0 || sDrawScene == NP_LOD_SCENE_OTHER)
        return true;
    nlMatrix4 view, proj;
    GetShadowedView()->m_Interface->GetViewMatrix(view);
    GetShadowedView()->m_Interface->GetProjectionMatrix(proj);
    const float depth = -(position.x * view.e2[0][2] + position.y * view.e2[1][2] + position.z * view.e2[2][2] + view.e2[3][2]);
    const float radius = source.m_DetChar.m_eCharacterClass == 3 ? 1.4f : 1.0f;
    const float px = NpLodProjectedPx(radius, depth, proj.e2[1][1]);
    int cull = 0;
    sDrawLevel = NpLodChoose(&source, sDrawScene, px, &cull);
    static const bool trace = getenv("MSC_LOD_TRACE") != 0;   // (once: this runs for every character of every frame)
    if (trace)
        fprintf(stderr, "[lod-trace] scene %d char %p class %d depth %.2f px %.1f level %d cull %d\n", sDrawScene, (const void*)&source, source.m_DetChar.m_eCharacterClass, depth, px, sDrawLevel, cull);
    *lowPoly = sDrawLevel >= 1;
    return !cull;
}

static bool ChooseLowPoly(const nlVector3& position)
{
    const unsigned long state = nlTaskManager::m_pInstance->mCurrentState;
    const float cutoff = NpSceneFarLod();
    float distanceSquared = 0.0f;
    if (cutoff > 0.0f && (state & (TASK_REPLAY | TASK_NIS)) != 0)
    {
        nlMatrix4 inverseView;
        GetShadowedView()->m_Interface->GetInverseViewMatrix(inverseView);
        const float dx = position.x - inverseView.m41;
        const float dy = position.y - inverseView.m42;
        const float dz = position.z - inverseView.m43;
        distanceSquared = dx * dx + dy * dy + dz * dz;
    }
    return NpSceneUseLowPoly(state == TASK_GAMEPLAY, (state & (TASK_REPLAY | TASK_NIS)) != 0, cutoff, distanceSquared) != 0;
}

// MSC_SHADOW_FAR, MSC_PICTURE_HOLD (np/amortize.h NpShadowExtraFrames): frames added to the interval of this player's shadow picture. Render thread; reads the camera and the position.
static int ShadowExtraFrames(const nlVector3& position, int key)   // key: the character's shadow partition index (stable; 0..10). It was a pointer bit, (this >> 5) & 7, which can put every player on the same step
{
    static const int farOn = NpAmortEnvInt("MSC_SHADOW_FAR", 0);
    const int step = VgxHoldShadowStepFor ? VgxHoldShadowStepFor(key & 7) : (VgxHoldShadowStep ? VgxHoldShadowStep() : 0);
    if (farOn <= 0 && step <= 0)
        return 0;
    static const float farPx = (float)NpAmortEnvInt("MSC_SHADOW_FAR_PX", 60);
    nlMatrix4 view, proj;
    GetShadowedView()->m_Interface->GetViewMatrix(view);
    GetShadowedView()->m_Interface->GetProjectionMatrix(proj);
    const float depth = -(position.x * view.e2[0][2] + position.y * view.e2[1][2] + position.z * view.e2[2][2] + view.e2[3][2]);
    const float px = NpLodProjectedPx(1.0f, depth, proj.e2[1][1]);
    return NpShadowExtraFrames(farOn, NpShadowIsFar(px, farPx), step);
}
#endif

#ifdef NP_NATIVE
// MSC_SKIN_POSE_PAR: the pose of one skin mesh, made now or (plan.defer) listed for the lanes; the order of the calls is the serial order
static inline void NpPoseOrDefer(cCharacter& source, cPoseAccumulator* accumulator, NpRenderPlan& plan, int modelType)
{
    if (plan.defer)
        plan.types[plan.nPose++] = (signed char)modelType;
    else
        source.PoseSkinMesh(accumulator, modelType);
}
#endif

// The old Render, up to the poses: false where it returned. Everything it decides is kept in `plan` for RenderDraw.
bool DrawableCharacter::RenderDecide(cCharacter& source, NpRenderPlan& plan)
{
    plan.nPose = 0;
    plan.shock = false;
    plan.flatShadow = false;
    if (!visible)
    {
        return false;
    }

    if (spRenderOnlyThisCharacter != 0
        && spRenderOnlyThisCharacter != &source
        && !sbRenderOpposingGoalieToo)
    {
        source.fn_8001C574();
        return false;
    }

    bool renderShockModel = false;
    if (useShockModel
        || (g_bForceShockModel != 0 && source.fn_8001C534(1)))
    {
        renderShockModel = true;
    }

#ifdef NP_NATIVE
    // MSC_POSE_CULL (np/overlap.h): SendToGl would return at its frustum test, and the skin meshes are read by nothing else
    if (NpPoseCullLevel() != 0)
    {
        const bool drawn = !poseCulled && WillBeDrawn(source, bip01Position);
        NpOverlapPose(!drawn);
        if (!drawn)
        {
            source.fn_8001C574();
            return false;
        }
    }
#endif

    if (renderShockModel)
    {
        NpPoseOrDefer(source, poseAccumulator, plan, 1);
    }

#ifdef NP_NATIVE
    sDrawLowPoly = ChooseLowPoly(bip01Position);
    if (!ChooseLodLevel(source, bip01Position, &sDrawLowPoly))
    {
        source.fn_8001C574();
        return false;
    }
#endif
    if (!renderShockModel || g_bRenderScorchPass != 0)
    {
#ifdef NP_NATIVE
        if (sDrawLowPoly)
#else
        if (nlTaskManager::m_pInstance->mCurrentState == TASK_GAMEPLAY)
#endif
        {
            NpPoseOrDefer(source, poseAccumulator, plan, 2);
        }
        else
        {
            NpPoseOrDefer(source, poseAccumulator, plan, 0);
        }
    }
#ifdef NP_NATIVE
    // MSC_SHADOW_DEADMESH (nxvis25, exact): in live play with MSC_SHADOW_LIVE=flat RenderCharacterShadow draws a flat ellipse from the position alone and drops the shadow mesh's model:
    // its pose, PrepareToRender and model copy (8-10 characters a picture) are dead work. The test is the one RenderCharacterShadow makes at its flat early return.
    static const bool deadMesh = [] { const char* e = getenv("MSC_SHADOW_DEADMESH"); return !(e && *e == '0'); }();
    plan.flatShadow = deadMesh && nlTaskManager::m_pInstance->mCurrentState == TASK_GAMEPLAY && !gMegastrikeBackgroundOverlay.mActive && NpShadowLiveFlat();
    if (!plan.flatShadow)
#endif
    NpPoseOrDefer(source, poseAccumulator, plan, 3);
    plan.shock = renderShockModel;
#ifdef NP_NATIVE
    plan.lowPoly = sDrawLowPoly;
    plan.level = sDrawLevel;
    plan.scene = sDrawScene;
#endif
    return true;
}

void DrawableCharacter::Render(cCharacter& source)
{
    NpRenderPlan plan;
    plan.defer = false;
    if (RenderDecide(source, plan))
        RenderDraw(source, plan);
}

void DrawableCharacter::RenderDraw(cCharacter& source, const NpRenderPlan& plan)
{
    const bool renderShockModel = plan.shock;
#ifdef NP_NATIVE
    sDrawLowPoly = plan.lowPoly;
    sDrawLevel = plan.level;
    sDrawFlatShadow = plan.flatShadow;
    sDrawScene = plan.scene;
#endif
    cCharacter* renderOnly = spRenderOnlyThisCharacter;
    if (renderOnly == 0 || renderOnly == &source
        || (sbRenderOpposingGoalieToo
            && &source == (cCharacter*)((cPlayer*)renderOnly)->m_pTeam->GetOtherTeam()->GetGoalie()))
    {
        if (renderShockModel)
        {
            if (g_bRenderScorchPass != 0)
            {
                SendToGl(source, 1);
            }
            SendToGl(source, 2);
        }
        else
        {
            SendToGl(source, 0);
        }
    }

    source.fn_8001C574();
}

// SendToGl's frustum test: `bip01` is the position it will test
bool DrawableCharacter::WillBeDrawn(const cCharacter& source, const nlVector3& bip01) const
{
    if (nlTaskManager::m_pInstance->mCurrentState == TASK_NIS)
    {
        return true;
    }
    if (IsStadiumWorldLoaded())
    {
        float fRadius;
        if (source.m_DetChar.m_eCharacterClass == 3)
        {
            fRadius = 3.5f;
        }
        else
        {
            fRadius = 2.5f;
        }
        return ClassifySphereInFrustum(
            GetShadowedView()->m_Interface->GetShadowMatrix(), &bip01, fRadius) != 0;
    }
    return true;
}

void DrawableCharacter::SendToGl(cCharacter& source, int renderPass)
{
    GLSkinMesh* skinMesh;
    int characterClass = source.m_DetChar.m_eCharacterClass;
    int view = g_nCharacterView;
    if (gPeachPhotoState.state == PEACH_PHOTO_ACTIVE)
    {
        view = eCLV_MoreCharacters;
        if (source.m_bCaughtInPhoto)
        {
            view = eCLV_Characters;
        }
    }
    if (characterClass == 5 && source.m_fMegaBlend > 0.0f)
    {
        view = eCLV_HighRange3D;
    }

    if (renderPass != 2)
    {
#ifdef NP_NATIVE
        if (sDrawLowPoly)
#else
        if (nlTaskManager::m_pInstance->mCurrentState == TASK_GAMEPLAY)
#endif
        {
            skinMesh = source.GetSkinMesh(2);
        }
        else
        {
            skinMesh = source.GetSkinMesh(0);
        }
    }
    else
    {
        skinMesh = source.GetSkinMesh(1);
    }

    if (sSTSLighting)
    {
        SetGameObjectLightingMode(OBJECT_LIGHTING_SHOOT_TO_SCORE);
    }
    else if (sCameraRelativeLighting || AlwaysUseCameraRelativeCharacterLighting())
    {
        SetGameObjectLightingMode(OBJECT_LIGHTING_CAMERA_RELATIVE);
    }
    else
    {
        SetGameObjectLightingMode(OBJECT_LIGHTING_IN_GAME);
    }

    if (!WillBeDrawn(source, bip01Position))
    {
        return;
    }

    skinMesh->m_Unknown0C = 1;
    if (g_bForceSoftwareSkinning
        || (nlTaskManager::m_pInstance->mCurrentState & 0x18) != 0
        || (nlTaskManager::m_pInstance->mCurrentState & 8) != 0
        || (nlTaskManager::m_pInstance->mCurrentState & 0x20000) != 0)
    {
        skinMesh->m_Unknown0C = 0;
    }
    skinMesh->PrepareToRender();
    glModel* pModel = glModelDupNoStreams(skinMesh->GetModel(), false, 0);
#ifdef NP_NATIVE
    if (renderPass != 2 && NpLodMode() != 0 && sDrawScene != NP_LOD_SCENE_OTHER)
    {
        const bool hasLow = source.fn_8001C534(2);
        if (NpLodMode() >= 2 && sDrawLevel >= 1 && !(sDrawLevel == 1 && hasLow))
            NpLodApply(pModel, skinMesh->pModel, sDrawLevel == 1 ? 1 : 2);
        NpLodNote(sDrawScene, sDrawLevel, pModel, source.GetSkinMesh(0)->pModel);
    }
#endif
    ApplyDamageEffects(source, pModel, renderPass);
    bool attachEffects = false;
    ApplyMaterialEffects(source, pModel, (eCharacterRenderPass)renderPass, &attachEffects);
    if (attachEffects)
    {
        source.PerformBlinking(skinMesh, pModel);
    }

    static u32 alphaValueHash = nlStringLowerHash("alphaValue");
    if (characterClass == 10)
    {
        int numPackets = pModel->numPackets;
        if ((g_bForceHidePacketA || !packetAVisible) && g_nPacketAIndex < numPackets)
        {
            glSetMaterialFloatParameter(&pModel->packets[g_nPacketAIndex], alphaValueHash, 0.0f);
        }
        if ((g_bForceHidePacketB || !packetBVisible) && g_nPacketBIndex < numPackets)
        {
            glSetMaterialFloatParameter(&pModel->packets[g_nPacketBIndex], alphaValueHash, 0.0f);
        }
    }

    AttachModelToLayerView((eCLV)view, pModel, 0);
    if (characterClass == 5)
    {
        view = eCLV_MoreCharacters;
    }

    GLSkinMesh* shadowMesh = source.GetSkinMesh(3);
    if (shadowMesh != skinMesh
#ifdef NP_NATIVE
        && !sDrawFlatShadow
#endif
        )
    {
        shadowMesh->m_Unknown0C = 1;
        if (g_bForceSoftwareSkinning
            || (nlTaskManager::m_pInstance->mCurrentState & 0x18) != 0
            || (nlTaskManager::m_pInstance->mCurrentState & 8) != 0
            || (nlTaskManager::m_pInstance->mCurrentState & 0x20000) != 0)
        {
            shadowMesh->m_Unknown0C = 0;
        }
        shadowMesh->PrepareToRender();
        pModel = glModelDupNoStreams(shadowMesh->GetModel(), false, 0);
    }
    RenderCharacterShadow(source, pModel, view);
}

void DrawableCharacter::Grab(SkinAnimatedNPC& npc)
{
    position = npc.mv3Position;
    nlMatrix4& nodeMatrix = npc.mpPoseAccumulator->GetNodeMatrix(0);
    height = nodeMatrix.m43;
    facingDirection = npc.maFacingDirection;
    object = npc.mpPoseTree;
    visible = npc.mbIsVisible;

    if (poseAccumulator == 0)
    {
        cPoseAccumulator* p = (cPoseAccumulator*)nlMalloc(sizeof(cPoseAccumulator), 8, false);
        p = new (p) cPoseAccumulator(*npc.mpPoseAccumulator);
        poseAccumulator = p;
    }
    else
    {
        *poseAccumulator = *npc.mpPoseAccumulator;
    }
}

void DrawableCharacter::Render(SkinAnimatedNPC& npc)
{
    if (!visible)
    {
        return;
    }

    nlMatrix4 worldMatrix;
    float angle = 0.0000958738f * (float)facingDirection;
    nlMakeRotationMatrixZ(worldMatrix, angle);

    worldMatrix.SetRow_(3, position);

    npc.mbIsVisible = visible;
    npc.RenderFromReplay(*poseAccumulator, &worldMatrix);
}

void DrawableCharacter::Blend(const float* blendFactors, DrawableCharacter& lhs, DrawableCharacter& rhs)
{
    if (NpPrefetchOn(2))
    {
        // the two snapshots come from the replay history (cold: written many pictures ago) and the target from the last picture
        NpPrefetchLines(&lhs, 256, 0);
        NpPrefetchLines(&rhs, 256, 0);
        NpPrefetchLines(this, 256, 1);
    }
    const float rhsWeight = *blendFactors;
    const float lhsWeight = 1.0f - rhsWeight;

    visible = lhs.visible && rhs.visible;
    bool normalBlend = false;
    bool isHammerBro = false;
    character = lhs.character;

    if (character != 0)
    {
        if (character->m_eClassType == FIELDER)
        {
            if (character->m_DetChar.m_eCharacterClass == HAMMERBROS)
            {
                isHammerBro = true;
                if (g_nLeftPropJointIndex <= 0)
                {
                    cSHierarchy* hierarchy = lhs.poseAccumulator->m_BaseSHierarchy;
                    g_nLeftPropJointIndex = hierarchy->GetNodeIndexByID(nlStringLowerHash("bip01 l prop"));
                    hierarchy = lhs.poseAccumulator->m_BaseSHierarchy;
                    g_nRightPropJointIndex = hierarchy->GetNodeIndexByID(nlStringLowerHash("bip01 r prop"));
                    hierarchy = lhs.poseAccumulator->m_BaseSHierarchy;
                    g_nSpine1JointIndex = hierarchy->GetNodeIndexByID(nlStringLowerHash("bip01 spine1"));
                }

                hammerTransformFrozen = rhs.hammerTransformFrozen;
                if (hammerTransformFrozen)
                {
                    frozenHammerTranslation = rhs.frozenHammerTranslation;
                    frozenHammerRotation = rhs.frozenHammerRotation;
                    frozenHammerScale = rhs.frozenHammerScale;
                }
                leftPropAnimated = rhs.leftPropAnimated;
                rightPropAnimated = rhs.rightPropAnimated;
            }
            else
            {
                hammerTransformFrozen = false;
                leftPropAnimated = true;
                rightPropAnimated = true;
            }
            packetAVisible = rhs.packetAVisible;
            packetBVisible = rhs.packetBVisible;
        }

        damage1 = lhs.damage1;
        damage2 = lhs.damage2;
        damageType = lhs.damageType;
        nlVecLerp(bip01Position, lhs.bip01Position, rhs.bip01Position, rhsWeight);
        nlVecLerp(headPosition, lhs.headPosition, rhs.headPosition, rhsWeight);
        velocity = lhs.velocity;
        scale = lhs.scale * lhsWeight + rhsWeight * rhs.scale;
        blendAmount = lhs.blendAmount * lhsWeight + rhsWeight * rhs.blendAmount;
        useShockModel = lhs.useShockModel;
        megaBlend = lhs.megaBlend * lhsWeight + rhsWeight * rhs.megaBlend;
        shadowLevel = lhs.shadowLevel * lhsWeight + rhsWeight * rhs.shadowLevel;
        effectsTexturing = lhs.effectsTexturing;

        if (!lhs.useObject && !rhs.useObject)
        {
            int headSpinOffset = (short)(rhsWeight * (float)(short)(rhs.headSpin - lhs.headSpin));
            headSpin = lhs.headSpin + headSpinOffset;
            normalBlend = true;
            int headTiltOffset = (short)(rhsWeight * (float)(short)(rhs.headTilt - lhs.headTilt));
            headTilt = lhs.headTilt + headTiltOffset;
            object = 0;
        }
    }
    else
    {
        object = 0;
        normalBlend = true;
        scale = 1.0f;
    }

    nlVecLerp(position, lhs.position, rhs.position, rhsWeight);
    int facingOffset = (short)(rhsWeight * (float)(short)(rhs.facingDirection - lhs.facingDirection));
    facingDirection = lhs.facingDirection + facingOffset;
    height = lhs.scale * lhsWeight + rhsWeight * rhs.scale;

    if (poseCulled)   // (MSC_POSE_CULL=2, RenderSnapshot::Blend decided it on the game thread: everything above is what the frame reads of this character)
    {
        return;
    }

    if (poseAccumulator == 0)
    {
        poseAccumulator = new (nlMalloc(sizeof(cPoseAccumulator), 8, false)) cPoseAccumulator(lhs.poseAccumulator->m_BaseSHierarchy, false);
    }

    poseAccumulator->m_Scale = scale;
    poseAccumulator->InitAccumulators();

    for (int i = 0; i < 20; i++)
    {
        float lhsMorphWeight = lhs.poseAccumulator->m_MorphWeights[i];
        poseAccumulator->m_MorphWeights[i] += lhsMorphWeight * lhsWeight;
        float rhsMorphWeight = rhs.poseAccumulator->m_MorphWeights[i];
        poseAccumulator->m_MorphWeights[i] += rhsMorphWeight * rhsWeight;
    }

    if (normalBlend)
    {
        for (int i = 0; i < poseAccumulator->GetNumNodes(); i++)
        {
            RotAccum& lhsRot = lhs.poseAccumulator->m_rot[i];
            RotAccum& rhsRot = rhs.poseAccumulator->m_rot[i];
            float lhsRotAroundZWeight = lhsRot.rotAroundZAccumulatedWeight * lhsWeight;
            float rhsRotAroundZWeight = rhsRot.rotAroundZAccumulatedWeight * rhsWeight;
            poseAccumulator->BlendRotAroundZ(i, lhsRot.rotAroundZ, lhsRotAroundZWeight);
            poseAccumulator->BlendRotAroundZ(i, rhsRot.rotAroundZ, rhsRotAroundZWeight);
            float lhsQuatWeight = lhsRot.quatAccumulatedWeight * lhsWeight;
            float rhsQuatWeight = rhsRot.quatAccumulatedWeight * rhsWeight;
            poseAccumulator->BlendRotRender(i, &lhsRot.q, lhsQuatWeight);
            poseAccumulator->BlendRotRender(i, &rhsRot.q, rhsQuatWeight);

            BlendTranslationAccum(
                poseAccumulator->m_trans[i],
                lhs.poseAccumulator->m_trans[i],
                rhs.poseAccumulator->m_trans[i],
                rhsWeight);
            BlendScaleAccum(
                poseAccumulator->m_scale[i],
                lhs.poseAccumulator->m_scale[i],
                rhs.poseAccumulator->m_scale[i],
                rhsWeight);
        }

        BuildNodeMatrices(poseAccumulator);
    }
    else
    {
        if (!lhs.useObject)
        {
            BuildNodeMatrices(lhs.poseAccumulator);
        }
        else if (!rhs.useObject)
        {
            BuildNodeMatrices(rhs.poseAccumulator);
        }

        for (int i = 0; i < poseAccumulator->GetNumNodes(); i++)
        {
            NpRQuatNLerp(
                poseAccumulator->GetNodeQuaternion(i),
                lhs.poseAccumulator->GetNodeQuaternion(i),
                rhs.poseAccumulator->GetNodeQuaternion(i),
                rhsWeight);
            BlendTranslationAccum(
                poseAccumulator->m_trans[i],
                lhs.poseAccumulator->m_trans[i],
                rhs.poseAccumulator->m_trans[i],
                rhsWeight);
            BlendScaleAccum(
                poseAccumulator->m_scale[i],
                lhs.poseAccumulator->m_scale[i],
                rhs.poseAccumulator->m_scale[i],
                rhsWeight);
        }

        BuildNpcMatrix();
    }

    if (isHammerBro)
    {
        if (!g_bIgnoreFrozenHammerTransform && hammerTransformFrozen)
        {
            nlMatrix4 matrix;
            nlQuatToMatrix(matrix, frozenHammerRotation, true);
            for (int row = 0; row < 3; ++row)
            {
                for (int column = 0; column < 3; ++column)
                {
                    matrix.e2[row][column] *= frozenHammerScale;
                }
            }
            matrix.SetTranslation(frozenHammerTranslation);
            poseAccumulator->m_NodeMatrices[g_nRightPropJointIndex] = matrix;
        }
        else if (g_bForceRightPropOnSpine || !rightPropAnimated)
        {
            nlMatrix4 rotation;
            nlMakeRotationMatrixEulerAngles(
                rotation,
                DegreesToRadians(g_fRightPropSpinePitch),
                DegreesToRadians(g_fRightPropSpineYaw),
                DegreesToRadians(g_fRightPropSpineRoll));
            rotation.SetRow4_(3, g_fRightPropSpineOffsetX, g_fRightPropSpineOffsetY, g_fRightPropSpineOffsetZ, 1.0f);
            nlMatrix4 matrix;
            nlMultMatrices(matrix, rotation, poseAccumulator->GetNodeMatrix(g_nSpine1JointIndex));
            poseAccumulator->m_NodeMatrices[g_nRightPropJointIndex] = matrix;
        }

        if (g_bForceLeftPropOnSpine || !leftPropAnimated)
        {
            nlMatrix4 rotation;
            nlMakeRotationMatrixEulerAngles(
                rotation,
                DegreesToRadians(g_fLeftPropSpinePitch),
                DegreesToRadians(g_fLeftPropSpineYaw),
                DegreesToRadians(g_fLeftPropSpineRoll));
            rotation.SetRow4_(3, g_fLeftPropSpineOffsetX, g_fLeftPropSpineOffsetY, g_fLeftPropSpineOffsetZ, 1.0f);
            nlMatrix4 matrix;
            nlMultMatrices(matrix, rotation, poseAccumulator->GetNodeMatrix(g_nSpine1JointIndex));
            poseAccumulator->m_NodeMatrices[g_nLeftPropJointIndex] = matrix;
        }
    }
}

void DrawableCharacter::EvaluateFrom(const cPoseNode& poseNode, const nlVector3& offset, unsigned short facingAngle, float poseScale)
{
    position = offset;
    nlVec3Set(velocity, 0.0f, 0.0f, 0.0f);
    facingDirection = facingAngle;
    headSpin = 0;
    headTilt = 0;
    height = 0.0f;
    scale = poseScale;
    leftPropAnimated = false;
    rightPropAnimated = false;
    hammerTransformFrozen = false;
    packetAVisible = false;
    packetBVisible = false;
    useShockModel = false;
    blendAmount = 1.0f;
    megaBlend = 0.0f;
    damage1 = character->GetDirt();
    damage2 = character->GetMinDirt();
    damageType = character->m_nDamageType;
    shadowLevel = 1.0f;

    poseAccumulator->m_Scale = poseScale;
    poseAccumulator->InitAccumulators();

    poseNode.Evaluate(1.0f, poseAccumulator);

    effectsTexturing = fxGetTexturing(eFXTex_Nothing);

    BuildNodeMatrices(poseAccumulator);

    nlMatrix4& bip01Matrix = poseAccumulator->GetNodeMatrix(character->m_nBip01JointIndex_0xA4);
    bip01Position = bip01Matrix.GetTranslation();

    nlMatrix4& headMatrix = poseAccumulator->GetNodeMatrix(character->m_nHeadJointIndex);
    headPosition = headMatrix.GetTranslation();
}

nlVector3 DrawableCharacter::GetBallPosition()
{
    cPlayer* pPlayer = (cPlayer*)character;
    nlMatrix4& matrix = poseAccumulator->GetNodeMatrix(pPlayer->m_nBallJointIndex);
    return matrix.GetTranslation();
}

nlQuaternion DrawableCharacter::GetBallOrientation()
{
    nlQuaternion result;
    nlMatrix4 matrix;
    int ballJointIndex = ((cPlayer*)character)->m_nBallJointIndex;

    if (1.0f != scale)
    {
        matrix = poseAccumulator->GetNodeMatrix(ballJointIndex);

        float reciprocalLength = nlRecipSqrt(nlVec3LengthSquared(*(nlVector3*)&matrix.e2[0][0]), true);
        nlVec3Scale(*(nlVector3*)&matrix.e2[0][0], reciprocalLength);

        reciprocalLength = nlRecipSqrt(nlVec3LengthSquared(*(nlVector3*)&matrix.e2[1][0]), true);
        nlVec3Scale(*(nlVector3*)&matrix.e2[1][0], reciprocalLength);

        reciprocalLength = nlRecipSqrt(nlVec3LengthSquared(*(nlVector3*)&matrix.e2[2][0]), true);
        nlVec3Scale(*(nlVector3*)&matrix.e2[2][0], reciprocalLength);

        nlMatrixToQuat(result, matrix);
    }
    else
    {
        nlMatrixToQuat(result, poseAccumulator->GetNodeMatrix(ballJointIndex));
    }

    return result;
}

void DrawableCharacter::RenderOnlyOneCharacter(cCharacter& character, bool renderOpposingGoalieToo)
{
    spRenderOnlyThisCharacter = &character;
    sbRenderOpposingGoalieToo = renderOpposingGoalieToo;
}

void DrawableCharacter::RenderAllCharacters()
{
    spRenderOnlyThisCharacter = 0;
    sbRenderOpposingGoalieToo = false;
}

cCharacter* DrawableCharacter::OnlyRenderingOneCharacter()
{
    return spRenderOnlyThisCharacter;
}

static inline void ApplyTexture(glModel* model, unsigned long texture, ResolvedTexture resolvedTexture)
{
    for (glModelPacket* packet = model->packets; packet < model->packets + model->numPackets; packet++)
    {
        if (texture != glGetMaterialUnsignedParameter(packet, gDiffuseTextureSemantic))
        {
            glSetMaterialTextureParameter(packet, gDiffuseTextureSemantic, texture);
            unsigned long packetTexture = resolvedTexture.value;
            glSetMaterialTextureIndexParameter(packet, gDiffuseTextureSemantic, &packetTexture);
        }
    }
}

void DrawableCharacter::ApplyMaterialEffects(const cCharacter& source, glModel* model, eCharacterRenderPass renderPass, bool* attachEffects)
{
    static u32 blendAmountHash = nlStringLowerHash("blendAmount");

    EffectsTexturing* fxtex = effectsTexturing;
    int characterClass = source.m_DetChar.m_eCharacterClass;
    u32 texture;
    glModelPacket* pPacket;

    if (fxtex != 0 && fxtex->m_uTexture == 0xFFFFFFFF)
    {
        fxtex = 0;
    }
    if (fxtex != 0 && fxtex->m_bDetail)
    {
        fxtex = 0;
    }

    if (fxtex != 0)
    {
        for (glModelPacket* pFxPacket = model->packets; pFxPacket < model->packets + model->numPackets; pFxPacket++)
        {
            unsigned int& raster = pFxPacket->rasterState;
            if (fxtex->m_eBlendMode != GLB_None)
            {
                glSetRasterState(raster, GLS_AlphaBlend, fxtex->m_eBlendMode);
            }

            if (fxtex->m_bDetail)
            {
                glSetMaterialTextureParameter(pFxPacket, gDetailTextureSemantic, fxtex->m_uTexture);
                unsigned long resolvedTexture = fxtex->m_ResolvedTexture.value;
                glSetMaterialTextureIndexParameter(pFxPacket, gDetailTextureSemantic, &resolvedTexture);
                glSetMaterialFloatParameter(pFxPacket, blendAmountHash, g_fDetailBlendAmount);
            }
            else
            {
                glSetMaterialTextureParameter(pFxPacket, gDiffuseTextureSemantic, fxtex->m_uTexture);
                unsigned long resolvedTexture = fxtex->m_ResolvedTexture.value;
                glSetMaterialTextureIndexParameter(pFxPacket, gDiffuseTextureSemantic, &resolvedTexture);
            }
        }
    }
    else
    {
        texture = source.m_uNormalTextureID;
        if (renderPass == CRP_Default)
        {
            if (texture != source.m_uSwapTextureID)
            {
                for (pPacket = model->packets; pPacket < model->packets + model->numPackets; pPacket++)
                {
                    if (texture == glGetMaterialUnsignedParameter(pPacket, gDiffuseTextureSemantic))
                    {
                        glSetMaterialTextureParameter(pPacket, gDiffuseTextureSemantic, source.m_uSwapTextureID);
                        unsigned long resolvedTexture = source.m_ResolvedSwapTexture.value;
                        glSetMaterialTextureIndexParameter(pPacket, gDiffuseTextureSemantic, &resolvedTexture);
                    }
                }
            }
            *attachEffects = true;
        }
        else if (renderPass == CRP_Scorch)
        {
            if (scorchTexture == 0)
            {
                scorchTexture = glGetTexture("global/black");
                resolvedScorchTexture.value = glGetTextureManager()->GetTextureIndex(scorchTexture);
            }
            ApplyTexture(model, scorchTexture, resolvedScorchTexture);
        }
        else if (renderPass == CRP_Shock)
        {
            if (characterClass != 0)
            {
                ApplyTexture(model, source.m_uShockTextureID, source.m_ResolvedShockTexture);
            }
        }
    }
}

void DrawableCharacter::ApplyDamageEffects(const cCharacter& source, glModel* model, int renderPass)
{
    glModelPacket* pPacket;
    static u32 shadowLevelHash = nlStringLowerHash("shadowLevel");

    int shadowAlpha = GetShadowLookupLevel(&bip01Position);
    float fade = 1.0f;
    if (bip01Position.z > fade)
    {
        if (bip01Position.z > 10.0f)
        {
            fade = 0.0f;
        }
        else
        {
            fade = (10.0f - bip01Position.z) / 9.0f;
        }
    }

    nlColour shadowColour;
    u8 channel = 0xFF - (int)((float)(0xFF - shadowAlpha) * fade);
    shadowColour.c[3] = 1;
    shadowColour.c[0] = channel;
    shadowColour.c[1] = channel;
    shadowColour.c[2] = channel;
    unsigned long shadowColourValue = *(u32*)&shadowColour;

    pPacket = model->packets;
    while (pPacket < model->packets + model->numPackets)
    {
        glSetMaterialUnsignedParameter(pPacket, shadowLevelHash, shadowColourValue);
        pPacket++;
    }

    static u32 alphaValueHash = nlStringLowerHash("alphaValue");
    float alphaValue = 1.0f != g_fAlphaValueOverride ? g_fAlphaValueOverride : blendAmount;
    if (alphaValue != 1.0f)
    {
        for (pPacket = model->packets; pPacket < model->packets + model->numPackets; pPacket++)
        {
            glSetMaterialFloatParameter(pPacket, alphaValueHash, alphaValue);
        }
    }

    static u32 megaBlendHash = nlStringLowerHash("megaBlend");
    float megaAmount = 0.0f != g_fMegaBlendOverride ? g_fMegaBlendOverride : source.m_fMegaBlend;
    if (megaAmount != 0.0f)
    {
        for (pPacket = model->packets; pPacket < model->packets + model->numPackets; pPacket++)
        {
            if (glHasMaterialParameter(pPacket, megaBlendHash)
                && glGetMaterialFloatParameter(pPacket, megaBlendHash) >= 0.0f)
            {
                glSetMaterialFloatParameter(pPacket, megaBlendHash, megaAmount);
            }
        }
    }

    if (renderPass == 1)
    {
        for (pPacket = model->packets; pPacket < model->packets + model->numPackets; pPacket++)
        {
            glSetRasterState(pPacket->rasterState, GLS_Culling, 2);
        }
    }

    static u32 damage1EnabledHash = nlStringLowerHash("damage1Enabled");
    bool damage1Enabled = false;
    if (g_fForceDamage1 > 0.0f || damage1 > 0.0f)
    {
        damage1Enabled = true;
    }
    if (damage1Enabled)
    {
        int stadium = GameInfoManager::Instance()->GetStadium();
        unsigned long damageTexture = glGetTexture("global/scorch");
        bool useDamageTexture = false;
        if (damageType != DAMAGE_TEXTURE_DIRT
            && (damageType == DAMAGE_TEXTURE_SCORCH
                || stadium == 0
                || stadium == 5
                || stadium == 7
                || stadium == 9
                || stadium == 0x10
                || stadium == 8
                || stadium == 0xE
                || stadium == 0xB
                || stadium == 0xF
                || stadium == 1
                || stadium == 6))
        {
            useDamageTexture = true;
        }
        if (damage2 > 0.0f)
        {
            useDamageTexture = true;
        }

        for (pPacket = model->packets; pPacket < model->packets + model->numPackets; pPacket++)
        {
            if (glHasMaterialParameter(pPacket, damage1EnabledHash))
            {
                glSetMaterialUnsignedParameter(pPacket, damage1EnabledHash, 1);
                if (useDamageTexture)
                {
                    glTextureBinding& damageBinding = static_cast<GXCharacterDamageParameters*>(pPacket->materialParameters)->damage1Texture;
                    if (savedScorchTexture == 0xFFFFFFFF)
                    {
                        savedScorchTexture = damageBinding.texture;
                    }
                    damageBinding.texture = damageTexture;
                    damageBinding.textureIndex = 0xFFFF;
                }
                else if (savedScorchTexture != 0xFFFFFFFF)
                {
                    glTextureBinding& damageBinding = static_cast<GXCharacterDamageParameters*>(pPacket->materialParameters)->damage1Texture;
                    damageBinding.texture = savedScorchTexture;
                    damageBinding.textureIndex = 0xFFFF;
                    savedScorchTexture = 0xFFFFFFFF;
                }
            }
        }
    }

    static u32 damage2EnabledHash = nlStringLowerHash("damage2Enabled");
    bool damage2Enabled = false;
    if (g_fForceDamage2 > 0.0f || damage2 > 0.0f)
    {
        damage2Enabled = true;
    }
    if (damage2Enabled)
    {
        for (pPacket = model->packets; pPacket < model->packets + model->numPackets; pPacket++)
        {
            if (glHasMaterialParameter(pPacket, damage2EnabledHash))
            {
                glSetMaterialUnsignedParameter(pPacket, damage2EnabledHash, 1);
            }
        }
    }
}

void DrawableCharacter::RenderCharacterShadow(const cCharacter& source, glModel* model, int view)
{
    ProjectedShadowParams params;
    BasicStadium* stadium;
    const CharacterInfo* info;
    float fHeight;
    float fRadius;
    int characterSizeIndex;
    float alphaValue;
    float fScalar = shadowLevel;

    if (sShadowRenderingDisabled || fScalar < 0.001f)
    {
        return;
    }

    stadium = BasicStadium::GetCurrentStadium();
    alphaValue = 1.0f != g_fAlphaValueOverride ? g_fAlphaValueOverride : blendAmount;

    static u32 alphaValueHash = nlStringLowerHash("alphaValue");
    static float s_fHeightFudge = 1.125f;
    params.fScalar = 1.0f;
    info = source.m_pCharacterInfo;
    float shadowRadius = info->unknown_0x30.unknown_0x4;
    float shadowHeight = info->unknown_0x30.unknown_0x0;
    characterSizeIndex = info->unknown_0x2C;
    fRadius = g_fRadiusScale * shadowRadius;
    fHeight = s_fHeightFudge * shadowHeight;
    float characterScale = source.m_DetChar.m_fPlayerScale;
    nlVec4Set(
        params.vLight,
        stadium->m_shadowLightPosition.x,
        stadium->m_shadowLightPosition.y,
        stadium->m_shadowLightPosition.z,
        1.0f);
    params.vPosition = bip01Position;
    params.fRadius = characterScale * fRadius;
    params.fHeight = characterScale * fHeight;
    params.pModel = 0;
    params.fScalar = fScalar;
    params.nPartitionIndex = source.m_nCharacterIndex;

    if (nlTaskManager::m_pInstance->mCurrentState == TASK_GAMEPLAY)
    {
#ifdef NP_NATIVE
        // MSC_SHADOW_PLAY, MSC_AMORT_60 (np/amortize.h)
        static const int shadowPlay = NpShadowPlayParse(NpAmortEnvInt("MSC_SHADOW_PLAY", 2));
        const int sixty = AmortizeSixty();
        params.nVisibleInterval = NpShadowVisibleEvery(g_nOnscreenUpdate[characterSizeIndex], shadowPlay, sixty);
        params.nInvisibleInterval = NpShadowHiddenEvery(g_nOffscreenUpdate[characterSizeIndex], sixty);
        // MSC_LOCK60 (the renderer's last step when the picture is at its floor and the frame is still over budget): shadow pictures redrawn 1, then 2 frames less often. The shadow's place
        // still follows the player every frame; nothing the simulation reads (frame % interval, no random number).
        if (VgxLock60ShadowStep)
        {
            const int extra = VgxLock60ShadowStep();
            params.nVisibleInterval += (unsigned long)extra;
        }
        params.nVisibleInterval += (unsigned long)ShadowExtraFrames(bip01Position, params.nPartitionIndex);   // MSC_SHADOW_FAR, MSC_PICTURE_HOLD
#else
        params.nVisibleInterval = g_nOnscreenUpdate[characterSizeIndex];
        params.nInvisibleInterval = g_nOffscreenUpdate[characterSizeIndex];
#endif
    }
    else
    {
#ifdef NP_NATIVE
        // MSC_SHADOW_EVERY (np/scenecut.h): the game redraws every shadow every frame outside open play; n > 1 staggers them as open play does (ShouldShadowBeUpdated)
        const unsigned long shadowEvery = (nlTaskManager::m_pInstance->mCurrentState & (TASK_REPLAY | TASK_NIS)) != 0 ? NpSceneShadowEvery() : 1;
        params.nVisibleInterval = shadowEvery;
        params.nInvisibleInterval = shadowEvery;
        if ((nlTaskManager::m_pInstance->mCurrentState & (TASK_REPLAY | TASK_NIS)) != 0)
        {
            // MSC_SHADOW_FAR, MSC_PICTURE_HOLD: replays and cut-scenes too (the policy's floor for them is two steps). The shape of a shadow lags, its place does not.
            params.nVisibleInterval += (unsigned long)ShadowExtraFrames(bip01Position, params.nPartitionIndex);
        }
#else
        params.nVisibleInterval = 1;
        params.nInvisibleInterval = 1;
#endif
    }

#ifdef NP_NATIVE
    // MSC_SHADOW_LIVE=flat / MSC_LIVE_TRADES (np/livetrades.h): in live play (and only there: the check is made every frame, so the cut is exact) the shadow is a soft ellipse in the
    // main scene; no shadow picture, so no model copy, pass or texture copy. Nothing here is read by the simulation.
    // A Mega Strike charge starts inside the gameplay task (its overlay is active some frames before the task state changes): the renderer already holds that scene at its full look, so it is not live play.
    if (((nlTaskManager::m_pInstance->mCurrentState == TASK_GAMEPLAY && !gMegastrikeBackgroundOverlay.mActive) || NpShadowLiveBreak()) && NpShadowLiveFlat())
    {
        RLView* flatView = SetCharacterShadowView(GetLayerView((eCLV)view));
        RenderFlatShadow(params);
        SetCharacterShadowView(flatView);
        return;
    }
#endif
#ifdef NP_NATIVE
    bool flatWait = false;
    const bool due = NpShadowLiveFlat() ? ShadowPictureDue(params, &flatWait) : ShouldShadowBeUpdated(params);
    if (flatWait)   // MSC_SHADOW_CUT: this character's picture is not ready yet; its flat shadow stays until it is
    {
        RLView* waitView = SetCharacterShadowView(GetLayerView((eCLV)view));
        RenderFlatShadow(params, true);
        SetCharacterShadowView(waitView);
        return;
    }
    if (due)
#else
    if (ShouldShadowBeUpdated(params))
#endif
    {
        params.pModel = glModelDupNoStreams(model, false, 0);
        if (1.0f != alphaValue)
        {
            for (glModelPacket* pPacket = params.pModel->packets; pPacket < params.pModel->packets + params.pModel->numPackets; pPacket++)
            {
                glSetMaterialFloatParameter(pPacket, alphaValueHash, 1.0f);
            }
        }
        RenderCharacterIntoTexture(params);
    }

    RLView* oldView = SetCharacterShadowView(GetLayerView((eCLV)view));
    RenderProjectedShadow(params);
    SetCharacterShadowView(oldView);
}
