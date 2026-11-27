// MSC_DYNRES_BLIND: which pictures count as GPU-late once the GPU timers have stopped reporting (vgx_dynres_blind.h). BREAK=1 builds a guard that ignores the CPU lanes.
#include "../src/vgx_dynres_blind.h"
#include <cstdio>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAILED line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

constexpr float kB30 = 33.37f, kB60 = 16.68f;
static vgx::BlindGuard Make() { vgx::BlindGuard g; g.on = true; return g; }
static vgx::BlindGuard::Verdict Pic(vgx::BlindGuard& g, bool fresh, float ms, float cpu, float b) {
#ifdef BLIND_BREAK
    if (cpu > 0.0f) cpu = 0.0f;   // the deliberate break: every late picture is the GPU's
#endif
    return g.Picture(fresh, ms, cpu, b);
}

int main() {
    using V = vgx::BlindGuard;
    {   // off: never a verdict
        vgx::BlindGuard g;
        for (int i = 0; i < 50; ++i) CHECK(Pic(g, false, 3 * kB30, 1.0f, kB30) == V::kNone);
        CHECK(!g.Blind());
    }
    {   // timers reporting: nothing, however late the pictures
        auto g = Make();
        for (int i = 0; i < 50; ++i) CHECK(Pic(g, true, 50.0f, 1.0f, kB30) == V::kNone);
        CHECK(!g.Blind() && g.quiet == 0);
    }
    {   // the usual two-frame delay and a few missing results are not a blind spell: quietAfter (6) pictures first
        auto g = Make();
        Pic(g, true, kB30, 5.0f, kB30);
        for (int i = 1; i < 6; ++i) CHECK(Pic(g, false, 50.0f, 5.0f, kB30) == V::kNone);
        CHECK(g.WantCpu());   // (the picture before the first blind one asks for the CPU read)
        CHECK(Pic(g, false, 50.0f, 5.0f, kB30) == V::kGpuLate);
        CHECK(g.Blind() && g.spells == 1);
    }
    {   // blind at 30 (the Galaxy console case: 25 fps, the CPU lanes well inside the budget): pictures of two and three refreshes, every late one is GPU-late
        auto g = Make();
        for (int i = 0; i < 6; ++i) Pic(g, false, 50.05f, 18.0f, kB30);
        int late = 0, onTime = 0;
        for (int i = 0; i < 100; ++i) {
            const V::Verdict v = Pic(g, false, (i & 1) ? 50.05f : 33.37f, 18.0f, kB30);
            late += v == V::kGpuLate;
            onTime += v == V::kOnTime;
        }
        CHECK(late == 50 && onTime == 50);
        CHECK(g.gpuLate == 51);
    }
    {   // the CPU lane is what is late (the game thread at 35 ms): not the GPU's, nothing is fed
        auto g = Make();
        for (int i = 0; i < 6; ++i) Pic(g, false, 50.0f, 35.0f, kB30);
        CHECK(Pic(g, false, 50.0f, 35.0f, kB30) == V::kCpuLate);
        CHECK(Pic(g, false, 50.0f, 0.95f * kB30, kB30) == V::kCpuLate);   // over cpuFit (90 %)
        CHECK(Pic(g, false, 50.0f, 0.85f * kB30, kB30) == V::kGpuLate);
        CHECK(g.gpuLate == 1 && g.cpuLate >= 2);
    }
    {   // no CPU measurement (no probe, or the first read): unknown, nothing is fed
        auto g = Make();
        for (int i = 0; i < 6; ++i) Pic(g, false, 50.0f, -1.0f, kB30);
        CHECK(Pic(g, false, 50.0f, -1.0f, kB30) == V::kUnknown);
        CHECK(g.gpuLate == 0);
    }
    {   // on-time pictures while blind: on time (the ladder gets a frame count, no measurement); the line is 1.25 budgets
        auto g = Make();
        for (int i = 0; i < 6; ++i) Pic(g, false, kB30, 10.0f, kB30);
        CHECK(Pic(g, false, kB30, 10.0f, kB30) == V::kOnTime);
        CHECK(Pic(g, false, 1.2f * kB30, 10.0f, kB30) == V::kOnTime);
        CHECK(Pic(g, false, 1.3f * kB30, 10.0f, kB30) == V::kGpuLate);
        // at 60: a late picture is two refreshes (33 ms)
        CHECK(Pic(g, false, kB60, 5.0f, kB60) == V::kOnTime);
        CHECK(Pic(g, false, 2 * kB60, 5.0f, kB60) == V::kGpuLate);
        CHECK(Pic(g, false, 2 * kB60, 16.0f, kB60) == V::kCpuLate);
    }
    {   // a timed frame ends the spell at once; a new spell is counted again
        auto g = Make();
        for (int i = 0; i < 10; ++i) Pic(g, false, 50.0f, 10.0f, kB30);
        CHECK(g.Blind());
        CHECK(Pic(g, true, 50.0f, 10.0f, kB30) == V::kNone);
        CHECK(!g.Blind() && g.quiet == 0 && !g.WantCpu());
        for (int i = 0; i < 6; ++i) Pic(g, false, 50.0f, 10.0f, kB30);
        CHECK(g.Blind() && g.spells == 2 && g.pictures == 1);
    }
    {   // the lane clock: busiest core's per-frame work between two reads; the time-following part (audio) is left out
        vgx::LaneClock l;
        double c0[4] = {100, 200, 300, 0}, t0[4] = {0, 0, 50, 0};
        CHECK(l.Take(c0, t0) < 0.0f);   // the first read: nothing to compare
        double c1[4] = {118, 214, 330, 2}, t1[4] = {0, 0, 70, 2};
        CHECK(l.Take(c1, t1) == 18.0f);   // core 0: 18, core 1: 14, core 2: 30 - 20 = 10, core 3: 0
        l.Forget();
        CHECK(l.Take(c1, t1) < 0.0f);
    }
    std::printf("%s\n", fails ? "dynres_blind: FAILED" : "dynres_blind: ok");
    return fails ? 1 : 0;
}
