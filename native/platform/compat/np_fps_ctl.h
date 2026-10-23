// np_fps_ctl.h: the decision logic of MSC_FPS=auto (W5 FPS). Pure: no clock, no I/O, no game types, so the game's np_fps.cpp and the unit
// test (tools/fps/fps_unit.cpp) run the same code. C++11 (the game's compat flags).
//
// What it decides: whether the presentation policy for the NEXT frame is "30" (a picture every two refreshes at most) or "60" (a picture
// per refresh, a late frame waits for the next one). It is fed once per drawn frame with what the previous frame cost on every core.
//
//   going up   (30 -> 60): the frames of at least UP_HOLD seconds (and UP_FRAMES frames) in a row ALL had a predicted 60 Hz cost of at most
//              UP_MARGIN of a refresh on every core; not before the cool-down of the last down is over; a new scene (task state) or a
//              loading stall starts the evidence again.
//   going down (60 -> 30): at once when a frame misses its refresh during the first PROBATION seconds after going up; later when MISS_LIMIT
//              of the last MISS_WINDOW frames missed, or the busiest core's cost stayed above DOWN_BUSY of a refresh for BUSY_FRAMES
//              frames; or when the frames of the last SLOW_FRAMES lasted more than SLOW_MEAN refreshes on average (the limit is then not on a
//              CPU core: the GPU, whose time the per-core counters do not see; a wait for a peer's input ends in 30 as well, which is harmless).
//              A miss is a frame of more than 1.5 refreshes while a core was busy (a wait for a peer's input is not a miss).
//   the GPU (23 Oct): when the renderer reports gpuMinMs (its predicted GPU time per frame at the smallest picture it would use in this scene,
//              vgx_backend_gl.cpp, measured by vitaGL's GPU timers), going up also needs that under the margin, and it goes down when it stayed
//              above DOWN_BUSY of a refresh for BUSY_FRAMES frames: a scene the GPU cannot draw 60 times a second is not tried, instead of being
//              found out by the sustained-slowness rule after a few slow seconds.
//   no flip-flop: the cool-down after a down is COOL_BASE seconds, doubled for every down in the last 60 s; a down inside the probation
//              (the prediction was too optimistic) also lowers the margin for going up (PENALTY_STEP per such down, up to 3, forgotten
//              after 120 s of stable 60). So a scene that does not hold 60 is tried a few times and then left at 30.
//
// The prediction of the 60 Hz cost (Predict): per core, the part of the frame's cost that follows the number of frames (frame building, the
// frame send, decoding, drawing) is the measured cost; the part that follows time (the audio mixer, helper threads) is scaled to one refresh;
// on the game core the simulation part (steps x cost per step) is replaced by ONE step: at 60 Hz a frame holds 0.834 steps on average, but five
// frames in six hold one, and that frame has to fit into the refresh (a frame with two steps, after a late one, is the exception).
// Settling: the first frames after a switch up carry the backlog of the 30 fps frames before them (two steps instead of one): they may miss and
// are not judged (SETTLE frames); a system whose one-step frame fits the refresh then settles into 60 by itself.
#ifndef NP_FPS_CTL_H
#define NP_FPS_CTL_H

#include <math.h>
#include <string.h>

namespace npfps {

enum Policy { kPolicyGame = 0, kPolicy30 = 1, kPolicy60 = 2, kPolicyUnlocked = 3 };

static const int kMaxCores = 4;

struct Config {
    double refresh;       // seconds: one VI refresh (16.683 ms NTSC)
    double upMargin;      // fraction of a refresh every core's predicted 60 Hz cost must stay under to go up
    double downBusy;      // fraction of a refresh the busiest core's measured cost may not stay above
    double upHold;        // seconds of good evidence before going up
    int upFrames;         // and at least this many frames
    double coolBase;      // seconds of cool-down after a down
    double coolMax;
    double probation;     // seconds after going up in which one miss sends it down
    int missWindow;       // frames
    int missLimit;        // misses in the window that send it down
    int busyFrames;       // consecutive frames above downBusy that send it down
    double missBusy;      // a long frame counts as a miss only when the busiest core used more than this fraction of a refresh
    double stall;         // refreshes: a frame this long (a loading stall) is neither evidence nor a miss
    double penaltyStep;   // margin lost per failed try
    int settle;           // frames after a switch that are not judged
    int slowFrames;       // window of the sustained-slowness rule
    double slowMean;      // refreshes: the mean frame length over the window that sends it down
    double stepSeconds;   // one simulation step
    Config()
        : refresh(1001.0 / 60000.0), upMargin(0.85), downBusy(0.97), upHold(1.0), upFrames(20), coolBase(4.0), coolMax(32.0), probation(3.0),
          missWindow(60), missLimit(3), busyFrames(8), missBusy(0.75), stall(8.0), penaltyStep(0.06), settle(8), slowFrames(30), slowMean(1.3), stepSeconds(0.02) {}
};

struct FrameIn {
    double now;            // seconds on any monotonic clock
    double interval;       // seconds the previous frame lasted (BeginFrame to BeginFrame)
    int steps;             // simulation steps it ran
    double simMs;          // game-thread time inside the fixed-update task
    bool costsValid;       // cpuMs / timeMs are measurements
    int gameCore;          // the core of the game thread
    double cpuMs[kMaxCores];   // busy milliseconds per core in that frame (all kinds)
    double timeMs[kMaxCores];  // the part of cpuMs that follows time, not frames
    double gameBusyMs;     // without a probe: the game thread's busy time (interval minus waits), the only figure there is
    bool locked;           // the game forced its own 30 fps cut-scene lock in that frame
    unsigned scene;        // scene identity (task state): a change starts a new evidence window
    double gpuMinMs;       // the renderer's predicted GPU ms per frame at its smallest picture for this scene; <= 0: not known (the rule is off)
};

struct Prediction {
    bool valid;
    double max60;          // the busiest core's predicted cost at 60 Hz, ms
    double maxNow;         // the busiest core's measured cost, ms
    int core;              // which core
};

struct Decision {
    Policy policy;
    bool switched;
    const char* reason;
};

class Auto {
public:
    explicit Auto(const Config& c = Config()) : cfg_(c) { Reset(); }

    void Reset() {
        cur_ = kPolicy30;
        goodSince_ = -1;
        goodFrames_ = 0;
        scene_ = 0xFFFFFFFFu;
        cooldownUntil_ = 0;
        upAt_ = -1e9;
        penalty_ = 0;
        stableSince_ = -1;
        memset(miss_, 0, sizeof miss_);
        missPos_ = 0;
        missCount_ = 0;
        busyHigh_ = 0;
        gpuHigh_ = 0;
        nDowns_ = 0;
        stepCost_ = 0.0;
        switches_ = 0;
        settle_ = 0;
        slowPos_ = slowCount_ = 0;
        slowSum_ = 0.0;
        slowFails_ = 0;
        lastPred_.valid = false;
        lastPred_.max60 = lastPred_.maxNow = 0;
        lastPred_.core = 0;
    }

    Policy Current() const { return cur_; }
    int Switches() const { return switches_; }
    int Penalty() const { return penalty_; }
    const Prediction& LastPrediction() const { return lastPred_; }
    double CooldownUntil() const { return cooldownUntil_; }

    Prediction Predict(const FrameIn& f) {
        Prediction p;
        p.valid = false;
        p.max60 = 0;
        p.maxNow = 0;
        p.core = 0;
        const double R = cfg_.refresh;
        const double stepsPerRefresh = ceil(R / cfg_.stepSeconds - 1e-9);   // the frame with the most steps that a refresh normally holds
        if (f.steps > 0 && f.simMs > 0) {
            const double c = f.simMs / f.steps;
            stepCost_ = stepCost_ > 0 ? 0.8 * stepCost_ + 0.2 * c : c;   // ms per simulation step, smoothed (a frame without steps still has a price)
        }
        if (f.costsValid) {
            p.valid = true;
            for (int c = 0; c < kMaxCores; ++c) {
                double now = f.cpuMs[c];
                double pred = now - f.timeMs[c] + f.timeMs[c] * (f.interval > 0 ? (R / f.interval) : 1.0);
                if (c == f.gameCore) {
                    pred = pred - f.simMs + stepCost_ * stepsPerRefresh;
                    if (pred < 0) pred = 0;
                }
                if (pred > p.max60) { p.max60 = pred; p.core = c; }
                if (now > p.maxNow) p.maxNow = now;
            }
        } else {
            // Only the game thread's busy time is known (a build without a probe): its unthrottled time with the simulation part re-priced.
            p.valid = f.gameBusyMs > 0;
            p.maxNow = f.gameBusyMs;
            double pred = f.gameBusyMs - f.simMs + stepCost_ * stepsPerRefresh;
            p.max60 = pred < 0 ? 0 : pred;
            p.core = f.gameCore;
        }
        return p;
    }

    Decision Frame(const FrameIn& f) {
        Decision d;
        d.policy = cur_;
        d.switched = false;
        d.reason = "";
        const double R = cfg_.refresh;
        const double Rms = R * 1000.0;
        if (f.locked) {
            // the game's own cut-scene lock: no evidence either way
            ResetEvidence();
            return d;
        }
        if (f.scene != scene_) {
            scene_ = f.scene;
            ResetEvidence();
            // a new scene is a new case: the GPU-bound back-off of the last one does not follow it (the ordinary cool-down does)
            if (slowFails_ > 0) {
                slowFails_ = 0;
                if (cooldownUntil_ > f.now + cfg_.coolBase) cooldownUntil_ = f.now + cfg_.coolBase;
            }
        }
        const Prediction p = Predict(f);
        lastPred_ = p;
        const bool stall = f.interval > cfg_.stall * R;
        if (cur_ == kPolicy30) {
            if (stall || !p.valid) { ResetEvidence(); return d; }
            const double margin = Margin();
            if (p.max60 > margin * Rms) { ResetEvidence(); return d; }
            if (f.gpuMinMs > 0 && f.gpuMinMs > margin * Rms) { ResetEvidence(); return d; }   // the GPU would not hold a refresh even at its smallest picture
            if (goodSince_ < 0) { goodSince_ = f.now; goodFrames_ = 0; }
            ++goodFrames_;
            if (f.now - goodSince_ >= cfg_.upHold && goodFrames_ >= cfg_.upFrames && f.now >= cooldownUntil_) {
                Go(kPolicy60, f.now);
                d.policy = cur_;
                d.switched = true;
                d.reason = "every core predicted under the budget for the whole hold time";
                upAt_ = f.now;
                settle_ = cfg_.settle;
                for (int i = 0; i < cfg_.missWindow && i < kMaxWindow; ++i) miss_[i] = 0;
                missCount_ = 0;
                slowPos_ = slowCount_ = 0;
                slowSum_ = 0.0;
                busyHigh_ = 0;
                gpuHigh_ = 0;
                if (stableSince_ < 0) stableSince_ = f.now;
            }
            return d;
        }
        if (cur_ == kPolicy60) {
            if (stall) return d;
            if (settle_ > 0) { --settle_; return d; }   // the backlog of the 30 fps frames before: not judged
            // forgive the penalty after a long stable stretch
            if ((penalty_ > 0 || slowFails_ > 0) && stableSince_ >= 0 && f.now - stableSince_ >= 120.0) {
                if (penalty_ > 0) --penalty_;
                if (slowFails_ > 0) --slowFails_;
                stableSince_ = f.now;
            }
            const bool longFrame = f.interval > 1.5 * R;
            const bool busy = !f.costsValid ? true : (p.maxNow > cfg_.missBusy * Rms);
            const bool miss = longFrame && busy;
            const int idx = missPos_ % cfg_.missWindow;
            if (miss_[idx]) --missCount_;
            miss_[idx] = miss ? 1 : 0;
            if (miss) ++missCount_;
            missPos_ = (missPos_ + 1) % cfg_.missWindow;
            if (p.valid && p.maxNow > cfg_.downBusy * Rms) ++busyHigh_; else busyHigh_ = 0;
            if (f.gpuMinMs > 0 && f.gpuMinMs > cfg_.downBusy * Rms) ++gpuHigh_; else gpuHigh_ = 0;
            const char* why = 0;
            bool failedTry = false, slowTry = false;
            if (miss && f.now - upAt_ < cfg_.probation) { why = "a frame missed its refresh right after going up"; failedTry = true; }
            else if (missCount_ >= cfg_.missLimit) why = "too many frames missed their refresh";
            else if (busyHigh_ >= cfg_.busyFrames) { why = "the busiest core stayed over the budget"; failedTry = f.now - upAt_ < 10.0; }
            else if (gpuHigh_ >= cfg_.busyFrames) { why = "the GPU stayed over the budget even at the smallest picture"; failedTry = f.now - upAt_ < 10.0; }
            // the sustained-slowness rule: the window of the last frames
            {
                const int w = cfg_.slowFrames < kMaxWindow ? cfg_.slowFrames : kMaxWindow;
                if (slowCount_ == w) slowSum_ -= slowRing_[slowPos_]; else ++slowCount_;
                slowRing_[slowPos_] = f.interval;
                slowSum_ += f.interval;
                slowPos_ = (slowPos_ + 1) % w;
                if (!why && slowCount_ == w && slowSum_ / w > cfg_.slowMean * R)
                {
                    why = "frames last longer than a refresh on average (the limit is not on a CPU core: the GPU?)";
                    failedTry = f.now - upAt_ < 10.0;
                    slowTry = true;
                }
            }
            if (why) {
                if (failedTry && penalty_ < 3) ++penalty_;
                // downs in the last 60 s double the cool-down
                int recent = 0;
                for (int i = 0; i < nDowns_; ++i) if (f.now - downs_[i] < 60.0) ++recent;
                double cool = cfg_.coolBase;
                for (int i = 0; i < recent && cool < cfg_.coolMax; ++i) cool *= 2.0;
                if (cool > cfg_.coolMax) cool = cfg_.coolMax;
                if (slowTry) {
                    // the margin cannot help here (no core is the limit): a scene that is slow with idle cores is tried again
                    // after 32, 64, 128, 256 s (a new scene or 120 s of stable 60 forgive one step)
                    if (slowFails_ < 4) ++slowFails_;
                    double slow = cfg_.coolMax;
                    for (int i = 1; i < slowFails_; ++i) slow *= 2.0;
                    if (slow > cool) cool = slow;
                }
                cooldownUntil_ = f.now + cool;
                if (nDowns_ < kMaxDowns) downs_[nDowns_++] = f.now;
                else { memmove(downs_, downs_ + 1, sizeof(double) * (kMaxDowns - 1)); downs_[kMaxDowns - 1] = f.now; }
                Go(kPolicy30, f.now);
                stableSince_ = -1;
                ResetEvidence();
                d.policy = cur_;
                d.switched = true;
                d.reason = why;
            }
        }
        return d;
    }

    // The margin for going up now (the build's own UP_MARGIN, lowered by the failed tries).
    double Margin() const {
        double m = cfg_.upMargin - cfg_.penaltyStep * penalty_;
        return m < 0.55 ? 0.55 : m;
    }

private:
    static const int kMaxWindow = 256;
    static const int kMaxDowns = 16;
    void ResetEvidence() { goodSince_ = -1; goodFrames_ = 0; }
    void Go(Policy p, double now) { (void)now; cur_ = p; ++switches_; }

    Config cfg_;
    Policy cur_;
    double goodSince_;
    int goodFrames_;
    unsigned scene_;
    double cooldownUntil_;
    double upAt_;
    int penalty_;
    double stableSince_;
    unsigned char miss_[kMaxWindow];
    int missPos_, missCount_, busyHigh_, gpuHigh_;
    int settle_;
    double slowRing_[kMaxWindow];
    int slowPos_, slowCount_;
    double slowSum_;
    int slowFails_;
    double downs_[kMaxDowns];
    int nDowns_;
    double stepCost_;
    int switches_;
    Prediction lastPred_;
};

} // namespace npfps

#endif // NP_FPS_CTL_H
