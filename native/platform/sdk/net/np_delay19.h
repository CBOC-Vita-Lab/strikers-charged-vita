#pragma once
// np_delay19.h (wave 11, helper delay19): pure decisions for the LAN / online input delay and the starved-step recovery. No clock, no sockets, C++11, so
// tools/net/delay19_test.cpp runs this very code on the host.
//
// SESSION FLOOR. The host's input delay (np_lan.cpp g_agreedDelay, carried in the game-start message and stored by every console) is the starting value and
// the floor of the router's congestion multiplier, so the delay the host chose is the delay that holds; congestion still raises it (to the max) and the decay
// stops at the choice. The multiplier is changed in one place only (NetworkInputRouter::ProcessMessages), by the `congested` flag of each POPPED input message,
// and every console pops the same messages of every machine in the same order: its value is a pure function of (floor, max, step, flags popped so far), so
// it is identical on all consoles. The floor is identical because every console stores the same byte from the same game-start message. 0 = no choice:
// the game's own constants (start 4.0, floor 2.0) as before.
//
//   MSC_LAN_DRAIN     (default 0) 1 = once a frame before the step loop, read the datagrams that already arrived (the receive loop only; never the transport's Update).
//   MSC_LAN_RETRY_US  (default 0) N > 0 = a step that would starve waits up to N microseconds a frame (cap 8000) for the other console's input, receiving only.
// Both change only WHEN a step runs, never what it computes: every console still applies the same inputs in the same order.
#include <cstdlib>

namespace NpDelay19 {

// The floor of the multiplier for a session: the agreed delay, else the game's own minimum.
inline float SessionFloor(int agreed, float gameMin) { return agreed >= 1 ? float(agreed) : gameMin; }
// Its starting value: the agreed delay, else the game's default start.
inline float SessionStart(int agreed, float gameDefault) { return agreed >= 1 ? float(agreed) : gameDefault; }
// The ceiling: the game's, but never below the floor (an agreed 12 must not be cut to 10 by the first congested tick).
inline float SessionMax(int agreed, float gameMax) { return agreed >= 1 && float(agreed) > gameMax ? float(agreed) : gameMax; }
// One popped message: the multiplier after it.
inline float Step(float mult, bool congested, float floorV, float maxV, float inc, float dec) {
    if (congested) { const float v = mult + inc; return v > maxV ? maxV : v; }
    const float v = mult - dec;
    return v < floorV ? floorV : v;
}

inline bool ParseDrain(const char* v) { return v && v[0] && !(v[0] == '0' && v[1] == 0); }
inline int ParseRetryUs(const char* v) {
    if (!v || !v[0]) return 0;
    const int n = std::atoi(v);
    return n < 0 ? 0 : (n > 8000 ? 8000 : n);
}
inline bool Drain() { static const bool v = ParseDrain(std::getenv("MSC_LAN_DRAIN")); return v; }
inline int RetryUs() { static const int v = ParseRetryUs(std::getenv("MSC_LAN_RETRY_US")); return v; }

}  // namespace NpDelay19
