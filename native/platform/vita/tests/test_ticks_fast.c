/* Host test of np_ticks_fast.h (MSC_TICKS_FAST): np_ticks_from_ns_243 against the original formula and against exact 128-bit floor(ns*clock/1e9), over random, log-uniform, boundary and
 * special values (every ns at which the answer steps, +-2). Mutant: -DNP_TICKS_FAST_BREAK. */
#include <stdint.h>
#include <stdio.h>
#include "../../sdk/src/np_ticks_fast.h"
static uint64_t s = 0x9E3779B97F4A7C15ull;
static uint64_t Rnd(void) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
static long checks, bad;
static void Check(uint64_t ns) {
    const unsigned __int128 exact = ((unsigned __int128)ns * NP_TICKS_FAST_CLOCK) / 1000000000ull;
    const uint64_t ref = np_ticks_from_ns_ref(ns, NP_TICKS_FAST_CLOCK);
    ++checks;
    if (ref != (uint64_t)exact) { ++bad; if (bad < 5) printf("ref != exact at %llu\n", (unsigned long long)ns); }
    if (ns < NP_TICKS_FAST_NS_LIMIT) {
        const uint64_t f = np_ticks_from_ns_243(ns);
        if (f != ref) { ++bad; if (bad < 5) printf("fast %llu != ref %llu at ns %llu\n", (unsigned long long)f, (unsigned long long)ref, (unsigned long long)ns); }
    }
}
int main(void) {
    for (uint64_t i = 0; i < 70000; ++i) Check(i);                                  /* the first 70 us: every value */
    for (long i = 0; i < 400000000; ++i) {                                          /* random below the limit (4e8) */
        const uint64_t r = Rnd(); const int bits = 1 + (int)(Rnd() % 50);
        Check((r & ((1ull << bits) - 1)));
    }
    for (uint64_t j = 0; j < 6000000; ++j) {                                        /* the steps: ns where floor(ns*243/4000) changes, and its neighbours */
        const uint64_t base = (j * 4000 + 242) / 243;
        for (int d = -2; d <= 2; ++d) if ((int64_t)base + d >= 0) Check(base + d);
    }
    for (int b = 0; b < 64; ++b) for (int d = -3; d <= 3; ++d) Check((1ull << b) + d);   /* powers of two, 2^50 edge and beyond (the limit is respected inside Check) */
    for (int k = 0; k < 8; ++k) { Check(0xFFFFFFFFFFFFFFFFull - k); Check(NP_TICKS_FAST_NS_LIMIT - 1 - k); Check(NP_TICKS_FAST_NS_LIMIT + k); }
    for (uint64_t sec = 0; sec < 2000000; sec += 997) for (int d = -2; d <= 2; ++d) Check(sec * 1000000000ull + 999999999ull + d);   /* second boundaries */
    printf("ticks_fast: %ld checks, %ld mismatches\n", checks, bad);
    return bad ? 1 : 0;
}
