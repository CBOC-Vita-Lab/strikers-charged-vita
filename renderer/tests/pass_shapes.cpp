// Host test of the pass-shape list logic (vgx_passshapes.h, held25): counted lines, the legacy reader, 'U' lines, caps, the memory check, the load window, what the pre-make records.
#include "../src/vgx_passshapes.h"
#include <cstdio>
#include <cstdlib>
using namespace vgx::passshapes;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); std::exit(1); } } while (0)
int main() {
    // the Vice intro of the 25 Nov console log: three 751x1072 with depth (f750 one, f805 two), 560x530, 146x146, two 73x73 copies without depth
    const char* intro[] = {"P 751 1072 1\n", "P 560 530 1\n", "P 146 146 1\n", "P 751 1072 1\n", "P 751 1072 1\n", "P 73 73 0\n", "P 73 73 0\n", "garbage\n", "P 8 8 0\n", "P 4000 100 1\n", "P 582 293 1\n"};
    List c; c.counted = true;
    for (const char* l : intro) c.AddLine(l);
    CHECK(c.v.size() == 5);
    CHECK(c.v[0].w == 751 && c.v[0].h == 1072 && c.v[0].depth && c.v[0].n == 3);   // counted: three objects of the atlas size
    CHECK(c.Find(73, 73, false) && c.Find(73, 73, false)->n == 2);
    CHECK(c.TotalObjects() == 3 + 1 + 1 + 2 + 1);
    // the legacy reader (INTRO off): once a size, 'U' ignored, damaged and out-of-range lines skipped
    List o;
    for (const char* l : intro) o.AddLine(l);
    CHECK(o.v.size() == 5 && o.TotalObjects() == 5 && o.v[0].n == 1);
    CHECK(!o.AddLine("U 73 73\n") && o.Find(73, 73, true) == nullptr);
    // a cap: kMaxPer objects of one shape, no more however many lines
    List k; k.counted = true;
    for (int i = 0; i < 10; ++i) k.AddLine("P 751 1072 1\n");
    CHECK(k.v.size() == 1 && k.v[0].n == kMaxPer);
    // 'U': an object made without depth that got one is one object, not two
    List u; u.counted = true;
    u.AddLine("P 640 448 0\n"); u.AddLine("U 640 448\n");
    CHECK(u.v.size() == 1 && u.v[0].depth && u.v[0].n == 1);
    u.AddLine("P 640 448 0\n"); u.AddLine("P 640 448 0\n"); u.AddLine("U 640 448\n");
    CHECK(u.Find(640, 448, true) && u.Find(640, 448, true)->n == 2 && u.Find(640, 448, false) && u.Find(640, 448, false)->n == 1);
    List u2; u2.counted = true;
    u2.AddLine("U 100 100\n");   // (no earlier line: the object is listed with depth)
    CHECK(u2.v.size() == 1 && u2.v[0].depth);
    // the shape count cap of the list (distinct sizes)
    List m; m.counted = true;
    for (int i = 0; i < kMaxShapes + 5; ++i) { char b[40]; std::snprintf(b, sizeof b, "P %d 64 1\n", 100 + i); m.AddLine(b); }
    CHECK(static_cast<int>(m.v.size()) == kMaxShapes);
    // objects to make
    CHECK(Missing(3, 0) == 3 && Missing(3, 1) == 2 && Missing(3, 3) == 0 && Missing(1, 5) == 0);
    // memory: the cap on what the pre-make takes and the floor that must remain
    CHECK(ObjectKib(751, 1072, true) == 6289 && ObjectKib(73, 73, false) == 20);
    CHECK(MemoryOk(0, 6289, 16384, 71000, 20480) && !MemoryOk(12000, 6289, 16384, 71000, 20480));   // over the cap
    CHECK(!MemoryOk(0, 6289, 16384, 25000, 20480) && MemoryOk(0, 6289, 16384, 26769, 20480) && MemoryOk(0, 6289, 16384, -1, 20480));   // floor + the object itself; unknown memory passes
    // the load window: the load of a stadium, not in a match, not in play, after the first part of the load; closed with the switch off
    CHECK(LoadWindow(true, true, false, false, 150, 150) && !LoadWindow(true, true, false, false, 149, 150));
    CHECK(!LoadWindow(false, true, false, false, 900, 150) && !LoadWindow(true, false, false, false, 900, 150));
    CHECK(!LoadWindow(true, true, true, false, 900, 150) && !LoadWindow(true, true, false, true, 900, 150));
    // what is recorded: the pre-make's own objects never; INTRO from the match's start; the old way only after kick-off
    CHECK(!RecordCreation(true, true, true, true));
    CHECK(RecordCreation(false, true, true, false) && !RecordCreation(false, true, false, false));
    CHECK(!RecordCreation(false, false, true, false) && RecordCreation(false, false, true, true));
    std::printf("pass_shapes: ok\n");
    return 0;
}
