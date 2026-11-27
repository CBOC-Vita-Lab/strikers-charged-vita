// The MULTIPLAYER entry of the game's own menus: ONLINE / LOCAL, then HOST / JOIN or NEARBY / SAME WI-FI. The saved MULTIPLAYER LINK setting only
// preselects a choice. np_menu_flow.cpp is the state machine; np_menu_ui.cpp is its screen (the game's popups); np_lan.cpp calls both from the
// main menu's hooks. No game types and no STL here: np_lan.cpp includes the game's headers, which define their own placement new.
//
//   choose HOST / JOIN -> (first time) a name, typed once -> an account on the service -> (join) the invite code, typed with the keyboard ->
//   bring-up of the online transport with a status line, cancellable with Circle -> the game's own lobby (Create Game / Join Game) over the link.
//
// Everything is stepped once per game loop by NpMenuStep(); nothing blocks (HTTPS and DNS run on worker threads, the keyboard is a system dialog), every
// wait has a limit on the game clock and a sentence when it runs out (A11).
#ifndef NP_MENU_FLOW_H
#define NP_MENU_FLOW_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- the screen: the game's popups (np_menu_ui.cpp) ------------------------------------------------------------------------------------------
// A popup with a message and one to three buttons. 1 = shown, 0 = not possible now (another popup is open). The text of an open popup can be replaced
// (same size of box). Poll: -1 open, 0..2 the button pressed, -2 gone without a press.
int NpMenuUiShow(const char* text, const char* b0, const char* b1, const char* b2, int highlight);
void NpMenuUiPrepare(const char* text, const char* b0, const char* b1, const char* b2, int highlight);   // fill the popup texts only (no push; a scene that pushes the popup itself)
void NpMenuUiSetText(const char* text);
int NpMenuUiPoll(void);
int NpMenuUiOpen(void);          // a popup (ours or another) is on screen
int NpMenuUiReleaseStaleLock(int staleFrames);   // a lock whose owner has left the scene stack for `staleFrames` calls in a row is released (returns 1 then); 0 otherwise. For a screen that must not wait the 300 frames of NpMenuUiShow
int NpMenuUiReady(void);         // the popup we opened has finished loading (its scene is ready: it can be seen and takes input)
void NpMenuUiClose(void);
int NpMenuUiBack(void);          // Circle was pressed this frame
int NpMenuUiSceneReady(int sceneId);   // the scene on top of the stack is `sceneId` (no popup over it)

// ---- the flow (np_menu_flow.cpp) ---------------------------------------------------------------------------------------------------------------
enum { kNpMenuBusy = 0, kNpMenuEnter = 1, kNpMenuDone = 2, kNpMenuLocal = 3, kNpMenuRanked = 4, kNpMenuQuick = 5, kNpMenuFriends = 6 };
// kNpMenuRanked: open Online Central for a ranked match (1.3). kNpMenuQuick: PLAY WITH ANYONE, Online Central too but an unranked quick match (NpMenuQuickWanted).
// kNpMenuFriends: FRIEND LIST, Online Central's hub with the game's own friend screens.
enum { kNpMenuNearby = 0, kNpMenuWifi = 1, kNpMenuOnline = 2 };
int NpMenuWanted(void);                 // 1: MULTIPLAYER starts this flow, except the desktop test shortcut MSC_NET_MODE
int NpMenuLinkMode(void);               // this run's committed choice; before one, the saved MSC_LAN_MODE preference
int NpMenuFriendsOffered(void);         // 1.3: the ONLINE choice is FRIENDS / PLAY WITH ANYONE / RANKED MATCH, and FRIENDS opens FRIEND LIST / HOST A ROOM / JOIN WITH A CODE
int NpMenuQuickWanted(void);            // this run's Online Central entry was PLAY WITH ANYONE (an unranked quick match; cleared when the main menu is entered again)
int NpMenuRankedOffered(void);          // 1.3: RANKED MATCH is the third button of the online choice (MSC_ONLINE_RANKED, default on)
void NpMenuBegin(void);                 // the player picked MULTIPLAYER
int NpMenuStep(void);                   // Busy, Enter (online link up), Local (local choice ready; start radio), Done (cancelled/failed)
void NpMenuShowFailure(const char* text);   // idle only: one OK popup with this sentence; NpMenuStep drives it and ends with Done
int NpMenuActive(void);                 // the flow is between Begin and Enter / Done
const char* NpMenuPlayerName(void);     // the name typed for online play ("" before the account exists): the lobby shows it instead of the Vita user name
// The connection-lost popup of an online session: why the link ended and that a match cannot be resumed. 0 = not an online session.
int NpMenuConnectionLostText(char* out, size_t size);

// ---- the lobby after a lost link (called from the lobby scene's update, SHNetworkStart.cpp) -----------------------------------------------------
// Offers RECONNECT once, in the same room, within the resume window; LEAVE (or the window running out) goes back to the main menu.
// Returns 1 when the lobby must be left (the caller pushes the lobby's back transition), 2 when the link is back and the lobby must be re-entered, else 0.
int NpMenuLobbyStep(void);
// The link of an online session was lost and the game ended its session (np_lan.cpp noted it): the lobby should offer RECONNECT.
void NpMenuLinkLostNote(void);

#ifdef __cplusplus
}
#endif
#endif
