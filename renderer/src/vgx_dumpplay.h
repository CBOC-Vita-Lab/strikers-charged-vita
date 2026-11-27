// MSC_DUMP_START (dumpplay16): The Dump's first live-play seconds start on the bottom of the picture ladder instead of one step above it.
// Why (console logs w8/console/r11 worstgpu1.log, gsdump.log; notes w8/dumpplay16.md): the safe-start table (vgx_hold.h StadiumClass 2) puts The Dump on step 9 (752x424, one sample,
// "match starts: stadium 4 ... starting at step 9"). The GPU there is 29.2 / 32.0 ms against a 26.7 / 29.8 ms budget, so the policy steps to 10 (704x400) in the first
// lane window of every run and stays there (r11: "30 s: step 10, moves down 1 up 0"; gsdump the same): the first 270-280 pictures carry 16 / 27 late pictures (5.7 / 9.9 % against 0.6-0.7 %
// in the rest of live play), and the step itself makes 2 / 5 pass objects in play (a hitch). Starting on step 10 skips that move. The picture is smaller by 6 % per side for the first
// seconds only (the same size the match keeps from second 10 on): the owner's rule "smooth and responsive ranks above picture quality" (14 Nov 14:28).
// Mechanism: no new decision code in the backend. The backend asks the match memory (g_holdMem) what the last match here learned; this memory answers "the bottom step" for The Dump
// when nothing was learned yet and the switch is on; every other stadium, and a Dump with a learned step, get the base answer. Render only: nothing here reaches the simulation.
#pragma once

#include "vgx_hold.h"

namespace vgx {
namespace dumpplay {

constexpr int kDumpStadium = 4;               // STAD_DUMP (stadium ids as np/stadiumcut.h: 4 The Dump, 7 Lava Pit)
constexpr int kBottomStep = hold::kSteps;     // the bottom of the five-size ladder; every user clamps it to the match's own ladder (StartLevel `last`, Policy::Start maxStep)

inline bool& Armed() { static bool a = false; return a; }

// The step a match here starts from when nothing was learned: the base answer (-1 = none) unless the switch is on for The Dump.
// (VGX_DUMPPLAY_BREAK: test-only mutants, always 0 in a build: 1 any stadium is seeded, 2 a learned step is overridden, 3 the switch is ignored)
#ifndef VGX_DUMPPLAY_BREAK
#define VGX_DUMPPLAY_BREAK 0
#endif
inline int SeedStep(bool on, int stadium, int base) {
    if (base >= 0 && VGX_DUMPPLAY_BREAK != 2) return base;
    if (VGX_DUMPPLAY_BREAK == 3) on = true;
    if (VGX_DUMPPLAY_BREAK == 1) return on ? kBottomStep : -1;
    return on && stadium == kDumpStadium ? kBottomStep : -1;
}

// Drop-in for hold::StadiumMemory (g_holdMem): Put/learned are the base's, Get answers the seed.
struct Memory : hold::StadiumMemory {
    int Get(int stadium) const { return SeedStep(Armed(), stadium, hold::StadiumMemory::Get(stadium)); }
};

} // namespace dumpplay
} // namespace vgx
