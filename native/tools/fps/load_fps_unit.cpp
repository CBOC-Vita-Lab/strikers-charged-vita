// MSC_LOAD_FPS (np_fps.cpp LoadPace): the real np_fps.cpp asked what it would do in the two loading task states, out of them, and in a session. Same build line as slip_unit.cpp.
#define NP_NATIVE 1
#include "../../platform/compat/np_fps.cpp"
#include "gs_stub.h"

#include <stdio.h>
#include <stdlib.h>

static bool g_lan = false;
bool NpLanActive() { return g_lan; }

static int g_fail = 0;
static void Check(bool ok, const char* what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) ++g_fail; }

static void Frame(unsigned state) { NpFpsBeginFrame(1.0f / 30.0f, state); }

// nxpace25: the [load] line's clock (LoadClockStep): a visit of FE_TO_GAME / GAME_TO_FE is measured from its first picture to the first picture outside it.
static void LoadClockCases() {
    LoadClock c;
    unsigned st = 0; double ms = 0; unsigned long long n = 0;
    Check(!LoadClockStep(c, 2, 1.0, st, ms, n), "load clock: a match picture is no visit");
    Check(!LoadClockStep(c, 0x200000, 2.0, st, ms, n), "load clock: the first load picture opens a visit");
    Check(!LoadClockStep(c, 0x200000, 2.5, st, ms, n) && !LoadClockStep(c, 0x200000, 3.0, st, ms, n), "load clock: pictures inside the visit end nothing");
    const bool e = LoadClockStep(c, 16, 3.25, st, ms, n);
    Check(e && st == 0x200000 && n == 3, "load clock: leaving ends the visit with its 3 pictures");
    Check(e && ms > 1249.0 && ms < 1251.0, "load clock: the time runs to the first picture outside (1250 ms, not 1000)");
    Check(!LoadClockStep(c, 16, 4.0, st, ms, n), "load clock: nothing more after the visit");
    LoadClockStep(c, 0x200000, 5.0, st, ms, n);
    const bool sw = LoadClockStep(c, 0x400000, 5.5, st, ms, n);
    Check(sw && st == 0x200000 && n == 1, "load clock: a change from one load state to the other closes the first visit");
    const bool e2 = LoadClockStep(c, 2, 6.0, st, ms, n);
    Check(e2 && st == 0x400000 && n == 1 && ms > 499.0 && ms < 501.0, "load clock: and the second visit is its own");
}

int main(int argc, char** argv) {
    LoadClockCases();
    setenv("MSC_BOOT_FPS", "0", 1);
    setenv("MSC_FPS", "30", 1);
    const bool on = argc > 1;
    if (on) setenv("MSC_LOAD_FPS", "60", 1);
    Frame(2);
    Frame(2);
    Check(NpFpsSwapMode(1) == 4 && NpFpsDisplayInterval() == 2, "a match is held at 30");
    Frame(0x200000);
    Frame(0x200000);
    if (!on) {
        Check(NpFpsSwapMode(1) == 4 && NpFpsDisplayInterval() == 2, "switch unset: loading is held at 30 as before");
    } else {
        Check(NpFpsSwapMode(1) == 1 && NpFpsDisplayInterval() == 1, "FE_TO_GAME: a picture per refresh");
        Check(NpFpsSwapMode(0) == 0, "swap mode 0 is left alone");
        Check(NpFpsSwapMode(2) == 2, "a cut-scene lock is left alone");
        Frame(0x400000);
        Check(NpFpsSwapMode(1) == 1 && NpFpsDisplayInterval() == 1, "GAME_TO_FE: a picture per refresh");
        Frame(16);
        Check(NpFpsSwapMode(1) == 4 && NpFpsDisplayInterval() == 2, "a cut-scene after the load is held at 30");
        g_lan = true;
        Frame(0x200000);
        Check(NpFpsSwapMode(1) == 4 && NpFpsDisplayInterval() == 2, "a session: as before");
    }
    printf(g_fail ? "load_fps_unit: FAILED\n" : "load_fps_unit: passed\n");
    return g_fail != 0;
}
