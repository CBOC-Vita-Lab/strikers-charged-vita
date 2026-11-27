// MSC_DYNRES_BLIND: the dynamic resolution keeps answering a GPU overload when the GPU timers stop reporting (header-only so a test and the trace model can run it on its own).
#pragma once

#include <cstdint>

namespace vgx {

// From the Galaxy port's r86 fix (console, Beach Bowl at zoom 2.0: the GPU timers stopped reporting under a heavy overload, so the quality ladder never stepped down:
// 25 fps at 960 for 25 s). The same blind spot is here. The ladder reads only the GPU's own busy time (vglRtLastGpuFrame), and it acts only when a new, fully timed
// frame arrives. vitaGL gives a scene no fragment notification once its watcher is 48 scenes behind (rt.c, RT_SLOTS - 16), and a frame with one untimed scene is
// published untimed (vglRtLastGpuFrame: timed != scenes). With ~17 scenes a frame, three frames in flight are enough: exactly when the GPU is the bottleneck and the
// queue is full, the measurements stop, and with them every step down (the queue-depth rule of the host reads the same timers and does not move either).
// The guard: after `quietAfter` pictures with no new timed GPU frame, a picture longer than `lateAt` times the budget whose CPU lanes (the busiest core's per-frame
// work over the same stretch: the game thread's core, the render thread's, the frame send's) are known and within `cpuFit` of the budget is counted as GPU-late:
// the renderer then feeds the ladder that picture with the GPU taken as busy for the whole of it (its length as the GPU time). A late picture with a busy CPU lane,
// or with no CPU measurement, is not counted (a smaller picture does not help a CPU-late one). Other blind pictures give no measurement, only a frame count, so a
// step's check window (MSC_DYNRES_UNDO) still ends; the steps up wait for real measurements. The guard steps aside as soon as a timed frame arrives.
//   Picture() returns kNone while the timers report, else the verdict for this picture.
struct BlindGuard {
    enum Verdict { kNone, kOnTime, kGpuLate, kCpuLate, kUnknown };
    bool on = false;
    int quietAfter = 6;      // pictures without a new timed GPU frame (results normally come two frames late)
    float lateAt = 1.25f;    // a picture this many budgets long or longer is late (at 30 a late one is 50 ms, at 60 33 ms; jitter of the thread's clock stays under)
    float cpuFit = 0.90f;    // the busiest CPU lane's work must be within this share of the budget
    int quiet = 0;           // pictures since the last timed GPU frame
    bool blind = false;
    // the log: pictures of the current or last blind stretch by verdict, blind stretches so far
    uint32_t pictures = 0, gpuLate = 0, cpuLate = 0, unknown = 0, spells = 0;
    // One picture. fresh: a new timed GPU frame arrived; intervalMs: the picture's length; cpuMs: the busiest CPU lane's work in that time (< 0: not known);
    // budget: the frame budget (16.7 ms at 60, 33.4 at 30).
    Verdict Picture(bool fresh, float intervalMs, float cpuMs, float budget) {
        if (!on) return kNone;
        if (fresh) {
            quiet = 0;
            blind = false;
            return kNone;
        }
        if (quiet < 1000000) ++quiet;
        if (quiet < quietAfter) return kNone;
        if (!blind) {
            blind = true;
            ++spells;
            pictures = gpuLate = cpuLate = unknown = 0;
        }
        ++pictures;
        if (!(intervalMs > lateAt * budget)) return kOnTime;
        if (!(cpuMs >= 0.0f)) { ++unknown; return kUnknown; }
        if (cpuMs > cpuFit * budget) { ++cpuLate; return kCpuLate; }
        ++gpuLate;
        return kGpuLate;
    }
    // The renderer wants the CPU lanes measured from the picture before the first blind one (a stretch needs two probe reads).
    bool WantCpu() const { return on && quiet + 1 >= quietAfter; }
    bool Blind() const { return on && blind; }
};

// The CPU lanes from the platform's per-core probe (NpFpsProbeOut: cumulative busy ms per core, and the part of it that follows time: audio, timers): the busiest core's
// per-frame work between two reads. < 0 when there is no earlier read or no probe.
struct LaneClock {
    double cpu[4] = {}, time[4] = {};
    bool have = false;
    float Take(const double cpuNow[4], const double timeNow[4]) {
        float busiest = -1.0f;
        if (have) {
            busiest = 0.0f;
            for (int c = 0; c < 4; ++c) {
                const double d = (cpuNow[c] - cpu[c]) - (timeNow[c] - time[c]);
                if (d > busiest) busiest = static_cast<float>(d);
            }
        }
        for (int c = 0; c < 4; ++c) { cpu[c] = cpuNow[c]; time[c] = timeNow[c]; }
        have = true;
        return busiest;
    }
    void Forget() { have = false; }
};

} // namespace vgx
