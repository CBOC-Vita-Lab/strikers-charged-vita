// Front end: turns the register state into self-contained draw records.
#include "vgx_state.h"
#include "vgx_perf_diag.h"
#include "vgx_ext.h"
#include "vgx_zfreeze.h"
#include "vgx_capture.h"
#include "vgx_interp.h"
extern "C" void NpLatDecoded(uint64_t tag) __attribute__((weak));

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <array>
#include <cmath>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace vgx {

namespace {

CmdBuf g_buf;
uint64_t g_frame = 1;

// ---- shader registry (ids are dense; sources are immutable and never freed)
template <typename K> struct Registry {
    struct Entry { K key; uint16_t id; };
    std::unordered_multimap<uint64_t, Entry> map;
    std::vector<std::unique_ptr<ShaderSource>> sources;
    std::vector<K> keys; // by id
    struct Front { uint64_t h = 0; uint16_t id = 0; bool valid = false; } front[64];
};
Registry<FsKey> g_fsReg;
Registry<VsKey> g_vsReg;

// ---- shader warm-up list
// A shader is generated the first time a draw needs it, and the back end
// compiles it there and then: on the console's replacement GPU that is a
// pause of many milliseconds in the middle of play (53 vertex and 47 fragment
// shaders, 78 programs over one match). So every key and every program pair
// is appended to a file as it first appears, and a run that finds the file
// generates all of them, and has the pairs linked, in its first frame.
// File: ListHeader, then records {u32 type; payload}: 1 = VsKey, 2 = FsKey
// (in id order of the run that wrote them), 3 = {u32 vsId, u32 fsId},
// 4 = {u32 vsId, fsId, layoutMask, blendKey}: a variant of a program the
// console's shader patcher builds at first use (VariantCmd in vgx_cmd.h);
// the back end is asked for an empty draw with each (CMD_VARIANT).
struct ListHeader {
    char magic[8];
    uint32_t vsKeySize, fsKeySize, genVersion, reserved;
};
constexpr char kListMagic[8] = {'V', 'G', 'X', 'S', 'H', 'L', '0', '1'};
std::string g_shaderListPath;
FILE* g_shaderList = nullptr;   // open for appending once the list has been read
bool g_shaderListDone = false;  // read (or found absent / not configured)
bool g_shaderListReplay = false;
std::unordered_set<uint32_t> g_progSeen;
struct VariantHash {
    size_t operator()(uint64_t key) const noexcept {
        // Mix shader IDs into the low word before 32-bit size_t truncates it.
        key ^= key >> 30;
        key *= 0xbf58476d1ce4e5b9ull;
        key ^= key >> 27;
        key *= 0x94d049bb133111ebull;
        key ^= key >> 31;
        return static_cast<size_t>(key);
    }
};
std::unordered_set<uint64_t, VariantHash> g_varSeen; // VariantKey
uint64_t g_lastVariant = ~0ull;         // of the last draw (most draws repeat it)
inline uint64_t VariantKey(uint32_t vsId, uint32_t fsId, uint32_t layoutMask, uint32_t blendKey) {
    return (static_cast<uint64_t>((vsId << 16) | fsId) << 32) | ((layoutMask & 0x1FFFu) << 12) | (blendKey & 0xFFFu);
}
void EnsureShaderList();
inline void ListAppend(uint32_t type, const void* p, size_t n) {
    if (!g_shaderList || g_shaderListReplay) return;
    std::fwrite(&type, 4, 1, g_shaderList);
    std::fwrite(p, n, 1, g_shaderList);
    std::fflush(g_shaderList);
}

template <typename K, typename Gen>
uint16_t Lookup(Registry<K>& reg, const K& key, bool fragment, Gen gen) {
    if (!g_shaderListDone) EnsureShaderList();
    const uint64_t h = HashKey(&key, sizeof(K));
    // (in front of the hash table: the last id seen for each of 64 hash values; the table's
    // lookup costs divisions, and the same few keys come round all frame)
    typename Registry<K>::Front& fr = reg.front[h & 63u];
    if (fr.valid && fr.h == h && SameStruct(reg.keys[fr.id], key)) return fr.id;
    auto range = reg.map.equal_range(h);
    for (auto it = range.first; it != range.second; ++it)
        if (std::memcmp(&it->second.key, &key, sizeof(K)) == 0) {
            fr.h = h; fr.id = it->second.id; fr.valid = true;
            return it->second.id;
        }
    auto src = std::make_unique<ShaderSource>();
    src->fragment = fragment;
    src->id = static_cast<uint16_t>(reg.sources.size());
    src->keyHash = h;
    gen(key, *src);
    g_buf.shaders.push_back(src.get());
    g_buf.cmds.push_back(Cmd{CMD_SHADER, static_cast<uint32_t>(g_buf.shaders.size() - 1)});
    reg.map.emplace(h, typename Registry<K>::Entry{key, src->id});
    reg.keys.push_back(key);
    reg.sources.push_back(std::move(src));
    stats::ShaderCreated(fragment, g_frame);
    VGX_LOG("%s shader %u generated in frame %llu%s\n", fragment ? "fragment" : "vertex", static_cast<unsigned>(reg.sources.size() - 1),
            static_cast<unsigned long long>(g_frame), reg.sources.back()->usesPalette ? " (reads the matrix palette)" : "");
    ListAppend(fragment ? 2u : 1u, &key, sizeof(K));
    fr.h = h; fr.id = static_cast<uint16_t>(reg.sources.size() - 1); fr.valid = true;
    return static_cast<uint16_t>(reg.sources.size() - 1);
}

template <typename K> void ListWriteKeys(FILE* f, const Registry<K>& reg, uint32_t type) {
    std::vector<const K*> byId(reg.sources.size(), nullptr);
    for (const auto& kv : reg.map) byId[kv.second.id] = &kv.second.key;
    for (const K* k : byId) {
        std::fwrite(&type, 4, 1, f);
        std::fwrite(k, sizeof(K), 1, f);
    }
}

void EnsureShaderList() {
    g_shaderListDone = true;
    if (g_shaderListPath.empty()) {
        if (const char* e = std::getenv("MSC_VGX_SHADERLIST")) g_shaderListPath = e;
    }
    if (g_shaderListPath.empty()) return;
    bool rewrite = true; // the file is missing, from another generator, damaged or has duplicates
    uint32_t nVs = 0, nFs = 0, nProg = 0, nVar = 0;
    if (FILE* f = std::fopen(g_shaderListPath.c_str(), "rb")) {
        ListHeader hd;
        if (std::fread(&hd, sizeof hd, 1, f) == 1 && std::memcmp(hd.magic, kListMagic, 8) == 0 && hd.vsKeySize == sizeof(VsKey) &&
            hd.fsKeySize == sizeof(FsKey) && hd.genVersion == kShaderGenVersion) {
            rewrite = false;
            g_shaderListReplay = true;
            std::vector<uint16_t> vsIds, fsIds; // id in this run of the n-th key in the file
            uint32_t type;
            while (std::fread(&type, 4, 1, f) == 1) {
                if (type == 1) {
                    VsKey k;
                    if (std::fread(&k, sizeof k, 1, f) != 1) { rewrite = true; break; }
                    const uint16_t id = Lookup(g_vsReg, k, false, GenerateVertexShader);
                    if (id != vsIds.size()) rewrite = true;
                    vsIds.push_back(id);
                } else if (type == 2) {
                    FsKey k;
                    if (std::fread(&k, sizeof k, 1, f) != 1) { rewrite = true; break; }
                    const uint16_t id = Lookup(g_fsReg, k, true, GenerateFragmentShader);
                    if (id != fsIds.size()) rewrite = true;
                    fsIds.push_back(id);
                } else if (type == 3) {
                    uint32_t v[2];
                    if (std::fread(v, sizeof v, 1, f) != 1 || v[0] >= vsIds.size() || v[1] >= fsIds.size()) { rewrite = true; break; }
                    const uint32_t pk = (static_cast<uint32_t>(vsIds[v[0]]) << 16) | fsIds[v[1]];
                    if (g_progSeen.insert(pk).second) g_buf.cmds.push_back(Cmd{CMD_PROGRAM, pk});
                    else rewrite = true;
                } else if (type == 4) {
                    uint32_t v[4];
                    if (std::fread(v, sizeof v, 1, f) != 1 || v[0] >= vsIds.size() || v[1] >= fsIds.size() || v[2] > 0x1FFFu || v[3] > 0xFFFu) { rewrite = true; break; }
                    if (g_varSeen.insert(VariantKey(vsIds[v[0]], fsIds[v[1]], v[2], v[3])).second) {
                        g_buf.variants.push_back(VariantCmd{vsIds[v[0]], fsIds[v[1]], static_cast<uint16_t>(v[2]), static_cast<uint16_t>(v[3])});
                        g_buf.cmds.push_back(Cmd{CMD_VARIANT, static_cast<uint32_t>(g_buf.variants.size() - 1)});
                        ++nVar;
                    } else {
                        rewrite = true;
                    }
                } else {
                    rewrite = true;
                    break;
                }
            }
            g_shaderListReplay = false;
            nVs = static_cast<uint32_t>(g_vsReg.sources.size());
            nFs = static_cast<uint32_t>(g_fsReg.sources.size());
            nProg = static_cast<uint32_t>(g_progSeen.size());
        }
        std::fclose(f);
    }
    if (rewrite) {
        if (FILE* f = std::fopen(g_shaderListPath.c_str(), "wb")) {
            ListHeader hd{};
            std::memcpy(hd.magic, kListMagic, 8);
            hd.vsKeySize = sizeof(VsKey);
            hd.fsKeySize = sizeof(FsKey);
            hd.genVersion = kShaderGenVersion;
            std::fwrite(&hd, sizeof hd, 1, f);
            ListWriteKeys(f, g_vsReg, 1u);
            ListWriteKeys(f, g_fsReg, 2u);
            std::vector<uint32_t> progs(g_progSeen.begin(), g_progSeen.end());
            std::sort(progs.begin(), progs.end());
            for (uint32_t pk : progs) {
                const uint32_t rec[3] = {3u, pk >> 16, pk & 0xFFFFu};
                std::fwrite(rec, sizeof rec, 1, f);
            }
            std::vector<uint64_t> vars(g_varSeen.begin(), g_varSeen.end());
            std::sort(vars.begin(), vars.end());
            for (uint64_t vk : vars) {
                const uint32_t rec[5] = {4u, static_cast<uint32_t>(vk >> 48), static_cast<uint32_t>(vk >> 32) & 0xFFFFu, static_cast<uint32_t>(vk >> 12) & 0x1FFFu,
                                         static_cast<uint32_t>(vk) & 0xFFFu};
                std::fwrite(rec, sizeof rec, 1, f);
            }
            std::fclose(f);
        }
    }
    g_shaderList = std::fopen(g_shaderListPath.c_str(), "ab");
    VGX_LOG("shader warm-up list %s: %u vertex shaders, %u fragment shaders, %u programs, %u program variants prepared%s%s\n", g_shaderListPath.c_str(), nVs, nFs, nProg, nVar,
            rewrite ? " (file written anew)" : "", g_shaderList ? "" : " (cannot append to it)");
}

// ---- cached resolution of the current state
struct Resolved {
    FsKey fs;
    VsKey vs;
    uint16_t fsId = 0, vsId = 0;
    RenderState rs;
    TexBind tex[8];
    uint8_t texMask = 0;
    bool valid = false;
    bool cullAll = false;
    bool soft = false; // this draw's vertices are transformed to eye space on the CPU
    uint8_t palP = 0; // what the vertex key was built with: position rows of the short palette (0: the full one)
    uint32_t vcdMtxMask = 0;
    uint16_t texW[8] = {}, texH[8] = {};
    // What each texmap resolves to, kept while its binding is unchanged (State::texSlotDirty):
    // a draw that changes one texmap, or only the TEV setup, does not look the others up again.
    uint8_t slotResolved = 0;
    TexBind slotBind[8];
    uint16_t slotW[8] = {}, slotH[8] = {};
} cur;

uint32_t g_blockOff[UB_COUNT] = {kNoBlock, kNoBlock, kNoBlock, kNoBlock, kNoBlock, kNoBlock};
uint32_t g_blockLen[UB_COUNT] = {}; // floats of the block at g_blockOff
// W5 SKIN: is the next EmitDraw / EmitStaticDraw skinned on the GPU (its layout has VL_SKIN; set by them before ResolveState), and the bone block the
// current skin state was pushed as (g_skinBlockSerial: the SkinState::serial it holds; the arena is per frame, so it is stale after a flush).
uint8_t g_pendingSkin = 0;
uint32_t g_skinBlockOff = kNoBlock;
uint32_t g_skinBlockSerial = 0;
// The highest position-matrix row the vertices of the next EmitDraw / EmitStaticDraw use
// (their PNMTXIDX), 0xFF: not known. Decides between the short palette (in its buckets, PalBucketFor) and the full matrix palette.
uint32_t g_pendingPosMtxMax = 0xFF;
bool g_blocksStale = true; // after a flush nothing is in the arena any more

// ---- palette buckets (W5 LOOK2). A skinned mesh reads the matrix rows its vertices name; the console copies a program's whole uniform buffer for every draw, so a mesh of
// a few bones should have a small palette. The short palette (position rows 0..P-1 then normal rows 0..P-1) comes in a few sizes P; a draw gets the smallest that holds its
// highest matrix row R plus the matrix's three rows (P >= R + 3). One program per (vertex shader key, P). The sizes are an ascending list that ends in 30 (the one size there was):
//   MSC_VGX_PALBUCKET unset, 0 or off: {30} (what the build did before: its draw hashes and shader lists are unchanged); a,b,c,30: explicit (each 3..30), e.g. 12,30.
//   DEFAULT OFF (measured, nav_match60 open play, glnull census per frame): {12,30} takes -4.8 % ring bytes (338 -> 322 KB) and -7.2 % uniform bytes, but program uses go 62 -> 98 (+57 %),
//   uniform calls +5 %, and 19 more programs to compile and seed; {4,8,16,30}: -4.1 % ring, 105 uses; {6,12,18,24,30}: -6.3 % ring, 113 uses. 56 % of the primitives with per-vertex matrices reach
//   row 27 (they need the whole palette) and only 24 % stay within rows 0..9, so the saving is small, and every size change is a program switch. Worth switching on only if a hardware A/B says so.
//   MSC_VGX_PALHIST=1: census of the highest row of every primitive that uses per-vertex matrices and of the size it got (frame 1 and every 600 frames).
struct PalBuckets {
    uint8_t p[8];
    int n = 0;
    PalBuckets() {
        const char* e = std::getenv("MSC_VGX_PALBUCKET");
        n = 0;
        if (!e || !*e || !std::strcmp(e, "0") || !std::strcmp(e, "off")) { p[n++] = kPalShortPos; return; }
        int prev = 2;
        while (*e && n < 7) {
            char* end = nullptr;
            const long v = std::strtol(e, &end, 10);
            if (end == e || v <= prev || v > kPalShortPos) break;
            p[n++] = static_cast<uint8_t>(v);
            prev = static_cast<int>(v);
            e = *end == ',' ? end + 1 : end;
        }
        if (n == 0 || p[n - 1] != kPalShortPos) p[n++] = kPalShortPos; // (the last size is always the whole short palette)
    }
};
const PalBuckets& PalBucketList() {
    static const PalBuckets b;
    return b;
}
// The position rows of the short palette for a draw whose highest matrix row is `maxRow`; 0 when the short palette does not hold it (the full palette is used).
inline uint8_t PalBucketFor(uint32_t maxRow) {
    if (maxRow > static_cast<uint32_t>(kPalShortMaxRow)) return 0;
    const PalBuckets& b = PalBucketList();
    for (int i = 0; i < b.n; ++i)
        if (maxRow + 3 <= b.p[i]) return b.p[i];
    return 0;
}
struct PalHist {
    uint64_t byRow[66] = {}; // [row] for rows 0..63, [64] 64..254, [65] not known (0xFF)
    uint64_t bySize[32] = {}; // [P] (0: full palette)
    uint64_t prims = 0, keyChanges = 0;
    uint8_t last = 0xFF;
} g_palHist;
inline bool PalHistOn() {
    static const bool on = EnvInt("MSC_VGX_PALHIST", 0) != 0;
    return on;
}
void PalHistNote(uint32_t maxRow, uint8_t palP) {
    ++g_palHist.prims;
    ++g_palHist.byRow[maxRow < 64 ? maxRow : maxRow == 0xFF ? 65 : 64];
    ++g_palHist.bySize[palP];
    if (palP != g_palHist.last) ++g_palHist.keyChanges;
    g_palHist.last = palP;
}

// ---- GX Z-freeze (MSC_VGX_ZFREEZE, default off; see vgx_zfreeze.h)
// The reference triangle: the last triangle of the last draw made with Z-freeze off, kept in model space with the matrices and
// viewport it was drawn with (a copy of some fifty floats per draw; the plane is only worked out when a frozen draw needs it).
// A culled draw counts, and a draw whose vertices this renderer cannot transform here (matrix per vertex, display-list
// geometry) takes the reference away: the frozen draws that follow then behave as before (their own depth).
bool ZFreezeOn() {
    static const bool on = EnvInt("MSC_VGX_ZFREEZE", 0) != 0;
    return on;
}
bool ZFreezeTracking() { // the reference is followed: for the effect, or for MSC_VGX_ZFTRACE (the numbers without the effect)
    static const bool on = ZFreezeOn() || EnvInt("MSC_VGX_ZFTRACE", 0) != 0;
    return on;
}
struct ZfRef {
    bool have = false;
    float pos[3][3] = {};
    float mv[12] = {};
    float proj[16] = {};
    zfreeze::Viewport vp;
    uint32_t version = 0;       // counts changes of the reference
    uint32_t planeVersion = ~0u; // the version `plane` was worked out for
    bool planeOk = false;
    zfreeze::Plane plane;
} g_zf;
bool ZFreezePlane(zfreeze::Plane& out) {
    if (!g_zf.have) return false;
    if (g_zf.planeVersion != g_zf.version) {
        float clip[3][4];
        for (int i = 0; i < 3; ++i) zfreeze::ModelToClip(g_zf.pos[i], g_zf.mv, g_zf.proj, clip[i]);
        g_zf.planeOk = zfreeze::PlaneFromClip(clip, g_zf.vp, g_zf.plane);
        g_zf.planeVersion = g_zf.version;
    }
    out = g_zf.plane;
    return g_zf.planeOk;
}

size_t g_pendingVtxStart = 0;
int g_lastDraw = -1; // index of the draw that may still be merged into

inline bool EfbHasAlpha() { return (g.bp[0x43] & 7) == 1; }

uint8_t SwapTable(int t) {
    const uint32_t rg = g.bp[0xF6 + 2 * t], ba = g.bp[0xF7 + 2 * t];
    return static_cast<uint8_t>((rg & 3) | (((rg >> 2) & 3) << 2) | ((ba & 3) << 4) | (((ba >> 2) & 3) << 6));
}

GXD_NI void BuildFsKey(FsKey& k) {
    std::memset(&k, 0, sizeof k);
    const uint32_t gm = g.bp[0x00];
    k.numStages = static_cast<uint8_t>(bits(gm, 10, 4) + 1);
    k.numTexGens = static_cast<uint8_t>(bits(gm, 0, 4));
    if (k.numTexGens > 8) k.numTexGens = 8;
    k.numIndStages = static_cast<uint8_t>(bits(gm, 16, 3));
    if (k.numIndStages > 4) k.numIndStages = 4;
    bool anyInd = false;
    for (int s = 0; s < k.numStages; ++s) {
        FsKey::Stage& st = k.stage[s];
        const uint32_t tevc = g.bp[0xC0 + 2 * s], teva = g.bp[0xC1 + 2 * s];
        const uint32_t tref = g.bp[0x28 + (s >> 1)] >> ((s & 1) * 12);
        const uint32_t ksel = g.bp[0xF6 + (s >> 1)];
        st.color = tevc & 0x00FFFFFFu;
        st.alpha = teva & 0x00FFFFF0u;
        bool usesTex = false, usesRas = false, usesKc = false, usesKa = false;
        for (int i = 0; i < 4; ++i) {
            const uint32_t c = bits(tevc, i * 4, 4);
            if (c == 8 || c == 9) usesTex = true;
            if (c == 10 || c == 11) usesRas = true;
            if (c == 14) usesKc = true;
            const uint32_t a = bits(teva, 4 + i * 3, 3);
            if (a == 4) usesTex = true;
            if (a == 5) usesRas = true;
            if (a == 6) usesKa = true;
        }
        st.rasChan = 7;
        st.rasSwap = st.texSwap = 0xE4;
        if (k.numIndStages) {
            const uint32_t ind = g.bp[0x10 + s] & 0x1FFFFFu;
            // A stage is "direct" when it has no matrix, no bias and no wrapping.
            if (bits(ind, 9, 4) != 0 || bits(ind, 4, 3) != 0 || bits(ind, 13, 6) != 0 || bits(ind, 20, 1) != 0 ||
                bits(ind, 7, 2) != 0) {
                st.ind = ind;
                anyInd = true;
            }
        }
        if (usesTex || st.ind) st.texCoord = static_cast<uint8_t>(bits(tref, 3, 3));
        if (usesTex && bits(tref, 6, 1)) {
            st.texEnable = 1;
            st.texMap = static_cast<uint8_t>(bits(tref, 0, 3));
            st.texSwap = SwapTable(bits(teva, 2, 2));
        }
        if (usesRas) {
            st.rasChan = static_cast<uint8_t>(bits(tref, 7, 3));
            st.rasSwap = SwapTable(bits(teva, 0, 2));
        }
        if (usesKc) st.kcsel = static_cast<uint8_t>(bits(ksel, (s & 1) ? 14 : 4, 5));
        if (usesKa) st.kasel = static_cast<uint8_t>(bits(ksel, (s & 1) ? 19 : 9, 5));
    }
    if (!anyInd) k.numIndStages = 0;
    for (int i = 0; i < k.numIndStages; ++i) {
        const uint32_t r = g.bp[0x27] >> (i * 6);
        k.ind[i].texMap = static_cast<uint8_t>(bits(r, 0, 3));
        k.ind[i].texCoord = static_cast<uint8_t>(bits(r, 3, 3));
    }
    const uint32_t ac = g.bp[0xF3];
    const uint32_t c0 = bits(ac, 16, 3), c1 = bits(ac, 19, 3), op = bits(ac, 22, 2);
    // Fold the cases that always pass.
    const bool pass = (op == 0 && c0 == 7 && c1 == 7) || (op == 1 && (c0 == 7 || c1 == 7)) || (op == 3 && c0 == 7 && c1 == 7) ||
                      (op == 2 && ((c0 == 7 && c1 == 0) || (c0 == 0 && c1 == 7)));
    if (!pass) {
        k.alphaTestOn = 1;
        k.alphaTest = static_cast<uint8_t>(c0 | (c1 << 3) | (op << 6));
    }
    const uint32_t fsel = bits(g.bp[0xF1], 21, 3);
    if (fsel) k.fog = static_cast<uint8_t>(fsel | (bits(g.bp[0xF1], 20, 1) << 3));
    const uint32_t zop = bits(g.bp[0xF5], 2, 2);
    if (zop) {
        stats::Note("z-texture (GXSetZTexture) requested: NOT implemented");
    }
    for (int i = 0; i < k.numTexGens; ++i)
        if (bits(g.xfReg[0x40 + i], 1, 1)) k.texProjMask |= static_cast<uint8_t>(1u << i);
    const uint32_t cm0 = g.bp[0x41];
    const bool blending = bits(cm0, 0, 1) || bits(cm0, 11, 1);
    if (bits(g.bp[0x42], 8, 1) && EfbHasAlpha() && bits(cm0, 4, 1)) {
        if (!blending) k.dstAlpha = 1;
        else stats::Note("GXSetDstAlpha together with blending: constant alpha NOT written (needs a second pass)");
    }
    // Z-freeze needs a reference; without one (nothing drawn yet, or the last triangle could not be followed) the draw keeps
    // its own depth, as before.
    if (ZFreezeOn() && (g.bp[0x00] & (1u << 19)) && g_zf.have) k.zfreeze = 1;
}

GXD_NI void BuildVsKey(VsKey& k, uint8_t numTexGens, uint8_t palP) {
    std::memset(&k, 0, sizeof k);
    k.numTexGens = numTexGens;
    k.numChans = static_cast<uint8_t>(g.xfReg[0x09] & 3);
    if (k.numChans > 2) k.numChans = 2;
    k.perVtxPos = g.vcd[0] ? 1 : 0;
    for (int i = 0; i < 4; ++i) {
        const int chanIdx = i & 1; // 0,1 colour; 2,3 alpha
        if (chanIdx >= k.numChans) continue;
        const uint32_t r = g.xfReg[0x0E + i];
        VsKey::Chan& c = k.chan[i];
        c.matSrcVtx = static_cast<uint8_t>(bits(r, 0, 1));
        c.enable = static_cast<uint8_t>(bits(r, 1, 1));
        if (c.enable) {
            c.ambSrcVtx = static_cast<uint8_t>(bits(r, 6, 1));
            c.lightMask = static_cast<uint8_t>(bits(r, 2, 4) | (bits(r, 11, 4) << 4));
            if (c.lightMask) {
                c.diffuse = static_cast<uint8_t>(bits(r, 7, 2));
                if (c.diffuse > 2) c.diffuse = 2;
                c.attn = bits(r, 9, 1) ? (bits(r, 10, 1) ? 2 : 1) : 0;
            }
        }
    }
    const bool dual = (g.xfReg[0x12] & 1) != 0;
    for (int i = 0; i < numTexGens; ++i) {
        const uint32_t r = g.xfReg[0x40 + i], d = g.xfReg[0x50 + i];
        VsKey::TexGen& t = k.tg[i];
        t.type = static_cast<uint8_t>(bits(r, 4, 3));
        if (t.type > 3) t.type = 0;
        t.src = static_cast<uint8_t>(bits(r, 7, 5));
        if (t.type >= 2) { t.src = 2; }
        else {
            t.proj = static_cast<uint8_t>(bits(r, 1, 1));
            t.inputAbc1 = static_cast<uint8_t>(bits(r, 2, 1));
            t.perVtxMtx = g.vcd[1 + i] ? 1 : 0;
            if (t.type == 0 && dual) {
                const bool norm = bits(d, 8, 1) != 0;
                if (norm) t.post = 2;
                else if (bits(d, 0, 6) != 61) t.post = 1;
            }
            if (t.type == 1) stats::Note("emboss (bump) texgen requested: NOT implemented (treated as regular)");
            if (t.src == 3 || t.src == 4) stats::Note("binormal/tangent texgen source: NOT implemented");
        }
    }
    // The short palette holds position rows 0..29 and their normal rows: enough when the
    // vertices use no higher row and no texture matrix comes from the palette.
    if (k.perVtxPos && palP) {
        bool texFromPal = false;
        for (int i = 0; i < numTexGens; ++i) texFromPal |= k.tg[i].perVtxMtx != 0;
        if (!texFromPal) {
            k.perVtxPos = 2;
            k.palRows = palP == kPalShortPos ? 0 : palP; // (30 stays what a key was before the buckets, so seeds and lists made then still match)
        }
    }
    k.chan[0].skin = g_pendingSkin; // W5 SKIN (0 for every draw that is not skinned on the GPU: the keys of all other draws are unchanged)
}

uint8_t MapSrcFactor(uint32_t f, bool hasAlpha) {
    switch (f) {
    case 0: return BF_ZERO;
    case 1: return BF_ONE;
    case 2: return BF_DST_COLOR;
    case 3: return BF_INV_DST_COLOR;
    case 4: return BF_SRC_ALPHA;
    case 5: return BF_INV_SRC_ALPHA;
    case 6: return hasAlpha ? BF_DST_ALPHA : BF_ONE;
    default: return hasAlpha ? BF_INV_DST_ALPHA : BF_ZERO;
    }
}
uint8_t MapDstFactor(uint32_t f, bool hasAlpha) {
    switch (f) {
    case 0: return BF_ZERO;
    case 1: return BF_ONE;
    case 2: return BF_SRC_COLOR;
    case 3: return BF_INV_SRC_COLOR;
    case 4: return BF_SRC_ALPHA;
    case 5: return BF_INV_SRC_ALPHA;
    case 6: return hasAlpha ? BF_DST_ALPHA : BF_ONE;
    default: return hasAlpha ? BF_INV_DST_ALPHA : BF_ZERO;
    }
}

GXD_NI void BuildRenderState(RenderState& rs) {
    rs = RenderState{};
    const uint32_t cm0 = g.bp[0x41], zm = g.bp[0x40], gm = g.bp[0x00];
    const bool hasAlpha = EfbHasAlpha();
    if (bits(cm0, 11, 1)) { // subtract: dst - src
        rs.blend = 1; rs.blendEq = BE_REVSUB; rs.blendSrc = BF_ONE; rs.blendDst = BF_ONE;
    } else if (bits(cm0, 0, 1)) {
        rs.blend = 1;
        rs.blendSrc = MapSrcFactor(bits(cm0, 8, 3), hasAlpha);
        rs.blendDst = MapDstFactor(bits(cm0, 5, 3), hasAlpha);
        if (rs.blendSrc == BF_ONE && rs.blendDst == BF_ZERO) rs.blend = 0;
    } else if (bits(cm0, 1, 1)) {
        // Logic ops do not exist on the target GPU API; approximate with blending
        // (same table Dolphin uses for back ends without logic op support).
        static const uint8_t lo[16][2] = {
            {BF_ZERO, BF_ZERO}, {BF_DST_COLOR, BF_ZERO}, {BF_ONE, BF_INV_SRC_COLOR}, {BF_ONE, BF_ZERO},
            {BF_DST_COLOR, BF_ONE}, {BF_ZERO, BF_ONE}, {BF_INV_DST_COLOR, BF_INV_SRC_COLOR}, {BF_INV_DST_COLOR, BF_ONE},
            {BF_INV_SRC_COLOR, BF_INV_DST_COLOR}, {BF_INV_SRC_COLOR, BF_SRC_COLOR}, {BF_INV_DST_COLOR, BF_INV_DST_COLOR},
            {BF_ONE, BF_INV_DST_ALPHA}, {BF_INV_SRC_COLOR, BF_INV_SRC_COLOR}, {BF_INV_SRC_COLOR, BF_ONE},
            {BF_INV_DST_COLOR, BF_INV_SRC_COLOR}, {BF_ONE, BF_ONE}};
        const uint32_t op = bits(cm0, 12, 4);
        if (op != 3) {
            rs.blend = 1; rs.blendSrc = lo[op][0]; rs.blendDst = lo[op][1];
            stats::Note("logic-op blend mode used (approximated with blend factors)");
        }
    }
    rs.colorWrite = static_cast<uint8_t>(bits(cm0, 3, 1));
    rs.alphaWrite = static_cast<uint8_t>(bits(cm0, 4, 1) && hasAlpha);
    if (bits(zm, 0, 1)) {
        rs.depthTest = 1;
        rs.depthFunc = static_cast<uint8_t>(bits(zm, 1, 3));
        rs.depthWrite = static_cast<uint8_t>(bits(zm, 4, 1));
    }
    switch (bits(gm, 14, 2)) {
    case 1: rs.cull = CULL_BACK; break;
    case 2: rs.cull = CULL_FRONT; break;
    case 3: rs.cull = CULL_ALL; break;
    default: rs.cull = CULL_NONE; break;
    }
    rs.lineWidth6 = static_cast<uint8_t>(bits(g.bp[0x22], 0, 8));
    // Scissor (BP 0x20/0x21) and its offset (0x59), which also shifts the viewport.
    const int xoff = static_cast<int>(bits(g.bp[0x59], 0, 10)) * 2, yoff = static_cast<int>(bits(g.bp[0x59], 10, 10)) * 2;
    const int y0 = static_cast<int>(bits(g.bp[0x20], 0, 11)) - yoff, x0 = static_cast<int>(bits(g.bp[0x20], 12, 11)) - xoff;
    const int y1 = static_cast<int>(bits(g.bp[0x21], 0, 11)) - yoff, x1 = static_cast<int>(bits(g.bp[0x21], 12, 11)) - xoff;
    rs.scX = static_cast<int16_t>(x0); rs.scY = static_cast<int16_t>(y0);
    rs.scW = static_cast<int16_t>(x1 - x0 + 1); rs.scH = static_cast<int16_t>(y1 - y0 + 1);
    const float sx = u2f(g.xfReg[0x1A]), sy = u2f(g.xfReg[0x1B]), sz = u2f(g.xfReg[0x1C]);
    const float ox = u2f(g.xfReg[0x1D]), oy = u2f(g.xfReg[0x1E]), oz = u2f(g.xfReg[0x1F]);
    rs.vpW = 2.0f * sx; rs.vpH = -2.0f * sy;
    rs.vpX = ox - sx - static_cast<float>(xoff);
    rs.vpY = oy + sy - static_cast<float>(yoff);
    rs.zFar = oz / 16777215.0f;
    rs.zNear = (oz - sz) / 16777215.0f;
    if (rs.zNear < 0) rs.zNear = 0;
    if (rs.zFar > 1) rs.zFar = 1;
}

// ---- uniform blocks
// Appends block b (n floats) to the frame's uniform arena unless the block the
// current draws refer to already holds exactly these values.
void PushBlock(int b, const float* data, uint32_t n) {
    if (!g_blocksStale && g_blockOff[b] != kNoBlock && g_blockLen[b] == n && SameWords(g_buf.uni.data() + g_blockOff[b], data, n)) return;
    const size_t at = g_buf.uni.size();
    g_blockOff[b] = static_cast<uint32_t>(at);
    g_blockLen[b] = n;
    if (Gxd(GXD_SMALL)) {
        g_buf.uni.resize(at + n); // (NoInitAlloc: nothing is cleared; insert() was about 100 instructions with its memmove call)
        CopyWords(g_buf.uni.data() + at, data, n);
    } else {
        g_buf.uni.insert(g_buf.uni.end(), data, data + n);
    }
}

GXD_NI void BuildProj() {
    float p[6];
    for (int i = 0; i < 6; ++i) p[i] = u2f(g.xfReg[0x20 + i]);
    float m[16] = {};
    // Column-major. Y is flipped (the EFB is rendered top row first, see NOTES)
    // and GX clip z in [-w, 0] is remapped to GL's [-w, w].
    if ((g.xfReg[0x26] & 1) == 0) { // perspective
        m[0] = p[0]; m[8] = p[1];
        m[5] = -p[2]; m[9] = -p[3];
        m[10] = 2.0f * p[4] - 1.0f; m[14] = 2.0f * p[5];
        m[11] = -1.0f;
    } else {
        m[0] = p[0]; m[12] = p[1];
        m[5] = -p[2]; m[13] = -p[3];
        m[10] = 2.0f * p[4]; m[14] = 2.0f * p[5] + 1.0f;
        m[15] = 1.0f;
    }
    PushBlock(UB_PROJ, m, 16);
}
GXD_NI void BuildMtx(const VsKey& vs, bool soft) {
    float mv[24] = {};
    const uint32_t pi = g.matIdxA & 0x3F;
    if (soft) {
        // Vertices arrive in eye space already (see SoftTransform).
        mv[0] = mv[5] = mv[10] = 1.0f;
        mv[12] = mv[17] = mv[22] = 1.0f;
    } else {
        for (int r = 0; r < 3; ++r) {
            const uint32_t row = pi + r < 64 ? pi + r : 63;
            std::memcpy(&mv[r * 4], g.xfPos[row], 16);
            const uint32_t nrow = (pi + r) & 31;
            std::memcpy(&mv[12 + r * 4], g.xfNrm[nrow], 12);
        }
    }
    PushBlock(UB_MV, mv, 24);
    // Texture matrices [0, 96) and post-transform matrices [96, 192), 12 floats per texgen.
    // Packed: only up to the last texgen, and the second half only when a texgen uses it.
    const bool pack = UniformPacking();
    bool anyPost = false;
    for (int i = 0; i < vs.numTexGens; ++i) anyPost |= vs.tg[i].type < 2 && vs.tg[i].post != 0;
    const uint32_t tmLen = !pack ? 192u : (anyPost ? 96u + vs.numTexGens * 12u : vs.numTexGens * 12u);
    float tm[192];
    if (!pack) std::memset(tm, 0, sizeof tm);
    else if (anyPost) std::memset(tm + vs.numTexGens * 12, 0, (96 - vs.numTexGens * 12) * sizeof(float));
    for (int i = 0; i < vs.numTexGens; ++i) {
        const uint32_t ti = i < 4 ? bits(g.matIdxA, 6 + 6 * i, 6) : bits(g.matIdxB, 6 * (i - 4), 6);
        const uint32_t po = g.xfReg[0x50 + i] & 0x3F;
        for (int r = 0; r < 3; ++r) {
            std::memcpy(&tm[i * 12 + r * 4], g.xfPos[ti + r < 64 ? ti + r : 63], 16);
            if (!pack || anyPost) std::memcpy(&tm[96 + i * 12 + r * 4], g.xfPost[po + r < 64 ? po + r : 63], 16);
        }
    }
    if (tmLen) PushBlock(UB_TEXM, tm, tmLen);
    else g_blockOff[UB_TEXM] = kNoBlock;
    bool pal = vs.perVtxPos != 0;
    for (int i = 0; i < vs.numTexGens; ++i) pal |= vs.tg[i].perVtxMtx != 0;
    if (pal) {
        float pb[384];
        if (vs.perVtxPos == 2) {
            // Short palette: position rows 0..P-1, then normal rows 0..P-1 (P = 30 whole, or a bucket: PalBucketFor).
            const int P = PalPosRows(vs);
            std::memcpy(pb, g.xfPos, static_cast<size_t>(P) * 4 * sizeof(float));
            for (int r = 0; r < P; ++r) {
                float* q = pb + (P + r) * 4;
                q[0] = g.xfNrm[r][0]; q[1] = g.xfNrm[r][1]; q[2] = g.xfNrm[r][2]; q[3] = 0;
            }
            PushBlock(UB_PAL, pb, static_cast<uint32_t>(2 * P) * 4);
        } else {
            std::memcpy(pb, g.xfPos, 256 * 4);
            for (int r = 0; r < 32; ++r) {
                pb[256 + r * 4] = g.xfNrm[r][0]; pb[257 + r * 4] = g.xfNrm[r][1]; pb[258 + r * 4] = g.xfNrm[r][2]; pb[259 + r * 4] = 0;
            }
            PushBlock(UB_PAL, pb, kPalRowsFull * 4);
        }
    }
}
// GXD_SMALL: float(i) / 255.0f for i in [-1024, 1023] from a table made with that very division (a VFP division is 15+ cycles and not
// pipelined on a Cortex-A9; a BuildTev has 32 of them, a BuildLight 40 or more). Bit-identical results.
static float g_d255[2048];
static const float* D255() {
    static bool init = false;
    if (!init) {
        for (int i = 0; i < 2048; ++i) g_d255[i] = static_cast<float>(i - 1024) / 255.0f;
        init = true;
    }
    return g_d255 + 1024;
}
inline void Rgba8ToFloat(uint32_t c, float* o) {
    if (Gxd(GXD_SMALL)) {
        const float* t = D255();
        o[0] = t[(c >> 24) & 0xFF]; o[1] = t[(c >> 16) & 0xFF]; o[2] = t[(c >> 8) & 0xFF]; o[3] = t[c & 0xFF];
        return;
    }
    o[0] = static_cast<float>((c >> 24) & 0xFF) / 255.0f; o[1] = static_cast<float>((c >> 16) & 0xFF) / 255.0f;
    o[2] = static_cast<float>((c >> 8) & 0xFF) / 255.0f; o[3] = static_cast<float>(c & 0xFF) / 255.0f;
}
// MSC_VGX_LIGHTNORM (default 1; 0 = the registers as loaded): a light's DIRECTION goes into the
// block as zero unless the vertex shader of the draw can produce a different result for a
// different direction. The game initialises the direction only of lights that need one; for
// the others the register holds whatever was in the stack frame of the function that built
// the GXLightObj, and that differs between builds of the game code (found by the integration
// worker: wt/integ/NOTES.md, 5 Oct 06:05; pointer-like words with the plain translation, NaN
// and infinity patterns in 558 of 8457 survey frames with the optimised one). Two reasons:
// (1) the generated shader computes `ca = max(0.0, dot(ld, dir))` and multiplies it by the
//     light's zero coefficients; a non-finite or very large `dir` makes that NaN (inf * 0)
//     on any GPU, and the colour of the vertex with it;
// (2) the frame hash (MSC_VGX_HASH) must not depend on dead stack bytes.
// What "can depend" means follows GenerateVertexShader (vgx_shadergen.cpp), which is the only
// reader of the block: the direction (u_lt[b + 1]) is read by channels with attn 2 (spot),
// where it only reaches the result through cosatt.y / cosatt.z, and with attn 1 (specular),
// where it reaches it through cosatt.y / .z and distatt.y / .z. Channels with attn 0 and
// lights outside every channel's mask do not read it. The block is built per vertex key
// (see ResolveState), so the decision may use the key.
bool LightNorm() {
    static const bool on = EnvInt("MSC_VGX_LIGHTNORM", 1) != 0;
    return on;
}
// MSC_VGX_LIGHTNORM=2: census, one line every 600 frames and at frame 1 (counts since start).
struct LightNormCensus {
    uint64_t blocks = 0, zeroed = 0, zeroedNonFinite = 0, zeroedUnread = 0, kept = 0, keptNonFinite = 0;
} g_lnc;
bool LightNormCensusOn() {
    static const bool on = EnvInt("MSC_VGX_LIGHTNORM", 1) == 2;
    return on;
}
inline bool NonFiniteDir(const uint32_t* L) {
    return (L[13] & 0x7F800000u) == 0x7F800000u || (L[14] & 0x7F800000u) == 0x7F800000u || (L[15] & 0x7F800000u) == 0x7F800000u;
}
// MSC_VGX_STREAMCENSUS=1 (diagnostic, W5 GXD; nothing is changed): what the per-frame vertex stream (CmdBuf::vtx, the bytes the back end uploads
// every frame) is made of. Per layout of the decoded vertices: draws, vertices, bytes, and how many of those bytes belong to an attribute the vertex
// shader of the draw does not read (the same rules as GenerateVertexShader: a_nrm for lit channels and normal texgens, a_col0 / a_col1, a_texN for the
// texgen sources, a_tmi, and a_pos.w when no palette row is named). One line every 600 frames (per-frame means over those frames) and at frame 1.
bool StreamCensusOn() {
    static const bool on = EnvInt("MSC_VGX_STREAMCENSUS", 0) != 0;
    return on;
}
} // namespace
bool g_streamCensusList = false; // set by the display-list cache while it replays a cached list into the stream (census only)
namespace {
struct StreamCensus {
    struct Rec { uint64_t draws = 0, verts = 0, bytes = 0, soft = 0, deadNrm = 0, deadCol = 0, deadTex = 0, deadTmi = 0, deadPosW = 0, listVerts = 0, listBytes = 0, smallVerts = 0, smallBytes = 0; };
    std::map<uint32_t, Rec> byLayout;
    uint64_t frames = 0;
} g_sc;
// Does the vertex shader of the key read a normal (and so the normal-matrix rows u_mv[3..5] / the palette's normal rows)? The rule of GenerateVertexShader.
static bool VsReadsNormal(const VsKey& k) {
    for (int i = 0; i < 4; ++i) {
        const VsKey::Chan& c = k.chan[i];
        if ((i & 1) < k.numChans && c.enable && c.lightMask && (c.diffuse || c.attn == 1)) return true;
    }
    for (int i = 0; i < k.numTexGens; ++i)
        if (k.tg[i].type < 2 && k.tg[i].src == 1) return true;
    return false;
}
void StreamCensusDraw(const VsKey& k, uint32_t mask, uint32_t count, bool soft) {
    bool needNrm = false, needCol[2] = {false, false}, needTmi = false;
    uint8_t texSrcMask = 0;
    for (int i = 0; i < 4; ++i) {
        const VsKey::Chan& c = k.chan[i];
        if ((i & 1) >= k.numChans) continue;
        if (c.matSrcVtx || (c.enable && c.ambSrcVtx)) needCol[i & 1] = true;
        if (c.enable && c.lightMask && (c.diffuse || c.attn == 1)) needNrm = true;
    }
    if (k.numChans < 1) needCol[0] = true;
    if (k.numChans < 2) needCol[1] = true;
    for (int i = 0; i < k.numTexGens; ++i) {
        const VsKey::TexGen& t = k.tg[i];
        if (t.type >= 2) continue;
        if (t.src == 1) needNrm = true;
        if (t.src >= 5 && t.src <= 12) texSrcMask |= static_cast<uint8_t>(1u << (t.src - 5));
        if (t.perVtxMtx) needTmi = true;
    }
    StreamCensus::Rec& r = g_sc.byLayout[mask];
    const VtxLayout L = MakeLayout(mask);
    ++r.draws;
    r.verts += count;
    r.bytes += static_cast<uint64_t>(count) * L.stride;
    if (soft) r.soft += count;
    if (g_streamCensusList) {
        r.listVerts += count;
        r.listBytes += static_cast<uint64_t>(count) * L.stride;
        if (!soft && count <= SoftXformLimit()) { r.smallVerts += count; r.smallBytes += static_cast<uint64_t>(count) * L.stride; } // list draws a promoted buffer could hold
    }
    if ((mask & VL_NRM) && !needNrm) r.deadNrm += static_cast<uint64_t>(count) * 12;
    if (((mask & VL_COL0) && !needCol[0])) r.deadCol += static_cast<uint64_t>(count) * 4;
    if (((mask & VL_COL1) && !needCol[1])) r.deadCol += static_cast<uint64_t>(count) * 4;
    for (int t = 0; t < 8; ++t)
        if ((mask & (VL_TEX0 << t)) && !(texSrcMask & (1u << t))) r.deadTex += static_cast<uint64_t>(count) * 8;
    if ((mask & VL_TMI) && !needTmi) r.deadTmi += static_cast<uint64_t>(count) * 8;
    if (!k.perVtxPos) r.deadPosW += static_cast<uint64_t>(count) * 4;
}
void StreamCensusFrame(uint64_t frame) {
    ++g_sc.frames;
    if (!(frame == 1 || frame % 600 == 0)) return;
    StreamCensus::Rec t;
    std::vector<std::pair<uint64_t, uint32_t>> order;
    for (const auto& kv : g_sc.byLayout) {
        const StreamCensus::Rec& r = kv.second;
        t.draws += r.draws; t.verts += r.verts; t.bytes += r.bytes; t.soft += r.soft; t.deadNrm += r.deadNrm; t.deadCol += r.deadCol;
        t.deadTex += r.deadTex; t.deadTmi += r.deadTmi; t.deadPosW += r.deadPosW; t.listVerts += r.listVerts; t.listBytes += r.listBytes; t.smallVerts += r.smallVerts; t.smallBytes += r.smallBytes;
        order.push_back({r.bytes, kv.first});
    }
    const double f = g_sc.frames ? static_cast<double>(g_sc.frames) : 1.0;
    std::fprintf(stderr, "[streamcensus] frame %llu, per frame over %llu frames: %.0f draws %.0f vertices %.0f bytes (%.0f soft-transformed vertices); bytes of attributes the shader does not read: normal %.0f, colour %.0f, texcoord %.0f, tmi %.0f, pos.w %.0f\n",
                 static_cast<unsigned long long>(frame), static_cast<unsigned long long>(g_sc.frames), t.draws / f, t.verts / f, t.bytes / f, t.soft / f,
                 t.deadNrm / f, t.deadCol / f, t.deadTex / f, t.deadTmi / f, t.deadPosW / f);
    std::fprintf(stderr, "[streamcensus]   from replayed display lists: %.0f vertices %.0f bytes; of those in draws of at most %u vertices that were not soft-transformed: %.0f vertices %.0f bytes\n",
                 t.listVerts / f, t.listBytes / f, SoftXformLimit(), t.smallVerts / f, t.smallBytes / f);
    std::sort(order.begin(), order.end(), [](const std::pair<uint64_t, uint32_t>& a, const std::pair<uint64_t, uint32_t>& b) { return a.first > b.first; });
    for (size_t i = 0; i < order.size() && i < 12; ++i) {
        const StreamCensus::Rec& r = g_sc.byLayout[order[i].second];
        std::fprintf(stderr, "[streamcensus]   layout 0x%03X stride %u: %.1f draws %.0f vertices %.0f bytes (soft %.0f, from lists %.0f, of those small and not soft %.0f) dead: nrm %.0f col %.0f tex %.0f tmi %.0f posw %.0f\n",
                     order[i].second, MakeLayout(order[i].second).stride, r.draws / f, r.verts / f, r.bytes / f, r.soft / f, r.listVerts / f, r.smallVerts / f, r.deadNrm / f, r.deadCol / f, r.deadTex / f,
                     r.deadTmi / f, r.deadPosW / f);
    }
    g_sc.byLayout.clear();
    g_sc.frames = 0;
}
GXD_NI void BuildLight(const VsKey& vs) {
    // amb 2 x vec4, mat 2 x vec4, then per light {pos, dir, col, cosatt, distatt}. Packed: only
    // the lights up to the last one the shader uses.
    const int nvec = UniformPacking() ? VsLightCount(vs) : 44;
    const int nLights = (nvec - 4) / 5;
    float lb[176];
    Rgba8ToFloat(g.xfReg[0x0A], &lb[0]); Rgba8ToFloat(g.xfReg[0x0B], &lb[4]);
    Rgba8ToFloat(g.xfReg[0x0C], &lb[8]); Rgba8ToFloat(g.xfReg[0x0D], &lb[12]);
    uint32_t spotUse = 0, specUse = 0, allUse = 0xFF; // bit l: some channel reads light l's direction that way
    if (LightNorm()) {
        allUse = 0;
        for (int i = 0; i < 4; ++i) {
            const VsKey::Chan& c = vs.chan[i];
            if ((i & 1) >= vs.numChans || !c.enable) continue;
            if (c.attn == 2) spotUse |= c.lightMask;
            else if (c.attn == 1) specUse |= c.lightMask;
        }
    }
    for (int i = 0; i < nLights; ++i) {
        const uint32_t* L = g.xfLight[i];
        float* o = &lb[16 + i * 20];
        for (int c = 0; c < 3; ++c) {
            o[c] = u2f(L[10 + c]);      // pos
            o[4 + c] = u2f(L[13 + c]);  // dir
            o[12 + c] = u2f(L[4 + c]);  // cos attenuation
            o[16 + c] = u2f(L[7 + c]);  // distance attenuation
        }
        o[3] = o[7] = o[15] = o[19] = 0.0f;
        Rgba8ToFloat(L[3], &o[8]);
        if (!(allUse & (1u << i))) {
            // (+0 and -0 both count as zero: bits without the sign)
            const bool cosConst = ((L[5] | L[6]) << 1) == 0, distConst = ((L[8] | L[9]) << 1) == 0;
            const bool live = ((spotUse & (1u << i)) && !cosConst) || ((specUse & (1u << i)) && !(cosConst && distConst));
            if (!live) o[4] = o[5] = o[6] = 0.0f;
            if (LightNormCensusOn()) {
                const bool nz = ((L[13] | L[14] | L[15]) << 1) != 0, nf = NonFiniteDir(L);
                if (live) { ++g_lnc.kept; g_lnc.keptNonFinite += nf; }
                else if (nz) {
                    ++g_lnc.zeroed; g_lnc.zeroedNonFinite += nf; g_lnc.zeroedUnread += !((spotUse | specUse) & (1u << i));
                    static uint64_t lastLogged = ~0ull;
                    if (nf && ((spotUse | specUse) & (1u << i)) && lastLogged != g_frame) {
                        lastLogged = g_frame;
                        std::fprintf(stderr, "[vgx] frame %llu: light %d has a non-finite direction (%08x %08x %08x) that a shader reads and multiplies by zero\n",
                                     (unsigned long long)g_frame, i, L[13], L[14], L[15]);
                    }
                }
            }
        }
    }
    if (LightNormCensusOn()) ++g_lnc.blocks;
    PushBlock(UB_LIGHT, lb, static_cast<uint32_t>(nvec) * 4);
}
inline float FogFloat(uint32_t r) { // sign(1) exp(8) mantissa(11) packed in the low 20 bits
    return u2f((bits(r, 19, 1) << 31) | (bits(r, 11, 8) << 23) | (bits(r, 0, 11) << 12));
}
GXD_NI void BuildTev(const FsKey& fs, const uint16_t texW[8], const uint16_t texH[8]) {
    float tb[112];
    const uint32_t tevLen = static_cast<uint32_t>(FsTevCount(fs)) * 4;
    // (GXD_SMALL: only what the block carries is cleared; every store below lands inside [0, tevLen) of its key: the registers, konst colours and
    // misc vector (36 floats) always, fog and Z-freeze within the length FsTevCount gives them, the indirect parts only with the 112-float block)
    if (Gxd(GXD_SMALL)) ZeroWords(tb, tevLen);
    else std::memset(tb, 0, sizeof tb);
    if (Gxd(GXD_SMALL)) {
        const float* t = D255();
        for (int i = 0; i < 4; ++i)
            for (int c = 0; c < 4; ++c) {
                tb[TEV_REG + i * 4 + c] = t[g.tevReg[i][c]];
                tb[TEV_KONST + i * 4 + c] = t[g.tevKonst[i][c] & 0xFF];
            }
    } else {
        for (int i = 0; i < 4; ++i)
            for (int c = 0; c < 4; ++c) {
                tb[TEV_REG + i * 4 + c] = static_cast<float>(g.tevReg[i][c]) / 255.0f;
                tb[TEV_KONST + i * 4 + c] = static_cast<float>(g.tevKonst[i][c] & 0xFF) / 255.0f;
            }
    }
    tb[TEV_MISC + 0] = static_cast<float>(bits(g.bp[0xF3], 0, 8));
    tb[TEV_MISC + 1] = static_cast<float>(bits(g.bp[0xF3], 8, 8));
    tb[TEV_MISC + 2] = static_cast<float>(bits(g.bp[0x42], 0, 8)) / 255.0f;
    tb[TEV_MISC + 3] = static_cast<float>(g.bp[0xF4]) / 16777215.0f;
    if (fs.fog) {
        const float a = FogFloat(g.bp[0xEE]);
        const float bm = static_cast<float>(g.bp[0xEF]);
        const float sh = std::ldexp(1.0f, static_cast<int>(bits(g.bp[0xF0], 0, 5)));
        if (fs.fog & 8) { tb[TEV_FOG + 0] = a; tb[TEV_FOG + 1] = 0; }
        else { tb[TEV_FOG + 0] = a * sh; tb[TEV_FOG + 1] = bm * sh / 16777216.0f; }
        tb[TEV_FOG + 2] = FogFloat(g.bp[0xF1]);
        tb[TEV_FOGCOL + 0] = static_cast<float>(bits(g.bp[0xF2], 16, 8)) / 255.0f;
        tb[TEV_FOGCOL + 1] = static_cast<float>(bits(g.bp[0xF2], 8, 8)) / 255.0f;
        tb[TEV_FOGCOL + 2] = static_cast<float>(bits(g.bp[0xF2], 0, 8)) / 255.0f;
    }
    if (fs.numIndStages) {
        for (int m = 0; m < 3; ++m) {
            const uint32_t r0 = g.bp[0x06 + m * 3], r1 = g.bp[0x07 + m * 3], r2 = g.bp[0x08 + m * 3];
            const int sc = static_cast<int>(bits(r0, 22, 2) | (bits(r1, 22, 2) << 2) | (bits(r2, 22, 2) << 4)) - 17;
            auto s11 = [](uint32_t v) { return static_cast<float>(static_cast<int>(v << 21) >> 21) / 1024.0f; };
            float* o = &tb[TEV_INDMTX + m * 8];
            // Offsets are produced in texels of the regular texture coordinate (see shadergen).
            const float scale = std::ldexp(1.0f, sc);
            o[0] = s11(bits(r0, 0, 11)) * scale; o[1] = s11(bits(r1, 0, 11)) * scale; o[2] = s11(bits(r2, 0, 11)) * scale; o[3] = scale;
            o[4] = s11(bits(r0, 11, 11)) * scale; o[5] = s11(bits(r1, 11, 11)) * scale; o[6] = s11(bits(r2, 11, 11)) * scale;
        }
        for (int i = 0; i < fs.numIndStages; ++i) {
            const uint32_t r = g.bp[0x25 + (i >> 1)] >> ((i & 1) * 8);
            float* o = &tb[TEV_INDSCALE + i * 4];
            o[0] = std::ldexp(1.0f, -static_cast<int>(bits(r, 0, 4)));
            o[1] = std::ldexp(1.0f, -static_cast<int>(bits(r, 4, 4)));
        }
        // Texture sizes per texmap: indirect offsets are in texels.
        for (int m = 0; m < 8; ++m) {
            tb[TEV_TEXSIZE + m * 2] = texW[m] ? static_cast<float>(texW[m]) : 1.0f;
            tb[TEV_TEXSIZE + m * 2 + 1] = texH[m] ? static_cast<float>(texH[m]) : 1.0f;
        }
    }
    if (fs.zfreeze) {
        zfreeze::Plane pl;
        const int zv = FsZFreezeVec(fs) * 4;
        if (ZFreezePlane(pl)) { tb[zv] = pl.a; tb[zv + 1] = pl.b; tb[zv + 2] = pl.c; }
        else { tb[zv] = tb[zv + 1] = 0; tb[zv + 2] = 1; } // (no usable plane: far plane. Cannot be reached: the key is only set with a reference)
    }
    PushBlock(UB_TEV, tb, tevLen);
}

// Small meshes can be transformed to eye space on the CPU so that consecutive
// draws that differ only in their model-view matrix (grass blades, crowd
// sprites, particles: hundreds per frame in this game) share one uniform set
// and merge into a single draw call. MSC_VGX_SOFTXFORM=<max verts>, 0 = off.
} // namespace
uint32_t SoftXformLimit() {
    static const uint32_t n = static_cast<uint32_t>(EnvInt("MSC_VGX_SOFTXFORM", 128));
    return n;
}
// (the Z-freeze reference is made from the vertices of arena draws: with it followed, a small list stays in the arena)
bool LastDrawNeedsArena() { return cur.soft || ZFreezeTracking(); }
namespace {
bool SoftEligible(const VsKey& vs) {
    if (vs.perVtxPos) return false;
    for (int i = 0; i < vs.numTexGens; ++i)
        if (vs.tg[i].perVtxMtx || (vs.tg[i].type < 2 && vs.tg[i].src < 2)) return false; // needs model-space pos / normal
    return true;
}
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
// GXD_SOFT_NEON: the same arithmetic, operation for operation (((m0 x + m1 y) + m2 z) + m3 with each product rounded before the sum, which
// is what vmla.f32 does and what the scalar loop compiles to for this target: no fused multiply-add on a Cortex-A9), for the three rows in
// the lanes of one register. One vertex is about 12 instructions instead of 45 (position) and 40 (normal). Only the first three lanes of
// the result are stored: the fourth float of a position (the matrix row index) is not touched.
static void SoftTransformNeon(uint8_t* v, uint32_t count, const VtxLayout& L, const float* m0, const float* n0) {
    // columns of the 3x4 position matrix and of the 3x3 normal matrix, one row of the result per lane
    const float pc0[4] = {m0[0], m0[4], m0[8], 0.0f}, pc1[4] = {m0[1], m0[5], m0[9], 0.0f}, pc2[4] = {m0[2], m0[6], m0[10], 0.0f}, pc3[4] = {m0[3], m0[7], m0[11], 0.0f};
    const float nc0[4] = {n0[0], n0[3], n0[6], 0.0f}, nc1[4] = {n0[1], n0[4], n0[7], 0.0f}, nc2[4] = {n0[2], n0[5], n0[8], 0.0f};
    const float32x4_t P0 = vld1q_f32(pc0), P1 = vld1q_f32(pc1), P2 = vld1q_f32(pc2), P3 = vld1q_f32(pc3);
    const float32x4_t N0 = vld1q_f32(nc0), N1 = vld1q_f32(nc1), N2 = vld1q_f32(nc2);
    const bool nrm = (L.mask & VL_NRM) != 0;
    const uint32_t stride = L.stride, offNrm = L.offNrm;
    for (uint32_t i = 0; i < count; ++i, v += stride) {
        float* p = reinterpret_cast<float*>(v);
        const float32x4_t in = vld1q_f32(p); // x y z w (w stays)
        float32x4_t r = vmulq_lane_f32(P0, vget_low_f32(in), 0);
        r = vmlaq_lane_f32(r, P1, vget_low_f32(in), 1);
        r = vmlaq_lane_f32(r, P2, vget_high_f32(in), 0);
        r = vaddq_f32(r, P3);
        vst1_f32(p, vget_low_f32(r));
        vst1q_lane_f32(p + 2, r, 2);
        if (nrm) {
            float* q = reinterpret_cast<float*>(v + offNrm);
            const float32x2_t nxy = vld1_f32(q);
            const float32x2_t nz = vld1_dup_f32(q + 2);
            float32x4_t o = vmulq_lane_f32(N0, nxy, 0);
            o = vmlaq_lane_f32(o, N1, nxy, 1);
            o = vmlaq_lane_f32(o, N2, nz, 0);
            vst1_f32(q, vget_low_f32(o));
            vst1q_lane_f32(q + 2, o, 2);
        }
    }
}
#endif
void SoftTransform(uint8_t* v, uint32_t count, const VtxLayout& L) {
    const uint32_t pi = g.matIdxA & 0x3F;
    const float* m0 = g.xfPos[pi < 62 ? pi : 61];
    const float* m1 = m0 + 4;
    const float* m2 = m0 + 8;
    const float* n0 = g.xfNrm[(pi & 31) < 30 ? (pi & 31) : 29];
    const float* n1 = n0 + 3;
    const float* n2 = n0 + 6;
    const bool nrm = (L.mask & VL_NRM) != 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    if (Gxd(GXD_SOFT_NEON)) {
        SoftTransformNeon(v, count, L, m0, n0);
        return;
    }
#endif
    for (uint32_t i = 0; i < count; ++i, v += L.stride) {
        float* p = reinterpret_cast<float*>(v);
        const float x = p[0], y = p[1], z = p[2];
        p[0] = m0[0] * x + m0[1] * y + m0[2] * z + m0[3];
        p[1] = m1[0] * x + m1[1] * y + m1[2] * z + m1[3];
        p[2] = m2[0] * x + m2[1] * y + m2[2] * z + m2[3];
        if (nrm) {
            float n[3];
            std::memcpy(n, v + L.offNrm, 12);
            const float o[3] = {n0[0] * n[0] + n0[1] * n[1] + n0[2] * n[2], n1[0] * n[0] + n1[1] * n[1] + n1[2] * n[2],
                                n2[0] * n[0] + n2[1] * n[1] + n2[2] * n[2]};
            std::memcpy(v + L.offNrm, o, 12);
        }
    }
}

// ---- GXD_KEYMEMO: the shader keys of a register state seen before.
// BuildFsKey / BuildVsKey (about 1 500 instructions with the key comparison and the registry lookup that follow, for each of the 200 key changes of an
// open-play frame) are functions of the registers listed in KeyInputs, and the game cycles through a few dozen states. A direct-mapped memo keyed on a
// hash of those words, and verified against them word for word, hands back the registry ids. The list must be exactly the g.bp / g.xfReg / g.vcd reads of
// BuildFsKey and BuildVsKey (and of what they call): keep in step; MSC_VGX_GXD with GXD_VERIFY builds the keys the long way on every hit and compares.
constexpr int kKmWords = 104;
struct KeyMemoEnt {
    uint32_t h = 0, n = 0;
    uint16_t fsId = 0, vsId = 0;
    bool valid = false;
    uint32_t in[kKmWords];
};
KeyMemoEnt g_km[128];
uint64_t g_kmLookups = 0, g_kmHits = 0, g_kmFails = 0;
inline uint32_t KeyInputs(uint32_t* in, uint8_t palP) {
    const uint32_t gm = g.bp[0x00];
    const uint32_t stages = bits(gm, 10, 4) + 1;
    uint32_t ntg = bits(gm, 0, 4);
    if (ntg > 8) ntg = 8;
    uint32_t nind = bits(gm, 16, 3);
    if (nind > 4) nind = 4;
    uint32_t n = 0;
    in[n++] = gm;
    in[n++] = (static_cast<uint32_t>(palP) << 2) | (g_zf.have ? 2u : 0u) | (static_cast<uint32_t>(g_pendingSkin) << 16); // (W5 SKIN: the vertex key's skin flag)
    in[n++] = g.bp[0x27];
    in[n++] = g.bp[0x41];
    in[n++] = g.bp[0x42];
    in[n++] = g.bp[0x43];
    in[n++] = g.bp[0xF1];
    in[n++] = g.bp[0xF3];
    in[n++] = g.bp[0xF5];
    CopyWords(in + n, &g.bp[0xF6], 8); n += 8;                  // key selection and swap tables
    CopyWords(in + n, &g.bp[0xC0], 2 * stages); n += 2 * stages; // TEV stages
    CopyWords(in + n, &g.bp[0x28], (stages + 1) / 2); n += (stages + 1) / 2; // TEV order
    if (nind) { CopyWords(in + n, &g.bp[0x10], stages); n += stages; }       // indirect stages
    in[n++] = g.xfReg[0x09];
    CopyWords(in + n, &g.xfReg[0x0E], 5); n += 5;                // colour channels, dual-texgen enable
    CopyWords(in + n, &g.xfReg[0x40], ntg); n += ntg;            // texgens and their post-transform rows
    CopyWords(in + n, &g.xfReg[0x50], ntg); n += ntg;
    uint32_t v0, v1;
    std::memcpy(&v0, &g.vcd[0], 4);
    std::memcpy(&v1, &g.vcd[4], 4);
    in[n++] = v0;
    in[n++] = v1;
    in[n++] = g.vcd[8];
    return n;
}
inline uint32_t KeyHash(const uint32_t* in, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; ++i) h = (h ^ in[i]) * 16777619u;
    return h ^ (h >> 15);
}

// W5 SKIN: the bone matrices of the current skin state as a block of the frame's uniform arena (kSkinBlockFloats; the bones past nBones are zero:
// no vertex names them, see SkinAttributes).
void BuildSkin() {
    const size_t at = g_buf.uni.size();
    g_buf.uni.resize(at + kSkinBlockFloats);
    float* d = g_buf.uni.data() + at;
    const uint32_t n = g_skin.nBones * 12u;
    std::memcpy(d, g_skin.mats, n * sizeof(float));
    std::memset(d + n, 0, (kSkinBlockFloats - n) * sizeof(float));
    g_skinBlockOff = static_cast<uint32_t>(at);
    g_skinBlockSerial = g_skin.serial;
}

void ResolveState(uint32_t vcdMtxMask, uint32_t vertexCount) {
    // The short matrix palette is enough when the draw's vertices stay within its rows.
    const uint8_t palP = UniformPacking() ? PalBucketFor(g_pendingPosMtxMax) : 0;
    if (PalHistOn() && (vcdMtxMask & 1u)) PalHistNote(g_pendingPosMtxMax, palP);
    g_pendingPosMtxMax = 0xFF;
    // (see MarkBP / MarkXFReg in vgx_state.cpp for what sets which flag)
    const bool keysDirty = !cur.valid || g.bpKeyDirty || g.xfKeyDirty || g.xfRegDirty || vcdMtxMask != cur.vcdMtxMask ||
                           ((vcdMtxMask & 1u) && palP != cur.palP) || g_pendingSkin != cur.vs.chan[0].skin;
    const bool texDirty = keysDirty || g.texDirty;
    // (a key that was built again and came out the same leaves the blocks that only depend
    // on the key alone)
    bool fsChanged = false, vsChanged = false;
    const bool vcdChanged = !cur.valid || vcdMtxMask != cur.vcdMtxMask;
    if (keysDirty) {
        bool memoDone = false;
        KeyMemoEnt* me = nullptr;
        uint32_t in[kKmWords], nIn = 0, hIn = 0;
        if (Gxd(GXD_KEYMEMO)) {
            nIn = KeyInputs(in, palP);
            hIn = KeyHash(in, nIn);
            me = &g_km[hIn & 127u];
            ++g_kmLookups;
            if (me->valid && me->h == hIn && me->n == nIn && SameWords(me->in, in, nIn)) {
                ++g_kmHits;
                if (Gxd(GXD_VERIFY)) {
                    FsKey fs2;
                    BuildFsKey(fs2);
                    VsKey vs2;
                    BuildVsKey(vs2, fs2.numTexGens, palP);
                    if (!SameStruct(fs2, g_fsReg.keys[me->fsId]) || !SameStruct(vs2, g_vsReg.keys[me->vsId])) {
                        if (++g_kmFails <= 8) std::fprintf(stderr, "[vgx] GXD VERIFY FAILED: key memo returned ids %u/%u for a state whose keys differ\n", me->fsId, me->vsId);
                    }
                }
                cur.palP = palP;
                fsChanged = !cur.valid || me->fsId != cur.fsId;
                vsChanged = !cur.valid || me->vsId != cur.vsId;
                if (fsChanged) { cur.fs = g_fsReg.keys[me->fsId]; cur.fsId = me->fsId; }
                if (vsChanged) { cur.vs = g_vsReg.keys[me->vsId]; cur.vsId = me->vsId; }
                cur.vcdMtxMask = vcdMtxMask;
                memoDone = true;
            }
        }
        if (!memoDone) {
            FsKey fs;
            BuildFsKey(fs);
            VsKey vs;
            BuildVsKey(vs, fs.numTexGens, palP);
            cur.palP = palP;
            fsChanged = !cur.valid || !SameStruct(fs, cur.fs);
            vsChanged = !cur.valid || !SameStruct(vs, cur.vs);
            if (fsChanged) { cur.fs = fs; cur.fsId = Lookup(g_fsReg, fs, true, GenerateFragmentShader); }
            if (vsChanged) { cur.vs = vs; cur.vsId = Lookup(g_vsReg, vs, false, GenerateVertexShader); }
            cur.vcdMtxMask = vcdMtxMask;
            if (me) {
                me->valid = true;
                me->h = hIn;
                me->n = nIn;
                me->fsId = cur.fsId;
                me->vsId = cur.vsId;
                CopyWords(me->in, in, nIn);
            }
        }
    }
    if (keysDirty || g.xfViewDirty || g.bpRsDirty) {
        BuildRenderState(cur.rs);
        cur.cullAll = cur.rs.cull == CULL_ALL;
    }
    if (texDirty) {
        uint8_t mask = 0;
        for (int s = 0; s < cur.fs.numStages; ++s)
            if (cur.fs.stage[s].texEnable) mask |= static_cast<uint8_t>(1u << cur.fs.stage[s].texMap);
        for (int i = 0; i < cur.fs.numIndStages; ++i) mask |= static_cast<uint8_t>(1u << cur.fs.ind[i].texMap);
        cur.texMask = mask;
        static const bool perSlot = EnvInt("MSC_VGX_TEXSLOTS", 1) != 0; // 0: look every used texmap up again (the old behaviour)
        cur.slotResolved &= static_cast<uint8_t>(~g.texSlotDirty);
        if (!perSlot) cur.slotResolved = 0;
        g.texSlotDirty = 0;
        for (int m = 0; m < 8; ++m) {
            const uint8_t bit = static_cast<uint8_t>(1u << m);
            if (!(mask & bit)) {
                cur.tex[m] = TexBind{};
                cur.texW[m] = cur.texH[m] = 0;
                continue;
            }
            if (!(cur.slotResolved & bit)) {
                cur.slotW[m] = cur.slotH[m] = 0;
                ResolveTexture(m, cur.slotBind[m], cur.slotW[m], cur.slotH[m]);
                cur.slotResolved |= bit;
            }
            cur.tex[m] = cur.slotBind[m];
            cur.texW[m] = cur.slotW[m];
            cur.texH[m] = cur.slotH[m];
        }
    }
    const bool soft = vertexCount <= SoftXformLimit() && SoftEligible(cur.vs) && !g_pendingSkin;
    if (g_pendingSkin && (g_blocksStale || g_skinBlockOff == kNoBlock || g_skinBlockSerial != g_skin.serial)) BuildSkin();
    if (g_blocksStale || g.xfProjDirty || g.xfRegDirty) BuildProj();
    if (g_blocksStale || g.xfRegDirty || g.xfMtxDirty || vsChanged || vcdChanged || soft != cur.soft) BuildMtx(cur.vs, soft);
    cur.soft = soft;
    // (the light block depends on the vertex key through its length when packed and through
    // the directions it carries when normalised)
    if (g_blocksStale || g.xfRegDirty || g.xfLightDirty || g.xfColorDirty || (vsChanged && (UniformPacking() || LightNorm()))) BuildLight(cur.vs);
    // (a frozen draw's block also carries the reference plane, which changes with draws, not with registers)
    static uint32_t zfBuilt = ~0u;
    if (g_blocksStale || g.bpDirty || fsChanged || (texDirty && cur.fs.numIndStages) || (cur.fs.zfreeze && zfBuilt != g_zf.version)) {
        BuildTev(cur.fs, cur.texW, cur.texH);
        zfBuilt = g_zf.version;
    }
    g_blocksStale = false;
    g.bpDirty = g.bpKeyDirty = g.bpRsDirty = g.xfRegDirty = g.xfKeyDirty = g.xfViewDirty = g.xfProjDirty = g.xfColorDirty = g.xfMtxDirty = g.xfLightDirty = g.texDirty = false;
    cur.valid = true;
}

} // namespace

// MSC_VGX_TRACE=<frame> traces that presented frame. MSC_VGX_TRACE_DRAWS=<n>
// traces the first frame that follows one with at least n draw records
// (frame numbers are not reproducible between runs: the boot sequence is
// partly wall-clock timed).
static uint64_t g_traceFrame = ~0ull;
static uint32_t g_frameDraws = 0;
bool TraceFrame() {
    static const long f = EnvInt("MSC_VGX_TRACE", -1);
    return (f >= 0 && static_cast<uint64_t>(f) == g_frame) || g_frame == g_traceFrame;
}

// MSC_VGX_TRACE=<frame>: print every draw of that frame with its resolved state.
static void TraceDraw(uint8_t gxPrim, uint32_t layoutMask, uint32_t count, const uint8_t* v) {
    const VtxLayout L = MakeLayout(layoutMask);
    const RenderState& rs = cur.rs;
    std::fprintf(stderr, "[trace] draw prim=0x%02X verts=%u layout=0x%X vs=%u fs=%u stages=%u texgens=%u chans=%u\n", gxPrim, count, layoutMask,
                 cur.vsId, cur.fsId, cur.fs.numStages, cur.fs.numTexGens, cur.vs.numChans);
    std::fprintf(stderr, "[trace]   blend=%u(%u,%u,eq%u) ztest=%u func=%u zwrite=%u cull=%u cw=%u aw=%u sc=(%d,%d,%d,%d) vp=(%.1f,%.1f,%.1f,%.1f) z=(%.4f,%.4f) atest=%u(0x%02X) fog=%u\n",
                 rs.blend, rs.blendSrc, rs.blendDst, rs.blendEq, rs.depthTest, rs.depthFunc, rs.depthWrite, rs.cull, rs.colorWrite, rs.alphaWrite,
                 rs.scX, rs.scY, rs.scW, rs.scH, rs.vpX, rs.vpY, rs.vpW, rs.vpH, rs.zNear, rs.zFar, cur.fs.alphaTestOn, cur.fs.alphaTest, cur.fs.fog);
    for (int s = 0; s < cur.fs.numStages; ++s) {
        const FsKey::Stage& st = cur.fs.stage[s];
        std::fprintf(stderr, "[trace]   stage %d: color=0x%06X alpha=0x%06X tex=%u map=%u coord=%u ras=%u kc=%u ka=%u ind=0x%X\n", s, st.color, st.alpha,
                     st.texEnable, st.texMap, st.texCoord, st.rasChan, st.kcsel, st.kasel, st.ind);
    }
    for (int m = 0; m < 8; ++m)
        if (cur.texMask & (1u << m)) {
            const TexObj& o = g.tex[m].obj;
            std::fprintf(stderr, "[trace]   texmap %d: id=%u %ux%u fmt=0x%X wrap=%u/%u filt=%u/%u mip=%u data=%p tlut=%u\n", m, cur.tex[m].texId, o.width,
                         o.height, o.format, cur.tex[m].wrapS, cur.tex[m].wrapT, cur.tex[m].minFilter, cur.tex[m].magFilter, o.mipmap, o.data, o.tlut);
        }
    for (int i = 0; i < 4; ++i)
        std::fprintf(stderr, "[trace]   chan%d: en=%u matVtx=%u ambVtx=%u diff=%u attn=%u lights=0x%02X\n", i, cur.vs.chan[i].enable, cur.vs.chan[i].matSrcVtx,
                     cur.vs.chan[i].ambSrcVtx, cur.vs.chan[i].diffuse, cur.vs.chan[i].attn, cur.vs.chan[i].lightMask);
    for (int i = 0; i < cur.vs.numTexGens; ++i)
        std::fprintf(stderr, "[trace]   texgen%d: type=%u src=%u proj=%u abc1=%u post=%u pervtx=%u\n", i, cur.vs.tg[i].type, cur.vs.tg[i].src,
                     cur.vs.tg[i].proj, cur.vs.tg[i].inputAbc1, cur.vs.tg[i].post, cur.vs.tg[i].perVtxMtx);
    std::fprintf(stderr, "[trace]   proj type=%u p=(%g,%g,%g,%g,%g,%g) matIdxA=0x%08X tevreg0=(%d,%d,%d,%d) k0=(%d,%d,%d,%d)\n", g.xfReg[0x26],
                 u2f(g.xfReg[0x20]), u2f(g.xfReg[0x21]), u2f(g.xfReg[0x22]), u2f(g.xfReg[0x23]), u2f(g.xfReg[0x24]), u2f(g.xfReg[0x25]), g.matIdxA,
                 g.tevReg[0][0], g.tevReg[0][1], g.tevReg[0][2], g.tevReg[0][3], g.tevKonst[0][0], g.tevKonst[0][1], g.tevKonst[0][2], g.tevKonst[0][3]);
    const uint32_t pi = g.matIdxA & 0x3F;
    std::fprintf(stderr, "[trace]   posmtx[%u]= (%g %g %g %g) (%g %g %g %g) (%g %g %g %g)\n", pi, g.xfPos[pi][0], g.xfPos[pi][1], g.xfPos[pi][2],
                 g.xfPos[pi][3], g.xfPos[pi + 1][0], g.xfPos[pi + 1][1], g.xfPos[pi + 1][2], g.xfPos[pi + 1][3], g.xfPos[pi + 2][0],
                 g.xfPos[pi + 2][1], g.xfPos[pi + 2][2], g.xfPos[pi + 2][3]);
    for (uint32_t i = 0; i < count && i < 4; ++i, v += L.stride) {
        const float* p = reinterpret_cast<const float*>(v);
        std::fprintf(stderr, "[trace]   v%u pos=(%g,%g,%g | %g)", i, p[0], p[1], p[2], p[3]);
        if (L.mask & VL_NRM) { const float* n = reinterpret_cast<const float*>(v + L.offNrm); std::fprintf(stderr, " nrm=(%g,%g,%g)", n[0], n[1], n[2]); }
        if (L.mask & VL_COL0) std::fprintf(stderr, " c0=%02X%02X%02X%02X", v[L.offCol0], v[L.offCol0 + 1], v[L.offCol0 + 2], v[L.offCol0 + 3]);
        if (L.mask & VL_COL1) std::fprintf(stderr, " c1=%02X%02X%02X%02X", v[L.offCol1], v[L.offCol1 + 1], v[L.offCol1 + 2], v[L.offCol1 + 3]);
        for (int t = 0; t < 8; ++t)
            if (L.mask & (VL_TEX0 << t)) { const float* tc = reinterpret_cast<const float*>(v + L.offTex[t]); std::fprintf(stderr, " t%d=(%g,%g)", t, tc[0], tc[1]); }
        std::fprintf(stderr, "\n");
    }
}

CmdBuf& Pending() { return g_buf; }
void SetPendingPosMtxMax(uint32_t row) { g_pendingPosMtxMax = row; }
uint64_t CurrentFrame() { return g_frame; }

// ---- vertex addressing
// A draw's indices count vertices from the start of a buffer, not from the
// draw's own first vertex: the back end then never needs a per-draw byte
// offset in its attribute pointers. That is what the console's GL layer
// requires (sceGxm attribute offsets have 16 bits, and every distinct offset
// costs a newly patched vertex program there; see vgx_backend_gl.cpp), and it
// lets consecutive draws with one vertex layout share one set of pointers
// everywhere. The frame's vertex arena is cut into chunks, each a buffer of
// its own in the back end; inside a chunk every draw's vertices start at a
// multiple of their own stride from the chunk's start (zero padding in
// between), so "first vertex" is a whole number for every layout, and a new
// chunk begins when that number would no longer fit 16-bit indices.
//   MSC_VGX_ABSIDX=0   the old scheme: 0-based indices and a byte offset per draw
//                      (what the reference frame hashes were made with)
// Build default (from Codex's work, 8-9 Oct): -DMSC_VGX_ABSIDX_DEFAULT=0 makes the old scheme the
// default of a build. That is the "emulator profile" for Vita3K without memory mapping (it copies a
// draw's vertex streams from their start up to the largest absolute index, so a big arena wraps its
// host ring): it needs vitaGL built with -DSTRICT_DRAW_COMPLIANCE (the packed route of the stock
// build truncates the per-draw byte offset to 16 bits) and the build says so with
// -DMSC_VGX_STRICT_DRAW_PROFILE=1. The console does not copy streams and keeps the default 1.
#ifndef MSC_VGX_ABSIDX_DEFAULT
#define MSC_VGX_ABSIDX_DEFAULT 1
#endif
#if MSC_VGX_ABSIDX_DEFAULT != 0 && MSC_VGX_ABSIDX_DEFAULT != 1
#error MSC_VGX_ABSIDX_DEFAULT must be 0 or 1
#endif
#if defined(__VGX_VITA__) && MSC_VGX_ABSIDX_DEFAULT == 0 && (!defined(MSC_VGX_STRICT_DRAW_PROFILE) || MSC_VGX_STRICT_DRAW_PROFILE != 1)
#error Relative default indices require the strict vitaGL build profile (-DMSC_VGX_STRICT_DRAW_PROFILE=1 with vitaGL -DSTRICT_DRAW_COMPLIANCE)
#endif
bool AbsoluteIndices() {
    static const bool on = EnvInt("MSC_VGX_ABSIDX", MSC_VGX_ABSIDX_DEFAULT) != 0;
    return on;
}
static size_t g_chunkStart = 0;    // byte offset in g_buf.vtx of the current chunk
static uint32_t g_pendingFirst = 0; // index of the pending vertices' first one, from the chunk start

// x / d for the vertex strides (16..255 bytes; layouts are at most 108) and x below 2^24 (a chunk holds at most 65536 vertices) as one multiplication:
// with m = ceil(2^32 / d) the error of x * m / 2^32 against x / d is below x / 2^32 < 2^-8 <= 1 / d, which never carries into the next integer.
// The library division on the console's processor (no divide instruction) is some 90 instructions.
static inline uint32_t DivStride(uint32_t x, uint32_t d) {
    static uint32_t recip[256];
    if (x < (1u << 24) && d - 16u < 240u && Gxd(GXD_DIV)) {
        uint32_t m = recip[d];
        if (!m) m = recip[d] = static_cast<uint32_t>(((1ull << 32) + d - 1) / d);
        return static_cast<uint32_t>((static_cast<uint64_t>(x) * m) >> 32);
    }
    return x / d;
}

uint8_t* BeginVertices(uint32_t layoutMask, uint32_t count) {
    const VtxLayout L = MakeLayout(layoutMask);
    size_t pos = g_buf.vtx.size();
    if (AbsoluteIndices()) {
        if (g_buf.vtxChunks.empty()) {
            g_buf.vtxChunks.push_back(0);
            g_chunkStart = 0;
        }
        size_t first = DivStride(static_cast<uint32_t>(pos - g_chunkStart + L.stride - 1), L.stride);
        if (first + count > 0x10000) {
            g_chunkStart = pos;
            g_buf.vtxChunks.push_back(static_cast<uint32_t>(pos));
            first = 0;
        }
        const size_t start = g_chunkStart + first * L.stride;
        if (start != pos) {
            g_buf.vtx.resize(start);
            std::memset(g_buf.vtx.data() + pos, 0, start - pos); // (the arena is not zero-filled, and it is hashed)
        }
        pos = start;
        g_pendingFirst = static_cast<uint32_t>(first);
    }
    g_pendingVtxStart = pos;
    g_buf.vtx.resize(g_pendingVtxStart + static_cast<size_t>(count) * L.stride);
    return g_buf.vtx.data() + g_pendingVtxStart;
}

uint32_t PrimIndices(uint8_t gxPrim, uint32_t count, uint32_t base, uint16_t* ix) {
    uint32_t idxCount = 0;
    switch (gxPrim & 0xF8) {
    case 0x80: case 0x88: idxCount = (count / 4) * 6; break;
    case 0x90: idxCount = (count / 3) * 3; break;
    case 0x98: case 0xA0: idxCount = count >= 3 ? (count - 2) * 3 : 0; break;
    case 0xA8: idxCount = (count / 2) * 2; break;
    case 0xB0: idxCount = count >= 2 ? (count - 1) * 2 : 0; break;
    case 0xB8: idxCount = count; break;
    default: break;
    }
    if (!ix || !idxCount) return idxCount;
    const auto B = [base](uint32_t v) { return static_cast<uint16_t>(base + v); };
    switch (gxPrim & 0xF8) {
    case 0x80: case 0x88:
        for (uint32_t q = 0; q + 3 < count; q += 4) {
            *ix++ = B(q); *ix++ = B(q + 1); *ix++ = B(q + 2);
            *ix++ = B(q); *ix++ = B(q + 2); *ix++ = B(q + 3);
        }
        break;
    case 0x90:
        for (uint32_t i = 0; i < idxCount; ++i) *ix++ = B(i);
        break;
    case 0x98:
        for (uint32_t i = 0; i + 2 < count; ++i) {
            if (i & 1) { *ix++ = B(i + 1); *ix++ = B(i); *ix++ = B(i + 2); }
            else { *ix++ = B(i); *ix++ = B(i + 1); *ix++ = B(i + 2); }
        }
        break;
    case 0xA0:
        for (uint32_t i = 0; i + 2 < count; ++i) { *ix++ = B(0); *ix++ = B(i + 1); *ix++ = B(i + 2); }
        break;
    case 0xA8:
        for (uint32_t i = 0; i < idxCount; ++i) *ix++ = B(i);
        break;
    case 0xB0:
        for (uint32_t i = 0; i + 1 < count; ++i) { *ix++ = B(i); *ix++ = B(i + 1); }
        break;
    default:
        for (uint32_t i = 0; i < count; ++i) *ix++ = B(i);
        break;
    }
    return idxCount;
}
static inline uint8_t PrimKind(uint8_t gxPrim) {
    switch (gxPrim & 0xF8) {
    case 0xA8: case 0xB0: return PRIM_LINES;
    case 0xB8: return PRIM_POINTS;
    default: return PRIM_TRIANGLES;
    }
}

// Fills a new record with the resolved state (everything except the geometry).
static DrawRec* NewDrawRec(uint8_t prim, uint32_t layoutMask) {
    g_buf.draws.emplace_back();
    DrawRec* rec = &g_buf.draws.back();
    rec->vsId = cur.vsId; rec->fsId = cur.fsId;
    rec->prim = prim;
    rec->texMask = cur.texMask;
    rec->layoutMask = layoutMask;
    std::memcpy(rec->ublock, g_blockOff, sizeof g_blockOff);
    rec->skinBlock = (layoutMask & VL_SKIN) ? g_skinBlockOff : kNoBlock;
    rec->rs = cur.rs;
    std::memcpy(rec->tex, cur.tex, sizeof cur.tex);
    g_lastDraw = static_cast<int>(g_buf.draws.size()) - 1;
    g_buf.cmds.push_back(Cmd{CMD_DRAW, static_cast<uint32_t>(g_lastDraw)});
    ++stats::Cur().drawCalls;
    if (stats::Detailed()) {
        stats::ProgramUsed(cur.vsId, cur.fsId);
        for (int m = 0; m < 8; ++m)
            if ((cur.texMask & (1u << m)) && cur.tex[m].texId) stats::TexBound(cur.tex[m].texId, static_cast<uint32_t>(cur.texW[m]) * cur.texH[m] * 4u);
    }
    if (g_shaderList) {
        const uint32_t bk = BlendKey(cur.rs);
        const uint64_t vk = VariantKey(cur.vsId, cur.fsId, layoutMask, bk);
        if (vk != g_lastVariant) {
            g_lastVariant = vk;
            const uint32_t pk = (static_cast<uint32_t>(cur.vsId) << 16) | cur.fsId;
            if (g_progSeen.insert(pk).second) {
                const uint32_t v[2] = {cur.vsId, cur.fsId};
                ListAppend(3u, v, sizeof v);
            }
            if (g_varSeen.insert(vk).second) {
                const uint32_t v[4] = {cur.vsId, cur.fsId, layoutMask & 0x1FFFu, bk};
                ListAppend(4u, v, sizeof v);
            }
        }
    }
    return rec;
}

// MSC_VGX_ZFTRACE=1 (diagnostic): the draws made with GX's Z-freeze (GXSetCoPlanar, genMode bit 19), which this renderer
// does not model: how many, with which depth test / blend / cull, and (MSC_VGX_ZFTRACE=<frame>) every one of that frame.
// The game puts the planar (flattened) player shadows, pitch decals and similar in its eCLV_CoPlanar layer
// (decomp: RLView.cpp BeginPacket, RenderShadow.cpp DrawPlanarShadow). Summary every 1000 frames on stderr.
namespace {
struct ZFreezeCheck { uint64_t noReference = 0, measured = 0, over1e5 = 0; double maxDz = 0, sumDz = 0; } g_zfs;
struct ZFreezeStats {
    std::map<uint32_t, uint64_t> combos; // key: cullAll<<20 | cull<<16 | blend<<15 | depthTest<<14 | depthWrite<<13 | depthFunc<<8 | prim<<4
    uint64_t draws = 0, verts = 0, frames = 0, lastFrame = ~0ull;
} g_zfd;
}
// Called for every stream draw right after its state is resolved (before a culled draw is dropped and before small meshes are
// moved to eye space): a draw with Z-freeze off becomes the reference. v: the draw's vertices as decoded (model space).
void ZFreezeRecord(uint8_t gxPrim, const uint8_t* v, const VtxLayout& L, uint32_t count) {
    if (!ZFreezeTracking()) return;
    uint32_t idx[3];
    switch (gxPrim & 0xF8) {
    case 0x80: case 0x88: if (count < 4) return; idx[0] = count - 4 - (count % 4); idx[1] = idx[0] + 2; idx[2] = idx[0] + 3; break;
    case 0x90: if (count < 3) return; idx[0] = count - 3 - (count % 3); idx[1] = idx[0] + 1; idx[2] = idx[0] + 2; break;
    case 0x98: if (count < 3) return; idx[0] = count - 3; idx[1] = count - 2; idx[2] = count - 1; break;
    case 0xA0: if (count < 3) return; idx[0] = 0; idx[1] = count - 2; idx[2] = count - 1; break;
    default: return; // lines and points set up no triangle: the reference stays
    }
    if (cur.vs.perVtxPos || g_blockOff[UB_PROJ] == kNoBlock) { g_zf.have = false; ++g_zf.version; return; } // matrix per vertex: cannot be followed here
    ZfRef& z = g_zf;
    for (int i = 0; i < 3; ++i) std::memcpy(z.pos[i], v + static_cast<size_t>(idx[i]) * L.stride, 12);
    const uint32_t pi = g.matIdxA & 0x3F;
    for (int r = 0; r < 3; ++r) std::memcpy(&z.mv[r * 4], g.xfPos[pi + r < 64 ? pi + r : 63], 16);
    std::memcpy(z.proj, g_buf.uni.data() + g_blockOff[UB_PROJ], sizeof z.proj);
    z.vp = zfreeze::Viewport{cur.rs.vpX, cur.rs.vpY, cur.rs.vpW, cur.rs.vpH, cur.rs.zNear, cur.rs.zFar};
    z.have = true;
    ++z.version;
}
void ZFreezeNote(const RenderState& rs, uint8_t prim, uint32_t count, uint32_t layoutMask, bool cullAll, const uint8_t* v = nullptr, uint32_t stride = 0) {
    static const long mode = EnvInt("MSC_VGX_ZFTRACE", 0);
    if (!mode) return;
    ++g_zfd.draws;
    g_zfd.verts += count;
    if (!g_zf.have) ++g_zfs.noReference;
    // How far the frozen mesh's own vertices are from the reference plane (window depth units): a flattened shadow that lies in
    // the plane the game chose differs by rounding only.
    zfreeze::Plane pl;
    if (v && !cur.vs.perVtxPos && ZFreezePlane(pl) && g_blockOff[UB_PROJ] != kNoBlock) {
        const uint32_t pi = g.matIdxA & 0x3F;
        float mv[12];
        for (int r = 0; r < 3; ++r) std::memcpy(&mv[r * 4], g.xfPos[pi + r < 64 ? pi + r : 63], 16);
        const zfreeze::Viewport vp{rs.vpX, rs.vpY, rs.vpW, rs.vpH, rs.zNear, rs.zFar};
        for (uint32_t i = 0; i < count && i < 3; ++i) {
            float pos[3], clip[4], win[3];
            std::memcpy(pos, v + static_cast<size_t>(i) * stride, 12);
            zfreeze::ModelToClip(pos, mv, g_buf.uni.data() + g_blockOff[UB_PROJ], clip);
            if (!(clip[3] > 1e-6f)) continue;
            zfreeze::ClipToWindow(clip, vp, win);
            const double dz = std::fabs(static_cast<double>(win[2]) - zfreeze::Eval(pl, win[0], win[1]));
            ++g_zfs.measured;
            if (dz > g_zfs.maxDz) g_zfs.maxDz = dz;
            g_zfs.sumDz += dz;
            g_zfs.over1e5 += dz > 1e-5;
        }
    }
    if (g_zfd.lastFrame != g_frame) { g_zfd.lastFrame = g_frame; ++g_zfd.frames; }
    ++g_zfd.combos[(static_cast<uint32_t>(cullAll) << 20) | (static_cast<uint32_t>(rs.cull) << 16) | (static_cast<uint32_t>(rs.blend) << 15) |
                  (static_cast<uint32_t>(rs.depthTest) << 14) | (static_cast<uint32_t>(rs.depthWrite) << 13) | (static_cast<uint32_t>(rs.depthFunc) << 8) |
                  (static_cast<uint32_t>(prim) << 4)];
    if (mode > 1 && static_cast<uint64_t>(mode) == g_frame)
        std::fprintf(stderr, "[zftrace] frame %llu: %u vertices prim %u layout 0x%X cull %u%s depth test %u func %u write %u blend %u (src %u dst %u) colour write %u alpha write %u\n",
                     static_cast<unsigned long long>(g_frame), count, prim, layoutMask, rs.cull, cullAll ? " (culls everything)" : "", rs.depthTest, rs.depthFunc,
                     rs.depthWrite, rs.blend, rs.blendSrc, rs.blendDst, rs.colorWrite, rs.alphaWrite);
}
void ZFreezeReport() {
    static const long mode = EnvInt("MSC_VGX_ZFTRACE", 0);
    if (!mode || g_frame % 1000 != 0) return;
    std::fprintf(stderr, "[zftrace] up to frame %llu: Z-freeze switched on %llu times by the game; %llu draws with Z-freeze in %llu frames, %llu vertices; %llu of them without a reference; %llu vertices measured against the reference plane: mean |dz| %.3g, max %.3g, %llu over 1e-5\n",
                 static_cast<unsigned long long>(g_frame), static_cast<unsigned long long>(g_coPlanarOn), static_cast<unsigned long long>(g_zfd.draws), static_cast<unsigned long long>(g_zfd.frames),
                 static_cast<unsigned long long>(g_zfd.verts), static_cast<unsigned long long>(g_zfs.noReference), static_cast<unsigned long long>(g_zfs.measured),
                 g_zfs.measured ? g_zfs.sumDz / static_cast<double>(g_zfs.measured) : 0.0, g_zfs.maxDz, static_cast<unsigned long long>(g_zfs.over1e5));
    for (const auto& kv : g_zfd.combos)
        std::fprintf(stderr, "[zftrace]   cullAll %u cull %u blend %u depth test %u write %u func %u prim %u: %llu draws\n", (kv.first >> 20) & 1, (kv.first >> 16) & 15,
                     (kv.first >> 15) & 1, (kv.first >> 14) & 1, (kv.first >> 13) & 1, (kv.first >> 8) & 31, (kv.first >> 4) & 15, static_cast<unsigned long long>(kv.second));
}

// The tail of the merge test: the uniform-block offsets, the render state and the texture bindings are the same (GXD_WORDCMP: as words, without
// three library calls; each of the three is a whole number of words).
static_assert(sizeof(RenderState) % 4 == 0 && sizeof(TexBind) % 4 == 0 && sizeof(g_blockOff) % 4 == 0, "word compares");
static inline bool SameBlocks(const DrawRec& p, const RenderState& rs) {
    if (Gxd(GXD_WORDCMP))
        return SameWords(p.ublock, g_blockOff, UB_COUNT) && SameWords(&p.rs, &rs, sizeof(RenderState) / 4) && SameWords(p.tex, cur.tex, sizeof cur.tex / 4);
    return std::memcmp(p.ublock, g_blockOff, sizeof g_blockOff) == 0 && std::memcmp(&p.rs, &rs, sizeof(RenderState)) == 0 &&
           std::memcmp(p.tex, cur.tex, sizeof cur.tex) == 0;
}

static inline bool SameDrawState(const DrawRec& a, const DrawRec& b) { // (uniform-block offsets, render state, texture bindings)
    if (a.skinBlock != b.skinBlock) return false; // (W5 SKIN)
    if (Gxd(GXD_WORDCMP))
        return SameWords(a.ublock, b.ublock, UB_COUNT) && SameWords(&a.rs, &b.rs, sizeof(RenderState) / 4) && SameWords(a.tex, b.tex, sizeof a.tex / 4);
    return std::memcmp(a.ublock, b.ublock, sizeof a.ublock) == 0 && std::memcmp(&a.rs, &b.rs, sizeof(RenderState)) == 0 && std::memcmp(a.tex, b.tex, sizeof a.tex) == 0;
}

// ---- soft instancing (W5 SKIN, MSC_VGX_SOFTINST=<K>, default 0 = off)
// Small meshes from cached display lists that the front end would transform to eye space on the CPU and merge (SoftTransform; in the 3D menus
// 825 of them a frame from 147 lists, 1.4 MB of stream) are drawn instead from a persistent buffer that holds the model-space vertices of up to
// K of them in the order they come, each vertex naming its instance's matrices through pos.w (3 x the instance), with the existing short-palette
// shaders: the instances' position and normal matrices are the draw's palette (P = 3K position rows, then the normal rows). The buffer is a
// function of the sequence of (vertex count, XXH3 of the decoded vertices) and the layout, so a frame that draws the same meshes in the same order
// finds last frame's buffer and streams nothing but the palette. The batch's draw record is made at its first instance (draw order) and finished
// when the batch closes: at a draw that does not join, at a static draw and at the end of the frame; an upload of a new buffer is put in front of it.
// No copy while the batch can still be found: the running key of the instances so far is looked up among the prefixes of the live buffers; only
// when it is not there (a new sequence) are the vertices gathered, the ones of the earlier instances from the buffer whose prefix they were.
namespace {
struct SiInst {
    float mv[12], nrm[9];
};
struct SiBuf {
    uint32_t bufId = 0, vtxCount = 0, idxCount = 0;
    uint64_t lastUse = 0;
    std::vector<uint8_t> vtx;          // CPU copy of what was uploaded (for a batch that starts like this one)
    std::vector<uint16_t> idx;
    std::vector<uint32_t> instVtx, instIdx; // where instance k starts (bytes, indices)
    std::vector<uint64_t> prefixes;    // the running key after each instance
};
struct SiState {
    bool open = false, gathered = false;
    int drawIdx = -1;
    size_t cmdIdx = 0;
    uint8_t gxPrim = 0;
    uint32_t layoutMask = 0, vtxCount = 0, idxCount = 0;
    uint16_t softVsId = 0, fsId = 0;
    uint64_t runKey = 0, repKey = 0;   // running key; a live buffer that starts with the instances so far (while !gathered)
    std::vector<SiInst> inst;
    std::vector<uint64_t> keys;        // running key after each instance
    std::vector<uint8_t> vtx;
    std::vector<uint16_t> idx;
    std::vector<uint32_t> instVtx, instIdx;
    std::unordered_map<uint64_t, SiBuf> bufs;
    std::unordered_map<uint64_t, uint64_t> prefix; // running key -> a live buffer that has it
    uint64_t batches = 0, instances = 0, hits = 0, misses = 0, uploadBytes = 0, lastSweep = 0, frames = 0, gathers = 0;
    // MSC_VGX_SOFTINST_CHECK=1 (diagnostic): every batch's vertices are also gathered the plain way and compared with the buffer it is drawn from
    std::vector<uint8_t> chkV;
    std::vector<uint16_t> chkI;
    uint64_t checked = 0, checkBad = 0;
} g_si;
bool SoftInstCheck() {
    static const bool on = EnvInt("MSC_VGX_SOFTINST_CHECK", 0) != 0;
    return on;
}
uint32_t SoftInstK() {
    static const uint32_t k = [] {
        const long v = EnvInt("MSC_VGX_SOFTINST", 0);
        return static_cast<uint32_t>(v <= 0 ? 0 : (v > 21 ? 21 : v));
    }();
    return k;
}
// The vertices and indices of the first n instances, from the buffer the batch has been a prefix of.
void SiGather(uint32_t n) {
    SiState& b = g_si;
    b.gathered = true;
    ++b.gathers;
    b.vtx.clear(); b.idx.clear(); b.instVtx.clear(); b.instIdx.clear();
    if (!n) return;
    const SiBuf& r = b.bufs[b.repKey];
    const uint32_t vEnd = n < r.instVtx.size() ? r.instVtx[n] : static_cast<uint32_t>(r.vtx.size());
    const uint32_t iEnd = n < r.instIdx.size() ? r.instIdx[n] : static_cast<uint32_t>(r.idx.size());
    b.vtx.assign(r.vtx.begin(), r.vtx.begin() + vEnd);
    b.idx.assign(r.idx.begin(), r.idx.begin() + iEnd);
    b.instVtx.assign(r.instVtx.begin(), r.instVtx.begin() + n);
    b.instIdx.assign(r.instIdx.begin(), r.instIdx.begin() + n);
}
void SiPrefixRebuild() {
    SiState& b = g_si;
    b.prefix.clear();
    for (auto& kv : b.bufs)
        for (uint64_t k : kv.second.prefixes) b.prefix.emplace(k, kv.first);
}
// Finishes the open batch: its buffer (found or made), its palette, its record.
void SiClose() {
    SiState& b = g_si;
    if (!b.open) return;
    b.open = false;
    if (b.drawIdx < 0 || static_cast<size_t>(b.drawIdx) >= g_buf.draws.size()) return;
    const uint64_t key = b.runKey;
    auto it = b.bufs.find(key);
    if (it == b.bufs.end()) {
        if (!b.gathered) SiGather(static_cast<uint32_t>(b.inst.size())); // (the whole batch is the start of a longer one)
        SiBuf& sb = b.bufs[key];
        BufUpload up;
        up.bufId = sb.bufId = AllocStaticBufferId();
        up.vtx = b.vtx;
        up.idx = b.idx;
        sb.vtxCount = b.vtxCount;
        sb.idxCount = b.idxCount;
        sb.vtx = std::move(b.vtx);
        sb.idx = std::move(b.idx);
        sb.instVtx = b.instVtx;
        sb.instIdx = b.instIdx;
        sb.prefixes = b.keys;
        for (uint64_t k : b.keys) b.prefix.emplace(k, key);
        b.vtx.clear(); b.idx.clear();
        ++b.misses;
        b.uploadBytes += up.vtx.size() + up.idx.size() * 2;
        stats::Frame& fr = stats::Cur();
        ++fr.bufUploads;
        fr.bufUploadBytes += static_cast<uint32_t>(up.vtx.size() + up.idx.size() * 2);
        g_buf.bufUploads.push_back(std::move(up));
        g_buf.cmds.insert(g_buf.cmds.begin() + static_cast<std::ptrdiff_t>(b.cmdIdx), Cmd{CMD_BUF_UPLOAD, static_cast<uint32_t>(g_buf.bufUploads.size() - 1)});
        it = b.bufs.find(key);
    } else {
        ++b.hits;
    }
    SiBuf& sb = it->second;
    sb.lastUse = g_frame;
    if (SoftInstCheck()) {
        ++b.checked;
        if (b.chkV != sb.vtx || b.chkI != sb.idx) {
            if (++b.checkBad <= 8) std::fprintf(stderr, "[softinst] CHECK FAILED frame %llu: batch of %lu instances drawn from a buffer whose vertices differ (%lu / %lu bytes)\n",
                                                 static_cast<unsigned long long>(g_frame), static_cast<unsigned long>(b.inst.size()), static_cast<unsigned long>(b.chkV.size()), static_cast<unsigned long>(sb.vtx.size()));
        }
    }
    // the palette: position rows 3k.. of instance k, then (from row P) its normal rows
    const uint32_t P = 3u * SoftInstK();
    const size_t at = g_buf.uni.size();
    g_buf.uni.resize(at + static_cast<size_t>(2u * P) * 4u);
    float* d = g_buf.uni.data() + at;
    std::memset(d, 0, static_cast<size_t>(2u * P) * 16u);
    for (size_t k = 0; k < b.inst.size(); ++k) {
        std::memcpy(d + k * 12, b.inst[k].mv, 48);
        for (int r = 0; r < 3; ++r) std::memcpy(d + (P + 3 * k + r) * 4, b.inst[k].nrm + 3 * r, 12);
    }
    DrawRec& rec = g_buf.draws[static_cast<size_t>(b.drawIdx)];
    rec.ublock[UB_PAL] = static_cast<uint32_t>(at);
    rec.vbuf = sb.bufId;
    rec.vtxOffset = 0;
    rec.vtxFirst = 0;
    rec.vchunk = 0;
    rec.idxOffset = 0;
    rec.vtxCount = sb.vtxCount;
    rec.idxCount = sb.idxCount;
    ++b.batches;
    b.instances += b.inst.size();
    stats::Frame& fr = stats::Cur();
    ++fr.staticDraws;
    fr.staticVerts += sb.vtxCount;
    if (TagCensusOn()) TagCensusDraw(sb.vtxCount, 0, 0, true);
}
// End of frame: close, report, drop buffers not drawn for 600 frames.
void SiFrame() {
    SiClose();
    SiState& b = g_si;
    ++b.frames;
    if (g_frame - b.lastSweep >= 120) {
        b.lastSweep = g_frame;
        bool dropped = false;
        for (auto it = b.bufs.begin(); it != b.bufs.end();) {
            if (g_frame - it->second.lastUse > 600) { FreeStaticBufferId(it->second.bufId); it = b.bufs.erase(it); dropped = true; }
            else ++it;
        }
        if (dropped) SiPrefixRebuild();
    }
    if (g_frame % 600 == 0 && b.frames) {
        std::fprintf(stderr, "[softinst] frame %llu, per frame over %llu frames: %.1f batches, %.1f instances, %.1f buffers found, %.1f made (%.0f bytes uploaded), %.1f gathered; %lu buffers live\n",
                     static_cast<unsigned long long>(g_frame), static_cast<unsigned long long>(b.frames), b.batches / double(b.frames), b.instances / double(b.frames),
                     b.hits / double(b.frames), b.misses / double(b.frames), b.uploadBytes / double(b.frames), b.gathers / double(b.frames), static_cast<unsigned long>(b.bufs.size()));
        if (SoftInstCheck()) std::fprintf(stderr, "[softinst] check: %llu batches compared with their buffer, %llu differ\n", static_cast<unsigned long long>(b.checked),
                                          static_cast<unsigned long long>(b.checkBad));
        b.batches = b.instances = b.hits = b.misses = b.uploadBytes = b.frames = b.gathers = 0;
    }
}
// A soft draw of `count` decoded vertices at `v` (layout L, model space) from a replayed list (its state resolved): joins the open batch or opens one.
// False: not taken (the caller transforms it on the CPU as before).
bool SiTake(uint8_t gxPrim, uint32_t layoutMask, uint32_t count, const uint8_t* v, const VtxLayout& L) {
    const uint32_t K = SoftInstK();
    if (!K || !g_replayList || ZFreezeTracking() || TraceFrame()) return false;
    const uint32_t idxCount = PrimIndices(gxPrim, count, 0, nullptr);
    SiState& b = g_si;
    const bool joins = b.open && b.drawIdx == static_cast<int>(g_buf.draws.size()) - 1 && b.cmdIdx + 1 == g_buf.cmds.size() && b.inst.size() < K &&
                       b.layoutMask == layoutMask && b.gxPrim == gxPrim && b.softVsId == cur.vsId && b.fsId == cur.fsId &&
                       b.vtxCount + count <= 0xFFFFu && [&] {
                           const DrawRec& r = g_buf.draws[static_cast<size_t>(b.drawIdx)];
                           if (r.texMask != cur.texMask || r.prim != PrimKind(gxPrim)) return false;
                           for (int k = 0; k < UB_COUNT; ++k)
                               if (k != UB_PAL && r.ublock[k] != g_blockOff[k]) return false;
                           return SameWords(&r.rs, &cur.rs, sizeof(RenderState) / 4) && SameWords(r.tex, cur.tex, sizeof cur.tex / 4);
                       }();
    if (!joins) {
        SiClose();
        // the palette variant of the soft draw's vertex key
        VsKey k = cur.vs;
        k.perVtxPos = 2;
        const uint32_t P = 3u * K;
        k.palRows = static_cast<uint8_t>(P == static_cast<uint32_t>(kPalShortPos) ? 0 : P);
        const uint16_t vsId = Lookup(g_vsReg, k, false, GenerateVertexShader);
        const uint16_t softVsId = cur.vsId;
        cur.vsId = vsId;
        DrawRec* rec = NewDrawRec(PrimKind(gxPrim), layoutMask);
        cur.vsId = softVsId;
        rec->vbuf = 0;
        rec->idxCount = 0; // (finished by SiClose)
        b.open = true;
        b.gathered = false;
        b.drawIdx = static_cast<int>(g_buf.draws.size()) - 1;
        b.cmdIdx = g_buf.cmds.size() - 1;
        b.gxPrim = gxPrim;
        b.layoutMask = layoutMask;
        b.softVsId = softVsId;
        b.fsId = cur.fsId;
        b.vtxCount = b.idxCount = 0;
        b.runKey = XXH3_64bits_withSeed(&layoutMask, 4, gxPrim);
        b.repKey = 0;
        b.inst.clear();
        b.keys.clear();
        b.vtx.clear(); b.idx.clear(); b.instVtx.clear(); b.instIdx.clear();
        b.chkV.clear(); b.chkI.clear();
    }
    if (SoftInstCheck()) { // (the plain gathering, as without prediction)
        const float row = static_cast<float>(3u * b.inst.size());
        const size_t at = b.chkV.size();
        b.chkV.insert(b.chkV.end(), v, v + static_cast<size_t>(count) * L.stride);
        for (uint32_t i = 0; i < count; ++i) std::memcpy(b.chkV.data() + at + static_cast<size_t>(i) * L.stride + 12, &row, 4);
        const size_t io = b.chkI.size();
        b.chkI.resize(io + idxCount);
        PrimIndices(gxPrim, count, b.vtxCount, b.chkI.data() + io);
    }
    const uint64_t w[2] = {count, g_replayHash}; // (the segment's decoded vertices: lists that draw the same vertices share buffers)
    const uint64_t key = XXH3_64bits_withSeed(w, sizeof w, b.runKey);
    if (!b.gathered) {
        auto pf = b.prefix.find(key);
        if (pf != b.prefix.end()) b.repKey = pf->second; // still the start of a known buffer: nothing to copy
        else SiGather(static_cast<uint32_t>(b.inst.size()));
    }
    if (b.gathered) {
        const float row = static_cast<float>(3u * b.inst.size());
        const size_t at = b.vtx.size();
        b.instVtx.push_back(static_cast<uint32_t>(at));
        b.instIdx.push_back(static_cast<uint32_t>(b.idx.size()));
        b.vtx.resize(at + static_cast<size_t>(count) * L.stride);
        std::memcpy(b.vtx.data() + at, v, static_cast<size_t>(count) * L.stride);
        for (uint32_t i = 0; i < count; ++i) std::memcpy(b.vtx.data() + at + static_cast<size_t>(i) * L.stride + 12, &row, 4);
        const size_t io = b.idx.size();
        b.idx.resize(io + idxCount);
        PrimIndices(gxPrim, count, b.vtxCount, b.idx.data() + io);
    }
    b.runKey = key;
    b.keys.push_back(key);
    SiInst in;
    const uint32_t pi = g.matIdxA & 0x3F;
    std::memcpy(in.mv, g.xfPos[pi < 62 ? pi : 61], 48);
    std::memcpy(in.nrm, g.xfNrm[(pi & 31) < 30 ? (pi & 31) : 29], 36);
    b.inst.push_back(in);
    b.vtxCount += count;
    b.idxCount += idxCount;
    {
        stats::Frame& fr = stats::Cur();
        ++fr.prims;
        fr.verts += count;
        if (PrimKind(gxPrim) == PRIM_TRIANGLES) fr.tris += idxCount / 3;
    }
    return true;
}
} // namespace

// The display-list cache's replay of a list segment (vgx_dlcache.cpp Emit), before it copies the vertices into the frame's arena: a draw that soft
// instancing takes is taken straight from the cache entry's vertices (no arena copy), a culled one is dropped; false: the caller goes the usual way.
bool SoftInstOn() { return SoftInstK() != 0; }
bool SoftInstEmit(uint8_t gxPrim, uint32_t layoutMask, uint32_t count, const uint8_t* v) {
    if (!SoftInstK() || (layoutMask & VL_SKIN) || count == 0 || count > SoftXformLimit() || ZFreezeTracking() || TraceFrame()) return false;
    if (PrimIndices(gxPrim, count, 0, nullptr) == 0) return false;
    uint32_t vcdMtxMask = 0;
    for (int i = 0; i <= 8; ++i)
        if (g.vcd[i]) vcdMtxMask |= 1u << i;
    if (vcdMtxMask & 1u) return false; // (a matrix per vertex: never soft)
    const uint32_t posMtxMax = g_pendingPosMtxMax;
    g_pendingSkin = 0;
    ResolveState(vcdMtxMask, count);
    if (g.bp[0x00] & (1u << 19)) { g_pendingPosMtxMax = posMtxMax; return false; } // (Z-freeze draws: the usual way)
    if (cur.cullAll && PrimKind(gxPrim) == PRIM_TRIANGLES) return true;
    if (!cur.soft) { g_pendingPosMtxMax = posMtxMax; return false; }
    const VtxLayout L = MakeLayout(layoutMask);
    if (SiTake(gxPrim, layoutMask, count, v, L)) return true;
    g_pendingPosMtxMax = posMtxMax;
    return false;
}

void EmitDraw(uint8_t gxPrim, uint32_t layoutMask, uint32_t count) {
    const VtxLayout L = MakeLayout(layoutMask);
    g_buf.vtx.resize(g_pendingVtxStart + static_cast<size_t>(count) * L.stride);
    auto drop = [&] { g_buf.vtx.resize(g_pendingVtxStart); };
    if (count == 0) return;

    const uint8_t prim = PrimKind(gxPrim);
    const uint32_t idxCount = PrimIndices(gxPrim, count, 0, nullptr);
    if (idxCount == 0) { drop(); return; }

    uint32_t vcdMtxMask = 0;
    for (int i = 0; i <= 8; ++i)
        if (g.vcd[i]) vcdMtxMask |= 1u << i;
    g_pendingSkin = (layoutMask & VL_SKIN) ? 1 : 0;
    ResolveState(vcdMtxMask, count);
    if (g.bp[0x00] & (1u << 19)) ZFreezeNote(cur.rs, prim, count, layoutMask, cur.cullAll, g_buf.vtx.data() + g_pendingVtxStart, L.stride);
    else ZFreezeRecord(gxPrim, g_buf.vtx.data() + g_pendingVtxStart, L, count);
    if (cur.cullAll && prim == PRIM_TRIANGLES) { drop(); return; }
    if (cur.soft && SiTake(gxPrim, layoutMask, count, g_buf.vtx.data() + g_pendingVtxStart, L)) { drop(); return; } // (W5 SKIN soft instancing)
    SiClose();
    if (StreamCensusOn()) StreamCensusDraw(cur.vs, layoutMask, count, cur.soft);
    if (TagCensusOn()) TagCensusDraw(count, count * L.stride, idxCount, false);
    if (cur.soft && SoftCensusOn()) {
        const uint32_t pi = g.matIdxA & 0x3F;
        SoftCensusDraw(count, layoutMask, g.xfPos[pi < 62 ? pi : 61], g.xfNrm[(pi & 31) < 30 ? (pi & 31) : 29]);
    }
    if (cur.soft) SoftTransform(g_buf.vtx.data() + g_pendingVtxStart, count, L);

    // Merge into the previous record when nothing but the geometry differs.
    // (MSC_VGX_NOMERGE=1 turns merging off: one record per primitive, for comparing runs.)
    static const bool noMerge = EnvInt("MSC_VGX_NOMERGE", 0) != 0;
    DrawRec* rec = nullptr;
    const bool abs = AbsoluteIndices();
    const uint16_t chunk = abs ? static_cast<uint16_t>(g_buf.vtxChunks.size() - 1) : 0;
    uint32_t base = abs ? g_pendingFirst : 0; // what the new primitive's 0-based indices are offset by
    if (!noMerge && g_lastDraw >= 0 && g_lastDraw == static_cast<int>(g_buf.draws.size()) - 1 && !g_buf.cmds.empty() &&
        g_buf.cmds.back().type == CMD_DRAW) {
        DrawRec& p = g_buf.draws[g_lastDraw];
        // (the new vertices follow the record's last one: by index within the same chunk, or by byte offset)
        const bool contiguous = abs ? (p.vchunk == chunk && p.vtxFirst + p.vtxCount == g_pendingFirst)
                                    : (p.vtxOffset + static_cast<size_t>(p.vtxCount) * L.stride == g_pendingVtxStart);
        if (p.vbuf == 0 && p.prim == prim && p.layoutMask == layoutMask && p.vsId == cur.vsId && p.fsId == cur.fsId &&
            p.vtxCount + count <= 0xFFFF && contiguous &&
            p.idxOffset + p.idxCount == g_buf.idx.size() && p.texMask == cur.texMask && SameBlocks(p, cur.rs) &&
            p.skinBlock == ((layoutMask & VL_SKIN) ? g_skinBlockOff : kNoBlock)) {
            rec = &p;
            if (!abs) base = p.vtxCount;
        }
    }
    if (!rec) {
        if (count > 0xFFFF) { drop(); return; }
        rec = NewDrawRec(prim, layoutMask);
        if (abs) {
            rec->vtxOffset = 0;
            rec->vtxFirst = g_pendingFirst;
            rec->vchunk = chunk;
        } else {
            rec->vtxOffset = static_cast<uint32_t>(g_pendingVtxStart);
        }
        rec->idxOffset = static_cast<uint32_t>(g_buf.idx.size());
    }
    const size_t io = g_buf.idx.size();
    g_buf.idx.resize(io + idxCount);
    PrimIndices(gxPrim, count, base, g_buf.idx.data() + io);
    rec->vtxCount += count;
    rec->idxCount += idxCount;
    ++g_frameDraws;
    {
        stats::Frame& fr = stats::Cur();
        ++fr.prims;
        fr.verts += count;
        if (prim == PRIM_TRIANGLES) fr.tris += idxCount / 3;
        if (cur.soft) fr.softVerts += count;
    }
    if (stats::Detailed()) stats::Draw(cur.fs, cur.vs, layoutMask, count);
    if (TraceFrame()) TraceDraw(gxPrim, layoutMask, count, g_buf.vtx.data() + g_pendingVtxStart);
}

void EmitStaticDraw(uint8_t gxPrim, uint32_t layoutMask, uint32_t count, uint32_t bufId, uint32_t vtxByteOffset, uint32_t idxOffset,
                    uint32_t vtxFirst) {
    const uint8_t prim = PrimKind(gxPrim);
    const uint32_t idxCount = PrimIndices(gxPrim, count, 0, nullptr);
    if (count == 0 || idxCount == 0 || count > 0xFFFF) return;
    SiClose(); // (W5 SKIN soft instancing: the open batch ends before any other draw)
    uint32_t vcdMtxMask = 0;
    for (int i = 0; i <= 8; ++i)
        if (g.vcd[i]) vcdMtxMask |= 1u << i;
    // Larger than the CPU-transform limit by construction (see vgx_dlcache.cpp), so never "soft".
    g_pendingSkin = (layoutMask & VL_SKIN) ? 1 : 0;
    ResolveState(vcdMtxMask, 0xFFFFFFFFu);
    if (g.bp[0x00] & (1u << 19)) ZFreezeNote(cur.rs, prim, count, layoutMask, cur.cullAll);
    else if (ZFreezeTracking() && prim == PRIM_TRIANGLES) { g_zf.have = false; ++g_zf.version; } // (geometry that is not in the arena: not followed)
    if (cur.cullAll && prim == PRIM_TRIANGLES) return;
    if (TagCensusOn()) TagCensusDraw(count, 0, 0, true);
    DrawRec* rec = NewDrawRec(prim, layoutMask);
    rec->vbuf = bufId;
    rec->vtxOffset = vtxByteOffset;
    rec->vtxFirst = vtxFirst;
    rec->idxOffset = idxOffset;
    rec->vtxCount = count;
    rec->idxCount = idxCount;
    ++g_frameDraws;
    {
        stats::Frame& fr = stats::Cur();
        ++fr.prims;
        fr.verts += count;
        ++fr.staticDraws;
        fr.staticVerts += count;
        if (prim == PRIM_TRIANGLES) fr.tris += idxCount / 3;
    }
    if (stats::Detailed()) stats::Draw(cur.fs, cur.vs, layoutMask, count);
}

// MSC_VGX_HASH=1: a hash of everything the back end is handed for a frame
// (commands, draw records, vertex / index / uniform arenas, texture uploads,
// shader sources, copies), written to the per-frame CSV. Two builds whose
// hash columns agree hand identical frames to the back end, which checks
// "pictures unchanged" for every frame of a run without rasterising any.
// MSC_VGX_HASH=2 ("content"): each draw is hashed with the vertices and indices it refers to
// instead of by position, so runs that keep geometry in different places (frame arenas / static
// buffers) agree when they draw the same things. Use with MSC_VGX_NOMERGE=1 on both sides.
static uint64_t HashBuf(const CmdBuf& b, uint64_t h, bool content) {
    auto mix = [&h](const void* p, size_t n) { if (n) h = XXH3_64bits_withSeed(p, n, h); };
    for (const Cmd& c : b.cmds) {
        if (content && (c.type == CMD_BUF_UPLOAD || c.type == CMD_BUF_FREE)) continue;
        const uint32_t v[2] = {c.type, (c.type == CMD_TEX_FREE || c.type == CMD_BUF_FREE || c.type == CMD_PROGRAM) ? c.arg : 0u};
        mix(v, sizeof v);
    }
    std::vector<uint16_t> rel;
    for (const DrawRec& d : b.draws) {
        if (content) {
            // What the draw draws, wherever the bytes live and however it addresses them:
            // its vertices, and its indices counted from its own first vertex.
            const uint32_t v[7] = {d.vsId, d.fsId, d.prim, d.texMask, d.layoutMask, d.vtxCount, d.idxCount};
            mix(v, sizeof v);
            const uint8_t* vp = b.vtx.data() + (b.vtxChunks.empty() ? 0 : b.vtxChunks[d.vchunk]);
            const uint16_t* ip = b.idx.data();
            if (d.vbuf && !StaticBufferData(d.vbuf, vp, ip)) { const uint32_t bad = 0xDEADBEEFu; mix(&bad, 4); continue; }
            const size_t stride = MakeLayout(d.layoutMask).stride;
            mix(vp + d.vtxOffset + d.vtxFirst * stride, static_cast<size_t>(d.vtxCount) * stride);
            if (d.vtxFirst) {
                rel.resize(d.idxCount);
                for (uint32_t i = 0; i < d.idxCount; ++i) rel[i] = static_cast<uint16_t>(ip[d.idxOffset + i] - d.vtxFirst);
                mix(rel.data(), static_cast<size_t>(d.idxCount) * 2);
            } else {
                mix(ip + d.idxOffset, static_cast<size_t>(d.idxCount) * 2);
            }
        } else {
            const uint32_t v[9] = {d.vsId, d.fsId, d.prim, d.texMask, d.layoutMask, d.vtxOffset, d.vtxCount, d.idxOffset, d.vbuf};
            mix(v, d.vbuf ? sizeof v : sizeof v - 4); // (unchanged hash for frames without static buffers)
            mix(&d.idxCount, 4);
            if (d.vtxFirst || d.vchunk) { const uint32_t a[2] = {d.vtxFirst, d.vchunk}; mix(a, sizeof a); } // (unchanged hash in the old addressing scheme)
        }
        mix(d.ublock, sizeof d.ublock);
        if (d.skinBlock != kNoBlock) mix(&d.skinBlock, 4); // (W5 SKIN: unchanged hash for draws that are not skinned on the GPU)
        mix(&d.rs, sizeof d.rs);   // no padding (12 x u8, 4 x s16, 6 x f32)
        mix(d.tex, sizeof d.tex);  // no padding
    }
    if (!content)
        for (const BufUpload& u : b.bufUploads) {
            mix(&u.bufId, 4);
            mix(u.vtx.data(), u.vtx.size());
            mix(u.idx.data(), u.idx.size() * 2);
        }
    for (const TexUpload& u : b.uploads) {
        const uint32_t v[3] = {u.texId, u.width, u.height};
        mix(v, sizeof v);
        if (u.format != TF_RGBA8 || u.genMips) { const uint32_t f[2] = {u.format, u.genMips}; mix(f, sizeof f); } // (unchanged hash for plain RGBA8 uploads)
        for (const auto& l : u.levels) mix(l.data(), l.size());
    }
    for (const ShaderSource* sh : b.shaders) {
        const uint32_t v[2] = {sh->id, sh->fragment};
        mix(v, sizeof v);
        mix(sh->text.data(), sh->text.size());
    }
    for (const CopyTexCmd& c : b.copyTex) {
        const int32_t v[8] = {static_cast<int32_t>(c.texId), c.src.x, c.src.y, c.src.w, c.src.h, c.dstW, c.dstH, c.depth};
        mix(v, sizeof v);
        mix(c.mat, sizeof c.mat);
        mix(c.add, sizeof c.add);
        const float cl[8] = {float(c.clear.enable), float(c.clear.color), float(c.clear.alpha), float(c.clear.depth), c.clear.rgba[0],
                             c.clear.rgba[1], c.clear.rgba[2], c.clear.rgba[3]};
        mix(cl, sizeof cl);
        mix(&c.clear.z, 4);
        if (c.srcTex) { const int32_t a[7] = {static_cast<int32_t>(c.srcTex), c.srcTexW, c.srcTexH, c.dstX, c.dstY, c.dstTexW, c.dstTexH}; mix(a, sizeof a); }
        if (c.fromPass || c.fill) { // (unchanged hash for frames without isolated passes)
            const float f[6] = {float(c.fromPass), float(c.fill), c.fillRgba[0], c.fillRgba[1], c.fillRgba[2], c.fillRgba[3]};
            mix(f, sizeof f);
        }
    }
    for (const PassCmd& ps : b.passes) {
        const float v[10] = {float(ps.rect.x), float(ps.rect.y), float(ps.rect.w), float(ps.rect.h), ps.rgba[0], ps.rgba[1], ps.rgba[2], ps.rgba[3], ps.z, float(ps.planes)};
        mix(v, sizeof v);
        mix(&ps.directTex, 4);
        if (ps.atlasTex) { const uint32_t a[6] = {ps.atlasTex, ps.atlasW, ps.atlasH, ps.atlasX, ps.atlasY, ps.atlasGutter}; mix(a, sizeof a); }
    }
    for (const CopyDispCmd& c : b.copyDisp) {
        const float v[13] = {float(c.src.x), float(c.src.y), float(c.src.w), float(c.src.h), float(c.clear.enable), float(c.clear.color),
                             float(c.clear.alpha), float(c.clear.depth), c.clear.rgba[0], c.clear.rgba[1], c.clear.rgba[2], c.clear.rgba[3],
                             c.clear.z};
        mix(v, sizeof v);
    }
    for (const SetTargetCmd& t : b.targets) { const uint32_t v[2] = {t.efbW, t.efbH}; mix(v, sizeof v); }
    if (!content) {
        mix(b.vtx.data(), b.vtx.size());
        mix(b.idx.data(), b.idx.size() * 2);
        if (b.vtxChunks.size() > 1) mix(b.vtxChunks.data(), b.vtxChunks.size() * 4);
    }
    mix(b.uni.data(), b.uni.size() * 4);
    mix(b.variants.data(), b.variants.size() * sizeof(VariantCmd));
    return h;
}

// MSC_VGX_HASHDUMP=<dir>:<from>-<to> (diagnostic, W5 GXD; nothing is changed): for presented frames in the range, writes what the frame hash covers to
// <dir>/f<frame>.{uni,vtx,idx} (the arenas, raw), f<frame>.txt (one line per draw record with its arena offsets and block offsets, one per texture upload and
// shader with a hash of its bytes, one per bufUpload) so that two runs whose hashes differ can be compared byte for byte (tools/gxd/hashdiff.py).
// Found for the address-dependent hash noise of goal / replay frames (W5 GXD notes).
static void HashDump(const CmdBuf& b, uint64_t frame) {
    static const char* const spec = std::getenv("MSC_VGX_HASHDUMP");
    static std::string dir;
    static uint64_t from = 0, to = 0;
    static bool init = false, on = false;
    if (!init) {
        init = true;
        if (spec && *spec) {
            const char* c = std::strrchr(spec, ':');
            if (c) { dir.assign(spec, static_cast<size_t>(c - spec)); on = std::sscanf(c + 1, "%llu-%llu", reinterpret_cast<unsigned long long*>(&from), reinterpret_cast<unsigned long long*>(&to)) == 2; }
        }
    }
    if (!on || frame < from || frame > to) return;
    char name[512];
    auto dump = [&](const char* ext, const void* p, size_t n) {
        std::snprintf(name, sizeof name, "%s/f%llu.%s", dir.c_str(), static_cast<unsigned long long>(frame), ext);
        if (FILE* f = std::fopen(name, "wb")) { if (n) std::fwrite(p, 1, n, f); std::fclose(f); }
    };
    dump("uni", b.uni.data(), b.uni.size() * 4);
    dump("vtx", b.vtx.data(), b.vtx.size());
    dump("idx", b.idx.data(), b.idx.size() * 2);
    std::snprintf(name, sizeof name, "%s/f%llu.txt", dir.c_str(), static_cast<unsigned long long>(frame));
    FILE* f = std::fopen(name, "w");
    if (!f) return;
    std::fprintf(f, "frame %llu cmds %zu draws %zu uni_floats %zu vtx_bytes %zu idx %zu chunks %zu variants %zu\n", static_cast<unsigned long long>(frame), b.cmds.size(),
                 b.draws.size(), b.uni.size(), b.vtx.size(), b.idx.size(), b.vtxChunks.size(), b.variants.size());
    for (size_t i = 0; i < b.draws.size(); ++i) {
        const DrawRec& d = b.draws[i];
        std::fprintf(f, "D %zu vs %u fs %u prim %u tm %u layout 0x%X vbuf %u vtxoff %u vtxcnt %u idxoff %u idxcnt %u first %u chunk %u ub %u %u %u %u %u %u tex %u,%u,%u nrm %d\n", i, d.vsId, d.fsId, d.prim,
                     d.texMask, d.layoutMask, d.vbuf, d.vtxOffset, d.vtxCount, d.idxOffset, d.idxCount, d.vtxFirst, d.vchunk, d.ublock[0], d.ublock[1], d.ublock[2], d.ublock[3],
                     d.ublock[4], d.ublock[5], d.tex[0].texId, d.tex[1].texId, d.tex[2].texId,
                     d.vsId < g_vsReg.keys.size() ? (VsReadsNormal(g_vsReg.keys[d.vsId]) ? 1 : 0) : -1); // (nrm: the vertex shader reads the normal matrix)
    }
    for (const TexUpload& u : b.uploads) {
        uint64_t h = 0;
        for (const auto& l : u.levels) h = XXH3_64bits_withSeed(l.data(), l.size(), h);
        std::fprintf(f, "U tex %u %ux%u fmt %u levels %zu hash %016llx\n", u.texId, u.width, u.height, u.format, u.levels.size(), static_cast<unsigned long long>(h));
    }
    for (const ShaderSource* sh : b.shaders)
        std::fprintf(f, "S id %u frag %d hash %016llx\n", sh->id, sh->fragment ? 1 : 0, static_cast<unsigned long long>(XXH3_64bits(sh->text.data(), sh->text.size())));
    for (const BufUpload& u : b.bufUploads)
        std::fprintf(f, "B buf %u vtx %zu idx %zu hash %016llx\n", u.bufId, u.vtx.size(), u.idx.size(),
                     static_cast<unsigned long long>(XXH3_64bits_withSeed(u.idx.data(), u.idx.size() * 2, XXH3_64bits(u.vtx.data(), u.vtx.size()))));
    std::fclose(f);
}

// MSC_VGX_DRAWLOG=<dir> (diagnostic, np branch): a text log of what the back end is handed, one file per frame of
// MSC_VGX_DRAWLOG_FRAMES=a-b[,c[-d]...], for comparing the native game's frames with the recompiled build's (tools/drawdiff.py).
// Everything that depends on allocation order (shader / texture / buffer ids, arena offsets) is replaced by content
// hashes, so two runs that draw the same things write the same lines. MSC_VGX_DRAWLOG_FULL=1 adds the uniform values and
// the first vertices of every draw (=2: all of them).
namespace drawlog {
struct Range { uint64_t a, b; };
std::vector<Range> g_ranges;
bool g_on = false, g_full = false, g_fullAll = false, g_init = false;
std::string g_dir;
std::unordered_map<uint32_t, uint64_t> g_tex, g_vs, g_fs;     // id -> content hash
std::unordered_map<uint32_t, uint32_t> g_texLevels;           // id -> authored mip levels
void Init() {
    g_init = true;
    const char* d = std::getenv("MSC_VGX_DRAWLOG");
    if (!d || !*d) return;
    g_dir = d;
    g_full = EnvInt("MSC_VGX_DRAWLOG_FULL", 0) != 0;
    g_fullAll = EnvInt("MSC_VGX_DRAWLOG_FULL", 0) >= 2;   // 2: every vertex of every draw (up to 4096), not the first four
    const char* l = std::getenv("MSC_VGX_DRAWLOG_FRAMES");
    for (const char* p = l; p && *p;) {
        char* e = nullptr;
        const uint64_t a = std::strtoull(p, &e, 10);
        if (e == p) break;
        uint64_t b = a;
        if (*e == '-') { p = e + 1; b = std::strtoull(p, &e, 10); }
        g_ranges.push_back({a, b});
        if (*e != ',') break;
        p = e + 1;
    }
    g_on = true;
}
bool Want(uint64_t f) {
    for (const Range& r : g_ranges) if (f >= r.a && f <= r.b) return true;
    return false;
}
const char* Name(uint64_t h, char*) {   // (a ring of buffers: several calls in one printf)
    static char ring[16][20];
    static unsigned next = 0;
    char* b = ring[next++ & 15];
    std::snprintf(b, 20, "%016llx", static_cast<unsigned long long>(h));
    return b;
}
uint64_t Lookup(const std::unordered_map<uint32_t, uint64_t>& m, uint32_t id) {
    auto it = m.find(id);
    return it == m.end() ? 0 : it->second;
}
void Frame(const CmdBuf& b, uint64_t frame) {
    if (!g_init) Init();
    if (!g_on) return;
    // the id -> content maps follow every frame (uploads and shaders arrive in the frame that first needs them)
    for (const TexUpload& u : b.uploads) {
        // The identity of a texture is its format, size and LEVEL 0 (the levels below it differ with how many the game authored,
        // a back end's caps and, in the recompiled build, with what its texture objects remember of the game's LOD range).
        uint64_t h = XXH3_64bits(&u.format, 1);
        const uint32_t dims[2] = {u.width, u.height};
        h = XXH3_64bits_withSeed(dims, sizeof dims, h);
        if (!u.levels.empty()) h = XXH3_64bits_withSeed(u.levels[0].data(), u.levels[0].size(), h);
        g_tex[u.texId] = h;
        g_texLevels[u.texId] = u.authored;
    }
    for (const ShaderSource* sh : b.shaders) (sh->fragment ? g_fs : g_vs)[sh->id] = XXH3_64bits(sh->text.data(), sh->text.size());
    if (!Want(frame)) return;
    char path[1024], nb[24], nb2[24];
    std::snprintf(path, sizeof path, "%s/f%06llu.txt", g_dir.c_str(), static_cast<unsigned long long>(frame));
    FILE* f = std::fopen(path, "w");
    if (!f) return;
    std::fprintf(f, "frame %llu cmds %zu draws %zu uploads %zu copyTex %zu copyDisp %zu passes %zu\n", static_cast<unsigned long long>(frame), b.cmds.size(),
                 b.draws.size(), b.uploads.size(), b.copyTex.size(), b.copyDisp.size(), b.passes.size());
    for (const TexUpload& u : b.uploads)
        std::fprintf(f, "U tex=%s %ux%u fmt=%u authored=%u gen=%d levels=%zu\n", Name(Lookup(g_tex, u.texId), nb), u.width, u.height, u.format, u.authored, u.genMips ? 1 : 0, u.levels.size());
    uint32_t di = 0;
    for (const Cmd& c : b.cmds) {
        if (c.type == CMD_DRAW) {
            const DrawRec& d = b.draws[c.arg];
            const VtxLayout L = MakeLayout(d.layoutMask);
            uint64_t vh = 0;
            const uint8_t* vp = b.vtx.data() + (b.vtxChunks.empty() ? 0 : b.vtxChunks[d.vchunk]);
            const uint16_t* ip = b.idx.data();
            bool ok = true;
            if (d.vbuf && !StaticBufferData(d.vbuf, vp, ip)) ok = false;
            if (ok) {
                const uint8_t* v0 = vp + d.vtxOffset + static_cast<size_t>(d.vtxFirst) * L.stride;
                vh = XXH3_64bits(v0, static_cast<size_t>(d.vtxCount) * L.stride);
                std::vector<uint16_t> rel(d.idxCount);
                for (uint32_t i = 0; i < d.idxCount; ++i) rel[i] = static_cast<uint16_t>(ip[d.idxOffset + i] - d.vtxFirst);
                vh = XXH3_64bits_withSeed(rel.data(), rel.size() * 2, vh);
            }
            std::fprintf(f, "D %u prim=%u vs=%s fs=%s layout=%x n=%u idx=%u geo=%s tm=%02x", di++, d.prim, Name(Lookup(g_vs, d.vsId), nb), Name(Lookup(g_fs, d.fsId), nb2),
                         d.layoutMask, d.vtxCount, d.idxCount, ok ? Name(vh, nb) : "missing", d.texMask);
            for (int t = 0; t < 8; ++t)
                if (d.texMask & (1u << t))
                {
                    std::fprintf(f, " t%d=%s/w%u%u/f%u%u", t, Name(Lookup(g_tex, d.tex[t].texId), nb), d.tex[t].wrapS, d.tex[t].wrapT, d.tex[t].minFilter, d.tex[t].magFilter);
                    auto lv = g_texLevels.find(d.tex[t].texId);
                    std::fprintf(f, " tl%d=%u", t, lv == g_texLevels.end() ? 0u : lv->second);
                }
            const RenderState& r = d.rs;
            std::fprintf(f, " rs=bl%u/%u/%u/%u,dt%u/%u/%u,cu%u,cw%u%u,da%u,lw%u,sc%d:%d:%d:%d,vp%g:%g:%g:%g,z%g:%g", r.blend, r.blendSrc, r.blendDst, r.blendEq, r.depthTest, r.depthFunc,
                         r.depthWrite, r.cull, r.colorWrite, r.alphaWrite, r.dstAlphaPass, r.lineWidth6, r.scX, r.scY, r.scW, r.scH, r.vpX, r.vpY, r.vpW, r.vpH, r.zNear, r.zFar);
            for (int k = 0; k < UB_COUNT; ++k) {
                if (d.ublock[k] == kNoBlock) continue;
                const size_t n = std::min<size_t>(kUbSize[k], b.uni.size() - d.ublock[k]);
                std::fprintf(f, " ub%d=%s", k, Name(XXH3_64bits(b.uni.data() + d.ublock[k], n * 4), nb));
            }
            std::fprintf(f, "\n");
            if (g_full) {
                for (int k = 0; k < UB_COUNT; ++k) {
                    if (d.ublock[k] == kNoBlock) continue;
                    const size_t n = std::min<size_t>(kUbSize[k], b.uni.size() - d.ublock[k]);
                    std::fprintf(f, "  ub%d:", k);
                    for (size_t i = 0; i < n; ++i) std::fprintf(f, " %.7g", b.uni[d.ublock[k] + i]);
                    std::fprintf(f, "\n");
                }
                if (ok) {
                    const uint8_t* v0 = vp + d.vtxOffset + static_cast<size_t>(d.vtxFirst) * L.stride;
                    for (uint32_t v = 0; v < d.vtxCount && v < (g_fullAll ? 4096u : 4u); ++v) {
                        std::fprintf(f, "  v%u:", v);
                        const uint8_t* q = v0 + static_cast<size_t>(v) * L.stride;
                        for (uint32_t o = 0; o + 4 <= L.stride; o += 4) {
                            float fl; std::memcpy(&fl, q + o, 4);
                            uint32_t u; std::memcpy(&u, q + o, 4);
                            if (std::isfinite(fl) && std::fabs(fl) < 1e9f && std::fabs(fl) > 1e-9f) std::fprintf(f, " %.7g", fl);
                            else std::fprintf(f, " %08x", u);
                        }
                        std::fprintf(f, "\n");
                    }
                }
            }
        } else if (c.type == CMD_COPY_TEX) {
            const CopyTexCmd& t = b.copyTex[c.arg];
            std::fprintf(f, "CT src=%d,%d,%d,%d dst=%ux%u depth=%d skip=%d pass=%d fill=%d clr=%d/%d%d%d/%g,%g,%g,%g/%g mat=%016llx\n", t.src.x, t.src.y, t.src.w, t.src.h, t.dstW, t.dstH, t.depth, t.skip,
                         t.fromPass, t.fill, t.clear.enable, t.clear.color, t.clear.alpha, t.clear.depth, t.clear.rgba[0], t.clear.rgba[1], t.clear.rgba[2], t.clear.rgba[3], t.clear.z, static_cast<unsigned long long>(XXH3_64bits_withSeed(t.add, sizeof t.add, XXH3_64bits(t.mat, sizeof t.mat))));
        } else if (c.type == CMD_COPY_DISP) {
            const CopyDispCmd& t = b.copyDisp[c.arg];
            std::fprintf(f, "CD src=%d,%d,%d,%d clr=%d/%d%d%d/%g,%g,%g,%g/%g\n", t.src.x, t.src.y, t.src.w, t.src.h, t.clear.enable, t.clear.color, t.clear.alpha, t.clear.depth, t.clear.rgba[0],
                         t.clear.rgba[1], t.clear.rgba[2], t.clear.rgba[3], t.clear.z);
        }
    }
    std::fclose(f);
}
} // namespace drawlog

namespace {
ClearSpec MakeClear(bool enable) {
    ClearSpec c;
    c.enable = enable;
    if (!enable) return c;
    const uint32_t cm0 = g.bp[0x41];
    c.color = bits(cm0, 3, 1) != 0;
    c.alpha = bits(cm0, 4, 1) != 0;
    c.depth = bits(g.bp[0x40], 4, 1) != 0;
    c.rgba[0] = static_cast<float>(bits(g.bp[0x4F], 0, 8)) / 255.0f;
    c.rgba[3] = static_cast<float>(bits(g.bp[0x4F], 8, 8)) / 255.0f;
    c.rgba[1] = static_cast<float>(bits(g.bp[0x50], 8, 8)) / 255.0f;
    c.rgba[2] = static_cast<float>(bits(g.bp[0x50], 0, 8)) / 255.0f;
    if (!EfbHasAlpha()) { c.rgba[3] = 1.0f; c.alpha = true; }
    c.z = static_cast<float>(g.bp[0x51] & 0xFFFFFF) / 16777215.0f;
    return c;
}
} // namespace

void QueueCopyTex(const void* dest, bool clear) {
    CopyTexCmd c;
    c.src = g.texCopySrc;
    c.dstW = g.texCopyDstW ? g.texCopyDstW : static_cast<uint16_t>(g.texCopySrc.w);
    c.dstH = g.texCopyDstH ? g.texCopyDstH : static_cast<uint16_t>(g.texCopySrc.h);
    const uint32_t fmt = g.texCopyFmt;
    stats::CopyFormat(fmt, false);
    const bool hasAlpha = EfbHasAlpha();
    // Luma weights of the hardware's RGB->Y conversion used by the intensity formats.
    const float yr = 0.257f, yg = 0.504f, yb = 0.098f, yo = 16.0f / 255.0f;
    auto row = [&](int r, float a, float b, float cc, float d) { c.mat[r][0] = a; c.mat[r][1] = b; c.mat[r][2] = cc; c.mat[r][3] = d; };
    auto ident = [&] { row(0, 1, 0, 0, 0); row(1, 0, 1, 0, 0); row(2, 0, 0, 1, 0); row(3, 0, 0, 0, 1); };
    switch (fmt) {
    case 0x00: case 0x01: // I4, I8: intensity in all four channels
        for (int r = 0; r < 4; ++r) { row(r, yr, yg, yb, 0); c.add[r] = yo; }
        break;
    case 0x02: case 0x03: // IA4, IA8
        for (int r = 0; r < 3; ++r) { row(r, yr, yg, yb, 0); c.add[r] = yo; }
        row(3, 0, 0, 0, 1);
        break;
    case 0x04: // RGB565
        ident(); row(3, 0, 0, 0, 0); c.add[3] = 1.0f;
        break;
    case 0x20: case 0x28: for (int r = 0; r < 4; ++r) row(r, 1, 0, 0, 0); break; // R4, R8
    case 0x22: case 0x23: for (int r = 0; r < 3; ++r) row(r, 1, 0, 0, 0); row(3, 0, 0, 0, 1); break; // RA4, RA8
    case 0x27: for (int r = 0; r < 4; ++r) row(r, 0, 0, 0, 1); break; // A8
    case 0x29: for (int r = 0; r < 4; ++r) row(r, 0, 1, 0, 0); break; // G8
    case 0x2A: for (int r = 0; r < 4; ++r) row(r, 0, 0, 1, 0); break; // B8
    case 0x2B: for (int r = 0; r < 3; ++r) row(r, 0, 1, 0, 0); row(3, 1, 0, 0, 0); break; // RG8 (as IA8: I=G, A=R)
    case 0x2C: for (int r = 0; r < 3; ++r) row(r, 0, 0, 1, 0); row(3, 0, 1, 0, 0); break; // GB8
    case 0x11: case 0x13: case 0x16: case 0x30: case 0x39: case 0x3A: case 0x3C:
        c.depth = true;
        ident();
        stats::Note("depth-buffer EFB copy (Z texture format) requested");
        break;
    default: // RGBA8, RGB5A3 and anything unknown
        ident();
        break;
    }
    if (!hasAlpha && !c.depth) {
        // No alpha channel in the EFB: reads return 1.
        for (int r = 0; r < 4; ++r) { c.add[r] += c.mat[r][3]; c.mat[r][3] = 0; }
    }
    c.texId = AcquireCopyTexture(dest, c.dstW, c.dstH, fmt);
    ++stats::Cur().copies;
    stats::Cur().copyPixels += static_cast<uint32_t>(c.dstW) * c.dstH;
    c.clear = MakeClear(clear);
    if (TraceFrame())
        std::fprintf(stderr, "[trace] COPYTEX dest=%p id=%u src=(%d,%d,%d,%d) dst=%ux%u fmt=0x%02X half=%d clear=%d pe=0x%X\n", dest, c.texId, c.src.x, c.src.y,
                     c.src.w, c.src.h, c.dstW, c.dstH, fmt, g.texCopyHalf, clear, g.bp[0x43]);
    g_buf.copyTex.push_back(c);
    g_buf.cmds.push_back(Cmd{CMD_COPY_TEX, static_cast<uint32_t>(g_buf.copyTex.size() - 1)});
    g_lastDraw = -1;
    g.texDirty = true; // a bound texture may now resolve to this copy
    g.texSlotDirty = 0xFF;
    // No flush: copies are ordinary commands in the per-frame buffer.
}

void QueueCopyDisp(bool clear) {
    CopyDispCmd c;
    c.src = g.dispCopySrc;
    c.clear = MakeClear(clear);
    stats::CopyFormat(0, true);
    if (TraceFrame())
        std::fprintf(stderr, "[trace] COPYDISP src=(%d,%d,%d,%d) clear=%d color=(%.2f,%.2f,%.2f,%.2f) z=%.3f masks c%d a%d z%d\n", c.src.x, c.src.y, c.src.w,
                     c.src.h, clear, c.clear.rgba[0], c.clear.rgba[1], c.clear.rgba[2], c.clear.rgba[3], c.clear.z, c.clear.color, c.clear.alpha, c.clear.depth);
    g_buf.copyDisp.push_back(c);
    g_buf.cmds.push_back(Cmd{CMD_COPY_DISP, static_cast<uint32_t>(g_buf.copyDisp.size() - 1)});
    g_lastDraw = -1;
}

// The game copies the EFB to a texture with "clear" as its way of clearing the
// screen, several times in a row to the same scratch texture; a copy whose
// texture is copied to again (or destroyed) later in the same frame with no
// draw sampling it in between has no effect beyond its clear. On a tile-based
// GPU every copy is a render-target round trip, so those are marked to be
// skipped. (A copy that is still unused at the end of the frame is kept: the
// next frame may sample it.) MSC_VGX_DEADCOPY=0 turns this off.
static void MarkDeadCopies() {
    static const bool on = EnvInt("MSC_VGX_DEADCOPY", 1) != 0;
    if (!on || g_buf.copyTex.size() < 2) return;
    struct Live { uint32_t texId, copyIndex; };
    Live live[64];
    int n = 0;
    // (how many live entries have each low id byte: every texture of every draw is asked
    // about, and almost none is in the list)
    uint8_t inClass[256] = {};
    auto find = [&](uint32_t id) {
        if (!inClass[id & 255u]) return -1;
        for (int i = 0; i < n; ++i) if (live[i].texId == id) return i;
        return -1;
    };
    auto drop = [&](int i) { --inClass[live[i].texId & 255u]; live[i] = live[--n]; };
    for (const Cmd& c : g_buf.cmds) {
        if (c.type == CMD_DRAW) {
            if (!n) continue;
            const DrawRec& d = g_buf.draws[c.arg];
            for (int m = 0; m < 8; ++m)
                if (d.texMask & (1u << m)) {
                    const int i = find(d.tex[m].texId);
                    if (i >= 0) drop(i);
                }
        } else if (c.type == CMD_COPY_TEX) {
            const uint32_t id = g_buf.copyTex[c.arg].texId;
            const int i = find(id);
            if (i >= 0) {
                g_buf.copyTex[live[i].copyIndex].skip = true;
                ++stats::Cur().deadCopies;
                live[i].copyIndex = c.arg;
            } else if (n < 64) {
                live[n++] = Live{id, c.arg};
                ++inClass[id & 255u];
            }
        } else if (c.type == CMD_TEX_FREE) {
            const int i = find(c.arg);
            if (i >= 0) {
                g_buf.copyTex[live[i].copyIndex].skip = true;
                ++stats::Cur().deadCopies;
                drop(i);
            }
        }
    }
}

// ------------------------------------------------------------ pass isolation
// The game renders many small pictures into a corner of the EFB and copies
// them to textures before it draws the scene (16 crowd impostors and 4 to 8
// shadow maps per match frame, the menus' blur chain), and it clears the
// screen by copying it somewhere "with clear". Executed literally, each copy
// makes a tile-based GPU store the whole EFB, run a copy pass and reload the
// EFB: 40 to 85 times a frame. Most of these copies do not depend on the EFB
// being one shared surface at all, and this pass over the finished frame
// finds the ones that provably do not:
//
//  - isolated pass: every draw since the previous copy has its scissor inside
//    the copy's source rectangle R; the planes (colour, alpha, depth) of R
//    that those draws or the copy read held one known constant value when the
//    draws began (they were last touched by clears with the same values); and
//    what the draws leave in R is never seen: the copy clears every plane
//    they write, or the next thing that touches R is the clear of a dead copy.
//    -> CMD_PASS_BEGIN before the draws, CopyTexCmd::fromPass.
//  - fill: the planes of R that the copy reads are known to hold a constant
//    (nothing was drawn there since they were cleared).
//    -> CopyTexCmd::fill, no source needed at all.
//
// Everything else is left as it was (efbReads in the statistics).
// What the EFB holds is tracked across frames as a short history of
// rectangles: clears with their values, and "drawn to" (unknown contents).
// The history describes the EFB as the back end keeps it, i.e. without the
// isolated passes' drawing in it.
//   MSC_VGX_PASSISO=0   leave frames as submitted (the default is on)
namespace {
// The EFB's three planes are followed separately: the menus' and cutscenes'
// blur chain works in the alpha plane alone, on top of the finished colour
// picture (alpha-only draws, alpha-only copies, alpha-only clears).
enum : uint8_t { CH_C = 1, CH_A = 2, CH_Z = 4, CH_ALL = 7 };
struct EfbEvent {
    EfbRect r;
    bool clear; // else: drawn to, contents unknown
    float v[3]; // colour: rgb; alpha, depth: v[0]
};
std::vector<EfbEvent> g_efbHist[3]; // colour, alpha, depth

// Copy textures that hold one constant colour because the last copy into them
// was a fill: the game "copies" the cleared screen corner into the same
// scratch textures every frame (up to 60 a frame), and a fill that would write
// what the texture already holds is not executed at all.
struct ConstTex {
    float rgba[4]; // as written (after the copy's conversion)
    uint16_t w, h;
};
std::unordered_map<uint32_t, ConstTex> g_constTex;

inline bool Inside(const EfbRect& a, const EfbRect& b) { // a inside b
    return a.x >= b.x && a.y >= b.y && a.x + a.w <= b.x + b.w && a.y + a.h <= b.y + b.h;
}
inline bool Overlap(const EfbRect& a, const EfbRect& b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h && a.w > 0 && a.h > 0 && b.w > 0 && b.h > 0;
}
inline EfbRect Union(const EfbRect& a, const EfbRect& b) {
    const int x0 = std::min<int>(a.x, b.x), y0 = std::min<int>(a.y, b.y), x1 = std::max<int>(a.x + a.w, b.x + b.w), y1 = std::max<int>(a.y + a.h, b.y + b.h);
    return EfbRect{static_cast<int16_t>(x0), static_cast<int16_t>(y0), static_cast<int16_t>(x1 - x0), static_cast<int16_t>(y1 - y0)};
}
inline uint8_t ClearMask(const ClearSpec& c) {
    return c.enable ? static_cast<uint8_t>((c.color ? CH_C : 0) | (c.alpha ? CH_A : 0) | (c.depth ? CH_Z : 0)) : 0;
}
// Planes a draw changes, and planes whose previous contents its result depends on.
inline uint8_t DrawWrites(const RenderState& rs) {
    return static_cast<uint8_t>((rs.colorWrite ? CH_C : 0) | ((rs.alphaWrite || rs.dstAlphaPass) ? CH_A : 0) | ((rs.depthTest && rs.depthWrite) ? CH_Z : 0));
}
inline uint8_t DrawReads(const RenderState& rs) {
    uint8_t m = 0;
    if (rs.depthTest && rs.depthFunc != CMP_ALWAYS) m |= CH_Z;
    if (rs.blend) m |= static_cast<uint8_t>(CH_A | (rs.colorWrite ? CH_C : 0)); // (destination alpha can be a factor of the colour blend)
    return m;
}
// Planes of the EFB a colour copy's result depends on.
inline uint8_t CopyReads(const CopyTexCmd& t) {
    uint8_t m = 0;
    for (int r = 0; r < 4; ++r) {
        if (t.mat[r][0] != 0 || t.mat[r][1] != 0 || t.mat[r][2] != 0) m |= CH_C;
        if (t.mat[r][3] != 0) m |= CH_A;
    }
    return m;
}

void HistPush(int plane, const EfbEvent& e) {
    if (e.r.w <= 0 || e.r.h <= 0) return;
    std::vector<EfbEvent>& h = g_efbHist[plane];
    // An older event inside the new one can never be the latest word on any pixel again.
    size_t n = 0;
    for (size_t i = 0; i < h.size(); ++i)
        if (!Inside(h[i].r, e.r)) h[n++] = h[i];
    h.resize(n);
    if (h.size() >= 48) {
        // Too fragmented to follow: everything touched so far is unknown.
        EfbEvent all = h[0];
        for (const EfbEvent& o : h) all.r = Union(all.r, o.r);
        all.clear = false;
        h.assign(1, all);
    }
    h.push_back(e);
}
void HistDrawn(uint8_t planes, const EfbRect& r) {
    for (int p = 0; p < 3; ++p) {
        if (!(planes & (1u << p))) continue;
        const std::vector<EfbEvent>& h = g_efbHist[p];
        if (!h.empty() && !h.back().clear && Inside(r, h.back().r)) continue;
        HistPush(p, EfbEvent{r, false, {}});
    }
}
void HistCleared(const EfbRect& r, const ClearSpec& c) {
    if (!c.enable) return;
    if (c.color) HistPush(0, EfbEvent{r, true, {c.rgba[0], c.rgba[1], c.rgba[2]}});
    if (c.alpha) HistPush(1, EfbEvent{r, true, {c.rgba[3], 0, 0}});
    if (c.depth) HistPush(2, EfbEvent{r, true, {c.z, 0, 0}});
}
// Does every pixel of r hold one known cleared value in each of `planes`?
// v: rgba, z (planes not asked for get 0 / z 1).
bool HistKnown(uint8_t planes, const EfbRect& r, float v[5]) {
    v[0] = v[1] = v[2] = v[3] = 0.0f;
    v[4] = 1.0f;
    for (int p = 0; p < 3; ++p) {
        if (!(planes & (1u << p))) continue;
        const std::vector<EfbEvent>& h = g_efbHist[p];
        bool have = false, covered = false;
        float val[3] = {};
        for (size_t i = h.size(); i-- > 0 && !covered;) {
            const EfbEvent& e = h[i];
            if (!Overlap(e.r, r)) continue;
            if (!e.clear) return false;
            if (have && std::memcmp(val, e.v, sizeof val) != 0) return false;
            std::memcpy(val, e.v, sizeof val);
            have = true;
            covered = Inside(r, e.r);
        }
        if (!covered) return false;
        if (p == 0) std::memcpy(v, val, sizeof val);
        else v[p == 1 ? 3 : 4] = val[0];
    }
    return true;
}

// Would this clear change nothing: does every plane it clears already hold
// exactly its value over the whole rectangle? (Most of the game's clears are
// like that once the offscreen passes no longer draw into the EFB, and each
// one that is executed is a scene of its own on the EFB's render target.)
//   MSC_VGX_NOOPCLEAR=0   execute them all the same
bool ClearIsNoOp(const EfbRect& r, const ClearSpec& c) {
    static const bool on = EnvInt("MSC_VGX_NOOPCLEAR", 1) != 0;
    if (!on || !c.enable) return false;
    const uint8_t m = ClearMask(c);
    float v[5];
    if (!HistKnown(m, r, v)) return false;
    if ((m & CH_C) && (v[0] != c.rgba[0] || v[1] != c.rgba[1] || v[2] != c.rgba[2])) return false;
    if ((m & CH_A) && v[3] != c.rgba[3]) return false;
    if ((m & CH_Z) && v[4] != c.z) return false;
    return true;
}

// After a pass's copy at command `from` that does not clear: is what the pass
// would have left in `r` of the EFB (in the planes `written`) seen by anything?
bool LeftoversUnseen(size_t from, const EfbRect& r, uint8_t written) {
    for (size_t j = from + 1; j < g_buf.cmds.size(); ++j) {
        const Cmd& c = g_buf.cmds[j];
        if (c.type == CMD_DRAW) {
            const RenderState& rs = g_buf.draws[c.arg].rs;
            if (Overlap(EfbRect{rs.scX, rs.scY, rs.scW, rs.scH}, r)) return false;
        } else if (c.type == CMD_COPY_TEX) {
            const CopyTexCmd& t = g_buf.copyTex[c.arg];
            if (!Overlap(t.src, r)) continue;
            if (!t.skip) return false;
            if ((written & ~ClearMask(t.clear)) == 0 && Inside(r, t.src)) return true;
        } else if (c.type == CMD_COPY_DISP) {
            if (Overlap(g_buf.copyDisp[c.arg].src, r)) return false;
        } else if (c.type == CMD_SET_TARGET) {
            return false;
        }
    }
    return false;
}

void IsolatePasses() {
    static const bool on = EnvInt("MSC_VGX_PASSISO", 1) != 0;
    if (!on) return;
    stats::Frame& fr = stats::Cur();
    std::vector<Cmd>& cmds = g_buf.cmds;
    size_t runFirst = 0; // index in cmds of the first draw of the current run
    uint32_t runDraws = 0;
    uint8_t runWrites = 0, runReads = 0;
    EfbRect runBox{};
    std::vector<std::pair<size_t, uint32_t>> begins; // (index of the run's first draw, pass index)
    auto runToEfb = [&] {
        if (runDraws) HistDrawn(runWrites, runBox);
        runDraws = 0;
        runWrites = runReads = 0;
    };
    for (size_t i = 0; i < cmds.size(); ++i) {
        const Cmd c = cmds[i];
        switch (c.type) {
        case CMD_DRAW: {
            const RenderState& rs = g_buf.draws[c.arg].rs;
            const EfbRect sc{rs.scX, rs.scY, rs.scW, rs.scH};
            if (sc.w <= 0 || sc.h <= 0) break; // draws nothing
            if (!runDraws) { runFirst = i; runBox = sc; }
            else runBox = Union(runBox, sc);
            runWrites |= DrawWrites(rs);
            runReads |= DrawReads(rs);
            ++runDraws;
            break;
        }
        case CMD_COPY_TEX: {
            CopyTexCmd& t = g_buf.copyTex[c.arg];
            const uint8_t copyReads = t.depth ? CH_Z : CopyReads(t);
            float v[5];
            // The pass target has to reproduce every plane the draws or the copy read; planes the
            // draws only write and nobody reads do not matter. What the draws would have left in
            // the EFB must be wiped by this copy's clear, or (no clear) never be looked at.
            if (runDraws && !t.skip && !t.depth && t.src.w > 0 && t.src.h > 0 && Inside(runBox, t.src) &&
                HistKnown(static_cast<uint8_t>(runReads | copyReads), t.src, v) &&
                (t.clear.enable ? (runWrites & ~ClearMask(t.clear)) == 0 : LeftoversUnseen(i, t.src, runWrites))) {
                PassCmd ps;
                ps.rect = t.src;
                std::memcpy(ps.rgba, v, sizeof ps.rgba);
                ps.z = v[4];
                ps.planes = static_cast<uint8_t>(runReads | copyReads | runWrites);
                ps.known = static_cast<uint8_t>(runReads | copyReads);
                // Same size, no conversion, and the pass does not sample its own destination:
                // it can be drawn straight into the destination texture.
                bool direct = t.dstW == t.src.w && t.dstH == t.src.h && t.add[0] == 0 && t.add[1] == 0 && t.add[2] == 0 && t.add[3] == 0;
                for (int r = 0; r < 4 && direct; ++r)
                    for (int col = 0; col < 4; ++col)
                        if (t.mat[r][col] != (r == col ? 1.0f : 0.0f)) direct = false;
                for (size_t j = runFirst; j < i && direct; ++j) {
                    if (cmds[j].type != CMD_DRAW) continue;
                    const DrawRec& d = g_buf.draws[cmds[j].arg];
                    for (int m = 0; m < 8; ++m)
                        if ((d.texMask & (1u << m)) && d.tex[m].texId == t.texId) direct = false;
                }
                if (direct) { ps.directTex = t.texId; ++fr.passesDirect; }
                g_constTex.erase(t.texId);
                g_buf.passes.push_back(ps);
                begins.emplace_back(runFirst, static_cast<uint32_t>(g_buf.passes.size() - 1));
                t.fromPass = true;
                ++fr.passes;
                fr.passDraws += runDraws;
                runDraws = 0;
                runWrites = runReads = 0;
            } else {
                runToEfb();
                if (!t.skip && !t.depth && HistKnown(copyReads, t.src, v)) {
                    t.fill = true;
                    std::memcpy(t.fillRgba, v, sizeof t.fillRgba);
                    ++fr.fills;
                    // The colour the back end will write (same arithmetic as its fill).
                    ConstTex ct;
                    ct.w = t.dstW;
                    ct.h = t.dstH;
                    float q[4];
                    for (int k = 0; k < 4; ++k) {
                        const float cv = v[k] < 0 ? 0 : (v[k] > 1 ? 1 : v[k]);
                        q[k] = std::floor(cv * 255.0f + 0.5f) / 255.0f;
                    }
                    for (int r = 0; r < 4; ++r) ct.rgba[r] = t.mat[r][0] * q[0] + t.mat[r][1] * q[1] + t.mat[r][2] * q[2] + t.mat[r][3] * q[3] + t.add[r];
                    auto it = g_constTex.find(t.texId);
                    if (it != g_constTex.end() && it->second.w == ct.w && it->second.h == ct.h && std::memcmp(it->second.rgba, ct.rgba, sizeof ct.rgba) == 0) {
                        t.skip = true; // the texture already holds exactly this
                        ++fr.fillsRedundant;
                    } else {
                        g_constTex[t.texId] = ct;
                    }
                } else if (!t.skip) {
                    ++fr.efbReads;
                    g_constTex.erase(t.texId);
                }
            }
            if (ClearIsNoOp(t.src, t.clear)) {
                t.clear.enable = false;
                t.clearElided = true;
                ++fr.clearsRedundant;
            } else {
                HistCleared(t.src, t.clear);
            }
            break;
        }
        case CMD_COPY_DISP: {
            runToEfb();
            CopyDispCmd& d = g_buf.copyDisp[c.arg];
            if (ClearIsNoOp(d.src, d.clear)) {
                d.clear.enable = false;
                d.clearElided = true;
                ++fr.clearsRedundant;
            } else {
                HistCleared(d.src, d.clear);
            }
            break;
        }
        case CMD_SET_TARGET:
            runToEfb();
            for (auto& h : g_efbHist) h.clear();
            g_constTex.clear(); // (the back end's copy textures change size with the target)
            break;
        case CMD_TEX_FREE:
            g_constTex.erase(c.arg);
            break;
        case CMD_PRESENT:
            runToEfb();
            break;
        default:
            break;
        }
    }
    if (begins.empty()) return;
    // Insert the CMD_PASS_BEGIN commands (back to front, so the recorded indices stay valid).
    for (size_t k = begins.size(); k-- > 0;) cmds.insert(cmds.begin() + static_cast<std::ptrdiff_t>(begins[k].first), Cmd{CMD_PASS_BEGIN, begins[k].second});
}
} // namespace

// ------------------------------------------------------------- copy atlas
// After pass isolation an open-play frame still has 16 offscreen passes that
// each draw one crowd figure into a 64x64 texture of its own, and 4 to 8 that
// draw a player's shadow at 144x144 and copy it into a 72x72 texture (and more
// of both kinds in cutscenes): 24 to 32 render-target switches, each a scene
// of its own for the console's GPU. The pictures are then sampled by plain
// quads. This step gives such pictures cells of shared atlas textures:
//  - a direct pass (PassCmd::directTex) of at most kAtlasMaxCell pixels a side
//    is drawn into a cell of the atlas for its size (PassCmd::atlasTex ...);
//  - a pass whose copy converts or scales (the shadows) is drawn into a cell
//    of a scratch atlas, and its copy becomes a copy from that cell into a
//    cell of the picture atlas for the copy's size. The copies of passes that
//    follow each other are held back until the last of them has been drawn,
//    so that the passes are one scene and the copies another;
//  - both only when the frame has at least two such passes of one size, or
//    the picture lives in an atlas already;
//  - every later draw that samples the picture is redirected: its texture
//    binding becomes the atlas and its texture coordinates are rewritten in
//    the vertex arena to address the cell. That is only possible for draws
//    whose coordinates for that texture are known here: vertices in the
//    frame's arena, one plain texture-coordinate attribute through an identity
//    texture matrix, every coordinate inside 0..1 (so that clamping never
//    comes into play). Draws that only differed in which picture they sampled
//    become mergeable and are merged;
//  - a draw that samples the picture in any other way takes it back out of
//    the atlas first (a copy from the cell into the picture's own texture),
//    and that picture stays out for a while (kAtlasBackOff frames, doubling each time).
// Where a picture lives is remembered across frames (a picture that is not
// drawn again in a frame keeps its cell and its contents).
// Not identical to separate textures in one respect: at the very edge of a
// picture, linear filtering blends with the cleared gutter instead of
// repeating the edge texel. For pictures whose border is the clear colour
// (figures on a transparent ground) that is the same thing.
//   MSC_VGX_ATLAS=0   off;  =1  direct passes only;  2 (default)  converted copies too
//   (needs absolute indices, MSC_VGX_ABSIDX, for the merging)
namespace {
constexpr int kAtlasGutter = 1;
constexpr int kAtlasMaxCell = 128;   // largest picture
constexpr int kAtlasMaxSource = 288; // largest pass of a converted copy
constexpr int kAtlasMaxRatio = 4;    // pass size : picture size of a converted copy
struct AtlasPage {
    uint32_t tex = 0;              // back-end texture id
    uint16_t cellW = 0, cellH = 0; // size of a picture
    uint16_t gutter = kAtlasGutter;
    uint16_t cols = 0, rows = 0;
    std::vector<uint32_t> owner;   // texture id of the picture in each cell, 0 = free
    std::vector<uint64_t> lastUse; // frame in which the cell's picture was last drawn or sampled
    struct Gut { float rgba[4]; bool valid; };
    std::vector<Gut> gut;          // what the cell's gutter was last cleared to
    uint16_t PitchX() const { return static_cast<uint16_t>(cellW + 2 * gutter); }
    uint16_t PitchY() const { return static_cast<uint16_t>(cellH + 2 * gutter); }
    uint16_t W() const { return static_cast<uint16_t>(cols * PitchX()); }
    uint16_t H() const { return static_cast<uint16_t>(rows * PitchY()); }
    uint16_t CellX(uint32_t c) const { return static_cast<uint16_t>((c % cols) * PitchX() + gutter); }
    uint16_t CellY(uint32_t c) const { return static_cast<uint16_t>((c / cols) * PitchY() + gutter); }
    void Init(uint16_t w, uint16_t h, uint16_t g, int side) {
        tex = AllocTextureId();
        cellW = w;
        cellH = h;
        gutter = g;
        cols = static_cast<uint16_t>(std::max(1, side / (w + 2 * g)));
        rows = static_cast<uint16_t>(std::max(1, side / (h + 2 * g)));
        owner.assign(static_cast<size_t>(cols) * rows, 0);
        lastUse.assign(owner.size(), 0);
        gut.assign(owner.size(), Gut{{0, 0, 0, 0}, false});
    }
    // Fills in the atlas part of a pass that draws into cell c. The gutter is only cleared
    // when it does not hold the pass's clear colour already (one scissor rectangle per
    // cell instead of two for the back end).
    void Target(PassCmd& ps, uint32_t c) {
        ps.atlasTex = tex;
        ps.atlasW = W();
        ps.atlasH = H();
        ps.atlasX = CellX(c);
        ps.atlasY = CellY(c);
        Gut& g = gut[c];
        if (g.valid && std::memcmp(g.rgba, ps.rgba, sizeof g.rgba) == 0) {
            ps.atlasGutter = 0;
        } else {
            ps.atlasGutter = static_cast<uint8_t>(gutter);
            std::memcpy(g.rgba, ps.rgba, sizeof g.rgba);
            g.valid = true;
        }
    }
};
struct AtlasLoc { uint16_t page, cell; };
std::vector<AtlasPage> g_atlasPages;               // pictures
std::vector<AtlasPage> g_scratchPages;             // passes of converted copies (cells are free again once the copies are made)
// A map from texture id to V as an array (ids are small and dense): find() is looked up for
// every texture of every draw, and a hash table costs a division each time on the console's
// processor. Only what the code below uses; find() gives a pointer (null = end()) that stays
// valid until the next operator[].
template <typename V> struct IdMap {
    struct Slot { uint32_t first; V second; bool used; };
    std::vector<Slot> s;
    size_t n = 0;
    Slot* find(uint32_t id) { return id < s.size() && s[id].used ? &s[id] : nullptr; }
    Slot* end() const { return nullptr; }
    size_t count(uint32_t id) const { return id < s.size() && s[id].used ? 1 : 0; }
    V& operator[](uint32_t id) {
        if (id >= s.size()) s.resize(static_cast<size_t>(id) + 256, Slot{0, V{}, false});
        Slot& x = s[id];
        if (!x.used) { x.used = true; x.first = id; x.second = V{}; ++n; }
        return x.second;
    }
    void erase(Slot* it) { if (it && it->used) { it->used = false; --n; } }
    void erase(uint32_t id) { erase(find(id)); }
    bool empty() const { return n == 0; }
    void clear() { for (Slot& x : s) x.used = false; n = 0; }
};
IdMap<AtlasLoc> g_atlasOf;  // picture (texture id) -> where it lives
// Pictures that had to be taken out of an atlas stay out for a while, twice as long each time.
struct AtlasBar { uint64_t until; uint8_t level; };
IdMap<AtlasBar> g_atlasBar;
constexpr uint64_t kAtlasBackOff = 300; // frames, the first time
bool AtlasBarred(uint32_t texId) {
    if (g_atlasBar.empty()) return false;
    auto it = g_atlasBar.find(texId);
    return it != g_atlasBar.end() && g_frame < it->second.until;
}
void AtlasBarNow(uint32_t texId) {
    AtlasBar& b = g_atlasBar[texId];
    b.until = g_frame + (kAtlasBackOff << b.level);
    if (b.level < 12) ++b.level;
}
void AtlasRelease(uint32_t texId) {
    auto it = g_atlasOf.find(texId);
    if (it == g_atlasOf.end()) return;
    g_atlasPages[it->second.page].owner[it->second.cell] = 0;
    g_atlasOf.erase(it);
}
// Appends a copy of cell `loc`'s picture into its own texture `texId` to `out` (the picture
// leaves the atlas with its contents).
void AtlasExtract(std::vector<Cmd>& out, uint32_t texId, AtlasLoc loc) {
    const AtlasPage& pg = g_atlasPages[loc.page];
    CopyTexCmd x;
    x.texId = texId;
    x.srcTex = pg.tex;
    x.srcTexW = pg.W();
    x.srcTexH = pg.H();
    x.src = EfbRect{static_cast<int16_t>(pg.CellX(loc.cell)), static_cast<int16_t>(pg.CellY(loc.cell)), static_cast<int16_t>(pg.cellW),
                    static_cast<int16_t>(pg.cellH)};
    x.dstW = pg.cellW;
    x.dstH = pg.cellH;
    for (int r = 0; r < 4; ++r) x.mat[r][r] = 1.0f;
    g_buf.copyTex.push_back(x);
    out.push_back(Cmd{CMD_COPY_TEX, static_cast<uint32_t>(g_buf.copyTex.size() - 1)});
    AtlasRelease(texId);
    ++stats::Cur().atlasEvicted;
}
// A cell for a picture of w x h (its own if it has one). All pictures of one size share one
// atlas as long as it has room, so that their passes stay one scene and the draws that
// sample them can merge; when it is full, the picture that has gone unused the longest (at
// least kAtlasIdle frames) gets a texture of its own again (`out`: where that copy goes).
constexpr uint64_t kAtlasIdle = 120;
AtlasLoc AtlasCellFor(std::vector<Cmd>& out, uint32_t texId, uint16_t w, uint16_t h) {
    auto it = g_atlasOf.find(texId);
    if (it != g_atlasOf.end()) {
        const AtlasPage& pg = g_atlasPages[it->second.page];
        if (pg.cellW == w && pg.cellH == h) return it->second;
        AtlasRelease(texId);
    }
    for (int reclaim = 0; reclaim < 2; ++reclaim)
        for (size_t p = 0; p < g_atlasPages.size(); ++p) {
            AtlasPage& pg = g_atlasPages[p];
            if (pg.cellW != w || pg.cellH != h) continue;
            int best = -1;
            for (size_t c = 0; c < pg.owner.size(); ++c) {
                if (!reclaim) {
                    if (!pg.owner[c]) { best = static_cast<int>(c); break; }
                } else if (pg.owner[c] && pg.lastUse[c] + kAtlasIdle < g_frame && (best < 0 || pg.lastUse[c] < pg.lastUse[best])) {
                    best = static_cast<int>(c);
                }
            }
            if (best < 0) continue;
            const AtlasLoc loc{static_cast<uint16_t>(p), static_cast<uint16_t>(best)};
            if (pg.owner[best]) {
                // (only if the old owner really lives here: a pass that is still open has the cell but no entry yet)
                auto o = g_atlasOf.find(pg.owner[best]);
                if (o == g_atlasOf.end() || o->second.page != loc.page || o->second.cell != loc.cell) continue;
                AtlasExtract(out, pg.owner[best], loc);
            }
            pg.owner[best] = texId;
            pg.lastUse[best] = g_frame;
            return loc;
        }
    AtlasPage pg;
    // About 512 pixels a side (6 x 6 pictures of 72, 3 x 3 of 128), but room for 100 of the
    // smallest: the walk-out and the replays draw 64 crowd pictures of 64 x 64 per frame, and
    // pictures spread over two pages cost a scene per change of page and keep the draws that
    // sample them from merging.
    // MSC_VGX_ATLAS_SIDE=<pixels> sets the side for the smallest pictures (264: 16 of them,
    // 330: 25, 462: 49). On the console a scissor change costs a pass over the whole target
    // (vitaGL keeps the scissor as a mask bit per pixel), so smaller pages trade scenes
    // against that; nothing here can tell which is cheaper.
    static const int smallSide = static_cast<int>(EnvInt("MSC_VGX_ATLAS_SIDE", 672));
    pg.Init(w, h, kAtlasGutter, (w <= 64 && h <= 64) ? smallSide : 512);
    pg.owner[0] = texId;
    pg.lastUse[0] = g_frame;
    g_atlasPages.push_back(pg);
    return AtlasLoc{static_cast<uint16_t>(g_atlasPages.size() - 1), 0};
}

// Points draw `d`'s sampling of texmap `m` (a picture in cell `loc`) at the atlas by
// rewriting the draw's texture coordinates. False (nothing changed) when that cannot be done.
// commit = false: only says whether it can be done.
int g_atlasWhy = 0; // which test of AtlasRedirect failed last (diagnostics)
inline bool Why(int n) { g_atlasWhy = n; return false; }
bool AtlasRedirect(DrawRec& d, int m, AtlasLoc loc, bool commit) {
    if (d.vbuf) return Why(1); // static geometry: the coordinates are not ours to change
    const FsKey& fs = g_fsReg.keys[d.fsId];
    const VsKey& vs = g_vsReg.keys[d.vsId];
    if (fs.numIndStages) return Why(2);
    int coord = -1;
    for (int s = 0; s < fs.numStages; ++s) {
        const FsKey::Stage& st = fs.stage[s];
        if (!st.texEnable || st.texMap != m) continue;
        if (coord >= 0 && st.texCoord != coord) return Why(3);
        coord = st.texCoord;
    }
    if (coord < 0 || coord >= vs.numTexGens) return Why(4);
    for (int s = 0; s < fs.numStages; ++s) // the same coordinate must not address another texture as well
        if (fs.stage[s].texEnable && fs.stage[s].texMap != m && fs.stage[s].texCoord == coord) return Why(5);
    const VsKey::TexGen& tg = vs.tg[coord];
    if (tg.type != 0 || tg.src < 5 || tg.src > 12 || tg.post || tg.perVtxMtx) return Why(6);
    // (a projective coordinate is fine when its third row makes q = 1: the game draws the same quads both ways)
    const bool proj = tg.proj || (fs.texProjMask & (1u << coord));
    if (proj && !(tg.proj && (fs.texProjMask & (1u << coord)))) return Why(11);
    for (int c = 0; c < vs.numTexGens; ++c) // nor may the attribute feed another coordinate
        if (c != coord && vs.tg[c].type < 2 && vs.tg[c].src == tg.src) return Why(7);
    const int k = tg.src - 5;
    if (!(d.layoutMask & (VL_TEX0 << k)) || d.ublock[UB_TEXM] == kNoBlock) return Why(8);
    static const float ident[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    if (Gxd(GXD_WORDCMP) ? !SameWords(g_buf.uni.data() + d.ublock[UB_TEXM] + coord * 12, ident, proj ? 12 : 8)
                         : std::memcmp(g_buf.uni.data() + d.ublock[UB_TEXM] + coord * 12, ident, proj ? 48 : 32) != 0)
        return Why(9);
    const VtxLayout L = MakeLayout(d.layoutMask);
    uint8_t* const v0 = g_buf.vtx.data() + (g_buf.vtxChunks.empty() ? 0 : g_buf.vtxChunks[d.vchunk]) + d.vtxOffset +
                        static_cast<size_t>(d.vtxFirst) * L.stride + L.offTex[k];
    // Every coordinate inside 0..1. On the bit patterns: a float is in [+0, 1] exactly when
    // its bits, read as an unsigned number, are at most those of 1.0; -0 also counts; a NaN
    // or anything negative does not. (Four floating-point comparisons per vertex cost four
    // transfers of the status word, some 9 000 vertices a frame.)
    {
        const uint8_t* q = v0;
        const size_t st = L.stride;
        uint32_t bad = 0;
        uint32_t i = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
        if (Gxd(GXD_LOOPS_NEON)) { // (GXD_LOOPS_NEON: two vertices per iteration, the same test on the bit patterns)
            const uint32x4_t one = vdupq_n_u32(0x3F800000u), negZero = vdupq_n_u32(0x80000000u);
            uint32x4_t badv = vdupq_n_u32(0);
            for (; i + 2 <= d.vtxCount; i += 2, q += 2 * st) {
                const uint32x4_t b = vcombine_u32(vreinterpret_u32_u8(vld1_u8(q)), vreinterpret_u32_u8(vld1_u8(q + st)));
                badv = vorrq_u32(badv, vbicq_u32(vcgtq_u32(b, one), vceqq_u32(b, negZero)));
            }
            const uint32x2_t h = vorr_u32(vget_low_u32(badv), vget_high_u32(badv));
            bad = vget_lane_u32(h, 0) | vget_lane_u32(h, 1);
        }
#endif
        for (; i < d.vtxCount; ++i, q += st) {
            uint32_t b[2];
            std::memcpy(b, q, 8);
            bad |= static_cast<uint32_t>(b[0] > 0x3F800000u && b[0] != 0x80000000u) | static_cast<uint32_t>(b[1] > 0x3F800000u && b[1] != 0x80000000u);
        }
        if (bad) return Why(10);
    }
    if (!commit) return true;
    const AtlasPage& pg = g_atlasPages[loc.page];
    const float sx = static_cast<float>(pg.cellW) / pg.W(), sy = static_cast<float>(pg.cellH) / pg.H();
    const float ox = static_cast<float>(pg.CellX(loc.cell)) / pg.W(), oy = static_cast<float>(pg.CellY(loc.cell)) / pg.H();
    {
        uint8_t* q = v0;
        const size_t st = L.stride;
        uint32_t i = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
        if (Gxd(GXD_LOOPS_NEON)) { // (one multiplication and one addition per lane, each rounded, as the scalar loop; the coordinates are in 0..1)
            const float s2[2] = {sx, sy}, o2[2] = {ox, oy};
            const float32x2_t vsc = vld1_f32(s2), vof = vld1_f32(o2);
            for (; i < d.vtxCount; ++i, q += st) {
                const float32x2_t uv = vreinterpret_f32_u8(vld1_u8(q));
                vst1_u8(q, vreinterpret_u8_f32(vadd_f32(vof, vmul_f32(uv, vsc))));
            }
        }
#endif
        for (; i < d.vtxCount; ++i, q += st) {
            float uv[2];
            std::memcpy(uv, q, 8);
            uv[0] = ox + uv[0] * sx;
            uv[1] = oy + uv[1] * sy;
            std::memcpy(q, uv, 8);
        }
    }
    d.tex[m].texId = pg.tex;
    return true;
}

// MSC_VGX_PASSREPEAT=1 (diagnostic): how often an offscreen pass draws exactly what the pass
// into the same texture drew the frame before, or what another pass of the same frame draws
// (candidates for not being drawn at all). The hash covers the pass's draws with their
// state, uniform values and geometry; it does not know when a sampled texture's contents
// change under the same id, so the counts are an upper bound.
struct PassRepeat {
    std::unordered_map<uint32_t, uint64_t> last; // destination texture -> hash of its last pass
    uint64_t passes = 0, sameAsLastFrame = 0, sameAsOther = 0, frames = 0;
} g_passRepeat;
void MeasurePassRepeats() {
    static const bool on = EnvInt("MSC_VGX_PASSREPEAT", 0) != 0;
    if (!on || g_buf.passes.empty()) return;
    ++g_passRepeat.frames;
    std::vector<uint64_t> seen;
    uint64_t h = 0;
    bool open = false;
    for (const Cmd& c : g_buf.cmds) {
        if (c.type == CMD_PASS_BEGIN) {
            const PassCmd& ps = g_buf.passes[c.arg];
            h = XXH3_64bits(&ps.rect, sizeof ps.rect) ^ XXH3_64bits(ps.rgba, sizeof ps.rgba);
            open = true;
        } else if (c.type == CMD_DRAW && open) {
            const DrawRec& d = g_buf.draws[c.arg];
            const uint32_t v[8] = {d.vsId, d.fsId, d.prim, d.texMask, d.layoutMask, d.vtxCount, d.idxCount, d.vbuf};
            h = XXH3_64bits_withSeed(v, sizeof v, h);
            h = XXH3_64bits_withSeed(&d.rs, sizeof d.rs, h);
            h = XXH3_64bits_withSeed(d.tex, sizeof d.tex, h);
            for (int b = 0; b < UB_COUNT; ++b)
                if (d.ublock[b] != kNoBlock) // (the block may be shorter than kUbSize: an upper bound that stays inside the arena)
                    h = XXH3_64bits_withSeed(g_buf.uni.data() + d.ublock[b], std::min<size_t>(kUbSize[b], g_buf.uni.size() - d.ublock[b]) * 4, h);
            if (d.vbuf) {
                const uint32_t g2[3] = {d.vtxOffset, d.vtxFirst, d.idxOffset};
                h = XXH3_64bits_withSeed(g2, sizeof g2, h);
            } else {
                const size_t stride = MakeLayout(d.layoutMask).stride;
                const uint8_t* vp = g_buf.vtx.data() + (g_buf.vtxChunks.empty() ? 0 : g_buf.vtxChunks[d.vchunk]) + d.vtxOffset + d.vtxFirst * stride;
                h = XXH3_64bits_withSeed(vp, d.vtxCount * stride, h);
            }
        } else if (c.type == CMD_COPY_TEX && open) {
            open = false;
            const uint32_t t = g_buf.copyTex[c.arg].texId;
            ++g_passRepeat.passes;
            auto it = g_passRepeat.last.find(t);
            if (it != g_passRepeat.last.end() && it->second == h) ++g_passRepeat.sameAsLastFrame;
            else if (std::find(seen.begin(), seen.end(), h) != seen.end()) ++g_passRepeat.sameAsOther;
            seen.push_back(h);
            g_passRepeat.last[t] = h;
        }
    }
}

void AtlasPasses() {
    MeasurePassRepeats();
    static const long mode = AbsoluteIndices() ? EnvInt("MSC_VGX_ATLAS", 2) : 0;
    if (!mode || (g_buf.passes.empty() && g_atlasOf.empty())) return;
    stats::Frame& fr = stats::Cur();
    std::vector<Cmd>& cmds = g_buf.cmds;
    // The copy each pass ends with, and how many passes of each kind and size the frame has.
    static std::vector<uint32_t> passCopy;
    passCopy.assign(g_buf.passes.size(), 0xFFFFFFFFu);
    struct Kind { uint16_t sw, sh, dw, dh; bool direct; uint32_t n; };
    Kind kinds[8];
    int nKinds = 0;
    // Is this pass one for an atlas at all? (k: its kind)
    auto classify = [&](const PassCmd& ps, uint32_t copy, Kind& k) {
        if (copy == 0xFFFFFFFFu || ps.rect.w <= 0 || ps.rect.h <= 0) return false;
        const CopyTexCmd& t = g_buf.copyTex[copy];
        if (!t.fromPass || t.skip || t.depth || t.fill || t.srcTex) return false;
        k = Kind{static_cast<uint16_t>(ps.rect.w), static_cast<uint16_t>(ps.rect.h), t.dstW, t.dstH, ps.directTex != 0, 0};
        if (k.dw == 0 || k.dh == 0 || k.dw > kAtlasMaxCell || k.dh > kAtlasMaxCell) return false;
        if (k.direct) return ps.directTex == t.texId && k.sw == k.dw && k.sh == k.dh;
        if (mode < 2 || k.sw > kAtlasMaxSource || k.sh > kAtlasMaxSource) return false;
        if (t.src.x != ps.rect.x || t.src.y != ps.rect.y || t.src.w != ps.rect.w || t.src.h != ps.rect.h) return false;
        return k.sw % k.dw == 0 && k.sh % k.dh == 0 && k.sw / k.dw == k.sh / k.dh && k.sw / k.dw <= kAtlasMaxRatio;
    };
    auto sameKind = [](const Kind& a, const Kind& b) { return a.sw == b.sw && a.sh == b.sh && a.dw == b.dw && a.dh == b.dh && a.direct == b.direct; };
    {
        uint32_t open = 0xFFFFFFFFu;
        for (const Cmd& c : cmds) {
            if (c.type == CMD_PASS_BEGIN) open = c.arg;
            else if (c.type == CMD_COPY_TEX) {
                if (open != 0xFFFFFFFFu) {
                    passCopy[open] = c.arg;
                    Kind k;
                    if (classify(g_buf.passes[open], c.arg, k)) {
                        int i = 0;
                        while (i < nKinds && !sameKind(kinds[i], k)) ++i;
                        if (i == nKinds && nKinds < 8) kinds[nKinds++] = k;
                        if (i < nKinds) ++kinds[i].n;
                    }
                }
                open = 0xFFFFFFFFu;
            }
        }
    }
    static std::vector<Cmd> out;
    out.clear();
    out.reserve(cmds.size() + 16);
    // The pass that is open (between its CMD_PASS_BEGIN and its copy), if it draws into an atlas.
    uint32_t openTex = 0;      // the picture
    uint32_t openCopy = 0;     // its copy command
    AtlasLoc openLoc{};        // the picture's cell
    bool openDirect = false;
    uint16_t openScratchCell = 0;
    bool inPass = false;       // inside any pass (atlas or not)
    // Converted copies that wait for the passes after them (all from one scratch atlas).
    struct Blit { uint32_t texId, copy; AtlasLoc dst; uint16_t cell; };
    static std::vector<Blit> pending;
    pending.clear();
    int scratch = -1; // index in g_scratchPages of the batch's atlas
    auto flush = [&] {
        if (pending.empty()) return;
        const AtlasPage& sp = g_scratchPages[scratch];
        for (const Blit& b : pending) {
            const AtlasPage& dp = g_atlasPages[b.dst.page];
            const CopyTexCmd& orig = g_buf.copyTex[b.copy];
            CopyTexCmd x;
            // From the pass's cell with its gutter into the picture's cell with its gutter: the
            // same mapping of texels as the original copy, and the picture's gutter gets what
            // the copy makes of the pass's clear colour.
            x.texId = dp.tex;
            x.dstTexW = dp.W();
            x.dstTexH = dp.H();
            x.dstX = static_cast<int16_t>(dp.CellX(b.dst.cell) - dp.gutter);
            x.dstY = static_cast<int16_t>(dp.CellY(b.dst.cell) - dp.gutter);
            x.dstW = static_cast<uint16_t>(dp.cellW + 2 * dp.gutter);
            x.dstH = static_cast<uint16_t>(dp.cellH + 2 * dp.gutter);
            x.srcTex = sp.tex;
            x.srcTexW = sp.W();
            x.srcTexH = sp.H();
            x.src = EfbRect{static_cast<int16_t>(sp.CellX(b.cell) - sp.gutter), static_cast<int16_t>(sp.CellY(b.cell) - sp.gutter),
                            static_cast<int16_t>(sp.cellW + 2 * sp.gutter), static_cast<int16_t>(sp.cellH + 2 * sp.gutter)};
            std::memcpy(x.mat, orig.mat, sizeof x.mat);
            std::memcpy(x.add, orig.add, sizeof x.add);
            g_buf.copyTex.push_back(x);
            out.push_back(Cmd{CMD_COPY_TEX, static_cast<uint32_t>(g_buf.copyTex.size() - 1)});
            g_atlasOf[b.texId] = b.dst; // the picture now lives in its cell
            g_atlasPages[b.dst.page].gut[b.dst.cell].valid = false; // (written by a copy, not by a pass's clear)
        }
        pending.clear();
        scratch = -1;
    };
    auto isPending = [&](uint32_t texId) {
        for (const Blit& b : pending)
            if (b.texId == texId) return true;
        return false;
    };
    for (size_t i = 0; i < cmds.size(); ++i) {
        const Cmd c = cmds[i];
        switch (c.type) {
        case CMD_PASS_BEGIN: {
            // Nothing can be taken out of an atlas in the middle of a pass (that is a draw
            // to another target): what the pass's draws sample and cannot be redirected for
            // them leaves its atlas now.
            if (!pending.empty()) { // (held-back copies whose pictures the pass samples are made first)
                bool samples = false;
                for (size_t j = i + 1; j < cmds.size() && cmds[j].type != CMD_COPY_TEX && !samples; ++j) {
                    if (cmds[j].type != CMD_DRAW) continue;
                    const DrawRec& d = g_buf.draws[cmds[j].arg];
                    for (int m = 0; m < 8; ++m)
                        if ((d.texMask & (1u << m)) && isPending(d.tex[m].texId)) samples = true;
                }
                if (samples) flush();
            }
            if (!g_atlasOf.empty()) {
                for (size_t j = i + 1; j < cmds.size() && cmds[j].type != CMD_COPY_TEX; ++j) {
                    if (cmds[j].type != CMD_DRAW) continue;
                    DrawRec& d = g_buf.draws[cmds[j].arg];
                    for (int m = 0; m < 8; ++m) {
                        if (!(d.texMask & (1u << m))) continue;
                        auto it = g_atlasOf.find(d.tex[m].texId);
                        if (it == g_atlasOf.end() || AtlasRedirect(d, m, it->second, false)) continue;
                        const uint32_t t = it->first;
                        flush();
                        AtlasExtract(out, t, g_atlasOf.find(t)->second); // (looked up again: flush() may have moved the array)
                        AtlasBarNow(t);
                    }
                }
            }
            inPass = true;
            PassCmd& ps = g_buf.passes[c.arg];
            Kind k{};
            const uint32_t copy = passCopy[c.arg];
            bool ok = classify(ps, copy, k);
            const uint32_t t = ok ? g_buf.copyTex[copy].texId : 0;
            if (ok) ok = !AtlasBarred(t);
            if (ok && !g_atlasOf.count(t)) {
                ok = false;
                for (int q = 0; q < nKinds; ++q)
                    if (sameKind(kinds[q], k)) ok = kinds[q].n >= 2;
            }
            // What the pass's own draws sample: not a picture of the atlas it would draw into
            // (a texture cannot be drawn to and sampled at once); and a pass whose copy is
            // held back must not sample atlas pictures at all (they could be ones whose
            // copies are still held back).
            if (ok) {
                for (size_t j = i + 1; j < cmds.size() && cmds[j].type != CMD_COPY_TEX && ok; ++j) {
                    if (cmds[j].type != CMD_DRAW) continue;
                    const DrawRec& d = g_buf.draws[cmds[j].arg];
                    for (int m = 0; m < 8 && ok; ++m) {
                        if (!(d.texMask & (1u << m))) continue;
                        const uint32_t id = d.tex[m].texId;
                        if (isPending(id)) { ok = false; break; }
                        auto it = g_atlasOf.find(id);
                        if (it == g_atlasOf.end()) continue;
                        if (!k.direct || (g_atlasPages[it->second.page].cellW == k.dw && g_atlasPages[it->second.page].cellH == k.dh)) ok = false;
                    }
                }
            }
            if (!ok || k.direct) flush(); // (a direct pass draws into a picture atlas: the held-back copies go first)
            if (ok && !k.direct) {
                // The scratch atlas for this pass size (gutter: the copy's ratio times the picture gutter).
                const uint16_t g = static_cast<uint16_t>(kAtlasGutter * (k.sw / k.dw));
                int sp = -1;
                for (size_t q = 0; q < g_scratchPages.size(); ++q)
                    if (g_scratchPages[q].cellW == k.sw && g_scratchPages[q].cellH == k.sh && g_scratchPages[q].gutter == g) sp = static_cast<int>(q);
                if (sp < 0) {
                    AtlasPage pg;
                    pg.Init(k.sw, k.sh, g, 512);
                    g_scratchPages.push_back(pg);
                    sp = static_cast<int>(g_scratchPages.size() - 1);
                }
                if (scratch != sp || pending.size() >= g_scratchPages[sp].owner.size()) flush();
                scratch = sp;
                openScratchCell = static_cast<uint16_t>(pending.size());
            }
            if (ok) {
                openLoc = AtlasCellFor(out, t, k.dw, k.dh); // (may put a copy of an evicted picture into `out` first)
                g_atlasPages[openLoc.page].lastUse[openLoc.cell] = g_frame;
                if (k.direct) g_atlasPages[openLoc.page].Target(ps, openLoc.cell);
                else g_scratchPages[scratch].Target(ps, openScratchCell);
                openTex = t;
                openCopy = copy;
                openDirect = k.direct;
                ++fr.atlasPasses;
            } else {
                openTex = 0;
            }
            out.push_back(c);
            break;
        }
        case CMD_DRAW: {
            if (!inPass) flush(); // a draw to the EFB: it may sample what the held-back copies make
            if (!g_atlasOf.empty()) {
                DrawRec* d = &g_buf.draws[c.arg];
                bool remapped = false;
                for (int m = 0; m < 8; ++m) {
                    if (!(d->texMask & (1u << m))) continue;
                    auto it = g_atlasOf.find(d->tex[m].texId);
                    if (it == g_atlasOf.end()) continue;
                    const uint32_t t = it->first;
                    const AtlasLoc loc = it->second;
                    if (AtlasRedirect(*d, m, loc, true)) {
                        remapped = true;
                        g_atlasPages[loc.page].lastUse[loc.cell] = g_frame;
                        continue;
                    }
                    if (inPass) { // (cannot happen: the pass's draws were looked at when it began)
                        VGX_LOG_N(8, "copy atlas: texture %u cannot be redirected for a draw inside a pass (frame %llu)\n", t, static_cast<unsigned long long>(g_frame));
                        continue;
                    }
                    // Take the picture back out: a copy from its cell into its own texture, before this draw.
                    AtlasExtract(out, t, loc);
                    AtlasBarNow(t);
                    VGX_LOG_N(16, "copy atlas: texture %u taken out again at frame %llu (a draw samples it in a way that cannot be redirected: test %d, "
                                  "vbuf %u, %u vertices, layout 0x%X, vs %u fs %u)\n", t, static_cast<unsigned long long>(g_frame), g_atlasWhy, d->vbuf,
                              d->vtxCount, d->layoutMask, d->vsId, d->fsId);
                }
                if (remapped) ++fr.atlasRemaps;
            }
            out.push_back(c);
            break;
        }
        case CMD_COPY_TEX: {
            CopyTexCmd& t = g_buf.copyTex[c.arg];
            if (openTex && c.arg == openCopy) {
                if (openDirect) {
                    g_atlasOf[openTex] = openLoc; // the picture now lives in its cell
                } else {
                    // The copy itself is made later (flush); what stays here is the end of the pass and the clear.
                    pending.push_back(Blit{openTex, c.arg, openLoc, openScratchCell});
                    t.skip = true;
                }
            } else if (!t.skip) {
                flush();
                AtlasRelease(t.texId); // written the ordinary way: a texture of its own again
            }
            openTex = 0;
            inPass = false;
            out.push_back(c);
            break;
        }
        case CMD_TEX_FREE:
            flush();
            AtlasRelease(c.arg);
            g_atlasBar.erase(c.arg);
            out.push_back(c);
            break;
        case CMD_SET_TARGET:
            flush();
            // (the back end makes its copy textures anew for another target size)
            for (std::vector<AtlasPage>* list : {&g_atlasPages, &g_scratchPages})
                for (AtlasPage& pg : *list) {
                    std::fill(pg.owner.begin(), pg.owner.end(), 0u);
                    for (AtlasPage::Gut& g : pg.gut) g.valid = false;
                }
            g_atlasOf.clear();
            out.push_back(c);
            break;
        case CMD_COPY_DISP:
        case CMD_PRESENT:
            flush();
            out.push_back(c);
            break;
        default:
            out.push_back(c);
            break;
        }
    }
    flush();
    // Passes that follow each other directly and draw into different pages come out in page
    // order, so that a page is one scene however its pictures were handed out. Only whole
    // passes move (begin, draws, the copy that ends them); only when their copies leave the
    // EFB the same whatever the order (one rectangle, one clear), and when no draw of them
    // samples a page that one of them draws into.
    {
        struct Grp { uint32_t b, e, page; };
        static std::vector<Grp> run;
        static std::vector<Cmd> tmp;
        size_t i = 0;
        while (i < out.size()) {
            run.clear();
            size_t j = i;
            while (j < out.size() && out[j].type == CMD_PASS_BEGIN) {
                const PassCmd& ps = g_buf.passes[out[j].arg];
                if (!ps.atlasTex || !ps.directTex) break;
                size_t e = j + 1;
                while (e < out.size() && out[e].type == CMD_DRAW) ++e;
                if (e >= out.size() || out[e].type != CMD_COPY_TEX || !g_buf.copyTex[out[e].arg].fromPass) break;
                run.push_back(Grp{static_cast<uint32_t>(j), static_cast<uint32_t>(e + 1), ps.atlasTex});
                j = e + 1;
            }
            bool sorted = true;
            for (size_t q = 1; q < run.size(); ++q)
                if (run[q].page < run[q - 1].page) sorted = false;
            if (!sorted) {
                bool safe = true;
                const CopyTexCmd& c0 = g_buf.copyTex[out[run[0].e - 1].arg];
                for (const Grp& g : run) {
                    const CopyTexCmd& cg = g_buf.copyTex[out[g.e - 1].arg];
                    if (std::memcmp(&cg.src, &c0.src, sizeof cg.src) != 0 || cg.clear.enable != c0.clear.enable || cg.clearElided != c0.clearElided ||
                        (cg.clear.enable && (cg.clear.color != c0.clear.color || cg.clear.alpha != c0.clear.alpha || cg.clear.depth != c0.clear.depth ||
                                             std::memcmp(cg.clear.rgba, c0.clear.rgba, sizeof cg.clear.rgba) != 0 || cg.clear.z != c0.clear.z)))
                        safe = false;
                    for (uint32_t k = g.b + 1; k + 1 < g.e && safe; ++k) {
                        const DrawRec& d = g_buf.draws[out[k].arg];
                        for (int m = 0; m < 8 && safe; ++m) {
                            if (!(d.texMask & (1u << m))) continue;
                            for (const Grp& h : run)
                                if (d.tex[m].texId == h.page || d.tex[m].texId == g_buf.passes[out[h.b].arg].directTex) safe = false;
                        }
                    }
                }
                if (safe) {
                    std::stable_sort(run.begin(), run.end(), [](const Grp& x, const Grp& y) { return x.page < y.page; });
                    tmp.clear();
                    for (const Grp& g : run) tmp.insert(tmp.end(), out.begin() + g.b, out.begin() + g.e);
                    std::copy(tmp.begin(), tmp.end(), out.begin() + static_cast<std::ptrdiff_t>(i));
                    ++fr.atlasSorted;
                }
            }
            i = j > i ? j : i + 1;
        }
    }
    // Draws that follow each other and now differ in nothing but their vertices become one.
    static const bool noMerge = EnvInt("MSC_VGX_NOMERGE", 0) != 0;
    size_t n = 0;
    for (size_t i = 0; i < out.size(); ++i) {
        const Cmd c = out[i];
        if (!noMerge && fr.atlasRemaps && c.type == CMD_DRAW && n > 0 && out[n - 1].type == CMD_DRAW) {
            DrawRec& a = g_buf.draws[out[n - 1].arg];
            const DrawRec& b = g_buf.draws[c.arg];
            if (a.vbuf == 0 && b.vbuf == 0 && a.vchunk == b.vchunk && a.vtxOffset == b.vtxOffset && a.prim == b.prim && a.layoutMask == b.layoutMask &&
                a.vsId == b.vsId && a.fsId == b.fsId && a.texMask == b.texMask && a.idxOffset + a.idxCount == b.idxOffset &&
                a.vtxFirst + a.vtxCount == b.vtxFirst && a.vtxCount + b.vtxCount <= 0xFFFF &&
                SameDrawState(a, b)) {
                a.idxCount += b.idxCount;
                a.vtxCount += b.vtxCount;
                g_buf.draws[c.arg].idxCount = 0; // (the record stays in the list, unused)
                g_buf.draws[c.arg].vtxCount = 0;
                ++fr.atlasMerged;
                if (fr.drawCalls) --fr.drawCalls;
                continue;
            }
        }
        out[n++] = c;
    }
    out.resize(n);
    cmds.swap(out);
}
} // namespace

// MSC_VGX_PASSCHECK=1 (diagnostic, slow): checks every claim the analysis
// above made about the EFB against a per-pixel model of it. The model follows
// what the back end will do with the finished frame (draws outside passes make
// the planes they write unknown inside their scissor; executed clears set
// their planes) and each claim is tested pixel by pixel: a pass's rectangle
// holds the pass's start values in the planes that matter, a fill's source
// holds the fill colour in the planes the copy reads, an elided clear's
// rectangle already holds the clear's values. The rectangle history is an
// approximation of exactly this model, so any disagreement is a bug in it.
namespace {
constexpr int kChkW = 640, kChkH = 528;
struct PassCheck {
    std::vector<uint8_t> px[3]; // value id per pixel per plane; 0 = unknown
    std::vector<std::array<float, 3>> vals[3];
    uint64_t checks = 0, failures = 0, pixelsChecked = 0;
    EfbRect lastDraw{};
    uint8_t lastDrawPlanes = 0;
    bool lastDrawValid = false;
    void Reset() {
        for (auto& p : px) p.assign(static_cast<size_t>(kChkW) * kChkH, 0);
        lastDrawValid = false;
    }
    uint8_t Id(int plane, float a, float b = 0, float c = 0) {
        auto& v = vals[plane];
        for (size_t i = 0; i < v.size(); ++i)
            if (v[i][0] == a && v[i][1] == b && v[i][2] == c) return static_cast<uint8_t>(i + 1);
        if (v.size() >= 254) return 255; // (never equal to a stored id below 255 unless it is the same overflow bucket)
        v.push_back({a, b, c});
        return static_cast<uint8_t>(v.size());
    }
    static bool Clip(EfbRect r, int& x0, int& y0, int& x1, int& y1) {
        x0 = std::max<int>(r.x, 0); y0 = std::max<int>(r.y, 0);
        x1 = std::min<int>(r.x + r.w, kChkW); y1 = std::min<int>(r.y + r.h, kChkH);
        return x1 > x0 && y1 > y0;
    }
    void Set(int plane, const EfbRect& r, uint8_t id) {
        int x0, y0, x1, y1;
        if (!Clip(r, x0, y0, x1, y1)) return;
        for (int y = y0; y < y1; ++y) std::memset(&px[plane][static_cast<size_t>(y) * kChkW + x0], id, static_cast<size_t>(x1 - x0));
    }
    bool Holds(int plane, const EfbRect& r, uint8_t id) {
        int x0, y0, x1, y1;
        if (!Clip(r, x0, y0, x1, y1)) return true;
        for (int y = y0; y < y1; ++y) {
            const uint8_t* row = &px[plane][static_cast<size_t>(y) * kChkW];
            for (int x = x0; x < x1; ++x)
                if (row[x] != id) return false;
        }
        pixelsChecked += static_cast<uint64_t>(x1 - x0) * (y1 - y0);
        return true;
    }
    bool HoldsValues(uint8_t planes, const EfbRect& r, const float rgba[4], float z) {
        bool ok = true;
        if (planes & CH_C) ok = Holds(0, r, Id(0, rgba[0], rgba[1], rgba[2])) && ok;
        if (planes & CH_A) ok = Holds(1, r, Id(1, rgba[3])) && ok;
        if (planes & CH_Z) ok = Holds(2, r, Id(2, z)) && ok;
        return ok;
    }
    void Claim(bool ok, const char* what, const EfbRect& r) {
        ++checks;
        if (ok) return;
        ++failures;
        VGX_LOG_N(40, "PASSCHECK FAILED frame %llu: %s, rectangle (%d,%d)-(%d,%d)\n", static_cast<unsigned long long>(g_frame), what, r.x, r.y, r.x + r.w,
                  r.y + r.h);
    }
    void ApplyClear(const EfbRect& r, const ClearSpec& c, bool elided) {
        if (elided) {
            const uint8_t m = static_cast<uint8_t>((c.color ? CH_C : 0) | (c.alpha ? CH_A : 0) | (c.depth ? CH_Z : 0));
            Claim(HoldsValues(m, r, c.rgba, c.z), "elided clear is not a no-op", r);
            return;
        }
        if (!c.enable) return;
        if (c.color) Set(0, r, Id(0, c.rgba[0], c.rgba[1], c.rgba[2]));
        if (c.alpha) Set(1, r, Id(1, c.rgba[3]));
        if (c.depth) Set(2, r, Id(2, c.z));
        lastDrawValid = false;
    }
} g_chk;
} // namespace

// MSC_VGX_COPYCHECK=1 (diagnostic, found while looking for the player-shadow flicker, 9 Oct): follows which
// copy textures hold something in the final command list of every frame and reports a draw that samples a
// copy texture that was never written by an executed copy (or by a pass drawn straight into it / into the
// atlas page it lives in), or whose last writer was freed (CMD_TEX_FREE) since. Both would show as a blank or
// stale picture for the frames until the next copy: a held shadow that lost its picture. Costs nothing off.
// Frames in which a later reader of a skipped copy exists are impossible by construction of MarkDeadCopies;
// this checks that the whole chain (dead-copy removal, pass isolation, atlas) kept that true.
namespace {
struct CopyCheckState {
    enum : uint8_t { IS_COPY = 1, HAS = 2, FREED = 4 };
    std::unordered_map<uint32_t, uint8_t> st;
    uint64_t draws = 0, noContent = 0, afterFree = 0, atlasReadNoContent = 0, frames = 0;
} g_cc;
void CopyCheckSummary() {
    std::fprintf(stderr, "[copycheck] %llu frames, %llu draws sampled a copy texture; %llu of them had never been written, %llu had been freed since; %llu atlas reads of an unwritten page\n",
                 static_cast<unsigned long long>(g_cc.frames), static_cast<unsigned long long>(g_cc.draws), static_cast<unsigned long long>(g_cc.noContent),
                 static_cast<unsigned long long>(g_cc.afterFree), static_cast<unsigned long long>(g_cc.atlasReadNoContent));
}
void CopyCheck() {
    static const bool on = EnvInt("MSC_VGX_COPYCHECK", 0) != 0;
    if (!on) return;
    CopyCheckState& c = g_cc;
    ++c.frames;
    if (g_frame % 1000 == 0) { CopyCheckSummary(); CopyCheckTexReport(); }
    uint32_t openAtlas = 0;
    for (const Cmd& cmd : g_buf.cmds) {
        switch (cmd.type) {
        case CMD_PASS_BEGIN: {
            const PassCmd& ps = g_buf.passes[cmd.arg];
            openAtlas = ps.atlasTex && ps.directTex ? ps.atlasTex : 0;
            if (ps.atlasTex) c.st[ps.atlasTex] |= CopyCheckState::IS_COPY | CopyCheckState::HAS;
            break;
        }
        case CMD_COPY_TEX: {
            const CopyTexCmd& t = g_buf.copyTex[cmd.arg];
            if (t.srcTex) {
                auto it = c.st.find(t.srcTex);
                if (it == c.st.end() || !(it->second & CopyCheckState::HAS)) {
                    ++c.atlasReadNoContent;
                    VGX_LOG_N(40, "[copycheck] frame %llu: a copy reads page %u that holds nothing\n", static_cast<unsigned long long>(g_frame), t.srcTex);
                }
            }
            uint8_t& s = c.st[t.texId];
            s |= CopyCheckState::IS_COPY;
            if (!t.skip && !openAtlas) { s |= CopyCheckState::HAS; s &= static_cast<uint8_t>(~CopyCheckState::FREED); }
            openAtlas = 0;
            break;
        }
        case CMD_TEX_FREE: {
            auto it = c.st.find(cmd.arg);
            if (it != c.st.end() && (it->second & CopyCheckState::IS_COPY)) it->second = CopyCheckState::IS_COPY | CopyCheckState::FREED;
            break;
        }
        case CMD_SET_TARGET:
            for (auto& kv : c.st) kv.second &= static_cast<uint8_t>(~CopyCheckState::HAS); // (the back end makes its copy textures anew)
            break;
        case CMD_DRAW: {
            const DrawRec& d = g_buf.draws[cmd.arg];
            if (!d.idxCount) break;
            for (int m = 0; m < 8; ++m) {
                if (!(d.texMask & (1u << m))) continue;
                auto it = c.st.find(d.tex[m].texId);
                if (it == c.st.end() || !(it->second & CopyCheckState::IS_COPY)) continue;
                ++c.draws;
                if (it->second & CopyCheckState::HAS) continue;
                const bool freed = (it->second & CopyCheckState::FREED) != 0;
                ++(freed ? c.afterFree : c.noContent);
                VGX_LOG_N(60, "[copycheck] frame %llu: a draw (vs %u fs %u) samples copy texture %u, %s\n", static_cast<unsigned long long>(g_frame), d.vsId, d.fsId,
                          d.tex[m].texId, freed ? "freed since its last copy" : "no executed copy has written it");
            }
            break;
        }
        default:
            break;
        }
    }
}
} // namespace

static void VerifyPasses() {
    static const bool on = EnvInt("MSC_VGX_PASSCHECK", 0) != 0;
    if (!on) return;
    if (g_chk.px[0].empty()) g_chk.Reset();
    bool inPass = false;
    for (const Cmd& c : g_buf.cmds) {
        switch (c.type) {
        case CMD_PASS_BEGIN: {
            const PassCmd& ps = g_buf.passes[c.arg];
            g_chk.Claim(g_chk.HoldsValues(ps.known, ps.rect, ps.rgba, ps.z), "pass does not start from the values given", ps.rect);
            inPass = true;
            break;
        }
        case CMD_DRAW: {
            if (inPass) break;
            const RenderState& rs = g_buf.draws[c.arg].rs;
            const EfbRect sc{rs.scX, rs.scY, rs.scW, rs.scH};
            const uint8_t w = DrawWrites(rs);
            if (g_chk.lastDrawValid && std::memcmp(&sc, &g_chk.lastDraw, sizeof sc) == 0 && (w & ~g_chk.lastDrawPlanes) == 0) break;
            for (int p = 0; p < 3; ++p)
                if (w & (1u << p)) g_chk.Set(p, sc, 0);
            if (!g_chk.lastDrawValid || std::memcmp(&sc, &g_chk.lastDraw, sizeof sc) != 0) g_chk.lastDrawPlanes = 0;
            g_chk.lastDraw = sc;
            g_chk.lastDrawPlanes |= w;
            g_chk.lastDrawValid = true;
            break;
        }
        case CMD_COPY_TEX: {
            const CopyTexCmd& t = g_buf.copyTex[c.arg];
            inPass = false;
            if (t.fill) g_chk.Claim(g_chk.HoldsValues(CopyReads(t), t.src, t.fillRgba, 1.0f), "fill source is not that constant", t.src);
            g_chk.ApplyClear(t.src, t.clear, t.clearElided);
            break;
        }
        case CMD_COPY_DISP:
            g_chk.ApplyClear(g_buf.copyDisp[c.arg].src, g_buf.copyDisp[c.arg].clear, g_buf.copyDisp[c.arg].clearElided);
            break;
        case CMD_SET_TARGET:
            g_chk.Reset();
            break;
        default: break;
        }
    }
}
void PassCheckReport(FILE* f) {
    if (g_passRepeat.frames)
        std::fprintf(f, "pass repeats (MSC_VGX_PASSREPEAT): %llu offscreen passes in %llu frames; %llu drew exactly what the pass into the same texture drew "
                        "the frame before, %llu what another pass of the same frame drew\n",
                     static_cast<unsigned long long>(g_passRepeat.passes), static_cast<unsigned long long>(g_passRepeat.frames),
                     static_cast<unsigned long long>(g_passRepeat.sameAsLastFrame), static_cast<unsigned long long>(g_passRepeat.sameAsOther));
    if (g_chk.px[0].empty()) return;
    std::fprintf(f, "pass check (MSC_VGX_PASSCHECK): %llu claims tested against the per-pixel model (%llu pixel-plane comparisons), %llu FAILED\n",
                 static_cast<unsigned long long>(g_chk.checks), static_cast<unsigned long long>(g_chk.pixelsChecked),
                 static_cast<unsigned long long>(g_chk.failures));
}

// Statistics: how often the finished frame makes a back end switch render
// targets (the order of operations is the GL back end's: a copy draws into its
// destination, then its clear goes to the EFB).
static void CountScenes() {
    if (!stats::Detailed()) return;
    stats::Frame& fr = stats::Cur();
    bool inPass = false;
    uint32_t passDirectTex = 0;
    uint32_t target = 0; // 0: none yet, 1: the EFB, 2: some other target, else: an atlas (texture id + 16)
    auto efb = [&] { if (target != 1) { ++fr.efbScenes; target = 1; } };
    auto other = [&](uint32_t id) { if (id == 2 || target != id) ++fr.offScenes; target = id; }; // (passes into one atlas in a row: one scene)
    for (const Cmd& c : g_buf.cmds) {
        switch (c.type) {
        case CMD_PASS_BEGIN:
            other(g_buf.passes[c.arg].atlasTex ? g_buf.passes[c.arg].atlasTex + 16 : 2);
            inPass = true;
            passDirectTex = g_buf.passes[c.arg].directTex;
            break;
        case CMD_DRAW: if (!inPass) efb(); break;
        case CMD_COPY_TEX: {
            const CopyTexCmd& t = g_buf.copyTex[c.arg];
            if (!t.skip) {
                // a copy quad or a fill into the destination (a direct pass has drawn there already)
                const bool direct = t.fromPass && inPass && passDirectTex == t.texId;
                if (!direct) other(t.dstTexW ? t.texId + 16 : 2);
            }
            if (!t.srcTex) inPass = false;
            if (t.clear.enable) efb();
            break;
        }
        case CMD_COPY_DISP:
            if (g_buf.copyDisp[c.arg].clear.enable) efb();
            break;
        default: break;
        }
    }
}

// MSC_VGX_PASSES=<frame> (diagnostic): the frame as a sequence of render
// passes: runs of draws (count, triangles, union of their scissor rectangles,
// copy textures of this frame they sample) separated by EFB copies. For
// planning a back end for a tile-based GPU, where every copy that interrupts
// drawing to the EFB costs a store and a reload of the whole render target.
static void TracePasses() {
    std::vector<uint32_t> copyIds;
    for (const CopyTexCmd& c : g_buf.copyTex) copyIds.push_back(c.texId);
    auto isCopy = [&](uint32_t id) { return std::find(copyIds.begin(), copyIds.end(), id) != copyIds.end(); };
    int n = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    uint8_t wr = 0, rd = 0;
    uint32_t tris = 0;
    std::vector<uint32_t> sampled;
    auto flush = [&] {
        if (!n) return;
        std::fprintf(stderr, "[passes]   %3d draws %6u tris  scissor union (%d,%d)-(%d,%d)  writes %s%s%s reads %s%s%s", n, tris, x0, y0, x1, y1,
                     wr & CH_C ? "c" : "-", wr & CH_A ? "a" : "-", wr & CH_Z ? "z" : "-", rd & CH_C ? "c" : "-", rd & CH_A ? "a" : "-", rd & CH_Z ? "z" : "-");
        if (!sampled.empty()) {
            std::fprintf(stderr, "  samples copies:");
            for (uint32_t id : sampled) std::fprintf(stderr, " %u", id);
        }
        std::fprintf(stderr, "\n");
        n = 0; tris = 0; wr = rd = 0; sampled.clear();
    };
    std::fprintf(stderr, "[passes] frame %llu: %u commands, %u draws, %u texture copies\n", static_cast<unsigned long long>(g_frame), static_cast<unsigned>(g_buf.cmds.size()),
                 static_cast<unsigned>(g_buf.draws.size()), static_cast<unsigned>(g_buf.copyTex.size()));
    for (const Cmd& c : g_buf.cmds) {
        if (c.type == CMD_DRAW) {
            const DrawRec& d = g_buf.draws[c.arg];
            const int ax = d.rs.scX, ay = d.rs.scY, bx = d.rs.scX + d.rs.scW, by = d.rs.scY + d.rs.scH;
            if (!n) { x0 = ax; y0 = ay; x1 = bx; y1 = by; }
            else { x0 = std::min(x0, ax); y0 = std::min(y0, ay); x1 = std::max(x1, bx); y1 = std::max(y1, by); }
            ++n;
            wr |= DrawWrites(d.rs);
            rd |= DrawReads(d.rs);
            tris += d.idxCount / 3;
            for (int m = 0; m < 8; ++m)
                if ((d.texMask & (1u << m)) && isCopy(d.tex[m].texId) && std::find(sampled.begin(), sampled.end(), d.tex[m].texId) == sampled.end())
                    sampled.push_back(d.tex[m].texId);
        } else if (c.type == CMD_COPY_TEX) {
            flush();
            const CopyTexCmd& t = g_buf.copyTex[c.arg];
            char cl[96] = "";
            if (t.clear.enable)
                std::snprintf(cl, sizeof cl, " +clear %s%s%s (%.3f,%.3f,%.3f,%.3f) z %.4f", t.clear.color ? "c" : "-", t.clear.alpha ? "a" : "-", t.clear.depth ? "z" : "-",
                              t.clear.rgba[0], t.clear.rgba[1], t.clear.rgba[2], t.clear.rgba[3], t.clear.z);
            std::fprintf(stderr, "[passes] COPY -> tex %u  src (%d,%d)-(%d,%d)  dst %ux%u%s%s%s%s%s\n", t.texId, t.src.x, t.src.y, t.src.x + t.src.w, t.src.y + t.src.h,
                         t.dstW, t.dstH, t.depth ? " depth" : "", cl, t.skip ? (t.fill ? " (texture already holds it: clear only)" : " (dead: clear only)") : "",
                         t.fromPass ? "  [ends isolated pass]" : "", t.fill ? "  [fill: constant source]" : "");
        } else if (c.type == CMD_PASS_BEGIN) {
            flush();
            const PassCmd& ps = g_buf.passes[c.arg];
            std::fprintf(stderr, "[passes] PASS BEGIN rect (%d,%d)-(%d,%d) cleared to (%.3f,%.3f,%.3f,%.3f) z %.4f%s\n", ps.rect.x, ps.rect.y, ps.rect.x + ps.rect.w,
                         ps.rect.y + ps.rect.h, ps.rgba[0], ps.rgba[1], ps.rgba[2], ps.rgba[3], ps.z, ps.directTex ? "  [straight into the destination texture]" : "");
        } else if (c.type == CMD_COPY_DISP) {
            flush();
            const CopyDispCmd& t = g_buf.copyDisp[c.arg];
            std::fprintf(stderr, "[passes] DISPLAY COPY src (%d,%d)-(%d,%d)%s\n", t.src.x, t.src.y, t.src.x + t.src.w, t.src.y + t.src.h, t.clear.enable ? " +clear" : "");
        }
    }
    flush();
}

void SetShaderListFile(const char* path) { g_shaderListPath = path ? path : ""; }

static void (*g_frameSink)(CmdBuf&) = nullptr;
void SetFrameSink(void (*sink)(CmdBuf& buf)) { g_frameSink = sink; }

void FrontPresent(bool direct, void (*onDecoded)(), uint64_t latencyTag) {
    if (!g_shaderListDone) EnsureShaderList();
    TexCacheEndFrame();
    SiFrame(); // (W5 SKIN soft instancing: the last batch is finished before the frame is looked at)
    g_buf.cmds.push_back(Cmd{CMD_PRESENT, 0});
    g_buf.frame = g_frame;
    g_buf.latencyTag = latencyTag;
    MarkDeadCopies();
    IsolatePasses();
    AtlasPasses();
    VerifyPasses();
    CopyCheck();
    ZFreezeReport();
    CountScenes();
    {
        // MSC_VGX_PASSES=<frame>[,<frame>...]
        static const char* const list = std::getenv("MSC_VGX_PASSES");
        for (const char* q = list; q && *q;) {
            char* end = nullptr;
            const unsigned long long f = std::strtoull(q, &end, 10);
            if (end == q) break;
            if (f == g_frame) TracePasses();
            q = *end == ',' ? end + 1 : end;
        }
    }
    {
        stats::Frame& fr = stats::Cur();
        fr.vtxBytes += static_cast<uint32_t>(g_buf.vtx.size());
        fr.idxBytes += static_cast<uint32_t>(g_buf.idx.size() * 2);
        fr.uniBytes += static_cast<uint32_t>(g_buf.uni.size() * 4);
        static const long hashMode = EnvInt("MSC_VGX_HASH", 0);
        if (hashMode) fr.hash = HashBuf(g_buf, fr.hash, hashMode == 2);
        drawlog::Frame(g_buf, g_frame);
        HashDump(g_buf, g_frame);
    }
    if (StreamCensusOn()) StreamCensusFrame(g_frame);
    if (TagCensusOn()) TagCensusFrame(g_frame);
    if (SoftCensusOn()) SoftCensusFrame(g_frame);
    if (LightNormCensusOn() && (g_frame == 1 || g_frame % 600 == 0))
        std::fprintf(stderr, "[vgx] light directions up to frame %llu: %llu blocks built; %llu non-zero directions written as zero (%llu of them non-finite, %llu not read by the shader at all); %llu kept (%llu non-finite)\n",
                     (unsigned long long)g_frame, (unsigned long long)g_lnc.blocks, (unsigned long long)g_lnc.zeroed, (unsigned long long)g_lnc.zeroedNonFinite,
                     (unsigned long long)g_lnc.zeroedUnread, (unsigned long long)g_lnc.kept, (unsigned long long)g_lnc.keptNonFinite);
    if (PalHistOn() && (g_frame == 1 || g_frame % 600 == 0)) {
        std::fprintf(stderr, "[vgx] palette census up to frame %llu: %llu primitives with per-vertex matrices, %llu size changes between neighbours; sizes P(rows):count",
                     (unsigned long long)g_frame, (unsigned long long)g_palHist.prims, (unsigned long long)g_palHist.keyChanges);
        for (int i = 0; i < 32; ++i)
            if (g_palHist.bySize[i]) std::fprintf(stderr, " %d:%llu", i, (unsigned long long)g_palHist.bySize[i]);
        std::fprintf(stderr, "; highest row:count");
        for (int i = 0; i < 66; ++i)
            if (g_palHist.byRow[i]) {
                if (i < 64) std::fprintf(stderr, " %d:%llu", i, (unsigned long long)g_palHist.byRow[i]);
                else std::fprintf(stderr, " %s:%llu", i == 64 ? "ge64" : "unknown", (unsigned long long)g_palHist.byRow[i]);
            }
        std::fprintf(stderr, "\n");
    }
    if(NpDiagActive&&NpDiagActive()){
        const auto& c=stats::Cur();
        NpDiagEvent(NPD_CACHE,0,latencyTag,NpDiagInts(c.dlHit,c.dlBuild),NpDiagInts(c.dlRehash,c.dlBypass),NpDiagInts(c.texUploads,c.texUploadBytes),NpDiagInts(c.texEvicted,c.texEvictBytes),NpDiagInts(c.texHashes,c.texHashBytes));
        NpDiagEvent(NPD_CACHE,1,latencyTag,NpDiagInts(c.drawCalls,c.tris),NpDiagInts(c.copies,c.copyPixels),NpDiagInts(c.efbScenes,c.offScenes),NpDiagInts(c.bufUploads,c.bufUploadBytes),NpDiagInts(c.vtxBytes,c.idxBytes));
        NpDiagEvent(NPD_CACHE,2,latencyTag,NpDiagInts(c.deadCopies,c.efbReads),NpDiagInts(c.passes,c.passesDirect),NpDiagInts(c.fills,c.fillsRedundant),NpDiagInts(c.shadersVs,c.shadersFs),NpDiagInts(c.softVerts,c.staticDraws));
    }
    DlCacheDiagnostic(latencyTag);
    stats::FrameEnd(g_frame,latencyTag);
    {
        static const long n = EnvInt("MSC_VGX_TRACE_DRAWS", 0);
        static bool done = false;
        if (n > 0 && !done && g_frameDraws >= static_cast<uint32_t>(n)) {
            done = true;
            g_traceFrame = g_frame + 1;
            std::fprintf(stderr, "[trace] tracing frame %llu (previous frame had %u draws)\n", static_cast<unsigned long long>(g_traceFrame), g_frameDraws);
        }
        g_frameDraws = 0;
    }
    g.texDirty = true; // re-resolve bindings at least once per frame
    g.texSlotDirty = 0xFF;
    ++g_frame;
    // Everything of this frame has been read from guest memory: the game
    // thread may go on while the back end draws.
    if (latencyTag && NpLatDecoded) NpLatDecoded(latencyTag);
    if (onDecoded) onDecoded();
    if (capture::Enabled()) capture::OnFrame(g_buf); // MSC_VGX_CAPTURE (vgx_capture.h)
    if (g_frameSink) g_frameSink(g_buf);
    else if (direct) interp::Submit(g_buf); // (backend::Execute unless MSC_VGX_INTERP is set)
    else SubmitToBackend(g_buf);
    g_buf.clear();
    g_blocksStale = true;
    g_skinBlockOff = kNoBlock; // (W5 SKIN: the arena is empty)
    for (uint32_t& o : g_blockOff) o = kNoBlock;
    g_lastDraw = -1;
}

void QueueSetTarget() {
    SetTargetCmd t;
    t.efbW = g.efbW;
    t.efbH = g.efbH;
    g_buf.targets.push_back(t);
    g_buf.cmds.push_back(Cmd{CMD_SET_TARGET, static_cast<uint32_t>(g_buf.targets.size() - 1)});
    g_lastDraw = -1;
}

} // namespace vgx
