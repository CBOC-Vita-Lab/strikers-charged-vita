// Synthetic input only: exercise the actual Auto controller, never a copied policy.
#include "np_fps_ctl.h"
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace npfps;

namespace {
int passed = 0, failed = 0;
const double R = 1001.0 / 60000.0;

void Check(bool ok, const char* name) {
    std::printf("%s %s\n", ok ? "ok" : "FAIL", name);
    if (ok) ++passed; else ++failed;
}

struct Trace {
    Auto ctl;
    double now = 0, lastNow = 0, lastUp = -1;
    Trace() : ctl(Config()) {}

    Decision Step(bool probationMiss = false) {
        FrameIn in;
        std::memset(&in, 0, sizeof in);
        in.now = now;
        in.interval = probationMiss || ctl.Current() == kPolicy30 ? 2 * R : R;
        in.scene = 7;  // same ordinary scene throughout; no scene-change forgiveness
        in.costsValid = true;
        in.gameCore = 0;
        in.cpuMs[0] = probationMiss ? 14.0 : 6.0;
        const Decision out = ctl.Frame(in);
        lastNow = now;
        if (out.switched && out.policy == kPolicy60) lastUp = now;
        now += in.interval;
        return out;
    }

    bool Promote(double maxSeconds) {
        const double end = now + maxSeconds;
        while (ctl.Current() == kPolicy30 && now < end) Step();
        return ctl.Current() == kPolicy60;
    }

    void Settle() { for (int i = 0; i < 8; ++i) Step(); }
};

bool EarnFailureAndRetry(Trace& trace) {
    if (!trace.Promote(3.0)) return false;
    trace.Settle();
    const Decision down = trace.Step(true);
    if (!down.switched || down.policy != kPolicy30 || trace.ctl.Penalty() != 1 || trace.ctl.SceneFails() != 1)
        return false;
    if (std::fabs(trace.ctl.CooldownUntil() - trace.lastNow - 12.0) > 1e-8) return false;
    return trace.Promote(13.0);
}
}

int main() {
    Trace trace;
    const bool prepared = EarnFailureAndRetry(trace);
    Check(prepared, "setup: default settled probation failure earns1/1 and retries after12s");
    if (!prepared) return 2;
    const double retryAt = trace.lastUp;
    while (trace.now < retryAt + 119.0) trace.Step();
    Check(trace.ctl.Current() == kPolicy60 && trace.now - retryAt >= 119.0 && trace.now - retryAt < 119.0 + R,
          "setup:119s uninterrupted60 after retry, same scene, default thresholds");
    Check(trace.ctl.Penalty() == 1 && trace.ctl.SceneFails() == 1,
          "119s of stable60 does not yet forgive either failure counter");

    const double heldAt = trace.now;
    const int switchesBefore = trace.ctl.Switches();
    const Decision held = trace.ctl.Hold30(heldAt);
    Check(held.switched && held.policy == kPolicy30 && trace.ctl.Current() == kPolicy30 &&
          trace.ctl.Switches() == switchesBefore + 1, "Hold30 changes policy exactly once");
    Check(trace.ctl.Penalty() == 1 && trace.ctl.SceneFails() == 1,
          "Hold30 does not charge a failure or forgive existing counters");
    Check(std::fabs(trace.ctl.CooldownUntil() - heldAt - 6.0) < 1e-8,
          "Hold30 retains ordinary6s cooldown without extra backoff charge");
    const Decision repeated = trace.ctl.Hold30(heldAt);
    Check(!repeated.switched && trace.ctl.Switches() == switchesBefore + 1 &&
          trace.ctl.Penalty() == 1 && trace.ctl.SceneFails() == 1,
          "repeated hold adds no switch/failure charge");

    Check(trace.Promote(7.0), "comfortable evidence permits re-promotion after hold cooldown");
    const double resumedAt = trace.lastUp;
    Check(resumedAt - heldAt >= 6.0 && resumedAt - heldAt < 6.0 + 2 * R,
          "re-promotion occurs at ordinary6s boundary, not a changed threshold");
    trace.Settle();
    Check(trace.ctl.Penalty() == 1 && trace.ctl.SceneFails() == 1,
          "all8 resumed settling frames preserve prior failure counters");
    trace.Step();
    Check(trace.ctl.Penalty() == 1,
          "first judged resumed frame cannot forgive penalty using forced30 time");
    Check(trace.ctl.SceneFails() == 1,
          "first judged resumed frame cannot forgive scene failures using forced30 time");

    const Decision secondDown = trace.Step(true);
    const double secondDownAt = trace.lastNow;
    const double cooldown = trace.ctl.CooldownUntil() - secondDownAt;
    std::printf("OBS resumed60_age=%.6f second_cooldown=%.6f penalty=%d scene_fails=%d\n",
                secondDownAt - resumedAt, cooldown, trace.ctl.Penalty(), trace.ctl.SceneFails());
    Check(secondDown.switched && secondDown.policy == kPolicy30, "next settled probation miss still causes natural down");
    Check(trace.ctl.Penalty() == 2 && trace.ctl.SceneFails() == 2,
          "second genuine failed try retains both failure charges");
    Check(std::fabs(cooldown - 24.0) < 1e-8, "second genuine failure requires24s, not prematurely forgiven12s");
    while (trace.now < secondDownAt + 13.0) trace.Step();
    Check(trace.ctl.Current() == kPolicy30, "still30 thirteen seconds into required24s cooldown");

    Trace uninterrupted;
    const bool controlReady = EarnFailureAndRetry(uninterrupted);
    Check(controlReady, "uninterrupted-control setup earns1/1 and retries");
    if (!controlReady) return 2;
    const double controlUp = uninterrupted.lastUp;
    while (uninterrupted.now < controlUp + 120.0) uninterrupted.Step();
    Check(uninterrupted.ctl.Penalty() == 1 && uninterrupted.ctl.SceneFails() == 1,
          "uninterrupted control keeps counters strictly before120s");
    uninterrupted.Step();
    Check(uninterrupted.ctl.Current() == kPolicy60 && uninterrupted.ctl.Penalty() == 0 && uninterrupted.ctl.SceneFails() == 0,
          "uninterrupted120s stable60 still forgives counters normally");

    std::printf("\n%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
