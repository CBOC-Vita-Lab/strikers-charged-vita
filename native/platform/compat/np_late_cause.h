// np_late_cause.h (lessons17, MSC_LATE_CAUSE=1, default ON, logging only): which lane made a late picture late. Pure code (no clock, no I/O; host test tools/fps/late_cause_unit.cpp).
// Galaxy's `floor:` line charges each late picture to the GPU, the game CPU, the render-thread CPU, the front-end CPU or waits, and in its normal stages almost none were the GPU (Honeyhive: front end 37, GPU 1):
// the cuts went where the late pictures came from. Strikers had the pieces ([lane] means, [hitch] causes of a gap, [pace] holds) but no per-picture verdict. Estimate, not a measurement:
//   a picture is LATE when it was held more refreshes than the cadence in force asks (held 30: 2, or the held cadence's 3; pair / 60: 1), live play only.
//   lanes, ms of ONE picture: game = the game thread's frame length less its waits (this picture), front end / render thread (back end) / GPU = the largest of the last 3 pictures the renderer reported
//   (the pipeline is two to three pictures deep, so the picture that made this one late left the game thread about then).
//   verdict = the lane with the largest time that is at least `lanePct` (80 %) of the cadence's period; if no lane is that long, WAITS when the game thread's waits (less the pad late sleep we put in
//   ourselves) were `waitPct` (40 %) or more of the picture's gap (the disc, the display queue, the back-pressure gate), else NONE (unattributed: the sum of small things, or a lane we do not time).
// Cost: two thread-clock reads per picture on the renderer's threads (vgx_workload.h LateScope) and one on the game thread; the line prints what they cost.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>

#ifndef NP_LATECAUSE_BREAK
#define NP_LATECAUSE_BREAK 0   // host test mutants: 1 a lane under the period share still gets the picture (the largest lane always wins), 2 waits are judged before the lanes, 3 the first lane over the share wins, not the largest, 4 the renderer lanes use the newest picture only, not the largest of the last 3
#endif

namespace nplc {

enum Cause : unsigned { kGpu, kGame, kRender, kFront, kWait, kNone, kCauseCount };
inline const char* CauseName(unsigned c) { static const char* const n[kCauseCount] = {"GPU", "game CPU", "render-thread CPU", "front-end CPU", "waits", "unattributed"}; return n[c < kCauseCount ? c : kCauseCount - 1]; }
enum Lane : int { kLaneFront = 0, kLaneRender = 1, kLaneGpu = 2 };

struct Config { float lanePct = 0.80f, waitPct = 0.40f; };

struct Lanes { float game = 0, fe = 0, be = 0, gpu = 0; };   // ms of one picture

inline Cause Judge(const Config& c, float periodMs, const Lanes& l, float waitMs, float gapMs) {
    const float need = periodMs * c.lanePct;
#if NP_LATECAUSE_BREAK == 2
    if (waitMs >= c.waitPct * gapMs && gapMs > 0) return kWait;
#endif
    const float v[4] = {l.gpu, l.game, l.be, l.fe};
    const Cause id[4] = {kGpu, kGame, kRender, kFront};
    int best = -1;
    for (int i = 0; i < 4; ++i) {
#if NP_LATECAUSE_BREAK == 1
        const bool ok = true;
#else
        const bool ok = v[i] >= need;
#endif
#if NP_LATECAUSE_BREAK == 3
        if (ok && best < 0) best = i;
#else
        if (ok && (best < 0 || v[i] > v[best])) best = i;
#endif
    }
    if (best >= 0) return id[best];
    if (gapMs > 0 && waitMs >= c.waitPct * gapMs) return kWait;
    return kNone;
}

// The renderer's per-picture lane times: each lane is written by ONE thread; the game thread reads the largest of the last three. (atomic floats: a torn read is not possible on one word)
class LaneRing {
public:
    void Note(float ms) { const uint32_t i = n_.load(std::memory_order_relaxed); v_[i % 4].store(ms, std::memory_order_relaxed); n_.store(i + 1, std::memory_order_release); }
    float Recent() const {
        const uint32_t n = n_.load(std::memory_order_acquire);
        if (n == 0) return 0.0f;
#if NP_LATECAUSE_BREAK == 4
        return v_[(n - 1) % 4].load(std::memory_order_relaxed);
#else
        float m = 0.0f;
        for (uint32_t k = 0; k < 3 && k < n; ++k) { const float x = v_[(n - 1 - k) % 4].load(std::memory_order_relaxed); if (x > m) m = x; }
        return m;
#endif
    }
    bool Seen() const { return n_.load(std::memory_order_relaxed) != 0; }
    // hold60: the newest value alone (np_fps.cpp keeps a mean of it: a pipelined lane limits the rate by its mean, not by its largest of three)
    float Last() const { const uint32_t n = n_.load(std::memory_order_acquire); return n ? v_[(n - 1) % 4].load(std::memory_order_relaxed) : 0.0f; }
    uint32_t Count() const { return n_.load(std::memory_order_acquire); }
private:
    std::atomic<float> v_[4] = {};
    std::atomic<uint32_t> n_{0};
};

struct Window {
    uint32_t pics = 0, late = 0, by[kCauseCount] = {};
    double sumGame = 0, sumFe = 0, sumBe = 0, sumGpu = 0;
    uint32_t nFe = 0, nBe = 0, nGpu = 0;
};

class Ledger {
public:
    Config cfg;
    LaneRing lane[3];
    Window w;
    uint32_t windowPics = 300;
    // One live-play picture. refreshes: how many refreshes it was held; expected: the cadence's refreshes a picture; gameMs / waitMs / gapMs: this picture. Returns true when the window is full.
    bool Picture(int refreshes, int expected, double gameMs, double waitMs, double gapMs) {
        Lanes l;
        l.game = (float)gameMs;
        l.fe = lane[kLaneFront].Recent(); l.be = lane[kLaneRender].Recent(); l.gpu = lane[kLaneGpu].Recent();
        ++w.pics;
        w.sumGame += gameMs;
        if (lane[kLaneFront].Seen()) { w.sumFe += l.fe; ++w.nFe; }
        if (lane[kLaneRender].Seen()) { w.sumBe += l.be; ++w.nBe; }
        if (lane[kLaneGpu].Seen()) { w.sumGpu += l.gpu; ++w.nGpu; }
        if (refreshes > expected && expected > 0) {
            ++w.late;
            ++w.by[Judge(cfg, (float)expected * 16.683f, l, (float)waitMs, (float)gapMs)];
        }
        return w.pics >= windowPics;
    }
    // The window's line; clears it. costUsPerPic / reads: what the renderer's clock reads cost (negative: not measured).
    size_t Line(char* out, size_t cap, double costUsPerPic, unsigned long reads) {
        const double n = w.pics ? (double)w.pics : 1.0;
        int k = std::snprintf(out, cap, "[late] %u live pictures, %u late (held more refreshes than the cadence): ", w.pics, w.late);
        size_t at = k > 0 ? (size_t)k : 0;
        for (unsigned c = 0; c < kCauseCount && at < cap; ++c) {
            k = std::snprintf(out + at, cap - at, "%s %u%s", CauseName(c), w.by[c], c + 1 < kCauseCount ? ", " : "");
            if (k > 0) at += (size_t)k;
        }
        if (at < cap) {
            k = std::snprintf(out + at, cap - at, " | ms a picture: game %.1f front end %.1f render thread %.1f GPU %.1f", w.sumGame / n, w.nFe ? w.sumFe / w.nFe : -1.0, w.nBe ? w.sumBe / w.nBe : -1.0, w.nGpu ? w.sumGpu / w.nGpu : -1.0);
            if (k > 0) at += (size_t)k;
        }
        if (at < cap) {
            k = costUsPerPic >= 0 ? std::snprintf(out + at, cap - at, " | measuring cost %.1f us a picture on the renderer threads (%lu clock reads)", costUsPerPic, reads) : std::snprintf(out + at, cap - at, " | measuring cost not timed");
            if (k > 0) at += (size_t)k;
        }
        w = Window();
        return at < cap ? at : cap - 1;
    }
};

} // namespace nplc
