// np_wait_banner.h (1.3): when the "Waiting for the other player..." line appears and goes, and that it is bookkeeping only.
//   c++ -std=c++11 -Wall -Wextra -I<native> native/platform/sdk/tests/test_wait_banner.cpp -o /tmp/test_wait_banner && /tmp/test_wait_banner
#include "platform/sdk/net/np_wait_banner.h"

#include <cstdio>

static int g_checks = 0, g_failed = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_failed; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

int main() {
    using namespace NpWait;
    StarveClock c;
    // a healthy match: no starved steps, nothing shown
    for (uint32_t t = 1000; t < 3000; t += 20) c.Step(false, t);
    CHECK(c.Ms(3000) == 0);
    CHECK(Alpha(c.Ms(3000)) == 0);
    // starvation begins at t = 5000, a step every 20 ms
    uint32_t t = 5000;
    for (; t < 5000 + 1480; t += 20) c.Step(true, t);
    CHECK(c.Ms(t) >= 1440 && c.Ms(t) < kShowAfterMs);
    CHECK(Alpha(c.Ms(t)) == 0);   // not yet
    for (; t < 5000 + 1700; t += 20) c.Step(true, t);
    CHECK(Alpha(c.Ms(t)) > 0 && Alpha(c.Ms(t)) < 255);   // fading in
    for (; t < 5000 + 4300; t += 20) c.Step(true, t);
    CHECK(Alpha(c.Ms(t)) == 255);
    CHECK(Seconds(c.Ms(t)) == 4 || Seconds(c.Ms(t)) == 3);
    // one good step clears it at once
    c.Step(false, t);
    CHECK(c.Ms(t + 1) == 0);
    // a short starve (a bad packet) never shows
    for (uint32_t u = t; u < t + 600; u += 20) c.Step(true, u);
    CHECK(Alpha(c.Ms(t + 600)) == 0);
    c.Step(false, t + 620);
    CHECK(c.Ms(t + 640) == 0);
    // this console's own stall (no steps at all for 10 s) is not "waiting for the other player": the banner stops, and the next run starts anew
    StarveClock d;
    for (uint32_t u = 100; u < 100 + 2000; u += 20) d.Step(true, u);
    CHECK(Alpha(d.Ms(2100)) == 255);
    CHECK(d.Ms(12100) == 0);   // stale: no starved step for 10 s
    d.Step(true, 12100);
    CHECK(d.Ms(12100) == 0 && Alpha(d.Ms(12120)) == 0);   // restarted at 12100
    // the clock wraps (49 days): differences stay right
    StarveClock w;
    const uint32_t base = 0xFFFFFF00u;
    for (uint32_t u = 0; u < 2000; u += 20) w.Step(true, base + u);
    CHECK(w.Ms(base + 2000) >= 1960 && Alpha(w.Ms(base + 2000)) == 255);
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
