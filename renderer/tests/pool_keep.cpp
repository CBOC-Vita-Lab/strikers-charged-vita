// Host test of vgx_poolkeep.h: TrimPool's keep age and per-present quota (MSC_POOL_KEEP, MSC_POOL_TRIM_ONE). A small pool model runs the same decisions as TrimPool:
// objects of ladder sizes used in turn, a revisit later, and a memory-pressure phase.
#include "../src/vgx_poolkeep.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace vgx::poolkeep;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

struct Obj { uint32_t last; bool alive = true; };

// one TrimPool call over the model; returns the objects let go
static unsigned Trim(std::vector<Obj>& pool, uint32_t now, bool fast, bool roomy, uint32_t keep, bool one, bool tight = false) {
    const uint32_t age = Age(fast, roomy, true, keep), quota = Quota(tight, one);
    unsigned freed = 0;
    for (Obj& o : pool)
        if (o.alive && Due(now, o.last, age) && freed < quota) { o.alive = false; ++freed; }
    return freed;
}

int main() {
    // the settings: unset / 0 / negative are the old 600
    CHECK(KeepFromSetting(0) == 600 && KeepFromSetting(-5) == 600 && KeepFromSetting(18000) == 18000, "setting");
    CHECK(KeptTargetAge(600) == 1800 && KeptTargetAge(18000) == 18000, "kept target age");

    // old rule (keep 600, all at once): six sizes used at present 100, revisited at 900 (15 s at 60): all gone
    {
        std::vector<Obj> pool(6, Obj{100});
        unsigned gone = 0;
        for (uint32_t now = 100; now < 900; ++now) gone += Trim(pool, now, false, true, KeepFromSetting(0), false);
        CHECK(gone == 6, "old rule lets all six go before the revisit (%u)", gone);
    }
    // new rule (keep 18000, one per present): still there at 900, and at 17000
    {
        std::vector<Obj> pool(6, Obj{100});
        unsigned gone = 0;
        for (uint32_t now = 100; now < 17000; ++now) gone += Trim(pool, now, false, true, 18000, true);
        CHECK(gone == 0, "new rule keeps the objects of a size used 16900 presents ago (%u gone)", gone);
        for (uint32_t now = 17000; now < 18200; ++now) gone += Trim(pool, now, false, true, 18000, true);
        CHECK(gone == 6, "...and lets them go after the keep age (%u)", gone);
    }
    // one per present: 40 due at once go over 40 presents, never two in one
    {
        std::vector<Obj> pool(40, Obj{0});
        unsigned perPresent = 0, worst = 0, total = 0;
        for (uint32_t now = 20000; now < 20100; ++now) {
            perPresent = Trim(pool, now, false, true, 18000, true);
            if (perPresent > worst) worst = perPresent;
            total += perPresent;
        }
        CHECK(worst == 1 && total == 40, "worst %u per present, total %u", worst, total);
    }
    // memory pressure: not roomy, the old rules: 60 presents, and 2 right after a size change, all at once
    {
        std::vector<Obj> pool(10, Obj{100});
        CHECK(Age(false, false, true, 18000) == 60 && Age(true, false, true, 18000) == 2, "pressure ages");
        CHECK(Trim(pool, 161, false, false, 18000, true, true) == 10, "tight: all that are due go at once");
        std::vector<Obj> pool2(10, Obj{100});
        CHECK(Trim(pool2, 161, false, false, 18000, true, false) == 1, "between 12 and 24 MiB: the old age, one a present");
    }
    // not keeping sizes at all (MSC_VGX_KEEP_SIZES=0): 60 even when roomy
    CHECK(Age(false, true, false, 18000) == 60, "keep sizes off");
    // roomy fast trim: a size change with room does not shorten the age
    CHECK(Age(true, true, true, 18000) == 18000, "fast trim only under pressure");
    // wrap-around of the present counter
    CHECK(Due(5u, 0xFFFFFFF0u, 10u) && !Due(5u, 0xFFFFFFFCu, 10u), "wrap");
    // emergency trim: every live object not used in this present goes, whatever its age, the quota or the roomy keep; the ones used now stay; dead ones are not counted
    {
        std::vector<Obj> pool{Obj{100}, Obj{5000}, Obj{17990}, Obj{18000}, Obj{100, false}};
        const uint32_t now = 18000;
        unsigned gone = 0;
        for (Obj& o : pool)
            if (EmergencyDue(now, o.last, o.alive)) { o.alive = false; ++gone; }
        CHECK(gone == 3 && pool[3].alive, "emergency trim lets three go and keeps the object used this present (%u)", gone);
        unsigned again = 0;
        for (Obj& o : pool) if (EmergencyDue(now, o.last, o.alive)) ++again;
        CHECK(again == 0, "second emergency finds nothing");
    }
    // int29: hysteresis prevents thrashing when free VRAM oscillates around 12 MiB / 16 MiB boundaries
    {
        int tier = -1;
        CHECK(AgeByFreeKibHyst(false, 20000, true, 18000, tier) == 18000 && tier == 2, "start roomy");
        CHECK(AgeByFreeKibHyst(false, 16000, true, 18000, tier) == 1800 && tier == 1, "drop to mid below 16 MiB");
        CHECK(AgeByFreeKibHyst(false, 17000, true, 18000, tier) == 1800 && tier == 1, "stay mid inside 16..18 MiB hysteresis band");
        CHECK(AgeByFreeKibHyst(false, 18432, true, 18000, tier) == 18000 && tier == 2, "recover to roomy at 18 MiB");
        CHECK(AgeByFreeKibHyst(false, 11000, true, 18000, tier) == 60 && tier == 0, "drop to tight below 12 MiB");
        CHECK(AgeByFreeKibHyst(false, 13000, true, 18000, tier) == 60 && tier == 0, "stay tight inside 12..14 MiB hysteresis band");
        CHECK(AgeByFreeKibHyst(false, 14336, true, 18000, tier) == 1800 && tier == 1, "recover to mid at 14 MiB");
    }
    std::printf(fails ? "FAILED\n" : "pool_keep: ok\n");
    return fails ? 1 : 0;
}
