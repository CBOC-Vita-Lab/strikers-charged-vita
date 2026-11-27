// MSC_DYNRES_LATEHOLD: when a late frame asks for a step down and how long the hold lasts (vgx_dynres_late.h).
#include "../src/vgx_dynres_late.h"
#include <cstdio>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAILED line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

static vgx::LateHold Make(float holdMs = 2000.0f) { vgx::LateHold h; h.on = true; h.holdMs = holdMs; return h; }
constexpr float kB = 16.68f;   // the 60 budget

int main() {
    {   // off: never asks, never holds
        vgx::LateHold h;
        for (int i = 0; i < 100; ++i) CHECK(!h.Frame(3 * kB, kB, kB, false));
        CHECK(!h.want && !h.Holding());
    }
    {   // frames at the budget are not late; one just over it asks at once and holds
        auto h = Make();
        for (int i = 0; i < 100; ++i) CHECK(!h.Frame(kB, kB, kB, false));
        CHECK(!h.Holding());
        CHECK(h.Frame(kB * 1.01f, kB, kB, false));
        CHECK(h.want && h.Holding());
        CHECK(h.wantMs > kB);
    }
    {   // the hold counts clock time after the LAST late frame: 2000 ms at 16.68 ms a frame is 120 frames
        auto h = Make();
        h.Frame(2 * kB, kB, kB, false);
        h.Took(true);
        int n = 0;
        while (h.Holding() && n < 1000) { h.Frame(0.5f * kB, kB, kB, false); ++n; }
        CHECK(n == 120);
        // a late frame inside the hold starts it again
        h.Frame(2 * kB, kB, kB, false);
        h.Took(true);
        for (int i = 0; i < 100; ++i) h.Frame(0.5f * kB, kB, kB, false);
        CHECK(h.Holding());
        h.Frame(2 * kB, kB, kB, false);   // renewed at 100 frames in
        for (int i = 0; i < 119; ++i) h.Frame(0.5f * kB, kB, kB, false);
        CHECK(h.Holding());
        h.Frame(0.5f * kB, kB, kB, false);
        CHECK(!h.Holding());
        CHECK(h.drops == 2);
    }
    {   // slow frames count their own clock time (dt), not the budget
        auto h = Make(1000.0f);
        h.Frame(2 * kB, kB, kB, false);
        h.Took(true);
        int n = 0;
        while (h.Holding() && n < 1000) { h.Frame(0.5f * kB, kB, 100.0f, false); ++n; }
        CHECK(n == 10);
    }
    {   // the frames queued at the old size (coolFrames) renew the hold but do not ask again
        auto h = Make();
        CHECK(h.Frame(2 * kB, kB, kB, false));
        h.Took(true);
        for (int i = 0; i < h.coolFrames - 1; ++i) CHECK(!h.Frame(2 * kB, kB, kB, false));
        CHECK(!h.want);
        CHECK(h.Frame(2 * kB, kB, kB, false));   // still late after the step was measured: asks for the next one
        CHECK(h.want);
    }
    {   // the request lapses with the hold if the ladder never took it (the size could not change)
        auto h = Make(100.0f);
        h.Frame(2 * kB, kB, kB, false);
        CHECK(h.want);
        for (int i = 0; i < 10; ++i) h.Frame(0.5f * kB, kB, kB, false);
        CHECK(!h.want && !h.Holding());
    }
    {   // blocked (a Mega Strike, a paced scene, the 30 cap without MSC_DYNRES_LATE30): nothing asked, the hold ends
        auto h = Make();
        h.Frame(2 * kB, kB, kB, false);
        CHECK(h.Holding());
        CHECK(!h.Frame(3 * kB, kB, kB, true));
        CHECK(!h.want && !h.Holding());
    }
    {   // the line: at = 0.95
        auto h = Make();
        h.at = 0.95f;
        CHECK(!h.Frame(0.94f * kB, kB, kB, false));
        CHECK(h.Frame(0.96f * kB, kB, kB, false));
    }
    std::printf("dynres_late: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
