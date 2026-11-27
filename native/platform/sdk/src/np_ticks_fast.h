// np_ticks_fast.h (1.3 cpuexact13, MSC_TICKS_FAST): np_os_ticks_from_ns without a 64-bit divide. Pure C so a host test can run it against the original.
//
// The original is  (ns / 1e9) * clock + (ns % 1e9) * clock / 1e9  with clock = OS_TIME_SPEED = OS_BUS_CLOCK_SPEED / 4. On the Vita that is two __aeabi_uldivmod calls (software
// division, no divide instruction on the Cortex-A9) every OSGetTick / OSGetTime, ~115 a picture. With sec = ns / 1e9 and r = ns % 1e9 the sum is exactly floor(ns * clock / 1e9),
// because sec * clock is an integer. For the bus clock the game uses (243,000,000 Hz, clock 60,750,000) clock / 1e9 = 243 / 4000, so
//     ticks = floor(ns * 243 / 4000) = floor( floor(ns * 243 / 32) / 125 )      (4000 = 32 * 125; floor(floor(a/m)/n) = floor(a/(m*n)))
// ns * 243 fits 64 bits for ns < 2^50 ns (13 days of run time, the np_time_ns clock since start), and y = ns*243 >> 5 < 2^53 is divided by 125 in three 32-bit steps
// (a 21-bit high word, then two 16-bit digits: each step's dividend is below 125 * 65536, a 32-bit constant divide the compiler turns into a multiply). Anything else (another clock
// value, ns >= 2^50) takes the original code.
#pragma once
#include <stdint.h>

#define NP_TICKS_FAST_CLOCK 60750000ull
#define NP_TICKS_FAST_NS_LIMIT (1ull << 50)

static inline uint64_t np_ticks_from_ns_ref(uint64_t ns, uint64_t clock) {
    return (ns / 1000000000ull) * clock + (ns % 1000000000ull) * clock / 1000000000ull;
}

// requires ns < NP_TICKS_FAST_NS_LIMIT
static inline uint64_t np_ticks_from_ns_243(uint64_t ns) {
#ifdef NP_TICKS_FAST_BREAK   // (test mutant: one digit step wrong)
    const uint64_t y = (ns * 243ull) >> 4;
#else
    const uint64_t y = (ns * 243ull) >> 5;
#endif
    const uint32_t h = (uint32_t)(y >> 32), l = (uint32_t)y;
    const uint32_t qh = h / 125u, rh = h - qh * 125u;
    const uint32_t t1 = (rh << 16) | (l >> 16);
    const uint32_t q1 = t1 / 125u, r1 = t1 - q1 * 125u;
    const uint32_t t2 = (r1 << 16) | (l & 0xFFFFu);
    const uint32_t q2 = t2 / 125u;
    return ((uint64_t)qh << 32) | ((uint64_t)q1 << 16) | q2;
}
