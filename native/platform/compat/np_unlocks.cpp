// The three locked captains (default on the Vita) and the three cup stadiums (default off) unlocked by the build: see include/np/unlocks.h.
#include "np/unlocks.h"

#include <stdio.h>
#include <stdlib.h>

extern "C" {
int g_npUnlockCaptains = -1;

int NpUnlockCaptainsDecide(void)
{
    int on = NP_UNLOCK_CAPTAINS_DEFAULT ? 1 : 0;
    const char* e = getenv("MSC_UNLOCK_CAPTAINS");
    if (e && *e) on = (*e != '0') ? 1 : 0;
    g_npUnlockCaptains = on;
    if (on)
        fprintf(stderr, "[unlock] captains Bowser Jr., Diddy Kong and Petey Piranha are selectable without the cup wins (%s); the save's "
                        "cup trophies and stadiums are not changed\n",
                e && *e ? "MSC_UNLOCK_CAPTAINS" : "the build's default");
    return on;
}

int g_npUnlockStadiums = -1;

int NpUnlockStadiumsDecide(void)
{
    int on = NP_UNLOCK_STADIUMS_DEFAULT ? 1 : 0;
    const char* e = getenv("MSC_UNLOCK_STADIUMS");
    if (e && *e) on = (*e != '0') ? 1 : 0;
    g_npUnlockStadiums = on;
    if (on)
        fprintf(stderr, "[unlock] stadiums Lava Pit, Crystal Canyon and Stormship are selectable without the cup wins (%s); the save's cup "
                        "trophies and captains are not changed\n",
                e && *e ? "MSC_UNLOCK_STADIUMS" : "the build's default");
    return on;
}
}
