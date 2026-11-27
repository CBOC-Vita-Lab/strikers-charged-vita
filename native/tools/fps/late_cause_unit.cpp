// lessons17: the late-picture attribution (np_late_cause.h). NP_LATECAUSE_BREAK 1-4 must fail.
//   g++ -std=c++11 -Wall -Inative/platform/compat native/tools/fps/late_cause_unit.cpp -o /tmp/late_cause_unit && /tmp/late_cause_unit
#include "np_late_cause.h"
#include <cstdio>
#include <cstring>
using namespace nplc;
static int g_fail = 0;
static void check(bool ok, const char* w) { if (!ok) { std::printf("FAIL: %s\n", w); ++g_fail; } }
int main() {
    Config c;
    const float P = 33.4f;   // the held 30's period
    { Lanes l; l.gpu = 30; l.game = 12; l.be = 8; l.fe = 9; check(Judge(c, P, l, 2, 50) == kGpu, "a 30 ms GPU picture in a 33.4 ms period is the GPU's"); }
    { Lanes l; l.gpu = 10; l.game = 29; l.be = 8; l.fe = 9; check(Judge(c, P, l, 2, 50) == kGame, "a 29 ms game thread is the game CPU's"); }
    { Lanes l; l.gpu = 10; l.game = 12; l.be = 28; l.fe = 9; check(Judge(c, P, l, 2, 50) == kRender, "a 28 ms back end is the render thread's"); }
    { Lanes l; l.gpu = 10; l.game = 12; l.be = 8; l.fe = 31; check(Judge(c, P, l, 2, 50) == kFront, "a 31 ms front end is the front end's"); }
    { Lanes l; l.gpu = 28; l.game = 31; l.be = 8; l.fe = 9; check(Judge(c, P, l, 2, 50) == kGame, "the largest lane over the share wins (game 31 over GPU 28)"); }
    { Lanes l; l.gpu = 30; l.game = 12; l.be = 8; l.fe = 9; check(Judge(c, P, l, 25, 50) == kGpu, "a lane over the share beats waits"); }
    { Lanes l; l.gpu = 10; l.game = 12; l.be = 8; l.fe = 9; check(Judge(c, P, l, 25, 50) == kWait, "no lane long, 25 of 50 ms waited: waits"); }
    { Lanes l; l.gpu = 10; l.game = 12; l.be = 8; l.fe = 9; check(Judge(c, P, l, 5, 50) == kNone, "no lane long, little waiting: unattributed"); }
    { Lanes l; l.gpu = 10; l.game = 12; l.be = 8; l.fe = 26; check(Judge(c, P, l, 0, 50) == kNone, "26 ms is under 80 % of 33.4 (26.7): not a lane's"); }
    // the ledger: the renderer's lanes are the largest of the last three pictures
    { Ledger L; L.windowPics = 10;
      L.lane[kLaneGpu].Note(30); L.lane[kLaneGpu].Note(8); L.lane[kLaneGpu].Note(8);
      L.lane[kLaneFront].Note(9); L.lane[kLaneRender].Note(7);
      L.Picture(3, 2, 12, 2, 50);
      check(L.w.late == 1 && L.w.by[kGpu] == 1, "a GPU picture two pictures back is still the cause (the pipeline is deep)");
      L.lane[kLaneGpu].Note(8); L.lane[kLaneGpu].Note(8); L.lane[kLaneGpu].Note(8);
      L.Picture(3, 2, 12, 2, 50);
      check(L.w.by[kNone] == 1, "five pictures back is not");
      L.Picture(2, 2, 40, 0, 33);   // on time: counted as a picture only
      L.Picture(1, 1, 40, 0, 16);
      check(L.w.late == 2 && L.w.pics == 4, "on-time pictures are not late; pair cadence expects 1 refresh");
      L.Picture(2, 1, 5, 0, 33);
      check(L.w.late == 3, "two refreshes at the pair cadence is late");
      char line[600]; L.Line(line, sizeof line, 3.5, 1200);
      check(strstr(line, "3 late") && strstr(line, "GPU 1") && strstr(line, "3.5 us") && L.w.pics == 0, "the line names the counts and the cost, and clears the window");
      std::printf("%s\n", line); }
    std::printf("%s: late_cause_unit\n", g_fail ? "FAILED" : "passed");
    return g_fail ? 1 : 0;
}
