// "Waiting for the other player..." (1.3): when the lockstep has been starved of the partner's input for a while (its console asleep, its Wi-Fi gone,
// a long stall), the match is frozen on both screens until the link's silence limit (MSC_LAN_KEEPALIVE_MS, 20 s) ends it with the connection-lost popup.
// This header is the banner's bookkeeping, header only and free of game types (tests/test_wait_banner.cpp).
//
// RENDER / UI ONLY. It reads a wall clock that the caller passes in and records two numbers of its own. It never touches the simulation, the input
// record, the random numbers or anything that is sent: NpLanStep calls Step() with the starved flag it was given, nothing flows back.
#pragma once

#include <stdint.h>

namespace NpWait {

const uint32_t kShowAfterMs = 1500;   // consecutive starved time before the banner appears
const uint32_t kFadeMs = 300;
const uint32_t kStaleMs = 400;        // a starved step is recorded every frame while waiting; no record for this long = not waiting (a pause, a stall of this console)

struct StarveClock {
    uint32_t since;   // wall time (ms, wraps) of the first starved step of the present run, 0 = none
    uint32_t last;    // wall time of the latest starved step
    StarveClock() : since(0), last(0) {}
    // One simulation tick of the match: `starved` = no input from some console. `now` is a wall clock in ms (never 0: the caller adds 1).
    void Step(bool starved, uint32_t now) {
        if (!starved) { __atomic_store_n(&since, 0u, __ATOMIC_RELAXED); return; }
        // a gap with no starved step (this console itself stalled, the app was suspended) starts a new run
        if (!__atomic_load_n(&since, __ATOMIC_RELAXED) || now - __atomic_load_n(&last, __ATOMIC_RELAXED) > kStaleMs) __atomic_store_n(&since, now, __ATOMIC_RELAXED);
        __atomic_store_n(&last, now, __ATOMIC_RELAXED);
    }
    // How long the present run of starved steps has lasted (ms), 0 when the last tick was not starved or the run is stale. Any thread.
    uint32_t Ms(uint32_t now) const {
        const uint32_t s = __atomic_load_n(&since, __ATOMIC_RELAXED), l = __atomic_load_n(&last, __ATOMIC_RELAXED);
        if (!s || now - l > kStaleMs) return 0;
        return now - s;
    }
};

// 0 until kShowAfterMs of waiting, then fades in over kFadeMs to 255.
inline int Alpha(uint32_t waitedMs) {
    if (waitedMs < kShowAfterMs) return 0;
    const uint32_t a = waitedMs - kShowAfterMs;
    return a >= kFadeMs ? 255 : int(255u * a / kFadeMs);
}

// Whole seconds waited, for the counter.
inline int Seconds(uint32_t waitedMs) { return int(waitedMs / 1000u); }

}  // namespace NpWait
