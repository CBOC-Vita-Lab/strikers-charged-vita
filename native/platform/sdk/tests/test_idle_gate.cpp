// np_idle_gate.h: an idle caller (no link up) does its system call once in `period` pictures, not every picture.
//   c++ -std=c++11 -Wall -Wextra -I<native> native/platform/sdk/tests/test_idle_gate.cpp -o /tmp/test_idle_gate && /tmp/test_idle_gate
#include "platform/sdk/net/np_idle_gate.h"

#include <cstdio>

static int g_checks = 0, g_failed = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_failed; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

int main() {
    NpIdle::Every g;
    unsigned calls = 0;
    for (int i = 0; i < 6400; ++i) calls += g.Due(NpIdle::kIdlePeriodFrames) ? 1u : 0u;   // 6400 idle pictures (about 107 s at 60)
    CHECK(calls == 100);                      // not 6400
    CHECK(!g.Due(NpIdle::kIdlePeriodFrames)); // the first call after a due one is not due
    NpIdle::Every h;
    for (unsigned i = 0; i + 1 < NpIdle::kIdlePeriodFrames; ++i) CHECK(!h.Due(NpIdle::kIdlePeriodFrames));
    CHECK(h.Due(NpIdle::kIdlePeriodFrames));  // the 64th
    h.Reset();
    CHECK(!h.Due(NpIdle::kIdlePeriodFrames));
    CHECK(NpIdle::Every().Due(1));            // period 1 = every call (the old behaviour: the negative control of this test)
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
