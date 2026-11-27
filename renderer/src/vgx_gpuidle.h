// A picture that drew nothing has no GPU time worth reading (nxr3a, MSC_GPU_IDLE_SKIP, default 1).
//
// The boot screens and the first loading pictures (6122.log:271-280: `draws 0 scenes 1`, one picture every 50 ms) are one display scene that only waits for a display
// buffer. vitaGL counts that wait as busy time and vgx_gpubusy.h keeps the raw figure for a one-scene frame (there the display scene is the whole picture), so LOCKED 60 read
// "GPU mean 50.1 ms of 16.7", took its first step after the floor (crowd refresh halved) and published a predicted GPU time of 50 ms to the frame-rate controller (VgxGpuMinMs).
// The sample that the ladder reads in a picture belongs to a picture one to three presents back (the GPU queue), so a sample is dropped while any of the last `window` pictures
// drew nothing. A picture of live play always draws, so nothing changes there. Pure, no platform includes (renderer/tests/gpuidle.cpp). Render only.
#pragma once
#include <cstdint>

namespace vgx::gpuidle {

struct Ring {
    uint32_t bits = 0;    // bit 0: the newest finished picture drew nothing, bit 1 the one before, ...
    int window = 3;       // pictures looked back over (the GPU queue is at most 3 deep)
    // One executed picture: `draws` is the number of draw calls it made.
    void Picture(uint32_t draws) { bits = (bits << 1) | (draws == 0 ? 1u : 0u); }
    // True: the sample about to be read may belong to a picture that drew nothing.
    bool Discard() const {
        const uint32_t mask = window >= 32 ? ~0u : ((1u << window) - 1u);
        return (bits & mask) != 0;
    }
};

}  // namespace vgx::gpuidle
