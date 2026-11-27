#pragma once
// gxvita14 (wave 14, after Galaxy even26.h EvenMeter): the smoothness score of one [present] window, 0..100, higher is better. Pure: no clock, no globals, no platform includes
// (tests/smooth.cpp runs this code; tools/console-1.2/abpair.py reads the figure from the log line and has its own copy of the formula for lines that lack it).
//
//   score = clamp(ideal% - 25 * judder - 5 * missed per second, 0, 100)
//
//  ideal%  the share of pictures whose hold (vblanks on screen) is one of the two whole numbers around the window's mean hold (the cadence the window had: 2.0 is 2 only, 1.5 is 1 or 2), at most 3.
//  judder  mean |hold - game step| in vblanks a picture (PresentSync errSum / pairs): 0 when the picture stays on screen as long as the game time it shows advanced.
//  missed  a picture held 3+ vblanks or a game jump of 3+ turns: 3+ holds plus 3+ steps, at most the pictures.
//
// ESTIMATE of Galaxy's EvenMeter score, which has the joint table hold x step: here only the two marginals (PresentSync keeps no joint), so ideal% is an upper bound (hold == step and the
// two-long-holds-in-a-row clumps are not subtracted) and missed is an upper bound (a picture with both is counted twice, then capped). Same window, same formula: ON and OFF
// compare like with like; the absolute number is not Galaxy's.
#include <cmath>
#include <cstdint>
#include <cstdio>

#ifndef VGX_SMOOTH_BREAK
#define VGX_SMOOTH_BREAK 0   // test mutants: 1 judder weighs 2.5, 2 hold 0 counts as a picture, 3 only the lower cadence hold is ideal
#endif

namespace vgx::smooth {

struct Mix {
    uint64_t hold[5] = {};   // pictures shown 0 / 1 / 2 / 3 / 4+ vblanks (0: two in one vblank, not counted)
    uint64_t step[5] = {};   // game steps 1 / 2 / 3 / 4+ vblanks at index 1..4 (index 0 unused)
    double judder = 0;       // mean |hold - step| over stamped pairs
    double secs = 0;
};
struct Score {
    uint64_t pictures = 0;
    double mean = 0, idealPct = 100, judder = 0, missedPerSec = 0, score = 100;
    unsigned lo = 1, hi = 1;
    uint64_t missed = 0;
};

inline Score Compute(const Mix& m) {
    Score r;
    uint64_t n = 0, sum = 0;
    for (unsigned h = (VGX_SMOOTH_BREAK == 2 ? 0u : 1u); h <= 4; ++h) { n += m.hold[h]; sum += h * m.hold[h]; }
    r.pictures = n;
    if (!n || m.secs <= 0.0) return r;
    r.mean = static_cast<double>(sum) / static_cast<double>(n);
    r.lo = static_cast<unsigned>(std::floor(r.mean + 1e-9));
    r.hi = static_cast<unsigned>(std::ceil(r.mean - 1e-9));
    if (r.lo < 1) r.lo = 1;
    if (r.hi < r.lo) r.hi = r.lo;
    uint64_t good = 0;
    for (unsigned h = r.lo; h <= (VGX_SMOOTH_BREAK == 3 ? r.lo : r.hi) && h <= 3; ++h) good += m.hold[h];
    r.idealPct = 100.0 * static_cast<double>(good) / static_cast<double>(n);
    r.judder = m.judder;
    uint64_t miss = m.hold[3] + m.hold[4] + m.step[3] + m.step[4];
    if (miss > n) miss = n;
    r.missed = miss;
    r.missedPerSec = static_cast<double>(miss) / m.secs;
    const double sc = r.idealPct - (VGX_SMOOTH_BREAK == 1 ? 2.5 : 25.0) * r.judder - 5.0 * r.missedPerSec;
    r.score = sc < 0.0 ? 0.0 : sc > 100.0 ? 100.0 : sc;
    return r;
}

// "smooth 87/100 (ideal 95 %, judder 0.08, missed 0.50/s)": the token abpair.py reads.
inline int Format(char* out, size_t n, const Score& s) {
    return std::snprintf(out, n, "smooth %.0f/100 (ideal %.0f %%, judder %.2f, missed %.2f/s)", s.score, s.idealPct, s.judder, s.missedPerSec);
}

}  // namespace vgx::smooth
