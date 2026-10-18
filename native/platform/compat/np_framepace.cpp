// 30 fps frame pacing: see include/np/framepace.h.
// Adapted from yannicksuter/mscharged-port (CC0) src/platform/game_tweaks.cpp (original-frame accumulator), reworked for presenting BELOW
// the original rate: whole original frames per drawn frame, not suppressed steps between them.
#include "np/framepace.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

extern "C" {
float g_npPaceFrames = 1.0f;
int g_npPaceSteps = 1;
unsigned long g_npPaceExtra = 0;
unsigned g_npPaceSites = 63;
unsigned long g_npPaceRolls = 0;
unsigned long g_npPaceChanges = 0;
double NpVirtualSeconds(void) __attribute__((weak));   // platform/harness (virtual clock), absent in a bare game link
}

namespace {
const float kNtscField = 1001.0f / 60000.0f;   // one VI field: 59.94 Hz
const float kPalField = 1.0f / 50.0f;
const float kMaxFrames = 4.0f;                 // longer is a stall (loading), not the game's speed
const float kSnap = 0.1f;                      // within this of a whole number of original frames: that number

int g_enabled = -1;
float g_originalSeconds = kNtscField;          // one original frame: a field, two in swap mode 2 (the 30 fps lock)
float g_phase = 0.0f;                          // original frames owed (+) or ahead (-) of the whole steps taken, in [-0.5, 0.5]
unsigned long g_drawn = 0;
int g_log = -1;                                // MSC_FRAME_PACING_LOG: 0 off, 1 periodic, 2 events too
double Now() { return &NpVirtualSeconds != 0 ? NpVirtualSeconds() : 0.0; }

bool Enabled()
{
    if (g_enabled < 0)
    {
        const char* e = getenv("MSC_FRAME_PACING");
        g_enabled = (e && *e == '0') ? 0 : 1;
        const char* l = getenv("MSC_FRAME_PACING_LOG");
        g_log = (l && *l) ? atoi(l) : 0;
        const char* m = getenv("MSC_FRAME_PACING_SITES");
        if (m && *m)
        {
            g_npPaceSites = (unsigned)strtoul(m, 0, 0);
        }
    }
    return g_enabled == 1;
}
}

extern "C" {

int NpPaceEnabled(void) { return Enabled() ? 1 : 0; }

void NpPaceSwapMode(int swapMode, int palTv)
{
    g_originalSeconds = (swapMode == 2 ? 2.0f : 1.0f) * (palTv ? kPalField : kNtscField);
}

void NpPaceBeginFrame(float realSeconds)
{
    if (!Enabled())
    {
        // Off: the original path (1.0, 1 step). Still counted and logged, so a run with pacing off can be compared with one with it on.
        ++g_drawn;
        if (g_log >= 1 && g_drawn % 120 == 0)
        {
            fprintf(stderr, "[pace] t=%.3f drawn=%lu original=%lu rolls=%lu changes=%lu last real=%.2f ms n=1.000 steps=1 orig=%.3f ms (pacing off)\n", Now(),
                    g_drawn, g_drawn, g_npPaceRolls, g_npPaceChanges, realSeconds * 1000.0f, g_originalSeconds * 1000.0f);
        }
        return;
    }
    float n = realSeconds > 0.0f ? realSeconds / g_originalSeconds : 1.0f;
    if (n > kMaxFrames) n = kMaxFrames;
    if (n < 1.0f) n = 1.0f;                     // faster than the original: its path, unchanged
    int k;
    float whole = floorf(n + 0.5f);
    if (fabsf(n - whole) < kSnap)
    {
        n = whole;
        k = (int)whole;
        g_phase = 0.0f;
    }
    else
    {
        // Error-diffused rounding: a steady 1.5 original frames alternates 1 and 2 steps, the average is right.
        float total = g_phase + n;
        k = (int)(total + 0.5f);
        if (k < 1) k = 1;
        g_phase = total - (float)k;
        if (g_phase < -0.5f) g_phase = -0.5f;
        if (g_phase > 0.5f) g_phase = 0.5f;
    }
    g_npPaceFrames = n;
    g_npPaceSteps = k;
    g_npPaceExtra += (unsigned long)(k - 1);
    ++g_drawn;
    if (g_log >= 1 && g_drawn % 120 == 0)
    {
        fprintf(stderr, "[pace] t=%.3f drawn=%lu original=%lu rolls=%lu changes=%lu last real=%.2f ms n=%.3f steps=%d orig=%.3f ms\n", Now(), g_drawn,
                g_drawn + g_npPaceExtra, g_npPaceRolls, g_npPaceChanges, realSeconds * 1000.0f, n, k, g_originalSeconds * 1000.0f);
    }
}

void NpPaceEvent(const char* what, float value)
{
    Enabled();
    if (g_log >= 2)
    {
        fprintf(stderr, "[pace-ev] t=%.4f drawn=%lu %s %.4f\n", Now(), g_drawn, what, value);
    }
}

} // extern "C"
