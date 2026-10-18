// LAN multiplayer glue of the native port: what the recompiled port's msc_lan.cpp does besides the sockets, on the
// game's own C++ API. The retail game contains a finished LAN mode (session mode 1) that no menu reaches; these hooks
// start it from the main menu's old Nintendo Wi-Fi Connection entry (relabelled MULTIPLAYER), keep the game away from
// the Nintendo login and Mii screens, and restart/leave it around a match. The hooks are called from small edits of
// the decompiled sources (np/patches/mp/); outside a LAN session every one of them does nothing.
// See np/docs/multiplayer.md.
#ifndef NP_LAN_H
#define NP_LAN_H

#include "Game/BaseGameSceneManager.h"

class SHMainMenu;
class NetMessageDraft;
class NetworkPeerChannel;
class nlLocalization;

// A LAN session (any phase) exists: NetworkSession::mSessionMode != 0.
bool NpLanActive();

// ---- main menu --------------------------------------------------------------------------------------------------
// SHMainMenu::SelectItem, before its own handling of `item`. True when it took the selection (item 1 = the old
// Wi-Fi entry, no session yet): the platform is asked to bring its radio up and SHMainMenu::Update continues.
bool NpLanMenuSelect(SHMainMenu* menu, unsigned int item);
// SHMainMenu::Update, once per frame: when the radio is ready, enters LAN mode through the game's own online entry.
void NpLanMenuUpdate(SHMainMenu* menu);
// Renames the old entry in the loaded English text table (in place, shorter strings): MULTIPLAYER and its help text.
// Call after the localization file is loaded (any byte order of the stored UTF-16 is handled).
void NpLanPatchLocalization(nlLocalization* localization);

// ---- scenes and match lifetime -------------------------------------------------------------------------------------
// BaseGameSceneManager::Push, before its own handling: redirects the online screens (login, Mii select, the online
// hub after a match) to the LAN lobby. May change the arguments. True when the call is fully handled and Push must
// return null (back out of the lobby: leaves multiplayer with the game's own transition).
bool NpLanScenePush(BaseGameSceneManager* manager, SceneList* scene, ScreenMovement* movement, bool* popFirst);
// AsyncLoadingManager::InitializeFEState: the front end is being rebuilt after a LAN match.
void NpLanBeforeInitializeFEState();

// ---- gameplay --------------------------------------------------------------------------------------------------------
// NetworkPeerChannel::CaptureNetworkPeerChannelInput, after the capture of this console's controller: the record that
// goes on the wire must not depend on this console's controller layout (see NpInputNormalizeForNetwork).
void NpLanAfterChannelCapture(NetworkPeerChannel* channel);
// NetworkDraft::BeginTeamDraft, before it reads the sides: the unfinished LAN launch never chose them.
void NpLanBeginTeamDraft(NetMessageDraft* message);

// NetworkInputRouter, for every input message that carries sync data (every 4th tick): the game's own checksum of the
// simulation (time, game object, ball, both teams) and its random seed at `tick` on `machine`. Logged as
// "[mp-lan] sync tick=.. m=.. crc=.. seed=.." every MSC_LAN_SYNC_LOG ticks (default 100, 0 = off): the router compares the
// consoles' values by itself and reports a mismatch; these lines show that the check ran and what it saw, so a pair's two
// logs can be compared tick by tick (tools/mp/native_pair.py).
void NpLanSyncSeen(int machine, unsigned tick, unsigned crc, unsigned seed);

// ---- input lag and link instrumentation (wave 5 NET, np/docs/wave5-NET.md) ----------------------------------------------
// NetworkInputRouter::OnInputCaptured, after the tick's input was handed to the transport: in a LAN session with MSC_LAN_SENDNOW
// (default 1) the queued input is sent now (TransportConnection::FlushOutgoing) instead of at the end-of-frame network task.
void NpLanInputSent();
// NetworkInputManager::PrepareUpdate, once per simulation tick of a networked match: starved = no input from some console, the
// tick's 20 ms of match clock pass without a step. Feeds the "[mp-lan] net" log line (input delay, queue depths, starved steps,
// steps per second, round trip, packets, send time) every MSC_LAN_NET_LOG ms (default 5000, 0 = off).
void NpLanStep(bool starved);
// A short form of the current window for the platform's periodic lines ([watch] on the desktop, "[vita] frame" on the Vita):
// " lan: delay .. starved .. rtt .. ms ..." into `out`. Returns the length, 0 outside a LAN match.
extern "C" int NpLanNetBrief(char* out, int size);
// The link is gone (the platform's Wi-Fi / cell watch, a resume from sleep): every link send fails from now on, so the game's own
// socket-error path ends the session with its "connection lost" popup at its next send. Safe outside a session (does nothing).
void NpLanLinkLost(const char* why);
// The platform calls this when the application comes back from sleep (Vita power callback). In a LAN session the partner has been
// waiting and the radio may be gone: the session is ended at once (NpLanLinkLost) instead of hanging on a dead link.
extern "C" void NpLanOnResume(void);

// ---- diagnostics -----------------------------------------------------------------------------------------------------
// printf-style line "[mp-lan] ..." on stderr, only while a LAN session exists. The lockstep reporters (desync, sync
// error, queue overflow, lost connection) call it so that a test run can never pass silently with a drifting match
// (scripts/mp/lan_pair.py greps these lines).
void NpLanLog(const char* format, ...);

// ---- platform radio (vlink_vita.cpp on the Vita; the desktop answers "ready" at once) --------------------------------------
// request 1 = start or keep starting, 0 = only report. Returns 0 idle, 1 busy, 2 ready, -1 failed.
int NpLanRadioPoll(int request);
void NpLanRadioStop();
// The console's user name (the lobby shows it), or null.
const char* NpLanPlayerName();

// ---- provided by the input layer (NPS), optional ---------------------------------------------------------------------
// Turns the captured local record into the form that every console can apply without reading its own controller
// (the recompiled port converts its virtual Wii Remote into GameCube-style controls here, and folds a touch shake into
// the Z button). Return false when it could not; the record is then sent as captured. Absent (weak) = no change.
class DetInput;
extern "C" bool NpInputNormalizeForNetwork(DetInput* captured) __attribute__((weak));

#endif  // NP_LAN_H
