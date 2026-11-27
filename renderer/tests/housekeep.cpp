// Host test of vgx_housekeep.h: the sweep back-off ladder and the trim-and-remake guard (MSC_VGX_HOUSEKEEP_BACKOFF, MSC_POOL_THRASH_GUARD).
// A small pool model runs the same decisions as TrimPool. -DVGX_HK_BREAK=1..4 must fail (run_housekeep_test.sh).
#include "../src/vgx_housekeep.h"
#include <cstdio>
#include <vector>

using namespace vgx::housekeep;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

struct Obj { int w, h; uint32_t last; bool alive; };

int main() {
    // the ladder: 120 -> 240 -> 480 -> 960, then stays; something freed: back to 120
    {
        uint64_t g = 120;
        g = SweepGap(120, g, true, 960);  CHECK(g == 240, "first empty sweep doubles (%llu)", (unsigned long long)g);
        g = SweepGap(120, g, true, 960);  CHECK(g == 480, "second (%llu)", (unsigned long long)g);
        g = SweepGap(120, g, true, 960);  CHECK(g == 960, "third (%llu)", (unsigned long long)g);
        g = SweepGap(120, g, true, 960);  CHECK(g == 960, "ceiling holds (%llu)", (unsigned long long)g);
        g = SweepGap(120, g, false, 960); CHECK(g == 120, "a sweep that freed something goes back to the base (%llu)", (unsigned long long)g);
        CHECK(SweepGap(4, 4, true, 4) == 4, "a ceiling at the base never lengthens");
    }
    // the cost: 20000 pictures with nothing to free, sweeps at the ladder's pace vs the fixed pace
    {
        uint64_t next = 0, gap = 30, fixedSweeps = 0, ladderSweeps = 0;
        for (uint64_t t = 0; t < 20000; ++t) if (t % 30 == 0) ++fixedSweeps;
        for (uint64_t t = 0; t < 20000; ++t) if (t >= next) { ++ladderSweeps; gap = SweepGap(30, gap, true, 960); next = t + gap; }
        CHECK(ladderSweeps * 8 < fixedSweeps, "the ladder scans far less when nothing is due (%llu vs %llu)", (unsigned long long)ladderSweeps, (unsigned long long)fixedSweeps);
        CHECK(ladderSweeps >= 20, "but it still looks (%llu)", (unsigned long long)ladderSweeps);
    }
    // the loop: a memory figure that reads 0 (tight) lets go of objects unused for 60 presents; a hold ladder rehearses its sizes again and again
    auto run = [](bool guard, unsigned* trims, unsigned* makes) {
        ThrashGuard tg;
        std::vector<Obj> pool;
        *trims = *makes = 0;
        const int sizes[3][2] = {{960, 544}, {864, 490}, {777, 441}};
        for (uint32_t now = 1; now <= 6000; ++now) {
            // the ladder visits one size every 100 presents (an object used then is fresh)
            const int s = static_cast<int>((now / 100) % 3);
            if (now % 100 == 0) {
                bool have = false;
                for (Obj& o : pool) if (o.alive && o.w == sizes[s][0] && o.h == sizes[s][1]) { o.last = now; have = true; }
                if (!have) {
                    pool.push_back(Obj{sizes[s][0], sizes[s][1], now, true});
                    ++*makes;
                    if (guard) tg.Made(sizes[s][0], sizes[s][1], now);
                }
            }
            for (Obj& o : pool) {
                if (!o.alive || now - o.last <= 60) continue;
                if (guard && tg.Pinned(o.w, o.h)) continue;
                o.alive = false;
                ++*trims;
                if (guard) tg.Trimmed(o.w, o.h, now);
            }
        }
    };
    unsigned t0, m0, t1, m1;
    run(false, &t0, &m0);
    run(true, &t1, &m1);
    CHECK(m0 > 30, "without the guard the sizes are made again and again (%u makes)", m0);
    CHECK(m1 <= 6, "with the guard each size is made at most twice (%u makes)", m1);
    CHECK(t1 <= 3, "and trimmed at most once (%u trims)", t1);
    // a remake long after the trim is not a loop
    {
        ThrashGuard tg;
        tg.Trimmed(960, 544, 100);
        CHECK(!tg.Made(960, 544, 100 + kWindow + 1), "a remake after the window does not pin");
        CHECK(!tg.Pinned(960, 544), "not pinned");
        tg.Trimmed(960, 544, 10000);
        CHECK(tg.Made(960, 544, 10100), "a remake soon after the trim pins");
        CHECK(tg.Pinned(960, 544) && !tg.Pinned(960, 543), "pinned exactly that size");
        CHECK(tg.Remakes() == 1, "counted once");
        tg.Clear();
        CHECK(!tg.Pinned(960, 544) && tg.Remakes() == 0, "cleared");
    }
    // more sizes than slots: the table never overflows, a pinned size is the last to be forgotten
    {
        ThrashGuard tg;
        tg.Trimmed(1, 1, 5); tg.Made(1, 1, 6);
        for (int i = 2; i < 100; ++i) tg.Trimmed(i, i, static_cast<uint32_t>(10 + i));
        CHECK(tg.Pinned(1, 1), "the pinned size is kept while others rotate");
    }
    std::printf(fails ? "housekeep: %d FAILED\n" : "housekeep: all checks passed\n", fails);
    return fails ? 1 : 0;
}
