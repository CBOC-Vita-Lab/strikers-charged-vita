// Unit test of the LOCKED 60 controller (platform/compat/np_fps_ctl.h Config::Lock60, MSC_LOCK60): synthetic frames, no game.
//   clang++ -std=c++11 -O1 -Iplatform/compat tools/fps/fps_lock60_unit.cpp -o /tmp/fps_lock60_unit && /tmp/fps_lock60_unit [--break=N]
// --break=1: the controller is built without lock60 (AUTO's start at 30 and its scene rules): the start-at-60 and new-scene checks must fail.
// --break=2: no hold for a ladder with room (ladderHold off): the check that a GPU miss waits for the renderer's rungs must fail.
// --break=3: a relief outside a match is not held to the scene's end (classHold off): the replay check must fail.
#include "np_fps_ctl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace npfps;

namespace {
int g_fail = 0, g_pass = 0, g_break = 0;
void check(bool ok, const char* what, const char* detail = "") {
    if (ok) { ++g_pass; printf("ok   %s %s\n", what, detail); }
    else { ++g_fail; printf("FAIL %s %s\n", what, detail); }
}
const double R = 1001.0 / 60000.0;

Config MakeConfig() {
    Config c = Config::Lock60();
    c.sticky = false;   // (these checks are of the base LOCKED 60 rules; the sticky layer has its own test, fps_sticky_unit.cpp)
    if (g_break == 1) c = Config::V11();
    if (g_break == 2) c.ladderHold = false;
    if (g_break == 3) c.classHold = false;
    return c;
}

struct Sim {
    Auto ctl;
    double now = 0;
    int frames = 0, toThirty = 0, toSixty = 0;
    unsigned scene = 2;     // task state: 2 gameplay
    int sceneClass = 0;
    double firstDownAt = -1;
    Sim() : ctl(MakeConfig()) {}
    // One frame: gpu = the GPU's ms per frame at the picture in force, gpuMin = what the renderer predicts at its floor, cpu = the busiest core's ms at 60 Hz,
    // room = the renderer has rungs left. The frame lasts a refresh when the stage times fit it at 60, two when they do not (a 60 that misses is held a refresh longer).
    void Step(double gpu, double gpuMin, double cpu, bool room, bool dynGrace = false) {
        FrameIn f;
        memset(&f, 0, sizeof f);
        const Policy p = ctl.Current();
        const bool miss = p == kPolicy60 && (gpu > R * 1000.0 || cpu > R * 1000.0);
        const double interval = p == kPolicy60 ? (miss ? 2.0 * R : R) : 2.0 * R;
        now += interval;
        f.now = now;
        f.interval = interval;
        f.steps = 1;
        f.simMs = cpu * 0.5;
        f.costsValid = true;
        f.gameCore = 0;
        f.cpuMs[0] = cpu;
        f.cpuMs[1] = cpu * 0.5;
        f.scene = scene;
        f.gpuMinMs = gpuMin;
        f.sceneClass = sceneClass;
        f.ladderRoom = room;
        f.dynGrace = dynGrace;
        const Decision d = ctl.Frame(f);
        if (d.switched) {
            if (d.policy == kPolicy30) { ++toThirty; if (firstDownAt < 0) firstDownAt = now; } else ++toSixty;
        }
        ++frames;
    }
    void Run(double secs, double gpu, double gpuMin, double cpu, bool room) {
        const double end = now + secs;
        while (now < end) Step(gpu, gpuMin, cpu, room);
    }
};
} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (!strncmp(argv[i], "--break=", 8)) g_break = atoi(argv[i] + 8);
    // 1. the start: at 60, and a load that fits stays there
    {
        Sim s;
        check(s.ctl.Current() == kPolicy60, "a session starts at 60");
        s.Run(60.0, 12.0, 11.0, 12.0, true);
        char d[100]; snprintf(d, sizeof d, "(%d switches in 60 s)", s.ctl.Switches());
        check(s.ctl.Current() == kPolicy60 && s.ctl.Switches() == 0, "a load that fits holds 60 for a minute with no switch", d);
    }
    // 2. a GPU-limited scene while the renderer has rungs: the verdict waits (at most 2 s), then 30
    {
        Sim s;
        s.Run(5.0, 12.0, 11.0, 12.0, true);
        const double t0 = s.now;
        // the GPU is over a refresh at the picture in force but fits at the floor (14.5 ms predicted there), with rungs left: the ladder is working
        s.Run(1.2, 21.0, 14.5, 12.0, true);
        const bool heldFirst = s.ctl.Current() == kPolicy60;
        // the ladder is out of rungs and the floor still does not fit
        s.Run(2.0, 21.0, 20.0, 12.0, false);
        char d[120]; snprintf(d, sizeof d, "(30 at +%.1f s)", s.firstDownAt - t0);
        check(heldFirst, "a GPU-limited miss waits while the renderer has rungs left", d);
        check(s.ctl.Current() == kPolicy30, "no rung left and frames still miss: 30 (the relief)", d);
    }
    // 3. a relief in gameplay is tried again after the cool-down, and a load that really fits holds
    {
        Sim s;
        s.Run(3.0, 12.0, 11.0, 12.0, true);
        s.Run(3.0, 21.0, 20.0, 12.0, false);   // to 30
        const bool at30 = s.ctl.Current() == kPolicy30;
        s.Run(12.0, 12.0, 11.0, 12.0, true);   // the scene is lighter now
        char d[100]; snprintf(d, sizeof d, "(%d to 30, %d to 60)", s.toThirty, s.toSixty);
        check(at30 && s.ctl.Current() == kPolicy60 && s.toThirty == 1, "gameplay: back to 60 after the cool-down once the load fits, one relief only", d);
    }
    // 4. a load at the margin: tried again and again, but with the cool-down doubling (no flip-flop)
    {
        Sim s;
        for (int i = 0; i < 12; ++i) {
            s.Run(8.0, 16.0, 14.2, 12.0, i % 2 == 0);   // the prediction at the floor is 85 % of a refresh: tried; the picture then misses
            s.Run(1.0, 18.5, 14.2, 12.0, false);
        }
        char d[100]; snprintf(d, sizeof d, "(%d to 30 and %d to 60 in %.0f s)", s.toThirty, s.toSixty, s.now);
        check(s.toThirty <= 8, "a load at the margin: at most 8 reliefs in two minutes", d);
    }
    // 5. a relief in a replay lasts until the replay ends; the next scene starts at 60 again
    {
        Sim s;
        s.Run(3.0, 12.0, 11.0, 12.0, true);
        s.scene = 8; s.sceneClass = 1;          // a replay
        s.Run(1.0, 12.0, 11.0, 12.0, true);
        s.Run(3.0, 22.0, 21.0, 12.0, false);    // GPU-bound replay at the floor
        const bool at30 = s.ctl.Current() == kPolicy30;
        s.Run(25.0, 12.0, 11.0, 12.0, true);    // the load fits again, but it is the same scene: no retry
        const bool still30 = s.ctl.Current() == kPolicy30;
        s.scene = 2; s.sceneClass = 0;          // back to play
        s.Step(12.0, 11.0, 12.0, true);
        char d[100]; snprintf(d, sizeof d, "(30 in the replay %d, still 30 after 25 s %d, 60 at the next scene %d)", at30, still30, s.ctl.Current() == kPolicy60);
        check(at30 && still30 && s.ctl.Current() == kPolicy60, "a relief outside gameplay lasts to the end of the scene; the next scene starts at 60", d);
    }
    // 6. a CPU core really over its budget goes to 30 at once (8 frames over 98 %), a core at 95 % does not
    {
        Sim s;
        s.Run(5.0, 12.0, 11.0, 15.8, true);   // 95 % of a refresh: still 60
        const bool ok95 = s.ctl.Current() == kPolicy60;
        s.Run(2.0, 12.0, 11.0, 17.5, true);   // over a refresh
        char d[100]; snprintf(d, sizeof d, "(95 %% stays: %d, over: 30 %d)", ok95, s.ctl.Current() == kPolicy30);
        check(ok95 && s.ctl.Current() == kPolicy30, "a CPU-limited scene goes to 30 quickly; a core at 95 % of a refresh does not", d);
    }
    // 7. AUTO's own controller still starts at 30 (the existing mode is untouched)
    {
        Auto a;
        check(a.Current() == kPolicy30, "AUTO (no lock60) still starts at 30");
    }
    printf("%d passed, %d failed%s\n", g_pass, g_fail, g_break ? " (deliberate break)" : "");
    return g_fail ? 1 : 0;
}
