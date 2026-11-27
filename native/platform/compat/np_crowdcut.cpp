// The crowd's cuts in replays and cut-scenes: see include/np/crowdcut.h. Switches are read once; counters are the renderer thread's (one thread calls them).
#include "np/crowdcut.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {

unsigned gNpCrowdStructGen = 1;

static int EnvInt(const char* name, int def, int lo, int hi)
{
    return NpCrowdIntParse(getenv(name), def, lo, hi);
}

int NpCrowdSized(void)
{
    static int v = -1;
    if (v < 0) v = EnvInt("MSC_CROWD_SIZED", 0, 0, 1);
    return v;
}

int NpCrowdSizedMax(void)
{
    static int v = -1;
    if (v < 0) v = EnvInt("MSC_CROWD_SIZED_MAX", 3, 2, 3);
    return v;
}

int NpCrowdSizedCluster(void)
{
    static int v = -1;
    if (v < 0) v = EnvInt("MSC_CROWD_SIZED_CLUSTER", 2, 1, 3);
    return v;
}

float NpCrowdSizedPx(int which)
{
    static float t[2] = { -1.0f, -1.0f };
    if (t[0] < 0.0f)
    {
        float a = 40.0f, b = 20.0f;
        const char* e = getenv("MSC_CROWD_SIZED_PX");
        if (e && *e)
        {
            char* end = 0;
            const float x = (float)strtod(e, &end);
            if (end != e && *end == ',')
            {
                const float y = (float)strtod(end + 1, 0);
                if (x > y && y > 0.0f) { a = x; b = y; }
            }
        }
        t[1] = b;
        t[0] = a;
    }
    return t[which ? 1 : 0];
}

int NpCrowdVisFast(void)
{
    static int v = -1;
    if (v < 0) v = EnvInt("MSC_CROWD_VISFAST", 0, 0, 2);
    return v;
}

static unsigned long long sGtCount[8];
static void GtReport(void)
{
    fprintf(stderr, "[crowdgt] sprite views skipped %llu computed %llu, visibility passes skipped %llu run %llu, render cached %llu rebuilt %llu, poses %llu sprites captured %llu\n", sGtCount[0], sGtCount[1], sGtCount[2], sGtCount[3], sGtCount[4], sGtCount[5], sGtCount[6], sGtCount[7]);
}

int NpCrowdGT(void)
{
    static int v = -1;
    if (v < 0)
    {
        v = EnvInt("MSC_CROWD_GT", 0, 0, 7);
        if (v) atexit(GtReport);
    }
    return v;
}

// MSC_CROWD_GT_BREAK=1 (proof only): the lazy views use the direction of the previous picture, so the frame hashes must change
int NpCrowdGTBreak(void)
{
    static int v = -1;
    if (v < 0) v = EnvInt("MSC_CROWD_GT_BREAK", 0, 0, 1);
    return v;
}

// MSC_CROWD_GT action counters: 0 sprite views skipped, 1 sprite views computed, 2 visibility passes skipped, 3 visibility passes run
void NpCrowdGTCount(int which, int n)
{
    sGtCount[which & 7] += (unsigned long long)n;
    if ((which & 7) == 7 && sGtCount[7] % 20000 < (unsigned long long)n)   // the harness may leave by _exit: a line every 20000 captured sprites too
        GtReport();
}

// MSC_CROWD_BLANKSKIP=0|1 (gem2a, default 1): a due sprite with no figure (and not a cluster) is captured once as the blank picture (clear + copy, as the game does); while it stays empty
// its picture is already that blank one, so the clear + copy (a scene break and a copy on the console) is not made again. Any capture with figures takes the flag down. Render only.
// MSC_CROWD_BLANKSKIP=3 (proof only): an empty due sprite is never captured at all (Gemini's form: the picture it had stays, whatever it held). =2 (proof only, mutant): the flag is never taken down, so a sprite that gets figures again keeps showing its blank picture until the next capture and the frame hashes differ.
static unsigned long long sBlankCount[2];
static void BlankReport(void)
{
    fprintf(stderr, "[crowdblank] MSC_CROWD_BLANKSKIP: empty sprite captures made %llu, left out %llu (the picture was already the blank one)\n", sBlankCount[0], sBlankCount[1]);
}
void NpCrowdBlankCount(int skipped)
{
    ++sBlankCount[skipped & 1];
    if (((sBlankCount[0] + sBlankCount[1]) & 4095) == 1) BlankReport();   // (the harness may leave by _exit: a line every 4096 events)
}
int NpCrowdBlankSkip(void)
{
    static int v = -1;
    if (v < 0)
    {
        v = EnvInt("MSC_CROWD_BLANKSKIP", 1, 0, 3);
        if (v) atexit(BlankReport);
    }
    return v;
}

int NpCrowdLazyView(void)
{
    static int v = -1;
    if (v < 0) v = EnvInt("MSC_CROWD_LAZYVIEW", 0, 0, 1);
    return v;
}

extern unsigned int nlDefaultSeed;        // the presentation's generator (the crowd's load-time and animation-change draws); the simulation's is gNetworkRandomSeed
extern unsigned int gNetworkRandomSeed;

// MSC_CROWD_SEEDLOG=1: both generators every 120 pictures that reach the crowd's block, so that two runs can be compared draw for draw
void NpCrowdSeedProbe(void)
{
    static int on = -1;
    static unsigned long n;
    if (on < 0) on = EnvInt("MSC_CROWD_SEEDLOG", 0, 0, 1);
    if (on && (++n % 120) == 0)
    {
        fprintf(stderr, "[crowdseed] %lu nlDefaultSeed=%u gNetworkRandomSeed=%u\n", n, nlDefaultSeed, gNetworkRandomSeed);
    }
}

// MSC_CROWD_SEEDLOG=1: the crowd block's state, one line when it changes
void NpCrowdNote(int readers, int hasClusters, int mUpdateClusters, int updateClusters, int heavyCrowd)
{
    static int on = -1;
    static int last[5] = { -9, -9, -9, -9, -9 };
    if (on < 0) on = EnvInt("MSC_CROWD_SEEDLOG", 0, 0, 1);
    if (!on) return;
    const int now[5] = { readers, hasClusters, mUpdateClusters, updateClusters, heavyCrowd };
    if (memcmp(last, now, sizeof(now)) != 0)
    {
        memcpy(last, now, sizeof(now));
        fprintf(stderr, "[crowdcut] cluster readers %d, mHasClusters %d, mUpdateClusters %d, cluster captured %d, heavy-stadium crowd hidden %d\n", readers, hasClusters, mUpdateClusters, updateClusters, heavyCrowd);
    }
}

static unsigned long sPictures, sSprites, sDrawn, sGroups, sVisMismatch, sVisChecks;

static unsigned long sBins[7];

void NpCrowdCountPicture(int sprites, int drawn, int groupsDrawn, const int* pxBins)
{
    for (int i = 0; i < 7; ++i) sBins[i] += (unsigned long)pxBins[i];
    sPictures++;
    sSprites += (unsigned long)sprites;
    sDrawn += (unsigned long)drawn;
    sGroups += (unsigned long)groupsDrawn;
    if ((sPictures % 100) == 0)
    {
        fprintf(stderr, "[crowdcut] pictures %lu: last 100 sized pictures: sprites with figures %lu a picture, redrawn %lu a picture (%lu pose groups)\n", sPictures, sSprites / 100, sDrawn / 100, sGroups / 100);
        fprintf(stderr, "[crowdcut]   nearest figure of a sprite, sprites a picture by height in lines: <10 %lu, <20 %lu, <40 %lu, <80 %lu, <160 %lu, <320 %lu, more %lu\n", sBins[0] / 100, sBins[1] / 100, sBins[2] / 100, sBins[3] / 100, sBins[4] / 100, sBins[5] / 100, sBins[6] / 100);
        for (int i = 0; i < 7; ++i) sBins[i] = 0;
        sSprites = sDrawn = sGroups = 0;
    }
}

void NpCrowdCountVis(int mismatch)
{
    sVisChecks++;
    if (mismatch)
    {
        sVisMismatch++;
        if (sVisMismatch <= 5) fprintf(stderr, "[crowdcut] VISFAST MISMATCH (check %lu)\n", sVisChecks);
    }
    if ((sVisChecks % 1200) == 0)
    {
        fprintf(stderr, "[crowdcut] VISFAST compare: %lu checks, %lu mismatches\n", sVisChecks, sVisMismatch);
    }
}

}

// ---- wave 12 (crowd24): pose jobs. MSC_POSE_JOBS 0..3 (bit 1 crowd, bit 2 NPCs), MSC_NPC_EVAL_LAZY 0..1, counters in one [posejobs] line at exit and every 20000 crowd pictures.
namespace
{
unsigned long long sPjCount[8];
void PjReport()
{
    fprintf(stderr, "[posejobs] MSC_POSE_JOBS=%d MSC_NPC_EVAL_LAZY=%d: crowd models posed on lanes %llu in %llu forks (evaluates skipped in the capture loop %llu), npc models posed on lanes %llu in %llu forks, npc evaluates skipped at update %llu run at render %llu\n",
        NpPoseJobs(), NpNpcEvalLazy(), sPjCount[0], sPjCount[1], sPjCount[6], sPjCount[2], sPjCount[3], sPjCount[4], sPjCount[5]);
}
}

extern "C" int NpPoseJobs(void)
{
    static int v = -1;
    if (v < 0)
    {
        v = EnvInt("MSC_POSE_JOBS", 0, 0, 3);
        if (v) atexit(PjReport);
    }
    return v;
}

extern "C" int NpPoseJobsBreak(void)
{
    static int v = -1;
#if defined(__vita__)
    if (v < 0) v = 0;   /* 18 Nov: the proof mutant is compiled out of the console build (np-player.txt / env.txt lines reach getenv there) */
#else
    if (v < 0) v = EnvInt("MSC_POSE_JOBS_BREAK", 0, 0, 1);
#endif
    return v;
}

extern "C" int NpNpcEvalLazy(void)
{
    static int v = -1;
    if (v < 0)
    {
        v = EnvInt("MSC_NPC_EVAL_LAZY", 0, 0, 1);
        if (v) NpPoseJobs();   // (one atexit report)
    }
    return v;
}

extern "C" void NpPoseJobsCount(int which, int n)
{
    sPjCount[which & 7] += (unsigned long long)n;
    if (((which & 7) == 1 || (which & 7) == 3) && sPjCount[which & 7] % 600 == 1) PjReport();   // (the harness may leave by _exit: a line every 600 forks too)
}
