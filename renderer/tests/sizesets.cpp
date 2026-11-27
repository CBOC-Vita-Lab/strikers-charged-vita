// Host test of vgx_sizesets.h (MSC_COPY_SIZESETS): a copy texture's size changes there and back: the second change takes the dormant texture.
#include "../src/vgx_sizesets.h"
#include <cstdio>
using namespace vgxsizesets;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)
int main() {
    Store s;
    CHECK(KibOf(960, 544) == 2040, "kib");
    CHECK(BudgetKib(0) == 6144 && BudgetKib(1) == 6144 && BudgetKib(9000) == 9000 && BudgetKib(1L << 30) == 65536, "budget");
    // play 960x544 -> replay 752x424: the 960 texture (name 11) goes dormant, nothing to take for 752x424 yet
    CHECK(s.Take(5, 752, 424) == 0 && s.missed == 1, "first change finds none");
    CHECK(s.Stash(5, 960, 544, 11, 100, 6144, 30000, 12288), "stash fits");
    // replay -> play: the dormant 960 texture is taken, the 752 one (name 12) goes dormant
    CHECK(s.Take(5, 960, 544) == 11 && s.taken == 1 && s.Count() == 0 && s.Kib() == 0, "second change takes it");
    CHECK(s.Stash(5, 752, 424, 12, 200, 6144, 30000, 12288) && s.Count() == 1, "stash 752");
    // another slot, same size: separate entry
    CHECK(s.Take(6, 752, 424) == 0, "other slot has none");
    CHECK(s.Stash(6, 752, 424, 13, 200, 6144, 30000, 12288) && s.Count() == 2, "other slot stashed");
    // the same slot and size twice: refused (the caller deletes)
    CHECK(!s.Stash(5, 752, 424, 14, 201, 6144, 30000, 12288), "duplicate refused");
    // budget: 6144 KiB holds three 960x544 textures (2040 each), not a fourth with the 2 small ones
    Store t;
    CHECK(t.Stash(1, 960, 544, 21, 0, 6144, -1, 0) && t.Stash(2, 960, 544, 22, 0, 6144, -1, 0) && t.Stash(3, 960, 544, 23, 0, 6144, -1, 0), "three fit");
    CHECK(!t.Stash(4, 960, 544, 24, 0, 6144, -1, 0) && t.refused == 1, "fourth over budget");
    // memory floor: 12288 + 2040 needed
    Store u;
    CHECK(!u.Stash(1, 960, 544, 31, 0, 6144, 12288 + 2039, 12288) && u.Stash(1, 960, 544, 31, 0, 6144, 12288 + 2040, 12288), "floor");
    // DropSlot and DropAll hand names back
    std::vector<uint32_t> out;
    t.DropSlot(2, out);
    CHECK(out.size() == 1 && out[0] == 22 && t.Count() == 2 && t.Kib() == 4080, "drop slot");
    out.clear(); t.DropAll(out);
    CHECK(out.size() == 2 && t.Count() == 0 && t.Kib() == 0, "drop all");
    std::printf(fails ? "sizesets: %d FAILED\n" : "sizesets: all passed\n", fails);
    return fails ? 1 : 0;
}
