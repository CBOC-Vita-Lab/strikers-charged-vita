#include "Game/Task/GameTaskState.h"
#include "Game/Task/LoadingTask.h"

#include "NL/globalpad.h"
#include "NL/platpad.h"

#include "Game/AsyncLoading.h"
#include "Game/Pad/FlickDetection.h"
#include "types.h"
#include "Game/Task/FrontEndTask.h"
#include "Game/main.h"
#ifdef NP_NATIVE
#include "np/fx.h"
#include "np/stall.h"   // MSC_STALL_TIMERS (stall10)
#endif

void LoadingTask::Start()
{
    mElapsed = 0.0f;
    fn_80118B50(AsyncLoadingManager::Instance());
}

void LoadingTask::Run(float dt)
{
    mElapsed += dt;

    g_pPadManager->SetActivePadSet(0);
    UpdatePlatPad(g_pPlatPadManager);
    g_pPadManager->Update(dt);
    FlickDetection::Update();

#ifdef NP_NATIVE
    // (MSC_TEX_PRETOUCH) this scene is where the match's textures are shown to the renderer (GameRenderTask does not run in it): see NpTexPretouchStep
    if (nlTaskManager::m_pInstance->mCurrentState & (TASK_FE_TO_GAME | TASK_BOOT_TO_GAME))
    {
        NP_STALL("LoadingTask: pretouch step");
        NpTexPretouchStep(1, nlTaskManager::m_pInstance->mCurrentState);
    }
#endif
#ifdef NP_NATIVE
    u32 stallLoadResult;
    {
        NP_STALL("LoadingTask: async loading step");
        stallLoadResult = fn_80118B7C(AsyncLoadingManager::Instance());
    }
    switch (stallLoadResult)
#else
    switch (fn_80118B7C(AsyncLoadingManager::Instance()))
#endif
    {
    case ASYNC_LOADING_FE_READY:
        nlTaskManager::SetNextState(TASK_CLEAN_BOOT);
        break;
    case ASYNC_LOADING_CLEAN_BOOT_COMPLETE:
        nlTaskManager::SetNextState(TASK_FRONTEND);
        break;
    case ASYNC_LOADING_GAME_READY:
    case ASYNC_LOADING_STADIUM_OR_GAME_READY:
#ifdef NP_NATIVE
        // (MSC_TEX_PRETOUCH) the walk-out starts with the match's textures already shown to the renderer: the loading scene waits for them, 3 s at most
        if (NpTexPretouchHold() != 0) break;
        np::stall::Sum("loading ready, going to play");
#endif
        nlTaskManager::SetNextState(TASK_GAMEPLAY);
        break;
    case ASYNC_LOADING_RETURN_TO_FE:
        nlTaskManager::SetNextState(TASK_FRONTEND);
        break;
    }
}

void LoadingTask::StateTransition(unsigned int from, unsigned int to)
{
    if (to == TASK_BOOT_TO_FE && from == TASK_BOOT)
    {
        fn_80119054(AsyncLoadingManager::Instance());
    }

    if (to == TASK_CLEAN_BOOT)
    {
        fn_801190A0(AsyncLoadingManager::Instance());
    }

    if (to == TASK_FE_TO_GAME && from == TASK_FRONTEND)
    {
        fn_801190EC(AsyncLoadingManager::Instance());
    }

    if (to == TASK_BOOT_TO_GAME && from == TASK_BOOT)
    {
        fn_801191D4(AsyncLoadingManager::Instance());
    }

    if (to == TASK_GAME_TO_FE && from != TASK_HOME_BUTTON_MENU)
    {
        if (g_e3_Build && g_bE3IdleReset)
        {
            fn_80119184(AsyncLoadingManager::Instance());
        }
        else
        {
            fn_80119138(AsyncLoadingManager::Instance());
        }
    }

    if (to == TASK_STADIUM_VIEWER && from == TASK_BOOT)
    {
        fn_80119220(AsyncLoadingManager::Instance());
    }
}

LoadingTask sLoadingTask;
