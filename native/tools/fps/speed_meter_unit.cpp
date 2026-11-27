// Host test of the game-speed meter (np_speed.h, MSC_SPEED_LOG): synthetic frame streams with a known speed. The mutants (NP_SPEED_BREAK 1..3) must FAIL.
#include "np_speed.h"
#include <cstdio>
#include <cmath>
using npfps::SpeedMeter;
static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { ++g_fail; std::printf("FAIL %s: ", #c); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

// `frames` pictures of `ms` wall each, `steps` fixed steps per picture on average (fractional carried).
struct Feed { SpeedMeter m; double t = 100.0; uint64_t steps = 0; double carry = 0; int windows = 0; double lastPct = 0;
    double scale = 1.0;
    void Run(int frames, double ms, double stepsPerPicture) {
        for (int i = 0; i < frames; ++i) {
            if (m.Frame(t, steps, scale)) { ++windows; lastPct = m.win().Percent(); }
            t += ms / 1000.0; carry += stepsPerPicture; const uint64_t n = (uint64_t)carry; steps += n; carry -= (double)n;
        }
    } };
int main() {
    {   // exact speed: 30 pictures a second, 1.6667 steps each = 50 steps a second
        Feed f; f.Run(1000, 1000.0 / 30.0, 50.0 / 30.0);
        CHECK(f.windows >= 6, "windows %d", f.windows);
        CHECK(std::fabs(f.lastPct - 100.0) < 0.6, "exact stream %.2f %%", f.lastPct);
        CHECK(f.m.underRule() == 0, "under %u", f.m.underRule());
    }
    {   // 95 % speed: 47.5 steps a second
        Feed f; f.Run(1000, 1000.0 / 30.0, 47.5 / 30.0);
        CHECK(std::fabs(f.lastPct - 95.0) < 0.6, "95 %% stream %.2f", f.lastPct);
        CHECK(f.m.underRule() == f.m.windows() && f.m.windows() > 0, "under %u of %u", f.m.underRule(), f.m.windows());
        CHECK(f.m.match().lostMs > 0, "lost %.1f", f.m.match().lostMs);
    }
    {   // 98.6 % is not under the rule; 97 % is
        Feed f; f.Run(800, 1000.0 / 30.0, 49.3 / 30.0);
        CHECK(f.m.underRule() <= 1, "98 %% stream under %u", f.m.underRule());
        Feed g; g.Run(800, 1000.0 / 30.0, 48.5 / 30.0);
        CHECK(g.m.underRule() >= g.m.windows() - 1 && g.m.windows() > 3, "97 %% stream under %u of %u", g.m.underRule(), g.m.windows());
    }
    {   // a 2 s pause (not live play) must not count: Break, then a long gap, then play at full speed
        Feed f; f.Run(400, 1000.0 / 30.0, 50.0 / 30.0);
        const bool rep = f.m.Break();
        (void)rep;
        f.t += 20.0; f.steps += 3;      // 20 s of replay with a few steps
        f.Run(400, 1000.0 / 30.0, 50.0 / 30.0);
        CHECK(std::fabs(f.m.match().Percent() - 100.0) < 0.8, "pause leaked into the match: %.2f %%", f.m.match().Percent());
        CHECK(f.m.match().worstPictureMs < 100.0, "worst %.1f", f.m.match().worstPictureMs);
    }
    {   // a hitch of 800 ms inside live play is counted (the slip ledger clamps at 100 ms; this does not)
        Feed f; f.Run(100, 1000.0 / 30.0, 50.0 / 30.0);
        f.t += 0.8; f.Run(100, 1000.0 / 30.0, 50.0 / 30.0);
        CHECK(f.m.match().worstPictureMs > 800.0, "worst %.1f", f.m.match().worstPictureMs);
        CHECK(f.m.match().lostMs > 700.0, "lost %.1f", f.m.match().lostMs);
    }
    {   // a short stretch (under 1 s) at the end of play is dropped; a longer one is reported
        Feed f; f.Run(20, 1000.0 / 30.0, 50.0 / 30.0);
        CHECK(!f.m.Break(), "short stretch reported");
        Feed g; g.Run(60, 1000.0 / 30.0, 50.0 / 30.0);
        CHECK(g.m.Break(), "1.9 s stretch not reported");
    }
    {   // a step counter that was reset (SlipInit) does not make a negative or huge step count
        SpeedMeter m; m.Frame(1.0, 1000); m.Frame(1.033, 3); m.Frame(1.066, 5);
        CHECK(m.match().gameMs < 100.0, "reset counter gave %.1f ms", m.match().gameMs);
    }
    {   // the game's own slow motion (time scale 0.85) with the simulation running exactly that: 85 % of real time, 100 % of wanted, no window under the rule
        Feed f; f.scale = 0.85; f.Run(1000, 1000.0 / 30.0, 0.85 * 50.0 / 30.0);
        CHECK(std::fabs(f.m.match().Percent() - 85.0) < 1.0, "of real %.2f", f.m.match().Percent());
        CHECK(std::fabs(f.m.match().WantedPercent() - 100.0) < 1.0, "of wanted %.2f", f.m.match().WantedPercent());
        CHECK(f.m.underRule() == 0 && f.m.underRuleReal() == f.m.windows(), "under wanted %u, under real %u of %u", f.m.underRule(), f.m.underRuleReal(), f.m.windows());
        CHECK(f.m.match().SlowShare() > 95.0, "slow share %.1f", f.m.match().SlowShare());
    }
    {   // negative control: slow motion at 0.85 but the simulation really loses time (runs 0.75 of real): wanted reads 88 %, windows under the rule on wanted
        Feed f; f.scale = 0.85; f.Run(1000, 1000.0 / 30.0, 0.75 * 50.0 / 30.0);
        CHECK(std::fabs(f.m.match().WantedPercent() - 88.2) < 1.5, "wanted %.2f", f.m.match().WantedPercent());
        CHECK(f.m.underRule() == f.m.windows() && f.m.windows() > 3, "under wanted %u of %u", f.m.underRule(), f.m.windows());
    }
    {   // normal play, then 10 s of slow motion at 0.5, then normal: 100 % of wanted throughout, slow share about a third
        Feed f; f.Run(300, 1000.0 / 30.0, 50.0 / 30.0); f.scale = 0.5; f.Run(300, 1000.0 / 30.0, 25.0 / 30.0); f.scale = 1.0; f.Run(300, 1000.0 / 30.0, 50.0 / 30.0);
        CHECK(f.m.match().WantedPercent() > 99.0 && f.m.underRule() == 0, "mixed wanted %.2f under %u", f.m.match().WantedPercent(), f.m.underRule());
        CHECK(f.m.match().SlowShare() > 28.0 && f.m.match().SlowShare() < 38.0, "slow share %.1f", f.m.match().SlowShare());
    }
    std::printf("speed meter: %s (%d failed)\n", g_fail ? "FAILED" : "passed", g_fail);
    return g_fail ? 1 : 0;
}
