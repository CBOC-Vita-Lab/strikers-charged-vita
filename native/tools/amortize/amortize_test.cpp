// Host test of np/amortize.h: the schedules of MSC_FX_HAZE_CACHE, MSC_SHADOW_*, MSC_AMORT_60, and the camera arithmetic that moves a held haze picture.
#include "np/amortize.h"
#include <cmath>
#include <cstdio>
#include <cstring>

static int gFailures;
#define CHECK(c) do { if (!(c)) { std::printf("FAILED line %d: %s\n", __LINE__, #c); gFailures++; } } while (0)

namespace {
// row vectors, as nlMatrix4
void Mul(float out[16], const float a[16], const float b[16])
{
    float r[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
        {
            r[i * 4 + j] = 0;
            for (int k = 0; k < 4; k++) r[i * 4 + j] += a[i * 4 + k] * b[k * 4 + j];
        }
    std::memcpy(out, r, sizeof(r));
}

// a camera at (x, y, z) looking straight down the z axis at the pitch, x right, y up on the screen; the projection is C_MTXPerspective's, transposed: z/w -1 at the near plane, 0 at the far one
void Camera(float vp[16], float x, float y, float z, float f = 2.0f)
{
    const float view[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -x, -y, -z, 1};
    const float n = 0.25f, fr = 4096.0f;
    const float proj[16] = {f, 0, 0, 0, 0, f, 0, 0, 0, 0, -n / (fr - n), -1, 0, 0, -n * fr / (fr - n), 0};
    Mul(vp, view, proj);
}

void ScheduleTests()
{
    NpHazeState s = NpHazeState();
    // 18 Nov: the one MSC_BG30 parser (np/bg30_env.h): every reader agrees, so a value that wakes the layer marks also turns bg30 on
    CHECK(NpBg30ParseMode(nullptr) == 0 && NpBg30ParseMode("") == 0 && NpBg30ParseMode("0") == 0 && NpBg30ParseMode("off") == 0);
    CHECK(NpBg30ParseMode("1") == 1 && NpBg30ParseMode("proof") == 1 && NpBg30ParseMode("on") == 1 && NpBg30ParseMode("yes") == 1);
    CHECK(NpBg30ParseMode("2") == 2 && NpBg30ParseMode("half") == 2);
    CHECK(NpBg30ParseMode("3") == 0 && NpBg30ParseMode("maybe") == 0);   // (=3 used to turn VIEW_LAYERS on while bg30 stayed off)
    CHECK(NpHazeStep(&s, 0, 1, 0) == NPHAZE_LIVE);
    CHECK(NpHazeStep(&s, 3, 0, 0) == NPHAZE_LIVE);
    CHECK(NpHazeStep(&s, 3, 1, 0) == NPHAZE_REFRESH);   // the first frame never shows an old picture
    CHECK(NpHazeStep(&s, 3, 1, 0) == NPHAZE_HOLD);
    CHECK(NpHazeStep(&s, 3, 1, 0) == NPHAZE_HOLD);
    CHECK(NpHazeStep(&s, 3, 1, 0) == NPHAZE_REFRESH);
    CHECK(NpHazeStep(&s, 3, 1, 0) == NPHAZE_HOLD);
    CHECK(NpHazeStep(&s, 3, 1, 1) == NPHAZE_REFRESH);   // a jump: that same frame
    CHECK(NpHazeStep(&s, 3, 0, 0) == NPHAZE_LIVE);      // a scene that may not be held forgets the picture
    CHECK(NpHazeStep(&s, 3, 1, 0) == NPHAZE_REFRESH);
    // over any run, a held picture is never older than every - 1 frames
    NpHazeState t = NpHazeState();
    int age = 0;
    for (int f = 0; f < 200; f++)
    {
        const int a = NpHazeStep(&t, 4, 1, 0);
        age = a == NPHAZE_REFRESH ? 0 : age + 1;
        CHECK(age <= 3);
    }

    CHECK(NpHazeCacheParse(0) == 0 && NpHazeCacheParse(1) == 0 && NpHazeCacheParse(3) == 3 && NpHazeCacheParse(9) == 4);
    CHECK(NpShadowPlayParse(0) == 2 && NpShadowPlayParse(3) == 3 && NpShadowPlayParse(9) == 4);
    // off: exactly the game's numbers
    CHECK(NpShadowVisibleEvery(2, 2, 0) == 2 && NpShadowHiddenEvery(4, 0) == 4);
    CHECK(NpShadowVisibleEvery(2, 3, 0) == 3 && NpShadowVisibleEvery(2, 2, 1) == 4);
    CHECK(NpShadowHiddenEvery(4, 1) == 8);
    CHECK(NpCrowdAt60(2, 0) == 2 && NpCrowdAt60(2, 1) == 4 && NpCrowdAt60(4, 1) == 4 && NpCrowdAt60(1, 1) == 2);
}

void CameraTests()
{
    float then[16], now[16], inverse[16], unit[16];
    Camera(then, 0, 0, 30);
    CHECK(NpAmortInvert(inverse, then));
    Mul(unit, inverse, then);
    for (int i = 0; i < 16; i++) CHECK(std::fabs(unit[i] - (i % 5 == 0 ? 1.0f : 0.0f)) < 1e-3f);
    const float zero[16] = {0};
    CHECK(!NpAmortInvert(inverse, zero));
    NpAmortInvert(inverse, then);

    // the same camera: the corners are the corners, in FullCoverage's order (top left first, v down)
    float uv[4][2];
    CHECK(NpHazeCorners(inverse, then, 10.0f, uv));
    const float want[4][2] = {{0, 0}, {0, 1}, {1, 1}, {1, 0}};
    for (int i = 0; i < 4; i++)
    {
        CHECK(std::fabs(uv[i][0] - want[i][0]) < 1e-3f);
        CHECK(std::fabs(uv[i][1] - want[i][1]) < 1e-3f);
    }

    // a pan to the right: what is at the middle of today's screen is right of the middle in the held picture
    Camera(now, 2, 0, 30);
    NpAmortInvert(inverse, now);
    CHECK(NpHazeCorners(inverse, then, 10.0f, uv));
    const float du = uv[0][0];
    CHECK(du > 0.0f);
    for (int i = 0; i < 4; i++) CHECK(std::fabs((uv[i][0] - want[i][0]) - du) < 1e-3f);   // a pure shift
    // by the truth: 2 units at 20 from the camera, f = 2, in -1..1 is 0.2, in texture coordinates 0.1
    CHECK(std::fabs(du - 0.1f) < 1e-3f);

    // a point of the plane lands on the same texel, whichever camera drew it
    float p[3], ndc[2];
    CHECK(NpAmortOnPlane(inverse, 0.3f, -0.4f, 10.0f, p));
    CHECK(std::fabs(p[2] - 10.0f) < 1e-3f);
    CHECK(NpAmortProject(now, p, ndc));
    CHECK(std::fabs(ndc[0] - 0.3f) < 1e-3f && std::fabs(ndc[1] + 0.4f) < 1e-3f);

    float shift[2];
    CHECK(NpAmortShift(inverse, then, 10.0f, shift));
    CHECK(std::fabs(shift[0] - 0.2f) < 1e-3f && std::fabs(shift[1]) < 1e-3f);

    // a zoom scales about the middle
    Camera(now, 0, 0, 30, 2.2f);
    NpAmortInvert(inverse, now);
    CHECK(NpHazeCorners(inverse, then, 10.0f, uv));
    CHECK(uv[0][0] > 0.0f && uv[2][0] < 1.0f);

    // a plane behind the camera has no point: the caller refreshes
    Camera(now, 0, 0, 5);
    NpAmortInvert(inverse, now);
    CHECK(!NpHazeCorners(inverse, then, 10.0f, uv));
}
} // namespace

static void ShadowFarTests()
{
    // MSC_SHADOW_FAR and the FULL picture policy's shadow step: frames added to the interval of a shadow picture
    CHECK(NpShadowExtraFrames(0, 0, 0) == 0 && NpShadowExtraFrames(0, 1, 0) == 0);          // everything off: the game's intervals
    CHECK(NpShadowExtraFrames(1, 1, 0) == 1 && NpShadowExtraFrames(1, 0, 0) == 0);          // static: far players only
    CHECK(NpShadowExtraFrames(2, 1, 0) == 2 && NpShadowExtraFrames(9, 1, 0) == 3);          // at most 3
    CHECK(NpShadowExtraFrames(0, 1, 1) == 1 && NpShadowExtraFrames(0, 0, 1) == 0);          // step 1: far players one frame, near none
    CHECK(NpShadowExtraFrames(0, 1, 2) == 2 && NpShadowExtraFrames(0, 0, 2) == 1);          // step 2: far two, near one
    CHECK(NpShadowExtraFrames(0, 1, 7) == 2 && NpShadowExtraFrames(0, 0, 7) == 1);          // nothing beyond step 2
    CHECK(NpShadowExtraFrames(1, 1, 2) == 3 && NpShadowExtraFrames(1, 0, 2) == 1);          // both together add
    for (int step = 0; step <= 2; ++step)                                                    // a far player never gets less than a near one
        for (int farOn = 0; farOn <= 3; ++farOn) CHECK(NpShadowExtraFrames(farOn, 1, step) >= NpShadowExtraFrames(farOn, 0, step));
    CHECK(NpShadowIsFar(40.0f, 60.0f) && !NpShadowIsFar(60.0f, 60.0f) && !NpShadowIsFar(1.0e9f, 60.0f));   // behind the camera plane: never far
    // the interval stays a positive number: the game's modulo never sees 0
    for (int g = 1; g <= 4; ++g) CHECK(NpShadowVisibleEvery(g, 2, 0) + NpShadowExtraFrames(3, 1, 2) >= 1);
}

int main()
{
    ShadowFarTests();
    ScheduleTests();
    CameraTests();
    if (gFailures == 0) std::printf("amortize: all passed\n");
    return gFailures != 0;
}
