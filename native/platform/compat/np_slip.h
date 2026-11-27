// np_slip.h: the slip budget (MSC_SLIP_BUDGET_MS), pure code (no clock, no I/O) so the game (np_fps.cpp SlipFrame) and the host test (tools/fps/slip_budget_unit.cpp) run the same thing.
//
// The bounded slip (MSC_SLIP, MSC_SLIP_STEPS) cuts the game time a LATE frame advances by: max(limit, dt / (1 + pct)), at most n steps. That rule has no memory, so a scene that is late
// EVERY frame (a 60 target the picture hold cannot meet and the controller has not given up yet, a held 30 that takes 40-50 ms) runs the game slow for as long as it lasts: at 33 ms a
// frame against a 16.7 ms target the game advances 27.8 ms, 83 % of real time, and the picture looks even while the game is in slow motion. The budget gives the cut a memory: a bucket
// of `capacity` seconds that fills at `refill` (a fraction, 0.02 = 2 %) of the real time of every frame; a late frame may cut at most what the bucket holds. A single hitch is cut by what
// the old rule would cut (up to the bucket); a run of late frames drains it and then advances in full: sustained, the game clock loses at most `refill` of real time, and in any window of
// W seconds at most refill * W + capacity.
#pragma once

#ifndef NP_SLIP_BREAK
#define NP_SLIP_BREAK 0   // host test mutants: 1 refill x10, 2 a cut is not paid for, 3 the bucket has no ceiling, 4 an on-time frame is cut too
#endif

namespace npfps {

// The old rule (what NpSlipDelta computed before the budget), kept in one place: the game time (s) a frame of `dt` s advances by. limit: the on-time bound (s); pct: MSC_SLIP; steps: MSC_SLIP_STEPS
// (0 = no cap). *capLost: seconds the step cap alone took off (diagnostic).
inline float SlipAdvance(float dt, float limit, int pct, int steps, float* capLost) {
    if (capLost) *capLost = 0.0f;
    if (dt <= limit) return dt;
    const float floor_ = dt / (1.0f + (float)pct / 100.0f);
    float adv = limit > floor_ ? limit : floor_;
    if (adv > dt) adv = dt;
    if (steps > 0) {
        float cap = (float)steps * 0.02f;
        if (cap < limit) cap = limit;
        if (adv > cap) {
            if (capLost) *capLost = adv - cap;
            adv = cap;
        }
    }
    return adv;
}

class SlipBudget {
public:
    void Configure(float capacityS, float refillFrac) {
        cap_ = capacityS > 0.0f ? capacityS : 0.0f;
        refill_ = refillFrac > 0.0f ? refillFrac : 0.0f;
#if NP_SLIP_BREAK == 1
        refill_ *= 10.0f;
#endif
        bucket_ = cap_;
    }
    bool On() const { return cap_ > 0.0f; }
    float Bucket() const { return bucket_; }
    float Capacity() const { return cap_; }
    // One frame: dt real seconds, want the seconds the old rule would cut from it (0 for an on-time frame). Returns the game time the frame advances by.
    float Frame(float dt, float want) {
        bucket_ += refill_ * dt;
#if NP_SLIP_BREAK != 3
        if (bucket_ > cap_) bucket_ = cap_;
#endif
#if NP_SLIP_BREAK == 4
        if (want <= 0.0f) want = dt * 0.05f;
#endif
        if (want <= 0.0f) return dt;
        const float cut = want < bucket_ ? want : bucket_;
#if NP_SLIP_BREAK != 2
        bucket_ -= cut;
#endif
        return dt - cut;
    }
    void Refill() { bucket_ = cap_; }   // (a new match starts with a full bucket)
private:
    float cap_ = 0.0f, refill_ = 0.0f, bucket_ = 0.0f;
};

} // namespace npfps
