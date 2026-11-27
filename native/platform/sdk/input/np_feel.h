// np_feel.h: how the controls FEEL (agent "feel", wave 6). Pure code, no game headers; the host test is
// platform/sdk/tests/test_feel.cpp (g++ -std=c++17, no libraries).
//
//  1. MegaHand: the Mega Save hand (the goalkeeper's catching pointer). The stick used to move the pointer by
//       cursor += dz(v) * speed * dt           (per-axis dead zone 0.2 on top of the stick's own 16/102 one, linear, the stick of
//                                                the END of the interval credited to the whole interval)
//     which has three faults on a Vita: (a) the stick has a small throw and the two dead zones eat a quarter of it (the hand
//     starts to move at 33 of 102 raw units, at a fifth of full speed: no slow speed exists); (b) the per-axis zone snaps a
//     diagonal onto an axis; (c) forward Euler: the old rule leads the true position by half a picture of travel while the stick is held (17 ms of travel at 30,
//     8 ms at 60: 32 and 16 canvas px at 3x speed), so the same stick reaches different places at different picture rates. MegaHand:
//       - one radial zone (MSC_MEGA_DEAD, default 0.04 of the stick's already dead-zoned range) and a power curve
//         (MSC_MEGA_CURVE, default 1.5; 1 = linear; 0 = the old formula): small deflections move slowly and precisely, the
//         full deflection keeps the old top speed; and the right stick drives the hand from its first unit of deflection;
//       - trapezoid integration (MSC_MEGA_TRAP=0 turns it off): the hand moves by the mean of the previous and the current velocity
//         times dt, which removes that lead: the position at a sample is the integral of the stick up to it, whatever the rate.
//     A pure step edge (a push or a release that falls somewhere inside one interval) is uncertain by up to a picture of travel
//     under either rule; that is the information the samples do not hold, and only a higher picture rate reduces it.
//  2. PadLate: the "late latch" controller (MSC_PAD_LATE, off). Under a held cadence (30, or 60 with a pipeline that needs more
//     than a refresh) the pad is read at the start of the period, the picture is ready some time before its refresh and waits
//     for it: the slack is pure latency. Reading the pad (and running the step that uses it) `delay` later takes the slack
//     out, with no change to the picture rate while the delay stays below the slack. The controller sees the slack of
//     each shown picture (time from "the GPU has it" to "it is on the screen") and returns the delay of the next read.
#ifndef NP_FEEL_H
#define NP_FEEL_H

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

namespace NpFeel {

// ---- 1. the Mega Save hand ----------------------------------------------------------------------------------------------
struct MegaCurve {
    float dead = 0.04f;    // of the stick range that is left after the stick's own dead zone
    float power = 1.5f;    // 1 = linear. <= 0: the legacy mapping
};
inline bool MegaLegacy(const MegaCurve& c) { return !(c.power > 0.0f); }

// Velocity factor of a stick (x, y in -1..1, the stick's own dead zone already applied): output vector with |out| in 0..1.
inline void MegaVelocity(const MegaCurve& c, float x, float y, float* ox, float* oy) {
    if (!isfinite(x) || !isfinite(y)) { *ox = *oy = 0.0f; return; }
    if (MegaLegacy(c)) {   // the old formula: per-axis zone of 0.2
        *ox = fabsf(x) < 0.2f ? 0.0f : x;
        *oy = fabsf(y) < 0.2f ? 0.0f : y;
        return;
    }
    const float len = sqrtf(x * x + y * y);
    if (len <= c.dead) { *ox = *oy = 0.0f; return; }
    float u = (len - c.dead) / (1.0f - c.dead);   // (a report longer than 1 counts as full)
    if (u > 1.0f) u = 1.0f;
    const float f = powf(u, c.power);
    const float k = f / len;
    *ox = x * k;
    *oy = y * k;
}

// The hand's integrator for one channel. Step(): dt seconds since the previous sample, (vx, vy) the velocity factors of the sample
// (MegaVelocity), speed in units of cursor range per second at full deflection; returns the displacement.
struct MegaHand {
    float pvx = 0, pvy = 0;
    bool have = false;
    void Reset() { have = false; pvx = pvy = 0; }
    void Step(bool trapezoid, float dt, float vx, float vy, float speed, float* dx, float* dy) {
        if (!(dt > 0.0f) || !isfinite(dt)) { *dx = *dy = 0.0f; pvx = vx; pvy = vy; have = true; return; }
        float ax = vx, ay = vy;
        if (trapezoid && have) { ax = 0.5f * (pvx + vx); ay = 0.5f * (pvy + vy); }
        else if (trapezoid) { ax *= 0.5f; ay *= 0.5f; }   // the first sample after the hand was idle: half the interval counts
        *dx = ax * speed * dt;
        *dy = ay * speed * dt;
        pvx = vx;
        pvy = vy;
        have = true;
    }
};

inline MegaCurve ConfiguredMegaCurve() {
    static MegaCurve c;
    static bool read = false;
    if (!read) {
        read = true;
        const char* e = getenv("MSC_MEGA_CURVE");
        if (e && *e) c.power = (float)atof(e);
        if (c.power > 4.0f) c.power = 4.0f;
        const char* d = getenv("MSC_MEGA_DEAD");
        if (d && *d) {
            c.dead = (float)atof(d);
            if (!(c.dead >= 0.0f)) c.dead = 0.0f;
            if (c.dead > 0.5f) c.dead = 0.5f;
        }
    }
    return c;
}
inline bool MegaTrapezoid() {
    static int v = -1;
    if (v < 0) { const char* e = getenv("MSC_MEGA_TRAP"); v = !(e && *e == '0'); }
    return v == 1;
}

// ---- 2. the late latch ---------------------------------------------------------------------------------------------------
// Observe(slackUs, late): one shown picture. slackUs = how long it waited, ready, for its refresh (>= 0); late = it missed the
// refresh it was aimed at (the picture took a whole period longer). DelayUs(): how long the next pad read is put off.
// Rules: the delay grows by at most 250 us a picture, only while the smallest slack of the last 24 pictures exceeds delay + margin;
// it falls to zero at once on a late picture and stays there for `holdOff` pictures; it never exceeds maxUs.
struct PadLate {
    uint32_t marginUs = 3000, maxUs = 12000, stepUs = 250, holdOff = 120;
    uint32_t delay = 0, hold = 0, n = 0;
    uint32_t win[24] = {};
    uint32_t seen = 0;
    void Observe(uint32_t slackUs, bool late) {
        win[seen % 24] = slackUs;
        ++seen;
        if (late) { delay = 0; hold = holdOff; return; }
        if (hold) { --hold; return; }
        if (seen < 24) return;
        uint32_t mn = win[0];
        for (uint32_t i = 1; i < 24; ++i) if (win[i] < mn) mn = win[i];
        // the slack of the pictures seen includes the delay already in force: the delay may grow while more than margin is left over
        if (mn > marginUs) {
            const uint32_t room = mn - marginUs;
            delay += stepUs < room ? stepUs : room;
        } else if (delay > 0) {
            delay = delay > stepUs ? delay - stepUs : 0;   // less than the margin left: give back a little at a time
        }
        if (delay > maxUs) delay = maxUs;
    }
    uint32_t DelayUs() const { return delay; }
};

}  // namespace NpFeel

#endif  // NP_FEEL_H
