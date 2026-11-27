/* np/scenecut.h: drawing cuts for the scenes that run slowest (1.3), all default OFF and all render-only.
 *
 *   MSC_FX_GLOW=full|half      full (default): the game's glow chain. half: the chain stops at hr160u and the composite stretches that picture: the last pass (the one
 *                              full-size 4-texture quad of the chain, 61 % of its fragments) and its copy are not made. HighRange.cpp.
 *   MSC_SHADOW_EVERY=<1..4>    1 (default): in replays and cut-scenes every player's shadow picture is redrawn every frame, as the game does. n: every n-th frame, staggered by
 *                              player, as open play already does with 2. DrawableCharacter.cpp.
 *   MSC_REPLAY_FARLOD=<units>  0 (default): replays and cut-scenes draw the full mesh of every player. >0: players further than this from the camera use the mesh of open play
 *                              (CHAR_MODEL_LOW_POLY). DrawableCharacter.cpp.
 *   MSC_CROWD_REPLAY=<1..4>    1 (default): a non-gameplay camera refreshes every crowd sprite and the crowd cluster every frame, as the game does. n: at least n times less often.
 *                              The crowd is the only list here in a match: its capture has no callback and draws no random number (GameRenderTask.cpp has the proof). The front
 *                              end's characters use the same manager and do draw numbers: they never reach this floor (no cluster in that list).
 *   MSC_CROWD_CLUSTER_LAZY=0|1 0 (default): the game. 1: no cluster capture in a stadium with no model that reads the cluster's picture (CrowdImpostors.cpp): the same picture.
 *   MSC_MENU_LOD=<0..3>        0 (default): the game. 1: the front end's captains use the low mesh of a match. 2: also their sprites refresh every second frame. 3: also the
 *                              backdrop world of the menus is not drawn (a test of where the time goes, not a mode to ship). 1 takes effect at the next load of a captain.
 *   MSC_STADIUM_HEAVY=<ids>    empty (default). Stadium numbers, by comma (STAD_* in DB/BasicGameInfo.h: Pipeline Central is 8). From the load of such a stadium to its end: crowd
 *                              and world NPCs not drawn (as Stadium Crowd HIDE), shadows of replays and cut-scenes at least every 2nd frame, and through VgxHeavyStadium the
 *                              renderer's reductions (a smaller floor, small effects, half-size warble). Drawing only: nothing the simulation reads changes. StadiumHeavy.cpp.
 *   MSC_STADIUM_HEAVY_CROWD=0|1 1 (default): the profile hides the crowd and world NPCs. 0: it leaves them drawn. The disc's data gives no reason to hide the crowd in Pipeline Central.
 *   MSC_FX_HAZE=<1..4>         2 (default, acts only under the profile). Long-lived large additive haze (Mist_rain is the only template on the disc that fits NpSceneHazeTemplate):
 *                              one sprite in n is drawn, with n times the alpha up to 255, the others collapse. 1: all drawn. Which sprites is a hash of the pool slot: no random
 *                              number, no change to any particle. ParticleSystem.cpp.
 *   MSC_FX_HAZE_WIDE=0|1       0 (default). 1: under the profile MSC_FX_HAZE also thins what NpSceneHazeTemplateWide takes: Crystal Canyon's caustics, Wastelands' ash, Storm Ship's wind.
 *   MSC_SHADOW_RES=<72..144>   144 (default, the game). The side in pixels of the picture a player's shadow is drawn in; the texture is half of it (72 as the game). 96 and 72: a
 *                              coarser shadow, a quarter to a half of the shadow pass's pixels. Read once at start. RLViewLayers.cpp.
 *
 *   MSC_SHADOW_PLANE=0|1       1 (default since 25 Nov, nxr3b: host pictures show the planar shadows of banana peels and other items came out missing or broken up at z = 0).
 *                              Planar shadows (stadium objects, hammers, power-ups) are drawn on the game's plane, the ini's Shadow Height (0.1), instead of at z = 0 on
 *                              the pitch. The console takes that height through GX's Z-freeze from a culled quad, which the renderer does not model by default. 0: as
 *                              before (z = 0). A matrix term, no draw or shader. RenderShadow.cpp.
 *
 * The policy is here, without game headers, so that a host test can compile it (native/tools/scenecut). Nothing here reads or writes simulation state.
 */
#ifndef NP_SCENECUT_H
#define NP_SCENECUT_H

#include <stdlib.h>
#include <string.h>

// MSC_AB (wave 15, renderer/src/vgx_ab.h): weak, so a build without the renderer (tools, tests) links none of it; VgxAbOn is 0 unless MSC_AB is set.
#ifdef __cplusplus
extern "C" {
extern int VgxAbOn __attribute__((weak));
long VgxAbOverride(const char* key, long dflt) __attribute__((weak));
int VgxHeldGovLevel(void) __attribute__((weak));   // scenes30 MSC_HELD_GOV (renderer/src/vgx_heldgov.h): 0 none .. 4, only in a held cut-scene or replay while its GPU time is over 33.3 ms
}
#endif

// scenes30 MSC_HELD_GOV: what the held-scene governor's level does to the knobs the game reads (the renderer's level, VgxHeldGovLevel; 0 outside held scenes and with the switch off).
// Level 1: the crowd's refresh floor one more (only where the floor applies, floor > 1: MSC_CROWD_REPLAY 2 -> 3). Level 2: shadows one picture less often (MSC_SHADOW_EVERY 2 -> 3).
// Level 3: the glow chain every third picture (MSC_HELD_GLOW_HALF 1 -> 2; 0 stays the game's chain). Each capped at 4 as the switches are. Pure: host test native/tools/scenecut.
// held25 MSC_HELD_GOV_EFFECTS=1: the renderer reports 5 for level 4 where there is no size step (it was never used under LOCKED 60): the crowd floor +2, the shadow picture +2 (both capped at 4), the glow chain every 4th picture.
static inline int NpHeldGovCrowd(int floor, int level) { return level >= 5 && floor > 1 ? (floor + 2 > 4 ? 4 : floor + 2) : (level >= 1 && floor > 1 ? (floor + 1 > 4 ? 4 : floor + 1) : floor); }
static inline int NpHeldGovShadow(int every, int level) { return level >= 5 ? (every + 2 > 4 ? 4 : every + 2) : (level >= 2 ? (every + 1 > 4 ? 4 : every + 1) : every); }
static inline int NpHeldGovGlow(int rate, int level) { return level >= 5 && rate >= 1 ? 3 : (level >= 3 && rate >= 1 ? 2 : rate); }
#ifdef __cplusplus
static inline int NpHeldGovLevel() { return &VgxHeldGovLevel != 0 ? VgxHeldGovLevel() : 0; }
#endif

static inline int NpSceneGlowHalfParse(const char* e)
{
    return e && strcmp(e, "half") == 0;
}

// The last level of the chain and the level the composite may read: 6 (hr320u) as the game, 5 (hr160u) with half.
static inline int NpSceneGlowLastLevel(int half)
{
    return half ? 5 : 6;
}

static inline int NpSceneCompositeLevel(int half, int tweakIndex)
{
    const int last = NpSceneGlowLastLevel(half);
    return tweakIndex > last ? last : tweakIndex;
}

// MSC_FX_GLOW_DEAD=1 (gpu25): the glow chain does not draw its last level (hr320u, 640 x 448 in EFB units: a pass, its copy and a scene of its own on the console) when the composite
// reads an earlier one (the tweak miHighRangeIndex <= 5, the game's own value at The Dump): the composite is the only reader of the last level, so the picture is the same.
static inline int NpSceneGlowDeadParse(const char* e)
{
    return e && e[0] == '1';
}
static inline int NpSceneGlowDeadUnused(int tweakIndex)
{
    return tweakIndex <= 5;
}

// MSC_HELD_GLOW_HALF=0|1|2 (held30, owner-approved small trade-off 17 Nov 23:23): in a held scene (replay, cut-scene, walk-out: TASK_REPLAY / TASK_NIS) the glow chain's blur passes (six
// single-quad scenes on the console) are redrawn only every other picture (1) or every third (2); on the others the composite reads the chain's last result, one picture old. The capture of
// the EFB into level 0 stays (it carries the clear of the alpha plane the shadow volumes count in). Players, ball, items and HUD are drawn every picture; open play never skips.
static inline int NpSceneGlowRateParse(const char* e)
{
    int n = e ? atoi(e) : 0;
    return n < 0 ? 0 : (n > 2 ? 2 : n);
}
// n: pictures since the held scene began (0 is its first). The first kNpGlowWarm pictures are always drawn in full: the host run showed the walk-out's first pictures change the whole glow from
// one picture to the next (the entrance fades in), where a stale chain lost it. true: this picture's chain is not drawn.
#define kNpGlowWarm 6
static inline int NpSceneGlowSkipPicture(int mode, int n)
{
    if (mode <= 0 || n < kNpGlowWarm) return 0;
    return mode == 1 ? (n & 1) : (mode == 2 ? (n % 3 != 0) : (n % 4 != 0));
}

// MSC_GLOW_HALF_PLAY=0|1|2 (pace13, from Galaxy's half-rate bloom, +3.7 fps there; default 0): the same skip in LIVE play. 1: a step of the picture ladder (vgx_hold.h kFxSize in this mode, taken before any size step,
// kept only if it bought GPU time: the renderer's VgxGlowHalfPlay); 2: every live picture is half-rate (tests and crops: no ladder). The glow chain's blur passes are redrawn every other picture and the composite
// reads the last result, one picture old; the capture into level 0 stays. Render only: nothing here reaches the simulation (the chain draws no random numbers and no game state).
static inline int NpSceneGlowPlayParse(const char* e)
{
    int n = e ? atoi(e) : 0;
    return n < 0 ? 0 : (n > 2 ? 2 : n);
}
// n: live pictures in a row with the step in force (0 is the first, which is drawn in full); true: this picture's chain is not drawn.
static inline int NpSceneGlowPlaySkipPicture(int n)
{
    return n > 0 && (n & 1);
}

// MSC_FX_GLOW_COARSE=1: the glow chain takes the game's own coarse route (HighRange.cpp's `coarseLevels`: 320, 160, 80, then 160 and 320 up, without the 40 x 28 step
// and the first 80 x 56 step back up): four blur passes in place of six. The game has the route behind its `mbFineHighRange` tweak (default on).
static inline int NpSceneGlowCoarseParse(const char* e)
{
    return e && e[0] == '1';
}

// MSC_GLOW_L0OFF=0|1|2 (gem2a, nxr4b; see NpSceneGlowL0Mode): on a picture whose blur passes MSC_HELD_GLOW_HALF / MSC_GLOW_HALF_PLAY skip, the capture of the EFB into level 0 (hr320d) is skipped too, nothing reads it then.
// MSC_HELD_GLOW_STAGGER=0|1 (gem2a, default 0, VISIBLE: the bloom lags one more picture on every other picture): with MSC_HELD_GLOW_HALF=1 the chain is split over the two pictures
// of a pair, the capture and the three steps down on one, the three steps up on the other, in place of all six on one and none on the other.
static inline int NpSceneGlowFlagParse(const char* e, int dflt)
{
    return e && e[0] ? (e[0] != '0') : dflt;
}

static inline int NpSceneShadowEveryParse(const char* e)
{
    int n = e ? atoi(e) : 1;
    return n < 1 ? 1 : (n > 4 ? 4 : n);
}

static inline float NpSceneFarLodParse(const char* e)
{
    float d = e ? (float)atof(e) : 0.0f;
    return d > 0.0f ? d : 0.0f;
}

static inline int NpSceneCrowdReplayParse(const char* e)
{
    int n = e ? atoi(e) : 1;
    return n < 1 ? 1 : (n > 4 ? 4 : n);
}

// The floor of the throttle under a camera that updates clusters. Only a list with a cluster is the crowd's: without one it is the front end's, which keeps the game's rate.
static inline int NpSceneCrowdFloor(int updateClusters, int hasClusters, int crowdReplay)
{
    return updateClusters && hasClusters ? crowdReplay : 1;
}

// Is the cluster captured this frame? As the game unless the switch is on and nothing reads the picture.
static inline int NpSceneClusterWanted(int updateClusters, int lazy, int readers)
{
    return updateClusters && (!lazy || readers > 0);
}

static inline int NpSceneMenuLodParse(const char* e)
{
    int n = e ? atoi(e) : 0;
    return n < 0 ? 0 : (n > 3 ? 3 : n);
}

// Does the list of stadium numbers name this one? Comma or space separated, anything that is not a number ends the list. STAD_INVALID (-1) and an empty list: no.
static inline int NpSceneHeavyHas(const char* list, int stadium)
{
    if (!list || stadium < 0) return 0;
    for (const char* p = list;;)
    {
        while (*p == ',' || *p == ' ') ++p;
        if (*p < '0' || *p > '9') return 0;
        int n = 0;
        while (*p >= '0' && *p <= '9')
        {
            if (n < 1000) n = n * 10 + (*p - '0');
            ++p;
        }
        if (n == stadium) return 1;
    }
}

static inline int NpSceneHeavyShadowEvery(int every, int heavy)
{
    return heavy && every < 2 ? 2 : every;
}

// The sprites MSC_FX_HAZE thins: additive, at least 6 world units wide, living at least 6 s, from a fountain that runs for a time (not a burst). Of the templates of all the disc's
// bundles only Pipeline Central's Mist_rain fits (12 units, 9 s).
static inline int NpSceneHazeTemplate(int additive, float size, float life, float fountain)
{
    return additive && size >= 6.0f && life >= 6.0f && fountain > 0.0f;
}

// MSC_FX_HAZE_WIDE: the same with lower floors, and a ceiling that leaves out effects.bun's ball glow (24 units; a gameplay signal). Of all the disc's templates it takes fx_mistrain,
// fx_crystalCaustic, fx_snow (Wastelands' ash) and fx_stormWind.
static inline int NpSceneHazeTemplateWide(int additive, float size, float life, float fountain)
{
    return additive && size >= 5.0f && size <= 12.0f && life >= 3.0f && fountain > 0.0f;
}

static inline int NpSceneHazeParse(const char* e)
{
    int n = e && *e ? atoi(e) : 2;
    return n < 1 ? 1 : (n > 4 ? 4 : n);
}

// MSC_SHADOW_RES: an even side from 72 to 144, the game's 144 for anything else
static inline int NpSceneShadowResParse(const char* e)
{
    const int n = e ? atoi(e) : 144;
    return n >= 72 && n <= 144 && (n & 1) == 0 ? n : 144;
}

// MSC_SHADOW_PLANE: the height of a flattened model's translation in its shadow matrix. A flattened mesh lies at z = 0; GX gave its pixels the depth of
// the reference plane (coplanarZ) when at least one packet of it was indexed (RLView::BeginPacket freezes only those).
static inline int NpSceneShadowPlaneParse(const char* e)
{
    return e && *e ? (atoi(e) != 0 ? 1 : 0) : 1;   // (on unless named 0: nxr3b, 25 Nov)
}
static inline float NpSceneShadowPlaneZ(int on, int frozenPackets, float coplanarZ)
{
    return on && frozenPackets ? coplanarZ : 0.0f;
}

// The alpha of a thinned sprite (MSC_FX_THIN, MSC_FX_HAZE). Up to alpha full one sprite in keep shows keep times the alpha and the others none; from full to full + span that fades
// out, so that the expected sum stays a. Clamped: past 255 a kept sprite loses what does not fit.
static inline unsigned NpSceneThinAlpha(unsigned a, unsigned full, unsigned span, unsigned keep, int kept)
{
    if (span == 0) span = 1;   // (a zero fade width would divide by zero: 0 on the console, a crash on the host)
    const unsigned sk = a <= full ? 0u : a - full;   // s * span
    const unsigned out = kept ? (a * (keep * span - (keep - 1u) * sk) + span / 2u) / span : (a * sk + span / 2u) / span;
    return out > 255u ? 255u : out;
}

// MSC_FX_GOV: the weight (256 = 1) of a sprite's alpha at governor level v, 0 (none) .. 256 (one in two) .. 512 (one in four); slot: 8 bits, fixed for the sprite's life. Half the
// sprites fade out as the other half rises by as much; then half of the rest. The mean over the slots stays 256, at every v: additive light keeps its sum, and nothing pops.
static inline unsigned NpSceneGovWeight(unsigned slot, unsigned v)
{
    if (v > 512u) v = 512u;
    if (v <= 256u) return (slot & 1u) ? 256u - v : 256u + v;
    const unsigned f = v - 256u;
    if (slot & 1u) return 0u;
    return (slot & 2u) ? 512u - 2u * f : 512u + 2u * f;
}

// Is the open-play mesh drawn? Always in open play; elsewhere only with the switch on, in a replay or cut-scene, for a player beyond the distance.
static inline int NpSceneUseLowPoly(int gameplayState, int replayOrNisState, float cutoff, float distanceSquared)
{
    if (gameplayState) return 1;
    return replayOrNisState && cutoff > 0.0f && distanceSquared > cutoff * cutoff;
}

#ifdef NP_NATIVE
#ifdef __cplusplus
static inline int NpSceneGlowCoarse()
{
    static int v = -1;
    if (v < 0) v = NpSceneGlowCoarseParse(getenv("MSC_FX_GLOW_COARSE"));
    return v;
}
// MSC_GLOW_L0OFF value (default 2): 0 off (the copy is made), 1 the old form (level 0 gets no target: no copy AND no clear of the EFB alpha plane, which the shadow volumes count in: NOT exact, 10/32 pictures differ, gem2a),
// 2 (nxr4b) the copy is marked clear-only: the clear of the alpha plane is made inside the EFB scene, only the copy (and the scene it splits) is left out.
static inline int NpSceneGlowL0Mode()
{
    static int v = -1;
    if (v < 0)
    {
        const char* e = getenv("MSC_GLOW_L0OFF");
        v = e && e[0] ? (e[0] == '2' ? 2 : (e[0] != '0' ? 1 : 0)) : 2;   // (nxr4b: default 2, the exact form; =0 is the old behaviour)
    }
    return v;
}
static inline int NpSceneGlowL0Off()
{
    return NpSceneGlowL0Mode() == 1;
}
static inline int NpSceneGlowStagger()
{
    static int v = -1;
    if (v < 0) v = NpSceneGlowFlagParse(getenv("MSC_HELD_GLOW_STAGGER"), 0);
    return v;
}
// MSC_HELD_GLOW_STAGGER=2 (nxr4b, VISIBLE, default off): the stagger also at the rates the held governor raises the skip to (HALF=2 or 3, every third / fourth picture): the drawn picture makes the
// capture and the steps down, the picture after it the steps up, the pictures after those (rate 2: none, rate 3: one more) skip the chain as before. 1: the rate-1 pair only.
static inline int NpSceneGlowStaggerAllRates()
{
    static int v = -1;
    if (v < 0)
    {
        const char* e = getenv("MSC_HELD_GLOW_STAGGER");
        v = e && e[0] == '2' ? 1 : 0;
    }
    return v;
}
// MSC_HELD_GLOW_STAGGER_DEAD=1 (nxvis25, VISIBLE class like STAGGER, default off, needs MSC_HELD_GLOW_STAGGER): the stagger also on a chain whose last level the composite does not read (MSC_FX_GLOW_DEAD on The Dump, index 5):
// the steps down are levels 1..3, the steps up levels 4..5 (level 6 is not drawn at all there). Before this the stagger was off for the whole of The Dump.
static inline int NpSceneGlowStaggerDead()
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("MSC_HELD_GLOW_STAGGER_DEAD"); v = e && e[0] == '1' ? 1 : 0; }
    return v;
}
static inline int NpSceneGlowRate()
{
    static int v = -1;
    if (v < 0) v = NpSceneGlowRateParse(getenv("MSC_HELD_GLOW_HALF"));
    return NpHeldGovGlow(v, NpHeldGovLevel());   // (scenes30 MSC_HELD_GOV level 3: every third picture; asked only in held scenes)
}
static inline int NpSceneGlowPlay()
{
    static int v = -1;
    if (v < 0) v = NpSceneGlowPlayParse(getenv("MSC_GLOW_HALF_PLAY"));
    return v;
}
static inline int NpSceneGlowDead()
{
    static int v = -1;
    if (v < 0) v = NpSceneGlowDeadParse(getenv("MSC_FX_GLOW_DEAD"));
    return v;
}
static inline int NpSceneGlowHalf()
{
    static int v = -1;
    if (v < 0) v = NpSceneGlowHalfParse(getenv("MSC_FX_GLOW"));
    return v;
}
// (StadiumHeavy.cpp) from the load of a stadium named by MSC_STADIUM_HEAVY to the end of that stadium; stadium < 0 ends it
int NpStadiumHeavy();
int NpStadiumHeavyCrowd();   // the profile, and the crowd goes with it (MSC_STADIUM_HEAVY_CROWD)
static inline int NpSceneHazeEvery()
{
    static int v = 0;
    if (v == 0) v = NpSceneHazeParse(getenv("MSC_FX_HAZE"));
    return v;
}
static inline int NpSceneHazeWide()
{
    static int v = -1;
    if (v < 0)
    {
        const char* e = getenv("MSC_FX_HAZE_WIDE");
        v = e && e[0] == '1' ? 1 : 0;
    }
    return v;
}
void NpStadiumHeavySet(int stadium);
static inline int NpSceneShadowEvery()
{
    static int v = 0;
    if (v == 0) v = NpSceneShadowEveryParse(getenv("MSC_SHADOW_EVERY"));
    int w = v;
    if (&VgxAbOn && VgxAbOn) { const long o = VgxAbOverride("MSC_SHADOW_EVERY", -1); if (o >= 0) w = o < 1 ? 1 : (o > 4 ? 4 : (int)o); }   // (MSC_AB, renderer/src/vgx_ab.h: the in-run A/B harness answers while it flips this key)
    return NpHeldGovShadow(NpSceneHeavyShadowEvery(w, NpStadiumHeavy()), NpHeldGovLevel());   // (scenes30 MSC_HELD_GOV level 2: one picture more; callers ask only in replays and cut-scenes)
}
static inline float NpSceneFarLod()
{
    static float v = -1.0f;
    if (v < 0.0f) v = NpSceneFarLodParse(getenv("MSC_REPLAY_FARLOD"));
    if (&VgxAbOn && VgxAbOn) { const long o = VgxAbOverride("MSC_REPLAY_FARLOD", -1); if (o >= 0) return (float)o; }   // (MSC_AB: as above)
    return v;
}

static inline int NpCrowdReplay()
{
    static int v = 0;
    if (v == 0) v = NpSceneCrowdReplayParse(getenv("MSC_CROWD_REPLAY"));
    return v;
}
static inline int NpCrowdClusterLazy()
{
    static int v = -1;
    if (v < 0)
    {
        const char* e = getenv("MSC_CROWD_CLUSTER_LAZY");
        v = e && *e && *e != '0';
    }
    return v;
}
// MSC_CROWD_CLUSTERFLAG=0|1 (default 0): ImpostorCluster's constructor sets ImpostorManager::mHasClusters (AddCharacter, which should have set it, runs in the base constructor before mIsCluster is
// true, so in 1.3 and before the flag was never true and MSC_CROWD_REPLAY's floor never applied in a match).
static inline int NpCrowdClusterFlag()
{
    static int v = -1;
    if (v < 0)
    {
        const char* e = getenv("MSC_CROWD_CLUSTERFLAG");
        v = e && *e && *e != '0';
    }
    return v;
}
static inline int NpMenuLod()
{
    static int v = -1;
    if (v < 0) v = NpSceneMenuLodParse(getenv("MSC_MENU_LOD"));
    return v;
}

// MSC_STADIUM_CENSUS=1 (diagnostic): one block at each stadium load: the world's objects by type and the geometry of its drawables (Game/Render/StadiumCensus.cpp)
class World;
void NpStadiumCensusObject(unsigned long type);
void NpStadiumCensusReport(int stadium, const World* world);
#endif
#else
#ifdef __cplusplus
static inline int NpStadiumHeavy() { return 0; }
static inline int NpStadiumHeavyCrowd() { return 0; }
static inline void NpStadiumHeavySet(int) {}
static inline int NpCrowdReplay() { return 1; }
static inline int NpCrowdClusterLazy() { return 0; }
static inline int NpCrowdClusterFlag() { return 0; }
static inline int NpMenuLod() { return 0; }
#endif
#endif

#endif // NP_SCENECUT_H
