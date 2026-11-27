// MSC_DYNRES_SEVERE: a sustained GPU overload in play opens a deeper floor of the dynamic resolution (header-only so a test can run it on its own).
#pragma once

namespace vgx {

// From the Galaxy port's GXV_AUTOQ_SEVERE (one drop-in held 11-12 FPS for 15 s on the console). The Strikers ladder needs no quiet moment to step down, so what holds such
// a scene here is the floor: 0.86 per side at 30. The mean GPU time at `ratio` times the budget or more, over `secs` of the GPU's own measured time (a lower bound
// of the time on the clock: the picture cannot come faster than the GPU draws it), opens the deeper floor. One slow frame cannot do it, a mean that falls under the
// line starts the count again, and nothing counts while `blocked` is set (a Mega Strike has its own ladder; a step found to buy nothing stays undone).
// It closes when blocked, or after `calmFrames` measured frames in a row that are predicted to fit at the ordinary floor.
struct SevereGuard {
    bool on = false;
    float ratio = 1.8f, secs = 3.0f;
    int calmFrames = 180;
    bool open = false;
    float heldMs = 0.0f;
    int calm = 0;
    // One measured frame. ema: the mean GPU time; ms: this frame's; budget: the frame budget; atFloor: the mean predicted at the ordinary floor (0.82 is the
    // ladder's own "fits" line). Returns 1 when the floor opened now, -1 when it closed now, else 0.
    int Frame(float ema, float ms, float budget, float atFloor, bool blocked) {
        if (!on) return 0;
        if (blocked) {
            heldMs = 0.0f;
            calm = 0;
            if (open) { open = false; return -1; }
            return 0;
        }
        if (open) {
            calm = atFloor < 0.82f * budget ? calm + 1 : 0;
            if (calm >= calmFrames) { open = false; calm = 0; heldMs = 0.0f; return -1; }
            return 0;
        }
        if (!(ema >= ratio * budget)) { heldMs = 0.0f; return 0; }
        heldMs += ms;
        if (heldMs < secs * 1000.0f) return 0;
        open = true;
        calm = 0;
        heldMs = 0.0f;
        return 1;
    }
};

} // namespace vgx
