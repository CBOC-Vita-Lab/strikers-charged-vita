// np_fps45.h (helper f45, 18 Nov 2026; MSC_FPS=45, alias MSC_CADENCE45=1): the 45 FPS cadence. Pure code, no platform calls (host test: native/tools/fps/f45_unit.cpp).
// The Vita's screen refreshes at a fixed 59.94 Hz, so 45 pictures a second is a fixed repeating pattern of holds: 1, 1, 2 refreshes (16.7, 16.7, 33.4 ms; 3 pictures in 4 refreshes, 44.96 a
// second). Three pieces, one per thread, sharing only the PHASE of that pattern:
//   Pattern45  the slots. A vblank v is a slot when (v - phase) mod 4 is 0, 1 or 2; a picture on slot 0 or 1 is held 1 refresh, on slot 2 held 2. Any three pictures in a row span 4 refreshes.
//   Gate45     the display (vgx_host_vita.cpp GateDisplay): each picture goes up on the NEXT SLOT after the one before, or on the slot its game time asks for (the game's prediction, below,
//              when that is later), or, when it is ready too late for that slot, on the first slot it can make. It never goes up on a vblank that is not a slot, so a late picture costs
//              its slot in the pattern, never a random extra refresh, and the phase is kept across hitches. Only a stall longer than `stall` refreshes starts the pattern again (phase taken
//              from the late picture, which goes up at once: a clean resync).
//   Clock45    the game time (np_fps.cpp NpFpsBeginFrame -> the slip/debt clock -> every gameplay task's dt -> FixedUpdateTask's accumulator, whose remainder is the render blend):
//              each frame's picture is predicted to be shown on S_k = the first slot at or after max(next slot after S_{k-1}, frame start + L) (L: start-to-ready refreshes, learned by
//              the display), re-anchored on the display's newest report; the frame advances the game by S_k - G_{k-1} refreshes, where G_{k-1} is the slot the previous picture's game time
//              stands for. So every picture shows the world at the vblank it will appear on (to the refresh), whatever the uneven holds and the frame start times; long-run the game
//              time is the vblank count (real time). The prediction is also the display's target, so the two agree unless a picture is late.
//   Lead45     the start (Fable's review, 18 Nov): a frame starts `lead` refreshes before the slot its picture is predicted for (np_fps.cpp's gate before the pad read), not when some
//              earlier picture is reported shown: the report and the game thread's wake-up sit on the same vblank edge, so a count of pictures makes the start (and the pad read) wander
//              by a refresh with the scheduler. Every picture then has the same start-to-screen time (lead x 16.7 ms: 83 / 67 / 50 ms at 5 / 4 / 3). The lead is learned: it starts at 5,
//              goes up at once when pictures come ready after their slot, and comes down a refresh only after 8 s in which the heaviest pictures (the ones after the long hold: they
//              carry the 2-refresh advance, so the 2-step pictures) all came ready more than a refresh + a guard early; a lowering that fails is blocked for 20 s, then 40, 80, ...
//   Fallback45 the rate: more than lateShare (8 %) of a window of pictures off their next slot (late, or held to a later predicted slot: both are an uneven hold on the screen), or
//              more than 4 % of the last 360 (8 s: a steady two hitches a second) -> a locked 30 at full size; back to 45 only after a minimum hold (25 s, doubled for each fallback in the last 180 s) AND a proof (12 s of pictures whose estimated cost at 45
//              fits); the third fallback of a match latches the 30 until the match ends. The LOCK60_STICKY rules on the 45 / 30 pair, with a harder latch (the owner hates flapping).
// Nothing here is read by the simulation: the steps are the same fixed 20 ms steps with the same inputs; the frame rate only decides how many run per picture.
#pragma once
#include <cstdint>

#ifndef NP_FLAP_BREAK
#define NP_FLAP_BREAK 0   // (tools/fps/tier_flap_unit.cpp mutants; the same macro np_fps_ctl.h defines)
#endif
#ifndef NP_HOLD_BREAK
#define NP_HOLD_BREAK 0   // (tools/fps/hold60_unit.cpp mutants; np_fps_ctl.h defines the same macro)
#endif
#ifndef NP_T24_BREAK
#define NP_T24_BREAK 0   // (tools/fps/trap24_unit.cpp mutants: 1 the relaxed proof ignores the mean, 2 no wait after the hold, 3 a lenient mean)
#endif
#ifndef NP_P24_BREAK
#define NP_P24_BREAK 0   // (tools/fps/pace24_unit.cpp mutants; np_fps_ctl.h defines the same macro)
#endif
#ifndef NP_P98_BREAK
#define NP_P98_BREAK 0   // (tools/fps/p98_unit.cpp mutants; np_fps_ctl.h defines the same macro)
#endif
#ifndef NP_F50_BREAK
#define NP_F50_BREAK 0   // (tools/fps/f50_unit.cpp mutants: 1 MSC_FPS=45 follows the 50 flag, 2 the 50 never falls to the 30, 3 the 60 -> 50 entry holds the last 60 picture 2 refreshes, 4 the seeded entry is ignored by the gate, 5 the clock seed is ignored)
#endif
#ifndef NP_F45_BREAK
#define NP_F45_BREAK 0   // host test mutants (native/tools/fps/f45_unit.cpp; each must fail it):
                         //  1 the display counts the pattern (1, 1, 2 by picture number from the last shown vblank), not the phase: after a late picture the pattern wanders
                         //  2 wrong alpha: the game advances by the frame's own start-to-start refreshes (the 30/60 paths' time), not the show slots
                         //  3 the phase is taken again at every late picture (not only after a long stall)
                         //  4 no hysteresis: the fallback 30 returns to 45 at the first picture that fits (no minimum hold, no proof)
                         //  5 the fallback ignores late pictures (never goes to 30)
                         //  6 a late picture goes up on the vblank it is ready (off the pattern: a random extra refresh)
                         //  7 wrong alpha, off by one picture: the frame advances by the PREVIOUS picture's hold
                         //  8 the lead never goes up (pictures that come ready after their slot do not start the frames earlier)
                         //  9 the lead comes down without the proof of room (any clean stretch lowers it: it flaps against a pipeline that needs it)
                         // 10 the fallback has no slow window (a steady 5 % of pictures off their slot, two hitches a second, never goes to 30)
#endif

namespace npfps {

// (f50, 24 Nov) The pattern is N pictures in M refreshes: 3 / 4 is the 45's 1, 1, 2; 5 / 6 is the 50's 1, 1, 1, 1, 2 (49.95 a second). A vblank v is a slot when (v - phase) mod M is below N (the last
// refresh of the M is the one a slot is not on); the picture on position N-1 is held through it (M - N + 1 = 2 refreshes), the others 1. The default is the 45, so every user of Pattern45 that
// does not name a pattern is the 45 exactly as before. (v - phase) mod M uses the unsigned difference: exact for M = 4, and for M = 6 only the 2^32 wrap of the vblank count (2.3 years) is a glitch.
struct Pattern45 {
    static const unsigned kEvery = 7u;   // (Fable, MSC_LOCK60_TIERS) the "phase" of the 60 pattern: every vblank is a slot, held 1 (3 bits in the display's report: above any M). The gate and the clock then give 60 its show-time
                                         // game time: a picture that lands a refresh late shows the world of the refresh it appears on (one hold, no positional jerk)
    unsigned n = 3u, m = 4u;
    Pattern45() {}
    Pattern45(unsigned pictures, unsigned refreshes) : n(pictures), m(refreshes) {}
    double Fps(double refreshHz = 59.94) const { return refreshHz * static_cast<double>(n) / static_cast<double>(m); }
    double PeriodRefreshes() const { return static_cast<double>(m) / static_cast<double>(n); }   // average refreshes a picture (the budget a picture has: 4/3 at 45, 6/5 at 50)
    unsigned Pos(uint32_t v, unsigned phase) const { return phase >= kEvery ? 0u : (v - phase) % m; }
#if NP_F45_BREAK == 11
    bool IsSlot(uint32_t v, unsigned phase) const { return Pos(v, phase) != 3u; }   // (the 45's skipped position, whatever the pattern)
#else
    bool IsSlot(uint32_t v, unsigned phase) const { return Pos(v, phase) < n; }
#endif
    uint32_t AtOrAfter(uint32_t v, unsigned phase) const { return IsSlot(v, phase) ? v : v + 1u; }
    uint32_t After(uint32_t v, unsigned phase) const { return AtOrAfter(v + 1u, phase); }
#if NP_F45_BREAK == 12
    unsigned Hold(uint32_t slot, unsigned phase) const { return Pos(slot, phase) == 2u ? 2u : 1u; }   // (the 45's long hold position, whatever the pattern)
#else
    unsigned Hold(uint32_t slot, unsigned phase) const { return Pos(slot, phase) == n - 1u ? m - n + 1u : 1u; }   // refreshes a picture on this slot stays
#endif
    // The phase a stretch of this pattern starts with when its first picture goes up on vblank s (that picture is position 0)
    unsigned PhaseAt(uint32_t s) const {
#if NP_F45_BREAK == 13
        return s & 3u;   // (the 45's modulus, whatever the pattern)
#else
        return s % m;
#endif
    }
    // slots strictly after a and at or before b (a slot skipped by a late picture counts once)
    unsigned SlotsBetween(uint32_t a, uint32_t b, unsigned phase) const {
        unsigned k = 0;
        for (uint32_t v = a + 1u; static_cast<int32_t>(b - v) >= 0 && k < 64u; ++v) if (IsSlot(v, phase)) ++k;
        return k;
    }
};

// ---- the display -------------------------------------------------------------------------------------------------------------------------------------------------------------------
struct Gate45Config {
    uint32_t stall = 8;      // a picture ready more than this many refreshes after its slot starts the pattern again (a long stall: a load, a GPU stall); a shorter hitch keeps the phase
    uint32_t maxAhead = 4;   // the game's target is followed when it is at most this many refreshes after the next slot (more is not a 45 picture: ignored)
    bool enterOnTarget = false;   // (f50, MSC_LOCK60_USE50) the first picture of a stretch goes up on the slot its game time stands for when that is a little after its earliest vblank (the clock was seeded from the tier before): its display time is its game time
};

class Gate45 {
public:
    explicit Gate45(const Gate45Config& c = Gate45Config(), bool every = false, const Pattern45& pat = Pattern45()) : c_(c), every_(every), pat_(pat) { if (every) phase_ = Pattern45::kEvery; }
    const Pattern45& Pat() const { return pat_; }
    bool Have() const { return have_; }
    unsigned Phase() const { return phase_; }
    // The vblank the picture is to go up on. earliest: the first vblank it can be on (the display callback's vcount + 1). target: the game's prediction (haveTarget false: none).
    // havePrev / prevShown: the picture before it when that one was NOT a 45 picture (the entry from the 30): it keeps its 2 refreshes, and the pattern starts with this picture.
    uint32_t Due(uint32_t earliest, bool haveTarget, uint32_t target, bool havePrev, uint32_t prevShown) {
        lastLate_ = false;
        lastPlanned_ = false;
        if (!have_) {
            uint32_t s = earliest;
            if (havePrev && static_cast<int32_t>(prevShown + 2u - s) > 0) s = prevShown + 2u;
#if NP_F50_BREAK == 4
            (void)0;
#else
            if (c_.enterOnTarget && haveTarget) { const int32_t a = static_cast<int32_t>(target - s); if (a > 0 && a <= static_cast<int32_t>(c_.maxAhead)) s = target; }
#endif
            phase_ = every_ ? Pattern45::kEvery : pat_.PhaseAt(s);
            ++resyncs_;
            return s;
        }
#if NP_F45_BREAK == 1
        uint32_t want = last_ + (count_ % pat_.n == pat_.n - 1u ? 2u : 1u);
#else
        uint32_t want = pat_.After(last_, phase_);
#endif
        if (static_cast<int32_t>(earliest - want) > static_cast<int32_t>(c_.stall)) {   // a long stall: the pattern starts again at this picture, which goes up at once
            phase_ = every_ ? Pattern45::kEvery : pat_.PhaseAt(earliest);
            ++resyncs_;
            ++stalls_;
            return earliest;
        }
        if (haveTarget && pat_.IsSlot(target, phase_)) {
            const int32_t a = static_cast<int32_t>(target - want);
            if (a > 0 && a <= static_cast<int32_t>(c_.maxAhead)) { want = target; lastPlanned_ = true; ++planned_; }
        }
        uint32_t due = want;
        if (static_cast<int32_t>(earliest - want) > 0) {
            lastLate_ = true;
            ++late_;
#if NP_F45_BREAK == 3
            phase_ = pat_.PhaseAt(earliest);
            ++resyncs_;
            due = earliest;
#elif NP_F45_BREAK == 6
            due = earliest;
#else
            due = pat_.AtOrAfter(earliest, phase_);
#endif
            slipSlots_ += pat_.SlotsBetween(want, due, phase_);
        }
        return due;
    }
    // The vblank the picture really went up on (the wait loop can overshoot when the thread wakes late: such a picture is counted off the pattern and the phase is still kept).
    void Shown(uint32_t shown) {
        if (have_) {
            const uint32_t hold = shown - last_;
            ++holds_[hold >= 3u ? 3u : hold];
            holdSum_ += hold > 8u ? 8u : hold;   // (pace24: for the delivered rate)
            if (shown != pat_.After(last_, phase_)) ++slips_;
        }
        if (!pat_.IsSlot(shown, phase_)) ++offPattern_;
        last_ = shown;
        have_ = true;
        ++count_;
        ++pictures_;
    }
    void Leave() { have_ = false; count_ = 0; }   // the next picture is not a 45 one (a held scene, the fallback 30): the entry rule applies when 45 comes back
    bool LastLate() const { return lastLate_; }
    bool LastPlanned() const { return lastPlanned_; }
    uint32_t LastShown() const { return last_; }
    // counters (all since the start)
    uint32_t pictures_ = 0, late_ = 0, slipSlots_ = 0, slips_ = 0, planned_ = 0, offPattern_ = 0, resyncs_ = 0, stalls_ = 0, holds_[4] = {0, 0, 0, 0}, holdSum_ = 0;
private:
    Gate45Config c_;
    bool every_ = false;
    Pattern45 pat_;
    uint32_t last_ = 0, count_ = 0;
    unsigned phase_ = 0;
    bool have_ = false, lastLate_ = false, lastPlanned_ = false;
};

// ---- the game time -----------------------------------------------------------------------------------------------------------------------------------------------------------------
class Clock45 {
public:
    explicit Clock45(const Pattern45& pat = Pattern45()) : pat_(pat) {}
    const Pattern45& Pat() const { return pat_; }
    static constexpr int kRing = 16;
    // One frame at its start. active: the 45 cadence is in force for this frame (live single-player play). stamp: the vblank count at the frame start. L: frame start to ready, refreshes
    // (the display's learned quantile). fb: the display's newest report (frame tag, the vblank it went up on, the gate's phase then; fbPhase < 0: none yet).
    // Returns the refreshes the game advances by this frame, or -1 (the frame takes its own time: nothing is known yet, a stall, or the first frame of a stretch).
    int Begin(bool active, uint32_t stamp, int L, bool haveFb, uint64_t fbTag, uint32_t fbShown, int fbPhase) {
        lastValid_ = false;
        if (!active) { count_ = 0; phase_ = -1; return -1; }
        if (L < 0) L = 0;
        if (L > 12) L = 12;
        ++frames_;
        if (count_ > 0) {
            const int32_t d = static_cast<int32_t>(stamp - At(count_ - 1).stamp);
            if (d > 15 || d < 0) { count_ = 0; ++resets_; }   // (a stall: the chain starts again from the display's reports)
        }
        uint32_t prevS = 0u;
        const bool havePrev = Anchor(L, haveFb, fbTag, fbShown, fbPhase, &prevS);
        if (phase_ < 0 && havePrev && seeded_) {   // (f50) the seeded first frame: its slot is position 0 of the new stretch (or every vblank, at 60)
            if (seedPhase_ >= 0) phase_ = seedPhase_;
            else { const uint32_t a = prevS + 1u, b = stamp + static_cast<uint32_t>(L); phase_ = static_cast<int>(pat_.PhaseAt(static_cast<int32_t>(a - b) >= 0 ? a : b)); }
        }
        seeded_ = false;
        if (phase_ < 0 || !havePrev) { Push(stamp, 0u, 0u, false, false); return -1; }
        uint32_t S = Next(prevS, stamp, L);
        const uint32_t cap = pat_.AtOrAfter(stamp + static_cast<uint32_t>(L) + 4u, static_cast<unsigned>(phase_));   // (more slack than 3 pictures is not this cadence)
        if (static_cast<int32_t>(S - cap) > 0) S = cap;
        const Rec& last = At(count_ - 1);
        if (!last.gValid) { Push(stamp, S, S, true, true); ++first_; return -1; }   // (the first predicted frame keeps its own time; its slot is the chain's start)
        const uint32_t minS = pat_.After(last.G, static_cast<unsigned>(phase_));   // (never the slot of the picture before: that one is shown there)
        if (static_cast<int32_t>(minS - S) > 0) S = minS;
        int adv = static_cast<int32_t>(S - last.G);
#if NP_F45_BREAK == 2
        adv = static_cast<int32_t>(stamp - last.stamp);
#elif NP_F45_BREAK == 7
        adv = lastAdv_ > 0 ? lastAdv_ : adv;
#endif
        lastAdv_ = static_cast<int32_t>(S - last.G);
        Push(stamp, S, S, true, true);
        lastValid_ = true;
        ++predicted_;
        if (adv != static_cast<int32_t>(stamp - last.stamp)) ++changed_;
        return adv;
    }
    // (f50) the first frame of a stretch after the other tier's: that tier's newest frame had its game slot gPrev (stamp: the vblank it began at). The chain starts from it, and the first frame of this tier is predicted at once
    // (its slot is the first one after gPrev that its start + L allows; its advance is the refreshes to there): the first picture at the new rate shows its display time, not its own start-to-start time. phase: the pattern's
    // phase to use (Pattern45::kEvery for the 60 tier), -1: this pattern's, with that first slot as position 0 (the gate takes the same phase from the same slot).
    void Seed(uint32_t stamp, uint32_t gPrev, int phase = -1) {
#if NP_F50_BREAK == 5
        (void)stamp; (void)gPrev; (void)phase;
#else
        count_ = 0; head_ = 0; phase_ = -1; seedPhase_ = phase; seeded_ = true;
        Push(stamp, gPrev, gPrev, true, true);
#endif
    }
    bool Empty() const { return count_ == 0; }
    // The gate before the next frame (np_fps.cpp, after the hand-over of the newest frame): the slot the NEXT frame's picture will be planned for when it starts in time (the slot after
    // the newest frame's, on the display's newest report). False: nothing is known yet (the chain has not started): the gate uses its count of pictures only.
    bool NextSlot(int L, bool haveFb, uint64_t fbTag, uint32_t fbShown, int fbPhase, uint32_t* slot) {
        if (count_ == 0) return false;
        if (L < 0) L = 0;
        if (L > 12) L = 12;
        uint32_t prevS = 0u;
        if (!Anchor(L, haveFb, fbTag, fbShown, fbPhase, &prevS) || phase_ < 0) return false;
        const Rec& last = At(count_ - 1);
        if (!last.gValid) return false;
        uint32_t S = pat_.After(prevS, static_cast<unsigned>(phase_));
        const uint32_t minS = pat_.After(last.G, static_cast<unsigned>(phase_));
        if (static_cast<int32_t>(minS - S) > 0) S = minS;
        *slot = S;
        return true;
    }
    // The frame just begun was handed over with this tag (np_latency.cpp's frame number).
    void Handed(uint64_t tag) { if (count_ > 0) Mut(count_ - 1).tag = tag & 0xFFFFFFFFull; }
    // The newest frame's game slot (the display's target for its picture); valid only when its Begin returned a prediction or started the chain.
    bool NewestTarget(uint32_t* g) const { if (count_ == 0 || !At(count_ - 1).gValid) return false; *g = At(count_ - 1).G; return true; }
    int Phase() const { return phase_; }
    uint64_t frames_ = 0, predicted_ = 0, changed_ = 0, reanchors_ = 0, resets_ = 0, phaseChanges_ = 0, first_ = 0;
private:
    struct Rec { uint32_t stamp, S, G; uint64_t tag; bool valid, gValid, anchored; };
    // The display's newest report applied to the chain (once per report): the reported frame's slot is where it went up, the frames after it are derived again. Returns whether the
    // newest frame has a slot, and that slot.
    bool Anchor(int L, bool haveFb, uint64_t fbTag, uint32_t fbShown, int fbPhase, uint32_t* prevSOut) {
        bool havePrev = count_ > 0 && At(count_ - 1).valid;
        uint32_t prevS = havePrev ? At(count_ - 1).S : 0u;
        if (haveFb && fbTag != 0 && fbPhase >= 0) {
            for (int i = count_ - 1; i >= 0; --i) {
                Rec& r = Mut(i);
                if (r.tag != (fbTag & 0xFFFFFFFFull)) continue;
                if (r.anchored && fbPhase == phase_) break;   // (applied already)
                if (fbPhase != phase_) { if (phase_ >= 0) ++phaseChanges_; phase_ = fbPhase; }
                uint32_t s = fbShown;
                r.S = s; r.valid = true; r.anchored = true;
                for (int j = i + 1; j < count_; ++j) { Rec& n = Mut(j); s = Next(s, n.stamp, L); n.S = s; n.valid = true; }
                if (!havePrev || s != prevS) ++reanchors_;
                prevS = s;
                havePrev = true;
                break;
            }
        }
        *prevSOut = prevS;
        return havePrev;
    }
    uint32_t Next(uint32_t prevS, uint32_t stamp, int L) const {
        const unsigned p = static_cast<unsigned>(phase_);
        const uint32_t a = pat_.After(prevS, p), b = stamp + static_cast<uint32_t>(L);
        return pat_.AtOrAfter(static_cast<int32_t>(a - b) >= 0 ? a : b, p);
    }
    void Push(uint32_t stamp, uint32_t S, uint32_t G, bool valid, bool gValid) {
        if (count_ == kRing) { head_ = (head_ + 1) % kRing; --count_; }
        Rec& r = ring_[(head_ + count_) % kRing];
        r.stamp = stamp; r.S = S; r.G = G; r.tag = 0; r.valid = valid; r.gValid = gValid; r.anchored = false;
        ++count_;
    }
    const Rec& At(int i) const { return ring_[(head_ + i) % kRing]; }
    Rec& Mut(int i) { return ring_[(head_ + i) % kRing]; }
    Pattern45 pat_;
    Rec ring_[kRing] = {};
    int head_ = 0, count_ = 0, phase_ = -1, seedPhase_ = -1;
    bool seeded_ = false;
    int32_t lastAdv_ = 0;
    bool lastValid_ = false;
};

// ---- the start: how many refreshes before its slot a frame begins --------------------------------------------------------------------------------------------------------------------
struct Lead45Config {
    int minLead = 3, maxLead = 5, startLead = 5;   // refreshes (50 / 67 / 83 ms start-to-screen)
    unsigned raiseWindow = 120;   // two pictures ready after their slot within this many pictures: one refresh more, at once
    unsigned lowerProof = 120;    // this many judged pictures in a row with room (and no late picture between them): one refresh less (8 s: only one picture in three is judged)
    unsigned settle = 12;         // pictures after a change that are not judged (made with the lead before it)
    unsigned failWindow = 450;    // a raise this soon after a lowering: the lowering failed
    unsigned block = 900;         // pictures a failed lowering blocks the next one for (20 s), doubled each time, at most 14400 (5 min 20 s)
    double guardMs = 6.0;         // room = ready more than a refresh + this before its slot's vblank (the game thread's p95 is 5 to 8 ms over its median on the console)
};
// What the display knows of one picture, for the lead (display thread; np_fps.cpp NpFps45Due). A picture is JUDGED for room only when the one before it was held 2 refreshes: the display
// callback of a picture cannot begin before the one before it has left, so after a hold of 1 no picture can be seen more than a refresh early; after the hold of 2 it can. That picture
// is also the heaviest of the three: its frame advances the game by 2 refreshes (1 or 2 steps, 2 two times in three).
struct Lead45Verdict {
    bool late = false, judged = false, roomy = false;
    // due / earliest: the gate's answer and the first vblank the picture could be on; prevHold: refreshes the picture before it is held; marginMs: time from ready to the vblank `due`
    // (< 0: not known, whole refreshes are used: room needs 2 of them); late: Gate45::LastLate; planned: Gate45::LastPlanned (held to a later slot by its game time: says nothing).
    static Lead45Verdict Of(uint32_t due, uint32_t earliest, unsigned prevHold, double marginMs, bool late, bool planned, double guardMs, double refreshMs) {
        Lead45Verdict v;
        v.late = late;
        if (late || planned || prevHold != 2u) return v;
        v.judged = true;
        if (marginMs >= 0.0) v.roomy = marginMs >= refreshMs + guardMs;
        else v.roomy = static_cast<int32_t>(due - earliest) >= 2;
        return v;
    }
};
class Lead45 {
public:
    explicit Lead45(const Lead45Config& c = Lead45Config()) : c_(c), lead_(c.startLead), blockLen_(c.block) {}
    int Lead() const { return lead_; }
    // fixed > 0: the tester's lead (MSC_F45_LEAD=3..5), nothing is learned
    void Fix(int lead) { fixed_ = true; lead_ = lead < c_.minLead ? c_.minLead : (lead > c_.maxLead ? c_.maxLead : lead); }
    // one picture shown at 45. Returns +1 / -1 when the lead changes now, else 0.
    int Picture(const Lead45Verdict& v) {
        ++n_;
        if (fixed_) return 0;
        if (block_) --block_;
        if (settle_) { --settle_; return 0; }
        ++sinceChange_;
        if (v.late) {
            ++late_;
            run_ = 0;
#if NP_F45_BREAK == 8
            return 0;
#else
            const bool second = haveLate_ && n_ - lastLateAt_ <= c_.raiseWindow;
            haveLate_ = true;
            lastLateAt_ = n_;
            if (!second) return 0;
            if (lead_ >= c_.maxLead) { ++atMax_; return 0; }
            if (lastWasLower_ && sinceChange_ <= c_.failWindow) { ++failed_; block_ = blockLen_; if (blockLen_ < 14400u) blockLen_ *= 2u; }
            ++lead_; ++raises_;
            Changed(false);
            return 1;
#endif
        }
        if (!v.judged) return 0;
#if NP_F45_BREAK == 9
        ++run_;
#else
        if (v.roomy) ++run_; else run_ = 0;
#endif
        if (run_ < c_.lowerProof || block_ || lead_ <= c_.minLead) return 0;
        --lead_; ++lowers_;
        Changed(true);
        return -1;
    }
    // leaving live play (a held scene, a pause): the proof starts again; the lead is kept
    void Pause() { run_ = 0; haveLate_ = false; }
    // a new match: the starting lead, nothing held against a lowering
    void MatchEnd() { if (!fixed_) lead_ = c_.startLead; run_ = 0; haveLate_ = false; block_ = 0; blockLen_ = c_.block; settle_ = 0; lastWasLower_ = false; }
    unsigned raises_ = 0, lowers_ = 0, failed_ = 0, atMax_ = 0, late_ = 0;
private:
    void Changed(bool lower) { settle_ = c_.settle; sinceChange_ = 0; run_ = 0; haveLate_ = false; lastWasLower_ = lower; }
    Lead45Config c_;
    int lead_;
    bool fixed_ = false, haveLate_ = false, lastWasLower_ = false;
    unsigned n_ = 0, lastLateAt_ = 0, run_ = 0, settle_ = 0, sinceChange_ = 0, block_ = 0, blockLen_;
};

// ---- the rate: 45, or the locked 30 at full size, with hysteresis -------------------------------------------------------------------------------------------------------------------
struct Fallback45Config {
    unsigned window = 90;        // pictures judged (2 s at 45)
    double lateShare = 0.08;     // more than this share of the window late: 30
    unsigned slowWindow = 360;   // (Fable's review) the slow window, 8 s of live play: a steady share of hitches under the fast limit is still worse to look at than an even 30
    double slowShare = 0.04;     // more than this share of it off their slot (over 14 of 360: about two 50 ms holds a second): 30
    unsigned settle = 45;        // pictures after (re)entering 45 that are not judged (the cut into play, the ladder's first step)
    double minHold = 25.0;       // seconds the 30 is held at least after a fallback, doubled for each fallback in the last stickyWindow seconds (at most 240)
    double stickyWindow = 180.0;
    int latchAfter = 3;          // this many fallbacks in a match latch the 30 until the match ends (MatchEnd)
    double latchSecs = 0.0;      // > 0 (MSC_LOCK60_TIERS: 240, Lock60's own latch): the latch ends after this long and the proof decides again; 0: until the match ends
    double proofSecs = 12.0;     // and only after this many seconds of pictures whose estimated cost at 45 fits
    double proofOutliers = 0.10; // share of pictures in the proof that may not fit
    double relapseSecs = 0.0;    // pace13 (MSC_TIER_RELAPSE, 0 = off): a fallback within this many seconds of the return to 45 counts as two in the fall history (hold 25 -> 50 s, the latch one sooner)
    double maxHold = 240.0;      // the doubled hold's cap
    // wave 24 (pace24, MSC_F45_LATEONLY, default off): the caller feeds Picture45 only the pictures that missed their slot (not the ones planned for a later slot by their game time: those are on
    // the show-time clock's own cadence), and the 45 falls on the DELIVERED rate instead: the last rateWindow pictures delivered under rateMin a second (the planned holds count there).
    unsigned rateWindow = 0;     // 0: no rate rule (every path before pace24)
    double rateMin = 37.0;
    bool sliding = false;        // hold60: the proof is a sliding window of the last proofPics pictures (a lone picture that does not fit does not start it again)
    unsigned proofPics = 120;    //   its size (at most 256)
    // wave 24 (trap24, MSC_F45_RETRY, default off): the 30's proof judges each picture against fitMs (20 ms, the longest slot): a scene whose game thread sits at 17-18 ms (the 45 ran it clean for 50 s)
    // has a third of its pictures over, more than the 10 % allowed, and the proof never passes. relaxAfter > 0: this many seconds after the hold is over the proof is judged on the MEAN estimated cost
    // of its pictures (at most relaxMean ms, the 45's own average slot is 22.2) with the GPU / lane inputs still judged per picture; a retry that fails falls again with the doubled hold.
    double relaxAfter = 0.0;
    double relaxMean = 18.0;
    // wave 24 (p98, MSC_LOCK60_TRIAL with MSC_F45_RETRY=1, default off): the 30 -> 45 return by TRIAL, in the style of Auto's trial of 60. trialAfter > 0: trialAfter seconds after a fall (no hold) and at every cut back into
    // play, the 45 is tried for trialSecs (after trialSkip pictures); it is kept when at most (1 - trialPct) of its pictures were late, else back to the 30 with the wait doubled (trialAfter, 2x ... trialMax);
    // the wait resets after trialKeep seconds at 45 or a new match. Never when the GPU / lane inputs were over fitMs for more than proofOutliers of the last 120 pictures (a GPU-bound scene stays at 30).
    double trialAfter = 0.0, trialSecs = 2.0, trialPct = 0.98, trialMax = 20.0, trialKeep = 30.0;
    unsigned trialSkip = 23;
    double fps = 44.96;          // (f50) the pictures a second of the mid tier: 44.96 (the 45), 49.95 (the 50); the trial's length in pictures
    // hold60 (MSC_LOCK60_HOLD=1, np_fps_ctl.h Config::Lock60Hold): the 30 only when 45 truly cannot hold, and back sooner. Fast window 15 % of 90 (8 % before: the console's falls at Vice were
    // 8 or 9 of 90 in the first seconds of play), slow window 5 % of 360, 90 pictures settle after entering 45; hold 10 s doubling to 120 s, no latch; proof 8 s (hold60b; 5 before) and the last 120 pictures
    // with at most 10 % not fitting.
    static Fallback45Config Hold() {
        Fallback45Config c;
#if NP_HOLD_BREAK != 6
        c.lateShare = 0.15; c.slowShare = 0.05; c.settle = 90;
#endif
        c.minHold = 10.0; c.maxHold = 120.0; c.latchAfter = 1 << 30; c.latchSecs = 0.0;
        c.proofSecs = 8.0; c.proofOutliers = 0.10; c.sliding = true; c.proofPics = 120;   // (hold60b: 8 s, was 5: a 60 that falls into the held 30 is not followed by the 45 within 8 s)
        return c;
    }
};

class Fallback45 {
public:
    explicit Fallback45(const Fallback45Config& c = Fallback45Config()) : c_(c) {}
    bool At45() const { return at45_; }
    double HoldUntil() const { return holdUntil_; }
    double LastWait() const { return lastWait_; }
    unsigned Fallbacks() const { return fallbacks_; }
    unsigned Returns() const { return returns_; }
    unsigned WindowLate() const { return late_; }
    unsigned WindowN() const { return n_ < c_.window ? n_ : c_.window; }
    unsigned SlowLate() const { return slowLate_; }
    unsigned SlowN() const { return n_ < c_.slowWindow ? n_ : c_.slowWindow; }
    bool LastSlow() const { return lastSlow_; }   // the last fallback was the slow window's
    bool LastRate() const { return lastRate_; }   // pace24: ... the delivered-rate rule's
    unsigned ProofN() const { return proofN_; }
    double RelaxMean() const { return c_.relaxMean; }
    unsigned Relaxed() const { return relaxed_; }
    unsigned Trials() const { return trials_; }
    unsigned TrialsOk() const { return trialOk_; }
    unsigned TrialsFailed() const { return trialFail_; }
    double TrialBackoff() const { return backoff_ > 0.0 ? backoff_ : c_.trialAfter; }
    bool TrialOn() const { return trialOn_; }
    bool LastTrialStart() const { return lastTrialStart_; }
    bool LastTrialFail() const { return lastTrialFail_; }
    bool LastTrialOk() const { return lastTrialOk_; }   // trap24: returns to 45 that the relaxed proof allowed
    double ProofMean() const { return proofN_ ? pEstSum_ / static_cast<double>(proofN_) : 0.0; }
    unsigned ProofBad() const { return proofBad_; }
    // a picture shown at 45 (late: the display's verdict that it did not go up on its next slot). Returns true when the rate goes to 30 now.
    // holdRefreshes (pace24): refreshes the picture was held on the screen, 0 = not known (no rate rule); with Fallback45Config::rateWindow the delivered rate is judged as well
    bool Picture45(bool late, double now, unsigned holdRefreshes = 0) {
        if (!at45_) return false;
        if (settleLeft_) { --settleLeft_; return false; }
        lastRate_ = false;
        if (c_.rateWindow > 0 && holdRefreshes > 0) {
            const unsigned W = c_.rateWindow > 256u ? 256u : c_.rateWindow;
            if (n_ >= W) rateSum_ -= holds8_[(pos_ - W) & 511u];
            holds8_[pos_ & 511u] = static_cast<unsigned char>(holdRefreshes > 8u ? 8u : holdRefreshes);
            rateSum_ += holds8_[pos_ & 511u];
            rateN_ = n_ + 1 < W ? n_ + 1 : W;
        }
        // one ring, two windows: the last `window` pictures (late_) and the last `slowWindow` (slowLate_)
        if (n_ >= c_.window && bits_[(pos_ - c_.window) & 511u]) --late_;
        if (n_ >= c_.slowWindow && bits_[(pos_ - c_.slowWindow) & 511u]) --slowLate_;
        bits_[pos_ & 511u] = late ? 1 : 0;
        if (late) { ++late_; ++slowLate_; }
        ++pos_;
        if (n_ < 512u) ++n_;
        lastTrialFail_ = lastTrialOk_ = false;
        if (trialOn_) {
            ++trialN_; if (late) ++trialLate_;
            const unsigned total = static_cast<unsigned>(c_.trialSecs * c_.fps + 0.5);
            const unsigned allow = static_cast<unsigned>((1.0 - c_.trialPct) * static_cast<double>(total) + 1e-9);
            const bool over = trialLate_ > allow;
#if NP_P98_BREAK == 4
            const bool done = false; (void)done;
            const bool ok = false;
#else
            const bool ok = trialN_ >= total && !over;
#endif
            if (over || trialN_ >= total) {
                trialOn_ = false;
                if (ok) { ++trialOk_; keptAt_ = now; lastTrialOk_ = true; Restart(); return false; }
                ++trialFail_; lastTrialFail_ = true;
                const double was = backoff_ > 0.0 ? backoff_ : c_.trialAfter;
                double nb = was * 2.0;
#if NP_P98_BREAK == 5
                nb = was;
#endif
                backoff_ = nb > c_.trialMax ? c_.trialMax : nb;
                nextTrial_ = now + backoff_;
                holdUntil_ = now; at45_ = false; proofStart_ = -1.0; proofN_ = proofBad_ = 0;
                return true;
            }
            return false;
        }
        if (backoff_ > 0.0 && keptAt_ > 0.0 && now - keptAt_ >= c_.trialKeep) { backoff_ = 0.0; keptAt_ = -1e9; }
#if NP_F45_BREAK == 5
        return false;
#else
        const bool fast = n_ >= c_.window && static_cast<double>(late_) > c_.lateShare * static_cast<double>(c_.window);
#if NP_F45_BREAK == 10
        const bool slow = false;
#else
        const bool slow = n_ >= c_.slowWindow && static_cast<double>(slowLate_) > c_.slowShare * static_cast<double>(c_.slowWindow);
#endif
        bool rate = false;
        if (c_.rateWindow > 0 && holdRefreshes > 0) {
            const unsigned W = c_.rateWindow > 256u ? 256u : c_.rateWindow;
#if NP_P24_BREAK == 4
            rate = false;
#elif NP_P24_BREAK == 5
            rate = rateN_ >= W && rateSum_ > 0 && 59.94 * static_cast<double>(W) / static_cast<double>(rateSum_) > c_.rateMin;
#else
            rate = rateN_ >= W && rateSum_ > 0 && 59.94 * static_cast<double>(W) / static_cast<double>(rateSum_) < c_.rateMin;
#endif
        }
        if (!fast && !slow && !rate) return false;
        lastSlow_ = !fast && slow;
        lastRate_ = !fast && !slow && rate;
#endif
        if (c_.relapseSecs > 0.0 && NP_FLAP_BREAK != 2 && now - enteredAt_ < c_.relapseSecs) {   // a relapse: it counts twice
            if (nFall_ < 8) fall_[nFall_++] = now; else { for (int i = 1; i < 8; ++i) fall_[i - 1] = fall_[i]; fall_[7] = now; }
            ++relapses_;
        }
        int recent = 1;
        for (int i = 0; i < nFall_; ++i) if (now - fall_[i] < c_.stickyWindow) ++recent;
        if (nFall_ < 8) fall_[nFall_++] = now; else { for (int i = 1; i < 8; ++i) fall_[i - 1] = fall_[i]; fall_[7] = now; }
        double wait = c_.minHold;
        for (int i = 1; i < recent && wait < c_.maxHold; ++i) wait *= 2.0;
        if (wait > c_.maxHold) wait = c_.maxHold;
        ++matchFalls_;
        latched_ = matchFalls_ >= static_cast<unsigned>(c_.latchAfter);
        latchedAt_ = now;
#if NP_F45_BREAK == 4
        wait = 0.0;
        latched_ = false;
#endif
        lastWait_ = wait;
        holdUntil_ = now + wait;
        if (c_.trialAfter > 0.0) { if (backoff_ <= 0.0) backoff_ = c_.trialAfter; nextTrial_ = now + backoff_; }
        at45_ = false;
        ++fallbacks_;
        proofStart_ = -1.0; proofN_ = proofBad_ = 0;
        return true;
    }
    // a live picture at the fallback 30: fits = its estimated cost at 45 (game thread with 45's steps, the GPU at the 45 ladder's floor) is inside the 45 budget. Returns true when 45 comes back now.
    // estMs / hwOk (trap24): the picture's estimated game-thread cost at 45 and whether the GPU / lane inputs fit; -1 = not known (the relaxed proof stays silent)
    bool Picture30(bool fits, double now, double estMs = -1.0, bool hwOk = true) {
        lastTrialStart_ = false;
        if (at45_) return false;
        if (latched_) { if (c_.latchSecs > 0.0 && now >= latchedAt_ + c_.latchSecs) { latched_ = false; holdUntil_ = now; } else return false; }
#if NP_F45_BREAK == 4
        if (fits) { Enter(); return true; }
        return false;
#endif
        if (c_.trialAfter > 0.0) {
            // hardware verdict history that survives a cut: a GPU / lane bound scene is not retried
            if (hwN_ == 128u) hwBad_ -= hwBits_[hwPos_]; else ++hwN_;
            hwBits_[hwPos_] = hwOk ? 0 : 1; hwBad_ += hwBits_[hwPos_]; hwPos_ = (hwPos_ + 1) & 127u;
#if NP_P98_BREAK == 7
            const bool hwFine = true;
#else
            const bool hwFine = hwN_ < 40u || static_cast<double>(hwBad_) <= c_.proofOutliers * static_cast<double>(hwN_);
#endif
            const bool cut = cutPending_;
            cutPending_ = false;
#if NP_P98_BREAK == 6
            const bool cutOk = false;
#else
            const bool cutOk = cut;
#endif
            if (hwFine && hwOk && (cutOk || now >= nextTrial_)) {
                trialOn_ = true; trialN_ = trialLate_ = 0; ++trials_; at45_ = true; lastTrialStart_ = true;
                Restart(); settleLeft_ = c_.trialSkip;
                return true;
            }
        }
        if (c_.sliding) {   // hold60: the last proofPics pictures, at most proofOutliers of them not fitting, over at least proofSecs
            const unsigned need = c_.proofPics < 1u ? 1u : (c_.proofPics > 256u ? 256u : c_.proofPics);
            if (proofStart_ < 0.0) { proofStart_ = now; proofN_ = proofBad_ = 0; pPos_ = 0; pEstSum_ = 0.0; pHwBad_ = 0; pEstKnown_ = 0; }
            if (proofN_ == need) { proofBad_ -= pBits_[pPos_]; pEstSum_ -= pEst_[pPos_]; pHwBad_ -= pHw_[pPos_]; pEstKnown_ -= pKnown_[pPos_]; } else ++proofN_;
            pBits_[pPos_] = fits ? 0 : 1;
            proofBad_ += pBits_[pPos_];
            pEst_[pPos_] = estMs >= 0.0 ? (estMs > 60.0 ? 60.0f : static_cast<float>(estMs)) : 0.0f; pKnown_[pPos_] = estMs >= 0.0 ? 1 : 0; pHw_[pPos_] = hwOk ? 0 : 1;
            pEstSum_ += pEst_[pPos_]; pHwBad_ += pHw_[pPos_]; pEstKnown_ += pKnown_[pPos_];
            pPos_ = (pPos_ + 1) % need;
#if NP_HOLD_BREAK == 7
            return false;
#endif
            if (now >= holdUntil_ && proofN_ == need && now - proofStart_ >= c_.proofSecs && static_cast<double>(proofBad_) <= c_.proofOutliers * static_cast<double>(need)) { enteredAt_ = now; Enter(); return true; }
#if NP_T24_BREAK == 2
            const bool relaxOn = c_.relaxAfter > 0.0;   // mutant: the retry is tried at once (no wait after the hold)
#else
            const bool relaxOn = c_.relaxAfter > 0.0 && now >= holdUntil_ + c_.relaxAfter;
#endif
#if NP_T24_BREAK == 1
            const bool meanOk = pEstKnown_ == need;   // mutant: the mean is not judged
#elif NP_T24_BREAK == 3
            const bool meanOk = pEstKnown_ == need && pEstSum_ <= c_.relaxMean * static_cast<double>(need) * 4.0;   // mutant: a mean four times too lenient
#else
            const bool meanOk = pEstKnown_ == need && pEstSum_ <= c_.relaxMean * static_cast<double>(need);
#endif
            if (relaxOn && proofN_ == need && now - proofStart_ >= c_.proofSecs && meanOk && static_cast<double>(pHwBad_) <= c_.proofOutliers * static_cast<double>(need)) { ++relaxed_; enteredAt_ = now; Enter(); return true; }
            return false;
        }
        if (proofStart_ < 0.0) { if (!fits) return false; proofStart_ = now; proofN_ = proofBad_ = 0; }
        ++proofN_;
        if (!fits) ++proofBad_;
        if (proofN_ >= 20 && static_cast<double>(proofBad_) > c_.proofOutliers * static_cast<double>(proofN_)) { proofStart_ = -1.0; proofN_ = proofBad_ = 0; return false; }
        if (now >= holdUntil_ && now - proofStart_ >= c_.proofSecs && proofN_ >= static_cast<unsigned>(c_.proofSecs * 20.0)) { enteredAt_ = now; Enter(); return true; }
        return false;
    }
    // leaving live play (a held scene, a pause, a load): the judging window starts again at the next live picture; a fallback's hold and its proof keep running (time only)
    void Pause() { cutPending_ = true; trialOn_ = false; if (at45_) Restart(); else { proofStart_ = -1.0; proofN_ = proofBad_ = 0; } }
    // the match is over (the front end, a load): a new match starts at 45 with nothing held against it
    void MatchEnd() { backoff_ = 0.0; nextTrial_ = 0.0; trialOn_ = false; keptAt_ = -1e9; if (!at45_) ++returns_; enteredAt_ = -1e9; at45_ = true; latched_ = false; matchFalls_ = 0; nFall_ = 0; holdUntil_ = 0.0; Restart(); proofStart_ = -1.0; proofN_ = proofBad_ = 0; }
    bool Latched() const { return latched_; }
    unsigned Relapses() const { return relapses_; }
    void SetLatchSecs(double s) { c_.latchSecs = s; }
    void SetRelapseSecs(double s) { c_.relapseSecs = s; }
    // pace13 MSC_TIER_MEMORY: a match that settled at the full-size 30 starts there: the hold (secs) and the proof apply before the 45 is tried, no fallback is counted (no latch nearer)
    void SeedFallback(double now, double secs) { at45_ = false; latched_ = false; holdUntil_ = now + secs; lastWait_ = secs; proofStart_ = -1.0; proofN_ = proofBad_ = 0; Restart(); }
private:
    void Enter() { at45_ = true; ++returns_; Restart(); }
    void Restart() { rateSum_ = rateN_ = 0; lastRate_ = false; n_ = late_ = slowLate_ = pos_ = 0; settleLeft_ = c_.settle; for (unsigned i = 0; i < 512; ++i) bits_[i] = 0; }
    Fallback45Config c_;
    bool at45_ = true, latched_ = false, lastSlow_ = false;
    unsigned matchFalls_ = 0;
    unsigned n_ = 0, late_ = 0, slowLate_ = 0, pos_ = 0, settleLeft_ = 45, fallbacks_ = 0, returns_ = 0, proofN_ = 0, proofBad_ = 0;
    unsigned char bits_[512] = {};   // (window and slowWindow are at most 512)
    unsigned char pBits_[256] = {}, pHw_[256] = {}, pKnown_[256] = {};   // (trap24: the relaxed proof's per-picture hardware verdict and whether its estimate was known)
    float pEst_[256] = {};
    double pEstSum_ = 0.0;
    unsigned pHwBad_ = 0, pEstKnown_ = 0, relaxed_ = 0;  // (hold60: the sliding proof)
    unsigned char holds8_[512] = {}; // (pace24: refreshes each of the last pictures was held, for the delivered rate)
    unsigned rateSum_ = 0, rateN_ = 0;
    bool lastRate_ = false;
    unsigned pPos_ = 0;
    bool trialOn_ = false, cutPending_ = false, lastTrialStart_ = false, lastTrialFail_ = false, lastTrialOk_ = false;
    unsigned trialN_ = 0, trialLate_ = 0, trials_ = 0, trialOk_ = 0, trialFail_ = 0, hwN_ = 0, hwBad_ = 0, hwPos_ = 0;
    unsigned char hwBits_[128] = {};
    double backoff_ = 0.0, nextTrial_ = 0.0, keptAt_ = -1e9;
    double fall_[8] = {}, holdUntil_ = 0.0, lastWait_ = 0.0, proofStart_ = -1.0, latchedAt_ = 0.0;
    int nFall_ = 0;
    double enteredAt_ = -1e9;
    unsigned relapses_ = 0;
};


// wave 24 (p98, MSC_LOCK60_USE45; owner 24 Nov: "Concentrate on 60 and 30 only. Remove 45 from it for now."): the 45 is a tier of live play only when allowed (the flag, or MSC_FPS=45's own mode).
// np_fps.cpp F45Allowed() / the live decision use these two; tools/fps/p98_unit.cpp tests them (mutant NP_P98_BREAK=8: the flag is ignored).
inline bool Tier45Allowed(bool modeIs45, bool use45) {
#if NP_P98_BREAK == 8
    (void)use45; (void)modeIs45; return true;
#else
    return modeIs45 || use45;
#endif
}
// (f50, MSC_LOCK60_USE50) The mid tier's pattern: the 50 (5 pictures in 6 refreshes) when MSC_LOCK60_USE50 asks for it inside Smooth 60's tiers, else the 45 (3 in 4). MSC_FPS=45 (modeIs45) is always the 45;
// MSC_LOCK60_USE45 alone is the 45 exactly as before. Mutant NP_F50_BREAK=1: MSC_FPS=45 follows the 50 flag.
inline Pattern45 MidPattern(bool modeIs45, bool use50) {
#if NP_F50_BREAK == 1
    (void)modeIs45;
    return use50 ? Pattern45(5u, 6u) : Pattern45(3u, 4u);
#else
    return (use50 && !modeIs45) ? Pattern45(5u, 6u) : Pattern45(3u, 4u);
#endif
}
// the mid tier is allowed: MSC_FPS=45, MSC_LOCK60_USE45 or MSC_LOCK60_USE50
inline bool MidAllowed(bool modeIs45, bool use45, bool use50) { return Tier45Allowed(modeIs45, use45 || use50); }
// the picture is made at 45: live single-player play, the 45 allowed and not on its fallback 30
inline bool Tier45Live(bool livePlay, bool allowed, bool at45) { return livePlay && allowed && at45; }
} // namespace npfps
