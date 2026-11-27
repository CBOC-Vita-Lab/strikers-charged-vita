// The freeze button's counting rule (src/np_freeze_button.h): 100 ms polls, 3 s unbroken hold, the first 20 s ignored, a settings screen blocks.
//   clang++ -std=c++17 -Wall -Wextra -fsanitize=address,undefined -Inative/platform/vita/src native/platform/vita/tests/test_freeze_button.cpp -o /tmp/test_freeze && /tmp/test_freeze
// run_freeze_button_test.sh also builds mutants of the header (each must fail this test).
#include "np_freeze_button.h"

#include <cstdio>

namespace {
int g_checks, g_failures;
void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_failures; std::printf("FAIL: %s\n", what); }
}
// Polls every 100 ms from startMs: the buttons are down for `heldMs` of samples (first sample at startMs, so 2900 ms = 30 samples ... see below), then up. Returns the time it fired, or 0.
// heldMs is the span from the first down sample to the last down sample: 2900 = 30 samples, 3000 = 31 samples.
uint32_t Run(uint32_t startMs, uint32_t heldMs, bool blocked = false, uint32_t tailMs = 500) {
    NpFreeze::Chord c;
    for (uint32_t t = startMs; t <= startMs + heldMs + tailMs; t += NpFreeze::kPollMs) {
        const bool down = t <= startMs + heldMs;
        if (c.Step(t, down, blocked)) return t;
    }
    return 0;
}
}

int main() {
    using NpFreeze::Chord;
    Check(Run(30000, 2900) == 0, "held 2.9 s does not fire");
    Check(Run(30000, 3000) == 33000, "held 3.0 s fires at the sample 3.0 s after the first");
    Check(Run(30000, 5000) == 33000, "held longer fires once, at 3.0 s");
    { // fires only once
        Chord c; int n = 0;
        for (uint32_t t = 30000; t < 40000; t += 100) n += c.Step(t, true, false);
        Check(n == 1, "fires once while still held");
    }
    { // released in between resets
        Chord c; bool fired = false;
        for (uint32_t t = 30000; t <= 32800; t += 100) fired |= c.Step(t, true, false);   // 2.8 s down
        fired |= c.Step(32900, false, false);                                               // let go for one sample
        for (uint32_t t = 33000; t <= 35700; t += 100) fired |= c.Step(t, true, false);   // 2.7 s down again: 5.5 s since the first press, 2.7 s unbroken
        Check(!fired, "a release between two holds resets the count");
        Check(c.Step(36000, true, false), "the second hold fires 3.0 s after it began");
    }
    { // one of the three buttons up for one sample resets too (the caller passes allDown=false)
        Chord c; bool fired = false;
        for (uint32_t t = 30000; t <= 32900; t += 100) fired |= c.Step(t, t != 31500, false);
        Check(!fired, "one sample without all three resets");
    }
    // the first 20 s are ignored
    Check(Run(0, 3000) == 0, "held from process start: nothing in the first 20 s");
    Check(Run(10000, 9000) == 0, "held 10..19 s: nothing");
    Check(Run(18000, 5000) == 23000, "held across the 20 s mark counts from the first sample at or after 20 s (fires 3.0 s after 20 s)");
    Check(Run(19900, 3000) == 23000 || Run(19900, 3200) == 23000, "the sample at 19.9 s does not start the hold");
    Check(Run(20000, 3000) == 23000, "the first counted sample is at exactly 20 s");
    // a screen that owns the pad blocks
    Check(Run(30000, 6000, true) == 0, "settings screen open: never");
    { // opening the screen in the middle resets
        Chord c; bool fired = false;
        for (uint32_t t = 30000; t <= 32800; t += 100) fired |= c.Step(t, true, false);
        fired |= c.Step(32900, true, true);
        fired |= c.Step(33000, true, false);   // the screen closed again: the hold starts anew
        Check(!fired, "a screen opening in the middle of a hold resets it");
        Check(c.Step(36000, true, false), "and 3 s after the restart it fires");
    }
    // the switch
    Check(NpFreeze::Enabled(nullptr, true) && !NpFreeze::Enabled(nullptr, false), "unset: the build's default");
    Check(NpFreeze::Enabled("", true), "empty: the default");
    Check(!NpFreeze::Enabled("0", true), "MSC_FREEZE_BUTTON=0 off in the player build");
    Check(NpFreeze::Enabled("1", false), "MSC_FREEZE_BUTTON=1 on in a lab build");
    Check(NpFreeze::Enabled("2", false), "any other value is on");
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
