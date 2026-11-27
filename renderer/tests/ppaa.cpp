// Host test of vgx_ppaa.h (aa24, MSC_VGX_PPAA): the scene classes, the GPU gate and the use rule.
#include "../src/vgx_ppaa.h"
#include <cstdio>
using namespace vgxppaa;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)
int main() {
    // lock60 kinds: 0 menu, 1 play, 2 replay, 3 cut-scene, 4 mega, 5 heavy
    CHECK(!ClassOn(0, true, true, 4), "mode 0 is never on");
    CHECK(ClassOn(1, true, false, 1) && ClassOn(1, false, true, 3) && ClassOn(1, false, false, 4), "mode 1: the Mega scene, the Mega cut hint");
    CHECK(!ClassOn(1, false, false, 2) && !ClassOn(1, false, false, 3) && !ClassOn(1, false, false, 1) && !ClassOn(1, false, false, 0), "mode 1: not a plain replay, cut-scene, play or menu");
    CHECK(ClassOn(2, false, false, 2) && ClassOn(2, false, false, 3), "mode 2: held scenes");
    CHECK(!ClassOn(2, false, false, 1) && !ClassOn(2, false, false, 5) && !ClassOn(2, false, false, 0), "mode 2: never live play, heavy profile or menus");
    Gate g;
    const float B = 33.4f;
    for (int i = 0; i < 30; ++i) g.Frame(26.0f, B);
    CHECK(!g.skip, "26 ms of 33.4 stays open");
    for (int i = 0; i < 30; ++i) g.Frame(34.5f, B);
    CHECK(g.skip && g.moves == 1, "over the budget closes");
    for (int i = 0; i < 100; ++i) g.Frame(30.0f, B);   // between leave (28.4) and enter (31.7): stays closed
    CHECK(g.skip, "30 ms: the gate stays shut (hysteresis)");
    for (int i = 0; i < 19; ++i) g.Frame(20.0f, B);
    CHECK(g.skip, "the calm count is not met yet");
    for (int i = 0; i < 40; ++i) g.Frame(20.0f, B);
    CHECK(!g.skip && g.moves == 2, "calm for 20 pictures opens it");
    g.Reset();
    CHECK(!g.skip && g.ema < 0, "reset");
    g.Frame(0.0f, B);
    CHECK(g.ema < 0, "an untimed picture changes nothing");
    g.Frame(60.0f, B);
    CHECK(g.skip, "one picture over 1.15 x the budget closes at once");
    CHECK(Use(1, true, 1, true, false), "use");
    CHECK(!Use(0, true, 1, true, false) && !Use(1, false, 1, true, false) && !Use(1, true, 2, true, false) && !Use(1, true, 1, false, false) && !Use(1, true, 1, true, true), "each refusal");
    std::printf(fails ? "ppaa: %d FAILED\n" : "ppaa: all passed\n", fails);
    return fails ? 1 : 0;
}
