// holdgpu24: MSC_LOCK60_STICKY (platform/compat/np_fps_ctl.h Config::sticky): the 60 / full-size-30 switching of Smooth 60 must not flap (s29 console: 4-8 switches per 30 s).
//   clang++ -std=c++11 -O1 -Iplatform/compat tools/fps/fps_sticky_unit.cpp -o /tmp/fps_sticky_unit && /tmp/fps_sticky_unit
// Mutants (compile with -DNP_STICKY_BREAK=N; the run must FAIL): 1 no minimum hold after a relief, 2 the ordinary proof (1.5 s at 85 %), 3 no latch, 4 a new scene forces 60 at once.
// Also --off: the controller without the sticky layer (the control: the bursty load flaps).
// Every figure is synthetic (an estimate): the evidence is the lanes' predicted cost, never lateness.
#include "np_fps_ctl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

using namespace npfps;

namespace {
int g_fail = 0, g_pass = 0;
bool g_off = false;
void check(bool ok, const char* what, const char* detail = "") {
    if (ok) { ++g_pass; printf("ok   %s %s\n", what, detail); }
    else { ++g_fail; printf("FAIL %s %s\n", what, detail); }
}
const double R = 1001.0 / 60000.0;

struct Sim {
    Auto ctl;
    double now = 0;
    unsigned scene = 2;
    int sceneClass = 0;
    std::vector<double> sw;   // times of every switch
    static Config Make() { Config c = Config::Lock60(); if (g_off) c.sticky = false; return c; }
    Sim() : ctl(Make()) {}
    void Step(double gpu, double gpuMin, double cpu, bool room) {
        FrameIn f;
        memset(&f, 0, sizeof f);
        const Policy p = ctl.Current();
        const bool miss = p == kPolicy60 && (gpu > R * 1000.0 || cpu > R * 1000.0);
        const double interval = p == kPolicy60 ? (miss ? 2.0 * R : R) : 2.0 * R;
        now += interval;
        f.now = now; f.interval = interval; f.steps = 1; f.simMs = cpu * 0.5; f.costsValid = true; f.gameCore = 0;
        f.cpuMs[0] = cpu; f.cpuMs[1] = cpu * 0.5; f.scene = scene; f.gpuMinMs = gpuMin; f.sceneClass = sceneClass; f.ladderRoom = room;
        if (ctl.Frame(f).switched) sw.push_back(now);
    }
    void Run(double secs, double gpu, double gpuMin, double cpu, bool room = false) { const double end = now + secs; while (now < end) Step(gpu, gpuMin, cpu, room); }
    int In(double a, double b) const { int n = 0; for (double t : sw) if (t >= a && t < b) ++n; return n; }
    int MaxIn30() const { int m = 0; for (double t : sw) { const int n = In(t, t + 30.0); if (n > m) m = n; } return m; }
};
} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) if (!strcmp(argv[i], "--off")) g_off = true;
    // 1. a bursty load (Vice: the game thread): 12 ms at 60 for 4 s, then 20 ms for 6 s, again and again for 200 s. The base rules flap; the sticky layer relieves once and stays.
    {
        Sim s;
        for (int c = 0; c < 20; ++c) { s.Run(4.0, 12.0, 11.0, 12.0); s.Run(6.0, 22.0, 20.0, 20.0); }
        char d[120]; snprintf(d, sizeof d, "(%zu switches in %.0f s, at most %d in any 30 s)", s.sw.size(), s.now, s.MaxIn30());
        if (g_off) check(s.sw.size() > 6, "(control, sticky off) the bursty load flaps", d);
        else check(s.sw.size() <= 4 && s.MaxIn30() <= 1, "bursty CPU-bound load: at most 4 switches in 200 s and one in any 30 s", d);
    }
    // 2. the GPU is the limit at The Dump (gpuMin over a refresh at the floor): one relief, no return
    {
        Sim s;
        s.Run(5.0, 12.0, 11.0, 12.0);
        s.Run(300.0, 24.0, 20.0, 12.0);
        char d[120]; snprintf(d, sizeof d, "(%zu switches in 300 s)", s.sw.size());
        check(s.sw.size() <= 1 && s.ctl.Current() == kPolicy30, "a GPU limit: one relief, the full-size 30 stays", d);
    }
    // 3. a genuine recovery: after the relief the load drops for good: back at 60 after the hold and the proof, once
    {
        Sim s;
        s.Run(3.0, 12.0, 11.0, 12.0);
        s.Run(3.0, 22.0, 20.0, 20.0);
        const double t30 = s.sw.empty() ? 0.0 : s.sw[0];
        s.Run(80.0, 11.0, 10.0, 11.0);
        const double t60 = s.sw.size() >= 2 ? s.sw[1] : -1.0;
        char d[120]; snprintf(d, sizeof d, "(30 at %.1f s, 60 at %.1f s: %.1f s later, %zu switches)", t30, t60, t60 - t30, s.sw.size());
        if (g_off) check(s.sw.size() == 2, "(control) the base rules return too", d);
        else check(s.sw.size() == 2 && s.ctl.Current() == kPolicy60 && t60 - t30 >= 25.0 && t60 - t30 <= 60.0, "a load that really fits comes back to 60 after the 25 s hold and the proof, once", d);
    }
    // 4. a short window of headroom (8 s at 60-level load, then over again) never returns: the proof is 12 s
    {
        Sim s;
        s.Run(3.0, 12.0, 11.0, 12.0);
        s.Run(3.0, 22.0, 20.0, 20.0);
        for (int c = 0; c < 8; ++c) { s.Run(8.0, 11.0, 10.0, 11.0); s.Run(10.0, 22.0, 20.0, 20.0); }
        char d[100]; snprintf(d, sizeof d, "(%zu switches in %.0f s)", s.sw.size(), s.now);
        if (g_off) check(s.sw.size() >= 3, "(control) 8 s windows of headroom do bring 60 back and forth", d);
        else check(s.sw.size() == 1, "8 s windows of headroom (under the 12 s proof) never bring 60 back", d);
    }
    // 5. three reliefs within 180 s latch the 30: scene changes (goal, kick-off) do not force 60 for the next 240 s
    {
        Sim s;
        for (int k = 0; k < 3; ++k) {
            s.Run(3.0, 12.0, 11.0, 12.0);
            s.Run(3.0, 22.0, 20.0, 20.0);
            s.Run(70.0, 11.0, 10.0, 11.0);   // (the hold (25 s, then 50), then the proof: back at 60 before the next relief)
        }
        const size_t before = s.sw.size();
        for (int k = 0; k < 4; ++k) { s.scene = 20 + k; s.Run(20.0, 11.0, 10.0, 11.0); s.scene = 2; s.Run(5.0, 22.0, 20.0, 20.0); }
        char d[140]; snprintf(d, sizeof d, "(%zu switches before the scene changes, %zu after, in %.0f s)", before, s.sw.size() - before, s.now);
        if (!g_off) check(s.sw.size() == before, "after repeated reliefs the 30 is latched: scene changes and 100 s of headroom add no switch", d);
        else check(true, "(control) skipped");
    }
    // 6. a relief in a replay is not a gameplay relief: the next scene is 60 as before
    {
        Sim s;
        s.Run(3.0, 12.0, 11.0, 12.0);
        s.scene = 8; s.sceneClass = 1;
        s.Run(1.0, 12.0, 11.0, 12.0);
        s.Run(3.0, 22.0, 21.0, 12.0);
        const bool at30 = s.ctl.Current() == kPolicy30;
        s.scene = 2; s.sceneClass = 0; s.Step(12.0, 11.0, 12.0, true);
        check(at30 && s.ctl.Current() == kPolicy60, "a replay relief does not stick: the next scene starts at 60");
    }
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
