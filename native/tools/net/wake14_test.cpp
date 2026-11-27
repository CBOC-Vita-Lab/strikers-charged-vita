// wake14: the Wi-Fi keep-awake filler of VLink (vlink.h SetKeepAwake, MSC_NET_KEEPAWAKE_MS) and the quiet-gap diagnostic (SetGapLog).
// Two links in one process joined by an in-memory medium, a manual clock, the real vlink.cpp. No game, no sockets.
//   clang++ -std=gnu++11 -I. platform/sdk/net/vlink.cpp tools/net/wake14_test.cpp -o /tmp/wake14_test && /tmp/wake14_test
#include "platform/sdk/net/vlink.h"
#include <stdio.h>
#include <string.h>
#include <string>
#include <deque>
#include <vector>
using namespace VLink;

static uint64_t g_now = 100000;   // ms, shared by both links
static uint64_t Clock(void*) { return g_now; }

struct Medium {
    std::deque<std::vector<uint8_t>> toA, toB;
    std::vector<uint64_t> sentAtA, sentAtB;   // when each side's frames left (the gaps are read from these)
    std::vector<size_t> lenA, lenB;
};
static Medium g_m;
struct Side { Medium* m; bool isA; };
static Side g_sa = {&g_m, true}, g_sb = {&g_m, false};
static uint32_t VipOf(void* c) { return static_cast<Side*>(c)->isA ? 0x0A000001u : 0x0A000002u; }
static int SendF(void* c, const PeerAddr*, bool, const void* d, size_t n) {
    Side* s = static_cast<Side*>(c);
    std::vector<uint8_t> v((const uint8_t*)d, (const uint8_t*)d + n);
    if (s->isA) { s->m->toB.push_back(v); s->m->sentAtA.push_back(g_now); s->m->lenA.push_back(n); }
    else { s->m->toA.push_back(v); s->m->sentAtB.push_back(g_now); s->m->lenB.push_back(n); }
    return int(n);
}
static int RecvF(void* c, PeerAddr* from, void* buf, size_t cap) {
    Side* s = static_cast<Side*>(c);
    std::deque<std::vector<uint8_t>>& q = s->isA ? s->m->toA : s->m->toB;
    if (q.empty()) return 0;
    memset(from, 0, sizeof *from);
    from->bytes[0] = s->isA ? 2 : 1;
    const size_t n = q.front().size() < cap ? q.front().size() : cap;
    memcpy(buf, q.front().data(), n);
    q.pop_front();
    return int(n);
}
static void CloseF(void*) {}
static const Backend kA = {"memA", &g_sa, VipOf, SendF, RecvF, CloseF, 0};
static const Backend kAd = {"adhoc", &g_sa, VipOf, SendF, RecvF, CloseF, 0};
static const Backend kB = {"memB", &g_sb, VipOf, SendF, RecvF, CloseF, 0};

static int bad = 0;
static void eq(long long a, long long b, const char* w) { if (a != b) { printf("FAIL %s: %lld != %lld\n", w, a, b); ++bad; } }
static void le(long long a, long long b, const char* w) { if (a > b) { printf("FAIL %s: %lld > %lld\n", w, a, b); ++bad; } }
static void ge(long long a, long long b, const char* w) { if (a < b) { printf("FAIL %s: %lld < %lld\n", w, a, b); ++bad; } }

struct Pair {
    Link a, b;
    int sa, sb;
    explicit Pair(uint32_t gapMs) {
        g_m = Medium();
        a.SetBackend(&kA); b.SetBackend(&kB);
        a.SetClock(Clock, 0); b.SetClock(Clock, 0);
        a.SetFingerprint(7); b.SetFingerprint(7);
        a.SetKeepAwake(gapMs); b.SetKeepAwake(gapMs);
        a.Start(); b.Start();
        sa = a.OpenSocket(); a.Bind(sa, 1000);
        sb = b.OpenSocket(); b.Bind(sb, 1000);
        // the game's way in: A broadcasts (a lobby), B answers A by unicast, A sends B a unicast
        a.SendTo(sa, kBroadcastVip, 1000, "hi", 2);
        char buf[64]; uint32_t v; uint16_t p;
        b.RecvFrom(sb, buf, sizeof buf, &v, &p);
        b.SendTo(sb, 0x0A000001u, 1000, "yo", 2);
        a.RecvFrom(sa, buf, sizeof buf, &v, &p);
        a.SendTo(sa, 0x0A000002u, 1000, "go", 2);
        b.RecvFrom(sb, buf, sizeof buf, &v, &p);
    }
    // run `ms` of quiet in steps of `step`, both consoles pumping (the game calls RecvFrom every frame)
    void Quiet(unsigned ms, unsigned step) {
        char buf[64]; uint32_t v; uint16_t p;
        for (unsigned t = 0; t < ms; t += step) {
            g_now += step;
            a.RecvFrom(sa, buf, sizeof buf, &v, &p);
            b.RecvFrom(sb, buf, sizeof buf, &v, &p);
        }
    }
};
static uint64_t MaxGap(const std::vector<uint64_t>& at, uint64_t from) {
    uint64_t prev = 0, worst = 0;
    for (uint64_t t : at) { if (t < from) continue; if (prev && t - prev > worst) worst = t - prev; prev = t; }
    return worst;
}

int main() {
    // 1. off: nothing is ever added
    { Pair p(0); p.a.SetGapLog(50, 0); p.b.SetGapLog(50, 0); /* (tracking on, so the peers exist) */ p.a.SendTo(p.sa, 0x0A000002u, 1000, "go", 2); const size_t before = g_m.sentAtA.size(); p.Quiet(2000, 5);
      eq(long(g_m.sentAtA.size() - before), 0, "off: no frame in 2 s of quiet"); eq((long long)p.a.GetStats().keepAwakeSent, 0, "off: no filler counted"); }
    // 2. on, 50 ms: a quiet link carries a frame at least every 50 ms (plus one step), both directions, and the filler is header only
    { Pair p(50); const uint64_t t0 = g_now; const size_t beforeA = g_m.sentAtA.size(); p.Quiet(3000, 5);
      le((long long)MaxGap(g_m.sentAtA, t0), 55, "A: no quiet spell over 50 ms (+5 step)");
      // B has had no unicast of its own other than "yo": it is a peer of A, A is a peer of B (B sent a unicast to A)
      le((long long)MaxGap(g_m.sentAtB, t0), 55, "B: no quiet spell over 50 ms (+5 step)");
      const long fillers = long(g_m.sentAtA.size() - beforeA);
      ge(fillers, 55, "about 3000/50 fillers from A"); le(fillers, 61, "and not more (the cap: 1000/gap a second per peer)");
      for (size_t i = beforeA; i < g_m.lenA.size(); ++i) if (g_m.lenA[i] != 24) { eq(long(g_m.lenA[i]), 24, "a filler is the 24-byte header"); break; }
      // the receiver counted and dropped them: nothing for the game, no 'no socket' drop, no malformed
      eq((long long)p.b.GetStats().keepAwakeReceived, (long long)p.a.GetStats().keepAwakeSent, "B counted every filler A sent");
      char buf[64]; uint32_t v; uint16_t pt;
      eq(p.b.RecvFrom(p.sb, buf, sizeof buf, &v, &pt), -1, "B's game socket holds no filler");
      eq((long long)p.b.GetStats().droppedNoSocket, 0, "no filler fell into 'no socket'"); eq((long long)p.b.GetStats().droppedMalformed, 0, "none malformed");
      eq((long long)p.b.GetStats().framesReceived, 2, "B's framesReceived is the game's own two frames only"); }
    // 3. traffic every 16 ms (a match): no filler at all
    { Pair p(50); const size_t before = g_m.sentAtA.size();
      char buf[64]; uint32_t v; uint16_t pt;
      for (int i = 0; i < 300; ++i) { g_now += 16; p.a.SendTo(p.sa, 0x0A000002u, 1000, "tick", 4); p.b.SendTo(p.sb, 0x0A000001u, 1000, "tick", 4);
          p.a.RecvFrom(p.sa, buf, sizeof buf, &v, &pt); p.b.RecvFrom(p.sb, buf, sizeof buf, &v, &pt); }
      eq(long(g_m.sentAtA.size() - before), 300, "match traffic: only the game's 300 frames from A"); eq((long long)p.a.GetStats().keepAwakeSent, 0, "match traffic: no filler from A");
      eq((long long)p.b.GetStats().keepAwakeSent, 0, "match traffic: no filler from B"); }
    // 4. the stream the game sees is the same with and without the filler (payloads, order, count), quiet spells in between
    { std::vector<std::string> seen[2];
      for (int mode = 0; mode < 2; ++mode) {
          Pair p(mode ? 50 : 0); char buf[64]; uint32_t v; uint16_t pt;
          for (int i = 0; i < 40; ++i) {
              char m[16]; snprintf(m, sizeof m, "m%d", i);
              p.a.SendTo(p.sa, 0x0A000002u, 1000, m, strlen(m)); p.Quiet((i % 4) * 70, 7);
              for (int n; (n = p.b.RecvFrom(p.sb, buf, sizeof buf, &v, &pt)) >= 0;) seen[mode].push_back(std::string(buf, buf + n));
          }
      }
      eq(long(seen[0].size()), long(seen[1].size()), "same number of datagrams for the game on / off"); eq(seen[0] == seen[1], 1, "same datagrams in the same order on / off"); }
    // 5. a peer is only kept awake while a unicast of ours reached it recently (10 s): the fillers do not renew that window
    { Pair p(50); const uint64_t t0 = g_now; p.Quiet(12000, 5);
      uint64_t last = 0; for (uint64_t t : g_m.sentAtA) last = t;
      le((long long)(last - t0), (long long)kKeepAwakePeerWindowMs + 60, "fillers stop when the peer window closes (10 s after the last real unicast)");
      ge((long long)(last - t0), (long long)kKeepAwakePeerWindowMs - 100, "and ran until then");
      const size_t n = g_m.sentAtA.size(); p.Quiet(2000, 5); eq(long(g_m.sentAtA.size()), long(n), "no filler after the window closed");
      // a new real unicast opens it again
      p.a.SendTo(p.sa, 0x0A000002u, 1000, "again", 5); p.Quiet(500, 5); ge(long(g_m.sentAtA.size() - n), 5, "fillers again after a real unicast"); }
    { Pair p(50); p.Quiet(15000, 5);   // (the unicast window has closed)
      // A has learned B and only BROADCASTS from now on (a lobby): a broadcast reaches B, but it is not a session link, so no filler
      const size_t before = g_m.sentAtA.size(); char buf[64]; uint32_t v; uint16_t pt;
      for (int i = 0; i < 100; ++i) { g_now += 30; p.a.SendTo(p.sa, kBroadcastVip, 1000, "lobby", 5); p.a.RecvFrom(p.sa, buf, sizeof buf, &v, &pt); }
      eq(long(g_m.sentAtA.size() - before), 100, "broadcasts only: no filler on top"); p.Quiet(1000, 5); eq(long(g_m.sentAtA.size() - before), 100, "and none afterwards"); }
    // 6. suspended / lost: none
    { Pair p(50); p.a.SetKeepAwakeSuspended(true); const size_t b0 = g_m.sentAtA.size(); p.Quiet(1000, 5); eq(long(g_m.sentAtA.size() - b0), 0, "suspended: no filler");
      p.a.SetKeepAwakeSuspended(false); p.Quiet(200, 5); ge(long(g_m.sentAtA.size() - b0), 1, "resumed: fillers again");
      p.a.SetLost(true, "test"); const size_t b1 = g_m.sentAtA.size(); p.Quiet(500, 5); eq(long(g_m.sentAtA.size() - b1), 0, "lost: no filler"); }
    // 7. the ad hoc radio (backend "adhoc") gets no filler
    { g_m = Medium(); Link c; c.SetBackend(&kAd); c.SetClock(Clock, 0); c.SetFingerprint(7); c.SetKeepAwake(50); c.Start();
      int s = c.OpenSocket(); c.Bind(s, 1000); char buf[64]; uint32_t v; uint16_t pt;
      // a peer announces itself (a frame from 10.0.0.2 reaches us), we unicast to it
      Link d; d.SetBackend(&kB); d.SetClock(Clock, 0); d.SetFingerprint(7); d.Start(); int ds = d.OpenSocket(); d.Bind(ds, 1000);
      d.SendTo(ds, kBroadcastVip, 1000, "hi", 2); c.RecvFrom(s, buf, sizeof buf, &v, &pt); c.SendTo(s, 0x0A000002u, 1000, "go", 2);
      const size_t before = g_m.sentAtA.size(); for (int i = 0; i < 400; ++i) { g_now += 5; c.RecvFrom(s, buf, sizeof buf, &v, &pt); }
      eq(long(g_m.sentAtA.size() - before), 0, "ad hoc backend: no filler"); }
    printf("wake14_test: %s\n", bad ? "FAILED" : "ok");
    return bad ? 1 : 0;
}
