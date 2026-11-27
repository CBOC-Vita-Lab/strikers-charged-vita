// 1.3 MSC_BG30 (np/bg30.h): the picture decision on synthetic camera streams. Run by run_bg30_test.sh with MSC_BG30=0, 1 and 2, and with the mutants
// NP_BG30_BREAK=1..4 (np_bg30.cpp), which must fail.
#include "../../platform/compat/np_bg30.cpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf("FAIL %s:%d ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

// a view matrix (row vectors, nlMatrix4 order) for a camera at p looking at t with z up
static void LookAt(float* v, const float* p, const float* t) {
    float f[3] = {t[0] - p[0], t[1] - p[1], t[2] - p[2]};
    float l = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (float& x : f) x /= l;
    const float up[3] = {0, 0, 1};
    float r[3] = {f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0]};
    l = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    for (float& x : r) x /= l;
    const float u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
    // eye axes: x = r, y = u, z = -f; eye = X R + T with R columns = axes
    for (int j = 0; j < 3; ++j) { v[j * 4 + 0] = r[j]; v[j * 4 + 1] = u[j]; v[j * 4 + 2] = -f[j]; v[j * 4 + 3] = 0; }
    for (int c = 0; c < 3; ++c) v[12 + c] = -(p[0] * v[0 * 4 + c] + p[1] * v[1 * 4 + c] + p[2] * v[2 * 4 + c]);
    v[15] = 1;
}
static NpBg30In In(float px, float py, int eligible, float fov = 27.5f, float lookZ = 0.0f) {
    NpBg30In in;
    in.eligible = eligible;
    in.why = eligible ? 0 : NPBG30_WHY_STATE;
    const float p[3] = {px, py - 29.0f, 20.0f}, t[3] = {px, py, lookZ};
    LookAt(in.view, p, t);
    in.fovDeg = fov;
    in.rate60 = 1;
    return in;
}

int main() {
    setenv("MSC_BG30_BALANCE", "3", 1);   // (read once, at the first slot question)
    const int mode = NpBg30Mode();
    // pure parts
    {
        NpBg30In a = In(0, 0, 1), b = In(0.5f, 0, 1), c = In(1.5f, 0, 1);
        CHECK(NpBg30CameraJump(a.view, a.view, 27.5f, 27.5f) == 0, "same view is a cut");
        CHECK(NpBg30CameraJump(a.view, b.view, 27.5f, 27.5f) == 0, "0.5 m pan is a cut");
        CHECK(NpBg30CameraJump(a.view, c.view, 27.5f, 27.5f) == 1, "1.5 m jump is not a cut");
        CHECK(NpBg30CameraJump(a.view, a.view, 27.5f, 29.0f) == 1, "1.5 degrees of zoom in one picture is not a cut");
        NpBg30In d = In(0, 0, 1, 27.5f, 1.0f);   // looks 1 m higher at the same place: about 1.5 degrees
        CHECK(NpBg30CameraJump(a.view, d.view, 27.5f, 27.5f) == 1, "a 1.5 degree turn is not a cut");
        CHECK(NpBg30SeesPlane(a.view, 27.5f, 1.8f) == 1, "the match camera does not see the pitch");
        NpBg30In h = In(0, 0, 1, 27.5f, 19.0f);  // nearly level: the top of the screen is above the horizon
        CHECK(NpBg30SeesPlane(h.view, 27.5f, 1.8f) == 0, "a level camera sees the plane in its top corners");
    }
    // the decision over a stream: 8 eligible pictures panning slowly, a cut, 4 more, 2 not eligible, 4 more
    const char* expect2 = "12121212" "1212" "00" "1212";
    const char* expect1 = "33333333" "3333" "00" "3333";
    const char* expect0 = "00000000" "0000" "00" "0000";
    const char* expect = mode == 2 ? expect2 : (mode == 1 ? expect1 : expect0);
    char got[32] = {};
    int k = 0;
    float x = 0;
    for (int i = 0; i < 8; ++i, x += 0.1f) { NpBg30In in = In(x, 0, 1); got[k++] = static_cast<char>('0' + NpBg30Decide(&in)); }
    x += 5.0f;   // a cut
    for (int i = 0; i < 4; ++i, x += 0.1f) { NpBg30In in = In(x, 0, 1); got[k++] = static_cast<char>('0' + NpBg30Decide(&in)); }
    for (int i = 0; i < 2; ++i, x += 0.1f) { NpBg30In in = In(x, 0, 0); got[k++] = static_cast<char>('0' + NpBg30Decide(&in)); }
    for (int i = 0; i < 4; ++i, x += 0.1f) { NpBg30In in = In(x, 0, 1); got[k++] = static_cast<char>('0' + NpBg30Decide(&in)); }
    CHECK(std::string(got) == expect, "mode %d: kinds %s, expected %s", mode, got, expect);
    // a cut right after a full picture gives a full picture (never a light one across a cut)
    if (mode == 2) {
        NpBg30Reset();
        NpBg30In a = In(0, 0, 1), b = In(9, 0, 1), c = In(9.1f, 0, 1);
        const int k1 = NpBg30Decide(&a), k2 = NpBg30Decide(&b), k3 = NpBg30Decide(&c);
        CHECK(k1 == 1 && k2 == 1 && k3 == 2, "full, cut, then: %d %d %d (expected 1 1 2)", k1, k2, k3);
        // a level camera (sees the sky) is never re-projected
        NpBg30In h = In(0, 0, 1, 27.5f, 19.0f);
        CHECK(NpBg30Decide(&h) == 0, "a camera above the horizon was not ordinary");
        // held at 30: ordinary pictures, and the controller is told what a pair would save
        NpBg30In q = In(0, 0, 1);
        q.rate60 = 0;
        CHECK(NpBg30Decide(&q) == 0 && NpBg30HeldEligible() == 1, "held at 30: kind %d, held-eligible %d", NpBg30Kind(), NpBg30HeldEligible());
        CHECK(NpBg30PairSavingMs() > 0.0, "no pair saving estimate");
        // the learned saving: full pictures 12 ms, light 8 ms -> 2 ms a picture over a pair
        NpBg30Reset();
        for (int i = 0; i < 80; ++i) { NpBg30In r = In(i * 0.01f, 0, 1); NpBg30Decide(&r); NpBg30NoteCost(NpBg30Kind() == 2 ? 8.0 : 12.0); }
        CHECK(std::fabs(NpBg30PairSavingMs() - 2.0) < 0.01, "learned pair saving %.3f, expected 2.0", NpBg30PairSavingMs());
    }
    if (mode == 2) {
        // the pan measure: 0 for the same view, about 3 px for 0.1 m of pan (960x544), growing with the pan, negative when the plane is not seen
        NpBg30In a = In(0, 0, 1), b = In(0.1f, 0, 1), c = In(0.5f, 0, 1), h = In(0, 0, 1, 27.5f, 19.0f);
        const float p0 = NpBg30PanPx(a.view, a.view, 27.5f, 27.5f, 960, 544), p1 = NpBg30PanPx(a.view, b.view, 27.5f, 27.5f, 960, 544), p5 = NpBg30PanPx(a.view, c.view, 27.5f, 27.5f, 960, 544);
        CHECK(p0 < 0.01f, "no pan measured %.3f", p0);
        CHECK(p1 > 1.5f && p1 < 6.0f, "0.1 m of pan measured %.2f px (expected about 3)", p1);
        CHECK(p5 > 4.0f * p1 && p5 < 6.0f * p1, "0.5 m of pan measured %.2f px (5 x %.2f expected)", p5, p1);
        CHECK(NpBg30PanPx(a.view, h.view, 27.5f, 27.5f, 960, 544) < 0.0f, "a level camera has a pan measure");
        // a slow pan keeps the pair, a fast one (0.5 m a picture: ~15 px, over the limit of 12) makes every picture full; a pan that slows down pairs again
        NpBg30Reset();
        char g2[24] = {};
        float x2 = 0;
        int k2 = 0;
        for (int i = 0; i < 6; ++i, x2 += 0.5f) { NpBg30In in = In(x2, 0, 1); g2[k2++] = static_cast<char>('0' + NpBg30Decide(&in)); }
        for (int i = 0; i < 4; ++i, x2 += 0.1f) { NpBg30In in = In(x2, 0, 1); g2[k2++] = static_cast<char>('0' + NpBg30Decide(&in)); }
        CHECK(std::string(g2) == "1111111212", "fast then slow pan: kinds %s, expected 1111111212", g2);
        // the balanced pair: the crowd sprite update and the stadium update: normal, then light does the pair's, the next full skips; a cut clears what was prepared
        NpBg30Reset();
        int cs[8], ss[8], kk[8];
        for (int i = 0; i < 4; ++i) { NpBg30In in = In(i * 0.05f, 0, 1); kk[i] = NpBg30Decide(&in); cs[i] = NpBg30CrowdSlot(); ss[i] = NpBg30StadiumSlot(); }
        CHECK(kk[0] == 1 && kk[1] == 2 && kk[2] == 1 && kk[3] == 2, "pair kinds %d %d %d %d", kk[0], kk[1], kk[2], kk[3]);
        CHECK(cs[0] == NPBG30_SLOT_NORMAL && cs[1] == NPBG30_SLOT_DO_PAIR && cs[2] == NPBG30_SLOT_SKIP && cs[3] == NPBG30_SLOT_DO_PAIR, "crowd slots %d %d %d %d", cs[0], cs[1], cs[2], cs[3]);
        CHECK(ss[0] == NPBG30_SLOT_NORMAL && ss[1] == NPBG30_SLOT_DO_PAIR && ss[2] == NPBG30_SLOT_SKIP && ss[3] == NPBG30_SLOT_DO_PAIR, "stadium slots %d %d %d %d", ss[0], ss[1], ss[2], ss[3]);
        NpBg30In cutp = In(40.0f, 0, 1);   // a cut after a light picture: the full picture that follows has nothing prepared and updates itself
        const int kc = NpBg30Decide(&cutp);
        CHECK(kc == 1 && NpBg30CrowdSlot() == NPBG30_SLOT_NORMAL && NpBg30StadiumSlot() == NPBG30_SLOT_NORMAL, "after a cut: kind %d crowd slot not normal", kc);
        NpBg30In notl = In(0, 0, 0);   // leaving live play clears too
        NpBg30Decide(&notl);
        CHECK(NpBg30CrowdSlot() == NPBG30_SLOT_NORMAL, "ordinary picture got a crowd slot");
    }
    std::printf("bg30 unit (MSC_BG30=%d): %s\n", mode, fails ? "FAILED" : "passed");
    return fails ? 1 : 0;
}
