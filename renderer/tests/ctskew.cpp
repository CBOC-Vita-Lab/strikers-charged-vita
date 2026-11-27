// gpueff18: host test of vgx_ctskew.h (crowd trim of welded lists' passes and of non-parallelogram quads). Mutants VGX_CTSKEW_BREAK=1..4 must fail.
#include "../src/vgx_ctskew.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

using namespace vgx::ctskew;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static uint32_t g_rng = 12345;
static float Rnd() { g_rng = g_rng * 1664525u + 1013904223u; return static_cast<float>((g_rng >> 8) & 0xFFFF) / 65535.0f; }

// A welded list as vgx_dlcache.cpp makes it: groups of segments, each group's distinct vertices one block, the records keep vtxFirst = the group's
// first vertex and vtxCount = the segment's unwelded count. Every index of every segment must lie inside [vtxFirst, vtxFirst + WeldCount).
static void TestWeld() {
    for (int trial = 0; trial < 200; ++trial) {
        const size_t stride = 44;
        std::vector<uint8_t> buf;
        struct Seg { uint32_t first, count; std::vector<uint32_t> idx; };
        std::vector<Seg> segs;
        const int groups = 1 + static_cast<int>(Rnd() * 3);
        for (int g = 0; g < groups; ++g) {
            const uint32_t base = static_cast<uint32_t>(buf.size() / stride);
            std::map<int, uint32_t> slot;   // distinct vertex key -> slot in the group
            const int nseg = 1 + static_cast<int>(Rnd() * 3);
            for (int s = 0; s < nseg; ++s) {
                Seg sg;
                sg.first = base;
                sg.count = 3 + static_cast<uint32_t>(Rnd() * 300);
                for (uint32_t i = 0; i < sg.count; ++i) {
                    const int key = static_cast<int>(Rnd() * (sg.count / 2 + 1));   // about half the vertices repeat
                    auto it = slot.find(key);
                    if (it == slot.end()) {
                        it = slot.emplace(key, static_cast<uint32_t>(slot.size())).first;
                        buf.resize(buf.size() + stride, static_cast<uint8_t>(key));
                    }
                    sg.idx.push_back(base + it->second);
                }
                segs.push_back(sg);
            }
        }
        std::vector<uint16_t> ib;   // the list's index copy, segment after segment
        std::vector<uint32_t> at;
        for (const Seg& sg : segs) { at.push_back(static_cast<uint32_t>(ib.size())); for (uint32_t i : sg.idx) ib.push_back(static_cast<uint16_t>(i)); }
        for (size_t k = 0; k < segs.size(); ++k) {
            const Seg& sg = segs[k];
            uint32_t first = 0, n = 0;
            CHECK(IndexRange(ib.data(), ib.size(), at[k], static_cast<uint32_t>(sg.idx.size()), first, n));
            uint32_t mn = 0xFFFFFFFFu, mx = 0;
            for (uint32_t i : sg.idx) { CHECK(i >= first && i < first + n); mn = std::min(mn, i); mx = std::max(mx, i); }
            CHECK(first == mn && first + n == mx + 1);                       // no wider than the draw needs
            CHECK((static_cast<size_t>(first) + n) * stride <= buf.size());  // inside the welded copy
            const uint32_t e = ToEnd(0, sg.first, stride, buf.size());      // the fallback without an index copy
            for (uint32_t i : sg.idx) CHECK(i >= sg.first && i < sg.first + e);
        }
    }
    uint32_t f = 0, n = 0;
    const uint16_t ix[6] = {4, 5, 6, 4, 6, 9};
    CHECK(IndexRange(ix, 6, 0, 6, f, n) && f == 4 && n == 6);
    CHECK(!IndexRange(ix, 6, 2, 5, f, n));   // past the copy
    CHECK(!IndexRange(ix, 6, 0, 0, f, n));
}
static void Lerp3(const float* a, const float* b, float t, float* o) { for (int j = 0; j < 3; ++j) o[j] = a[j] + t * (b[j] - a[j]); }
static float Dist(const float* a, const float* b) { float m = 0; for (int j = 0; j < 3; ++j) m = std::fmax(m, std::fabs(a[j] - b[j])); return m; }

static void TestSkew() {
    // the index patterns the crowd uses: vertex 0 at (1,0), 1 at (0,0), 2 at (0,1), 3 at (1,1) (seen on the host), and other rotations
    const int pats[4][2][4] = {{{1, 0, 0, 1}, {0, 0, 1, 1}}, {{0, 1, 1, 0}, {0, 0, 1, 1}}, {{0, 0, 1, 1}, {0, 1, 1, 0}}, {{1, 1, 0, 0}, {1, 0, 0, 1}}};
    for (int trial = 0; trial < 400; ++trial) {
        const int* xi = pats[trial & 3][0];
        const int* yi = pats[trial & 3][1];
        float c00[3], ex[3], ey[3];
        for (int j = 0; j < 3; ++j) { c00[j] = -50 + 100 * Rnd(); ex[j] = -2 + 4 * Rnd(); ey[j] = -2 + 4 * Rnd(); }
        const bool bent = trial % 2 == 1;
        float pos[4][3];
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 3; ++j) pos[i][j] = c00[j] + xi[i] * ex[j] + yi[i] * ey[j] + (bent && xi[i] && yi[i] ? 0.02f * (Rnd() - 0.5f) : 0.0f);
        int at[2][2];
        for (int i = 0; i < 4; ++i) at[xi[i]][yi[i]] = i;
        const float side = std::fmax(std::fmax(std::fabs(ex[0]), std::fabs(ex[1])), std::fmax(std::fmax(std::fabs(ex[2]), std::fabs(ey[0])), std::fmax(std::fabs(ey[1]), std::fabs(ey[2]))));
        CHECK(SkewOk(pos[at[0][0]], pos[at[1][0]], pos[at[0][1]], pos[at[1][1]], 0.03f) == (side > 0.0f && (!bent || 0.01f <= 0.03f * side) ? true : SkewOk(pos[at[0][0]], pos[at[1][0]], pos[at[0][1]], pos[at[1][1]], 0.03f)));
        // the four corners come back exactly where they were
        for (int i = 0; i < 4; ++i) {
            float o[3];
            SkewPos(pos, xi, yi, static_cast<float>(xi[i]), static_cast<float>(yi[i]), o);
            CHECK(Dist(o, pos[i]) <= 1e-4f);
        }
        // any point lies on the original triangle that holds it: on the segment between two edge points of that triangle (same fraction line)
        for (int k = 0; k < 20; ++k) {
            const float fx = Rnd(), fy = Rnd();
            float o[3];
            SkewPos(pos, xi, yi, fx, fy, o);
            // the triangle of the index pattern: 0 1 2 or 0 2 3; their shared edge is vertex 0 - vertex 2
            const float dx = static_cast<float>(xi[2] - xi[0]), dy = static_cast<float>(yi[2] - yi[0]);
            const float side1 = dx * (static_cast<float>(yi[1]) - yi[0]) - dy * (static_cast<float>(xi[1]) - xi[0]);
            const float sp = dx * (fy - yi[0]) - dy * (fx - xi[0]);
            const int a = 0, b = (sp * side1 >= 0) ? 1 : 3, c = 2;
            // barycentric by hand in the unit square, then compare
            const float ax = xi[a], ay = yi[a], bx = xi[b], by = yi[b], cx = xi[c], cy = yi[c];
            const float d = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy);
            const float w0 = ((by - cy) * (fx - cx) + (cx - bx) * (fy - cy)) / d, w1 = ((cy - ay) * (fx - cx) + (ax - cx) * (fy - cy)) / d, w2 = 1 - w0 - w1;
            float e[3];
            for (int j = 0; j < 3; ++j) e[j] = w0 * pos[a][j] + w1 * pos[b][j] + w2 * pos[c][j];
            CHECK(Dist(o, e) <= 1e-3f);
            if (!bent) {   // a parallelogram: the affine formula of the old trim
                float l0[3], l1[3], f[3];
                Lerp3(pos[at[0][0]], pos[at[1][0]], fx, l0);
                float d01[3];
                for (int j = 0; j < 3; ++j) d01[j] = pos[at[0][1]][j] - pos[at[0][0]][j];
                for (int j = 0; j < 3; ++j) f[j] = l0[j] + fy * d01[j];
                (void)l1;
                CHECK(Dist(o, f) <= 1e-3f);
            }
        }
    }
    // tolerance: a corner bent by 5 % of the side is refused at 3 %, accepted at 6 %
    const float a00[3] = {0, 0, 0}, a10[3] = {2, 0, 0}, a01[3] = {0, 2, 0}, a11[3] = {2, 2, 0.1f};
    CHECK(!SkewOk(a00, a10, a01, a11, 0.03f));
    CHECK(SkewOk(a00, a10, a01, a11, 0.06f));
    const float b11[3] = {2, 2, 0};
    CHECK(SkewOk(a00, a10, a01, b11, 0.0f));
}

int main() {
    TestWeld();
    TestSkew();
    if (g_fail) { std::printf("ctskew: %d failures\n", g_fail); return 1; }
    std::printf("ctskew: ok\n");
    return 0;
}
