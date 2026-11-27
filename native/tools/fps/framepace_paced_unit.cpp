// MSC_PACE_VBLANK while MSC_FPS_BETWEEN=pace is in force: the real np_framepace.cpp, pictures held 1, 1, 1, 1, 2 for frames of 1.2 refreshes. The sum of NpPaceFrames must stay the real time.
//   g++ -std=c++11 -Inative/include native/tools/fps/framepace_paced_unit.cpp -o /tmp/framepace_paced_unit && MSC_PACE_VBLANK=1 /tmp/framepace_paced_unit
#define NP_NATIVE 1
#include <cstdio>
#include "../../platform/compat/np_framepace.cpp"

static int g_held = 1, g_paced = 1;
extern "C" int NpDisplayLastHeld(void) { return g_held; }
extern "C" int NpFpsPaced(void) { return g_paced; }

static double Run() {
    double sum = 0;
    for (int i = 0; i < 5000; ++i) {
        g_held = i % 5 == 4 ? 2 : 1;
        NpPaceBeginFrame(1.2f * 1001.0f / 60000.0f);
        sum += g_npPaceFrames;
    }
    return sum / (5000.0 * 1.2);
}

int main() {
    const double paced = Run();
    g_paced = 0;
    const double trusted = Run();
    std::printf("animation speed paced %.3f, with the display's count trusted %.3f\n", paced, trusted);
    if (paced < 0.99 || paced > 1.01 || trusted > 0.95) { std::printf("FAILED: framepace_paced_unit\n"); return 1; }
    std::printf("passed: framepace_paced_unit\n");
    return 0;
}
