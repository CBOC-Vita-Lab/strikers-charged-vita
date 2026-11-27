#include <stdlib.h>
#include "Game/NetworkMessageRegistry.h"
#include "Game/InputManager.h"
#include "Game/Sys/debug.h"
#include "Game/InputRouter.h"
#include "Game/NetworkInput.h"

#include "Game/NetworkSession.h"
#include "NL/nlMemory.h"
#include "NL/nlTicker.h"
#include "platform/sdk/net/np_lan.h"  // multiplayer (np/patches/mp)

NetworkInputManager* gInputManager;

static NetworkMessageFactory<NetMessageInput> sFactoryType0;
static NetworkMessageFactory<NetMessageInputBundle> sFactoryType1;
static NetworkMessageFactory<NetMessageAllInputs> sFactoryType8;
static NetworkMessageFactory<NetMessageAllInputsBundle> sFactoryType9;


bool IsNetworkOrRecordedGame()
{
    NetworkSessionControl& online = *g_pNetworkSessionBase;
    int mode = online.GetSessionMode();
    if (mode == NET_MODE_LAN || mode == NET_MODE_ONLINE)
    {
        return true;
    }

    if (gNetworkInputRecording->mRecording != 0
        || gNetworkInputRecording->mPlaybackReady != 0)
    {
        return true;
    }
    return false;
}

#ifdef NP_NATIVE
bool NpLanActive() __attribute__((weak));
// MSC_DISCARD_FAST (stall10; default 0 in the code, 1 in the Vita plan): discarded frames (NL/gl/gl.cpp glSendFrame) are not paced at one retrace each. Never in a LAN, online or
// recorded game (their lockstep clock counts frames and time), and not while the session objects do not exist yet.
extern "C" int NpDiscardFastAllowed(void)
{
    static int on = -1;
    if (on < 0)
    {
        const char* e = getenv("MSC_DISCARD_FAST");
        on = (e != 0 && *e != 0 && atoi(e) != 0) ? 1 : 0;
    }
    if (on == 0) return 0;
    if (g_pNetworkSessionBase == 0 || gNetworkInputRecording == 0) return 0;
    if (NpLanActive && NpLanActive()) return 0;
    return IsNetworkOrRecordedGame() ? 0 : 1;
}
// MSC_DISCARD_REHEARSE=N (stall10; default 0 in the code and 0 (off) in the Vita plan; kit run rehearse6 is the ON side): every Nth discarded frame of the walk-out's wait (Presentation.cpp, "GameBegin") is sent to the renderer
// as a hidden frame (executed, never presented): its display lists, textures, shaders and targets are made under the loading picture instead of on the first intro picture.
// Same exclusions as MSC_DISCARD_FAST. 0 = none.
extern "C" int NpDiscardRehearseEvery(void)
{
    static int n = -1;
    if (n < 0)
    {
        const char* e = getenv("MSC_DISCARD_REHEARSE");
        n = (e != 0 && *e != 0) ? atoi(e) : 0;
        if (n < 0) n = 0;
    }
    if (n == 0) return 0;
    if (g_pNetworkSessionBase == 0 || gNetworkInputRecording == 0) return 0;
    if (NpLanActive && NpLanActive()) return 0;
    return IsNetworkOrRecordedGame() ? 0 : n;
}
// MSC_LOADHOLD=N (stall10; default 0 in the code and 0 (off) in the Vita plan; kit run loadhold1 is the ON side): the first N frames of the walk-out (NIS) are executed hidden too, after the rehearsal frames and before the first
// picture, so the loading picture stays up until the walk-out's own first frame has been made. Only with MSC_DISCARD_REHEARSE on (the same exclusions); capped at 3 frames.
extern "C" int NpLoadHoldFrames(void)
{
    static int n = -1;
    if (n < 0)
    {
        const char* e = getenv("MSC_LOADHOLD");
        n = (e != 0 && *e != 0) ? atoi(e) : 0;
        if (n < 0) n = 0;
        if (n > 3) n = 3;
    }
    if (n == 0 || NpDiscardRehearseEvery() == 0) return 0;
    return n;
}
#endif

void InitializeInputManager()
{
    NetworkInputManager* manager
        = (NetworkInputManager*)nlMalloc(
            sizeof(NetworkInputManager), 8, false);
    if (manager != 0)
    {
        manager->mFrameProvider = 0;
        manager->mEnabled = false;

        gNetworkMessageRegistry->RegisterFactory(NETMSG_INPUT, &sFactoryType0);
        gNetworkMessageRegistry->RegisterFactory(NETMSG_INPUT_BUNDLE, &sFactoryType1);
        gNetworkMessageRegistry->RegisterFactory(NETMSG_ALL_INPUTS, &sFactoryType8);
        gNetworkMessageRegistry->RegisterFactory(NETMSG_ALL_INPUTS_BUNDLE, &sFactoryType9);

        manager->mCongestionStartTicker = 0;
        manager->mLastCongestionMilliseconds = 0.0f;
        manager->mCongestionCount = 0;
        manager->mTotalCongestionMilliseconds = 0.0f;
        manager->mQueueOverflowReported = false;
    }
    gInputManager = manager;
}

void NetworkInputManager::Reset()
{
    mCongestionStartTicker = 0;
    mLastCongestionMilliseconds = 0.0f;
    mCongestionCount = 0;
    mTotalCongestionMilliseconds = 0.0f;
    mQueueOverflowReported = false;
}

void NetworkInputManager::SetFrameProvider(
    InputFrameProvider* frameProvider)
{
    mFrameProvider = frameProvider;
}

int NetworkInputManager::GetUpdateCount()
{
    InputRouter* router = GetInputRouter();
    int inputCount = router->GetUpdateCount();
    return inputCount + gNetworkInputRecording->GetNetworkInputPlaybackExtraUpdates();
}

void NetworkInputManager::CaptureInputs()
{
    if (gNetworkInputRecording->mPlaybackReady != 0)
    {
        return;
    }

    InputRouter* router = GetInputRouter();
    if (!router->mQueueOverflowed)
    {
        if (!router->CanCaptureInput())
        {
            return;
        }

        NetworkPeer* peer
            = g_pNetworkSessionBase->GetLocalPeer();
        int numControllers = peer->mPlayerCount;
        for (int i = 0; i < numControllers; ++i)
        {
            (peer->GetNetworkPeerChannel(i))->CaptureNetworkPeerChannelInput();
        }
        router->OnInputCaptured();
    }
    else if (!mQueueOverflowReported)
    {
        tDebugPrintManager::Print(DC_NETWORK, "Overflowed Queues just occured\n");
        mFrameProvider->OnInputQueueOverflow();
        mQueueOverflowReported = true;
    }
}

bool NetworkInputManager::PrepareUpdate()
{
    InputRouter* router = GetInputRouter();
    if (gNetworkInputRecording->mPlaybackReady != 0)
    {
        if (router->ProcessPlaybackFrame() != 0)
        {
            return true;
        }
    }
    else if (!router->mQueueOverflowed)
    {
        router->CheckCongestion();
        if (!router->HasInput() && NpLanRetryWait(router))   // delay19 (MSC_LAN_RETRY_US, default off): a bounded receive-only wait inside the frame
        {
            router->CheckCongestion();
        }
        if (router->HasInput())
        {
            NpLanStep(false);  // multiplayer (np/patches/mp 0006): input delay and queue depths of this step, for the log
            router->mStarvedForInput = false;
            if (mCongestionStartTicker != 0)
            {
                mLastCongestionMilliseconds
                    = nlGetTickerDifference(mCongestionStartTicker, nlGetTicker());
                mCongestionStartTicker = 0;
                ++mCongestionCount;
                tDebugPrintManager::Print(DC_NETWORK,
                    "Congested for %dth time for %f ms at game frame %d\n",
                    mCongestionCount, mLastCongestionMilliseconds,
                    mFrameProvider->GetFrame());
                mTotalCongestionMilliseconds += mLastCongestionMilliseconds;
            }
            router->OnInputReady();
            return true;
        }

        router->mStarvedForInput = true;
        NpLanStep(true);  // multiplayer (np/patches/mp 0006): a starved step (20 ms of match clock without a simulation step)
        mCongestionStartTicker = nlGetTicker();
    }
    else if (!mQueueOverflowReported)
    {
        tDebugPrintManager::Print(DC_NETWORK, "Overflowed Queues just occured\n");
        mFrameProvider->OnInputQueueOverflow();
        mQueueOverflowReported = true;
    }
    return false;
}
