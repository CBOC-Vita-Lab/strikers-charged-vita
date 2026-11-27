// cpu22: see include/np/cpu22.h
#include "np/cpu22.h"

#ifdef NP_NATIVE
#include <stdio.h>
#include <stdlib.h>

extern "C" {

int NpCpu22Modes[NP_C22_COUNT] = {-1, -1, -1};

namespace {
const char* const kName[NP_C22_COUNT] = {"invlazy", "powerup", "replay"};
const char* const kEnv[NP_C22_COUNT] = {"MSC_CPU22_INVLAZY", "MSC_CPU22_POWERUP", "MSC_CPU22_REPLAY"};
unsigned long g_calls[NP_C22_COUNT], g_hits[NP_C22_COUNT], g_diffs[NP_C22_COUNT];
unsigned long g_frames;
int g_break = -1;
} // namespace

unsigned NpCpu22DrawEpochV;

static void ReportAtExit(void);

int NpCpu22Init(int which)
{
    const char* e = getenv(kEnv[which]);
    int v = (e && *e) ? atoi(e) : 0;
    v = v < 0 ? 0 : (v > 2 ? 2 : v);
    NpCpu22Modes[which] = v;
    static int hooked;
    if (v && !hooked)
    {
        hooked = 1;
        atexit(ReportAtExit);
    }
    if (v)
        fprintf(stderr, "[cpu22] %s=%d (%s)\n", kEnv[which], v, v == 2 ? "both ways, the original's answer is used, differences are counted" : "new way");
    return v;
}

int NpCpu22Break(void)
{
    if (g_break < 0)
    {
        const char* e = getenv("MSC_CPU22_BREAK");
        g_break = (e && *e) ? atoi(e) : 0;
        if (g_break)
            fprintf(stderr, "[cpu22] MSC_CPU22_BREAK=%d (proof mutant)\n", g_break);
    }
    return g_break;
}

void NpCpu22Note(int which, int what)
{
    if (what == 0) ++g_calls[which];
    else if (what == 1) ++g_hits[which];
    else
    {
        ++g_diffs[which];
        if (g_diffs[which] <= 5)
            fprintf(stderr, "[cpu22] %s: a difference (number %lu, after %lu calls)\n", kName[which], g_diffs[which], g_calls[which]);
    }
}

static void Report(const char* why)
{
    for (int w = 0; w < NP_C22_COUNT; ++w)
    {
        if (NpCpu22Modes[w] <= 0)
            continue;
        fprintf(stderr, "[cpu22] %s mismatches %lu (mode %d, %s: calls %lu, new way used %lu)\n", kName[w], g_diffs[w], NpCpu22Modes[w], why, g_calls[w], g_hits[w]);
    }
}

static void ReportAtExit(void) { Report("at exit, totals"); }

void NpCpu22Frame(void)
{
    if (++g_frames < 600)
        return;
    g_frames = 0;
    Report("running totals");
}

} // extern "C"
#endif
