// np_fps_bp.h (wave 11, MSC_LAT_BP): back-pressure BEFORE the pad is read, pure code (host test: tools/fps/bp_unit.cpp).
// Console r9, held 30: a picture waited about a whole 30 Hz interval for its flip (presented -> handed 49 ms of 110 pad -> shown) and in heavy moments pictures piled up
// behind a 40 ms GPU (presented -> handed 95-158 ms) because the frames-in-flight limit (depth 2, one refresh a probe) stood aside or switched itself off exactly then.
// The rule: the game thread starts a picture (and so reads the pad) only when at most `depth` earlier pictures are unshown. In a heavy moment it WAITS there (one refresh at a
// time, at most maxRefreshes a picture, then goes on: a display that stops reporting must not stop the game). Waiting before the sample costs frame rate nothing when the GPU is
// the limit: the next picture's game work (about 22 ms) overlaps the GPU's work on the one before.
// depth is not fixed: a pipeline longer than depth + 1 pictures needs more pictures in flight to keep its rate (60 fps with a 45 ms pipeline needs 3). The controller starts at
// the depth the interval needs, goes down one when the cadence has been clean for `calm` pictures, goes up one when a picture is late AND the game thread had been waiting at the
// gate (a late picture without a wait at the gate is the GPU's or the game's own cost: depth is not the cause and is left alone), and doubles `calm` for a descent that failed
// within `probe` pictures (no flapping; at most 8 times the base).
#pragma once
#include <cstdint>

#ifndef NP_BP_BREAK
#define NP_BP_BREAK 0   // host test mutants: 10 no release window at the cut back to play, 11 the held gate uses maxDepth (4), not heldDepth, 8 a held scene waits at the learned depth, 9 the controller learns from held pictures, 7 (GateRoomRefreshes) the pad late sleep is not taken off the room, 1 the depth never goes up, 2 the wait has no cap, 3 a late picture raises the depth even when the gate did not wait, 4 the depth never goes down
#endif

namespace npfps {

struct BpConfig {
    unsigned minDepth = 1, maxDepth = 4;
    unsigned heldDepth = 2;        // held24 (MSC_LAT_BP_HELD_DEPTH): depth of the gate in a held scene: at most heldDepth + 1 pictures in flight (live play 2-3), so sound leads its picture by about one picture more than live
    unsigned releasePics = 8;      // held24: pictures after the cut back to play in which the gate waits at most ONE refresh (a held scene's backlog would otherwise cost one 67-100 ms wait)
    unsigned calm = 45;            // clean pictures before trying one picture less in flight (1.5 s at 30)
    unsigned probe = 60;           // a descent that is followed by a late-and-waited picture within this many pictures failed
    unsigned maxRefreshes = 6;     // the most one picture waits at the gate
    unsigned capStrikes = 8;       // pictures in a row that hit the cap: the display is not reporting; stand aside
    unsigned asideFrames = 300;
    unsigned calmMax = 16;         // calm doubles up to this multiple
    unsigned raiseAtPerMille = 250; // share of late pictures (about 16 pictures' average) before the depth goes up: one late picture is a hitch, not a pipeline that needs more
    unsigned verify = 60;          // pictures a raise is checked for (the queue needs a few pictures to fill): when the share of late pictures has not fallen (mean gap between pictures, 5 %) the depth was not the cause (the GPU is the limit, a GPU of 60 ms is late at 30 whatever the depth): back down
    unsigned noRaise = 600;        // pictures the depth is not raised after a raise that did not help (doubles, up to 8 times)
};

class BackPressure {
public:
    explicit BackPressure(const BpConfig& c = BpConfig()) : c_(c) { calmNeed_ = c_.calm; noRaiseLen_ = c_.noRaise; }
    unsigned Depth() const { return depth_; }
    // MSC_LAT_BP_HELD: while a replay / cut-scene / walk-out is held no pad matters, so the gate waits only on a backlog over maxDepth; the learned depth_ is neither used nor changed meanwhile and is in force again at the cut back to live play.
    void SetHeld(bool h) {
#if NP_BP_BREAK != 10
        if (held_ && !h) rel_ = c_.releasePics;
#endif
        if (held_ && !h) { calm_ = 0; ema_ = 0; verify_ = 0; probing_ = 0; prevWaited_ = false; } held_ = h; }
    bool Held() const { return held_; }
    unsigned GateDepthHeld() const { return c_.heldDepth; }
    unsigned ReleaseLeft() const { return rel_; }
    unsigned GateDepth() const {
#if NP_BP_BREAK == 8
        return depth_;
#else
#if NP_BP_BREAK == 11
        return held_ ? c_.maxDepth : depth_;
#else
        return held_ ? c_.heldDepth : depth_;
#endif
#endif
    }
    bool Aside() const { return aside_ > 0; }
    unsigned Raises() const { return raises_; }
    unsigned Descents() const { return descents_; }
    unsigned Failed() const { return failed_; }
    unsigned Reverted() const { return reverted_; }
    unsigned CalmNeed() const { return calmNeed_; }
    unsigned MaxRefreshes() const {
#if NP_BP_BREAK == 2
        return 1000000u;
#else
        return c_.maxRefreshes;
#endif
    }
    // the gate's question: must the game thread wait for another refresh? sent: pictures handed over; shown: the newest picture the display reported.
    // slackRefreshes (pacing12): the refreshes the game thread can wait here and still finish the picture in its period (period - the thread's own busy time - a margin, in refreshes). r10 (console):
    // a wait made AFTER the cadence wait, with a 22 ms picture of work behind it in a 33 ms period, made the picture miss its flip: 3-refresh holds ([hitch] counts +40 %). So a wait of the first
    // refreshes over the depth is only made while it fits; a BACKLOG of more than depth + 1 pictures is made even when it does not (those pictures are late anyway: the queue is the delay).
    bool Waits(uint64_t sent, uint64_t shown, unsigned waitedSoFar, unsigned slackRefreshes = 1000) const {
        if (aside_ || shown == 0) return false;
        if (waitedSoFar >= MaxRefreshes()) return false;
        if (rel_ && waitedSoFar >= 1) return false;   // (held24: the first pictures after a held scene wait one refresh at most)
        if (sent <= shown + GateDepth()) return false;
#if NP_BP_BREAK == 6
        return true;
#else
        if (sent > shown + GateDepth() + 1) return true;
        return waitedSoFar < slackRefreshes;
#endif
    }
    // after the gate: waited refreshes this picture; gap: the game thread's entry-to-entry time in refreshes (valid only when haveGap); interval: refreshes a picture is shown for.
    void After(unsigned waited, unsigned gap, bool haveGap, int interval) {
        if (interval != interval_) {   // a new cadence (30 <-> 60, boot): start again at the depth it needs
            interval_ = interval;
            depth_ = StartDepth(interval);
            calm_ = 0; probing_ = 0; calmNeed_ = c_.calm; prevWaited_ = false; capRun_ = 0; ema_ = 0; gapEma_ = static_cast<unsigned>(interval > 0 ? interval : 1) * 1000u; verify_ = 0; noRaise_ = 0; noRaiseLen_ = c_.noRaise;
        }
        if (rel_) --rel_;
        if (aside_) { --aside_; prevWaited_ = false; return; }
#if NP_BP_BREAK != 9
        if (held_) { prevWaited_ = false; return; }   // held pictures say nothing about the depth live play needs
#endif
        if (waited >= MaxRefreshes()) { if (++capRun_ >= c_.capStrikes) { aside_ = c_.asideFrames; capRun_ = 0; } } else capRun_ = 0;
        const bool waitedNow = waited > 0;
        if (!haveGap || interval <= 0) { prevWaited_ = waitedNow; return; }
        const bool late = gap > static_cast<unsigned>(interval);   // entries are retrace-aligned: more refreshes than the period is a picture that missed its flip
#if NP_BP_BREAK == 3
        const bool cause = true;
#else
        const bool cause = waitedNow || prevWaited_;
#endif
        { const int e = static_cast<int>(ema_), x = late ? 1000 : 0; ema_ = static_cast<unsigned>(e + (x - e) / 16); const int ge = static_cast<int>(gapEma_), gx = static_cast<int>(gap) * 1000; gapEma_ = static_cast<unsigned>(ge + (gx - ge) / 16); }   // share of late pictures, per mille, and the mean gap in thousandths of a refresh, over about 16 pictures
        if (probing_) --probing_;
        if (noRaise_) --noRaise_;
        // int29: at 60 fps (interval == 1), depth 1 trips the hard backlog rule (sent > shown + 2) on every frame of the 4-stage ~45 ms Vita pipeline, cutting 60 fps in half to 30-31 fps. Floor 60 fps at depth 2 while keeping 30 fps (interval >= 2) unchanged at c_.minDepth.
        const unsigned floorDepth = (interval == 1 && c_.minDepth < 2u) ? 2u : c_.minDepth;
        if (verify_) {
            verifyGap_ += gap;
            if (--verify_ == 0) {
#if NP_BP_BREAK != 5
                if (verifyGap_ * 20000u >= c_.verify * baseline_ * 19u && depth_ > floorDepth) {   // the mean gap between pictures fell by less than 5 %: the extra picture bought nothing, only delay
                    --depth_; ++reverted_;
                    noRaise_ = noRaiseLen_;
                    noRaiseLen_ = noRaiseLen_ * 2u > c_.noRaise * 8u ? c_.noRaise * 8u : noRaiseLen_ * 2u;
                    calm_ = 0;
                }
#endif
            }
        }
        if (late && cause) {
#if NP_BP_BREAK != 1
            if (depth_ < c_.maxDepth && !noRaise_ && !verify_ && ema_ >= c_.raiseAtPerMille) { ++depth_; ++raises_; verify_ = c_.verify; verifyGap_ = 0; baseline_ = gapEma_ < 1u ? 1u : gapEma_; }
#endif
            if (probing_) { ++failed_; const unsigned cap = c_.calm * c_.calmMax; calmNeed_ = calmNeed_ * 2u > cap ? cap : calmNeed_ * 2u; }
            probing_ = 0;
            calm_ = 0;
        } else if (!late) {
            ++calm_;
#if NP_BP_BREAK != 4
            if (calm_ >= calmNeed_ && depth_ > floorDepth && !verify_) { --depth_; ++descents_; calm_ = 0; probing_ = c_.probe; }
#endif
        } else {
            calm_ = 0;   // a late picture the gate did not cause: no evidence either way
        }
        prevWaited_ = waitedNow;
    }
private:
    unsigned StartDepth(int interval) const { return interval >= 2 ? 2u : 3u; }   // (pacing12: 2 at 30, the depth of the wave 10 limit: r10 had the first window of play at 14.7 fps against 20.8 with depth 1 from the start)
    BpConfig c_;
    unsigned depth_ = 2, calm_ = 0, calmNeed_ = 45, probing_ = 0, aside_ = 0, capRun_ = 0, raises_ = 0, descents_ = 0, failed_ = 0, reverted_ = 0, verify_ = 0, verifyGap_ = 0, ema_ = 0, gapEma_ = 0, baseline_ = 0, noRaise_ = 0, noRaiseLen_ = 600;
    int interval_ = -1;
    bool prevWaited_ = false;
    bool held_ = false;
    unsigned rel_ = 0;
};

// lessons17 (MSC_LAT_BP_SLEEP=1, default off): the refreshes a picture can wait at the gate and still be finished in its period. The gate decides BEFORE the pad late sleep (MSC_PAD_LATE / MSC_PHASE_SYNC), and the
// sleep comes out of the same slack: a picture that waits a refresh here AND sleeps 10 ms after it spends the room twice (busy = the thread's own time WITHOUT its waits, the sleep is one of them). With the sleep
// added to the busy time the gate waits one refresh less exactly when the sleep has taken that refresh. periodMs: the picture's period; busyMs: the thread's own busy time; sleepUs: the pad sleep the thread makes
// after the gate (the last one's length); marginMs: 3.
inline unsigned GateRoomRefreshes(double periodMs, double busyMs, uint32_t sleepUs, double marginMs, double refreshMs) {
#if NP_BP_BREAK == 7
    const double sleepMs = 0.0;
#else
    const double sleepMs = (double)sleepUs * 1e-3;
#endif
    const double room = periodMs - busyMs - sleepMs - marginMs;
    return room > 0.0 ? static_cast<unsigned>(room / refreshMs) : 0u;
}

} // namespace npfps
