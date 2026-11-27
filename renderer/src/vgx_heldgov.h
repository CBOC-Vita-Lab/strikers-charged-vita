// MSC_HELD_GOV (scenes30): the held-scene GPU governor (header-only so a host test runs the shipped code).
//
// Walk-outs, goal celebrations, Mega Strike launch / result cut-scenes and replays are held at a steady 30 (each picture shown two refreshes). On the console (23 Nov, s37/s41 logs, 500 MHz)
// their GPU time is 40-50 ms a picture against the 33.3 ms of the period, so 25-60 % of their pictures are shown three refreshes; the live-play policy is frozen there and the fill governor
// stands aside as soon as a frame has no large sprites. This governor acts ONLY in such a scene (the renderer's held cut-scene or replay, never live play, a Mega Strike's own scene or a menu)
// and ONLY while its GPU time is over the budget. Its levels, least visible first; each adds to the one below:
//   1  the crowd's impostor refresh floor one more picture (MSC_CROWD_REPLAY 2 -> 3: the far rows animate at 10 Hz instead of 15)
//   2  every player's shadow picture redrawn one picture less often (MSC_SHADOW_EVERY 2 -> 3: the shadow's SHAPE lags a picture more; its place follows the player every picture)
//   3  the glow chain's six blur passes every third picture (MSC_HELD_GLOW_HALF 1 -> 2; the composite reads the last chain drawn)
//   4  the picture one ladder rung below the Quality floor (MSC_HELD_GOV_SIZE names the rung, 0 = no such step: the floor is kept) - the only step that touches the size
// Down (shed): the mean (EMA) over `downAt` of the budget for `hot` pictures, or `lateN` of the last 8 pictures over the budget, takes one level; then `gap` pictures before the next move.
// Up (give back): the mean plus the level's estimated saving under `upAt` of the budget for `calm` pictures gives one level back.
// A new held scene starts at the level the last scene of its kind (cut-scene / replay) spent most pictures at (Learned), so a stadium's second goal does not pay the climb again.
// Render only: what is drawn and how often; nothing reaches the simulation, a random number or an input record.
#pragma once

#include <algorithm>

#ifndef VGX_HELDGOV_BREAK
#define VGX_HELDGOV_BREAK 0   // host test mutants (renderer/tests/held_gov.cpp): 1 sheds without the hot window, 2 never gives back, 3 gives back without the saving, 4 a new scene starts at 0, 5 the late count ignores the budget, 6 (heldcpu24) a replay with its own history is seeded from the cut-scene too
#endif

namespace vgx {
namespace heldgov {

constexpr int kMaxLevel = 4;

struct Gov {
    // configuration
    float downAt = 0.97f;   // EMA over this share of the budget is hot (32.4 ms of 33.4: the queue absorbs a picture a little over; the late count catches a steady one)
    float upAt = 0.85f;     // a level is given back when EMA + its saving stays under this share
    int hot = 8;            // hot pictures in a row that take a level (a quarter second at 30)
    int lateN = 4;          // ... or this many of the last 8 pictures over the budget itself
    int gap = 10;           // pictures between two moves (so a level's effect reaches the measured GPU time, which lags 2-3 pictures)
    int calm = 60;          // calm pictures before a level is given back (2 s)
    int maxLevel = 3;       // 3 without a size rung, 4 with (MSC_HELD_GOV_SIZE)
    float save[kMaxLevel + 1] = {0.0f, 1.7f, 2.3f, 1.1f, 2.5f};   // ms a level is estimated to buy (ESTIMATE: the next notch of each, scaled from the console's A/Bs: crowd floor 1 -> 2 ~5 ms GPU (cf18), shadows 1 -> 2 ~7 ms (ab-rep), glow half ~3 ms (held30); 864x488 -> 800x456 -14 % pixels)
    // state
    int level = 0, force = -1;
    float ema = -1.0f;
    int hotRun = 0, calmRun = 0, wait = 0;
    unsigned lateBits = 0;   // the last 8 pictures, bit set = over the budget
    unsigned in[kMaxLevel + 1] = {0, 0, 0, 0, 0};   // pictures of this scene at each level
    unsigned moves = 0, pictures = 0;
    int learned[2] = {0, 0};   // per kind (0 cut-scene, 1 replay): the level the last scene spent most pictures at
    bool known[2] = {false, false};   // heldcpu24: that kind has taught a level (a scene of 30 pictures or more ended)
    bool seedReplay = false;   // heldcpu24 (MSC_HELD_GOV_SEED=1): the first replay of a session starts at the level the cut-scenes taught (the goal's celebration the replay follows), not at 0: console s53 Vice, 151 of the replay's 229 pictures went by at level 0-2 (GPU mean 34.5 ms) before it reached level 3

    void Begin(int kind) {
        int l = learned[kind & 1];
#if VGX_HELDGOV_BREAK == 6
        if (seedReplay && (kind & 1) == 1) l = learned[0];
#else
        if (seedReplay && (kind & 1) == 1 && !known[1]) l = learned[0];
#endif
        level = force >= 0 ? std::min(force, maxLevel) : std::min(l, maxLevel);
#if VGX_HELDGOV_BREAK == 4
        level = 0;
#endif
        ema = -1.0f;
        hotRun = calmRun = 0;
        wait = gap;
        lateBits = 0;
        for (unsigned& v : in) v = 0;
        moves = pictures = 0;
    }
    // the scene's end: what it learned for the next one of its kind (the level with most pictures; ties to the higher)
    int End(int kind) {
        unsigned total = 0;
        int best = 0;
        for (int l = 0; l <= kMaxLevel; ++l) { total += in[l]; if (in[l] >= in[best]) best = l; }
        if (total >= 30 && force < 0) { learned[kind & 1] = best; known[kind & 1] = true; }
        const int was = level;
        level = 0;
        return was;
    }
    // one measured picture of the held scene: its GPU ms and the budget (one period). Returns the new level when it moved, else -1.
    int Frame(float ms, float budget) {
        ++pictures;
        ++in[std::min(std::max(level, 0), kMaxLevel)];
        if (force >= 0) { level = std::min(force, maxLevel); return -1; }
        ema = ema < 0.0f ? ms : 0.75f * ema + 0.25f * ms;
#if VGX_HELDGOV_BREAK == 5
        lateBits = ((lateBits << 1) | 1u) & 0xffu;
#else
        lateBits = ((lateBits << 1) | (ms > budget ? 1u : 0u)) & 0xffu;
#endif
        int late = 0;
        for (unsigned b = lateBits; b; b &= b - 1) ++late;
        if (ema > downAt * budget) { ++hotRun; calmRun = 0; }
        else { hotRun = 0; }
        const float back = level > 0 ? save[level] : 0.0f;
#if VGX_HELDGOV_BREAK == 3
        const bool calmNow = level > 0 && ema < upAt * budget;
#else
        const bool calmNow = level > 0 && ema + back < upAt * budget;
#endif
        if (calmNow) ++calmRun; else calmRun = 0;
        if (wait > 0) { --wait; return -1; }
#if VGX_HELDGOV_BREAK == 1
        const bool shed = ema > downAt * budget;
#else
        const bool shed = hotRun >= hot || late >= lateN;
#endif
        if (shed && level < maxLevel) {
            ++level;
            ++moves;
            hotRun = 0;
            lateBits = 0;
            wait = gap;
            return level;
        }
#if VGX_HELDGOV_BREAK != 2
        if (calmRun >= calm && level > 0) {
            --level;
            ++moves;
            calmRun = 0;
            wait = gap;
            return level;
        }
#endif
        return -1;
    }
};

// What each level means for the knobs it drives (the game reads them through VgxHeldGovLevel):
inline int CrowdFloor(int base, int level) { return level >= 1 && base > 1 ? std::min(base + 1, 4) : base; }       // MSC_CROWD_REPLAY's floor (only where the floor applies: base > 1)
inline int ShadowEvery(int base, int level) { return level >= 2 ? std::min(base + 1, 4) : base; }                  // MSC_SHADOW_EVERY in replays and cut-scenes
inline int GlowRate(int base, int level) { return level >= 3 && base >= 1 ? 2 : base; }                             // MSC_HELD_GLOW_HALF (1 half, 2 a third; 0 stays 0: the chain is the game's)
inline bool SizeStep(int level, int sizeLv) { return level >= 4 && sizeLv > 0; }                                     // the rung below the floor

} // namespace heldgov
} // namespace vgx
