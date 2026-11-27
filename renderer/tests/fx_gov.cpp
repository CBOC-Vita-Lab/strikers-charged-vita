// Host test of the fill governor (vgx_fxgov.h): thins before the ladder moves, stands aside without sprites, comes back, stays put at a steady load.
#include "../src/vgx_fxgov.h"
#include <cstdio>
#include <cstdlib>
using vgx::FxGovernor;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); std::exit(1); } } while (0)
int main() {
    FxGovernor g;
    g.Frame(40, 40, 33.4f, 5, false);
    CHECK(g.level == 0 && !g.Holding(5, false));                      // off
    g.on = true;
    g.Frame(20, 20, 33.4f, 5, false);
    CHECK(g.target == 0 && !g.Holding(5, false));                     // room: nothing
    // an overload with sprites: the ladder waits, both steps come, one at a time
    int firstFull = -1;
    for (int i = 0; i < 40; ++i) {
        g.Frame(31, 31, 33.4f, 5, false);
        if (g.target == 256 && firstFull < 0) firstFull = i;
        CHECK(g.level <= 512);
        if (i < 5) CHECK(g.target == 256 && g.Holding(5, false));
    }
    CHECK(firstFull == 0 && g.target == 512 && g.level == 512);
    CHECK(!g.Holding(5, false));                                      // all spent and still over: the ladder's turn
    // the load falls: one step back after the calm, none in between, the ladder may not go up meanwhile
    for (int i = 0; i < 89; ++i) { g.Frame(20, 20, 33.4f, 5, false); CHECK(g.Holding(5, false) && g.target == 512); }
    g.Frame(20, 20, 33.4f, 5, false);
    CHECK(g.target == 256);
    for (int i = 0; i < 200; ++i) g.Frame(20, 20, 33.4f, 5, false);
    CHECK(g.target == 0 && g.level == 0 && !g.Holding(5, false));
    // between leave and enter: nothing moves
    for (int i = 0; i < 300; ++i) g.Frame(26, 26, 33.4f, 5, false);
    CHECK(g.target == 0);
    // no sprites to thin: never in the way, and it lets go at once
    g.Frame(31, 31, 33.4f, 5, false);
    CHECK(g.target == 256);
    g.Frame(31, 31, 33.4f, 0.2f, false);
    CHECK(g.target == 0 && !g.Holding(0.2f, false));
    // a Mega Strike: no
    for (int i = 0; i < 40; ++i) g.Frame(31, 31, 33.4f, 5, true);
    CHECK(g.level == 0 && !g.Holding(5, true));
    // one slow frame is an overload (ms over the budget): a step, not the ladder
    FxGovernor h;
    h.on = true;
    h.Frame(25, 34, 33.4f, 5, false);
    CHECK(h.target == 256 && h.Holding(5, false));
    // 1.3 FX MSC_FX_LOWRES_MAXH: the effects target's size
    {
        int w = 0, h = 0, e = 0;
        vgx::FxTargetDims(640, 448, 2, 0, w, h, e);   CHECK(w == 320 && h == 224 && e == 2);   // as before
        vgx::FxTargetDims(640, 448, 2, 224, w, h, e); CHECK(w == 320 && h == 224 && e == 2);   // the game's own size: the cap does nothing
        vgx::FxTargetDims(960, 544, 2, 0, w, h, e);   CHECK(w == 480 && h == 272 && e == 2);
        vgx::FxTargetDims(960, 544, 2, 224, w, h, e); CHECK(w == 395 && h == 224 && e == 3);   // 960x544: 88,480 pixels instead of 130,560
        vgx::FxTargetDims(960, 544, 4, 224, w, h, e); CHECK(w == 240 && h == 136 && e == 4);   // a quarter is under the cap already
        vgx::FxTargetDims(961, 545, 2, 0, w, h, e);   CHECK(w == 481 && h == 273 && e == 2);   // rounded up, as BeginFx always did
        vgx::FxTargetDims(448, 448, 2, 100, w, h, e); CHECK(w == 100 && h == 100 && e == 5);
        vgx::FxTargetDims(640, 448, 0, 0, w, h, e);   CHECK(w == 640 && h == 448 && e == 1);
    }
    std::printf("OK\n");
}
