#pragma once
// np_galaxy16.h (wave 11, helper galaxy16): decisions taken from the Galaxy port's multiplayer line (mp-perf radiostub, 6c8227c8 + f4265a5e, 13 Nov 2026) for the ad hoc radio
// (vlink_vita_radio.cpp). Pure: no clock, no sceNet calls, C++11, so native/tools/net/galaxy16_test.cpp runs this very code on the host.
//
// What Galaxy found when it reviewed the radio it had taken from this port (host evidence and code review only: Galaxy's multiplayer has never run on a console):
//   1. The four teardown calls that can fail because the cell is already gone (PdpDelete, AdhocctlDisconnect, AdhocctlTerm, AdhocTerm) were retried for ever, so the radio stayed
//      CLOSING and no new session could start until the game was restarted. VitAdhoc ignores the result. Three tries, then the failure is logged and the close goes on.
//   2. One PdpRecv error that is not a time-out ended the link. Now 200 of them in a row (200 x 5 ms, about a second of nothing but errors) end it.
//   3. A datagram whose source port is not kPort was dropped. The socket only receives datagrams sent to kPort and a byte-swapped source port would have dropped every frame, so
//      the filter is off; the first odd source port is noted for the log.
//   4. Log lines that name the evidence for the first console session: own MAC, PdpCreate result, the NetCheck dialog's result, the first PdpRecv error, the first odd source port,
//      and for each of the first 8 senders whether its MAC is in the peer list.
// All four are bits of MSC_LAN_RADIO_FIX (default 0 = the radio as in 1.2, byte for byte the same calls). The two dialog switches of the same Galaxy commit are
// MSC_LAN_GROUP (0: no group name, start at the "id without a group" choice) and MSC_LAN_TIMEOUT_MS (the NetCheck dialog's time-out, default 60000).
// None of them touches the game's simulation: this is the transport under the game's own lobby, and both consoles of a LAN game run the same build.
#include <cstdint>
#include <cstdlib>

namespace NpGalaxy16 {

enum RadioFixBit : unsigned { kRadioSoftClose = 1, kRadioStrayErrors = 2, kRadioAnyPort = 4, kRadioDiag = 8, kRadioAll = 15 };

// "0".."15" (decimal), anything else or out of range = 0 (off). Empty / null = 0.
inline unsigned ParseRadioFix(const char* v) {
    if (!v || !*v) return 0;
    char* end = nullptr;
    const long n = std::strtol(v, &end, 10);
    if (!end || *end || n < 0 || n > static_cast<long>(kRadioAll)) return 0;
    return static_cast<unsigned>(n);
}
inline unsigned RadioFix() {
    static const unsigned v = ParseRadioFix(std::getenv("MSC_LAN_RADIO_FIX"));
    return v;
}
inline bool RadioFixOn(unsigned bit) { return (RadioFix() & bit) != 0; }

// Integer from an environment string within [lo, hi]; anything else = fallback.
inline long ParseRange(const char* v, long fallback, long lo, long hi) {
    if (!v || !*v) return fallback;
    char* end = nullptr;
    const long n = std::strtol(v, &end, 0);
    return (end && !*end && n >= lo && n <= hi) ? n : fallback;
}
// 1 = the dialog starts with the group name (the 1.2 behaviour), 0 = without.
inline int LanGroup(const char* v) { return ParseRange(v, 1, 0, 1) ? 1 : 0; }
// The NetCheck dialog's time-out in microseconds: MSC_LAN_TIMEOUT_MS 0..3600000, default 60000 (the 1.2 value; 0 is passed as 0 = whatever the system makes of it).
inline uint32_t LanTimeoutUs(const char* v) { return static_cast<uint32_t>(ParseRange(v, 60000, 0, 3600000)) * 1000u; }

// A failed teardown call, `failures` counting this one (cleanup_failed has already counted it): true = try again later, false = give up on this call and go on closing.
inline bool SoftCloseRetry(unsigned failures) { return failures < 3; }

// `run` consecutive PdpRecv errors that are not time-outs, this one included: true = the link is over.
inline bool StrayErrorEndsLink(unsigned run) { return run >= 200; }

// Is a datagram with this source port kept? (the length and ring checks are the caller's)
inline bool PortKept(bool anyPortFix, unsigned port, unsigned wanted) { return anyPortFix || port == wanted; }

// One sender MAC against the peer list: 0 = in the list, 1 = all zero (a unicast to it fails), 2 = only the first byte differs (a unicast may fail), 3 = not in the list (yet).
inline int MacVerdict(const uint8_t* mac, const uint8_t* peers, int peerCount) {
    bool zero = true;
    for (int i = 0; i < 6; ++i) zero = zero && mac[i] == 0;
    if (zero) return 1;
    bool exact = false, tail5 = false;
    for (int k = 0; k < peerCount; ++k) {
        const uint8_t* q = peers + 6 * k;
        bool all = true, rest = true;
        for (int i = 0; i < 6; ++i) all = all && q[i] == mac[i];
        for (int i = 1; i < 6; ++i) rest = rest && q[i] == mac[i];
        if (all) exact = true;
        else if (rest) tail5 = true;
    }
    return exact ? 0 : tail5 ? 2 : 3;
}

}  // namespace NpGalaxy16
