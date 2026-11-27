#include <cstdlib>
// vgx_gpubusy.h on traces: a display scene that waits for a buffer must not make the GPU look busy; real work must stay.
#include "vgx_gpubusy.h"
#include <cstdio>
#include <cstring>
using namespace vgx::gpubusy;
static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { ++fails; std::printf("FAIL: %s\n", m); } } while (0)
int main(int argc, char** argv) {
    int brk = 0;
    for (int i = 1; i < argc; ++i) if (!std::strncmp(argv[i], "--break=", 8)) brk = std::atoi(argv[i] + 8);
    Config c;
    if (brk == 1) c.on = false;               // fix off: the trace below must then fail
    if (brk == 2) c.blitUs = 0;               // no blit allowance: a real blit is dropped
    // 3D scenes 6 ms, display scene held 10.7 ms waiting for a buffer: busy == the 16.7 ms period
    uint32_t v = CorrectedUs(c, 16700, 10700, 4);
    CHECK(v <= 8500, "busy == period with short 3D scenes: corrected figure must be 3D scenes plus the blit (<= 8.5 ms)");
    CHECK(v >= 7900 || brk, "the blit is still counted (>= 7.9 ms)");
    // a display scene that is only the blit: nothing subtracted
    CHECK(CorrectedUs(c, 9000, 1800, 3) == 9000 || brk == 1, "display scene under the blit allowance: unchanged");
    // one scene: the display scene is the whole picture, keep raw
    CHECK(CorrectedUs(c, 15000, 15000, 1) == 15000, "single scene keeps its raw time");
    // an older library (display_us 0) and nonsense (display > busy): raw
    CHECK(CorrectedUs(c, 12000, 0, 5) == 12000, "no display_us: raw");
    CHECK(CorrectedUs(c, 5000, 6000, 5) == 5000, "display_us above busy: raw");
    // real GPU overload: 3D scenes 18 ms, display 2 ms: still over the budget after the correction (governor must still step down)
    CHECK(CorrectedUs(c, 20000, 2000, 4) >= 17000, "a true overload stays an overload");
    // ladder decision on a 120-frame trace: busy == period while the 3D part is 7 ms. Raw would step down at 0.90*16.7 = 15.0 ms.
    int stepsRaw = 0, stepsFix = 0;
    for (int i = 0; i < 120; ++i) {
        const uint32_t busy = 16700, disp = 16700 - 7000;
        if (busy * 0.001 > 0.90 * 16.7) ++stepsRaw;
        if (CorrectedUs(c, busy, disp, 4) * 0.001 > 0.90 * 16.7) ++stepsFix;
    }
    CHECK(stepsRaw == 120, "(model) raw busy would call every frame over");
    CHECK(stepsFix == 0, "corrected busy calls none over, and leaves headroom for a step up (< 0.80 of the budget)");
    CHECK(CorrectedUs(c, 16700, 9700, 4) * 0.001 < 0.80 * 16.7, "step-up headroom visible after the correction");
    std::printf(fails ? "gpubusy: %d FAILED\n" : "gpubusy: all checks passed\n", fails);
    return fails ? 1 : 0;
}
