#ifndef GAME_RENDER_IMPOSTOR_MODEL_H
#define GAME_RENDER_IMPOSTOR_MODEL_H

#include "Game/SAnim.h"
#include "NL/nlMath.h"
#include "types.h"

class AnimRetarget;
class cPN_SAnimController;
class cPoseAccumulator;
class cPoseNode;
class cSHierarchy;
template <typename T>
class cInventory;
class GLView;
class GLSkinMesh;
class GLResourcePool;
struct glModel;

class ImpostorModel
{
public:
    ImpostorModel(cSHierarchy& hierarchy, unsigned long modelID,
        cInventory<cSAnim>* animations,
        GLResourcePool* resource);
    ImpostorModel(cSHierarchy& hierarchy, unsigned long modelID,
        GLResourcePool* resource);
    virtual ~ImpostorModel();
    virtual void Render(
        GLView* opaqueView, GLView* translucentView);
    virtual void Update(float dt);
    virtual void RenderPose(GLView* opaqueView,
        GLView* translucentView, const cPoseAccumulator& poseAccumulator,
        const nlMatrix4* worldMatrix);

    ImpostorModel* Clone(GLResourcePool* resource) const;
    void UpdateAnimation(float dt);
    void EvaluatePose();
    void SetAnimationTime(float time);
    void RenderSamePose(GLView* opaqueView);
    // wave 12 (MSC_POSE_JOBS, np/crowdcut.h): the pose of one model made on a job lane: (the evaluate,) the node matrices and the skin mesh's pose, exactly what Render does before RenderPose; writes only this
    // model's own accumulator and skin mesh. A model posed this way is drawn by RenderSamePose / skipped by AnimatedImpostorCharacter::EvaluatePose until ClearPosed().
    void PoseForJob(bool evaluate);
    static void MarkPosed(ImpostorModel* model);
    static bool IsPosed(const ImpostorModel* model);
    static void ClearPosed();
    void PlayAnimation(cSAnim& anim, ePlayMode playMode, const AnimRetarget* retarget);
    void ResolveTextures();

    void SetReplacementTexture(unsigned long texture);
    void PlayAnimation(const char* name, float blendTime, ePlayMode playMode);

    // Bounded fit, not recovered source: R4QE01's Nis::Render keeps the skin
    // mesh it reads here in a register allocated ahead of the impostor loop's
    // strength-reduced array walker. Under GC/3.0a5 every caller-scope local
    // is allocated after that walker, so the value came out of an inline
    // function. That function's owner and name are not recoverable - Render is
    // the only reader outside this class and the DOL keeps no out-of-line copy
    // - so it sits with the field it reads.
    GLSkinMesh* GetSkinMesh() const { return mSkinMesh; }

    /* 0x04 */ nlMatrix4 mWorldMatrix;
    /* 0x44 */ cPN_SAnimController* mAnimController;
    /* 0x48 */ cPoseAccumulator* mPoseAccumulator;
    /* 0x4C */ GLSkinMesh* mSkinMesh;
    /* 0x50 */ cPoseNode* mPoseTree;
    /* 0x54 */ unsigned long mModelID;
    /* 0x58 */ cSHierarchy* mHierarchy;
    /* 0x5C */ bool mVisible;
    /* 0x5D */ bool mTexturesResolved;
    /* 0x5E */ u8 mPadding05E[2];
    /* 0x60 */ glModel* mLastModel;
    /* 0x64 */ cInventory<cSAnim>* mAnimations;
    /* 0x68 */ unsigned long mOriginalTexture;
    /* 0x6C */ unsigned long mReplacementTexture;
    /* 0x70 */ unsigned long mResolvedTexture;
    /* 0x74 */ void (*mModelCallback)(
        ImpostorModel*, glModel*);
    // MSC_CROWD_GOV (np/crowdgov.h), render bookkeeping: a crowd model is one pose clock for all the sprites that show it. Under the governor its clock only advances when one of its sprites is
    // redrawn (so the pose a sprite shows is always the pose the clock holds: a section that was frozen resumes from where it froze, never snapping to the live pose).
    float mGovPending = 0.0f;       // animation time banked while no sprite of the model was redrawn
    unsigned mGovAdvTick = ~0u;     // the crowd picture on which the clock was last advanced
    signed char mGovLevel = 0;      // this model's own level (follows the governor one notch at a time, staggered)
    unsigned char mGovClass = 2;    // size class from the mean height of its sprites' nearest figures (1 near .. 3 far)
    bool mGovCaptured = false;
}; // size: 0x78

#endif // GAME_RENDER_IMPOSTOR_MODEL_H
