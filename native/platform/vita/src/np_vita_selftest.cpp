// Start-up self-tests of the platform layer's pure logic: the pad mapping, the log-tail line
// splitter and one job-pool call. They take a few microseconds (the pool test a few hundred), run
// on every boot and are what the armhf check build verifies under qemu. (Ported from
// one/vita/src/vita_selftest.cpp.)
#include "np_vita.h"
#include "np_vita_internal.h"

#include <cstdio>
#include <cstring>

namespace VitaPlatform {

bool SelfTest() {
    int failures = 0;
    const auto expect = [&](bool ok, const char* what) {
        if (!ok) {
            ++failures;
            std::fprintf(stderr, "[vita] self-test FAILED: %s\n", what);
        }
    };

    // Pad mapping.
    {
        NpPadState s{};
        VitaInput::Raw raw;
        VitaInput::Map(raw, s);
        expect(s.buttons == 0 && s.stickX == 0 && s.stickY == 0 && s.substickX == 0 && s.substickY == 0,
               "centred pad maps to a neutral GameCube pad");
        raw.buttons = VitaInput::kCross | VitaInput::kStart | VitaInput::kLTrigger | VitaInput::kUp;
        VitaInput::Map(raw, s);
        expect(s.buttons == (0x0100 | 0x1000 | 0x0040 | 0x0008), "Cross/Start/L/Up -> A/Start/L/Up");
        expect(s.triggerL == 255 && s.triggerR == 0, "L sets the analog left trigger");
        raw = {};
        raw.buttons = VitaInput::kSquare | VitaInput::kCircle | VitaInput::kTriangle | VitaInput::kSelect | VitaInput::kRTrigger;
        VitaInput::Map(raw, s);
        expect(s.buttons == (0x0200 | 0x0400 | 0x0800 | 0x0010 | 0x0020),
               "Square/Circle/Triangle/Select/R -> B/X/Y/Z/R");
        raw = {};
        raw.lx = 255; raw.ly = 128;
        VitaInput::Map(raw, s);
        expect(s.stickX == 72 && s.stickY == 0, "stick fully right is +72,0");
        raw.lx = 128; raw.ly = 0;
        VitaInput::Map(raw, s);
        expect(s.stickX == 0 && s.stickY == 72, "stick fully up is 0,+72 (Vita Y is inverted)");
        raw.lx = 0; raw.ly = 255;
        VitaInput::Map(raw, s);
        expect(s.stickX < -45 && s.stickX > -56 && s.stickY < -45 && s.stickY > -56, "full diagonal stays on the +-72 circle");
        raw.lx = 138; raw.ly = 120;
        VitaInput::Map(raw, s);
        expect(s.stickX == 0 && s.stickY == 0, "resting jitter is inside the dead zone");
        raw = {};
        raw.rx = 0; raw.ry = 128;
        VitaInput::Map(raw, s);
        expect(s.substickX == -72 && s.substickY == 0, "right stick drives the C-stick");
        VitaInput::Map(VitaInput::Raw{}, s);
    }

    // Log tail: wrapping and ordering (absent in silent player builds).
#if NP_LOGGING
    {
        // A 120-character line; it also shows up once in the log file.
        char marker[160];
        const char prefix[] = "[vita] log-wrap self-test ";
        std::memset(marker, '.', sizeof(marker));
        std::memcpy(marker, prefix, sizeof(prefix) - 1);
        for (int i = static_cast<int>(sizeof(prefix)) - 1; i < 119; ++i) marker[i] = static_cast<char>('0' + i % 10);
        marker[119] = '>';
        marker[120] = '\n';
        VitaLog::Write(marker, 121);
        static char tail[VitaLog::kTailLines][VitaLog::kTailColumns + 1];
        uint32_t generation = 0;
        const int count = VitaLog::Tail(tail, VitaLog::kTailLines, &generation);
        bool ok = count >= 2;
        if (ok) {
            const char* first = tail[count - 2];
            const char* second = tail[count - 1];
            ok = std::strlen(first) == 80 && first[0] == '[' && std::strlen(second) == 42 &&
                 second[0] == ' ' && second[1] == ' ' && second[41] == '>';
        }
        expect(ok, "a 120-character log line wraps to 80 + indented 40 in the console tail");
    }
#endif

    // Job pool: every item of a small job exactly once (whatever the lane count).
    {
        static uint32_t out[100];
        std::memset(out, 0, sizeof(out));
        NpParallelFor(100, 4, [](void*, size_t b, size_t e, uint32_t) { for (size_t i = b; i < e; ++i) out[i] += 1 + static_cast<uint32_t>(i); }, nullptr);
        bool ok = true;
        for (size_t i = 0; i < 100; ++i) ok = ok && out[i] == 1 + i;
        expect(ok, "the job pool runs every item of a job exactly once");
    }

    std::fprintf(stderr, "[vita] platform self-test: %s\n", failures == 0 ? "passed" : "FAILED");
    return failures == 0;
}

} // namespace VitaPlatform
