// MSC_AI_AVOID: see include/np/aiavoid.h. Switches are read once; the counters belong to the game thread (the AI runs there).
#include "np/aiavoid.h"

#include <stdio.h>
#include <stdlib.h>

static unsigned long long sCount[8];

static void Report(void)
{
    fprintf(stderr, "[aiavoid] mode %d: CanAddAvoidance tests %llu, AVL lookups avoided %llu, base IsWithinRange calls %llu, square roots avoided %llu, verify wrong %llu\n",
        NpAiAvoidMode(), sCount[0], sCount[1], sCount[2], sCount[3], sCount[4]);
}

int NpAiAvoidMode(void)
{
    static int v = -1;
    if (v < 0)
    {
        const char* e = getenv("MSC_AI_AVOID");
        int n = (e && *e) ? atoi(e) : 0;
        v = n < 0 ? 0 : (n > 7 ? 7 : n);
        if (v) atexit(Report);
    }
    return v;
}

int NpAiAvoidBreak(void)
{
    static int v = -1;
    if (v < 0)
    {
        const char* e = getenv("MSC_AI_AVOID_BREAK");
        v = (e && *e == '1') ? 1 : (e && *e == '2') ? 2 : 0;
    }
    return v;
}

void NpAiAvoidCount(int which, int n)
{
    sCount[which & 7] += (unsigned long long)n;
    // the harness may leave by _exit: also a line every 100000 tests
    if ((which & 7) == 0 && sCount[0] % 100000 < (unsigned long long)n)
        Report();
}
