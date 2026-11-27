// UDP ports and the bind fallback chain (port17). Header only, no SDK: the Vita sockets and the host sockets share it, and tools/net/bind_chain_test.cpp
// runs it with a fake bind.
//
// Measured on the owner's Vita (Galaxy port, 23 Nov, notes-from-galaxy-perf-1123/ADDENDUM-UDP-PORTS.md): sceNetBind of a chosen UDP port from about 32768
// up fails with 0x8041010D (SCE_NET_ERROR_EACCES): 40000, 41000, 41100, 49151..65535 and 9300 refused; 1024, 2000, 5000, 8080, 20000, 30000 fine; port 0
// (the system picks, around 62900) works. So Strikers' old default 41000 could never have bound on a console. Defaults now sit below 32768 and away from
// Galaxy's 24100-24110 (so a Galaxy and a Strikers on one Wi-Fi never hear each other's broadcasts):
//   Same Wi-Fi  25100 (first try; 25101..25103 when taken; every console broadcasts to 25100, replies go to the sender's own port)
//   online      25110 (25111..25117 when taken, then port 0: STUN learns the mapping, the local candidate reads the real port back)
#pragma once
#include <stdint.h>

namespace NpNet {

const uint16_t kLanDefaultPort = 25100;      // Same Wi-Fi (VLink over UDP), MSC_LINK_PORT overrides
const int kLanPortTries = 4;                 // 25100..25103, then the system's choice
const uint16_t kOnlineDefaultPort = 25110;   // online socket, MSC_NET_PORT overrides
const int kOnlinePortTries = 8;              // 25110..25117, then the system's choice

// The bind itself: 0 or more = bound, negative = the error code (the platform's own: sceNet 0x8041010D, or -errno). `port` 0 = the system chooses.
typedef int (*BindFn)(void* ctx, uint16_t port);

struct BindResult {
    bool ok;
    uint16_t requested;   // the port that bound; 0 when the system chose (the caller reads the real one back with getsockname)
    int attempts;         // binds tried, including the one that worked
    int firstError;       // the first failure's code (0 when the first try bound)
};

// Tries first, first+1 .. first+tries-1 (never past 65535), then port 0 when anySystemPort. Stops early only when the bind succeeds.
// `first` 0 means the caller wants the system's choice outright: one try, no chain. `onFail(ctx, port, code)` is called for every refusal (logging).
inline BindResult BindChain(BindFn bind, void* ctx, uint16_t first, int tries, bool anySystemPort, void (*onFail)(void*, uint16_t, int) = 0) {
    BindResult r = {false, 0, 0, 0};
    if (first == 0) tries = 0, anySystemPort = true;
    for (int t = 0; t < tries && unsigned(first) + unsigned(t) <= 65535u; ++t) {
        const uint16_t port = uint16_t(first + t);
        const int rc = bind(ctx, port);
        ++r.attempts;
        if (rc >= 0) { r.ok = true; r.requested = port; return r; }
        if (!r.firstError) r.firstError = rc;
        if (onFail) onFail(ctx, port, rc);
    }
    if (anySystemPort) {
        const int rc = bind(ctx, 0);
        ++r.attempts;
        if (rc >= 0) { r.ok = true; r.requested = 0; return r; }
        if (!r.firstError) r.firstError = rc;
        if (onFail) onFail(ctx, 0, rc);
    }
    return r;
}

}  // namespace NpNet
