#ifndef GAME_RENDER_IMPOSTOR_CHARACTER_H
#define GAME_RENDER_IMPOSTOR_CHARACTER_H

#include "Game/Render/ImpostorModel.h"
#include "Game/SAnim.h"
#include "Game/TweakValue.h"
#include "NL/gl/glTarget.h"
#include "NL/nlDLListContainer.h"
#include "NL/nlList.h"
#include "NL/nlMath.h"
#include "types.h"

class AnimRetarget;
class cPN_SAnimController;
class cPoseAccumulator;
class cPoseNode;
class cSAnim;
class cSHierarchy;
template <typename T>
class cInventory;
class GLView;
class GLSkinMesh;
class Impostor;
class ImpostorCharacter;
class GLResourcePool;
class ImpostorView;
struct glModel;
class GLCompactColourMeshWriter;

#include "Game/Render/ImpostorSprite.h"

u16 QuantizeImpostorAngle(u16 target, int count);

// The camera of the crowd's main view, for the size of a figure on screen (MSC_CROWD_SIZED)
struct NpCrowdViewInfo
{
    float depthRow[4];   // the view matrix's third column: depth = -(x * r0 + y * r1 + z * r2 + r3)
    float projY;         // the projection's vertical scale
    float figureScale;   // ImpostorManager::GetImpostorSizeScale()
};

struct ImpostorCharacterParams
{
    /* 0x00 */ int mWidth;
    /* 0x04 */ int mHeight;
    /* 0x08 */ u8 mUseAdditiveBlend;
    /* 0x09 */ u8 mUseIntensityAlpha;
    /* 0x0A */ u16 mBaseAngle;
}; // size: 0x0C

class ImpostorCharacter
{
public:
    ImpostorCharacter(const char* name, int budget, int numAngles,
        int numTextures, const ImpostorCharacterParams* params);
    ~ImpostorCharacter();

    // With the configured CodeWarrior flags, these inline accessors keep
    // this translation unit as the reconstructed vtable provider while
    // retaining their out-of-line copies.
    virtual void SetScale(float scale) { mfScale = scale; }
    virtual float GetScale() { return mfScale; }
    virtual float GetCameraDistance() { return mfCameraDistance; }
    virtual float GetCameraLookatZ() { return mfCameraLookatZ; }
    virtual void SetAnimationTime(int index, float phase) = 0;
    virtual void EvaluatePose(int texture) = 0;
    virtual void Render(GLView* target, int texture) = 0;
    virtual void UpdateAnimation(float dt) = 0;
    virtual void PlayAnimation(float blendTime, const char* name) = 0;
    virtual void UpdateView(const nlVector3* direction, const nlVector3* up);
    virtual ImpostorModel* PoseModel(int) { return 0; }   // (wave 12, MSC_POSE_JOBS) the model that Render(texture) draws, if it is a plain posed model

    void Acquire(Impostor* impostor);
    void EnableSprites(bool enable);
    void ReleaseSprites();
    void RegisterSprites(GLView* parentView);
    // MSC_POSE_JOBS: the models UpdateSprites(period, slot) will pose on this picture (the same selection of sprites), appended to out without repeats
    // and, with lazyView (MSC_CROWD_GT bit 1), the sprites whose view matrices it will make
    void CollectPoseModels(int period, int slot, bool lazyView, ImpostorModel** out, int* count, int max, ImpostorSprite** sprites, int* nSprites, int maxSprites);
    void UpdateSprites(int period, int slot, bool lazyView = false, const nlVector3* direction = 0, const nlVector3* up = 0);
    // MSC_CROWD_SIZED (np/crowdcut.h): the sprites redrawn on this picture by the size of their nearest figure
    void MeasureSprites(const Impostor* impostors, const struct NpCrowdViewInfo& view, unsigned frame);
    // MSC_CROWD_GOV (np/crowdgov.h)
    void MeasureNear(const Impostor* impostors, const struct NpCrowdViewInfo& view, unsigned tick);
    bool GovFollow(int target, unsigned tick, unsigned sinceChange);
    int UpdateSpritesSized(unsigned frame, int minStep, bool at60, bool lazyView, const nlVector3* direction, const nlVector3* up, int* inView, int* groups);

    /* 0x04 */ int mNumAngles;
    /* 0x08 */ int mNumTextures;
    /* 0x0C */ u8 mIsCluster;
    /* 0x0D */ u8 m_pad00D[3];
    /* 0x10 */ nlDLListSlotPool<ImpostorSprite*> mSprites;
    /* 0x2C */ int mWidth;
    /* 0x30 */ int mHeight;
    /* 0x34 */ u8 mUseAdditiveBlend;
    /* 0x35 */ u8 mUseIntensityAlpha;
    /* 0x36 */ u16 mBaseAngle;
    /* 0x38 */ const char* mName;
    /* 0x3C */ TweakFloatBinding mfScale;
    /* 0x4C */ TweakFloatBinding mfCameraLookatZ;
    /* 0x5C */ TweakFloatBinding mfCameraDistance;
    int mSerial;   // MSC_CROWD_SIZED: the order in which the characters were made
}; // size: 0x6C

// One animated model per texture set.
class AnimatedImpostorCharacter : public ImpostorCharacter
{
public:
    AnimatedImpostorCharacter(const char* name,
        ImpostorModel* model, void* animation, int budget,
        int numAngles, int numTextures, const ImpostorCharacterParams* params);
    virtual ~AnimatedImpostorCharacter();

    virtual void SetAnimationTime(int index, float phase);
    virtual void EvaluatePose(int texture);
    virtual void Render(GLView* target, int texture);
    virtual void UpdateAnimation(float dt);
    virtual void PlayAnimation(float blendTime, const char* name);
    virtual ImpostorModel* PoseModel(int texture) { return texture >= 0 && texture < mNumModels ? mModels[texture] : 0; }

    /* 0x6C */ ImpostorModel** mModels;
    /* 0x70 */ int mNumModels;
}; // size: 0x74

// Captures the crowd as a single sprite.
class ImpostorCluster : public ImpostorCharacter
{
public:
    ImpostorCluster(const char* name, int budget,
        const ImpostorCharacterParams* params);

    virtual void SetAnimationTime(int index, float phase);
    virtual void EvaluatePose(int texture);
    virtual void Render(GLView* target, int texture);
    virtual void UpdateAnimation(float dt);
    virtual void PlayAnimation(float blendTime, const char* name);
    virtual void UpdateView(
        const nlVector3* direction, const nlVector3* up);

    unsigned long GetTexture();

    /* 0x6C */ const char* mName;
}; // size: 0x70


#endif // GAME_RENDER_IMPOSTOR_CHARACTER_H
