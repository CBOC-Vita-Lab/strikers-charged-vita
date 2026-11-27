// ================================================================ static batches (1.3 wave 6, MSC_VGX_SBATCH)
// Included by vgx_draw.cpp (needs its g_buf, g_vsReg, g_layerCls, stats). See the switch list below.
//
// A stadium is drawn as many rigid meshes: one display list per prop, often the same list several times with another model-view matrix, each a draw
// call of its own although program, vertex layout, textures and render state are the same as the draw before it. GX has no view matrix: a model-view
// matrix is V x M_i (V the camera, M_i the object's place), so for two rigid objects MV_A^-1 x MV_i = M_A^-1 x M_i does not depend on the camera.
// A batch ("group") is a material (shaders, layout, textures, render state, class / layer) and the objects seen with it; each object's vertices are
// stored once, moved into the group's frame (the frame of an object A: p' = R_i p with R_i = M_A^-1 M_i, normals n' = Q_i n with Q_i = N_A^-1 N_i), in
// one vertex / index buffer per 65535 vertices. A run of adjacent records of that material in a frame becomes one record per stretch of objects that
// are consecutive in the group's buffer, drawn with the group's matrix MVrec = MV_i x R_i^-1 (the same for every member in a frame).
// What makes it safe:
//   - every member, every frame: MVrec x R_i must equal its own model-view matrix within MSC_VGX_SBATCH_TOL (relative, default 1e-5: at the
//     member's bounds the eye-space point moves by at most tol x (its distance + size), about 0.01 pixel), its normal matrix within 1e-4, and every
//     texture matrix of a position / normal texture generation likewise; a member that fails (it moved: a goal net, machinery, an animated prop) is
//     drawn as its own record that frame, as before, and the stretch is cut there. Nothing is drawn that the frame did not draw: only the members the
//     frame's own records name (static culling, MSC_VGX_CULL, has already dropped the off-screen ones), so no more off-screen geometry than before;
//   - draw order is kept: a stretch is members that were adjacent in the frame AND consecutive in the buffer, in that order;
//   - every other uniform block (projection, lights, TEV, post matrices, texture-coordinate matrices) must be the same in the run;
//   - an object joins the buffer only after two frames in which its relative matrix was the same (moving objects never join);
//   - no palette, no GPU skin, no matrix index per vertex, no Z-freeze, triangles only, background classes only (stadium, crowd, sky, far).
// Pixels: not bit-exact (the eye-space position is MVrec x (R_i p) instead of MV_i x p: float rounding, bounded by the tolerance above); the owner's
// tolerance (1-2 levels / 1-2 pixels at edges) is far above it. Proof: picture comparisons (pics.sh) and MSC_VGX_SBATCH_CHECK.
//   MSC_VGX_SBATCH=0|1          off / on
//   MSC_VGX_SBATCH_MB=<n>       (default 6) cap of the decoded vertices kept in the C heap for building batches (vgx_dlcache.cpp)
//   MSC_VGX_SBATCH_BUFMB=<n>    (default 8) cap of the batch buffers (GPU memory)
//   MSC_VGX_SBATCH_TOL=<f>      (default 1e-5) relative tolerance of the matrix checks
//   MSC_VGX_SBATCH_STATS=1      one [sbatch] line every 600 frames
//   MSC_VGX_SBATCH_CHECK=1      diagnostic: each batched record's members are compared with what they would have drawn (max eye-space error)
namespace {
long SbMode() {
    const long m = static_cast<long>(VGX_KNOB(EnvInt("MSC_VGX_SBATCH", 0)));
    return m;
}
bool SbStatsOn() {
    const bool on = static_cast<bool>(VGX_KNOB(EnvInt("MSC_VGX_SBATCH_STATS", 0) != 0));
    return on;
}
bool SbCheckOn() {
    const bool on = static_cast<bool>(VGX_KNOB(EnvInt("MSC_VGX_SBATCH_CHECK", 0) != 0));
    return on;
}
double SbTol() {
    static double t = -1;
    if (t < 0) {
        const char* v = std::getenv("MSC_VGX_SBATCH_TOL");
        t = (v && *v) ? std::atof(v) : 1e-5;
        if (!(t > 0)) t = 1e-5;
    }
    return t;
}
uint64_t SbBufCap() {
    const uint64_t b = static_cast<uint64_t>(VGX_KNOB(static_cast<uint64_t>(EnvInt("MSC_VGX_SBATCH_BUFMB", 8)) << 20));
    return b;
}

// ---- 3x4 affine matrices (row-major, implied last row 0 0 0 1) and 3x3, in double
using M34 = std::array<double, 12>;
using M33 = std::array<double, 9>;
M34 Mul34(const M34& a, const M34& b) {
    M34 c;
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 4; ++k)
            c[r * 4 + k] = a[r * 4] * b[k] + a[r * 4 + 1] * b[4 + k] + a[r * 4 + 2] * b[8 + k] + (k == 3 ? a[r * 4 + 3] : 0.0);
    return c;
}
M33 Mul33(const M33& a, const M33& b) {
    M33 c;
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k) c[r * 3 + k] = a[r * 3] * b[k] + a[r * 3 + 1] * b[3 + k] + a[r * 3 + 2] * b[6 + k];
    return c;
}
bool Inv33(const M33& m, M33& o) {
    const double a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5], g2 = m[6], h = m[7], i = m[8];
    const double A = e * i - f * h, B = -(d * i - f * g2), C = d * h - e * g2;
    const double det = a * A + b * B + c * C;
    double mx = 0;
    for (double x : m) mx = std::max(mx, std::fabs(x));
    if (!(std::fabs(det) > 1e-12 * mx * mx * mx) || !std::isfinite(det)) return false;
    const double id = 1.0 / det;
    o = {A * id, -(b * i - c * h) * id, (b * f - c * e) * id, B * id, (a * i - c * g2) * id, -(a * f - c * d) * id, C * id, -(a * h - b * g2) * id, (a * e - b * d) * id};
    return true;
}
bool Inv34(const M34& m, M34& o) {
    M33 l = {m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]}, li;
    if (!Inv33(l, li)) return false;
    for (int r = 0; r < 3; ++r) {
        o[r * 4] = li[r * 3]; o[r * 4 + 1] = li[r * 3 + 1]; o[r * 4 + 2] = li[r * 3 + 2];
        o[r * 4 + 3] = -(li[r * 3] * m[3] + li[r * 3 + 1] * m[7] + li[r * 3 + 2] * m[11]);
    }
    return true;
}
M34 Load34(const float* f) { M34 m; for (int i = 0; i < 12; ++i) m[i] = f[i]; return m; }
M33 LoadN(const float* f) { return {f[0], f[1], f[2], f[4], f[5], f[6], f[8], f[9], f[10]}; } // (rows of vec4, xyz used)
bool Finite(const float* f, int n) { for (int i = 0; i < n; ++i) if (!std::isfinite(f[i])) return false; return true; }
// |a - b| at the corners of a box of half-extent ext around the origin plus its translation, against tol x (translation + size) of b
bool Near34(const M34& a, const float* b, const float* ext, int rows, double tol) {
    double err = 0, scale = 0;
    for (int r = 0; r < rows; ++r) {
        double e = std::fabs(a[r * 4 + 3] - b[r * 4 + 3]), s = std::fabs(b[r * 4 + 3]);
        for (int k = 0; k < 3; ++k) { e += std::fabs(a[r * 4 + k] - b[r * 4 + k]) * ext[k]; s += std::fabs(b[r * 4 + k]) * ext[k]; }
        err = std::max(err, e);
        scale = std::max(scale, s);
    }
    return err <= tol * scale;
}
bool Near33(const M33& a, const float* bRows, int rows, double tol) { // (bRows: rows of vec4, xyz)
    double err = 0, scale = 0;
    for (int r = 0; r < rows; ++r)
        for (int k = 0; k < 3; ++k) { err = std::max(err, std::fabs(a[r * 3 + k] - bRows[r * 4 + k])); scale = std::max(scale, std::fabs(static_cast<double>(bRows[r * 4 + k]))); }
    return err <= tol * scale;
}

// ---- what EmitStaticDraw notes about each static record that may join (this frame)
struct SbCand {
    uint32_t drawIdx = 0;
    uint32_t bufId = 0, serial = 0, byteOff = 0, count = 0, idxCount = 0;
    uint8_t gxPrim = 0;
    float ext[3] = {};   // largest |coordinate| of the stored positions per axis (from the cache's bounds)
    float box[6] = {};
    uint64_t mkey = 0;   // SbMaterialKey of its record
};
std::vector<SbCand> g_sbCand;
std::vector<int32_t> g_sbCandOf; // by draw index: index into g_sbCand, -1

// ---- a group: one material, the objects seen with it, their batch buffers
struct SbMember {
    uint32_t bufId, serial, byteOff, count, idxCount;
    uint8_t gxPrim;
    M34 R, Ri;           // into / out of the group's frame
    M33 Q, Qi;           // the same for normals
    float ext[3], box[6]; // half-extent of the stored positions; the box of the moved positions (group frame)
    uint8_t sightings = 1;
    bool admitted = false;
    uint64_t lastFrame = 0;
    // in the buffer (admitted, built):
    int32_t chunk = -1;
    uint32_t vFirst = 0, iFirst = 0;
    uint32_t bpos = 0;   // position in buffer order
    uint32_t lastPos = 0; // position in the latest run that held it (buffer order follows it)
};
struct SbChunk { uint32_t bufId = 0, vtx = 0, idx = 0, bytes = 0; };
struct SbGroup {
    uint64_t key = 0;
    uint32_t layoutMask = 0;
    uint8_t posTg = 0, nrmTg = 0, tgRows[8] = {}; // texture generations from the position / the normal, and how many matrix rows each reads
    std::vector<SbMember> m;
    std::vector<SbChunk> chunks;
    std::unordered_multimap<uint64_t, uint32_t> byGeo;
    bool dirty = false, dead = false;
    uint64_t lastUse = 0;
    uint32_t bytes = 0;
};
struct SbState {
    std::vector<SbGroup*> groups;
    std::unordered_multimap<uint64_t, SbGroup*> byKey;
    uint64_t bufBytes = 0;
    uint64_t lastSweep = 0;
    // statistics (since the last line)
    uint64_t frames = 0, runs = 0, runRecs = 0, recsOut = 0, saved = 0, unassigned = 0, fresh = 0, admitted = 0, rebuilds = 0, rebuildBytes = 0, refusedCap = 0;
    uint64_t checked = 0, checkBad = 0;
    double checkMax = 0;
    uint64_t byCls[CLS_COUNT][2] = {};
} g_sb;

uint64_t SbGeoKey(uint32_t bufId, uint32_t serial, uint32_t byteOff, uint32_t count, uint32_t idxCount) {
    const uint32_t w[5] = {bufId, serial, byteOff, count, idxCount};
    return XXH3_64bits(w, sizeof w);
}
uint64_t SbMaterialKey(const DrawRec& d) {
    uint64_t h = XXH3_64bits_withSeed(&d.layoutMask, 4, 0x5BA7C4ull);
    const uint32_t w[6] = {d.vsId, d.fsId, d.prim, d.texMask, d.cls, d.layer};
    h = XXH3_64bits_withSeed(w, sizeof w, h);
    h = XXH3_64bits_withSeed(d.tex, sizeof d.tex, h);
    const RenderState& r = d.rs;
    const uint8_t s[11] = {r.blend, r.blendSrc, r.blendDst, r.blendEq, r.depthTest, r.depthFunc, r.depthWrite, r.cull, r.colorWrite, r.alphaWrite, r.dstAlphaPass};
    return XXH3_64bits_withSeed(s, sizeof s, h);
}
bool SameBlock(const DrawRec& a, const DrawRec& b, int k, uint32_t n) {
    if (a.ublock[k] == b.ublock[k]) return true;
    if (a.ublock[k] == kNoBlock || b.ublock[k] == kNoBlock) return false;
    if (a.ublock[k] + n > g_buf.uni.size() || b.ublock[k] + n > g_buf.uni.size()) return false;
    return std::memcmp(g_buf.uni.data() + a.ublock[k], g_buf.uni.data() + b.ublock[k], n * sizeof(float)) == 0;
}
// The texture generations of a vertex key: which read the position / the normal through a texture matrix (0: one that cannot be batched)
bool SbTexGens(const VsKey& k, uint8_t& posTg, uint8_t& nrmTg, uint8_t* rows) {
    posTg = nrmTg = 0;
    for (int i = 0; i < k.numTexGens; ++i) {
        const VsKey::TexGen& t = k.tg[i];
        rows[i] = t.proj ? 3 : 2;
        if (t.type >= 2) continue;
        if (t.perVtxMtx || t.type == 1) return false;
        if (t.src == 0 || t.src == 1) {
            if (!t.inputAbc1) return false;
            (t.src == 0 ? posTg : nrmTg) |= static_cast<uint8_t>(1u << i);
        } else if (t.src == 3 || t.src == 4) {
            return false;
        }
    }
    return true;
}
// Same everything but the model-view block and the texture matrices of position / normal texture generations (those are checked by relation)
bool SbSameRun(const DrawRec& p, const DrawRec& d, const SbGroup& G, const VsKey& k) {
    if (p.vsId != d.vsId || p.fsId != d.fsId || p.prim != d.prim || p.layoutMask != d.layoutMask || p.texMask != d.texMask || p.cls != d.cls || p.layer != d.layer ||
        p.fx != d.fx || p.skinBlock != d.skinBlock)
        return false;
    if (std::memcmp(&p.rs, &d.rs, sizeof p.rs) != 0 || std::memcmp(p.tex, d.tex, sizeof p.tex) != 0) return false;
    if (!SameBlock(p, d, UB_PROJ, kUbSize[UB_PROJ]) || !SameBlock(p, d, UB_LIGHT, kUbSize[UB_LIGHT]) || !SameBlock(p, d, UB_TEV, kUbSize[UB_TEV])) return false;
    // (the palette block is not compared: the caller's key has no palette position and SbTexGens refuses palette texture matrices, so the vertex
    // shader does not read u_pal; the front end leaves the last palette draw's offset in g_blockOff[UB_PAL])
    if (p.ublock[UB_TEXM] == d.ublock[UB_TEXM]) return true;
    if (p.ublock[UB_TEXM] == kNoBlock || d.ublock[UB_TEXM] == kNoBlock) return false;
    // texture matrices: rows of the other generations equal, post matrices equal (packed layout: see BuildMtx)
    const float* a = g_buf.uni.data() + p.ublock[UB_TEXM];
    const float* b = g_buf.uni.data() + d.ublock[UB_TEXM];
    for (int i = 0; i < k.numTexGens; ++i)
        if (!(((G.posTg | G.nrmTg) >> i) & 1u) && std::memcmp(a + i * 12, b + i * 12, 12 * sizeof(float)) != 0) return false;
    bool anyPost = false;
    for (int i = 0; i < k.numTexGens; ++i) anyPost |= k.tg[i].type < 2 && k.tg[i].post != 0;
    if (anyPost && std::memcmp(a + 96, b + 96, static_cast<size_t>(k.numTexGens) * 12 * sizeof(float)) != 0) return false;
    return true;
}

// The run's matrices in the group's frame
struct SbFrameMats {
    M34 mv;
    M33 n;
    M34 tm[8];
    bool ok = false;
};
const float* MvOf(const DrawRec& d) { return g_buf.uni.data() + d.ublock[UB_MV]; }
const float* TmOf(const DrawRec& d) { return d.ublock[UB_TEXM] == kNoBlock ? nullptr : g_buf.uni.data() + d.ublock[UB_TEXM]; }
// From member j drawn by record d: the group frame's matrices
SbFrameMats SbMatsFrom(const SbGroup& G, const SbMember& j, const DrawRec& d) {
    SbFrameMats f;
    const float* mv = MvOf(d);
    f.mv = Mul34(Load34(mv), j.Ri);
    f.n = Mul33(LoadN(mv + 12), j.Qi);
    const float* tm = TmOf(d);
    for (int i = 0; i < 8; ++i) {
        if (!(((G.posTg | G.nrmTg) >> i) & 1u)) continue;
        if (!tm) return f;
        const M34 t = Load34(tm + i * 12);
        if ((G.posTg >> i) & 1u) f.tm[i] = Mul34(t, j.Ri);
        else {
            const M33 l = {t[0], t[1], t[2], t[4], t[5], t[6], t[8], t[9], t[10]};
            const M33 q = Mul33(l, j.Qi);
            f.tm[i] = {q[0], q[1], q[2], t[3], q[3], q[4], q[5], t[7], q[6], q[7], q[8], t[11]};
        }
    }
    f.ok = true;
    return f;
}
// Does member j drawn with the frame matrices f give record d's matrices (within the tolerance)?
bool SbFits(const SbGroup& G, const SbMember& j, const SbFrameMats& f, const DrawRec& d) {
    const double tol = SbTol();
    const float* mv = MvOf(d);
    if (!Near34(Mul34(f.mv, j.R), mv, j.ext, 3, tol)) return false;
    if ((G.layoutMask & VL_NRM) && !Near33(Mul33(f.n, j.Q), mv + 12, 3, tol * 10)) return false;
    const float* tm = TmOf(d);
    for (int i = 0; i < 8; ++i) {
        if (!(((G.posTg | G.nrmTg) >> i) & 1u)) continue;
        if (!tm) return false;
        if ((G.posTg >> i) & 1u) {
            if (!Near34(Mul34(f.tm[i], j.R), tm + i * 12, j.ext, G.tgRows[i], tol)) return false;
        } else {
            const M33 l = {f.tm[i][0], f.tm[i][1], f.tm[i][2], f.tm[i][4], f.tm[i][5], f.tm[i][6], f.tm[i][8], f.tm[i][9], f.tm[i][10]};
            const M33 q = Mul33(l, j.Q);
            const M34 t = {q[0], q[1], q[2], f.tm[i][3], q[3], q[4], q[5], f.tm[i][7], q[6], q[7], q[8], f.tm[i][11]};
            const float one[3] = {1, 1, 1};
            if (!Near34(t, tm + i * 12, one, G.tgRows[i], tol * 10)) return false;
        }
    }
    return true;
}

std::vector<uint32_t> g_sbFreeLater; // buffer ids freed while SbFrame rebuilds the command list (released after it, at the end of the frame)
bool g_sbInFrame = false;
void SbFreeChunks(SbGroup& G) {
    for (SbChunk& c : G.chunks) {
        if (g_sbInFrame) g_sbFreeLater.push_back(c.bufId);
        else FreeStaticBufferId(c.bufId);
        g_sb.bufBytes -= c.bytes;
    }
    G.chunks.clear();
    G.bytes = 0;
    for (SbMember& mb : G.m) mb.chunk = -1;
}
// (Re)makes the group's buffers from the admitted members, in the order of the latest run (then the old order). The uploads go in front of command `at`.
// false: not made (a member's kept vertices are gone, or the cap): the group draws nothing batched until it is made.
bool SbBuild(SbGroup& G, std::vector<Cmd>& out) {
    std::vector<uint32_t> order;
    for (uint32_t i = 0; i < G.m.size(); ++i)
        if (G.m[i].admitted) order.push_back(i);
    std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        const SbMember &x = G.m[a], &y = G.m[b];
        if (x.lastFrame != y.lastFrame) return x.lastFrame > y.lastFrame; // (the latest run's members first, in its order)
        return x.lastFrame == g_frame ? x.lastPos < y.lastPos : x.bpos < y.bpos;
    });
    const VtxLayout L = MakeLayout(G.layoutMask);
    uint64_t need = 0;
    for (uint32_t i : order) need += static_cast<uint64_t>(G.m[i].count) * L.stride + G.m[i].idxCount * 2u;
    SbFreeChunks(G);
    G.dirty = false;
    if (g_sb.bufBytes + need > SbBufCap()) { ++g_sb.refusedCap; G.dead = true; return false; }
    std::vector<BufUpload> ups;
    BufUpload up;
    uint32_t pos = 0;
    for (uint32_t i : order) {
        SbMember& mb = G.m[i];
        const uint8_t* src = StaticSegVerts(mb.bufId, mb.serial, mb.byteOff, mb.count * L.stride);
        if (!src) { mb.admitted = false; continue; } // (its list is gone: never again)
        const uint32_t v0 = static_cast<uint32_t>(up.vtx.size() / L.stride);
        if (v0 + mb.count > 0xFFFFu) { ups.push_back(std::move(up)); up = BufUpload{}; }
        const uint32_t first = static_cast<uint32_t>(up.vtx.size() / L.stride);
        const size_t at = up.vtx.size();
        up.vtx.insert(up.vtx.end(), src, src + static_cast<size_t>(mb.count) * L.stride);
        for (uint32_t v = 0; v < mb.count; ++v) {
            uint8_t* w = up.vtx.data() + at + static_cast<size_t>(v) * L.stride;
            float p[3];
            std::memcpy(p, w, 12);
            const M34& R = mb.R;
            const float q[3] = {static_cast<float>(R[0] * p[0] + R[1] * p[1] + R[2] * p[2] + R[3]), static_cast<float>(R[4] * p[0] + R[5] * p[1] + R[6] * p[2] + R[7]),
                                static_cast<float>(R[8] * p[0] + R[9] * p[1] + R[10] * p[2] + R[11])};
            std::memcpy(w, q, 12);
            if (G.layoutMask & VL_NRM) {
                float n[3];
                std::memcpy(n, w + L.offNrm, 12);
                const M33& Q = mb.Q;
                const float o[3] = {static_cast<float>(Q[0] * n[0] + Q[1] * n[1] + Q[2] * n[2]), static_cast<float>(Q[3] * n[0] + Q[4] * n[1] + Q[5] * n[2]),
                                    static_cast<float>(Q[6] * n[0] + Q[7] * n[1] + Q[8] * n[2])};
                std::memcpy(w + L.offNrm, o, 12);
            }
        }
        mb.chunk = static_cast<int32_t>(ups.size());
        mb.vFirst = first;
        mb.iFirst = static_cast<uint32_t>(up.idx.size());
        mb.bpos = pos++;
        const size_t io = up.idx.size();
        up.idx.resize(io + mb.idxCount);
        PrimIndices(mb.gxPrim, mb.count, first, up.idx.data() + io);
    }
    if (!up.vtx.empty()) ups.push_back(std::move(up));
    stats::Frame& fr = stats::Cur();
    for (BufUpload& u : ups) {
        SbChunk c;
        c.vtx = static_cast<uint32_t>(u.vtx.size() / L.stride);
        c.idx = static_cast<uint32_t>(u.idx.size());
        c.bytes = static_cast<uint32_t>(u.vtx.size() + u.idx.size() * 2);
        u.bufId = c.bufId = AllocStaticBufferIdFresh();
        G.chunks.push_back(c);
        G.bytes += c.bytes;
        g_sb.bufBytes += c.bytes;
        g_sb.rebuildBytes += c.bytes;
        ++fr.bufUploads;
        fr.bufUploadBytes += c.bytes;
        g_buf.bufUploads.push_back(std::move(u));
        out.push_back(Cmd{CMD_BUF_UPLOAD, static_cast<uint32_t>(g_buf.bufUploads.size() - 1)});
    }
    ++g_sb.rebuilds;
    return true;
}
SbGroup* SbNewGroup(uint64_t key, const DrawRec& d, uint8_t posTg, uint8_t nrmTg, const uint8_t* rows) {
    SbGroup* G = new SbGroup;
    G->key = key;
    G->layoutMask = d.layoutMask;
    G->posTg = posTg;
    G->nrmTg = nrmTg;
    std::memcpy(G->tgRows, rows, 8);
    G->lastUse = g_frame;
    g_sb.groups.push_back(G);
    g_sb.byKey.emplace(key, G);
    return G;
}
void SbAddMember(SbGroup& G, const SbCand& c, const M34& R, const M33& Q) {
    SbMember mb{};
    mb.bufId = c.bufId; mb.serial = c.serial; mb.byteOff = c.byteOff; mb.count = c.count; mb.idxCount = c.idxCount; mb.gxPrim = c.gxPrim;
    mb.R = R;
    mb.Q = Q;
    if (!Inv34(R, mb.Ri) || !Inv33(Q, mb.Qi)) return;
    std::memcpy(mb.ext, c.ext, sizeof mb.ext);
    // the box of the moved corners
    for (int k = 0; k < 3; ++k) { mb.box[k] = 1e30f; mb.box[3 + k] = -1e30f; }
    for (int cn = 0; cn < 8; ++cn) {
        const double x = c.box[(cn & 1) ? 3 : 0], y = c.box[(cn & 2) ? 4 : 1], z = c.box[(cn & 4) ? 5 : 2];
        for (int r = 0; r < 3; ++r) {
            const float v = static_cast<float>(R[r * 4] * x + R[r * 4 + 1] * y + R[r * 4 + 2] * z + R[r * 4 + 3]);
            mb.box[r] = std::min(mb.box[r], v);
            mb.box[3 + r] = std::max(mb.box[3 + r], v);
        }
    }
    for (int k = 0; k < 3; ++k) { mb.box[k] = std::nextafter(mb.box[k], -1e30f); mb.box[3 + k] = std::nextafter(mb.box[3 + k], 1e30f); }
    mb.lastFrame = g_frame;
    G.byGeo.emplace(SbGeoKey(c.bufId, c.serial, c.byteOff, c.count, c.idxCount), static_cast<uint32_t>(G.m.size()));
    G.m.push_back(mb);
    ++g_sb.fresh;
}
void SbPushBlock(int k, const float* v, uint32_t n, DrawRec& d) {
    const size_t at = g_buf.uni.size();
    g_buf.uni.resize(at + n);
    std::memcpy(g_buf.uni.data() + at, v, n * sizeof(float));
    d.ublock[k] = static_cast<uint32_t>(at);
}
void SbSetBox(uint32_t drawIdx, const float* b) {
    auto& v = g_buf.staticBoxes;
    auto it = std::lower_bound(v.begin(), v.end(), drawIdx, [](const StaticBox& a, uint32_t dd) { return a.draw < dd; });
    if (it != v.end() && it->draw == drawIdx) { std::memcpy(it->b, b, sizeof it->b); return; }
    StaticBox sb;
    sb.draw = drawIdx;
    std::memcpy(sb.b, b, sizeof sb.b);
    v.insert(it, sb);
}

// One run of adjacent candidate records of one material (cmd indices in `runCmds`). Writes the commands that replace them to `out`.
void SbRun(const std::vector<uint32_t>& run, std::vector<Cmd>& out) {
    const size_t n = run.size();
    const DrawRec& d0 = g_buf.draws[run[0]];
    auto passThrough = [&] { for (uint32_t di : run) out.push_back(Cmd{CMD_DRAW, di}); };
    if (n < 2 || d0.vsId >= g_vsReg.keys.size()) { passThrough(); return; }
    const VsKey& k = g_vsReg.keys[d0.vsId];
    uint8_t posTg, nrmTg, rows[8] = {};
    if (k.perVtxPos || !SbTexGens(k, posTg, nrmTg, rows)) { passThrough(); return; }
    ++g_sb.runs;
    g_sb.runRecs += n;
    const uint64_t key = g_sbCand[g_sbCandOf[run[0]]].mkey;
    // the islands of this material: the first in which members of the run are found with one frame matrix; else a new one (when none of them is known)
    std::vector<int32_t> jOf(n, -1);
    SbGroup* G = nullptr;
    SbFrameMats F;
    (void)rows;
    auto range = g_sb.byKey.equal_range(key);
    const M33 I3 = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    auto geoOf = [&](size_t i) { const SbCand& c = g_sbCand[g_sbCandOf[run[i]]]; return SbGeoKey(c.bufId, c.serial, c.byteOff, c.count, c.idxCount); };
    // the frame: of the admitted members of the run's first 8 records (in every island of the material), the one whose frame makes the most records
    // of the run fit (any frame is correct: a record is only ever drawn from the buffer when its own matrices fit the frame it is drawn with)
    {
        int best = 0;
        for (auto it = range.first; it != range.second; ++it) {
            SbGroup& g = *it->second;
            if (g.dead || g.m.empty()) continue;
            for (size_t a = 0; a < n && a < 8; ++a) {
                auto rg = g.byGeo.equal_range(geoOf(a));
                int tries = 0;
                for (auto jt = rg.first; jt != rg.second && tries < 4; ++jt, ++tries) {
                    SbMember& ja = g.m[jt->second];
                    if (!ja.admitted) continue;
                    SbFrameMats f = SbMatsFrom(g, ja, g_buf.draws[run[a]]);
                    if (!f.ok) continue;
                    if (!(g.layoutMask & VL_NRM)) f.n = I3;
                    if (!SbFits(g, ja, f, g_buf.draws[run[a]])) continue;
                    int cnt = 1;
                    for (size_t b = 0; b < n; ++b) {
                        if (b == a) continue;
                        auto rb = g.byGeo.equal_range(geoOf(b));
                        for (auto bt = rb.first; bt != rb.second; ++bt)
                            if (bt->second != jt->second && g.m[bt->second].admitted && SbFits(g, g.m[bt->second], f, g_buf.draws[run[b]])) { ++cnt; break; }
                    }
                    if (cnt > best) { best = cnt; G = &g; F = f; }
                    if (best == static_cast<int>(n)) break;
                }
                if (best == static_cast<int>(n)) break;
            }
            if (best == static_cast<int>(n)) break;
        }
    }
    if (!G) {
        // an island none of whose admitted members is here: the one whose pending members are (they learn in its frame), else an empty one, else a new one
        for (int pass = 0; pass < 2 && !G; ++pass)
            for (auto it = range.first; it != range.second && !G; ++it) {
                SbGroup& g = *it->second;
                if (g.dead) continue;
                if (pass == 1 && g.m.empty()) { G = &g; break; }
                if (pass == 1) continue;
                for (size_t a = 0; a < n && !G; ++a) {
                    auto rg = g.byGeo.equal_range(geoOf(a));
                    for (auto jt = rg.first; jt != rg.second; ++jt) {
                        SbFrameMats f = SbMatsFrom(g, g.m[jt->second], g_buf.draws[run[a]]);
                        if (!f.ok) continue;
                        if (!(g.layoutMask & VL_NRM)) f.n = I3;
                        bool other = false;
                        for (size_t b = 0; b < n && !other; ++b) {
                            if (b == a) continue;
                            auto rb = g.byGeo.equal_range(geoOf(b));
                            for (auto bt = rb.first; bt != rb.second; ++bt)
                                if (bt->second != jt->second && SbFits(g, g.m[bt->second], f, g_buf.draws[run[b]])) { other = true; break; }
                        }
                        if (other) { G = &g; F = f; break; }
                    }
                }
            }
        if (!G) {
            bool any = false; // (a record of the run is a member somewhere: it moves against the others; nothing to learn)
            for (auto it = range.first; it != range.second && !any; ++it)
                for (size_t a = 0; a < n && !any; ++a) any = it->second->byGeo.count(geoOf(a)) != 0;
            if (any) { passThrough(); return; }
            if (static_cast<size_t>(std::distance(range.first, range.second)) >= 8) { passThrough(); return; }
            G = SbNewGroup(key, d0, posTg, nrmTg, rows);
        }
        if (G->m.empty()) {
            // the island's frame: the run's first record's
            if (!Finite(MvOf(d0), 24)) { passThrough(); return; }
            SbMember tmp{};
            tmp.Ri = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
            tmp.Qi = I3;
            F = SbMatsFrom(*G, tmp, d0);
            if (!(G->layoutMask & VL_NRM)) F.n = I3;
        }
    }
    G->lastUse = g_frame;
    // assign: each record a member of this island that fits in the frame (one member per record, each member once)
    std::vector<uint8_t> used(G->m.size(), 0);
    for (size_t i = 0; i < n; ++i) {
        const SbCand& c = g_sbCand[g_sbCandOf[run[i]]];
        const DrawRec& d = g_buf.draws[run[i]];
        auto rg = G->byGeo.equal_range(SbGeoKey(c.bufId, c.serial, c.byteOff, c.count, c.idxCount));
        for (auto jt = rg.first; jt != rg.second; ++jt) {
            if (jt->second < used.size() && used[jt->second]) continue;
            if (SbFits(*G, G->m[jt->second], F, d)) { jOf[i] = static_cast<int32_t>(jt->second); used[jt->second] = 1; break; }
        }
        if (jOf[i] >= 0) {
            SbMember& mb = G->m[jOf[i]];
            if (mb.lastFrame != g_frame) {
                mb.lastFrame = g_frame;
                if (!mb.admitted && ++mb.sightings >= 2) { mb.admitted = true; G->dirty = true; ++g_sb.admitted; }
            }
            mb.lastPos = static_cast<uint32_t>(i);
            continue;
        }
        // not known in this island: a new member, in the frame's terms (R = F.mv^-1 x MV, Q = F.n^-1 x N), if the texture matrices follow;
        // not when a member of this geometry is left unused (it moved: no new member every frame for a moving object)
        bool spare = false;
        for (auto jt = rg.first; jt != rg.second && !spare; ++jt) spare = !(jt->second < used.size() && used[jt->second]);
        if (spare || G->m.size() >= 512) continue;
        const float* mv = MvOf(d);
        if (!Finite(mv, 24)) continue;
        M34 Fi;
        M33 Fni;
        if (!Inv34(F.mv, Fi) || !Inv33(F.n, Fni)) continue;
        const M34 R = Mul34(Fi, Load34(mv));
        const M33 Q = (G->layoutMask & VL_NRM) ? Mul33(Fni, LoadN(mv + 12)) : M33{1, 0, 0, 0, 1, 0, 0, 0, 1};
        const size_t before = G->m.size();
        SbAddMember(*G, c, R, Q);
        if (G->m.size() == before) continue;
        SbMember& mb = G->m.back();
        mb.lastPos = static_cast<uint32_t>(i);
        if (!SbFits(*G, mb, F, d)) { // (texture matrices do not follow)
            const uint32_t idx = static_cast<uint32_t>(G->m.size() - 1);
            auto r2 = G->byGeo.equal_range(SbGeoKey(c.bufId, c.serial, c.byteOff, c.count, c.idxCount));
            for (auto it2 = r2.first; it2 != r2.second; ++it2)
                if (it2->second == idx) { G->byGeo.erase(it2); break; }
            G->m.pop_back();
            continue;
        }
        used.push_back(1);
        jOf[i] = -1; // (not drawn from the buffer before it is admitted)
    }
    if (G->dirty) SbBuild(*G, out);
    {
        static const long tf = EnvInt("MSC_VGX_SBATCH_TRACE", -1);
        if (tf >= 0 && static_cast<uint64_t>(tf) == g_frame) {
            std::fprintf(stderr, "[sbatch-trace] f=%llu run n=%lu vs=%u fs=%u tex=%u island members=%lu posTg=%x nrmTg=%x:", static_cast<unsigned long long>(g_frame), static_cast<unsigned long>(n), d0.vsId, d0.fsId,
                         d0.tex[0].texId, static_cast<unsigned long>(G->m.size()), G->posTg, G->nrmTg);
            for (size_t q = 0; q < n; ++q) {
                const int32_t jq = jOf[q];
                const SbCand& cq = g_sbCand[g_sbCandOf[run[q]]];
                const float* mq = MvOf(g_buf.draws[run[q]]);
                std::fprintf(stderr, " [b%u %s t=(%.2f %.2f %.2f)]", cq.bufId, jq < 0 ? "none" : !G->m[jq].admitted ? "pend" : G->m[jq].chunk < 0 ? "nochunk" : "ok", mq[3], mq[7], mq[11]);
            }
            std::fprintf(stderr, "\n");
        }
    }
    // stretches: records whose members are consecutive in the buffer
    size_t i = 0;
    while (i < n) {
        const int32_t j = jOf[i];
        if (j < 0 || !G->m[j].admitted || G->m[j].chunk < 0) { out.push_back(Cmd{CMD_DRAW, run[i]}); ++g_sb.recsOut; ++g_sb.unassigned; ++i; continue; }
        size_t e = i + 1;
        while (e < n) {
            const int32_t jn = jOf[e];
            const SbMember& prev = G->m[jOf[e - 1]];
            if (jn < 0 || !G->m[jn].admitted || G->m[jn].chunk != prev.chunk || G->m[jn].bpos != prev.bpos + 1) break;
            ++e;
        }
        if (e - i == 1) { out.push_back(Cmd{CMD_DRAW, run[i]}); ++g_sb.recsOut; ++i; continue; } // (alone: its own record, exactly as before)
        DrawRec& rec = g_buf.draws[run[i]];
        const SbMember& m0 = G->m[jOf[i]];
        uint32_t vtx = 0, idx = 0;
        float box[6] = {1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f};
        for (size_t q = i; q < e; ++q) {
            const SbMember& mq = G->m[jOf[q]];
            vtx += mq.count;
            idx += mq.idxCount;
            for (int a = 0; a < 3; ++a) { box[a] = std::min(box[a], mq.box[a]); box[3 + a] = std::max(box[3 + a], mq.box[3 + a]); }
        }
        if (SbCheckOn()) {
            // every member's corners through MVrec x R against its own matrix
            for (size_t q = i; q < e; ++q) {
                const SbMember& mq = G->m[jOf[q]];
                const M34 a = Mul34(F.mv, mq.R);
                const float* b = MvOf(g_buf.draws[run[q]]);
                double err = 0;
                for (int r = 0; r < 3; ++r) {
                    double ee = std::fabs(a[r * 4 + 3] - b[r * 4 + 3]);
                    for (int kk = 0; kk < 3; ++kk) ee += std::fabs(a[r * 4 + kk] - b[r * 4 + kk]) * mq.ext[kk];
                    err = std::max(err, ee);
                }
                ++g_sb.checked;
                g_sb.checkMax = std::max(g_sb.checkMax, err);
                if (!(err <= 1e-2)) ++g_sb.checkBad;
            }
        }
        float mvb[24] = {};
        for (int a = 0; a < 12; ++a) mvb[a] = static_cast<float>(F.mv[a]);
        for (int r = 0; r < 3; ++r)
            for (int a = 0; a < 3; ++a) mvb[12 + r * 4 + a] = static_cast<float>(F.n[r * 3 + a]);
        SbPushBlock(UB_MV, mvb, 24, rec);
        if (G->posTg | G->nrmTg) {
            const float* tm = TmOf(rec);
            uint32_t len = 0;
            bool anyPost = false;
            for (int t = 0; t < k.numTexGens; ++t) anyPost |= k.tg[t].type < 2 && k.tg[t].post != 0;
            len = anyPost ? 96u + k.numTexGens * 12u : k.numTexGens * 12u;
            if (UniformPacking() == false) len = 192;
            std::vector<float> nb(tm, tm + len);
            for (int t = 0; t < 8; ++t)
                if (((G->posTg | G->nrmTg) >> t) & 1u)
                    for (int a = 0; a < 12; ++a) nb[t * 12 + a] = static_cast<float>(F.tm[t][a]);
            SbPushBlock(UB_TEXM, nb.data(), len, rec);
        }
        const SbChunk& ch = G->chunks[m0.chunk];
        rec.vbuf = ch.bufId;
        rec.vtxOffset = 0;
        rec.vchunk = 0;
        rec.vtxFirst = m0.vFirst;
        rec.vtxCount = vtx;
        rec.idxOffset = m0.iFirst;
        rec.idxCount = idx;
        if (FillBoxesWanted() && (rec.rs.alphaWrite || rec.rs.dstAlphaPass)) SbSetBox(run[i], box);
        out.push_back(Cmd{CMD_DRAW, run[i]});
        for (size_t q = i + 1; q < e; ++q) { // (the absorbed records draw nothing if anything still looks at them)
            DrawRec& z = g_buf.draws[run[q]];
            z.idxCount = 0;
            z.vtxCount = 0;
            --stats::Cur().drawCalls;
        }
        ++g_sb.recsOut;
        g_sb.saved += e - i - 1;
        const uint8_t cl = rec.cls < CLS_COUNT ? rec.cls : 0;
        g_sb.byCls[cl][0] += e - i;
        g_sb.byCls[cl][1] += 1;
        i = e;
    }
}
} // namespace

// (any class: a stadium's lists are often promoted in the walk-out or a cut-scene, before the match marks them; copies of lists that never become a
// candidate are dropped by SBatchTrimKept)
bool SBatchKeepWanted() { return SbMode() != 0; }
uint64_t SBatchKeepCapBytes() {
    const uint64_t b = static_cast<uint64_t>(VGX_KNOB(static_cast<uint64_t>(EnvInt("MSC_VGX_SBATCH_MB", 6)) << 20));
    return b;
}
// A static buffer is being released: the islands holding its geometry are dropped (relearned from the frames that follow).
void SBatchForget(uint32_t bufId) {
    for (SbGroup* G : g_sb.groups) {
        if (G->dead && G->chunks.empty()) continue;
        for (const SbMember& mb : G->m)
            if (mb.bufId == bufId) { SbFreeChunks(*G); G->dead = true; break; }
    }
}
namespace {
// EmitStaticDraw: the record just made may join a static batch
uint64_t g_sbRej[6] = {};
uint64_t g_sbWhy[10] = {}; // (statistics: of the "no kept vertices", by the cache's reason: vgx_dlcache.cpp Entry::sbWhy, 9 no owner) // (statistics: background static draws not noted: no kept vertices, no bounds, matrix index / skin / layout, shader / blocks, Z-freeze)
void SbNote(uint8_t gxPrim, uint32_t layoutMask, uint32_t vcdMtxMask, uint32_t count, uint32_t idxCount, uint32_t bufId, uint32_t vtxByteOffset, uint32_t vtxFirst) {
    if (g_layerCls < CLS_BG_FIRST) return;
    if (!g_staticSegVerts) { ++g_sbRej[0]; ++g_sbWhy[SBatchWhyNotKept(bufId) % 10]; SBatchWant(bufId); return; }
    if (!g_staticSegBounds) { ++g_sbRej[1]; return; }
    if (PrimKind(gxPrim) != PRIM_TRIANGLES || (layoutMask & (VL_SKIN | VL_TMI)) || vcdMtxMask != 0) { ++g_sbRej[2]; return; }
    bool palTg = false;
    for (int i = 0; i < cur.vs.numTexGens; ++i) palTg |= cur.vs.tg[i].perVtxMtx != 0;
    if (cur.soft || cur.vs.perVtxPos || palTg || g_blockOff[UB_MV] == kNoBlock) { ++g_sbRej[3]; return; }
    if ((g.bp[0x00] & (1u << 19)) || ZFreezeTracking() || TraceFrame()) { ++g_sbRej[4]; return; }
    ++g_sbRej[5];
    SBatchNoted(bufId);
    SbCand c;
    c.drawIdx = static_cast<uint32_t>(g_buf.draws.size() - 1);
    c.bufId = bufId;
    c.serial = g_staticSegSerial;
    c.byteOff = vtxByteOffset + vtxFirst * MakeLayout(layoutMask).stride;
    c.count = count;
    c.idxCount = idxCount;
    c.gxPrim = gxPrim;
    std::memcpy(c.box, g_staticSegBounds, sizeof c.box);
    c.mkey = SbMaterialKey(g_buf.draws[c.drawIdx]);
    for (int a = 0; a < 3; ++a) c.ext[a] = std::max(std::fabs(c.box[a]), std::fabs(c.box[3 + a]));
    if (g_sbCandOf.size() < g_buf.draws.size()) g_sbCandOf.resize(g_buf.draws.size() + 256, -1);
    g_sbCandOf[c.drawIdx] = static_cast<int32_t>(g_sbCand.size());
    g_sbCand.push_back(c);
}

// ---- MSC_VGX_SBATCH_ORDER=1: an opaque candidate record is moved back to just after the last earlier record of its material in the same stretch of
// draws (no copy, target, pass or upload between) when it provably commutes with every record it passes, so that the two join one run:
//   - the pixel rectangles of the two (the box of their bounds through matrices, projection and viewport, clipped to their scissor) do not meet: no pixel
//     is touched by both, so their order cannot matter; or
//   - both are opaque depth writers (no blending, depth test LESS / LEQUAL with depth write, no second alpha pass) and their window depth ranges are
//     apart by more than 2^-15: at every pixel both touch, one is nearer by more than a 16-bit depth step, so the result is the nearer one in either
//     order (no tie a LEQUAL test could resolve by order).
// A record whose bounds are not known (no box, a corner behind the camera, a palette or GPU skin) commutes with nothing. Pixels: identical (no
// arithmetic changes, only the order of operations that commute).
bool SbOrderOn() {
    const bool on = static_cast<bool>(VGX_KNOB(EnvInt("MSC_VGX_SBATCH_ORDER", 0) != 0));
    return on;
}
struct SbInfo {
    uint8_t state = 0; // 0 not computed, 1 unknown, 2 known
    bool none = false;  // touches no pixel (outside its scissor)
    bool opaque = false;
    float x0, y0, x1, y1, z0, z1; // pixel rectangle, window depth range
};
std::vector<SbInfo> g_sbInfo;
bool SbBoxOf(uint32_t di, float* b) {
    const DrawRec& d = g_buf.draws[di];
    if (di < g_sbCandOf.size() && g_sbCandOf[di] >= 0) { std::memcpy(b, g_sbCand[g_sbCandOf[di]].box, 24); return true; }
    if (d.vbuf) {
        auto& v = g_buf.staticBoxes;
        auto it = std::lower_bound(v.begin(), v.end(), di, [](const StaticBox& a, uint32_t dd) { return a.draw < dd; });
        if (it != v.end() && it->draw == di) { std::memcpy(b, it->b, 24); return true; }
        return false;
    }
    if (d.vtxCount == 0 || d.vtxCount > 512) return false;
    const VtxLayout L = MakeLayout(d.layoutMask);
    const size_t base = (g_buf.vtxChunks.empty() ? 0 : g_buf.vtxChunks[d.vchunk]) + d.vtxOffset + static_cast<size_t>(d.vtxFirst) * L.stride;
    if (base + static_cast<size_t>(d.vtxCount) * L.stride > g_buf.vtx.size()) return false;
    for (int k = 0; k < 3; ++k) { b[k] = 1e30f; b[3 + k] = -1e30f; }
    for (uint32_t i = 0; i < d.vtxCount; ++i) {
        float p[3];
        std::memcpy(p, g_buf.vtx.data() + base + static_cast<size_t>(i) * L.stride, 12);
        for (int k = 0; k < 3; ++k) { if (!std::isfinite(p[k])) return false; b[k] = std::min(b[k], p[k]); b[3 + k] = std::max(b[3 + k], p[k]); }
    }
    return true;
}
const SbInfo& SbInfoOf(uint32_t di) {
    if (g_sbInfo.size() < g_buf.draws.size()) g_sbInfo.resize(g_buf.draws.size());
    SbInfo& r = g_sbInfo[di];
    if (r.state) return r;
    r.state = 1;
    const DrawRec& d = g_buf.draws[di];
    const RenderState& rs = d.rs;
    r.opaque = rs.blend == 0 && rs.depthTest && rs.depthWrite && (rs.depthFunc == CMP_LESS || rs.depthFunc == CMP_LEQUAL) && !rs.dstAlphaPass;
    // (a fragment shader that writes its own depth, Z-freeze or a Z texture, is not bounded by its geometry's depth)
    if (d.fsId >= g_fsReg.keys.size() || g_fsReg.keys[d.fsId].zfreeze || g_fsReg.keys[d.fsId].ztex) r.opaque = false;
    if (rs.scW <= 0 || rs.scH <= 0) { r.none = true; r.state = 2; return r; }
    if (d.prim != PRIM_TRIANGLES || d.skinBlock != kNoBlock || d.ublock[UB_MV] == kNoBlock || d.ublock[UB_PROJ] == kNoBlock) return r;
    if (d.ublock[UB_MV] + 24 > g_buf.uni.size() || d.ublock[UB_PROJ] + 16 > g_buf.uni.size()) return r;
    if (d.vsId >= g_vsReg.keys.size() || g_vsReg.keys[d.vsId].perVtxPos) return r;
    for (int i = 0; i < g_vsReg.keys[d.vsId].numTexGens; ++i)
        if (g_vsReg.keys[d.vsId].tg[i].perVtxMtx) return r; // (a palette texture matrix: the box is not through u_mv alone... only the position matters, but keep it simple)
    float b[6];
    if (!SbBoxOf(di, b)) return r;
    const float* m = g_buf.uni.data() + d.ublock[UB_MV];
    const float* P = g_buf.uni.data() + d.ublock[UB_PROJ];
    double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30, z0 = 1e30, z1 = -1e30;
    for (int c = 0; c < 8; ++c) {
        const double x = b[(c & 1) ? 3 : 0], y = b[(c & 2) ? 4 : 1], z = b[(c & 4) ? 5 : 2];
        const double ex = m[0] * x + m[1] * y + m[2] * z + m[3], ey = m[4] * x + m[5] * y + m[6] * z + m[7], ez = m[8] * x + m[9] * y + m[10] * z + m[11];
        const double cx = P[0] * ex + P[4] * ey + P[8] * ez + P[12], cy = P[1] * ex + P[5] * ey + P[9] * ez + P[13];
        const double cz = P[2] * ex + P[6] * ey + P[10] * ez + P[14], cw = P[3] * ex + P[7] * ey + P[11] * ez + P[15];
        if (!(cw >= 1e-6) || !std::isfinite(cx + cy + cz)) return r; // (behind the camera: not bounded by the corners)
        const double sx = rs.vpX + (cx / cw * 0.5 + 0.5) * rs.vpW, sy = rs.vpY + (cy / cw * 0.5 + 0.5) * rs.vpH;
        const double wz = rs.zNear + (cz / cw * 0.5 + 0.5) * (rs.zFar - rs.zNear);
        x0 = std::min(x0, sx); x1 = std::max(x1, sx); y0 = std::min(y0, sy); y1 = std::max(y1, sy); z0 = std::min(z0, wz); z1 = std::max(z1, wz);
    }
    // (2 pixels of margin; the Y flip may be folded into the projection: the rectangle is compared with others made the same way and with the scissor
    // only through min / max, so only the margin matters)
    x0 -= 2; y0 -= 2; x1 += 2; y1 += 2;
    // clipped to the scissor (both in EFB pixels, the viewport's units)
    const double sx0 = rs.scX, sy0 = rs.scY, sx1 = static_cast<double>(rs.scX) + rs.scW, sy1 = static_cast<double>(rs.scY) + rs.scH;
    const double fy0 = std::min(y0, static_cast<double>(rs.vpY + rs.vpH) - (y1 - rs.vpY)), fy1 = std::max(y1, static_cast<double>(rs.vpY + rs.vpH) - (y0 - rs.vpY));
    // (fy0..fy1: the rectangle and its mirror in the viewport together, so a flip either way is covered)
    x0 = std::max(x0, sx0); x1 = std::min(x1, sx1);
    const double cy0 = std::max(fy0, sy0), cy1 = std::min(fy1, sy1);
    if (x0 > x1 || cy0 > cy1) { r.none = true; r.state = 2; return r; }
    r.x0 = static_cast<float>(x0); r.x1 = static_cast<float>(x1); r.y0 = static_cast<float>(cy0); r.y1 = static_cast<float>(cy1);
    r.z0 = static_cast<float>(z0); r.z1 = static_cast<float>(z1);
    r.state = 2;
    return r;
}
bool SbCommute(uint32_t a, uint32_t b) {
    const SbInfo& A = SbInfoOf(a);
    const SbInfo& B = SbInfoOf(b);
    if (A.state != 2 || B.state != 2) return false;
    if (A.none || B.none) return true;
    if (A.x1 < B.x0 || B.x1 < A.x0 || A.y1 < B.y0 || B.y1 < A.y0) return true;
    const float gap = 1.0f / 32768.0f;
    return A.opaque && B.opaque && (A.z1 + gap < B.z0 || B.z1 + gap < A.z0);
}
uint64_t g_sbMoved = 0, g_sbMoveTried = 0;
// Reorders the draws of each stretch (see above); declarations stay where they are.
void SbReorder() {
    std::vector<Cmd>& cmds = g_buf.cmds;
    std::vector<Cmd> out;
    out.reserve(cmds.size());
    size_t i = 0;
    std::vector<Cmd> st;   // the stretch being built (draws and declarations)
    std::vector<uint64_t> mk; // material key per item of st (0: not a movable candidate / a declaration)
    auto flushSt = [&] { out.insert(out.end(), st.begin(), st.end()); st.clear(); mk.clear(); };
    for (; i < cmds.size(); ++i) {
        const Cmd c = cmds[i];
        if (c.type == CMD_SHADER || c.type == CMD_PROGRAM || c.type == CMD_VARIANT) { st.push_back(c); mk.push_back(0); continue; }
        if (c.type != CMD_DRAW) { flushSt(); out.push_back(c); continue; }
        const bool cand = c.arg < g_sbCandOf.size() && g_sbCandOf[c.arg] >= 0;
        uint64_t key = 0;
        if (cand && SbInfoOf(c.arg).opaque) key = g_sbCand[g_sbCandOf[c.arg]].mkey | 1ull;
        if (key) {
            // the last earlier item of the material; already adjacent: nothing to do
            size_t p = st.size();
            while (p > 0 && mk[p - 1] != key) --p;
            if (p > 0 && p < st.size()) {
                ++g_sbMoveTried;
                bool ok = true;
                for (size_t q = p; q < st.size() && ok; ++q)
                    if (st[q].type == CMD_DRAW) ok = SbCommute(c.arg, st[q].arg);
                if (ok) {
                    st.insert(st.begin() + static_cast<std::ptrdiff_t>(p), c);
                    mk.insert(mk.begin() + static_cast<std::ptrdiff_t>(p), key);
                    ++g_sbMoved;
                    continue;
                }
            }
        }
        st.push_back(c);
        mk.push_back(key);
    }
    flushSt();
    cmds.swap(out);
    for (SbInfo& r : g_sbInfo) r.state = 0;
}
// FrontPresent: the frame's runs of candidate records become batched records
void SbFrame() {
    SbState& S = g_sb;
    if (!g_sbCand.empty()) {
        if (SbOrderOn()) SbReorder();
        g_sbInFrame = true;
        std::vector<Cmd> out;
        out.reserve(g_buf.cmds.size() + 8);
        std::vector<uint32_t> run;
        const DrawRec* runRec = nullptr;
        uint64_t runKey = 0;
        auto flush = [&] {
            if (!run.empty()) SbRun(run, out);
            run.clear();
            runRec = nullptr;
        };
        for (const Cmd& c : g_buf.cmds) {
            if (c.type == CMD_DRAW && c.arg < g_sbCandOf.size() && g_sbCandOf[c.arg] >= 0) {
                const DrawRec& d = g_buf.draws[c.arg];
                if (runRec) {
                    bool same = runKey == g_sbCand[g_sbCandOf[c.arg]].mkey;
                    if (same && runRec->vsId < g_vsReg.keys.size()) {
                        SbGroup tmp;
                        uint8_t rows[8];
                        same = SbTexGens(g_vsReg.keys[runRec->vsId], tmp.posTg, tmp.nrmTg, rows) && SbSameRun(*runRec, d, tmp, g_vsReg.keys[runRec->vsId]);
                    }
                    if (!same) flush();
                }
                run.push_back(c.arg);
                if (!runRec) { runRec = &g_buf.draws[c.arg]; runKey = g_sbCand[g_sbCandOf[c.arg]].mkey; }
                continue;
            }
            if (c.type == CMD_SHADER || c.type == CMD_PROGRAM || c.type == CMD_VARIANT) { out.push_back(c); continue; } // (declarations: no effect on draws)
            flush();
            out.push_back(c);
        }
        flush();
        g_buf.cmds.swap(out);
        g_sbInFrame = false;
        for (uint32_t id : g_sbFreeLater) FreeStaticBufferId(id);
        g_sbFreeLater.clear();
        for (const SbCand& c : g_sbCand)
            if (c.drawIdx < g_sbCandOf.size()) g_sbCandOf[c.drawIdx] = -1;
        g_sbCand.clear();
    }
    ++S.frames;
    // islands not used for 600 frames, and dead ones, go
    if (g_frame - S.lastSweep >= 120) {
        S.lastSweep = g_frame;
        SBatchTrimKept(600); // (idle copies; a cut-scene that hides the stadium for a few seconds keeps them)
        for (size_t i = 0; i < S.groups.size();) {
            SbGroup* G = S.groups[i];
            if (G->dead || g_frame - G->lastUse > 600) {
                SbFreeChunks(*G);
                auto r = S.byKey.equal_range(G->key);
                for (auto it = r.first; it != r.second; ++it)
                    if (it->second == G) { S.byKey.erase(it); break; }
                delete G;
                S.groups[i] = S.groups.back();
                S.groups.pop_back();
            } else {
                ++i;
            }
        }
    }
    if (SbStatsOn() && g_frame % 600 == 0 && S.frames) {
        const double f = static_cast<double>(S.frames);
        size_t mem = 0, adm = 0;
        for (const SbGroup* G : S.groups) { mem += G->m.size(); for (const SbMember& m : G->m) adm += m.admitted; }
        std::fprintf(stderr, "[sbatch] frame %llu, per frame over %llu frames: %.1f runs of %.1f records -> %.1f records (%.1f saved, %.1f not from a batch); %.2f new members, %.2f admitted, "
                             "%.2f rebuilds (%.0f bytes) | live: %lu islands, %lu members (%lu admitted), buffers %.2f MB, kept vertices %.2f MB, %llu refused (cap)",
                     static_cast<unsigned long long>(g_frame), static_cast<unsigned long long>(S.frames), S.runs / f, S.runRecs / f, S.recsOut / f, S.saved / f, S.unassigned / f, S.fresh / f, S.admitted / f,
                     S.rebuilds / f, S.rebuildBytes / f, static_cast<unsigned long>(S.groups.size()), static_cast<unsigned long>(mem), static_cast<unsigned long>(adm), S.bufBytes / 1048576.0, SBatchKeptBytes() / 1048576.0, static_cast<unsigned long long>(S.refusedCap));
        std::fprintf(stderr, " | by class (members->records):");
        for (int c = 0; c < CLS_COUNT; ++c)
            if (S.byCls[c][0]) std::fprintf(stderr, " %d:%.1f->%.1f", c, S.byCls[c][0] / f, S.byCls[c][1] / f);
        std::fprintf(stderr, "; noted %.1f, not: %.1f no kept vertices, %.1f no bounds, %.1f matrix index/skin/layout, %.1f shader/blocks, %.1f Z-freeze", g_sbRej[5] / f, g_sbRej[0] / f,
                     g_sbRej[1] / f, g_sbRej[2] / f, g_sbRej[3] / f, g_sbRej[4] / f);
        std::memset(g_sbRej, 0, sizeof g_sbRej);
        std::fprintf(stderr, " (why:");
        for (int w = 0; w < 10; ++w)
            if (g_sbWhy[w]) std::fprintf(stderr, " %d:%.1f", w, g_sbWhy[w] / f);
        std::fprintf(stderr, ", %llu lists decoded again so far)", static_cast<unsigned long long>(SBatchRekept()));
        std::memset(g_sbWhy, 0, sizeof g_sbWhy);
        if (SbOrderOn()) std::fprintf(stderr, "; moved back %.1f of %.1f tried", g_sbMoved / f, g_sbMoveTried / f);
        g_sbMoved = g_sbMoveTried = 0;
        if (SbCheckOn()) std::fprintf(stderr, "; check: %llu members, %llu over 1e-2, largest eye-space error %.3g", static_cast<unsigned long long>(S.checked), static_cast<unsigned long long>(S.checkBad), S.checkMax);
        std::fprintf(stderr, "\n");
        S.frames = S.runs = S.runRecs = S.recsOut = S.saved = S.unassigned = S.fresh = S.admitted = S.rebuilds = S.rebuildBytes = 0;
        std::memset(S.byCls, 0, sizeof S.byCls);
    }
}
} // namespace
