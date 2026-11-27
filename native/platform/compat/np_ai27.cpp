// MSC_AI27_*: see include/np/ai27.h. Read once on first use (the Vita plan sets the environment before the game starts); the counters belong to
// the thread that runs the AI step (game thread, or the sim thread with MSC_SIMT: one at a time).
#include "np/ai27.h"

#include <stdio.h>
#include <stdlib.h>

int gNpAi27Ready = 0;
int gNpAi27Val[NPAI27_N];
int gNpAi27Break = 0;
unsigned long long gNpAi27Cnt[NPAI27_N][4];

static const char* const kNames[NPAI27_N] = { "SORT", "VCOPY", "AVOID", "CTRL" };

static void Report(void)
{
    for (int s = 0; s < NPAI27_N; s++)
    {
        if (!gNpAi27Val[s]) continue;
        fprintf(stderr, "[ai27] %s=%d: used %llu, skipped/saved %llu, verified %llu, wrong %llu\n", kNames[s], gNpAi27Val[s],
            gNpAi27Cnt[s][0], gNpAi27Cnt[s][1], gNpAi27Cnt[s][2], gNpAi27Cnt[s][3]);
    }
}

void NpAi27Init(void)
{
    const char* env[NPAI27_N] = { getenv("MSC_AI27_SORT"), getenv("MSC_AI27_VCOPY"), getenv("MSC_AI27_AVOID"), getenv("MSC_AI27_CTRL") };
    for (int s = 0; s < NPAI27_N; s++)
    {
        const char* e = env[s];
        int n = (e && *e) ? atoi(e) : 0;
        gNpAi27Val[s] = n < 0 ? 0 : (n > 2 ? 2 : n);
    }
    const char* b = getenv("MSC_AI27_BREAK");
    gNpAi27Break = (b && *b) ? atoi(b) : 0;
    gNpAi27Ready = 1;
    int any = 0;
    for (int s = 0; s < NPAI27_N; s++) any |= gNpAi27Val[s];
    if (any) atexit(Report);
}

void NpAi27Tick(void)
{
    // the harness may leave by _exit: also a block of lines every 200000 events of the first active switch
    static unsigned long long sLast;
    unsigned long long t = 0;
    for (int s = 0; s < NPAI27_N; s++) t += gNpAi27Cnt[s][0];
    if (t - sLast >= 200000ULL)
    {
        sLast = t;
        Report();
    }
}
