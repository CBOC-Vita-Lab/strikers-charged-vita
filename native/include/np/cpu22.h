// cpu22: exact game-thread CPU cuts, each behind its own switch with a verify mode.
//   MSC_CPU22_INVLAZY=0|1|2  SetGameObjectShadowViewMatrix (GameObjectLighting.cpp): the view matrix's inverse is made when a skinned draw first needs it, not at every material program Activate
//   MSC_CPU22_POWERUP=0|1|2  DrawablePowerup::Render (DrawablePowerup.cpp): the stadium drawable of each of the 150 power-up slots is looked up once per stadium state, not every picture
//   MSC_CPU22_REPLAY=0|1|2   ReplayManager::GetReplayExcitement (ReplayManager.cpp): the two TimeOfLastOccurence walks of the replay ring are made as one
// 0 = the original code, 1 = the new way only, 2 = both ways, the original's answer is used, a difference is counted ("[cpu22] <name> mismatches N").
// Every switch changes speed only: same results, same order of effects. Default 0 in code (the console plan turns a proven one on).
// MSC_CPU22_BREAK=<n> (host proof mutant, never set in play): 1 = INVLAZY inverts a slightly different matrix, 2 = POWERUP serves "no object" for every 8th slot, 3 = REPLAY reads the wrong event mask.
#ifndef NP_CPU22_H
#define NP_CPU22_H

#ifdef NP_NATIVE
#ifdef __cplusplus
extern "C" {
#endif

enum { NP_C22_INVLAZY = 0, NP_C22_POWERUP = 1, NP_C22_REPLAY = 2, NP_C22_COUNT = 3 };

extern int NpCpu22Modes[NP_C22_COUNT];   /* -1 until first asked */
int NpCpu22Init(int which);
static inline int NpCpu22Mode(int which)
{
    const int v = NpCpu22Modes[which];
    return v >= 0 ? v : NpCpu22Init(which);
}

/* counters: what 0 = a call that could use the new way, 1 = it did / the work that was made, 2 = a difference found by mode 2 */
void NpCpu22Note(int which, int what);
int NpCpu22Break(void);                       /* MSC_CPU22_BREAK, read once */
void NpCpu22Frame(void);                      /* once a picture: a line every 600 pictures with a switch on */

/* the stadium's drawable objects (BasicStadium::m_registeredDrawables, World::m_drawableMap) change only where this is bumped */
extern unsigned NpCpu22DrawEpochV;
static inline void NpCpu22DrawEpochBump(void) { ++NpCpu22DrawEpochV; }
int NpMemGetGameThread(void);                 /* NL/nlMemory.cpp */

#ifdef __cplusplus
}
#endif
#endif /* NP_NATIVE */
#endif
