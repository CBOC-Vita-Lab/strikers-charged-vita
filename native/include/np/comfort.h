/* np/comfort.h: comfort options that only change what is drawn (1.2).
 *
 * MSC_REDUCED_MOTION=1 (default off): camera shake is a quarter of its size. The two camera filters that shake the match camera
 * (cNoiseFilter: Daisy's fist, the Donkey Kong super hit and the like; cRumbleFilter: the shot-presentation kick) multiply the displacement they were
 * started with by NpShakeScale(). Only the displacement is scaled: the random draws of cNoiseFilter::Start and every timer stay exactly as they were,
 * so the simulation, its random stream and the length of each effect do not depend on the switch. (The Galaxy port also halves its zoom blur; this game
 * has no zoom blur to scale, only the shake.)
 * 1.3: the choice applies to the running match when it is saved in the settings screen (np/live_settings.h); the displacement of a shake that
 * already started keeps the size it started with.
 */
#ifndef NP_COMFORT_H
#define NP_COMFORT_H

#ifdef NP_NATIVE
#include "np/live_settings.h"

static inline float NpShakeScale(void)
{
    /* one shared cell (np/live_settings.h): the first read takes MSC_REDUCED_MOTION from the environment, a save in the settings screen changes it live */
    return NpLiveShakeQuarter() ? 0.25f : 1.0f;
}
#else
static inline float NpShakeScale(void) { return 1.0f; }
#endif

#endif /* NP_COMFORT_H */
