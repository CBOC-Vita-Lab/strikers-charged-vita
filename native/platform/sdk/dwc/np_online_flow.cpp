// The online entry of the main menu. See np_online_flow.h.
#include "platform/sdk/dwc/np_online_flow.h"
#include "platform/sdk/net/np_net_census.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Game/DB/SaveLoad.h"
#include "Game/GameInfo.h"
#include "Game/NetworkDebug.h"
#include "Game/NetworkSession.h"
#include "Game/SH/SHMainMenu.h"
#include "platform/sdk/dwc/np_online_name.h"
#include "platform/sdk/dwc/np_quick_policy.h"

extern "C" long long OSGetTime(void);   // the console clock (declared here: the SDK header clashes with the game's type headers)

#define FLOW_LOG(...) fprintf(stderr, "[online] " __VA_ARGS__)

namespace {

const unsigned kOnlineItem = 1;                         // MAIN_MENU_ONLINE
const unsigned long long kVirtualProfileId = 0x4D53434F4E4C4E31ull;   // "MSCONLN1": stands in for the Mii's create id in the save slot

enum class State { Idle, Naming, WaitSave, Saving };
State g_state = State::Idle;
SHMainMenu* g_menu = 0;
bool g_flow = false;

int SessionMode() { return g_pNetworkSession ? g_pNetworkSession->GetSessionMode() : 0; }

// Chooses the save slot of this player: the slot that already holds the virtual id, else the first empty one, else (all ten taken by Miis
// that cannot exist here) the slot used longest ago. Writes the public name into the game's globals and the slot. Returns false on failure;
// *needsSave tells whether the slot changed.
bool PrepareProfile(bool* needsSave) {
    GameInfoManager* gi = GameInfoManager::Instance();
    if (!gi) return false;
    *needsSave = false;
    int slot = -1, empty = -1, oldest = 0;
    unsigned long long oldestTime = ~0ull;
    for (int i = 0; i < 10; ++i) {
        unsigned long long id;
        memcpy(&id, gi->GetUnknown0xA80(i), sizeof id);
        if (id == kVirtualProfileId && slot < 0) slot = i;
        if (id == 0 && empty < 0) empty = i;
        unsigned long long t;
        memcpy(&t, gi->GetUnknown0xA88(i), sizeof t);
        if (t < oldestTime) { oldestTime = t; oldest = i; }
    }
    if (slot < 0) {
        slot = empty >= 0 ? empty : oldest;
        if (empty < 0) FLOW_LOG("all ten save slots are taken: the one used longest ago (%d) becomes the online profile\n", slot);
        gi->ClearSaveSlot(slot);
        const unsigned long long id = kVirtualProfileId;
        memcpy(gi->GetUnknown0xA80(slot), &id, sizeof id);
        const long long now = OSGetTime();
        memcpy(gi->GetUnknown0xA88(slot), &now, sizeof now);
        *needsSave = true;
    }
    gNetworkSaveSlotIndex = slot;

    const char* name = NpOnline::Name();
    unsigned short wide[11] = {0};
    for (size_t i = 0; i < 10 && name[i]; ++i) wide[i] = (unsigned char)name[i];
    memset(gNetworkMiiName, 0, sizeof gNetworkMiiName);
    for (size_t i = 0; i < 10 && name[i]; ++i) gNetworkMiiName[i] = name[i];
    memcpy(gNetworkMiiNameWide, wide, sizeof gNetworkMiiNameWide);
    unsigned short* saved = static_cast<unsigned short*>(gi->GetUnknown0xAA8(slot));
    if (memcmp(saved, wide, sizeof wide) != 0) {
        memcpy(saved, wide, sizeof wide);
        gNetworkMiiChanged = 1;   // the game then writes the player's row again with the new name (NetworkSession login stages)
        *needsSave = true;
    }
    FLOW_LOG("online profile: slot %d, name \"%s\"\n", slot, name);
    return true;
}

void Go() {
    g_state = State::Idle;
    g_flow = true;
    if (g_menu) g_menu->ApplyItem(kOnlineItem);   // the game's own transition into the online screens
}

}  // namespace

bool g_centralUsed = false;   // Online Central was opened from the online choice (RANKED MATCH) in this run

bool NpOnlineHideMiiIcon() {
    const char* show = getenv("MSC_ONLINE_MII_ICON");
    return (NpOnlineEnabled() || g_centralUsed) && !(show && show[0] == '1');
}

// 1.3: Online Central for every player, behind MULTIPLAYER -> ONLINE -> RANKED MATCH (np_menu_flow.cpp). MSC_ONLINE=1 stays what it was: a
// developer shortcut that puts Online Central on the main menu's old Wi-Fi entry instead of the multiplayer chooser.
extern "C" int NpOnlineCentralAvailable(void) { return 1; }

bool NpOnlineCentralBegin(SHMainMenu* menu) {
    if (g_state != State::Idle || SessionMode() != 0 || !menu) return false;
    FLOW_LOG("online central selected (ranked)\n");
    g_centralUsed = true;
    g_state = State::Naming;
    g_menu = menu;
    return true;
}

bool NpOnlineEnabled() {
    static const bool on = [] { const char* e = getenv("MSC_ONLINE"); return e && e[0] == '1'; }();
    return on;
}

bool NpOnlineFlowActive() { return g_flow; }

bool NpOnlineMenuSelect(SHMainMenu* menu, unsigned int item) {
    if (!NpOnlineEnabled() || item != kOnlineItem || g_state != State::Idle || SessionMode() != 0) return false;
    FLOW_LOG("online selected\n");
    g_state = State::Naming;
    g_menu = menu;
    return true;
}

void NpOnlineMenuUpdate(SHMainMenu* menu) {
    if (g_state == State::Idle || menu != g_menu) return;
    switch (g_state) {
    case State::Naming: {
        const NpOnline::NameState n = NpOnline::NameRequest();
        if (n == NpOnline::NameState::Pending) return;
        if (n == NpOnline::NameState::Cancelled) { FLOW_LOG("no name chosen; staying in the main menu\n"); g_state = State::Idle; return; }
        g_state = State::WaitSave;
        // fall through: the save may be ready at once
    }
    case State::WaitSave: {
        if (SaveEnabled && (!NormalSaveLoaded || SaveLoad::CardBusy())) return;
        bool needsSave = false;
        if (!PrepareProfile(&needsSave)) { g_state = State::Idle; return; }
        if (needsSave && SaveEnabled) { SaveLoad::StartSave(true); g_state = State::Saving; return; }
        Go();
        return;
    }
    case State::Saving:
        if (SaveLoad::CardBusy()) return;
        Go();
        return;
    default: return;
    }
}

extern "C" int NpMenuQuickWanted(void);   // np_menu_flow.h: the entry was PLAY WITH ANYONE
void NpDwcSetQuickMatch(bool quick);      // np_dwc.h: the next matchmaking is the unranked quick queue

void NpOnlineScenePush(SceneList* scene) {
    if (!g_flow) return;
    // PLAY WITH ANYONE: the first time the hub would open after the login, go straight to the match search instead (the hub's own RANKED MATCH button leads to the
    // same scene); Circle there goes back to the hub as usual.
    // (mp24, from onmatch23) The search that follows is the UNRANKED quick queue: nothing set that before, so PLAY WITH ANYONE joined the ranked queue with a ranked
    // ticket (the log said "looking for an opponent" without "(quick match: unranked)"), met ranked players and was rated. np_quick_policy.h decides; the hub's
    // RANKED MATCH, a new sign-in and the main menu end the quick choice.
    static QuickState quick;
    const QuickScene qs = *scene == SCENE_ONLINE_LOGIN || *scene == SCENE_ONLINE_MII_SELECT || *scene == SCENE_ONLINE_MII_SELECT_OVERLAY ? kQsLogin
                          : *scene == SCENE_ONLINE_MENU ? kQsHub : *scene == SCENE_MAIN_MENU ? kQsMainMenu : kQsOther;
    const bool jump = QuickPolicyScene(quick, qs, NpMenuQuickWanted() != 0);
    NpDwcSetQuickMatch(quick.quick);
    if (jump) {
        FLOW_LOG("play with anyone: straight to the match search\n");
        *scene = SCENE_ONLINE_RANKED;
        return;
    }
    if (*scene == SCENE_ONLINE_MII_SELECT || *scene == SCENE_ONLINE_MII_SELECT_OVERLAY) {
        FLOW_LOG("scene %d -> login (no Miis here)\n", (int)*scene);
        *scene = SCENE_ONLINE_LOGIN;
    } else if (*scene == SCENE_MAIN_MENU) {
        if (g_flow) NpNetCensus::Log("the online screens were left");   // MSC_NET_CENSUS=1: "[net] sockets open N ..." (np_net_census.h)
        g_flow = false;
    }
}
