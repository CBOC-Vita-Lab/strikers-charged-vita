// LAN multiplayer glue of the native port. See np_lan.h and np/docs/multiplayer.md.
// Port of the non-socket half of the recompiled port's msc_lan.cpp (port/runtime/src/hle/msc_lan.cpp, branch mp-one):
// same flow, same redirects, on the decompiled game's own C++ API instead of guest addresses.
#include "platform/sdk/net/np_lan_flag.h"
#include "platform/sdk/net/np_lan.h"
#include "platform/sdk/include/np/np_text.h"
#include "platform/sdk/net/np_lan_text.h"
#include "platform/sdk/net/np_wait_banner.h"
#include "platform/sdk/net/np_mpdiag.h"
#include "platform/sdk/net/np_mpbeta.h"
#include "platform/sdk/net/np_delay19.h"
#include "platform/sdk/net/vlink.h"
#include "platform/sdk/net/np_online_save.h"
#include "platform/sdk/net/online/np_online_api.h"
#include "platform/sdk/net/np_menu_flow.h"
#include "platform/sdk/dwc/np_online_flow.h"

#include <initializer_list>
#include <ctype.h>
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
#include "Game/AI/Fielder.h"
#include "Game/InputManager.h"
#include "Game/PackedDetInput.h"
#include "Game/Sys/clock.h"
#include "Game/Task/FixedUpdateTask.h"
#include <time.h>
#include <unistd.h>
#include "platform/sdk/net/np_mpfix18.h"   // mpfix18: stranger-line limiter

// Retail tweak values ("Network/InputMan", InputRouter.cpp) and the router; not declared in a header.
extern int g_nTicksPerPacket;
extern NetworkInputRouter* gNetworkInputRouter;
extern unsigned int gNetworkRandomSeed;   // the simulation's random-number state (NetworkRandomSeed.cpp)

// The harness's end-of-run hooks (platform/harness: it leaves with _Exit, no atexit handlers). Weak: absent in builds without it.
extern "C" void NpHarnessAtExit(void (*fn)(void)) __attribute__((weak));
// The ad hoc radio's receive ring (vlink_vita_radio.cpp): most records waiting since the last call. Weak: absent off the Vita.
extern "C" unsigned np_vita_radio_ring_take_peak(void) __attribute__((weak));
// ... and the datagrams it dropped since the radio opened (ring full, oversized, wrong port).
extern "C" unsigned np_vita_radio_rx_dropped(void) __attribute__((weak));
// (delay19) the ad hoc sends the radio answered WOULD_BLOCK to (counted as sent, not delivered). Weak: absent off the Vita.
extern "C" unsigned np_vita_radio_send_refused(void) __attribute__((weak));
extern "C" int NpSimtEnabled(void) __attribute__((weak));   // MSC_SIMT: the step loop runs on another thread; the starved-step wait is refused there

// LAN_LOG: the line goes to the log as before and into the diagnostics' log tail (np_mpdiag.h: the last 8 KiB of multiplayer lines, kept in every build, for SEND A REPORT)
static void LanLogTee(const char* format, ...) __attribute__((format(printf, 1, 2)));
#define LAN_LOG(...) LanLogTee("[mp-lan] " __VA_ARGS__)
// (mpfix18) a line that a stranger's traffic can make us write again and again (a refused console's frames while the lobby looks for a game): 8 at once, then 2 a second, per site
#define LAN_LOG_LIMITED(...) do { static NpMpfix18::LogBucket lb_ = {0, 0, false}; if (NpMpfix18::LogAllow(lb_, NowUs() / 1000u)) LAN_LOG(__VA_ARGS__); } while (0)

static void LanLogTee(const char* format, ...) {
    va_list log, tail;
    va_start(log, format);
    va_copy(tail, log);
    vfprintf(stderr, format, log);
    va_end(log);
    NpMpDiag::NoteLogLine(format, tail);
    va_end(tail);
}

namespace {

// SceneList values the game has no names for (BaseGameSceneManager.h): sms2_network_start.fen, the Create Game /
// Join Game lobby of the LAN mode.
const SceneList kSceneNetworkStart = (SceneList)24;
const unsigned kMainMenuWiFiItem = 1;  // SHMainMenu item 1: Nintendo Wi-Fi Connection

bool g_entering = false;   // the player picked Multiplayer; waiting for the radio
SHMainMenu* g_enterMenu = 0;
bool g_lanFlow = false;    // we put the game in LAN mode and own its scene redirects
bool g_postMatch = false;  // the front end is being rebuilt after a LAN match
bool g_menuFlow = false;   // the online entry (np_menu_flow.cpp) is running: host / join, account, invite code, bring-up
char g_noGamesText[200];   // why the lobby found no game to join, when a console with another version or rules was refused (A13); "" otherwise

int SessionMode() { return g_pNetworkSession ? g_pNetworkSession->GetSessionMode() : 0; }
void RunWireSelfTest();
struct EndDigest { unsigned tick, crc, seed; bool seen; };
EndDigest g_endDigest[4];        // the last checksum/seed each machine's input message carried (A9: logged when the match ends)
int g_endScore[2] = {-1, -1};    // the score at the last step
int g_lastScene = -1;            // wake14: the scene pushed last (NpLanGapPhase)
bool g_quietLog = false;         // the wire self-test feeds the parser garbage on purpose: its "malformed" notices are not logged
int g_agreedDelay = 0;           // A7: the input delay the host chose for this match (0 = the game's default)
bool g_suspendSaidBye = false;   // NpLanOnSuspend said goodbye before the sleep
bool g_digestExtSuspended = false;   // a test computes the retail checksum next to the extended one

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

// The MSC_LAN_TEST_* switches change this console's own state on purpose (a goal, a desync, a stall, a lost link). They exist for the harness runs and
// the lab builds only: a quiet release build (NP_LOGGING=0) never reads them, from env.txt, np-player.txt or anywhere else, so that a partner cannot
// use them to cause a desync in a ranked match. NP_LOGGING is 1 when undefined (host builds, the pair test).
#ifndef NP_LOGGING
#define NP_LOGGING 1
#endif
const char* TestEnv(const char* name) {
#if NP_LOGGING
    return getenv(name);
#else
    (void)name;
    return nullptr;
#endif
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
    uint32_t refusedStart;         // (delay19) the radio's refused-send counter at the window's start
    unsigned drainRuns, drainDgrams, retryTries, retryOk, retryUs;   // (delay19) MSC_LAN_DRAIN / MSC_LAN_RETRY_US
};
NetWindow g_win, g_match;
uint64_t g_nextLogUs = 0;

void ResetWindow(NetWindow& w, uint64_t now) {
    memset(&w, 0, sizeof w);
    w.startUs = now;
    w.limitMin = ~0u;
    w.link = VLink::Default().GetStats();
    w.ringDrops = np_vita_radio_rx_dropped ? np_vita_radio_rx_dropped() : 0;
    w.refusedStart = np_vita_radio_send_refused ? np_vita_radio_send_refused() : 0;
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
    const unsigned refused = np_vita_radio_send_refused ? np_vita_radio_send_refused() - w.refusedStart : 0u;   // (delay19) ad hoc sends the radio refused (WOULD_BLOCK) this window
    const double n = w.samples ? double(w.samples) : 1.0;
    LAN_LOG("%s: t=%.1f s active=%.1f s steps=%u starved=%u (%.1f%%) sps=%.1f delay=%.2f [%u-%u] now=%u mult=%.2f qlocal=%.2f qremote=%.2f "
            "rtt=%d ms tx=%.1f/s %.0f B max %u rx=%.1f/s %.0f B max %u send=%.0f us max %u us (%llu sends) rxq=%u ring=%u drops=%llu flush=%u "
            "refused=%u drain=%s/%u/%u retry=%dus/%u/%u/%u\n",
            what, wall, active, w.steps, w.starved, ticks ? 100.0 * w.starved / ticks : 0.0, active > 0 ? w.steps / active : 0.0,
            w.limitSum / n, w.limitMin == ~0u ? 0u : w.limitMin, w.limitMax, g_lastLimit,
            w.multSum / n, w.localDepthSum / n, w.remoteDepthSum / n, g_lastRtt, wall > 0 ? txN / wall : 0.0,
            txN ? double(txB) / double(txN) : 0.0, (unsigned)w.peak.payloadSentMax, wall > 0 ? rxN / wall : 0.0,
            rxN ? double(rxB) / double(rxN) : 0.0, (unsigned)w.peak.payloadReceivedMax, sends ? double(sendUs) / double(sends) : 0.0,
            (unsigned)w.peak.sendUsMax, (unsigned long long)sends, (unsigned)w.peak.queueHighWater, (unsigned)w.ringPeak,
            (unsigned long long)drops, w.flushes,
            refused, NpDelay19::Drain() ? "on" : "off", w.drainRuns, w.drainDgrams, NpDelay19::RetryUs(), w.retryTries, w.retryOk, w.retryUs);
}

// Ends the match's totals: logged once when a match is over (the front end is rebuilt), the session is left or the process exits.
void EndMatchTotals() {
    if (!g_match.samples && !g_match.starved) return;
    const uint64_t now = NowUs();
    FoldPeaks(g_win, VLink::Default().TakeWindow(), np_vita_radio_ring_take_peak ? np_vita_radio_ring_take_peak() : 0);
    FoldPeaks(g_match, g_win.peak, g_win.ringPeak);
    LogWindow("net match", g_match, now);
    {   // the diagnostics ring: the match's totals and the last digest the first two consoles reported (equal crc at the same tick = in step)
        char digests[48] = "";
        if (g_endDigest[0].seen) snprintf(digests, sizeof digests, "t=%u crc=%08x/%08x", g_endDigest[0].tick, g_endDigest[0].crc, g_endDigest[1].seen ? g_endDigest[1].crc : 0u);
        NpMpDiag::Note(NpMpDiag::kMatchEnd, int(g_match.steps), int(g_match.starved), g_lastRtt, digests);
        NpMpDiag::SetLoss(NpNetActive() ? NpNetLossPermille() : 0);   // the session numbers of a report (np_mpdiag.h SessionNumbers)
    }
    {   // (delay19) the radio's refusals and the starved-step recovery, for the report (only when there is something to say)
        const unsigned refused = np_vita_radio_send_refused ? np_vita_radio_send_refused() - g_match.refusedStart : 0u;
        if (refused || g_match.retryTries || g_match.drainRuns) {
            char note[56];
            snprintf(note, sizeof note, "radio refused %u sends; retry %u/%u ok; drain %u/%u", refused, g_match.retryTries, g_match.retryOk, g_match.drainRuns, g_match.drainDgrams);
            NpMpDiag::Note(NpMpDiag::kNote, int(refused), 0, 0, note);
        }
    }
    memset(&g_match, 0, sizeof g_match);
    memset(&g_win, 0, sizeof g_win);
    NpMpDiag::Reach(NpMpDiag::kStageResult);   // (mpbeta27) the match is over: the result screen
}

// The end of the process inside multiplayer: the match's totals; and (mpbeta27, test only: MSC_LAN_TEST_REPORT_AT_EXIT=1, never read by a quiet release build) the problem
// report this console would send, saved as <data>/mp-report.txt, so that a pair run's fault cases can check what the report says and that it is scrubbed.
void ExitHook() {
    EndMatchTotals();
    static const bool reportAtExit = TestEnv("MSC_LAN_TEST_REPORT_AT_EXIT") && TestEnv("MSC_LAN_TEST_REPORT_AT_EXIT")[0] == '1';
    if (!reportAtExit) return;
    char path[300] = "";
    const bool saved = NpMpDiag::SaveReport(nullptr, path, sizeof path);
    fprintf(stderr, "[mp-lan] test report at exit: %s; offer %s\n", saved ? "saved" : "NOT saved", NpMpDiag::OfferWanted() ? "armed" : "not armed");
}

void SetPlayerName() {
    const char* name = NpNetActive() ? NpMenuPlayerName() : "";   // internet play: the name typed for it, not the Vita user name
    if (!name || !*name) name = NpLanPlayerName();
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
    NpMpDiag::Note(NpMpDiag::kSessionEnd);
    NpMpDiag::SessionMarkClear(nullptr);   // (mpbeta27) the session was left: no "crash" for the next start
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
// The online screens and their dialogs speak of Nintendo's service, the Wii console and Nintendo support: say what is true here. (1.3) By message id,
// in every language file the disc carries (np_lan_text.h), with the Spanish and French texts of np_text_strings.tsv; the replacement is written in place and
// must fit the old text. {0} and {p} are the game's own markers and stay.
}  // namespace

bool NpLanActive() {   // (game thread: it reads g_pNetworkSession; the answer is published for the other cores, NpLanActiveShared)
    const bool active = SessionMode() != 0;
    NpLanFlag::Publish(active);
    return active;
}
bool NpLanActiveShared() { return NpLanFlag::Read(); }
bool NpLanEntryActive() { return g_entering || g_menuFlow; }   // (mp18) see np_lan.h

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
    if (NpOnlineMenuSelect(menu, item)) return true;   // MSC_ONLINE=1: the online flow (platform/sdk/dwc/np_online_flow.cpp) instead of the LAN lobby
    if (item != kMainMenuWiFiItem || g_lanFlow || SessionMode() != 0) return false;
    if (!g_entering) {
        LAN_LOG("Multiplayer selected\n");
        if (NpMenuWanted() && !NpNetActive()) {   // choose online or local before starting either transport (np_menu_flow.cpp)
            NpMenuBegin();
            g_menuFlow = true;
        }
    }
    g_entering = true;
    g_enterMenu = menu;
    if (!g_menuFlow) NpLanRadioPoll(1);   // the radio is only needed by Nearby / Same Wi-Fi; the online flow brings its own link up
    return true;
}

// Once the radio is up, enter LAN mode through the game's own menu action, so that it plays its own transition into
// the online screens (redirected to the lobby by NpLanScenePush).
void NpLanMenuUpdate(SHMainMenu* menu) {
    NpOnlineMenuUpdate(menu);
    if (!g_entering || menu != g_enterMenu) return;
    if (g_menuFlow) {
        const int step = NpMenuStep();   // once per frame, never blocks
        if (step == kNpMenuBusy) return;
        g_menuFlow = false;
        if (step == kNpMenuRanked || step == kNpMenuQuick || step == kNpMenuFriends) {   // 1.3: Online Central (ranked matchmaking, the quick match, the friend screens and the ranking tables), the game's own online screens
            g_entering = false;
            if (!NpOnlineCentralBegin(menu)) LAN_LOG("online central could not start; staying in the main menu\n");
            return;
        }
        if (step != kNpMenuEnter && step != kNpMenuLocal) {   // cancelled or failed: never fall back to the radio
            LAN_LOG("online entry ended without a link; staying in the main menu\n");
            g_entering = false;
            return;
        }
        if (step == kNpMenuEnter) LAN_LOG("online link up; entering the lobby\n");
        else LAN_LOG("local choice ready; starting the radio\n");
    }
    const int radio = NpLanRadioPoll(1);
    if (radio == 1) return;
    g_entering = false;
    if (radio != 2) {
        LAN_LOG("radio not available (%d); staying in the main menu\n", radio);
        NpLanRadioStop();
        if (NpMenuLinkMode() == kNpMenuWifi) {   // (Nearby: the system's dialog already spoke, or the player cancelled it)
            NpMenuShowFailure("Same Wi-Fi needs a Wi-Fi connection. Connect this Vita to a network and try again; just after Nearby play that can take a few seconds.");
            g_menuFlow = true;
            g_entering = true;
        }
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
            if (NpHarnessAtExit) NpHarnessAtExit(ExitHook);   // the harness leaves with _Exit: its own end-of-run hooks
            else atexit(ExitHook);
        }
        VLink::Default().SetLost(false, 0);   // a link declared lost in an earlier visit (resume, Wi-Fi watch) starts afresh
        g_linkLostUs = 0;
        g_leaveAfterMatch = false;
        LAN_LOG("LAN mode on\n");
        NpMpDiag::Note(NpMpDiag::kSessionStart, NpNetActive() ? 1 : 0);   // always-on diagnostics ring (np_mpdiag.h)
        {   // (mpbeta27) the session trail: the lobby is reached; the crash marker says "in a session" until LeaveLan (menu time, never in the match)
            const NpMpDiag::Trail t = NpMpDiag::CurrentTrail();
            if (t.link == NpMpDiag::kLinkUnknown) NpMpDiag::SetMode(NpNetActive() ? NpMpDiag::kLinkOnline : NpMpDiag::kLinkSameWifi, t.play);
            NpMpDiag::Reach(NpMpDiag::kStageLobby);
            NpMpDiag::SessionMarkWrite(nullptr);
        }
        RunWireSelfTest();
    } else {
        LAN_LOG("LAN mode failed to start\n");
        NpLanRadioStop();
    }
}

void NpLanPatchLocalization(nlLocalization* localization) {
    if (!localization || !localization->m_pFile || !localization->m_LookupTable || !localization->m_FirstString) return;
    const char* switchText = getenv("MSC_ONLINE_TEXT");   // 0: leave the Nintendo service wording of the online screens as the disc has it
    const bool onlineText = !(switchText && switchText[0] == '0');
    const int count = NpLanText::PatchById(localization->m_LookupTable, localization->m_pFile->StringCount, localization->m_FirstString,
                                           (int)localization->m_CurrentLanguage, onlineText, NpMpBeta::On());   // (mpbeta27) MULTIPLAYER BETA, ONLINE CENTRAL BETA
    if (count) LAN_LOG("menu text: %d string(s) replaced by id (language %d)\n", count, (int)localization->m_CurrentLanguage);
}

// ---- scenes ------------------------------------------------------------------------------------------------------
// The online transition heads for the Nintendo login and Mii screens; in LAN mode go to the game's own lobby instead.
// Backing out of the lobby (which returns to the online hub) leaves multiplayer for the main menu.
bool NpLanScenePush(BaseGameSceneManager* manager, SceneList* scene, ScreenMovement* movement, bool* popFirst) {
    (void)popFirst;
    g_lastScene = (int)*scene;   // (wake14: the gap diagnostic's phase label)
    NpOnlineScenePush(scene);
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
    {   // A9: the end-of-match digest of this console: score and, per machine, the last checksum and random seed its input message carried. The
        // checksum covers the score, the ball's owner and the clock (extended checksum), so equal digests on both consoles mean equal results.
        char text[200];
        int n = snprintf(text, sizeof text, "end digest: score %d-%d", g_endScore[0], g_endScore[1]);
        for (int m = 0; m < 4 && n > 0 && n < (int)sizeof text - 48; ++m)
            if (g_endDigest[m].seen) n += snprintf(text + n, sizeof text - n, " m%d@%u=%08x/%08x", m, g_endDigest[m].tick, g_endDigest[m].crc, g_endDigest[m].seed);
        LAN_LOG("%s\n", text);
        memset(g_endDigest, 0, sizeof g_endDigest);
        g_endScore[0] = g_endScore[1] = -1;
    }
    EndMatchTotals();
    g_postMatch = true;
    // Keep the game on its online route (mIsOnlineMode): it pushes the online hub with the matching camera
    // transition, and the push above turns that into the LAN lobby.
    GameInfoManager::Instance()->mIsOnlineMode = true;
}

// ---- gameplay ------------------------------------------------------------------------------------------------------
// ---- wire self-test (STR-144): MSC_LAN_WIRE_SELFTEST=1, once per run when LAN mode starts (desktop and emulator tests; nothing on a console) ----
// A message with every field set and four distinct 16-byte records is written and read back (byte-exact); every truncation of it is refused with
// nothing applied; garbage of every length (a few thousand random buffers) never crosses the buffer end and never leaves a count above its bound.
namespace {
uint32_t g_wireSeed = 0x2468ACE1u;
uint32_t WireRand() { g_wireSeed = g_wireSeed * 1664525u + 1013904223u; return g_wireSeed >> 8; }

void RunWireSelfTest() {
    static bool done = false;
    if (done || EnvInt("MSC_LAN_WIRE_SELFTEST", 0) == 0) return;
    done = true;
    g_quietLog = true;
    int failures = 0, checks = 0;
#define WIRE_CHECK(c) do { ++checks; if (!(c)) { ++failures; LAN_LOG("wire self-test FAILED: %s\n", #c); } } while (0)
    NetMessageInput in;
    for (int p = 0; p < 4; ++p) {
        PackedDetInput rec;
        for (int i = 0; i < 16; ++i) ((u8*)&rec)[i] = (u8)(0x11 * (p + 1) + i * 5 + 1);   // no zero byte: a cut-off tail shows
        in.SetNetworkInputMessageRecord((s8)p, &rec);
    }
    in.SetNetworkInputMessageRemapAngle(0x3400);
    in.SetNetworkInputMessageSyncData(0x11223344u, 77u, 0x55667788u);
    u8 buf[256];
    NetworkMessageSerializer w(NETWORK_SERIALIZE_WRITE, buf, sizeof buf);
    in.Serialize(&w);
    const int len = w.GetLength();
    WIRE_CHECK(!w.mFailed && len > 16 * 4);
    NetMessageInput out;
    NetworkMessageSerializer r(NETWORK_SERIALIZE_READ, buf, len);
    out.Serialize(&r);
    WIRE_CHECK(!r.mFailed && r.GetLength() == len);
    for (int p = 0; p < 4; ++p) WIRE_CHECK(memcmp(out.mRecords[p].mData, in.mRecords[p].mData, 16) == 0);   // all 16 bytes of every record
    WIRE_CHECK(out.mChecksum == 0x11223344u && out.mFrame == 77u && out.mRandomSeed == 0x55667788u && out.mRemapAngleHigh == 0x34);
    int refused = 0;
    for (int cut = 0; cut < len; ++cut) {
        NetMessageInput t;
        NetworkMessageSerializer rc(NETWORK_SERIALIZE_READ, buf, cut);
        t.Serialize(&rc);
        bool nothing = t.mFlags == 0 && t.mEventCount == 0;
        for (int p = 0; p < 4; ++p) nothing = nothing && t.mRecordChanges[p] == 0;
        if (nothing) ++refused;
    }
    WIRE_CHECK(refused == len);   // every proper prefix is refused, none applied
    // a write into a buffer that is too small fails instead of overrunning it
    u8 small[8];
    memset(small + 4, 0xEE, 4);
    NetworkMessageSerializer ws(NETWORK_SERIALIZE_WRITE, small, 4);
    in.Serialize(&ws);
    WIRE_CHECK(ws.mFailed && small[4] == 0xEE && small[7] == 0xEE);
    // garbage: random buffers of random length with a canary after the end
    int garbled = 0, maxCount = 0;
    for (int n = 0; n < 4000; ++n) {
        u8 g[72];
        const int glen = (int)(WireRand() % 64);
        for (int i = 0; i < 72; ++i) g[i] = (u8)WireRand();
        memset(g + glen, 0xA5, 72 - glen);
        u8 copy[72];
        memcpy(copy, g, sizeof copy);
        NetMessageInput t;
        NetworkMessageSerializer rg(NETWORK_SERIALIZE_READ, g, glen);
        t.Serialize(&rg);
        WIRE_CHECK(rg.GetLength() <= glen && memcmp(copy, g, sizeof g) == 0);
        if (t.mEventCount > maxCount) maxCount = t.mEventCount;
        if (rg.mFailed || t.mFlags == 0) ++garbled;
        NetMessageDraft d;
        NetworkMessageSerializer rd(NETWORK_SERIALIZE_READ, g, glen);
        d.Serialize(&rd);
        WIRE_CHECK(d.mMachineCount >= 0 && d.mMachineCount <= 8 && rd.GetLength() <= glen);
    }
    WIRE_CHECK(maxCount <= 4);
    LAN_LOG("wire self-test: %d checks, %d failed (message %d bytes, %d truncations refused, %d of 4000 garbage buffers refused or empty)\n", checks, failures,
            len, refused, garbled);
#undef WIRE_CHECK
    g_quietLog = false;
}
}  // namespace

namespace {
// ---- input traces (STR-143 / STR-144 evidence) ---------------------------------------------------------------------------------------
// MSC_LAN_INPUT_TRACE=1 (tests only): a line "[mp-in] cap|app tick=.. ch=.. h=.. rec=<16 bytes hex> conn=.. remap=.." whenever a channel's captured record
// (what this console puts on the wire) or applied record (what the lockstep hands to the simulation) changes. tools/mp/input_compare.py checks that
// (1) both consoles applied exactly the same records at the same ticks, and (2) the records a console captured for its own channels are the records
// both consoles applied, in order: the wire carries all 16 bytes and no console applied anything read from its own pad.
bool InputTrace() {
    static const bool on = EnvInt("MSC_LAN_INPUT_TRACE", 0) != 0;
    return on;
}
uint32_t RecordHash(const uint8_t* rec, unsigned conn, unsigned remap) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < 16; ++i) h = (h ^ rec[i]) * 16777619u;
    h = (h ^ (conn & 255u)) * 16777619u;
    h = (h ^ ((remap >> 8) & 255u)) * 16777619u;   // the wire keeps the high byte of the remap angle
    return h;
}
void TraceRecord(const char* kind, int channel, const uint8_t* rec, unsigned conn, unsigned remap) {
    static uint32_t last[2][16];
    static bool seen[2][16];
    const int k = kind[0] == 'c' ? 0 : 1;
    if (channel < 0 || channel > 15) return;
    const uint32_t h = RecordHash(rec, conn, remap);
    if (seen[k][channel] && last[k][channel] == h) return;
    seen[k][channel] = true;
    last[k][channel] = h;
    char hex[40];
    for (int i = 0; i < 16; ++i) snprintf(hex + i * 2, 3, "%02x", rec[i]);
    fprintf(stderr, "[mp-in] %s tick=%d ch=%d h=%08x rec=%s conn=%u remap=%04x\n", kind, GetFixedUpdateTask()->GetFrame(), channel, (unsigned)h, hex, conn,
            remap & 0xFF00u);
}

}  // namespace

void NpLanAfterChannelCapture(NetworkPeerChannel* channel) {
    if (!NpLanActive() || !channel) return;
    if (NpInputNormalizeForNetwork && !NpInputNormalizeForNetwork(&channel->mLocalInput)) {
        static int warned = 0;
        if (warned++ < 4) LAN_LOG("capture normalization failed for a channel\n");
    }
    if (InputTrace()) {
        PackedDetInput rec;
        channel->PackNetworkPeerChannelInput(&rec);
        TraceRecord("cap", channel->GetNetworkPeerChannelId(), reinterpret_cast<const uint8_t*>(&rec), channel->GetNetworkPeerChannelConnectionStatus(),
                    channel->GetNetworkPeerChannelRemapAngle());
    }
}

void NpLanAppliedInput(NetworkPeerChannel* channel, const PackedDetInput* record, unsigned short remapAngle, unsigned char connected) {
    // (mpbeta27) test only (MSC_LAN_TEST_INPUT_FLIP=<step>[:<channel>] on ONE console of a pair; never read by a quiet release build): the first record of that channel
    // applied at or after <step> is applied with its left stick turned the other way and one button flipped, as a record damaged on the way would be. The other console
    // applied the record as it was sent: the checksum compare must report the desync within a few ticks, and the report must carry the step and both checksums.
    static const long flipAt = [] { const char* v = TestEnv("MSC_LAN_TEST_INPUT_FLIP"); return v && *v ? atol(v) : -1L; }();
    if (flipAt >= 0 && channel && NpLanActive()) {
        static const int flipChannel = [] { const char* v = TestEnv("MSC_LAN_TEST_INPUT_FLIP"); const char* c = v ? strchr(v, ':') : 0; return c ? atoi(c + 1) : 1; }();
        static bool flipped = false;
        if (!flipped && channel->GetNetworkPeerChannelId() == flipChannel && long(GetFixedUpdateTask()->GetFrame()) >= flipAt) {
            flipped = true;
            channel->mInput.m_AnalogLeftX = channel->mInput.m_AnalogLeftX > 0.5f ? -1.0f : 1.0f;
            channel->mInput.m_ButtonBitfield ^= 0x0100;
            LAN_LOG("test input flip: channel %d's record applied damaged at step %d on this console only\n", flipChannel, GetFixedUpdateTask()->GetFrame());
        }
    }
    if (!InputTrace() || !channel || !record || !NpLanActive()) return;
    TraceRecord("app", channel->GetNetworkPeerChannelId(), reinterpret_cast<const uint8_t*>(record), connected, remapAngle);
}

// The LAN launch (NetworkSession::Update -> SendDraftToEveryone) never chose sides: in the Wi-Fi friends flow a
// choose-sides screen filled NetMessageDraft::mPlayerSides first, but the unfinished LAN path sends it untouched, so
// nobody is put on a team and the draft waits forever. Every console receives the same draft message, so assigning
// sides from machine index here gives the same teams everywhere: even machines home, odd machines away, a console's
// second local player (guest) on its own console's side.
//
// STR-145: the draft indexes its two team tables (3 players each) with these sides, so a roster that does not fit must never get that far.
// A roster is valid when 2..4 machines are present, every present machine (and its guest, if enabled) is on side 0 or 1, no team holds more
// than 3 players and both teams hold someone; sides the message already carries are used when they are valid, otherwise the deterministic rule
// above is applied (the same on every console, so no console can end up with other teams than its partner); a roster the rule cannot make
// valid (more guests than a team has places) ends the session through the link-lost path instead of corrupting the draft.
namespace {
bool RosterFits(const NetMessageDraft* message, int count) {
    if (count < 2 || count > 4) return false;
    int perTeam[2] = {0, 0};
    for (int m = 0; m < count; ++m) {
        const int primary = message->mPlayerSides.mData[m][0];
        if (primary != 0 && primary != 1) return false;
        ++perTeam[primary];
        if (message->mEntries[m].mGuestEnabled) {
            const int guest = message->mPlayerSides.mData[m][1];
            if (guest != 0 && guest != 1) return false;
            ++perTeam[guest];
        }
    }
    return perTeam[0] >= 1 && perTeam[1] >= 1 && perTeam[0] <= 3 && perTeam[1] <= 3;
}
void LogSides(const NetMessageDraft* message, const char* how) {
    char text[160];
    int n = snprintf(text, sizeof text, "draft sides (%s) machines=%d local=%d:", how, (int)message->mMachineCount, (int)message->mMachineIndex);
    for (int m = 0; m < 4 && m < message->mMachineCount && n > 0 && n < (int)sizeof text - 24; ++m)
        n += snprintf(text + n, sizeof text - n, " m%d=[%d,%d]", m, (int)message->mPlayerSides.mData[m][0], (int)message->mPlayerSides.mData[m][1]);
    LAN_LOG("%s\n", text);
}
}  // namespace

bool NpLanBeginTeamDraft(NetMessageDraft* message) {
    g_agreedDelay = 0;   // a new session or rematch round: the game-start message sets it again
    if (SessionMode() != 1 || !message) return true;
    const int count = message->mMachineCount;
    NpMpDiag::Note(NpMpDiag::kPeers, count);
    if (count >= 2) NpMpDiag::NotePeerBuild(true, NpLanFingerprint(), NpLanRules());   // (mpbeta27) the partner's frames passed the link's fingerprint check: the same build
    if (RosterFits(message, count)) {
        LogSides(message, "given");
        return true;
    }
    if (count >= 2 && count <= 4) {
        for (int m = 0; m < 4; ++m) {
            const bool present = m < count;
            const s8 side = present ? s8(m & 1) : s8(-1);
            const bool guest = present && message->mEntries[m].mGuestEnabled != 0;
            message->mPlayerSides.mData[m][0] = side;
            message->mPlayerSides.mData[m][1] = guest ? side : s8(-1);
        }
        if (RosterFits(message, count)) {
            LogSides(message, "assigned");
            return true;
        }
    }
    LAN_LOG("invalid draft roster (%d machines, a team would hold more than 3 players or none): the session ends\n", count);
    NpLanLinkLost("invalid draft roster");
    return false;
}

// 1.3 ranked (platform/sdk/dwc/np_dwc_match.cpp): the game's online session over the port's link gets the transport settings the LAN session
// has been played with: the keep-alive expiry (MSC_LAN_KEEPALIVE_MS, 20 s instead of the retail few seconds that a match load outlasts) and
// the input transport (send-now, ticks per message, redundancy). 0 puts the retail values back.
extern "C" void NpLanOnlineSessionTuning(int on) {
    if (on) {
        ApplyKeepAliveLimit();
        ApplyInputTransport();
    } else {
        RestoreKeepAliveLimit();
        RestoreInputTransport();
    }
}

void NpLanSyncSeen(int machine, unsigned tick, unsigned crc, unsigned seed) {
    static const long every = [] {
        const char* e = getenv("MSC_LAN_SYNC_LOG");
        return e && *e ? atol(e) : 100L;
    }();
    static unsigned long messages = 0;
    ++messages;
    if (machine >= 0 && machine < 4) g_endDigest[machine] = {tick, crc, seed, true};
    // Test only (desktop harness runs, never set on a console): MSC_LAN_TEST_SCORE=tick:team adds one goal for a team when the
    // match's own tick counter reaches `tick`, in every match of the session. Every console applies it while it processes the
    // same tick's inputs, so the lockstep simulations stay identical; scripted matches in which nobody scores still end in
    // regulation and reach the results, series and rematch screens instead of sudden death.
    static const unsigned testTick = [] { const char* v = TestEnv("MSC_LAN_TEST_SCORE"); return v ? (unsigned)strtoul(v, 0, 10) : 0u; }();
    static const int testTeam = [] { const char* v = TestEnv("MSC_LAN_TEST_SCORE"); const char* c = v ? strchr(v, ':') : 0; return c && c[1] == '1' ? 1 : 0; }();
    if (testTick && tick == testTick && machine == 0 && g_pTeams[testTeam]) {
        g_pTeams[testTeam]->m_nScore += 1;
        LAN_LOG("test score: team %d -> %d at match tick %u\n", testTeam, g_pTeams[testTeam]->m_nScore, tick);
    }
    // Test only (MSC_LAN_TEST_STALL=tick:ms): this console's game thread sleeps for `ms` once, at match tick `tick`, as it would in a frame
    // that waits for a first-use shader compile; no frames, no keep-alives from this console meanwhile.
    static const unsigned stallTick = [] { const char* v = TestEnv("MSC_LAN_TEST_STALL"); return v ? (unsigned)strtoul(v, 0, 10) : 0u; }();
    static const unsigned stallMs = [] { const char* v = TestEnv("MSC_LAN_TEST_STALL"); const char* c = v ? strchr(v, ':') : 0; return c ? (unsigned)strtoul(c + 1, 0, 10) : 0u; }();
    static bool stalled = false;
    if (stallTick && !stalled && tick == stallTick && machine == 0) {
        stalled = true;
        LAN_LOG("test stall: %u ms at match tick %u\n", stallMs, tick);
        usleep(stallMs * 1000u);
    }
    // Test only (MSC_LAN_TEST_LINKDOWN=tick): this console's link is declared lost at match tick `tick` (what the Vita's Wi-Fi / cell watch
    // does when the radio goes): every send fails from then on and the game's own socket-error path must end the session at once.
    static const unsigned downTick = [] { const char* v = TestEnv("MSC_LAN_TEST_LINKDOWN"); return v ? (unsigned)strtoul(v, 0, 10) : 0u; }();
    static bool downDone = false;
    if (downTick && !downDone && tick == downTick && machine == 0) {
        downDone = true;
        NpLanLinkLost("test: MSC_LAN_TEST_LINKDOWN");
    }
    // Test only (MSC_LAN_TEST_RESUME=tick:ms): as MSC_LAN_TEST_STALL, then the platform's resume notice (NpLanOnResume), as after a sleep.
    static const unsigned resumeTick = [] { const char* v = TestEnv("MSC_LAN_TEST_RESUME"); return v ? (unsigned)strtoul(v, 0, 10) : 0u; }();
    static const unsigned resumeMs = [] { const char* v = TestEnv("MSC_LAN_TEST_RESUME"); const char* c = v ? strchr(v, ':') : 0; return c ? (unsigned)strtoul(c + 1, 0, 10) : 0u; }();
    static bool resumed = false;
    if (resumeTick && !resumed && tick == resumeTick && machine == 0) {
        resumed = true;
        LAN_LOG("test sleep: %u ms at match tick %u, then the resume notice\n", resumeMs, tick);
        if (TestEnv("MSC_LAN_TEST_SUSPEND_NOTICE")) NpLanOnSuspend();   // the platform got the suspend notice in time
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

// ---- extended lockstep checksum (STR-151) ----------------------------------------------------------------------------------------------
// The game's own CalculateChecksum (simulation time, game object state / ball in net / last scorer, the ball's physics and state, each team's meters and
// its ten players' movement, actions and goalie state) is what the lockstep compares every tick, together with the random seed. It does not read the score,
// who owns the ball, the game clock, the Mega Strike progress, the power-ups held or used, the players' tangibility, or the timers that gate pick-ups, so
// two consoles could disagree about any of those and play on until the difference reached a field that is checked. In a LAN match the checksum now also
// folds in those fields (NpLanChecksumExtra, called from FixedUpdateTask::CalculateChecksum; single-player and recorded games are untouched). Both consoles of
// a pair run this build (the fingerprint says so), so the value they compare is still equal on equal states. MSC_LAN_DIGEST_EXT=0 turns the extension off
// (a pair of the previous behaviour for A/B runs: then the fingerprint should be overridden too).
// NOT folded: what the consoles exchange outside the lockstep and so legitimately differ in for a few ticks (the Mega Strike ready flags, the cleanup flag,
// the meter phase timers: msg NetMessageMegaStrikeMeter / MegaBallPointer arrive when they arrive; see multiplayer.md on the meter sections).
// The random-number state (gNetworkRandomSeed) is compared by the router separately, every 4th tick, next to the checksum; it is folded in here as well so
// that a diagnostic that only prints the checksum sees it.
namespace {
bool DigestExt() {
    static const bool on = EnvInt("MSC_LAN_DIGEST_EXT", 1) != 0;
    return on && !g_digestExtSuspended;
}
int CharIdx(const cPlayer* p) { return p ? p->m_nCharacterIndex : -1; }
// MSC_LAN_DIGEST_TRACE=1 (tests): every value folded in is printed ("[mp-dig] n=<index> <hex>") while the lockstep tick is the first one it saw, so that two
// consoles' lines can be diffed to find the field that differs.
// The folded values of the last few ticks are kept (name and low 32 bits) and written out when a DESYNC is reported (DumpDigestRing, from NpLanLog):
// two consoles' dumps are diffed to find the field that differs.
const int kRingTicks = 6, kRingFields = 240;
struct Ring {
    int tick[kRingTicks];
    unsigned value[kRingTicks][kRingFields];
    int count[kRingTicks];
    const char* name[kRingFields];
    int head;
} g_ring;
struct Fold {
    RunningChecksum* c;
    int n;
    bool trace;
    template <class T> void Raw(const char* name, const T& v) {
        c->ChecksumData(&v, sizeof(T));
        unsigned low = 0;
        memcpy(&low, &v, sizeof(T) < 4 ? sizeof(T) : 4);
        if (n < kRingFields) {
            g_ring.value[g_ring.head][n] = low;
            g_ring.name[n] = name;
        }
        if (trace) fprintf(stderr, "[mp-dig] n=%d %s %08x\n", n, name, low);
        ++n;
    }
    template <class T> void operator()(const char* name, const T& v) { Raw(name, v); }
    void Idx(const char* name, const cPlayer* p) { const int i = CharIdx(p); Raw(name, i); }
    void T_(const char* name, const Timer& t) { Raw(name, t.m_uPackedTime); }
};
void DumpDigestRing() {
    for (int k = 1; k <= kRingTicks; ++k) {
        const int slot = (g_ring.head - k + 2 * kRingTicks) % kRingTicks;
        if (g_ring.count[slot] <= 0) continue;
        for (int i = 0; i < g_ring.count[slot] && i < kRingFields; ++i)
            fprintf(stderr, "[mp-dig] tick=%d n=%d %s %08x\n", g_ring.tick[slot], i, g_ring.name[i] ? g_ring.name[i] : "?", g_ring.value[slot][i]);
    }
}
}  // namespace

void NpLanChecksumExtra(RunningChecksum* checksum) {
    if (!checksum || !DigestExt() || !NpLanActive()) return;
    static const int traceTick = EnvInt("MSC_LAN_DIGEST_TRACE", -1);
    Fold f = {checksum, 0, traceTick >= 0 && GetFixedUpdateTask()->GetFrame() == traceTick};
    if (g_pGame) {
        f("g_pGame->m_fGameDuration", g_pGame->m_fGameDuration);
        f("g_pGame->m_uMegastrikeNumShots", g_pGame->m_uMegastrikeNumShots);
        f("g_pGame->m_uMegastrikeCurShot", g_pGame->m_uMegastrikeCurShot);
        f("g_pGame->m_uMegastrikeGoals", g_pGame->m_uMegastrikeGoals);
        f("g_pGame->m_uMegastrikeResults", g_pGame->m_uMegastrikeResults);
        f("g_pGame->mbCaptainShotToScoreOn", g_pGame->mbCaptainShotToScoreOn);
        f.Idx("g_pGame->mpMegaStrikeShooter", g_pGame->mpMegaStrikeShooter);
        f.Idx("g_pGame->m_pScorer", g_pGame->m_pScorer);
        f.Idx("g_pGame->m_pAssister", g_pGame->m_pAssister);
        f.Idx("g_pGame->m_pTeamTouch[0]", g_pGame->m_pTeamTouch[0]);
        f.Idx("g_pGame->m_pTeamTouch[1]", g_pGame->m_pTeamTouch[1]);
        if (const Clock* clock = g_pGame->m_pGameClock) {
            f("clock->m_fTimer", clock->m_fTimer);
            f("clock->m_fEndTime", clock->m_fEndTime);
            f("clock->m_fTimeScale", clock->m_fTimeScale);
            f("clock->m_clockState", clock->m_clockState);
        }
    }
    if (g_pBall) {
        f.Idx("g_pBall->m_pOwner", g_pBall->m_pOwner);
        f.Idx("g_pBall->m_pPrevOwner", g_pBall->m_pPrevOwner);
        f.Idx("g_pBall->m_pLastTouch", g_pBall->m_pLastTouch);
        f.Idx("g_pBall->m_pPassTarget", g_pBall->m_pPassTarget);
        f.Idx("g_pBall->m_pShooter", g_pBall->m_pShooter);
        f.Idx("g_pBall->mpDamageTarget", g_pBall->mpDamageTarget);
        f("g_pBall->mePrevBallState", g_pBall->mePrevBallState);
        f("g_pBall->mbBallOnFire", g_pBall->mbBallOnFire);
        f("g_pBall->mbBallFrozen", g_pBall->mbBallFrozen);
        f("g_pBall->m_iConsecutiveVolleyPasses", g_pBall->m_iConsecutiveVolleyPasses);
        f("g_pBall->mnShotClockTeam", g_pBall->mnShotClockTeam);
        f("g_pBall->m_uGoalType", g_pBall->m_uGoalType);
        f("g_pBall->m_bBallPathChangeCount", g_pBall->m_bBallPathChangeCount);
        f("g_pBall->m_bBallDeflectCount", g_pBall->m_bBallDeflectCount);
        f.T_("g_pBall->m_tShotTimer", g_pBall->m_tShotTimer);
        f.T_("g_pBall->m_tLightningTimer", g_pBall->m_tLightningTimer);
        f.T_("g_pBall->m_tNoPickupTimer", g_pBall->m_tNoPickupTimer);
        f.T_("g_pBall->m_tPassTargetTimer", g_pBall->m_tPassTargetTimer);
        f.T_("g_pBall->mtShotClockTimer", g_pBall->mtShotClockTimer);
    }
    for (int t = 0; t < 2; ++t) {
        const cTeam* team = g_pTeams[t];
        if (!team) continue;
        f("team->m_nSide", team->m_nSide);
        f("team->m_nScore", team->m_nScore);
        f("team->m_nCurrentPowerUp", team->m_nCurrentPowerUp);
        for (int k = 0; k < 2; ++k) {   // field by field: the struct has padding bytes that are never written
            f("team->m_ePowerupList[k].eType", team->m_ePowerupList[k].eType);
            f("team->m_ePowerupList[k].nnumOfPowerups", team->m_ePowerupList[k].nnumOfPowerups);
            // bIsNew is not folded: only the HUD reads it, and the HUD clears it when its flare animation ends, which follows each
            // console's own drawn frames, not the lockstep tick (a gate pair showed it differ at tick 1932 with identical play).
        }
        for (int i = 0; i < 5; ++i) {
            const cPlayer* p = team->m_pPlayers[i];
            if (!p) continue;
            f("p->m_nCharacterIndex", p->m_nCharacterIndex);
            const int hasBall = p->m_pBall != 0;
            f("hasBall", hasBall);
            f.T_("p->m_DetPlayer.m_tNoPickupTimer", p->m_DetPlayer.m_tNoPickupTimer);
            f.T_("p->m_DetPlayer.m_tFireTimer", p->m_DetPlayer.m_tFireTimer);
            f.T_("p->m_DetPlayer.m_tInactivityTimer", p->m_DetPlayer.m_tInactivityTimer);
            f.T_("p->m_DetPlayer.m_tSlideAttackTimer", p->m_DetPlayer.m_tSlideAttackTimer);
            if (p->m_eClassType == FIELDER) {
                const cFielder* fielder = static_cast<const cFielder*>(p);
                f("fielder->mbTangible", fielder->mbTangible);
            }
        }
    }
    f("gNetworkRandomSeed", gNetworkRandomSeed);
    g_ring.count[g_ring.head] = f.n;
    g_ring.tick[g_ring.head] = GetFixedUpdateTask()->GetFrame();
    g_ring.head = (g_ring.head + 1) % kRingTicks;
}

namespace {
// ---- tests of the digest (desktop and emulator runs only, never set on a console) --------------------------------------------------------
// MSC_LAN_TEST_MUTATE=<tick>:<field> on ONE console of a pair: at simulation step `tick` that console changes one piece of its own state that the retail
// checksum does not read; the other console must report DESYNC within a few ticks (the router compares the checksum every tick it receives one) and the
// match ends with the sync-error popup. field: score, owner (the ball's last-touch player), clock, powerup, tangible, megastrike, rng (the retail seed check),
// timer (the ball's pick-up lockout).
// MSC_LAN_TEST_DIGEST=1 (one console is enough): at the first step after tick 300 every field above is changed in turn and put back, and the run says whether
// the retail checksum and the extended checksum noticed ("[mp-lan] digest test: field retail=same|changed extended=same|changed").
struct Mutation {
    const char* name;
    bool (*apply)(bool undo);
};
int g_savedInt[4];
bool MutScore(bool undo) { if (!g_pTeams[0]) return false; g_pTeams[0]->m_nScore += undo ? -1 : 1; return true; }
bool MutOwner(bool undo) {
    if (!g_pBall || !g_pTeams[0] || !g_pTeams[1]) return false;
    static cPlayer* saved;
    if (!undo) {
        saved = g_pBall->m_pLastTouch;
        g_pBall->m_pLastTouch = g_pTeams[saved == g_pTeams[1]->m_pPlayers[0] ? 0 : 1]->m_pPlayers[0];
    } else g_pBall->m_pLastTouch = saved;
    return true;
}
bool MutClock(bool undo) {
    if (!g_pGame || !g_pGame->m_pGameClock) return false;
    static float saved;
    if (!undo) { saved = g_pGame->m_pGameClock->m_fTimer; g_pGame->m_pGameClock->m_fTimer = saved + 0.5f; }
    else g_pGame->m_pGameClock->m_fTimer = saved;   // restored exactly (adding and subtracting 0.5 can change the last bit)
    return true;
}
bool MutPowerup(bool) { if (!g_pTeams[0]) return false; g_pTeams[0]->m_nCurrentPowerUp ^= 1u; return true; }
bool MutTangible(bool) {
    if (!g_pTeams[0]) return false;
    for (int i = 0; i < 5; ++i)
        if (g_pTeams[0]->m_pPlayers[i] && g_pTeams[0]->m_pPlayers[i]->m_eClassType == FIELDER) {
            cFielder* fielder = static_cast<cFielder*>(g_pTeams[0]->m_pPlayers[i]);
            fielder->mbTangible = !fielder->mbTangible;
            return true;
        }
    return false;
}
bool MutMega(bool) { if (!g_pGame) return false; g_pGame->m_uMegastrikeGoals ^= 1u; return true; }
bool MutRng(bool) { gNetworkRandomSeed ^= 1u; return true; }
bool MutTimer(bool undo) {
    if (!g_pBall) return false;
    static u32 saved;
    if (!undo) { saved = g_pBall->m_tNoPickupTimer.m_uPackedTime; g_pBall->m_tNoPickupTimer.m_uPackedTime = saved ^ 0x10u; }
    else g_pBall->m_tNoPickupTimer.m_uPackedTime = saved;
    return true;
}
const Mutation kMutations[] = {{"score", MutScore}, {"owner", MutOwner}, {"clock", MutClock}, {"powerup", MutPowerup},
                               {"tangible", MutTangible}, {"megastrike", MutMega}, {"rng", MutRng}, {"timer", MutTimer}};

u32 ChecksumWith(bool extended) {
    g_digestExtSuspended = !extended;
    const u32 v = GetFixedUpdateTask()->CalculateChecksum();
    g_digestExtSuspended = false;
    return v;
}

void RunDigestTest() {
    static bool done = false;
    if (done) return;
    done = true;
    const u32 baseRetail = ChecksumWith(false), baseExt = ChecksumWith(true);
    LAN_LOG("digest test: retail checksum %08x, extended %08x (extension %s)\n", baseRetail, baseExt, DigestExt() ? "on" : "off");
    for (const Mutation& m : kMutations) {
        if (!m.apply(false)) { LAN_LOG("digest test: %s: not applicable here\n", m.name); continue; }
        const u32 r = ChecksumWith(false), e = ChecksumWith(true);
        m.apply(true);
        LAN_LOG("digest test: %-10s retail=%s extended=%s\n", m.name, r == baseRetail ? "same" : "changed", e == baseExt ? "same" : "changed");
    }
    const bool restored = ChecksumWith(false) == baseRetail && ChecksumWith(true) == baseExt;
    LAN_LOG("digest test: state restored %s\n", restored ? "yes" : "NO");
}

void RunMutationTest() {
    static const char* spec = TestEnv("MSC_LAN_TEST_MUTATE");
    static bool done = false;
    if (!spec || done) return;
    const unsigned tick = (unsigned)strtoul(spec, 0, 10);
    const char* colon = strchr(spec, ':');
    if (!colon || (unsigned)GetFixedUpdateTask()->GetFrame() < tick) return;
    done = true;
    for (const Mutation& m : kMutations)
        if (!strcmp(m.name, colon + 1)) {
            const bool ok = m.apply(false);
            LAN_LOG("test mutation: %s %s at step %d on this console only\n", m.name, ok ? "applied" : "not applicable", GetFixedUpdateTask()->GetFrame());
            return;
        }
    LAN_LOG("test mutation: unknown field '%s'\n", colon + 1);
}
}  // namespace

// ---- incompatible peers (STR-146) --------------------------------------------------------------------------------------------------------
// A console with another build or rule set is refused by the link (fingerprint, rule byte in the header) and is simply never seen by the lobby. When the
// join then finds no game, say in the log which rules the refused console runs, so that a pair that "does not find each other" is explained.
void NpLanJoinFoundNoGame() {
    VLink::Incompatible inc;
    g_noGamesText[0] = 0;
    if (!VLink::Default().GetIncompatible(&inc)) return;
    char theirs[64], ours[64];
    NpLanRulesText(inc.rules, theirs, sizeof(theirs));
    NpLanRulesText(NpLanRules(), ours, sizeof(ours));
    const bool rules = inc.rules != NpLanRules();
    NpMpDiag::NotePeerBuild(false, inc.fingerprint, inc.rules);   // (mpbeta27) for the report's [peers] line
    LAN_LOG_LIMITED("no game to join, but %llu frame(s) from %u.%u.%u.%u were refused: %s (theirs: %s; ours: %s)\n",
            (unsigned long long)inc.count, inc.srcVip >> 24, (inc.srcVip >> 16) & 255, (inc.srcVip >> 8) & 255, inc.srcVip & 255,
            rules ? "different rules" : "different version", theirs, ours);
    // ... and tell the player, in one plain sentence, in the popup that follows (NpLanNoGamesText)
    if (rules) snprintf(g_noGamesText, sizeof g_noGamesText, NpText::Tr("Different rules (theirs: %s, ours: %s). Both players need the same settings."), theirs, ours);   // (1.3: np_text.h)
    else snprintf(g_noGamesText, sizeof g_noGamesText, "Different version: the other player has another build of the game. Both need the same one.%s",
                  NpMpBeta::On() ? " Update both Vitas to the latest version." : "");   // (mpbeta27) what to do about it
}

const char* NpLanNoGamesText() { return g_noGamesText; }

// (mpbeta27) InputRouter's compare of the consoles' sync values failed: the first of each kind goes to the report with both values. Logging only: nothing flows back.
void NpLanNoteMismatch(int what, int tick, unsigned ours, unsigned theirs) {
    if (SessionMode() != 1) return;
    LAN_LOG("sync mismatch (%s) at network tick %d: ours %08x, theirs %08x\n", what == 1 ? "frame" : what == 2 ? "checksum" : "seed", tick, ours, theirs);
    NpMpDiag::NoteMismatch(what, tick, ours, theirs);
}
extern "C" int NpLanKeepAliveLimitMs(void) { return SessionMode() == 1 ? int(s_nExpireKeepAliveMS) : -1; }
// (mpbeta27) the link refused frames of a console with another build or rules since it started (np_menu_flow.cpp: the "update both Vitas" sentence)
extern "C" int NpLanPartnerRefused(void) {
    VLink::Incompatible inc;
    if (!VLink::Default().GetIncompatible(&inc) || !inc.count) return 0;
    NpMpDiag::NotePeerBuild(false, inc.fingerprint, inc.rules);
    return 1;
}
extern "C" int NpLanAgreedDelayTicks(void) { return g_agreedDelay; }

// ---- agreed input delay (A7) -------------------------------------------------------------------------------------------------------------
// See np_lan.h. The host's choice, in ticks; everyone stores what the game-start message carried.
int NpLanChooseInputDelay() {
    if (!g_lanFlow || SessionMode() != 1) return 0;
    const char* v = getenv("MSC_LAN_INPUT_DELAY");
    if (!v || !*v) return 0;                                  // default: the game's own delays (start 4.0, floor 2.0 ticks; congestion raises it)
    int ticks = 0;
    if (!strcmp(v, "auto")) {
        // Internet play: the transport's own once-a-second ping (Peer::RttMs, worst member) is the better number than the game's reliable-socket
        // estimate over the lobby traffic, and the internet floor is 3 ticks (plan A7). LAN / ad hoc: the reliable socket's, as before.
        const bool online = NpNetActive() != 0;
        int rtt = online ? NpNetRttMs() : -1;
        if (rtt < 0) rtt = RoundTripMs();
        if (rtt < 0) {
            LAN_LOG("input delay: auto, but no round trip measured yet; the game's default stays\n");
            return 0;
        }
        const float tickMs = g_fSimulationTick * 1000.0f;
        float jitterMs = rtt / 2 > 10 ? float(rtt / 2) : 10.0f;   // a margin, not a measurement: the transport keeps one smoothed round trip
        if (online && NpNetJitterMs() >= 0 && 2.0f * float(NpNetJitterMs()) > jitterMs) jitterMs = 2.0f * float(NpNetJitterMs());   // a measured spread wins when it is larger
        ticks = 1 + int((float(rtt) / 2.0f + jitterMs) / tickMs + 0.999f);
        const int floorTicks = online ? 3 : 2;
        if (ticks < floorTicks) ticks = floorTicks;
        LAN_LOG("input delay: auto from a %d ms round trip (%s, tick %.1f ms, margin %.0f ms): %d tick(s)\n", rtt, online ? "internet transport" : "lobby traffic", tickMs, jitterMs, ticks);
        NpMpDiag::Note(NpMpDiag::kInputDelay, ticks, rtt, 1);
    } else {
        ticks = atoi(v);
        LAN_LOG("input delay: fixed %d tick(s) by MSC_LAN_INPUT_DELAY\n", ticks);
    }
    return ticks < 1 ? 0 : (ticks > 20 ? 20 : ticks);
}
void NpLanSetAgreedInputDelay(int ticks) {
    g_agreedDelay = (SessionMode() == 1 && ticks >= 1 && ticks <= 20) ? ticks : 0;
    if (g_agreedDelay) LAN_LOG("input delay for this match: %d tick(s) (the host's choice)\n", g_agreedDelay);
    if (g_agreedDelay) NpMpDiag::Note(NpMpDiag::kInputDelay, g_agreedDelay, -1, 0);
    if (SessionMode() == 1) NpMpDiag::Reach(NpMpDiag::kStageInMatch);   // (mpbeta27) StartNetworkedGame on every console: the match starts
}
int NpLanInitialQueueLimit(int fallback) {
    return (g_agreedDelay && SessionMode() == 1) ? g_agreedDelay : fallback;
}
int NpLanAgreedInputDelay() {
    // Test only (MSC_LAN_TEST_NOFLOOR=1 on ONE console of a pair): that console keeps the game's own start and floor, as a build without delay19 would; the pair must then DESYNC (negative control).
    static const bool noFloor = (TestEnv("MSC_LAN_TEST_NOFLOOR") ? atoi(TestEnv("MSC_LAN_TEST_NOFLOOR")) : 0) != 0;
    return (g_agreedDelay && SessionMode() == 1 && !noFloor) ? g_agreedDelay : 0;
}

// ---- starved steps: receive-only drain and bounded retry (delay19) -----------------------------------------------------------------------
// Both only read datagrams that are already there (NetworkSocket::ReceiveOnly: the receive loop, never the transport's Update) and change WHEN a step runs,
// never what it computes. Off by default (MSC_LAN_DRAIN, MSC_LAN_RETRY_US).
namespace { uint64_t g_retryUsedUs = 0; }
void NpLanFrameBegin() {
    g_retryUsedUs = 0;
    if (!NpDelay19::Drain() || SessionMode() != 1 || !g_pNetworkSession) return;
    NetworkSocket* socket = g_pNetworkSession->GetDirectSocket();
    if (!socket || !socket->mInitialized) return;
    const int n = socket->ReceiveOnly();
    for (NetWindow* w : {&g_win, &g_match}) { ++w->drainRuns; w->drainDgrams += unsigned(n); }
}
bool NpLanRetryWait(InputRouter* r) {
    const int budget = NpDelay19::RetryUs();
    if (budget <= 0 || SessionMode() != 1 || !g_pNetworkSession || !r) return false;
    if (NpSimtEnabled && NpSimtEnabled() > 0) return false;
    if (g_retryUsedUs >= uint64_t(budget)) return false;
    NetworkSocket* socket = g_pNetworkSession->GetDirectSocket();
    if (!socket || !socket->mInitialized) return false;
    const uint64_t t0 = NowUs();
    bool ok = false;
    for (;;) {
        socket->ReceiveOnly();
        if (r->HasInput()) { ok = true; break; }
        const uint64_t spent = NowUs() - t0;
        if (g_retryUsedUs + spent >= uint64_t(budget)) break;
        usleep(100);
    }
    const uint64_t spent = NowUs() - t0;
    g_retryUsedUs += spent;
    for (NetWindow* w : {&g_win, &g_match}) { ++w->retryTries; if (ok) ++w->retryOk; w->retryUs += unsigned(spent); }
    return ok;
}

// ---- wave 14 (wake14): the phase label of the link's quiet-gap diagnostic (MSC_LINK_GAPLOG, vlink.h SetGapLog) ----------------------------------------------
// "st<n>" is the session state (3 pre-start, 4 loading, 5 in game, 6 game end; 0 none: a lobby before a game), in game " sim" when a simulation tick ran in the last 100 ms
// and " nosim" when not (a cut-scene, a pause, a replay, a load), "sc<n>" the last scene pushed. Reads only; any call outside a session gives "none".
extern "C" const char* NpLanGapPhase(void) {
    static char text[40];
    if (!g_pNetworkSession) return "none";
    const int state = g_pNetworkSession->GetSessionState();
    const bool sim = state == 5 && g_win.lastStepUs && NowUs() - g_win.lastStepUs < 100000u;
    snprintf(text, sizeof text, "st%d%s sc%d", state, state == 5 ? (sim ? " sim" : " nosim") : "", g_lastScene);
    return text;
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

namespace { NpWait::StarveClock g_starveClock; }   // "Waiting for the other player..." (np_wait_banner.h): render bookkeeping, nothing reads it but the banner

unsigned NpLanWaitedMs() { return g_starveClock.Ms(uint32_t(NowUs() / 1000u) + 1u); }

void NpLanStep(bool starved) {
    if (SessionMode() != 1 || !gNetworkInputRouter) return;
    // Test only (MSC_LAN_TEST_SLOWSTEP=us[:machine[:fromTick]]): every simulation step of that console costs `us` more, as a console whose frames cannot keep 50 steps a
    // second would. The lockstep must slow BOTH consoles to its rate (never one side only); tools/mp/lan_speed.py reads the two "[mp-lan] net" lines. Not in a quiet release build.
    static const long slowUs = [] { const char* v = TestEnv("MSC_LAN_TEST_SLOWSTEP"); return v ? atol(v) : 0L; }();
    // (mpbeta27) test only (MSC_LAN_TEST_KILL_AT=<step> on ONE console; never read by a quiet release build): the process dies at that step without a word, as a crash or
    // a pulled battery would. The partner must end the match with one clean "connection lost"; this console's crash marker (mp-session.txt) stays for its next start.
    static const long killAt = [] { const char* v = TestEnv("MSC_LAN_TEST_KILL_AT"); return v && *v ? atol(v) : -1L; }();
    if (killAt >= 0 && long(GetFixedUpdateTask()->GetFrame()) >= killAt) {
        fprintf(stderr, "[mp-lan] test kill: this console dies at step %d\n", GetFixedUpdateTask()->GetFrame());
        fflush(stderr);
        _Exit(9);
    }
    if (slowUs > 0 && !starved) {
        static const int slowMachine = [] { const char* v = TestEnv("MSC_LAN_TEST_SLOWSTEP"); const char* c = v ? strchr(v, ':') : 0; return c ? atoi(c + 1) : 0; }();
        static const long slowFrom = [] { const char* v = TestEnv("MSC_LAN_TEST_SLOWSTEP"); const char* c = v ? strchr(v, ':') : 0; c = c ? strchr(c + 1, ':') : 0; return c ? atol(c + 1) : 0L; }();
        const int local = gNetworkInputRouter->mSession ? gNetworkInputRouter->mSession->GetLocalMachineId() : 0;
        if (local == slowMachine && long(GetFixedUpdateTask()->GetFrame()) >= slowFrom) usleep(useconds_t(slowUs));
    }
    {
        const uint32_t nowMs1 = uint32_t(NowUs() / 1000u) + 1u;
        if (!starved) {   // (mpbeta27) a run of starved steps ended: the report keeps the longest (an event; two loads otherwise)
            const uint32_t waited = g_starveClock.Ms(nowMs1);
            if (waited >= 200) NpMpDiag::NoteWait(waited);
        }
        g_starveClock.Step(starved, nowMs1);
    }
    if (!starved && g_pBall && g_pTeams[0] && g_pTeams[1]) {
        static const bool digestTest = (TestEnv("MSC_LAN_TEST_DIGEST") ? atoi(TestEnv("MSC_LAN_TEST_DIGEST")) : 0) != 0;
        if (digestTest && GetFixedUpdateTask()->GetFrame() > 300) RunDigestTest();
        RunMutationTest();
        g_endScore[0] = g_pTeams[0]->m_nScore;
        g_endScore[1] = g_pTeams[1]->m_nScore;
    }
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
    // internet play: tell the members, close the stale socket; the next session start rebuilds it (online/np_online.cpp). Also while the
    // menu is still waiting for the connection (no LAN session yet).
    NpOnlineSave::NoteError();   // an online session that ends from here saves nothing (np_online_save.h)
    NpNetOnLinkLost(why);
    NpMpDiag::Note(NpMpDiag::kLinkLost, 0, 0, 0, why ? why : "?");
    if (!g_lanFlow && SessionMode() == 0) return;
    LAN_LOG("link lost (%s): the session ends at the next send, with the game's own message\n", why ? why : "?");
    NpMenuLinkLostNote();   // internet play: the lobby that follows offers to reconnect to the same room (np_menu_flow.cpp)
    if (!g_linkLostUs) g_linkLostUs = NowUs();
    VLink::Default().SetLost(true, why);
}

// The menu's RECONNECT rebuilt the online link in the lobby (np_menu_flow.cpp): the loss latch set by NpLanLinkLost is cleared, otherwise every send of the
// session the players create next would fail ("FAILED TO JOIN GAME"). The latch is otherwise cleared only when a LAN session starts (NpLanStart).
extern "C" void NpLanLinkRestored(void) {
    VLink::Default().SetLost(false, 0);
    g_linkLostUs = 0;
}

// A10: a console that is about to sleep (when the platform is told in time: the Vita's power callback sometimes delivers a suspend notice) says goodbye
// the transport's own way before the radio goes: the partner leaves at once with the game's "connection lost" instead of starving for the 20 s
// keep-alive limit. MSC_LAN_LINKWATCH=0 turns it off with the rest of the watch. The resume side (below) ends the session on this console.
extern "C" void NpLanOnSuspend(void) {
    if (!g_lanFlow || SessionMode() != 1) return;
    static const bool on = EnvInt("MSC_LAN_LINKWATCH", 1) != 0;
    if (!on) return;
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
        socket->mReliableSocket.FlushOutgoing();   // out now: the console may be asleep before the next frame
    }
    g_suspendSaidBye = true;
    LAN_LOG("suspending in a LAN session: said goodbye on %d connection(s)\n", closing);
    NpMpDiag::Note(NpMpDiag::kSuspend);
}

extern "C" void NpLanOnResume(void) {
    if (!g_lanFlow) {
        // Internet play before the lobby (hosting, waiting for the partner, punching): the socket and the NAT mapping did not survive either. The menu's connect
        // step then ends with a sentence (phase LOST); without this it waited on a dead socket until its time limits ran out.
        if (NpNetActive()) NpLanLinkLost("resumed from sleep");
        return;
    }
    NpMpDiag::Note(NpMpDiag::kResume, 0);
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
    if (NpNetActive()) {
        // Internet play: the NAT mappings and perhaps the address did not survive the sleep. The members are told (a close message through the
        // old socket, which may still work) and the session ends at once; the lobby rebuilds the link (STUN, punching) when it starts over.
        LAN_LOG("resumed from sleep in an online session: the link is closed and rebuilt\n");
        NpLanLinkLost("resumed from sleep");
        return;
    }
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
    LAN_LOG("resumed from sleep in a LAN session (%s): closing %d connection(s); the link ended because the console slept\n",
            g_suspendSaidBye ? "the partner was told before the sleep" : "the partner was left waiting, the radio may be gone", closing);
    g_suspendSaidBye = false;
    if (!closing) NpLanLinkLost("resumed from sleep, no connection to close");
}

void NpLanLog(const char* format, ...) {
    {   // the diagnostics ring (np_mpdiag.h) records the lines that mean something, quiet builds included: it needs no log and no allocation
        va_list ring;
        va_start(ring, format);
        NpMpDiag::NoteGameLog(format, ring);
        va_end(ring);
        va_list tail;
        va_start(tail, format);
        NpMpDiag::NoteLogLine(format, tail);   // the log tail of SEND A REPORT (np_mpdiag.h)
        va_end(tail);
    }
    if (!strncmp(format, "DESYNC", 6) || !strncmp(format, "connection lost", 15)) NpOnlineSave::NoteError();   // before the quiet-log and session guards: the latch must not depend on logging
    if (!NpLanActive() || g_quietLog) return;
    if (!strncmp(format, "DESYNC", 6)) DumpDigestRing();   // the extended checksum's fields of the last ticks, to be diffed against the other console's log
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
int NpLanRadioPoll(int request) {
    const int online = NpNetRadioPoll(request);   // internet play selected (MSC_NET_MODE or the menu's NpNetHost/NpNetJoin)
    return online == -2 ? 2 : online;
}
void NpLanRadioStop() { NpNetRadioStop(); }
const char* NpLanPlayerName() { return getenv("MSC_LINK_NAME"); }
#endif
