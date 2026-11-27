// wave 10 (warm10): the warm-up's heap rule (vgx_warmspread.h Stop) and the spread queue (Queue::Run), no GL.
#include "vgx_warmspread.h"
#include <cstdio>
#include <cstdlib>

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { ++fails; std::printf("FAIL: %s (line %d)\n", m, __LINE__); } } while (0)

using vgx::warm::Why;
using vgx::warm::Stop;

int main() {
    const unsigned total = 98304u;   // the Vita's C heap, KiB
    // the old absolute rule, explicit: stop at 44 MiB in use
    CHECK(Stop(44, 24, 43u * 1024u, total, true, true) == Why::Go, "abs: under the figure goes");
    CHECK(Stop(44, 24, 44u * 1024u, total, true, true) == Why::Absolute, "abs: at the figure stops");
    CHECK(Stop(0, 24, 90u * 1024u, total, true, true) == Why::Go, "abs 0: never stops, floor ignored");
    // unset: the floor on the free heap. At the game's start-up use (52 MiB in use, 44 MiB free) the warm-up must run (the r7..r13 bug: stopped at picture 0)
    CHECK(Stop(-1, 24, 52u * 1024u, total, true, true) == Why::Go, "floor: 52 MiB in use (44 free) goes");
    CHECK(Stop(-1, 24, 72u * 1024u - 1u, total, true, true) == Why::Go, "floor: just over 24 MiB free goes");
    CHECK(Stop(-1, 24, 72u * 1024u, total, true, true) == Why::Go, "floor: exactly 24 MiB free goes");
    CHECK(Stop(-1, 24, 72u * 1024u + 1u, total, true, true) == Why::Floor, "floor: under 24 MiB free stops");
    CHECK(Stop(-1, 24, 99999u, total, true, true) == Why::Floor, "floor: use over the heap size stops (no wrap)");
    CHECK(Stop(-1, 0, 97u * 1024u, total, true, true) == Why::Go, "floor 0: no floor");
    CHECK(Stop(-1, 24, 97u * 1024u, 0, true, false) == Why::Go, "no total accessor (host): never stops");
    CHECK(Stop(-1, 24, 97u * 1024u, total, false, true) == Why::Go, "no used accessor (host): never stops");
    CHECK(Stop(44, 24, 97u * 1024u, 0, true, false) == Why::Absolute, "abs needs no total");

    if (fails) { std::printf("warm_spread: %d FAILED\n", fails); return 1; }
    std::puts("warm_spread: PASS");
    return 0;
}
