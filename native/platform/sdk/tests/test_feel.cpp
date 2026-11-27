// Host test of np_feel.h (the Mega Save hand, the late latch) and the tap latch's gap rule (np_controls.h).
//   g++ -std=c++17 -O1 -I native/platform/sdk/input native/platform/sdk/tests/test_feel.cpp -o /tmp/test_feel && /tmp/test_feel
#include "../input/np_controls.h"
#include "../input/np_feel.h"

#include <stdio.h>
#include <math.h>
#include <random>

static int g_fail = 0, g_checks = 0;
#define CHECK(c)                                                                    \
    do {                                                                            \
        ++g_checks;                                                                 \
        if (!(c)) {                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                     \
            g_fail++;                                                               \
        }                                                                           \
    } while (0)

using namespace NpFeel;

// A stick given as a function of continuous time: (x, y) in the stick's own range.
struct Profile { float (*x)(double); float (*y)(double); };
static float stepX(double t) { return (t >= 0.1234 && t < 0.5947) ? 1.0f : 0.0f; }      // pushed to the stop for 0.4713 s (not a whole number of frames at any rate)
static float zeroY(double) { return 0.0f; }
static float rampX(double t) { return t < 0.2 ? 0.0f : t > 0.7 ? 1.0f : (float)((t - 0.2) / 0.5); }   // a slow push to full

// Displacement of the hand over [0, T] when the game samples at `hz` (phase p of one period), legacy or new.
static double g_tLast = 0;
static double Run(const Profile& pr, double hz, double phase, bool legacy, double T, MegaCurve curve, bool trap) {
    MegaHand hand;
    double pos = 0, last = 0;
    const double dt = 1.0 / hz;
    bool first = true;
    for (double t = phase * dt; t <= T; t += dt) {
        const float sx = pr.x(t), sy = pr.y(t);
        float vx, vy, dx, dy;
        if (legacy) {
            const MegaCurve lc{0.04f, 0.0f};
            MegaVelocity(lc, sx, sy, &vx, &vy);
            const float step = first ? 0.0f : (float)(t - last);
            pos += vx * 1.0f * step;
            (void)dx; (void)dy;
        } else {
            MegaVelocity(curve, sx, sy, &vx, &vy);
            hand.Step(trap, first ? 0.0f : (float)(t - last), vx, vy, 1.0f, &dx, &dy);
            pos += dx;
        }
        last = t;
        first = false;
    }
    g_tLast = last;
    return pos;
}

int main() {
    const MegaCurve def{0.04f, 1.5f};
    // ---- the curve ----
    {
        float x, y;
        MegaVelocity(def, 0.0f, 0.0f, &x, &y);
        CHECK(x == 0 && y == 0);
        MegaVelocity(def, 0.03f, 0.0f, &x, &y);
        CHECK(x == 0);                                  // inside the zone
        MegaVelocity(def, 1.0f, 0.0f, &x, &y);
        CHECK(fabsf(x - 1.0f) < 1e-5f && y == 0);       // the full deflection keeps the old top speed
        float prev = 0;
        bool mono = true;
        for (int i = 0; i <= 100; ++i) {
            MegaVelocity(def, i / 100.0f, 0, &x, &y);
            if (x < prev - 1e-6f) mono = false;
            prev = x;
        }
        CHECK(mono);
        // a small deflection is slower than the old formula's and a diagonal keeps its direction (the old per-axis zone snapped it to an axis)
        MegaVelocity(def, 0.3f, 0.0f, &x, &y);
        const float newAt03 = x;
        MegaVelocity(MegaCurve{0.04f, 0.0f}, 0.3f, 0.0f, &x, &y);
        CHECK(newAt03 < x);                              // 0.3 stick: legacy 0.3, new about 0.14
        printf("curve: stick 0.3 -> %.3f of top speed (was 0.300), 0.5 -> ", newAt03);
        MegaVelocity(def, 0.5f, 0, &x, &y);
        printf("%.3f, 0.8 -> ", x);
        MegaVelocity(def, 0.8f, 0, &x, &y);
        printf("%.3f\n", x);
        float ox, oy;
        MegaVelocity(def, 0.5f, 0.12f, &ox, &oy);
        CHECK(oy > 0.0f);
        MegaVelocity(MegaCurve{0.04f, 0.0f}, 0.5f, 0.12f, &ox, &oy);
        CHECK(oy == 0.0f);                               // legacy: snapped to the horizontal
        MegaVelocity(def, 0.5f, 0.12f, &ox, &oy);
        CHECK(fabsf(oy / ox - 0.12f / 0.5f) < 1e-4f);   // direction kept exactly
        MegaVelocity(def, NAN, 0.0f, &ox, &oy);
        CHECK(ox == 0 && oy == 0);
    }
    // ---- frame-rate independence: the hand's position at its last sample against the exact integral of the stick up to that instant ----
    {
        std::mt19937 rng(7);
        const Profile steps{stepX, zeroY};
        const Profile ramp{rampX, zeroY};
        const double rates[] = {15, 20, 30, 50, 60};
        const MegaCurve lin{0.0f, 1.0f};
        auto integral = [&](const Profile& pr, MegaCurve c, double upTo) {
            double sum = 0;
            for (double t = 0.00005; t < upTo; t += 0.0001) { float vx, vy; MegaVelocity(c, pr.x(t), pr.y(t), &vx, &vy); sum += vx * 0.0001; }
            return sum;
        };
        // a push held (0.4713 s, not a whole number of frames at any rate) and released: error at the end, Euler against trapezoid
        double sumOld = 0, sumNew = 0, absOld = 0, absNew = 0;
        int cnt = 0;
        for (double hz : rates) {
            for (int k = 0; k < 40; ++k) {
                const double ph = (rng() % 1000) / 1000.0;
                const double o = Run(steps, hz, ph, true, 1.0, lin, false);
                const double eo = o - integral(steps, lin, g_tLast);
                const double n = Run(steps, hz, ph, false, 1.0, lin, true);
                const double en = n - integral(steps, lin, g_tLast);
                sumOld += eo; sumNew += en; absOld += fabs(eo); absNew += fabs(en); ++cnt;
            }
        }
        printf("push and release, 15..60 Hz, 200 phases: mean error old %+.4f new %+.4f; mean |error| old %.4f new %.4f (units of cursor range at full speed 1/s)\n",
               sumOld / cnt, sumNew / cnt, absOld / cnt, absNew / cnt);
        CHECK(fabs(sumNew / cnt) < 0.005);
        CHECK(absNew <= absOld + 1e-9);
        // a slow push to full with the curve: the position at the last sample against the exact integral; the old rule leads by half a frame of travel
        double bOld = 0, bNew = 0;
        for (double hz : rates) {
            const double po = Run(ramp, hz, 0.37, false, 1.0, def, false);
            const double eo = po - integral(ramp, def, g_tLast);
            const double pn = Run(ramp, hz, 0.37, false, 1.0, def, true);
            const double en = pn - integral(ramp, def, g_tLast);
            bOld = fmax(bOld, fabs(eo)); bNew = fmax(bNew, fabs(en));
            printf("  %2.0f Hz: Euler %+.4f trapezoid %+.4f\n", hz, eo, en);
        }
        printf("ramp push with the curve: worst error over 15..60 Hz: Euler %.4f, trapezoid %.4f\n", bOld, bNew);
        CHECK(bNew < 0.3 * bOld);
        CHECK(bNew < 0.004);
    }
    // ---- the first sample after idle, reset ----
    {
        MegaHand h;
        float dx, dy;
        h.Step(true, 0.033f, 1.0f, 0.0f, 1.0f, &dx, &dy);
        CHECK(fabsf(dx - 0.5f * 0.033f) < 1e-6f);        // half the interval counts
        h.Step(true, 0.033f, 1.0f, 0.0f, 1.0f, &dx, &dy);
        CHECK(fabsf(dx - 0.033f) < 1e-6f);
        h.Reset();
        h.Step(true, 0.0f, 1.0f, 0.0f, 1.0f, &dx, &dy);
        CHECK(dx == 0);
        h.Step(false, 0.02f, 0.5f, 0.0f, 2.0f, &dx, &dy);  // trapezoid off: forward Euler, as before
        CHECK(fabsf(dx - 0.5f * 2.0f * 0.02f) < 1e-6f);
    }
    // ---- the tap carry (wave 9): a frame without a step reads the pad at its end only; its latched tap must reach the next step
    {
        using namespace NpControls;
        TapCarry c;
        CHECK(c.Apply(kCross, 0) == kCross);              // an ordinary read: shown, nothing kept
        CHECK(c.Apply(0, 0) == 0);
        c.SetHold(true);                                  // the zero-step frame's end read: a tap latched in it (Cross, already up)
        CHECK(c.Apply(kCross, 0) == kCross);
        c.SetHold(false);
        CHECK(c.Apply(0, 0) == kCross);                   // the next read (a step) still shows it, once
        CHECK(c.Apply(0, 0) == 0);
        c.SetHold(true);
        CHECK(c.Apply(kSquare, kSquare) == kSquare);      // still down: nothing to carry (the next sample has it)
        c.SetHold(false);
        CHECK(c.Apply(kSquare, kSquare) == kSquare);
        CHECK(c.Apply(0, 0) == 0);
        c.SetHold(true);
        c.Apply(kL, 0);
        c.SetHold(true);                                  // two frames without a step in a row: kept until a read that is not held
        CHECK(c.Apply(0, 0) == kL);
        c.SetHold(false);
        CHECK(c.Apply(0, 0) == kL);                       // (still shown at the first read that is not held, then gone)
        CHECK(c.Apply(0, 0) == 0);
    }

    // ---- the tap latch's gap rule ----
    {
        using namespace NpControls;
        uint64_t last = 0;
        uint32_t b[4] = {kCross, 0, 0, 0};
        uint64_t t[4] = {100000, 116000, 132000, 148000};
        CHECK(LatchTaps(b, t, 4, &last, 250000) == 0);                    // first read: newest only
        uint32_t b2[4] = {0, kCircle, 0, 0};
        uint64_t t2[4] = {148000, 164000, 180000, 196000};
        CHECK(LatchTaps(b2, t2, 4, &last, 250000) == kCircle);          // a tap between two reads still counts
        uint32_t b3[4] = {kSquare, kSquare, 0, 0};
        uint64_t t3[4] = {5196000, 5212000, 5228000, 5244000};           // five seconds later (a Home-button suspend)
        CHECK(LatchTaps(b3, t3, 4, &last, 250000) == 0);                  // not replayed
        last = 196000;
        CHECK(LatchTaps(b3, t3, 4, &last, 0) == kSquare);                 // the old rule would
    }
    // ---- the late latch against a pipeline model ----
    {
        std::mt19937 rng(3);
        auto sim = [&](double period, double pipeMs, double jitterMs, double spikeMs, int frames, bool on, double* meanDelayMs, int* latePics, double* meanLat) {
            PadLate c;
            double padRead = 0, sumLat = 0, sumDelay = 0;
            int late = 0, n = 0;
            double slot = period;
            for (int f = 0; f < frames; ++f) {
                const double periodStart = f * period;
                const double delay = on ? c.DelayUs() / 1000.0 : 0.0;
                padRead = periodStart + delay;
                double T = pipeMs + (rng() % 1000) / 1000.0 * jitterMs;
                if (spikeMs > 0 && f % 211 == 100) T += spikeMs;
                const double ready = padRead + T;
                double shown = periodStart + 2 * period;          // the slot after the one the frame was aimed at (pad -> screen is two periods when the pipeline is under 2)
                bool lateF = false;
                while (shown < ready) { shown += period; lateF = true; }
                const double slack = shown - ready;
                const double cpuSlack = fmax(0.0, period - (delay + 0.6 * T));   // the game thread's wait after its build, with the delay in force
                c.Observe((uint32_t)(fmin(slack, cpuSlack + delay) * 1000.0), lateF);
                (void)slot;
                sumLat += shown - padRead;
                sumDelay += delay;
                late += lateF;
                ++n;
            }
            *meanDelayMs = sumDelay / n;
            *latePics = late;
            *meanLat = sumLat / n;
        };
        double d0, l0, d1, l1;
        int late0, late1;
        sim(33.366, 24.0, 3.0, 0.0, 3000, false, &d0, &late0, &l0);
        sim(33.366, 24.0, 3.0, 0.0, 3000, true, &d1, &late1, &l1);
        printf("held 30, pipeline 24-27 ms: pad to screen %.1f ms -> %.1f ms (mean delay %.1f ms), late pictures %d -> %d\n", l0, l1, d1, late0, late1);
        CHECK(l1 < l0 - 3.0);
        CHECK(late1 <= late0 + 3);
        sim(33.366, 24.0, 3.0, 30.0, 3000, true, &d1, &late1, &l1);   // a 30 ms spike every 211 frames
        sim(33.366, 24.0, 3.0, 30.0, 3000, false, &d0, &late0, &l0);
        printf("the same with a 30 ms hitch every 211 pictures: %.1f -> %.1f ms, late pictures %d -> %d (each hitch costs one, then the delay is off for 120)\n", l0, l1, late0, late1);
        CHECK(late1 <= late0 + 3 + 3000 / 211);
        sim(16.683, 14.0, 1.5, 0.0, 3000, false, &d0, &late0, &l0);
        sim(16.683, 14.0, 1.5, 0.0, 3000, true, &d1, &late1, &l1);
        printf("held 60, pipeline 14-15.5 ms: pad to screen %.1f ms -> %.1f ms (delay %.1f ms), late %d -> %d\n", l0, l1, d1, late0, late1);
        CHECK(l1 <= l0 + 0.01);
        // never over the cap, never negative
        PadLate c;
        for (int i = 0; i < 1000; ++i) c.Observe(50000, false);
        CHECK(c.DelayUs() <= c.maxUs);
        c.Observe(1000, true);
        CHECK(c.DelayUs() == 0);
        for (int i = 0; i < 50; ++i) c.Observe(50000, false);
        CHECK(c.DelayUs() == 0);                                         // held off for 120 pictures
    }
    printf("test_feel: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail != 0;
}
