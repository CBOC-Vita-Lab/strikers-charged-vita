// rung25 MSC_LOCK60_RUNG_PREMAKE: the bookkeeping of making every picture-size rung's objects ahead (outside play) and never freeing them in a match. Header-only and pure (no GL):
// tests/rungpremake.cpp runs the shipped rules. Render only: nothing here reaches the simulation, the random numbers or the network.
//   - Plan: which rungs to walk at load. All of them when the free video memory covers every one of them (plus the floor kept free); else only the next rung below the current one; else none.
//   - Ledger: each rung is made once per match (a second make is counted as a remake), a free in the middle of a match is a violation (counted, and the test fails on it), the match end frees all.
#pragma once
#include <cstdint>

namespace vgx {
namespace rungpremake {

// Levels to walk (in the order given: the rungs a scene needs first), written to `out`. tour: the levels of the tour order; cur: the level in use (not walked: its objects are in use already);
// freeKib < 0: unknown (no objection). estKib: the video memory one rung's objects are expected to take; floorKib: what must stay free after all of them.
// *nextOnly: the memory covered only the next rung below `cur` (the smallest level number above cur among the tour's levels).
inline int Plan(const int* tour, int tourN, int cur, long freeKib, long floorKib, long estKib, int* out, int cap, bool* nextOnly) {
    int cand[16], n = 0;
    for (int i = 0; i < tourN && n < 16; ++i) {
        bool dup = false;
        for (int j = 0; j < n; ++j) if (cand[j] == tour[i]) dup = true;
        if (tour[i] != cur && !dup) cand[n++] = tour[i];
    }
    if (nextOnly) *nextOnly = false;
    int m = 0;
    if (freeKib < 0 || freeKib >= floorKib + estKib * n) {
        for (int i = 0; i < n && m < cap; ++i) out[m++] = cand[i];
        return m;
    }
    int next = -1;
    for (int i = 0; i < n; ++i) if (cand[i] > cur && (next < 0 || cand[i] < next)) next = cand[i];
    if (next >= 0 && freeKib >= floorKib + estKib && cap > 0) {
        out[m++] = next;
        if (nextOnly) *nextOnly = true;
    }
    return m;
}

struct Ledger {
    static constexpr int kMax = 16;
    uint8_t made[kMax] = {};
    uint32_t makes = 0, remakes = 0, midFrees = 0, endFrees = 0, steps = 0, stepsMaking = 0;
    bool inMatch = false;
    void BeginMatch() { for (uint8_t& m : made) m = 0; inMatch = true; }
    // a rung's objects are made (true: the first time this match)
    bool Make(int level) {
        if (level < 0 || level >= kMax) return false;
        ++makes;
        if (made[level]) { ++remakes; return false; }
        made[level] = 1;
        return true;
    }
    // something of a rung is freed: only the match end may
    void Free(int level) {
        if (level < 0 || level >= kMax || !made[level]) return;
        if (inMatch) ++midFrees;
    }
    // a size step: `makingNow`: it made an object
    void Step(bool makingNow) { ++steps; if (makingNow) ++stepsMaking; }
    int EndMatch() {
        int n = 0;
        for (uint8_t& m : made) if (m) { ++n; m = 0; }
        endFrees += static_cast<uint32_t>(n);
        inMatch = false;
        return n;
    }
};

} // namespace rungpremake
} // namespace vgx
