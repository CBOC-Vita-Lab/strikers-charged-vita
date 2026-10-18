// The Wii SDK's SO socket API for the native port, over VLink (the virtual LAN link of vlink.h).
//
// The game's own LAN mode calls only this part of SO: SocketNetworkStartup (SOInit, SOStartup),
// SocketNetworkShutdown (SOCleanup, SOFinish), the TransportSocket wrappers (SOSocket2, SOBind, SOClose, SOFcntl,
// SOSendTo, SORecvFrom; SOConnect/SOSend only for the unused connected-UDP path), NetworkSocket::AcquireLocalAddress
// (SOGetHostID) and the byte-order helpers. Everything is UDP, non-blocking in effect (a receive with nothing queued
// returns SO_EAGAIN, as a non-blocking SO socket does), IPv4 only. Stream sockets, name resolution, DHCP and the
// Nintendo online services are not provided: the Wi-Fi Connection flow stays unavailable (the menu entry that used
// to start it starts the LAN lobby instead, see np/docs/multiplayer.md).
//
// Addresses cross this interface as the SDK has them: SOSockAddrIn::addr is a u32 whose four bytes in memory are
// a.b.c.d (network order), SOSockAddrIn::port is network order; VLink's vip is a.b.c.d in host order.
#include <revolution/so.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "np/gamefixes.h"
#include "platform/sdk/net/vlink.h"

#if !defined(NP_TARGET_VITA)
#include <pthread.h>
#include <unistd.h>
#endif

#define SO_LOG(...) fprintf(stderr, "[so-net] " __VA_ARGS__)

namespace {

// Compatibility fingerprint of this console's LAN protocol: link frames, whole 16-byte input records (the
// np/patches/mp patches), LAN-mode rules. A recompiled-port console has a different one (it does not simulate
// bit-identically) and is ignored. MSC_LINK_FINGERPRINT=<hex> overrides it in desktop test runs only.
const char kProtocol[] = "MSCLAN/2 R4QE01 native full-records";

uint32_t Fnv1a(const void* data, size_t len) {
    uint32_t h = 2166136261u;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) h = (h ^ p[i]) * 16777619u;
    return h;
}

bool g_initialized = false;
bool g_started = false;
int g_fcntlFlags[VLink::kMaxSockets + 1];  // per handle: SO_O_NONBLOCK as set (receives never block either way)

uint32_t NetToVip(u32 networkOrder) {  // bytes a.b.c.d in memory -> host-order a<<24|b<<16|c<<8|d
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&networkOrder);
    return uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3];
}
u32 VipToNet(uint32_t vip) {
    u32 out;
    uint8_t* b = reinterpret_cast<uint8_t*>(&out);
    b[0] = uint8_t(vip >> 24);
    b[1] = uint8_t(vip >> 16);
    b[2] = uint8_t(vip >> 8);
    b[3] = uint8_t(vip);
    return out;
}

bool IsLittleEndian() {
    const u16 probe = 1;
    return *reinterpret_cast<const uint8_t*>(&probe) == 1;
}

VLink::Link& L() { return VLink::Default(); }

// The settings that change what a match computes belong in the fingerprint: two consoles that disagree on them would simulate different
// matches. Today that is the NK-bug fix of MSC_GAME_FIXES (np/gamefixes.h: "in a LAN match every console must use the same setting"; the
// lobby does not compare it): a console with it on and one with it off ignore each other (the lobby does not list the game) instead of starting
// a match that desyncs. The other options (pacing, Performance Mode, the picture, the other two fixes) only change what is drawn. The default
// settings give the plain protocol string, i.e. the same fingerprint as before.
uint32_t ComputeFingerprint() {
    char text[96];
    snprintf(text, sizeof text, "%s%s", kProtocol, NpGameFix(NP_FIX_NK_BUG) ? " nk-fix" : "");
    uint32_t fingerprint = Fnv1a(text, strlen(text));
#if !defined(NP_TARGET_VITA)
    if (const char* v = getenv("MSC_LINK_FINGERPRINT")) fingerprint = uint32_t(strtoul(v, 0, 16));
#endif
    return fingerprint;
}

#if !defined(NP_TARGET_VITA)
// Desktop test impairment (MSC_LINK_DELAY_MS / _JITTER_MS): a delayed frame used to leave only when its console next touched the link (its
// next send or its end-of-frame drain), so the delay a test asked for was rounded up to the sender's frame grid, and a console that sends
// more often (MSC_LAN_SENDNOW) got a shorter one. This thread releases due frames every millisecond: the delay is wall-clock time, as a
// network's is. MSC_LINK_DELAY_THREAD=0 brings the old behaviour back (wave 3's impaired pairs ran with it).
volatile bool g_delayClockStop = false;
bool g_delayClockRunning = false;
pthread_t g_delayClock;
void* DelayClock(void*) {
    while (!g_delayClockStop) {
        L().ReleaseDue();
        usleep(1000);
    }
    return 0;
}
void StopDelayClock() {
    if (!g_delayClockRunning) return;
    g_delayClockStop = true;
    pthread_join(g_delayClock, 0);
    g_delayClockRunning = false;
}
void StartDelayClock() {
    if (g_delayClockRunning) return;
    if (pthread_create(&g_delayClock, 0, DelayClock, 0) != 0) {
        SO_LOG("impairment clock thread could not start: delays follow the console's own sends\n");
        return;
    }
    g_delayClockRunning = true;
    atexit(StopDelayClock);   // registered after the link object was built: runs before the link's destructor
    SO_LOG("impairment clock thread on (delayed frames leave on time; MSC_LINK_DELAY_THREAD=0 for the old behaviour)\n");
}
#endif

struct FingerprintInit {
    FingerprintInit() {
        uint32_t fingerprint = ComputeFingerprint();
#if !defined(NP_TARGET_VITA)
        if (const char* v = getenv("MSC_LINK_TRACE")) L().SetTrace(atoi(v));
        VLink::Impairment imp;
        memset(&imp, 0, sizeof imp);
        if (const char* v = getenv("MSC_LINK_DROP")) imp.drop = atof(v) / 100.0;
        if (const char* v = getenv("MSC_LINK_DUP")) imp.dup = atof(v) / 100.0;
        if (const char* v = getenv("MSC_LINK_DELAY_MS")) imp.delayMs = uint32_t(atof(v));
        if (const char* v = getenv("MSC_LINK_JITTER_MS")) imp.jitterMs = uint32_t(atof(v));
        if (imp.drop > 0 || imp.dup > 0 || imp.delayMs || imp.jitterMs) L().SetImpairment(imp, 0);
        const char* clock = getenv("MSC_LINK_DELAY_THREAD");
        if ((imp.delayMs || imp.jitterMs) && !(clock && clock[0] == '0')) StartDelayClock();
#endif
        L().SetFingerprint(fingerprint);
    }
} g_fingerprintInit;

}  // namespace

extern "C" {

// ---- byte order ---------------------------------------------------------------------------------------------
u32 SOHtoNl(u32 v) { return IsLittleEndian() ? __builtin_bswap32(v) : v; }
u32 SONtoHl(u32 v) { return IsLittleEndian() ? __builtin_bswap32(v) : v; }
u16 SOHtoNs(u16 v) { return IsLittleEndian() ? __builtin_bswap16(v) : v; }
u16 SONtoHs(u16 v) { return IsLittleEndian() ? __builtin_bswap16(v) : v; }

// ---- library lifetime ---------------------------------------------------------------------------------------
int SOInit(SOLibraryConfig*) {
    if (g_initialized) return SO_EALREADY;
    g_initialized = true;
    return SO_SUCCESS;
}

int SOStartup(void) {
    if (!g_initialized) return SO_EINVAL;
    if (g_started) return SO_SUCCESS;
#if !defined(NP_TARGET_VITA)
    // Desktop tests: no radio to bring up, the UDP backend comes from MSC_LINK_* when nothing was installed.
    if (!L().GetBackend()) L().SetBackend(VLink::CreateUdpBackendFromEnvironment());
#endif
    L().SetFingerprint(ComputeFingerprint());   // the game's settings may have been decided since the static initialisers ran
    if (!L().Start()) {
        SO_LOG("SOStartup: no usable link (is the radio up?)\n");
        return SO_ENETDOWN;
    }
    g_started = true;
    memset(g_fcntlFlags, 0, sizeof g_fcntlFlags);
    return SO_SUCCESS;
}

int SOStartupEx(int) { return SOStartup(); }

int SOCleanup(void) {
    if (g_started) L().Stop();
    g_started = false;
    return SO_SUCCESS;
}

int SOFinish(void) {
    g_initialized = false;
    return SO_SUCCESS;
}

int SOiIsInitialized(void) { return g_initialized ? 1 : 0; }

// ---- sockets ------------------------------------------------------------------------------------------------
int SOSocket2(int pf, int type, int) {
    if (!g_started) return SO_ENETDOWN;
    if (pf != SO_PF_INET) return SO_EAFNOSUPPORT;
    if (type != SO_SOCK_DGRAM) return SO_EPROTONOSUPPORT;  // no stream sockets on the link
    const int handle = L().OpenSocket();
    if (handle < 0) return SO_EMFILE;
    g_fcntlFlags[handle] = 0;
    return handle;
}

int SOSocket(int pf, int type, int protocol) { return SOSocket2(pf, type, protocol); }

int SOClose(int s) {
    if (!L().IsOpen(s)) return SO_EBADF;
    L().Close(s);
    g_fcntlFlags[s] = 0;
    return SO_SUCCESS;
}

int SOBind(int s, const void* sockAddr) {
    if (!L().IsOpen(s)) return SO_EBADF;
    if (!sockAddr) return SO_EFAULT;
    const SOSockAddrIn* in = static_cast<const SOSockAddrIn*>(sockAddr);
    if (in->family != SO_PF_INET) return SO_EAFNOSUPPORT;
    // The address (INADDR_ANY in the game) selects nothing here: the link has one address.
    return L().Bind(s, SONtoHs(in->port)) ? SO_SUCCESS : SO_EADDRINUSE;
}

int SOFcntl(int s, int cmd, ...) {
    if (!L().IsOpen(s)) return SO_EBADF;
    if (cmd == SO_F_GETFL) return g_fcntlFlags[s];
    if (cmd == SO_F_SETFL) {
        // The variadic argument is read by the caller's convention (an int); only the non-blocking flag exists.
        va_list args;
        va_start(args, cmd);
        const int flags = va_arg(args, int);
        va_end(args);
        g_fcntlFlags[s] = flags & SO_O_NONBLOCK;
        return SO_SUCCESS;
    }
    return SO_EINVAL;
}

int SOConnect(int s, const void*) {  // connected UDP belongs to the stream/DWC paths: refused visibly
    if (!L().IsOpen(s)) return SO_EBADF;
    SO_LOG("SOConnect is not supported on the link\n");
    return SO_EOPNOTSUPP;
}

int SOSend(int s, const void*, int, int) {
    if (!L().IsOpen(s)) return SO_EBADF;
    SO_LOG("SOSend (connected UDP) is not supported on the link\n");
    return SO_EOPNOTSUPP;
}

int SOSendTo(int s, const void* buf, int len, int, const void* sockTo) {
    if (!L().IsOpen(s)) return SO_EBADF;
    if (len < 0 || (len && !buf) || !sockTo) return SO_EFAULT;
    if (size_t(len) > VLink::kMaxPayload) return SO_EMSGSIZE;
    const SOSockAddrIn* to = static_cast<const SOSockAddrIn*>(sockTo);
    const uint32_t vip = NetToVip(to->addr.addr);
    const int sent = L().SendTo(s, vip, SONtoHs(to->port), buf, size_t(len));
    return sent < 0 ? SO_ENETDOWN : sent;
}

int SORecvFrom(int s, void* buf, int len, int, void* sockFrom) {
    if (!L().IsOpen(s)) return SO_EBADF;
    if (len < 0 || (len && !buf)) return SO_EFAULT;
    uint32_t vip = 0;
    uint16_t port = 0;
    const int n = L().RecvFrom(s, buf, size_t(len), &vip, &port);
    if (n < 0) return SO_EAGAIN;
    if (sockFrom) {
        SOSockAddrIn* from = static_cast<SOSockAddrIn*>(sockFrom);
        from->len = sizeof(SOSockAddrIn);
        from->family = SO_PF_INET;
        from->port = SOHtoNs(port);
        from->addr.addr = VipToNet(vip);
    }
    return n;
}

int SORecv(int s, void* buf, int len, int flags) { return SORecvFrom(s, buf, len, flags, 0); }

int SOGetSockName(int s, void* sockAddr) {
    if (!L().IsOpen(s)) return SO_EBADF;
    if (!sockAddr) return SO_EFAULT;
    SOSockAddrIn* in = static_cast<SOSockAddrIn*>(sockAddr);
    in->len = sizeof(SOSockAddrIn);
    in->family = SO_PF_INET;
    in->port = SOHtoNs(L().LocalPort(s));
    in->addr.addr = VipToNet(L().LocalVip());
    return SO_SUCCESS;
}

// NetworkSocket::AcquireLocalAddress: this console's address, four bytes a.b.c.d in memory.
long SOGetHostID(void) { return long(VipToNet(L().LocalVip())); }

}  // extern "C"
