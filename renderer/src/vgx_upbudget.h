// vgx_upbudget.h (wave 9, MSC_DYNRES_PACED_UP_MS): the GPU budget the dynamic-resolution ladder uses to step UP (to a larger picture or a multisampled one) while the frame rate is free-running
// (NpFpsDisplayInterval 0: MSC_FPS_BETWEEN=pace, or unlocked). Pure code, so a host test runs it (tools/fps/pace_mid_model.cpp).
// The ladder judged every step by the 30 budget (33.4 ms) when no interval was set, so a free run that had 18 ms of GPU time (55 pictures a second) was given the headroom up to 80 % of 33.4 ms
// (26.7 ms): a larger picture or 2x anti-aliasing until the run gave 38-45 pictures a second with a 1/2 refresh mix, which is what the gate's limits then had to give up on. With a cap the ladder
// does not buy quality with a free run's pictures: the ladder's steps (up and down, one hysteresis) are judged by min(budget, cap) while the run is free, so a scene that the gate let in at a GPU of 20 ms is brought back under the cap (AA rung off, picture smaller) instead of being left at 25 ms, where the gate's way out would end the free run. 0 = off (the budget as before).
#pragma once

#ifndef VGX_UPBUDGET_BREAK
#define VGX_UPBUDGET_BREAK 0   // host test mutants: 1 the cap also applies to a held 30 (interval 2), 2 the cap raises the budget instead of lowering it
#endif

namespace vgx::pacedup {

inline double UpBudgetMs(int interval, double budgetMs, double capMs) {
#if VGX_UPBUDGET_BREAK == 1
    const bool free = true;
#else
    const bool free = interval == 0;
#endif
    if (!free || capMs <= 0.0) return budgetMs;
#if VGX_UPBUDGET_BREAK == 2
    return capMs > budgetMs ? capMs : budgetMs;
#else
    return capMs < budgetMs ? capMs : budgetMs;
#endif
}

} // namespace vgx::pacedup
