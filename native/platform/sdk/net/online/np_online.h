// OnlineNet: the controller of the internet transport. One object per console; it owns the UDP socket and runs, in this order,
//   resolve the STUN servers (worker thread) -> STUN from the game's own socket -> signaling room (host / join by invite code)
//   -> publish our candidates -> hole punching with every member -> encrypted sessions (np_peer.cpp)
// all driven by Tick() (once a frame on the game thread; nothing here blocks). The VLink backend (vlink_online.cpp) is a thin
// adapter over SendFrame/RecvFrame; the game's menu talks to the C API at the end of this file through NpNetHost/NpNetJoin.
//
// A link that dropped (Wi-Fi lost, resume from sleep, the other side leaving) is closed and rebuilt by OnLinkStart(): the next time the
// game's session layer starts its link (the tested lobby path of STR-155), the socket is reopened, STUN repeated, the candidates are
// published again (generation + 1) and every member punches afresh with new hellos and keys.
#ifndef NP_NET_ONLINE_CTL_H
#define NP_NET_ONLINE_CTL_H

#include "platform/sdk/net/online/np_fault.h"
#include "platform/sdk/net/online/np_net.h"
#include "platform/sdk/net/online/np_peer.h"
#include "platform/sdk/net/online/np_signal.h"
#include "platform/sdk/net/online/np_stun.h"
#include "platform/sdk/net/online/np_turn.h"

namespace NpNet {

struct OnlineConfig {
    char stun[256];                 // "host:port,host:port"; empty = the default
    uint16_t port;                  // local UDP port (25110, kDefaultPort; the real one is Udp::LocalPort())
    PunchConfig punch;
    StunConfig stunTiming;
    uint32_t signalPollMs;          // how often the signaling object is polled (the file fake reads files)
    // injection (tests); null = the platform's
    Udp* udp;                       // owned by OnlineNet afterwards
    Signal* signal;                 // owned by OnlineNet afterwards
    Env env;                        // clock / random; udp is filled in by OnlineNet
    Addr stunAddrs[kMaxStunServers];   // pre-resolved servers (no DNS)
    int nStunAddrs;
    bool forceTryLocal;             // probe the peers' local candidates even when the public addresses differ
    FaultConfig fault;              // loss / dup / delay / jitter injected into this console's sends (A12; MSC_NET_DROP ...)
    // Time limits of the bring-up stages, on the transport's clock (V5: every wait has a limit, and a sentence when it runs out; 0 = none).
    // Cancelling is NpNetLeave() at any time: nothing blocks.
    uint32_t resolveLimitMs;        // DNS of the STUN servers; on timeout the session goes on without STUN (same-network paths only)
    uint32_t roomLimitMs;           // signaling: the room (create / join) must be there by then
    uint32_t hostWaitLimitMs;       // in the room with nobody connected (the host waiting for a friend)
    uint32_t rebuildLimitMs;        // after a lost link: back to READY within this, counted from OnLinkStart()
    uint32_t statsLogMs;            // link quality log line interval while connected (0 = off)
    uint32_t refreshMs;             // while nobody is connected: STUN again and republish this often (a NAT mapping lives ~30 s; the server marks older endpoints stale)
    bool honorSteer;                // the service says "use the relay" (relay-only profile, symmetric NAT, failures of this pair): get credentials and allocate at once; direct probing is skipped where it cannot work
    // ---- the relay (TURN) path, np_turn.h ----
    bool relayEnabled;              // false: no relay at all (tests, MSC_NET_RELAY=0): a failed punch ends in RELAY_NEEDED as before
    bool relayOnly;                 // "Connection: Relay only" (MSC_NET_RELAY_ONLY=1): no STUN, no raw address ever leaves this console, only the relayed one is published
    TurnConfig turn;
    Addr turnAddrs[4];              // tests / MSC_NET_TURN_ADDR: the relay servers to use instead of resolving the credentials' URLs
    int nTurnAddrs;
    uint32_t relayLimitMs;          // credentials (and DNS) must be there within this once the relay is wanted; 0 = none
    uint32_t relayWaitMs;           // extra wait for the partner's relay address / channel / confirmation after punching ends; 0 = no extra wait
    int maxPacketsPerPump;          // datagrams handled per PumpSocket call (the work per frame is bounded)
    // called once when a connected peer is gone (lost, closed): the glue declares the VLink lost and the game ends its session
    void (*onLost)(void* ctx, const char* why);
    void* onLostCtx;
    // called once when the session goes on but degraded in a way the player should hear about at once (the signaling service failed while a link
    // was up: the match continues, no new room or reconnect is possible). The same text is in OnlineInfo::warning.
    void (*onNotice)(void* ctx, const char* text);
    void* onNoticeCtx;
    OnlineConfig() : port(kDefaultPort), signalPollMs(100), udp(0), signal(0), nStunAddrs(0), forceTryLocal(false), resolveLimitMs(8000), roomLimitMs(15000),
          hostWaitLimitMs(600000), rebuildLimitMs(45000), statsLogMs(5000), refreshMs(20000), honorSteer(true), relayEnabled(true), relayOnly(false), nTurnAddrs(0), relayLimitMs(20000), relayWaitMs(28000), maxPacketsPerPump(64), onLost(0), onLostCtx(0), onNotice(0), onNoticeCtx(0) {
        stun[0] = 0;
    }
};

extern const char kDefaultStunServers[];   // "stun.cloudflare.com:3478,stun.cloudflare.com:53"

struct PeerInfo {
    bool present;
    int slot;
    char name[16];
    int state;          // Peer::State
    int pathKind;       // CandidateKind of the path in use (valid when state == CONNECTED)
    bool established;
    int nat;
    uint32_t vip;
    uint64_t dataSent, dataRecv, dropAuth, dropReplay;
    int rttMs, rttMinMs, jitterMs, lossPermille;   // -1 until measured (the lockstep's input delay, A7)
};

struct OnlineInfo {
    int phase;                  // OnlineNet::Phase
    char invite[16];
    int mySlot;
    uint32_t myVip;
    int myNat;
    Addr publicAddr, localAddr;
    int peerCount;              // other members present in the room
    int connectedCount;
    PeerInfo peers[kMaxSlots];
    char error[96];
    char warning[96];           // the session goes on but degraded (e.g. no STUN: same-network paths only)
    char endReason[96];         // why the last link ended, as a sentence for the toast on return to the lobby ("" if none)
    int relayState;             // 0 not used, 1 being set up, 2 allocated, 3 failed (relayText says why)
    char relayText[200];        // a sentence about the relay: why it is used, or why it failed
    Addr relayedAddr;           // our allocation (zero until allocated)
};

class OnlineNet {
public:
    enum Phase {
        OFF,
        RESOLVING,      // looking up the STUN servers
        STUN,           // binding requests out
        ROOM,           // waiting for the signaling room
        WAITING,        // in the room, nobody else yet (host) or no candidates yet
        PUNCHING,       // at least one member is being punched, none connected yet
        READY,          // at least one member connected
        RELAY_NEEDED,   // every member we could not reach directly needs the relay
        FAILED,         // signaling or the network is unusable (error text in the info)
        LOST            // was connected, the link is gone; OnLinkStart() rebuilds it
    };

    OnlineNet();
    ~OnlineNet();

    // Starts a session: hosts a room (invite = null) or joins one. False when the arguments are unusable.
    bool Start(const OnlineConfig& config, const char* invite, const char* playerName);
    // Leaves the room, tells the members, closes the socket.
    void Stop();
    void Tick();

    Phase GetPhase() const { return phase_; }
    void GetInfo(OnlineInfo* info) const;
    uint32_t MyVip() const;
    int MySlot() const { return mySlot_; }

    // ---- the VLink backend ----------------------------------------------------------------------------------------------
    // Unicast to a member slot, or every connected member (slot < 0). Bytes accepted (silently dropped when nobody is reachable: UDP).
    int SendFrame(int slot, const void* data, size_t len);
    // The next received frame of any member; its slot. 0 when nothing is waiting.
    int RecvFrame(int* slot, void* buffer, size_t capacity);
    // The game's session layer starts its link: rebuild the socket and the paths when they were lost.
    void OnLinkStart();
    // The platform lost the network (Wi-Fi watch) or resumed from sleep: tell the members, close the stale socket.
    void OnLinkLost(const char* why);
    // The relay (TURN) path to a member: bound channel on the relay server. The member is punched again with it as a candidate.
    void BindRelay(int slot, const Addr& relayServer, uint16_t channel);
    // Worst link quality over the connected members (the host picks the input delay from it, A7); -1 when nothing measured yet.
    int WorstRttMs() const;
    int WorstJitterMs() const;
    int WorstLossPermille() const;
    const char* EndReason() const { return endReason_; }

    const StunClient& Stun() const { return stun_; }
    Peer& PeerAt(int slot) { return peers_[slot]; }
    Signal* GetSignal() { return signal_; }

private:
    struct Resolver;
    void BeginStun();
    void CheckResolver();
    void PumpSocket();
    void PublishIfReady();
    void SyncMembers();
    void CheckPartnerLost(uint64_t now);   // before any link is up: a member the service reports lost (15 s without a call) ends the attempt
    void UpdatePhase();
    void CloseSocket();
    void Fail(const char* why);
    void CheckLimits(uint64_t now);
    void ReportPunches();
    void LogStats(uint64_t now);
    void AbandonResolver();
    void RefreshCandidates(uint64_t now);
    bool RoomUsable() const;
    // relay
    void UpdateRelay(uint64_t now);
    void ServiceRelayPaths();
    void RelayFail(const char* why);
    void StartTurnResolve();
    void FinishRelayStart();
    bool CredentialsAlive(uint64_t now) const;
    bool RelayInProgress() const;
    bool WaitingForRelayPath();
    void ReleaseRelay();
    void AbandonTurnResolve();
    bool AnyLinkUp() const;
    void SignalingLost(const char* why);

    OnlineConfig cfg_;
    Env env_;
    Udp* udp_;
    bool ownUdp_;
    Signal* signal_;
    StunClient stun_;
    Peer peers_[kMaxSlots];
    uint32_t peerSig_[kMaxSlots];       // candidate signature a peer was begun with
    bool peerBegun_[kMaxSlots];
    bool wasConnected_[kMaxSlots];
    bool lostReported_[kMaxSlots];
    uint64_t partnerLostSince_[kMaxSlots];   // when the service first reported this member's presence as lost (0 = not lost)
    Phase phase_;
    int mySlot_;
    uint32_t myVip_;
    uint32_t gen_;
    bool host_, started_, published_, stunStarted_, rebuild_;
    char invite_[16], name_[16], error_[96], warning_[96], endReason_[96];
    bool reportedOk_[kMaxSlots], reportedFail_[kMaxSlots];
    bool refreshing_, startedSent_, closedHandled_;
    bool signalLost_;                   // the signaling service failed while a link was up: no more polling, the match goes on
    uint64_t keyWaitSince_;             // a member is waiting for the room's session key (0 = no)
    bool memberSeen_[kMaxSlots];
    uint64_t lastPublishAt_;
    Phase stagePhase_;
    uint64_t stageAt_, rebuildDeadline_, nextStats_;
    uint32_t roomVersion_;
    SigCandidates mine_;
    uint64_t startedAt_, lastTick_, nextSignalPoll_;
    Resolver* resolver_;
    // relay
    enum RelayStage { RL_OFF, RL_CREDS, RL_RESOLVE, RL_ALLOC, RL_READY, RL_FAILED };
    struct TurnResolve;
    TurnClient turn_;
    RelayStage relayStage_;
    bool relayWanted_, rotating_, relayEnabled_, skipDirect_, swapPending_;
    int credRetries_;
    char relayWhy_[64], relayError_[200];
    uint64_t relayStageAt_, credsAt_, usageSent_, swapAt_, rotateRetryAt_;
    TurnInfo creds_, pendingCreds_;   // (pending: new credentials waiting for the allocation of the refused ones to run out)
    bool haveCreds_;
    TurnResolve* turnResolve_;
    Addr relayServers_[4];
    int nRelayServers_;
    uint32_t relayKey_[kMaxSlots];
    uint32_t relayGenSeen_;
    uint64_t relayWaitDeadline_;       // extra wait after punching ends; zero when no wait is active
    Addr stunAddrs_[kMaxStunServers];
    int nStunAddrs_;
    uint8_t rxBuf_[kMaxDatagram + kRelayOverhead];   // one datagram plus the relay header; a longer one is cut here and refused by the peer
};

}  // namespace NpNet

#include "platform/sdk/net/online/np_online_api.h"

// C++ side (the STL stays out of np_online_api.h: the game's headers define their own placement new): progress for the menu, a test
// entry with a full configuration, and the signaling factory the HTTPS client plugs in.
void NpNetGetInfo(NpNet::OnlineInfo* info);
bool NpNetStartWith(const NpNet::OnlineConfig& config, const char* invite, const char* playerName);
NpNet::OnlineNet* NpNetController();
void NpNetSetSignalFactory(NpNet::Signal* (*factory)(void));

#endif  // NP_NET_ONLINE_CTL_H
