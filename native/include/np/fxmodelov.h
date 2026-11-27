// MSC_FX_MODEL_OFFVIEW (nxrain25): the test that leaves a model particle (The Dump's rain) out of the picture when its bounding sphere lies wholly beyond one side plane of the view's clip volume.
// The same homogeneous test as np/fx.h NpFxOffView (clip x or y beyond +-w, with a margin of 2 % of |w|), applied to a sphere. Render only. A header so that a host test can run it.
//   world position p (x, y, z, 1) -> clip x, y, w = clip[0..2][0..2] . p + clip[0..2][3]  (view stored for row vectors, projection for column vectors: clip_r = sum_i p_i * (sum_c P[r][c] * V[i][c]))
//   sphere: centre p, radius r0 * |size| * 1.25 + 0.05, where r0 = (the model's largest vertex distance from its origin) * (Frobenius norm of the 3 x 3 part of the particle's coordinate matrix):
//   the particle's matrix is mCoord * scale(size) * rotationZ, whose 3 x 3 part has 2-norm <= |size| * norm(mCoord) (a rotation has 2-norm 1; the 1.25 covers the sin/cos table).
#ifndef NP_FXMODELOV_H
#define NP_FXMODELOV_H
#include <math.h>

struct NpModelOv {
    float clip[3][4];
    float gn[4];   // the length of the gradient (over world x, y, z) of w - x, w + x, w - y, w + y
    float r0;
};

// P, V: the projection and view matrices as the game stores them (16 floats, e[]); coordFrob: the Frobenius norm of mCoord's 3 x 3 part; radius: the model's own. false: not usable (no culling).
static inline bool NpModelOvSetup(NpModelOv* o, const float* P, const float* V, float coordFrob, float radius)
{
    if (!(radius > 0.0f && radius < 1e6f && coordFrob > 0.0f && coordFrob < 1e6f)) return false;
    for (int r = 0; r < 3; ++r) {
        const int pr = r == 2 ? 3 : r;   // clip x, clip y, clip w
        for (int i = 0; i < 4; ++i)
            o->clip[r][i] = P[pr * 4 + 0] * V[i * 4 + 0] + P[pr * 4 + 1] * V[i * 4 + 1] + P[pr * 4 + 2] * V[i * 4 + 2] + P[pr * 4 + 3] * V[i * 4 + 3];
    }
    for (int k = 0; k < 4; ++k) {
        float g2 = 0.0f;
        for (int i = 0; i < 3; ++i) {
            const float g = k < 2 ? (k == 0 ? o->clip[2][i] - o->clip[0][i] : o->clip[2][i] + o->clip[0][i]) : (k == 2 ? o->clip[2][i] - o->clip[1][i] : o->clip[2][i] + o->clip[1][i]);
            g2 += g * g;
        }
        o->gn[k] = sqrtf(g2);
        if (!(o->gn[k] < 1e6f)) return false;
    }
    o->r0 = radius * coordFrob;
    return o->r0 < 1e6f;
}

#ifndef NP_MODELOV_PAD
#define NP_MODELOV_PAD 1.25f   /* the test's mutant lowers it */
#endif
#ifndef NP_MODELOV_MARGIN
#define NP_MODELOV_MARGIN 1.0f
#endif

// true: nothing of the drop can be drawn
static inline bool NpModelOvOff(const NpModelOv& o, float px, float py, float pz, float size)
{
    const float cx = o.clip[0][0] * px + o.clip[0][1] * py + o.clip[0][2] * pz + o.clip[0][3];
    const float cy = o.clip[1][0] * px + o.clip[1][1] * py + o.clip[1][2] * pz + o.clip[1][3];
    const float cw = o.clip[2][0] * px + o.clip[2][1] * py + o.clip[2][2] * pz + o.clip[2][3];
    const float r = o.r0 * fabsf(size) * NP_MODELOV_PAD + 0.05f * NP_MODELOV_MARGIN;
    const float m = NP_MODELOV_MARGIN * (0.02f * fabsf(cw) + 0.001f * (fabsf(cx) + fabsf(cy)) + 1e-4f);
    return (cw - cx < -(r * o.gn[0]) - m) || (cw + cx < -(r * o.gn[1]) - m) || (cw - cy < -(r * o.gn[2]) - m) || (cw + cy < -(r * o.gn[3]) - m);
}
#endif
