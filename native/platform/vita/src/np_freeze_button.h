// The freeze button (galaxyport18, Galaxy c164a from NFS MW 1.6): L + R + Select held for 3 s ends a stuck game with a FATAL line in log.txt (every thread's state)
// and, if the system allows it, a core dump (ux0:data/psp2core-*.psp2dmp). This header is the pure rule (host-testable, tests/test_freeze_button.cpp); the thread that polls
// the pad and the trap are in np_vita_main.cpp / np_vita_crash.cpp.
//
// MSC_FREEZE_BUTTON: "0" off, any other value on; unset = on in the player build (NP_PLAYER), off elsewhere (lab runs: a script that holds the three buttons must not end the run).
// The rule: the thread looks every kPollMs. A sample counts only when all three buttons are down, the first kIgnoreMs of the process are over, and no screen that owns L+R is open
// (the settings screen: its start window reads L+R at the very start, inside the ignored 20 s, and the screen itself while it is open). Anything else resets the hold, so the
// 3 s must be unbroken and must begin after the 20 s. It fires once, at the first sample at least kHoldMs after the hold began (30 polls: 2.9 s of samples is not enough).
#pragma once

#include <cstdint>

namespace NpFreeze {

constexpr uint32_t kPollMs = 100;
constexpr uint32_t kIgnoreMs = 20000;
constexpr uint32_t kHoldMs = 3000;

// env: the value of MSC_FREEZE_BUTTON (nullptr: unset); playerBuild: the default when unset.
inline bool Enabled(const char* env, bool playerBuild) {
    if (!env || !*env) return playerBuild;
    return !(env[0] == '0' && env[1] == '\0');
}

struct Chord {
    bool holding = false;
    bool fired = false;
    uint32_t holdStartMs = 0;
    // nowMs: milliseconds since the process started; allDown: L, R and Select all down in this sample; blocked: a screen that owns the pad is open.
    // True exactly once, on the sample that completes the hold.
    bool Step(uint32_t nowMs, bool allDown, bool blocked) {
        if (fired) return false;
        if (!allDown || blocked || nowMs < kIgnoreMs) {
            holding = false;
            return false;
        }
        if (!holding) {
            holding = true;
            holdStartMs = nowMs;
        }
        if (nowMs - holdStartMs >= kHoldMs) {
            fired = true;
            return true;
        }
        return false;
    }
};

} // namespace NpFreeze
