// Vita backends of VLink for the native port (vlink.h) and the platform hooks of np_lan.h (NpLanRadioPoll/Stop,
// NpLanPlayerName). The same code, with the renamed link, runs in the recompiled port's Vita platform layer (vita
// repo, branch mp-one, src/vita_lan.cpp).
//
// Two ways for consoles to reach each other:
//   * Nearby (default, MSC_LAN_MODE=adhoc): PSP-compatible ad hoc Wi-Fi, no router. The radio is
//     brought up by the state machine in vlink_vita_radio.cpp, which shows the system NetCheck dialog
//     through the renderer. Frames travel as PDP datagrams
//     between MAC addresses.
//   * Same Wi-Fi (MSC_LAN_MODE=wifi): plain UDP on the access point the Vita is already connected
//     to (also what Vita3K offers). Broadcast finds the other console.
// Only datagrams cross here. The game's own LAN protocol (lobby, reliability, lockstep) runs above.
#if defined(NP_TARGET_VITA)  // the Vita target only (the np Makefile compiles every platform/ source for every target)

#include "platform/sdk/net/vlink.h"
#include "platform/sdk/net/np_lan.h"
#include "platform/sdk/net/vlink_vita_radio.h"

#include <psp2/apputil.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/power.h>
#include <psp2/net/netctl.h>
#include <psp2/pspnet_adhoc.h>
#include <psp2/sysmodule.h>
#include <psp2/system_param.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#define VLV_LOG(...) std::fprintf(stderr, "[vlink-vita] " __VA_ARGS__)

namespace {

enum class Mode { Adhoc, Wifi };

Mode ChosenMode()
{
    const char* mode = std::getenv("MSC_LAN_MODE");
    return (mode && std::strcmp(mode, "wifi") == 0) ? Mode::Wifi : Mode::Adhoc;
}

// ---- ad hoc PDP backend ----------------------------------------------------------------------
uint32_t AdhocVip(void*)
{
    uint8_t mac[6];
    if (!np_vita_radio_link_mac(mac)) return 0;
    // 10.x.y.z from the low MAC bytes: stable per console, unique between two nearby Vitas.
    uint32_t vip = 0x0A000000u | uint32_t(mac[3]) << 16 | uint32_t(mac[4]) << 8 | mac[5];
    if ((vip & 0xFF) == 0 || (vip & 0xFF) == 0xFF) vip ^= 0x80;
    return vip;
}

int AdhocSend(void*, const VLink::PeerAddr* to, bool broadcast, const void* data, size_t len)
{
    return np_vita_radio_link_send(broadcast || !to ? nullptr : to->bytes, data, len);
}

int AdhocRecv(void*, VLink::PeerAddr* from, void* buffer, size_t capacity)
{
    std::memset(from, 0, sizeof *from);
    return np_vita_radio_link_recv(from->bytes, buffer, capacity);
}

void AdhocClose(void*) {}  // the radio is closed by MscLanRadioStop

// PDP carries at most SCE_NET_ADHOC_PDP_MFS = 1444 bytes per datagram; the game's datagrams (up to 1,415) plus the link's 24-byte frame header
// (up to 1,496 with the largest the link accepts) are split into two PDP datagrams by the link when they do not fit (vlink.cpp SendFrame).
const VLink::Backend kAdhoc = {"adhoc", 0, AdhocVip, AdhocSend, AdhocRecv, AdhocClose, SCE_NET_ADHOC_PDP_MFS};

// ---- Wi-Fi UDP backend -----------------------------------------------------------------------
struct Wifi {
    bool moduleLoaded = false, netOwned = false, netctlOwned = false;
    void* pool = nullptr;
    int socket = -1;
    uint16_t port = 41000;
    uint32_t vip = 0;                 // this console's IPv4 (host order)
    uint32_t targets[8] = {};         // broadcast addresses (host order)
    uint16_t targetPorts[8] = {};
    int targetCount = 0;
} g_wifi;

uint32_t ParseIp(const char* text)
{
    uint32_t parts[4];
    if (std::sscanf(text, "%u.%u.%u.%u", &parts[0], &parts[1], &parts[2], &parts[3]) != 4) return 0;
    for (uint32_t p : parts)
        if (p > 255) return 0;
    return parts[0] << 24 | parts[1] << 16 | parts[2] << 8 | parts[3];
}

void WifiClose(void*)
{
    if (g_wifi.socket >= 0) sceNetSocketClose(g_wifi.socket);
    g_wifi.socket = -1;
}

bool WifiOpen()
{
    if (g_wifi.socket >= 0) return true;
    if (sceSysmoduleIsLoaded(SCE_SYSMODULE_NET) != SCE_SYSMODULE_LOADED) {
        if (sceSysmoduleLoadModule(SCE_SYSMODULE_NET) < 0) return false;
        g_wifi.moduleLoaded = true;
    }
    // MSC_LAN_WIFI_RCVBUF=<KiB> (wave 5 NET, default 0 = the system's default): a larger receive buffer on the Wi-Fi socket, as VitAdhoc's
    // transport_lan sets (256 KiB), so no datagram is dropped by the stack while the game thread is inside a long frame (the socket is drained
    // once a frame). The net pool grows by the same amount so the buffer cannot starve the stack. Not measured on a console: off by default.
    const int rcvKiB = [] { const char* v = std::getenv("MSC_LAN_WIFI_RCVBUF"); const int k = v ? std::atoi(v) : 0; return k < 0 ? 0 : (k > 1024 ? 1024 : k); }();
    if (!g_wifi.netOwned) {
        const int kPool = 256 * 1024 + rcvKiB * 1024;
        g_wifi.pool = std::malloc(kPool);
        SceNetInitParam param{};
        param.memory = g_wifi.pool;
        param.size = kPool;
        const int rc = sceNetInit(&param);
        if (rc >= 0) {
            g_wifi.netOwned = true;
        } else {
            std::free(g_wifi.pool);
            g_wifi.pool = nullptr;
            if (uint32_t(rc) != uint32_t(SCE_NET_ERROR_EBUSY)) {
                VLV_LOG("sceNetInit failed 0x%08X\n", rc);
                return false;
            }
        }
    }
    if (!g_wifi.netctlOwned) {
        const int rc = sceNetCtlInit();
        if (rc >= 0) g_wifi.netctlOwned = true;
        else if (uint32_t(rc) != 0x80412102u) {
            VLV_LOG("sceNetCtlInit failed 0x%08X\n", rc);
            return false;
        }
    }
    int state = 0;
    if (sceNetCtlInetGetState(&state) < 0 || state != SCE_NETCTL_STATE_CONNECTED) {
        VLV_LOG("Wi-Fi is not connected (state %d)\n", state);
        return false;
    }
    SceNetCtlInfo ip{}, mask{};
    if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &ip) < 0) return false;
    if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_NETMASK, &mask) < 0) std::strcpy(mask.netmask, "255.255.255.0");
    g_wifi.vip = ParseIp(ip.ip_address);
    // Two emulators on one computer report the same IP: MSC_LINK_VIP gives each its own virtual address (as the desktop UDP backend).
    if (const char* forced = std::getenv("MSC_LINK_VIP"); forced && *forced) {
        if (const uint32_t v = ParseIp(forced)) g_wifi.vip = v;
    }
    const uint32_t netmask = ParseIp(mask.netmask);
    if (!g_wifi.vip) return false;
    if (const char* port = std::getenv("MSC_LINK_PORT")) g_wifi.port = uint16_t(std::atoi(port));
    // Broadcast targets: the subnet broadcast and 255.255.255.255 on our port, plus any extra
    // "ip:port" list in MSC_LINK_BCAST (two emulator instances on one computer use two ports).
    g_wifi.targetCount = 0;
    const auto add = [](uint32_t ip, uint16_t port) {
        if (g_wifi.targetCount < 8) {
            g_wifi.targets[g_wifi.targetCount] = ip;
            g_wifi.targetPorts[g_wifi.targetCount++] = port;
        }
    };
    if (const char* list = std::getenv("MSC_LINK_BCAST"); list && *list) {
        char copy[256];
        std::snprintf(copy, sizeof copy, "%s", list);
        for (char* item = std::strtok(copy, ","); item; item = std::strtok(nullptr, ",")) {
            char* colon = std::strrchr(item, ':');
            uint16_t port = g_wifi.port;
            if (colon) {
                *colon = 0;
                port = uint16_t(std::atoi(colon + 1));
            }
            if (const uint32_t target = ParseIp(item)) add(target, port);
        }
    } else {
        add(g_wifi.vip | ~netmask, g_wifi.port);
        add(0xFFFFFFFFu, g_wifi.port);
    }
    g_wifi.socket = sceNetSocket("msc_lan", SCE_NET_AF_INET, SCE_NET_SOCK_DGRAM, 0);
    if (g_wifi.socket < 0) {
        VLV_LOG("socket failed 0x%08X\n", g_wifi.socket);
        g_wifi.socket = -1;
        return false;
    }
    int one = 1;
    sceNetSetsockopt(g_wifi.socket, SCE_NET_SOL_SOCKET, SCE_NET_SO_BROADCAST, &one, sizeof one);
    sceNetSetsockopt(g_wifi.socket, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &one, sizeof one);
    if (rcvKiB > 0) {
        int bytes = rcvKiB * 1024;
        const int rc = sceNetSetsockopt(g_wifi.socket, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVBUF, &bytes, sizeof bytes);
        int now = 0;
        unsigned int len = sizeof now;
        sceNetGetsockopt(g_wifi.socket, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVBUF, &now, &len);
        VLV_LOG("Wi-Fi receive buffer %d KiB asked: %s (0x%08X), now %d bytes\n", rcvKiB, rc >= 0 ? "set" : "refused", uint32_t(rc), now);
    }
    SceNetSockaddrIn local{};
    local.sin_len = sizeof local;
    local.sin_family = SCE_NET_AF_INET;
    local.sin_port = sceNetHtons(g_wifi.port);
    local.sin_addr.s_addr = 0;
    if (sceNetBind(g_wifi.socket, reinterpret_cast<SceNetSockaddr*>(&local), sizeof local) < 0) {
        VLV_LOG("bind to UDP port %u failed\n", g_wifi.port);
        WifiClose(nullptr);
        return false;
    }
    VLV_LOG("Wi-Fi link on %s port %u (%d broadcast targets)\n", ip.ip_address, g_wifi.port, g_wifi.targetCount);
    return true;
}

uint32_t WifiVip(void*) { return g_wifi.vip; }

int WifiSendOne(uint32_t ip, uint16_t port, const void* data, size_t len)
{
    SceNetSockaddrIn to{};
    to.sin_len = sizeof to;
    to.sin_family = SCE_NET_AF_INET;
    to.sin_port = sceNetHtons(port);
    to.sin_addr.s_addr = __builtin_bswap32(ip);
    return sceNetSendto(g_wifi.socket, data, unsigned(len), 0, reinterpret_cast<SceNetSockaddr*>(&to), sizeof to);
}

int WifiSend(void*, const VLink::PeerAddr* to, bool broadcast, const void* data, size_t len)
{
    if (g_wifi.socket < 0) return -1;
    if (!broadcast && to) {
        uint32_t ip;
        uint16_t port;
        std::memcpy(&ip, to->bytes, 4);
        std::memcpy(&port, to->bytes + 4, 2);
        return WifiSendOne(ip, port, data, len);
    }
    int best = -1;
    for (int i = 0; i < g_wifi.targetCount; ++i) {
        if (g_wifi.targets[i] == g_wifi.vip && g_wifi.targetPorts[i] == g_wifi.port) continue;
        const int n = WifiSendOne(g_wifi.targets[i], g_wifi.targetPorts[i], data, len);
        if (n >= 0) best = n;
    }
    return best;
}

int WifiRecv(void*, VLink::PeerAddr* from, void* buffer, size_t capacity)
{
    if (g_wifi.socket < 0) return 0;
    SceNetSockaddrIn src{};
    unsigned int srcLen = sizeof src;
    const int n = sceNetRecvfrom(g_wifi.socket, buffer, unsigned(capacity), SCE_NET_MSG_DONTWAIT,
                                 reinterpret_cast<SceNetSockaddr*>(&src), &srcLen);
    if (n < 0) return uint32_t(n) == uint32_t(SCE_NET_ERROR_EAGAIN) ? 0 : (uint32_t(n) == 0x80410104u ? 0 : -1);
    std::memset(from, 0, sizeof *from);
    const uint32_t ip = __builtin_bswap32(src.sin_addr.s_addr);
    const uint16_t port = __builtin_bswap16(src.sin_port);
    std::memcpy(from->bytes, &ip, 4);
    std::memcpy(from->bytes + 4, &port, 2);
    return n;
}

const VLink::Backend kWifi = {"wifi", 0, WifiVip, WifiSend, WifiRecv, WifiClose};

Mode g_active = Mode::Adhoc;
bool g_failedReported = false;
bool g_stopping = false;   // NpLanRadioStop is closing the radio: not a loss

// ---- link watch and resume (wave 5 NET, MSC_LAN_LINKWATCH, default 1) -----------------------------------------------------------
// Once per presented frame on the game thread (np_vita_radio_pump calls np_vita_lan_frame first):
//   * Wi-Fi: sceNetCtlInetGetState once a second; two looks in a row that are not CONNECTED end the link (VitAdhoc's 2-strike watch).
//   * Nearby: the radio closes itself when its cell is gone (vlink_vita_radio.cpp, the same 2-strike rule); that is reported here.
//   * Resume: an application is not told before it is suspended, only after it comes back (VitAdhoc's finding); the power callback
//     (delivered by sceKernelCheckCallback on this thread) reports the resume, and a LAN session is then ended (np_lan NpLanOnResume).
// Each loss goes to NpLanLinkLost: every link send fails from then on and the game ends the session through its own socket-error path,
// with its own "connection lost" popup, instead of a frozen match. The 20 s keep-alive limit is unchanged (the partner's side).
bool LinkWatchOn() {
    static const int on = [] { const char* v = std::getenv("MSC_LAN_LINKWATCH"); return v && v[0] == '0' ? 0 : 1; }();
    return on != 0;
}
volatile uint32_t g_powerInfo = 0;
volatile bool g_resumed = false;
SceUID g_powerCb = -1;
int PowerCallback(int, int, int info, void*) {
    g_powerInfo = uint32_t(info);
    if (info & (SCE_POWER_CB_APP_RESUME | SCE_POWER_CB_SYSTEM_RESUME | SCE_POWER_CB_SYSTEM_RESUMING | SCE_POWER_CB_APP_RESUMING |
                SCE_POWER_CB_AFTER_SYSTEM_RESUME))
        g_resumed = true;
    return 0;
}
uint64_t g_nextWifiLookUs = 0;
int g_wifiStrikes = 0;
bool g_lossReported = false;
NpRadioPhase g_lastPhase = NP_RADIO_DOWN;

}  // namespace

extern "C" void np_vita_lan_frame(void) {
    if (!LinkWatchOn()) return;
    const bool wifiUp = g_active == Mode::Wifi && g_wifi.socket >= 0;
    NpRadioSnapshot snap{};
    np_vita_radio_snapshot(&snap);
    const bool radioUp = g_active == Mode::Adhoc && snap.phase != NP_RADIO_DOWN;
    if (!wifiUp && !radioUp) {
        g_lastPhase = snap.phase;
        return;
    }
    // The power callback lives on this (the game) thread: created once, delivered here.
    if (g_powerCb < 0) {
        g_powerCb = sceKernelCreateCallback("msc_lan_power", 0, PowerCallback, nullptr);
        const int rc = g_powerCb >= 0 ? scePowerRegisterCallback(g_powerCb) : g_powerCb;
        VLV_LOG("power callback %s (0x%08X): a resume from sleep ends a LAN session\n", rc >= 0 ? "registered" : "NOT registered", uint32_t(rc));
        if (rc < 0 && g_powerCb >= 0) { sceKernelDeleteCallback(g_powerCb); g_powerCb = -2; }
    }
    if (g_powerCb >= 0) sceKernelCheckCallback();
    if (g_resumed) {
        g_resumed = false;
        VLV_LOG("resumed from sleep (power notice 0x%08X)\n", uint32_t(g_powerInfo));
        NpLanOnResume();
    }
    if (radioUp) {
        // The radio closed by itself (cell gone, receive error) while nobody asked it to.
        if (g_lastPhase == NP_RADIO_OPEN && snap.phase != NP_RADIO_OPEN && !g_stopping && !g_lossReported) {
            g_lossReported = true;
            NpLanLinkLost("the ad hoc radio closed: the cell is gone");
        }
        g_lastPhase = snap.phase;
        return;
    }
    const uint64_t now = sceKernelGetProcessTimeWide();
    if (now < g_nextWifiLookUs) return;
    g_nextWifiLookUs = now + 1000000;
    int state = 0;
    const int rc = sceNetCtlInetGetState(&state);
    if (rc >= 0 && state == SCE_NETCTL_STATE_CONNECTED) {
        g_wifiStrikes = 0;
        return;
    }
    if (++g_wifiStrikes == 2 && !g_lossReported) {
        g_lossReported = true;
        VLV_LOG("Wi-Fi is not connected (state %d, 0x%08X) at two looks a second apart\n", state, uint32_t(rc));
        NpLanLinkLost("Wi-Fi lost");
    }
}

int NpLanRadioPoll(int request)
{
    if (request) {
        g_active = ChosenMode();
        g_stopping = false;
        g_lossReported = false;
        g_wifiStrikes = 0;
    }
    if (g_active == Mode::Wifi) {
        if (g_wifi.socket >= 0) return 2;
        if (!request) return 0;
        if (!WifiOpen()) return -1;
        VLink::Default().SetBackend(&kWifi);
        return 2;
    }
    NpRadioSnapshot snap{};
    np_vita_radio_snapshot(&snap);
    switch (snap.phase) {
    case NP_RADIO_OPEN:
        VLink::Default().SetBackend(&kAdhoc);
        g_failedReported = false;
        return 2;
    case NP_RADIO_STARTING:
    case NP_RADIO_DIALOG:
        return 1;
    case NP_RADIO_CLOSING:
        return request ? 1 : 0;  // a previous session is still releasing the radio
    case NP_RADIO_DOWN:
    default:
        if (!request) return 0;
        if (snap.status != NP_RADIO_OK && snap.status != NP_RADIO_AGAIN && !g_failedReported && snap.stage) {
            // The previous attempt ended (cancelled dialog, no partner radio, error).
            g_failedReported = true;
            VLV_LOG("ad hoc start ended at %s (status %d, error 0x%08X)\n", snap.stage, int(snap.status), snap.sce_error);
            return -1;
        }
        g_failedReported = false;
        const NpRadioStatus status = np_vita_radio_begin_link();
        if (status == NP_RADIO_AGAIN) return 1;
        VLV_LOG("ad hoc start refused (%d)\n", int(status));
        return -1;
    }
}

void NpLanRadioStop()
{
    g_stopping = true;
    if (g_active == Mode::Wifi) {
        WifiClose(nullptr);
        return;
    }
    NpRadioSnapshot snap{};
    np_vita_radio_snapshot(&snap);
    VLV_LOG("ad hoc closing: %u received datagram(s) dropped by the radio ring, %u send(s) refused for exceeding the PDP maximum of %d bytes\n",
            snap.rx_dropped, snap.oversize, int(SCE_NET_ADHOC_PDP_MFS));
    np_vita_radio_close();
    g_failedReported = true;  // a later DOWN after this close is not a new failure
}

const char* NpLanPlayerName()
{
    static char name[SCE_SYSTEM_PARAM_USERNAME_MAXSIZE] = {};
    if (!name[0] && sceAppUtilSystemParamGetString(SCE_SYSTEM_PARAM_ID_USERNAME, reinterpret_cast<SceChar8*>(name), sizeof name) < 0)
        name[0] = 0;
    return name;
}

#endif  // NP_TARGET_VITA
