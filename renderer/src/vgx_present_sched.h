// Display scheduler of the frame presenter (workstream FI2, 9 Oct): pure logic, no renderer, no clock of its own,
// so the game's presenter (vgx_present.cpp) and the offline pacing simulator (tools/fi_pacing/sim.cpp) run the
// SAME code.
//
// The game builds frames at a rate of its own (somewhere between 8 and 20 per second on the console), each with a
// game time `g` (milliseconds; the time the game thread began building it: its content is the world at `g`). The
// display wants a picture every `tick` (30 or 60 per second). The scheduler keeps a display clock `c`, a content time:
// the picture of tick k shows the world at c_k, which is between the game times of two built frames A (g_A <= c) and
// B (c <= g_B), i.e. B's in-between picture at t = (c - g_A) / (g_B - g_A). The clock advances one tick per tick, and
// never beyond the newest frame that has arrived: when the game thread is late the last picture is shown again
// (Repeat) and the display falls further behind the game by exactly the time it waited, which is the jitter buffer
// that makes the next late frame less likely to be a hitch. A small controller speeds the clock up (at most +gain)
// when the buffer is larger than it needs to be and slows it down when it is smaller, so the display's delay settles
// near what the game's jitter requires; the delay is one built frame plus the game thread's build-to-ready latency
// plus this margin.
//
// Frames that cannot be shown through in-between pictures (first frame, camera cut, other scene: Backend::Prepare
// says false) are held on the previous picture until the clock reaches their time, then shown as themselves. A frame
// the clock jumps over without a tick landing in its interval is executed hidden (the back end needs every frame's
// commands in order). When frames arrive at least as fast as the ticks (built rate near or above the display's) the
// scheduler switches to bypass: every frame is shown as itself at the next tick, with no added delay.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>

namespace vgx::present {

struct SchedCfg {
    double tickMs = 1000.0 / 30.0;
    double bypassMs = 0.0;     // smoothed built interval at or below which frames are shown as themselves (0: 1.15 x tick)
    double marginMs = 20.0;    // least buffer margin the clock controller aims for (the larger of this and 2 sigma)
    double gain = 0.10;        // largest change of the clock's speed (fraction)
    bool controller = true;
    double minT = 0.02, maxT = 0.98; // t at or below / above which a picture is the previous / this frame itself
    double maxGapMs = 400.0;   // game times further apart than this are a discontinuity (loading, pause): no hold, no blend
    double extrapMs = 0.0;     // how far past the newest frame's game time the clock may run (t above 1: extrapolated matrices)
};

class Backend {
public:
    virtual ~Backend() {}
    // The frame is about to be shown for the first time: pair it with the frame finished before it (interp::Begin).
    // true: in-between pictures are possible.
    virtual bool Prepare(uint64_t id, bool wantPairs) = 0;
    virtual void Blend(uint64_t id, float t, bool first) = 0;
    virtual void Self(uint64_t id, bool first, bool hidden) = 0;
    virtual void Finish(uint64_t id) = 0; // no more pictures of this frame (interp::End)
    virtual void Repeat() = 0;            // present the last picture again
};

enum class Kind : uint8_t { None, Repeat, Blend, Self, Hold, Stall };

struct TickInfo {
    Kind kind = Kind::None;
    uint64_t frame = 0;
    double t = 0, c = 0, rate = 1, margin = 0;
    int queued = 0;
};

struct SchedStats {
    uint64_t ticks = 0, blends = 0, selfs = 0, repeats = 0, holds = 0, stalls = 0, hidden = 0, bypassTicks = 0, empty = 0, frames = 0, cuts = 0, gaps = 0;
    double sumDelay = 0, maxDelay = 0, minDelay = 1e30; // tick time minus display content time, over ticks with a clock
    uint64_t delayN = 0;
};

class Scheduler {
public:
    Scheduler(const SchedCfg& cfg, Backend& be) : cfg_(cfg), be_(be) {
        if (cfg_.bypassMs <= 0) cfg_.bypassMs = 1.15 * cfg_.tickMs;
    }

    // A built frame has arrived (decoded, on the render thread) at time `now`; `g` is its game time.
    void Arrive(uint64_t id, double g, double now) {
        Fr f;
        f.id = id;
        f.arrive = now;
        if (haveLastG_) {
            if (g < lastG_) g = lastG_;
            const double dt = g - lastG_;
            if (dt > cfg_.maxGapMs) { f.gap = true; ++stats_.gaps; }
            else if (dt > 0) ewmaInterval_ = ewmaInterval_ > 0 ? 0.75 * ewmaInterval_ + 0.25 * dt : dt;
            // buffer margin at this arrival: how much content time was still ahead of the display clock
            if (haveC_ && !f.gap) {
                const double cEst = c_ + (now - lastTick_) * rate_;
                const double m = lastG_ + cfg_.extrapMs - cEst - consecStall_ * cfg_.tickMs; // (a stall of n ticks counts n ticks)
                if (!haveMargin_) { margin_ = m; var_ = 0; haveMargin_ = true; }
                else { const double d = m - margin_; margin_ += 0.3 * d; var_ = 0.8 * var_ + 0.2 * d * d; }
                if (cfg_.controller) {
                    const double target = std::max(cfg_.marginMs, 2.0 * std::sqrt(var_));
                    rate_ = 1.0 + cfg_.gain * std::max(-1.0, std::min(1.0, (margin_ - target) / target));
                }
            }
        }
        f.g = g;
        lastG_ = g;
        haveLastG_ = true;
        const bool wasBypass = bypass_;
        if (ewmaInterval_ > 0) {
            if (!bypass_ && ewmaInterval_ < cfg_.bypassMs) bypass_ = true;
            else if (bypass_ && ewmaInterval_ > cfg_.bypassMs * 1.15) bypass_ = false;
        }
        (void)wasBypass;
        ++stats_.frames;
        q_.push_back(f);
    }

    // One display tick at time `now` (the caller calls this every tickMs).
    TickInfo Tick(double now) {
        TickInfo ti;
        ++stats_.ticks;
        const double dtReal = lastTick_ > 0 ? now - lastTick_ : cfg_.tickMs;
        (void)dtReal;
        lastTick_ = now;
        ti.rate = rate_;
        ti.margin = margin_;
        ti.queued = static_cast<int>(q_.size());
        if (q_.empty()) {
            ++stats_.empty;
            ++consecStall_;
            ++stats_.repeats;
            be_.Repeat();
            ti.kind = Kind::Repeat;
            ti.frame = lastShown_;
            ti.c = c_;
            Account(now);
            return ti;
        }
        if (bypass_) {
            // every frame as itself, no delay: all but the newest hidden
            while (!q_.empty()) {
                Fr& f = q_.front();
                const bool newest = q_.size() == 1;
                be_.Prepare(f.id, false);
                be_.Self(f.id, true, !newest);
                be_.Finish(f.id);
                if (newest) { ++stats_.selfs; ti.kind = Kind::Self; ti.frame = f.id; haveC_ = true; c_ = f.g; lastShown_ = f.id; havePrev_ = true; prevG_ = f.g; }
                else ++stats_.hidden;
                q_.pop_front();
            }
            ++stats_.bypassTicks;
            ti.c = c_;
            selfShown_ = 0;
            Account(now, true);
            return ti;
        }
        Fr& newest = q_.back();
        double cn = haveC_ ? c_ + cfg_.tickMs * rate_ : q_.front().g;
        bool clamped = false;
        const double cap = newest.g + (virtual_ ? 0.0 : cfg_.extrapMs);
        if (cn > cap - (virtual_ ? 0.05 : 0.02) * cfg_.tickMs) { cn = cap; clamped = true; } // (a tick that falls just short of the frame's time lands on it)
        // frames the clock has passed entirely
        while (q_.size() > 1 && q_.front().g < cn - 1e-6) {
            Fr& f = q_.front();
            if (!f.prepared) { f.interp = be_.Prepare(f.id, true); f.prepared = true; }
            if (!f.executed) { be_.Self(f.id, true, true); f.executed = true; ++stats_.hidden; }
            be_.Finish(f.id);
            prevG_ = f.g;
            havePrev_ = true;
            q_.pop_front();
        }
        Fr& B = q_.front();
        ti.frame = B.id;
        if (!B.prepared) { B.interp = be_.Prepare(B.id, true); B.prepared = true; }
        if (!havePrev_ || B.gap) {
            // first frame, or a discontinuity: as itself, at once; the clock jumps
            c_ = B.g;
            haveC_ = true;
            ti.c = c_;
            if (selfShown_ == B.id) { // already shown, nothing newer yet
                ++stats_.stalls;
                ++stats_.repeats;
                ++consecStall_;
                be_.Repeat();
                ti.kind = Kind::Stall;
            } else {
                ++stats_.selfs;
                be_.Self(B.id, !B.executed, false);
                B.executed = true;
                lastShown_ = B.id;
                selfShown_ = B.id;
                consecStall_ = 0;
                if (B.gap) ++stats_.cuts;
                ti.kind = Kind::Self;
            }
            Account(now);
            return ti;
        }
        const double gA = prevG_;
        const double span = B.g - gA;
        c_ = cn;
        haveC_ = true;
        ti.c = c_;
        if (!B.interp || span < 1e-3) {
            if (cn < B.g - 1e-6) { // hold the previous picture until B's time
                ++stats_.holds;
                ++stats_.repeats;
                be_.Repeat();
                ti.kind = Kind::Hold;
                Account(now);
                return ti;
            }
            if (selfShown_ == B.id) { // nothing new: the game is late
                ++stats_.stalls;
                ++consecStall_;
                ++stats_.repeats;
                be_.Repeat();
                ti.kind = Kind::Stall;
                Account(now);
                return ti;
            }
            be_.Self(B.id, !B.executed, false);
            B.executed = true;
            selfShown_ = B.id;
            lastShown_ = B.id;
            consecStall_ = 0;
            ++stats_.selfs;
            ti.kind = Kind::Self;
            Account(now);
            return ti;
        }
        const double t = (cn - gA) / span;
        ti.t = t;
        if (t <= cfg_.minT) {
            ++stats_.repeats;
            be_.Repeat();
            ti.kind = Kind::Repeat;
        } else if (t > 1.0 + (1.0 - cfg_.maxT)) { // beyond this frame: extrapolated
            if (clamped && lastBlendId_ == B.id && std::fabs(t - lastBlendT_) < 1e-6) {
                ++stats_.stalls;
                ++consecStall_;
                ++stats_.repeats;
                be_.Repeat();
                ti.kind = Kind::Stall;
            } else {
                be_.Blend(B.id, static_cast<float>(t), !B.executed);
                B.executed = true;
                lastShown_ = B.id;
                selfShown_ = 0;
                lastBlendId_ = B.id;
                lastBlendT_ = t;
                consecStall_ = 0;
                ++stats_.blends;
                ti.kind = Kind::Blend;
            }
        } else if (t >= cfg_.maxT) {
            if (selfShown_ == B.id) {
                ++stats_.stalls;
                ++consecStall_;
                ++stats_.repeats;
                be_.Repeat();
                ti.kind = Kind::Stall;
            } else {
                be_.Self(B.id, !B.executed, false);
                B.executed = true;
                selfShown_ = B.id;
                lastShown_ = B.id;
                consecStall_ = 0;
                ++stats_.selfs;
                ti.kind = Kind::Self;
                ti.t = 1.0;
            }
        } else {
            be_.Blend(B.id, static_cast<float>(t), !B.executed);
            B.executed = true;
            lastShown_ = B.id;
            selfShown_ = 0;
            consecStall_ = 0;
            ++stats_.blends;
            ti.kind = Kind::Blend;
        }
        (void)clamped;
        Account(now);
        return ti;
    }

    // Offline (no real time, no jitter buffer): ticks while the display clock can advance a whole tick without passing the
    // newest frame (and once for the very first frame). Rate 1. Returns the number of ticks made.
    template <class Fn>
    int RunVirtual(double now, Fn&& onTick) {
        int n = 0;
        rate_ = 1.0;
        virtual_ = true;
        while (!q_.empty()) {
            if (bypass_) {
                if (n > 0) break;
            } else if (haveC_ && c_ + cfg_.tickMs > q_.back().g + 0.05 * cfg_.tickMs) { // (5 %: the game times of a harness are not exact multiples)
                if (!(n == 0 && FirstUnshown())) break;
            }
            const TickInfo ti = Tick(now);
            onTick(ti);
            ++n;
            if (n > 1000) break;
        }
        return n;
    }

    const SchedStats& stats() const { return stats_; }
    double clock() const { return c_; }
    bool haveClock() const { return haveC_; }
    double rate() const { return rate_; }
    double margin() const { return margin_; }
    bool bypass() const { return bypass_; }
    double interval() const { return ewmaInterval_; }
    size_t queued() const { return q_.size(); }
    // Time of the newest frame the display has not caught up with yet (for the adapter: how much is waiting).
    double newestG() const { return q_.empty() ? c_ : q_.back().g; }
    // Smoothed display delay (tick time minus the content time shown), ms: what an audio output has to be delayed by to stay in step with the picture.
    double delayMs() const { return delayEwma_; }

private:
    struct Fr {
        uint64_t id = 0;
        double g = 0, arrive = 0;
        bool prepared = false, interp = false, executed = false, gap = false;
    };
    // a frame that has never been shown and that the clock does not reach by advancing a tick (it would never be
    // executed otherwise): its time lies at or before the clock
    bool FirstUnshown() const { return !q_.empty() && !q_.back().executed && q_.back().g <= c_ + 1e-6; }
    void Account(double now, bool bypass = false) {
        if (haveC_ && !bypass) {
            const double d = now - c_;
            delayEwma_ = delayEwma_ > 0 ? 0.98 * delayEwma_ + 0.02 * d : d;
            stats_.sumDelay += d;
            ++stats_.delayN;
            if (d > stats_.maxDelay) stats_.maxDelay = d;
            if (d < stats_.minDelay) stats_.minDelay = d;
        }
    }

    SchedCfg cfg_;
    Backend& be_;
    std::deque<Fr> q_;
    SchedStats stats_;
    bool haveC_ = false, havePrev_ = false, haveLastG_ = false, haveMargin_ = false, bypass_ = false;
    double c_ = 0, prevG_ = 0, lastG_ = 0, lastTick_ = 0, rate_ = 1.0, margin_ = 0, var_ = 0, ewmaInterval_ = 0, delayEwma_ = 0;
    uint64_t selfShown_ = 0, lastShown_ = 0;
    uint32_t consecStall_ = 0;
    uint64_t lastBlendId_ = 0;
    double lastBlendT_ = 0;
public:
    void SetVirtual(bool v) { virtual_ = v; }
private:
    bool virtual_ = false;
};

// ---- MSC_PRESENT_SYNC: pictures shown on the vblank that matches their game time (ported from the Galaxy port's frame_pace.h PresentSync; that port's measured result:
// judder 0.3-0.8 -> 0.0-0.1 vblanks a picture at about +0.8 vblank of latency, on Galaxy, not here) ----------------------------------------------------------------------
// Between 30 and 60 pictures a second a picture goes to the display at the first vblank after the GPU finished it, and how long that takes from the start of its frame varies from
// picture to picture, so the time on the screen between two pictures is not the game time between them: a picture 2 vblanks of game time after the last can be held for 1, the next,
// 1 vblank later, for 2. The display gate (vgx_host_vita.cpp) cannot help: it only keeps a minimum between pictures, and MSC_PACE_VBLANK works the other way round (animation follows
// what the display held). Each frame's vblank at its start is the stamp (np_fps.cpp); the display callback holds the picture until vblank stamp + L, L the pipeline's latency in
// vblanks. A picture in time is then shown exactly L vblanks after its frame began. L adapts: up by one when pictures keep missing it (raise of the last 32), down by one after 180
// pictures that (almost) all would also have met L - 1, with a hold-off after a raise; a picture over the largest L is shown when ready and does not raise it.
// Target + Shown once per picture, on the display-queue thread. Vblank numbers are sceDisplayGetVcount's; a stamp is the low 16 bits.
class PresentSync {
public:
    static constexpr unsigned kSane = 60;   // a stamp more than a second old is not this picture's (a reset, a settings hold)
    struct Stats {
        uint64_t pictures = 0, stamped = 0, waits = 0, misses = 0, overload = 0, replaced = 0, raises = 0, lowers = 0;
        uint64_t hold[5] = {}, step[5] = {}, err[4] = {};  // vblanks held, turns advanced (index 4: 4 or more); |hold - step| 0..3+
        uint64_t holdLive[5] = {};                         // 1.3: the same holds, counted only while the scene is live match play (SetLive)
        uint64_t natural[8] = {};                          // ready-to-show minus stamp (vblanks), 0..7+
        uint64_t errSum = 0, pairs = 0;                    // sum of |hold - step| over consecutive stamped pictures
        int64_t latSum = 0, latSq = 0;                     // shown - stamp: its mean and spread (the timing judder)
    };
    // align false: measure only (every picture is shown when ready, as without the switch). minLat..maxLat: L's range.
    // raise: late pictures among the last 32 that raise the latency by one (the tolerance: more is smoother, fewer is quicker).
    // fastStart (MSC_PRESENT_SYNC_START, wave 7): until the first raise or the first window that did not lower it, L is judged over 32 pictures instead of 180 (it tightens from its
    // start value in about a second a step, not 3 s a step: the start of play ran 4 vblanks behind and took 6-9 s to settle). A raise ends it for good (the pipeline needs what it has).
    void Configure(bool align, unsigned minLat, unsigned maxLat, unsigned initial, unsigned raise = 4, bool fastStart = false) {
        fast_ = fastStart;
        align_ = align;
        raise_ = raise < 1 ? 1 : (raise > 32 ? 32 : raise);
        min_ = minLat < 1 ? 1 : (minLat > 8 ? 8 : minLat);
        max_ = maxLat < min_ ? min_ : (maxLat > 8 ? 8 : maxLat);
        lat_ = initial < min_ ? min_ : (initial > max_ ? max_ : initial);
        Reset();
    }
    void Reset() {
        havePrev_ = false;
        haveShown_ = false;
        missBits_ = 0;
        run_ = runOver_ = 0;
        holdoff_ = 0;
    }
    // (the policy moved between aligned and measured: L is kept, the picture pairs are not)
    void SetAlign(bool align) {
        if (align != align_) { align_ = align; Reset(); }
    }
    unsigned Latency() const { return lat_; }
    bool Aligned() const { return align_; }
    // earliest: the first vblank the picture can be on (counter + 1). valid / stamp: the picture's turn, if it has one.
    // Returns the vblank to show it on (earliest or later; never more than max_ + 1 later).
    uint32_t Target(uint32_t earliest, bool valid, uint16_t stamp) {
        ++stats_.pictures;
        cur_ = false;
        uint32_t target = earliest;
        if (valid) {
            const uint32_t natural = static_cast<uint16_t>(static_cast<uint16_t>(earliest) - stamp);
            if (natural <= kSane) {
                cur_ = true;
                curStamp_ = earliest - natural;
                curNatural_ = natural;
                ++stats_.stamped;
                ++stats_.natural[natural < 7 ? natural : 7];
                if (align_) {
                    uint32_t t = curStamp_ + lat_;
                    if (haveShown_ && static_cast<int32_t>(t - (lastShown_ + 1)) < 0) t = lastShown_ + 1;  // one picture per vblank
                    if (static_cast<int32_t>(t - earliest) > static_cast<int32_t>(max_ + 1)) t = earliest + max_ + 1;
                    if (static_cast<int32_t>(t - earliest) > 0) target = t;
                }
            }
        }
        if (!cur_) havePrev_ = false;  // (the next stamped picture starts a new pair)
        return target;
    }
    // The vblank the picture is on (after any wait); waited: a wait was made.
    void Shown(uint32_t shown, bool waited) {
        if (waited) ++stats_.waits;
        if (haveShown_ && shown == lastShown_) ++stats_.replaced;  // (two pictures in one vblank: the first was never seen)
        lastShown_ = shown;
        haveShown_ = true;
        if (!cur_) return;
        const int32_t lat = static_cast<int32_t>(shown - curStamp_);
        stats_.latSum += lat;
        stats_.latSq += static_cast<int64_t>(lat) * lat;
        if (havePrev_) {
            const int32_t hold = static_cast<int32_t>(shown - prevShown_), step = static_cast<int32_t>(curStamp_ - prevStamp_);
            if (hold >= 0 && step > 0 && hold < 64 && step < 64) {
                ++stats_.hold[hold < 4 ? hold : 4];
                if (live_) ++stats_.holdLive[hold < 4 ? hold : 4];
                ++stats_.step[step < 4 ? step : 4];
                const uint32_t e = static_cast<uint32_t>(hold > step ? hold - step : step - hold);
                ++stats_.err[e < 3 ? e : 3];
                stats_.errSum += e;
                ++stats_.pairs;
            }
        }
        prevShown_ = shown;
        prevStamp_ = curStamp_;
        havePrev_ = true;
        Adapt();
    }
    const Stats& GetStats() const { return stats_; }
    void ClearStats() { stats_ = Stats(); }
    void SetLive(bool live) { live_ = live; }   // 1.3: set before Shown by the present: the scene is live match play (the hold mix of the log line)

private:
    void Adapt() {
        const bool overload = curNatural_ > max_;           // no L in range would have it: show it, learn nothing
        const bool miss = curNatural_ > lat_;
        if (miss) ++stats_.misses;
        if (overload) {
            ++stats_.overload;
            return;
        }
        missBits_ = (missBits_ << 1) | (miss ? 1u : 0u);
        if (holdoff_) --holdoff_;
        if (!align_) return;
        unsigned recent = 0;
        for (uint32_t b = missBits_; b; b &= b - 1) ++recent;
        if (recent >= raise_ && lat_ < max_) {
            ++lat_;
            ++stats_.raises;
            missBits_ = 0;
            run_ = runOver_ = 0;
            holdoff_ = 300;  // (about 6 s at 45 pictures a second before it may come down again)
            fast_ = false;
            return;
        }
        ++run_;
        if (curNatural_ + 1 > lat_) ++runOver_;  // would have missed L - 1
        if (run_ >= (fast_ ? 32u : 180u)) {
            if (runOver_ <= 1 && lat_ > min_ && holdoff_ == 0) {
                --lat_;
                ++stats_.lowers;
                missBits_ = 0;
            } else if (fast_) {
                fast_ = false;   // a window that did not lower it: settled
            }
            run_ = runOver_ = 0;
        }
    }
    Stats stats_;
    bool live_ = false;
    bool align_ = false, cur_ = false, havePrev_ = false, haveShown_ = false, fast_ = false;
    unsigned min_ = 1, max_ = 5, lat_ = 3, run_ = 0, runOver_ = 0, holdoff_ = 0, raise_ = 4;
    uint32_t missBits_ = 0;
    uint32_t curStamp_ = 0, curNatural_ = 0, prevStamp_ = 0, prevShown_ = 0, lastShown_ = 0;
};

} // namespace vgx::present
