/* np/unlocks.h: optional settings to unlock the three cup captains and the six locked stadiums without touching the save.
 *
 * Retail: twelve captains (three locked behind Fire/Crystal/Striker Cup wins: Bowser Jr., Diddy Kong, Petey Piranha) and six locked stadiums
 * (Lava Pit, Crystal Canyon, Stormship from cup wins; The Wastelands, The Dump, Galactic Stadium from cup awards). Setting the save's cup bits
 * would claim trophies that were never won, so these switches answer yes in the game's own unlock predicates (GameProgress.cpp) without changing
 * the save, trophies or cheats. Both default OFF so players can unlock characters and stadiums through normal play; both can be toggled in
 * OPTIONS > VITA SETTINGS.
 *
 *   MSC_UNLOCK_CAPTAINS=1|0   (environment; env.txt / np-player.txt, UNLOCK ALL CHARACTERS) overrides the build's default (0)
 *   MSC_UNLOCK_STADIUMS=1|0   (environment; env.txt / np-player.txt, UNLOCK ALL STADIUMS) overrides the build's default (0)
 */
#ifndef NP_UNLOCKS_H
#define NP_UNLOCKS_H

#ifndef NP_UNLOCK_CAPTAINS_DEFAULT
#define NP_UNLOCK_CAPTAINS_DEFAULT 0
#endif

#ifndef NP_UNLOCK_STADIUMS_DEFAULT
#define NP_UNLOCK_STADIUMS_DEFAULT 0
#endif

#ifdef NP_NATIVE
#ifdef __cplusplus
extern "C" {
#endif
extern int g_npUnlockCaptains;   /* -1 until decided */
extern int g_npUnlockStadiums;
int NpUnlockCaptainsDecide(void);
int NpUnlockStadiumsDecide(void);
#ifdef __cplusplus
}
#endif
static inline int NpUnlockCaptains(void)
{
    int v = g_npUnlockCaptains;
    if (v < 0) v = NpUnlockCaptainsDecide();
    return v;
}
static inline int NpUnlockStadiums(void)
{
    int v = g_npUnlockStadiums;
    if (v < 0) v = NpUnlockStadiumsDecide();
    return v;
}
#else
static inline int NpUnlockCaptains(void) { return 0; }
static inline int NpUnlockStadiums(void) { return 0; }
#endif

#endif // NP_UNLOCKS_H
