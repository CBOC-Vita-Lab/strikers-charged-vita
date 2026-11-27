// Host test of vgx_nodepth.h: which acquires qualify for a pass object without depth memory (MSC_VGX_NODEPTH_PASS), and which pooled objects may serve which acquire.
#include "../src/vgx_nodepth.h"
#include <cstdio>
#include <vector>

using namespace vgx::nodepth;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

struct Obj { int w, h; bool noDepth; unsigned uses = 0; };
// the pool search of AcquireFboOnce: a preferred object first, else any accepted one, else a new object (made without depth memory when the acquire qualifies)
static int Acquire(std::vector<Obj>& pool, int w, int h, bool okNoDepth) {
    for (size_t i = 0; i < pool.size(); ++i)
        if (pool[i].w == w && pool[i].h == h && Preferred(pool[i].noDepth, okNoDepth)) return static_cast<int>(i);
    for (size_t i = 0; i < pool.size(); ++i)
        if (pool[i].w == w && pool[i].h == h && Accepts(pool[i].noDepth, okNoDepth)) return static_cast<int>(i);
    pool.push_back(Obj{w, h, okNoDepth});
    return static_cast<int>(pool.size() - 1);
}

int main() {
    CHECK(LevelFromSetting(0) == 0 && LevelFromSetting(-1) == 0 && LevelFromSetting(1) == 1 && LevelFromSetting(2) == 1, "setting");
    // qualify: off never; a copy (no depth wanted) always; a pass only with no depth plane
    CHECK(!Qualifies(0, false, true) && !Qualifies(0, true, true), "off never qualifies");
    CHECK(Qualifies(1, false, false), "a copy target qualifies");
    CHECK(Qualifies(1, true, QualifiesPass(1 | 2)), "a colour pass without the depth plane qualifies");
    CHECK(!Qualifies(1, true, QualifiesPass(7)), "a pass that uses depth does not");
    CHECK(!Qualifies(1, true, QualifiesPass(4)), "a depth-only pass does not");
    // pool: off (every acquire not ok) is the old behaviour: one object per size, reused
    {
        std::vector<Obj> p;
        const int a = Acquire(p, 512, 256, false), b = Acquire(p, 512, 256, false);
        CHECK(p.size() == 1 && a == 0 && b == 0 && !p[0].noDepth, "off: one object, with depth");
    }
    // on: a depth pass and a filter pass of one size make two objects; each later acquire finds its own
    {
        std::vector<Obj> p;
        const int f = Acquire(p, 512, 256, true), d = Acquire(p, 512, 256, false);
        CHECK(p.size() == 2 && f == 0 && d == 1 && p[0].noDepth && !p[1].noDepth, "on: two objects");
        CHECK(Acquire(p, 512, 256, true) == 0 && Acquire(p, 512, 256, false) == 1 && p.size() == 2, "each acquire finds its own");
    }
    // on: a filter pass takes an existing object with depth when there is no object without; a depth pass never takes one without
    {
        std::vector<Obj> p;
        Acquire(p, 96, 64, false);
        CHECK(Acquire(p, 96, 64, true) == 0 && p.size() == 1, "a qualifying acquire may use an object with depth");
        std::vector<Obj> q;
        Acquire(q, 96, 64, true);
        CHECK(Acquire(q, 96, 64, false) == 1 && q.size() == 2 && !q[1].noDepth, "a depth acquire never takes an object without depth");
    }
    // the memory: 990x1139 at one sample is 4.36 MiB with the tile alignment; 775x888 is 2.7 MiB (research16 item 2 said 4.3 and 2.6 unaligned)
    CHECK(DepthBytes(990, 1139, 1, false) / 1024 == 4464, "990x1139: %llu KiB", static_cast<unsigned long long>(DepthBytes(990, 1139, 1, false) / 1024));
    CHECK(DepthBytes(775, 888, 1, false) / 1024 == 2800, "775x888: %llu KiB", static_cast<unsigned long long>(DepthBytes(775, 888, 1, false) / 1024));
    CHECK(DepthBytes(960, 544, 2, true) / 1024 == 5100, "960x544 2x stencil");
    std::printf(fails ? "FAILED\n" : "nodepth: all checks passed\n");
    return fails != 0;
}
