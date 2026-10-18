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
    nlTask* currentTask = nlDLRingGetStart<nlTask>(m_pInstance->mTaskList);
    if (currentTask != 0)
    {
        if (m_pInstance->mCurrentState != m_pInstance->mPendingState)
        {
#ifdef NP_NATIVE
            // NP (frame-send worker): a state change tears scenes down and builds new ones: the send of the last frame ends first
            NpFsGate("state");
#endif
        transition_loop:
            currentTask->StateTransition(m_pInstance->mCurrentState, m_pInstance->mPendingState);
            if (!nlDLRingIsEnd<nlTask>(m_pInstance->mTaskList, currentTask))
            {
                currentTask = currentTask->m_next;
                goto transition_loop;
            }
            m_pInstance->mPreviousState = m_pInstance->mCurrentState;
            m_pInstance->mCurrentState = m_pInstance->mPendingState;
        }

        nlTask* taskIterator = nlDLRingGetStart<nlTask>(m_pInstance->mTaskList);
    task_loop:
        u32 currentTicker = nlGetTicker();
        float tickerDifference = nlGetTickerDifference(taskIterator->mPreviousTicker, currentTicker);
        taskIterator->mPreviousTicker = currentTicker;
        if (taskIterator->mActiveStates & m_pInstance->mCurrentState)
        {
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
                NpFsGateSlow("task");
            }
            if (NpTaskBegin) NpTaskBegin(taskIterator->GetName(), taskIterator->mPriority);
#endif
            taskIterator->Run(deltaTime);
#ifdef NP_NATIVE
            if (NpTaskEnd) NpTaskEnd(0, taskIterator->mPriority);
#endif
            taskIterator->mExecutionTime =
                nlGetTickerDifference(taskIterator->mPreviousTicker, nlGetTicker());
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
