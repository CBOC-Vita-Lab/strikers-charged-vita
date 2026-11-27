// MSC_VGX_MIDFLUSH scheduling rule (src/vgx_midflush.h): flush counts and places for the three modes; never a flush with no draw recorded.
#include "vgx_midflush.h"
#include <cstdio>
using namespace vgx::midflush;
static int g_fail = 0;
static void Check(bool ok, const char* w) { if (!ok) { std::printf("FAIL: %s\n", w); ++g_fail; } }
int main() {
    Check(!Parse(0).On() && !Parse(20005).On() && Parse(7).mode == 0 && Parse(7).n == 7 && Parse(10000).n == 64 && Parse(30000).n == 2 && Parse(30099).n == 8 && Parse(5000).n == 4095, "parse");
    { // mode 0: every 4th draw, wherever
        Sched s; s.cfg = Parse(4); s.SceneBoundary(false, true);
        int fl = 0;
        for (int i = 0; i < 20; ++i) if (s.AfterDraw()) { ++fl; s.Flushed(); }
        Check(fl == 5, "mode 0: 20 draws, every 4th: 5 flushes");
    }
    { // mode 1: 10 draws then the first program change; all draws same program: forced after max(8, 5) = 8 more
        Sched s; s.cfg = Parse(10010); s.SceneBoundary(false, true);
        int boundary = 0, forced = 0, draws = 0;
        for (int i = 0; i < 60; ++i) {
            const bool diff = (i == 12);   // one program change, two draws after the chunk was full
            const int a = s.BeforeDraw(diff);
            if (a) { (a == 1 ? boundary : forced)++; s.Flushed(); Check(s.draws == 0, "flushed: counter reset"); }
            ++draws; (void)s.AfterDraw();
        }
        Check(boundary == 1, "mode 1: the change at draw 12 takes one boundary flush");
        Check(forced >= 2, "mode 1: with no change the flush is forced after the cap (mutant: never)");
        Check(draws == 60, "draws");
    }
    { // mode 3: k = 2 flushes in the scene, chunk = bigPrev / 3 (>= 24); the first picture (no bigPrev) flushes nothing
        Sched s; s.cfg = Parse(30002); s.SceneBoundary(false, true);
        int fl = 0;
        for (int i = 0; i < 300; ++i) { if (s.BeforeDraw(i % 7 == 0)) { ++fl; s.Flushed(); } (void)s.AfterDraw(); }
        Check(fl == 0, "mode 3: no flush before a previous picture is known");
        s.PictureEnd(); s.SceneBoundary(true, true);
        Check(s.chunk == 100, "mode 3: chunk = 300 / 3");
        for (int i = 0; i < 300; ++i) { if (s.BeforeDraw(i % 7 == 0)) { ++fl; s.Flushed(); } (void)s.AfterDraw(); }
        Check(fl == 2, "mode 3: exactly k flushes in the scene");
        s.SceneBoundary(true, false);
        Check(s.chunk == 0 && !s.pending, "a scene that is not the EFB's is never flushed");
    }
    std::printf(g_fail ? "FAILED\n" : "PASS\n");
    return g_fail ? 1 : 0;
}
