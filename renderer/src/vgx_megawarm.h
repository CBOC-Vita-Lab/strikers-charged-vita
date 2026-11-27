// nxfly25 (25 Nov 2026): which render target MegaPrewarm may make ahead (renderer/src/vgx_backend_gl.cpp MegaPrewarm).
// The Mega Strike starts at level `start` of the ladder (the game's own size plus two rungs with the effects at the lower resolution). With the Smooth 60 / Quality tables
// (MSC_PICTURE_FLOORS, LOCKED 60) the table has 3 or 4 rungs, all at or above the pixel floor, and `start` (native + 2) can lie BEHIND its end: the entry it names is a left-over of the
// old 13-rung table (640x448, 286,720 pixels against the floor's 364,800). LOCKED 60 draws a Mega Strike at its own preset rung (the 25 Nov console logs: "[lock60] scene mega: preset
// level 2 (1.13)", 728x512), so that target was made (41 ms, 2240 KiB of video memory, a long picture in live play) and never drawn into. The rule: a target is made ahead only when
// its size is a rung of the table in force (the level exists, or another rung has the same size and samples).
#pragma once
#ifndef VGX_MEGAWARM_BREAK
#define VGX_MEGAWARM_BREAK 0   // host test mutants (renderer/tests/mega_warm.cpp): 1 the level test is off by one, 2 the table is not consulted (old behaviour), 3 a negative level passes
#endif
namespace vgx {
namespace megawarm {
// start: the level the Mega Strike starts at; tableN: rungs in the table in force; sizeIsRung: another rung of the table has this size and sample count.
inline bool Wanted(int start, int tableN, bool sizeIsRung) {
#if VGX_MEGAWARM_BREAK == 2
    (void)start; (void)tableN; (void)sizeIsRung;
    return true;
#else
#if VGX_MEGAWARM_BREAK == 1
    const bool inTable = start >= 0 && start <= tableN;
#elif VGX_MEGAWARM_BREAK == 3
    const bool inTable = start < tableN;
#else
    const bool inTable = start >= 0 && start < tableN;
#endif
    return inTable || sizeIsRung;
#endif
}
}  // namespace megawarm
}  // namespace vgx
