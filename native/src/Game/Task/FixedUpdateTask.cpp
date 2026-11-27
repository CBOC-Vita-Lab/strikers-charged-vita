#include "Game/Task/GameTaskState.h"
#include "np/gsplit.h"
#include "NL/nlDLListContainer.inl"
#include "Game/Task/FixedUpdateTask.h"
#include "Game/Task/GameRenderTask.h"
#include "Game/HBMManager.h"
#ifdef NP_NATIVE
#include "np/touch_diag.h"
#endif
#include "Game/EventDispatcher.inl"
#include "platform/sdk/net/np_lan.h"  // multiplayer (np/patches/mp)
#ifdef NP_NATIVE
#include "np/fps.h"
#endif

#include "Game/FE/feManager.h"

#include "Game/Physics/PhysicsCharacter.h"

#include "Game/Render/Presentation.h"

#include "Game/Render/PeachPhoto.h"

#include "Game/AI/AiUtil.h"
#include "Game/Ball.h"
#include "Game/Camera/CameraMan.h"
#include "Game/CharacterTemplate.h"
#include "Game/DebugWriteCache.h"
#include "Game/Field.h"
#include "Game/Game.h"
#include "Game/Goalie.h"
#include "Game/NetworkSession.h"
#include "Game/NetworkStatsManager.h"
#include "Game/Pad/FlickDetection.h"
#include "Game/Physics/Physics.h"
#include "Game/Physics/PhysicsAIBall.h"
#include "Game/Render/NPCManager.h"
#include "Game/Render/NetMesh.h"
#include "Game/ReplayManager.h"
#include "np/simorder17.h"
#include "Game/Sys/clock.h"
#include "Game/Team.h"
#include "Game/SharedStaticStorage.h"
#include "NL/globalpad.h"
#include "NL/platpad.h"
#include "NL/nlMain.h"
#include "NL/nlPrint.h"
#include "NL/nlString.h"
#include "types.h"
#include "Game/DetInput.h"
#include "Game/InputRouter.h"
#include "Game/NetworkInput.h"
#include "Game/NetworkSync.h"

#include <math.h>

float g_fFixedUpdateTick = 0.02f;
bool g_bRunSimAndRenderInLockStep;

static u16 sSimulationTimeType = 0xFFFF;
static u16 sTimeScaleType = 0xFFFF;

void fn_80111654(int)
{
}

void fn_80111658(int)
{
}

void fn_8011165C(int)
{
}

void fn_80111660(int)
{
}

bool fn_80111664()
{
    return false;
}

static FixedUpdateTask fixedUpdateTask;
float g_fSimulationTick = g_fFixedUpdateTick;

FixedUpdateTask* GetFixedUpdateTask()
{
    return &fixedUpdateTask;
}

EventDispatcher* GetFixedUpdateEventDispatcher()
{
    return &fixedUpdateTask.mEventDispatcher;
}

void FixedUpdateTask::Reset()
{
    DispatcherHeaderScope headerScope;
    DispatcherInlineScope inlineScope;

    mInterpolationDeltaT = mAccumulatedDeltaT = g_fFixedUpdateTick;
    mSimulationTime = 0.0f;
    mTimeScale = 1.0f;
    mfFrameLockTime = 0.0f;
    mFrame = 0;
    mSimulationStarted = false;

    mEventDispatcher.Clear();
    mEventDispatcher.FreeBlocks();
}

FixedUpdateTask::FixedUpdateTask()
{
    mInterpolationDeltaT = mAccumulatedDeltaT = g_fFixedUpdateTick;
    mSimulationTime = 0.0f;
    mTimeScale = 1.0f;
    mfFrameLockTime = 0.0f;
    mFrame = 0;
    mSimulationStarted = false;

    mEventDispatcher.Clear();
    BasicSlotPool<DLListEntry<EventCallback> >* pool = &mEventDispatcher.callbacks.m_Allocator;
    pool->FreeBlocks();
}

float FixedUpdateTask::GetFixedUpdateMilliseconds()
{
    return 1000.0f * g_fFixedUpdateTick;
}

u32 FixedUpdateTask::CalculateChecksum()
{
    RunningChecksum checksum;
    float simulationTime = fixedUpdateTask.mSimulationTime;
    checksum.ChecksumData(&simulationTime, sizeof(simulationTime));
    g_pGame->ChecksumState(&checksum);
    g_pBall->ChecksumState(&checksum);
    for (int i = 0; i < 2; i++)
    {
        g_pTeams[i]->ChecksumState(&checksum);
    }
    NpLanChecksumExtra(&checksum);   // LAN sessions only: state the retail checksum does not read (STR-151)
    return ~checksum.m_nChecksum;
}

#define PAD_FIELD_OFFSET(pad, field) \
    ((unsigned char*)&(pad)->field - (unsigned char*)(pad))

inline void RegisterDetInputDebugFields(DebugWriteCache* cache, DetInput* pad)
{
    gDetInputDebugType = cache->BeginType("DetInput");
    cache->AddField(17, gDebugFieldTypes[17].size, 0, "m_AnalogLeftX");
    cache->AddField(17, gDebugFieldTypes[17].size, PAD_FIELD_OFFSET(pad, m_AnalogLeftY), "m_AnalogLeftY");
    cache->AddField(17, gDebugFieldTypes[17].size, PAD_FIELD_OFFSET(pad, m_AnalogRightX), "m_AnalogRightX");
    cache->AddField(17, gDebugFieldTypes[17].size, PAD_FIELD_OFFSET(pad, m_AnalogRightY), "m_AnalogRightY");
    cache->AddField(0, gDebugFieldTypes[0].size, PAD_FIELD_OFFSET(pad, m_nConnected), "m_nConnected");
    cache->AddField(1, gDebugFieldTypes[1].size, PAD_FIELD_OFFSET(pad, m_ButtonBitfield), "m_ButtonBitfield");
    cache->AddField(0, gDebugFieldTypes[0].size, PAD_FIELD_OFFSET(pad, m_LeftTrigger), "m_LeftTrigger");
    cache->AddField(0, gDebugFieldTypes[0].size, PAD_FIELD_OFFSET(pad, m_RightTrigger), "m_RightTrigger");
    cache->AddField(22, gDebugFieldTypes[22].size, PAD_FIELD_OFFSET(pad, m_v3RevRemoteAccel), "m_v3RevRemoteAccel");
    cache->AddField(22, gDebugFieldTypes[22].size, PAD_FIELD_OFFSET(pad, m_v3RevFreeStyleAccel), "m_v3RevFreeStyleAccel");
    cache->AddField(0, gDebugFieldTypes[0].size, PAD_FIELD_OFFSET(pad, m_nRevDPDNumTargets), "m_nRevDPDNumTargets");
    cache->AddField(21, gDebugFieldTypes[21].size, PAD_FIELD_OFFSET(pad, m_v2RevDPDCoord), "m_v2RevDPDCoord");
    cache->AddField(15, gDebugFieldTypes[15].size, PAD_FIELD_OFFSET(pad, m_pPrevInput), "m_pPrevInput");
    cache->AddField(15, gDebugFieldTypes[15].size, PAD_FIELD_OFFSET(pad, m_pMyUser), "m_pMyUser");
    cache->AddField(19, gDebugFieldTypes[19].size, PAD_FIELD_OFFSET(pad, m_PolarAnalogLeft.a), "m_PolarAnalogLeft.a");
    cache->AddField(17, gDebugFieldTypes[17].size, PAD_FIELD_OFFSET(pad, m_PolarAnalogLeft.r), "m_PolarAnalogLeft.r");
    cache->AddField(19, gDebugFieldTypes[19].size, PAD_FIELD_OFFSET(pad, m_PolarAnalogLeft.a), "m_PolarAnalogLeft.a");
    cache->AddField(17, gDebugFieldTypes[17].size, PAD_FIELD_OFFSET(pad, m_PolarAnalogLeft.r), "m_PolarAnalogLeft.r");
    cache->AddArrayField(8, gDebugFieldTypes[8].size, 13, PAD_FIELD_OFFSET(pad, m_buttonStateTicks), "m_buttonStateTicks");
    cache->AddField(19, gDebugFieldTypes[19].size, PAD_FIELD_OFFSET(pad, m_aRemapAngle), "m_aRemapAngle");
    cache->EndType();
}

u32 FixedUpdateTask::WriteSyncLog()
{
    DebugWriteCache* cache = gNetworkSyncState->GetWriteCache();
    if (cache == 0)
    {
        return 0;
    }

    cache->BeginFrame(GetFrame());

    RunningChecksum checksum;
    cache->WriteFloat(&sSimulationTimeType, "simulationTime", &checksum,
        fixedUpdateTask.mSimulationTime);
    cache->WriteFloat(&sTimeScaleType, "timeScale", &checksum,
        fixedUpdateTask.mTargetTimeScale);

    int numGroups = g_pNetworkSessionBase->GetNumMachines();
    for (int groupIndex = 0; groupIndex < numGroups; groupIndex++)
    {
        NetworkPeer* group = g_pNetworkSessionBase->GetPeer((s8)groupIndex);
        int numControllers = group->mPlayerCount;
        for (int controllerIndex = 0; controllerIndex < numControllers; controllerIndex++)
        {
            DetInput* pad = (group->GetNetworkPeerChannel(controllerIndex))->GetNetworkPeerChannelInput();
            if (gDetInputDebugType == 0xFFFF)
            {
                RegisterDetInputDebugFields(cache, pad);
            }

            DetInput* copy =
                (DetInput*)cache->WriteData(gDetInputDebugType, pad, sizeof(DetInput));
            if (copy != 0)
            {
                copy->m_pPrevInput = 0;
                copy->m_pMyUser = (void*)pad->GetPadID();
                cache->ChecksumData(gDetInputDebugType, copy, &checksum);
            }
        }
    }

    g_pGame->SyncLog(&checksum, cache);
    g_pBall->SyncLog(&checksum, cache);
    for (int i = 0; i < 2; i++)
    {
        g_pTeams[i]->SyncLog(&checksum, cache);
    }
    g_PhysicsWorld->SyncLog(&checksum, cache);

    u32 crc = ~checksum.m_nChecksum;
    char buffer[0x100];
    nlSNPrintf(buffer, sizeof(buffer),
        "------------------------ END Frame:%d CRC:%x -------------------------\n\n",
        GetFrame(), crc);
    cache->WriteText(buffer);
    return crc;
}

#undef PAD_FIELD_OFFSET

void FixedUpdateTask::OnSyncError()
{
    NpLanLog("sync error reported to the player\n");
    if (g_pNetworkSession->GetSessionMode())
    {
        NetworkStatsManager::Instance()->CalculateAndReportGameResult(2);
    }
    g_pNetworkSession->PopupNetworkError(NET_ERROR_SYNC);
}

void FixedUpdateTask::OnInputQueueOverflow()
{
    NpLanLog("input queue overflow\n");
    NetworkStatsManager::Instance()->CalculateAndReportGameResult(4);
    g_pNetworkSession->PopupNetworkError(NET_ERROR_QUEUE_OVERFLOW);
}

u16 FixedUpdateTask::GetInputRemapAngle()
{
    return cCameraManager::m_aJoystickRemap - 0x4000;
}

bool FixedUpdateTask::IsInPauseMenu()
{
    return FrontEnd::m_bInPauseMenuState;
}

const char* FixedUpdateTask::GetName()
{
    return "Game Fixed Update";
}

float FixedUpdateTask::GetPhysicsUpdateTick()
{
    return g_fSimulationTick;
}

void FixedUpdateTask::SetTimeScale(float timeScale)
{
    fixedUpdateTask.mTimeScale = timeScale;
    fixedUpdateTask.mTargetTimeScale = timeScale;
}

float FixedUpdateTask::GetTargetTimeScale()
{
    return fixedUpdateTask.mTargetTimeScale;
}

void FixedUpdateTask::SetTimeScale(float timeScale, float transitionTime)
{
    fixedUpdateTask.mTimeScaleTransitionTime = transitionTime;
    fixedUpdateTask.mTimeScaleTransitionRemaining = transitionTime;
    fixedUpdateTask.mTimeScaleTransitionStart = fixedUpdateTask.mTimeScale;
    fixedUpdateTask.mTargetTimeScale = timeScale;
}

float FixedUpdateTask::GetTimeScale()
{
    return fixedUpdateTask.mTimeScale;
}

extern "C" float NpFpsGameTimeScale(void) { return fixedUpdateTask.mTimeScale; }   // pacing13: np_fps.cpp's [speed] line judges against what the game wanted

void FixedUpdateTask::SetFrameLock(float frameLockTime)
{
    fixedUpdateTask.mfFrameLockTime = frameLockTime;
    nlTaskManager::SetNextState(TASK_PAUSED);
}

void FixedUpdateTask::DecrementFrameLock(float fDeltaT)
{
    fixedUpdateTask.mfFrameLockTime -= fDeltaT;
    if (fixedUpdateTask.mfFrameLockTime < 0.0f)
    {
        if (nlTaskManager::m_pInstance->mCurrentState == TASK_PAUSED
            && nlTaskManager::m_pInstance->mPendingState != TASK_NIS)
        {
            nlTaskManager::SetNextState(TASK_GAMEPLAY);
        }
        fixedUpdateTask.mfFrameLockTime = 0.0f;
    }
}

#ifdef NP_NATIVE
extern "C" void NpVitaPadHoldTaps(int on) __attribute__((weak));
extern "C" void NpSmoothNoteSteps(int steps);
bool NpLanActive() __attribute__((weak));
#endif

#ifdef NP_NATIVE
extern "C" int NpFrameLockStepOn();            // src/Game/NpSimThread.cpp (MSC_FRAMELOCK_STEP)
extern "C" void NpFrameLockCount(int moved);
extern "C" int NpBlendStepOn();                // MSC_BLEND_STEP
extern "C" void NpBlendEarly();                // src/Game/ReplayManager.cpp
#include "np/simrun17.h"                           // block17 (MSC_SIMT)
#endif

// The step loop (block17: one function, so that MSC_SIMT=1 can run it on np_sim; plan-simthread17 stage 2a). The members it touches are the task's own, written between the
// kick and the join by one thread only; the caller reads them after the join.
static void FixedStepLoop(FixedUpdateTask* t, int* stepsRan)
{
    float simulationTick;
#ifdef NP_NATIVE
    { NP_GS(NPGS_SP_EDGE);   // gt27 (MSC_GSPLIT): the probes around the steps
    NpFpsSimBegin();   // FPS (np/fps.h): the cost of the simulation steps of this frame, for MSC_FPS=auto (the thread clock is the calling thread's: np_sim's own cost with MSC_SIMT=1)
    NpSimtPhase(3); }
#endif
    while (g_bRunSimAndRenderInLockStep
        || t->mAccumulatedDeltaT >= g_fFixedUpdateTick)
    {
#ifdef NP_NATIVE
        NpGsSeq gsIn(NPGS_SP_INPUT);   // gt27 (MSC_GSPLIT): the step's pad and input reads
#endif
        g_pPadManager->SetActivePadSet(1);
        simulationTick = g_fFixedUpdateTick;
        UpdatePlatPad(g_pPlatPadManager);
        g_pPadManager->Update(simulationTick);
        FlickDetection::Update();

        t->mAccumulatedDeltaT -= g_fFixedUpdateTick;
        if (g_bRunSimAndRenderInLockStep)
        {
            t->mAccumulatedDeltaT = 0.0f;
        }

        int updateCount = gInputManager->GetUpdateCount();
        gInputManager->CaptureInputs();
#ifdef NP_NATIVE
        gsIn.Next(NPGS_SP_LOOP);
#endif

        bool updated = false;
        for (int i = 0; i < updateCount; ++i)
        {
#ifdef NP_NATIVE
            bool prepared;
            { NP_GS(NPGS_SP_INPUT); prepared = gInputManager->PrepareUpdate(); }
            if (prepared)
#else
            if (gInputManager->PrepareUpdate())
#endif
            {
                t->CallFixedUpdateTasks();
                updated = true;
#ifdef NP_NATIVE
                { NP_GS(NPGS_SP_EDGE); NpFpsSimStep(); }
                ++*stepsRan;
#endif
            }
            if (IsNisLoadedOnAllMachines(GetPresentation()))
            {
                break;
            }
        }

        if (updated)
        {
            t->mInterpolationDeltaT = t->mAccumulatedDeltaT;
        }
        else
        {
            t->mInterpolationDeltaT = g_fFixedUpdateTick;
        }

        if (IsNisLoadedOnAllMachines(GetPresentation()))
        {
            break;
        }
        if (g_bRunSimAndRenderInLockStep)
        {
            break;
        }
    }
#ifdef NP_NATIVE
    { NP_GS(NPGS_SP_EDGE); NpFpsSimEnd(); }
#endif
}

#ifdef NP_NATIVE
struct NpStepLoopCtx
{
    FixedUpdateTask* task;
    int stepsRan;
};
static void NpStepLoopTramp(void* p)
{
    NpStepLoopCtx* c = (NpStepLoopCtx*)p;
    FixedStepLoop(c->task, &c->stepsRan);
}
#endif

void FixedUpdateTask::Run(float dt)
{
#ifdef NP_NATIVE
    int stepsRan = 0;
    NP_GS(NPGS_SIM_PRE);
#endif
#ifdef NP_NATIVE
    if (NpDiagEnabled()) { NP_GS(NPGS_SP_EDGE); NpDiagEvent(NPD_TASK_DT,0,NpDiagFrame(),NpDiagFloats(dt,nlTaskManager::m_pInstance->mRealTimeDelta),
        NpDiagFloats(mTimeScale,mAccumulatedDeltaT),mFrame,mSimulationStarted,0); }
#endif
#ifdef NP_NATIVE
    { NP_GS(NPGS_WAIT_PACE); NpFpsLateWaitRun(0); }   // W5 RT (MSC_LATE_WAIT=1, np/fps.h): the 30 fps wait of the previous frame, made now that the pads are about to be read
    if (NpSimOrder17Mode()) NpSimOrder17Early();   // mapi17 (MSC_SIMT_ORDER=B/I, default A = off): the draw window (world NPCs + crowd) starts here, before the step loop (src/Game/NpSimOrder17.cpp)
#endif
    bool runFixedUpdate = true;
    if (IsNisLoadedOnAllMachines(GetPresentation()))
    {
        runFixedUpdate = false;
    }
    if (!mSimulationStarted)
    {
        runFixedUpdate = false;
    }
    if (nlTaskManager::m_pInstance->mPendingState == TASK_NIS)
    {
        runFixedUpdate = false;
    }

    if (runFixedUpdate
        && nlTaskManager::m_pInstance->mCurrentState == TASK_GAMEPLAY
        && !g_pNetworkSession->GetPausedMachineMask())
    {
        if (mTimeScaleTransitionRemaining != 0.0f)
        {
            mTimeScaleTransitionRemaining -= dt * mTimeScale;
            if (mTimeScaleTransitionRemaining < 0.0f)
            {
                mTimeScaleTransitionRemaining = 0.0f;
            }

            float percent = 1.0f
                - mTimeScaleTransitionRemaining / mTimeScaleTransitionTime;
            mTimeScale = Interpolate(
                mTimeScaleTransitionStart, mTargetTimeScale, percent);
        }

        mAccumulatedDeltaT += dt * mTimeScale;
        mInterpolationDeltaT = mAccumulatedDeltaT;
#ifdef NP_NATIVE
        stepsRan = 0;
        if (mAccumulatedDeltaT >= g_fFixedUpdateTick && !g_bRunSimAndRenderInLockStep)
        {
            NP_GS(NPGS_SP_PADCTL);   // gt27: the controller; its sleep is wait-padlate (np_fps.cpp)
            NpFpsPadLateWait();   // wave 6 (MSC_PAD_LATE, default off): read the pad as late as the held cadence's slack allows; single player only
        }
#endif

#ifdef NP_NATIVE
        { NP_GS(NPGS_SP_LAN); NpLanFrameBegin(); }   // delay19 (MSC_LAN_DRAIN, default off): LAN / online only; the datagrams that already arrived are read before the step loop
        {
            NpStepLoopCtx loopCtx = {this, 0};
            NpSimtRun(NpStepLoopTramp, &loopCtx);   // block17 (MSC_SIMT, default off): np/simrun17.h; inline when off
            stepsRan = loopCtx.stepsRan;
            NpSimtAddSteps(stepsRan);
        }
#else
        int unusedSteps = 0;
        FixedStepLoop(this, &unusedSteps);
#endif
#ifdef NP_NATIVE
        NpSmoothNoteSteps(stepsRan);   // wave 9 (np_smooth.cpp): steps of this picture, for the trace
#endif
    }
#ifdef NP_NATIVE
    { NP_GS(NPGS_SP_LAN); NpSimOrder17RunPending(); }   // overlap18 (MSC_SIMT=2): a picture whose step did not run (no kick) draws its window here; nothing pending otherwise
#endif

#ifdef NP_NATIVE
    // wave 10 (MSC_FRAMELOCK_STEP): the frame-lock countdown is the simulation task's, not the drawing task's (same picture, before Game Render)
    NpGsSeq gsTail(NPGS_SP_TAIL);   // gt27 (MSC_GSPLIT)
    if (NpFrameLockStepOn()
        && (nlTaskManager::m_pInstance->mCurrentState & 0x2001B)
        && !(gpHBMManager != 0 && gpHBMManager->mActive && gpHBMManager->mReady)
        && g_bRenderWorld
        && mfFrameLockTime > 0.0f)
    {
        NpFrameLockCount(1);
        DecrementFrameLock(dt);
    }
    if (NpBlendStepOn()
        && (nlTaskManager::m_pInstance->mCurrentState & 0x2001B)
        && !(gpHBMManager != 0 && gpHBMManager->mActive && gpHBMManager->mReady)
        && g_bRenderWorld)
    {
        NpBlendEarly();   // wave 10 (MSC_BLEND_STEP): the blended snapshot the camera aims at is the simulation task's (ReplayManager.cpp)
    }
    gsTail.Next(NPGS_SIM_PAD);
#endif
    g_pPadManager->SetActivePadSet(0);
#ifdef NP_NATIVE
    // wave 9 (MSC_TAP_CARRY, plan on): a frame in gameplay that ran no step (60 pictures against the 50 Hz simulation) reads the pad here only: what this read latches reaches the next step
    if (NpVitaPadHoldTaps) NpVitaPadHoldTaps(runFixedUpdate && stepsRan == 0 && mSimulationStarted && !g_bRunSimAndRenderInLockStep && !(NpLanActive && NpLanActive()) ? 1 : 0);
#endif
    UpdatePlatPad(g_pPlatPadManager);
#ifdef NP_NATIVE
    if (NpVitaPadHoldTaps) NpVitaPadHoldTaps(0);
#endif
    g_pPadManager->Update(dt);
    FlickDetection::Update();
#ifdef NP_NATIVE
    if(NpDiagActive())NpDiagEvent(NPD_SIM_END,0,NpDiagFrame(),NpDiagFloats(mAccumulatedDeltaT,mSimulationTime),mFrame,NpDiagStepId(),NpDiagUsedPoll(),NpDiagFloats(mTimeScale,g_fFixedUpdateTick));
#endif
}

static void AIUpdateTask(float fDeltaT)
{
    g_pGame->PreUpdate(fDeltaT);
    g_pGame->Update(fDeltaT);
}

static void PrePhysicsAITask(float fDeltaT)
{
    int i;
    for (i = 0; i < 10; i++)
    {
        g_pCharacters[i]->Unknown7(fDeltaT);
    }
}

static void PostPhysicsAITask(float fDeltaT)
{
    int i;
    for (i = 0; i < 10; i++)
    {
        g_pCharacters[i]->PrePhysicsUpdate();
    }
    g_pBall->PostPhysicsUpdate(fDeltaT);
}

#ifdef NP_NATIVE
// NP (src/Game/NpDet.cpp): determinism probes around each simulation step (NP_DET_TRACE, NP_STACK_POISON)
extern "C" void NpDetStepBegin();
extern "C" void NpDetStepEnd();
extern "C" void NpDetPhase(int);
#define NP_DET_PHASE(n) NpDetPhase(n)
#else
#define NP_DET_PHASE(n)
#endif

extern "C" void NpSimtStep(int begin);   // NP (src/Game/NpSimThread.cpp): MSC_SIMT_PROBE marks the simulation step

void FixedUpdateTask::CallFixedUpdateTasks()
{
#ifdef NP_NATIVE
    { NP_GS(NPGS_SP_EDGE);   // gt27 (MSC_GSPLIT): the probes at the step's edges
    NpSimtStep(1);
    if (NpDiagEnabled()) NpDiagStep();
    NpDetStepBegin(); }
#endif
    ++lbl_806E2130;
    mFrame++;
    mSimulationTime += g_fSimulationTick;

    { NP_GS(NPGS_SIM_CLOCK);
    ClockManager::Update(g_fSimulationTick);
    GetInputRouter();
    DispatchDetermDataEvents(); }

    { NP_GS(NPGS_SIM_AI);
    AIUpdateTask(g_fSimulationTick); }
    NP_DET_PHASE(1);
    { NP_GS(NPGS_SIM_NPC);
    ClearPlayerPlayerCollisionCache();
    gNPCManager->UpdateAINPCs(g_fSimulationTick); }
    NP_DET_PHASE(2);
    { NP_GS(NPGS_SIM_PREPHYS);
    PrePhysicsAITask(g_fSimulationTick); }
    NP_DET_PHASE(3);
    { NP_GS(NPGS_SIM_PHYSICS);
    PhysicsUpdate(g_PhysicsWorld, GetPhysicsUpdateTick()); }
    NP_DET_PHASE(4);
    { NP_GS(NPGS_SIM_POSE);
    PostPhysicsAITask(g_fSimulationTick); }
    NP_DET_PHASE(5);
    NP_GS(NPGS_SIM_NET);

    if (NetMesh::s_bAnimatedNetMeshEnabled)
    {
        bool i = true;
        float goalieX = (float)fabs(g_pTeams[0]->GetGoalie()->m_DetChar.m_v3Position.x);
        if (goalieX > cField::GetGoalLineX(1U))
        {
        }
        else
        {
            goalieX = (float)fabs(g_pTeams[1]->GetGoalie()->m_DetChar.m_v3Position.x);
            if (goalieX > cField::GetGoalLineX(1U))
            {
            }
            else
            {
                i = false;
            }
        }

        NetMesh::spPositiveXNetMesh->Update(g_fSimulationTick, g_pBall->m_v3Position, g_pBall->m_v3PrevPosition, i, g_pBall->m_pPhysicsBall);
        NetMesh::spNegativeXNetMesh->Update(g_fSimulationTick, g_pBall->m_v3Position, g_pBall->m_v3PrevPosition, i, g_pBall->m_pPhysicsBall);
    }

    NP_GS(NPGS_SIM_EVENTS);
    { NP_GS(NPGS_EV_DISPATCH); mEventDispatcher.Dispatch(true); }
    { NP_GS(NPGS_EV_PEACH); UpdatePeachPhoto(&gPeachPhotoState, g_fSimulationTick, lbl_806E2130--); }
    { NP_GS(NPGS_EV_SNAPSHOT); ReplayManager::Instance()->GrabSnapshot(); }
#ifdef NP_NATIVE
    { NP_GS(NPGS_SP_EDGE); NpDetStepEnd(); NpSimtStep(0); }
#endif
}
