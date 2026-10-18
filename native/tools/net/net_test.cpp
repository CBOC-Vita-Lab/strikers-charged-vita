// Tests of the native port's LAN networking layer (platform/sdk/net): VLink (frames, sockets, peers, impairment) with
// an in-memory hub between two Link instances, and the SO API over the POSIX UDP backend between two processes
// (fork) on 127.0.0.1, including the byte order of addresses as the game sees them.
//   tools/net/run_tests.sh      (host32; no game data, a few seconds)
#include <revolution/so.h>

#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <deque>
#include <vector>

#include "platform/sdk/net/vlink.h"

static int g_checks = 0, g_failed = 0;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        ++g_checks;                                                                       \
        if (!(cond)) {                                                                    \
            ++g_failed;                                                                   \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
        }                                                                                 \
    } while (0)

// ---- an in-memory medium between consoles ---------------------------------------------------------------------
struct Hub;
struct Endpoint {
    Hub* hub;
    int id;
    uint32_t vip;
    std::deque<std::vector<uint8_t> > rx;
    std::deque<int> rxFrom;
    VLink::Backend backend;
    int closed;
};
struct Hub {
    std::vector<Endpoint*> eps;
    int sent, bcast;
    // fragment fates (TestFragmentation): 0 as sent, 1 every fragment twice, 2 each pair of fragments swapped, 3 the dropNth-th fragment lost
    int mode, dropNth, fragSeen;
    size_t maxLen;
    std::vector<uint8_t> held;
    Hub() : sent(0), bcast(0), mode(0), dropNth(-1), fragSeen(0), maxLen(0) {}
};
static uint32_t HubVip(void* ctx) { return static_cast<Endpoint*>(ctx)->vip; }
static int HubSend(void* ctx, const VLink::PeerAddr* to, bool broadcast, const void* data, size_t len);
static int HubDeliver(Endpoint* me, const VLink::PeerAddr* to, bool broadcast, const void* data, size_t len) {
    for (size_t i = 0; i < me->hub->eps.size(); ++i) {
        Endpoint* e = me->hub->eps[i];
        if (e == me) continue;
        if (!broadcast && to && to->bytes[0] != e->id) continue;
        e->rx.push_back(std::vector<uint8_t>(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + len));
        e->rxFrom.push_back(me->id);
    }
    return int(len);
}
static int HubSend(void* ctx, const VLink::PeerAddr* to, bool broadcast, const void* data, size_t len) {
    Endpoint* me = static_cast<Endpoint*>(ctx);
    Hub& hub = *me->hub;
    ++hub.sent;
    if (broadcast) ++hub.bcast;
    if (len > hub.maxLen) hub.maxLen = len;
    const uint8_t* d = static_cast<const uint8_t*>(data);
    const bool fragment = len >= 4 && d[0] == 'M' && d[1] == 'S' && d[2] == 'L' && d[3] == 'F';
    if (!fragment || hub.mode == 0) return HubDeliver(me, to, broadcast, data, len);
    const int n = hub.fragSeen++;
    if (hub.mode == 1) {
        HubDeliver(me, to, broadcast, data, len);
        return HubDeliver(me, to, broadcast, data, len);
    }
    if (hub.mode == 3) return n == hub.dropNth ? int(len) : HubDeliver(me, to, broadcast, data, len);
    // mode 2: hold every first fragment, deliver it after the second
    if (hub.held.empty()) {
        hub.held.assign(d, d + len);
        return int(len);
    }
    HubDeliver(me, to, broadcast, data, len);
    std::vector<uint8_t> first;
    first.swap(hub.held);
    return HubDeliver(me, to, broadcast, &first[0], first.size());
}
static int HubRecv(void* ctx, VLink::PeerAddr* from, void* buffer, size_t capacity) {
    Endpoint* me = static_cast<Endpoint*>(ctx);
    if (me->rx.empty()) return 0;
    std::vector<uint8_t> f = me->rx.front();
    me->rx.pop_front();
    memset(from, 0, sizeof *from);
    from->bytes[0] = uint8_t(me->rxFrom.front());
    me->rxFrom.pop_front();
    const size_t n = f.size() < capacity ? f.size() : capacity;
    memcpy(buffer, &f[0], n);
    return int(n);
}
static void HubClose(void* ctx) { ++static_cast<Endpoint*>(ctx)->closed; }
static Endpoint* AddEndpoint(Hub& hub, uint32_t vip) {
    Endpoint* e = new Endpoint;
    e->hub = &hub;
    e->id = int(hub.eps.size()) + 1;
    e->vip = vip;
    e->closed = 0;
    memset(&e->backend, 0, sizeof e->backend);
    e->backend.name = "hub";
    e->backend.ctx = e;
    e->backend.local_vip = HubVip;
    e->backend.send = HubSend;
    e->backend.recv = HubRecv;
    e->backend.close = HubClose;
    hub.eps.push_back(e);
    return e;
}

static const uint32_t kFp = 0xC0FFEE01u;
static uint32_t Vip(int a, int b, int c, int d) { return uint32_t(a) << 24 | uint32_t(b) << 16 | uint32_t(c) << 8 | uint32_t(d); }

static uint64_t g_now = 1000;
static uint64_t FakeNow(void*) { return g_now; }

static void TestLinkBasics() {
    Hub hub;
    hub.sent = hub.bcast = 0;
    Endpoint* ea = AddEndpoint(hub, Vip(10, 0, 0, 1));
    Endpoint* eb = AddEndpoint(hub, Vip(10, 0, 0, 2));
    VLink::Link a, b;
    a.SetBackend(&ea->backend);
    b.SetBackend(&eb->backend);
    a.SetFingerprint(kFp);
    b.SetFingerprint(kFp);
    CHECK(!a.IsUp());
    CHECK(a.OpenSocket() == -1);  // link down
    CHECK(a.Start() && b.Start());
    CHECK(a.LocalVip() == Vip(10, 0, 0, 1));

    const int ad = a.OpenSocket(), ab = a.OpenSocket();  // direct (1000) and broadcast (1001) sockets, as the game does
    const int bd = b.OpenSocket(), bb = b.OpenSocket();
    CHECK(ad == 1 && ab == 2 && bd == 1 && bb == 2);
    CHECK(a.Bind(ad, 1000) && a.Bind(ab, 1001) && b.Bind(bd, 1000) && b.Bind(bb, 1001));
    CHECK(!a.Bind(ab, 1000));  // taken
    CHECK(a.Bind(ab, 1001));   // re-binding its own port is fine
    CHECK(a.LocalPort(ad) == 1000);

    // broadcast from a reaches b's broadcast socket, not a's own
    const char hello[] = "lobby-hello";
    CHECK(a.SendTo(ab, VLink::kBroadcastVip, 1001, hello, sizeof hello) == int(sizeof hello));
    char buf[64];
    uint32_t src = 0;
    uint16_t sport = 0;
    CHECK(a.RecvFrom(ab, buf, sizeof buf, &src, &sport) == -1);  // no echo
    CHECK(b.RecvFrom(bd, buf, sizeof buf, &src, &sport) == -1);  // wrong port
    int n = b.RecvFrom(bb, buf, sizeof buf, &src, &sport);
    CHECK(n == int(sizeof hello) && memcmp(buf, hello, sizeof hello) == 0);
    CHECK(src == Vip(10, 0, 0, 1) && sport == 1001);

    // b learned a's address, so a unicast goes out as a unicast (not a broadcast)
    const int before = hub.bcast;
    const char reply[] = "join";
    CHECK(b.SendTo(bd, src, 1000, reply, sizeof reply) == int(sizeof reply));
    CHECK(hub.bcast == before);
    n = a.RecvFrom(ad, buf, sizeof buf, &src, &sport);
    CHECK(n == int(sizeof reply) && memcmp(buf, reply, sizeof reply) == 0 && src == Vip(10, 0, 0, 2));

    // a unicast to an unknown vip goes out as a broadcast frame and only the addressee keeps it
    const int bc = hub.bcast;
    CHECK(a.SendTo(ad, Vip(10, 9, 9, 9), 1000, "x", 1) == 1);
    CHECK(hub.bcast == bc + 1);
    CHECK(b.RecvFrom(bd, buf, sizeof buf, &src, &sport) == -1);

    // truncation (UDP semantics), zero-length datagram, oversize
    std::vector<uint8_t> big(500, 0x5A);
    CHECK(a.SendTo(ad, Vip(10, 0, 0, 2), 1000, &big[0], big.size()) == 500);
    CHECK(b.RecvFrom(bd, buf, 16, &src, &sport) == 16);
    CHECK(a.SendTo(ad, Vip(10, 0, 0, 2), 1000, buf, 0) == 0);
    CHECK(b.RecvFrom(bd, buf, sizeof buf, &src, &sport) == 0);
    std::vector<uint8_t> max(VLink::kMaxPayload, 1), over(VLink::kMaxPayload + 1, 1);
    CHECK(a.SendTo(ad, Vip(10, 0, 0, 2), 1000, &max[0], max.size()) == int(max.size()));
    CHECK(a.SendTo(ad, Vip(10, 0, 0, 2), 1000, &over[0], over.size()) == -1);
    std::vector<uint8_t> rbuf(2000);
    CHECK(b.RecvFrom(bd, &rbuf[0], rbuf.size(), &src, &sport) == int(VLink::kMaxPayload));

    // queue limit: 128 datagrams, the oldest are dropped
    for (int i = 0; i < 130; ++i) {
        uint8_t v = uint8_t(i);
        a.SendTo(ad, Vip(10, 0, 0, 2), 1000, &v, 1);
    }
    uint8_t first = 0;
    CHECK(b.RecvFrom(bd, &first, 1, &src, &sport) == 1 && first == 2);
    CHECK(b.GetStats().droppedQueueFull == 2);
    while (b.RecvFrom(bd, &first, 1, &src, &sport) >= 0) {}

    // a closed socket neither sends nor receives
    a.Close(ad);
    CHECK(!a.IsOpen(ad) && a.SendTo(ad, Vip(10, 0, 0, 2), 1000, "x", 1) == -1 && a.RecvFrom(ad, buf, 8, 0, 0) == -1);
    const int ad2 = a.OpenSocket();
    CHECK(ad2 == ad);  // handle reuse
    CHECK(a.Bind(ad2, 0) && a.LocalPort(ad2) >= 49152);  // ephemeral

    // stop / start: sockets are gone, the backend is closed, learned peers are forgotten
    a.Stop();
    CHECK(!a.IsUp() && ea->closed == 1 && !a.IsOpen(ab));
    CHECK(a.Start() && a.OpenSocket() == 1);
    b.Stop();
    a.Stop();
}

static void TestLinkRejects() {
    Hub hub;
    hub.sent = hub.bcast = 0;
    Endpoint* ea = AddEndpoint(hub, Vip(10, 0, 0, 1));
    Endpoint* eb = AddEndpoint(hub, Vip(10, 0, 0, 2));
    VLink::Link a, b;
    a.SetBackend(&ea->backend);
    b.SetBackend(&eb->backend);
    a.SetFingerprint(kFp);
    b.SetFingerprint(kFp + 1);  // another build
    CHECK(a.Start() && b.Start());
    const int as = a.OpenSocket(), bs = b.OpenSocket();
    a.Bind(as, 1000);
    b.Bind(bs, 1000);
    char buf[16];
    a.SendTo(as, VLink::kBroadcastVip, 1000, "x", 1);
    CHECK(b.RecvFrom(bs, buf, sizeof buf, 0, 0) == -1);
    CHECK(b.GetStats().droppedFingerprint == 1);

    // malformed frames straight into b's backend: short, bad magic, bad length
    std::vector<uint8_t> junk(10, 0xFF);
    eb->rx.push_back(junk);
    eb->rxFrom.push_back(1);
    std::vector<uint8_t> frame(24, 0);
    frame[0] = 'X';
    eb->rx.push_back(frame);
    eb->rxFrom.push_back(1);
    b.Pump();
    CHECK(b.GetStats().droppedMalformed == 2);
    // a frame to no bound port is counted, not delivered
    a.SetFingerprint(kFp + 1);
    a.Stop();
    CHECK(a.Start());
    const int as2 = a.OpenSocket();
    a.Bind(as2, 2000);
    a.SendTo(as2, VLink::kBroadcastVip, 3000, "y", 1);
    CHECK(b.RecvFrom(bs, buf, sizeof buf, 0, 0) == -1 && b.GetStats().droppedNoSocket == 1);
    // a console without a backend or with an unusable address does not start
    VLink::Link c;
    CHECK(!c.Start());
    Endpoint* ez = AddEndpoint(hub, 0);
    c.SetBackend(&ez->backend);
    CHECK(!c.Start());
}

static void TestImpairment() {
    Hub hub;
    hub.sent = hub.bcast = 0;
    Endpoint* ea = AddEndpoint(hub, Vip(10, 0, 0, 1));
    Endpoint* eb = AddEndpoint(hub, Vip(10, 0, 0, 2));
    VLink::Link a, b;
    a.SetBackend(&ea->backend);
    b.SetBackend(&eb->backend);
    a.SetFingerprint(kFp);
    b.SetFingerprint(kFp);
    a.SetClock(FakeNow, 0);
    CHECK(a.Start() && b.Start());
    const int as = a.OpenSocket(), bs = b.OpenSocket();
    a.Bind(as, 1000);
    b.Bind(bs, 1000);
    char buf[16];

    VLink::Impairment imp;
    memset(&imp, 0, sizeof imp);
    imp.delayMs = 30;
    a.SetImpairment(imp, 7);
    g_now = 1000;
    a.SendTo(as, VLink::kBroadcastVip, 1000, "d", 1);
    CHECK(b.RecvFrom(bs, buf, sizeof buf, 0, 0) == -1);  // still in the air
    g_now = 1029;
    a.Pump();
    CHECK(b.RecvFrom(bs, buf, sizeof buf, 0, 0) == -1);
    g_now = 1030;
    a.Pump();
    CHECK(b.RecvFrom(bs, buf, sizeof buf, 0, 0) == 1);

    imp.delayMs = 0;
    imp.dup = 1.0;  // every frame twice
    a.SetImpairment(imp, 7);
    a.SendTo(as, VLink::kBroadcastVip, 1000, "u", 1);
    CHECK(b.RecvFrom(bs, buf, sizeof buf, 0, 0) == 1 && b.RecvFrom(bs, buf, sizeof buf, 0, 0) == 1 &&
          b.RecvFrom(bs, buf, sizeof buf, 0, 0) == -1);

    imp.dup = 0;
    imp.drop = 1.0;  // everything lost, and the sender is told it was sent
    a.SetImpairment(imp, 7);
    CHECK(a.SendTo(as, VLink::kBroadcastVip, 1000, "l", 1) == 1);
    CHECK(b.RecvFrom(bs, buf, sizeof buf, 0, 0) == -1);

    // loss rate roughly as asked
    imp.drop = 0.25;
    a.SetImpairment(imp, 99);
    int got = 0;
    for (int i = 0; i < 2000; ++i) {
        a.SendTo(as, VLink::kBroadcastVip, 1000, "r", 1);
        while (b.RecvFrom(bs, buf, sizeof buf, 0, 0) >= 0) ++got;
    }
    CHECK(got > 1350 && got < 1650);
}

// ---- wave 5 NET: the instrumentation counters, the lost state, the impairment clock's ReleaseDue ------------------------------------------
static void TestInstrumentationAndLost() {
    Hub hub;
    Endpoint* ea = AddEndpoint(hub, Vip(10, 0, 0, 1));
    Endpoint* eb = AddEndpoint(hub, Vip(10, 0, 0, 2));
    VLink::Link a, b;
    a.SetBackend(&ea->backend);
    b.SetBackend(&eb->backend);
    a.SetFingerprint(kFp);
    b.SetFingerprint(kFp);
    CHECK(a.Start() && b.Start());
    const int as = a.OpenSocket(), bs = b.OpenSocket();
    a.Bind(as, 1000);
    b.Bind(bs, 1000);
    char buf[64];
    // three datagrams, not read yet: the backend sent three frames (timed), b's queue held three at most
    CHECK(a.SendTo(as, VLink::kBroadcastVip, 1000, "one", 3) == 3);
    CHECK(a.SendTo(as, VLink::kBroadcastVip, 1000, "three", 5) == 5);
    CHECK(a.SendTo(as, VLink::kBroadcastVip, 1000, "two", 3) == 3);
    VLink::Stats sa = a.GetStats();
    CHECK(sa.backendSends == 3 && sa.sendUsTotal < 3000000u && sa.sendUsMax < 1000000u);
    b.Pump();
    CHECK(b.GetStats().queueHighWater == 3);
    VLink::WindowStats wa = a.TakeWindow(), wb = b.TakeWindow();
    CHECK(wa.payloadSentMax == 5 && wb.queueHighWater == 3 && wb.payloadReceivedMax == 5);
    wa = a.TakeWindow();
    wb = b.TakeWindow();
    CHECK(wa.payloadSentMax == 0 && wa.sendUsMax == 0 && wb.queueHighWater == 0 && wb.payloadReceivedMax == 0);  // a new window
    while (b.RecvFrom(bs, buf, sizeof buf, 0, 0) >= 0) {
    }
    // lost: every send fails (the game's socket-error path), counted; Start clears it, SetLost(false) as well
    CHECK(!a.IsLost());
    a.SetLost(true, "test");
    CHECK(a.IsLost() && a.SendTo(as, VLink::kBroadcastVip, 1000, "x", 1) == -1 && a.GetStats().sendsRefusedLost == 1);
    CHECK(b.RecvFrom(bs, buf, sizeof buf, 0, 0) == -1);
    CHECK(b.SendTo(bs, Vip(10, 0, 0, 1), 1000, "y", 1) == 1 && a.RecvFrom(as, buf, sizeof buf, 0, 0) == 1);  // receiving still works
    a.SetLost(false, 0);
    CHECK(!a.IsLost() && a.SendTo(as, VLink::kBroadcastVip, 1000, "z", 1) == 1 && b.RecvFrom(bs, buf, sizeof buf, 0, 0) == 1);
    a.SetLost(true, "test again");
    a.Stop();
    CHECK(a.Start() && !a.IsLost());
    // ReleaseDue: a delayed frame leaves at its time without the sender sending or reading anything (the desktop's impairment clock thread)
    const int as2 = a.OpenSocket();
    a.Bind(as2, 1000);
    a.SetClock(FakeNow, 0);
    VLink::Impairment imp;
    memset(&imp, 0, sizeof imp);
    imp.delayMs = 40;
    a.SetImpairment(imp, 3);
    g_now = 5000;
    CHECK(a.SendTo(as2, VLink::kBroadcastVip, 1000, "late", 4) == 4);
    const int sentBefore = hub.sent;
    g_now = 5039;
    a.ReleaseDue();
    CHECK(hub.sent == sentBefore && b.RecvFrom(bs, buf, sizeof buf, 0, 0) == -1);
    g_now = 5040;
    a.ReleaseDue();
    CHECK(hub.sent == sentBefore + 1 && b.RecvFrom(bs, buf, sizeof buf, 0, 0) == 4);
}

// ---- fragmentation: a medium that carries fewer bytes than a link frame (the Vita's ad hoc PDP: SCE_NET_ADHOC_PDP_MFS = 1444) ----------------
static std::vector<uint8_t> Pattern(size_t n, uint8_t seed) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = uint8_t(seed + i * 7 + (i >> 8));
    return v;
}
static bool GetsBack(VLink::Link& b, int socket, const std::vector<uint8_t>& want) {
    std::vector<uint8_t> got(2048);
    uint32_t src = 0;
    uint16_t sport = 0;
    const int n = b.RecvFrom(socket, &got[0], got.size(), &src, &sport);
    return n == int(want.size()) && memcmp(&got[0], &want[0], want.size()) == 0;
}
static void TestFragmentation() {
    const size_t kMfs = 1444;  // SCE_NET_ADHOC_PDP_MFS
    Hub hub;
    Endpoint* ea = AddEndpoint(hub, Vip(10, 0, 0, 1));
    Endpoint* eb = AddEndpoint(hub, Vip(10, 0, 0, 2));
    ea->backend.maxFrame = eb->backend.maxFrame = kMfs;
    VLink::Link a, b;
    a.SetBackend(&ea->backend);
    b.SetBackend(&eb->backend);
    a.SetFingerprint(kFp);
    b.SetFingerprint(kFp);
    a.SetClock(FakeNow, 0);
    b.SetClock(FakeNow, 0);
    g_now = 5000;
    CHECK(a.Start() && b.Start());
    const int as = a.OpenSocket(), bs = b.OpenSocket();
    a.Bind(as, 1000);
    b.Bind(bs, 1000);
    const uint32_t bvip = Vip(10, 0, 0, 2);

    // what fits goes whole: the game's own largest datagram (0x587 = 1415 bytes) plus the 24-byte frame header is 1439 <= 1444
    size_t sizes[] = {1, 100, 1000, 1415, 1420};
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; ++i) {
        std::vector<uint8_t> p = Pattern(sizes[i], uint8_t(i));
        CHECK(a.SendTo(as, bvip, 1000, &p[0], p.size()) == int(p.size()));
        CHECK(GetsBack(b, bs, p));
    }
    CHECK(a.GetStats().framesSplit == 0 && a.GetStats().maxBackendFrame == 24 + 1420 && hub.maxLen == 24 + 1420);
    CHECK(a.GetStats().maxPayloadSent == 1420);

    // a byte more, up to the link's own maximum (1472): split in two, put together, nothing longer than the medium's limit on the wire
    size_t big[] = {1421, 1430, 1472};
    for (size_t i = 0; i < sizeof big / sizeof big[0]; ++i) {
        std::vector<uint8_t> p = Pattern(big[i], uint8_t(40 + i));
        CHECK(a.SendTo(as, bvip, 1000, &p[0], p.size()) == int(p.size()));
        CHECK(GetsBack(b, bs, p));
    }
    CHECK(a.GetStats().framesSplit == 3 && a.GetStats().fragmentsSent == 6 && b.GetStats().framesReassembled == 3);
    CHECK(hub.maxLen <= kMfs);
    CHECK(a.GetStats().maxPayloadSent == 1472 && a.GetStats().maxBackendFrame == kMfs);
    // a broadcast is split too, and a reply to the learned peer goes the other way
    std::vector<uint8_t> bc = Pattern(1450, 9);
    a.SendTo(as, VLink::kBroadcastVip, 1000, &bc[0], bc.size());
    CHECK(GetsBack(b, bs, bc));
    uint32_t src = 0;
    uint16_t sport = 0;
    CHECK(b.RecvFrom(bs, &bc[0], 0, &src, &sport) == -1);
    std::vector<uint8_t> back = Pattern(1460, 77);
    CHECK(b.SendTo(bs, Vip(10, 0, 0, 1), 1000, &back[0], back.size()) == int(back.size()));
    CHECK(GetsBack(a, as, back));

    // fragments reordered, duplicated: still one datagram, exactly once
    hub.mode = 2;
    std::vector<uint8_t> p = Pattern(1472, 3);
    a.SendTo(as, bvip, 1000, &p[0], p.size());
    CHECK(GetsBack(b, bs, p));
    hub.mode = 1;
    p = Pattern(1466, 4);
    a.SendTo(as, bvip, 1000, &p[0], p.size());
    CHECK(GetsBack(b, bs, p));
    CHECK(b.RecvFrom(bs, &p[0], p.size(), 0, 0) == -1);   // the duplicates did not make a second datagram
    // a lost fragment loses the datagram (as a lost UDP datagram would): nothing delivered, nothing stuck, the next one is fine
    hub.mode = 3;
    hub.dropNth = hub.fragSeen;   // the first fragment of the next datagram
    p = Pattern(1472, 5);
    a.SendTo(as, bvip, 1000, &p[0], p.size());
    CHECK(b.RecvFrom(bs, &p[0], p.size(), 0, 0) == -1);
    hub.mode = 0;
    std::vector<uint8_t> after = Pattern(1450, 6);
    a.SendTo(as, bvip, 1000, &after[0], after.size());
    CHECK(GetsBack(b, bs, after));
    CHECK(b.GetStats().droppedFragments == 0);            // the partial ones are still waiting for their other half ...
    g_now += VLink::kReassemblyMs + 1;
    after = Pattern(1451, 8);
    a.SendTo(as, bvip, 1000, &after[0], after.size());
    CHECK(GetsBack(b, bs, after));
    CHECK(b.GetStats().droppedFragments == 2);            // ... and are dropped when their time is up: the lost half, and the stray duplicate of the doubled run
    // eight datagrams lose their first half: the four reassembly slots recycle, nothing grows
    hub.mode = 3;
    for (int i = 0; i < 8; ++i) {
        hub.dropNth = hub.fragSeen + 1;   // the second fragment is lost, the first one is held
        a.SendTo(as, bvip, 1000, &p[0], p.size());
        g_now += 10;
    }
    CHECK(b.RecvFrom(bs, &p[0], p.size(), 0, 0) == -1);
    hub.mode = 0;

    // garbage that looks like a fragment: bad counts, offsets past the end, a total beyond the link's maximum, an empty piece
    const uint64_t droppedBefore = b.GetStats().droppedFragments;
    uint8_t g[64];
    memset(g, 0, sizeof g);
    g[0] = 'M'; g[1] = 'S'; g[2] = 'L'; g[3] = 'F';
    struct { int index, count, offset, total, len; } bad[] = {
        {0, 1, 0, 100, 40}, {5, 2, 0, 100, 40}, {0, 9, 0, 100, 40}, {0, 2, 90, 100, 40}, {0, 2, 0, 2000, 40}, {0, 2, 0, 10, 40}, {0, 2, 0, 100, 12},
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        g[4] = 0; g[5] = uint8_t(i + 1);
        g[6] = uint8_t(bad[i].index); g[7] = uint8_t(bad[i].count);
        g[8] = uint8_t(bad[i].offset >> 8); g[9] = uint8_t(bad[i].offset);
        g[10] = uint8_t(bad[i].total >> 8); g[11] = uint8_t(bad[i].total);
        eb->rx.push_back(std::vector<uint8_t>(g, g + bad[i].len));
        eb->rxFrom.push_back(1);
    }
    b.Pump();
    CHECK(b.GetStats().droppedFragments - droppedBefore == sizeof bad / sizeof bad[0]);
    std::vector<uint8_t> fine = Pattern(1472, 11);
    a.SendTo(as, bvip, 1000, &fine[0], fine.size());
    CHECK(GetsBack(b, bs, fine));
    // a fragment of a console with another fingerprint is dropped when the frame completes (counted as a fingerprint drop, not delivered)
    a.SetFingerprint(kFp ^ 0x55);
    a.SendTo(as, bvip, 1000, &fine[0], fine.size());
    CHECK(b.RecvFrom(bs, &fine[0], fine.size(), 0, 0) == -1 && b.GetStats().droppedFingerprint == 1);
    a.Stop();
    b.Stop();

    // no limit (the Wi-Fi and desktop backends): whole frames as before
    Hub hub2;
    Endpoint* ec = AddEndpoint(hub2, Vip(10, 0, 0, 3));
    Endpoint* ed = AddEndpoint(hub2, Vip(10, 0, 0, 4));
    VLink::Link c, d;
    c.SetBackend(&ec->backend);
    d.SetBackend(&ed->backend);
    c.SetFingerprint(kFp);
    d.SetFingerprint(kFp);
    CHECK(c.Start() && d.Start());
    const int cs = c.OpenSocket(), ds = d.OpenSocket();
    c.Bind(cs, 1000);
    d.Bind(ds, 1000);
    p = Pattern(1472, 2);
    c.SendTo(cs, Vip(10, 0, 0, 4), 1000, &p[0], p.size());
    CHECK(GetsBack(d, ds, p) && hub2.maxLen == 24 + 1472 && c.GetStats().framesSplit == 0);
}

// The native and the recompiled build must never meet in one match: their fingerprints differ ("MSCLAN/2 R4QE01 native full-records" against the
// recompiled port's "MSCLAN/2 R4QE01 full-records"), and a console ignores frames of the other one.
static uint32_t Fnv(const char* t) {
    uint32_t h = 2166136261u;
    for (; *t; ++t) h = (h ^ uint8_t(*t)) * 16777619u;
    return h;
}
static void TestFingerprintRefusal() {
    const uint32_t native = Fnv("MSCLAN/2 R4QE01 native full-records"), recompiled = Fnv("MSCLAN/2 R4QE01 full-records");
    const uint32_t nativeNk = Fnv("MSCLAN/2 R4QE01 native full-records nk-fix");
    CHECK(native != recompiled && native != nativeNk && recompiled != nativeNk);
    Hub hub;
    Endpoint* ea = AddEndpoint(hub, Vip(10, 0, 0, 1));
    Endpoint* eb = AddEndpoint(hub, Vip(10, 0, 0, 2));
    VLink::Link a, b;
    a.SetBackend(&ea->backend);
    b.SetBackend(&eb->backend);
    a.SetFingerprint(native);
    b.SetFingerprint(recompiled);
    CHECK(a.Start() && b.Start());
    const int as = a.OpenSocket(), bs = b.OpenSocket();
    a.Bind(as, 1001);
    b.Bind(bs, 1001);
    char buf[8];
    a.SendTo(as, VLink::kBroadcastVip, 1001, "lobby", 5);
    b.SendTo(bs, VLink::kBroadcastVip, 1001, "lobby", 5);
    CHECK(a.RecvFrom(as, buf, sizeof buf, 0, 0) == -1 && b.RecvFrom(bs, buf, sizeof buf, 0, 0) == -1);
    CHECK(a.GetStats().droppedFingerprint == 1 && b.GetStats().droppedFingerprint == 1);
    // and the NK-bug setting: a console with the fix and one without do not pair either
    a.SetFingerprint(native);
    b.SetFingerprint(nativeNk);
    b.SendTo(bs, VLink::kBroadcastVip, 1001, "lobby", 5);
    CHECK(a.RecvFrom(as, buf, sizeof buf, 0, 0) == -1 && a.GetStats().droppedFingerprint == 2);
}

// ---- the SO API over the POSIX UDP backend, two processes -----------------------------------------------------------
static int g_pipeToParent[2], g_pipeToChild[2];

static void Say(int fd, const char* fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    const ssize_t r = write(fd, line, strlen(line));
    (void)r;
}
static bool WaitFor(int fd, const char* word, int ms) {
    char acc[512] = {0};
    size_t used = 0;
    for (int waited = 0; waited < ms; waited += 5) {
        char c;
        while (read(fd, &c, 1) == 1) {
            if (used < sizeof acc - 1) acc[used++] = c;
            if (strstr(acc, word)) return true;
        }
        usleep(5000);
    }
    return false;
}

// One console: opens the game's two sockets (direct 1000, broadcast 1001) and talks. role 0 = host, 1 = joiner.
static int Console(int role, int ownRead, int peerWrite) {
    char port[16], bcast[64], vip[32];
    snprintf(port, sizeof port, "%d", 41500 + role);
    snprintf(bcast, sizeof bcast, "127.0.0.1:%d", 41500 + (1 - role));
    snprintf(vip, sizeof vip, "10.0.0.%d", role + 1);
    setenv("MSC_LINK_PORT", port, 1);
    setenv("MSC_LINK_BCAST", bcast, 1);
    setenv("MSC_LINK_VIP", vip, 1);
    SOLibraryConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    CHECK(SOSocket2(SO_PF_INET, SO_SOCK_DGRAM, 0) == SO_ENETDOWN);  // before SOStartup
    CHECK(SOStartup() == SO_EINVAL);                                // before SOInit
    CHECK(SOInit(&cfg) == 0);
    CHECK(SOStartup() == 0);
    const int direct = SOSocket2(SO_PF_INET, SO_SOCK_DGRAM, 0), bcastSock = SOSocket2(SO_PF_INET, SO_SOCK_DGRAM, 0);
    CHECK(direct > 0 && bcastSock > 0 && direct != bcastSock);
    CHECK(SOSocket2(SO_PF_INET, SO_SOCK_STREAM, 0) == SO_EPROTONOSUPPORT);
    // TransportSocketBind: SOSockAddrIn { sizeof, SO_PF_INET, htons(port), INADDR_ANY }
    SOSockAddrIn a1 = {sizeof(SOSockAddrIn), SO_PF_INET, SOHtoNs(1000), {0}}, a2 = {sizeof(SOSockAddrIn), SO_PF_INET, SOHtoNs(1001), {0}};
    CHECK(SOBind(direct, &a1) == 0 && SOBind(bcastSock, &a2) == 0);
    CHECK(SOBind(bcastSock, &a1) == SO_EADDRINUSE);
    CHECK(SOBind(99, &a1) == SO_EBADF);
    int fl = SOFcntl(direct, SO_F_GETFL, 0);
    CHECK(fl == 0 && SOFcntl(direct, SO_F_SETFL, fl | SO_O_NONBLOCK) == 0 && (SOFcntl(direct, SO_F_GETFL, 0) & SO_O_NONBLOCK));
    CHECK(SOConnect(direct, &a1) == SO_EOPNOTSUPP && SOSend(direct, "x", 1, 0) == SO_EOPNOTSUPP);
    // NetworkSocket::AcquireLocalAddress: the four bytes of the address
    const long host = SOGetHostID();
    const uint8_t* hb = reinterpret_cast<const uint8_t*>(&host);
    CHECK(hb[0] == 10 && hb[1] == 0 && hb[2] == 0 && hb[3] == role + 1);
    char buf[128];
    SOSockAddrIn from = {sizeof(SOSockAddrIn), SO_PF_INET, 0, {0}};
    CHECK(SORecvFrom(direct, buf, sizeof buf, 0, &from) == SO_EAGAIN);  // nothing yet

    Say(peerWrite, "up ");
    if (!WaitFor(ownRead, "up", 5000)) return 90;
    if (role == 0) {
        // LANLobby: broadcast on 1001 (TransportSocketBroadcast), then wait for the answer on the direct socket
        SOSockAddrIn bc = {sizeof(SOSockAddrIn), SO_PF_INET, SOHtoNs(1001), {0xFFFFFFFFu}};
        bool answered = false;
        for (int i = 0; i < 400 && !answered; ++i) {
            CHECK(SOSendTo(bcastSock, "HOST?", 5, 0, &bc) == 5);
            usleep(5000);
            from.port = 0;
            const int n = SORecvFrom(direct, buf, sizeof buf, 0, &from);
            if (n == 6 && memcmp(buf, "JOIN!!", 6) == 0) {
                answered = true;
                const uint8_t* fa = reinterpret_cast<const uint8_t*>(&from.addr.addr);
                CHECK(fa[0] == 10 && fa[1] == 0 && fa[2] == 0 && fa[3] == 2);  // a.b.c.d in memory order
                CHECK(SONtoHs(from.port) == 1000);                             // the joiner's direct socket
                CHECK(from.family == SO_PF_INET && from.len == sizeof(SOSockAddrIn));
                // a direct send back to the address as received (TransportSocketSendTo)
                SOSockAddrIn to = {sizeof(SOSockAddrIn), SO_PF_INET, SOHtoNs(1000), from.addr};
                CHECK(SOSendTo(direct, "START", 5, 0, &to) == 5);
            }
        }
        CHECK(answered);
        Say(peerWrite, "host-done ");
        CHECK(WaitFor(ownRead, "join-done", 5000));
    } else {
        bool heard = false;
        for (int i = 0; i < 1000 && !heard; ++i) {
            from.port = 0;
            const int n = SORecvFrom(bcastSock, buf, sizeof buf, 0, &from);
            if (n == 5 && memcmp(buf, "HOST?", 5) == 0) {
                heard = true;
                const uint8_t* fa = reinterpret_cast<const uint8_t*>(&from.addr.addr);
                CHECK(fa[0] == 10 && fa[1] == 0 && fa[2] == 0 && fa[3] == 1);
                SOSockAddrIn to = {sizeof(SOSockAddrIn), SO_PF_INET, SOHtoNs(1000), from.addr};
                CHECK(SOSendTo(direct, "JOIN!!", 6, 0, &to) == 6);
            } else {
                usleep(5000);
            }
        }
        CHECK(heard);
        bool started = false;
        for (int i = 0; i < 1000 && !started; ++i) {
            const int n = SORecvFrom(direct, buf, sizeof buf, 0, 0);
            if (n == 5 && memcmp(buf, "START", 5) == 0) started = true;
            else usleep(5000);
        }
        CHECK(started);
        Say(peerWrite, "join-done ");
        CHECK(WaitFor(ownRead, "host-done", 5000));
    }
    CHECK(SOClose(direct) == 0 && SOClose(direct) == SO_EBADF && SOClose(bcastSock) == 0);
    CHECK(SOCleanup() == 0 && SOFinish() == 0);
    CHECK(SOSocket2(SO_PF_INET, SO_SOCK_DGRAM, 0) == SO_ENETDOWN);  // after SOCleanup
    return g_failed ? 1 : 0;
}

static void TestSoTwoProcesses() {
    CHECK(pipe(g_pipeToParent) == 0 && pipe(g_pipeToChild) == 0);
    // non-blocking read ends
    const int readEnds[2] = {g_pipeToParent[0], g_pipeToChild[0]};
    for (int i = 0; i < 2; ++i) fcntl(readEnds[i], F_SETFL, fcntl(readEnds[i], F_GETFL, 0) | O_NONBLOCK);
    pid_t pid = fork();
    if (pid == 0) {
        _exit(Console(1, g_pipeToChild[0], g_pipeToParent[1]));
    }
    const int rc = Console(0, g_pipeToParent[0], g_pipeToChild[1]);
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(rc == 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int main() {
    TestLinkBasics();
    TestLinkRejects();
    TestImpairment();
    TestFragmentation();
    TestFingerprintRefusal();
    TestInstrumentationAndLost();
    TestSoTwoProcesses();
    printf("net_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
