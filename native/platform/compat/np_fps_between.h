// np_fps_between.h: when MSC_FPS_BETWEEN=pace leaves AUTO's held 30 (np_fps.cpp NpFpsPaced). Pure: no clock, no I/O, so the game and the model (tools/fps/pace_mid_model.cpp) run the
// same code. C++11. The model's reason for a gate at all: free-running pictures win clearly from about 37 a second up (a third more pictures, no judder with MSC_PRESENT_SYNC), and lose
// to the held 30 below about 34 (the same 30-32 pictures, with 3-refresh holds in them). So:
//   in   the busiest CPU core (or the GPU at its smallest picture) under ENTER ms a frame for HOLD seconds, after the cool-down
//   out  the frame intervals or the cost over LEAVE ms on average for OUT_FRAMES frames: the intervals see what the core counters do not (the GPU). Such a stretch within FAIL seconds of
//        (pacing12: the MEAN of the last OUT_FRAMES frames' lengths and costs, each clipped at three times the limit, over the limit; a stall of 150 ms and more is no frame at all: r10 took a 72 ms frame for the stage cost.)
//        going in was a failed try: the cool-down doubles (COOL, at most COOL_MAX); a new scene takes the doubling back. A scene that may not pace at all ends it at once, without blame.
#ifndef NP_FPS_BETWEEN_H
#define NP_FPS_BETWEEN_H

#ifndef NP_BETWEEN_BREAK
#define NP_BETWEEN_BREAK 0   // host test mutants (between_sticky_unit.cpp): 1 a failed try does not double the cool-down, 2 the way out ignores its frame count (the first frame over the limit leaves), 3 a stall frame counts as a frame over the limit, 4 the way-out window is five frames, not OUT_FRAMES
#endif
namespace npfps {

struct BetweenConfig {
    double enterMs, leaveMs, hold, cool, coolMax, fail;
    int outFrames;
    BetweenConfig() : enterMs(26.7), leaveMs(30.0), hold(1.0), cool(6.0), coolMax(96.0), fail(30.0), outFrames(20) {}   // 26.7 ms: 37.5 pictures a second, 1.6 refreshes
};

class Between {
public:
    explicit Between(const BetweenConfig& c = BetweenConfig()) : cfg_(c) {}

    // Once per frame. eligible: this scene may pace. busyMs: the busiest stage's cost of the last frame, <= 0 unknown (never goes in on that). intervalMs: the last frame's length.
    bool Frame(double now, unsigned scene, bool eligible, double busyMs, double intervalMs) {
        if (scene != scene_) {
            scene_ = scene;
            fails_ = 0;
            if (coolUntil_ > now + cfg_.cool) coolUntil_ = now + cfg_.cool;
        }
        const bool stall = intervalMs >= 150.0;   // a load, a card read, a suspend: no evidence of the free run's cost
        // the cost is clipped at twice the way-out limit: one hitch moves the average by a few ms, not by 90
        const double clipped = busyMs > 2.0 * cfg_.leaveMs ? 2.0 * cfg_.leaveMs : busyMs;
        if (busyMs > 0 && !stall) cost_ = cost_ > 0 ? 0.9 * cost_ + 0.1 * clipped : clipped;
        else if (busyMs <= 0) cost_ = 0;
        if (intervalMs > 0 && !stall) interval_ = interval_ > 0 ? 0.9 * interval_ + 0.1 * intervalMs : intervalMs;
        if (!eligible) {
            on_ = false;
            since_ = -1;
            return false;
        }
        if (!on_) {
            if (cost_ <= 0 || cost_ >= cfg_.enterMs) { since_ = -1; return false; }
            if (since_ < 0) since_ = now;
            if (now - since_ < cfg_.hold || now < coolUntil_) return false;
            on_ = true;
            onAt_ = now;
            ringN_ = ringAt_ = 0; sumIv_ = sumCs_ = 0;
            interval_ = 0;
            ++entries_;
            return true;
        }
        {
#if NP_BETWEEN_BREAK == 3
            const bool counted = true;
#else
            const bool counted = !stall;
#endif
            if (counted) {
                // the last OUT_FRAMES frames' lengths and costs, each clipped at three times the limit: a hitch is one frame's worth of it, not the frame
                const double iv = intervalMs > 3.0 * cfg_.leaveMs ? 3.0 * cfg_.leaveMs : intervalMs;
                const double cs = busyMs > 3.0 * cfg_.leaveMs ? 3.0 * cfg_.leaveMs : busyMs;
                if (ringN_ < kRing) ++ringN_; else { sumIv_ -= ivRing_[ringAt_]; sumCs_ -= csRing_[ringAt_]; }
                ivRing_[ringAt_] = iv; csRing_[ringAt_] = cs > 0 ? cs : 0;
                sumIv_ += iv; sumCs_ += csRing_[ringAt_];
                ringAt_ = (ringAt_ + 1) % kRing;
            }
        }
        const int win = cfg_.outFrames < kRing ? cfg_.outFrames : kRing;
        double mIv = 0, mCs = 0;   // the means over the last `win` frames
        for (int i = 1; i <= win && i <= ringN_; ++i) { mIv += ivRing_[(ringAt_ + kRing - i) % kRing]; mCs += csRing_[(ringAt_ + kRing - i) % kRing]; }
        const int have = ringN_ < win ? ringN_ : win;
        if (have > 0) { mIv /= have; mCs /= have; }
        const bool over = mIv > cfg_.leaveMs || mCs > cfg_.leaveMs;
#if NP_BETWEEN_BREAK == 2
        if (!(have >= 1 && over)) return true;
#elif NP_BETWEEN_BREAK == 4
        if (!(have >= 5 && over)) return true;   // (a window of five frames: a hitch or two closes the gate)
#else
        if (ringN_ < win || !over) return true;
#endif
        on_ = false;
        since_ = -1;
        double cool = cfg_.cool;
        #if NP_BETWEEN_BREAK == 1
        if (false) {
#else
        if (now - onAt_ < cfg_.fail) {
#endif
            if (fails_ < 8) ++fails_;
            for (int i = 0; i < fails_ && cool < cfg_.coolMax; ++i) cool *= 2.0;
            if (cool > cfg_.coolMax) cool = cfg_.coolMax;
        }
        coolUntil_ = now + cool;
        return false;
    }
    // MSC_CADENCE: the cut back to open play after a held scene. If the free run was in force before the scene (`wasOn`) it is back at once, without the hold time and the cool-down
    // that a first entry needs (a held replay is not a failed try of the free run); its failure counters are kept.
    void Resume(double now, bool wasOn) {
        since_ = -1;
        if (wasOn && !on_) {
            coolUntil_ = 0;
            on_ = true;
            onAt_ = now;
            ringN_ = ringAt_ = 0; sumIv_ = sumCs_ = 0;
            interval_ = 0;
            ++entries_;
        }
    }
    bool On() const { return on_; }
    int Entries() const { return entries_; }
    double CostMs() const { return cost_; }

private:
    BetweenConfig cfg_;
    bool on_ = false;
    unsigned scene_ = 0xFFFFFFFFu;
    double cost_ = 0, interval_ = 0, since_ = -1, onAt_ = 0, coolUntil_ = 0;
    static const int kRing = 128;
    double ivRing_[kRing] = {}, csRing_[kRing] = {}, sumIv_ = 0, sumCs_ = 0;
    int ringN_ = 0, ringAt_ = 0;
    int fails_ = 0, entries_ = 0;
};

} // namespace npfps

#endif // NP_FPS_BETWEEN_H
