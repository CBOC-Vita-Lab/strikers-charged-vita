/* np/unlocks.h: the three captains the original game keeps locked, unlocked by default on the Vita (W5-CAM, the maintainer 17 Oct: "let's unlock the 3
 * missing captains by default").
 *
 * Retail: twelve captains, three locked until a cup of Road to the Striker Cup is won: Bowser Jr. (Fire Cup), Diddy Kong (Crystal Cup) and
 * Petey Piranha (Striker Cup). There is no captain flag of its own: the unlock is the cup-won bit of CupManager::mCupRecord.mUnlockFlags
 * (1, 2, 4; saved), which ALSO unlocks the stadiums Lava Pit / Crystal Canyon / Stormship and the cup's trophy in the Hall of Fame, and makes
 * the cup's final opponent random instead of the locked captain (CupManager::SelectFinalOpponents). Setting those bits would claim three cup
 * trophies that were never won, so this switch does not touch the save: it answers yes in the game's own captain predicates
 * (IsBowserJrUnlocked, IsDiddyKongUnlocked, IsPeteyUnlocked; GameProgress.cpp), the place where the game's own overrides already act
 * (the unlock-all tweak, and online play, which is how a LAN match already offers all twelve captains). Every reader of the predicates
 * (captain select, its random pick, the cup's final opponents, the Hall of Fame summary) follows; stadiums, trophies and cheats stay as
 * the save has them, and nothing that the save has unlocked can be locked by it.
 *
 *   MSC_UNLOCK_CAPTAINS=1|0   (environment; env.txt / np-player.txt on the Vita, the Settings screen's BONUS CAPTAINS) overrides the build's default
 *   NP_UNLOCK_CAPTAINS_DEFAULT  1 for the Vita (__vita__), 0 elsewhere (host32, armhf and the regression suite keep retail)
 *
 * The same, for the three stadiums of the same cup bits (the maintainer has not decided on them: default OFF everywhere): MSC_UNLOCK_STADIUMS=1 (BONUS
 * STADIUMS) answers yes in IsLavaPitUnlocked / IsCrystalCanyonUnlocked / IsStormshipUnlocked: stadium select (and its random pick) and the
 * stadium's preview movie follow; the cups' own stadium lists, the trophies and the save do not change. (The Wastelands, The Dump and Galactic
 * Stadium are locked by the cups' goal trophies, other bits: not covered.)
 */
#ifndef NP_UNLOCKS_H
#define NP_UNLOCKS_H

#ifndef NP_UNLOCK_CAPTAINS_DEFAULT
#if defined(__vita__)
#define NP_UNLOCK_CAPTAINS_DEFAULT 1
#else
#define NP_UNLOCK_CAPTAINS_DEFAULT 0
#endif
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
