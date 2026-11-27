// np_simpace18.h (pace18, plan-simthread17 phase 2d): the pacing cost model when the simulation step runs on its own thread.
// Pure code: no clock, no I/O, no game types. Used by np_fps_ctl.h (Auto::Predict) and np_fps.cpp (the game clock's per-picture cost); host test tools/fps/simpace18_unit.cpp.
//
// Modes (what NpSimtRun does, MSC_SIMT):
//   kOff (0)      the step runs on the game thread inside the picture: the picture's cost is  perPicture + step.            (today)
//   kBlock (1)    the step runs on np_sim, the game thread waits for it: the picture's cost is still perPicture + step        (today's numbers; if np_sim sits
//                 on another core than the game thread the game core's counter does not hold the step, so the step is added)
//   kOverlap (2)  the step runs on np_sim while the game thread draws the window (mapi17's stages), then joins: the part of the picture that holds both costs
//                     max(window, step) + overhead        and not window + step.
//                 overhead = what the join wait is beyond (step - window): the hand-over and wake-up of the thread, measured, smoothed.
//
// What the rest of the controller sees: the game core's 60 Hz cost (Predict), its free-run cost (FreeRateCostMs's overlap form below), and the per-picture cost without steps that the game
// clock uses to cap the steps of a picture (np_game_clock.h CapSteps). Modes 0 and 1 (with np_sim on the game core) return EXACTLY the expressions np_fps_ctl.h / np_fps.cpp had
// (same operations in the same order), which tools/fps/simpace18_unit.cpp proves bit for bit against a copy of the old code. NP_SIMPACE_BREAK mutants must fail that test.
#pragma once
#include <cmath>

#ifndef NP_SIMPACE_BREAK
#define NP_SIMPACE_BREAK 0   // host test mutants: 1 overlap prices window + step (the sum), 2 overhead dropped, 3 mode 1 priced as overlap, 4 clock per-picture cost not corrected, 5 free-run solution takes the min
#endif

namespace npsimpace {

enum Mode { kOff = 0, kBlock = 1, kOverlap = 2 };

inline double Max(double a, double b) { return a > b ? a : b; }

// Smoothed per-picture figures of the overlap (the window on the game thread, the join overhead). One per controller.
class Model {
public:
    Model() : win_(0), over_(0), have_(false) {}
    void Reset() { win_ = 0; over_ = 0; have_ = false; }
    // One picture that kicked a step (mode 2): windowMs = the game thread's window, joinMs = its wait at the join, simWallMs = np_sim's wall time for the step.
    void Observe(double windowMs, double joinMs, double simWallMs) {
        if (windowMs < 0) windowMs = 0;
        if (joinMs < 0) joinMs = 0;
        const double expected = simWallMs > windowMs ? simWallMs - windowMs : 0.0;
        double over = joinMs - expected;
        if (over < 0) over = 0;
        if (over > 2.0) over = 2.0;   // (a descheduled picture is not the steady overhead)
        if (!have_) { win_ = windowMs; over_ = over; have_ = true; }
        else { win_ += (windowMs - win_) * 0.2; over_ += (over - over_) * 0.2; }
    }
    double WindowMs() const { return win_; }
    double OverheadMs() const {
#if NP_SIMPACE_BREAK == 2
        return 0.0;
#else
        return over_;
#endif
    }
    bool Have() const { return have_; }
private:
    double win_, over_;
    bool have_;
};

// Is the step inside the game core's own counter? (modes 0 and 1 with np_sim on the game core; the picture then prices as today.)
inline bool StepOnGameCore(int mode, bool simOnGameCore) {
#if NP_SIMPACE_BREAK == 3
    return mode == kOff;   // (mutant: blocking mode priced as if the step were elsewhere)
#else
    return mode == kOff || (mode == kBlock && simOnGameCore);
#endif
}

// The game core's predicted cost at the display rate. pred0 = its measured busy ms of the picture with the time-following part scaled to the refresh (as Predict computes it),
// windowNowMs = this picture's window on the game thread (0 when the picture had none), step60Ms = stepCost x steps a refresh normally holds.
inline double GameCore60(int mode, bool simOnGameCore, double pred0, double simMs, double step60Ms, double windowNowMs, const Model& m) {
    if (StepOnGameCore(mode, simOnGameCore)) {
        double pred = pred0 - simMs + step60Ms;   // exactly the old expression
        if (pred < 0) pred = 0;
        return pred;
    }
    if (mode == kBlock) return pred0 + step60Ms;   // np_sim on another core, the game thread waits: serial
    // kOverlap
    double base = pred0 - windowNowMs;
    if (base < 0) base = 0;
    const double w = m.Have() ? m.WindowMs() : windowNowMs;
#if NP_SIMPACE_BREAK == 1
    return base + w + step60Ms + m.OverheadMs();
#else
    return base + Max(w, step60Ms) + m.OverheadMs();
#endif
}

// The same without a probe: the game thread's wall busy ms (it holds the join wait), joinMs this picture's wait.
inline double GameBusy60(int mode, bool simOnGameCore, double busyMs, double simMs, double step60Ms, double windowNowMs, double joinNowMs, const Model& m) {
    if (StepOnGameCore(mode, simOnGameCore)) {
        double pred = busyMs - simMs + step60Ms;   // old: gameBusyMs - simMs + stepCost x steps, floored by the caller
        return pred;
    }
    if (mode == kBlock) return busyMs - joinNowMs + step60Ms;   // the wait for np_sim was the step
    double base = busyMs - joinNowMs - windowNowMs;
    if (base < 0) base = 0;
    const double w = m.Have() ? m.WindowMs() : windowNowMs;
#if NP_SIMPACE_BREAK == 1
    return base + w + step60Ms + m.OverheadMs();
#else
    return base + Max(w, step60Ms) + m.OverheadMs();
#endif
}

// The free-run cost of the overlapped picture: P = base + max(window, k P) + overhead, k = step ms per ms of picture period (the step runs 50 a second whatever the picture rate).
// Solution of that fixed point: max(b + window, b / (1 - k)) with b = base + overhead (k < 0.9); at least one step a picture, at most two (as FreeRateCostMs).
inline double FreeRateOverlapMs(double baseMs, double windowMs, double stepMs, double fixedStepMs, const Model& m) {
    const double b = baseMs + m.OverheadMs();
    const double k = stepMs / fixedStepMs;
    double P;
    if (k < 0.9) {
        const double p1 = b + windowMs, p2 = b / (1.0 - k);
#if NP_SIMPACE_BREAK == 5
        P = p1 < p2 ? p1 : p2;
#else
        P = p1 > p2 ? p1 : p2;
#endif
    } else P = b + Max(windowMs, stepMs);
    const double floorMs = b + Max(windowMs, stepMs);
    if (P < floorMs) P = floorMs;
    const double cap = b + Max(windowMs, stepMs * 2.0);
    if (P > cap) P = cap;
    return P;
}

// np_fps.cpp's game clock: the ms of a picture without its steps (np_game_clock.h CapSteps: the steps that fit are (ref - this) / step ms).
// Old: realMs - waitMs - simMs. Overlap: the window is not removable by dropping steps up to its length, the join wait is the step's part: base + overhead.
inline double ClockPerPictureMs(int mode, bool simOnGameCore, double realMs, double waitMs, double simMs, double windowMs, double joinMs, const Model& m) {
#if NP_SIMPACE_BREAK == 4
    (void)mode; (void)simOnGameCore; (void)windowMs; (void)joinMs; (void)m;
    return realMs - waitMs - simMs;
#else
    if (mode == kOverlap) return realMs - waitMs - joinMs - windowMs + m.OverheadMs();
    if (mode == kBlock && !simOnGameCore) return realMs - waitMs - joinMs;
    return realMs - waitMs - simMs;   // exactly the old expression
#endif
}

} // namespace npsimpace
