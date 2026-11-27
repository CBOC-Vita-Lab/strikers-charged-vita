// gxvita14 (wave 14): the smoothness score (vgx_smooth.h). -DVGX_SMOOTH_BREAK=1..3 must fail (run_smooth_test.sh).
#include "../src/vgx_smooth.h"

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace vgx::smooth;
static int g_fail = 0;
static void Check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) ++g_fail; }
static bool Near(double a, double b, double e = 1e-6) { return std::fabs(a - b) <= e; }

int main() {
    {   // a steady 30: every picture held 2, steps 2, no judder: 100
        Mix m; m.hold[2] = 600; m.step[2] = 600; m.secs = 20;
        const Score s = Compute(m);
        Check(s.pictures == 600 && s.lo == 2 && s.hi == 2 && Near(s.idealPct, 100) && Near(s.score, 100), "steady 30: score 100, cadence 2..2");
    }
    {   // 45 a second: holds 1 and 2 alternate: both are ideal
        Mix m; m.hold[1] = 300; m.hold[2] = 300; m.step[1] = 300; m.step[2] = 300; m.secs = 13.33;
        const Score s = Compute(m);
        Check(s.lo == 1 && s.hi == 2 && Near(s.mean, 1.5) && Near(s.idealPct, 100), "45 a second: holds 1 and 2 are both on the cadence");
    }
    {   // 10 % of the pictures held 3 in a mostly-2 window: not ideal, and a missed moment each
        Mix m; m.hold[2] = 540; m.hold[3] = 60; m.secs = 20;
        const Score s = Compute(m);
        // mean 2.1: lo 2, hi 3 -> the 3s count as ideal by the band; missed = 60 over 20 s = 3 a second: 100 - 15 = 85
        Check(s.lo == 2 && s.hi == 3 && Near(s.missedPerSec, 3.0) && Near(s.score, 85.0), "long holds: missed moments cost 5 a second");
    }
    {   // judder costs 25 a vblank a picture; the score never goes under 0
        Mix m; m.hold[2] = 100; m.judder = 0.4; m.secs = 3.3;
        Check(Near(Compute(m).score, 90.0), "judder 0.4 costs 10");
        m.judder = 9; Check(Compute(m).score == 0.0, "clamped at 0");
    }
    {   // holds spread over 1..4: only the two around the mean count (mean 2.5: 2 and 3)
        Mix m; m.hold[1] = 100; m.hold[2] = 100; m.hold[3] = 100; m.hold[4] = 100; m.secs = 400.0 / 20;
        const Score s = Compute(m);
        Check(s.lo == 2 && s.hi == 3 && Near(s.idealPct, 50.0), "spread holds: half the pictures are on the cadence");
    }
    {   // an empty window, no seconds: neutral 100, no division by zero
        Mix m; Check(Compute(m).pictures == 0 && Compute(m).score == 100.0, "empty window");
        m.hold[2] = 5; Check(Compute(m).score == 100.0, "no seconds: neutral");
    }
    {   // hold 0 (two pictures in one vblank) is not counted
        Mix m; m.hold[0] = 50; m.hold[2] = 100; m.secs = 3.3;
        Check(Compute(m).pictures == 100, "hold 0 is not a picture");
    }
    {   // missed is capped at the pictures
        Mix m; m.hold[3] = 10; m.step[3] = 10; m.secs = 1;
        Check(Compute(m).missed == 10, "missed capped at the pictures");
    }
    {   // the log token
        Mix m; m.hold[2] = 540; m.hold[3] = 60; m.judder = 0.08; m.secs = 20;
        char b[120]; Format(b, sizeof b, Compute(m));
        Check(std::strcmp(b, "smooth 83/100 (ideal 100 %, judder 0.08, missed 3.00/s)") == 0, "log token text");
    }
    std::printf("%s\n", g_fail ? "FAILED" : "all ok");
    return g_fail ? 1 : 0;
}
