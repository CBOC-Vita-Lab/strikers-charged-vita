// np_crowdgov.h (crowd25, MSC_CROWD_GOV): the adaptive crowd-cost governor. Pure code (no clock, no I/O but the log line formatter; host test tools/fps/crowdgov_unit.cpp).
// The owner (17 Nov 22:19): "I am ok with cheaper crowd animation when needed if that helps." RENDER SIDE ONLY: the level it holds changes how often the crowd sprites' animation pictures are redrawn
// (Game/Render/ImpostorManager.cpp), never the simulation, never a player, the ball, an item or the HUD.
//
// Inputs, one call per LIVE PLAY picture (np_fps.cpp, beside the [late] / [head] bookkeeping, the same numbers): the game thread's busy ms (the frame length less its waits), the period of the cadence in
// force (33.3 ms at the held 30, 16.7 ms at 60) and the largest of the renderer's lanes (front end, back end, GPU: the [late] lanes, ms of one picture, the largest of the last three).
//   a picture is OVER when busy > budget (87 % of the period: 14.5 ms at 60, 29 ms at 30) AND the game thread is the limiting lane (busy >= 95 % of the largest lane). A GPU- or back-end-bound
//   picture is never counted: cheaper crowd animation would not make it shorter.
// Levels 0 (the full look) .. 5. Each level asks the render side for a longer refresh interval per size class of crowd sprite (far first, then middling, then near; ImpostorManager.cpp):
//   L1 far x1.5 | L2 mid x1.5 far x2 | L3 near x1.5 mid x2 far x3 | L4 near x2 mid x3 far x3 | L5 (owner 22:23, the bare minimum: used only when L4 still misses) mid and far on a very slow idle (a picture every ~1.5 s, each section frozen on its own pose in between), near x2.   (prior save: ms of game-thread time each level takes off the one before it)
// Rules (WAVE9-RULES acceptance):
//   step down one level when, over a window of 1 s (at least 20 pictures), at least 20 % of the pictures are OVER (at least 2 s after the last move, 1 s when half of them are);
//   give back FAST when there is room: the window's busy time plus the LEARNED saving of the levels given back leaves at most 3 % of the pictures over (the saving is learned from what the
//   level changes really did to the mean busy time; the prior only starts it), as many levels at once as fit, after a hold of 2 s at the level;
//   no flapping: a give-back that is followed by a step down within 12 s FAILED: the hold (before any give-back) doubles (10, 20, 40, 60 s) and give-backs go one level at a time; a give-back that stays halves the hold again.
// Estimates: the saving figures are host callgrind and console gsplit estimates (w8/crowd25.md), not measurements of the console.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>

#ifndef NP_CROWDGOV_BREAK
#define NP_CROWDGOV_BREAK 0   // host test mutants (tools/fps/crowdgov_unit.cpp): 1 the lanes are ignored (a GPU-bound picture counts), 2 no failure hold (the hold never grows), 3 the give-back ignores the cost of what it gives back, 5 the 60 floor holds at every cadence (core60); 4 no minimum dwell between moves
#endif

namespace npcg {

enum { kMaxLevel = 5 };

// core60 (MSC_CROWD_FLOOR60): the level the render side is given. A live picture at 60 holds at least `floor60` (0 = off); everything else (30, replays, cut-scenes, menus: at60 false) gets the governor's own level.
inline int FloorAt60(int govLevel, int floor60, bool at60Live) {
    if (floor60 > kMaxLevel) floor60 = kMaxLevel;
#if NP_CROWDGOV_BREAK == 5
    at60Live = true;   // (host mutant: the floor holds at 30 and in replays too)
#endif
    return (at60Live && floor60 > govLevel) ? floor60 : govLevel;
}

struct Config {
    double window = 1.0;          // s
    int minPics = 20;
    double budgetFrac = 0.87;     // of the cadence's period
    double laneSlack = 0.95;      // the game thread limits when busy >= laneSlack * the largest lane
    double downShare = 0.20;
    double fastShare = 0.50;
    double upShare = 0.03;
    double minDwell = 2.0;        // s after any move before a step down
    double minDwellFast = 1.0;
    double hold0 = 2.0;           // s at a level before a give-back from it
    double holdFail = 10.0, holdMax = 60.0;
    double failWithin = 12.0;     // a step down this soon after a give-back from that level = the give-back failed
    double idleGap = 5.0;         // s without a live picture: the window starts again (the level stays)
    double prior[kMaxLevel + 1] = {0.0, 0.15, 0.14, 0.14, 0.06, 0.15};   // ms the game thread gains from level k over level k-1
    // nxr4a MSC_CROWD_PEND_CLEAR (default on): the saving of a move is learned from the first full window after it against the mean before it. A replay, cut-scene or menu in between (Idle) or a change of the
    // cadence's period (60 <-> 30) makes that comparison of two different scenes: the measurement is dropped (and a period change starts the window again) instead of teaching save_[] a wrong figure.
    bool clearPend = true;
};

struct Stats {
    uint32_t down = 0, up = 0, fails = 0, windows = 0;
    double timeAt[kMaxLevel + 1] = {};
    uint32_t pics = 0, over = 0, gpuBound = 0;
    int maxLevel = 0;
};

class Gov {
public:
    explicit Gov(const Config& c = Config()) : c_(c) {
        for (int k = 0; k <= kMaxLevel; ++k) save_[k] = c.prior[k];
        hold_ = c.hold0;
    }
    int Level() const { return level_; }
    const Stats& St() const { return st_; }
    double Save(int k) const { return save_[k]; }
    double Hold() const { return hold_; }
    double LastMean() const { return lastMean_; }
    double LastOverShare() const { return lastShare_; }
    // returns true when the level moved (the caller logs it)
    bool Picture(double now, double busyMs, double periodMs, double laneMaxMs) {
        if (lastNow_ >= 0.0 && now - lastNow_ > c_.idleGap) ResetWindow(now);
#if NP_CROWDGOV_BREAK != 7
        if (c_.clearPend && lastPeriod_ > 0.0 && (periodMs > lastPeriod_ + 1.0 || periodMs < lastPeriod_ - 1.0)) { pend_ = false; ResetWindow(now); }
#endif
        lastPeriod_ = periodMs;
        if (lastNow_ >= 0.0) { const double dt = now - lastNow_; st_.timeAt[level_] += dt > 0.5 ? 0.5 : dt; }
        lastNow_ = now;
        if (wStart_ < 0.0) wStart_ = now;
        const double budget = periodMs * c_.budgetFrac;
        ++n_; ++st_.pics;
        sum_ += busyMs;
#if NP_CROWDGOV_BREAK == 1
        const bool bound = true;
#else
        const bool bound = busyMs >= c_.laneSlack * laneMaxMs;
#endif
        if (busyMs > budget) { if (bound) { ++over_; ++st_.over; } else ++st_.gpuBound; }
        double cum = 0.0;
        for (int k = 1; k <= level_; ++k) {
            cum += save_[level_ - k + 1];
#if NP_CROWDGOV_BREAK == 3
            cum = 0.0;
#endif
            const double b = busyMs + cum;
            if (b > budget && b >= c_.laneSlack * laneMaxMs) ++overBack_[k];
        }
        if (now - wStart_ >= c_.window && n_ >= c_.minPics) return Decide(now);
        return false;
    }
    // why60 (MSC_CROWD_SEED60): jump up to level `to` at once (a 60 trial begins at the level the last 60 needed, not at the level the relaxed 30 gave back to). Only ever raises the level; nothing learned from it
    // (no pending saving measurement), the window starts again. Returns true when the level moved.
    bool Seed(double now, int to) {
        if (to > kMaxLevel) to = kMaxLevel;
        if (to <= level_) return false;
        pend_ = false;
        level_ = to;
        if (level_ > st_.maxLevel) st_.maxLevel = level_;
        lastMove_ = now;
        lastGbAt_ = -1.0; gbCredited_ = true;   // (a seed is not a failed give-back's step down)
        ResetWindow(now);
        return true;
    }
    // not a live play picture (a menu, a replay, a cut-scene, a load): the window starts over at the next live picture
    void Idle() {
        if (n_) ResetWindow(-1.0);
        lastNow_ = -1.0;
#if NP_CROWDGOV_BREAK != 6
        if (c_.clearPend) pend_ = false;   // (nxr4a: a measurement across a replay / cut-scene / menu compares two scenes)
#endif
    }
    void SetClearPend(bool on) { c_.clearPend = on; }
    void Line(char* out, size_t size) const {
        snprintf(out, size, "[crowdgov] level %d (max %d): %u down %u up %u failed give-backs, %u windows, %u live pictures (%u over budget with the game thread limiting, %u over with another lane limiting), s at level 0..5: %.1f %.1f %.1f %.1f %.1f %.1f; learned save ms L1..L5: %.2f %.2f %.2f %.2f %.2f; hold %.0f s",
                 level_, st_.maxLevel, st_.down, st_.up, st_.fails, st_.windows, st_.pics, st_.over, st_.gpuBound, st_.timeAt[0], st_.timeAt[1], st_.timeAt[2], st_.timeAt[3], st_.timeAt[4], st_.timeAt[5],
                 save_[1], save_[2], save_[3], save_[4], save_[5], hold_);
    }

private:
    void ResetWindow(double) {
        n_ = 0; over_ = 0; sum_ = 0.0; wStart_ = -1.0;
        for (int k = 0; k <= kMaxLevel; ++k) overBack_[k] = 0;
    }
    bool Decide(double now) {
        ++st_.windows;
        const double mean = sum_ / n_;
        const double share = (double)over_ / n_;
        lastMean_ = mean; lastShare_ = share;
        // what the last move really did to the mean busy time (the first full window after it)
        if (pend_) {
            const double delta = pendUp_ ? mean - pendBefore_ : pendBefore_ - mean;   // ms the level to saves, whichever way it moved
            if (delta > 0.02 && delta < 4.0) {
                const int lv = pendUp_ ? pendLevel_ : pendLevel_;   // the level whose saving it is: the higher of the two
                double v = 0.5 * save_[lv] + 0.5 * delta;
                if (v < 0.03) v = 0.03;
                if (v > 2.0) v = 2.0;
                save_[lv] = v;
            }
            pend_ = false;
        }
        // credit a give-back that stayed
        if (lastGbAt_ >= 0.0 && !gbCredited_ && now - lastGbAt_ > c_.failWithin) {
            gbCredited_ = true;
#if NP_CROWDGOV_BREAK != 2
            hold_ = hold_ * 0.5 < c_.hold0 ? c_.hold0 : hold_ * 0.5;
#endif
        }
        bool moved = false;
        const double since = now - lastMove_;
#if NP_CROWDGOV_BREAK == 4
        const double dwell = 0.0;
#else
        const double dwell = share >= c_.fastShare ? c_.minDwellFast : c_.minDwell;
#endif
        if (level_ < kMaxLevel && share >= c_.downShare && since >= dwell) {
            if (lastGbAt_ >= 0.0 && !gbCredited_ && now - lastGbAt_ <= c_.failWithin && level_ + 1 == lastGbFrom_) {
                gbCredited_ = true;
                ++st_.fails;
#if NP_CROWDGOV_BREAK != 2
                double h = hold_ * 2.0;
                if (h < c_.holdFail) h = c_.holdFail;
                hold_ = h > c_.holdMax ? c_.holdMax : h;
#endif
            }
            Move(now, level_ + 1, mean);
            ++st_.down;
            moved = true;
        } else if (level_ > 0 && since >= hold_) {
            int best = 0;
            for (int k = 1; k <= level_; ++k)
                if ((double)overBack_[k] / n_ <= c_.upShare && (k == 1 || hold_ <= c_.hold0)) best = k;   // (after a failed give-back: one level at a time)
            if (best > 0) {
                lastGbFrom_ = level_;
                lastGbAt_ = now;
                gbCredited_ = false;
                Move(now, level_ - best, mean);
                ++st_.up;
                moved = true;
            }
        }
        ResetWindow(now);
        return moved;
    }
    void Move(double now, int to, double meanBefore) {
        pend_ = true;
        pendUp_ = to < level_;                 // gave back: busy rises
        pendLevel_ = to < level_ ? level_ : to;
        pendBefore_ = meanBefore;
        level_ = to;
        if (level_ > st_.maxLevel) st_.maxLevel = level_;
        lastMove_ = now;
    }

    Config c_;
    Stats st_;
    int level_ = 0;
    double save_[kMaxLevel + 1], hold_;
    // window
    double wStart_ = -1.0, lastNow_ = -1.0, sum_ = 0.0, lastPeriod_ = -1.0;
    int n_ = 0, over_ = 0, overBack_[kMaxLevel + 1] = {};
    double lastMove_ = -1e9, lastMean_ = 0.0, lastShare_ = 0.0;
    // learning
    bool pend_ = false, pendUp_ = false;
    int pendLevel_ = 0;
    double pendBefore_ = 0.0;
    // failure tracking
    double lastGbAt_ = -1.0;
    int lastGbFrom_ = 0;
    bool gbCredited_ = true;
};

}  // namespace npcg
