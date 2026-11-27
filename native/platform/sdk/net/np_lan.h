// LAN multiplayer glue of the native port: what the recompiled port's msc_lan.cpp does besides the sockets, on the
// game's own C++ API. The retail game contains a finished LAN mode (session mode 1) that no menu reaches; these hooks
// start it from the main menu's old Nintendo Wi-Fi Connection entry (relabelled MULTIPLAYER), keep the game away from
// the Nintendo login and Mii screens, and restart/leave it around a match. The hooks are called from small edits of
// the decompiled sources (np/patches/mp/); outside a LAN session every one of them does nothing.
// See np/docs/multiplayer.md.
#ifndef NP_LAN_H
#define NP_LAN_H

#include <stddef.h>
#include "Game/BaseGameSceneManager.h"

class SHMainMenu;
class NetMessageDraft;
class NetworkPeerChannel;
class nlLocalization;

// A LAN session (any phase) exists: NetworkSession::mSessionMode != 0.
bool NpLanActive();
// The last answer the game thread gave NpLanActive(), readable from any thread (no pointer is followed): the render thread's MSC_LOCK60_NET_HOLD uses it.
bool NpLanActiveShared();
// (mp18) the multiplayer entry before a session exists: the online menu flow (np_menu_flow.cpp: account, room, STUN / TURN, the wait for a partner) or the local radio's
// bring-up. The fourth core's online guard (platform/vita) counts it as online: the network stack is already busy then (a room can wait 10 minutes).
bool NpLanEntryActive();
// A LAN or online match is running (session state IN_GAME), as opposed to a lobby, queue or wait.
bool NpLanInMatch();

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
// NetworkDraft::BeginTeamDraft, before it reads the sides: the unfinished LAN launch never chose them. False = the roster is invalid
// (the session is being ended): BeginTeamDraft must return without drafting.
bool NpLanBeginTeamDraft(NetMessageDraft* message);

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
// How long (ms) the lockstep has been starved of another console's input without a break, 0 when it is not (np_wait_banner.h). For the banner
// "Waiting for the other player..." (np_notice.cpp) only; reads a wall clock, changes nothing. Any thread.
unsigned NpLanWaitedMs();
// A short form of the current window for the platform's periodic lines ([watch] on the desktop, "[vita] frame" on the Vita):
// " lan: delay .. starved .. rtt .. ms ..." into `out`. Returns the length, 0 outside a LAN match.
extern "C" int NpLanNetBrief(char* out, int size);
// The link is gone (the platform's Wi-Fi / cell watch, a resume from sleep): every link send fails from now on, so the game's own
// socket-error path ends the session with its "connection lost" popup at its next send. Safe outside a session (does nothing).
void NpLanLinkLost(const char* why);
// The menu rebuilt the online link after a loss (RECONNECT in the lobby): the loss latch is cleared so the lobby's next session can send.
extern "C" void NpLanLinkRestored(void);
// The platform calls this when the application comes back from sleep (Vita power callback). In a LAN session the partner has been
// waiting and the radio may be gone: the session is ended at once (NpLanLinkLost) instead of hanging on a dead link.
extern "C" void NpLanOnResume(void);
// The platform calls this when it is told the application is about to be suspended (Vita power callback, when that notice is delivered): the
// session says goodbye to the partner the transport's own way, so the partner leaves at once instead of after the keep-alive limit (A10).
extern "C" void NpLanOnSuspend(void);

// The sentence for the popup "no game to join": set when a console with another version or other rules was refused ("Different version: ..." / "Different rules (theirs: ..., ours: ...)"), else "".
const char* NpLanNoGamesText();

// ---- wave 12 (STR-143/144/146/151) -----------------------------------------------------------------------------------------
// NetworkPeerChannel::ApplyNetworkPeerChannelInput, after the record was applied: the input trace of MSC_LAN_INPUT_TRACE (tests).
class PackedDetInput;
void NpLanAppliedInput(NetworkPeerChannel* channel, const PackedDetInput* record, unsigned short remapAngle, unsigned char connected);
// FixedUpdateTask::CalculateChecksum, before it returns: in a LAN session folds the state the retail checksum does not read (score, ball owner, clock,
// Mega Strike progress, power-ups, tangibility, pick-up timers, the random state) into the value the consoles compare. Nothing outside a LAN session.
class RunningChecksum;
void NpLanChecksumExtra(RunningChecksum* checksum);
// LANLobby::JoinGame found no game: if a console with another build or rule set answered, say so in the log (and leave a note for the platform).
void NpLanJoinFoundNoGame();
// (mpbeta27) NetworkInputRouter's compare of the consoles' sync values (what 1 frame, 2 checksum, 3 seed) failed at `tick`: both values are logged and go to the
// problem report's [lockstep] line (the first of each kind). Logging only: nothing flows back into the match or onto the wire.
void NpLanNoteMismatch(int what, int tick, unsigned ours, unsigned theirs);
// The rule byte of this console (np_lan rules: the settings that change what a match computes; bit 0 = the NK/Boo fix of MSC_GAME_FIXES) and the
// fingerprint derived from it and the protocol string. so_net.cpp.
extern "C" unsigned char NpLanRules();
extern "C" const char* NpLanRulesText(unsigned char rules, char* out, size_t size);
extern "C" unsigned NpLanFingerprint();
// wake14: a short phase label (session state, simulation running or not, last scene) for the link's gap diagnostic (MSC_LINK_GAPLOG). Reads only.
extern "C" const char* NpLanGapPhase(void);

// A7: the lockstep starts every match with `limit` neutral ticks before the first remote input is applied, so every console must start with the SAME
// value (a different one is a desync). Only the host decides: NetworkSession::SendGameStartToEveryone puts NpLanChooseInputDelay() (0 = leave the game's
// default of 4) into the game-start message, every console stores it (NpLanSetAgreedInputDelay, StartNetworkedGame) and NetworkInputRouter::Reset
// starts from NpLanInitialQueueLimit(4). MSC_LAN_INPUT_DELAY=<1..20> chooses it; =auto derives it from the measured round trip:
// 1 + ceil((rtt/2 + jitter margin) / tick), floor 2 (3 on the internet, where the round trip is the transport's own ping), cap 20; unset (default) changes nothing.
// (delay19) The choice is also the START VALUE AND FLOOR of the router's congestion multiplier (np_delay19.h): the delay holds at the choice for the whole match
// and congestion only raises it for a while (0.3 a congested message, back down 0.06 a calm one, up to 10 or the choice if larger). It is not a fixed delay: a
// link that starves still gets more. Unset: the game's own start 4.0 and floor 2.0, which settles at 2 after about 17 calm ticks.
int NpLanChooseInputDelay();
void NpLanSetAgreedInputDelay(int ticks);
int NpLanInitialQueueLimit(int fallback);
int NpLanAgreedInputDelay();   // (delay19) the host's choice in ticks while a LAN / online match is on, else 0 (the game's own constants)
// (delay19) FixedUpdateTask::Run, before the step loop of a frame: resets the frame's retry budget and, with MSC_LAN_DRAIN=1, reads the datagrams that have already arrived.
void NpLanFrameBegin();
// (delay19) NetworkInputManager::PrepareUpdate, a step that would starve (router->HasInput() false): with MSC_LAN_RETRY_US=N waits up to N us a frame, receiving only, for the
// input. True = the input is there now. Off (false at once) by default.
class InputRouter;
bool NpLanRetryWait(InputRouter* router);

// ---- the online banner (np_notice.cpp) -------------------------------------------------------------------------------------------------
// FERender::RenderNotice, once per frame after the last front-end scene was drawn (view and matrix of that scene): in a match of an online session whose
// transport has a warning ("... The match goes on.") a short line of text in the game's font, a few seconds. Draws nothing otherwise; never blocks or pauses.
class GLView;
class nlMatrix4;
void NpNoticeRender(GLView* view, const nlMatrix4& matView);
// The banner's timing alone (host tests): the alpha 0..255 at `nowMs`; the state is kept between calls.
int NpNoticeAlpha(const char* warning, int inMatch, unsigned long long nowMs, int showMs);

// ---- diagnostics -----------------------------------------------------------------------------------------------------
// printf-style line "[mp-lan] ..." on stderr, only while a LAN session exists. The lockstep reporters (desync, sync
// error, queue overflow, lost connection) call it so that a test run can never pass silently with a drifting match
// (scripts/mp/lan_pair.py greps these lines).
void NpLanLog(const char* format, ...) __attribute__((format(printf, 1, 2)));

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
