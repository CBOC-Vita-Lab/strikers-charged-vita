// Wave 10: the frames-in-flight gate's drain rule (np_fps_fif.h). NP_FIF_BREAK 1-3 must fail.
#include "np_fps_fif.h"
#include <cstdio>
using npfps::FifWaits;
static int g_fail = 0;
static void check(bool ok, const char* w) { if (!ok) { std::printf("FAIL: %s\n", w); ++g_fail; } }
int main() {
    check(FifWaits(false, true, 3, 2, true) && !FifWaits(false, true, 2, 2, true), "not standing aside: waits above the depth, as before");
    check(!FifWaits(true, true, 9, 2, false), "standing aside without the switch: never waits (as before)");
    check(FifWaits(true, true, 4, 2, true), "a held 30 standing aside drains a backlog of depth + 2");
    check(!FifWaits(true, true, 3, 2, true), "... and leaves depth + 1 alone (no wait at every frame)");
    check(!FifWaits(true, false, 9, 2, true), "a free run standing aside does not drain (it is throughput-bound: waiting costs pictures)");
    // a backlog of 6 after a long present falls to depth + 1 in four frames, one refresh a frame
    int backlog = 6, frames = 0;
    while (FifWaits(true, true, backlog, 2, true) && frames < 20) { --backlog; ++frames; }
    check(backlog == 3 && frames == 3, "a backlog of 6 drains to depth + 1 (3) in three frames");
    std::printf("%s: fif_drain_unit\n", g_fail ? "FAILED" : "passed");
    return g_fail ? 1 : 0;
}
