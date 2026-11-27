#include "NL/nlTask.h"

#include "NL/nlDLRing.h"
#include "NL/nlMemory.h"
#include "NL/nlTicker.h"

#include "Game/Sys/movie.h"
#include "Game/SharedStaticStorage.h"

#ifdef NP_NATIVE
// NP: per-task instruction marks for the qemu profiler (platform/harness, MSC_TASK_MARKS=1); weak, so a link
// without the harness leaves them null and the cost is one compare per task.
extern "C" void NpTaskBegin(const char* name, unsigned index) __attribute__((weak));
extern "C" void NpTaskEnd(const char* name, unsigned index) __attribute__((weak));
#include "np/framesend.h"
#include "np/fps.h"
#include "np/gsplit.h"
#include "np/stall.h"   // MSC_STALL_TIMERS (stall10)
#include <stdlib.h>
// NP (src/Game/NpSimThread.cpp): MSC_SIMT_PROBE per-task checks, MSC_SIMT_PERTURB
extern "C" int gNpSimtProbe;
extern "C" int NpSimtEnabled();
extern "C" int NpSimtPerturbTask(const char* name);
extern "C" void NpSimtTaskBegin(const char* name, unsigned prio);
extern "C" void NpSimtTaskEnd();
static unsigned sSimtPicture;
static int sTaskExecSkip = -1;                 // gxp17 MSC_TASK_EXEC_SKIP: -1 not read yet
static unsigned long sTaskExecSkipped = 0;     // (action counter: clock reads not made)
extern "C" unsigned long NpTaskExecSkipped(void) { return sTaskExecSkipped; }
#include "Game/Task/GameTaskState.h"
#endif

float g_fTaskTimeUpperBound = 0.1f;
nlTaskManager* nlTaskManager::m_pInstance;
float g_fTaskTimeLowerBound;

void nlTaskManager::Startup(u32 initialState)
{
    m_pInstance = new (nlMalloc(sizeof(nlTaskManager), 8, false)) nlTaskManager;
    m_pInstance->mPreviousState = initialState;
    m_pInstance->mCurrentState = initialState;
    m_pInstance->mPendingState = initialState;
    m_pInstance->mTaskList = 0;
    m_pInstance->mTimeDilation = 1.0f;
    m_pInstance->mLocked = false;
}

void nlTaskManager::AddTask(nlTask* task, u32 priority, u32 activeStates)
{
    task->mPriority = priority;
    task->mActiveStates = activeStates;
    task->mPreviousTicker = nlGetTicker();

    if (m_pInstance->mTaskList == 0)
    {
        nlDLRingAddStart<nlTask>(&m_pInstance->mTaskList, task);
        return;
    }

    nlTask* currentTask = nlDLRingGetStart<nlTask>(m_pInstance->mTaskList);
    while (currentTask != 0)
    {
        if (currentTask->mPriority >= priority)
        {
            currentTask = currentTask->m_prev;
            break;
        }
        else if (!nlDLRingIsEnd<nlTask>(m_pInstance->mTaskList, currentTask))
        {
            currentTask = currentTask->m_next;
        }
        else
        {
            break;
        }
    }

    nlDLRingInsert<nlTask>(&m_pInstance->mTaskList, currentTask, task);
}

void nlTaskManager::RunAllTasks()
{
#ifdef NP_NATIVE
    NpGsPassBegin();   // MSC_GSPLIT (np/gsplit.h)
    struct NpGsPassGuard { ~NpGsPassGuard() { if (g_npGsOn) NpGsPassEnd(); } } npGsPassGuard;
    if (sTaskExecSkip < 0) { const char* e = getenv("MSC_TASK_EXEC_SKIP"); sTaskExecSkip = (e && *e && e[0] != '0') ? (e[0] == '2' ? 2 : 1) : 0; }   // (gxp17; 2 = the proof's mutant, below)
#endif
    nlTask* currentTask = nlDLRingGetStart<nlTask>(m_pInstance->mTaskList);
#ifdef NP_NATIVE
    {
        static unsigned sStallFrame = 0;
        np::stall::Frame() = ++sStallFrame;   // (MSC_STALL_TIMERS: the task picture number and state for the [stall] lines)
        np::stall::State() = m_pInstance->mCurrentState;
    }
#endif
    if (currentTask != 0)
    {
        if (m_pInstance->mCurrentState != m_pInstance->mPendingState)
        {
#ifdef NP_NATIVE
            // NP (frame-send worker): a state change tears scenes down and builds new ones: the send of the last frame ends first
            {
                NP_STALL_ARG("fs-gate state", m_pInstance->mPendingState);
                NpFsGate("state");
            }
#endif
        transition_loop:
            {
#ifdef NP_NATIVE
                const char* const stallTaskName = currentTask->GetName();
                NP_STALL_CM("StateTransition", m_pInstance->mPendingState, &stallTaskName);
#endif
            currentTask->StateTransition(m_pInstance->mCurrentState, m_pInstance->mPendingState);
            }
            if (!nlDLRingIsEnd<nlTask>(m_pInstance->mTaskList, currentTask))
            {
                currentTask = currentTask->m_next;
                goto transition_loop;
            }
            m_pInstance->mPreviousState = m_pInstance->mCurrentState;
            m_pInstance->mCurrentState = m_pInstance->mPendingState;
#ifdef NP_NATIVE
            np::stall::State() = m_pInstance->mCurrentState;
            np::stall::Sum("after the state change");
#endif
        }

        nlTask* taskIterator = nlDLRingGetStart<nlTask>(m_pInstance->mTaskList);
#ifdef NP_NATIVE
        // int28: nlGetTicker() (OSGetTick -> sceKernelGetSystemTimeWide, #1 Core 0 profile symbol) is read once before the loop
        // and refreshed after an active task executes, eliminating ~10-15 redundant kernel timer syscalls/frame for inactive tasks.
        u32 cachedTicker = sTaskExecSkip == 2 ? 0u : nlGetTicker();
        bool needTickerRefresh = false;
#endif
    task_loop:
#ifdef NP_NATIVE
        const int npGsTick = g_npGsOn ? NpGsEnter(NPGS_RUN_TICKER) : -1;   // gt27 (MSC_GSPLIT): the runner's parts
        if (needTickerRefresh) { cachedTicker = nlGetTicker(); needTickerRefresh = false; }
        // (MSC_TASK_EXEC_SKIP=2, host proof mutant, never set in play: the read that gives a task its time step is skipped too, so a step that reads it must change)
        u32 currentTicker = sTaskExecSkip == 2 ? taskIterator->mPreviousTicker + 1 : cachedTicker;
#else
        u32 currentTicker = nlGetTicker();
#endif
        float tickerDifference = nlGetTickerDifference(taskIterator->mPreviousTicker, currentTicker);
        taskIterator->mPreviousTicker = currentTicker;
#ifdef NP_NATIVE
        if (npGsTick >= 0) NpGsLeave(npGsTick);
#endif
        if (taskIterator->mActiveStates & m_pInstance->mCurrentState)
        {
#ifdef NP_NATIVE
            if (sTaskExecSkip != 2) needTickerRefresh = true;
#endif
            float deltaTime = tickerDifference / 1000.0f;
            m_pInstance->mRealTimeDelta = deltaTime;
            if (deltaTime < g_fTaskTimeLowerBound)
            {
                deltaTime = g_fTaskTimeLowerBound;
            }
            else if (deltaTime > g_fTaskTimeUpperBound)
            {
                deltaTime = g_fTaskTimeUpperBound;
            }
#ifdef NP_NATIVE
            if (m_pInstance->mCurrentState == TASK_GAMEPLAY) deltaTime = NpSlipDelta(deltaTime);   // MSC_SLIP (np/fps.h): bounded slip, default off
#endif
            if (taskIterator->mTimeDilated)
            {
                deltaTime *= m_pInstance->mTimeDilation;
            }
            m_pInstance->mCurrentTimeDelta = deltaTime;
#ifdef NP_NATIVE
            // NP (frame-send worker): while a send is pending only the tasks of the allowed priorities run beside it (the tail of the
            // task list, the head up to the simulation); WorldUpdate .. EndFrame use what the send reads and join first.
            if (&NpFsBusy != 0 && NpFsBusy != 0 && (taskIterator->mPriority >= 32 || !((NpFsTaskMask() >> taskIterator->mPriority) & 1)))
            {
                NP_STALL_ARG("fs-gate task", taskIterator->mPriority);
                NpFsGateSlow("task");
            }
            const int npGsHook = g_npGsOn ? NpGsEnter(NPGS_RUN_HOOKS) : -1;
            if (NpTaskBegin) NpTaskBegin(taskIterator->GetName(), taskIterator->mPriority);
            if (npGsHook >= 0) NpGsLeave(npGsHook);
            const int npGsPrev = g_npGsOn ? NpGsEnter(NpGsTaskId(taskIterator->GetName())) : -1;
            if (gNpSimtProbe != 0 && NpSimtEnabled() > 0) NpSimtTaskBegin(taskIterator->GetName(), taskIterator->mPriority);
            if (taskIterator->mPriority == 4) ++sSimtPicture;
            const int simtPerturb = gNpSimtProbe != 0 && m_pInstance->mCurrentState == 2 ? NpSimtPerturbTask(taskIterator->GetName()) : 0;
            {
                const char* const stallTaskName = taskIterator->GetName();
                NP_STALL_CM("Run", taskIterator->mPriority, &stallTaskName);
            if (simtPerturb != 1 || (sSimtPicture & 1) == 0)
            taskIterator->Run(deltaTime);
            if (simtPerturb == 2) taskIterator->Run(deltaTime);
            }
            if (gNpSimtProbe > 0) NpSimtTaskEnd();
#endif
#ifndef NP_NATIVE
            taskIterator->Run(deltaTime);
#endif
#ifdef NP_NATIVE
            if (npGsPrev >= 0) NpGsLeave(npGsPrev);
            const int npGsHook2 = g_npGsOn ? NpGsEnter(NPGS_RUN_HOOKS) : -1;
            if (NpTaskEnd) NpTaskEnd(0, taskIterator->mPriority);
            if (npGsHook2 >= 0) NpGsLeave(npGsHook2);
#endif
#ifdef NP_NATIVE
            // gxp17 MSC_TASK_EXEC_SKIP=0|1 (code default 0, the Vita plan sets 1): mExecutionTime is written here and read nowhere (the Wii build's task profiler read it); the clock read for it
            // (about 18 a picture in play) is not made. Read once, a plain int (no guarded static: its barrier). The task's next deltaTime comes from the read at the top of the loop, as before.
            if (sTaskExecSkip == 0)
#endif
            taskIterator->mExecutionTime =
                nlGetTickerDifference(taskIterator->mPreviousTicker, nlGetTicker());
#ifdef NP_NATIVE
            else ++sTaskExecSkipped;
#endif
#ifdef NP_NATIVE
            NP_STALL("MoviePlay");
            NP_GS(NPGS_RUN_MOVIE);
#endif
            MoviePlay();
        }
        if (taskIterator != m_pInstance->mTaskList)
        {
            taskIterator = taskIterator->m_next;
            goto task_loop;
        }
    }
}

void nlTaskManager::SetNextState(unsigned int nextState)
{
    m_pInstance->mPendingState = nextState;
}

void nlTaskManager::SetTimeDilation(float timeDilation)
{
    m_pInstance->mTimeDilation = timeDilation;
}
