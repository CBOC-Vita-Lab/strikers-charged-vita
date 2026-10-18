// Drives the harness core with the mock state (tests/mock_state.cpp): one simulated frame per loop
// iteration, the harness's pad printed as the game would read it. Run by tests/run_tests.sh with
// different environments; the harness leaves the process with its own exit codes.
#include "np_harness.h"
#include "np_time.h"
#include <cstdio>
#include <cstdlib>

namespace NpHarness { void MockSetFrame(uint64_t); }

// A library constructor that reads the (interposed) clock before the harness' own static initialisers, as Mesa / libstdc++ do in
// the game: the main thread must still be recognised as the game thread afterwards (MSC_EARLY_READ=1 enables it for the test).
#pragma GCC diagnostic ignored "-Wprio-ctor-dtor"
__attribute__((constructor(100))) static void EarlyClockRead() {
    if (std::getenv("MSC_EARLY_READ")) (void)NpMonotonicNs();
}

int main(int argc, char** argv) {
    const uint64_t frames = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 200;
    std::printf("start active=%d virtual=%d\n", NpHarnessActive(), NpVirtualTimeActive());
    for (uint64_t f = 1; f <= frames; ++f) {
        NpHarness::MockSetFrame(f - 1);
        NpPadState pad{};
        const int over = NpHarnessFillPad(&pad);
        std::printf("pad f=%llu over=%d buttons=%04x s=%d,%d c=%d,%d trig=%d,%d t=%lld\n", (unsigned long long)f, over, pad.buttons,
                    pad.stickX, pad.stickY, pad.substickX, pad.substickY, pad.triggerL, pad.triggerR, (long long)NpMonotonicNs());
        NpHarnessRaw raw{};   // W5 CTL: a Vita-terms command
        if (NpHarnessFillRaw(&raw))
            std::printf("raw f=%llu buttons=%04x s=%d,%d c=%d,%d touch=%d %d@%.0f,%.0f rear=%d %d@%.0f,%.0f\n", (unsigned long long)f,
                        (unsigned)raw.buttons, raw.stickX, raw.stickY, raw.substickX, raw.substickY, raw.touchCount, raw.touch[0].id,
                        raw.touch[0].x, raw.touch[0].y, raw.rearCount, raw.rear[0].id, raw.rear[0].x, raw.rear[0].y);
        std::fflush(stdout);
        NpHarness::MockSetFrame(f);
        NpOnFramePresented();
    }
    std::printf("loop ended after %llu frames\n", (unsigned long long)frames);
    return 0;
}
