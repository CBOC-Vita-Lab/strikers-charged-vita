// The real monotonic clock by a direct system call, for the platform layer's OWN bookkeeping
// on Linux (log flush timer, console refresh timer, frame statistics). Not used on the Vita.
//
// Why not clock_gettime / std::chrono: the test harness's virtual clock
// (plat_headless.cpp, MSC_VIRTUAL_TIME=1) interposes those and moves the virtual time on by
// one tick (1 us) for every clock read on the game thread. The headless builds read the clock
// only where the runtime does; the Vita platform layer also read it for every log write,
// console update and frame statistic, so in the armhf check build of the Vita configuration
// (Makefile.armcheck) the game's time base depended on how many lines had been logged and a
// match was not reproducible against the headless builds (found by the integration worker,
// wt/integ NOTES.md "FOR V", 5 Oct; its check_realclock.cpp is this function and was
// confirmed by a run). A raw system call is not interposed.
#pragma once
#if !defined(__vita__)
#include <cstdint>
#include <ctime>
#include <sys/syscall.h>
#include <unistd.h>

inline uint64_t PlatRealMonotonicUs() {
#if defined(__arm__)
    long ts[2] = {0, 0};                       // old_timespec32 of the 32-bit clock_gettime call
    syscall(SYS_clock_gettime, CLOCK_MONOTONIC, ts);
    return static_cast<uint64_t>(ts[0]) * 1000000u + static_cast<uint64_t>(ts[1]) / 1000u;
#else
    struct timespec ts{};
    syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000u + static_cast<uint64_t>(ts.tv_nsec) / 1000u;
#endif
}
#endif
