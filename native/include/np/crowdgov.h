/* np/crowdgov.h: MSC_CROWD_GOV, the adaptive crowd-cost governor (crowd25, platform/compat/np_crowdgov.h is the controller). Render only.
 * The controller holds a LEVEL 0..5 from the game thread's measured load; the crowd sprites' refresh (ImpostorManager.cpp) reads it once a picture.
 *   Each crowd MODEL (one pose clock, all the sprites that show it) has a size class (1 near, 2 middling, 3 far, from the mean height of its sprites' nearest figures) and its own level, which follows the governor one notch
 *   at a time (staggered by model over ~12 pictures: the crowd never changes all at once; a step down starts with the far class, a give-back with the near one).
 *   A model's sprites are redrawn together every `period` pictures of its level and class: base period * mult2 / 2 (mult2 2 = unchanged). Its animation clock only moves when its sprites are redrawn, by at most 0.25 s a time,
 *   so a slowed or frozen model shows exactly the pose its clock holds and resumes from it: no snap to the live pose (the lag is dropped, the animations are cycles).
 *   Level 5 is the bare minimum: middling and far models redraw about every 1.5 s, near ones at four times the base interval. */
#ifndef NP_CROWDGOV_H
#define NP_CROWDGOV_H

/* mult2 (in halves) per level (0..5) and class (1 near, 2 middling, 3 far); the redraw interval is multiplied by mult2 / 2 */
static inline int NpCrowdGovMult2(int level, int cls)
{
    static const signed char t[6][3] = { {2, 2, 2}, {2, 3, 3}, {3, 4, 4}, {4, 6, 6}, {6, 10, 10}, {8, 20, 20} };
    if (level < 0) level = 0;
    if (level > 5) level = 5;
    return t[level][cls < 1 ? 0 : (cls > 3 ? 2 : cls - 1)];
}
static inline int NpCrowdGovPeriod(int base, int mult2)
{
    return mult2 <= 2 ? base : (base * mult2 + 1) / 2;
}
/* is a sprite with this key due on picture `tick` at this period? (period == base: the game's own rule is used instead, see ImpostorCharacter.cpp) */
static inline int NpCrowdGovDue(unsigned tick, int key, int period)
{
    const unsigned p = (unsigned)period;
    return ((tick % p) + p - ((unsigned)key % p)) % p == 0;
}

#ifdef NP_NATIVE
#ifdef __cplusplus
extern "C" {
#endif
int NpCrowdGovLevel(void);                      /* np_fps.cpp: the level in force this picture (0 when the switch is off) */
void NpCrowdGovCount(int which, unsigned n);    /* 0 sprites measured, 1 notch moves, 2 captures kept, 3 captures skipped, 4 slow-idle captures at level 5 */
#ifdef __cplusplus
}
#endif
#else
static inline int NpCrowdGovLevel(void) { return 0; }
static inline void NpCrowdGovCount(int, unsigned) {}
#endif
#endif
