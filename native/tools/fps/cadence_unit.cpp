// Unit test of MSC_CADENCE (platform/compat/np_fps_cadence.h, with Auto::Resume of np_fps_ctl.h and Between::Resume of np_fps_between.h): synthetic scene sequences, no game.
//   clang++ -std=c++11 -O1 -Iplatform/compat tools/fps/cadence_unit.cpp -o /tmp/cadence_unit && /tmp/cadence_unit [--break=N]
// What it proves: open play is never held; a replay and a cut-scene start held at a locked 30 from their first frame; a verdict never changes inside a scene (only at a cut) and
// does not flap; a class is released only after consecutive fit visits and held again with a longer proof after a failed release; the policy of before a held scene is put back at
// the cut to open play (where the controller alone would keep the match at 30 for its cool-down); a goal makes the controller wait for its cut, for 4 s at most.
// --break=1: everything counts as fit (fitMargin 100): the held-at-first-frame and no-flap checks must fail.
// --break=2: the controller is not given Resume at the cut to open play: the check that open play is not left at 30 must fail.
// --break=3: a failed release does not double the proof (visitsMax 2): the longer-proof check must fail.
// --break=4: the deferral never ends (deferMax huge): the 4 s limit check must fail.
#include "np_fps_ctl.h"
#include "np_fps_between.h"
#include "np_fps_cadence.h"

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

CadenceConfig MakeConfig() {
    CadenceConfig c;
    if (g_break == 1) c.fitMargin = 100.0;
    if (g_break == 3) c.visitsMax = 2;
    if (g_break == 4) c.deferMax = 1e9;
    return c;
}

struct Sim {
    Cadence cad;
    double now = 0;
    int policy = 2;      // what the "controller" is in (the test's own: 2 = 60, 1 = 30)
    bool paced = false;
    int frames = 0;
    // the verdict changes seen, and where
    bool lastHold = false;
    int changesInside = 0, changesAtCut = 0;
    int resumes = 0, go60s = 0, defers = 0;
    int lastResumePolicy = 0;
    bool lastResumePaced = false;
    bool openHeld = false;       // a frame of open play or other that was held
    Sim() : cad(MakeConfig()) {}
    // one frame of a scene; cost: its predicted 60 Hz cost in ms; dropToThirty: the controller reports policy 30 (as if it dropped a released scene)
    CadenceOut Frame(int cls, unsigned scene, double cost, double intervalMs = 33.4, bool enabled = true) {
        CadenceIn in;
        memset(&in, 0, sizeof in);
        in.now = now;
        in.cls = cls;
        in.scene = scene;
        in.valid = true;
        in.costMs = cost;
        in.refreshMs = R * 1000.0;
        in.intervalMs = intervalMs;
        in.enabled = enabled;
        in.policy = policy;
        in.paced = paced;
        const CadenceOut o = cad.Frame(in);
        if ((cls == kCadOpen || cls == kCadOther) && o.hold30) openHeld = true;
        if (o.hold30 != lastHold) { if (o.cut) ++changesAtCut; else ++changesInside; lastHold = o.hold30; }
        if (o.resume) { ++resumes; lastResumePolicy = o.resumePolicy; lastResumePaced = o.resumePaced; }
        if (o.go60) ++go60s;
        if (o.deferDown) ++defers;
        now += 0.0334;
        ++frames;
        return o;
    }
    // a whole visit of n frames
    void Visit(int cls, unsigned scene, int n, double cost) { for (int i = 0; i < n; ++i) Frame(cls, scene, cost); }
};

enum { kPlay = 2, kNisState = 16, kReplayState = 8 };

void TestOpenNeverHeld() {
    Sim s;
    srand(7);
    for (int i = 0; i < 4000; ++i) {
        const int pick = rand() % 6;
        int cls = pick < 3 ? kCadOpen : (pick == 3 ? kCadReplay : (pick == 4 ? kCadCut : kCadOther));
        const unsigned scene = cls == kCadOpen ? kPlay : (cls == kCadReplay ? kReplayState : (cls == kCadCut ? kNisState : 1));
        const int n = 5 + rand() % 60;
        for (int k = 0; k < n; ++k) s.Frame(cls, scene, 8.0 + (rand() % 3000) / 100.0);
    }
    check(!s.openHeld, "open play and other scenes are never held (4000 random scenes)");
    check(s.changesInside == 0, "a verdict changes only at a cut (random sequence)");
}

void TestStartsHeld() {
    Sim s;
    s.Visit(kCadOpen, kPlay, 300, 9.0);
    CadenceOut first = s.Frame(kCadCut, kNisState, 24.0);
    check(first.cut && first.hold30, "a goal celebration is held from its first frame (the cut)");
    bool all = true;
    for (int i = 0; i < 200; ++i) all = all && s.Frame(kCadCut, kNisState, 24.0).hold30;
    check(all, "and for every frame of the shot");
    CadenceOut r = s.Frame(kCadReplay, kReplayState, 22.0);
    check(r.cut && r.hold30, "the replay after it is held from its first frame");
}

void TestEarnsAndNoFlap() {
    Sim s;
    int released = -1;
    for (int v = 0; v < 6 && released < 0; ++v) {
        s.Visit(kCadOpen, kPlay, 200, 9.0);
        const CadenceOut f = s.Frame(kCadReplay, kReplayState, 10.0);
        for (int i = 0; i < 100; ++i) s.Frame(kCadReplay, kReplayState, 10.0);
        if (!f.hold30) released = v;
    }
    check(released == 2, "a class whose frames fit is released at the cut of its third visit (two fit visits first)", released == 2 ? "" : "wrong visit");
    check(s.go60s >= 1, "the released class starts at 60 from the cut (go60), not two seconds into the shot");
    check(s.changesInside == 0, "no verdict change inside a shot");
    // alternate fit and unfit visits: never released
    Sim t;
    for (int v = 0; v < 30; ++v) {
        t.Visit(kCadOpen, kPlay, 100, 9.0);
        t.Visit(kCadReplay, kReplayState, 100, (v & 1) ? 24.0 : 10.0);
    }
    check(t.cad.Held(kCadReplay, 0), "fit and unfit visits alternating never release a class (no flapping)");
}

void TestShortAndStall() {
    Sim s;
    for (int v = 0; v < 5; ++v) {
        s.Visit(kCadOpen, kPlay, 60, 9.0);
        s.Visit(kCadReplay, kReplayState, 30, 10.0);   // 1 s but fewer than the judged frames
    }
    check(s.cad.Held(kCadReplay, 0), "short visits say nothing (never released)");
    Sim t;
    for (int v = 0; v < 5; ++v) {
        t.Visit(kCadOpen, kPlay, 60, 9.0);
        t.Frame(kCadReplay, kReplayState, 10.0);
        for (int i = 0; i < 160; ++i) t.Frame(kCadReplay, kReplayState, 10.0, i % 2 ? 300.0 : 33.4);   // every second frame a load stall: not judged, the rest fits
    }
    check(!t.cad.Held(kCadReplay, 0), "loading stalls inside a visit are not judged against it");
}

void TestFailedRelease() {
    Sim s;
    // earn the release (need 2)
    for (int v = 0; v < 2; ++v) { s.Visit(kCadOpen, kPlay, 100, 9.0); s.Visit(kCadReplay, kReplayState, 100, 10.0); }
    s.Visit(kCadOpen, kPlay, 5, 9.0);
    check(!s.cad.Held(kCadReplay, 0), "released after two fit visits");
    // the released visit: the controller drops to 30 inside it
    s.Visit(kCadOpen, kPlay, 100, 9.0);
    s.policy = 2;
    for (int i = 0; i < 30; ++i) s.Frame(kCadReplay, kReplayState, 10.0);
    s.policy = 1;
    for (int i = 0; i < 30; ++i) s.Frame(kCadReplay, kReplayState, 10.0);
    s.policy = 2;
    s.Visit(kCadOpen, kPlay, 100, 9.0);
    check(s.cad.Held(kCadReplay, 0), "a released class that the controller dropped inside the shot is held again");
    check(s.cad.Need(kCadReplay, 0) == 4, "and needs twice as many fit visits next time");
    int v = 0;
    for (; v < 10 && s.cad.Held(kCadReplay, 0); ++v) { s.Visit(kCadReplay, kReplayState, 100, 10.0); s.Visit(kCadOpen, kPlay, 100, 9.0); }
    check(v == 4, "four fit visits release it again", v == 4 ? "" : "wrong count");
}

void TestResume() {
    // the controller on its own: Hold30 for a replay, then open play: stays 30 for its cool-down
    Config cfg;
    Auto ctl(cfg), ctlResume(cfg);
    double now = 0;
    auto frame = [&](Auto& c, unsigned scene, int cls, bool hold, bool resume, int policyBefore, FrameIn& fi) {
        (void)cls; (void)policyBefore;
        memset(&fi, 0, sizeof fi);
        fi.now = now;
        fi.interval = R;
        fi.steps = 1;
        fi.simMs = 4;
        fi.costsValid = true;
        fi.cpuMs[0] = 9; fi.cpuMs[1] = 6; fi.cpuMs[2] = 5; fi.gameCore = 0;
        fi.scene = scene;
        if (hold) c.Hold30(now);
        else if (resume) c.Resume(kPolicy60, now);
        else c.Frame(fi);
        now += R;
    };
    FrameIn fi;
    // both are at 60 in open play (settled by a long fit)
    for (int i = 0; i < 400; ++i) { frame(ctl, 2, 0, false, false, 0, fi); }
    for (int i = 0; i < 400; ++i) { frame(ctlResume, 2, 0, false, false, 0, fi); }
    // (a fresh controller starts at 30 and needs 2 s of evidence: 400 frames at 16.7 ms is 6.7 s: both are at 60 now)
    check(ctl.Current() == kPolicy60 && ctlResume.Current() == kPolicy60, "the controller reaches 60 in open play on a light load");
    // a replay of 5 s held
    double t0 = now;
    for (int i = 0; i < 300; ++i) { frame(ctl, 8, 0, true, false, 0, fi); }
    now = t0;
    for (int i = 0; i < 300; ++i) { frame(ctlResume, 8, 0, true, false, 0, fi); }
    check(ctl.Current() == kPolicy30 && ctlResume.Current() == kPolicy30, "a replay is held at 30");
    // back to open play: without Resume the cool-down holds 30; with it 60 at the cut
    frame(ctlResume, 2, 0, false, g_break != 2, 0, fi);
    int framesAt30 = 0;
    for (int i = 0; i < 1200; ++i) { frame(ctl, 2, 0, false, false, 0, fi); if (ctl.Current() == kPolicy30) ++framesAt30; else break; }
    check(framesAt30 >= 240, "without Resume the controller keeps open play at 30 for its cool-down and 2 s of evidence (several seconds)");
    check(ctlResume.Current() == kPolicy60, "with Resume open play is at 60 at the first frame of its cut");
    int stays = 0;
    for (int i = 0; i < 600; ++i) { frame(ctlResume, 2, 0, false, false, 0, fi); if (ctlResume.Current() == kPolicy60) ++stays; }
    check(stays >= 590, "and stays there on a light load (the settled frames are not judged, no cool-down)");
    // Resume to 30 changes nothing else
    Auto c30(cfg);
    const Decision d = c30.Resume(kPolicy30, 1.0);
    check(!d.switched && c30.Current() == kPolicy30, "Resume to the policy already in force is no switch");
    // Between::Resume
    Between b;
    for (int i = 0; i < 80; ++i) b.Frame(i * 0.0334, 2, true, 14.0, 25.0);
    check(b.On(), "the free run is in force in open play (setup)");
    b.Frame(10.0, 8, false, 14.0, 25.0);
    check(!b.On(), "a replay ends the free run");
    b.Resume(10.1, true);
    const bool on = b.Frame(10.1, 2, true, 14.0, 25.0);
    check(on, "Between::Resume puts the free run back at once at the cut to open play");
    Between b2;
    b2.Resume(1.0, false);
    check(!b2.Frame(1.0, 2, true, 14.0, 25.0), "Resume with the free run not in force before leaves it off (hold time as ever)");
}

void TestCadenceResumeFlag() {
    Sim s;
    s.policy = 2;
    s.paced = false;
    s.Visit(kCadOpen, kPlay, 100, 9.0);
    s.Visit(kCadCut, kNisState, 100, 24.0);
    s.Visit(kCadReplay, kReplayState, 100, 22.0);
    const CadenceOut o = s.Frame(kCadOpen, kPlay, 9.0);
    check(o.resume && o.resumePolicy == 2 && !o.hold30, "open play after the shots: resume with the policy of before (60)");
    check(s.resumes == 1, "exactly once");
    Sim p;
    p.policy = 1;
    p.paced = true;
    p.Visit(kCadOpen, kPlay, 100, 20.0);
    p.Visit(kCadReplay, kReplayState, 100, 22.0);
    p.policy = 1;
    const CadenceOut o2 = p.Frame(kCadOpen, kPlay, 20.0);
    check(o2.resume && o2.resumePolicy == 1 && o2.resumePaced, "AUTO's free run of before is remembered (30, paced)");
    Sim q;   // pause and the menus between: not a resume
    q.Visit(kCadOpen, kPlay, 100, 9.0);
    q.Visit(kCadOther, 1, 100, 9.0);
    q.Visit(kCadOpen, kPlay, 100, 9.0);
    check(q.resumes == 0, "a pause and back is not a resume (no held scene in between)");
    Sim d;   // disabled (a LAN session)
    d.Visit(kCadOpen, kPlay, 50, 9.0);
    bool held = false;
    for (int i = 0; i < 100; ++i) held = held || d.Frame(kCadReplay, kReplayState, 30.0, 33.4, false).hold30;
    check(!held, "disabled: nothing is ever held");
}

void TestDefer() {
    Sim s;
    s.Visit(kCadOpen, kPlay, 100, 9.0);
    s.cad.NoteGoal(s.now);
    int deferred = 0;
    double firstEnd = -1;
    const double t0 = s.now;
    for (int i = 0; i < 400; ++i) {
        const CadenceOut o = s.Frame(kCadOpen, kPlay, 20.0);
        if (o.deferDown) ++deferred; else if (firstEnd < 0) firstEnd = s.now - t0;
    }
    check(deferred > 100, "after a goal the controller waits (deferDown)");
    check(firstEnd > 3.9 && firstEnd < 4.2, "for 4 seconds at most (no cut came)", "");
    Sim u;
    u.Visit(kCadOpen, kPlay, 100, 9.0);
    u.cad.NoteGoal(u.now);
    for (int i = 0; i < 40; ++i) u.Frame(kCadOpen, kPlay, 9.0);
    const CadenceOut cut = u.Frame(kCadCut, kNisState, 24.0);
    check(cut.cut && cut.hold30 && !cut.deferDown, "the cut ends the wait, and the celebration is held from it");
    check(u.cad.Held(kCadCut, 1), "(the goal's cut-scene is its own class)");
    for (int i = 0; i < 50; ++i) u.Frame(kCadCut, kNisState, 24.0);
    const CadenceOut back = u.Frame(kCadOpen, kPlay, 9.0);
    check(back.resume && !back.deferDown, "no wait left after the shots");
}

void TestOnlyAtCuts() {
    Sim s;
    srand(11);
    int cls = kCadOpen;
    unsigned scene = kPlay;
    for (int i = 0; i < 20000; ++i) {
        if (rand() % 120 == 0) {
            const int pick = rand() % 5;
            cls = pick < 2 ? kCadOpen : (pick == 2 ? kCadReplay : (pick == 3 ? kCadCut : kCadOther));
            scene = cls == kCadOpen ? kPlay : (cls == kCadReplay ? kReplayState : (cls == kCadCut ? kNisState : 1));
        }
        s.Frame(cls, scene, (rand() % 2) ? 9.0 : 24.0);
    }
    check(s.changesInside == 0, "20000 random frames: a verdict changed only at cuts");
    check(!s.openHeld, "and open play was never held");
}
} // namespace


// wave 7 MSC_CADENCE_MENUS: the class list, and the menus held like the other classes.
void TestMenus() {
    // the class list (ClassOfTaskState), menus on
    check(ClassOfTaskState(2, true) == kCadOpen, "class list: a match is open play");
    check(ClassOfTaskState(2 | 1, true) == kCadMenu, "class list: the pause menu over a match is a menu");
    check(ClassOfTaskState(4, true) == kCadMenu, "class list: the front end is a menu");
    check(ClassOfTaskState(16777216, true) == kCadMenu, "class list: the stadium viewer is a menu");
    check(ClassOfTaskState(4 | 33554432, true) == kCadMenu, "class list: the home-button menu over the front end stays a menu");
    check(ClassOfTaskState(2 | 33554432, true) == kCadOpen, "class list: the home-button menu over a match is not held by this");
    check(ClassOfTaskState(8, true) == kCadReplay && ClassOfTaskState(131072, true) == kCadReplay, "class list: replays");
    check(ClassOfTaskState(16, true) == kCadCut, "class list: walk-outs, intros, results cut-scenes (NIS)");
    check(ClassOfTaskState(2097152, true) == kCadOther && ClassOfTaskState(4194304, true) == kCadOther && ClassOfTaskState(65536, true) == kCadOther &&
              ClassOfTaskState(1048576, true) == kCadOther && ClassOfTaskState(8388608, true) == kCadOther && ClassOfTaskState(524288, true) == kCadOther,
          "class list: loading and boot states are never held (the controller's own load pace)");
    check(ClassOfTaskState(4, false) == kCadOther && ClassOfTaskState(2 | 1, false) == kCadOpen && ClassOfTaskState(16777216, false) == kCadOther, "class list: menus off, as before this wave");
    // behaviour
    Sim s;
    s.Visit(kCadOpen, 2, 300, 9.0);
    CadenceOut first = s.Frame(kCadMenu, 4, 6.0);
    check(first.cut && first.hold30, "a menu is held from its cut");
    bool inside = false;
    for (int i = 0; i < 600; ++i) { CadenceOut o = s.Frame(kCadMenu, 4, 6.0 + (rand() % 100) / 100.0); if (o.cut) inside = true; }
    check(!inside && s.changesInside == 0, "no rate change while a menu is navigated (the scene does not change)");
    CadenceOut back = s.Frame(kCadOpen, 2, 9.0);
    check(back.cut && back.resume && !back.hold30, "the cut back to open play puts the earlier policy back at once and holds nothing");
    // the pause menu: a cut in, a cut out, open play untouched between
    s.Visit(kCadOpen, 2, 120, 9.0);
    const CadenceOut p1 = s.Frame(kCadMenu, 2 | 1, 5.0);
    check(p1.cut && p1.hold30, "the pause menu is held from the pause");
    s.Visit(kCadMenu, 2 | 1, 60, 5.0);
    const CadenceOut p2 = s.Frame(kCadOpen, 2, 9.0);
    check(p2.cut && p2.resume && !p2.hold30, "unpausing resumes open play's rate at once");
    check(!s.openHeld, "open play is still never held with menus in the sequence");
    // a menu that fits 60 at full size earns 60 over visits (the same hysteresis as the other classes)
    Sim f;
    bool released = false;
    for (int v = 0; v < 4; ++v) {
        f.Visit(kCadOpen, 2, 60, 9.0);
        for (int i = 0; i < 160; ++i) { CadenceOut o = f.Frame(kCadMenu, 4, 5.0); if (v >= 2 && o.cut && o.go60) released = true; }
    }
    check(released, "a menu that fits 60 earns it after two fit visits, from its cut");
    // a menu that does not fit stays at 30 and is never taken inside the scene
    Sim h;
    for (int v = 0; v < 5; ++v) { h.Visit(kCadOpen, 2, 60, 9.0); h.Visit(kCadMenu, 4, 160, 22.0); }
    check(h.cad.Held(kCadMenu, 0) && h.changesInside == 0, "a menu that does not fit 60 stays held, and changes only at cuts");
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (!strncmp(argv[i], "--break=", 8)) g_break = atoi(argv[i] + 8);
    TestOpenNeverHeld();
    TestStartsHeld();
    TestEarnsAndNoFlap();
    TestShortAndStall();
    TestFailedRelease();
    TestResume();
    TestCadenceResumeFlag();
    TestDefer();
    TestOnlyAtCuts();
    TestMenus();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
