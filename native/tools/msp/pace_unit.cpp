// Unit test of the 30 fps pacing arithmetic (np/framepace.h, platform/compat/np_framepace.cpp), no game needed:
//   clang++ -m32 -DNP_NATIVE=1 -Iinclude tools/msp/pace_unit.cpp platform/compat/np_framepace.cpp -o /tmp/pace_unit && /tmp/pace_unit
#include "np/framepace.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

extern "C" unsigned long g_npPaceExtra;
static const float F = 1001.0f / 60000.0f;   // one NTSC original frame

// run frames of the given real lengths (seconds); returns the total steps taken
static unsigned long run(int count, float (*len)(int), int* minSteps, int* maxSteps)
{
    unsigned long total = 0;
    *minSteps = 99; *maxSteps = 0;
    for (int i = 0; i < count; ++i)
    {
        NpPaceBeginFrame(len(i));
        int k = NpPaceSteps(NP_PACE_ROLLS);
        total += (unsigned long)k;
        if (k < *minSteps) *minSteps = k;
        if (k > *maxSteps) *maxSteps = k;
    }
    return total;
}

static float lenOriginal(int) { return F; }
static float lenFast(int) { return F * 0.5f; }
static float len30(int) { return 2.0f * F; }
static float len33(int) { return 0.033f; }
static float len3x(int) { return 3.0f * F; }
static float lenStall(int) { return 0.5f; }
static float lenJitter(int i) { static unsigned s = 12345; s = s * 1103515245u + 12345u; return 0.025f + 0.020f * ((s >> 16) & 0x7fff) / 32768.0f; }
static float len30Drift(int i) { return 0.0300f; }   // a steady 30.0 ms: 1.8 original frames

int main()
{
    int lo, hi;
    unsigned long t;
    NpPaceSwapMode(1, 0);

    t = run(1000, lenOriginal, &lo, &hi);
    CHECK(lo == 1 && hi == 1 && t == 1000 && g_npPaceExtra == 0, "original rate must be one step, no extra: steps %lu extra %lu", t, g_npPaceExtra);
    CHECK(NpPaceFrames(NP_PACE_CAMERA) == 1.0f, "frames %f", NpPaceFrames(NP_PACE_CAMERA));

    t = run(1000, lenFast, &lo, &hi);
    CHECK(lo == 1 && hi == 1 && NpPaceFrames(NP_PACE_CAMERA) == 1.0f, "faster than the original is the original path");

    unsigned long before = g_npPaceExtra;
    t = run(1000, len30, &lo, &hi);
    CHECK(lo == 2 && hi == 2 && NpPaceFrames(NP_PACE_CAMERA) == 2.0f, "two retraces per frame: 2 steps, n=2: %d %d %f", lo, hi, NpPaceFrames(NP_PACE_CAMERA));
    CHECK(g_npPaceExtra - before == 1000, "extra original frames %lu", g_npPaceExtra - before);

    t = run(1000, len33, &lo, &hi);
    CHECK(lo == 2 && hi == 2, "33 ms (1.978 original frames) snaps to 2: %d %d", lo, hi);

    t = run(100, len3x, &lo, &hi);
    CHECK(lo == 3 && hi == 3, "three retraces: 3 steps");

    t = run(100, lenStall, &lo, &hi);
    CHECK(lo == 4 && hi == 4 && NpPaceFrames(NP_PACE_CAMERA) == 4.0f, "a 500 ms frame is a stall and counts as 4: %d %f", lo, NpPaceFrames(NP_PACE_CAMERA));

    // steady 30.0 ms = 1.7983 original frames: error-diffused, the mean steps per frame must be 1.7983 (+-0.002 over 10000 frames)
    t = run(10000, len30Drift, &lo, &hi);
    double mean = t / 10000.0, want = 0.030 / F;
    CHECK(fabs(mean - want) < 0.002 && lo >= 1 && hi <= 2, "30 ms steady: mean steps %.4f want %.4f (%d..%d)", mean, want, lo, hi);

    // jitter 25..45 ms: the total of original frames must follow the total time within a frame or two per 100 (snapping loses at most 0.1 per frame)
    double secs = 0;
    t = 0;
    for (int i = 0; i < 10000; ++i) { float l = lenJitter(i); secs += l; NpPaceBeginFrame(l); t += NpPaceSteps(NP_PACE_ROLLS); }
    double want2 = secs / F;
    CHECK(fabs((double)t - want2) / want2 < 0.03, "jitter: %lu original frames for %.2f s (%.1f wanted, %.2f%%)", t, secs, want2, 100.0 * ((double)t - want2) / want2);
    printf("jitter 25-45 ms: %lu original frames for %.2f s = %.3f /s (59.94 wanted)\n", t, secs, t / secs);

    // swap mode 2 (30 fps lock): one original frame is two retraces
    NpPaceSwapMode(2, 0);
    t = run(100, len30, &lo, &hi);
    CHECK(lo == 1 && hi == 1, "swap mode 2 at 2 retraces per frame is the original: %d %d", lo, hi);
    t = run(100, len3x, &lo, &hi);
    CHECK(lo == 1 && hi == 2 && fabs(t / 100.0 - 1.5) < 0.02, "swap mode 2 at 3 retraces: 1.5 original frames, alternating 1 and 2: %d %d (mean %.2f)", lo, hi, t / 100.0);
    NpPaceSwapMode(1, 0);

    // PAL
    NpPaceSwapMode(1, 1);
    t = run(100, [](int) { return 0.02f; }, &lo, &hi);
    CHECK(lo == 1 && hi == 1, "PAL: 20 ms is one original frame");
    t = run(100, [](int) { return 0.04f; }, &lo, &hi);
    CHECK(lo == 2 && hi == 2, "PAL: 40 ms is two");

    printf(fails ? "pace_unit: %d FAILED\n" : "pace_unit: all passed\n", fails);
    return fails != 0;
}
