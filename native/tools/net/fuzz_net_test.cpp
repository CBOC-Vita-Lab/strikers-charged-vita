// fuzz_net_test.cpp (wave 13, netclient13, item O6): AddressSanitizer + UndefinedBehaviorSanitizer fuzz of the online client's parsers, no sockets, no
// service. Modelled on the VitAdhoc fuzz_nat.c (3.4 million damaged and re-signed datagrams, 0 reports). It builds REAL messages, damages them, and
// re-signs a share of the damaged ones with the right key (a server or peer that lies with a valid signature):
//   STUN   Binding success / error answers (xor-mapped, mapped, unknown attributes, padding)        StunClient::ParseResponse + HandlePacket
//   TURN   401 / 438 with realm and nonce up to 250 bytes, Allocate / Refresh / CreatePermission /
//          ChannelBind answers, errors, Data indications, all with MESSAGE-INTEGRITY under the long-term key       TurnClient::HandlePacket
//   PEER   PUNCH packets with a valid MAC, DATA / CTRL packets sealed with the session key, replays, oversize     Peer::OnPacket / PopFrame / SendFrame
//   JSON   room views, relay credentials, friend lists and errors, with bytes outside printable ASCII             NpNet::Json, NpOnline::JsonParse, SafeCopy
// Every input is copied into an exact-size heap block (ASan sees a read one byte past it) and every output buffer is exact-size too.
// Invariants are checked after every step: bounded lengths, clamped lifetimes, state in range, strings printable (MSC_NET_TEXT_ASCII), sends <= one datagram.
//   fuzz_net_test [--cases N] [--seed S] [--only stun|turn|peer|json|text] [-v]       default 200000 cases (env NP_FUZZ_CASES, NP_FUZZ_SEED)
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "platform/sdk/dwc/np_online_json.h"
#include "platform/sdk/net/online/np_crypto.h"
#include "platform/sdk/net/online/np_json.h"
#include "platform/sdk/net/online/np_peer.h"
#include "platform/sdk/net/online/np_stun.h"
#include "platform/sdk/net/online/np_text_safe.h"
#include "platform/sdk/net/online/np_turn.h"

namespace NpNet {
struct PeerTestAccess {
    static const uint8_t* DataKey(Peer& p) { return p.dataKey_; }
    static const uint8_t* Hello(Peer& p) { return p.hello_; }
    static bool KeysReady(Peer& p) { return p.keysReady_; }
    static size_t RxQueue(Peer& p) { return p.rx_.size(); }
};
}  // namespace NpNet

using namespace NpNet;
typedef std::vector<uint8_t> Bytes;

static unsigned long g_violations = 0;
#define VIOLATION(...)                                                          \
    do {                                                                        \
        ++g_violations;                                                         \
        if (g_violations <= 30) { fprintf(stderr, "VIOLATION %s:%d (case %lu): ", __FILE__, __LINE__, g_caseNo); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } \
    } while (0)
#define EXPECT(cond, ...) do { if (!(cond)) VIOLATION(__VA_ARGS__); } while (0)
static unsigned long g_caseNo = 0;

// ---- randomness (xorshift64*; the seed prints, a failing case number replays with --seed) ------------------------------------
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    uint64_t Next() { s ^= s >> 12; s ^= s << 25; s ^= s >> 27; return s * 0x2545F4914F6CDD1Dull; }
    uint32_t U(uint32_t n) { return n ? uint32_t(Next() >> 33) % n : 0; }   // [0, n)
    bool Chance(unsigned pct) { return U(100) < pct; }
    uint8_t Byte() { return uint8_t(Next() >> 40); }
    void Fill(void* p, size_t n) { uint8_t* b = static_cast<uint8_t*>(p); for (size_t i = 0; i < n; ++i) b[i] = Byte(); }
};

// ---- the environment: virtual clock, deterministic random, a UDP that records what is sent --------------------------------------
static uint64_t g_nowMs = 1000;
static Rng* g_envRng = 0;
static uint64_t NowCb(void*) { return g_nowMs; }
static bool RandCb(void*, void* out, size_t n) { g_envRng->Fill(out, n); return true; }
struct SinkUdp : Udp {
    std::vector<Bytes> sent;
    unsigned long total, oversize;
    SinkUdp() : total(0), oversize(0) {}
    bool Open(uint16_t) { return true; }
    void Close() {}
    bool IsOpen() const { return true; }
    int SendTo(const Addr&, const void* d, size_t n) {
        ++total;
        if (n > kMaxDatagram + 4) ++oversize;   // (+4: the TURN ChannelData header of a relayed datagram; kRelayOverhead = 8 leaves headroom)
        if (sent.size() >= 64) sent.erase(sent.begin());
        sent.push_back(Bytes(static_cast<const uint8_t*>(d), static_cast<const uint8_t*>(d) + n));
        return int(n);
    }
    int RecvFrom(Addr*, void*, size_t) { return 0; }
    uint32_t LocalIp() { return 0x0A000002u; }
    uint16_t LocalPort() { return 41000; }
};
static Env MakeEnv(SinkUdp* u) { Env e; e.udp = u; e.now = NowCb; e.random = RandCb; return e; }

// ---- exact-size heap copy: the parser under test gets a block ASan watches to the byte ------------------------------------------
struct Exact {
    uint8_t* p;
    size_t n;
    explicit Exact(const Bytes& v) : n(v.size()) { p = static_cast<uint8_t*>(malloc(n ? n : 1)); if (n) memcpy(p, &v[0], n); }
    explicit Exact(size_t cap) : n(cap) { p = static_cast<uint8_t*>(malloc(n ? n : 1)); memset(p, 0xA5, n ? n : 1); }
    ~Exact() { free(p); }
private:
    Exact(const Exact&);
    Exact& operator=(const Exact&);
};

// ---- damage -------------------------------------------------------------------------------------------------------------------
static void Put16(Bytes& m, size_t at, uint32_t v) { if (at + 2 <= m.size()) { m[at] = uint8_t(v >> 8); m[at + 1] = uint8_t(v); } }
static void Damage(Bytes& m, Rng& r) {
    const int rounds = 1 + int(r.U(3));
    for (int k = 0; k < rounds; ++k) {
        switch (r.U(9)) {
        case 0: if (!m.empty()) m[r.U(uint32_t(m.size()))] ^= uint8_t(1u << r.U(8)); break;
        case 1: if (!m.empty()) m[r.U(uint32_t(m.size()))] = r.Byte(); break;
        case 2: if (!m.empty()) m[r.U(uint32_t(m.size()))] = r.Chance(50) ? 0xFF : 0x00; break;
        case 3: m.resize(r.U(uint32_t(m.size() + 1))); break;                                   // truncate
        case 4: { const size_t add = 1 + r.U(40); for (size_t i = 0; i < add; ++i) m.push_back(r.Byte()); break; }   // extend
        case 5: if (m.size() > 2) { const size_t a = r.U(uint32_t(m.size())), n = r.U(uint32_t(m.size() - a)); m.erase(m.begin() + a, m.begin() + a + n); } break;
        case 6: if (m.size() > 2) { const size_t a = r.U(uint32_t(m.size())), n = r.U(uint32_t(m.size() - a)); Bytes c(m.begin() + a, m.begin() + a + n); m.insert(m.begin() + r.U(uint32_t(m.size())), c.begin(), c.end()); } break;
        case 7: {   // a 16-bit field: a length or a type, set to a boundary value or the message length +-
            if (m.size() < 2) break;
            const size_t at = r.U(uint32_t(m.size() - 1));
            const uint32_t pick[] = {0, 1, 3, 4, 0x7FFF, 0xFFFF, uint32_t(m.size()), uint32_t(m.size() ? m.size() - 20 : 0), uint32_t(m.size() + 1), 0x0101, 0x0111, 0x0003};
            Put16(m, at, pick[r.U(12)]);
            break;
        }
        default: if (m.size() >= 4) { const size_t at = r.U(uint32_t(m.size() - 3)); const uint8_t v = r.Chance(50) ? 0xFF : 0x7F; m[at] = v; m[at + 1] = v; m[at + 2] = v; m[at + 3] = v; } break;   // 32-bit boundary
        }
    }
}

// ---- STUN / TURN message builder -----------------------------------------------------------------------------------------------
static const uint32_t kCookie = 0x2112A442u;
struct Stm {
    Bytes m;
    Stm(uint16_t type, const uint8_t txid[12]) : m(20, 0) {
        m[0] = uint8_t(type >> 8); m[1] = uint8_t(type);
        m[4] = uint8_t(kCookie >> 24); m[5] = uint8_t(kCookie >> 16); m[6] = uint8_t(kCookie >> 8); m[7] = uint8_t(kCookie);
        memcpy(&m[8], txid, 12);
    }
    void Fix() { Put16(m, 2, uint32_t(m.size() - 20)); }
    void Attr(uint16_t t, const void* v, size_t len) {
        const size_t at = m.size();
        m.resize(at + 4 + ((len + 3) & ~size_t(3)), 0);
        Put16(m, at, t); Put16(m, at + 2, uint32_t(len));
        if (len) memcpy(&m[at + 4], v, len);
        Fix();
    }
    void XorAddr(uint16_t t, uint32_t ip, uint16_t port, bool xorIt = true) {
        uint8_t a[8] = {0, 1, 0, 0, 0, 0, 0, 0};
        const uint16_t p = xorIt ? uint16_t(port ^ (kCookie >> 16)) : port;
        const uint32_t i = xorIt ? ip ^ kCookie : ip;
        a[2] = uint8_t(p >> 8); a[3] = uint8_t(p); a[4] = uint8_t(i >> 24); a[5] = uint8_t(i >> 16); a[6] = uint8_t(i >> 8); a[7] = uint8_t(i);
        Attr(t, a, 8);
    }
    void U32(uint16_t t, uint32_t v) { uint8_t b[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)}; Attr(t, b, 4); }
    void Str(uint16_t t, const std::string& s) { Attr(t, s.data(), s.size()); }
    void Error(int code) { uint8_t b[4] = {0, 0, uint8_t(code / 100), uint8_t(code % 100)}; Attr(0x0009, b, 4); }
    // MESSAGE-INTEGRITY with the long-term key, as the TURN server (or a liar who knows the key) signs
    void Sign(const uint8_t key[16]) {
        Bytes b = m;
        b.resize(m.size() + 24);
        size_t len = m.size();
        if (len >= 20 && TurnClient::AddIntegrity(&b[0], &len, b.size(), key)) { b.resize(len); m = b; }
    }
};
// Re-signs a damaged message: fixes the header length to the bytes present (rounded to 4), drops any old integrity attribute at the end, signs again.
static void Resign(Bytes& m, const uint8_t key[16]) {
    if (m.size() < 20) return;
    while ((m.size() - 20) % 4) m.push_back(0);
    Put16(m, 2, uint32_t(m.size() - 20));
    Stm s(0, reinterpret_cast<const uint8_t*>("0123456789ab"));
    s.m = m;
    s.Sign(key);
    m = s.m;
}

// ---- invariants shared by the tests -------------------------------------------------------------------------------------------
static void CheckSends(SinkUdp& u, size_t maxLen, const char* who) {
    EXPECT(u.oversize == 0, "%s sent a datagram over kMaxDatagram + 4", who);
    for (size_t i = 0; i < u.sent.size(); ++i) EXPECT(u.sent[i].size() <= maxLen, "%s sent %zu bytes (> %zu)", who, u.sent[i].size(), maxLen);
}
static bool TxidOfLastRequest(SinkUdp& u, uint8_t txid[12], uint16_t* type, size_t minLen = 20) {
    if (u.sent.empty()) return false;
    const Bytes& b = u.sent.back();
    if (b.size() < minLen || (b[0] & 0xC0) != 0) return false;
    memcpy(txid, &b[8], 12);
    *type = uint16_t(b[0] << 8 | b[1]);
    return true;
}
static std::string RandName(Rng& r, size_t maxLen) {
    static const char kAl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._- ";
    std::string s;
    const size_t n = 1 + r.U(uint32_t(maxLen));
    for (size_t i = 0; i < n; ++i) s += kAl[r.U(sizeof kAl - 1)];
    return s;
}

// =================================================================================================================================
// STUN
// =================================================================================================================================
static unsigned long g_stunParsedOk = 0, g_stunAnswered = 0;
static void FuzzStun(Rng& r) {
    uint8_t tx[12];
    r.Fill(tx, 12);
    const uint32_t ip = uint32_t(r.Next()) | 1u;
    uint16_t port = uint16_t(r.Next()) | 1u;
    const bool errorAnswer = r.Chance(10);
    Stm s(errorAnswer ? 0x0111 : (r.Chance(5) ? uint16_t(r.Next()) : 0x0101), tx);
    const int nAttrs = int(r.U(5));
    bool placed = false;
    for (int i = 0; i < nAttrs; ++i) {
        switch (r.U(5)) {
        case 0: s.XorAddr(0x0020, ip, port); placed = true; break;
        case 1: s.XorAddr(0x0001, ip, port, false); placed = true; break;
        case 2: { Bytes junk(r.U(60)); r.Fill(junk.empty() ? 0 : &junk[0], junk.size()); s.Attr(uint16_t(r.Next()), junk.empty() ? 0 : &junk[0], junk.size()); break; }
        case 3: s.Str(0x8022, RandName(r, 40)); break;
        default: { uint8_t a[20]; r.Fill(a, sizeof a); a[1] = r.Chance(50) ? 2 : 1; s.Attr(0x0020, a, r.Chance(50) ? 20 : 4 + r.U(8)); break; }   // IPv6 or short address attributes
        }
    }
    if (!errorAnswer && r.Chance(70) && !placed) s.XorAddr(0x0020, ip, port), placed = true;
    const Bytes original = s.m;
    // 1. the honest message parses to what was put in it (positive control: the fuzz is not just feeding garbage that is refused)
    if (placed && !errorAnswer && original.size() >= 20 && original[0] == 0x01 && original[1] == 0x01) {
        uint8_t got[12];
        Addr mapped;
        Exact ex(original);
        if (StunClient::ParseResponse(ex.p, ex.n, got, &mapped)) {
            ++g_stunParsedOk;
            EXPECT(memcmp(got, tx, 12) == 0, "STUN transaction id not returned");
        }
    }
    // 2. damaged, plus the client's own matching and source checks
    Bytes d = original;
    if (r.Chance(75)) Damage(d, r);
    Exact ex(d);
    uint8_t got[12];
    Addr mapped;
    const bool ok = StunClient::ParseResponse(ex.p, ex.n, got, &mapped);
    if (ok && d.size() >= 20 && d[0] == 0x01 && d[1] == 0x01) EXPECT(!mapped.IsZero(), "success answer without an address was accepted");
    SinkUdp udp;
    Env env = MakeEnv(&udp);
    StunClient c;
    Addr servers[2] = {MakeAddr(198, 51, 100, 1, 3478), MakeAddr(198, 51, 100, 2, 3478)};
    c.Start(&env, servers, 2);
    uint8_t t0[12];
    uint16_t ty;
    if (!TxidOfLastRequest(udp, t0, &ty)) { VIOLATION("STUN client sent no request"); return; }
    // the same damaged message but carrying the real transaction id, from the right server, from another source, and from another port
    Bytes withTx = d;
    if (withTx.size() >= 20) memcpy(&withTx[8], t0, 12);
    Exact e2(withTx);
    const int before = c.Answered();
    const Addr wrong = r.Chance(50) ? MakeAddr(198, 51, 100, 9, 3478) : MakeAddr(198, 51, 100, 1, 3479);
    c.HandlePacket(wrong, e2.p, e2.n);
    EXPECT(c.Answered() == before, "a STUN answer from the wrong source was taken");
    const bool consumed = c.HandlePacket(servers[0], e2.p, e2.n);
    if (consumed) ++g_stunAnswered;
    EXPECT(c.Answered() >= 0 && c.Answered() <= 2, "answered count out of range");
    Exact e3(d);
    c.HandlePacket(servers[1], e3.p, e3.n);   // (a random txid: not ours, must be refused without effect)
    EXPECT(c.Answered() <= 2, "answered count out of range");
    for (int i = 0; i < 12; ++i) { g_nowMs += 700; c.Tick(); }
    EXPECT(c.GetState() == StunClient::DONE || c.GetState() == StunClient::FAILED || c.GetState() == StunClient::RUNNING, "STUN state out of range");
    (void)c.Classify(0x0A000002u);
    CheckSends(udp, 20, "STUN");
}

// =================================================================================================================================
// TURN
// =================================================================================================================================
static unsigned long g_turnForged = 0, g_turnReady = 0, g_turnData = 0, g_turnSignedAccepted = 0, g_turnChannels = 0;
static bool RequestSigned(SinkUdp& u) {   // the client's last request carries MESSAGE-INTEGRITY
    if (u.sent.empty()) return false;
    const Bytes& b = u.sent.back();
    size_t o = 20;
    while (o + 4 <= b.size()) {
        const size_t l = size_t(b[o + 2] << 8 | b[o + 3]);
        if ((b[o] << 8 | b[o + 1]) == 0x0008) return true;
        o += 4 + ((l + 3) & ~size_t(3));
    }
    return false;
}
static void FuzzTurn(Rng& r) {
    SinkUdp udp;
    Env env = MakeEnv(&udp);
    TurnClient* cp = new TurnClient;
    TurnClient& c = *cp;
    const Addr server = MakeAddr(198, 51, 100, 7, 3478);
    const std::string user = "1700000000:" + RandName(r, 20), pass = RandName(r, 30);
    TurnConfig cfg;
    cfg.verifyIntegrity = r.Chance(50);   // MSC_TURN_VERIFY on in half of the cases
    c.Start(&env, &server, 1, user.c_str(), pass.c_str(), cfg);
    std::string realm = "msc.example", nonce = "n0nce";
    uint8_t key[16];
    TurnClient::LongTermKey(key, user.c_str(), realm.c_str(), pass.c_str());
    bool keyForServer = false;   // the server's idea of the key (true once it issued a nonce)
    const int steps = 3 + int(r.U(9));
    for (int step = 0; step < steps; ++step) {
        uint8_t tx[12];
        uint16_t reqType = 0;
        if (!TxidOfLastRequest(udp, tx, &reqType)) break;
        if (r.Chance(8)) r.Fill(tx, 12);   // an answer to nothing
        // what kind of answer the server (or a liar) sends
        const unsigned kind = r.U(12);
        const uint16_t method = uint16_t(reqType & 0x3EEF);
        Stm s(uint16_t(method | 0x0100), tx);
        bool signIt = keyForServer;
        bool forged = false;
        switch (kind) {
        case 0: case 1: {   // 401 / 438 with realm and nonce, lengths up to 250 bytes
            s = Stm(uint16_t(method | 0x0110), tx);
            s.Error(kind == 0 ? 401 : 438);
            realm = RandName(r, r.Chance(30) ? 250 : 20);
            nonce = RandName(r, r.Chance(30) ? 250 : 30);
            s.Str(0x0014, realm); s.Str(0x0015, nonce);
            TurnClient::LongTermKey(key, user.c_str(), realm.c_str(), pass.c_str());
            keyForServer = true;
            signIt = false;
            break;
        }
        case 2: { s = Stm(uint16_t(method | 0x0110), tx); static const int codes[] = {400, 403, 437, 441, 486, 508, 500, 0, 999, 300}; s.Error(codes[r.U(10)]); break; }
        case 3: { s = Stm(uint16_t(method | 0x0110), tx); s.Error(401); s.Str(0x0014, RandName(r, 20)); s.Str(0x0015, RandName(r, 20)); signIt = false; break; }   // 401 again with a new nonce (the retry limit)
        case 4: {   // Data indication
            s = Stm(0x0017, tx);
            s.XorAddr(0x0012, uint32_t(r.Next()), uint16_t(r.Next()));
            Bytes data(r.Chance(10) ? kMaxDatagram + 1 + r.U(300) : r.U(200));
            r.Fill(data.empty() ? 0 : &data[0], data.size());
            s.Attr(0x0013, data.empty() ? 0 : &data[0], data.size());
            signIt = false;
            break;
        }
        default: {  // success: Allocate (relayed + mapped + lifetime), Refresh (lifetime), permission, channel
            if (r.Chance(80)) s.XorAddr(0x0016, uint32_t(r.Next()), uint16_t(r.Next()));
            if (r.Chance(70)) s.XorAddr(0x0020, uint32_t(r.Next()), uint16_t(r.Next()));
            if (r.Chance(80)) { static const uint32_t lt[] = {0, 1, 9, 10, 20, 300, 600, 86399, 86400, 86401, 0x7FFFFFFFu, 0xFFFFFFFFu}; s.U32(0x000D, r.Chance(70) ? lt[r.U(12)] : uint32_t(r.Next())); }
            if (r.Chance(20)) { Bytes junk(r.U(50)); r.Fill(junk.empty() ? 0 : &junk[0], junk.size()); s.Attr(uint16_t(r.Next()), junk.empty() ? 0 : &junk[0], junk.size()); }
            if (cfg.verifyIntegrity && keyForServer && RequestSigned(udp) && r.Chance(25)) {   // a forged success: unsigned, or signed with a key that is not the long-term key
                forged = true;
                signIt = false;
                if (r.Chance(50)) { uint8_t wrong[16]; r.Fill(wrong, 16); s.Sign(wrong); }
            }
            break;
        }
        }
        if (signIt) s.Sign(key);
        Bytes msg = s.m;
        const bool damaged = !forged && r.Chance(55);
        if (damaged) {
            Damage(msg, r);
            if (r.Chance(50)) Resign(msg, key);   // the liar knows the key
        }
        const uint64_t succBefore = c.Stats().successes;
        const uint32_t refusedBefore = c.Stats().integrityRefused;
        Exact ex(msg);
        const uint8_t* inner = 0;
        size_t innerLen = 0;
        Addr peer;
        const Addr from = r.Chance(4) ? MakeAddr(198, 51, 100, 8, 3478) : server;
        const int rc = c.HandlePacket(from, ex.p, ex.n, &inner, &innerLen, &peer);
        EXPECT(rc >= 0 && rc <= 2, "TURN HandlePacket returned %d", rc);
        if (forged && from == server) EXPECT(c.Stats().successes == succBefore, "MSC_TURN_VERIFY: a success answer without a valid MESSAGE-INTEGRITY was taken");
        if (forged) g_turnForged += (c.Stats().integrityRefused - refusedBefore);
        if (!cfg.verifyIntegrity) EXPECT(c.Stats().integrityRefused == 0, "integrity refusals with the switch off");
        if (rc == 2) {
            ++g_turnData;
            EXPECT(inner >= ex.p && inner + innerLen <= ex.p + ex.n, "Data indication payload outside the datagram");
            EXPECT(innerLen <= kMaxDatagram, "Data indication payload %zu > kMaxDatagram", innerLen);
        }
        if (rc != 0 && !damaged && from == server) { /* honest answers are consumed: not asserted for 'answers to nothing' */ }
        if (c.Ready()) {
            ++g_turnReady;
            EXPECT(c.LifetimeSec() >= 10 && c.LifetimeSec() <= 86400, "TURN lifetime %u outside 10..86400", unsigned(c.LifetimeSec()));
            if (damaged) ++g_turnSignedAccepted;
            if (r.Chance(40)) {
                const int slot = int(r.U(6)) - 1;
                const uint16_t ch = c.BindPeer(slot, Addr(uint32_t(r.Next()) | 1u, uint16_t(r.Next())));
                EXPECT(ch == 0 || (ch >= 0x4000 && ch <= 0x7FFE), "channel 0x%04x outside the TURN range", unsigned(ch));
                if (ch) ++g_turnChannels;
            }
        }
        EXPECT(c.GetState() >= TurnClient::IDLE && c.GetState() <= TurnClient::FAILED, "TURN state out of range");
        EXPECT(memchr(c.Error(), 0, 128) != 0, "TURN error text not terminated inside 128 bytes");
        EXPECT(IsPrintableAscii(c.Error(), strlen(c.Error())), "TURN error text not printable");
        const unsigned adv = r.Chance(10) ? 20000 : r.U(1500);
        g_nowMs += adv;
        c.Tick();
        if (r.Chance(3)) c.Release();
        // what the client sends: a well-formed message under 512 bytes with a header length that matches
        for (size_t i = 0; i < udp.sent.size(); ++i) {
            const Bytes& b = udp.sent[i];
            if (b.size() < 20) { VIOLATION("TURN request of %zu bytes", b.size()); continue; }
            EXPECT(b.size() <= 512, "TURN request of %zu bytes", b.size());
            EXPECT(size_t(b[2] << 8 | b[3]) + 20 == b.size() && (b.size() % 4) == 0, "TURN request header length %u for %zu bytes", unsigned(b[2] << 8 | b[3]), b.size());
        }
        if (c.GetState() == TurnClient::FAILED) break;
    }
    CheckSends(udp, 512, "TURN");
    delete cp;
}

// =================================================================================================================================
// PEER (punch + secure session)
// =================================================================================================================================
static unsigned long g_peerPunchOk = 0, g_peerFramesIn = 0, g_peerFramesChecked = 0, g_peerConnected = 0, g_peerCtrl = 0, g_peerReplayRefused = 0, g_peerLiarFrames = 0;
static Bytes MakePunch(const uint8_t punchKey[32], int from, int to, uint8_t flags, const uint8_t hello[8], const uint8_t echo[8], uint32_t cookie) {
    Bytes p(Peer::kPunchSize, 0);
    p[0] = kTypePunch; p[1] = uint8_t(from); p[2] = uint8_t(to); p[3] = flags;
    memcpy(&p[4], hello, 8); memcpy(&p[12], echo, 8);
    p[20] = uint8_t(cookie >> 24); p[21] = uint8_t(cookie >> 16); p[22] = uint8_t(cookie >> 8); p[23] = uint8_t(cookie);
    Mac16(&p[24], punchKey, &p[0], 24);
    return p;
}
static Bytes MakeSecure(const uint8_t dataKey[32], uint8_t type, int fromSlot, uint32_t seq, const Bytes& plain) {
    Bytes pkt(kDataHeader + plain.size() + kTagSize);
    pkt[0] = type; pkt[1] = uint8_t(fromSlot);
    pkt[2] = uint8_t(seq >> 24); pkt[3] = uint8_t(seq >> 16); pkt[4] = uint8_t(seq >> 8); pkt[5] = uint8_t(seq);
    uint8_t nonce[24] = {0};
    nonce[0] = uint8_t(fromSlot);
    nonce[20] = pkt[2]; nonce[21] = pkt[3]; nonce[22] = pkt[4]; nonce[23] = pkt[5];
    AeadSeal(&pkt[kDataHeader], &pkt[kDataHeader + plain.size()], dataKey, nonce, &pkt[0], kDataHeader, plain.empty() ? 0 : &plain[0], plain.size());
    return pkt;
}
// Seals a damaged packet again: the header the damage left is the associated data, the nonce follows its sender byte and sequence number (a lying peer with the key).
static void ResealSecure(Bytes& m, const uint8_t dataKey[32]) {
    if (m.size() < kDataOverhead) return;
    const size_t plainLen = m.size() - kDataOverhead;
    Bytes plain(m.begin() + kDataHeader, m.begin() + kDataHeader + plainLen);
    uint8_t nonce[24] = {0};
    nonce[0] = m[1];
    nonce[20] = m[2]; nonce[21] = m[3]; nonce[22] = m[4]; nonce[23] = m[5];
    AeadSeal(&m[kDataHeader], &m[kDataHeader + plainLen], dataKey, nonce, &m[0], kDataHeader, plainLen ? &plain[0] : 0, plainLen);
}
static Bytes RandPlain(Rng& r, uint8_t type, bool* wantFrame) {
    *wantFrame = false;
    Bytes p;
    if (type == kTypeCtrl) {
        switch (r.U(7)) {
        case 0: p.push_back(1); break;                       // keep-alive
        case 1: p.push_back(2); break;                       // close
        case 2: case 3: { p.assign(9, 0); p[0] = r.Chance(50) ? 3 : 4; r.Fill(&p[1], 8); if (r.Chance(60)) { const uint64_t us = g_nowMs * 1000u - r.U(3000000); for (int i = 0; i < 8; ++i) p[1 + i] = uint8_t(us >> (56 - 8 * i)); } break; }   // ping / pong
        case 4: p.assign(r.U(20), 0); r.Fill(p.empty() ? 0 : &p[0], p.size()); break;
        default: p.assign(1 + r.U(12), 0); r.Fill(&p[0], p.size()); p[0] = uint8_t(r.U(6)); break;
        }
        return p;
    }
    const size_t n = r.Chance(8) ? kMaxFrame + r.U(300) : (r.Chance(15) ? r.U(8) : 24 + r.U(400));
    p.assign(n, 0);
    r.Fill(n ? &p[0] : 0, n);
    *wantFrame = true;
    return p;
}
static void FuzzPeer(Rng& r) {
    SinkUdp udp;
    Env env = MakeEnv(&udp);
    PeerParams pp;
    pp.mySlot = uint8_t(r.U(2)); pp.peerSlot = uint8_t(1 - pp.mySlot);
    r.Fill(pp.key, 32);
    pp.cookie = uint32_t(r.Next());
    const Addr pub = MakeAddr(203, 0, 113, uint8_t(1 + r.U(200)), 41000), loc = MakeAddr(192, 168, 1, 20, 41000);
    pp.cands[0] = Candidate(pub, CAND_PUBLIC); pp.cands[1] = Candidate(loc, CAND_LOCAL); pp.nCands = 2;
    pp.tryLocal = r.Chance(50);
    Peer* pe = new Peer;
    Peer& p = *pe;
    if (!p.Begin(&env, pp)) { VIOLATION("Peer::Begin failed with a working generator"); delete pe; return; }
    uint8_t punchKey[32];
    Kdf(punchKey, pp.key, "MSCN punch v1", 13);
    const uint8_t peerHello[8] = {uint8_t(1 + r.U(250)), r.Byte(), r.Byte(), r.Byte(), r.Byte(), r.Byte(), r.Byte(), r.Byte()};
    uint32_t seq = r.Chance(50) ? 0 : uint32_t(r.Next());
    const Addr sources[3] = {pub, loc, MakeAddr(203, 0, 113, 250, 5000 + r.U(100))};
    // honest handshake first, half of the cases, so DATA / CTRL reach the decryption and the frame queue
    bool honest = r.Chance(55);
    bool upExpected = false;
    if (honest) {
        const Bytes hs = MakePunch(punchKey, pp.peerSlot, pp.mySlot, 3 /*ack|confirmed*/, peerHello, PeerTestAccess::Hello(p), pp.cookie);
        Exact ex(hs);
        EXPECT(p.OnPacket(pub, ex.p, ex.n), "an honest punch was refused");
        ++g_peerPunchOk;
        for (int i = 0; i < 6 && !p.Connected(); ++i) { g_nowMs += 400; p.Tick(); if (p.Connected()) break; }
        upExpected = p.Connected();
        if (upExpected) ++g_peerConnected;
    }
    const int ops = 4 + int(r.U(14));
    for (int op = 0; op < ops; ++op) {
        const Addr from = sources[r.U(3)];
        const bool relay = r.Chance(10);
        switch (r.U(11)) {
        case 0: {   // valid punch, hostile contents
            uint8_t hello[8], echo[8];
            r.Fill(hello, 8); hello[0] |= 1; r.Fill(echo, 8);
            if (r.Chance(60)) memcpy(echo, PeerTestAccess::Hello(p), 8);
            if (r.Chance(30)) memcpy(hello, peerHello, 8);
            Bytes b = MakePunch(punchKey, pp.peerSlot, pp.mySlot, r.Byte() & 3, hello, echo, pp.cookie);
            Exact ex(b);
            if (p.OnPacket(from, ex.p, ex.n, relay)) ++g_peerPunchOk;
            break;
        }
        case 1: case 2: {   // damaged punch, re-MACed in half the cases (a member who lies with the key)
            uint8_t hello[8], echo[8];
            r.Fill(hello, 8); hello[0] |= 1;
            memcpy(echo, PeerTestAccess::Hello(p), 8);
            Bytes b = MakePunch(punchKey, pp.peerSlot, pp.mySlot, r.Byte() & 3, hello, echo, pp.cookie);
            Damage(b, r);
            if (b.size() >= 40 && r.Chance(50)) Mac16(&b[24], punchKey, &b[0], 24);
            Exact ex(b);
            p.OnPacket(from, ex.p, ex.n, relay);
            break;
        }
        case 3: case 4: {   // sealed DATA / CTRL
            if (!PeerTestAccess::KeysReady(p)) { Bytes b(r.U(80)); r.Fill(b.empty() ? 0 : &b[0], b.size()); if (!b.empty()) b[0] = r.Chance(50) ? kTypeData : kTypeCtrl; Exact ex(b); p.OnPacket(from, ex.p, ex.n, relay); break; }
            const uint8_t type = r.Chance(70) ? kTypeData : kTypeCtrl;
            bool wantFrame;
            const Bytes plain = RandPlain(r, type, &wantFrame);
            uint32_t s2 = seq;
            switch (r.U(6)) { case 0: s2 = seq - r.U(70); break; case 1: s2 = uint32_t(r.Next()); break; case 2: s2 = r.Chance(50) ? 0xFFFFFFFFu : 0; break; default: ++seq; s2 = seq; break; }
            Bytes b = MakeSecure(PeerTestAccess::DataKey(p), type, pp.peerSlot, s2, plain);
            const bool bad = r.Chance(45);
            if (bad) { Damage(b, r); if (r.Chance(50)) ResealSecure(b, PeerTestAccess::DataKey(p)); }
            const size_t beforeQ = PeerTestAccess::RxQueue(p);
            Exact ex(b);
            const bool taken = p.OnPacket(from, ex.p, ex.n, relay);
            if (taken && bad) ++g_peerLiarFrames;
            if (taken && !bad) { Exact again(b); EXPECT(!p.OnPacket(from, again.p, again.n, relay), "a replayed sealed packet was taken twice"); }   // the replay window
            if (type == kTypeCtrl) ++g_peerCtrl;
            if (taken && !bad && type == kTypeData && p.GetState() == Peer::CONNECTED && PeerTestAccess::RxQueue(p) > beforeQ) {
                // an honest frame that went in comes out byte for byte
                const size_t q = PeerTestAccess::RxQueue(p);
                if (q < 500) {
                    // (drain the older ones first so the newest is last: PopFrame is FIFO)
                    Bytes last;
                    for (size_t k = 0; k < q; ++k) {
                        Exact out(kMaxDatagram);
                        const int n = p.PopFrame(out.p, out.n);
                        if (k + 1 == q && n > 0) last.assign(out.p, out.p + n);
                    }
                    EXPECT(last == plain || plain.empty(), "an accepted frame came back different (%zu vs %zu bytes)", last.size(), plain.size());
                    ++g_peerFramesChecked;
                }
            }
            if (taken && type == kTypeData) ++g_peerFramesIn;
            if (!taken && !bad && s2 <= seq) ++g_peerReplayRefused;
            break;
        }
        case 9: {   // a punch with every field right but a MAC under another key, and a sealed packet with one bit flipped: never taken (a forger without the key)
            uint8_t hello[8];
            r.Fill(hello, 8); hello[0] |= 1;
            uint8_t wrongKey[32];
            r.Fill(wrongKey, 32);
            Bytes f = MakePunch(wrongKey, pp.peerSlot, pp.mySlot, r.Byte() & 3, hello, PeerTestAccess::Hello(p), pp.cookie);
            Exact ex(f);
            EXPECT(!p.OnPacket(from, ex.p, ex.n, relay), "a punch with a MAC under another key was taken");
            break;
        }
        case 10: {
            if (!PeerTestAccess::KeysReady(p)) break;
            const uint8_t type = r.Chance(70) ? kTypeData : kTypeCtrl;
            bool wantFrame;
            const Bytes plain = RandPlain(r, type, &wantFrame);
            Bytes b = MakeSecure(PeerTestAccess::DataKey(p), type, pp.peerSlot, seq + 1000 + r.U(1000), plain);
            b[r.U(uint32_t(b.size()))] ^= uint8_t(1u << r.U(8));
            Exact ex(b);
            EXPECT(!p.OnPacket(from, ex.p, ex.n, relay), "a sealed packet with a flipped bit was taken");
            break;
        }
        case 5: {   // plain garbage of every length, to the wire's first byte classes
            Bytes b(r.Chance(10) ? kMaxDatagram + kRelayOverhead + r.U(50) : r.U(200));
            r.Fill(b.empty() ? 0 : &b[0], b.size());
            if (!b.empty() && r.Chance(80)) b[0] = (r.Chance(34) ? kTypePunch : (r.Chance(50) ? kTypeData : kTypeCtrl));
            if (b.size() > 1 && r.Chance(70)) b[1] = pp.peerSlot;
            Exact ex(b);
            p.OnPacket(from, ex.p, ex.n, relay);
            int slot = -1;
            Peer::PeekFromSlot(ex.p, ex.n, &slot);
            break;
        }
        case 6: {   // reading frames into an exact-size buffer of any capacity
            const size_t cap = r.Chance(30) ? r.U(8) : r.U(kMaxDatagram + 40);
            Exact out(cap);
            const int n = p.PopFrame(out.p, out.n);
            EXPECT(n >= 0 && size_t(n) <= cap, "PopFrame returned %d for a buffer of %zu", n, cap);
            break;
        }
        case 7: {   // sending: any length, never past a datagram
            Bytes f(r.Chance(10) ? kMaxFrame + 1 + r.U(200) : r.U(kMaxFrame + 1));
            r.Fill(f.empty() ? 0 : &f[0], f.size());
            Exact ex(f);
            const bool sent = p.SendFrame(ex.p, ex.n);
            if (f.size() > kMaxFrame) EXPECT(!sent, "a frame of %zu bytes was sent", f.size());
            if (r.Chance(5)) p.SendClose();
            break;
        }
        default: {
            const unsigned adv = r.Chance(10) ? 30000 : r.U(2500);
            g_nowMs += adv;
            if (r.Chance(30)) p.CreditLocalStall(r.U(5000));
            p.Tick();
            if (r.Chance(8)) p.Restart();
            if (r.Chance(8)) p.SetRelay(MakeAddr(198, 51, 100, 7, 3478), uint16_t(0x4000 + r.U(0x3FFF)));
            break;
        }
        }
        EXPECT(p.GetState() >= Peer::IDLE && p.GetState() <= Peer::CLOSED, "peer state out of range");
        EXPECT(PeerTestAccess::RxQueue(p) <= 512, "receive queue %zu > 512", PeerTestAccess::RxQueue(p));
        EXPECT(p.LearnedCount() <= 8 && p.ConfirmedCount() <= 8, "learned %zu confirmed %zu: a list grew past its bound", p.LearnedCount(), p.ConfirmedCount());
        EXPECT(p.LossPermille() >= -1 && p.LossPermille() <= 1000, "loss %d", p.LossPermille());
    }
    CheckSends(udp, kMaxDatagram + 4, "Peer");
    (void)upExpected;
    delete pe;
}

// =================================================================================================================================
// JSON and text
// =================================================================================================================================
// A document built from real answer shapes. `bad` tells the oracle that a string in it holds a byte outside printable ASCII (after unescaping).
static std::string AsciiStr(Rng& r, size_t maxLen) { return RandName(r, maxLen); }
static std::string BadStr(Rng& r, bool* bad) {
    const uint32_t pick = r.U(8);
    if (pick != 6) *bad = true;   // (the surrogate case is good: it becomes '?')
    switch (pick) {
    case 0: return "Ni\\nck";                 // an escaped line feed: a forged log line
    case 1: return "caf\\u00e9";              // UTF-8 the fonts cannot draw
    case 2: return std::string("raw") + char(0xC3) + char(0xA9);
    case 3: return "tab\\there";
    case 4: return "\\u0000nul";
    case 5: return "del\\u007f";              // 0x7F
    case 6: return "sur\\ud800rogate";        // a surrogate half becomes '?' (printable), but the replacement is not what is tested here
    default: return std::string("hi") + char(0xFF) + "x";
    }
}
static std::string Piece(Rng& r, int pct, bool* bad) {
    if (r.Chance(unsigned(pct))) { const std::string s = BadStr(r, bad); return s; }
    return AsciiStr(r, 12);
}
static std::string MakeDoc(Rng& r, bool* bad) {
    *bad = false;
    const unsigned pct = r.Chance(50) ? 0 : 12;
    bool b = false;
    std::string d;
    switch (r.U(4)) {
    case 0:   // a room view (np_signal_http.cpp ApplyRoom)
        d = "{\"state\":\"open\",\"slot\":" + std::string(r.Chance(50) ? "0" : "1") + ",\"code\":\"" + Piece(r, pct, &b) + "\",\"version\":" + std::to_string(r.U(100000)) + ",\"changed\":true,"
            "\"you\":{\"name\":\"" + Piece(r, pct, &b) + "\",\"vip\":\"10.64.0.1\"},\"peer\":{\"slot\":1,\"name\":\"" + Piece(r, pct, &b) + "\",\"vip\":\"10.64.0.2\",\"link\":\"ok\","
            "\"endpoint\":{\"public\":\"203.0.113.5:41000\",\"local\":\"192.168.1.9:41000\",\"nat\":\"cone\",\"stale\":false,\"nonce\":\"AAECAwQFBgc=\",\"relay\":\"198.51.100.7:50000\"}},"
            "\"session\":{\"key\":\"" + std::string("q83vEjRWeJCrze8SNFZ4kKvN7xI0VniQq83vEjRWeJA=") + "\",\"cookie\":" + std::to_string(r.U(1u << 30)) + ",\"steer\":{\"relay\":true,\"reason\":\"" + Piece(r, pct, &b) + "\"}}}";
        break;
    case 1:   // relay credentials
        d = "{\"ttlSec\":3600,\"servers\":[{\"urls\":[\"stun:198.51.100.7:3478\"]},{\"urls\":[\"turn:198.51.100.7:3478?transport=udp\",\"turn:198.51.100.7:3478?transport=tcp\"],\"username\":\"" + Piece(r, pct, &b) +
            "\",\"credential\":\"" + Piece(r, pct, &b) + "\"}]}";
        break;
    case 2: {   // friends
        d = "{\"changed\":true,\"rev\":\"" + Piece(r, pct, &b) + "\",\"heldMs\":0,\"code\":\"" + Piece(r, pct, &b) + "\",\"friends\":[";
        const int n = int(r.U(4));
        for (int i = 0; i < n; ++i) d += std::string(i ? "," : "") + "{\"code\":\"" + Piece(r, pct, &b) + "\",\"name\":\"" + Piece(r, pct, &b) + "\",\"state\":\"friend\",\"presence\":\"online\",\"status\":\"" + Piece(r, pct, &b) + "\"}";
        d += "]}";
        break;
    }
    default:  // an error object
        d = "{\"error\":{\"code\":\"" + Piece(r, pct, &b) + "\",\"message\":\"" + Piece(r, pct, &b) + "\",\"retryAfterSec\":" + std::to_string(r.U(100)) + ",\"latestVersion\":\"" + Piece(r, pct, &b) + "\"}}";
        break;
    }
    *bad = b;
    return d;
}
static void WalkNp(const Json& j, int n, unsigned depth, unsigned long* nodes) {
    if (n < 0 || depth > 14) return;
    ++*nodes;
    if (j.TypeOf(n) == Json::STR) EXPECT(IsPrintableAscii(j.Str(n).data(), j.Str(n).size()), "np_json kept a string outside printable ASCII");
    for (int i = 0; i < j.Size(n); ++i) WalkNp(j, j.At(n, i), depth + 1, nodes);
}
static void WalkOn(const NpOnline::JValue& v, unsigned depth, unsigned long* nodes) {
    if (depth > 14) return;
    ++*nodes;
    if (v.type == NpOnline::JValue::Str) EXPECT(IsPrintableAscii(v.s.data(), v.s.size()), "NpOnline::JsonParse kept a string outside printable ASCII");
    for (size_t i = 0; i < v.a.size(); ++i) WalkOn(v.a[i], depth + 1, nodes);
    for (size_t i = 0; i < v.o.size(); ++i) {
        EXPECT(IsPrintableAscii(v.o[i].first.data(), v.o[i].first.size()), "NpOnline::JsonParse kept a key outside printable ASCII");
        WalkOn(v.o[i].second, depth + 1, nodes);
    }
}
static unsigned long g_jsonGood = 0, g_jsonRefusedBad = 0, g_jsonDamagedOk = 0, g_jsonNodes = 0;
static void FuzzJson(Rng& r) {
    bool bad = false;
    const std::string doc = MakeDoc(r, &bad);
    TextAsciiOverride() = 1;
    // 1. oracle: a well-formed document always parses; a string with a bad byte comes out scrubbed to '?' (WalkNp / WalkOn then check every string is printable)
    {
        Bytes b(doc.begin(), doc.end());
        Exact ex(b);
        Json j;
        const bool ok = j.Parse(reinterpret_cast<const char*>(ex.p), ex.n);
        NpOnline::JValue v;
        const bool ok2 = NpOnline::JsonParse(reinterpret_cast<const char*>(ex.p), ex.n, &v);
        EXPECT(ok, "np_json refused a well-formed document (bad=%d): %.60s", int(bad), doc.c_str());
        EXPECT(ok2, "NpOnline::JsonParse refused a well-formed document (bad=%d): %.60s", int(bad), doc.c_str());
        if (!bad) ++g_jsonGood; else ++g_jsonRefusedBad;
        if (ok) WalkNp(j, j.Root(), 0, &g_jsonNodes);
        if (ok2) WalkOn(v, 0, &g_jsonNodes);
        // the earlier behaviour is still there with the switch off (bytes >= 0x20 pass)
        TextAsciiOverride() = 0;
        Json jo;
        NpOnline::JValue vo;
        const bool off = jo.Parse(reinterpret_cast<const char*>(ex.p), ex.n);
        const bool off2 = NpOnline::JsonParse(reinterpret_cast<const char*>(ex.p), ex.n, &vo);
        EXPECT(off == off2, "the two parsers disagree with the switch off");
        TextAsciiOverride() = 1;
    }
    // 2. damaged: no crash, and whatever parses holds printable strings only
    Bytes m(doc.begin(), doc.end());
    if (r.Chance(10)) {   // structure attacks: deep nesting, a huge string, many nodes
        switch (r.U(3)) {
        case 0: { std::string deep; const size_t n = 5 + r.U(60); for (size_t i = 0; i < n; ++i) deep += r.Chance(50) ? "[" : "{\"a\":"; deep += "1"; for (size_t i = 0; i < n; ++i) deep += "]"; m.assign(deep.begin(), deep.end()); break; }
        case 1: { std::string big = "{\"a\":\"" + std::string(r.U(70000), 'x') + "\"}"; m.assign(big.begin(), big.end()); break; }
        default: { std::string many = "["; const size_t n = r.U(5000); for (size_t i = 0; i < n; ++i) many += (i ? ",1" : "1"); many += "]"; m.assign(many.begin(), many.end()); break; }
        }
    } else {
        Damage(m, r);
    }
    Exact ex(m);
    Json j;
    if (j.Parse(reinterpret_cast<const char*>(ex.p), ex.n)) { ++g_jsonDamagedOk; WalkNp(j, j.Root(), 0, &g_jsonNodes); (void)j.GetStr(j.Root(), "name"); (void)j.GetNum(j.Root(), "slot"); }
    NpOnline::JValue v;
    if (NpOnline::JsonParse(reinterpret_cast<const char*>(ex.p), ex.n, &v)) WalkOn(v, 0, &g_jsonNodes);
    TextAsciiOverride() = -1;
}

static unsigned long g_textCopies = 0;
static void FuzzText(Rng& r) {
    TextAsciiOverride() = 1;
    // SafeCopy into an exact-size buffer: terminated, inside the buffer, printable, a printable source comes through unchanged
    Bytes src(r.U(60));
    for (size_t i = 0; i < src.size(); ++i) src[i] = r.Chance(70) ? uint8_t(0x20 + r.U(0x5F)) : r.Byte();
    if (r.Chance(20) && !src.empty()) src[r.U(uint32_t(src.size()))] = 0;
    src.push_back(0);
    Exact in(src);
    const size_t cap = 1 + r.U(48);
    Exact out(cap);
    const size_t limit = r.Chance(50) ? static_cast<size_t>(-1) : r.U(70);
    const size_t n = SafeCopy(reinterpret_cast<char*>(out.p), cap, reinterpret_cast<const char*>(in.p), limit);
    ++g_textCopies;
    EXPECT(n < cap && out.p[n] == 0, "SafeCopy not terminated inside %zu bytes (n=%zu)", cap, n);
    EXPECT(IsPrintableAscii(reinterpret_cast<char*>(out.p), n), "SafeCopy left a byte outside printable ASCII");
    EXPECT(strlen(reinterpret_cast<char*>(out.p)) == n, "SafeCopy length mismatch");
    size_t srcLen = strlen(reinterpret_cast<const char*>(in.p));
    if (srcLen > limit) srcLen = limit;
    const size_t want = srcLen < cap - 1 ? srcLen : cap - 1;
    EXPECT(n == want, "SafeCopy copied %zu bytes, expected %zu", n, want);
    for (size_t i = 0; i < n; ++i) if (in.p[i] >= 0x20 && in.p[i] <= 0x7E) EXPECT(out.p[i] == in.p[i], "SafeCopy changed a printable byte");
    // a nickname never carries a line feed into the log
    for (size_t i = 0; i < n; ++i) EXPECT(out.p[i] != '\n' && out.p[i] != '\r', "line break in a copied name");
    // base64 into an exact-size output: bounded, and a damaged text never writes past it
    Bytes raw(r.U(60));
    r.Fill(raw.empty() ? 0 : &raw[0], raw.size());
    std::string enc = Base64Encode(raw.empty() ? 0 : &raw[0], raw.size());
    if (r.Chance(50) && !enc.empty()) enc[r.U(uint32_t(enc.size()))] = char(r.Byte());
    if (r.Chance(10)) enc.resize(r.U(uint32_t(enc.size() + 1)));
    const size_t ocap = r.U(70);
    Exact ob(ocap);
    const int got = Base64Decode(enc, ob.p, ocap);
    EXPECT(got >= -1 && got <= int(ocap), "Base64Decode returned %d for a buffer of %zu", got, ocap);
    std::vector<uint8_t> dec;
    if (NpOnline::Base64Decode(enc, &dec)) EXPECT(dec.size() <= enc.size(), "NpOnline::Base64Decode longer than its text");
    // a quoted string comes back through the parser
    std::string q;
    const std::string name = RandName(r, 30);
    JsonEscapeAppend(&q, name.c_str());
    Json j;
    EXPECT(j.Parse(q.data(), q.size()) && j.Str(j.Root()) == name, "JsonEscapeAppend round trip");
    EXPECT(NpOnline::JsonQuote(name) == q, "the two quoting functions differ");
    TextAsciiOverride() = -1;
}

// O5 (a diagnostic, not a fuzz): the longest gap between two datagrams on a connected path is recorded, and an idle link is never quieter than about a second (the
// 1 s ping), a running one never quieter than its frame interval. This is what Galaxy's "a note at least every 50 ms to keep the Wi-Fi awake" would change; Strikers' lockstep
// already sends a frame per step in a match (16-33 ms), so the quiet stretches are the lobby and the waiting banner, at most ~1 s.
static void TestSendGap() {
    SinkUdp udp;
    Rng rr(77);
    Rng* const savedEnvRng = g_envRng;
    g_envRng = &rr;
    Env env = MakeEnv(&udp);
    PeerParams pp;
    pp.mySlot = 0; pp.peerSlot = 1;
    for (int i = 0; i < 32; ++i) pp.key[i] = uint8_t(i + 1);
    pp.cookie = 0x01020304u;
    pp.cands[0] = Candidate(MakeAddr(203, 0, 113, 9, 41000), CAND_PUBLIC); pp.nCands = 1;
    Peer p;
    g_nowMs = 5000;
    p.Begin(&env, pp);
    uint8_t pk[32];
    Kdf(pk, pp.key, "MSCN punch v1", 13);
    const uint8_t hello[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    Bytes hs = MakePunch(pk, 1, 0, 3, hello, PeerTestAccess::Hello(p), pp.cookie);
    p.OnPacket(MakeAddr(203, 0, 113, 9, 41000), &hs[0], hs.size());
    for (int i = 0; i < 10 && !p.Connected(); ++i) { g_nowMs += 100; p.Tick(); }
    EXPECT(p.Connected(), "send-gap test: not connected");
    uint8_t frame[100] = {0};
    for (int i = 0; i < 40; ++i) { g_nowMs += 25; p.SendFrame(frame, sizeof frame); p.Tick(); }   // a match: a frame every 25 ms
    const uint64_t running = p.Stats().maxTxGapMs;
    EXPECT(running <= 1000, "send-gap test: a running link was quiet for %llu ms", (unsigned long long)running);
    for (int i = 0; i < 60; ++i) { g_nowMs += 100; p.Tick(); }                                  // a lobby: nothing but the transport's own ping / keep-alive
    const uint64_t idle = p.Stats().maxTxGapMs;
    EXPECT(idle >= 800 && idle <= 1200, "send-gap test: an idle link's longest gap was %llu ms (the ping is 1000 ms)", (unsigned long long)idle);
    g_envRng = savedEnvRng;
    printf("  O5 send gap: running link %llu ms, idle link (lobby) %llu ms\n", (unsigned long long)running, (unsigned long long)idle);
}

int main(int argc, char** argv) {
    unsigned long cases = 200000;
    uint64_t seed = 0x5EED13u;
    const char* only = 0;
    bool verbose = false;
    if (const char* e = getenv("NP_FUZZ_CASES")) cases = strtoul(e, 0, 10);
    if (const char* e = getenv("NP_FUZZ_SEED")) seed = strtoull(e, 0, 0);
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--cases") && i + 1 < argc) cases = strtoul(argv[++i], 0, 10);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = strtoull(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--only") && i + 1 < argc) only = argv[++i];
        else if (!strcmp(argv[i], "-v")) verbose = true;
    }
    Rng rng(seed), envRng(seed ^ 0xABCDEF);
    g_envRng = &envRng;
    unsigned long n[5] = {0, 0, 0, 0, 0};
    TestSendGap();
    for (g_caseNo = 0; g_caseNo < cases; ++g_caseNo) {
        g_nowMs = 1000 + (g_caseNo & 0xFFFF);
        // mix: 12% STUN, 18% TURN, 40% PEER, 20% JSON, 10% text
        unsigned k = rng.U(100);
        int which = k < 12 ? 0 : k < 30 ? 1 : k < 70 ? 2 : k < 90 ? 3 : 4;
        if (only) which = !strcmp(only, "stun") ? 0 : !strcmp(only, "turn") ? 1 : !strcmp(only, "peer") ? 2 : !strcmp(only, "json") ? 3 : 4;
        ++n[which];
        switch (which) {
        case 0: FuzzStun(rng); break;
        case 1: FuzzTurn(rng); break;
        case 2: FuzzPeer(rng); break;
        case 3: FuzzJson(rng); break;
        default: FuzzText(rng); break;
        }
        if (verbose && (g_caseNo % 50000) == 49999) fprintf(stderr, "  ... %lu cases\n", g_caseNo + 1);
    }
    printf("fuzz_net_test: %lu cases (seed 0x%llx): stun %lu, turn %lu, peer %lu, json %lu, text %lu\n", cases, static_cast<unsigned long long>(seed), n[0], n[1], n[2], n[3], n[4]);
    printf("  reached: stun honest parses %lu, stun answers taken %lu; turn ready %lu (after damage %lu), data indications %lu, channels %lu, forged successes refused %lu;\n", g_stunParsedOk, g_stunAnswered, g_turnReady, g_turnSignedAccepted, g_turnData, g_turnChannels, g_turnForged);
    printf("           peer punches taken %lu, connected %lu, frames taken %lu (byte-checked %lu), ctrl %lu, liar frames taken %lu, replays refused %lu;\n", g_peerPunchOk, g_peerConnected, g_peerFramesIn, g_peerFramesChecked, g_peerCtrl, g_peerLiarFrames, g_peerReplayRefused);
    printf("           json good %lu, scrubbed (had a bad byte) %lu, damaged but parsed %lu, nodes walked %lu; text copies %lu\n", g_jsonGood, g_jsonRefusedBad, g_jsonDamagedOk, g_jsonNodes, g_textCopies);
    printf(g_violations ? "fuzz_net_test: FAILED (%lu violations)\n" : "fuzz_net_test: PASS (%lu violations)\n", g_violations);
    return g_violations ? 1 : 0;
}
