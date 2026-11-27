// cpu24: see include/np/cpu24.h
#include "np/cpu24.h"

#ifdef NP_NATIVE
#include <stdio.h>
#include <stdlib.h>

extern "C" {

int NpCpu24Modes[NP_C24_COUNT] = {-1, -1, -1};

namespace {
const char* const k24Name[NP_C24_COUNT] = {"part", "tightstat", "notify"};
const char* const k24Env[NP_C24_COUNT] = {"MSC_CPU24_PART", "MSC_CPU24_TIGHTSTAT", "MSC_CPU24_NOTIFY"};
unsigned long g24_new[NP_C24_COUNT], g24_checked[NP_C24_COUNT], g24_diffs[NP_C24_COUNT];
int g24_hooked;
} // namespace

static void ReportAtExit(void)
{
    for (int i = 0; i < NP_C24_COUNT; ++i)
        if (NpCpu24Modes[i] == 2)
            fprintf(stderr, "[cpu24] %s: new way %lu, checked %lu, mismatches %lu\n", k24Name[i], g24_new[i], g24_checked[i], g24_diffs[i]);
}

int NpCpu24Init(int which)
{
    const char* e = getenv(k24Env[which]);
    int v = (e && *e) ? atoi(e) : 0;
    v = v < 0 ? 0 : (v > 2 ? 2 : v);
    // the proof mutant of a switch in mode 1 is mode 3 (the new way with the fault), so the production path tests one number only
    if (v == 1 && ((which == NP_C24_PART && NpCpu24Break() == 1)))
        v = 3;
    NpCpu24Modes[which] = v;
    if (v && !g24_hooked)
    {
        g24_hooked = 1;
        atexit(ReportAtExit);
    }
    if (v)
        fprintf(stderr, "[cpu24] %s=%d (%s)\n", k24Env[which], v, v == 2 ? "both ways, the original's answer is used, differences are counted" : "new way");
    return v;
}

void NpCpu24Note(int which, int what)
{
    if (what == 0)
        ++g24_new[which];
    else if (what == 1)
    {
        ++g24_checked[which];
        if ((g24_checked[which] % 1000000) == 0)
            fprintf(stderr, "[cpu24] %s: checked %lu mismatches %lu\n", k24Name[which], g24_checked[which], g24_diffs[which]);
    }
    else
        ++g24_diffs[which];
}

void NpCpu24Frame(void)
{
    static unsigned long frames;
    if (!g24_hooked || (++frames % 600) != 0)
        return;
    for (int i = 0; i < NP_C24_COUNT; ++i)
        if (NpCpu24Modes[i] == 2)
            fprintf(stderr, "[cpu24] %s: checked %lu mismatches %lu\n", k24Name[i], g24_checked[i], g24_diffs[i]);
}

int NpCpu24BreakV = -1;
int NpCpu24BreakInit(void)
{
    const char* e = getenv("MSC_CPU24_BREAK");
    NpCpu24BreakV = (e && *e) ? atoi(e) : 0;
    return NpCpu24BreakV;
}

} // extern "C"
#endif
