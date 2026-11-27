/* np/amortize.h: render work that need not happen every frame (1.3), each behind its own switch, default OFF, render-only.
 *
 *   MSC_VIEW_LAYERS=0|1  0 (default; MSC_BG30 on makes it 1). 1: the layer markers (RLView::BeginRender) name the live view also when the two-stage pipeline sends a shadow of it. With 0 every draw of a
 *                        forked frame is layer 0, class unknown: the census cannot tell the crowd from the stadium, and MSC_DYNRES_FG's split sees no background.
 *   MSC_AMORT_LOG=1      Every 300 frames of match play, how far the picture moves from one frame to the next: the measure of what a held picture would show.
 *   MSC_FX_HAZE_CACHE=0|2|3|4   0 (default). n: in open play the haze (NpSceneHazeTemplate, with MSC_FX_HAZE_WIDE also its wide list) is drawn into a small target every n-th frame and that
 *                        picture is added to every frame, moved by what the camera did since (MSC_FX_HAZE_CACHE_Z, default 5: the height of the plane it is taken to lie in).
 *                        Particles update and emit as ever: only the drawing of the held frames is left out, and no draw path takes a random number. The haze no longer
 *                        goes behind anything: nothing the camera sees stands above it in open play. A camera that jumps, any other scene, warble or Mega Strike: live.
 *   MSC_SHADOW_PLAY=2|3|4       2 (default, the game's). In open play a visible player's shadow picture is redrawn every n-th frame. The shadow still moves with the player every frame.
 *   MSC_AMORT_60=0|1     0 (default). 1: while pictures run at 60 (NpFpsPolicy 2), the crowd sprites and the players' shadow pictures keep the rate in time that they have at 30
 *                        (each interval twice as many frames), instead of doubling their work with the frame rate. Only under a 60 that is in force: a missed frame ends AUTO's
 *                        60 at once, so it cannot wait for the miss.
 *
 * The arithmetic is here, without game headers, so that a host test can compile it (native/tools/amortize).
 */
#ifndef NP_AMORTIZE_H
#define NP_AMORTIZE_H

#include "np/bg30_env.h"
#include <stdlib.h>

static inline int NpViewLayers(void)
{
    static int v = -1;
    if (v < 0)
    {
        const char* e = getenv("MSC_VIEW_LAYERS");
        v = e && e[0] == '1';
        /* 1.3 MSC_BG30 (bg30c): the background / foreground split of a light picture needs the draw classes of the forked (two-stage) frames, which are all "unknown" without this
           (the first draw of every console picture was class 0, so the plan found no background prefix: "fallbacks: prefix N"). MSC_BG30 off: nothing changes. */
        if (!v)
        {
            if (NpBg30ParseMode(getenv("MSC_BG30")) != 0) v = 1;   /* (np/bg30_env.h: the one parser) */
        }
    }
    return v;
}

static inline int NpAmortEnvInt(const char* name, int fallback)
{
    const char* e = getenv(name);
    return e && *e ? atoi(e) : fallback;
}

static inline int NpHazeCacheParse(int n) { return n < 2 ? 0 : (n > 4 ? 4 : n); }
static inline int NpShadowPlayParse(int n) { return n < 2 ? 2 : (n > 4 ? 4 : n); }

/* The intervals of a player's shadow picture in open play. policy60: MSC_AMORT_60 and pictures at 60. */
static inline int NpShadowVisibleEvery(int game, int play, int policy60)
{
    const int n = play > game ? play : game;
    return policy60 ? n * 2 : n;
}
static inline int NpShadowHiddenEvery(int game, int policy60)
{
    return policy60 ? game * 2 : game;
}
/* The floor of the crowd throttle: 1..4, as MSC_CROWD_REFRESH */
static inline int NpCrowdAt60(int floor, int policy60)
{
    const int n = policy60 ? floor * 2 : floor;
    return n > 4 ? 4 : n;
}

/* MSC_SHADOW_FAR=n (0 off, 1..3) and the FULL picture policy's shadow step (MSC_PICTURE_HOLD, renderer vgx_hold.h): frames added to the interval of a player's shadow PICTURE.
 * `far`: the player's projected size (np/lod.h NpLodProjectedPx of a unit sphere, screen lines of 544) is under MSC_SHADOW_FAR_PX (default 60: about 37 units from the camera).
 * Static part: a far player gets n frames. Policy step 1: a far player one more; step 2: a far player two more, every other player one. Only the shape of the shadow lags: its place
 * follows the player every frame (RenderProjectedShadow). No simulation state, no random number: the frame count is the renderer's own. */
static inline int NpShadowExtraFrames(int farOn, int far, int holdStep)
{
    int e = (farOn > 0 && far) ? (farOn > 3 ? 3 : farOn) : 0;
    if (holdStep > 0)
    {
        e += far ? (holdStep > 2 ? 2 : holdStep) : (holdStep > 2 ? 1 : holdStep - 1);
    }
    return e;
}
static inline int NpShadowIsFar(float px, float limit)
{
    return px < limit;
}

enum NpHazeAct
{
    NPHAZE_LIVE = 0,     /* the sprites go to the particle view, as the game drew them */
    NPHAZE_REFRESH = 1,  /* they go to the target, and the target is added to the picture */
    NPHAZE_HOLD = 2,     /* no sprites, the target as it stands is added to the picture */
};

struct NpHazeState
{
    int valid;  /* the target holds a picture from the frames before this one */
    int age;    /* frames since it was made */
};

/* One frame. every: MSC_FX_HAZE_CACHE; active: a scene that may be held; jumped: the camera moved further than a shift can stand for. */
static inline int NpHazeStep(struct NpHazeState* s, int every, int active, int jumped)
{
    if (every < 2 || !active)
    {
        s->valid = 0;
        s->age = 0;
        return NPHAZE_LIVE;
    }
    if (!s->valid || jumped || s->age + 1 >= every)
    {
        s->valid = 1;
        s->age = 0;
        return NPHAZE_REFRESH;
    }
    s->age++;
    return NPHAZE_HOLD;
}

/* a 4x4 matrix of floats, row vectors (v * M), as nlMatrix4 */
static inline void NpAmortXform(float out[4], const float* m, float x, float y, float z)
{
    for (int i = 0; i < 4; i++)
    {
        out[i] = x * m[i] + y * m[4 + i] + z * m[8 + i] + m[12 + i];
    }
}

/* The inverse of a view * projection (nlInvertMatrix takes the last column as 0 0 0 1). 0: none. */
static inline int NpAmortInvert(float out[16], const float* m)
{
    double a[4][8];
    for (int i = 0; i < 4; i++)
    {
        for (int j = 0; j < 4; j++)
        {
            a[i][j] = m[i * 4 + j];
            a[i][j + 4] = i == j ? 1.0 : 0.0;
        }
    }
    for (int c = 0; c < 4; c++)
    {
        int p = c;
        for (int r = c + 1; r < 4; r++)
        {
            const double x = a[r][c] < 0.0 ? -a[r][c] : a[r][c], y = a[p][c] < 0.0 ? -a[p][c] : a[p][c];
            if (x > y) p = r;
        }
        if (a[p][c] == 0.0)
        {
            return 0;
        }
        for (int j = 0; j < 8; j++)
        {
            const double t = a[c][j];
            a[c][j] = a[p][j];
            a[p][j] = t;
        }
        const double d = 1.0 / a[c][c];
        for (int j = 0; j < 8; j++) a[c][j] *= d;
        for (int r = 0; r < 4; r++)
        {
            if (r == c) continue;
            const double f = a[r][c];
            for (int j = 0; j < 8; j++) a[r][j] -= f * a[c][j];
        }
    }
    for (int i = 0; i < 4; i++)
    {
        for (int j = 0; j < 4; j++) out[i * 4 + j] = (float)a[i][j + 4];
    }
    return 1;
}

/* The point of the plane at height z under the point of the picture (ndcX, ndcY), given the inverse of view * projection. 0: no such point in front of the camera. */
static inline int NpAmortOnPlane(const float* inverse, float ndcX, float ndcY, float height, float point[3])
{
    float a[4], b[4];
    NpAmortXform(a, inverse, ndcX, ndcY, -1.0f);
    NpAmortXform(b, inverse, ndcX, ndcY, 0.0f);
    if (a[3] == 0.0f || b[3] == 0.0f)
    {
        return 0;
    }
    float ax = a[0] / a[3], ay = a[1] / a[3], az = a[2] / a[3];
    float bx = b[0] / b[3], by = b[1] / b[3], bz = b[2] / b[3];
    float dz = bz - az;
    if (dz > -1e-5f && dz < 1e-5f)
    {
        return 0;
    }
    float t = (height - az) / dz;
    if (t <= 0.0f)
    {
        return 0;
    }
    point[0] = ax + (bx - ax) * t;
    point[1] = ay + (by - ay) * t;
    point[2] = height;
    return 1;
}

/* Where a world point lands in the picture, -1..1. 0: behind the camera. */
static inline int NpAmortProject(const float* viewProjection, const float point[3], float ndc[2])
{
    float c[4];
    NpAmortXform(c, viewProjection, point[0], point[1], point[2]);
    if (c[3] <= 1e-5f)
    {
        return 0;
    }
    ndc[0] = c[0] / c[3];
    ndc[1] = c[1] / c[3];
    return 1;
}

/* How far a held picture of the plane at height z is from the truth at the centre of the screen: the picture's shift, in units of -1..1. 0: it cannot be said (a cut). */
static inline int NpAmortShift(const float* inverseNow, const float* viewProjectionThen, float height, float shift[2])
{
    float p[3], then[2];
    if (!NpAmortOnPlane(inverseNow, 0.0f, 0.0f, height, p) || !NpAmortProject(viewProjectionThen, p, then))
    {
        return 0;
    }
    shift[0] = then[0];
    shift[1] = then[1];
    return 1;
}

/* Where the four corners of today's picture lie in the held one: (-1,-1), (-1,1), (1,1), (1,-1) of the screen, as texture coordinates (v down). The plane of the haze is taken as flat at
 * height z, which is exact there and close for what is near it. 0: a corner has no such point. */
static inline int NpHazeCorners(const float* inverseNow, const float* viewProjectionThen, float height, float uv[4][2])
{
    static const float kCorners[4][2] = {{-1.0f, 1.0f}, {-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}};   /* the order of glPoly2::FullCoverage: top left, bottom left, bottom right, top right */
    for (int i = 0; i < 4; i++)
    {
        float p[3], then[2];
        if (!NpAmortOnPlane(inverseNow, kCorners[i][0], kCorners[i][1], height, p) || !NpAmortProject(viewProjectionThen, p, then))
        {
            return 0;
        }
        uv[i][0] = 0.5f + 0.5f * then[0];
        uv[i][1] = 0.5f - 0.5f * then[1];
    }
    return 1;
}

#endif /* NP_AMORTIZE_H */
