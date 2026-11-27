// np_head17.h (wave 17, MSC_HEAD_STAT=1, default 0, logging only): the 60 mode's headroom line, "[head]" every 30 s per scene class.
// What it answers for "does Smooth hold on this stadium": per 30 s window and scene class (play, replay, cut-scene, other)
//   - the game thread's busy ms per picture (frame length less its waits: the figure np_fps.cpp already has) p50 / p95 / max, kept apart for pictures that ran a simulation step and pictures
//     that did not (at 60 one picture in six has no step; the worst picture is a step picture)
//   - the render thread's (lane 1) and the GPU's (lane 2) ms per picture p95 / max, from the lanes the renderer already reports (NpLateLaneNote, MSC_LATE_CAUSE=1 feeds them); a sample belongs
//     to the class that was current when the renderer reported it (the pipeline is 2-3 pictures deep: a sample at a scene change can fall in the neighbour class)
//   - late pictures (held more refreshes than the cadence in force), pictures under the controller's relief to 30, switches
//   - how many pictures had game / render thread / GPU over 16.7 ms, and the verdict (the lane with most)
// Cost: no clock read of its own; per picture some arithmetic and one store, per renderer lane note one store; one sort of the window's samples every 30 s on the game thread.
// Pure code (no I/O, no clock): host test tools/fps/head17_unit.cpp (NP_HEAD17_BREAK 1-4 must fail).
// scenes30: each held moment is measured on its own: walk-out (the presentation's GameBegin), celebration (a goal's presentation, Goal*), mega (the Mega Strike's own scene, VgxMegaActive),
// mega-cut (its launch and result cut-scenes, Megastrike*), replay, and cut-scene for any other cut-scene (half-time, full time); MomentOf below. A held class (everything but play and other) is
// shown at 30, so its "over" counts are against one 30 period (33.4 ms), not 16.7; play and other keep 16.7. Classes with no picture print no line.
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#ifndef NP_HEAD17_BREAK
#define NP_HEAD17_BREAK 0   // host test mutants: 1 the percentile rank rounds down, 2 step and no-step pictures swapped, 3 "over" counts from 16.0 ms, 4 a lane sample goes to the wrong class, 5 (pace18) the join wait is filled with the sim thread's busy ms
#endif

namespace nphead {

enum Class : int { kPlay = 0, kReplay = 1, kCut = 2, kOther = 3, kWalkout = 4, kCeleb = 5, kMega = 6, kMegaCut = 7, kClasses = 8 };
inline const char* ClassName(int c) { static const char* const n[kClasses] = {"play", "replay", "cut-scene", "other", "walk-out", "celebration", "mega", "mega-cut"}; return n[c >= 0 && c < kClasses ? c : kOther]; }
inline bool HeldClass(int c) { return c != kPlay && c != kOther; }

// scenes30: the class of a picture. paused: pause or home menu; replay / nis / gameplay: the task state's bits (gameplay = exactly the gameplay task); mega: the Mega Strike's own scene
// (VgxMegaActive); moment: the presentation function (1 GameBegin = the walk-out, 2 Goal* = a goal's celebration, 3 Megastrike* = the Mega Strike's launch / result, 0 anything else or unknown).
inline int MomentOf(bool paused, bool replay, bool nis, bool gameplay, bool mega, int moment) {
    if (paused) return kOther;
    if (replay) return kReplay;
    if (mega) return kMega;
#if NP_HEAD17_BREAK == 6
    if (nis) return kCut;
#endif
    if (nis) return moment == 1 ? kWalkout : moment == 2 ? kCeleb : moment == 3 ? kMegaCut : kCut;
    return gameplay ? kPlay : kOther;
}

const float kOverMs = 16.7f;      // one refresh at 60 (the verdict's limit)
const float kOverHeldMs = 33.4f;  // scenes30: one 30 period, the limit of a held class
const float kShipP95Ms = 14.5f;   // the plan's rule: game-thread p95 under this to ship Smooth for a stadium class
const double kWindowS = 30.0;
const int kBuckets = 7;           // buckets per class in the pool (step, nostep, rt, gpu, sim, join, work)
const int kCap = 4096;            // samples kept per bucket in a window (30 s at up to 130 pictures a second)

// Nearest-rank percentile of a[0..n) (sorts it), pct 1..100 (integer: no floating rounding at the rank). n == 0: -1.
inline float Pct(float* a, int n, int pct) {
    if (n <= 0) return -1.0f;
    std::sort(a, a + n);
    int rank = (n * pct + 99) / 100;   // ceil(n * pct / 100)
#if NP_HEAD17_BREAK == 1
    rank = (n * pct) / 100;            // rounds down
#endif
    int k = rank - 1;
    if (k < 0) k = 0;
    if (k >= n) k = n - 1;
    return a[k];
}

struct Bucket {
    float* v = nullptr;
    int n = 0;
    unsigned dropped = 0;
    float mx = 0;
    void Add(float x) { if (n < kCap) v[n++] = x; else ++dropped; if (x > mx) mx = x; }
    void Clear() { n = 0; dropped = 0; mx = 0; }
};

struct ClassWin {
    Bucket step, nostep;       // game-thread ms of a picture with / without a simulation step
    Bucket rt, gpu;            // renderer lanes, ms
    Bucket sim, join;          // pace18 (MSC_SIMT=1|2): np_sim's wall ms (milliseconds) of the picture's step and the game thread's wait at the join; filled only for pictures that kicked a step
    unsigned joinWaited = 0;   // ... pictures whose join waited more than 0.05 ms
    Bucket work;               // heldcpu24: game ms less the frame-send join's wait (NpFsJoin: blocked time the controller's wait counters do not count); filled when the platform reports the join
    double fsjSum = 0; float fsjMax = 0; unsigned fsjN = 0, fsjWaited = 0;   // the join's wait ms a picture: sum, max, pictures, pictures that waited more than 0.5 ms
    unsigned pics = 0, late = 0, relief = 0;
    unsigned gameOver = 0, rtOver = 0, gpuOver = 0;
    void Clear() { step.Clear(); nostep.Clear(); rt.Clear(); gpu.Clear(); sim.Clear(); join.Clear(); work.Clear(); fsjSum = 0; fsjMax = 0; fsjN = fsjWaited = 0; joinWaited = 0; pics = late = relief = gameOver = rtOver = gpuOver = 0; }
};

#if NP_HEAD17_BREAK == 7
inline float OverMs(int) { return kOverMs; }
#else
inline float OverMs(int cls) { return HeldClass(cls) ? kOverHeldMs : kOverMs; }
#endif

class Stat {
public:
    void Init() {   // allocates (only when the switch is on)
        if (mem_) return;
        mem_ = (float*)std::calloc((size_t)kClasses * kBuckets * kCap + 2 * kCap, sizeof(float));   // + the sort scratch of Line
        if (!mem_) return;
        for (int c = 0; c < kClasses; ++c) {
            float* b = mem_ + (size_t)c * kBuckets * kCap;
            w_[c].step.v = b; w_[c].nostep.v = b + kCap; w_[c].rt.v = b + 2 * kCap; w_[c].gpu.v = b + 3 * kCap; w_[c].sim.v = b + 4 * kCap; w_[c].join.v = b + 5 * kCap; w_[c].work.v = b + 6 * kCap;
        }
    }
    bool Ready() const { return mem_ != nullptr; }
    // Game thread, once per picture (the one that just ended). cls: the class of the scene it was in; late: held more refreshes than the cadence asked; relief: under the controller's 30.
    void Picture(int cls, float gameMs, int steps, bool late, bool relief, float simMs = -1.0f, float joinMs = -1.0f, float fsJoinMs = -1.0f) {
        if (!mem_) return;
        if (cls < 0 || cls >= kClasses) cls = kOther;
        cur_.store(cls, std::memory_order_relaxed);
        ClassWin& w = w_[cls];
        ++w.pics;
#if NP_HEAD17_BREAK == 2
        if (steps <= 0) w.step.Add(gameMs); else w.nostep.Add(gameMs);
#else
        if (steps > 0) w.step.Add(gameMs); else w.nostep.Add(gameMs);
#endif
        if (late) ++w.late;
        if (relief) ++w.relief;
        if (simMs >= 0.0f) {   // pace18: the picture kicked a step on np_sim
            w.sim.Add(simMs);
#if NP_HEAD17_BREAK == 5
            w.join.Add(simMs);
            if (simMs > 0.05f) ++w.joinWaited;
#else
            w.join.Add(joinMs > 0.0f ? joinMs : 0.0f);
            if (joinMs > 0.05f) ++w.joinWaited;
#endif
        }
        if (fsJoinMs >= 0.0f) {   // heldcpu24: the frame-send join's wait is inside gameMs (the controller counts only the swap / late / pad waits); the work is the rest
            w.fsjSum += fsJoinMs;
            if (fsJoinMs > w.fsjMax) w.fsjMax = fsJoinMs;
            ++w.fsjN;
            if (fsJoinMs > 0.5f) ++w.fsjWaited;
#if NP_HEAD17_BREAK == 8
            w.work.Add(gameMs);
#else
            w.work.Add(gameMs > fsJoinMs ? gameMs - fsJoinMs : 0.0f);
#endif
        }
#if NP_HEAD17_BREAK == 3
        if (gameMs > 16.0f) ++w.gameOver;
#else
        if (gameMs > OverMs(cls)) ++w.gameOver;
#endif
    }
    // Renderer threads: lane 1 = render thread (back end), lane 2 = GPU (the lanes of NpLateLaneNote). One writer a lane.
    void LaneNote(int lane, float ms) {
        if (!mem_ || lane < 1 || lane > 2) return;
        Ring& r = ring_[lane - 1];
        const uint32_t i = r.n.load(std::memory_order_relaxed);
#if NP_HEAD17_BREAK == 4
        r.cls[i % kRing] = (uint8_t)((cur_.load(std::memory_order_relaxed) + 1) % kClasses);
#else
        r.cls[i % kRing] = (uint8_t)cur_.load(std::memory_order_relaxed);
#endif
        r.ms[i % kRing] = ms;
        r.n.store(i + 1, std::memory_order_release);
    }
    // Game thread, at a window's end: pulls the renderer samples noted since the last window into the class buckets.
    void Pull() {
        for (int l = 0; l < 2; ++l) {
            Ring& r = ring_[l];
            const uint32_t n = r.n.load(std::memory_order_acquire);
            uint32_t from = read_[l];
            if (n - from > (uint32_t)kRing) from = n - (uint32_t)kRing;
            for (uint32_t i = from; i != n; ++i) {
                const int c = r.cls[i % kRing] < kClasses ? r.cls[i % kRing] : kOther;
                const float ms = r.ms[i % kRing];
                ClassWin& w = w_[c];
                if (l == 0) { w.rt.Add(ms); if (ms > OverMs(c)) ++w.rtOver; }
                else { w.gpu.Add(ms); if (ms > OverMs(c)) ++w.gpuOver; }
            }
            read_[l] = n;
        }
    }
    // The line of one class (clears it). Returns 0 when the class had no pictures.
    size_t Line(int cls, char* out, size_t cap, bool lock60, int policyAt60, int switches, double seconds) {
        ClassWin& w = w_[cls];
        if (w.pics == 0) { w.Clear(); return 0; }
        float* allv = mem_ + (size_t)kClasses * kBuckets * kCap;
        int na = 0;
        for (int i = 0; i < w.step.n; ++i) allv[na++] = w.step.v[i];
        for (int i = 0; i < w.nostep.n; ++i) allv[na++] = w.nostep.v[i];
        const float allP95 = Pct(allv, na, 95);
        const float sP50 = Pct(w.step.v, w.step.n, 50), sP95 = Pct(w.step.v, w.step.n, 95);
        const float nP50 = Pct(w.nostep.v, w.nostep.n, 50), nP95 = Pct(w.nostep.v, w.nostep.n, 95);
        const float rP95 = Pct(w.rt.v, w.rt.n, 95), gP95 = Pct(w.gpu.v, w.gpu.n, 95);
        const char* verdict = HeldClass(cls) ? "none (nothing over 33.4 ms)" : "none (nothing over 16.7 ms)";
        unsigned best = 0;
        if (w.gameOver > best) { best = w.gameOver; verdict = "game"; }
        if (w.rtOver > best) { best = w.rtOver; verdict = "render"; }
        if (w.gpuOver > best) { best = w.gpuOver; verdict = "GPU"; }
        const int k = std::snprintf(out, cap,
            "[head] %.0f s, class %s (lock60 %d): %u pictures (%d with a step, %d without), late %u (%.1f %%), relief: policy %d now, %u pictures at 30, %d switches | "
            "game ms: with step p50 %.1f p95 %.1f max %.1f, without p50 %.1f p95 %.1f max %.1f, all p95 %.1f (limit %.1f: %s) | "
            "render thread ms p95 %.1f max %.1f (n %d), GPU ms p95 %.1f max %.1f (n %d) | over %.1f ms: game %u render %u GPU %u: %s%s",
            seconds, ClassName(cls), lock60 ? 1 : 0, w.pics, w.step.n + (int)w.step.dropped, w.nostep.n + (int)w.nostep.dropped, w.late, 100.0 * w.late / w.pics, policyAt60, w.relief, switches,
            sP50, sP95, w.step.mx, nP50, nP95, w.nostep.mx, allP95, kShipP95Ms, allP95 <= kShipP95Ms ? "ok" : "OVER",
            rP95, w.rt.mx, w.rt.n, gP95, w.gpu.mx, w.gpu.n, OverMs(cls), w.gameOver, w.rtOver, w.gpuOver, verdict, (w.step.dropped || w.nostep.dropped) ? " (samples capped)" : "");
        size_t used = k > 0 ? (size_t)k : 0;
        if (w.sim.n > 0 && used + 1 < cap) {   // pace18: only with MSC_SIMT=1|2, so the line of the other runs is unchanged
            const float qP50 = Pct(w.sim.v, w.sim.n, 50), qP95 = Pct(w.sim.v, w.sim.n, 95), jP95 = Pct(w.join.v, w.join.n, 95);
            const int k2 = std::snprintf(out + used, cap - used, " | sim thread (MSC_SIMT) busy ms p50 %.1f p95 %.1f max %.1f, join wait ms p95 %.1f max %.1f, joins that waited %u of %d",
                                         qP50, qP95, w.sim.mx, jP95, w.join.mx, w.joinWaited, w.join.n + (int)w.join.dropped);
            if (k2 > 0 && (size_t)k2 < cap - used) used += (size_t)k2;
        }
        if (w.fsjN > 0 && used + 1 < cap) {   // heldcpu24: where the game ms went; only with the frame-send worker (the join exists), so the line of other runs is unchanged
            const float wP50 = Pct(w.work.v, w.work.n, 50), wP95 = Pct(w.work.v, w.work.n, 95);
            const int k3 = std::snprintf(out + used, cap - used, " | frame-send join wait (inside game ms, not counted as a wait) mean %.1f max %.1f ms, %u of %u pictures waited over 0.5 ms; game ms less that wait (the work) p50 %.1f p95 %.1f",
                                         w.fsjSum / (double)w.fsjN, w.fsjMax, w.fsjWaited, w.fsjN, wP50, wP95);
            if (k3 > 0 && (size_t)k3 < cap - used) used += (size_t)k3;
        }
        w.Clear();
        return used;
    }
    ClassWin& Win(int c) { return w_[c]; }
private:
    static const int kRing = 4096;
    struct Ring { float ms[kRing]; uint8_t cls[kRing]; std::atomic<uint32_t> n{0}; };
    float* mem_ = nullptr;
    ClassWin w_[kClasses];
    Ring ring_[2];
    uint32_t read_[2] = {0, 0};
    std::atomic<int> cur_{0};
};

} // namespace nphead
