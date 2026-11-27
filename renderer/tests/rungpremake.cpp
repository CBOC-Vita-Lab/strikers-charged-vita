// Host test of vgx_rungpremake.h (MSC_LOCK60_RUNG_PREMAKE): the walk plan under the memory rule, and the bookkeeping: every rung's copy textures are made once in the walk, a size step in play makes and frees
// nothing (it takes the dormant texture of the size and stashes the one it leaves), the match end frees all. A mutant (argument "mutant") deletes the texture a step leaves, as the code without the switch does:
// the test MUST fail then. Uses the shipped vgx_sizesets.h store as the backend does.
#include "../src/vgx_rungpremake.h"
#include "../src/vgx_sizesets.h"
#include <cstdio>
#include <cstring>
#include <vector>
using namespace vgx::rungpremake;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)
int main(int argc, char** argv) {
    const bool mutant = argc > 1 && !std::strcmp(argv[1], "mutant");
    // ---- Plan
    const int tour[3] = {2, 1, 3};   // levels in tour order: 2 = 728x512 (in use), 1 = 768x544, 3 = a deeper one
    int out[8]; bool nextOnly = true;
    int n = Plan(tour, 3, 2, 40000, 12288, 4096, out, 8, &nextOnly);
    CHECK(n == 2 && out[0] == 1 && out[1] == 3 && !nextOnly, "all rungs but the current one: n=%d", n);
    n = Plan(tour, 3, 2, 12288 + 4096 * 2 - 1, 12288, 4096, out, 8, &nextOnly);
    CHECK(n == 1 && out[0] == 3 && nextOnly, "short of memory: only the next rung down (level 3), n=%d out0=%d", n, n ? out[0] : -1);
    n = Plan(tour, 3, 1, 12288 + 4096, 12288, 4096, out, 8, &nextOnly);
    CHECK(n == 1 && out[0] == 2 && nextOnly, "from level 1 the next down is 2: n=%d", n);
    n = Plan(tour, 3, 2, 12288 + 4095, 12288, 4096, out, 8, &nextOnly);
    CHECK(n == 0, "not even one rung fits: n=%d", n);
    n = Plan(tour, 3, 2, -1, 12288, 4096, out, 8, &nextOnly);
    CHECK(n == 2, "unknown free memory: no objection: n=%d", n);
    const int dup[4] = {1, 1, 2, 2};
    n = Plan(dup, 4, 2, -1, 0, 0, out, 8, &nextOnly);
    CHECK(n == 1 && out[0] == 1, "duplicates and the current level dropped: n=%d", n);
    n = Plan(tour, 3, 3, 100000, 0, 4096, out, 8, &nextOnly);
    CHECK(n == 2 && !nextOnly, "deepest level in use: the others are walked");
    n = Plan(tour, 3, 3, 12288 + 4096, 12288, 4096, out, 8, &nextOnly);
    CHECK(n == 0, "deepest level in use, memory for one: no rung below it, none walked: n=%d", n);
    // ---- a match: three copy-texture slots, sizes per level, the walk makes each size once; steps afterwards make and free nothing
    const int wh[4][2] = {{960, 544}, {768, 544}, {728, 512}, {700, 500}};
    vgxsizesets::Store st;
    Ledger L;
    int madeTex = 0, deletedTex = 0;
    uint32_t nextName = 100;
    int curLevel[3] = {2, 2, 2};   // the level each slot's live texture is at (the match starts at level 2: three live textures)
    uint32_t live[3] = {nextName++, nextName++, nextName++};
    madeTex = 3;
    auto step = [&](int to) {   // CopyTexture's resize path, for each slot
        for (int s = 0; s < 3; ++s) {
            if (curLevel[s] == to) continue;
            uint32_t fresh = st.Take(static_cast<uint32_t>(s), wh[to][0], wh[to][1]);
            if (!fresh) { fresh = nextName++; ++madeTex; }
            const uint32_t old = live[s];
            const bool stashed = !mutant && st.Stash(static_cast<uint32_t>(s), wh[curLevel[s]][0], wh[curLevel[s]][1], old, 0, 12288, -1, 0);
            if (!stashed) ++deletedTex;
            live[s] = fresh;
            curLevel[s] = to;
        }
    };
    L.BeginMatch();
    // the walk: level 1, then 3, then home (2)
    for (int lv : {1, 3}) { L.Make(lv); step(lv); }
    step(2);
    const int madeAfterWalk = madeTex;
    CHECK(madeAfterWalk == 3 + 6, "walk made 2 rungs x 3 slots of new textures: %d", madeAfterWalk);
    if (!mutant) CHECK(st.Count() == 6, "six dormant textures after the walk: %zu", st.Count());
    // play: 500 steps among the rungs
    unsigned seed = 12345;
    for (int i = 0; i < 500; ++i) {
        seed = seed * 1664525u + 1013904223u;
        const int to = 1 + static_cast<int>((seed >> 16) % 3);
        const int before = madeTex, delBefore = deletedTex;
        step(to);
        L.Step(madeTex != before);
        if (deletedTex != delBefore) L.Free(to);   // a texture deleted in the match
    }
    CHECK(madeTex == madeAfterWalk, "no texture made by a step in play: %d -> %d", madeAfterWalk, madeTex);
    CHECK(deletedTex == 0, "no texture deleted by a step in play: %d", deletedTex);
    CHECK(L.stepsMaking == 0 && L.midFrees == 0 && L.remakes == 0, "ledger: stepsMaking %u midFrees %u remakes %u", L.stepsMaking, L.midFrees, L.remakes);
    CHECK(!L.Make(1) && L.remakes == 1, "a second make of a rung is counted as a remake");
    // the match end frees everything once
    std::vector<uint32_t> gone;
    st.DropAll(gone);
    const int freed = L.EndMatch();
    CHECK(freed == 2 && L.endFrees == 2 && !L.inMatch, "match end frees the 2 walked rungs: %d", freed);
    CHECK(gone.size() == 3 * 2 || mutant, "the dormant textures come back: %zu", gone.size());
    CHECK(st.Count() == 0 && st.Kib() == 0, "store empty");
    L.Free(1);
    CHECK(L.midFrees == 0, "a free after the match end is not a mid-match free");
    // a free in a match IS counted
    L.BeginMatch(); L.Make(2); L.Free(2);
    CHECK(L.midFrees == 1, "mid-match free counted");
    std::printf(fails ? "rungpremake: %d FAILED\n" : "rungpremake: all passed\n", fails);
    return fails ? 1 : 0;
}
