// Test of the bounded slip (MSC_SLIP, np_fps.cpp NpSlipDelta) on a model of the frame loop: a virtual 59.94 Hz display, a picture policy of 30 (two
// refreshes between hand-overs), a frame that costs P + M x (simulation steps it has to make), and the simulation clock (20 ms steps) fed with
// NpSlipDelta(frame time) the way nlTask.cpp does. The REAL np_fps.cpp is included; this is a model of the loop, not the game.
//   clang++ -std=c++11 -O1 -DNP_NATIVE=1 -Inative/include -Inative/platform/compat native/tools/fps/slip_unit.cpp -o /tmp/slip_unit && /tmp/slip_unit
#define NP_NATIVE 1
#include "../../platform/compat/np_fps.cpp"
#include "gs_stub.h"

#include <initializer_list>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static bool g_lan = false;
bool NpLanActive() { return g_lan; }   // (strong definition for the weak reference in np_fps.cpp)

namespace {
int g_fail = 0;
void Check(bool ok, const char* what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) ++g_fail; }

struct Result { double fps, speed, long3; };   // long3: percent of pictures held 3 refreshes or more
const double R = 1001.0 / 60000.0;
// P ms of fixed cost per frame, M ms per simulation step, `seconds` of play.
Result Run(double P, double M, double seconds, bool hitch = false) {
    double t = 0.0, lastT = 0.0, lastHand = 0.0, acc = 0.02;
    uint64_t frames = 0, steps = 0, longHolds = 0;
    unsigned burst = 0;
    while (t < seconds) {
        const double dt = t - lastT;
        lastT = t;
        // a hitch (a 100 ms frame: a shader compile, a pause for a card read) every 5 seconds' worth of frames, the way the game meets one
        if (hitch && frames && frames % 150 == 0) burst = 1;
        acc += (frames ? NpSlipDelta((float)dt + (burst ? 0.07f : 0.0f)) : 0.0f);
        burst = 0;
        unsigned n = 0;
        while (acc >= 0.02) { acc -= 0.02; ++n; }
        steps += n;
        const double ready = t + (P + M * n) * 1e-3;
        double hand = ceil(ready / R - 1e-9) * R;                       // the next refresh at or after the frame is built
        if (hand < lastHand + 2 * R - 1e-9) hand = lastHand + 2 * R;   // policy 30: two refreshes between pictures
        if (hand - lastHand > 3 * R - 1e-6) ++longHolds;
        lastHand = t = hand;
        ++frames;
    }
    return Result{frames / t, steps * 0.02 / t, 100.0 * longHolds / frames};
}
} // namespace

int main() {
    setenv("MSC_FPS", "30", 1);
    setenv("MSC_BOOT_FPS", "0", 1);
    setenv("MSC_FPS_LOG", "0", 1);
    NpFpsBeginFrame(0.0334f, 2);   // starts the policy
    // slip is off (MSC_SLIP unset): the frame time is passed through whatever it is
    Check(NpSlipDelta(0.0334f) == 0.0334f && NpSlipDelta(0.05f) == 0.05f, "slip off: the frame time is unchanged");
    Result base[3];
    const double load[3][2] = {{14, 6}, {26, 6}, {30, 6}};
    for (int i = 0; i < 3; ++i) base[i] = Run(load[i][0], load[i][1], 120);
    Check(base[0].fps > 29.9 && base[0].speed > 0.999, "off, light load: 30 fps at game speed 100 %");
    Check(base[2].fps < 25.0 && base[2].speed > 0.999, "off, heavy load: the picture rate falls, game speed stays 100 %");

    setenv("MSC_SLIP", "20", 1);
    g_slipPct = -1;   // read again
    Check(NpSlipDelta(0.0334f) == 0.0334f, "on: a frame within the period is unchanged");
    Check(NpSlipDelta(0.0345f) == 0.0345f, "on: a frame within 5 % of the period (ticker jitter) is unchanged");
    const float a = NpSlipDelta(0.040f), b = NpSlipDelta(0.060f), c = NpSlipDelta(0.100f);
    Check(a < 0.040f && a >= 0.040f / 1.2f - 1e-6f, "on: a 40 ms frame advances the game by less, by at most 20 %");
    Check(fabsf(b - 0.060f / 1.2f) < 1e-6f && fabsf(c - 0.100f / 1.2f) < 1e-6f, "on: long frames advance by frame / 1.2");
    Result on[3];
    for (int i = 0; i < 3; ++i) {
        on[i] = Run(load[i][0], load[i][1], 120);
        printf("     load P=%.0f ms M=%.0f ms per step: off %.1f fps, speed %.1f %%   on %.1f fps, speed %.1f %%\n", load[i][0], load[i][1], base[i].fps, 100 * base[i].speed,
               on[i].fps, 100 * on[i].speed);
    }
    Check(on[0].fps > 29.9 && on[0].speed > 0.999, "on, light load: nothing changes (30 fps, speed 100 %)");
    Check(on[1].fps >= base[1].fps && on[2].fps >= base[2].fps, "on, overload: never fewer pictures a second than off");
    Check(on[1].speed >= 1.0 / 1.2 - 0.01 && on[2].speed >= 1.0 / 1.2 - 0.01, "on, overload: the game never runs below 83 % of real time");
    {   // where it pays: the picture rate with slip against without, over a range of loads (M = the cost of one 20 ms simulation step)
        printf("     P ms  M ms | off fps  speed | on fps  speed\n");
        double best = 0;
        for (double M : {4.0, 6.0, 8.0})
            for (double P = 18; P <= 34; P += 2) {
                setenv("MSC_SLIP", "0", 1); g_slipPct = -1;
                const Result o = Run(P, M, 60);
                setenv("MSC_SLIP", "20", 1); g_slipPct = -1;
                const Result n = Run(P, M, 60);
                printf("     %4.0f %5.0f | %6.1f %6.1f%% | %6.1f %6.1f%%%s\n", P, M, o.fps, 100 * o.speed, n.fps, 100 * n.speed, n.fps > o.fps + 0.5 ? "   <- slip keeps the picture rate" : "");
                if (n.fps - o.fps > best) best = n.fps - o.fps;
            }
        printf("     best gain %.1f fps\n", best);
        Check(true, "scan done");
        setenv("MSC_SLIP", "20", 1); g_slipPct = -1;
    }
    {   // the catch-up cap (MSC_SLIP_STEPS)
        setenv("MSC_SLIP_STEPS", "2", 1); g_slipPct = -1;
        Check(NpSlipDelta(0.0334f) == 0.0334f && NpSlipDelta(0.0345f) == 0.0345f, "cap 2: an on-time frame is unchanged");
        const float c40 = NpSlipDelta(0.040f), c60 = NpSlipDelta(0.060f), c100 = NpSlipDelta(0.100f);
        Check(c40 <= 0.0400001f && c60 <= 0.0400001f && c100 <= 0.0400001f, "cap 2: a late frame advances the game by at most two 20 ms steps");
        Check(c100 >= 0.0399f, "cap 2: and by that much, not less");
        setenv("MSC_SLIP_STEPS", "9", 1); g_slipPct = -1;
        Check(fabsf(NpSlipDelta(0.5f) - 0.1f) < 1e-5f, "a cap above five steps is five (0.1 s)");
        setenv("MSC_SLIP_STEPS", "0", 1); g_slipPct = -1;
        Check(fabsf(NpSlipDelta(0.100f) - 0.100f / 1.2f) < 1e-6f, "cap 0: as before");
        printf("     with a 70 ms hitch every 150 pictures: P ms M ms | slip only: fps speed 3+ holds | cap 2: fps speed 3+ holds\n");
        double gainLong = 0;
        for (double P : {14.0, 20.0, 26.0}) {
            setenv("MSC_SLIP_STEPS", "0", 1); g_slipPct = -1;
            const Result o = Run(P, 6, 120, true);
            setenv("MSC_SLIP_STEPS", "2", 1); g_slipPct = -1;
            const Result n = Run(P, 6, 120, true);
            printf("     %4.0f %5.0f | %6.1f %6.1f%% %5.2f%% | %6.1f %6.1f%% %5.2f%%\n", P, 6.0, o.fps, 100 * o.speed, o.long3, n.fps, 100 * n.speed, n.long3);
            Check(n.long3 <= o.long3 + 0.5, "cap 2: no more pictures held three refreshes or longer than without");
            Check(n.speed >= o.speed - 0.05, "cap 2: the game clock gives up at most 5 percentage points of speed here (80 % against 84 % at a steady 20 fps: the cap is below the slip floor there)");
            gainLong += o.long3 - n.long3;
        }
        Check(gainLong > 0.0, "cap 2: removes some long holds in the model");
        setenv("MSC_SLIP_STEPS", "0", 1); g_slipPct = -1;
    }
    g_lan = true;
    Check(NpSlipDelta(0.100f) == 0.100f, "on, LAN session: no slip");
    g_lan = false;
    printf("%d failures\n", g_fail);
    return g_fail ? 1 : 0;
}
