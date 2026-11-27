// nxfly25 (25 Nov 2026): MSC_VGX_DLWELD_BUDGET_US / MSC_VGX_DLWELD_LISTS, a per-picture limit on the welding of display lists (vgx_dlcache.cpp Promote).
// A list is welded at its promotion (its first repeat call). The first pictures of a stadium's fly-in promote every list the stadium has at once (6121: 257 lists, 272.9 ms, in the
// walk-out's first pictures; 6033: 346 ms): one long picture in a held scene. With a budget the picture stops starting promotions once it has spent `budgetUs` microseconds welding
// or promoted `maxLists` lists; the lists left over are drawn from the per-picture arena for one more picture (exactly as a list that was not promoted yet is) and promoted at one
// of the next pictures. Each list still gets the same welded buffer in the end: only the picture it happens in moves. 0 = no limit.
#pragma once
#include <cstdint>
#ifndef VGX_DLWELDBUDGET_BREAK
#define VGX_DLWELDBUDGET_BREAK 0   // host test mutants (renderer/tests/dlweld_budget.cpp): 1 the time limit is ignored, 2 the list cap is ignored, 3 a new picture does not start from zero, 4 the time limit lets one more through (> for >=), 5 a weld is not counted, 6 a small list's promotion (MSC_VGX_PROMOSMALL) ignores the budget, 7 a solo list's (MSC_VGX_PROMOSOLO) does
#endif
namespace vgx {
namespace dlweldbudget {
struct Budget {
    uint32_t budgetUs = 0, maxLists = 0;   // limits, 0 = none
    uint32_t usedUs = 0, lists = 0;        // this picture
    uint64_t picture = ~0ull;
    uint64_t deferred = 0;                 // promotions put off, in all
    uint32_t deferredPic = 0;              // ... in this picture
    // The picture the next call belongs to; false when it is a new one (the caller reports the old one first).
    bool NewPicture(uint64_t p) {
        if (p == picture) return false;
        picture = p;
#if VGX_DLWELDBUDGET_BREAK != 3
        usedUs = 0;
        lists = 0;
#endif
        deferredPic = 0;
        return true;
    }
    // May another promotion (a weld) start in this picture?
    bool Allow() const {
#if VGX_DLWELDBUDGET_BREAK == 1
        const bool timeOk = true;
#elif VGX_DLWELDBUDGET_BREAK == 4
        const bool timeOk = budgetUs == 0 || usedUs <= budgetUs;
#else
        const bool timeOk = budgetUs == 0 || usedUs < budgetUs;
#endif
#if VGX_DLWELDBUDGET_BREAK == 2
        const bool listOk = true;
#else
        const bool listOk = maxLists == 0 || lists < maxLists;
#endif
        return timeOk && listOk;
    }
    void Deferred() { ++deferred; ++deferredPic; }
    void Spent(uint32_t us) {
#if VGX_DLWELDBUDGET_BREAK != 5
        usedUs += us;
#endif
        ++lists;
    }
};
// nxr6a (NEON r6 #7): which promotion a repeated list gets, in ONE place so a test can drive it. The same three paths as vgx_dlcache.cpp had inline (a list too large for the CPU merge path; a small one after
// `promoSmall` calm replays; a solo one after `promoSolo` replays that merged with nothing, at most `soloLeft` a picture), but EVERY path that can weld now asks `mayPromote` (the budget): the small and solo
// paths ran unbudgeted before, welding at 3 ms of budget or not (MSC_VGX_PROMOSMALL=3 is the Vita plan's). `mayPromote` is called once, only for the path the list is on (it counts a deferral).
// A deferred list is drawn from the arena once more and asked again at a later picture: the same welded buffer in the end (host: the [dlweld] "buffer sum" is equal with and without a budget).
enum class Route { kNone, kPromote, kPromoteSolo };
template <class May>
inline Route ChoosePromotion(bool promotable, bool smallOk, uint32_t calm, uint32_t solo, uint32_t promoSmall, uint32_t promoSolo, uint32_t soloLeft, May mayPromote) {
    if (promotable) return mayPromote() ? Route::kPromote : Route::kNone;
    if (promoSmall && smallOk && calm >= promoSmall) {
#if VGX_DLWELDBUDGET_BREAK == 6
        return Route::kPromote;
#else
        return mayPromote() ? Route::kPromote : Route::kNone;
#endif
    }
    if (promoSolo && soloLeft && smallOk && solo >= promoSolo) {
#if VGX_DLWELDBUDGET_BREAK == 7
        return Route::kPromoteSolo;
#else
        return mayPromote() ? Route::kPromoteSolo : Route::kNone;
#endif
    }
    return Route::kNone;
}
}  // namespace dlweldbudget
}  // namespace vgx
