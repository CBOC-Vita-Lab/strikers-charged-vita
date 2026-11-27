// NetTournManager::HandleTournamentGameUpdate against a game index the wire made up (online audit, 1.3). The handler indexed mGames[7] with
// the message's u8 index. The test runs the real handler (src/Game/NetTournManager.cpp) on a manager in a large zeroed buffer and checks that
// no byte outside mGames[0..6] changes for indices 7..255, and that a valid index still updates its game (positive control).
// Built by run.sh: 32-bit host, nothing else of the game linked (symbols the handler does not reach stay unresolved).
#define private public
#define protected public
#include "Game/NetTournManager.h"
#include "Game/NetworkMessages.h"
#undef private
#undef protected
#include "Game/TweakRegistration.h"
#include "Game/TweakValueBase.h"
#include <stdio.h>
#include <string.h>

// the file's static tweak bindings run at start-up: the registry is "not ready", so they only queue themselves (nothing kept)
TweakValueBase::TweakValueBase() {}
TweakValueBase::~TweakValueBase() {}
int IsTweakRegistryInitialized(void) { return 0; }
void QueueTweakValue(TweakPendingValue*, TweakValueBase*, const char*) {}
void* nlMalloc(unsigned long, unsigned int, bool) { return 0; }

static unsigned char g_buf[sizeof(NetTournManager) + 300 * sizeof(NetworkTournamentGame)] __attribute__((aligned(16)));
static unsigned char g_before[sizeof(g_buf)];

int main()
{
    int failures = 0, checks = 0;
    NetTournManager* m = reinterpret_cast<NetTournManager*>(g_buf);
    const size_t gamesLo = reinterpret_cast<unsigned char*>(&m->mGames[0]) - g_buf;
    const size_t gamesHi = reinterpret_cast<unsigned char*>(&m->mGames[7]) - g_buf;
    const unsigned char types[] = {NET_TOURN_UPDATE_DID_NOT_FINISH, NET_TOURN_UPDATE_COULD_NOT_START, NET_TOURN_UPDATE_PROGRESS, NET_TOURN_UPDATE_RESULT};
    for (int index = 0; index < 256; ++index)
    {
        for (unsigned t = 0; t < sizeof types; ++t)
        {
            memset(g_buf, 0, sizeof g_buf);
            m->mState = NET_TOURN_RUNNING;
            for (int k = 0; k < 300; ++k)   // every slot the index could reach looks like a game ready to take the update
                reinterpret_cast<NetworkTournamentGame*>(g_buf + gamesLo)[k].mState = NET_TOURN_GAME_READY;
            // (raw storage: the message's constructors live in other files; the handler reads plain fields only)
            static unsigned char raw[sizeof(NetMessageTournamentGameUpdate)] __attribute__((aligned(16)));
            memset(raw, 0, sizeof raw);
            NetMessageTournamentGameUpdate& msg = *reinterpret_cast<NetMessageTournamentGameUpdate*>(raw);
            msg.mUpdateType = types[t];
            msg.mGameIndex = (u8)index;
            msg.mIsHomeMachine = true;
            msg.mGameStatus = 1;
            msg.mGameTimeDelta = 7;
            msg.mHasGameInfo = true;
            memset(&msg.mGameInfo, 0x5A, sizeof msg.mGameInfo);
            memcpy(g_before, g_buf, sizeof g_buf);
            m->HandleTournamentGameUpdate(&msg);
            size_t outside = 0;
            for (size_t i = 0; i < sizeof g_buf; ++i)
                if ((i < gamesLo || i >= gamesHi) && g_buf[i] != g_before[i]) ++outside;
            ++checks;
            if (outside)
            {
                if (failures < 5) printf("FAIL: game index %d, update %u: %zu bytes changed outside mGames\n", index, (unsigned)types[t], outside);
                ++failures;
            }
            if (index < 7)
            {
                ++checks;
                if (memcmp(g_buf + gamesLo, g_before + gamesLo, gamesHi - gamesLo) == 0)
                {
                    printf("FAIL: valid game index %d, update %u: nothing changed\n", index, (unsigned)types[t]);
                    ++failures;
                }
            }
        }
    }
    printf("tourn_test: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
