// Host test of the MegaPrewarm size rule (vgx_megawarm.h, nxfly25).
#include "../src/vgx_megawarm.h"
#include <cstdio>
#include <cstdlib>
using namespace vgx::megawarm;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); std::exit(1); } } while (0)
int main() {
    // Smooth 60 table: 3 rungs (960x544, 768x544, 728x512), native 2; with the effects at low resolution start = 4: outside, 640x448 is no rung
    CHECK(!Wanted(4, 3, false));
    CHECK(Wanted(2, 3, false));    // the table's own end (728x512): kept as before
    CHECK(Wanted(4, 3, true));     // outside the table but the same size as a rung (the boot-time 768x544): kept
    CHECK(!Wanted(3, 3, false));   // one past the end
    CHECK(!Wanted(-1, 3, false));
    // Quality table: 4 rungs, native 0, start 2: inside
    CHECK(Wanted(2, 4, false));
    // the old 13-rung table with the native size at 3 (MSC_PICTURE_FLOORS=0): start 5 inside
    CHECK(Wanted(5, 13, false));
    std::printf("mega_warm: ok\n");
    return 0;
}
