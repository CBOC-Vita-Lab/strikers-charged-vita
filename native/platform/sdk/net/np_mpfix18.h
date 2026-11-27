#pragma once
// np_mpfix18.h (wave 12, helper mpfix18): small pure decisions taken from the Galaxy port's multiplayer review. No clock of its own, no sceNet calls, C++11,
// so tools/net/mpfix18_test.cpp and tools/net/net_test.cpp run this very code on the host.
//   MSC_NET_KEEP_STACK  (default 1): after a Nearby (ad hoc) session leave sceNet / sceNetCtl / the NET module up. The online socket (np_udp_vita) and HTTPS
//                       (np_https_sys g_netUp) cache "the stack is up" for the life of the process; a sceNetTerm after a Nearby session that had been the first
//                       to initialise the stack left them with a dead stack (resolver and socket errors) until the game was restarted. 0 = the 1.2 teardown.
//   LogBucket           burst then a steady rate for log lines that a stranger on the Wi-Fi can trigger (Galaxy MP_LOG_BURST).
// Nothing here touches the wire format or the lockstep.
#include <cstdint>
#include <cstdlib>

namespace NpMpfix18 {

// "0" = off, anything else (or unset) = on.
inline bool ParseKeepStack(const char* v) { return !(v && v[0] == '0' && v[1] == 0); }
inline bool KeepStack() {
    static const bool v = ParseKeepStack(std::getenv("MSC_NET_KEEP_STACK"));
    return v;
}

// 8 lines at once, then one every 500 ms (2 a second). `now` is any millisecond clock; a clock that steps back re-anchors and never underflows.
struct LogBucket {
    uint64_t at;
    int tokens;
    bool primed;
};
inline bool LogAllow(LogBucket& b, uint64_t now, int burst = 8, uint32_t refillMs = 500) {
    if (!b.primed) { b.primed = true; b.at = now; b.tokens = burst; }
    if (now < b.at) b.at = now;   // a clock that stepped back re-anchors instead of waiting for the old time
    if (b.tokens < burst && now - b.at >= refillMs) {
        const uint64_t add = (now - b.at) / refillMs;
        b.tokens = int(uint64_t(b.tokens) + add > uint64_t(burst) ? uint64_t(burst) : uint64_t(b.tokens) + add);
        b.at = now;
    }
    if (b.tokens <= 0) return false;
    --b.tokens;
    return true;
}
// Counted lines the limiter swallowed are not logged (a log line about the missing log lines would be the flood again).

}  // namespace NpMpfix18
