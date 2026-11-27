// Host tests of np/lod.h (the selection rule) and np/lod_simplify.h (the simplifier). Build and run: native/tools/lod/run_tests.sh
#include "np/lod.h"
#include "np/lod_simplify.h"

#include <cstdio>
#include <cstdlib>
#include <array>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <vector>

static int fails;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

// ---------------------------------------------------------------- selection rule
static void TestSelect()
{
    const float thr[3] = {90.0f, 60.0f, 35.0f};
    const float h = 0.15f;
    // monotone sweep down then up: levels never decrease then never increase, each level change happens once
    int prev = 0, changes = 0, last = 0;
    for (float px = 200.0f; px > 5.0f; px -= 0.05f)
    {
        const int l = NpLodSelect(px, prev, thr, 3, h);
        CHECK(l >= prev);
        if (l != prev) ++changes;
        prev = l;
        last = l;
    }
    CHECK(last == 3 && changes <= 3);
    changes = 0;
    for (float px = 5.0f; px < 200.0f; px += 0.05f)
    {
        const int l = NpLodSelect(px, prev, thr, 3, h);
        CHECK(l <= prev);
        if (l != prev) ++changes;
        prev = l;
    }
    CHECK(prev == 0 && changes <= 3);
    // the hysteresis band: sizes that stay inside t*(1-h)..t*(1+h) never change the level, whichever side the object came from
    for (int start = 0; start <= 1; ++start)
        for (int k = 0; k < 3; ++k)
        {
            int level = start ? k + 1 : k;
            int flips = 0;
            unsigned rng = 12345u + k;
            for (int i = 0; i < 100000; ++i)
            {
                rng = rng * 1664525u + 1013904223u;
                const float j = ((rng >> 8) / 16777216.0f) * 2.0f - 1.0f;           // -1..1
                const float px = thr[k] * (1.0f + 0.95f * h * j);                    // inside the band
                const int l = NpLodSelect(px, level, thr, 3, h);
                if (l != level) ++flips;
                level = l;
            }
            CHECK(flips == 0);
        }
    // a walking figure jittering by +-10 % around every threshold (more than any frame to frame change) changes level at most once while the mean is steady
    for (int k = 0; k < 3; ++k)
    {
        int level = 0, flips = 0;
        for (int i = 0; i < 20000; ++i)
        {
            const float mean = thr[k] * (1.0f - 0.2f) - 1.0f;   // steady at 20 % under the threshold
            const float px = mean * (1.0f + 0.1f * sinf(i * 0.37f));
            const int l = NpLodSelect(px, level, thr, 3, h);
            if (l != level) ++flips;
            level = l;
        }
        CHECK(flips <= 3);
    }
    // jumps over several levels in one step are taken (a cut)
    CHECK(NpLodSelect(10.0f, 0, thr, 3, h) == 3);
    CHECK(NpLodSelect(500.0f, 3, thr, 3, h) == 0);
    // no thresholds: always level 0
    CHECK(NpLodSelect(1.0f, 0, thr, 0, h) == 0);
    // projected size: a 2 unit sphere at depth 20 with the game's 40 degree projection on 544 lines
    const float projY = 1.0f / tanf(20.0f * 3.14159265f / 180.0f);
    const float px = NpLodProjectedPx(1.0f, 20.0f, projY);
    CHECK(fabsf(px - 2.0f * projY / 20.0f * 272.0f) < 1e-3f);
    CHECK(NpLodProjectedPx(1.0f, -1.0f, projY) > 1e6f);   // behind the camera plane: never reduced or culled
    // cull band
    int c = 0, cf = 0;
    for (int i = 0; i < 100000; ++i)
    {
        const float p = 2.0f * (1.0f + 0.1f * sinf(i * 0.9f));   // 1.8 .. 2.2 around the limit 2
        const int n = NpLodCulled(p, c, 2.0f, 0.15f);
        if (n != c) ++cf;
        c = n;
    }
    CHECK(cf == 0);
    CHECK(NpLodCulled(0.5f, 0, 2.0f, 0.15f) == 1 && NpLodCulled(5.0f, 1, 2.0f, 0.15f) == 0 && NpLodCulled(0.1f, 0, 0.0f, 0.15f) == 0);
    // list parsing
    float v[4];
    CHECK(NpLodParseList("90,60,35", v, 4) == 3 && v[0] == 90.0f && v[2] == 35.0f);
    CHECK(NpLodParseList("", v, 4) == 0 && NpLodParseList("x", v, 4) == 0 && NpLodParseList("50", v, 4) == 1);
    // error bound grows as the size falls
    CHECK(NpLodMaxError(4.5f, 90.0f, 1.0f) < NpLodMaxError(4.5f, 40.0f, 1.0f));
}

// ---------------------------------------------------------------- meshes
struct Mesh
{
    std::vector<float> pos, uv, nrm, w;
    std::vector<uint8_t> bones;
    std::vector<uint16_t> idx;
    uint32_t nv() const { return (uint32_t)(pos.size() / 3); }
};

static void AddV(Mesh& m, float x, float y, float z, float u = 0, float v = 0)
{
    m.pos.push_back(x); m.pos.push_back(y); m.pos.push_back(z);
    m.uv.push_back(u); m.uv.push_back(v);
    const float l = sqrtf(x * x + y * y + z * z);
    m.nrm.push_back(x / l); m.nrm.push_back(y / l); m.nrm.push_back(z / l);
}

// a subdivided octahedron projected on the unit sphere: closed, manifold, no seams
static Mesh Sphere(int sub)
{
    Mesh m;
    std::vector<std::array<float, 3>> v = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    std::vector<std::array<int, 3>> f = {{0, 2, 4}, {2, 1, 4}, {1, 3, 4}, {3, 0, 4}, {2, 0, 5}, {1, 2, 5}, {3, 1, 5}, {0, 3, 5}};
    for (int s = 0; s < sub; ++s)
    {
        std::map<std::pair<int, int>, int> mid;
        std::vector<std::array<int, 3>> nf;
        auto get = [&](int a, int b) {
            if (a > b) std::swap(a, b);
            auto it = mid.find({a, b});
            if (it != mid.end()) return it->second;
            std::array<float, 3> p = {v[a][0] + v[b][0], v[a][1] + v[b][1], v[a][2] + v[b][2]};
            const float l = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
            p = {p[0] / l, p[1] / l, p[2] / l};
            v.push_back(p);
            return mid[{a, b}] = (int)v.size() - 1;
        };
        for (auto& t : f)
        {
            const int a = get(t[0], t[1]), b = get(t[1], t[2]), c = get(t[2], t[0]);
            nf.push_back({t[0], a, c}); nf.push_back({t[1], b, a}); nf.push_back({t[2], c, b}); nf.push_back({a, b, c});
        }
        f = nf;
    }
    for (auto& p : v) AddV(m, p[0], p[1], p[2], 0.5f + 0.5f * p[0], 0.5f + 0.5f * p[1]);
    for (auto& t : f) { m.idx.push_back((uint16_t)t[0]); m.idx.push_back((uint16_t)t[1]); m.idx.push_back((uint16_t)t[2]); }
    return m;
}

static nplod::SimpIn In(const Mesh& m, int prim = 0)
{
    nplod::SimpIn in;
    in.nVerts = m.nv(); in.pos = m.pos.data(); in.uv = m.uv.data(); in.nrm = m.nrm.data();
    in.idx = m.idx.data(); in.nIdx = (uint32_t)m.idx.size(); in.prim = prim;
    if (!m.w.empty()) { in.weights = m.w.data(); in.bones = m.bones.data(); }
    return in;
}

struct Topo { bool watertight; int euler; uint32_t verts, edges, faces; };
static Topo Check(const std::vector<uint16_t>& idx)
{
    std::map<std::pair<int, int>, std::vector<int>> e;   // edge -> directions
    std::set<int> used;
    Topo t{true, 0, 0, 0, (uint32_t)idx.size() / 3};
    for (size_t i = 0; i + 2 < idx.size(); i += 3)
        for (int k = 0; k < 3; ++k)
        {
            const int a = idx[i + k], b = idx[i + (k + 1) % 3];
            used.insert(a);
            e[{std::min(a, b), std::max(a, b)}].push_back(a < b ? 1 : -1);
        }
    for (auto& kv : e)
        if (kv.second.size() != 2 || kv.second[0] == kv.second[1]) t.watertight = false;   // two faces, opposite directions (consistent winding)
    t.verts = (uint32_t)used.size(); t.edges = (uint32_t)e.size();
    t.euler = (int)t.verts - (int)t.edges + (int)t.faces;
    return t;
}

static double PointTri(const double* p, const double* a, const double* b, const double* c)
{
    // distance from p to triangle abc (Ericson)
    auto sub = [](const double* x, const double* y, double* o) { for (int i = 0; i < 3; ++i) o[i] = x[i] - y[i]; };
    auto dot = [](const double* x, const double* y) { return x[0] * y[0] + x[1] * y[1] + x[2] * y[2]; };
    double ab[3], ac[3], ap[3];
    sub(b, a, ab); sub(c, a, ac); sub(p, a, ap);
    double d1 = dot(ab, ap), d2 = dot(ac, ap);
    double q[3];
    auto res = [&](const double* r) { double d[3]; sub(p, r, d); return sqrt(dot(d, d)); };
    if (d1 <= 0 && d2 <= 0) return res(a);
    double bp[3]; sub(p, b, bp);
    double d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return res(b);
    double vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) { double v = d1 / (d1 - d3); for (int i = 0; i < 3; ++i) q[i] = a[i] + v * ab[i]; return res(q); }
    double cp[3]; sub(p, c, cp);
    double d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return res(c);
    double vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) { double w = d2 / (d2 - d6); for (int i = 0; i < 3; ++i) q[i] = a[i] + w * ac[i]; return res(q); }
    double va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) { double w = (d4 - d3) / ((d4 - d3) + (d5 - d6)); for (int i = 0; i < 3; ++i) q[i] = b[i] + w * (c[i] - b[i]); return res(q); }
    double den = 1.0 / (va + vb + vc), v = vb * den, w = vc * den;
    for (int i = 0; i < 3; ++i) q[i] = a[i] + ab[i] * v + ac[i] * w;
    return res(q);
}

static double MaxDistToSurface(const Mesh& m, const std::vector<uint16_t>& idx, uint32_t v)
{
    double p[3] = {m.pos[3 * v], m.pos[3 * v + 1], m.pos[3 * v + 2]}, best = 1e30;
    for (size_t i = 0; i + 2 < idx.size(); i += 3)
    {
        double a[3], b[3], c[3];
        for (int k = 0; k < 3; ++k) { a[k] = m.pos[3 * idx[i] + k]; b[k] = m.pos[3 * idx[i + 1] + k]; c[k] = m.pos[3 * idx[i + 2] + k]; }
        best = std::min(best, PointTri(p, a, b, c));
    }
    return best;
}

static void TestSphere()
{
    Mesh m = Sphere(4);   // 8 * 4^4 = 2048 triangles
    CHECK(m.idx.size() / 3 == 2048);
    const Topo t0 = Check(m.idx);
    CHECK(t0.watertight && t0.euler == 2);
    nplod::SimpParams p;
    p.maxError = 0.02f;
    p.uvMax = 10.0f; p.normalCos = -1.0f;
    nplod::SimpStats st;
    std::vector<uint16_t> out;
    CHECK(nplod::Simplify(In(m), p, out, &st));
    std::printf("sphere 2048 tris, maxError 0.02: %u tris, %u collapses, maxErrSeen %.4f\n", st.outTris, st.collapses, st.maxErrorSeen);
    CHECK(st.outTris < 2048 * 0.8 && st.outTris >= 8);
    const Topo t1 = Check(out);
    CHECK(t1.watertight);                    // every edge in two faces with opposite directions: closed and consistently wound
    CHECK(t1.euler == 2);                    // still a sphere
    CHECK(st.maxErrorSeen <= p.maxError + 1e-6f);
    // no flipped triangles: every face normal points away from the origin on a convex surface
    for (size_t i = 0; i + 2 < out.size(); i += 3)
    {
        const float* a = &m.pos[3 * out[i]]; const float* b = &m.pos[3 * out[i + 1]]; const float* c = &m.pos[3 * out[i + 2]];
        const double e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        const double n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        const double ctr[3] = {a[0] + b[0] + c[0], a[1] + b[1] + c[1], a[2] + b[2] + c[2]};
        CHECK(n[0] * ctr[0] + n[1] * ctr[1] + n[2] * ctr[2] > 0.0);
    }
    // the surface stays within the bound: every removed vertex is within ~maxError of the new surface
    double worst = 0.0;
    for (uint32_t v = 0; v < m.nv(); ++v) worst = std::max(worst, MaxDistToSurface(m, out, v));
    std::printf("  farthest original vertex from the new surface: %.4f (bound %.4f)\n", worst, p.maxError);
    CHECK(worst <= p.maxError * 1.5);
    // indices are a subset of the input's: no vertex made, the buffer is not touched
    for (uint16_t i : out) CHECK(i < m.nv());
    // a larger bound gives fewer triangles
    nplod::SimpParams p2 = p; p2.maxError = 0.06f;
    std::vector<uint16_t> out2;
    nplod::Simplify(In(m), p2, out2, &st);
    CHECK(out2.size() < out.size());
    CHECK(Check(out2).watertight);
    // zero error budget: nothing is removed
    nplod::SimpParams p0 = p; p0.maxError = 0.0f;
    std::vector<uint16_t> out0;
    nplod::Simplify(In(m), p0, out0, &st);
    CHECK(out0.size() >= m.idx.size() * 0.97);
}

static void TestStrip()
{
    // a strip of a ring: the same triangles as the list built from it
    Mesh m;
    for (int i = 0; i < 40; ++i)
    {
        const float a = i * 0.157f;
        AddV(m, cosf(a), sinf(a), 0.0f);
        AddV(m, cosf(a), sinf(a), 1.0f);
    }
    for (uint16_t i = 0; i < 80; ++i) m.idx.push_back(i);
    nplod::SimpParams p; p.maxError = 0.0f;
    std::vector<uint16_t> out;
    nplod::SimpStats st;
    CHECK(nplod::Simplify(In(m, 1), p, out, &st));
    CHECK(st.inTris == 78);
    CHECK(out.size() == 78 * 3);
    // winding follows the strip: triangle 1 is (1,3,2)... i.e. the second triangle is reversed
    bool found = false;
    for (size_t i = 0; i + 2 < out.size(); i += 3) if (out[i] == 1 && out[i + 1] == 3 && out[i + 2] == 2) found = true;
    CHECK(found);
}

static void TestSeamAndSkin()
{
    // a grid with a seam: column 8 is duplicated (two vertices per position, one side each)
    const int N = 17;
    Mesh m;
    std::vector<int> ix(N * N), ix2(N * N, -1);
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x)
        {
            ix[y * N + x] = (int)m.nv();
            AddV(m, (float)x, (float)y, 0.1f * sinf(x * 0.3f), x / 16.0f, y / 16.0f);
            if (x == 8) { ix2[y * N + x] = (int)m.nv(); AddV(m, (float)x, (float)y, 0.1f * sinf(x * 0.3f), x / 16.0f + 0.5f, y / 16.0f); }
        }
    auto V = [&](int x, int y, bool right) { return (x == 8 && right) ? ix2[y * N + x] : ix[y * N + x]; };
    for (int y = 0; y < N - 1; ++y)
        for (int x = 0; x < N - 1; ++x)
        {
            const bool right = x >= 8;
            const int a = V(x, y, right), b = V(x + 1, y, right), c = V(x, y + 1, right), d = V(x + 1, y + 1, right);
            m.idx.push_back((uint16_t)a); m.idx.push_back((uint16_t)b); m.idx.push_back((uint16_t)c);
            m.idx.push_back((uint16_t)b); m.idx.push_back((uint16_t)d); m.idx.push_back((uint16_t)c);
        }
    // skin: two bones, weight 1 for x < 8, 0 for x > 8: the left half bone 3, the right half bone 5, hard split at the seam
    m.w.assign(m.nv() * 4, 0.0f);
    m.bones.assign(m.nv() * 4, 0);
    for (uint32_t v = 0; v < m.nv(); ++v)
    {
        bool isRightCopy = false;
        for (int y = 0; y < N; ++y) if ((int)v == ix2[y * N + 8]) isRightCopy = true;
        const bool left = m.pos[3 * v] < 8.0f || (m.pos[3 * v] == 8.0f && !isRightCopy);
        m.bones[4 * v] = left ? 3 : 5; m.w[4 * v] = 1.0f;
    }
    nplod::SimpParams p; p.maxError = 0.5f; p.uvMax = 10.0f; p.normalCos = -1.0f;   // plenty of budget: the locks and the skin rule decide
    std::vector<uint16_t> out;
    nplod::SimpStats st;
    CHECK(nplod::Simplify(In(m), p, out, &st));
    std::printf("seam grid: %u -> %u tris, locked %u of %u\n", st.inTris, st.outTris, st.lockedVerts, st.usedVerts);
    CHECK(st.outTris < st.inTris);
    // seam vertices (both copies of x == 8) and the border all survive and keep their meaning
    std::set<int> inOut(out.begin(), out.end());
    for (int y = 0; y < N; ++y) { CHECK(inOut.count(ix[y * N + 8]) == 1); CHECK(inOut.count(ix2[y * N + 8]) == 1); }
    for (int i = 0; i < N; ++i) { CHECK(inOut.count(ix[i]) == 1 && inOut.count(ix[(N - 1) * N + i]) == 1 && inOut.count(ix[i * N]) == 1); }
    // skin: what stands for a vertex has the same bone and weight (the tolerance is far under the 1.0 of a bone change)
    for (uint32_t v = 0; v < m.nv(); ++v)
    {
        const uint16_t r = st.collapsedTo[v];
        CHECK(m.bones[4 * r] == m.bones[4 * v]);
        CHECK(fabsf(m.w[4 * r] - m.w[4 * v]) <= p.weightTol);
    }
    // no triangle joins the two bones except along the seam (a triangle never mixes a left and a right bone away from x == 8)
    for (size_t i = 0; i + 2 < out.size(); i += 3)
    {
        const int b0 = m.bones[4 * out[i]], b1 = m.bones[4 * out[i + 1]], b2 = m.bones[4 * out[i + 2]];
        CHECK(b0 == b1 && b1 == b2);
    }
    // the border of the mesh is unchanged: the outer edges are still the same
    std::map<std::pair<int, int>, int> e;
    for (size_t i = 0; i + 2 < out.size(); i += 3)
        for (int k = 0; k < 3; ++k) { int a = out[i + k], b = out[i + (k + 1) % 3]; ++e[{std::min(a, b), std::max(a, b)}]; }
    for (auto& kv : e) CHECK(kv.second <= 2);
}

static void TestSoftWeights()
{
    // a cylinder skinned by a gradient between two bones: collapses may only join vertices with near weights; the deviation of what is merged stays under the tolerance
    Mesh m;
    const int R = 12, L = 40;
    for (int l = 0; l <= L; ++l)
        for (int r = 0; r < R; ++r)
        {
            const float a = r * 6.2831853f / R;
            AddV(m, cosf(a), sinf(a), l * 0.1f);
        }
    for (int l = 0; l < L; ++l)
        for (int r = 0; r < R; ++r)
        {
            const int a = l * R + r, b = l * R + (r + 1) % R, c = (l + 1) * R + r, d = (l + 1) * R + (r + 1) % R;
            m.idx.push_back((uint16_t)a); m.idx.push_back((uint16_t)c); m.idx.push_back((uint16_t)b);
            m.idx.push_back((uint16_t)b); m.idx.push_back((uint16_t)c); m.idx.push_back((uint16_t)d);
        }
    // caps are open: the two rings are boundaries (locked)
    m.w.assign(m.nv() * 4, 0.0f); m.bones.assign(m.nv() * 4, 0);
    for (int l = 0; l <= L; ++l)
        for (int r = 0; r < R; ++r)
        {
            const uint32_t v = l * R + r;
            const float t = (l > 14 && l < 26) ? (l - 14) / 12.0f : (l >= 26 ? 1.0f : 0.0f);
            m.bones[4 * v] = 1; m.bones[4 * v + 1] = 2; m.w[4 * v] = 1.0f - t; m.w[4 * v + 1] = t;
        }
    nplod::SimpParams p; p.maxError = 0.3f; p.weightTol = 0.5f; p.uvMax = 10.0f; p.normalCos = -1.0f; p.skinSpan = 0.0f;
    std::vector<uint16_t> out;
    nplod::SimpStats st;
    CHECK(nplod::Simplify(In(m), p, out, &st));
    std::printf("skinned cylinder: %u -> %u tris\n", st.inTris, st.outTris);
    CHECK(st.outTris < st.inTris / 2);
    double worst = 0.0;
    for (uint32_t v = 0; v < m.nv(); ++v)
    {
        const uint16_t r = st.collapsedTo[v];
        worst = std::max(worst, (double)fabsf(m.w[4 * r] - m.w[4 * v]) + fabsf(m.w[4 * r + 1] - m.w[4 * v + 1]));
    }
    std::printf("  largest L1 weight deviation of a merged vertex: %.3f (tolerance %.3f)\n", worst, p.weightTol);
    CHECK(worst <= p.weightTol + 1e-6 && worst > 0.0);
    // with the weight tolerance at zero, vertices with different weights never merge
    nplod::SimpParams p0 = p; p0.weightTol = 0.0f;
    std::vector<uint16_t> out0;
    nplod::Simplify(In(m), p0, out0, &st);
    for (uint32_t v = 0; v < m.nv(); ++v)
    {
        const uint16_t r = st.collapsedTo[v];
        CHECK(m.w[4 * r] == m.w[4 * v]);
    }
}


static void TestStripify()
{
    Mesh m = Sphere(3);
    std::vector<uint16_t> list;
    for (size_t i = 0; i < m.idx.size(); ++i) list.push_back(m.idx[i]);
    std::vector<uint16_t> strip, back;
    nplod::Stripify(list, strip);
    nplod::StripToTriangles(strip.data(), (uint32_t)strip.size(), back);
    auto canon = [](const std::vector<uint16_t>& v) {
        std::vector<std::array<uint16_t, 3>> r;
        for (size_t i = 0; i + 2 < v.size(); i += 3)
        {
            uint16_t a = v[i], b = v[i + 1], c = v[i + 2];
            if (b < a && b < c) { uint16_t t = a; a = b; b = c; c = t; } else if (c < a && c < b) { uint16_t t = a; a = c; c = b; b = t; }
            r.push_back({a, b, c});
        }
        std::sort(r.begin(), r.end());
        return r;
    };
    CHECK(canon(list) == canon(back));   /* same triangles, same winding, each once */
    std::printf("stripify: %zu tris -> %zu indices (%.2f per triangle)\n", list.size() / 3, strip.size(), (double)strip.size() / (list.size() / 3));
    CHECK(strip.size() < list.size());
}

int main()
{
    TestStripify();
    TestSelect();
    TestSphere();
    TestStrip();
    TestSeamAndSkin();
    TestSoftWeights();
    std::printf(fails ? "FAILED %d\n" : "lod tests passed\n", fails);
    return fails ? 1 : 0;
}
