// np_online_flow.h: the game's own online screens (main menu entry -> login -> hub -> rankings) without the Wii's Miis.
// Reached by every player through MULTIPLAYER -> ONLINE -> RANKED MATCH (1.3, NpOnlineCentralBegin; MSC_ONLINE_RANKED=0 hides it). MSC_ONLINE=1
// (developer shortcut) also makes item 1 of the main menu (the old Nintendo Wi-Fi Connection entry) start it instead of the LAN lobby of np_lan.cpp. The hooks are called from np_lan.cpp's hooks in the main menu and the scene manager.
//
//   Select item 1 -> the public name (typed once with the system keyboard on the Vita; MSC_ONLINE_NAME on a host) ->
//   a "virtual profile" save slot (the retail game keys a slot to a Mii; here a fixed id stands in for the player) ->
//   SHMainMenu::ApplyItem(ONLINE) -> the Mii screens are skipped -> SCENE_ONLINE_LOGIN (DWC_LoginAsync on the game's own login thread) ->
//   the online hub with the ranking views.
#ifndef NP_ONLINE_FLOW_H
#define NP_ONLINE_FLOW_H

#include "Game/BaseGameSceneManager.h"

class SHMainMenu;

bool NpOnlineHideMiiIcon();                                     // the hub's Mii icon is hidden while the online flow is on (no Miis here); MSC_ONLINE_MII_ICON=1 shows it
bool NpOnlineEnabled();                                         // MSC_ONLINE=1
bool NpOnlineFlowActive();                                      // between selecting the entry and leaving the online screens
bool NpOnlineMenuSelect(SHMainMenu* menu, unsigned int item);   // true when it took the selection (item 1)
bool NpOnlineCentralBegin(SHMainMenu* menu);                    // 1.3: MULTIPLAYER -> ONLINE -> RANKED MATCH opens Online Central (any build)
void NpOnlineMenuUpdate(SHMainMenu* menu);                      // once per frame from SHMainMenu::Update
void NpOnlineScenePush(SceneList* scene);                       // may redirect the Mii screens to the login; leaves the flow at the main menu

#endif
