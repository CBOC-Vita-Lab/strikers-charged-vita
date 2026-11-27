// split17: the held-present counter pair (vgx_split17.h) on simulated presents with every early exit of the split path. --break=N: a deliberate fault the checks must catch (exit code != 0).
//   1: one Finish dropped   2: a Begin whose buffer ends with the display still open and nobody finishes it   3: a Finish twice
#include "vgx_split17.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
using namespace vgx::split17;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
static int g_break = 0;
// One buffer as ExecuteEx sees it: the exits are the ones the back end has (hidden, skipped draws, capture: no Begin at all; failed Begin: no count; ok Begin: HudEnd at the copy or at the buffer end).
enum Exit { HIDDEN, SKIPDRAWS, CAPTURE, BEGIN_FAILED, NORMAL, ENDS_OPEN, SCENE_CHANGE, PAUSE, LOADING };
static void Present(Exit e, int n) {
    bool hudOn = false;
    switch (e) {
    case HIDDEN: case SKIPDRAWS: case CAPTURE: case BEGIN_FAILED: case LOADING: return;   // no split run planned, or the host refused: a normal present
    case NORMAL: case SCENE_CHANGE: case PAUSE: Begin(); hudOn = true; break;
    case ENDS_OPEN: Begin(); hudOn = true; break;   // the copy never came
    }
    if (e == ENDS_OPEN) { if (g_break == 2) return; if (BufferEnd(hudOn)) Finish(); return; }   // the fixed path: BufferEnd says finish
    if (g_break == 1 && n == 7) return;   // (n == 7 is a NORMAL present)
    Finish();
    if (g_break == 3 && n == 9) Finish();
}
int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) if (!std::strncmp(argv[i], "--break=", 8)) g_break = std::atoi(argv[i] + 8);
    Reset();
    const Exit seq[] = {NORMAL, HIDDEN, NORMAL, SKIPDRAWS, CAPTURE, BEGIN_FAILED, NORMAL, NORMAL, SCENE_CHANGE, NORMAL, PAUSE, LOADING, NORMAL};
    int n = 0;
    for (int rep = 0; rep < 50; ++rep) for (Exit e : seq) Present(e, ++n);
    CHECK(Balanced());
    CHECK(S().held == 50u * 8u);   // 8 of the 13 exits in the sequence begin a split run
    // a buffer that ends with the display standing in: counted as a leak ONCE, repaired by the caller's Finish, never left open
    Reset();
    Present(ENDS_OPEN, 1);
    CHECK(S().leaked == 1 && S().held == 1 && S().finished == 1 && !S().open);
    // BufferEnd with nothing open and no display stand-in: no action
    Reset();
    CHECK(!BufferEnd(false));
    CHECK(Balanced());
    // a Begin while one is open is a leak, a Finish with nothing is stray
    Reset(); Begin(); Begin(); CHECK(S().leaked == 1); Finish(); CHECK(!Balanced());
    Reset(); Finish(); CHECK(S().stray == 1 && !Balanced());
    if (g_fail) { std::printf("split17 test: %d FAILED%s\n", g_fail, g_break ? " (as the mutant should)" : ""); return 1; }
    std::printf("split17 test: PASS\n");
    return 0;
}
