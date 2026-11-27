// Host test of the alpha-test sinking plan (vgx_atlate.h).
#include "vgx_atlate.h"
#include <cstdio>
#include <cstdlib>
using namespace vgx::atlate;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); std::exit(1); } } while (0)
static bool Perm(const std::vector<uint32_t>& o, size_t n) {
    std::vector<int> seen(n, 0);
    if (o.size() != n) return false;
    for (uint32_t v : o) { if (v >= n || seen[v]++) return false; }
    return true;
}
int main() {
    std::vector<uint32_t> o;
    // O L O O L O -> O O O L L ... (all in one window): late draws go to the end, in their own order, the rest keep theirs
    std::vector<uint8_t> k = {kOpaque, kLate, kOpaque, kOpaque, kLate, kOpaque};
    CHECK(Plan(k, o) == 2 && Perm(o, k.size()));
    const uint32_t want[] = {0, 2, 3, 5, 1, 4};
    for (size_t i = 0; i < 6; ++i) CHECK(o[i] == want[i]);
    // a barrier ends the window: the late draw stays before it
    k = {kLate, kOpaque, kBarrier, kOpaque, kLate};
    CHECK(Plan(k, o) == 1 && Perm(o, k.size()));
    const uint32_t want2[] = {1, 0, 2, 3, 4};
    for (size_t i = 0; i < 5; ++i) CHECK(o[i] == want2[i]);
    // nothing to do: identity, no moves
    k = {kOpaque, kNeutral, kOpaque, kBarrier, kLate};
    CHECK(Plan(k, o) == 0 && o[4] == 4 && Perm(o, k.size()));
    // a late draw never passes a barrier, never crosses a neutral command it follows
    k = {kLate, kNeutral, kLate, kBarrier};
    CHECK(Plan(k, o) == 1 && Perm(o, k.size()) && o[0] == 1 && o[1] == 0 && o[2] == 2 && o[3] == 3);
    // empty
    k.clear();
    CHECK(Plan(k, o) == 0 && o.empty());
    std::printf("atlate: ok\n");
    return 0;
}
