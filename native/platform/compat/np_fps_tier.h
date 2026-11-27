// np_fps_tier.h (Fable, 18 Nov 2026; MSC_LOCK60_TIERS=1 with MSC_LOCK60=1): Smooth 60 in three tiers on the fixed 60 Hz screen. Pure code (host test: native/tools/fps/tier_unit.cpp).
//   60  defended: np_fps_ctl.h Config::Lock60Tiered (isolated repeated refreshes are tolerated; a sustained 56 pictures a second or less is not), each picture's game time the vblank
//       it is shown on (np_fps45.h's clock and gate on the every-vblank pattern), so a tolerated miss is one hold of 2 refreshes with the world in the right place after it
//   45  the strict 1, 1, 2 cadence (np_fps45.h) at the picture size the ladder gives the 22.2 ms budget: what the controller's relief means in live play
//   30  the full-size locked 30: only when 45 cannot hold either (Fallback45: more than 8 % of 90 or 4 % of 360 pictures off their slot)
// Up again only on proof and in order: 30 -> 45 by Fallback45 (25 s hold, doubling; 12 s of pictures whose cost at 45 fits; three fallbacks latch the 30 for 240 s), 45 -> 60 by the
// controller's sticky rules (25 s hold, doubling; 12 s at 75 % of a refresh; latch), and 60 is not tried from the 30 (Auto::DeferUp).
// Outside live single-player play nothing changes: held scenes, menus, LAN / online / recorded games are Lock60's.
#pragma once
#include "np_fps_ctl.h"

namespace npfps {

enum Tier { kTier60 = 0, kTier45 = 1, kTier30 = 2 };

// The tier of a live-play picture: the controller's policy, and under its relief the 45 fallback's state.
inline Tier TierOf(bool policy60, bool at45) {
    if (policy60) return kTier60;
#if NP_TIER_BREAK == 4
    (void)at45;
    return kTier30;
#else
    return at45 ? kTier45 : kTier30;
#endif
}

// The [tier] line's figures.
struct TierStats {
    double secs[3] = {0, 0, 0}, winSecs[3] = {0, 0, 0};
    unsigned changes[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};   // [from][to]
    unsigned misses60 = 0, winMisses60 = 0, missesAtFall = 0, pictures = 0;
    int last = -1;
    unsigned sinceFallMisses = 0;
    // one live-play picture: its tier, how long it lasted, whether it was a repeated refresh at 60. Returns true when the tier changed.
    bool Picture(Tier t, double seconds, bool miss60) {
        ++pictures;
        if (seconds > 0.0 && seconds < 1.0) { secs[t] += seconds; winSecs[t] += seconds; }
        if (t == kTier60 && miss60) { ++misses60; ++winMisses60; ++sinceFallMisses; }
        bool changed = false;
        if (last >= 0 && last != static_cast<int>(t)) {
            ++changes[last][t];
            changed = true;
            if (last == kTier60) { missesAtFall += sinceFallMisses < 12u ? sinceFallMisses : 12u; }   // (the window that tripped it: not "tolerated")
            sinceFallMisses = 0;
        }
        last = static_cast<int>(t);
        return changed;
    }
    unsigned Tolerated() const { return misses60 - missesAtFall; }
    unsigned Changes() const { unsigned n = 0; for (int a = 0; a < 3; ++a) for (int b = 0; b < 3; ++b) n += changes[a][b]; return n; }
    void Window() { winSecs[0] = winSecs[1] = winSecs[2] = 0; winMisses60 = 0; }
};

} // namespace npfps
