// nxr4a MSC_SCENE_EMA_RESET: what the renderer's GPU-time mean and its "samples to ignore" mark are when live play begins after another scene (header-only so a test can run it on its own).
#pragma once

#include <algorithm>
#include <cstdint>

namespace vgx::scenefresh {

// The mean GPU time (DynRes::ema, 4-frame, 0.75 / 0.25) that the effects governor (FxGovernor::Frame) and the 30-second line read is the OTHER scene's at the first pictures of play: 6123.log:1897
// `fill governor: frame 5402 level 256 -> 512: GPU mean 17.7 ms, this frame 9.1, budget 16.7` five pictures after a cut-scene whose frames cost 35 ms. The two timed frames that arrive right after the cut are
// of pictures that were drawn before it (the GPU timers come two frames late). So: the mean is unknown again (-1: the next sample seeds it, as at the start of a scene), and the next `skip` timed frames
// are not read (the same mark a picture-size step sets, DynSetLevel). Returns true when it acted.
inline bool PlayBegins(bool on, float& ema, uint32_t& ignoreUntil, uint32_t seq, uint32_t skip) {
    if (!on) return false;
    ema = -1.0f;
    ignoreUntil = std::max(ignoreUntil, seq + skip);
    return true;
}

// nxr6a (NEON round 6 #1): how many timed frames of the scene before are still on their way when play begins. The GPU timers come `depth + 1` frames late (the swap of frame N waits for frame N-depth;
// the mean was last read at the call of frame N-1, from frame N-2-depth), so frames N-1-depth .. N-1 are the old scene's and unread: depth 1 -> 2, depth 2 -> 3 (the 30 fps walk-out's queue depth, 6122.log:752).
// nxr4a skipped 2 (3 without LOCKED 60): at depth 2 the third old frame (a walk-out's 28-35 ms) was read as the first of play. Default 3 covers depth 2; MSC_SCENE_SKIP overrides (0 = none).
// The skip also serves MSC_SCENE_GPU_RESET alone (EMA_RESET=0): without it the walk-out frame that is still in flight wrote g_gpuMinMs again in the frame that zeroed it.
constexpr uint32_t kDefaultSkip = 3;
inline uint32_t SkipFrom(int envValue) { return envValue < 0 ? kDefaultSkip : static_cast<uint32_t>(envValue > 8 ? 8 : envValue); }   // (-1: unset)
// The renderer's mark alone (no mean change): the timed frames up to seq + skip are not read.
inline bool DiscardInFlight(bool on, uint32_t& ignoreUntil, uint32_t seq, uint32_t skip) {
    if (!on || skip == 0) return false;
    ignoreUntil = std::max(ignoreUntil, seq + skip);
    return true;
}
// The host's GPU queue judge (LimitGpuQueue): `left` is how many new timed frames it still drops after a scene reset before it takes samples again. Returns true when this frame is dropped.
inline bool DropSample(uint32_t& left) {
    if (left == 0) return false;
    --left;
    return true;
}

} // namespace vgx::scenefresh
