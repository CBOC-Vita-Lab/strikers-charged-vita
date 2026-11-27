// Actual-method host32 lifecycle regression: no assets or real graphics context.
// Virtual callback counters do not establish real animation/model/FE purity.
#include "NL/nlDLListContainer.inl"
#include "Game/Render/ImpostorCharacter.h"
#include "Game/Render/ImpostorManager.h"
#include "NL/gl/glView.h"
#include <cstddef>
#include <cstdio>

extern int boundary_view_creates, boundary_view_destroys, boundary_child_removes;
static int checks, failed;
static void Check(bool value, const char* label)
{
    ++checks;
    if (!value) { ++failed; std::printf("FAIL %s\n", label); }
}
static void Dimensions(ImpostorSprite* sprite, unsigned long width, unsigned long height, const char* label)
{
    bool equal = sprite->mView->m_Viewport.width == width && sprite->mView->m_Viewport.height == height;
    Check(equal, label);
    if (!equal) std::printf("  dimensions actual=%lux%lu expected=%lux%lu\n",
        sprite->mView->m_Viewport.width, sprite->mView->m_Viewport.height, width, height);
}
static void Suspended(ImpostorSprite* sprite, const char* label)
{
    bool equal = sprite->mCaptureSuspended && sprite->mView->m_Target == GLViewTarget_None;
    Check(equal, label);
    if (!equal) std::printf("  target=%lu suspended=%u enabled=%u disabled=%u\n", sprite->mView->m_Target,
        unsigned(sprite->mCaptureSuspended), unsigned(sprite->mEnabled), unsigned(sprite->mCaptureDisabled));
}

class CounterCharacter : public ImpostorCharacter
{
public:
    CounterCharacter() : ImpostorCharacter("fixture", 1, 1, 0, 0), poses(0), renders(0), animations(0) {}
    void SetAnimationTime(int, float) {}
    void EvaluatePose(int) { ++poses; }
    void Render(GLView*, int) { ++renders; }
    void UpdateAnimation(float) { ++animations; }
    void PlayAnimation(float, const char*) {}
    int poses, renders, animations;
};

static void OrdinaryLifecycle(GLView& parent, ImpostorManager* manager)
{
    CounterCharacter character;
    ImpostorSprite* sprite = new ImpostorSprite(&character, 0, 1, 64, 64);
    sprite->Initialize("fixture-sprite");
    character.mSprites.AddEnd(sprite);
    character.RegisterSprites(&parent);
    sprite->mNumRenderSlots = 1;
    Check(parent.HasChildren() && sprite->mView->m_Parent == &parent, "real registration attaches sprite view");
    Check(sprite->mID == 0, "fresh process first real sprite ID is zero");
    Check(!sprite->mCaptureSuspended && sprite->mView->m_Target == GLViewTarget_Mode9, "normal capture initially active");
    unsigned int seed = nlDefaultSeed;
    manager->UpdateAnimations(1.0f / 60.0f);
    manager->UpdateSprites();
    Check(character.animations == 1 && character.poses == 1 && character.renders == 1, "actual manager and character virtual boundaries");
    Check(nlDefaultSeed == seed, "real random seed unchanged by observed update methods");

    sprite->SuspendCapture();
    Suspended(sprite, "first real suspension disables target");
    sprite->UpdateViewport();
    Suspended(sprite, "viewport update cannot reactivate suspended target");
    sprite->SuspendCapture();
    Suspended(sprite, "repeated suspension remains disabled");
    sprite->ResumeCapture();
    Check(!sprite->mCaptureSuspended && sprite->mView->m_Target == GLViewTarget_Mode9, "real resume restores enabled target");
    sprite->SuspendCapture();
    character.EnableSprites(false);
    Suspended(sprite, "character disable while suspended");
    character.EnableSprites(true);
    Suspended(sprite, "character enable while suspended");
    sprite->SuspendCapture();
    Suspended(sprite, "repeat suspension after character enable");
    sprite->mCaptureDisabled = true;
    sprite->ResumeCapture();
    Check(!sprite->mCaptureSuspended && sprite->mView->m_Target == GLViewTarget_None, "resume honors capture-disabled flag");
    manager->UpdateSprites();
    Check(character.poses == 2 && character.renders == 2, "disabled target does not omit original callbacks");
    Check(sprite->mView->m_Target == GLViewTarget_None, "scheduled resume honors capture-disabled flag");
    sprite->mCaptureDisabled = false;
    sprite->UpdateViewport();
    Check(sprite->mView->m_Target == GLViewTarget_Mode9, "unsuspended viewport activates normal target");
    character.EnableSprites(false);
    sprite->SuspendCapture();
    sprite->ResumeCapture();
    Check(!sprite->mCaptureSuspended && sprite->mView->m_Target == GLViewTarget_None, "resume honors disabled sprite");
    character.EnableSprites(true);
    Check(sprite->mView->m_Target == GLViewTarget_Mode9, "ordinary re-enable restores target");
    Check(nlDefaultSeed == seed, "original lifecycle preserves real random seed");

    manager->SetUpdatePeriod(2);
    manager->SetUpdateThrottle(1);
    int poses = character.poses, renders = character.renders;
    manager->UpdateSprites();
    Check(!sprite->mCaptureSuspended && sprite->mView->m_Target == GLViewTarget_Mode9, "period2 scheduled-on resumes capture");
    Check(character.poses == poses + 1 && character.renders == renders + 1, "period2 scheduled-on callbacks");
    manager->UpdateSprites();
    Suspended(sprite, "period2 scheduled-off suspends capture");
    Check(character.poses == poses + 1 && character.renders == renders + 1, "period2 scheduled-off omits callbacks");

    manager->SetEnabled(false);
    Check(!manager->mEnabled && !sprite->mEnabled, "real manager disable reaches sprite");
    Dimensions(sprite, 0, 0, "DIM manager disable updates viewport while suspended");
    Suspended(sprite, "manager disable retains suspension");
    manager->SetEnabled(true);
    Check(manager->mEnabled && sprite->mEnabled, "real manager enable reaches sprite");
    Dimensions(sprite, 64, 64, "DIM manager enable restores dimensions while suspended");
    Suspended(sprite, "manager enable cannot reactivate scheduled-off target");

    sprite->mWidth = 32;
    sprite->mHeight = 24;
    sprite->UpdateViewport();
    Dimensions(sprite, 32, 24, "DIM explicit viewport size transition while suspended");
    Suspended(sprite, "dimension transition retains suspension");
    sprite->mWidth = 64;
    sprite->mHeight = 64;
    sprite->UpdateViewport();
    Dimensions(sprite, 64, 64, "DIM viewport restoration while suspended");
    manager->UpdateSprites();
    Check(!sprite->mCaptureSuspended && sprite->mView->m_Target == GLViewTarget_Mode9, "period2 resumes next scheduled-on after enable interleave");
    Check(character.poses == poses + 2 && character.renders == renders + 2, "manager off-on did not reset scheduler phase");
    manager->UpdateSprites();
    Suspended(sprite, "period2 second scheduled-off");
    Check(character.poses == poses + 2 && character.renders == renders + 2, "period2 second scheduled-off callbacks unchanged");

    sprite->mCaptureDisabled = true;
    manager->SetEnabled(false);
    Dimensions(sprite, 0, 0, "DIM disabled-capture manager disable while suspended");
    manager->SetEnabled(true);
    Dimensions(sprite, 64, 64, "DIM disabled-capture manager enable while suspended");
    Suspended(sprite, "capture-disabled enable remains suspended");
    manager->UpdateSprites();
    Check(!sprite->mCaptureSuspended && sprite->mView->m_Target == GLViewTarget_None, "period2 scheduled-on still honors capture-disabled");
    Check(character.poses == poses + 3 && character.renders == renders + 3, "period2 disabled target still enters original callbacks");
    manager->UpdateSprites();
    Suspended(sprite, "period2 disabled target scheduled-off");
    sprite->mCaptureDisabled = false;
    manager->SetEnabled(true);
    Suspended(sprite, "clear capture-disabled flag during scheduled-off does not activate target");
    manager->UpdateSprites();
    Check(!sprite->mCaptureSuspended && sprite->mView->m_Target == GLViewTarget_Mode9, "next scheduled-on restores ordinary capture");
    Check(character.poses == poses + 4 && character.renders == renders + 4, "period2 callback phase retained throughout interleave");
    Check(character.animations == 1 && nlDefaultSeed == seed, "added lifecycle controls do not advance animation or real RNG");
    manager->mCharacters.Clear();
}

static void DefensiveSingleInitialize(GLView& parent, ImpostorManager* manager)
{
    CounterCharacter character;
    ImpostorSprite* sprite = new ImpostorSprite(&character, 0, 1, 64, 64);
    Check(!sprite->mCaptureSuspended && sprite->mView == 0, "fresh defensive sprite has normal constructor state");
    unsigned int seed = nlDefaultSeed;
    // Whitebox future-state robustness only: not a known shipped pre-Initialize sequence.
    sprite->mCaptureSuspended = true;
    sprite->Initialize("defensive-single-initialize");
    character.mSprites.AddEnd(sprite);
    character.RegisterSprites(&parent);
    Check(parent.HasChildren() && sprite->mView->m_Parent == &parent, "defensive sprite registered after its only Initialize");
    Dimensions(sprite, 64, 64, "DIM defensive single Initialize preserves dimensions");
    Suspended(sprite, "DEFENSIVE single Initialize honors injected suspended state");
    Check(nlDefaultSeed == seed, "single Initialize does not advance real RNG");
    sprite->ResumeCapture();
    Check(!sprite->mCaptureSuspended && sprite->mView->m_Target == GLViewTarget_Mode9, "defensive case resumes ordinarily");
    Check(character.poses == 0 && character.renders == 0 && character.animations == 0, "defensive Initialize is not animation or render-callback proof");
    manager->mCharacters.Clear();
}

int main()
{
    static_assert(sizeof(void*) == 4, "host32 ABI");
    static_assert(sizeof(unsigned long) == 4, "host32 long ABI");
    static_assert(sizeof(wchar_t) == 2, "game wide-character ABI");
    // The game's own part of both objects keeps its layout (the render-side bookkeeping of MSC_CROWD_SIZED / MSC_POSE_JOBS follows it).
    static_assert(offsetof(ImpostorSprite, mUseIntensityAlpha) == 0x88 && sizeof(ImpostorSprite) >= 0x8c, "sprite ABI");
    static_assert(offsetof(ImpostorCharacter, mfCameraDistance) == 0x5c && sizeof(ImpostorCharacter) >= 0x6c, "character ABI");
    std::puts("capture lifecycle: actual viewport/manager/period2 methods; defensive pre-Initialize suspension is whitebox only, unreachable in the known production construction path");
    GLRenderPair noTarget;
    GLView parent(0, noTarget, GLViewSort_None);
    ImpostorManager* manager = ImpostorManager::GetInstance();
    manager->mParentView = &parent;
    manager->SetEnabled(true);
    OrdinaryLifecycle(parent, manager);
    Check(!parent.HasChildren() && boundary_child_removes == 1, "ordinary real destructor unregisters attached view");
    DefensiveSingleInitialize(parent, manager);
    Check(!parent.HasChildren() && boundary_child_removes == 2, "defensive real destructor unregisters attached view");
    Check(boundary_view_creates == 3 && boundary_view_destroys == 2, "two distinct sprites initialize exactly once and clean up");
    manager->SetEnabled(false);
    manager->mParentView = 0;
    std::printf("extended actual-method lifecycle checks=%d failed=%d\n", checks, failed);
    return failed != 0;
}
