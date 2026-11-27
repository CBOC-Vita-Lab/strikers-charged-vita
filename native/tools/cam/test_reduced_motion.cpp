// MSC_REDUCED_MOTION (np/comfort.h): the camera filters of the match camera (noisefilter.cpp, rumblefilter.cpp) shake a quarter as far, and nothing else
// about them changes: same number of random draws, same length of the effect. Prints one line of numbers; run_reduced_motion_test.sh runs it with the
// switch unset and set and compares.
#include "Game/Camera/noisefilter.h"
#include "Game/Camera/rumblefilter.h"
#include "NL/nlTask.h"

#include <math.h>
#include <stdio.h>

unsigned int nlDefaultSeed = 12345;
static int g_draws = 0;
unsigned int nlRandom(unsigned int range, unsigned int* seed) { ++g_draws; *seed = *seed * 1664525u + 1013904223u; return (*seed >> 8) % range; }
nlTaskManager* nlTaskManager::m_pInstance = 0;
float nlSqrt(float x, bool) { return sqrtf(x); }

int main() {
    cNoiseFilter n;
    nlVector3 amp = {0.15f, 0.4f, 0.1f};
    n.Start(amp, 30.0f, 1.0f);
    float peak = 0.0f, sum = 0.0f;
    int frames = 0;
    for (; n.mActive && frames < 200; ++frames) {
        n.Update(1.0f / 60.0f);
        const float d = fabsf(n.mDisplacement.x) + fabsf(n.mDisplacement.y) + fabsf(n.mDisplacement.z);
        sum += d;
        if (d > peak) peak = d;
    }
    cRumbleFilter r;
    r.Rumble(0.2f, 0.225f, 4300.0f, 6.0f);
    // noise: draws made, frames the effect lasted, peak and sum of the displacement; rumble: start offset, spring and damping
    printf("draws=%d frames=%d peak=%.6f sum=%.6f rumble=%.6f,%.6f ks=%.1f kd=%.1f\n", g_draws, frames, peak, sum, r.v2Pos1.x, r.v2Pos1.y, r.Ks, r.Kd);
    return 0;
}
