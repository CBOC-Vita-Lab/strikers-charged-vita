// Host test of the weld budget policy (vgx_dlweldbudget.h, nxfly25), and of the claim "the same buffers in the end": a queue of lists welded under a budget is the set of lists
// welded without one, in the same order, only spread over more pictures.
#include "../src/vgx_dlweldbudget.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace vgx::dlweldbudget;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); std::exit(1); } } while (0)
// pictures needed to promote `costs` (microseconds each, one list per call, in order) under b; order[] gets the lists in the order they were welded
static int Run(Budget b, const std::vector<uint32_t>& costs, std::vector<int>& order, std::vector<uint32_t>& perPic) {
    std::vector<bool> done(costs.size(), false);
    size_t left = costs.size();
    int pic = 0;
    while (left && pic < 100000) {
        b.NewPicture(static_cast<uint64_t>(pic));
        uint32_t us = 0;
        for (size_t i = 0; i < costs.size(); ++i) {   // each picture calls every list not welded yet (a list that is not welded is called again next picture)
            if (done[i]) continue;
            if (!b.Allow()) { b.Deferred(); continue; }
            b.Spent(costs[i]);
            us += costs[i];
            done[i] = true; --left;
            order.push_back(static_cast<int>(i));
        }
        perPic.push_back(us);
        ++pic;
    }
    return pic;
}
int main() {
    std::vector<uint32_t> costs(257, 1000);   // 6121: 257 lists, about a millisecond each
    std::vector<int> o0, o1, o2, o3; std::vector<uint32_t> p0, p1, p2, p3;
    Budget none;
    CHECK(Run(none, costs, o0, p0) == 1 && p0[0] == 257000);   // no budget: one picture pays everything (the burst)
    Budget t; t.budgetUs = 2000;
    const int n1 = Run(t, costs, o1, p1);
    CHECK(n1 == 129);                          // 2 ms a picture: 2 lists, the budget is met after the second (usedUs 2000 >= 2000); 257 lists in 129 pictures
    for (uint32_t u : p1) CHECK(u <= 2000);
    CHECK(o1 == o0);                           // the same lists, the same order, in the end
    Budget l; l.maxLists = 8;
    const int n2 = Run(l, costs, o2, p2);
    CHECK(n2 == 33 && o2 == o0);               // ceil(257 / 8)
    // both limits: the tighter one wins
    Budget both; both.budgetUs = 5000; both.maxLists = 3;
    const int n3 = Run(both, costs, o3, p3);
    CHECK(n3 == 86 && o3 == o0);
    // one long list overshoots by itself, never starves: a list dearer than the budget is welded alone in its picture
    std::vector<uint32_t> big = {9000, 500, 500}; std::vector<int> ob; std::vector<uint32_t> pb;
    Budget bb; bb.budgetUs = 2000;
    CHECK(Run(bb, big, ob, pb) == 2 && pb[0] == 9000 && pb[1] == 1000);
    // a new picture starts from zero
    Budget z; z.budgetUs = 1000; z.NewPicture(5); z.Spent(1000); CHECK(!z.Allow()); CHECK(z.NewPicture(6) && z.Allow()); CHECK(!z.NewPicture(6));
    // nxr6a (NEON r6 #7): the promotion routes. Every path that can weld asks the budget: a spent budget defers a promotable, a small and a solo list alike (the small / solo paths ran unbudgeted before)
    {
        Budget sp; sp.budgetUs = 3000; sp.NewPicture(1); sp.Spent(3000);   // this picture has welded its 3 ms
        auto ask = [&](Budget& b, bool promotable, bool smallOk, uint32_t calm, uint32_t solo, uint32_t soloLeft) {
            return ChoosePromotion(promotable, smallOk, calm, solo, 3, 2, soloLeft, [&] { if (b.Allow()) return true; b.Deferred(); return false; });
        };
        CHECK(ask(sp, true, false, 0, 0, 8) == Route::kNone);
        CHECK(ask(sp, false, true, 3, 0, 8) == Route::kNone);      // small, 3 calm replays: put off
        CHECK(ask(sp, false, true, 0, 2, 8) == Route::kNone);      // solo, 2 lone replays: put off
        CHECK(sp.deferredPic == 3);
        Budget ok; ok.budgetUs = 3000; ok.NewPicture(1);
        CHECK(ask(ok, true, false, 0, 0, 8) == Route::kPromote);
        CHECK(ask(ok, false, true, 3, 0, 8) == Route::kPromote);
        CHECK(ask(ok, false, true, 0, 2, 8) == Route::kPromoteSolo);
        CHECK(ask(ok, false, true, 0, 2, 0) == Route::kNone);      // no solo ration left this picture
        CHECK(ask(ok, false, false, 9, 9, 8) == Route::kNone);     // not small enough
        CHECK(ask(ok, false, true, 2, 1, 8) == Route::kNone);      // not calm / solo long enough
        CHECK(ok.deferredPic == 0);                                // a list that is on no path does not ask, so it is not counted as put off
        // the same lists in the end: 200 small lists of 1 ms with calm 3, one picture's budget 3 ms: all promoted, in the order they came, 3 or 4 a picture (the budget is met after the third: usedUs 3000 >= 3000)
        Budget q; q.budgetUs = 3000;
        std::vector<int> order; std::vector<bool> done(200, false); size_t left = 200; int pics = 0;
        while (left && pics < 1000) {
            q.NewPicture(static_cast<uint64_t>(pics)); int n = 0;
            for (int i = 0; i < 200; ++i) {
                if (done[i]) continue;
                if (ask(q, false, true, 3, 0, 8) == Route::kPromote) { q.Spent(1000); done[i] = true; --left; order.push_back(i); ++n; }
            }
            CHECK(n <= 3);
            ++pics;
        }
        CHECK(left == 0 && pics == 67 && order.size() == 200);
        for (size_t i = 1; i < order.size(); ++i) CHECK(order[i] > order[i - 1]);
    }
    std::printf("dlweld_budget: ok\n");
    return 0;
}
