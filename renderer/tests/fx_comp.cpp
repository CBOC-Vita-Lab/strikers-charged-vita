// Host test of the effects compositor's rules (vgx_fxcomp.h) and of the governor's accumulation step (vgx_fxgov.h): the size rule, the divisor from the fill, the cost
// model, the order of the governor's steps. FXCOMP_SRC names the directory of the headers (the run script points it at a mutated copy to prove the test can fail).
#include "vgx_fxcomp.h"
#include "vgx_fxgov.h"
#include <cstdio>
#include <cstdlib>
using namespace vgx;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); std::exit(1); } } while (0)
int main() {
    // size rule: a triangle's mean fill against the threshold; the boundary is large; unmeasurable follows the caller
    CHECK(fxcomp::Big(900.0 * 2, 2, 900.0, false));            // one quad at exactly the threshold
    CHECK(fxcomp::Big(5000.0, 2, 900.0, false));               // a big sprite
    CHECK(!fxcomp::Big(1799.0, 2, 900.0, false));              // just under
    CHECK(!fxcomp::Big(40.0 * 100, 200, 900.0, true));         // 100 sparks of 40 px each: small, even when the caller trusts unknowns
    CHECK(fxcomp::Big(1e6, 6, 900.0, false));                  // a 3-triangle-pair mist
    CHECK(!fxcomp::Big(-1.0, 4, 900.0, false) && fxcomp::Big(-1.0, 4, 900.0, true));   // not measurable
    CHECK(!fxcomp::Big(0.0, 0, 900.0, false) && fxcomp::Big(0.0, 0, 900.0, true));     // no triangles
    // divisor: half while the fill at half size is within 2 screens (8 screens), then a quarter; the governor's steps double it, never past 8
    CHECK(fxcomp::Div(0.0, 0) == 2 && fxcomp::Div(5.9, 0) == 2 && fxcomp::Div(8.0, 0) == 2);   // Pipeline's mist
    CHECK(fxcomp::Div(8.01, 0) == 4 && fxcomp::Div(40.0, 0) == 4);                              // a Mega Save
    CHECK(fxcomp::Div(5.9, 1) == 4 && fxcomp::Div(40.0, 1) == 8 && fxcomp::Div(40.0, 5) == 8);
    CHECK(fxcomp::GameDiv(8) == 4 && fxcomp::GameDiv(2) == 2);
    // the total stays within about 2 screens for every fill up to 32 screens at the chosen divisor (the sprites' own part)
    for (double f = 0.1; f <= 32.0; f += 0.1) {
        const int d = fxcomp::Div(f, 0);
        CHECK(f / (d * d) <= 2.0 + 1e-9);
    }
    // the cost model: a small fill does not pay (the composite is a screen), 6 screens at half do
    CHECK(fxcomp::Cost(1.0, 2) >= 1.0);
    CHECK(fxcomp::Cost(5.9, 2) < 5.9 && fxcomp::Cost(40.0, 4) < 40.0 / 10.0);
    // MSC_FX_COMPOSITE_BOX: the depth copy and the composite cover a share of the picture; share 1 is the old cost, a smaller share costs less and lets a smaller fill pay
    CHECK(fxcomp::Cost(3.0, 2, 1.0) == fxcomp::Cost(3.0, 2));
    CHECK(fxcomp::Cost(3.0, 2, 0.25) < fxcomp::Cost(3.0, 2) && fxcomp::Cost(1.2, 2) >= 1.2 && fxcomp::Cost(1.2, 2, 0.2) < 1.2);
    // the governor: the accumulation step comes before any sprite is thinned, holds the ladder, comes back last
    FxGovernor g;
    g.on = true;
    g.accumOn = true;
    int firstAccum = -1, firstThin = -1;
    for (int i = 0; i < 60; ++i) {
        g.Frame(31, 31, 33.4f, 5, false);
        if (g.accum == 1 && firstAccum < 0) firstAccum = i;
        if (g.target >= 256 && firstThin < 0) firstThin = i;
        if (g.accum == 0) CHECK(g.target == 0);                 // never thinning without the accumulation step first
    }
    CHECK(firstAccum == 0 && firstThin > firstAccum);
    CHECK(g.accum == 1 && g.target == 512 && !g.Holding(5, false));   // all steps spent and still over: the ladder's turn
    for (int i = 0; i < 89; ++i) { g.Frame(20, 20, 33.4f, 5, false); CHECK(g.Holding(5, false)); }
    int accumBackAt = -1;
    for (int i = 0; i < 600; ++i) {
        g.Frame(20, 20, 33.4f, 5, false);
        if (g.accum == 0 && accumBackAt < 0) { accumBackAt = i; CHECK(g.target == 0); }   // the accumulation is the last to come back
    }
    CHECK(accumBackAt > 0 && !g.Holding(5, false));
    // without accumOn the governor is exactly the old one
    FxGovernor o;
    o.on = true;
    for (int i = 0; i < 40; ++i) o.Frame(31, 31, 33.4f, 5, false);
    CHECK(o.accum == 0 && o.target == 512);
    // Mega Strike without MSC_FX_GOV_MEGA (blocked) and no sprites: stands aside, accumulation back at once
    g.Frame(31, 31, 33.4f, 5, false);
    g.Frame(31, 31, 33.4f, 0.2f, false);
    CHECK(g.accum == 0 && g.target == 0);
    std::printf("fx_comp: OK\n");
    return 0;
}
