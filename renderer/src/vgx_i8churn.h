// MSC_VGX_I8CHURN (walk15): a GX I8 texture that the game rewrites picture after picture (the THP movie's three planes: the stadium preview, the intro, the credits)
// is uploaded as ONE byte a texel (TF_I8, sampled as I,I,I,I) instead of four (RGBA8): a quarter of the decode writes and of the copy into the graphics memory.
// Only a texture that has just shown it churns (uploaded again within `window` pictures, `minRun` times in a row) takes the new form: every other I8 texture keeps
// today's upload, so the one-byte format is not paid for by a texture that is uploaded once. Header-only so a host test runs the shipped decision.
#pragma once
#include <cstdint>

namespace vgx {
namespace i8churn {

struct State {
    uint64_t last = 0;   // picture of the last upload (0: none yet)
    uint32_t run = 0;    // uploads in a row, each within `window` pictures of the one before
};

// Called at every upload of the entry, `frame` = the current picture. True: take the one-byte form for this upload.
inline bool Note(State& s, uint64_t frame, uint32_t minRun, uint32_t window) {
    if (s.last != 0 && frame >= s.last && frame - s.last <= window) {
        if (s.run < 0xFFFFFFFFu) ++s.run;
    } else {
        s.run = 0;
    }
    s.last = frame;
    return minRun != 0 && s.run >= minRun;
}

// The shapes DecodeTextureI8 takes (whole 8x4 tiles; anything else keeps the generic path).
inline bool Shape(uint32_t w, uint32_t h) { return w && h && (w & 7u) == 0 && (h & 3u) == 0; }

}  // namespace i8churn
}  // namespace vgx
