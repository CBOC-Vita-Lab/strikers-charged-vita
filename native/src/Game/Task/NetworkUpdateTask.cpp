#include "NL/nlDLListContainer.inl"
#include "Game/NetworkMessageRegistry.h"
#include "NL/plat/SocketNetwork.h"
#include "Game/Task/NetworkUpdateTask.h"

#include "Game/InputRouter.h"

#include "Game/Task/FixedUpdateTask.h"
#include "Game/InputManager.h"

#include "Game/NetTournManager.h"
#include "Game/NetworkDraft.h"
#include "Game/NetworkSession.h"
#include "Game/NetworkMessages.h"
#include "Game/NetworkStatsManager.h"
#include "Game/FriendManager.h"
#include "Game/main.h"
#include "Game/NetworkInput.h"
#include "Game/NetworkSync.h"
#ifdef NP_NATIVE
#include "np/online_clock.h"
#endif

#include "NL/nlMemory.h"
#include "types.h"

u8 lbl_806E1008;
bool gOnlineFourMachineFriendLobby;

void NetworkUpdateTask::Initialize()
{
    SocketNetworkInitializeMemory();
    NetworkSession::Create();
    InitializeNetworkSyncState();
    InitializeNetworkInputRecording();
    InitializeNetworkMessageRegistry();
    RegisterNetworkMessageFactories();
    InitializeInputManager();

    InputFrameProvider* handler = GetFixedUpdateTask();
    gInputManager->SetFrameProvider(handler);

    InitializeInputRouters();
    NetTournManager::CreateInstance();
    NetworkDraft::CreateInstance();

    if (g_pFriendManager == 0)
    {
        void* instance
            = nlMalloc(sizeof(FriendManager), 8, false);
        g_pFriendManager
            = new (instance) FriendManager();
    }

    NetworkStatsManager::CreateInstance();
#ifdef NP_NATIVE
    // NP: the online service's season and daily windows are UTC; the retail shift moved the game's own season/day hours away from them
    // (np/online_clock.h, MSC_ONLINE_RETAIL_TZ=1 keeps it)
    if (GetRegion() == GAME_REGION_JAPAN)
    {
        g_nAddHoursTime = NpOnlineHourShift(9);
    }
    else if (GetRegion() == GAME_REGION_US)
    {
        g_nAddHoursTime = NpOnlineHourShift(-8);
    }
#else
    if (GetRegion() == GAME_REGION_JAPAN)
    {
        g_nAddHoursTime = 9;
    }
    else if (GetRegion() == GAME_REGION_US)
    {
        g_nAddHoursTime = -8;
    }
#endif

    lbl_806E1008 = 0;
    gInputManager->mEnabled = 1;
    gNetworkSyncState->mEnabled = false;
}

void NetworkUpdateTask::Run(float)
{
    g_pNetworkSessionBase->Update();
}
