/* np/crowdcut.h: the crowd's work in replays, goal celebrations and cut-scenes (1.3, wave 7). Render-only: every input is a camera matrix or a figure's place, every output
 * is which crowd pictures are redrawn on this picture and which figure lists are built. Nothing here reads or writes simulation state or draws a random number (the crowd
 * path has none per picture: its random calls are at load and at a crowd animation change, ImpostorCharacter.cpp, CrowdImpostorManager.cpp, GameRenderTask.cpp has the list).
 *
 *   MSC_CROWD_SIZED=0|1        0 (code default). 1: under a camera that is not the gameplay one, a crowd sprite (one pose of one crowd model seen from one angle, shared by
 *                              every figure that uses it) is redrawn every picture when the nearest figure that shows it is large on screen, every 2nd picture when it is
 *                              middling, every 3rd when it is small (at 60 pictures a second; at 30: 1, 2, 2: the rate in time is what is fixed). The sprites of one model and texture go together (one pose is evaluated for the four angles), and the groups
 *                              are staggered by their number so that every picture carries about the same share. A sprite that comes into view is drawn at once.
 *                              The pose and the view matrices of a sprite that is not redrawn are not computed.
 *   MSC_CROWD_SIZED_PX=a,b     40,20: the size in lines of the 544-line screen of a figure (its height) at and above which the step is 1 (a) and 2 (b); under b it is 3.
 *   MSC_CROWD_SIZED_MAX=2|3    3: the largest step.
 *   MSC_CROWD_SIZED_CLUSTER=n  2: the step of the cluster picture (the stands' far crowd, 128 x 128, drawn from every figure) where a stadium reads it.
 *   MSC_CROWD_VISFAST=0|1|2    0 (code default). 1: UpdateCrowdVisibility appends precomputed lists of figure slots per layout and sprite instead of testing every figure (the
 *                              same slot lists, in the same order). 2: also runs the plain loop and counts any difference (a test).
 *   MSC_CROWD_LAZYVIEW=0|1     0 (code default). 1: the view matrices of a sprite are computed when the sprite is drawn, not on every picture (UpdatePositions).
 */
#ifndef NP_CROWDCUT_H
#define NP_CROWDCUT_H

#include <stdlib.h>
#include <string.h>

#include "np/lod.h"

static inline int NpCrowdIntParse(const char* e, int fallback, int lo, int hi)
{
    if (!e || !*e) return fallback;
    int n = atoi(e);
    return n < lo ? lo : (n > hi ? hi : n);
}

/* The size class (1 near, 2 middling, 3 far) of a sprite whose nearest figure is `px` lines tall. thr[0] > thr[1]. prevClass with hysteresis h (a fraction of a threshold):
 * to a nearer class the size must pass thr*(1+h), to a farther one fall under thr*(1-h). */
static inline int NpCrowdClass(float px, int prevClass, float t1, float t2, float h)
{
    const float thr[2] = { t1, t2 };
    return NpLodSelect(px, prevClass < 1 ? 0 : prevClass - 1, thr, 2, h) + 1;
}

/* The step in pictures of a class. Fixed in time, not in pictures: the near class is drawn every picture, the middling one 30 times a second, the far one about 20 times a second
 * at 60 pictures a second (3), and 15 times at 30 (2: what MSC_CROWD_REPLAY=2 gave every sprite at 30). maxStep caps it (MSC_CROWD_SIZED_MAX). */
static inline int NpCrowdStepOfClass(int cls, int at60, int maxStep)
{
    const int step = cls <= 1 ? 1 : (cls == 2 ? 2 : (at60 ? 3 : 2));
    return step > maxStep ? maxStep : step;
}

/* Is a group (serial number `cls`) drawn on picture `frame` at `step`? Groups of one step are spread over its pictures by their serial. */
static inline int NpCrowdDue(unsigned frame, unsigned cls, int step)
{
    return step <= 1 || ((frame + cls) % (unsigned)step) == 0;
}

/* The nearest class of the sprites of a group wins (the group shares one pose). */
static inline int NpCrowdGroupClass(int a, int b)
{
    return a < b ? a : b;
}

/* The height in lines of a figure of world height `h` at `depth` in front of the camera plane (projY = the projection's vertical scale). */
static inline float NpCrowdFigurePx(float h, float depth, float projY)
{
    return NpLodProjectedPx(0.5f * h, depth, projY);
}

#ifdef NP_NATIVE
#ifdef __cplusplus
extern "C" {
#endif
int NpCrowdSized(void);
int NpCrowdSizedMax(void);
int NpCrowdSizedCluster(void);
float NpCrowdSizedPx(int which);   /* 0: the large threshold, 1: the middling one */
int NpCrowdVisFast(void);
int NpCrowdLazyView(void);
int NpCrowdGT(void);   /* MSC_CROWD_GT (wave 9, game-thread cost of the crowd; 0 = the old path): see ImpostorManager.cpp */
int NpCrowdGTBreak(void);
int NpCrowdBlankSkip(void);
void NpCrowdBlankCount(int skipped);   /* 0 an empty capture made, 1 one left out */   /* MSC_CROWD_BLANKSKIP (gem2a, default 1, exact): a due sprite with no figure whose picture is already the blank one is not captured again; 0 = the game's clear + copy every due picture */
void NpCrowdGTCount(int which, int n);   /* counters printed once at exit of the stage's life */
/* wave 12 (crowd24): MSC_POSE_JOBS bit 1 the crowd sprites' poses (animation evaluate, node and skin matrices) are made on the job lanes before the capture loop, bit 2 the same for the
 * decorative NPCs; MSC_NPC_EVAL_LAZY=1 an NPC's pose is evaluated where it is drawn (only the ones the render filter passes), not on every update. MSC_POSE_JOBS_BREAK=1 (proof only). */
int NpPoseJobs(void);
int NpPoseJobsBreak(void);
int NpNpcEvalLazy(void);
void NpPoseJobsCount(int which, int n);   /* 0 crowd models posed on lanes, 1 crowd pictures with a fork, 2 npc models posed on lanes, 3 npc pictures with a fork, 4 npc evaluates skipped, 5 npc evaluates at render, 6 crowd evaluates skipped in the loop */
extern unsigned gNpCrowdStructGen;   /* bumped wherever the set of figures or the place of one changes (load-time paths): the precomputed lists are rebuilt */
/* MSC_CROWD_* counters, one line at the end of a stage's life (np_crowdcut.cpp) */
void NpCrowdCountPicture(int sprites, int drawn, int groupsDrawn, const int* pxBins);   /* pxBins[7]: nearest-figure size of the sprites with figures, <10 <20 <40 <80 <160 <320 more */
void NpCrowdCountVis(int mismatch);
void NpCrowdSeedProbe(void);   /* MSC_CROWD_SEEDLOG=1 */
void NpCrowdNote(int readers, int hasClusters, int mUpdateClusters, int updateClusters, int heavyCrowd);
#ifdef __cplusplus
}
#endif
#else
#ifdef __cplusplus
static inline int NpCrowdSized(void) { return 0; }
static inline int NpCrowdSizedMax(void) { return 3; }
static inline int NpCrowdSizedCluster(void) { return 2; }
static inline float NpCrowdSizedPx(int which) { return which ? 20.0f : 40.0f; }
static inline int NpCrowdVisFast(void) { return 0; }
static inline int NpCrowdLazyView(void) { return 0; }
static inline int NpCrowdGT(void) { return 0; }
static inline int NpCrowdGTBreak(void) { return 0; }
static inline int NpCrowdBlankSkip(void) { return 0; }
static inline void NpCrowdBlankCount(int) {}
static inline void NpCrowdGTCount(int, int) {}
static inline int NpPoseJobs(void) { return 0; }
static inline int NpPoseJobsBreak(void) { return 0; }
static inline int NpNpcEvalLazy(void) { return 0; }
static inline void NpPoseJobsCount(int, int) {}
static inline void NpCrowdSeedProbe(void) {}
static inline void NpCrowdNote(int, int, int, int, int) {}
#endif
#endif

#endif /* NP_CROWDCUT_H */
