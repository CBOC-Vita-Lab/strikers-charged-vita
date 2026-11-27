// Host test of vgx_heldaa.h: the budget bounds of MSC_FXAA_WARM=2 and the rule that makes the one-sample held targets ahead.
#include "../src/vgx_heldaa.h"
#include <cstdio>
using namespace vgxheldaa;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)
int main() {
    CHECK(WarmN(0) == 1 && WarmN(-3) == 1 && WarmN(6) == 6 && WarmN(1000) == 64, "WarmN");
    CHECK(WarmMs(0) == 1.0 && WarmMs(10) == 10.0 && WarmMs(999) == 40.0, "WarmMs");
    const long kib = 960L * 544 * 8 / 1024;   // 4080 KiB: colour + depth at one sample
    AheadIn a{true, true, false, false, false, false, kib, 40000, 12288};
    CHECK(Ahead(a) == kAheadMake, "make when room");
    AheadIn b = a; b.warm2 = false;      CHECK(Ahead(b) == kAheadNo, "mode 1 or 0 never");
    b = a; b.heldOneSample = false;      CHECK(Ahead(b) == kAheadNo, "a stadium whose held scenes keep 2x never");
    b = a; b.play = true;                CHECK(Ahead(b) == kAheadNo, "never in live play");
    b = a; b.mega = true;                CHECK(Ahead(b) == kAheadNo, "never in a Mega Strike");
    b = a; b.heavy = true;               CHECK(Ahead(b) == kAheadNo, "never in the heavy profile");
    b = a; b.kept = true;                CHECK(Ahead(b) == kAheadNo, "already kept");
    b = a; b.freeKib = 12288 + kib - 1;  CHECK(Ahead(b) == kAheadNoRoom, "one KiB short of the floor");
    b = a; b.freeKib = 12288 + kib;      CHECK(Ahead(b) == kAheadMake, "exactly at the floor");
    b = a; b.freeKib = -1;               CHECK(Ahead(b) == kAheadMake, "unknown free memory: no objection");
    CHECK(SceneWant(2, false, 2, 3) && SceneWant(3, false, 2, 3) && !SceneWant(1, false, 2, 3) && !SceneWant(2, true, 2, 3), "SceneWant");
    std::printf(fails ? "heldaa: %d FAILED\n" : "heldaa: all passed\n", fails);
    return fails ? 1 : 0;
}
