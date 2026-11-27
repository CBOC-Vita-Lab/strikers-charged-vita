// Signaling: how two consoles find each other before there is a UDP path between them.
//
// The real implementation is HTTPS to the Cloudflare Worker (server/PROTOCOL.md, written by another worker; it sits on the TLS client
// of platform/sdk/net/np_https). This interface is all the transport needs from it. It is polled and never blocks: an implementation
// does its network work on its own thread and Poll() only moves results over.
//
// The room is the lobby: the host creates it (invite code), friends join with the code. The server decides everything that has to be
// the same everywhere: the slot of each member, its virtual IP (vip: this fixes the collision of two NATed Vitas that both report
// 192.168.1.x), the per-match session key (32 random bytes, the key of np_peer.cpp) and the punch cookie (natneg style). Each member
// publishes its candidates (the public address STUN reported, its local address, its NAT class) and sees everyone else's.
#ifndef NP_NET_SIGNAL_H
#define NP_NET_SIGNAL_H

#include "platform/sdk/net/online/np_net.h"

namespace NpNet {

struct SigCandidates {
    bool valid;
    Addr pub;          // as the STUN servers saw it
    Addr pub2;         // as the second STUN server saw it (differs from `pub` behind a symmetric NAT: lets the server see the port allocation)
    Addr local;        // this console's address on its own network
    uint8_t nat;       // NatClass
    uint32_t gen;      // bumped whenever the console rebuilt its link (new socket, resume from sleep): the other side then punches afresh
    Addr relay;        // the console's TURN allocation (relayed address), zero until it has one; publishing it does not change `gen`
    bool relayOnly;    // privacy mode ("Connection: Relay only"): only `relay` is published, never an address of this console
    SigCandidates() : valid(false), nat(NAT_UNKNOWN), gen(0), relayOnly(false) {}
};

// Short-lived relay (TURN) credentials the service minted for this room (POST /v1/turn). The TURN key itself never reaches the console.
struct TurnInfo {
    char urls[4][160];     // "turn:host:port?transport=udp" (UDP only; the Vita has no TCP), best first
    int nUrls;
    char user[160];
    char pass[160];
    uint32_t ttlSec;       // how long the credentials work from now
    TurnInfo() : nUrls(0), ttlSec(0) { user[0] = pass[0] = 0; memset(urls, 0, sizeof urls); }
};

// What a console reports once its attempt to reach a member ended (A6): the server counts failures per pair and steers to the relay.
struct PunchReport {
    uint8_t peerSlot;
    bool ok;
    uint32_t ms;             // time from the start of the attempt to the verdict
    uint8_t pathKind;        // CandidateKind of the path that worked (ok only)
    uint8_t myNat, peerNat;  // NatClass
    Addr observed;           // the peer's address as seen on the working path (ok only)
    PunchReport() : peerSlot(0), ok(false), ms(0), pathKind(0), myNat(0), peerNat(0) {}
};

struct SigMember {
    bool present;
    uint8_t slot;
    uint32_t vip;          // virtual IPv4 (host order) the lobby assigned; unique in the room
    char name[16];
    SigCandidates cands;
    int link;              // the server's view of the member's presence: 0 ok, 1 quiet (5 s without a call), 2 lost (15 s)
    bool stale;            // the member's endpoint is older than the NAT mapping life: it should run STUN again and republish
    SigMember() : present(false), slot(0), vip(0), link(0), stale(false) { name[0] = 0; }
};

struct SigRoom {
    char invite[16];
    uint32_t cookie;
    uint8_t key[32];        // per-match session key
    int mySlot;
    SigMember members[kMaxSlots];
    uint32_t version;       // changes whenever anything in the room changed
    uint64_t punchAtMs;     // when both sides start punching, on the transport's clock (Signal::SetClock); 0 = at once
    bool steerRelay;        // the server says: skip hole punching, use the relay (relay-only profile, symmetric NAT, failures of this pair)
    char steerReason[24];
    SigRoom() : cookie(0), mySlot(-1), version(0), punchAtMs(0), steerRelay(false) {
        steerReason[0] = 0;
        invite[0] = 0;
        memset(key, 0, sizeof key);
    }
};

class Signal {
public:
    enum Status { IDLE, WORKING, IN_ROOM, FAILED, CLOSED };
    virtual ~Signal() {}
    virtual void Host(const char* playerName) = 0;                       // create a room; asynchronous
    virtual void Join(const char* invite, const char* playerName) = 0;   // join a room by invite code; asynchronous
    virtual void PublishCandidates(const SigCandidates& mine) = 0;       // after STUN (and again whenever they change)
    virtual void ReportPunch(const PunchReport& report) = 0;             // fire and forget
    virtual void Leave() = 0;
    virtual void Poll() = 0;                                              // once a frame
    virtual Status GetStatus() const = 0;
    virtual const SigRoom& Room() const = 0;                              // valid while IN_ROOM
    virtual const char* Error() const = 0;                                // when FAILED
    // Optional (defaults do nothing): the transport's clock, so a server time can be turned into "when to start punching"; the link is confirmed
    // both ways and the match is about to start (a room's TTL and match record); the STUN servers the service recommends ("host:port,host:port").
    virtual void SetClock(uint64_t (*now)(void*), void* ctx) { (void)now; (void)ctx; }
    virtual void Started() {}
    virtual const char* StunList() const { return ""; }

    // ---- relay (TURN) credentials ----------------------------------------------------------------------------------------
    // RequestTurn() asks the service for credentials for this room (only possible once a second member joined); asynchronous, can be called
    // again for fresh ones. TurnState() then goes WORKING -> READY (Turn() valid) or FAILED (TurnError() is a sentence for the player: "the relay budget
    // is used up", ...). ReportTurnUsage() tells the service roughly how many bytes were relayed (its budget); fire and forget.
    enum TurnStatus { TURN_NONE, TURN_WORKING, TURN_READY, TURN_FAILED };
    virtual void SetRelayOnly(bool on) { (void)on; }   // the player's "Connection: Relay only" setting, before Host() / Join(): the service learns it as the profile's relay-only flag
    virtual void RequestTurn() {}
    virtual TurnStatus TurnState() const { return TURN_NONE; }
    virtual const TurnInfo& Turn() const { static TurnInfo none; return none; }
    virtual const char* TurnError() const { return ""; }
    virtual void ReportTurnUsage(uint64_t bytes) { (void)bytes; }
};

// Test fakes (np_signal_fake.cpp, not built for the Vita).
//   SignalMemoryHub: rooms shared by Signal objects of one process.  SignalFileDir: rooms in a directory, shared by processes.
class SignalMemoryHub;
SignalMemoryHub* CreateSignalMemoryHub(uint32_t seed);
void DestroySignalMemoryHub(SignalMemoryHub* hub);
Signal* CreateMemorySignal(SignalMemoryHub* hub);
// The punch reports a hub received (tests), oldest first; the count.
int SignalMemoryHubReports(SignalMemoryHub* hub, PunchReport* out, int max);
Signal* CreateFileSignal(const char* directory);
// Relay credentials of the fakes. They mint what the real service mints: username "<expiry unix seconds>:<id>", credential = base64(HMAC-SHA1(secret,
// username)) (the TURN REST scheme), which the test TURN server (tools/net/turnsim.cpp) checks. `urls` is comma separated. The memory fake mints with
// the transport's clock; `reportedTtl` is what the client is told, `realTtl` what is inside the username (tests make them differ to exercise the
// reactive path), `budget` the number of credential requests that succeed (-1 = unlimited; then "turn_budget" is returned).
void SignalMemoryHubSetTurn(SignalMemoryHub* hub, const char* urls, const char* secret, uint32_t reportedTtlSec, uint32_t realTtlSec, int budget);
int SignalMemoryHubTurnRequests(SignalMemoryHub* hub);
void FakeMintTurn(TurnInfo* out, const char* urls, const char* secret, uint64_t nowMs, uint32_t reportedTtlSec, uint32_t realTtlSec, uint32_t serial);

}  // namespace NpNet

#endif
