// np_fps_phase.h (pace10, MSC_PHASE_SYNC): the late start of a held 30, pure code (host test: tools/fps/pace10_unit.cpp, mutants NP_PHASE_BREAK 1-5).
// The same idea as np_feel.h PadLate (MSC_PAD_LATE) and the Galaxy port's GXV_LATE_TURN ("phase sync": finish just before the deadline, not as early as possible): the pad is read
// `delay` microseconds later inside the slack the picture would otherwise spend waiting for its refresh. What this controller changes against PadLate:
//   1. A picture that is late while NO delay was in force is not our doing (a GPU spike, a load): it neither zeroes the delay nor starts a hold-off. PadLate zeroed and held off for
//      120 pictures after ANY late picture; at the 1-3 % late pictures the console shows at Vice and The Dump that was most of the time (r11: 5 of 2400 steps delayed at Vice).
//   2. A picture that is late while a delay WAS in force is treated as ours, but softly: the delay is halved (not zeroed), the controller holds for `backoff` pictures, and the margin
//      widens by half a step (at most 3 times) and shrinks back after `heal` clean pictures (Galaxy late_turn.h's rule).
//   3. The slack of a picture is the smaller of the display slack and the thread's own idle time, each taken WITHOUT the delay that was in force (raw), over a window of `window`
//      pictures; the target delay is that minimum less the margin. The delay rises by `stepUp` a picture and falls at once to the target.
// It only ever shifts the start inside the period; the picture's slot on the display is not touched, no step or dt changes (the pad is read inside the frame, as with PadLate).
#pragma once
#include <algorithm>
#include <cstdint>

#ifndef NP_PHASE_BREAK
#define NP_PHASE_BREAK 0   // host test mutants: 1 a late picture with no delay in force zeroes the delay (PadLate's rule), 2 the margin never widens, 3 the display slack is not made raw (taken as seen, the delay in force is not added back), 4 no back-off at all, 5 the delay ignores the thread's idle time
#endif

namespace npfps {

struct PhaseConfig {
    uint32_t marginUs = 2500;     // MSC_PHASE_SYNC_MARGIN_US: the picture is planned to be ready this long before its slot
    uint32_t maxUs = 12000;       // MSC_PHASE_SYNC_MAX_US
    uint32_t stepUp = 250;        // the delay rises this much a picture
    unsigned window = 90;         // MSC_PHASE_SYNC_WINDOW: pictures the slack is taken over (90 = 3 s at 30)
    unsigned ignore = 3;          // the delay is planned for the (ignore+1)-th smallest slack of the window: the few worst pictures (a hitch has no slack whatever the delay) do not set it
    unsigned backoff = 12;        // pictures the delay is not raised after a late picture that a delay may have caused
    unsigned heal = 600;          // clean pictures that take one widening of the margin back
    unsigned widenAt = 2;         // late pictures with a delay in force inside `widenSpan` pictures that widen the margin by half a step
    unsigned widenSpan = 300;
    unsigned lag = 3;             // pictures between a start and the display's report of its picture (the delay in force at the picture that is being reported)
};

struct PhaseStats {
    uint32_t calls = 0, delayed = 0, ours = 0, notOurs = 0, backoffs = 0, widens = 0;
    uint64_t delayUs = 0;
    uint32_t delayMaxUs = 0;
};

class PhaseSync {
public:
    static constexpr unsigned kRing = 128;
    static constexpr uint32_t kNone = 0xFFFFFFFFu;
    PhaseSync() { Reset(); }
    void Configure(const PhaseConfig& c) {
        c_ = c;
        if (c_.window < 8) c_.window = 8;
        if (c_.window > kRing) c_.window = kRing;
        if (c_.ignore > c_.window / 4) c_.ignore = c_.window / 4;
        if (c_.maxUs > 20000) c_.maxUs = 20000;
        if (c_.lag > 7) c_.lag = 7;
        Reset();
        stats_ = PhaseStats();
    }
    void Reset() { n_ = h_ = 0; delay_ = 0; hold_ = 0; clean_ = 0; widen_ = 2; sinceLate_ = 1000000; recent_ = 0; lateRun_ = 0; lo_ = 0xFFFFFFFFu; for (unsigned i = 0; i < 8; ++i) dh_[i] = 0; }
    // One call per eligible picture (a held 30, a frame with a step), BEFORE the sleep. displayUs: the smallest handed -> shown slack of the pictures the display reported since the last call
    // (kNone when it reported none); cpuUs: the game thread's idle time of the previous frame WITHOUT the sleep (its waits less the delay); late: a reported picture came up more than a
    // period and a half after the one before it. Returns the delay of THIS picture's pad read (microseconds, 0 = none).
    uint32_t Step(uint32_t displayUs, uint32_t cpuUs, bool late) {
        ++stats_.calls;
        ++sinceLate_;
        // the delay that was in force at the picture the display is reporting (c_.lag pictures ago)
        const uint32_t dSeen = dh_[(hIdx_ + 8 - c_.lag) % 8];
        if (late) {
            // a late picture is charged to the delay only when the delay in force had eaten into half the margin of the smallest slack seen (lo_): with a delay well inside the margin a late picture is a spike (GPU, a hitch) whatever the delay was
            ++lateRun_;
            // (two late pictures in a row are not spikes: pictures that are all late never enter the window, so only this can see a slack that fell)
            const bool ours = dSeen >= 300 && (dSeen + MarginNowUs() / 2 > lo_ || lateRun_ >= 2);
#if NP_PHASE_BREAK == 1
            const bool treat = true;
#else
            const bool treat = ours;
#endif
            if (treat) {
                ++stats_.ours;
#if NP_PHASE_BREAK != 4
                ++stats_.backoffs;
                hold_ = c_.backoff;
                delay_ = delay_ / 2;
#if NP_PHASE_BREAK == 1
                delay_ = 0;
#endif
#endif
#if NP_PHASE_BREAK != 2
                if (sinceLate_ <= c_.widenSpan) { if (++recent_ >= c_.widenAt - 1 && widen_ < 6) { ++widen_; ++stats_.widens; recent_ = 0; } } else recent_ = 0;
#endif
                sinceLate_ = 0;
                clean_ = 0;
                ring_[h_] = 0; h_ = (h_ + 1) % c_.window; if (n_ < c_.window) ++n_;   // (a picture with no slack left)
            } else {
                ++stats_.notOurs;
            }
        } else {
            lateRun_ = 0;
            if (displayUs != kNone || cpuUs != kNone) {
#if NP_PHASE_BREAK == 3
                const uint32_t dRaw = displayUs;
#else
                const uint32_t dRaw = displayUs == kNone ? kNone : displayUs + dSeen;
#endif
#if NP_PHASE_BREAK == 5
                const uint32_t raw = dRaw;
#else
                const uint32_t raw = std::min(dRaw, cpuUs);
#endif
                if (raw != kNone) { ring_[h_] = raw; h_ = (h_ + 1) % c_.window; if (n_ < c_.window) ++n_; }
            }
            if (++clean_ >= c_.heal) { clean_ = 0; if (widen_ > 2) --widen_; }
        }
        if (hold_ > 0) --hold_;
        if (n_ >= c_.window) {
            uint32_t tmp[kRing];
            for (unsigned i = 0; i < n_; ++i) tmp[i] = ring_[i];
            std::nth_element(tmp, tmp + c_.ignore, tmp + n_);
            const uint32_t lo = tmp[c_.ignore];
            lo_ = lo;
            const uint32_t margin = c_.marginUs * widen_ / 2;
            uint32_t target = lo > margin ? lo - margin : 0;
            if (target > c_.maxUs) target = c_.maxUs;
            if (target < delay_) delay_ = target;
            else if (hold_ == 0) delay_ = std::min(target, delay_ + c_.stepUp);
        }
        const uint32_t d = delay_ < 300 ? 0u : delay_;   // (a sleep under 0.3 ms is not worth its own jitter)
        hIdx_ = (hIdx_ + 1) % 8;
        dh_[hIdx_] = d;
        if (d) { ++stats_.delayed; stats_.delayUs += d; stats_.delayMaxUs = std::max(stats_.delayMaxUs, d); }
        return d;
    }
    uint32_t DelayNow() const { return delay_; }
    uint32_t MarginNowUs() const { return c_.marginUs * widen_ / 2; }
    const PhaseConfig& Cfg() const { return c_; }
    const PhaseStats& Stats() const { return stats_; }
    void ClearStats() { stats_ = PhaseStats(); }
private:
    PhaseConfig c_;
    PhaseStats stats_;
    uint32_t ring_[kRing] = {};
    uint32_t dh_[8] = {};
    unsigned n_ = 0, h_ = 0, hold_ = 0, clean_ = 0, widen_ = 2, hIdx_ = 0, sinceLate_ = 0, recent_ = 0, lateRun_ = 0;
    uint32_t delay_ = 0;
    uint32_t lo_ = 0xFFFFFFFFu;   // the slack the delay was planned from (the window's low quantile)
};

} // namespace npfps
