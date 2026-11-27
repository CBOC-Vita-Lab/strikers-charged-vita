#include "Game/Render/ImpostorModel.h"
#include "NL/gl/glTexture.h"

#include "Game/CharacterEffects.h"
#include "Game/GL/GLInventory.h"
#include "Game/Inventory.h"
#include "Game/GL/ShaderSkinMesh.h"
#include "Game/PoseAccumulator.h"
#include "Game/SHierarchy.h"
#include "Game/SAnim/pnBlender.h"
#include "Game/SAnim/pnSAnimController.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glModel.h"
#include "NL/gl/glState.h"
#include "NL/gl/glView.h"
#include "NL/gl/glMaterialParameters.h"
#include "NL/glx/glxTexture.h"
#include "NL/nlMemory.h"
#include "NL/nlString.h"

static const unsigned long sTextureParameterHashes[10] = {
    nlStringLowerHash("NLG_DIFFUSE"),
    nlStringLowerHash("NLG_DETAIL"),
};

ImpostorModel::ImpostorModel(cSHierarchy& hierarchy,
    unsigned long modelID, cInventory<cSAnim>* animations,
    GLResourcePool* resource)
{
    mModelID = modelID;
    mAnimController = 0;
    mPoseAccumulator = 0;
    mSkinMesh = 0;
    mPoseTree = 0;
    mHierarchy = &hierarchy;
    mVisible = false;
    mTexturesResolved = false;
    mLastModel = 0;
    mAnimations = animations;
    mOriginalTexture = 0;
    mReplacementTexture = 0;
    mModelCallback = 0;

    if (modelID == (unsigned long)-1)
    {
        mSkinMesh = 0;
    }
    else
    {
        mSkinMesh = resource->m_inventory->MakeSkinMesh(
            modelID, &hierarchy);
        if (mSkinMesh != 0)
        {
            mSkinMesh->m_Unknown0C = 1;
        }
    }

    mPoseAccumulator = new (nlMalloc(sizeof(cPoseAccumulator), 8, false))
        cPoseAccumulator(&hierarchy, true);
    mWorldMatrix.SetIdentity();
    mVisible = true;
}

ImpostorModel::ImpostorModel(cSHierarchy& hierarchy,
    unsigned long modelID, GLResourcePool* resource)
{
    mModelID = modelID;
    mAnimController = 0;
    mPoseAccumulator = 0;
    mSkinMesh = 0;
    mPoseTree = 0;
    mHierarchy = &hierarchy;
    mVisible = false;
    mLastModel = 0;
    mAnimations = 0;
    mModelCallback = 0;

    if (modelID == (unsigned long)-1)
    {
        mSkinMesh = 0;
    }
    else
    {
        mSkinMesh = resource->m_inventory->MakeSkinMesh(
            modelID, &hierarchy);
        if (mSkinMesh != 0)
        {
            mSkinMesh->m_Unknown0C = 1;
        }
    }

    mPoseAccumulator = new (nlMalloc(sizeof(cPoseAccumulator), 8, false))
        cPoseAccumulator(&hierarchy, true);
    mWorldMatrix.SetIdentity();
    mVisible = true;
}

ImpostorModel* ImpostorModel::Clone(GLResourcePool* resource) const
{
    return new (8, false) ImpostorModel(
        *mHierarchy, mModelID, mAnimations, resource);
}

ImpostorModel::~ImpostorModel()
{
    if (mPoseTree != 0)
    {
        delete mPoseTree;
    }
    if (mPoseAccumulator != 0)
    {
        delete mPoseAccumulator;
    }
    if (mSkinMesh != 0)
    {
        delete mSkinMesh;
    }
}

void ImpostorModel::Update(float dt)
{
    mPoseTree = mPoseTree->Update(dt);
    mPoseAccumulator->InitAccumulators();
    mPoseTree->Evaluate(1.0f, mPoseAccumulator);
}

void ImpostorModel::UpdateAnimation(float dt)
{
    mPoseTree = mPoseTree->Update(dt);
}

void ImpostorModel::EvaluatePose()
{
    mPoseAccumulator->InitAccumulators();
    mPoseTree->Evaluate(1.0f, mPoseAccumulator);
}

void ImpostorModel::SetAnimationTime(float time)
{
    mAnimController->SetTime(time);
}

// MSC_CROWD_POSEONCE (np/overlap.h, AnimatedImpostorCharacter::Render): the sprite before this one drew the same model in the same pose
static bool sSamePose;

// MSC_POSE_JOBS: the models whose pose a job made on this picture (game thread only)
static const ImpostorModel* sPosed[160];
static int sNumPosed;

void ImpostorModel::MarkPosed(ImpostorModel* model)
{
    if (sNumPosed < 160) sPosed[sNumPosed++] = model;
}

bool ImpostorModel::IsPosed(const ImpostorModel* model)
{
    for (int i = 0; i < sNumPosed; ++i)
    {
        if (sPosed[i] == model) return true;
    }
    return false;
}

void ImpostorModel::ClearPosed()
{
    sNumPosed = 0;
}

void ImpostorModel::PoseForJob(bool evaluate)
{
    if (evaluate)
    {
        mPoseAccumulator->InitAccumulators();
        mPoseTree->Evaluate(1.0f, mPoseAccumulator);
    }
    mPoseAccumulator->BuildNodeMatrices(mWorldMatrix);
    if (mVisible && mSkinMesh != 0)   // (RenderPose: a hidden model or one with no skin mesh is not posed)
    {
        mSkinMesh->Pose(const_cast<cPoseAccumulator*>(mPoseAccumulator));
    }
}

void ImpostorModel::RenderSamePose(GLView* opaqueView)
{
    sSamePose = true;
    RenderPose(opaqueView, 0, *mPoseAccumulator, &mWorldMatrix);
    sSamePose = false;
}

void ImpostorModel::Render(
    GLView* opaqueView, GLView* translucentView)
{
    mPoseAccumulator->BuildNodeMatrices(mWorldMatrix);
    RenderPose(
        opaqueView, translucentView, *mPoseAccumulator, &mWorldMatrix);
}

void ImpostorModel::RenderPose(GLView* opaqueView,
    GLView* translucentView, const cPoseAccumulator& poseAccumulator,
    const nlMatrix4*)
{
    if (!mVisible)
    {
        return;
    }
    if (!mTexturesResolved)
    {
        ResolveTextures();
    }
    if (mSkinMesh == 0)
    {
        return;
    }

    if (!sSamePose)
    {
        mSkinMesh->Pose(
            const_cast<cPoseAccumulator*>(&poseAccumulator));
    }
    mSkinMesh->PrepareToRender();
    glModel* model = glModelDupNoStreams(
        mSkinMesh->GetModel(), false, 0);

    if (mOriginalTexture != 0 && mReplacementTexture != 0)
    {
        for (glModelPacket* packet = model->packets;
            packet < model->packets + model->numPackets;
            ++packet)
        {
            unsigned long texture = glGetMaterialUnsignedParameter(packet, gDiffuseTextureSemantic);
            if (texture == mOriginalTexture)
            {
                glSetMaterialTextureParameter(packet, gDiffuseTextureSemantic, mReplacementTexture);
                unsigned long resolvedTexture = mResolvedTexture;
                glSetMaterialTextureIndexParameter(packet, gDiffuseTextureSemantic, &resolvedTexture);
            }
        }
    }

    if (mModelCallback != 0)
    {
        mModelCallback(this, model);
    }

    for (unsigned long i = 0; i < model->numPackets; ++i)
    {
        glModelPacket* packet = &model->packets[i];
        if (glGetRasterState(packet->rasterState, GLS_AlphaBlend) == 0)
        {
            opaqueView->AttachPacket(packet, 0);
        }
        else
        {
            translucentView->AttachPacket(packet, 1);
        }
    }
    mLastModel = model;
}

void ImpostorModel::SetReplacementTexture(unsigned long texture)
{
    mReplacementTexture = texture;
    mResolvedTexture = glGetTextureManager()->GetTextureIndex(mReplacementTexture);
}

void ImpostorModel::PlayAnimation(const char* name, float blendTime, ePlayMode playMode)
{
    cSAnim* anim = mAnimations->Find(const_cast<char*>(name));

    cPN_SAnimController* controller = new cPN_SAnimController(
        anim, 0, playMode, 0, 0, false);
    if (mPoseTree != 0 && blendTime > 0.0f)
    {
        mPoseTree = new cPN_Blender(
            mPoseTree, controller, blendTime);
    }
    else
    {
        if (mPoseTree != 0)
        {
            delete mPoseTree;
        }
        mPoseTree = controller;
    }
    mAnimController = controller;
}

void ImpostorModel::PlayAnimation(cSAnim& anim, ePlayMode playMode, const AnimRetarget* retarget)
{
    cPN_SAnimController* controller = new cPN_SAnimController(
        &anim, retarget, playMode, 0, 0, false);
    if (mPoseTree != 0)
    {
        delete mPoseTree;
    }
    mPoseTree = controller;
    mAnimController = controller;
}

void ImpostorModel::ResolveTextures()
{
    if (mTexturesResolved)
    {
        return;
    }
    if (mSkinMesh != 0)
    {
        int modelIndex = mSkinMesh->GetModelIndex();
        glModelPacket* packet;
        for (int i = 0; i < 2; ++i)
        {
            mSkinMesh->m_Unknown0C = i;
            if (mSkinMesh->GetModel() != 0)
            {
                for (packet = mSkinMesh->GetModel()->packets;
                    packet < mSkinMesh->GetModel()->packets
                                 + mSkinMesh->GetNumPackets();
                    ++packet)
                {
                    for (int j = 0; j < 10; ++j)
                    {
                        if (glHasMaterialParameter(packet, sTextureParameterHashes[j]))
                        {
                            unsigned long texture = glGetMaterialUnsignedParameter(
                                packet, sTextureParameterHashes[j]);
                            unsigned long resolvedTexture = glGetTextureManager()->GetTextureIndex(texture);
                            glSetMaterialTextureIndexParameter(
                                packet, sTextureParameterHashes[j], &resolvedTexture);
                        }
                    }
                }
            }
        }
        mSkinMesh->m_Unknown0C = modelIndex;
    }
    mTexturesResolved = true;
}
