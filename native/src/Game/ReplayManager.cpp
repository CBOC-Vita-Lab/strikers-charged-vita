#include "Game/Task/GameTaskState.h"
#include "np/gsplit.h"
#ifdef NP_NATIVE
#include "np/cpu22.h"
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "revolution/types.h"
#include "NL/nlDLListContainer.inl"
#include "NL/plat/PlatPadManager.h"
#include "NL/plat/WiiRemotePad.h"
#include "NL/plat/WiiFreestylePad.h"

#include "Game/Render/NPCManager.h"
#include "Game/Render/CrowdManager.h"
#include "Game/Render/NetMesh.h"
#include "Game/Render/ShootToScoreArrow.h"
#include "Game/Effects/EmissionManager.h"
#include "Game/Physics/PhysicsNet.h"

#include "Game/ReplayManager.h"
#include "np/render_neon.h"
#include "np/snap_onecopy.h"
#include "Game/PoseNode.h"
#include "Game/SAnim/pnBlender.h"
#include "Game/SAnim/pnFeather.h"
#include "Game/SAnim/pnSAnimController.h"
#include "Game/SAnim/pnSingleAxisBlender.h"
#include "Game/SAnim/pnScaleBlender.h"
#include "Game/DB/SaveLoad.h"
#include "Game/Render/Presentation.h"
#include "NL/plat/nlFlash.h"
#include "NL/nlPrint.h"
#include "Game/Character.h"
#include "Game/CharacterQueries.h"
#include "Game/MathHelpers.h"
#include "Game/Sys/tweak.h"
#include "Game/TweakValue.h"

#include "Game/Camera/CameraMan.h"
#include "Game/Camera/DebugCam.h"
#include "Game/Event.h"
#include "Game/EventRegistry.h"
#include "Game/ExcitementSystem.h"
#include "Game/GameEventQueue.h"
#include "Game/ReplayChoreo.h"
#include "Game/Task/FixedUpdateTask.h"
#include "Game/Task/ProfilerTask.h"
#include "Game/Task/TweakerTask.h"
#include "NL/nlBind.h"
#include "NL/nlFunction.inl"
#include "NL/nlConfig.h"
#include "NL/globalpad.h"
#include "NL/nlMemory.h"
#include "NL/nlTask.h"
#include "Game/InputManager.h"

#include "Game/SharedStaticStorage.h"


float GetRemoteReplaySpeed();

bool gbDebugReplayFixedStepMode;
bool gbReplayFlashError;
extern float lbl_806E14CC;
extern bool gbSavingReplay;
extern bool gbLoadingReplay;

ReplayManager::ReplayManager()
    : mCurrent(mSnapshots)
    , mPrevious(mSnapshots + 1)
    , mRender(0)
    , mDebugCamera(cFollowCamera::FOLLOW_SELECTABLE)
    , mReplayDebugCamera(0)
    , mEvents(0)
    , mSpeed(1.0f)
    , mSpeedUp(0.0f)
    , mDeltaTime(0.0f)
    , mTime(0.0f)
    , mReplay(0)
    , mMemory(0)
{
}

ReplayManager* ReplayManager::Instance()
{
    static ReplayManager* rm;
    if (rm == 0)
    {
        rm = new ReplayManager;
    }
    return rm;
}

bool gbNetMeshReplayResync;
float gfLastAheadOfFrameTime;
nlVector3 gLastReplayBallPosition;

template <typename T>
void RenderSnapshot::Replay(T& frame)
{
    for (int i = 0; i < 10; i++)
        Replayable<0>(frame, mCharacters[i]);
    frame.fn_80191504();
    for (int i = 0; i < 150; i++)
        Replayable<0>(frame, mPowerups[i]);
    frame.fn_80191504();
    frame.fn_80191504();
    frame.fn_80191504();
    if ((mFlags.raw >> 30) & 1)
    {
        Replayable<0>(frame, mNumFlyingCameras);
        for (unsigned int i = 0; i < mNumFlyingCameras; i++)
            Replayable<0>(frame, _2298[i]);
    }
    frame.fn_80191504();
    if ((mFlags.raw >> 29) & 1)
    {
        Replayable<0>(frame, mHasHammers);
        if (mHasHammers)
            for (unsigned int i = 0; i < 15; i++)
                Replayable<0>(frame, mHammers[i]);
        frame.fn_80191504();
    }
    if ((mFlags.raw >> 22) & 1)
        for (unsigned int i = 0; i < 8; i++)
            Replayable<0>(frame, mThwomps[i]);
    if ((mFlags.raw >> 28) & 1)
    {
        Replayable<0>(frame, mNumBulletBills);
        for (unsigned int i = 0; i < mNumBulletBills; i++)
            Replayable<0>(frame, mBulletBills[i]);
        frame.fn_80191504();
    }
    Replayable<0>(frame, mChainChomp);
    if ((mFlags.raw >> 24) & 1)
    {
        if (NPCManager::fn_801948A0()->fn_801919A4() != 0)
            Replayable<0>(frame, mDiddyBanana);
    }
    if ((mFlags.raw >> 25) & 1)
    {
        Replayable<0>(frame, mNumVisibleDaisyFists);
        for (unsigned int i = 0; i < mNumVisibleDaisyFists; i++)
            Replayable<0>(frame, mDaisyFists[i]);
    }
    if ((mFlags.raw >> 31) & 1)
        Replayable<0>(frame, mYoshiEgg);
    if ((mFlags.raw >> 26) & 1)
        Replayable<0>(frame, mBirdoEgg);
    if ((mFlags.raw >> 27) & 1)
        Replayable<0>(frame, mKoopaShell);
    Replayable<0>(frame, mBall);
    Replayable<1>(frame, mCameraUp);
    Replayable<1>(frame, mGoalLight);
    Replayable<1>(frame, CrowdManager::fn_801919AC());
    Replayable<0>(frame, WorldDarkening::Instance());
    if ((mFlags.raw >> 23) & 1)
    {
        for (int i = 0; i < 3; i++)
        {
            if (NPCManager::fn_801948A0()->fn_801A9DE0(i) != 0)
                Replayable<0>(frame, mWindDebris[i]);
        }
        frame.fn_80191504();
    }
    frame.fn_80191504();
    if (NetMesh::IsAnimatedNetMeshEnabled())
    {
        if (ReplayFrameTraits<T>::IsLoadFrame
            && ((LoadFrame&)frame).GetInterval() == 1)
        {
            if (gbNetMeshReplayResync)
            {
                gbNetMeshReplayResync = false;
                NetMesh::GetPositiveXNetMesh()->Update(g_fFixedUpdateTick,
                    mBall.GetPosition(),
                    gLastReplayBallPosition,
                    mPositiveGoalieNetCheck,
                    0);
                NetMesh::GetNegativeXNetMesh()->Update(g_fFixedUpdateTick,
                    mBall.GetPosition(),
                    gLastReplayBallPosition,
                    mNegativeGoalieNetCheck,
                    0);
                gLastReplayBallPosition = mBall.GetPosition();
                mpNetMeshPositiveX->Grab(*PhysicsNet::GetPositiveXNet()->GetNetMesh());
                mpNetMeshNegativeX->Grab(*PhysicsNet::GetNegativeXNet()->GetNetMesh());
            }
            if (((LoadFrame&)frame).fn_801948B0() > 0.0f)
            {
                if (((LoadFrame&)frame).fn_801948B0() < gfLastAheadOfFrameTime)
                {
                    mpNetMeshPositiveX->Grab(*PhysicsNet::GetPositiveXNet()->GetNetMesh());
                    mpNetMeshNegativeX->Grab(*PhysicsNet::GetNegativeXNet()->GetNetMesh());
                    gbNetMeshReplayResync = true;
                }
                gfLastAheadOfFrameTime = ((LoadFrame&)frame).fn_801948B0();
            }
        }
        Replayable<1>(frame, *mpNetMeshPositiveX);
        Replayable<1>(frame, *mpNetMeshNegativeX);
    }
    Replayable<1>(frame, mSimulationTime);
    frame.fn_80191504();
    if (frame.fn_801919D0())
    {
        Replayable<3>(frame, EmissionManager::Instance()->InstanceForReplayOnly());
    }
    mValid = true;
}

#ifdef NP_NATIVE
static unsigned sNpReplayGen = 1;
#endif

void ReplayManager::Initialize()
{
    mMemory = (u8*)nlMalloc(0x100000, 0x20, false);
    mReplay = new (nlMalloc(sizeof(Replay), 8, false)) Replay((char*)mMemory, 0x100000, 0x8000);
    mTime = 0.0f;
#ifdef NP_NATIVE
    ++sNpReplayGen;
#endif
}

template <typename EventData>
static inline BindExp2<void,
    Detail::MemFunImpl<void, void (ReplayManager::*)(EventData*)>,
    ReplayManager*, Placeholder<0> >
MakeReplayBinding(
    void (ReplayManager::*callback)(EventData*), ReplayManager* manager)
{
    typedef Detail::MemFunImpl<void,
        void (ReplayManager::*)(EventData*)>
        CallbackMemFun;
    typedef BindExp2<void, CallbackMemFun, ReplayManager*, Placeholder<0> >
        CallbackBind;
    CallbackMemFun function(callback);
    return CallbackBind(function, manager, placeholder0);
}

static inline BindExp1<void,
    Detail::MemFunImpl<void, void (ReplayManager::*)()>, ReplayManager*>
MakeReplayBinding(
    void (ReplayManager::*callback)(), ReplayManager* manager)
{
    typedef Detail::MemFunImpl<void, void (ReplayManager::*)()>
        CallbackMemFun;
    typedef BindExp1<void, CallbackMemFun, ReplayManager*> CallbackBind;
    CallbackMemFun function(callback);
    return CallbackBind(function, manager);
}

void ReplayManager::RegisterEventHandlers()
{
    FindEvent<ReceiveBallData>("ReceiveBall", -1)->Add(Function<ReceiveBallData*>(MakeReplayBinding(&ReplayManager::OnReceiveBall, this)), 0, -1);
    FindEvent<ShotAtGoalData>("ShotAtGoal", -1)->Add(Function<ShotAtGoalData*>(MakeReplayBinding(&ReplayManager::OnShotAtGoal, this)), 0, -1);
    FindEvent<PassBallData>("PassBall", -1)->Add(Function<PassBallData*>(MakeReplayBinding(&ReplayManager::OnPassBall, this)), 0, -1);
    FindEvent<GoalScoredData>("GoalScored", -1)->Add(Function<GoalScoredData*>(MakeReplayBinding(&ReplayManager::OnGoalScored, this)), 0, -1);
    FindEvent<GoalieSaveData>("GoalieSave", -1)->Add(Function<GoalieSaveData*>(MakeReplayBinding(&ReplayManager::OnGoalieSave, this)), 0, -1);
    FindEvent<NoEventData>("Kickoff", -1)->Add(Function<FnVoidVoid>(MakeReplayBinding(&ReplayManager::OnKickoff, this)), 0, -1);
}

void ReplayManager::InitializeSnapshots()
{
    for (int i = 0; i < 3; i++)
    {
        mSnapshots[i].Initialize();
    }
}

void ReplayManager::OnMegaStrikeResult()
{
    mEvents |= 0x40;
}

void ReplayManager::OnReceiveBall(ReceiveBallData* event)
{
    mEvents |= 4;
}

void ReplayManager::OnShotAtGoal(ShotAtGoalData* event)
{
    mEvents |= 2;
}

void ReplayManager::OnPassBall(PassBallData* event)
{
    mEvents |= 8;
}

void ReplayManager::OnGoalScored(GoalScoredData* event)
{
    if (event->uGoalType != GOAL_MEGA_STRIKE)
    {
        mEvents |= 1;
    }
}

void ReplayManager::OnGoalieSave(GoalieSaveData* event)
{
    mEvents |= 0x11;
}

void ReplayManager::OnKickoff()
{
    mEvents |= 0x20;
}

void ReplayManager::Uninitialize()
{
    for (int i = 0; i < 3; i++)
    {
        mSnapshots[i].Free();
    }

    delete mReplay;
    mReplay = 0;

    nlFree(mMemory);
    mMemory = 0;
#ifdef NP_NATIVE
    ++sNpReplayGen;
#endif
}

void ReplayManager::SwapPreviousAndCurrent()
{
    RenderSnapshot* tmp = mCurrent;
    mCurrent = mPrevious;
    mPrevious = tmp;
}

// NP_REPLAY_HASH=1 (proofs only; nothing without it): every 300 recorded steps print an FNV-1a hash of everything the recorder holds (frame bytes, time, interval, events,
// excitement of every frame of reel 0), so that two builds or switches can be compared on the bytes of the replay itself.
static void NpReplayHashStep(Replay* replay)
{
    static int on = -1;
    static unsigned steps = 0;
    if (on < 0)
    {
        const char* e = getenv("NP_REPLAY_HASH");
        on = (e != 0 && *e == '1') ? 1 : 0;
    }
    if (on == 0 || ++steps % 300 != 0)
        return;
    unsigned long long h = 1469598103934665603ull;
    unsigned frames = 0;
    unsigned long long bytes = 0;
    for (Replay::Frame* f = replay->mReels[0].mBegin; f != 0; f = replay->Next(f, 0))
    {
        const unsigned char* p = (const unsigned char*)f->mBegin;
        for (int i = 0; i < f->mSize; i++)
            h = (h ^ p[i]) * 1099511628211ull;
        unsigned meta[4];
        memcpy(&meta[0], &f->mTime, 4);
        meta[1] = (unsigned)f->mInterval;
        meta[2] = f->mEvents;
        meta[3] = f->mExcitement;
        const unsigned char* m = (const unsigned char*)meta;
        for (int i = 0; i < 16; i++)
            h = (h ^ m[i]) * 1099511628211ull;
        frames++;
        bytes += (unsigned)f->mSize;
    }
    fprintf(stderr, "[replayhash] step %u frames %u bytes %llu hash %016llx\n", steps, frames, bytes, h);
}

void ReplayManager::GrabSnapshot()
{
    SwapPreviousAndCurrent();

    mCurrent->Grab();

    if (nlTaskManager::m_pInstance->mCurrentState == TASK_GAMEPLAY)
    {
        mTime = mReplay->EndTime() + g_fSimulationTick;
        unsigned int excitement = ExcitementSystem::Instance().mExcitement;
        excitement <<= 16;
        excitement += ExcitementSystem::Instance().mExcitementCount;
        mReplay->Record<RenderSnapshot>(mTime, *mCurrent, mEvents, excitement);
        NpReplayHashStep(mReplay);
        {
            // [renderneon] verify (MSC_RENDER_NEON=2) and the [snapcopy] mode: a line at the first step and then every 1200 steps
            static unsigned reportSteps = 0;
            if (reportSteps++ % 1200 == 0)
            {
                if (NpRenderNeon() == 2)
                    fprintf(stderr, "[renderneon] verify: step %u kernel calls %u, NEON results that differ from the scalar %u\n", reportSteps - 1, gNpRenderNeonCalls, gNpRenderNeonMismatch);
                if (reportSteps == 1)
                    fprintf(stderr, "[snapcopy] MSC_SNAP_ONECOPY=%d MSC_RENDER_NEON=%d\n", NpSnapOneCopy(), NpRenderNeon());
            }
        }

        ExcitementSystem& state = ExcitementSystem::Instance();
        state.mExcitement = 0;
        state.mExcitementCount = 0;
        mEvents = 0;
    }
}

RenderSnapshot& ReplayManager::GetMutableRenderSnapshot()
{
    mRender = mCurrent;
    return mRender->GetMutable();
}

void ReplayManager::Flush()
{
    delete mReplay;
    mReplay = new (nlMalloc(sizeof(Replay), 8, false)) Replay((char*)mMemory, 0x100000, 0x8000);

    ResetSnapshots();
#ifdef NP_NATIVE
    ++sNpReplayGen;
#endif
}

#include "Game/Render/NPCManager.inl"

void ReplayManager::DoPotentialAutoReplay(float deltaTime)
{
    if (nlTaskManager::m_pInstance->mCurrentState == TASK_REPLAY && !gbLoadingReplay)
    {
        mSpeed = mSpeedUp * deltaTime + mSpeed;
        if (mSpeed < 0.1f)
        {
            mSpeed = 0.1f;
        }
        mDeltaTime = mSpeed * deltaTime;
        mTime = mTime + mDeltaTime;
        mReplay->Play<RenderSnapshot>(mTime, *mPrevious, *mCurrent, mBlend);
    }
}

float GetRemoteReplaySpeed()
{
    static TweakFloatBinding unidentifiedScale(
        "/Camera/Debug Cam/Revolution/Revolution Accelerometer Scale", 1.0f);

    float acceleration = g_pPlatPadManager->GetFreestyleStatus(0)->wpad.accX;
    float speed;
    if (gbDebugReplayFixedStepMode == 1)
    {
        speed = acceleration * unidentifiedScale;
        speed = nlMaxEquals(speed, -1.0f);
        speed = nlMinEquals(speed, 1.0f);
    }
    else
    {
        speed = acceleration * unidentifiedScale;
        speed = (1.0f + speed) / 2.0f;
        if (speed < 0.0f)
        {
            speed = 0.0f;
        }
        if (speed > 1.0f)
        {
            speed = 1.0f;
        }
    }

    nlScreenPrintf(0, 1, false, 0, "Replay Speed %0.1f", speed);
    return speed;
}

void ReplayManager::DoPotentialDebugReplay(float& deltaTime)
{
    static bool debugReplay
        = GetConfigBool(Config::Global(), "debug_replay_in_release", false);

    cGlobalPad* unidentifiedPad = g_pPadManager->GetPad(0);
    if (debugReplay && !g_bTweaking && !IsProfiling()
        && !IsNetworkOrRecordedGame()
        && unidentifiedPad->PlatJustPressed(4, true))
    {
        if (nlTaskManager::m_pInstance->mCurrentState == TASK_DEBUG_REPLAY)
        {
            nlTaskManager::SetNextState(TASK_GAMEPLAY);
            if (mReplayDebugCamera != 0)
            {
                cCameraManager::PopCamera();
                delete mReplayDebugCamera;
                mReplayDebugCamera = 0;
            }
            return;
        }
        else if (nlTaskManager::m_pInstance->mCurrentState == TASK_GAMEPLAY)
        {
            mTime = mReplay->EndTime();
            nlTaskManager::SetNextState(TASK_DEBUG_REPLAY);
        }
        return;
    }

    if (nlTaskManager::m_pInstance->mCurrentState == TASK_DEBUG_REPLAY)
    {
        if (cCameraManager::PeekCamera()->GetType() != eCameraType_Debug)
        {
            mReplayDebugCamera
                = new (nlMalloc(sizeof(cDebugCamera), 8, false)) cDebugCamera(true);
            cCameraManager::PushCamera(mReplayDebugCamera);
        }

        mDeltaTime = 0.0f;
        if (gbDebugReplayFixedStepMode == 1)
        {
            mDeltaTime = 0.02f * unidentifiedPad->GetPressure(5, true);
            if (unidentifiedPad->GetPressure(5, true))
            {
                mDeltaTime = 0.02f;
            }
            else if (unidentifiedPad->GetPressure(6, true))
            {
                mDeltaTime = 0.02f * 5.0f;
            }
        }
        else
        {
            mDeltaTime
                -= 0.02f * unidentifiedPad->GetPressure(5, true);
            mDeltaTime
                += 0.02f * unidentifiedPad->GetPressure(6, true);
        }

        int unidentifiedClassID
            = unidentifiedPad->mBackend->GetClassID();
        if (g_pPlatPadManager->type[0] == 2
            && (unidentifiedClassID == gWiiRemotePadClassID
                || unidentifiedClassID == gWiiFreestylePadClassID))
        {
            mDeltaTime *= GetRemoteReplaySpeed();
        }

        float time = mTime + mDeltaTime;
        if (time < mReplay->BeginTime())
        {
            time = mReplay->BeginTime();
        }
        if (time > mReplay->EndTime())
        {
            time = mReplay->EndTime();
        }

        mDeltaTime = time - mTime;
        mTime = time;
        mReplay->Play<RenderSnapshot>(mTime, *mPrevious, *mCurrent, mBlend);
    }
}

float lbl_806E14CC;
#ifdef NP_NATIVE
extern "C" int NpSmoothWanted(void);
extern "C" int NpSmoothBreak(void);
extern "C" void NpSmoothNoteDraw(float blend, float rep, float live, int blended, int stuck, const float* ball, const float* pos, const float* bone);
#endif
bool gbSavingReplay;
bool gbLoadingReplay;

void ReplayManager::ResetSnapshots()
{
    for (int i = 0; i < 3; i++)
    {
        mSnapshots[i].Invalidate();
    }

    GrabSnapshot();
}

void ReplayManager::PrepareForRecording()
{
    cCameraManager::Remove(mDebugCamera);
    mTime = mReplay->EndTime();
    mPrevious->Invalidate();
    mCurrent->Invalidate();
    mRender = 0;
}

void ReplayManager::SetCurrentTime(float time)
{
    mTime = time;

    if (mTime < mReplay->BeginTime())
    {
        mTime = mReplay->BeginTime();
    }

    if (mTime > mReplay->EndTime())
    {
        mTime = mReplay->EndTime();
    }
}

static bool NisOverridesReplayBuffer()
{
    return nlTaskManager::m_pInstance->mCurrentState == TASK_NIS || (nlTaskManager::m_pInstance->mPreviousState == TASK_NIS && nlTaskManager::m_pInstance->mCurrentState == TASK_PAUSED);
}

#ifdef NP_NATIVE
// wave 10 (MSC_BLEND_STEP, simthread10): the blended snapshot (mRender) is what the gameplay camera aims at (GameplayCam.cpp CameraOwnerPosition and the free
// ball), and the camera decides which players are on screen, which the simulation takes (cGame::SendPlayerVisibility). So the blend must not depend on whether
// the drawing task ran this picture: the simulation task makes it (end of FixedUpdateTask::Run, gameplay only, where the debug/auto replay steps below are
// no-ops) and RenderSnapshotAt, when the drawing runs, finds it done and only draws.
static bool sBlendEarlyDone = false;
extern "C" int NpBlendStepOn();
extern "C" void NpBlendStepCount(int made);
extern "C" void NpBlendEarly()
{
    sBlendEarlyDone = false;
    ReplayManager* r = ReplayManager::Instance();
    if (r == 0 || nlTaskManager::m_pInstance->mCurrentState != TASK_GAMEPLAY)
    {
        return;
    }
    for (int i = 0; i < 3; i++)
    {
        r->mBlend[i] = GetFixedUpdateTask()->mInterpolationDeltaT / g_fFixedUpdateTick;
    }
    r->mDeltaTime = GetFixedUpdateTask()->mInterpolationDeltaT;
    r->mRender = r->mCurrent;
    if (!NisOverridesReplayBuffer() && r->mPrevious->mValid)
    {
        { NP_GS(NPGS_R_BLEND); r->mSnapshots[2].Blend(r->mBlend, *r->mPrevious, *r->mCurrent); }
        r->mRender = &r->mSnapshots[2];
    }
    sBlendEarlyDone = true;
    NpBlendStepCount(1);
}

// gt27 (MSC_BLEND_ASYNC, RenderSnapshot.cpp): the top of RenderFrame kicks the live-play blend that RenderSnapshotAt will make below (same snapshots, same factors);
// the join and its check are in RenderSnapshot::Blend. Only where RenderSnapshotAt's own steps before the blend are no-ops: gameplay, no PiP, no blend step, no smooth mutant.
bool NpBlendAsyncBegin(RenderSnapshot& d, const float* f, RenderSnapshot& lhs, RenderSnapshot& rhs);
extern "C" int NpBlendAsyncOn();
extern "C" void NpBlendAsyncCount(int notKicked);
extern "C" void NpBlendAsyncKick(int pip)
{
    if (!NpBlendAsyncOn())
    {
        return;
    }
    ReplayManager* r = ReplayManager::Instance();
    if (pip || r == 0 || nlTaskManager::m_pInstance->mCurrentState != TASK_GAMEPLAY || NpBlendStepOn() || NpSmoothBreak() == 1 || NisOverridesReplayBuffer() || !r->mPrevious->mValid)
    {
        NpBlendAsyncCount(1);
        return;
    }
    float f[3];
    for (int i = 0; i < 3; i++)
    {
        f[i] = GetFixedUpdateTask()->mInterpolationDeltaT / g_fFixedUpdateTick;
    }
    NpBlendAsyncBegin(r->mSnapshots[2], f, *r->mPrevious, *r->mCurrent);
}
#endif

void ReplayManager::RenderSnapshotAt(float deltaTime)
{
#ifdef NP_NATIVE
    const bool blendDone = sBlendEarlyDone && nlTaskManager::m_pInstance->mCurrentState == TASK_GAMEPLAY;
    sBlendEarlyDone = false;
    if (blendDone)
    {
        NpBlendStepCount(0);
    }
    else
    {
#endif
    for (int i = 0; i < 3; i++)
    {
        mBlend[i] = GetFixedUpdateTask()->mInterpolationDeltaT / g_fFixedUpdateTick;
    }
    mDeltaTime = GetFixedUpdateTask()->mInterpolationDeltaT;
#ifdef NP_NATIVE
    if (NpSmoothBreak() == 1)   // (np_smooth.cpp MSC_SMOOTH_BREAK=1, a proof mutant for tools/fps/smooth_trace.py: no blend, every picture shows the newest step)
        for (int i = 0; i < 3; i++) mBlend[i] = 1.0f;
#endif

    DoPotentialDebugReplay(deltaTime);
    DoPotentialAutoReplay(deltaTime);

    mRender = mCurrent;

    bool transitioning = NisOverridesReplayBuffer();

    if (!transitioning && mPrevious->mValid)
    {
        { NP_GS(NPGS_R_BLEND); mSnapshots[2].Blend(mBlend, *mPrevious, *mCurrent); }
        mRender = &mSnapshots[2];
    }
#ifdef NP_NATIVE
    }
#endif

    { NP_GS(NPGS_R_CHARS); mRender->Render(deltaTime); }
    lbl_806E14CC = mRender->mSimulationTime;
#ifdef NP_NATIVE
    if (NpSmoothWanted())   // wave 9 helper smooth (np_smooth.cpp, MSC_SMOOTH_LOG / MSC_SMOOTH_TRACE): what this picture shows, for the [smooth] line
    {
        float pos[30], bone[30], ball[3];
        for (int i = 0; i < 10; i++)
        {
            const DrawableCharacter& c = mRender->mCharacters[i];
            const bool have = c.character != 0 && c.visible;
            pos[3 * i] = have ? c.position.x : NAN; pos[3 * i + 1] = have ? c.position.y : NAN; pos[3 * i + 2] = have ? c.position.z : NAN;
            bone[3 * i] = bone[3 * i + 1] = bone[3 * i + 2] = NAN;
            if (have && !c.poseCulled && c.poseAccumulator != 0 && c.poseAccumulator->m_NodeMatrices != 0 && c.poseAccumulator->GetNumNodes() > 20)
            {
                const nlVector3 t = c.poseAccumulator->m_NodeMatrices[20].GetTranslation();   // (a limb bone, model space: the pose alone, without the root's travel)
                bone[3 * i] = t.x; bone[3 * i + 1] = t.y; bone[3 * i + 2] = t.z;
            }
        }
        ball[0] = mRender->mBall.mPosition.x; ball[1] = mRender->mBall.mPosition.y; ball[2] = mRender->mBall.mPosition.z;
        NpSmoothNoteDraw(mBlend[0], mRender->mSimulationTime, mCurrent->mSimulationTime, mRender == &mSnapshots[2] ? 1 : 0, mBlend[0] >= 0.9999f ? 1 : 0, ball, pos, bone);
    }
#endif

    if (nlTaskManager::m_pInstance->mCurrentState == TASK_DEBUG_REPLAY)
    {
        mSnapshots[2].RenderDebugInfo(*mPrevious, *mCurrent, mBlend[0]);
    }
}

int ReplayManager::GetReplayExcitement(float time) const
{
    if (Instance()->IsSavingReplay() || Instance()->IsLoadingReplay())
    {
        return 0;
    }

    Replay::Frame* begin;
    unsigned int value = 0;
    unsigned int count = 0;
    if (mReplay != 0 && mReplay->mReelIdx == 0)
    {
        begin = mReplay->mReels[mReplay->mReelIdx].mBegin;
        const float endTime = mReplay->EndTime();
#ifdef NP_NATIVE
        const int replayMode = NpCpu22Mode(NP_C22_REPLAY);
        static unsigned sExcGen = 0;
        static const Replay* sExcReplay = 0;
        static const Replay::Frame* sExcBegin = 0;
        static const Replay::Frame* sExcLast = 0;
        static int sExcTick = -1;
        static float sExcEndTime = 0.0f;
        static float sExcTime = 0.0f;
        static int sExcResult = 0;
        const Replay::Frame* const last = mReplay->mReels[0].mLast;
        const bool excHit = replayMode != 0 && sExcGen == sNpReplayGen && mReplay == sExcReplay
            && begin == sExcBegin && last == sExcLast && mReplay->mTick == sExcTick
            && endTime == sExcEndTime && time == sExcTime;
        if (replayMode == 1 && excHit)
        {
            return sExcResult;
        }
#endif
        float beginTime = nlMaxEquals(time + endTime, 0.0f);
        float goalTime;
        float kickoffTime;
#ifdef NP_NATIVE
        if (const int mode = replayMode)   // MSC_CPU22_REPLAY: one walk of the ring for the two times (include/np/cpu22.h)
        {
            float g2, k2;
            mReplay->TimeOfLastOccurence2(1, NpCpu22Break() == 3 ? 0x40 : 0x20, &g2, &k2);
            NpCpu22Note(NP_C22_REPLAY, 0);
            NpCpu22Note(NP_C22_REPLAY, 1);
            if (mode == 1)
            {
                goalTime = g2;
                kickoffTime = k2;
            }
            else
            {
                goalTime = mReplay->TimeOfLastOccurence(1);
                kickoffTime = mReplay->TimeOfLastOccurence(0x20);
                if (memcmp(&goalTime, &g2, sizeof g2) != 0 || memcmp(&kickoffTime, &k2, sizeof k2) != 0)
                    NpCpu22Note(NP_C22_REPLAY, 2);
            }
        }
        else
#endif
        {
            goalTime = mReplay->TimeOfLastOccurence(1);
            kickoffTime = mReplay->TimeOfLastOccurence(0x20);
        }
        if (goalTime > kickoffTime)
        {
            beginTime = nlMaxEquals(goalTime + time, 0.0f);
        }
        beginTime = nlMaxEquals(beginTime, kickoffTime);

        float lastTime = 0.0f;
        for (Replay::Frame* frame = begin->mNext;
             frame != 0 && frame != begin; frame = frame->mNext)
        {
            if (frame->mTime > beginTime && frame->mTime <= endTime
                && frame->mTime != lastTime && frame->mExcitement != 0)
            {
                lastTime = frame->mTime;
                value += frame->mExcitement >> 16;
                count += frame->mExcitement & 0xFFFF;
            }
        }
#ifdef NP_NATIVE
        const int excResult = (unsigned short)count * (unsigned short)value;
        if (replayMode == 2 && excHit && excResult != sExcResult)
        {
            NpCpu22Note(NP_C22_REPLAY, 2);
        }
        if (replayMode != 0)
        {
            sExcGen = sNpReplayGen;
            sExcReplay = mReplay;
            sExcBegin = begin;
            sExcLast = last;
            sExcTick = mReplay->mTick;
            sExcEndTime = endTime;
            sExcTime = time;
            sExcResult = excResult;
            return sExcResult;
        }
#endif
    }
    else if (mReplay != 0)
    {
        value = (unsigned short)mReplay->mReels[mReplay->mReelIdx].mAge;
        count = 1;
    }

    return (unsigned short)count * (unsigned short)value;
}

static void OnReplayFileClosed(s32 result)
{
}

static void OnReplayWritten(s32 result)
{
    gbSavingReplay = false;
    if (result < 0)
    {
        gbReplayFlashError = true;
    }
    else
    {
        nlFlashClose(OnReplayFileClosed);
    }
}

static void OnReplayRead(s32 result)
{
    gbLoadingReplay = false;
#ifdef NP_NATIVE
    if (result < 0x100000)
    {
        gbReplayFlashError = true;
        if (result >= 0)
        {
            nlFlashClose(0);
        }
        GetPresentation()->SendSkipNis();
    }
    else
    {
        nlFlashClose(0);
    }
#else
    if (result < 0)
    {
        gbReplayFlashError = true;
        GetPresentation()->SendSkipNis();
    }
    else
    {
        nlFlashClose(0);
    }
#endif
}

bool ReplayManager::IsSavingReplay() const
{
    return gbSavingReplay;
}

bool ReplayManager::IsLoadingReplay() const
{
    return gbLoadingReplay;
}

bool ReplayManager::SaveReplay(int index)
{
    if (SaveError == 1 || gbReplayFlashError == 1)
    {
        return false;
    }

    char name[10];
    nlSNPrintf(name, 10, "REPLAY_%d", index);
    if (nlFlashChangeDirectory(2, 0) != 0)
    {
        return false;
    }

    s32 result = nlFlashOpen(name, 2, 0);
    if (result != 0)
    {
        if (nlFlashCreate(name, 0x30, 0) != 0)
        {
            return false;
        }
        result = nlFlashOpen(name, 2, 0);
    }
    if (result != 0)
    {
        return false;
    }

    gbSavingReplay = true;
    nlFlashWrite(mMemory, 0x100000, OnReplayWritten);
    return gbSavingReplay;
}

bool ReplayManager::LoadReplay(int index)
{
    if (SaveError == 1 || gbReplayFlashError == 1)
    {
        GetPresentation()->SendSkipNis();
        return false;
    }

    char name[128];
    nlSNPrintf(name, 10, "REPLAY_%d", index);
    if (nlFlashChangeDirectory(2, 0) != 0)
    {
        GetPresentation()->SendSkipNis();
        return false;
    }
    if (nlFlashOpen(name, 1, 0) != 0)
    {
        GetPresentation()->SendSkipNis();
        return false;
    }

    gbLoadingReplay = true;
#ifdef NP_NATIVE
    ++sNpReplayGen;
#endif
    u32 size = 0x100000;
    if (nlFlashRead((void**)&mMemory, &size, OnReplayRead, true) != 0)
    {
        GetPresentation()->SendSkipNis();
        return false;
    }
    return gbLoadingReplay;
}
