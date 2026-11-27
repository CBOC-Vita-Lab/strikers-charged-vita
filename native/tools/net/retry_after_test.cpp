// retryAfterSec clamps (np_service_busy.h): a hostile or broken answer cannot reach undefined behaviour or a wrapped wait.
// g++ -std=c++17 -fsanitize=undefined -I. tools/net/retry_after_test.cpp -o /tmp/retry_after_test && /tmp/retry_after_test
#include "platform/sdk/net/online/np_service_busy.h"
#include <cmath>
#include <cstdio>
int main() {
    int bad = 0;
    auto eq = [&](long a, long b, const char* w) { if (a != b) { std::printf("FAIL %s: %ld != %ld\n", w, a, b); ++bad; } };
    using namespace NpNet;
    eq(ClampRetryAfterSec(0), 0, "zero");
    eq(ClampRetryAfterSec(-5), 0, "negative");
    eq(ClampRetryAfterSec(std::nan("")), 0, "nan");
    eq(ClampRetryAfterSec(7.9), 7, "fraction");
    eq(ClampRetryAfterSec(1e30), 86400, "1e30");
    eq(ClampRetryAfterSec(INFINITY), 86400, "inf");
    eq(ClampRetryAfterSec(5e6), 86400, "5e6 (would wrap uint32 * 1000)");
    eq(long(RetryAfterMs(0)), 0, "ms 0");
    eq(long(RetryAfterMs(-1)), 0, "ms negative");
    eq(long(RetryAfterMs(12)), 12000, "ms 12");
    eq(long(RetryAfterMs(60)), 60000, "ms 60");
    eq(long(RetryAfterMs(3600)), 60000, "ms 3600 clamps to a minute");
    eq(long(RetryAfterMs(2000000000)), 60000, "ms huge");
    // PollFailWaitMs: legacy equals the old expression for every streak; the new rule keeps streaks 1..4, reaches 60 s, stays inside +-12.5 percent, never wraps
    for (int st = 0; st < 40; ++st) {
        const uint32_t old = st > 4 ? 15000u : 1000u << (st < 4 ? st : 4);
        eq(long(PollFailWaitMs(st, 123456, true)), long(old), "legacy equals the old rule");
    }
    for (uint64_t seed = 1; seed < 2000; seed += 7) {
        for (int st = 1; st <= 4; ++st) {
            const uint32_t base = 1000u << st;
            const uint32_t w = PollFailWaitMs(st, seed);
            if (st < 3) eq(long(w), long(base), "streaks 1 and 2 are exact");
            else if (w < base - base / 8 || w > base + base / 8) { std::printf("FAIL jitter range st %d seed %llu w %u\n", st, (unsigned long long)seed, w); ++bad; }
        }
        for (int st = 5; st < 100; ++st) {
            const uint32_t b = st == 5 ? 15000u : st == 6 ? 30000u : 60000u, w = PollFailWaitMs(st, seed);
            if (w < b - b / 8 || w > b + b / 8) { std::printf("FAIL range st %d seed %llu w %u\n", st, (unsigned long long)seed, w); ++bad; }
        }
    }
    { bool varies = false; for (uint64_t sd = 1; sd < 50; ++sd) if (PollFailWaitMs(8, sd) != PollFailWaitMs(8, 1)) varies = true; if (!varies) { std::printf("FAIL no jitter\n"); ++bad; } }
    std::printf(bad ? "retry_after_test: FAILED\n" : "retry_after_test: PASS\n");
    return bad ? 1 : 0;
}
