// Host test of vgx_atlasapart.h (nxfly25).
#include "../src/vgx_atlasapart.h"
#include <cstdio>
#include <cstdlib>
using namespace vgx::atlasapart;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); std::exit(1); } } while (0)
int main() {
    // the 25 Nov Vice case: the 128 x 128 figure pass (direct, joint size) samples crowd picture 490 (64 x 64 cell of the joint texture)
    Conflict c{true, true, true, true, false, 0, 8, true, false, false, true};
    CHECK(ApartTarget(c));
    Conflict d = c; d.sampledSameSize = true;       CHECK(!ApartTarget(d));   // a 128 pass reading a 128 cell: no help, refused as before
    d = c; d.apartCount = 8;                         CHECK(!ApartTarget(d));   // cap
    d = c; d.sampledApart = true;                    CHECK(!ApartTarget(d));   // the picture it reads is in an apart page already: no conflict
    d = c; d.sampledIsAtlasPicture = false;          CHECK(!ApartTarget(d));
    d = c; d.sampledInPassTexture = false;           CHECK(!ApartTarget(d));   // reads a picture of another texture: nothing to solve
    d = c; d.apartOn = false;                        CHECK(!ApartTarget(d));
    d = c; d.joinOn = false;                         CHECK(!ApartTarget(d));
    d = c; d.direct = false;                         CHECK(!ApartTarget(d));
    d = c; d.jointSize = false;                      CHECK(!ApartTarget(d));
    d = c; d.alreadyApart = true; d.apartCount = 8; d.sampledIsAtlasPicture = false; CHECK(ApartTarget(d));   // its page is its own: stays
    // sort safety
    CHECK(MaySample(false, true, true, true));      // the figure (apart) reads the crowd page that is before it in both orders
    CHECK(MaySample(true, false, true, true));
    CHECK(!MaySample(false, false, true, true));    // neither is apart: the old rule (no reorder)
    CHECK(!MaySample(false, true, false, true));    // the writer was after the reader in the order given (it reads last picture's content)
    CHECK(!MaySample(false, true, true, false));    // the sort would put the writer after the reader
    std::printf("atlas_apart: ok\n");
    return 0;
}
