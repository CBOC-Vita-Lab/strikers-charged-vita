// popin16: the walk-out window rule (vgx_popin.h). Build with -DVGX_POPIN_BREAK=1|2 to see a broken rule fail.
#include "vgx_popin.h"
#include <cstdio>
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)
int main() {
    using namespace vgx::popin;
    Window w;
    CHECK(!Active(w, 90));                                   // no match: closed
    w.inMatch = true; w.serial = 100;
    CHECK(Active(w, 90));                                    // walk-out, before kick-off
    w.kicked = true; w.kickSerial = 400; w.serial = 400;
    CHECK(Active(w, 90));                                    // kick-off picture
    w.serial = 489;
    CHECK(Active(w, 90));                                    // last picture of the tail
    w.serial = 490;
    CHECK(!Active(w, 90));                                   // live play pays nothing
    w.serial = 5000;
    CHECK(!Active(w, 90));
    CHECK(ForceWhole(true, true));
    CHECK(!ForceWhole(false, true));
    CHECK(!ForceWhole(true, false));
    std::printf(g_fail ? "popin: FAILED\n" : "popin: ok\n");
    return g_fail ? 1 : 0;
}
