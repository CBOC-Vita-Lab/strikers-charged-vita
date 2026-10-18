// LAN multiplayer glue of the native port. See np_lan.h and np/docs/multiplayer.md.
// Port of the non-socket half of the recompiled port's msc_lan.cpp (port/runtime/src/hle/msc_lan.cpp, branch mp-one):
// same flow, same redirects, on the decompiled game's own C++ API instead of guest addresses.
#include "platform/sdk/net/np_lan.h"
#include "platform/sdk/net/vlink.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Game/BaseGameSceneManager.h"
#include "Game/GameInfo.h"
#include "Game/NetworkDebug.h"
#include "Game/NetworkInput.h"
#include "Game/NetworkMessages.h"
#include "Game/NetworkPeer.h"
#include "Game/NetworkSession.h"
#include "Game/Render/FrontEndPresentation.h"
#include "Game/SH/SHMainMenu.h"
#include "Game/SH/SHOnlinePlayerCount.h"
#include "Game/Team.h"
#include "Game/Game.h"
#include "Game/Ball.h"
#include "Game/Player.h"
#include "NL/nlMain.h"
#include "NL/nlLocalization.h"
#include "NL/plat/ReliableSocket.h"
#include "NL/plat/TransportConnection.h"
#include "Game/InputRouter.h"
#include <time.h>
#include <unistd.h>

// Retail tweak values ("Network/InputMan", InputRouter.cpp) and the router; not declared in a header.
extern int g_nTicksPerPacket;
extern NetworkInputRouter* gNetworkInputRouter;

// The harness's end-of-run hooks (platform/harness: it leaves with _Exit, no atexit handlers). Weak: absent in builds without it.
extern "C" void NpHarnessAtExit(void (*fn)(void)) __attribute__((weak));
// The ad hoc radio's receive ring (vlink_vita_radio.cpp): most records waiting since the last call. Weak: absent off the Vita.
extern "C" unsigned np_vita_radio_ring_take_peak(void) __attribute__((weak));
// ... and the datagrams it dropped since the radio opened (ring full, oversized, wrong port).
extern "C" unsigned np_vita_radio_rx_dropped(void) __attribute__((weak));

#define LAN_LOG(...) fprintf(stderr, "[mp-lan] " __VA_ARGS__)

namespace {

// SceneList values the game has no names for (BaseGameSceneManager.h): sms2_network_start.fen, the Create Game /
// Join Game lobby of the LAN mode.
const SceneList kSceneNetworkStart = (SceneList)24;
const unsigned kMainMenuWiFiItem = 1;  // SHMainMenu item 1: Nintendo Wi-Fi Connection

bool g_entering = false;   // the player picked Multiplayer; waiting for the radio
SHMainMenu* g_enterMenu = 0;
bool g_lanFlow = false;    // we put the game in LAN mode and own its scene redirects
bool g_postMatch = false;  // the front end is being rebuilt after a LAN match

int SessionMode() { return g_pNetworkSession ? g_pNetworkSession->GetSessionMode() : 0; }

// The retail transport layer declares a peer lost when no keep-alive message arrived for s_nExpireKeepAliveMS (6000). Both
// consoles stop sending keep-alives while one game thread is stuck in a long frame, and on the Vita a frame can take many seconds
// the first time a shader is needed (vitaGL compiles lazily with libshacccg: 10-14 s for a Mega Strike's first frame in Vita3K, which
// ended the match with "connection lost" on both consoles). While a LAN session is up the limit is raised (MSC_LAN_KEEPALIVE_MS,
// default 20000, minimum 1000); real silence is still detected, 20 seconds later than the retail 6.
int g_savedExpireMs = -1;
void ApplyKeepAliveLimit() {
    static const int ms = [] {
        const char* e = getenv("MSC_LAN_KEEPALIVE_MS");
        const int v = e && *e ? atoi(e) : 20000;
        return v < 1000 ? 1000 : v;
    }();
    if (g_savedExpireMs < 0) g_savedExpireMs = s_nExpireKeepAliveMS;
    s_nExpireKeepAliveMS = ms;
    LAN_LOG("keep-alive expiry %d ms (retail %d)\n", ms, g_savedExpireMs);
}
void RestoreKeepAliveLimit() {
    if (g_savedExpireMs >= 0) s_nExpireKeepAliveMS = g_savedExpireMs;
}

int EnvInt(const char* name, int fallback) {
    const char* v = getenv(name);
    return v && *v ? atoi(v) : fallback;
}

uint64_t NowUs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000u + uint64_t(ts.tv_nsec / 1000);
}

// ---- input transport (wave 5 NET, np/docs/wave5-NET.md) ----------------------------------------------------------------------
// The retail game queues each tick's input and sends it from the network task at the end of the frame, after the frame's wait for
// the retrace: at 30 fps that is 26-32 ms later, and two ticks are bundled per message, so every third tick waits one more frame. In a
// LAN session this port sends each tick's input right after it was captured, one tick per message (both are retail tweak paths:
// the transport's own send block, TransportConnection::FlushOutgoing, and g_nTicksPerPacket = 1), with twice the payload redundancy
// so that a datagram still carries the same span of earlier ticks. A console on the retail setting reads both message kinds, so
// mixed pairs link and stay in sync (np/docs/wave5-NET.md).
//   MSC_LAN_SENDNOW=0|1           (default 1) send at capture
//   MSC_LAN_TICKS_PER_PACKET=1|2  (default 1) ticks per input message (retail 2)
//   MSC_LAN_REDUNDANCY=0..9       (default 6 with one tick per message, 3 with two) earlier unacknowledged packets repeated per datagram
const int kDefaultSendNow = 1, kDefaultTicksPerPacket = 1;
bool g_sendNow = false;  // only while a LAN session of ours is up
uint64_t g_linkLostUs = 0;   // when this console's link was declared lost (a line after the game's "connection lost" says how long it took)
bool g_leaveAfterMatch = false;   // a resume ended the session (the log says so after the match)
int g_savedTicksPerPacket = -1, g_savedRedundancy = -1;

void ApplyInputTransport() {
    const int tpp = EnvInt("MSC_LAN_TICKS_PER_PACKET", kDefaultTicksPerPacket) == 2 ? 2 : 1;  // the router knows only 1 and 2
    int redundancy = EnvInt("MSC_LAN_REDUNDANCY", tpp == 1 ? 6 : 3);
    if (redundancy < 0) redundancy = 0;
    if (redundancy > 9) redundancy = 9;  // sPayloadRedundancySteps has 10 entries
    if (g_savedTicksPerPacket < 0) {
        g_savedTicksPerPacket = g_nTicksPerPacket;
        g_savedRedundancy = s_nPayloadRedundancy;
    }
    g_nTicksPerPacket = tpp;
    s_nPayloadRedundancy = redundancy;
    g_sendNow = EnvInt("MSC_LAN_SENDNOW", kDefaultSendNow) != 0;
    LAN_LOG("input transport: %s, %d tick(s) per message, payload redundancy %d (retail: at the frame's end, 2, %d)\n",
            g_sendNow ? "sent right after capture" : "sent at the frame's end", tpp, redundancy, g_savedRedundancy);
}
void RestoreInputTransport() {
    g_sendNow = false;
    if (g_savedTicksPerPacket < 0) return;
    g_nTicksPerPacket = g_savedTicksPerPacket;
    s_nPayloadRedundancy = g_savedRedundancy;
}

// ---- the "[mp-lan] net" line: what the first two-Vita session has to measure --------------------------------------------------
// Per window (MSC_LAN_NET_LOG ms, default 5000) and per match: simulation steps and starved steps, steps per second of active match
// time (gaps over 250 ms between ticks, a replay or a load, are not counted), the input delay the lockstep chose (mQueueLimit, the
// congestion multiplier it follows) and the queue depths a step found, the game's round trip estimate, datagrams and bytes per second
// each way, the backend's send time (PdpSend / sendto on the game thread) and the receive queue's high-water mark.
struct NetWindow {
    uint64_t startUs, lastStepUs, activeUs;
    unsigned steps, starved, flushes, samples, limitMin, limitMax;
    double limitSum, multSum, localDepthSum, remoteDepthSum;
    VLink::Stats link;   // the link's counters at the window's start
    VLink::WindowStats peak;
    uint32_t ringPeak, ringDrops;  // ringDrops: the radio's drop counter at the window's start
};
NetWindow g_win, g_match;
uint64_t g_nextLogUs = 0;

void ResetWindow(NetWindow& w, uint64_t now) {
    memset(&w, 0, sizeof w);
    w.startUs = now;
    w.limitMin = ~0u;
    w.link = VLink::Default().GetStats();
    w.ringDrops = np_vita_radio_rx_dropped ? np_vita_radio_rx_dropped() : 0;
}

int RoundTripMs() {
    if (!g_pNetworkSession) return -1;
    NetworkSocket* socket = g_pNetworkSession->GetDirectSocket();
    if (!socket || !socket->mInitialized) return -1;
    int best = -1;
    for (int i = 0; i < socket->mReliableSocket.mConnectionCount; ++i) {
        const TransportConnection* c = socket->mReliableSocket.mConnections[i];
        if (c && c->mState == TransportConnection::STATE_6 && c->mRoundTripTimeMS > best) best = c->mRoundTripTimeMS;
    }
    return best;
}

void FoldPeaks(NetWindow& w, const VLink::WindowStats& p, uint32_t ring) {
    if (p.sendUsMax > w.peak.sendUsMax) w.peak.sendUsMax = p.sendUsMax;
    if (p.queueHighWater > w.peak.queueHighWater) w.peak.queueHighWater = p.queueHighWater;
    if (p.payloadSentMax > w.peak.payloadSentMax) w.peak.payloadSentMax = p.payloadSentMax;
    if (p.payloadReceivedMax > w.peak.payloadReceivedMax) w.peak.payloadReceivedMax = p.payloadReceivedMax;
    if (ring > w.ringPeak) w.ringPeak = ring;
}

int g_lastRtt = -1;          // the round trip and the input delay of the last step (the totals at exit read these, no pointers)
unsigned g_lastLimit = 0;

void LogWindow(const char* what, NetWindow& w, uint64_t now) {
    const VLink::Stats st = VLink::Default().GetStats();
    const double wall = now > w.startUs ? double(now - w.startUs) / 1e6 : 0.0;
    const double active = double(w.activeUs) / 1e6;
    const unsigned ticks = w.steps + w.starved;
    const uint64_t txN = st.framesSent - w.link.framesSent, rxN = st.framesReceived - w.link.framesReceived;
    const uint64_t txB = st.bytesSent - w.link.bytesSent, rxB = st.bytesReceived - w.link.bytesReceived;
    const uint64_t sends = st.backendSends - w.link.backendSends, sendUs = st.sendUsTotal - w.link.sendUsTotal;
    // datagrams lost on this console's receiving side: the radio's ring (full / oversized), the link's socket queues, unfinished fragments
    const uint64_t drops = (np_vita_radio_rx_dropped ? uint64_t(np_vita_radio_rx_dropped() - w.ringDrops) : 0u) +
                           (st.droppedQueueFull - w.link.droppedQueueFull) + (st.droppedFragments - w.link.droppedFragments);
    const double n = w.samples ? double(w.samples) : 1.0;
    LAN_LOG("%s: t=%.1f s active=%.1f s steps=%u starved=%u (%.1f%%) sps=%.1f delay=%.2f [%u-%u] now=%u mult=%.2f qlocal=%.2f qremote=%.2f "
            "rtt=%d ms tx=%.1f/s %.0f B max %u rx=%.1f/s %.0f B max %u send=%.0f us max %u us (%llu sends) rxq=%u ring=%u drops=%llu flush=%u\n",
            what, wall, active, w.steps, w.starved, ticks ? 100.0 * w.starved / ticks : 0.0, active > 0 ? w.steps / active : 0.0,
            w.limitSum / n, w.limitMin == ~0u ? 0u : w.limitMin, w.limitMax, g_lastLimit,
            w.multSum / n, w.localDepthSum / n, w.remoteDepthSum / n, g_lastRtt, wall > 0 ? txN / wall : 0.0,
            txN ? double(txB) / double(txN) : 0.0, (unsigned)w.peak.payloadSentMax, wall > 0 ? rxN / wall : 0.0,
            rxN ? double(rxB) / double(rxN) : 0.0, (unsigned)w.peak.payloadReceivedMax, sends ? double(sendUs) / double(sends) : 0.0,
            (unsigned)w.peak.sendUsMax, (unsigned long long)sends, (unsigned)w.peak.queueHighWater, (unsigned)w.ringPeak,
            (unsigned long long)drops, w.flushes);
}

// Ends the match's totals: logged once when a match is over (the front end is rebuilt), the session is left or the process exits.
void EndMatchTotals() {
    if (!g_match.samples && !g_match.starved) return;
    const uint64_t now = NowUs();
    FoldPeaks(g_win, VLink::Default().TakeWindow(), np_vita_radio_ring_take_peak ? np_vita_radio_ring_take_peak() : 0);
    FoldPeaks(g_match, g_win.peak, g_win.ringPeak);
    LogWindow("net match", g_match, now);
    memset(&g_match, 0, sizeof g_match);
    memset(&g_win, 0, sizeof g_win);
}

void SetPlayerName() {
    const char* name = NpLanPlayerName();
    if (!name || !*name) return;
    char clean[11] = {0};
    size_t j = 0;
    for (size_t i = 0; name[i] && j < 10; ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if (c >= 0x20 && c < 0x7F) clean[j++] = char(c);  // the lobby font is ASCII
    }
    if (!j) return;
    for (size_t i = 0; i < sizeof gNetworkMiiName; ++i) gNetworkMiiName[i] = i < j ? clean[i] : 0;
}

// Leaves LAN mode and the radio. Safe to call more than once.
void LeaveLan() {
    if (!g_lanFlow) return;
    g_lanFlow = false;
    if (g_pNetworkSession && SessionMode() == 1) g_pNetworkSession->Shutdown();  // ShutdownLAN: lobby, sockets, receivers
    GameInfoManager::Instance()->mIsOnlineMode = false;
    EndMatchTotals();
    NpLanRadioStop();
    RestoreKeepAliveLimit();
    RestoreInputTransport();
    LAN_LOG("left multiplayer\n");
}

// The game shuts its session down by itself after a lost connection or a sync error. The radio and the link stay up
// for the whole multiplayer visit, so LAN mode can simply be started again.
bool EnsureLanSession() {
    if (SessionMode() == 1) return true;
    if (!g_pNetworkSession || SessionMode() != 0) return false;
    g_pNetworkSession->InitializeLAN();  // SocketNetworkStartup (the link) first, then the session
    const bool ok = SessionMode() == 1;
    LAN_LOG("LAN mode %s after the match\n", ok ? "restarted" : "could not restart");
    return ok;
}

void PlayBackToMainMenu(BaseGameSceneManager* manager) {
    LeaveLan();
    manager->Pop();
    FrontEndPresentation::GetInstance()->Call("TransitionOnlineMatchToMainMenu");
}

void RedirectToLobby(SceneList* scene) {
    InitializeOnlineMatch(false, 0, false);  // one local player, not a tournament, unranked
    *scene = kSceneNetworkStart;
}

// ---- menu text ---------------------------------------------------------------------------------------------------
struct TextPatch {
    const char* from;
    const char* to;
};
const TextPatch kTextPatches[] = {
    {"NINTENDO Wi-Fi CONNECTION", "MULTIPLAYER"},
    {"BATTLE AGAINST OTHERS OR COMPETE WITH FRIENDS IN NINTENDO Wi-Fi CONNECTION MODE!",
     "PLAY A FRIEND ON ANOTHER PS VITA NEARBY. NO ROUTER NEEDED!"},
};

// Compares a NUL-terminated UTF-16 string stored in either byte order with ASCII `text`.
bool Utf16Equals(const unsigned short* s, const char* text, bool swapped) {
    size_t i = 0;
    for (; text[i]; ++i) {
        const unsigned short v = swapped ? (unsigned short)((s[i] << 8) | (s[i] >> 8)) : s[i];
        if (v != (unsigned char)text[i]) return false;
    }
    const unsigned short end = swapped ? (unsigned short)((s[i] << 8) | (s[i] >> 8)) : s[i];
    return end == 0;
}

int PatchString(unsigned short* s, const TextPatch& patch) {
    const size_t fromLen = strlen(patch.from), toLen = strlen(patch.to);
    if (toLen >= fromLen) return 0;  // in place only: the table's layout must not change
    for (int swap = 0; swap < 2; ++swap) {
        if (!Utf16Equals(s, patch.from, swap != 0)) continue;
        for (size_t i = 0; i < fromLen; ++i) {
            const unsigned short c = i < toLen ? (unsigned char)patch.to[i] : 0;
            s[i] = swap ? (unsigned short)((c << 8) | (c >> 8)) : c;
        }
        return 1;
    }
    return 0;
}

}  // namespace

bool NpLanActive() { return SessionMode() != 0; }

// For the nav-script harness (platform/harness: wait variables smode sstate peers machines found launch joinst): the
// session and lobby counters the recompiled port's MSC_WATCH expressions read from guest memory. All zero outside a
// session. Called on the game thread at a frame start.
extern "C" void NpLanStatus(int out[8]) {
    for (int i = 0; i < 8; ++i) out[i] = 0;
    if (!g_pNetworkSession) return;
    out[0] = g_pNetworkSession->GetSessionMode();
    out[1] = g_pNetworkSession->GetSessionState();
    out[3] = g_pNetworkSession->GetNumMachines();
    if (out[0] != 1) return;
    if (LANLobby* lobby = g_pNetworkSession->GetTransport()) {
        out[2] = lobby->mPeerCount;
        out[4] = lobby->mFoundGameCount;
        out[5] = lobby->mLaunchState;
        out[6] = lobby->mJoinState;
        out[7] = lobby->mHostState;
    }
}

// ---- main menu ---------------------------------------------------------------------------------------------------
// Item 1 used to check for a Wii save, Miis and the Nintendo service; none of that exists here. Ask the platform to
// bring the radio up instead.
bool NpLanMenuSelect(SHMainMenu* menu, unsigned int item) {
    if (item != kMainMenuWiFiItem || g_lanFlow || SessionMode() != 0) return false;
    if (!g_entering) LAN_LOG("Multiplayer selected\n");
    g_entering = true;
    g_enterMenu = menu;
    NpLanRadioPoll(1);
    return true;
}

// Once the radio is up, enter LAN mode through the game's own menu action, so that it plays its own transition into
// the online screens (redirected to the lobby by NpLanScenePush).
void NpLanMenuUpdate(SHMainMenu* menu) {
    if (!g_entering || menu != g_enterMenu) return;
    const int radio = NpLanRadioPoll(1);
    if (radio == 1) return;
    g_entering = false;
    if (radio != 2) {
        LAN_LOG("radio not available (%d); staying in the main menu\n", radio);
        return;
    }
    // Same order as the game's own online entry: the menu action first (it chooses the online transition only while
    // no session mode is set), then the session.
    menu->ApplyItem(kMainMenuWiFiItem);
    SetPlayerName();
    if (g_pNetworkSession) g_pNetworkSession->InitializeLAN();
    if (SessionMode() == 1) {
        g_lanFlow = true;
        ApplyKeepAliveLimit();
        ApplyInputTransport();
        static bool exitHook = false;
        if (!exitHook) {  // a test run ends by quitting in the match: its totals are still logged (EndMatchTotals reads no game object)
            exitHook = true;
            if (NpHarnessAtExit) NpHarnessAtExit(EndMatchTotals);   // the harness leaves with _Exit: its own end-of-run hooks
            else atexit(EndMatchTotals);
        }
        VLink::Default().SetLost(false, 0);   // a link declared lost in an earlier visit (resume, Wi-Fi watch) starts afresh
        g_linkLostUs = 0;
        g_leaveAfterMatch = false;
        LAN_LOG("LAN mode on\n");
    } else {
        LAN_LOG("LAN mode failed to start\n");
        NpLanRadioStop();
    }
}

void NpLanPatchLocalization(nlLocalization* localization) {
    if (!localization || !localization->m_pFile || !localization->m_LookupTable || !localization->m_FirstString) return;
    int count = 0;
    const unsigned long strings = localization->m_pFile->StringCount;
    for (unsigned long i = 0; i < strings; ++i) {
        unsigned short* s = localization->m_FirstString + localization->m_LookupTable[i].StringOffset;
        for (size_t p = 0; p < sizeof kTextPatches / sizeof kTextPatches[0]; ++p) count += PatchString(s, kTextPatches[p]);
    }
    if (count) LAN_LOG("menu text: %d string(s) now name Multiplayer\n", count);
}

// ---- scenes ------------------------------------------------------------------------------------------------------
// The online transition heads for the Nintendo login and Mii screens; in LAN mode go to the game's own lobby instead.
// Backing out of the lobby (which returns to the online hub) leaves multiplayer for the main menu.
bool NpLanScenePush(BaseGameSceneManager* manager, SceneList* scene, ScreenMovement* movement, bool* popFirst) {
    (void)popFirst;
    if (!g_lanFlow) return false;
    if (*scene == SCENE_ONLINE_LOGIN || *scene == SCENE_ONLINE_MII_SELECT || *scene == SCENE_ONLINE_MII_SELECT_OVERLAY) {
        RedirectToLobby(scene);
        LAN_LOG("scene %d -> lobby\n", (int)*scene);
    } else if (*scene == SCENE_ONLINE_MENU && g_postMatch) {
        // After a LAN match (finished, abandoned or broken off) the game heads for the online hub: go back to the LAN
        // lobby instead, with LAN mode restarted if the game ended it.
        g_postMatch = false;
        if (VLink::Default().IsLost() || g_leaveAfterMatch) {
            // The link is gone (Wi-Fi watch, cell lost) or was closed at a resume. The lobby is the game's own, tested way out: with a dead link it
            // finds nobody and the player backs out to the main menu, where MULTIPLAYER starts the radio again. (Going to the main menu from here
            // with the lobby's back transition crashed: SHNavigation::HideButtons on a front end that has no lobby yet. PlayBackToMainMenu below
            // has the same risk; EnsureLanSession does not fail while the link is up, which it stays for the whole visit.)
            LAN_LOG("after the match: the link is %s; the lobby follows (back out to the main menu to start the radio again)\n",
                    VLink::Default().IsLost() ? "gone" : "closed at a resume");
        }
        if (!EnsureLanSession()) {
            PlayBackToMainMenu(manager);
            return true;
        }
        RedirectToLobby(scene);
        LAN_LOG("after the match -> lobby\n");
    } else if (*scene == SCENE_ONLINE_MENU) {
        // Lobby "back" (SCREEN_BACK): leave multiplayer with the game's own online-to-main-menu transition.
        LAN_LOG("lobby back (movement %d) -> main menu\n", (int)*movement);
        PlayBackToMainMenu(manager);
        return true;
    } else {
        if (*scene == SCENE_MAIN_MENU) LeaveLan();  // somehow back at the main menu
        LAN_LOG("scene push %d (session mode %d)\n", (int)*scene, SessionMode());
    }
    return false;
}

// The front end is rebuilt when a match is over. Players of a LAN match return to the LAN lobby (see the online hub
// redirect above); the radio stays up until they back out of the lobby to the main menu.
void NpLanBeforeInitializeFEState() {
    if (!g_lanFlow) return;
    LAN_LOG("match over: rebuilding the front end (session mode %d)\n", SessionMode());
    EndMatchTotals();
    g_postMatch = true;
    // Keep the game on its online route (mIsOnlineMode): it pushes the online hub with the matching camera
    // transition, and the push above turns that into the LAN lobby.
    GameInfoManager::Instance()->mIsOnlineMode = true;
}

// ---- gameplay ------------------------------------------------------------------------------------------------------
void NpLanAfterChannelCapture(NetworkPeerChannel* channel) {
    if (!NpLanActive() || !channel) return;
    if (NpInputNormalizeForNetwork && !NpInputNormalizeForNetwork(&channel->mLocalInput)) {
        static int warned = 0;
        if (warned++ < 4) LAN_LOG("capture normalization failed for a channel\n");
    }
}

// The LAN launch (NetworkSession::Update -> SendDraftToEveryone) never chose sides: in the Wi-Fi friends flow a
// choose-sides screen filled NetMessageDraft::mPlayerSides first, but the unfinished LAN path sends it untouched, so
// nobody is put on a team and the draft waits forever. Every console receives the same draft message, so assigning
// sides from machine index here gives the same teams everywhere: even machines home, odd machines away, a console's
// second local player (guest) on its own console's side.
void NpLanBeginTeamDraft(NetMessageDraft* message) {
    if (SessionMode() != 1 || !message) return;
    const int count = message->mMachineCount;
    bool valid = count >= 2 && count <= 4;
    for (int m = 0; valid && m < count; ++m) {
        const int primary = message->mPlayerSides.mData[m][0];
        valid = primary == 0 || primary == 1;
    }
    if (valid || count < 2 || count > 4) return;
    for (int m = 0; m < 4; ++m) {
        const bool present = m < count;
        const s8 side = present ? s8(m & 1) : s8(-1);
        const bool guest = present && message->mEntries[m].mGuestEnabled != 0;
        message->mPlayerSides.mData[m][0] = side;
        message->mPlayerSides.mData[m][1] = guest ? side : s8(-1);
    }
    LAN_LOG("draft sides assigned for %d consoles\n", count);
}

void NpLanSyncSeen(int machine, unsigned tick, unsigned crc, unsigned seed) {
    static const long every = [] {
        const char* e = getenv("MSC_LAN_SYNC_LOG");
        return e && *e ? atol(e) : 100L;
    }();
    static unsigned long messages = 0;
    ++messages;
    // Test only (desktop harness runs, never set on a console): MSC_LAN_TEST_SCORE=tick:team adds one goal for a team when the
    // match's own tick counter reaches `tick`, in every match of the session. Every console applies it while it processes the
    // same tick's inputs, so the lockstep simulations stay identical; scripted matches in which nobody scores still end in
    // regulation and reach the results, series and rematch screens instead of sudden death.
    static const unsigned testTick = [] { const char* v = getenv("MSC_LAN_TEST_SCORE"); return v ? (unsigned)strtoul(v, 0, 10) : 0u; }();
    static const int testTeam = [] { const char* v = getenv("MSC_LAN_TEST_SCORE"); const char* c = v ? strchr(v, ':') : 0; return c && c[1] == '1' ? 1 : 0; }();
    if (testTick && tick == testTick && machine == 0 && g_pTeams[testTeam]) {
        g_pTeams[testTeam]->m_nScore += 1;
        LAN_LOG("test score: team %d -> %d at match tick %u\n", testTeam, g_pTeams[testTeam]->m_nScore, tick);
    }
    // Test only (MSC_LAN_TEST_STALL=tick:ms): this console's game thread sleeps for `ms` once, at match tick `tick`, as it would in a frame
    // that waits for a first-use shader compile; no frames, no keep-alives from this console meanwhile.
    static const unsigned stallTick = [] { const char* v = getenv("MSC_LAN_TEST_STALL"); return v ? (unsigned)strtoul(v, 0, 10) : 0u; }();
    static const unsigned stallMs = [] { const char* v = getenv("MSC_LAN_TEST_STALL"); const char* c = v ? strchr(v, ':') : 0; return c ? (unsigned)strtoul(c + 1, 0, 10) : 0u; }();
    static bool stalled = false;
    if (stallTick && !stalled && tick == stallTick && machine == 0) {
        stalled = true;
        LAN_LOG("test stall: %u ms at match tick %u\n", stallMs, tick);
        usleep(stallMs * 1000u);
    }
    // Test only (MSC_LAN_TEST_LINKDOWN=tick): this console's link is declared lost at match tick `tick` (what the Vita's Wi-Fi / cell watch
    // does when the radio goes): every send fails from then on and the game's own socket-error path must end the session at once.
    static const unsigned downTick = [] { const char* v = getenv("MSC_LAN_TEST_LINKDOWN"); return v ? (unsigned)strtoul(v, 0, 10) : 0u; }();
    static bool downDone = false;
    if (downTick && !downDone && tick == downTick && machine == 0) {
        downDone = true;
        NpLanLinkLost("test: MSC_LAN_TEST_LINKDOWN");
    }
    // Test only (MSC_LAN_TEST_RESUME=tick:ms): as MSC_LAN_TEST_STALL, then the platform's resume notice (NpLanOnResume), as after a sleep.
    static const unsigned resumeTick = [] { const char* v = getenv("MSC_LAN_TEST_RESUME"); return v ? (unsigned)strtoul(v, 0, 10) : 0u; }();
    static const unsigned resumeMs = [] { const char* v = getenv("MSC_LAN_TEST_RESUME"); const char* c = v ? strchr(v, ':') : 0; return c ? (unsigned)strtoul(c + 1, 0, 10) : 0u; }();
    static bool resumed = false;
    if (resumeTick && !resumed && tick == resumeTick && machine == 0) {
        resumed = true;
        LAN_LOG("test sleep: %u ms at match tick %u, then the resume notice\n", resumeMs, tick);
        usleep(resumeMs * 1000u);
        NpLanOnResume();
    }
    if (every <= 0 || tick % (unsigned long)every != 0) return;
    LAN_LOG("sync tick=%u m=%d crc=%08x seed=%08x (message %lu)\n", tick, machine, crc, seed, messages);
    // The link's counters with every fourth sync line of the first console: a session that ends without a clean close (the match is left by quitting the
    // application, a power-off, a crash) still has them in the log, and the ad hoc radio's 1444-byte fragmentation is visible while it happens.
    static unsigned linkLines = 0;
    if (machine == 0 && (linkLines++ & 3u) == 0) {
        const VLink::Stats st = VLink::Default().GetStats();
        LAN_LOG("link: sent %llu frames, received %llu, dropped fp=%llu bad=%llu full=%llu; split %llu into %llu fragments, reassembled %llu, fragments dropped %llu; "
                "largest guest payload %u, largest frame to the backend %u\n",
                (unsigned long long)st.framesSent, (unsigned long long)st.framesReceived, (unsigned long long)st.droppedFingerprint,
                (unsigned long long)st.droppedMalformed, (unsigned long long)st.droppedQueueFull, (unsigned long long)st.framesSplit,
                (unsigned long long)st.fragmentsSent, (unsigned long long)st.framesReassembled, (unsigned long long)st.droppedFragments,
                (unsigned)st.maxPayloadSent, (unsigned)st.maxBackendFrame);
    }
    // MSC_LAN_PARTS=1: the game's checksum split into its sections, taken at this logical tick on every console (while the
    // router applies tick `tick`'s input), to name the section that drifts if two consoles ever disagree.
    static const bool parts = getenv("MSC_LAN_PARTS") != 0;
    if (parts && machine == 0 && g_pGame && g_pBall && g_pTeams[0] && g_pTeams[1]) {
        RunningChecksum game, ball;
        g_pGame->ChecksumState(&game);
        g_pBall->ChecksumState(&ball);
        char text[512];
        int n = snprintf(text, sizeof text, "game=%08x ball=%08x", game.m_nChecksum, ball.m_nChecksum);
        for (int t = 0; t < 2; ++t) {
            RunningChecksum team;
            g_pTeams[t]->ChecksumState(&team);
            n += snprintf(text + n, sizeof text - n, " team%d=%08x", t, team.m_nChecksum);
            for (int i = 0; i < 5; ++i) {
                RunningChecksum player;
                g_pTeams[t]->m_pPlayers[i]->ChecksumState(&player);
                n += snprintf(text + n, sizeof text - n, " p%d%d=%08x", t, i, player.m_nChecksum);
            }
        }
        LAN_LOG("parts tick=%u %s\n", tick, text);
    }
}

// ---- input transport and instrumentation (wave 5 NET) -------------------------------------------------------------------------------
void NpLanInputSent() {
    if (!g_sendNow || SessionMode() != 1 || !g_pNetworkSession) return;
    NetworkSocket* socket = g_pNetworkSession->GetDirectSocket();
    if (!socket || !socket->mInitialized) return;
    socket->mReliableSocket.FlushOutgoing();
    ++g_win.flushes;
    ++g_match.flushes;
}

void NpLanStep(bool starved) {
    if (SessionMode() != 1 || !gNetworkInputRouter) return;
    static const long everyMs = EnvInt("MSC_LAN_NET_LOG", 5000);
    const uint64_t now = NowUs();
    NetWindow* windows[2] = {&g_win, &g_match};
    for (NetWindow* w : windows) {
        if (!w->startUs) ResetWindow(*w, now);
        else if (now - w->lastStepUs < 250000u) w->activeUs += now - w->lastStepUs;
        w->lastStepUs = now;
    }
    if (!g_nextLogUs || g_nextLogUs < g_win.startUs) g_nextLogUs = g_win.startUs + uint64_t(everyMs > 0 ? everyMs : 5000) * 1000u;
    NetworkInputRouter* r = gNetworkInputRouter;
    const int local = r->mSession ? r->mSession->GetLocalMachineId() : 0;
    const int machines = r->mSession ? r->mSession->GetNumMachines() : 0;
    unsigned localDepth = 0, remoteDepth = ~0u;
    for (int m = 0; m < machines && m < 4; ++m) {
        const unsigned d = (unsigned)r->m_InputQueue[m].mCount;
        if (m == local) localDepth = d;
        else if (d < remoteDepth) remoteDepth = d;
    }
    if (remoteDepth == ~0u) remoteDepth = 0;
    for (NetWindow* w : windows) {
        if (starved) ++w->starved;
        else ++w->steps;
        ++w->samples;
        const unsigned limit = (unsigned)r->mQueueLimit;
        w->limitSum += limit;
        if (limit < w->limitMin) w->limitMin = limit;
        if (limit > w->limitMax) w->limitMax = limit;
        w->multSum += r->mCongestionMultiplier;
        w->localDepthSum += localDepth;
        w->remoteDepthSum += remoteDepth;
    }
    g_lastLimit = (unsigned)r->mQueueLimit;
    if (everyMs > 0 && now >= g_nextLogUs) {
        g_lastRtt = RoundTripMs();
        FoldPeaks(g_win, VLink::Default().TakeWindow(), np_vita_radio_ring_take_peak ? np_vita_radio_ring_take_peak() : 0);
        FoldPeaks(g_match, g_win.peak, g_win.ringPeak);
        LogWindow("net", g_win, now);
        ResetWindow(g_win, now);
        g_win.lastStepUs = now;
        g_nextLogUs = now + uint64_t(everyMs) * 1000u;
    }
}

extern "C" int NpLanNetBrief(char* out, int size) {
    if (!out || size <= 0) return 0;
    out[0] = 0;
    if (SessionMode() != 1 || !g_win.samples || !gNetworkInputRouter) return 0;
    const double active = double(g_win.activeUs) / 1e6;
    g_lastRtt = RoundTripMs();
    const int n = snprintf(out, (size_t)size, " lan: delay=%.2f now=%u starved=%u sps=%.1f rtt=%d", g_win.limitSum / g_win.samples,
                           (unsigned)gNetworkInputRouter->mQueueLimit, g_win.starved, active > 0 ? g_win.steps / active : 0.0, g_lastRtt);
    return n < 0 ? 0 : (n >= size ? size - 1 : n);
}

void NpLanLinkLost(const char* why) {
    if (!g_lanFlow && SessionMode() == 0) return;
    LAN_LOG("link lost (%s): the session ends at the next send, with the game's own message\n", why ? why : "?");
    if (!g_linkLostUs) g_linkLostUs = NowUs();
    VLink::Default().SetLost(true, why);
}

extern "C" void NpLanOnResume(void) {
    if (!g_lanFlow) return;
    static const bool on = EnvInt("MSC_LAN_LINKWATCH", 1) != 0;
    if (!on) {
        LAN_LOG("resumed from sleep in a LAN session; MSC_LAN_LINKWATCH=0: left to the keep-alive expiry\n");
        return;
    }
    // The partner has been waiting (and starving) since the sleep began, and the radio may or may not have survived it. Every connection is
    // closed the transport's own way (TransportConnection::Disconnect(false): a reliable "closing" message, then the closing time-out of 500 ms):
    // if the radio still works the partner learns it at once from that message instead of after the 20 s keep-alive limit, and this console
    // shows the game's "connection lost" within half a second; if the radio is gone the send fails and the game's socket-error path ends it
    // at once. No callback runs here: the network task does the rest.
    g_leaveAfterMatch = true;
    if (!g_linkLostUs) g_linkLostUs = NowUs();
    int closing = 0;
    NetworkSocket* socket = g_pNetworkSession ? g_pNetworkSession->GetDirectSocket() : 0;
    if (socket && socket->mInitialized) {
        for (int i = 0; i < socket->mReliableSocket.mConnectionCount; ++i) {
            TransportConnection* c = socket->mReliableSocket.mConnections[i];
            if (c && c->GetState() == TransportConnection::STATE_6) {
                socket->mReliableSocket.Disconnect(c, false);
                ++closing;
            }
        }
    }
    LAN_LOG("resumed from sleep in a LAN session (the partner was left waiting, the radio may be gone): closing %d connection(s)\n", closing);
    if (!closing) NpLanLinkLost("resumed from sleep, no connection to close");
}

void NpLanLog(const char* format, ...) {
    if (!NpLanActive()) return;
    fputs("[mp-lan] ", stderr);
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    if (g_linkLostUs && strncmp(format, "connection lost", 15) == 0) {
        LAN_LOG("session ended %.0f ms after this console's link was declared lost (or closed at a resume)\n", double(NowUs() - g_linkLostUs) / 1000.0);
        g_linkLostUs = 0;
    }
}

// ---- desktop radio: nothing to bring up ----------------------------------------------------------------------------------
#if !defined(NP_TARGET_VITA)
int NpLanRadioPoll(int) { return 2; }
void NpLanRadioStop() {}
const char* NpLanPlayerName() { return getenv("MSC_LINK_NAME"); }
#endif
