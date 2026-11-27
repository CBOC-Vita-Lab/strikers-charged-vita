// Host test of np/fxmodelov.h (MSC_FX_MODEL_OFFVIEW): a drop the test calls off the view has no vertex inside the clip volume, whatever the view, the model's shape, the particle's matrix
// mCoord * scale(size) * rotationZ and its position; and some drops are called off (the test is not vacuous). Also the stream state of GLVertexAnim::PatchSharedStreams: leaving out the GetModel
// of drops (culled or fast path) and patching once for the last of them ends in the state the full sequence ends in.
// MODELOV_BREAK=1 builds with the pad and margin cut to a quarter (a live mutant: the test must fail); MODELOV_BREAK=2 uses the radius of one axis only (also must fail).
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <vector>
#ifdef MODELOV_MUT1
#define NP_MODELOV_PAD 0.25f
#define NP_MODELOV_MARGIN 0.0f
#endif
#include "np/fxmodelov.h"

static int fails;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static unsigned rs = 12345u;
static float Rnd() { rs = rs * 1664525u + 1013904223u; return (float)(rs >> 8) / 16777216.0f; }
static float Rr(float a, float b) { return a + (b - a) * Rnd(); }

// row-major 4x4 stored the way the game stores them: the view for row vectors (translation in e[12..14]), the projection for column vectors (e[11] = z -> w)
static void MakeView(float* V, float yaw, float pitch, float tx, float ty, float tz)
{
    const float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);
    // rows are the images of the x, y, z axes (row vector convention)
    const float R[3][3] = {{cy, sy * sp, -sy * cp}, {0, cp, sp}, {sy, -cy * sp, cy * cp}};
    for (int i = 0; i < 3; ++i) { for (int j = 0; j < 3; ++j) V[i * 4 + j] = R[i][j]; V[i * 4 + 3] = 0; }
    V[12] = tx; V[13] = ty; V[14] = tz; V[15] = 1;
}
static void MakeProj(float* P, float fx, float fy, float ox, float oy, float n, float f)
{
    memset(P, 0, 16 * sizeof(float));
    P[0] = fx; P[5] = fy; P[8] = ox; P[9] = oy;   // column-vector form stored by columns as the game's e[] (e[8], e[9]: x, y offsets by z)
    P[10] = -(f + n) / (f - n); P[14] = -2 * f * n / (f - n); P[11] = -1;
}

// clip of a world point by the same rows the test uses, independently written (view then projection, as separate steps)
static bool InsideVolume(const float* P, const float* V, const float p[3])
{
    float v[3];
    for (int j = 0; j < 3; ++j) v[j] = p[0] * V[0 * 4 + j] + p[1] * V[1 * 4 + j] + p[2] * V[2 * 4 + j] + V[12 + j];
    const float cx = P[0] * v[0] + P[4] * v[1] + P[8] * v[2] + P[12];
    const float cy = P[1] * v[0] + P[5] * v[1] + P[9] * v[2] + P[13];
    const float cw = P[3] * v[0] + P[7] * v[1] + P[11] * v[2] + P[15];
    return cw > 0 && fabsf(cx) <= cw && fabsf(cy) <= cw;
}

int main()
{
    int off = 0, kept = 0, trials = 0;
    for (int t = 0; t < 4000; ++t) {
        float V[16], P[16];
        MakeView(V, Rr(-3.0f, 3.0f), Rr(-0.6f, 0.6f), Rr(-30, 30), Rr(-20, 20), Rr(-80, -3));
        MakeProj(P, Rr(0.8f, 3.0f), Rr(1.0f, 4.0f), Rr(-0.3f, 0.3f), Rr(-0.3f, 0.3f), 1.0f, 500.0f);
        // the model: a 4-vertex quad of half extent 0.75 in x and z, around (cx, 0, cz), as the rain model; its own radius over "frames"
        float model[4][3];
        float radius = 0;
        const float hx = Rr(0.3f, 1.2f), hz = Rr(0.3f, 1.2f), ox = Rr(-0.5f, 0.5f), oz = Rr(-0.5f, 0.5f), oy = Rr(-0.2f, 0.2f);
        const float qx[4] = {-hx, hx, hx, -hx}, qz[4] = {-hz, -hz, hz, hz};
        for (int i = 0; i < 4; ++i) { model[i][0] = ox + qx[i]; model[i][1] = oy; model[i][2] = oz + qz[i]; const float d = sqrtf(model[i][0] * model[i][0] + model[i][1] * model[i][1] + model[i][2] * model[i][2]); if (d > radius) radius = d; }
        if (std::getenv("MODELOV_BREAK") && atoi(std::getenv("MODELOV_BREAK")) == 2) radius = 0.3f * hx;   // the mutant with a far too small model radius
        // mCoord: a rotation with a (possibly mirrored) axis and a non uniform scale, as the game's coordinate system; size and rotation Z of the particle
        float mc[3][3];
        const float a = Rr(0, 6.28f), b = Rr(0, 6.28f), sx = Rr(0.5f, 2.0f), sz = Rr(0.5f, 2.0f);
        const float ca = cosf(a), sa = sinf(a), cb = cosf(b), sb = sinf(b);
        const float Rz[3][3] = {{ca, sa, 0}, {-sa, ca, 0}, {0, 0, 1}}, Rx[3][3] = {{1, 0, 0}, {0, cb, sb}, {0, -sb, cb}};
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) { mc[i][j] = 0; for (int k = 0; k < 3; ++k) mc[i][j] += Rz[i][k] * Rx[k][j]; }
        for (int j = 0; j < 3; ++j) { mc[0][j] *= sx; mc[2][j] *= sz * (t & 1 ? -1.0f : 1.0f); }
        float cf = 0; for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) cf += mc[i][j] * mc[i][j];
        NpModelOv o; memset(&o, 0, sizeof o);
        CHECK(NpModelOvSetup(&o, P, V, sqrtf(cf), radius));
        for (int d = 0; d < 20; ++d) {
            const float size = Rr(0.1f, 3.0f) * (d == 5 ? -1.0f : 1.0f), rot = Rr(0, 6.28f);
            float pos[3] = {Rr(-60, 60), Rr(-5, 40), Rr(-120, 40)};
            if (d & 1) {   // half of them near the edge of the view: a view-space point at depth z with its NDC x, y a little beyond +-1, back to the world (the view's rotation is orthonormal)
                const float zv = -Rr(2.0f, 100.0f), nx = Rr(-1.5f, 1.5f), ny = Rr(-1.5f, 1.5f);
                const float vx = (nx * (-zv) - P[8] * zv) / P[0], vy = (ny * (-zv) - P[9] * zv) / P[5];
                const float vv[3] = {vx - V[12], vy - V[13], zv - V[14]};
                for (int j = 0; j < 3; ++j) pos[j] = vv[0] * V[0 * 4 + j] + vv[1] * V[1 * 4 + j] + vv[2] * V[2 * 4 + j];   // (the transpose of the rotation: rows of V are its images)
            }
            const float cr = cosf(rot), sr = sinf(rot);
            // m3 = mCoord * (scale * rotationZ)
            float m3[3][3], sr3[3][3] = {{size * cr, size * sr, 0}, {-size * sr, size * cr, 0}, {0, 0, size}};
            for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) { m3[i][j] = 0; for (int k = 0; k < 3; ++k) m3[i][j] += mc[i][k] * sr3[k][j]; }
            bool any = false;
            for (int gi = 0; gi < 9 && !any; ++gi) for (int gj = 0; gj < 9 && !any; ++gj) {   // a 9 x 9 grid of points over the quad (the drawn hull lies inside it)
                const float fu = gi / 8.0f, fv = gj / 8.0f;
                float mp[3];
                for (int j = 0; j < 3; ++j) mp[j] = (1 - fu) * (1 - fv) * model[0][j] + fu * (1 - fv) * model[1][j] + fu * fv * model[2][j] + (1 - fu) * fv * model[3][j];
                float w[3];
                for (int j = 0; j < 3; ++j) w[j] = mp[0] * m3[0][j] + mp[1] * m3[1][j] + mp[2] * m3[2][j] + pos[j];
                if (InsideVolume(P, V, w)) any = true;
            }
            // (also the middle of each edge and the centre: the drawn hull is inside the quad, the quad's convex hull is inside the sphere)
            ++trials;
            const bool isOff = NpModelOvOff(o, pos[0], pos[1], pos[2], size);
            if (isOff) { ++off; CHECK(!any); } else ++kept;
        }
    }
    CHECK(off > trials / 20 && kept > trials / 20);   // both outcomes happen
    std::printf("modelov: %d drops, %d off the view, %d kept\n", trials, off, kept);

    // PatchSharedStreams: the source streams are written by every GetModel(frame); the last write wins
    {
        const int N = 64;
        int bad = 0;
        for (int t = 0; t < 2000; ++t) {
            std::vector<int> frames, kind;   // kind 0: a GetModel that is made, 1: one that is left out (culled or fast path)
            const int n = 1 + (int)(Rnd() * 30);
            for (int i = 0; i < n; ++i) { frames.push_back((int)(Rnd() * N)); kind.push_back(Rnd() < 0.7f ? 1 : 0); }
            int full = -1, mine = -1, pend = -1;
            for (int i = 0; i < n; ++i) {
                full = frames[i];
                if (kind[i] == 0) { mine = frames[i]; pend = -1; } else pend = frames[i];
            }
            if (pend >= 0) mine = pend;   // the call after the loop
            if (std::getenv("MODELOV_BREAK") && atoi(std::getenv("MODELOV_BREAK")) == 3 && pend >= 0) mine = -2;   // mutant: the final patch forgotten
            if (mine != full) ++bad;
        }
        CHECK(bad == 0);
    }
    std::printf(fails ? "FAILED\n" : "OK\n");
    return fails ? 1 : 0;
}
