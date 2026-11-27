// MSC_DYNRES_SEVERE: when the deeper floor opens and closes (vgx_dynres_severe.h).
#include "../src/vgx_dynres_severe.h"
#include <cstdio>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAILED line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

static vgx::SevereGuard Make() { vgx::SevereGuard g; g.on = true; return g; }
constexpr float kBudget = 33.37f;

int main() {
    {   // off: never
        vgx::SevereGuard g;
        for (int i = 0; i < 1000; ++i) CHECK(g.Frame(100, 100, kBudget, 100, false) == 0);
        CHECK(!g.open);
    }
    {   // exactly at the line: opens after 3 s of measured time, not before
        auto g = Make();
        const float ms = 1.8f * kBudget;
        int n = 0, r = 0;
        while (!(r = g.Frame(ms, ms, kBudget, 90, false)) && n < 1000) ++n;
        CHECK(r == 1 && g.open);
        CHECK((n + 1) * ms >= 3000.0f && n * ms < 3000.0f);
    }
    {   // just under the line: never
        auto g = Make();
        const float ms = 1.79f * kBudget;
        for (int i = 0; i < 2000; ++i) CHECK(g.Frame(ms, ms, kBudget, 90, false) == 0);
    }
    {   // a dip of the mean starts the count again: a stretch of 2.9 s, a dip, 2.9 s does not open
        auto g = Make();
        const float ms = 60.1f;
        for (int k = 0; k < 5; ++k) {
            for (int i = 0; i < 48; ++i) CHECK(g.Frame(ms, ms, kBudget, 90, false) == 0);
            CHECK(g.Frame(40, 20, kBudget, 90, false) == 0);
        }
        CHECK(!g.open);
    }
    {   // a NaN mean does not count
        auto g = Make();
        const float nan = __builtin_nanf("");
        for (int i = 0; i < 1000; ++i) CHECK(g.Frame(nan, 100, kBudget, 90, false) == 0);
    }
    {   // a Mega Strike (blocked) neither counts nor keeps the floor open
        auto g = Make();
        for (int i = 0; i < 1000; ++i) CHECK(g.Frame(100, 100, kBudget, 90, true) == 0);
        for (int i = 0; i < 29; ++i) g.Frame(100, 100, kBudget, 90, false);
        CHECK(!g.open);
        g.Frame(100, 100, kBudget, 90, true);   // (the count is gone)
        for (int i = 0; i < 29; ++i) g.Frame(100, 100, kBudget, 90, false);
        CHECK(!g.open);
        g.Frame(100, 100, kBudget, 90, false);
        CHECK(g.open);
        CHECK(g.Frame(100, 100, kBudget, 90, true) == -1 && !g.open);
    }
    {   // closes only after 180 frames in a row predicted to fit at the ordinary floor
        auto g = Make();
        while (!g.open) g.Frame(100, 100, kBudget, 90, false);
        const float fits = 0.8f * kBudget, not_ = 0.9f * kBudget;
        for (int i = 0; i < 179; ++i) CHECK(g.Frame(30, 30, kBudget, fits, false) == 0);
        CHECK(g.Frame(30, 30, kBudget, not_, false) == 0);
        for (int i = 0; i < 179; ++i) CHECK(g.Frame(30, 30, kBudget, fits, false) == 0);
        CHECK(g.open);
        CHECK(g.Frame(30, 30, kBudget, fits, false) == -1 && !g.open);
        // and has to be earned again
        CHECK(g.Frame(100, 100, kBudget, 90, false) == 0);
    }
    if (fails) return 1;
    std::printf("dynres severe test: passed\n");
    return 0;
}
