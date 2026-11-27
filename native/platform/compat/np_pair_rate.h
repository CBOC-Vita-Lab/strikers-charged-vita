// np_pair_rate.h (pacing13): the rate controller of the PAIR cadence, pure code (no clock, no I/O; host test tools/fps/pair_rate_unit.cpp).
// Owner 14 Nov 14:04: one automatic behaviour. In live play every refresh gets a picture (a FULL one, then a LIGHT one that re-uses the background: bg30); when the pair does not fit the
// game thread or the GPU, the game falls to an even 30 (full pictures held two refreshes) for a stretch and comes back. This replaces the free-run gate, its cool-downs and the PERFORMANCE
// / held-30 split as player-facing modes. Acceptance rules of 13 Nov 09:05: fast recovery by evidence (the pair's extra cost is LEARNED from the last pair, so a wrong guess costs one
// failed try and not a loop), no flapping (a window mean, hitches and stalls excused, a short doubling cool-down and a margin that grows with a failed try), switches only at a pair boundary.
//
//   enter  even 30 -> pair: the cost the pair would have = what the even 30 costs now (p80 of the last 90 pictures, game thread and GPU) + the pair's learned extra, both under ENTER less the
//          margin, for HOLD seconds, no overload, the background has room (hold11), after the cool-down
//   leave  pair -> even 30: the mean of the last 30 pairs' cost (clipped at 3 x LEAVE) over LEAVE, or more than 15 % of the last 60 pictures held two refreshes or more, or overload, or the
//          background has no room, or play ended. A try that ends within FAIL seconds of its start is a failed try: cool-down 2, 4, 8, 16, 32 s, margin + 0.5 ms (at most 3 ms)
//   a pair is never cut: the pair -> 30 switch is made after a light picture (or when no pair picture is running)
#pragma once
#include <algorithm>
#include <cstdint>

#ifndef NP_PAIR_BREAK
#define NP_PAIR_BREAK 0   // host test mutants: 1 no hysteresis (leave at the enter limit), 2 a pair is cut mid-way, 3 the learned extra is not updated, 4 hitches are not clipped (a hitch closes the pair), 5 the cool-down does not double, 6 (fast leave) counts the pairs that FIT, 7 (fast leave) looks at all 30 remembered pairs, not the last 16
#endif

namespace npfps {

struct PairConfig {
    double enterMs = 30.5, leaveMs = 33.0, holdS = 1.5, failS = 20.0, coolS = 2.0, coolMaxS = 32.0;
    double extraGameInit = 8.5, extraGpuInit = 10.0;   // bg30.md: the light picture's game thread 8.5, its GPU 7-11 plus the capture 1-2 on the full picture
    double lateShare = 0.15, marginStep = 0.5, marginMax = 3.0;
    // lessons17 (MSC_PAIR_FAST_LEAVE=<n>, default 0 = off; Galaxy's "3 of 16 pairs over 31 ms of game work switch the in-between picture off"): the mean over 30 pairs needs a second of evidence and a
    // few bad pairs among good ones never move it. With fastN > 0 the pair is also left, at a pair boundary, when fastN of the last fastOf pairs (at least 8 seen) cost more than fastMs (0: leaveMs).
    unsigned fastN = 0, fastOf = 16;
    double fastMs = 0.0;
};

struct PairPicture {
    double now = 0;
    bool live = false;          // live play (the gameplay state, single player): a held scene is never a pair
    bool bgRoom = true;         // hold11: the background ladder has room (not at the bottom)
    bool overload = false;      // NpFpsGameOverload
    int phase = 0;              // 0 an ordinary picture, 1 the full picture of a pair, 2 the light picture of a pair
    double gameMs = 0;          // game thread busy ms of this picture (its length less its waits); 0 unknown
    double gpuMs = 0;           // GPU ms of this picture; 0 unknown
    int heldRefreshes = 0;      // how many refreshes the picture was shown for (0 unknown)
};

class PairRate {
public:
    explicit PairRate(const PairConfig& c = PairConfig()) : c_(c), extraGame_(c.extraGameInit), extraGpu_(c.extraGpuInit) {}
    bool Wanted() const { return want_; }
    int Entries() const { return entries_; }
    int Leaves() const { return leaves_; }
    int FailedTries() const { return failed_; }
    double ExtraGame() const { return extraGame_; }
    double ExtraGpu() const { return extraGpu_; }
    double Margin() const { return margin_; }
    // One picture. Returns whether the pair cadence is wanted from the next picture on.
    bool Picture(const PairPicture& p) {
        if (!p.live || p.overload || !p.bgRoom) {
            // the end of play / an overload / no room: out at once (a pair boundary or none), not a failed try unless it was the overload
            if (want_ && Boundary(p)) Leave(p.now, p.overload);
            if (!p.live) { n30_ = 0; pairs_ = 0; sinceOk_ = -1; }
            if (!want_) { Note30(p); }
            return want_;
        }
        if (!want_) {
            Note30(p);
            TryEnter(p);
        } else {
            NotePair(p);
            TryLeave(p);
        }
        return want_;
    }
private:
    static const int kN30 = 90, kPairs = 30, kLate = 60;
    bool Boundary(const PairPicture& p) const {
#if NP_PAIR_BREAK == 2
        (void)p; return true;
#else
        return p.phase != 1;   // after a light picture, or an ordinary one
#endif
    }
    double Clip(double v) const {
#if NP_PAIR_BREAK == 4
        return v;
#else
        const double m = 3.0 * c_.leaveMs;
        return v > m ? m : v;
#endif
    }
    static double P80(const double* a, int n) {
        double t[kN30];
        for (int i = 0; i < n; ++i) t[i] = a[i];
        std::sort(t, t + n);
        return t[(n * 4) / 5 >= n ? n - 1 : (n * 4) / 5];
    }
    void Note30(const PairPicture& p) {
        if (p.phase == 0 && p.gameMs > 0 && p.gameMs < 150.0) {   // (a stall is no evidence)
            g30_[at30_ % kN30] = Clip(p.gameMs);
            u30_[at30_ % kN30] = p.gpuMs > 0 ? Clip(p.gpuMs) : 0.0;
            ++at30_;
            if (n30_ < kN30) ++n30_;
        }
    }
    void TryEnter(const PairPicture& p) {
        if (n30_ < kN30 / 2) { sinceOk_ = -1; return; }
        const double g = P80(g30_, n30_);
        double ut[kN30]; int m = 0;
        for (int j = 0; j < n30_; ++j) if (u30_[j] > 0) ut[m++] = u30_[j];
        const double u = m >= 10 ? P80(ut, m) : 0.0;
        const double predGame = g + extraGame_, predGpu = u > 0 ? u + extraGpu_ : 0.0;
        const double worst = predGame > predGpu ? predGame : predGpu;
        if (worst + margin_ > c_.enterMs) { sinceOk_ = -1; return; }
        if (sinceOk_ < 0) sinceOk_ = p.now;
        if (p.now - sinceOk_ < c_.holdS || p.now < coolUntil_) return;
        want_ = true;
        ++entries_;
        onAt_ = p.now;
        game30AtEntry_ = g;
        gpu30AtEntry_ = u;
        pairs_ = 0; lates_ = 0; latesAt_ = 0; fullGame_ = fullGpu_ = 0; haveFull_ = false;
        for (int i = 0; i < kLate; ++i) late_[i] = 0;
        sinceOk_ = -1;
    }
    void NotePair(const PairPicture& p) {
        if (p.heldRefreshes > 0) {
            late_[latesAt_ % kLate] = p.heldRefreshes >= 2 ? 1 : 0;
            ++latesAt_;
            if (lates_ < kLate) ++lates_;
        }
        if (p.phase == 1) { fullGame_ = p.gameMs; fullGpu_ = p.gpuMs; haveFull_ = true; return; }
        if (p.phase == 2 && haveFull_) {
            const double pg = Clip(fullGame_ + p.gameMs), pu = (fullGpu_ > 0 && p.gpuMs > 0) ? Clip(fullGpu_ + p.gpuMs) : 0.0;
            haveFull_ = false;
            pg_[pairs_ % kPairs] = pg;
            pu_[pairs_ % kPairs] = pu;
            ++pairs_;
            // learn what the pair adds to the even 30 (the full picture alone, with every step, is what the even 30 costs)
#if NP_PAIR_BREAK != 3
            if (pairs_ >= 8 && pairs_ <= 24 && game30AtEntry_ > 0) {
                const double eg = pg - game30AtEntry_;
                extraGame_ += ((eg < 0 ? 0 : eg) - extraGame_) * 0.1;
                if (pu > 0 && gpu30AtEntry_ > 0) { const double eu = pu - gpu30AtEntry_; extraGpu_ += ((eu < 0 ? 0 : eu) - extraGpu_) * 0.1; }
            }
#endif
        }
    }
    void TryLeave(const PairPicture& p) {
        if (!Boundary(p)) return;
        if (FastBad()) { Leave(p.now, true); return; }
        const int have = pairs_ < kPairs ? pairs_ : kPairs;
        double mg = 0, mu = 0; int nu = 0;
        for (int i = 0; i < have; ++i) { mg += pg_[i]; if (pu_[i] > 0) { mu += pu_[i]; ++nu; } }
        if (have > 0) mg /= have;
        if (nu > 0) mu /= nu;
        int late = 0;
        for (int i = 0; i < lates_; ++i) late += late_[i];
#if NP_PAIR_BREAK == 1
        const double leaveMs = c_.enterMs;
#else
        const double leaveMs = c_.leaveMs;
#endif
        const bool full = have >= kPairs;
        const bool cost = full && (mg > leaveMs || mu > leaveMs);
        const bool slow = lates_ >= kLate && late > c_.lateShare * lates_;
        if (cost || slow) Leave(p.now, true);
    }
    bool FastBad() const {
        if (c_.fastN == 0) return false;
#if NP_PAIR_BREAK == 7
        const int span = kPairs;
#else
        const int span = (int)c_.fastOf < kPairs ? (int)c_.fastOf : kPairs;
#endif
        const int n = pairs_ < span ? pairs_ : span;
        if (n < 8) return false;
        const double lim = c_.fastMs > 0.0 ? c_.fastMs : c_.leaveMs;
        unsigned bad = 0;
        for (int i = 0; i < n; ++i) {
            const int k = (pairs_ - 1 - i) % kPairs;
#if NP_PAIR_BREAK == 6
            if (!(pg_[k] > lim || (pu_[k] > 0 && pu_[k] > lim))) ++bad;
#else
            if (pg_[k] > lim || (pu_[k] > 0 && pu_[k] > lim)) ++bad;
#endif
        }
        return bad >= c_.fastN;
    }
    void Leave(double now, bool blame) {
        want_ = false;
        ++leaves_;
        n30_ = 0; at30_ = 0;   // (the even 30 is measured afresh)
        sinceOk_ = -1;
        if (blame && now - onAt_ < c_.failS) {
            ++failed_;
            if (fails_ < 8) ++fails_;
            double cool = c_.coolS;
#if NP_PAIR_BREAK != 5
            for (int i = 1; i < fails_ && cool < c_.coolMaxS; ++i) cool *= 2.0;
#endif
            if (cool > c_.coolMaxS) cool = c_.coolMaxS;
            coolUntil_ = now + cool;
            margin_ = margin_ + c_.marginStep > c_.marginMax ? c_.marginMax : margin_ + c_.marginStep;
        } else if (blame) {
            // a try that lasted: the margin eases, the failures are forgotten
            fails_ = 0;
            margin_ = margin_ > 0.5 ? margin_ - 0.5 : 0.0;
            coolUntil_ = now + c_.coolS;
        } else {
            coolUntil_ = now;   // not the pair's fault (play ended): no brake
        }
    }
    PairConfig c_;
    bool want_ = false;
    double g30_[kN30] = {}, u30_[kN30] = {}, pg_[kPairs] = {}, pu_[kPairs] = {};
    int late_[kLate] = {};
    int at30_ = 0, n30_ = 0, pairs_ = 0, lates_ = 0, latesAt_ = 0, entries_ = 0, leaves_ = 0, failed_ = 0, fails_ = 0;
    double extraGame_, extraGpu_, margin_ = 0, sinceOk_ = -1, coolUntil_ = 0, onAt_ = 0, game30AtEntry_ = 0, gpu30AtEntry_ = 0, fullGame_ = 0, fullGpu_ = 0;
    bool haveFull_ = false;
};

} // namespace npfps
