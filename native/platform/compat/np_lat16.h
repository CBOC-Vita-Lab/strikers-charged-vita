// np_lat16.h (wave 11, press16): the pad read aligned to the controller's own samples.
//
// WHY (console logs r13 gsx.log l.1080-1373, play windows): a press reaches the picture through   pad stamp -> handover 22 ms -> decoded +18 -> presented +10 -> handed +34 -> shown +17
// = 100-105 ms at a held 30. Most of it is by design (50 Hz steps, send/decode/GPU in series, a 2-refresh grid). Two things the [lat] chain hides:
//  1. its pad stamp is the time of the Peek, not of the controller sample behind it; the sample is up to one controller period (16.7 ms, mean 8) old. The late latch (MSC_PAD_LATE /
//     MSC_PHASE_SYNC) puts the Peek off by a few ms: [lat] gets shorter by that much but, when the controller refreshes its sample once a period, the SAME sample is read later: for a
//     real press the delay to the picture is unchanged and the picture has lost slack. A later read only pays when it crosses the arrival of the next controller sample.
//  2. the room it works with is mis-measured: "handed" is stamped when the display gate has finished waiting, so handed -> shown never exceeds one refresh (p50 16.7, r13 l.1080-1373)
//     while the picture really sat finished for about 2-3 refreshes ([present] ready after 5 vblanks, shown at 7: l.1062). np_latency.cpp now also stamps the callback's entry
//     (NpLatEntered, before the gate waits): entry -> shown is a lower bound of the time a finished picture waited.
//
// WHAT (MSC_LAT_SAMPLE=1, default OFF; MSC_LAT_PADAGE=1 only logs): before the step that reads the pad, with the room the picture has (smallest entry -> shown time of the last 24
// pictures, the waits of the last 4 frames added back, minus a margin of 3 ms that grows 1 ms per late picture; and the game thread's own idle time), the thread polls the controller
// (Peek of one sample, 0.3 ms steps) until a newer sample than the one at the start of the frame appears, and reads right after it. Having seen when the sample arrives relative to the
// frame's start it sleeps most of the way next time (the retrace-locked frame start and the controller's sample clock keep their phase for seconds) and re-finds it by polling; when the
// arrival does not fit the room it does not wait at all and looks again every 12th frame (a wait that crosses no arrival gains nothing). 30 pictures of back-off after a picture that came
// up a refresh late. The simulation is not touched: the step's dt and the picture's blend were fixed at the frame's start, the sleep is counted as a wait (like NpFpsPadLateWait's), and
// the pad layer takes its sample at its first request as before. Single player only: the caller refuses it in LAN/online sessions, free runs and the game's own locked scenes; the host
// (no controller stamps) never acts. Pure code except the clock, the stamp source and the sleep, which the caller supplies; tools/fps/lat16_unit.cpp models it with mutants.
#ifndef NP_LAT16_H
#define NP_LAT16_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <algorithm>

#ifndef NP_LAT16_BREAK
#define NP_LAT16_BREAK 0   // proof mutants (unit test only): 1 an unchanged stamp counts as a find, 2 the deadline is ignored, 3 room ignores the margin, 4 no back-off after a late picture, 5 never looks again
#endif

namespace npl16 {

constexpr uint32_t kPeriodNomUs = 16683;

// The controller's sample clock as seen through the stamps of our reads: the offset between its clock and ours (for the age log) and its period.
class StampClock {
public:
    static constexpr unsigned kWin = 96, kDet = 16, kPer = 17;
    // A read made at nowUs (our clock, us) returned a newest sample stamped tsUs (controller clock, us). Its age is not known: now - ts is the offset plus the age.
    void Observe(uint64_t tsUs, uint64_t nowUs) {
        off_[nObs_ % kWin] = (int64_t)nowUs - (int64_t)tsUs;
        ++nObs_;
        if (haveTs_ && tsUs > lastTs_) {
            const uint64_t d = tsUs - lastTs_;
            const uint32_t nom = per_ ? per_ : kPeriodNomUs;
            const uint32_t n = (uint32_t)((d + nom / 2) / nom);   // frames of 33 ms see two periods between the newest stamps
            if (n >= 1 && n <= 4) {
                const int64_t dev = (int64_t)d - (int64_t)n * nom;
                if (dev > -(int64_t)(300 * n) && dev < (int64_t)(300 * n)) { pr_[nPer_ % kPer] = (uint32_t)(d / n); ++nPer_; Recompute(); }
            }
        }
        if (!haveTs_ || tsUs > lastTs_) { lastTs_ = tsUs; haveTs_ = true; }
    }
    // A poll saw a new sample appear: it arrived no earlier than the previous poll and no later than tNowUs (the age is the poll step at most): an accurate offset.
    void Detected(uint64_t tsUs, uint64_t tNowUs) {
        det_[nDet_ % kDet] = (int64_t)tNowUs - (int64_t)tsUs;
        ++nDet_;
    }
    bool Ready() const { return nObs_ >= 24 && nPer_ >= 6 && per_ >= 15000 && per_ <= 18500; }
    bool Accurate() const { return nDet_ > 0; }
    uint32_t PeriodUs() const { return per_; }
    int64_t OffsetUs() const {
        int64_t m = INT64_MAX;
        for (unsigned i = 0, n = nObs_ < kWin ? nObs_ : kWin; i < n; ++i) m = std::min(m, off_[i]);
        for (unsigned i = 0, n = nDet_ < kDet ? nDet_ : kDet; i < n; ++i) m = std::min(m, det_[i]);
        return m == INT64_MAX ? 0 : m;
    }
    // Age of a sample at the moment of the read (the offset is the smallest age seen: without a detection this is the age beyond that, so a LOWER bound).
    int64_t AgeUs(uint64_t tsUs, uint64_t nowUs) const { const int64_t a = (int64_t)nowUs - (int64_t)tsUs - OffsetUs(); return a < 0 ? 0 : a; }
private:
    void Recompute() {
        const unsigned n = nPer_ < kPer ? nPer_ : kPer;
        uint32_t v[kPer] = {};
        for (unsigned i = 0; i < n; ++i) v[i] = pr_[i];
        std::sort(v, v + n);
        per_ = v[n / 2];
    }
    int64_t off_[kWin] = {}, det_[kDet] = {};
    uint32_t pr_[kPer] = {};
    uint32_t nObs_ = 0, nDet_ = 0, nPer_ = 0, per_ = 0;
    uint64_t lastTs_ = 0;
    bool haveTs_ = false;
};

struct Config {
    uint32_t marginUs = 3000, marginMaxUs = 8000, capUs = 18000, arriveUs = 700, window = 24, holdOff = 30, stepUs = 300, minRoomUs = 1500, probeEvery = 12;
};

// How long the pad read may be put off: the smallest room of the last pictures, as in np_feel.h PadLate but absolute (a wait is all or nothing, it does not ramp).
class Room {
public:
    Config cfg;
    uint32_t margin = 3000;
    // slackUs: smallest callback entry -> shown time of the pictures shown since the last call (valid when have); cpuIdleUs: the game thread's waits last frame less our sleep;
    // late: a picture came up more than 1.5 periods after the previous one. lastWaitUs: what we slept before the previous frame's read.
    void Observe(bool have, uint32_t slackUs, uint32_t cpuIdleUs, bool late, uint32_t lastWaitUs) {
        // the slack seen includes the waits of the frames before: add back the smallest of the last 4 (never more than was certainly in it)
        ring_[nRing_ % 4] = lastWaitUs;
        ++nRing_;
        uint32_t minW = ring_[0];
        for (unsigned i = 1, nr = nRing_ < 4 ? nRing_ : 4; i < nr; ++i) minW = std::min(minW, ring_[i]);
        if (late) {
            ++lates;
            if (NP_LAT16_BREAK != 4) hold_ = cfg.holdOff;
            margin = std::min(cfg.marginMaxUs, margin + 1000);
            clean_ = 0;
            seen_ = 0;   // the window is from before the late picture: it fills again after the hold
            return;
        }
        ++clean_;
        if (clean_ >= 60 && margin > cfg.marginUs) { margin -= std::min(250u, margin - cfg.marginUs); clean_ = 0; }
        if (hold_) { --hold_; return; }
        if (!have) return;
        uint64_t s = (uint64_t)slackUs + minW;
        if (cpuIdleUs < s) s = cpuIdleUs;
        win_[seen_ % 24] = (uint32_t)s;
        ++seen_;
    }
    uint32_t AllowedUs() const {
        if (hold_ || seen_ < cfg.window) return 0;
        uint32_t mn = win_[0];
        for (unsigned i = 1; i < 24; ++i) mn = std::min(mn, win_[i]);
        const uint32_t m = NP_LAT16_BREAK == 3 ? 0u : margin;
        return mn > m ? std::min(mn - m, cfg.capUs) : 0u;
    }
    uint32_t lates = 0;
    uint32_t Hold() const { return hold_; }
private:
    uint32_t win_[24] = {}, ring_[4] = {};
    uint32_t seen_ = 0, nRing_ = 0, hold_ = 0, clean_ = 0;
};

struct Plan { uint32_t preUs = 0, untilUs = 0, stepUs = 0; };   // sleep preUs, then poll every stepUs until untilUs after the start (all 0: no wait)

// Where the next controller sample arrives, relative to the start of the wait (the frame's retrace-locked start): learned from the finds, kept while they repeat.
class PhaseTracker {
public:
    Config cfg;
    uint32_t noRoom = 0, noFit = 0, probes = 0;
    Plan Begin(uint32_t allowedUs) {
        Plan p;
        p.stepUs = cfg.stepUs;
        if (allowedUs < cfg.minRoomUs) { ++noRoom; return Plan(); }
        if (valid_) {
            if (relHat_ + cfg.arriveUs > allowedUs) {   // the arrival is later than the room: no wait; look again now and then (the phases drift against each other)
                if (NP_LAT16_BREAK == 5 || ++since_ < cfg.probeEvery) { ++noFit; return Plan(); }
                since_ = 0; ++probes;
                p.untilUs = allowedUs;
                return p;
            }
            p.preUs = relHat_ > 1500 ? relHat_ - 1500 : 0;
            p.untilUs = NP_LAT16_BREAK == 2 ? relHat_ + 2500 : std::min(allowedUs, relHat_ + 2500);
            return p;
        }
        if (skip_) { --skip_; ++noFit; return Plan(); }
        ++probes;
        p.untilUs = allowedUs;
        return p;
    }
    // found: a newer sample appeared; relUs: when (from the start); immediate: at the first poll after the planned sleep (it had arrived before that: the arrival is earlier than relUs)
    void End(bool found, bool immediate, uint32_t relUs) {
        if (found) {
            ++finds;
            misses_ = 0;
            since_ = 0;
            if (valid_ && immediate) relHat_ = relHat_ > 1500 ? relHat_ - 1500 : 0;
            else relHat_ = relUs;
            valid_ = true;
        } else {
            ++missed;
            if (valid_) { if (++misses_ >= 2) { valid_ = false; skip_ = 3; } }
            else skip_ = cfg.probeEvery - 1;
        }
    }
    bool Valid() const { return valid_; }
    uint32_t RelHat() const { return relHat_; }
    uint32_t finds = 0, missed = 0;
private:
    bool valid_ = false;
    uint32_t relHat_ = 0, misses_ = 0, skip_ = 0, since_ = 0;
};

struct Stats {
    uint32_t reads = 0, waits = 0, finds = 0;
    uint64_t waitSumUs = 0;
    uint32_t maxWaitUs = 0;
    uint32_t lbN = 0, lbMinUs = 0xFFFFFFFFu;   // the room fed in: smallest callback entry -> shown time of the pictures seen
    uint64_t lbSumUs = 0;
    uint32_t ageHist[64] = {};   // age of the sample at the game's read, 0.5 ms bins (last bin: 31.5 ms and more)
    uint32_t ageN = 0;
};

class Latch {
public:
    StampClock sc;
    Room room;
    PhaseTracker pt;
    Stats st;
    uint32_t lastWaitUs = 0;
    // The game's own read of the pad (every Read that took a sample).
    void OnRead(uint64_t tsUs, uint64_t nowUs) {
        sc.Observe(tsUs, nowUs);
        ++st.reads;
        if (sc.Ready()) {
            const unsigned b = (unsigned)std::min<int64_t>(63, sc.AgeUs(tsUs, nowUs) / 500);
            ++st.ageHist[b];
            ++st.ageN;
        }
    }
    void NoteRoom(uint32_t slackUs) { ++st.lbN; st.lbSumUs += slackUs; st.lbMinUs = std::min(st.lbMinUs, slackUs); }
    void Observe(bool have, uint32_t slackUs, uint32_t cpuWaitPrevUs, bool late) {
        const uint32_t idle = cpuWaitPrevUs > lastWaitUs ? cpuWaitPrevUs - lastWaitUs : 0u;
        if (have && slackUs != 0xFFFFFFFFu) NoteRoom(slackUs);
        room.Observe(have, slackUs, idle, late, lastWaitUs);
    }
};

// The wait itself, with the clock, the stamp source and the sleep injected (microseconds; the stamp source returns 0 when there is none). Returns the time slept.
template <class NowFn, class StampFn, class SleepFn>
inline uint32_t DoWait(Latch& l, NowFn nowUs, StampFn stampUs, SleepFn sleepUs) {
    const Plan p = l.pt.Begin(l.room.AllowedUs());
    if (!p.untilUs) { l.lastWaitUs = 0; return 0; }
    const uint64_t t0 = nowUs(), ts0 = stampUs();
    if (!ts0) { l.lastWaitUs = 0; return 0; }
    l.sc.Observe(ts0, t0);
    if (p.preUs) sleepUs(p.preUs);
    bool found = false, immediate = false;
    uint64_t tFound = 0;
    const unsigned maxIter = p.untilUs / p.stepUs + 4;   // (a clock that does not advance must not hold the thread)
    for (unsigned i = 0; i < maxIter; ++i) {
        const uint64_t tn = nowUs(), ts = stampUs();
        if (NP_LAT16_BREAK == 1 ? ts >= ts0 : ts > ts0) { found = true; immediate = i == 0 && p.preUs > 0; tFound = tn; l.sc.Detected(ts, tn); break; }
        if (tn - t0 >= p.untilUs) break;
        sleepUs(p.stepUs);
    }
    const uint32_t waited = (uint32_t)(nowUs() - t0);
    l.pt.End(found, immediate, found ? (uint32_t)(tFound - t0) : 0u);
    l.lastWaitUs = waited;
    ++l.st.waits;
    l.st.waitSumUs += waited;
    l.st.maxWaitUs = std::max(l.st.maxWaitUs, waited);
    if (found) ++l.st.finds;
    return waited;
}

inline int EnvInt(const char* name, int dflt) { const char* e = getenv(name); return (e && *e) ? atoi(e) : dflt; }
inline Latch& L() {
    static Latch l;
    static bool cfg = false;
    if (!cfg) {   // (trial knobs for the console: the margin, the poll step, the longest wait)
        cfg = true;
        l.room.cfg.marginUs = (uint32_t)std::max(0, std::min(20000, EnvInt("MSC_LAT_SAMPLE_MARGIN_US", 3000)));
        l.room.margin = l.room.cfg.marginUs;
        l.room.cfg.capUs = (uint32_t)std::max(0, std::min(20000, EnvInt("MSC_LAT_SAMPLE_CAP_US", 18000)));
        l.pt.cfg = l.room.cfg;
        l.pt.cfg.stepUs = (uint32_t)std::max(100, std::min(2000, EnvInt("MSC_LAT_SAMPLE_STEP_US", 300)));
    }
    return l;
}
inline bool Wanted() { static const bool on = EnvInt("MSC_LAT_SAMPLE", 0) > 0; return on; }
// MSC_LAT_SLACK_LB=1 (default off): the existing late latches (MSC_PAD_LATE, MSC_PHASE_SYNC) take the room from the display callback's entry, not from the gate's exit (see WHY 2)
inline bool LbWanted() { static const bool on = EnvInt("MSC_LAT_SLACK_LB", 0) > 0; return on; }
inline bool AgeLog() { static const bool on = EnvInt("MSC_LAT_PADAGE", 0) > 0; return on; }
// the host test hook: MSC_LAT_SAMPLE_SIM=1 makes the controller's stamps from our own clock (phase MSC_LAT_SAMPLE_SIM_PHASE_US) and the slack a constant (MSC_LAT_SAMPLE_SIM_SLACK_US)
inline bool Sim() { static const bool on = EnvInt("MSC_LAT_SAMPLE_SIM", 0) > 0; return on; }
inline uint64_t SimStampUs(uint64_t nowUs) {
    static const uint64_t ph = (uint64_t)EnvInt("MSC_LAT_SAMPLE_SIM_PHASE_US", 5000);
    const uint64_t n = nowUs >= ph ? (nowUs - ph) / kPeriodNomUs : 0;
    return 1000000000ull + n * kPeriodNomUs + ph;   // (a stamp clock of its own, unrelated to ours)
}

// The percentiles of the age histogram, microseconds (0.5 ms bins).
inline uint32_t AgePct(const Stats& s, unsigned pct) {
    if (!s.ageN) return 0;
    const uint32_t need = (s.ageN * pct + 99) / 100;
    uint32_t acc = 0;
    for (unsigned i = 0; i < 64; ++i) { acc += s.ageHist[i]; if (acc >= need) return i * 500 + 250; }
    return 63 * 500;
}

// One clock for the stamps and the waits: the monotonic clock in microseconds (on the Vita the same clock as np_fps.cpp's NowNs; the host's virtual clock is used by the caller).
inline uint64_t RealUs() { timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000u; }

inline void LogLine(Latch& l) {
    const Stats& s = l.st;
    fprintf(stderr, "[lat16] %u pad reads%s: controller sample age at the read p50 %.1f p95 %.1f ms (estimate: now less the stamp less the smallest such difference seen%s; %u reads measured), "
            "sample period %u us; waits %u (mean %.1f ms, longest %.1f ms), %u found a newer sample (%.0f %%), arrival %s%.1f ms after the frame start; looked at and skipped: no room %u, arrival later than the room %u; "
            "late pictures %u, room now %.1f ms (margin %.1f ms); room fed in (picture finished to screen, lower bound): min %.1f mean %.1f ms over %u pictures\n",
            s.reads, Wanted() ? " (MSC_LAT_SAMPLE=1)" : "", AgePct(s, 50) / 1000.0, AgePct(s, 95) / 1000.0, l.sc.Accurate() ? ", a find seen" : ", no find seen: a lower bound", s.ageN, l.sc.PeriodUs(), s.waits,
            s.waits ? s.waitSumUs / 1000.0 / s.waits : 0.0, s.maxWaitUs / 1000.0, s.finds, s.waits ? 100.0 * s.finds / s.waits : 0.0, l.pt.Valid() ? "" : "not learned, last ", l.pt.RelHat() / 1000.0, l.pt.noRoom, l.pt.noFit,
            l.room.lates, l.room.AllowedUs() / 1000.0, l.room.margin / 1000.0, s.lbN ? s.lbMinUs / 1000.0 : 0.0, s.lbN ? s.lbSumUs / 1000.0 / s.lbN : 0.0, s.lbN);
}
// The game's own read of the pad (the Vita input layer, host test hook): the stamp of the newest controller sample and our clock.
inline void GameRead(uint64_t tsUs, uint64_t nowUs) {
    Latch& l = L();
    l.OnRead(tsUs, nowUs);
    if (l.st.reads % 300 == 0) { LogLine(l); l.st = Stats(); }
}

} // namespace npl16
#endif
