// MSC_DYNRES_LATEHOLD: one late frame steps the dynamic resolution down at once and holds it there (header-only so a test and the trace model can run it on its own).
#pragma once

namespace vgx {

// From the strikersVita port's dynamic scale (render at 72 % of the screen; after a late frame in play 64 % for two seconds, because late frames come in clusters).
// The ladder's own down rule waits for the smoothed GPU time (a quarter of each frame) to pass 90 % of the budget, or three frames in a row over it, so one late
// frame at a mean of 70 % moves nothing and the frames after it, already queued, are late too. Here a frame whose OWN GPU time is over `at` times the budget
// asks for a step down at once (the renderer takes up to `maxStep` rungs, as far as this frame's time says it needs), and every such frame starts the hold
// again: for `holdMs` of clock time after the last late frame no step goes up. Only the GPU's own time counts: a frame that was late for the CPU (a load, the
// game thread) asks for nothing, because a smaller picture would not help it. Nothing counts while `blocked` is set (a Mega Strike or a paced scene has its own
// floor, a forced level, a step found to buy nothing).
//   want: a step down is asked for and not yet taken (the renderer takes it when the ladder may change size; it lapses with the hold)
//   cool: measured frames after a step in which another late frame only renews the hold (they were queued at the old size)
struct LateHold {
    bool on = false;
    float holdMs = 2000.0f;
    int maxStep = 2;
    float at = 1.0f;
    int coolFrames = 4;
    float leftMs = 0.0f;
    int cool = 0;
    bool want = false;
    float wantMs = 0.0f;   // the GPU time of the late frame that asked (the renderer sizes the step on it)
    int drops = 0;         // steps taken (the log)
    // One measured frame: ms its GPU time, budget the frame budget, dtMs the clock time since the previous measured frame.
    // Returns true when this frame asked for a step down now.
    bool Frame(float ms, float budget, float dtMs, bool blocked) {
        if (!on) return false;
        if (blocked) {
            leftMs = 0.0f;
            cool = 0;
            want = false;
            return false;
        }
        if (cool > 0) --cool;
        const bool late = ms > at * budget;
        if (late) {
            leftMs = holdMs;
        } else if (leftMs > 0.0f) {
            leftMs -= dtMs > 0.0f ? dtMs : budget;
            if (leftMs <= 0.0f) { leftMs = 0.0f; want = false; }
        }
        if (late && cool == 0) {
            if (!want || ms > wantMs) wantMs = ms;
            want = true;
            return true;
        }
        return false;
    }
    // The renderer took the step (or there was no rung left to take): the frames already queued at the old size do not ask again.
    void Took(bool stepped) {
        want = false;
        cool = coolFrames;
        if (stepped) ++drops;
    }
    bool Holding() const { return on && leftMs > 0.0f; }
};

} // namespace vgx
