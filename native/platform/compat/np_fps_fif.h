// np_fps_fif.h (wave 10, MSC_LAT_DRAIN): the frames-in-flight gate's decision after a long frame, pure code (host test: tools/fps/fif_drain_unit.cpp).
// The gate (np_fps.cpp FramesInFlightGate) stood aside for 150 frames after a frame that lasted over two refreshes (a load, a cut, a long present), because always waiting cost
// 5 fps in heavy scenes. At a clean held 30 that lets a backlog made by the long present STAND for five seconds (the Galaxy port's console run: 200 ms of input lag with perfect cadence
// because nothing drained the pictures queued behind a stage load). The drain rule: while standing aside, a held 30 (policy 30 with a gate, not a free run) still waits when more than
// depth + 1 pictures are on their way: the wait is one refresh, the late wait of the held cadence absorbs it, and the queue falls by one picture a frame to the depth.
#pragma once
#include <cstdint>

#ifndef NP_FIF_BREAK
#define NP_FIF_BREAK 0   // host test mutants: 1 the drain also applies to a free run, 2 the drain threshold is depth (waits at every frame), 3 no drain
#endif

namespace npfps {

// paused: the gate is standing aside; held: a held cadence (interval 2, not paced); backlog: pictures handed over and not yet shown; depth: the gate's depth; drain: MSC_LAT_DRAIN.
inline bool FifWaits(bool paused, bool held, uint64_t backlog, uint64_t depth, bool drain) {
    if (!paused) return backlog > depth;
#if NP_FIF_BREAK == 3
    return false;
#else
    if (!drain) return false;
#if NP_FIF_BREAK != 1
    if (!held) return false;
#endif
#if NP_FIF_BREAK == 2
    return backlog > depth;
#else
    return backlog > depth + 1;
#endif
#endif
}

} // namespace npfps
