// wave 17: the headroom line's percentile and window code (np_head17.h). NP_HEAD17_BREAK 1-8 must fail (5: pace18's join, 6-7: scenes30's moments and held limit, 8: heldcpu24's frame-send join not taken out of the work).
//   g++ -std=c++11 -Wall -Inative/platform/compat native/tools/fps/head17_unit.cpp -o /tmp/head17_unit && /tmp/head17_unit
#include "np_head17.h"
#include <cstdio>
#include <cstring>
using namespace nphead;
static int g_fail = 0;
static void check(bool ok, const char* w) { if (!ok) { std::printf("FAIL: %s\n", w); ++g_fail; } }
int main() {
    { float a[21]; for (int i = 0; i < 21; ++i) a[i] = (float)(20 - i); check(Pct(a, 21, 95) == 19.0f, "p95 of 0..20 (21 samples) is the 20th smallest = 19"); }
    { float a[100]; for (int i = 0; i < 100; ++i) a[i] = (float)(99 - i); check(Pct(a, 100, 95) == 94.0f && Pct(a, 100, 50) == 49.0f, "p95 / p50 of 0..99 are 94 / 49 (nearest rank)"); }
    { float a[1] = {7}; check(Pct(a, 1, 95) == 7.0f && Pct(a, 0, 95) < 0, "one sample is its own percentile; none gives -1"); }
    Stat s;
    s.Init();
    check(s.Ready(), "init");
    // 100 play pictures: 80 with a step at 10 ms (one at 17.5), 20 without at 3 ms; GPU lane 16 x 12 ms and 4 x 20 ms; render lane 9 ms
    for (int i = 0; i < 100; ++i) s.Picture(kPlay, i == 7 ? 17.5f : i == 11 ? 16.3f : (i % 5 ? 10.0f : 3.0f), i % 5 ? 1 : 0, i == 3, false);
    for (int i = 0; i < 20; ++i) { s.LaneNote(2, i < 4 ? 20.0f : 12.0f); s.LaneNote(1, 9.0f); }
    s.Picture(kReplay, 5.0f, 1, false, true);   // a replay picture: the lane samples above belong to the class current when noted (the last Picture before them: play)
    s.Pull();
    ClassWin& w = s.Win(kPlay);
    check(w.pics == 100 && w.step.n == 80 && w.nostep.n == 20, "pictures with / without a step counted apart");
    check(w.gameOver == 1 && w.late == 1, "one game picture over 16.7 (17.5) and one late");
    // the lane samples were noted after 100 play pictures and before the replay picture: they are play's
    check(w.gpu.n == 20 && w.gpuOver == 4 && w.rt.n == 20 && w.rtOver == 0, "renderer lanes: 20 samples each of the class current when noted, 4 GPU pictures over 16.7");
    char line[1300];
    check(s.Line(kPlay, line, sizeof line, true, 60, 0, 30.0) > 0, "a line for play");
    std::printf("%s\n", line);
    check(std::strstr(line, "with a step") && std::strstr(line, "class play (lock60 1)"), "format: class and counts");
    check(std::strstr(line, "with step p50 10.0 p95 10.0 max 17.5"), "game ms with a step: p50 10, p95 10 (79 of 80 at 10), max 17.5");
    check(std::strstr(line, "without p50 3.0 p95 3.0 max 3.0"), "game ms without a step");
    check(std::strstr(line, "GPU ms p95 20.0 max 20.0 (n 20)") != nullptr, "GPU p95 of 16 x 12 + 4 x 20 (nearest rank: 19th of 20) is 20");
    check(std::strstr(line, "over 16.7 ms: game 1 render 0 GPU 4: GPU"), "over counts and the verdict");
    check(w.pics == 0, "the line clears the window");
    check(s.Line(kReplay, line, sizeof line, true, 60, 0, 30.0) > 0 && std::strstr(line, "class replay"), "the replay picture has its own line");
    check(std::strstr(line, "relief: policy 60 now, 1 pictures at 30") != nullptr, "relief pictures counted");
    check(s.Line(kCut, line, sizeof line, false, 60, 0, 30.0) == 0, "no pictures, no line");
    // pace18: pictures that kicked a step on np_sim carry its busy ms and the join wait; 10 pictures, step 7 ms, join waited 4 ms in 8 and 0.01 ms in 2
    for (int i = 0; i < 10; ++i) s.Picture(kCut, 12.0f, 1, false, false, 7.0f, i < 8 ? 4.0f : 0.01f);
    check(s.Line(kCut, line, sizeof line, false, 60, 0, 30.0) > 0, "a line for the cut-scene class with sim pictures");
    check(std::strstr(line, "sim thread (MSC_SIMT) busy ms p50 7.0 p95 7.0 max 7.0, join wait ms p95 4.0 max 4.0, joins that waited 8 of 10") != nullptr, "sim busy ms, join wait p95 / max and the joins that waited");
    s.Picture(kCut, 5.0f, 0, false, false);
    check(s.Line(kCut, line, sizeof line, false, 60, 0, 30.0) > 0 && std::strstr(line, "sim thread") == nullptr, "no sim pictures: the line has no sim part (the line of MSC_SIMT=0 is unchanged)");
    // scenes30: the moments (np_head17.h MomentOf) and the held classes' limit
    check(MomentOf(false, false, true, false, false, 1) == kWalkout && MomentOf(false, false, true, false, false, 2) == kCeleb && MomentOf(false, false, true, false, false, 3) == kMegaCut, "presentation moments: walk-out, celebration, mega-cut");
    check(MomentOf(false, false, true, false, false, 0) == kCut, "an unknown presentation function in a cut-scene stays cut-scene");
    check(MomentOf(false, false, false, true, true, 0) == kMega && MomentOf(false, false, true, false, true, 3) == kMega, "the Mega Strike's own scene is mega, in play or in a cut-scene task");
    check(MomentOf(true, true, true, true, true, 1) == kOther && MomentOf(false, true, true, false, true, 2) == kReplay, "pause first, replay before mega and the cut-scene");
    check(MomentOf(false, false, false, true, false, 0) == kPlay && MomentOf(false, false, false, false, false, 0) == kOther, "play and other");
    check(std::strcmp(ClassName(kWalkout), "walk-out") == 0 && std::strcmp(ClassName(kMegaCut), "mega-cut") == 0 && std::strcmp(ClassName(kCeleb), "celebration") == 0, "class names (one word each: abpair's [head] pattern)");
    for (int i = 0; i < 10; ++i) s.Picture(kWalkout, i < 3 ? 40.0f : 20.0f, 0, i < 3, false);
    for (int i = 0; i < 10; ++i) s.LaneNote(2, i < 4 ? 45.0f : 25.0f);
    s.Pull();
    check(s.Line(kWalkout, line, sizeof line, false, 30, 0, 30.0) > 0, "a walk-out line");
    std::printf("%s\n", line);
    check(std::strstr(line, "class walk-out") && std::strstr(line, "over 33.4 ms: game 3 render 0 GPU 4: GPU"), "a held class counts over 33.4 ms: 3 game pictures at 40, 4 GPU at 45 (the 20 / 25 ms ones are under one 30 period)");
    // heldcpu24: the frame-send join's wait is inside game ms; 10 replay pictures of 40 ms of which 16 ms waited at the join in 8 and 0.1 ms in 2: the line says so and gives the work
    for (int i = 0; i < 10; ++i) s.Picture(kReplay, 40.0f, 0, false, false, -1.0f, -1.0f, i < 8 ? 16.0f : 0.1f);
    check(s.Line(kReplay, line, sizeof line, false, 30, 0, 30.0) > 0, "a replay line with the frame-send join");
    std::printf("%s\n", line);
    check(std::strstr(line, "game ms: with step p50 -1.0 p95 -1.0 max 0.0, without p50 40.0") != nullptr, "game ms keeps its meaning (the join is inside it)");
    check(std::strstr(line, "frame-send join wait (inside game ms, not counted as a wait) mean 12.8 max 16.0 ms, 8 of 10 pictures waited over 0.5 ms; game ms less that wait (the work) p50 24.0 p95 39.9") != nullptr, "join mean / max / waited and the work p50 / p95");
    s.Picture(kReplay, 5.0f, 0, false, false);
    check(s.Line(kReplay, line, sizeof line, false, 30, 0, 30.0) > 0 && std::strstr(line, "frame-send join") == nullptr, "no join figure (no frame-send worker): the line of other runs is unchanged");
    if (g_fail) { std::printf("head17 unit test: %d FAILED\n", g_fail); return 1; }
    std::printf("head17 unit test: passed\n");
    return 0;
}
