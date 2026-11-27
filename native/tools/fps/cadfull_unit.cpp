// Unit test of MSC_CADENCE_FULL (renderer/src/vgx_cadence_full.h driven by platform/compat/np_fps_cadence.h): scripted scene sequences, no game data.
//   clang++ -std=c++11 -O1 -Iplatform/compat -I../renderer/src tools/fps/cadfull_unit.cpp -o /tmp/cadfull_unit && /tmp/cadfull_unit [--break=N]
// What it proves: the size changes only on a frame that is a cut of the hold (the cut in and the cut out), never inside a shot and never in open play; the game's own size is taken
// only when its target is kept (never a size change that needs a target made); the size of before comes back at the cut out; nothing is asked of a scene that is not held; a disabled
// policy (LAN, MSC_CADENCE off) never asks for a size.
// --break=1: the size is also given on a frame inside the shot (every 10th): the "only at cuts" check must fail.
// --break=2: the target-kept test is ignored: the "never without its target" check must fail.
// --break=3: open play is treated as held: the open-play check must fail.
// --break=4: the cut out gives nothing back: the "back at the cut out" check must fail.
#include "np_fps_cadence.h"
#include "vgx_cadence_full.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace npfps;

namespace {
int g_fail = 0, g_pass = 0, g_break = 0;
void check(bool ok, const char* what) {
    if (ok) { ++g_pass; printf("ok   %s\n", what); }
    else { ++g_fail; printf("FAIL %s\n", what); }
}
const double R = 1001.0 / 60000.0;
const int kNative = 3;

struct Rig {
    Cadence cad;
    vgx::cadfull::Gov gov;
    double now = 0;
    int level = 8;          // the ladder's level in open play (0.78)
    bool nativeReady = true;
    bool backReady = true;
    bool enabled = true;
    int frame = 0;
    int cmdsOutsideCut = 0, cmdsInOpen = 0, cmdsWithoutTarget = 0, cmds = 0, cutFrames = 0, levelAtOutCut = -1;
    // one frame; returns the Cadence verdict
    CadenceOut Step(int cls, unsigned scene, double cost) {
        CadenceIn in;
        memset(&in, 0, sizeof in);
        in.now = now; in.cls = cls; in.scene = scene; in.valid = true; in.costMs = cost; in.refreshMs = R * 1000.0; in.intervalMs = 33.4;
        in.enabled = enabled; in.policy = 2;
        CadenceOut co = cad.Frame(in);
        vgx::cadfull::In fi;
        memset(&fi, 0, sizeof fi);
        fi.on = true;
        fi.held = co.hold30 || (g_break == 3 && cls == kCadOpen);
        fi.level = level; fi.native = kNative;
        fi.nativeReady = g_break == 2 ? true : nativeReady;
        fi.backReady = backReady;
        vgx::cadfull::Out fo = gov.Frame(fi);
        int want = fo.setLevel;
        if (g_break == 1 && want < 0 && (frame % 10) == 5 && co.hold30 && level > kNative) want = kNative;
        if (g_break == 4 && want > kNative) want = -1;
        if (want >= 0) {
            ++cmds;
            if (!fo.cut && !(g_break == 1)) ++cmdsOutsideCut;
            if (g_break == 1 && !co.cut) ++cmdsOutsideCut;
            if (cls == kCadOpen && !(co.resume || co.cut)) ++cmdsInOpen;
            if (want == kNative && !nativeReady) ++cmdsWithoutTarget;
            level = want;
        }
        if (fo.cut) ++cutFrames;
        ++frame;
        now += 1.0 / 30.0;
        return co;
    }
    void Run(int cls, unsigned scene, double cost, int n) { for (int i = 0; i < n; ++i) Step(cls, scene, cost); }
};
} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) if (!strncmp(argv[i], "--break=", 8)) g_break = atoi(argv[i] + 8);

    {   // open play, a goal celebration (held from its cut), open play again, a replay, open play
        Rig r;
        r.Run(kCadOpen, 2, 12.0, 300);
        check(r.cmds == 0, "open play alone asks for no size");
        const int openLevel = r.level;
        r.Run(kCadCut, 16, 24.0, 200);   // celebration: held at its cut
        check(r.level == kNative, "the held scene is at the game's own size from its cut");
        check(r.gov.CutsIn() == 1, "one cut in");
        r.Run(kCadOpen, 2, 12.0, 200);
        check(r.level == openLevel, "the cut back to open play gives the ladder's size of before");
        check(r.gov.CutsOut() == 1, "one cut out");
        r.Run(kCadReplay, 8, 24.0, 200);
        check(r.level == kNative, "a replay is at the game's own size too");
        r.Run(kCadOpen, 2, 12.0, 100);
        check(r.level == openLevel, "and back to the ladder's size after it");
        check(r.cmds == 4, "four commands in all: two cuts in, two cuts out");
        check(r.cmdsOutsideCut == 0, "every size command came on a cut frame");
        check(r.cmdsInOpen == 0 || r.cmdsInOpen == 2, "open play only gets its size back on the cut frame");
        check(r.cmdsWithoutTarget == 0, "no command without the target kept");
    }
    {   // the target is not kept: the picture stays, the miss is counted, nothing is made at a cut
        Rig r;
        r.nativeReady = false;
        r.Run(kCadOpen, 2, 12.0, 100);
        r.Run(kCadCut, 16, 24.0, 100);
        check(r.level == 8 && r.cmds == 0, "no kept target: no size change at the cut");
        check(r.gov.SkippedIn() == 1, "the miss is counted");
        r.Run(kCadOpen, 2, 12.0, 100);
        check(r.cmds == 0 && r.level == 8, "and nothing comes back either");
        check(r.cmdsWithoutTarget == 0, "never a size without its target");
    }
    {   // the way back needs its target too
        Rig r;
        r.Run(kCadOpen, 2, 12.0, 100);
        r.Run(kCadReplay, 8, 24.0, 100);
        r.backReady = false;
        r.Run(kCadOpen, 2, 12.0, 100);
        check(r.level == kNative && r.gov.SkippedOut() == 1, "the size of before is not asked for when its target is gone");
    }
    {   // a ladder that already is at the game's own size: nothing to give, nothing to give back
        Rig r;
        r.level = kNative;
        r.Run(kCadOpen, 2, 12.0, 50);
        r.Run(kCadReplay, 8, 24.0, 100);
        r.Run(kCadOpen, 2, 12.0, 50);
        check(r.cmds == 0, "already at the game's own size: no command");
    }
    {   // a class that has earned 60 is not held: its size is the ladder's
        Rig r;
        CadenceConfig cc;
        for (int v = 0; v < 3; ++v) { r.Run(kCadOpen, 2, 10.0, 60); r.Run(kCadReplay, 8, 5.0, 120); }
        r.Run(kCadOpen, 2, 10.0, 60);
        const int before = r.cmds;
        r.level = 8;
        r.Run(kCadReplay, 8, 5.0, 120);   // released by now
        check(r.cad.Held(kCadReplay, 0) == false, "(the replay class is released after fit visits)");
        check(r.cmds == before && r.level == 8, "a released class gets no size from this rule");
    }
    {   // a disabled policy never asks for a size
        Rig r;
        r.enabled = false;
        r.Run(kCadOpen, 2, 12.0, 50);
        r.Run(kCadReplay, 8, 24.0, 100);
        r.Run(kCadOpen, 2, 12.0, 50);
        check(r.cmds == 0, "policy off (LAN, MSC_CADENCE=0): no size");
    }
    {   // a size command at a cut back needs the cut to be in the hold's own frames: the check that Frame never gives one on a steady frame
        Rig r;
        r.Run(kCadOpen, 2, 12.0, 30);
        for (int i = 0; i < 5; ++i) { r.Run(kCadCut, 16, 24.0, 90); r.Run(kCadOpen, 2, 12.0, 90); }
        check(r.cmds == 10 && r.cmdsOutsideCut == 0, "five goals: ten commands, all at cuts");
    }
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
