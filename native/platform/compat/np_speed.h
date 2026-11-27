// np_speed.h: the game-speed meter (MSC_SPEED_LOG, default on), pure code (no clock, no I/O) so the game (np_fps.cpp SpeedFrame) and the host test
// (tools/fps/speed_meter_unit.cpp) run the same thing.
//
// The owner's rule is no slow motion: game time advanced / real time >= 98 % in live play. The slip ledger ([slip] lines) counts what the slip rule cut from a frame's
// time (and only with MSC_SLIP_BUDGET_MS on), from the frame time the task manager clamps at 100 ms; it cannot see a frame that took longer than that, nor a step the
// simulation did not run. This meter counts the other end: FIXED STEPS the simulation really ran (50 a second of game time, 20 ms each) against the WALL time between
// two frames of live play, unclamped. It does not depend on any other switch.
//   window  every 5 s of live play: speed percent, steps, ms lost (wall - steps x 20 ms), the longest picture
//   match   the same since the match's first live frame, and how many windows were under 98 %
// Frames outside live play (pause, replay, cut-scene, loading) are not counted: the first live frame after them only sets the baseline. A window cut short by the end of live
// play is reported if it holds at least one second.
#pragma once
#include <cstdint>

#ifndef NP_SPEED_BREAK
#define NP_SPEED_BREAK 0   // host test mutants: 1 a step counts 16 ms, 2 a break keeps the baseline (a pause counts as slow play), 3 the rule limit is 90 %, 4 the game's time scale is ignored (wanted = real)
#endif

namespace npfps {

struct SpeedWindow {
    double wallMs = 0, gameMs = 0, lostMs = 0, worstPictureMs = 0, wantedMs = 0, slowMs = 0;   // wantedMs: real time x the game's own time scale (slow motion on a goal); slowMs: real time spent under 98 % scale
    uint32_t pictures = 0, steps = 0;
    double Percent() const { return wallMs > 0 ? 100.0 * gameMs / wallMs : 0.0; }                        // of real time
    double WantedPercent() const { return wantedMs > 0 ? 100.0 * gameMs / wantedMs : 0.0; }              // of what the game wanted (time scale applied): the owner's rule is judged on this
    double SlowShare() const { return wallMs > 0 ? 100.0 * slowMs / wallMs : 0.0; }
};

class SpeedMeter {
public:
#if NP_SPEED_BREAK == 1
    static constexpr double kStepMs = 16.0;
#else
    static constexpr double kStepMs = 20.0;      // the simulation's fixed step
#endif
    static constexpr double kWindowMs = 5000.0;
#if NP_SPEED_BREAK == 3
    static constexpr double kRuleMs = 90.0;
#else
    static constexpr double kRuleMs = 98.0;
#endif
    // One frame start in live play: `nowS` the wall clock (s), `stepsTotal` the simulation's running step counter. Returns true when a window is complete
    // (win() then holds it; the next call starts a new one).
    // `scale` is the game's time scale now (1 normally, below 1 in its own slow motion); the interval that ends at this call was run at the scale given at the previous call.
    bool Frame(double nowS, uint64_t stepsTotal, double scale = 1.0) {
        if (done_) { win_ = SpeedWindow(); done_ = false; }
        bool complete = false;
        if (have_) {
            const double dt = (nowS - lastS_) * 1000.0;
            const uint64_t ds = stepsTotal >= lastSteps_ ? stepsTotal - lastSteps_ : 0;
            if (dt > 0) {
                Add(win_, dt, ds, lastScale_);
                Add(all_, dt, ds, lastScale_);
                if (win_.wallMs >= kWindowMs) complete = Close();
            }
        }
        have_ = true; lastS_ = nowS; lastSteps_ = stepsTotal; lastScale_ = scale > 0 ? (scale > 1.0 ? 1.0 : scale) : 1.0;
        return complete;
    }
    // Live play ended (or has not begun): the next live frame is a baseline. Returns true when a partial window of at least one second is complete to report.
    bool Break() {
#if NP_SPEED_BREAK != 2
        have_ = false;
#endif
        if (done_) return false;
        if (win_.wallMs >= 1000.0) return Close();
        win_ = SpeedWindow();
        return false;
    }
    // A new match: the match totals start again.
    void NewMatch() { all_ = SpeedWindow(); windows_ = under_ = underReal_ = 0; if (!done_) win_ = SpeedWindow(); }
    const SpeedWindow& win() const { return win_; }
    const SpeedWindow& match() const { return all_; }
    uint32_t windows() const { return windows_; }
    uint32_t underRule() const { return under_; }          // windows under the rule on the WANTED figure
    uint32_t underRuleReal() const { return underReal_; }  // ... on real time (what r11 counted)
private:
    static void Add(SpeedWindow& w, double dtMs, uint64_t ds, double scale) {
        w.wallMs += dtMs;
#if NP_SPEED_BREAK == 4
        (void)scale; w.wantedMs += dtMs;
#else
        w.wantedMs += dtMs * scale;
        if (scale < 0.98) w.slowMs += dtMs;
#endif
        w.gameMs += (double)ds * kStepMs;
        w.steps += (uint32_t)ds;
        ++w.pictures;
        if (dtMs > w.worstPictureMs) w.worstPictureMs = dtMs;
    }
    bool Close() {
        win_.lostMs = win_.wallMs - win_.gameMs;
        all_.lostMs = all_.wallMs - all_.gameMs;
        ++windows_;
        if (win_.WantedPercent() < kRuleMs) ++under_;
        if (win_.Percent() < kRuleMs) ++underReal_;
        done_ = true;
        return true;
    }
    SpeedWindow win_, all_;
    bool have_ = false, done_ = false;
    double lastS_ = 0;
    uint64_t lastSteps_ = 0;
    double lastScale_ = 1.0;
    uint32_t windows_ = 0, under_ = 0, underReal_ = 0;
};

} // namespace npfps
