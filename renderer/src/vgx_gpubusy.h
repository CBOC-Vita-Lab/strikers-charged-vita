// The GPU "busy" figure the ladder, the LOCKED 60 governor and the auto queue depth read, without the display scene's wait for a display buffer.
//
// vitaGL's rt.c counts a scene's busy time as done minus the later of its submission and the previous scene's end. The display's scene is begun with the
// back buffer's sync object as its fragment sync (gxm.c scene_reset): its fragment job is held until the display queue has released that buffer, and the
// hold is counted as busy. When the producer runs ahead of the display (near 60 fps, three buffers) busy_us follows the frame period although the GPU is
// idle, and a governor reading it steps the picture down for nothing (Galaxy's ladder did exactly this on its console). vitaGL 0030 reports display_us,
// the part of busy_us that belongs to scenes without a framebuffer object; this subtracts it and adds back what the real blit costs.
// A frame of one scene keeps its raw figure: there the display scene is the whole picture (no separate 3D scenes), so its time is real work.
// Pure, no platform includes (renderer/tests/gpubusy.cpp). Render only.
#pragma once
#include <algorithm>
#include <cstdint>

namespace vgx::gpubusy {

struct Config {
    bool on = true;            // MSC_GPU_BUSY_FIX
    uint32_t blitUs = 2000;    // MSC_GPU_BUSY_BLIT_US: what the display scene really costs (the upscale or copy to the display)
};

// busyUs, displayUs, scenes: the fields of vglRtGpuFrame. Returns microseconds.
inline uint32_t CorrectedUs(const Config& c, uint32_t busyUs, uint32_t displayUs, uint32_t scenes) {
    if (!c.on || scenes < 2 || displayUs == 0 || displayUs > busyUs) return busyUs;
    const uint32_t hold = displayUs > c.blitUs ? displayUs - c.blitUs : 0;
    return busyUs - hold;
}

}  // namespace vgx::gpubusy
