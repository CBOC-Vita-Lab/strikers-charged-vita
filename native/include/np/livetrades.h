/* np/livetrades.h: the 1.3 "live play trades" (owner decision, 12 Nov 2026): to hold 960x544 at 60 with anti-aliasing in LIVE PLAY ONLY (TASK_GAMEPLAY; replays, cut-scenes,
 * celebrations, Mega Strikes and menus keep the full look at their steady 30), some work is done more cheaply. Render side only: nothing the simulation reads or draws a number for.
 *
 *   MSC_LIVE_TRADES=0|1     0 (code default; the Vita plan sets 1 with the FULL picture policy). The master: the trades below follow it unless they are named.
 *   MSC_SHADOW_LIVE=full|flat   flat: a player's shadow in live play is one soft dark ellipse quad on the pitch, cast along the light as the real shadow's quad is, in the main
 *                           scene: no shadow picture is drawn (no pass, no copy, no model copy for it). full: the game's shadow pictures. Unset: follows MSC_LIVE_TRADES (1 = flat).
 *   MSC_SHADOW_LIVE_W / _L / _A / _LIFT   (diagnostic/tuning, floats) ellipse width and length as a fraction of the shadow quad, peak alpha as a fraction of the game's, lift fade.
 *   MSC_CROWD_LIVE=0|1      1: while pictures run at 60 the crowd sprites refresh at the rate in time they have at 30 (see NpCrowdLive30). Unset: 0 (off; it does NOT follow MSC_LIVE_TRADES:
 *                           coordinator decision 12 Nov, the near crowd animating at 7.5 a second shows its steps; kit run crowdlive1 lets the console judge it).
 */
#ifndef NP_LIVETRADES_H
#define NP_LIVETRADES_H

#include <stdlib.h>
#include <string.h>

static inline int NpLiveTrades(void)
{
    static int v = -1;
    if (v < 0)
    {
        const char* e = getenv("MSC_LIVE_TRADES");
        v = e && *e && *e != '0';
    }
    return v;
}

/* MSC_SHADOW_LIVE: 1 flat, 0 full; unset follows the master */
static inline int NpShadowLiveFlat(void)
{
    static int v = -1;
    if (v < 0)
    {
        const char* e = getenv("MSC_SHADOW_LIVE");
        if (e && *e)
        {
            v = (!strcmp(e, "flat") || !strcmp(e, "1")) ? 1 : 0;
        }
        else
        {
            v = NpLiveTrades();
        }
    }
    return v;
}

/* MSC_CROWD_LIVE=0|1: while pictures run at 60 and the gameplay camera is in use, the crowd sprites refresh at the rate in time they have at 30 on the Vita (the crowd half of MSC_AMORT_60, without
 * its shadow half): GameRenderTask passes the policy to NpCrowdAt60. Unset: follows MSC_LIVE_TRADES. Host census (w8/trade.md): about 25 fewer draws a frame in open play (8 sprite passes a frame
 * in place of 16), a sprite's picture changes 7.5 times a second instead of 15. At 30 pictures a second nothing changes (NpFpsPolicy is not 2). Off unless MSC_CROWD_LIVE=1. */
static inline int NpCrowdLive30(void)
{
    static int v = -1;
    if (v < 0)
    {
        const char* e = getenv("MSC_CROWD_LIVE");
        v = (e && *e) ? (*e != '0') : 0;   /* off unless named: the near crowd's steps at 7.5 a second are visible (12 Nov decision) */
    }
    return v;
}

/* MSC_SHADOW_CUT=n (default 3, 0..10): at the first frames outside live play after flat shadows, at most n stale shadow pictures are redrawn a frame (those in view first); the others show the
 * flat shadow until theirs is ready. 0: all of them on the first frame (the first 1.3 rule). */
static inline int NpShadowCutBudget(void)
{
    static int v = -1;
    if (v < 0)
    {
        const char* e = getenv("MSC_SHADOW_CUT");
        v = (e && *e) ? atoi(e) : 3;
        v = v < 0 ? 0 : (v > 10 ? 10 : v);
    }
    return v;
}

/* MSC_SHADOW_LIVE_BREAK=1 (test only): flat shadows are drawn in every scene, not live play alone. The negative control of the held-scene hash proof (w8/trade9.md): it must be caught. */
static inline int NpShadowLiveBreak(void)
{
    static int v = -1;
    if (v < 0)
    {
        const char* e = getenv("MSC_SHADOW_LIVE_BREAK");
        v = e && *e && *e != '0';
    }
    return v;
}

static inline float NpLiveFloat(const char* name, float fallback)
{
    const char* e = getenv(name);
    return e && *e ? (float)atof(e) : fallback;
}

#endif
