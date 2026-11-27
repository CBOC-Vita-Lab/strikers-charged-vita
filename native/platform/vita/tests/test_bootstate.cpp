// The loading screen's life cycle (vita_bootstate.h): it ends for good, whichever path the game's pictures take, and is never drawn again. Paths driven here:
//   hold      the display callback composes every picture (fade runs, Compose ends it)
//   ladder    the display callback never composes (MSC_PICTURE_HOLD=0 in the Vita3K run that showed the screen over a match): only the game's presents
//   long      one present takes 17 s right after Finish (the cut into the walk-out in that run)
//   resume    a wake from sleep / the settings screen after Finish
//   thread    nothing else happens: the screen's own thread ends it
//   after     everything after the end: nothing draws, nothing re-arms
// Compile with -DBOOTSTATE_BREAK=1..4 to see each mutant caught (run_bootstate_test.sh does it).
#include "vita_bootstate.h"

#include <cstdio>

using namespace BootState;

static int g_fail = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
            ++g_fail;                                                         \
        }                                                                     \
    } while (0)

static Machine Started() {
    Machine m;
    m.mode = kArt;
    return m;
}
// The checks every path ends with: final, no drawing from any caller, no new Finish, one guard line.
static void AfterTheEnd(Machine& m, double t) {
    CHECK(m.mode == kDone);
    CHECK(!OnCompose(m, t + 0.01));
    CHECK(!OnCompose(m, t + 50.0));
    CHECK(!OnResume(m, t + 60.0));
    CHECK(!OnGamePresent(m, t + 61.0));
    CHECK(!OnThreadTick(m, t + 62.0));
    CHECK(!Finish(m, t + 63.0));
    CHECK(m.mode == kDone);
    CHECK(Guard(m));       // the first attempt is logged ...
    CHECK(!Guard(m));      // ... once
}

int main() {
    {   // hold path: Compose every picture
        Machine m = Started();
        CHECK(OnCompose(m, 29.0));                    // before Finish: drawn
        CHECK(Finish(m, 30.0));
        CHECK(!Finish(m, 30.1));
        CHECK(OnCompose(m, 30.2) && m.mode == kArt);  // the fade
        CHECK(OnCompose(m, 30.5) && m.mode == kDone); // the last picture of the fade is drawn, then it is over
        AfterTheEnd(m, 30.5);
    }
    {   // ladder path: no Compose after Finish, only game presents
        Machine m = Started();
        CHECK(Finish(m, 30.0));
        CHECK(!OnGamePresent(m, 30.2) && m.mode == kArt);   // inside the fade time
        CHECK(OnGamePresent(m, 30.6) && m.mode == kDone);   // the first present after it
        CHECK(m.endedBy != nullptr);
        AfterTheEnd(m, 30.6);
    }
    {   // a present before Finish never ends it
        Machine m = Started();
        CHECK(!OnGamePresent(m, 5.0) && m.mode == kArt);
        CHECK(!OnGamePresent(m, 500.0) && m.mode == kArt);
    }
    {   // a long present: Finish, then nothing for 17 s, then the present
        Machine m = Started();
        CHECK(Finish(m, 30.0));
        CHECK(OnGamePresent(m, 47.0) && m.mode == kDone);
        AfterTheEnd(m, 47.0);
    }
    {   // sleep / settings: before Finish the screen may be redrawn; after, it ends instead
        Machine m = Started();
        CHECK(OnResume(m, 10.0) && m.mode == kArt);
        CHECK(Finish(m, 30.0));
        CHECK(!OnResume(m, 31.0) && m.mode == kDone);
        AfterTheEnd(m, 31.0);
    }
    {   // the screen's own thread
        Machine m = Started();
        CHECK(!OnThreadTick(m, 20.0));
        CHECK(Finish(m, 30.0));
        CHECK(!OnThreadTick(m, 31.0) && m.mode == kArt);
        CHECK(OnThreadTick(m, 31.6) && m.mode == kDone);
        AfterTheEnd(m, 31.6);
    }
    {   // never re-armed: a text-mode or finished machine stays so
        Machine m;
        CHECK(!OnCompose(m, 1.0) && !OnResume(m, 1.0) && !OnGamePresent(m, 1.0) && !Finish(m, 1.0));
        Machine t;
        t.mode = kText;
        CHECK(!OnCompose(t, 1.0) && !Finish(t, 1.0) && t.mode == kText);
    }
    if (g_fail) {
        std::printf("test_bootstate: %d FAILED\n", g_fail);
        return 1;
    }
    std::printf("test_bootstate: ok\n");
    return 0;
}
