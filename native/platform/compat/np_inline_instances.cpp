// Out-of-line instances of `inline` functions that some TUs call without seeing their definition.
//
// The decomp keeps many inline definitions in *.inl files that only a few TUs include. With Metrowerks
// every `inline` function that was not (fully) inlined is emitted as a weak symbol where its definition
// is visible, and TUs that only see the declaration link to it. Clang emits an inline function only
// where it is used and not inlined, so those cross-TU calls would be undefined. This TU includes the
// definitions and calls each function from a function that is never executed (optnone, so the calls are
// not inlined and the out-of-line copies are emitted, as weak symbols, which also satisfy the callers).
// Found by the first link (docs/build.md, "unresolved C++ symbols"). To add one: add the include and a call.
#include "Game/BaseSceneHandler.h"
#include "Game/BaseSceneHandler.inl"
#include "Game/DB/CharacterInfo.h"
#include "Game/DB/CharacterInfo.inl"
#include "Game/FE/fePresentation.h"
#include "Game/FE/fePresentation.inl"
#include "Game/FE/tlInstance.h"
#include "Game/FE/tlInstance.inl"
#include "Game/FE/feInlineHasher.h"
#include "Game/GameInfo.h"
#include "Game/GameInfo.inl"
#include "Game/SH/SHNavigation.h"
#include "Game/SH/SHNavigation.inl"
#include "Game/AI/Variant.h"
#include "Game/AI/Variant.inl"
#include "Game/AI/DesireUpdate.h"
#include "Game/AI/DesireUpdate.inl"
#include "Game/World/WorldObject.h"
#include "Game/World/WorldObject.inl"
#include "Game/AI/FuzzyVariantCollection.inl"
#include "Game/AI/Desire.h"
#include "Game/AI/DesireDebug.inl"
#include "Game/AI/FuzzyRuntimeCall.h"
#include "NL/nlLocalization.h"
#include "NL/nlLocalization.inl"
#include "NL/nlMath.h"
#include "NL/nlMath.inl"
#include "NL/nlFormat.h"
#include "NL/nlstring_impl.h"

// clang: optnone; GCC has no such attribute (it would inline every call and emit nothing: the first Vita link had these
// functions undefined), its equivalent is optimize("O0")
#if defined(__clang__)
#define NP_KEEP __attribute__((used, optnone, noinline))
#else
#define NP_KEEP __attribute__((used, noinline, optimize("O0")))
#endif

NP_KEEP void np_keep_inline_functions()
{
    ((BaseSceneHandler*)0)->GetPresentation();
    ((CharacterInfo*)0)->GetName();
    ((FEPresentation*)0)->GetActiveSlide();
    ((TLInstance*)0)->SetVisible(false);
    ((SHNavigation*)0)->SetTimerVisible(false);
    ((Variant*)0)->fn_800C2BD4();
    ((nlLocalization*)0)->GetCurrentLanguage();
    ((nlVector3*)0)->Set(0.0f, 0.0f, 0.0f);
    ((GameInfoManager*)0)->GetUserInfo();
    ((UserInfo*)0)->IsWidescreen();
    fn_801CA660();
    // WorldObject::OnWorldLoaded / WorldHelperObject::Initialize / ::ReleaseResources are out of line now (World/WorldPhysics.cpp)
    { DesireUpdate v; v = 0; (void)v; }          // operator=<int>: only declared where shdStateMachine.cpp calls it
    GetOverlayManager();
    GetStrikerChallenge();
    GetFriendManager();
    ((Desire*)0)->Desire::SyncLog(0, 0);
    InlineHasher hashByString("");
    InlineHasher hashByValue(0UL);
    (void)hashByString;
    (void)hashByValue;
    IndexedFuzzyVariant fuzzy(0, FuzzyVariant());
    (void)fuzzy;
    CallFielderFuzzyFunction((void*)0, (cFielder*)0, "");
    { unsigned int hash = 0; CallFielderFuzzyFunction((void*)0, hash, (cFielder*)0); }   // DesireMark / DesireUsePowerup
    CallFielderFuzzyFunction((InterpreterCore*)0, "", (cFielder*)0);
}

// explicit instantiations of function templates whose definition the callers do not see
template char* nlStrNCat<char>(char*, const char*, const char*, unsigned long);
template BasicString<unsigned short, Detail::TempStringAllocator>
Format<BasicString<unsigned short, Detail::TempStringAllocator>, unsigned short[4], unsigned short[4]>(
    const BasicString<unsigned short, Detail::TempStringAllocator>&, const unsigned short (&)[4],
    const unsigned short (&)[4]);

template BasicString<unsigned short, Detail::TempStringAllocator>
Format<BasicString<unsigned short, Detail::TempStringAllocator>, const unsigned short*>(
    const BasicString<unsigned short, Detail::TempStringAllocator>&, const unsigned short* const&);

// QueuedEvent<void()>::Queue (Game/Event.h) builds a Function<bool> from a bind expression; the
// constructor template lives in NL/nlFunction.inl, which GoalieActions.cpp (the user) does not include.
#include "Game/Event.h"
#include "NL/nlFunction.inl"
template Function<bool>::Function(
    BindExp3<void,
        Detail::MemFunImpl<void, void (QueuedEventBase0<void>::*)(Function<void()>, unsigned char)>,
        QueuedEvent<void()>*, Function<void()>, Placeholder<0> >);
