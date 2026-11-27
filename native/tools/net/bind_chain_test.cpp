// np_bind_chain.h: the UDP bind fallback chain against a fake bind that behaves like the Vita (refuses >= 32768 with 0x8041010D, plus listed ports).
// g++ -std=c++11 -I. [-DBIND_CHAIN_HEADER='"path"'] tools/net/bind_chain_test.cpp -o /tmp/bind_chain_test && /tmp/bind_chain_test
#ifdef BIND_CHAIN_HEADER
#include BIND_CHAIN_HEADER
#else
#include "platform/sdk/net/np_bind_chain.h"
#endif
#include <cstdio>
#include <set>
#include <vector>
using namespace NpNet;
static const int kEacces = int(0x8041010Du) < 0 ? int(0x8041010Du) : -int(0x8041010Du);   // as a negative sceNet code
struct Fake {
    std::set<int> refuse;   // exact ports refused
    bool refuseHigh = true; // >= 32768 refused (EACCES)
    bool refuseZero = false;
    std::vector<int> tried, failed;
    int sysPort = 62901;
};
static int fakeBind(void* c, uint16_t port) {
    Fake& f = *static_cast<Fake*>(c);
    f.tried.push_back(port);
    if (port == 0) return f.refuseZero ? -2 : f.sysPort;
    if ((f.refuseHigh && port >= 32768) || f.refuse.count(port)) return kEacces;
    return 0;
}
static void onFail(void* c, uint16_t port, int) { static_cast<Fake*>(c)->failed.push_back(port); }
int main() {
    int bad = 0;
    auto eq = [&](long a, long b, const char* w) { if (a != b) { std::printf("FAIL %s: %ld != %ld\n", w, a, b); ++bad; } };
    // defaults are below 32768, distinct from each other, from Galaxy's 24100-24110, and the chains do not overlap
    eq(kLanDefaultPort < 32768 && kOnlineDefaultPort < 32768, 1, "defaults below 32768");
    eq(kLanDefaultPort + kLanPortTries - 1 < 32768 && kOnlineDefaultPort + kOnlinePortTries - 1 < 32768, 1, "whole chains below 32768");
    eq(kLanDefaultPort + kLanPortTries <= kOnlineDefaultPort, 1, "LAN and online chains do not overlap");
    eq(kLanDefaultPort > 24110 && kOnlineDefaultPort > 24110, 1, "clear of Galaxy's 24100-24110");
    // first try works
    { Fake f; BindResult r = BindChain(fakeBind, &f, kLanDefaultPort, kLanPortTries, true, onFail);
      eq(r.ok, 1, "default binds"); eq(r.requested, kLanDefaultPort, "default port bound"); eq(r.attempts, 1, "one attempt"); eq(r.firstError, 0, "no error"); eq(f.failed.size(), 0, "no failure logged"); }
    // the old default 41000: refused, then 41001.. all refused, then the system's port
    { Fake f; BindResult r = BindChain(fakeBind, &f, 41000, 8, true, onFail);
      eq(r.ok, 1, "41000 chain ends ok"); eq(r.requested, 0, "falls to port 0"); eq(r.attempts, 9, "8 refused + port 0"); eq(r.firstError, kEacces, "first error is EACCES"); eq(f.failed.size(), 8, "8 refusals logged"); eq(f.tried.back(), 0, "last try is 0"); }
    // 65535 end: no wrap past 65535
    { Fake f; BindResult r = BindChain(fakeBind, &f, 65534, 8, true, onFail);
      eq(f.tried.size(), 3, "65534, 65535, then 0 (no wrap)"); eq(f.tried[0], 65534, "starts at 65534"); eq(f.tried[1], 65535, "then 65535"); eq(r.requested, 0, "ends at port 0"); }
    { Fake f; BindResult r = BindChain(fakeBind, &f, 65534, 8, false, onFail);
      eq(r.ok, 0, "no fallback at the top of the range: fails, no wrap to port 0"); eq(f.tried.size(), 2, "only 65534 and 65535 tried"); }
    // a taken first port moves to the next
    { Fake f; f.refuse = {25100}; BindResult r = BindChain(fakeBind, &f, 25100, 4, true, onFail);
      eq(r.requested, 25101, "25101 after 25100 refused"); eq(r.attempts, 2, "two attempts"); eq(r.firstError, kEacces, "first error kept"); eq(f.failed.size(), 1, "one refusal logged"); }
    // specific ports refused: the chain picks the first that works, in order
    { Fake f; f.refuse = {25110, 25111, 25113}; BindResult r = BindChain(fakeBind, &f, 25110, 8, true, onFail);
      eq(r.requested, 25112, "25112 is the first free"); eq(f.failed.size(), 2, "two refusals before it"); }
    { Fake f; f.refuse = {25110, 25111, 25112, 25113, 25114, 25115, 25116}; BindResult r = BindChain(fakeBind, &f, 25110, 8, true, onFail);
      eq(r.requested, 25117, "the last of the range still counts (7 refused, the 8th binds)"); eq(r.attempts, 8, "eight attempts, no port 0"); }
    // the whole range refused: port 0 when allowed, failure when not
    { Fake f; for (int p = 25110; p < 25118; ++p) f.refuse.insert(p); BindResult r = BindChain(fakeBind, &f, 25110, 8, true, onFail);
      eq(r.ok, 1, "range refused: port 0"); eq(r.requested, 0, "system choice"); eq(r.attempts, 9, "nine attempts"); }
    { Fake f; for (int p = 25110; p < 25118; ++p) f.refuse.insert(p); BindResult r = BindChain(fakeBind, &f, 25110, 8, false, onFail);
      eq(r.ok, 0, "no fallback: fails"); eq(r.attempts, 8, "eight attempts"); eq(r.firstError, kEacces, "error kept"); }
    // port 0 itself refused: a clean failure with the first error
    { Fake f; f.refuseZero = true; BindResult r = BindChain(fakeBind, &f, 41000, 2, true, onFail);
      eq(r.ok, 0, "port 0 refused: fail"); eq(r.firstError, kEacces, "first error is the first port's"); eq(f.failed.size(), 3, "three refusals logged"); }
    // first = 0: the system's choice outright, one try
    { Fake f; BindResult r = BindChain(fakeBind, &f, 0, 8, false, onFail);
      eq(r.ok, 1, "first 0 binds"); eq(r.requested, 0, "requested 0"); eq(r.attempts, 1, "one attempt"); eq(f.tried.size(), 1, "one bind call"); }
    // a Vita that refuses nothing below 32768: the Same Wi-Fi default and the online default both bind first time
    { Fake f; eq(BindChain(fakeBind, &f, kLanDefaultPort, kLanPortTries, true).requested, kLanDefaultPort, "LAN default");
      Fake g; eq(BindChain(fakeBind, &g, kOnlineDefaultPort, kOnlinePortTries, true).requested, kOnlineDefaultPort, "online default"); }
    if (!bad) std::printf("bind_chain_test: all checks passed\n");
    return bad ? 1 : 0;
}
