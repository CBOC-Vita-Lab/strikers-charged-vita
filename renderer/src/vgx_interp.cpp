// Frame interpolation: see vgx_interp.h for what it does and the switches.
//
// How draws are paired. A draw record names its geometry: a static buffer (the cached
// display list) with an index range, or vertices in the frame's arena. Two static draws can
// be the same object in two frames when buffer, range, layout and shaders agree (textures are
// left out: a face that changes its eye texture is still the face); two arena draws when
// vertex count, index count, layout, shaders, textures and blend state agree. Draws with the
// same key form a group in each frame. One on each side: paired. More (the same mesh drawn
// for several characters, or once per pass): every combination gets a cost, the distance
// between the two model-view translations (the object's origin in eye space), and pairs are
// taken cheapest first; equal costs keep submission order. A pair whose origin then turns out
// to have moved further than anything moves in one built frame is not blended (kept as
// "jump"): it gets the new frame's matrices like an unpaired draw.
//
// How blocks are blended. Component-wise: every vertex of the in-between picture is then on
// the straight line between its two eye-space positions, whatever the matrices mean, so
// skinned meshes stay connected and things that are attached to each other stay attached
// (the camera is part of every model-view matrix here: GX has no view matrix). The price is
// that a rotation by an angle a shrinks what rotates by cos(a/2) half-way: 1.5 % at 20
// degrees. A rigid draw (no palette) that rotates by more than MSC_VGX_INTERP_ROT (15) degrees
// about its own origin (the ball) is blended as a rotation instead (quaternion), with its
// scale. Normal matrices are scaled back to their length. Projections are blended only
// between two of the same kind; texture matrices only when no element moves by more than
// MSC_VGX_INTERP_TEXM (a matrix that selects a cell of an atlas must not pass through the
// cells between).
#include "vgx_interp.h"
#include "vgx_shadergen.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace vgx::interp {

namespace {

enum : uint8_t { kReasonNone = 0, kReasonFirst = 1, kReasonNotReplayable = 2, kReasonFewPairs = 3, kReasonCut = 4, kReasonReset = 5, kReasonGap = 6, kReasonEfbState = 7 };

struct Params {
    int mode = -1;            // -1: not read yet
    float jumpAngleCos = 0.9659f; // perspective: the origin's direction from the eye turns by more than acos(this) (15 degrees) ...
    float jumpDepth = 1.5f;       // ... or its distance changes by more than this factor: a jump
    float jumpNdc = 0.6f;         // orthographic: the origin moves by more than this (the screen is 2 x 2)
    float cutFraction = 0.5f;     // more than this share of the paired rigid static draws jump: a cut
    float minPair = 0.5f;         // less than this share of the static draws paired: another scene
    float texmMax = 0.25f;        // largest change of a texture-matrix element that is still blended
    float rotC = 0.9773f;         // (1 + 2 cos a) / 3 for a = 15 degrees: beyond it a rigid draw is blended as a rotation
    int dyn = 1;                  // pair arena draws at all
    int verts = 1;                // blend the vertex positions of paired arena draws
    int noPlan = 0;               // MSC_VGX_INTERP_NOPLAN=1: every picture does all the work again (the first version)
    int keyShaders = 0;           // shader ids are part of the pairing key of static draws (the first prototype's rule)
    int log = 0;
} g_p;
bool g_timing = false;

void Init() {
    if (g_p.mode >= 0) return;
    g_p.mode = static_cast<int>(EnvInt("MSC_VGX_INTERP", 0));
    if (g_p.mode != 0 && g_p.mode != 1 && g_p.mode != 2 && g_p.mode != 32) g_p.mode = 1;
    g_p.log = static_cast<int>(EnvInt("MSC_VGX_INTERP_LOG", 0));
    g_p.dyn = static_cast<int>(EnvInt("MSC_VGX_INTERP_DYN", 1));
    g_p.verts = static_cast<int>(EnvInt("MSC_VGX_INTERP_VERTS", 1));
    g_p.keyShaders = static_cast<int>(EnvInt("MSC_VGX_INTERP_KEYSHADERS", 0));
    g_timing = EnvInt("MSC_VGX_INTERP_TIMING", 0) != 0;
    g_p.noPlan = static_cast<int>(EnvInt("MSC_VGX_INTERP_NOPLAN", 0));
    auto envf = [](const char* n, float d) { const char* v = std::getenv(n); return (v && *v) ? static_cast<float>(std::atof(v)) : d; };
    const float deg = 3.14159265f / 180.0f;
    g_p.jumpAngleCos = std::cos(envf("MSC_VGX_INTERP_JUMP", 15.0f) * deg);
    g_p.jumpDepth = envf("MSC_VGX_INTERP_JUMPDEPTH", 1.5f);
    g_p.jumpNdc = envf("MSC_VGX_INTERP_JUMPNDC", 0.6f);
    g_p.cutFraction = envf("MSC_VGX_INTERP_CUT", 0.5f);
    g_p.minPair = envf("MSC_VGX_INTERP_MINPAIR", 0.5f);
    g_p.texmMax = envf("MSC_VGX_INTERP_TEXM", 0.25f);
    g_p.rotC = (1.0f + 2.0f * std::cos(envf("MSC_VGX_INTERP_ROT", 15.0f) * deg)) / 3.0f;
}

PictureHook g_hook = nullptr;
// MSC_VGX_INTERP_TIMING=1 (diagnostic, fi_replay prints it): microseconds of wall time in the phases of Begin() and Blend().
struct Timing { double begin = 0, pairing = 0, jumps = 0, buildReset = 0, buildPaired = 0, buildUnpaired = 0, arena = 0, exec = 0; uint64_t blends = 0, begins = 0; } g_tm;
inline double TNow() { return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
FrameStats g_last;
Totals g_tot;

// ---- shader sources by id (for the sizes of the uniform arrays a program declares)
std::vector<const ShaderSource*> g_vs, g_fs;
void NoteShaders(const CmdBuf& buf) {
    for (const Cmd& c : buf.cmds) {
        if (c.type != CMD_SHADER) continue;
        const ShaderSource* s = buf.shaders[c.arg];
        auto& tab = s->fragment ? g_fs : g_vs;
        if (tab.size() <= s->id) tab.resize(static_cast<size_t>(s->id) + 16, nullptr);
        tab[s->id] = s;
    }
}
// Floats of block b that the draw's program reads (0: none, or not known).
inline uint32_t BlockLen(int b, const DrawRec& d) {
    const ShaderSource* vs = d.vsId < g_vs.size() ? g_vs[d.vsId] : nullptr;
    switch (b) {
    case UB_PROJ: return 16;
    case UB_MV: return 24;
    case UB_TEXM: return !vs ? 0u : (vs->nPtm ? 96u + 4u * vs->nPtm : 4u * vs->nTm);
    case UB_PAL: return vs ? 4u * vs->nPal : 0u;
    case UB_LIGHT: return vs ? 4u * vs->nLt : 0u;
    default: return 0;
    }
}

// ---- the previous frame
struct Prev {
    bool valid = false;
    uint64_t frame = 0;
    std::vector<DrawRec> draws;
    std::vector<float, NoInitAlloc<float>> uni; // (same type as CmdBuf::uni, swapped with it)
    std::vector<uint64_t> key;
    std::vector<uint8_t, NoInitAlloc<uint8_t>> vtx; // the frame's vertex arena (arena draws are blended vertex by vertex)
    std::vector<uint32_t> vtxChunks;
} g_prev;

// One frame as the pairing and blending code sees it.
struct View {
    const DrawRec* draws = nullptr;
    const float* uni = nullptr;
    size_t uniN = 0;
    const uint8_t* vtx = nullptr;
    size_t vtxN = 0;
    const uint32_t* chunks = nullptr;
    size_t nChunks = 0;
};
inline View ViewOfPrev() {
    return View{g_prev.draws.data(), g_prev.uni.data(), g_prev.uni.size(), g_prev.vtx.data(), g_prev.vtx.size(), g_prev.vtxChunks.data(), g_prev.vtxChunks.size()};
}
inline View ViewOf(const CmdBuf& b) { return View{b.draws.data(), b.uni.data(), b.uni.size(), b.vtx.data(), b.vtx.size(), b.vtxChunks.data(), b.vtxChunks.size()}; }
// The vertices of an arena draw (null if it has none or they are not all inside the arena).
inline const uint8_t* ArenaVerts(const DrawRec& d, const View& v, uint32_t stride) {
    if (d.vbuf || !d.vtxCount) return nullptr;
    size_t off = static_cast<size_t>(d.vtxOffset) + static_cast<size_t>(d.vtxFirst) * stride;
    if (v.nChunks) {
        if (d.vchunk >= v.nChunks) return nullptr;
        off += v.chunks[d.vchunk];
    }
    if (off + static_cast<size_t>(d.vtxCount) * stride > v.vtxN) return nullptr;
    return v.vtx + off;
}
// Does the draw's vertex shader read the matrix palette (a skinned mesh)? Not d.ublock[UB_PAL]:
// the front end leaves the last palette's offset in every later record.
inline bool Skinned(const DrawRec& d) {
    const ShaderSource* vs = d.vsId < g_vs.size() ? g_vs[d.vsId] : nullptr;
    return vs && vs->nPal != 0;
}

// ---- per-frame work arrays (members so that nothing is allocated per frame)
struct Group { // draws of both frames with one key
    uint64_t key = 0;
    int32_t firstA = -1, lastA = -1, firstB = -1, lastB = -1;
    uint16_t nA = 0, nB = 0;
};
std::vector<Group> g_groups;
std::vector<int32_t> g_slot;   // open addressing: index into g_groups, -1 empty
std::vector<int32_t> g_nextA, g_nextB; // next draw with the same key
std::vector<uint64_t> g_keyB;
std::vector<int32_t> g_pairOfB; // draw of the previous frame, -1 none, -2 - a: paired with a but a jump
std::vector<DrawRec> g_alt;

struct KeyData {
    uint32_t vbuf, vtxOffset, vtxFirst, idxOffset, idxCount, vtxCount, layoutMask;
    uint16_t vsId, fsId;
    uint8_t prim, texMask, blend, pad;
    uint32_t tex[8];
    uint32_t target; // 0: the EFB; else the texture (and cell) of the offscreen pass the draw belongs to
};
inline uint64_t DrawKey(const DrawRec& d, uint32_t target) {
    KeyData k;
    std::memset(&k, 0, sizeof k);
    k.target = target;
    k.vbuf = d.vbuf;
    k.idxCount = d.idxCount;
    k.layoutMask = d.layoutMask;
    k.prim = d.prim;
    if (d.vbuf) {
        // A static draw is the same object whatever shaders it is drawn with: when the number of lights in play changes (the glow of a shot, a
        // flare) every vertex shader of the scene is another variant for a frame or two, and with the shader ids in the key nothing paired
        // (frames 1212 and 1217 of the survey: a kick). Blocks of another length are not blended (BuildPicture), the draw keeps its partner.
        if (!g_p.keyShaders) { k.vsId = 0; k.fsId = 0; }
        else { k.vsId = d.vsId; k.fsId = d.fsId; }
    } else {
        k.vsId = d.vsId;
        k.fsId = d.fsId;
    }
    if (d.vbuf) {
        k.vtxOffset = d.vtxOffset;
        k.vtxFirst = d.vtxFirst;
        k.idxOffset = d.idxOffset;
    } else {
        k.vtxCount = d.vtxCount;
        k.texMask = d.texMask;
        k.blend = static_cast<uint8_t>(d.rs.blend | (d.rs.blendSrc << 1) | (d.rs.blendDst << 4));
        for (int m = 0; m < 8; ++m)
            if (d.texMask & (1u << m)) k.tex[m] = d.tex[m].texId;
    }
    uint64_t h = HashKey(&k, sizeof k);
    return h ? h : 1;
}

// Where the object is in eye space, and what kind of projection it is drawn with: the origin
// of a static draw's model-view matrix; for an arena draw (sprites, impostor quads, HUD,
// immediate geometry: their matrix is often just the camera's or the identity) the centre of
// its first vertices under that matrix.
struct Anchor {
    float x = 0, y = 0, z = 0;
    bool have = false, ortho = false;
    float nx = 0, ny = 0; // orthographic: position on the screen (-1..1)
};
inline Anchor AnchorOf(const DrawRec& d, const View& v) {
    Anchor a;
    if (d.ublock[UB_MV] == kNoBlock || d.ublock[UB_MV] + 24 > v.uniN) return a;
    const float* mv = v.uni + d.ublock[UB_MV];
    a.x = mv[3]; a.y = mv[7]; a.z = mv[11];
    a.have = true;
    if (!d.vbuf && !Skinned(d)) {
        const uint32_t stride = MakeLayout(d.layoutMask).stride;
        if (const uint8_t* p = ArenaVerts(d, v, stride)) {
            const uint32_t n = d.vtxCount < 16 ? d.vtxCount : 16;
            float cx = 0, cy = 0, cz = 0;
            for (uint32_t i = 0; i < n; ++i) {
                float q[3];
                std::memcpy(q, p + static_cast<size_t>(i) * stride, 12);
                cx += q[0]; cy += q[1]; cz += q[2];
            }
            const float k = 1.0f / static_cast<float>(n);
            cx *= k; cy *= k; cz *= k;
            a.x = mv[0] * cx + mv[1] * cy + mv[2] * cz + mv[3];
            a.y = mv[4] * cx + mv[5] * cy + mv[6] * cz + mv[7];
            a.z = mv[8] * cx + mv[9] * cy + mv[10] * cz + mv[11];
        }
    }
    if (d.ublock[UB_PROJ] != kNoBlock && d.ublock[UB_PROJ] + 16 <= v.uniN) {
        const float* p = v.uni + d.ublock[UB_PROJ];
        if (p[15] == 1.0f) {
            a.ortho = true;
            a.nx = p[0] * a.x + p[12];
            a.ny = p[5] * a.y + p[13];
        }
    }
    return a;
}
inline float Dist2(const Anchor& a, const Anchor& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}
// Has a point in eye space (perspective) moved further than something that moves does in one built frame?
inline bool PointJump(const float* a, const float* b) {
    const float la = a[0] * a[0] + a[1] * a[1] + a[2] * a[2], lb = b[0] * b[0] + b[1] * b[1] + b[2] * b[2];
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    const float d2 = dx * dx + dy * dy + dz * dz;
    if (la < 1e-6f || lb < 1e-6f) return d2 > 1.0f; // (at the eye: identity model-view, nothing to turn)
    // Close to the eye a small step is a large angle: anything that moved less than a
    // tenth of its distance is not a jump whatever the angle.
    if (d2 < 0.01f * std::max(la, lb)) return false;
    const float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    if (dot <= 0) return true;
    if (dot * dot < g_p.jumpAngleCos * g_p.jumpAngleCos * la * lb) return true;
    const float r = g_p.jumpDepth * g_p.jumpDepth;
    return la > lb * r || lb > la * r;
}
inline bool IsJump(const Anchor& a, const Anchor& b) {
    if (!a.have || !b.have) return false;
    if (a.ortho != b.ortho) return true;
    if (a.ortho) {
        const float dx = a.nx - b.nx, dy = a.ny - b.ny;
        return dx * dx + dy * dy > g_p.jumpNdc * g_p.jumpNdc;
    }
    return PointJump(&a.x, &b.x);
}
// A skinned mesh: its model-view matrix says nothing about where it is, its palette does,
// but a palette is the whole matrix memory as it was at the draw, with rows the mesh does
// not use left over from whatever was drawn before. So: a jump when most of the matrices
// that changed at all jumped.
inline bool PaletteJump(const DrawRec& da, const View& va, const DrawRec& db, const View& vb) {
    const ShaderSource* vs = db.vsId < g_vs.size() ? g_vs[db.vsId] : nullptr;
    if (!vs || !vs->nPal || da.ublock[UB_PAL] == kNoBlock || db.ublock[UB_PAL] == kNoBlock) return false;
    const uint32_t rows = vs->nPal == kPalRowsFull ? 63u : vs->nPal / 2u; // position rows (three per matrix): the full palette's, or the short one's (60 rows: 30; a bucket: half its rows)
    if (da.ublock[UB_PAL] + rows * 4 > va.uniN || db.ublock[UB_PAL] + rows * 4 > vb.uniN) return false;
    const float* a = va.uni + da.ublock[UB_PAL];
    const float* b = vb.uni + db.ublock[UB_PAL];
    uint32_t moved = 0, jumped = 0;
    for (uint32_t m = 0; m + 3 <= rows; m += 3) {
        const float pa[3] = {a[m * 4 + 3], a[m * 4 + 7], a[m * 4 + 11]};
        const float pb[3] = {b[m * 4 + 3], b[m * 4 + 7], b[m * 4 + 11]};
        if (pa[0] == pb[0] && pa[1] == pb[1] && pa[2] == pb[2]) continue;
        ++moved;
        if (PointJump(pa, pb)) ++jumped;
    }
    return moved && jumped * 2 > moved;
}

// ---- pairing
void BuildGroups(const CmdBuf& buf) {
    const size_t nA = g_prev.draws.size(), nB = buf.draws.size();
    size_t cap = 64;
    while (cap < 2 * (nA + nB)) cap <<= 1;
    g_slot.assign(cap, -1);
    g_groups.clear();
    g_nextA.assign(nA, -1);
    g_nextB.assign(nB, -1);
    const size_t mask = cap - 1;
    auto find = [&](uint64_t key) -> Group& {
        size_t i = static_cast<size_t>(key ^ (key >> 29)) & mask;
        for (;;) {
            const int32_t s = g_slot[i];
            if (s < 0) {
                g_slot[i] = static_cast<int32_t>(g_groups.size());
                g_groups.emplace_back();
                g_groups.back().key = key;
                return g_groups.back();
            }
            if (g_groups[s].key == key) return g_groups[s];
            i = (i + 1) & mask;
        }
    };
    for (size_t i = 0; i < nA; ++i) {
        if (!g_p.dyn && !g_prev.draws[i].vbuf) continue;
        Group& g = find(g_prev.key[i]);
        if (g.lastA >= 0) g_nextA[g.lastA] = static_cast<int32_t>(i); else g.firstA = static_cast<int32_t>(i);
        g.lastA = static_cast<int32_t>(i);
        if (g.nA < 0xFFFF) ++g.nA;
    }
    for (size_t i = 0; i < nB; ++i) {
        if (!g_p.dyn && !buf.draws[i].vbuf) continue;
        Group& g = find(g_keyB[i]);
        if (g.lastB >= 0) g_nextB[g.lastB] = static_cast<int32_t>(i); else g.firstB = static_cast<int32_t>(i);
        g.lastB = static_cast<int32_t>(i);
        if (g.nB < 0xFFFF) ++g.nB;
    }
}

struct Cand { float cost; uint16_t a, b; };
constexpr uint32_t kMaxCand = 1024;

void PairDraws(const CmdBuf& buf, FrameStats& st) {
    const size_t nB = buf.draws.size();
    g_pairOfB.assign(nB, -1);
    const View va = ViewOfPrev(), vb = ViewOf(buf);
    static std::vector<int32_t> la, lb;
    static std::vector<Anchor> aa, ab;
    static std::vector<uint8_t> usedA, usedB;
    static Cand cand[kMaxCand];
    for (const Group& g : g_groups) {
        if (!g.nA || !g.nB) continue;
        if (g.nA == 1 && g.nB == 1) {
            g_pairOfB[g.firstB] = g.firstA;
            continue;
        }
        ++st.groupsAmbiguous;
        la.clear(); lb.clear(); aa.clear(); ab.clear();
        for (int32_t i = g.firstA; i >= 0; i = g_nextA[i]) { la.push_back(i); aa.push_back(AnchorOf(g_prev.draws[i], va)); }
        for (int32_t i = g.firstB; i >= 0; i = g_nextB[i]) { lb.push_back(i); ab.push_back(AnchorOf(buf.draws[i], vb)); }
        const size_t na = la.size(), nb = lb.size();
        if (na * nb > kMaxCand) {
            // Too many to compare: in order.
            for (size_t i = 0; i < std::min(na, nb); ++i) g_pairOfB[lb[i]] = la[i];
            continue;
        }
        uint32_t n = 0;
        for (size_t i = 0; i < na; ++i)
            for (size_t j = 0; j < nb; ++j) cand[n++] = Cand{Dist2(aa[i], ab[j]), static_cast<uint16_t>(i), static_cast<uint16_t>(j)};
        // Cheapest first; equal costs in submission order (same position in the two lists first).
        std::sort(cand, cand + n, [](const Cand& x, const Cand& y) {
            if (x.cost != y.cost) return x.cost < y.cost;
            const int dx = std::abs(static_cast<int>(x.a) - static_cast<int>(x.b)), dy = std::abs(static_cast<int>(y.a) - static_cast<int>(y.b));
            if (dx != dy) return dx < dy;
            return x.a < y.a;
        });
        usedA.assign(na, 0);
        usedB.assign(nb, 0);
        bool reordered = false;
        for (uint32_t k = 0; k < n; ++k) {
            const Cand& c = cand[k];
            if (usedA[c.a] || usedB[c.b]) continue;
            usedA[c.a] = usedB[c.b] = 1;
            g_pairOfB[lb[c.b]] = la[c.a];
            reordered |= c.a != c.b;
        }
        if (reordered) ++st.groupsReordered;
    }
}

// ---- blending
// Blended blocks of this picture: (kind, block of the new frame) -> block of the previous
// frame it was blended with and where the result is.
struct BlendMap {
    struct E { uint32_t offB = kNoBlock, offA = 0, out = 0; };
    std::vector<E> tab[UB_COUNT];
    void reset(size_t blocksGuess) {
        size_t cap = 256;
        while (cap < 2 * blocksGuess) cap <<= 1;
        for (int b = 0; b < UB_COUNT; ++b) tab[b].assign(b == UB_TEV ? 1 : cap, E{});
    }
    E& at(int b, uint32_t offB) {
        std::vector<E>& t = tab[b];
        const size_t mask = t.size() - 1;
        size_t i = (offB * 2654435761u >> 12) & mask;
        while (t[i].offB != kNoBlock && t[i].offB != offB) i = (i + 1) & mask;
        return t[i];
    }
} g_map;
// Model-view and projection blocks by CONTENT, for draws that could not be paired but are
// drawn with the same camera as something that was (particles, crowd, lines).
struct ContentMap {
    struct E { uint64_t h = 0; uint32_t out = 0; };
    std::vector<E> tab;
    void reset(size_t guess) {
        size_t cap = 256;
        while (cap < 2 * guess) cap <<= 1;
        tab.assign(cap, E{});
    }
    E& at(uint64_t h) {
        const size_t mask = tab.size() - 1;
        size_t i = static_cast<size_t>(h ^ (h >> 31)) & mask;
        while (tab[i].h && tab[i].h != h) i = (i + 1) & mask;
        return tab[i];
    }
} g_content[2]; // [0] projection, [1] model-view

inline void Lerp(float* o, const float* a, const float* b, uint32_t n, float t) {
    uint32_t i = 0;
#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && !defined(VGX_NO_NEON)
    const float32x4_t vt = vdupq_n_f32(t);
    for (; i + 4 <= n; i += 4) {
        const float32x4_t va = vld1q_f32(a + i), vb = vld1q_f32(b + i);
        vst1q_f32(o + i, vmlaq_f32(va, vsubq_f32(vb, va), vt));
    }
#endif
    for (; i < n; ++i) o[i] = a[i] + (b[i] - a[i]) * t;
}

// 3x3 parts of a 3x4 row-major matrix: m[r * 4 + c].
inline float Frob2(const float* m) {
    float s = 0;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) s += m[r * 4 + c] * m[r * 4 + c];
    return s;
}
// Rotation with a scale per column? Gives the column lengths.
inline bool Similarity(const float* m, float* scale) {
    float l[3];
    for (int c = 0; c < 3; ++c) {
        l[c] = std::sqrt(m[c] * m[c] + m[4 + c] * m[4 + c] + m[8 + c] * m[8 + c]);
        if (l[c] < 1e-6f) return false;
    }
    auto dot = [&](int i, int j) { return m[i] * m[j] + m[4 + i] * m[4 + j] + m[8 + i] * m[8 + j]; };
    const float tol = 0.02f;
    if (std::fabs(dot(0, 1)) > tol * l[0] * l[1] || std::fabs(dot(0, 2)) > tol * l[0] * l[2] || std::fabs(dot(1, 2)) > tol * l[1] * l[2]) return false;
    const float det = m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) + m[2] * (m[4] * m[9] - m[5] * m[8]);
    if (det <= 0) return false;
    scale[0] = l[0]; scale[1] = l[1]; scale[2] = l[2];
    return true;
}
inline void ToQuat(const float* m, const float* s, float* q) { // q = (x, y, z, w)
    const float r00 = m[0] / s[0], r01 = m[1] / s[1], r02 = m[2] / s[2];
    const float r10 = m[4] / s[0], r11 = m[5] / s[1], r12 = m[6] / s[2];
    const float r20 = m[8] / s[0], r21 = m[9] / s[1], r22 = m[10] / s[2];
    const float tr = r00 + r11 + r22;
    if (tr > 0) {
        const float k = std::sqrt(tr + 1.0f) * 2.0f;
        q[3] = 0.25f * k; q[0] = (r21 - r12) / k; q[1] = (r02 - r20) / k; q[2] = (r10 - r01) / k;
    } else if (r00 > r11 && r00 > r22) {
        const float k = std::sqrt(1.0f + r00 - r11 - r22) * 2.0f;
        q[3] = (r21 - r12) / k; q[0] = 0.25f * k; q[1] = (r01 + r10) / k; q[2] = (r02 + r20) / k;
    } else if (r11 > r22) {
        const float k = std::sqrt(1.0f + r11 - r00 - r22) * 2.0f;
        q[3] = (r02 - r20) / k; q[0] = (r01 + r10) / k; q[1] = 0.25f * k; q[2] = (r12 + r21) / k;
    } else {
        const float k = std::sqrt(1.0f + r22 - r00 - r11) * 2.0f;
        q[3] = (r10 - r01) / k; q[0] = (r02 + r20) / k; q[1] = (r12 + r21) / k; q[2] = 0.25f * k;
    }
}
// The 3x3 of `o` becomes the rotation between a and b at t (shorter way round) with blended column scales.
inline bool BlendRotation(float* o, const float* a, const float* b, float t) {
    float sa[3], sb[3], qa[4], qb[4];
    if (!Similarity(a, sa) || !Similarity(b, sb)) return false;
    ToQuat(a, sa, qa);
    ToQuat(b, sb, qb);
    float d = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3];
    const float sgn = d < 0 ? -1.0f : 1.0f;
    d *= sgn;
    // Spherical for wide angles (a normalised straight blend runs fast in the middle), straight for small ones.
    float wa = 1.0f - t, wb = t;
    if (d < 0.9995f) {
        const float th = std::acos(d), sn = std::sin(th);
        wa = std::sin((1.0f - t) * th) / sn;
        wb = std::sin(t * th) / sn;
    }
    float q[4];
    for (int i = 0; i < 4; ++i) q[i] = wa * qa[i] + wb * sgn * qb[i];
    const float n = 1.0f / std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    const float x = q[0] * n, y = q[1] * n, z = q[2] * n, w = q[3] * n;
    const float s0 = sa[0] + (sb[0] - sa[0]) * t, s1 = sa[1] + (sb[1] - sa[1]) * t, s2 = sa[2] + (sb[2] - sa[2]) * t;
    o[0] = (1 - 2 * (y * y + z * z)) * s0; o[1] = (2 * (x * y - z * w)) * s1;     o[2] = (2 * (x * z + y * w)) * s2;
    o[4] = (2 * (x * y + z * w)) * s0;     o[5] = (1 - 2 * (x * x + z * z)) * s1; o[6] = (2 * (y * z - x * w)) * s2;
    o[8] = (2 * (x * z - y * w)) * s0;     o[9] = (2 * (y * z + x * w)) * s1;     o[10] = (1 - 2 * (x * x + y * y)) * s2;
    return true;
}

// Blends one block. Returns false when this pair is not to be blended (the caller keeps the new frame's block).
bool BlendBlock(int kind, float* o, const float* a, const float* b, uint32_t n, float t, bool rigid) {
    switch (kind) {
    case UB_PROJ:
        if (a[15] != b[15] || a[11] != b[11]) return false; // perspective <-> orthographic
        Lerp(o, a, b, 16, t);
        return true;
    case UB_MV: {
        Lerp(o, a, b, 24, t);
        const float fa = Frob2(a), fb = Frob2(b);
        bool asRotation = false;
        if (rigid && fa > 1e-12f && fb > 1e-12f) {
            float dot = 0;
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) dot += a[r * 4 + c] * b[r * 4 + c];
            if (dot < g_p.rotC * std::sqrt(fa * fb)) asRotation = BlendRotation(o, a, b, t);
        }
        // Normal matrix (rows 3..5): as a rotation too, or scaled back to its length.
        const float* na = a + 12;
        const float* nb = b + 12;
        float* no = o + 12;
        const float ga = Frob2(na), gb = Frob2(nb);
        if (!(asRotation && BlendRotation(no, na, nb, t))) {
            const float go = Frob2(no);
            if (go > 1e-12f && ga > 1e-12f && gb > 1e-12f) {
                const float k = std::sqrt((ga + (gb - ga) * t) / go);
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 3; ++c) no[r * 4 + c] *= k;
            }
        }
        return true;
    }
    case UB_TEXM: {
        float worst = 0;
        for (uint32_t i = 0; i < n; ++i) {
            const float d = std::fabs(a[i] - b[i]);
            if (d > worst) worst = d;
        }
        if (!(worst <= g_p.texmMax)) return false;
        Lerp(o, a, b, n, t);
        return true;
    }
    default:
        Lerp(o, a, b, n, t);
        return true;
    }
}

inline uint64_t ContentHash(const float* p, uint32_t n) {
    const uint64_t h = HashKey(p, n * sizeof(float));
    return h ? h : 1;
}

// What the first picture of a built frame found out is kept for the others: which blocks are blended (their place in the previous frame's
// uniform arena, in the frame's, and where the result lies in the appended part of the frame's arena), and which vertices of the arena (their
// two positions). A later picture of the same frame, whatever its t, only redoes the arithmetic into the same places: no pairing lookups,
// no hashing, no comparing of the two arenas. (The decisions depend on the two frames, never on t.)
struct PlanBlk { uint32_t offA, offB, out, n; uint8_t kind; bool rigid; };
struct PlanVtx { uint32_t off; float a[3], b[3]; };
std::vector<PlanBlk> g_plan;
std::vector<PlanVtx> g_vplan;
uint64_t g_frameSerial = 0; // built frames seen by Begin()
uint64_t g_planSerial = ~0ull, g_vplanSerial = ~0ull; // the built frame (g_frameSerial) the plans are of

// Fills g_alt: the new frame's draw records with blended blocks for picture `t`.
void BuildPicture(CmdBuf& buf, float t, FrameStats& st) {
    const size_t nB = buf.draws.size();
    const double tm0 = g_timing ? TNow() : 0.0;
    g_plan.clear();
    g_alt.assign(buf.draws.begin(), buf.draws.end());
    g_map.reset(nB * 2);
    g_content[0].reset(nB);
    g_content[1].reset(nB);
    const double tm1 = g_timing ? TNow() : 0.0;
    const size_t uniA = g_prev.uni.size();
    const size_t uniStart = buf.uni.size();
    float tmp[kUbSize[UB_PAL]];
    static_assert(kUbSize[UB_PAL] >= kUbSize[UB_TEXM] && kUbSize[UB_PAL] >= kUbSize[UB_LIGHT], "tmp too small");
    static const int kinds[5] = {UB_PROJ, UB_MV, UB_TEXM, UB_PAL, UB_LIGHT};
    for (size_t i = 0; i < nB; ++i) {
        const int32_t pa = g_pairOfB[i];
        if (pa < 0) continue;
        const DrawRec& da = g_prev.draws[pa];
        const DrawRec& db = buf.draws[i];
        DrawRec& out = g_alt[i];
        const bool rigid = !Skinned(db);
        for (int kind : kinds) {
            const uint32_t offA = da.ublock[kind], offB = db.ublock[kind];
            if (offA == kNoBlock || offB == kNoBlock) continue;
            BlendMap::E& e = g_map.at(kind, offB);
            if (e.offB == offB && e.offA == offA) { out.ublock[kind] = e.out; continue; }
            const uint32_t n = BlockLen(kind, db);
            if (n == 0 || n != BlockLen(kind, da) || offA + n > uniA || offB + n > uniStart) continue;
            const float* a = g_prev.uni.data() + offA;
            const float* b = buf.uni.data() + offB;
            uint32_t res = offB;
            if (SameWords(a, b, n)) {
                // unchanged: the new frame's block as it is
            } else if (BlendBlock(kind, tmp, a, b, n, t, rigid)) {
                res = static_cast<uint32_t>(buf.uni.size());
                buf.uni.insert(buf.uni.end(), tmp, tmp + n);
                g_plan.push_back(PlanBlk{offA, offB, res, n, static_cast<uint8_t>(kind), rigid});
                ++st.blocksBlended[kind];
            } else {
                ++st.blocksHeld;
            }
            if (e.offB == kNoBlock) { e.offB = offB; e.offA = offA; e.out = res; }
            out.ublock[kind] = res;
            if (kind == UB_PROJ || kind == UB_MV) {
                ContentMap::E& c = g_content[kind == UB_MV].at(ContentHash(buf.uni.data() + offB, n));
                if (!c.h) { c.h = ContentHash(buf.uni.data() + offB, n); c.out = res; }
            }
        }
    }
    const double tm2 = g_timing ? TNow() : 0.0;
    // Draws without a partner: the camera of a paired draw, if they use exactly its matrices.
    for (size_t i = 0; i < nB; ++i) {
        if (g_pairOfB[i] >= 0) continue;
        const DrawRec& db = buf.draws[i];
        DrawRec& out = g_alt[i];
        if (Skinned(db)) continue; // (a skinned mesh carries its own pose: all or nothing)
        for (int k = 0; k < 2; ++k) {
            const int kind = k ? UB_MV : UB_PROJ;
            const uint32_t offB = db.ublock[kind];
            if (offB == kNoBlock) continue;
            const uint32_t n = kind == UB_MV ? 24u : 16u;
            if (offB + n > uniStart) continue;
            const ContentMap::E& c = g_content[k].at(ContentHash(buf.uni.data() + offB, n));
            if (c.h) out.ublock[kind] = c.out;
        }
    }
    st.floatsAppended += static_cast<uint32_t>(buf.uni.size() - uniStart);
    g_planSerial = g_frameSerial;
    if (g_timing) { const double tm3 = TNow(); g_tm.buildReset += tm1 - tm0; g_tm.buildPaired += tm2 - tm1; g_tm.buildUnpaired += tm3 - tm2; }
}

// A later picture of the frame: the blocks of the plan again, for another t, in place.
void ApplyPlan(CmdBuf& buf, float t) {
    float* uni = buf.uni.data();
    const float* pu = g_prev.uni.data();
    for (const PlanBlk& e : g_plan) BlendBlock(e.kind, uni + e.out, pu + e.offA, uni + e.offB, e.n, t, e.rigid);
}

// Which picture each draw goes into: 0 the EFB, else a number for the texture (and atlas
// cell) the offscreen pass it belongs to ends in. The game renders half of its small
// pictures (crowd figures, one picture per player) in even frames and the other half in odd
// ones, with the same meshes: a draw of such a pass may only be paired with the draw that made
// the SAME picture in the previous frame, and if there is none it is drawn as it is.
std::vector<uint32_t> g_target;
void DrawTargets(const CmdBuf& buf) {
    g_target.assign(buf.draws.size(), 0);
    bool inPass = false;
    static std::vector<uint32_t> run;
    run.clear();
    for (const Cmd& c : buf.cmds) {
        if (c.type == CMD_PASS_BEGIN) {
            inPass = true;
            run.clear();
        } else if (c.type == CMD_DRAW) {
            if (inPass) run.push_back(c.arg);
        } else if (c.type == CMD_COPY_TEX) {
            const CopyTexCmd& t = buf.copyTex[c.arg];
            if (t.srcTex) continue; // (a copy out of an atlas: ends no pass)
            if (inPass) {
                const uint32_t id = 1u + t.texId * 4099u + static_cast<uint32_t>(t.dstX) * 31u + static_cast<uint32_t>(t.dstY) * 131071u +
                                    static_cast<uint32_t>(t.src.x) * 7u + static_cast<uint32_t>(t.src.y) * 8191u;
                for (uint32_t d : run) g_target[d] = id ? id : 1u;
            }
            inPass = false;
            run.clear();
        }
    }
}

// The state of the EFB a frame leaves behind, as far as it can be told from the frame alone:
// its last command that touches the EFB is a display copy whose clear (executed, or left
// out by the front end because the rectangle held those values already) covers, in all three
// planes, everything the frame reads or writes. The front end leaves out clears and turns copies into fills
// from what it knows the EFB to hold when a frame BEGINS, i.e. what the frame before left;
// the second execution of a frame begins with what the frame itself left. The two are the
// same when both frames end like this with the same values.
struct EndState {
    bool uniform = false;
    float rgba[4] = {}, z = 0;
    uint16_t w = 0, h = 0;
    bool operator==(const EndState& o) const {
        return uniform && o.uniform && w == o.w && h == o.h && z == o.z && std::memcmp(rgba, o.rgba, sizeof rgba) == 0;
    }
};
uint16_t g_efbW = 640, g_efbH = 528;
EndState EndStateOf(const CmdBuf& buf) {
    EndState e;
    for (const Cmd& c : buf.cmds)
        if (c.type == CMD_SET_TARGET) { g_efbW = buf.targets[c.arg].efbW; g_efbH = buf.targets[c.arg].efbH; }
    for (size_t i = buf.cmds.size(); i-- > 0;) {
        const Cmd& c = buf.cmds[i];
        if (c.type == CMD_DRAW || c.type == CMD_COPY_TEX || c.type == CMD_PASS_BEGIN || c.type == CMD_SET_TARGET) {
            if (g_p.log >= 3) std::fprintf(stderr, "[interp]   frame %llu: command of type %d after the last display copy\n", static_cast<unsigned long long>(buf.frame), c.type);
            return e;
        }
        if (c.type != CMD_COPY_DISP) continue;
        const CopyDispCmd& d = buf.copyDisp[c.arg];
        if (g_p.log >= 3)
            std::fprintf(stderr, "[interp]   frame %llu ends with a display copy of (%d,%d,%d,%d) of %ux%u, clear %d (left out %d) planes c%d a%d z%d to (%.3f,%.3f,%.3f,%.3f) z %.4f\n",
                         static_cast<unsigned long long>(buf.frame), d.src.x, d.src.y, d.src.w, d.src.h, g_efbW, g_efbH, d.clear.enable, d.clearElided, d.clear.color,
                         d.clear.alpha, d.clear.depth, d.clear.rgba[0], d.clear.rgba[1], d.clear.rgba[2], d.clear.rgba[3], d.clear.z);
        if (!(d.clear.enable || d.clearElided) || !d.clear.color || !d.clear.alpha || !d.clear.depth) return e;
        // Everything the frame touches on the EFB lies inside the cleared rectangle? (The game
        // uses 640 x 448 of a 640 x 528 target; what is outside is never read or written.)
        int x0 = d.src.x, y0 = d.src.y, x1 = d.src.x + d.src.w, y1 = d.src.y + d.src.h;
        auto inside = [&](int x, int y, int w, int h) { return w <= 0 || h <= 0 || (x >= x0 && y >= y0 && x + w <= x1 && y + h <= y1); };
        for (const DrawRec& r : buf.draws)
            if (!inside(r.rs.scX, r.rs.scY, r.rs.scW, r.rs.scH)) return e;
        for (const CopyTexCmd& t : buf.copyTex)
            if (!t.srcTex && !inside(t.src.x, t.src.y, t.src.w, t.src.h)) return e;
        for (const PassCmd& ps : buf.passes)
            if (!inside(ps.rect.x, ps.rect.y, ps.rect.w, ps.rect.h)) return e;
        for (const CopyDispCmd& o : buf.copyDisp)
            if (!inside(o.src.x, o.src.y, o.src.w, o.src.h)) return e;
        e.uniform = true;
        std::memcpy(e.rgba, d.clear.rgba, sizeof e.rgba);
        e.z = d.clear.z;
        e.w = static_cast<uint16_t>(d.src.w);
        e.h = static_cast<uint16_t>(d.src.h);
        return e;
    }
    return e;
}
EndState g_prevEnd;

// Arena draws (vertices decoded per frame: impostor quads of far players, shadow quads,
// markers, particles, HUD, small immediate geometry) mostly carry their motion in the
// VERTICES, computed by the game on the CPU, under the camera's matrix or none. A paired
// arena draw with the same vertex count gets, for the in-between picture, vertex positions
// between the two frames', vertex by vertex, in a copy of the arena (g_altVtx). A vertex is
// left as the new frame has it when its texture coordinates differ (another sprite of an
// atlas now sits at this place of the record: records are merged runs of quads, and the
// game re-sorts them) or when it moved further than something that moves can (a particle
// slot that was given to a new particle).
std::vector<uint8_t, NoInitAlloc<uint8_t>> g_altVtx;
uint64_t g_altVtxSerial = ~0ull; // the built frame g_altVtx is a copy of the arena of
bool BlendArena(const CmdBuf& buf, float t, FrameStats& st) {
    g_vplan.clear();
    g_vplanSerial = g_frameSerial;
    if (!g_p.verts) return false;
    const View va = ViewOfPrev(), vb = ViewOf(buf);
    bool made = false;
    for (size_t i = 0; i < buf.draws.size(); ++i) {
        const int32_t pa = g_pairOfB[i];
        if (pa < 0) continue;
        const DrawRec& db = buf.draws[i];
        const DrawRec& da = g_prev.draws[pa];
        if (db.vbuf || da.vbuf || !db.vtxCount || db.vtxCount != da.vtxCount || db.layoutMask != da.layoutMask) continue;
        const VtxLayout L = MakeLayout(db.layoutMask);
        const uint8_t* pA = ArenaVerts(da, va, L.stride);
        const uint8_t* pB = ArenaVerts(db, vb, L.stride);
        if (!pA || !pB) continue;
        const size_t bytes = static_cast<size_t>(db.vtxCount) * L.stride;
        if (SameWords(pA, pB, static_cast<uint32_t>(bytes / 4))) continue; // (the usual case: the same model-space vertices sent again)
        if (da.ublock[UB_MV] == kNoBlock || db.ublock[UB_MV] == kNoBlock || da.ublock[UB_MV] + 12 > va.uniN || db.ublock[UB_MV] + 12 > vb.uniN) continue;
        const float* ma = va.uni + da.ublock[UB_MV];
        const float* mb = vb.uni + db.ublock[UB_MV];
        bool ortho = false;
        float px = 1, py = 1;
        if (db.ublock[UB_PROJ] != kNoBlock && db.ublock[UB_PROJ] + 16 <= vb.uniN) {
            const float* pr = vb.uni + db.ublock[UB_PROJ];
            ortho = pr[15] == 1.0f;
            px = pr[0]; py = pr[5];
        }
        const bool skinned = Skinned(db);
        // texture coordinates: one run of bytes in the layout
        uint32_t tcOff = 0, tcLen = 0;
        for (int k = 0; k < 8; ++k)
            if (L.mask & (VL_TEX0 << k)) { if (!tcLen) tcOff = L.offTex[k]; tcLen = L.offTex[k] + 8u - tcOff; }
        if (!made) {
            // The copy of the arena is made once per built frame: the vertices that get in-between positions are the same in every picture of the frame
            // (they depend on the two frames, not on t), each picture overwrites exactly those, and the others stay as the frame has them.
            if (g_altVtxSerial != g_frameSerial || g_altVtx.size() != buf.vtx.size()) {
                g_altVtx.resize(buf.vtx.size());
                std::memcpy(g_altVtx.data(), buf.vtx.data(), buf.vtx.size());
                g_altVtxSerial = g_frameSerial;
            }
            made = true;
        }
        uint8_t* out = g_altVtx.data() + (pB - buf.vtx.data());
        for (uint32_t v = 0; v < db.vtxCount; ++v) {
            const uint8_t* qa = pA + static_cast<size_t>(v) * L.stride;
            const uint8_t* qb = pB + static_cast<size_t>(v) * L.stride;
            float a[3], b[3];
            std::memcpy(a, qa, 12);
            std::memcpy(b, qb, 12);
            if (a[0] == b[0] && a[1] == b[1] && a[2] == b[2]) continue;
            if (tcLen && std::memcmp(qa + tcOff, qb + tcOff, tcLen) != 0) { ++st.vertsHeld; continue; }
            if (!skinned) {
                const float ea[3] = {ma[0] * a[0] + ma[1] * a[1] + ma[2] * a[2] + ma[3], ma[4] * a[0] + ma[5] * a[1] + ma[6] * a[2] + ma[7],
                                     ma[8] * a[0] + ma[9] * a[1] + ma[10] * a[2] + ma[11]};
                const float eb[3] = {mb[0] * b[0] + mb[1] * b[1] + mb[2] * b[2] + mb[3], mb[4] * b[0] + mb[5] * b[1] + mb[6] * b[2] + mb[7],
                                     mb[8] * b[0] + mb[9] * b[1] + mb[10] * b[2] + mb[11]};
                bool jump;
                if (ortho) {
                    const float dx = px * (eb[0] - ea[0]), dy = py * (eb[1] - ea[1]);
                    jump = dx * dx + dy * dy > g_p.jumpNdc * g_p.jumpNdc;
                } else {
                    jump = PointJump(ea, eb);
                }
                if (jump) { ++st.vertsHeld; continue; }
            }
            const float o[3] = {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t};
            std::memcpy(out + static_cast<size_t>(v) * L.stride, o, 12);
            ++st.vertsBlended;
            {
                PlanVtx pv;
                pv.off = static_cast<uint32_t>((pB - buf.vtx.data()) + static_cast<size_t>(v) * L.stride);
                std::memcpy(pv.a, a, 12);
                std::memcpy(pv.b, b, 12);
                g_vplan.push_back(pv);
            }
        }
    }
    return made;
}

bool ApplyVertexPlan(float t, FrameStats& st) {
    if (g_vplan.empty()) return false;
    uint8_t* base = g_altVtx.data();
    for (const PlanVtx& v : g_vplan) {
        const float o[3] = {v.a[0] + (v.b[0] - v.a[0]) * t, v.a[1] + (v.b[1] - v.a[1]) * t, v.a[2] + (v.b[2] - v.a[2]) * t};
        std::memcpy(base + v.off, o, 12);
    }
    st.vertsBlended += static_cast<uint32_t>(g_vplan.size());
    return true;
}

// Can the buffer be executed twice? Not when it replaces or frees something its own earlier draws use.
bool Replayable(const CmdBuf& buf) {
    bool any = false;
    for (const Cmd& c : buf.cmds)
        if (c.type == CMD_TEX_UPLOAD || c.type == CMD_TEX_FREE || c.type == CMD_BUF_UPLOAD || c.type == CMD_BUF_FREE) { any = true; break; }
    if (!any) return true;
    static std::vector<uint8_t> texUsed, bufUsed;
    texUsed.clear();
    bufUsed.clear();
    auto mark = [](std::vector<uint8_t>& v, uint32_t id) { if (v.size() <= id) v.resize(static_cast<size_t>(id) + 64, 0); v[id] = 1; };
    auto used = [](const std::vector<uint8_t>& v, uint32_t id) { return id < v.size() && v[id]; };
    for (const Cmd& c : buf.cmds) {
        switch (c.type) {
        case CMD_DRAW: {
            const DrawRec& d = buf.draws[c.arg];
            if (d.vbuf) mark(bufUsed, d.vbuf);
            for (int m = 0; m < 8; ++m)
                if ((d.texMask & (1u << m)) && d.tex[m].texId) mark(texUsed, d.tex[m].texId);
            break;
        }
        case CMD_TEX_UPLOAD: if (used(texUsed, buf.uploads[c.arg].texId)) return false; break;
        case CMD_TEX_FREE: if (used(texUsed, c.arg)) return false; break;
        case CMD_BUF_UPLOAD: if (used(bufUsed, buf.bufUploads[c.arg].bufId)) return false; break;
        case CMD_BUF_FREE: if (used(bufUsed, c.arg)) return false; break;
        default: break;
        }
    }
    return true;
}

bool g_resetPending = false;
uint32_t g_phase = 0; // mode 32: 0 = this built frame is shown through one in-between only

// The frame being shown (between Begin() and End()).
struct Cur {
    FrameStats st;
    bool streamAlt = false; // the back end's stream buffer holds a blended arena of this frame
} g_cur;

void Keep(CmdBuf& buf) {
    g_prev.valid = true;
    g_prev.frame = buf.frame;
    g_prev.draws.swap(buf.draws);
    g_prev.uni.swap(buf.uni);
    g_prev.vtx.swap(buf.vtx);
    g_prev.vtxChunks.swap(buf.vtxChunks);
    g_prev.key.swap(g_keyB);
}

void LogFrame(const FrameStats& st) {
    static const char* const why[] = {"", "first frame", "replaces a resource it uses", "too few draws paired", "cut", "reset", "frame gap",
                                      "does not leave the EFB as the frame before did"};
    std::fprintf(stderr, "[interp] frame %llu: %u draws (%u static), paired %u (%u static), jumps %u, groups with several %u (reordered %u), blended proj %u mv %u texm %u pal %u light %u, held %u, +%u floats, arena vertices blended %u held %u, %s%s\n",
                 static_cast<unsigned long long>(st.frame), st.draws, st.staticDraws, st.paired, st.pairedStatic, st.jumps, st.groupsAmbiguous, st.groupsReordered,
                 st.blocksBlended[UB_PROJ], st.blocksBlended[UB_MV], st.blocksBlended[UB_TEXM], st.blocksBlended[UB_PAL], st.blocksBlended[UB_LIGHT], st.blocksHeld,
                 st.floatsAppended, st.vertsBlended, st.vertsHeld, st.pictures ? "in-between" : "NO in-between: ", st.pictures ? "" : why[st.reason]);
}

} // namespace

bool Enabled() {
    Init();
    return g_p.mode != 0;
}
void SetMode(int mode) {
    Init();
    g_p.mode = mode;
}
void SetPictureHook(PictureHook hook) { g_hook = hook; }
PictureHook GetPictureHook() { return g_hook; }
const FrameStats& LastStats() { return g_last; }
const Totals& GetTotals() { return g_tot; }
void PrintTiming() {
    if (!g_timing || !g_tm.begins) return;
    const double nb = static_cast<double>(g_tm.begins), np = static_cast<double>(g_tm.blends ? g_tm.blends : 1);
    std::fprintf(stderr, "[interp] timing (wall microseconds on this machine): per built frame Begin %.0f; per blended picture: reset+copy of the draw records %.0f, paired draws' blocks %.0f, unpaired draws %.0f, arena %.0f, back end %.0f\n",
                 g_tm.begin / nb, g_tm.buildReset / np, g_tm.buildPaired / np, g_tm.buildUnpaired / np, g_tm.arena / np, g_tm.exec / np);
}
void Reset() { g_resetPending = true; }

// ---- one built frame in steps (Submit() below uses them in the order of the old Submit(); the presenter in vgx_present.cpp
// uses them over several display ticks)
bool Begin(CmdBuf& buf, bool wantPairs) {
    Init();
    const double tb0 = g_timing ? TNow() : 0.0;
    NoteShaders(buf);
    FrameStats& st = g_cur.st;
    st = FrameStats();
    st.frame = buf.frame;
    st.draws = static_cast<uint32_t>(buf.draws.size());
    g_cur.streamAlt = false;
    ++g_frameSerial;
    DrawTargets(buf);
    g_keyB.resize(buf.draws.size());
    for (size_t i = 0; i < buf.draws.size(); ++i) {
        g_keyB[i] = DrawKey(buf.draws[i], g_target[i]);
        if (buf.draws[i].vbuf) ++st.staticDraws;
    }
    const EndState end = EndStateOf(buf);

    uint8_t reason = kReasonNone;
    if (!g_prev.valid) reason = kReasonFirst;
    else if (g_resetPending) reason = kReasonReset;
    else if (buf.frame != g_prev.frame + 1) reason = kReasonGap;
    else if (!(end == g_prevEnd)) reason = kReasonEfbState;
    else if (!Replayable(buf)) reason = kReasonNotReplayable;
    if (!reason && !wantPairs) reason = kReasonReset; // (not an error: nobody asked for pairs)
    g_prevEnd = end;
    g_resetPending = false;
    if (!reason) {
        BuildGroups(buf);
        PairDraws(buf, st);
        // Jumps, and the two tests on the frame as a whole.
        const View va = ViewOfPrev(), vb = ViewOf(buf);
        uint32_t rigidStatic = 0, rigidStaticJump = 0;
        for (size_t i = 0; i < buf.draws.size(); ++i) {
            const int32_t pa = g_pairOfB[i];
            if (pa < 0) continue;
            const DrawRec& db = buf.draws[i];
            const bool skinned = Skinned(db);
            const bool jump = skinned ? PaletteJump(g_prev.draws[pa], va, db, vb) : IsJump(AnchorOf(g_prev.draws[pa], va), AnchorOf(db, vb));
            const bool rigidSt = db.vbuf && !skinned;
            if (rigidSt) ++rigidStatic;
            if (jump) {
                ++st.jumps;
                if (rigidSt) ++rigidStaticJump;
                if (g_p.log >= 2)
                    std::fprintf(stderr, "[interp]   frame %llu draw %u (vbuf %u idx %u+%u vs %u fs %u): jump, not blended\n", static_cast<unsigned long long>(buf.frame),
                                 static_cast<unsigned>(i), db.vbuf, db.idxOffset, db.idxCount, db.vsId, db.fsId);
                g_pairOfB[i] = -2 - pa;
                continue;
            }
            ++st.paired;
            if (db.vbuf) ++st.pairedStatic;
        }
        if (g_p.log >= 2)
            for (size_t i = 0; i < buf.draws.size(); ++i)
                if (g_pairOfB[i] == -1 && buf.draws[i].vbuf)
                    std::fprintf(stderr, "[interp]   frame %llu draw %u (vbuf %u idx %u+%u vs %u fs %u): no partner\n", static_cast<unsigned long long>(buf.frame),
                                 static_cast<unsigned>(i), buf.draws[i].vbuf, buf.draws[i].idxOffset, buf.draws[i].idxCount, buf.draws[i].vsId, buf.draws[i].fsId);
        {
            // MSC_VGX_INTERP_DUMP=<frame>: every draw of that frame with what it was paired with.
            static const long dumpFrame = EnvInt("MSC_VGX_INTERP_DUMP", -1);
            if (dumpFrame >= 0 && static_cast<uint64_t>(dumpFrame) == buf.frame) {
                for (size_t i = 0; i < buf.draws.size(); ++i) {
                    const DrawRec& d = buf.draws[i];
                    const Anchor a = AnchorOf(d, vb);
                    const int32_t pr = g_pairOfB[i];
                    const int32_t pa = pr >= 0 ? pr : (pr <= -2 ? -2 - pr : -1);
                    Anchor b;
                    if (pa >= 0) b = AnchorOf(g_prev.draws[pa], va);
                    std::fprintf(stderr, "[dump] %3u target %08x vbuf %4u vtx %5u idx %5u lay %x voff %u vfirst %u ioff %u prim %u vs %3u fs %3u %s %s tex %u,%u  origin (%8.2f %8.2f %8.2f)  %s %d  was (%8.2f %8.2f %8.2f) moved %.3f\n",
                                 static_cast<unsigned>(i), g_target[i], d.vbuf, d.vtxCount, d.idxCount, d.layoutMask, d.vtxOffset, d.vtxFirst, d.idxOffset, d.prim, d.vsId, d.fsId, Skinned(d) ? "PAL" : "   ",
                                 a.ortho ? "ortho" : "persp", d.tex[0].texId, d.tex[1].texId, a.x, a.y, a.z, pr >= 0 ? "paired with" : (pr == -1 ? "NO PARTNER" : "JUMP from"), pa,
                                 b.x, b.y, b.z, pa >= 0 ? std::sqrt(Dist2(a, b)) : 0.0f);
                }
            }
        }
        if (rigidStatic >= 4 && static_cast<float>(rigidStaticJump) > g_p.cutFraction * static_cast<float>(rigidStatic)) reason = kReasonCut;
        else if (st.staticDraws >= 8 && static_cast<float>(st.pairedStatic) < g_p.minPair * static_cast<float>(st.staticDraws)) reason = kReasonFewPairs;
        else if (st.paired == 0) reason = kReasonFewPairs;
    }
    st.reason = reason;
    if (g_timing) { g_tm.begin += TNow() - tb0; ++g_tm.begins; }
    return reason == 0;
}

// An in-between picture at `t` (0 = the previous frame, 1 = this one; values above 1 extrapolate). `first`: the first execution of
// this frame (it creates what the frame creates), else a replay. `hidden`: drawn but not presented.
void Blend(CmdBuf& buf, float t, bool first, bool hidden) {
    const bool again = g_planSerial == g_frameSerial && g_vplanSerial == g_frameSerial && !g_p.noPlan; // not the first picture of this frame
    if (again) ApplyPlan(buf, t); else BuildPicture(buf, t, g_cur.st);
    const double ta0 = g_timing ? TNow() : 0.0;
    const bool alt = again ? ApplyVertexPlan(t, g_cur.st) : BlendArena(buf, t, g_cur.st);
    const double ta1 = g_timing ? TNow() : 0.0;
    backend::ExecOpts o;
    o.draws = g_alt.data();
    o.replay = !first;
    o.hidden = hidden;
    if (alt) o.vtx = g_altVtx.data();
    o.restream = !first && (alt || g_cur.streamAlt);
    g_cur.streamAlt = alt;
    backend::ExecuteEx(buf, o);
    if (g_timing) { g_tm.arena += ta1 - ta0; g_tm.exec += TNow() - ta1; ++g_tm.blends; }
    ++g_cur.st.pictures;
}

// The frame as itself. `first`: nothing of this frame has been executed yet.
void Self(CmdBuf& buf, bool first, bool hidden) {
    backend::ExecOpts o;
    o.replay = !first;
    o.restream = !first && g_cur.streamAlt;
    o.hidden = hidden;
    g_cur.streamAlt = false;
    backend::ExecuteEx(buf, o);
}

// Done with this frame: it becomes the previous one (its draw records and arenas are taken from `buf`).
void End(CmdBuf& buf) {
    const FrameStats& st = g_cur.st;
    ++g_tot.frames;
    g_tot.draws += st.draws;
    g_tot.paired += st.paired;
    g_tot.jumps += st.jumps;
    g_tot.floats += st.floatsAppended;
    if (st.pictures) ++g_tot.withInBetween;
    ++g_tot.cuts[st.reason & 7];
    g_last = st;
    if (g_p.log) LogFrame(st);
    Keep(buf);
}

const char* ReasonText(uint8_t r) {
    static const char* const why[] = {"", "first frame", "replaces a resource it uses", "too few draws paired", "cut", "reset", "frame gap",
                                      "does not leave the EFB as the frame before did"};
    return why[r & 7];
}

void Submit(CmdBuf& buf) {
    Init();
    if (g_p.mode == 0) {
        backend::Execute(buf);
        return;
    }
    if (buf.cmds.empty() || buf.cmds.back().type != CMD_PRESENT) { // not a frame (a tool's resource preamble)
        NoteShaders(buf);
        backend::Execute(buf);
        return;
    }
    // What this built frame is shown through.
    float ts[2];
    int nBlend = 1;
    bool self = true;
    if (g_p.mode == 2) {
        ts[0] = 1.0f / 3.0f; ts[1] = 2.0f / 3.0f; nBlend = 2;
    } else if (g_p.mode == 32) {
        if (g_phase == 0) { ts[0] = 2.0f / 3.0f; self = false; }
        else ts[0] = 1.0f / 3.0f;
        g_phase ^= 1u;
    } else {
        ts[0] = 0.5f;
    }
    const int slots = nBlend + (self ? 1 : 0);

    if (!Begin(buf)) {
        // Shown as it is in the first slot; the picture stays for the others.
        Self(buf, true, false);
        if (g_hook) {
            g_hook(buf.frame, 0, true, 1.0f);
            for (int s = 1; s < slots; ++s) g_hook(buf.frame, s, false, 1.0f);
        }
    } else {
        for (int k = 0; k < nBlend; ++k) {
            Blend(buf, ts[k], k == 0, false);
            if (g_hook) g_hook(buf.frame, k, true, ts[k]);
        }
        if (self) {
            Self(buf, false, false);
            if (g_hook) g_hook(buf.frame, nBlend, true, 1.0f);
        }
    }
    End(buf);
}

} // namespace vgx::interp
