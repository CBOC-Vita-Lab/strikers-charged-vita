// np_game_clock.h (pacing12, MSC_CLOCK=debt): the game clock of live play, pure code (host test: tools/fps/game_clock_unit.cpp, judged by np_speed.h's SpeedMeter).
// The owner's rule: NO SLOWDOWNS, game time >= 98 % of real time in live play. The wave 9 slip budget cut a late frame's game time (up to a 20 % cut, a two-step catch-up cap, a bucket that
// refilled at 2 % of real time) and so held the game AT 98.0 % on a clean match and lower wherever the bucket was dry; the nlTask 100 ms clamp took the rest of any longer frame without a trace
// (the ledger said 98.1 % while worstgpu1's steps said 95.4 %). The clock keeps a DEBT instead: real time not yet advanced. A late frame advances the game by as much as the game thread can
// run in one picture (capMs, from the measured cost of a picture and of a step), the rest is owed and is paid back by the next pictures, which run more steps than their own time needs.
//   - the debt is bounded (maxDebtMs): what does not fit is SHED (the game runs behind real time for good), and that is the OVERLOAD signal;
//   - a stall (real frame over stallMs: a load, a card read, a suspend) is forgiven, not owed: the debt is left as it was;
//   - overload: more than 2 % of the real time of a second of live play was SHED (the debt was full; stalls excluded): `Overload()`, read by the picture and frame-rate controllers. (A debt that is
//     being paid back is not an overload: the speed meter reads it as a short dip, which is the price of a hitch, bounded by maxDebtMs.)
// The honest behaviour when the game thread cannot do it (a step costs more than the picture it is in): the game runs slow by the shed share, logged, and the controllers lower the picture rate
// (a held 30 instead of a free run) or the picture cost. The clock never makes the slow-down look like a smooth cadence.
#pragma once
#include <cmath>
#include <cstdint>

#ifndef NP_SPREAD_BREAK
#define NP_SPREAD_BREAK 0   // host test mutant (tools/fps/hold60_unit.cpp): 1 no spread (a late picture at 60 is caught up in the next picture, two steps)
#endif
#ifndef NP_CLOCK_BREAK
#define NP_CLOCK_BREAK 0   // host test mutants: 1 the debt is not carried (a late frame's cut is lost), 2 the cap is one step, 3 the debt has no bound, 4 overload is never signalled, 5 the repayment ignores the cap
#endif

namespace npfps {

struct ClockConfig {
    double stepMs = 20.0;
    double maxDebtMs = 100.0;      // the largest game time owed at any moment: 2 % of 5 s
    double stallMs = 250.0;        // a frame longer than this is forgiven (a load, a card read, a suspend)
    unsigned minCapSteps = 2, maxCapSteps = 5;   // 5 x 20 ms = nlTask's 100 ms task clamp
    double overloadPct = 98.0, overloadWindowMs = 1000.0;
};

// The steps a picture can run: what fits in the longer of the picture the cadence aims at and the pictures actually seen, after the picture's own work.
inline unsigned CapSteps(const ClockConfig& c, double targetMs, double seenMs, double perPictureMs, double stepCostMs) {
    const double ref = seenMs > targetMs ? seenMs : targetMs;
    const double room = ref - perPictureMs;
    unsigned n = stepCostMs > 0.5 && room > 0 ? static_cast<unsigned>(room / stepCostMs) : c.minCapSteps;
    if (n < c.minCapSteps) n = c.minCapSteps;
    if (n > c.maxCapSteps) n = c.maxCapSteps;
#if NP_CLOCK_BREAK == 2
    n = 1;
#endif
    return n;
}

// hold60b (MSC_LOCK60_SPREAD, default on with SMOOTH 60's hold, single-player live play at the 60 tier only): the catch-up after a late picture is SPREAD. Console s50 (forced 60, Vice): after one
// picture misses its refresh the next one is given two refreshes of game time, runs two steps (+4.6 ms), misses in turn, and the 60 stays at two steps a picture ("game steps 2 vblanks" on
// every picture): a picture that fits one refresh with one step (up to 16.7 ms) and not with two (over 12.1 ms) cannot get out. While the debt is at most one step, a picture at 60 runs at
// most ONE step (cap 20 ms of game time: the step accumulator then never holds two); the owed time is paid by the next pictures, 3.3 ms each (20 - 16.7), so one missed refresh is repaid in
// about four pictures of one step, the game a step behind real time for that long (the owner's "smooth loss of a frame", no slow motion: the debt is repaid, not shed). A debt over one step (two
// late pictures in a row or more) takes the ordinary cap (two steps or more): a sustained shortfall runs real time and is the hold's to judge (60 falls on its delivered rate).
// pace24 (MSC_LOCK60_SPREAD_CAP, default 0 = the step above): debtCapMs > one step lets the spread take a debt of up to that many ms (40 = two steps): two late pictures in a row (the second
// finds 30 ms owed) are then repaid one step a picture as well, not by a two-step picture that misses in turn. The debt is still repaid in full (3.3 ms a picture), never shed: no slow motion;
// a debt over the cap takes the ordinary cap as before (maxDebtMs 100 still bounds it).
inline unsigned Spread60Cap(unsigned capSteps, double debtMs, const ClockConfig& c, double debtCapMs = 0.0) {
#if NP_SPREAD_BREAK == 1
    (void)debtMs; (void)c; (void)debtCapMs;
    return capSteps;
#else
    const double lim = debtCapMs > c.stepMs ? debtCapMs : c.stepMs;
#if NP_SPREAD_BREAK == 2
    (void)lim;
    return debtMs <= c.stepMs ? 1u : capSteps;   // (mutant: the debt cap is ignored)
#else
    return debtMs <= lim ? 1u : capSteps;
#endif
#endif
}

class GameClock {
public:
    explicit GameClock(const ClockConfig& c = ClockConfig()) : c_(c) {}
    // One live frame: realMs the wall time since the previous frame (unclamped), capSteps from CapSteps. Returns the game time (ms) this frame advances by.
    double Frame(double realMs, unsigned capSteps) {
        if (!(realMs > 0)) return 0;
        const double cap = capSteps * c_.stepMs;
        double adv;
        if (realMs > c_.stallMs) {
            adv = cap < realMs ? cap : realMs;
            forgivenMs_ += realMs - adv;
            ++stalls_;
        } else {
#if NP_CLOCK_BREAK == 1
            const double want = realMs;
#else
            const double want = realMs + debt_;
#endif
#if NP_CLOCK_BREAK == 5
            adv = want;
#else
            adv = want < cap ? want : cap;
            if (adv < realMs && realMs <= cap) adv = realMs;   // (never less than the frame's own time when the thread can run it)
#endif
            double owed = want - adv;
            if (owed < 0) owed = 0;
#if NP_CLOCK_BREAK != 3
            if (owed > c_.maxDebtMs) { shedMs_ += owed - c_.maxDebtMs; wShed_ += owed - c_.maxDebtMs; owed = c_.maxDebtMs; }
#endif
            debt_ = owed;
            Window(realMs, adv);
        }
        totalReal_ += realMs;
        totalAdv_ += adv;
        return adv;
    }
    void Reset() { debt_ = 0; wReal_ = wAdv_ = wShed_ = 0; over_ = false; }
    double DebtMs() const { return debt_; }
    double ShedMs() const { return shedMs_; }
    double ForgivenMs() const { return forgivenMs_; }
    unsigned Stalls() const { return stalls_; }
    double TotalPercent() const { return totalReal_ > 0 ? 100.0 * totalAdv_ / totalReal_ : 100.0; }
    bool Overload() const {
#if NP_CLOCK_BREAK == 4
        return false;
#else
        return over_;
#endif
    }
    unsigned OverloadEntries() const { return entries_; }
private:
    void Window(double real, double adv) {
        wReal_ += real; wAdv_ += adv;
        if (wReal_ >= c_.overloadWindowMs) {
            const bool under = 100.0 * wShed_ / wReal_ > 100.0 - c_.overloadPct;
            if (under && !over_) ++entries_;
            over_ = under;
            wReal_ = wAdv_ = wShed_ = 0;
        }
    }
    ClockConfig c_;
    double debt_ = 0, shedMs_ = 0, forgivenMs_ = 0, totalReal_ = 0, totalAdv_ = 0, wReal_ = 0, wAdv_ = 0, wShed_ = 0;
    unsigned stalls_ = 0, entries_ = 0;
    bool over_ = false;
};

} // namespace npfps
