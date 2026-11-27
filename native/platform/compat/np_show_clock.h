// np_show_clock.h (wave 9 helper "smooth", MSC_SMOOTH_CLOCK): the game time of a picture under a held cadence = the time it will be SHOWN. Pure code (host test:
// tools/fps/smooth_unit.cpp).
// Each picture shows the game at the moment its frame began (FixedUpdateTask's accumulator remainder is the blend factor; RenderSnapshot::Blend interpolates every bone,
// position and object between the two newest 20 ms steps), while the display shows the pictures on the display gate's cadence:
//     shown_k = max(ready_k, shown_{k-1} + n)          (vblanks; n = 2 at a held 30, 1 at 60)
// Frame starts do not keep that cadence (the late wait's anchor, the back-pressure wait), and the 2-deep queue hides a start that moved by a refresh from the hold but not
// from the motion: a picture shown for 2 refreshes then carries 1 or 3 refreshes of motion (console r10, live play, windows with every picture held 2: 8-41 % of pictures).
// The clock PREDICTS each picture's show vblank with the gate's own rule, ready_k ~ stamp_k + L (stamp: the vblank its frame began at, MSC_PRESENT_SYNC's stamp; L: the
// pipeline's latency to ready, a high quantile of the display's recent "ready after" counts), re-anchored on the display's report of what was really shown:
//     S_k = max(stamp_k + L, S_{k-1} + n),  S_{k-1} re-derived from the newest picture the display has reported
// and the frame advances the game by (S_k - S_{k-1}) refreshes. All in whole vblanks, the display's own unit: a prediction that holds gives a picture exactly the motion
// of the refreshes it is shown for. Long-run the game time is the vblank count, i.e. real time; an EARLY start (the queue holds its picture) is filtered out, a LATE start
// beyond the queue's slack (its picture will be late too) is followed, a hitch passes through (the debt clock, np_game_clock.h, then caps and owes it as before).
// Not a delay: the steps still run at the frame start and read the pad there; S - stamp is how far ahead of its frame start the picture is shown, which it already was.
#pragma once
#include <cstdint>

#ifndef NP_SHOW_BREAK
#define NP_SHOW_BREAK 0   // host test mutants: 1 L is ignored (pure cadence: late pictures spread), 2 no re-anchoring on the display's report, 3 the old clock (frame starts),
                          // 4 the chain is not carried (S_k = stamp_k + L: early starts are not filtered)
#endif

namespace npfps {

class ShowClock {
public:
    static constexpr int kRing = 16;
    // One frame at its start. active: a held cadence in live single-player play (else the clock resets and returns -1: use the frame's own time).
    // stamp: the vblank count at the frame start; n: the cadence (refreshes a picture); L: frames start to ready, refreshes; fb: the display's newest report (tag, shown vblank).
    // Returns the refreshes the game advances by this frame, or -1 (use real time: not steady, or the prediction equals the frame's own refreshes).
    // steadyRun: how many of the newest pictures in a row the display held exactly n refreshes. Below kSteady the cadence is not trusted: the frame takes its own time
    // (-1, exactly today's clock) and the chain only carries the offset, so a stretch with late pictures behaves as before.
    static constexpr unsigned kSteady = 8;   // default; SetSteady (MSC_SMOOTH_STEADY): fewer trusts the cadence sooner after a late picture (console statistics better, a pipeline with no queue slack worse)
    void SetSteady(unsigned n) { steady_ = n < 1 ? 1 : (n > 64 ? 64 : n); }
    int Begin(bool active, uint32_t stamp, int n, int L, unsigned steadyRun, bool haveFb, uint64_t fbTag, uint32_t fbShown) {
        if (!active || n <= 0) { count_ = 0; return -1; }
        if (L < 0) L = 0;
        if (count_ > 0 && n != n_) count_ = 0;
        n_ = n;
        ++frames_;
#if NP_SHOW_BREAK == 1
        L = -64;
#endif
        if (count_ == 0) { Push(stamp, stamp + (uint32_t)(L > 0 ? L : 0)); ++resets_; return -1; }
        const Rec& last = At(count_ - 1);
        if (static_cast<int32_t>(stamp - last.stamp) > 15 || static_cast<int32_t>(stamp - last.stamp) < 0) { count_ = 0; Push(stamp, stamp + (uint32_t)L); ++resets_; return -1; }   // (a stall: real time)
        // re-anchor: the newest reported picture's real show vblank, carried forward over the frames after it
        uint32_t prevS = last.S;
#if NP_SHOW_BREAK != 2
        if (haveFb && fbTag != 0) {
            for (int i = count_ - 1; i >= 0; --i) {
                const Rec& r = At(i);
                if (r.tag != fbTag) continue;
                if (r.anchored) break;   // (applied already)
                uint32_t s = fbShown;
                for (int j = i + 1; j < count_; ++j) s = Next(s, At(j).stamp, n, L);
                Mut(i).anchored = true;
                if (s != prevS) ++reanchors_;
                prevS = s;
                break;
            }
        }
#endif
        if (steadyRun < steady_) {   // not a steady cadence: today's clock, the offset carried
            Push(stamp, stamp + (last.S - last.stamp));
            ++unsteady_;
            return -1;
        }
        uint32_t S = Next(prevS, stamp, n, L);
#if NP_SHOW_BREAK == 4
        S = stamp + (uint32_t)L;
#endif
        // at most L + 2 periods ahead of the start (more slack than that is not a held cadence)
        if (static_cast<int32_t>(S - (stamp + (uint32_t)L + 2u * (uint32_t)n)) > 0) S = stamp + (uint32_t)L + 2u * (uint32_t)n;
        int adv = static_cast<int32_t>(S - last.S);
        if (adv < 0) { adv = 0; S = last.S; }
        Push(stamp, S);
#if NP_SHOW_BREAK == 3
        return static_cast<int32_t>(stamp - last.stamp);
#endif
        const int own = static_cast<int32_t>(stamp - last.stamp);
        if (adv == own) { ++agreed_; return -1; }   // (the prediction agrees with the frame's own refreshes: its own time, to the microsecond, exactly as before)
        ++changed_;
        return adv;
    }
    // The frame just begun was handed over with this tag (np_latency.cpp's frame number).
    void Handed(uint64_t tag) { if (count_ > 0) Mut(count_ - 1).tag = tag; }
    uint64_t Frames() const { return frames_; }
    uint64_t Changed() const { return changed_; }      // frames whose game time differs from their own start-to-start time (the action counter)
    uint64_t Reanchors() const { return reanchors_; }  // display reports that moved the chain
    uint64_t Resets() const { return resets_; }
    uint64_t Unsteady() const { return unsteady_; }
    uint64_t Agreed() const { return agreed_; }       // steady frames whose prediction equals their own refreshes (left to their own time)   // frames left to their own time (the cadence was not steady)
private:
    struct Rec { uint32_t stamp, S; uint64_t tag; bool anchored; };
    static uint32_t Next(uint32_t prevS, uint32_t stamp, int n, int L) {
        const uint32_t a = prevS + (uint32_t)n, b = stamp + (uint32_t)(L > 0 ? L : 0);
        return static_cast<int32_t>(a - b) >= 0 ? a : b;
    }
    void Push(uint32_t stamp, uint32_t S) {
        if (count_ == kRing) { head_ = (head_ + 1) % kRing; --count_; }
        Rec& r = ring_[(head_ + count_) % kRing];
        r.stamp = stamp; r.S = S; r.tag = 0; r.anchored = false;
        ++count_;
    }
    const Rec& At(int i) const { return ring_[(head_ + i) % kRing]; }
    Rec& Mut(int i) { return ring_[(head_ + i) % kRing]; }
    Rec ring_[kRing] = {};
    int head_ = 0, count_ = 0, n_ = 0;
    unsigned steady_ = kSteady;
    uint64_t frames_ = 0, changed_ = 0, reanchors_ = 0, resets_ = 0, unsteady_ = 0, agreed_ = 0;
};

// L from the display's counts: `natural` = vblanks from the frame start's vblank to the first vblank the picture could be on (vgx_present PresentSync "ready after").
// A high quantile of the recent pictures: a picture predicted late but ready in time is still shown on the cadence, so over-estimating L costs only the filtering of
// late starts; under-estimating it spreads real late pictures.
class LatencyLearner {
public:
    void Add(unsigned natural) { if (natural > 15) natural = 15; ring_[pos_++ & 63] = (uint8_t)natural; if (n_ < 64) ++n_; }
    bool Ready() const { return n_ >= 16; }
    int Quantile(double q = 0.8) const {
        unsigned hist[16] = {};
        for (unsigned i = 0; i < n_; ++i) ++hist[ring_[i]];
        const unsigned want = (unsigned)(q * n_ + 0.999);
        unsigned acc = 0;
        for (int v = 0; v < 16; ++v) { acc += hist[v]; if (acc >= want) return v; }
        return 15;
    }
private:
    uint8_t ring_[64] = {};
    unsigned pos_ = 0, n_ = 0;
};

} // namespace npfps
