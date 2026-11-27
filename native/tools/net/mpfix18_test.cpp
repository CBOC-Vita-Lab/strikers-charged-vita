// np_mpfix18.h: the sceNet keep-stack switch and the stranger-line limiter.
// g++ -std=c++11 -I. tools/net/mpfix18_test.cpp -o /tmp/mpfix18_test && /tmp/mpfix18_test
#include "platform/sdk/net/np_mpfix18.h"
#include <cstdio>
int main() {
    int bad = 0;
    auto eq = [&](long a, long b, const char* w) { if (a != b) { std::printf("FAIL %s: %ld != %ld\n", w, a, b); ++bad; } };
    using namespace NpMpfix18;
    // keep-stack: on by default, only an exact "0" turns it off
    eq(ParseKeepStack(nullptr), 1, "unset on"); eq(ParseKeepStack(""), 1, "empty on"); eq(ParseKeepStack("1"), 1, "1 on"); eq(ParseKeepStack("0"), 0, "0 off");
    eq(ParseKeepStack("00"), 1, "00 on"); eq(ParseKeepStack("off"), 1, "text on");
    // limiter: 8 at once, then 2 a second
    LogBucket b = {0, 0, false};
    int n = 0;
    for (int i = 0; i < 1000; ++i) n += LogAllow(b, 5000);
    eq(n, 8, "burst of 8 at one instant");
    n = 0; for (int i = 0; i < 100; ++i) n += LogAllow(b, 5499); eq(n, 0, "nothing before 500 ms");
    n = 0; for (int i = 0; i < 100; ++i) n += LogAllow(b, 5500); eq(n, 1, "one token after 500 ms");
    n = 0; for (int i = 0; i < 100; ++i) n += LogAllow(b, 7500); eq(n, 4, "four tokens after 2 s more");
    n = 0; for (int i = 0; i < 100; ++i) n += LogAllow(b, 600000); eq(n, 8, "refill is capped at the burst");
    // a steady 100 lines a second for 10 s: 8 + about 2 a second, not 1000
    LogBucket s = {0, 0, false};
    n = 0; for (uint64_t t = 0; t < 10000; t += 10) n += LogAllow(s, 100000 + t);
    eq(n >= 24 && n <= 30, 1, "steady flood stays near 2 a second");
    // a clock that steps back does not wedge the bucket
    LogBucket c = {0, 0, false};
    for (int i = 0; i < 20; ++i) LogAllow(c, 900000);
    n = 0; for (int i = 0; i < 20; ++i) n += LogAllow(c, 1000); eq(n, 0, "after a step back no refill yet");
    n = 0; for (int i = 0; i < 20; ++i) n += LogAllow(c, 1600); eq(n, 1, "re-anchored: refills from the new time");
    std::printf("mpfix18_test: %s\n", bad ? "FAILED" : "ok");
    return bad ? 1 : 0;
}
