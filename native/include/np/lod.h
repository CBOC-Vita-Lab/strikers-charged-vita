/* np/lod.h: screen-size level of detail for everything that moves or repeats (1.3, wave 5). Render-only: the inputs are a position, a bounding radius and the
 * view the draw already uses; the outputs are which index buffer of a model is drawn and whether it is drawn at all. Nothing here reads or writes simulation
 * state, and nothing the simulation reads (pose, blend, node matrices) depends on it.
 *
 *   MSC_LOD_SIZE=0|1|2   0 (default on the host): the game's own choices. 1: the game's lower mesh is chosen by projected size (characters in replays and
 *                        cut-scenes too; open play keeps its low mesh). 2: also meshes built at load by the simplifier (np/lod_simplify.h) where the data has no
 *                        lower one, and the draws under MSC_LOD_CULL_PX are not made.
 *   MSC_LOD_PX=a,b,c     the sizes in screen pixels (of the 544-line screen) under which level 1, 2, 3 is used (descending). Defaults in np/lod.h.
 *   MSC_LOD_HYST=h       the hysteresis band, a fraction of a threshold (0.15): to go to a lower level the size must fall under t*(1-h), to come back it must
 *                        pass t*(1+h).
 *   MSC_LOD_CULL_PX=p    objects whose projected size is under p pixels are not drawn (2). 0: never.
 *   MSC_LOD_LOG=1        one line per 600 frames: draws per level, triangles saved. MSC_LOD_INV=1: the inventory of every model loaded (id, packets, triangles).
 *
 * The size is the projected DIAMETER of the object's bounding sphere on a 544-line screen, so the same number means the same on the Vita at 960x544, 720x404
 * and 480x272 (it is measured in screen lines, not in rendered pixels). The policy is plain C so a host test can compile it (native/tools/lod).
 */
#ifndef NP_LOD_H
#define NP_LOD_H

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define NP_LOD_MAX_LEVELS 4          /* levels 0..3 (0 is the full mesh) */
#define NP_LOD_SCREEN_LINES 544.0f

/* Projected diameter in screen lines of a sphere of `radius` at `depth` in front of the camera. `projY` is the projection's vertical scale (cot(fov/2), mproj.m22). */
static inline float NpLodProjectedPx(float radius, float depth, float projY)
{
    if (depth <= 0.0001f) return 1.0e9f;   /* at or behind the camera plane: as large as it gets, never reduced or culled */
    return 2.0f * radius * projY / depth * (NP_LOD_SCREEN_LINES * 0.5f);
}

/* Which level for a size, with hysteresis. thr[i] is the size under which level i+1 is wanted (n thresholds, descending: thr[0] > thr[1] > ...). prev is the
 * level used the last time this object was drawn (0 when it has none). The result moves by as many levels as the size demands but only across a band:
 * down in detail when size < thr*(1-h), up when size > thr*(1+h). So a size that stays within thr*(1 +- h) never changes the level. */
static inline int NpLodSelect(float px, int prev, const float* thr, int n, float h)
{
    int level = prev < 0 ? 0 : (prev > n ? n : prev);
    while (level < n && px < thr[level] * (1.0f - h)) ++level;
    while (level > 0 && px > thr[level - 1] * (1.0f + h)) --level;
    return level;
}

/* Is the draw made? Under `cullPx` it is not (0: always made). The same band as above keeps an object that hovers at the limit from blinking. */
static inline int NpLodCulled(float px, int prevCulled, float cullPx, float h)
{
    if (cullPx <= 0.0f) return 0;
    return prevCulled ? (px < cullPx * (1.0f + h)) : (px < cullPx * (1.0f - h));
}

/* The largest geometric error (world units) a built mesh may have when it is used from size `px` down: one screen line at that size. */
static inline float NpLodMaxError(float diameter, float px, float pixelBound)
{
    return px > 1.0f ? diameter / px * pixelBound : diameter * pixelBound;
}

/* The scene classes (task states): the thresholds differ. Open play's figures are small and many; replays and cut-scenes show a few large ones; menus keep the game's. */
enum NpLodScene { NP_LOD_SCENE_PLAY = 0, NP_LOD_SCENE_REPLAY = 1, NP_LOD_SCENE_NIS = 2, NP_LOD_SCENE_OTHER = 3, NP_LOD_SCENES = 4 };

static inline int NpLodParseList(const char* e, float* out, int max)
{
    int n = 0;
    while (e && *e && n < max)
    {
        char* end = 0;
        float v = (float)strtod(e, &end);
        if (end == e) break;
        out[n++] = v;
        e = *end == ',' ? end + 1 : end;
        if (*end != ',') break;
    }
    return n;
}

#ifdef NP_NATIVE
#ifdef __cplusplus
struct glModel;
extern "C" {
#else
struct glModel;
#endif
int NpLodMode(void);                 /* MSC_LOD_SIZE 0..2 */
float NpLodHyst(void);
float NpLodCullPx(void);
/* scene class of a task state (NpLodScene) */
int NpLodSceneOf(unsigned long taskState);
/* the level (0 full, 1 the game's lower mesh or the built L1, 2 the built L2; never above 1 in mode 1) for a drawn character of size px, with hysteresis
 * per `key` (the character); *cull is set when the object is under MSC_LOD_CULL_PX. Render thread only. */
int NpLodChoose(const void* key, int scene, float px, int* cull);
/* loaded character model: build the simplified levels. kind 0: the full mesh (builds L1 and L2 when hasLow is 0, else nothing), 2: the game's lower mesh (L2). */
void NpLodBuild(glModel* m, int kind, int hasLow);
/* the draw's per-frame copy of a model: swap in the built index buffers and display lists of `level` (1 or 2) for the model whose id src has; returns 1 if swapped */
int NpLodApply(glModel* dup, const glModel* src, int level);
void NpLodNote(int scene, int level, const glModel* drawn, const glModel* full);   /* one draw, for MSC_LOD_LOG */
#ifdef __cplusplus
}
#endif
#else
static inline int NpLodMode(void) { return 0; }
#endif

#endif /* NP_LOD_H */
