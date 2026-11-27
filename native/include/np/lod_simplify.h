/* np/lod_simplify.h: edge-collapse mesh simplifier over an EXISTING vertex buffer (MSC_LOD_SIZE=2, 1.3 wave 5). Plain C++11, no game headers, so a host test
 * compiles it (native/tools/lod).
 *
 * It returns a triangle list whose indices are a subset of the input's vertex indices: the vertex buffer (positions, normals, UVs, skin weights and bone numbers)
 * is NOT touched, so skinning and animation, the display-list cache and the skin streams work as they do for the full mesh. A collapse removes vertex v and
 * puts u (which stays where it is, with its own normal, UV and weights) in its place in every triangle that used v ("half-edge collapse"): no vertex is moved or
 * made, so there is nothing to upload.
 *
 * What it keeps:
 *   - silhouette / shape: the quadric error (sum of squared distances to the planes of the faces the collapsed vertices stood for, unit weights, so its square
 *     root is an upper bound of the distance of the new surface to the old planes) stays under maxError (world units). The caller derives maxError from the
 *     size at which the mesh will be used (NpLodMaxError: one screen line).
 *   - seams and boundaries: a vertex whose position is shared by another vertex of the buffer (a UV or normal seam) or that lies on an open edge is never
 *     removed, so no crack opens and no seam moves. (A vertex next to one may still collapse onto it.)
 *   - skin: v collapses onto u only if their bone / weight sets agree within weightTol (L1 over the union of the bones), and the total deviation of everything
 *     that has been merged into a vertex stays under weightTol; the weight deviation is also charged to the error as deviation * skinSpan.
 *   - texture / shading: the UV distance and the normal angle of what has been merged stay under uvMax and normalCos.
 *   - topology: the link condition (no non-manifold pinch, no hole is closed or opened), and no remaining triangle flips (normal dot >= flipCos) or collapses
 *     to a sliver.
 */
#ifndef NP_LOD_SIMPLIFY_H
#define NP_LOD_SIMPLIFY_H

#include <stdint.h>
#include <string.h>
#include <math.h>

#include <algorithm>
#include <queue>
#include <unordered_map>
#include <vector>

namespace nplod {

struct SimpIn
{
    uint32_t nVerts = 0;
    const float* pos = 0;       /* 3 floats per vertex, host order */
    uint32_t posStride = 3;     /* in floats */
    const float* uv = 0;        /* optional: 2 floats per vertex (texture 0) */
    uint32_t uvStride = 2;
    const float* nrm = 0;       /* optional: 3 floats per vertex */
    uint32_t nrmStride = 3;
    const uint8_t* bones = 0;   /* optional: 4 per vertex (with weights) */
    const float* weights = 0;   /* optional: 4 per vertex */
    const uint16_t* idx = 0;
    uint32_t nIdx = 0;
    int prim = 0;               /* 0 triangle list, 1 triangle strip (the game's eGLPrimitive) */
};

struct SimpParams
{
    float maxError = 0.05f;     /* world units: the largest distance the surface may move */
    float skinSpan = 1.0f;      /* world units that a full weight change can displace a point (charged as deviation * span) */
    float weightTol = 0.05f;    /* L1 weight deviation, 0..2 */
    float uvMax = 0.03f;        /* texture units */
    float normalCos = 0.7f;     /* the normal of a removed vertex vs the one that replaces it */
    float flipCos = 0.2f;       /* a neighbouring triangle's normal after vs before */
    float minArea = 1.0e-9f;    /* a triangle smaller than this (after the collapse) is a sliver: collapse refused */
    uint32_t minTris = 0;       /* stop here */
};

struct SimpStats
{
    uint32_t inTris = 0, outTris = 0, collapses = 0;
    uint32_t lockedVerts = 0, usedVerts = 0;
    uint32_t refusedLink = 0, refusedFlip = 0, refusedSkin = 0, refusedUv = 0, refusedLock = 0, refusedError = 0;
    float maxErrorSeen = 0.0f;           /* sqrt of the largest quadric error of any collapse made */
    std::vector<uint16_t> collapsedTo;   /* per vertex: the vertex that stands for it now (itself when it stays); chains are resolved */
};

namespace detail {

struct Quadric
{
    double a[10];
    Quadric() { for (int i = 0; i < 10; ++i) a[i] = 0.0; }
    void AddPlane(double nx, double ny, double nz, double d, double w)
    {
        a[0] += w * nx * nx; a[1] += w * nx * ny; a[2] += w * nx * nz; a[3] += w * nx * d;
        a[4] += w * ny * ny; a[5] += w * ny * nz; a[6] += w * ny * d;
        a[7] += w * nz * nz; a[8] += w * nz * d;
        a[9] += w * d * d;
    }
    void Add(const Quadric& o) { for (int i = 0; i < 10; ++i) a[i] += o.a[i]; }
    double Eval(double x, double y, double z) const
    {
        const double v = a[0] * x * x + 2 * a[1] * x * y + 2 * a[2] * x * z + 2 * a[3] * x + a[4] * y * y + 2 * a[5] * y * z + 2 * a[6] * y + a[7] * z * z + 2 * a[8] * z + a[9];
        return v < 0.0 ? 0.0 : v;
    }
};

struct V3 { double x, y, z; };
inline V3 Sub(const V3& a, const V3& b) { return V3{a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 Cross(const V3& a, const V3& b) { return V3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline double Dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline double Len(const V3& a) { return sqrt(Dot(a, a)); }

struct Entry
{
    float cost;
    uint16_t v, u;      /* v is removed, u stays */
    uint32_t verV, verU;
    bool operator<(const Entry& o) const { return cost > o.cost; }   /* min-heap */
};

struct PosKey
{
    uint32_t x, y, z;
    bool operator==(const PosKey& o) const { return x == o.x && y == o.y && z == o.z; }
};
struct PosKeyHash
{
    size_t operator()(const PosKey& k) const { return (size_t)(k.x * 73856093u ^ k.y * 19349663u ^ k.z * 83492791u); }
};

} // namespace detail

/* Fills `out` with a triangle list. Returns false when the input is unusable (no triangles). */
inline bool Simplify(const SimpIn& in, const SimpParams& prm, std::vector<uint16_t>& out, SimpStats* stats = 0)
{
    using namespace detail;
    SimpStats local;
    SimpStats& st = stats ? *stats : local;
    st = SimpStats();
    out.clear();
    const uint32_t nv = in.nVerts;
    if (nv == 0 || nv > 65535 || in.pos == 0) return false;

    /* ---- triangles (strips become lists; degenerate triangles are dropped) */
    std::vector<uint16_t> tri;
    if (in.prim == 0)
    {
        for (uint32_t i = 0; i + 2 < in.nIdx; i += 3)
        {
            const uint16_t a = in.idx[i], b = in.idx[i + 1], c = in.idx[i + 2];
            if (a == b || b == c || a == c || a >= nv || b >= nv || c >= nv) continue;
            tri.push_back(a); tri.push_back(b); tri.push_back(c);
        }
    }
    else if (in.prim == 1)
    {
        for (uint32_t i = 0; i + 2 < in.nIdx; ++i)
        {
            uint16_t a = in.idx[i], b = in.idx[i + 1], c = in.idx[i + 2];
            if (a == b || b == c || a == c || a >= nv || b >= nv || c >= nv) continue;
            if (i & 1) std::swap(b, c);   /* keep the winding of the strip */
            tri.push_back(a); tri.push_back(b); tri.push_back(c);
        }
    }
    else
        return false;
    const uint32_t nt = (uint32_t)(tri.size() / 3);
    st.inTris = nt;
    if (nt == 0) return false;

    std::vector<V3> P(nv);
    for (uint32_t i = 0; i < nv; ++i)
    {
        const float* p = in.pos + (size_t)i * in.posStride;
        P[i] = V3{p[0], p[1], p[2]};
    }
    std::vector<uint8_t> alive(nt, 1);
    std::vector<std::vector<uint32_t>> vf(nv);
    std::vector<uint8_t> used(nv, 0);
    for (uint32_t t = 0; t < nt; ++t)
        for (int k = 0; k < 3; ++k) { vf[tri[3 * t + k]].push_back(t); used[tri[3 * t + k]] = 1; }

    /* ---- locks: seams (a position shared by two vertices) and open edges */
    std::vector<uint8_t> locked(nv, 0);
    {
        std::unordered_map<PosKey, uint16_t, PosKeyHash> seen;
        for (uint32_t i = 0; i < nv; ++i)
        {
            if (!used[i]) continue;
            const float* p = in.pos + (size_t)i * in.posStride;
            PosKey k;
            memcpy(&k.x, p, 4); memcpy(&k.y, p + 1, 4); memcpy(&k.z, p + 2, 4);
            auto it = seen.find(k);
            if (it == seen.end()) seen[k] = (uint16_t)i;
            else { locked[i] = 1; locked[it->second] = 1; }
        }
        std::unordered_map<uint32_t, int> edgeCount;
        for (uint32_t t = 0; t < nt; ++t)
            for (int k = 0; k < 3; ++k)
            {
                uint32_t a = tri[3 * t + k], b = tri[3 * t + (k + 1) % 3];
                if (a > b) std::swap(a, b);
                ++edgeCount[a << 16 | b];
            }
        for (auto& e : edgeCount)
            if (e.second != 2) { locked[e.first >> 16] = 1; locked[e.first & 0xFFFF] = 1; }
        for (uint32_t i = 0; i < nv; ++i) if (used[i]) { ++st.usedVerts; if (locked[i]) ++st.lockedVerts; }
    }

    /* ---- quadrics */
    std::vector<Quadric> Q(nv);
    for (uint32_t t = 0; t < nt; ++t)
    {
        const V3& a = P[tri[3 * t]]; const V3& b = P[tri[3 * t + 1]]; const V3& c = P[tri[3 * t + 2]];
        V3 n = Cross(Sub(b, a), Sub(c, a));
        const double l = Len(n);
        if (l < 1.0e-20) continue;
        n.x /= l; n.y /= l; n.z /= l;
        const double d = -Dot(n, a);
        for (int k = 0; k < 3; ++k) Q[tri[3 * t + k]].AddPlane(n.x, n.y, n.z, d, 1.0);
    }

    const bool haveSkin = in.bones && in.weights;
    auto skinDist = [&](uint32_t u, uint32_t v) -> double {
        if (!haveSkin) return 0.0;
        double d = 0.0;
        bool matched[4] = {false, false, false, false};
        for (int i = 0; i < 4; ++i)
        {
            const double wu = in.weights[4 * u + i];
            double wv = 0.0;
            for (int j = 0; j < 4; ++j)
                if (in.bones[4 * v + j] == in.bones[4 * u + i] && !matched[j]) { wv = in.weights[4 * v + j]; matched[j] = true; break; }
            d += fabs(wu - wv);
        }
        for (int j = 0; j < 4; ++j) if (!matched[j]) d += fabs((double)in.weights[4 * v + j]);
        return d;
    };
    auto uvDist = [&](uint32_t u, uint32_t v) -> double {
        if (!in.uv) return 0.0;
        const float* a = in.uv + (size_t)u * in.uvStride; const float* b = in.uv + (size_t)v * in.uvStride;
        return sqrt((double)(a[0] - b[0]) * (a[0] - b[0]) + (double)(a[1] - b[1]) * (a[1] - b[1]));
    };
    auto nrmDot = [&](uint32_t u, uint32_t v) -> double {
        if (!in.nrm) return 1.0;
        const float* a = in.nrm + (size_t)u * in.nrmStride; const float* b = in.nrm + (size_t)v * in.nrmStride;
        return (double)a[0] * b[0] + (double)a[1] * b[1] + (double)a[2] * b[2];
    };

    std::vector<float> wDev(nv, 0.0f), uvDev(nv, 0.0f);   /* the largest deviation of anything merged into the vertex */
    std::vector<uint32_t> ver(nv, 0);
    std::vector<uint16_t> to(nv);
    for (uint32_t i = 0; i < nv; ++i) to[i] = (uint16_t)i;
    st.collapsedTo.assign(nv, 0);

    uint32_t liveTris = nt;
    const double maxErr2 = (double)prm.maxError * prm.maxError;

    /* neighbours of a vertex (alive triangles) */
    auto compactFaces = [&](uint32_t v) {
        std::vector<uint32_t>& f = vf[v];
        size_t w = 0;
        for (size_t i = 0; i < f.size(); ++i) if (alive[f[i]]) f[w++] = f[i];
        f.resize(w);
    };

    /* cost of collapsing v onto u; returns false (with the reason counted when `count`) when it is not allowed. */
    auto evaluate = [&](uint32_t v, uint32_t u, bool count, double& cost) -> bool {
        if (locked[v]) { if (count) ++st.refusedLock; return false; }
        const double sd = skinDist(u, v);
        const double wnew = std::max((double)wDev[u], (double)wDev[v] + sd);
        if (haveSkin && wnew > prm.weightTol) { if (count) ++st.refusedSkin; return false; }
        const double ud = uvDist(u, v);
        if (in.uv && std::max((double)uvDev[u], (double)uvDev[v] + ud) > prm.uvMax) { if (count) ++st.refusedUv; return false; }
        if (in.nrm && nrmDot(u, v) < prm.normalCos) { if (count) ++st.refusedUv; return false; }
        Quadric m = Q[u];
        m.Add(Q[v]);
        const double e = m.Eval(P[u].x, P[u].y, P[u].z);
        const double skinE = wnew * prm.skinSpan;
        cost = e + skinE * skinE;
        if (cost > maxErr2) { if (count) ++st.refusedError; return false; }
        return true;
    };

    /* topology and flips: v's alive faces; the edge (u,v) must exist */
    auto topologyOk = [&](uint32_t v, uint32_t u, bool count) -> bool {
        compactFaces(v);
        compactFaces(u);
        /* faces on the edge, and the link */
        std::vector<uint32_t> third;
        for (uint32_t t : vf[v])
        {
            bool hasU = false;
            uint32_t other = 0xFFFFFFFFu;
            for (int k = 0; k < 3; ++k)
            {
                const uint32_t w = tri[3 * t + k];
                if (w == u) hasU = true;
                else if (w != v) other = w;
            }
            if (hasU) third.push_back(other);
        }
        if (third.size() != 2) { if (count) ++st.refusedLink; return false; }   /* not an interior edge */
        /* link condition: the common neighbours of u and v are exactly those two */
        std::vector<uint32_t> nu, nvv;
        for (uint32_t t : vf[u]) for (int k = 0; k < 3; ++k) { const uint32_t w = tri[3 * t + k]; if (w != u && w != v) nu.push_back(w); }
        for (uint32_t t : vf[v]) for (int k = 0; k < 3; ++k) { const uint32_t w = tri[3 * t + k]; if (w != u && w != v) nvv.push_back(w); }
        std::sort(nu.begin(), nu.end()); nu.erase(std::unique(nu.begin(), nu.end()), nu.end());
        std::sort(nvv.begin(), nvv.end()); nvv.erase(std::unique(nvv.begin(), nvv.end()), nvv.end());
        uint32_t common = 0;
        for (uint32_t w : nvv) if (std::binary_search(nu.begin(), nu.end(), w)) ++common;
        if (common != 2) { if (count) ++st.refusedLink; return false; }
        /* flips and slivers of the faces that stay (those of v without u) */
        for (uint32_t t : vf[v])
        {
            bool hasU = false;
            for (int k = 0; k < 3; ++k) if (tri[3 * t + k] == u) hasU = true;
            if (hasU) continue;
            V3 q[3], r[3];
            for (int k = 0; k < 3; ++k)
            {
                const uint32_t w = tri[3 * t + k];
                q[k] = P[w];
                r[k] = P[w == v ? u : w];
            }
            const V3 n0 = Cross(Sub(q[1], q[0]), Sub(q[2], q[0]));
            const V3 n1 = Cross(Sub(r[1], r[0]), Sub(r[2], r[0]));
            const double l0 = Len(n0), l1 = Len(n1);
            if (l1 * 0.5 < prm.minArea || (l0 > 0 && Dot(n0, n1) / (l0 * l1) < prm.flipCos)) { if (count) ++st.refusedFlip; return false; }
        }
        return true;
    };

    std::priority_queue<Entry> heap;
    auto push = [&](uint32_t v, uint32_t u) {
        double cost;
        if (evaluate(v, u, false, cost))
            heap.push(Entry{(float)cost, (uint16_t)v, (uint16_t)u, ver[v], ver[u]});
    };
    for (uint32_t t = 0; t < nt; ++t)
        for (int k = 0; k < 3; ++k)
        {
            const uint32_t a = tri[3 * t + k], b = tri[3 * t + (k + 1) % 3];
            push(a, b); push(b, a);
        }

    while (!heap.empty() && liveTris > prm.minTris)
    {
        const Entry e = heap.top();
        heap.pop();
        if (ver[e.v] != e.verV || ver[e.u] != e.verU || !used[e.v] || !used[e.u]) continue;
        double cost;
        if (!evaluate(e.v, e.u, true, cost)) continue;
        if (!topologyOk(e.v, e.u, true)) continue;

        /* collapse v onto u */
        const double sd = skinDist(e.u, e.v);
        wDev[e.u] = (float)std::max((double)wDev[e.u], (double)wDev[e.v] + sd);
        uvDev[e.u] = (float)std::max((double)uvDev[e.u], (double)uvDev[e.v] + uvDist(e.u, e.v));
        Q[e.u].Add(Q[e.v]);
        st.maxErrorSeen = std::max(st.maxErrorSeen, (float)sqrt(cost));
        for (uint32_t t : vf[e.v])
        {
            if (!alive[t]) continue;
            bool hasU = false;
            for (int k = 0; k < 3; ++k) if (tri[3 * t + k] == e.u) hasU = true;
            if (hasU) { alive[t] = 0; --liveTris; continue; }
            for (int k = 0; k < 3; ++k) if (tri[3 * t + k] == e.v) tri[3 * t + k] = (uint16_t)e.u;
            vf[e.u].push_back(t);
        }
        vf[e.v].clear();
        used[e.v] = 0;
        to[e.v] = (uint16_t)e.u;
        ++ver[e.u]; ++ver[e.v];
        ++st.collapses;

        compactFaces(e.u);
        std::vector<uint32_t> nb;
        for (uint32_t t : vf[e.u]) for (int k = 0; k < 3; ++k) { const uint32_t w = tri[3 * t + k]; if (w != e.u) nb.push_back(w); }
        std::sort(nb.begin(), nb.end()); nb.erase(std::unique(nb.begin(), nb.end()), nb.end());
        for (uint32_t w : nb) { push(w, e.u); push(e.u, w); }
    }

    for (uint32_t t = 0; t < nt; ++t)
        if (alive[t]) { out.push_back(tri[3 * t]); out.push_back(tri[3 * t + 1]); out.push_back(tri[3 * t + 2]); }
    st.outTris = (uint32_t)(out.size() / 3);
    for (uint32_t i = 0; i < nv; ++i)
    {
        uint32_t r = i;
        while (to[r] != r) r = to[r];
        st.collapsedTo[i] = (uint16_t)r;
    }
    return true;
}


/* Triangle list -> ONE triangle strip with degenerate stitches (the game's display lists draw a packet as a single strip; a list would cost three indices a
 * triangle). Greedy: walk to the neighbouring triangle across the strip's last edge, taking the longest of the three starting rotations, restarting with
 * repeated indices (degenerate triangles, as the game's own strips have). Winding is kept: StripToTriangles gives back every input triangle exactly once. */
inline void Stripify(const std::vector<uint16_t>& tris, std::vector<uint16_t>& strip)
{
    strip.clear();
    const uint32_t nt = (uint32_t)(tris.size() / 3);
    if (nt == 0) return;
    std::unordered_map<uint32_t, uint32_t> edge;   /* directed edge (a<<16|b) -> triangle */
    edge.reserve(nt * 6);
    for (uint32_t t = 0; t < nt; ++t)
        for (int k = 0; k < 3; ++k)
            edge[((uint32_t)tris[3 * t + k] << 16) | tris[3 * t + (k + 1) % 3]] = t;
    std::vector<uint8_t> used(nt, 0);
    /* grow from triangle t0 listed with rotation rot as strip triangle 0; mark: keep the triangles used */
    auto grow = [&](uint32_t t0, int rot, std::vector<uint16_t>* seq, bool mark) -> uint32_t {
        const uint16_t s0 = tris[3 * t0 + rot], s1 = tris[3 * t0 + (rot + 1) % 3], s2 = tris[3 * t0 + (rot + 2) % 3];
        if (seq) { seq->clear(); seq->push_back(s0); seq->push_back(s1); seq->push_back(s2); }
        std::vector<uint32_t> got; got.push_back(t0);
        used[t0] = 1;
        uint32_t k = 0;
        uint16_t u = s1, w = s2;   /* the tail: the next strip triangle is (u,w,x) */
        for (;;)
        {
            ++k;   /* its index; in list orientation it is (u,w,x) when k is even and (u,x,w) when odd */
            const uint32_t key = (k & 1) ? (((uint32_t)w << 16) | u) : (((uint32_t)u << 16) | w);
            auto it = edge.find(key);
            if (it == edge.end() || used[it->second]) break;
            const uint32_t t = it->second;
            uint16_t x = 0;
            bool ok = false;
            for (int j = 0; j < 3; ++j) { const uint16_t v = tris[3 * t + j]; if (v != u && v != w) { x = v; ok = true; } }
            if (!ok) break;
            used[t] = 1; got.push_back(t);
            if (seq) seq->push_back(x);
            u = w; w = x;
        }
        if (!mark) for (uint32_t t : got) used[t] = 0;
        return (uint32_t)got.size();
    };
    std::vector<uint16_t> cur;
    bool first = true;
    for (uint32_t t = 0; t < nt; ++t)
    {
        if (used[t]) continue;
        uint32_t bestN = 0; int bestRot = 0;
        for (int rot = 0; rot < 3; ++rot)
        {
            const uint32_t n = grow(t, rot, 0, false);
            if (n > bestN) { bestN = n; bestRot = rot; }
        }
        grow(t, bestRot, &cur, true);
        if (first) { strip = cur; first = false; continue; }
        const uint16_t last = strip.back(), head = cur[0];
        strip.push_back(last);
        strip.push_back(head);
        if (strip.size() & 1) strip.push_back(head);   /* the next strip's first vertex must sit at an even index to keep its winding */
        strip.insert(strip.end(), cur.begin(), cur.end());
    }
}

/* The strip back to a list of non-degenerate triangles with the strip's winding (tests, statistics). */
inline void StripToTriangles(const uint16_t* idx, uint32_t n, std::vector<uint16_t>& out)
{
    out.clear();
    for (uint32_t i = 0; i + 2 < n; ++i)
    {
        uint16_t a = idx[i], b = idx[i + 1], c = idx[i + 2];
        if (a == b || b == c || a == c) continue;
        if (i & 1) std::swap(b, c);
        out.push_back(a); out.push_back(b); out.push_back(c);
    }
}

} // namespace nplod

#endif /* NP_LOD_SIMPLIFY_H */
