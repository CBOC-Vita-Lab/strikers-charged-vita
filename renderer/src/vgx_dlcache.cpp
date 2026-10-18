// Cached display-list geometry (player side).
//
// Nearly all of the game's geometry arrives as display lists in guest RAM
// that hold nothing but one or a few draw commands with indexed attributes,
// and that are called again every frame (often several times per frame, once
// per pass) with the same vertex descriptor and arrays: static meshes, and
// skinned characters whose vertices are static in model space (the skinning
// is done with the matrix palette). Decoding such a list means resolving
// every index against the guest arrays and converting big-endian fixed-point
// to floats, ~100 instructions per vertex even in the attribute-major decoder.
//
// An entry here keeps the decoded vertices of a list for one (vertex
// descriptor, vertex format, array bindings) combination. It is valid while
// the write-generation counters (vgx_guestmem.cpp) under the list bytes and
// under the array ranges the decode read have not moved; when they have, the
// content hash decides. Lists that keep changing are not cached for a while.
//
//   MSC_VGX_DLCACHE=0       off: every call is decoded
//   MSC_VGX_DLCACHE=verify  hash on every call and report an entry whose
//                           bytes changed without a write notification
// Counters: per frame in the CSV (dlHit, dlBuild, dlRehash, dlBypass), totals in vgx_stats.txt.
#include "vgx_guestmem.h"
#include "vgx_state.h"
#include "vgx_ext.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <unordered_map>

namespace vgx {

namespace {

struct Seg {
    uint8_t prim;       // GX draw opcode (primitive type bits)
    uint8_t fmt;
    uint32_t layoutMask;
    uint32_t count;
    uint32_t offset;    // into Entry::verts (bytes)
    uint32_t idxOffset; // into the static index buffer (elements), once promoted
    uint32_t vtxFirst;  // once promoted: what the primitive's indices in the static buffer are offset by
    uint32_t posMtxMax; // highest position-matrix row its vertices name
    uint64_t vhash = 0; // W5 SKIN soft instancing: XXH3 of its decoded vertices (made at the build; the identity of what a batch buffer holds)
};

struct Entry {
    // ---- identity
    const uint8_t* list = nullptr;
    uint32_t nbytes = 0;
    uint8_t vcd[24] = {};          // (24 = whole words: the descriptor array of the front end is 26 long and 21..25 are never set)
    uint8_t fmtMask = 0;           // vertex formats the list's draws use
    VtxAttrFmt vat[8][12] = {};    // rows of those formats (attributes POS..TEX7)
    uint32_t idxMask = 0;          // indexed attributes that were read
    const uint8_t* arr[12] = {};
    uint32_t arrStride[12] = {};
    uint16_t arrLe = 0;            // bit per array (attribute - 9) that was little-endian when decoded (native port)
    // W5 SKIN: decoded with a skin state (vgx_ext.h): the weight / bone arrays and the bone count the VL_SKIN attributes were made from
    bool skin = false;
    const uint8_t* skinW = nullptr;
    const uint8_t* skinI = nullptr;
    uint32_t skinBones = 0, skinVerts = 0;
    uint64_t genSkinW = 0, genSkinI = 0;
    // ---- validity
    uint64_t genList = 0;
    uint64_t genArr[12] = {};
    uint32_t lo[12] = {}, hi[12] = {};
    uint64_t hash = 0;
    uint64_t okEpoch = ~0ull;      // GXD_DLFAST: the write-notification count read just before the generations last found this entry valid
    // ---- content
    std::vector<Seg> segs;
    std::vector<uint8_t> verts;    // decoded vertices (handed to the back end when promoted)
    std::vector<uint16_t> idx;     // only kept for MSC_VGX_HASH=2
    uint32_t totalVerts = 0;
    uint32_t bufId = 0;            // static buffer in the back end (0: vertices are copied into the frame arena per use)
    uint32_t bufBytes = 0;
    bool promotable = false;       // every draw is too large for the CPU-transform-and-merge path
    bool smallOk = false;          // MSC_VGX_PROMOSMALL: not promotable only because some draws are small (none is above 0xFFFF vertices)
    uint8_t calm = 0;              // MSC_VGX_PROMOSMALL: consecutive replays from the arena in which no draw of the list was transformed on the CPU
    // ---- policy
    uint64_t lastUse = 0;
    uint8_t fails = 0;             // consecutive content changes
    bool raced = false;            // a write notification arrived while this was being built: rebuild on next use
    Entry* next = nullptr;         // other variants of the same list
};

// Per list: its variants (same bytes, different vertex state / arrays) and the
// bookkeeping that notices a list whose bindings change every frame (meshes
// whose arrays come from a per-frame allocator): for those every call would
// build a new variant that is never used again.
struct ListInfo {
    Entry* head = nullptr;
    uint64_t lastNewVariant = 0; // frame
    uint8_t streak = 0;          // consecutive frames that needed a new variant
};
constexpr int kMaxVariants = 8;
constexpr uint64_t kRetainFrames = 600;
std::unordered_map<const uint8_t*, ListInfo> g_map;
// In front of g_map (see CacheFront in vgx_texcache.cpp): the list last looked up for each
// of 256 address classes. Emptied whenever an entry of g_map is erased.
// (GXD_DLFAST: 1024 classes from a mix of address bits, emptied by a stamp; with 256 classes of the low bits 39 % of the calls of an open-play frame
// found another list in their class and went to the hash table, whose bucket index is a library division on the A9)
constexpr uint32_t kMapFrontN = 1024;
struct MapFront { const uint8_t* list; ListInfo* li; uint32_t stamp; };
MapFront g_mapFront[kMapFrontN] = {};
uint32_t g_mapFrontStamp = 1;
inline uint32_t MapFrontIndex(const uint8_t* list) {
    const uintptr_t p = reinterpret_cast<uintptr_t>(list);
    return Gxd(GXD_DLFAST) ? static_cast<uint32_t>((p >> 5) ^ (p >> 15)) & (kMapFrontN - 1) : static_cast<uint32_t>(p >> 5) & 255u;
}
inline void MapFrontClear() {
    if (++g_mapFrontStamp == 0) { for (MapFront& f : g_mapFront) f.li = nullptr; g_mapFrontStamp = 1; }
}
struct Bypass {
    uint64_t until = 0;
    uint8_t level = 0; // each time a list proves volatile again it is left alone twice as long
};
uint64_t SetBypass(Bypass& b, uint64_t frame) {
    b.until = frame + (120ull << b.level);
    if (b.level < 4) ++b.level;
    return b.until;
}
std::unordered_map<const uint8_t*, Bypass> g_bypass; // lists not worth caching for now (state inside, volatile)
// GXD_DLFAST: a bit set for every list ever put into g_bypass (never cleared), so a call whose list has no bit skips the hash table lookup (two library
// divisions on the A9 for a miss that walks one node); a set bit only means "look".
uint32_t g_bypassBits[32] = {};
inline uint32_t BypassBit(const uint8_t* list) {
    const uintptr_t p = reinterpret_cast<uintptr_t>(list);
    return static_cast<uint32_t>((p >> 2) ^ (p >> 12)) & 1023u;
}
inline Bypass& BypassFor(const uint8_t* list) {
    const uint32_t b = BypassBit(list);
    g_bypassBits[b >> 5] |= 1u << (b & 31);
    return g_bypass[list];
}
inline bool BypassMaybe(const uint8_t* list) {
    const uint32_t b = BypassBit(list);
    return (g_bypassBits[b >> 5] >> (b & 31)) & 1u;
}

int g_mode = -1; // 0 off, 1 on, 2 verify
bool g_static = true;   // MSC_VGX_STATICBUF=0: never promote (decoded vertices are copied into the arena every time)
bool g_keepCopy = false; // MSC_VGX_HASH=2: keep the bytes of promoted entries for hashing
// MSC_VGX_PROMOSMALL=<n> (default 0 = off): a list with draws of at most MSC_VGX_SOFTXFORM vertices is normally never promoted, because such draws may be
// transformed on the CPU and merged with their neighbours. Those the front end did NOT transform (palette-matrix and model-space texgen draws) stay in the
// stream every frame and are not merged more than any other draw: after n consecutive replays without a CPU-transformed draw the list gets a static
// buffer like a big one. Never undone (a promoted list is drawn on the GPU path whatever its state turns out to be).
uint32_t g_promoSmall = 0;
struct Counters {
    uint64_t hits = 0, builds = 0, rehashOk = 0, rehashChanged = 0, bypassed = 0, uncacheable = 0, staleNoNotify = 0, evicted = 0;
    uint64_t hitVerts = 0, builtVerts = 0, liveBytes = 0, staticBytes = 0, promoted = 0, staticBufs = 0, promotedSmall = 0;
} g_ctr;

// Static buffer ids: small, reused, owner known (for MSC_VGX_HASH=2).
std::vector<uint32_t> g_freeIds;
std::vector<Entry*> g_bufOwner(1, nullptr);
uint32_t AllocBufId(Entry* e) {
    uint32_t id;
    if (!g_freeIds.empty()) { id = g_freeIds.back(); g_freeIds.pop_back(); }
    else { id = static_cast<uint32_t>(g_bufOwner.size()); g_bufOwner.push_back(nullptr); }
    g_bufOwner[id] = e;
    return id;
}
void ReleaseBuffer(Entry& e) {
    if (!e.bufId) return;
    Pending().cmds.push_back(Cmd{CMD_BUF_FREE, e.bufId});
    g_bufOwner[e.bufId] = nullptr;
    g_freeIds.push_back(e.bufId);
    g_ctr.staticBytes -= e.bufBytes;
    --g_ctr.staticBufs;
    e.bufId = 0;
    e.bufBytes = 0;
}
// Moves the entry's geometry into a static buffer of the back end.
void Promote(Entry& e) {
    // With absolute indices every primitive of the buffer is addressed from the buffer's
    // start: that needs one stride for all of them and 16 bits to be enough. (Lists with
    // more than one primitive are rare in this game; one that does not qualify stays in
    // the per-frame arena.)
    const bool abs = AbsoluteIndices();
    const uint32_t stride = MakeLayout(e.segs[0].layoutMask).stride;
    if (abs) {
        bool ok = e.totalVerts <= 0x10000;
        for (const Seg& sg : e.segs) ok = ok && MakeLayout(sg.layoutMask).stride == stride;
        if (!ok) { e.promotable = false; e.smallOk = false; return; }
    }
    BufUpload up;
    up.bufId = e.bufId = AllocBufId(&e);
    for (Seg& sg : e.segs) {
        sg.idxOffset = static_cast<uint32_t>(up.idx.size());
        sg.vtxFirst = abs ? sg.offset / stride : 0;
        const uint32_t n = PrimIndices(sg.prim, sg.count, 0, nullptr);
        up.idx.resize(up.idx.size() + n);
        PrimIndices(sg.prim, sg.count, sg.vtxFirst, up.idx.data() + sg.idxOffset);
    }
    e.bufBytes = static_cast<uint32_t>(e.verts.size() + up.idx.size() * 2);
    g_ctr.liveBytes -= e.verts.capacity();
    if (g_keepCopy) {
        up.vtx = e.verts;
        e.idx = up.idx;
        g_ctr.liveBytes += e.verts.capacity();
    } else {
        up.vtx = std::move(e.verts);
        e.verts = std::vector<uint8_t>();
    }
    g_ctr.staticBytes += e.bufBytes;
    ++g_ctr.promoted;
    if (!e.promotable) ++g_ctr.promotedSmall;
    ++g_ctr.staticBufs;
    stats::Frame& fr = stats::Cur();
    ++fr.bufUploads;
    fr.bufUploadBytes += e.bufBytes;
    CmdBuf& buf = Pending();
    buf.bufUploads.push_back(std::move(up));
    buf.cmds.push_back(Cmd{CMD_BUF_UPLOAD, static_cast<uint32_t>(buf.bufUploads.size() - 1)});
}

void Init() {
    if (g_mode >= 0) return;
    const char* m = std::getenv("MSC_VGX_DLCACHE");
    if (m && !std::strcmp(m, "verify")) g_mode = 2;
    else g_mode = (m && *m == '0') ? 0 : 1;
    g_static = EnvInt("MSC_VGX_STATICBUF", 1) != 0;
    g_keepCopy = EnvInt("MSC_VGX_HASH", 0) == 2;
    g_promoSmall = static_cast<uint32_t>(EnvInt("MSC_VGX_PROMOSMALL", 0));
}

// Walks a list that consists of draw commands (and padding) only. Fills
// e.segs / e.verts / spans; false if the list holds anything else.
bool Build(Entry& e, const uint8_t* p, uint32_t nbytes) {
    const uint8_t* const end = p + nbytes;
    ReleaseBuffer(e);
    e.segs.clear();
    e.verts.clear();
    e.idx.clear();
    e.totalVerts = 0;
    e.promotable = true;
    e.smallOk = true;
    e.calm = 0;
    e.fmtMask = 0;
    e.idxMask = 0;
    for (int i = 0; i < 12; ++i) { e.lo[i] = 0xFFFFFFFFu; e.hi[i] = 0; }
    while (p < end) {
        const uint8_t cmd = *p++;
        if (cmd == 0x00) continue;
        if (!(cmd & 0x80)) return false; // register loads, nested lists: not a pure geometry list
        if (end - p < 2) return false;
        const uint32_t count = be16(p);
        p += 2;
        const int fmt = cmd & 7;
        const Plan& plan = CurrentPlan(fmt);
        if (plan.streamSize == 0) return false;
        const uint64_t used = static_cast<uint64_t>(count) * plan.streamSize;
        if (used > static_cast<uint64_t>(end - p)) return false; // truncated: let the general parser deal with it
        if (count) {
            Seg s;
            s.prim = cmd & 0xF8;
            s.fmt = static_cast<uint8_t>(fmt);
            s.layoutMask = plan.layoutMask;
            s.count = count;
            s.offset = static_cast<uint32_t>(e.verts.size());
            s.idxOffset = 0;
            s.vtxFirst = 0;
            if (count <= SoftXformLimit() || count > 0xFFFF) e.promotable = false;
            if (count > 0xFFFF) e.smallOk = false;
            const bool sk = e.skin && SkinPosIndexed(plan); // (W5 SKIN)
            if (e.skin && !sk) SkinNoteMissed(count);
            if (sk) s.layoutMask |= VL_SKIN;
            e.verts.resize(e.verts.size() + static_cast<size_t>(count) * MakeLayout(s.layoutMask).stride);
            ArraySpans sp;
            DecodeVertices(plan, p, count, e.verts.data() + s.offset, &sp);
            if (sk) SkinExpand(plan, p, count, e.verts.data() + s.offset);
            s.posMtxMax = LastDecodedPosMtxMax();
            for (int a = 9; a <= 20; ++a)
                if (sp.mask & (1u << a)) {
                    e.idxMask |= 1u << a;
                    if (sp.lo[a - 9] < e.lo[a - 9]) e.lo[a - 9] = sp.lo[a - 9];
                    if (sp.hi[a - 9] > e.hi[a - 9]) e.hi[a - 9] = sp.hi[a - 9];
                }
            e.segs.push_back(s);
            e.totalVerts += count;
            if (!(e.fmtMask & (1u << fmt))) {
                e.fmtMask |= static_cast<uint8_t>(1u << fmt);
                std::memcpy(e.vat[fmt], &g.vat[fmt][9], sizeof e.vat[fmt]);
            }
        }
        p += used;
    }
    return !e.segs.empty();
}

uint64_t ContentHash(const Entry& e) {
    uint64_t h = XXH3_64bits(e.list, e.nbytes);
    for (int a = 9; a <= 20; ++a)
        if (e.idxMask & (1u << a)) h = XXH3_64bits_withSeed(e.arr[a - 9] + e.lo[a - 9], e.hi[a - 9] - e.lo[a - 9], h);
    if (e.skin) {
        h = XXH3_64bits_withSeed(e.skinW, static_cast<size_t>(e.skinVerts) * 16u, h);
        h = XXH3_64bits_withSeed(e.skinI, static_cast<size_t>(e.skinVerts) * 4u, h);
    }
    return h;
}

// Reads the generations of everything the entry depends on; false if some of
// it is not tracked guest RAM.
bool ReadGenerations(const Entry& e, uint64_t& genList, uint64_t genArr[12]) {
    genList = GuestWriteGeneration(e.list, e.nbytes);
    bool ok = genList != kNotGuest;
    for (int a = 9; a <= 20; ++a)
        if (e.idxMask & (1u << a)) {
            genArr[a - 9] = GuestWriteGeneration(e.arr[a - 9] + e.lo[a - 9], e.hi[a - 9] - e.lo[a - 9]);
            ok = ok && genArr[a - 9] != kNotGuest;
        }
    if (e.skin) { // (W5 SKIN: folded into the list's generation: the weight and bone arrays are model data like the list)
        const uint64_t gw = GuestWriteGeneration(e.skinW, static_cast<size_t>(e.skinVerts) * 16u);
        const uint64_t gi = GuestWriteGeneration(e.skinI, static_cast<size_t>(e.skinVerts) * 4u);
        ok = ok && gw != kNotGuest && gi != kNotGuest;
        genList = genList * 0x9E3779B97F4A7C15ull ^ gw * 0xC2B2AE3D27D4EB4Full ^ gi;
    }
    return ok;
}

static_assert(sizeof(VtxAttrFmt[12]) % 4 == 0, "the vertex-format rows compare as words");
bool SameState(const Entry& e) {
    if (Gxd(GXD_WORDCMP)) {
        if (!SameWords(e.vcd, g.vcd, 6)) return false;
        for (int f = 0; f < 8; ++f)
            if ((e.fmtMask & (1u << f)) && !SameWords(e.vat[f], &g.vat[f][9], sizeof e.vat[f] / 4)) return false;
    } else {
        if (std::memcmp(e.vcd, g.vcd, 21) != 0) return false;
        for (int f = 0; f < 8; ++f)
            if ((e.fmtMask & (1u << f)) && std::memcmp(e.vat[f], &g.vat[f][9], sizeof e.vat[f]) != 0) return false;
    }
    for (int a = 9; a <= 20; ++a)
        if ((e.idxMask & (1u << a)) && (e.arr[a - 9] != g.arrays[a].data || e.arrStride[a - 9] != g.arrays[a].stride ||
                                         e.hi[a - 9] > g.arrays[a].size || (((e.arrLe >> (a - 9)) & 1u) != 0) != g.arrays[a].le))
            return false;
    if (e.skin != g_skin.on) return false; // (W5 SKIN: the attributes, not the matrices: those are uniforms)
    if (e.skin && (e.skinW != g_skin.weights || e.skinI != g_skin.indices || e.skinBones != g_skin.nBones || e.skinVerts != g_skin.nVerts)) return false;
    return true;
}

void Emit(Entry& e, uint32_t nbytes) {
    stats::Frame& fr = stats::Cur();
    ++fr.streams;
    fr.streamBytes += nbytes;
    if (e.bufId) {
        for (const Seg& s : e.segs) {
            SetPendingPosMtxMax(s.posMtxMax);
            EmitStaticDraw(s.prim, s.layoutMask, s.count, e.bufId, AbsoluteIndices() ? 0 : s.offset, s.idxOffset, s.vtxFirst);
            ++fr.primsDL;
            fr.vertsDL += s.count;
        }
        return;
    }
    bool anySoft = false;
    for (const Seg& s : e.segs) {
        const uint32_t stride = MakeLayout(s.layoutMask).stride;
        SetPendingPosMtxMax(s.posMtxMax);
        g_replayList = e.list;
        g_replaySeg = static_cast<uint32_t>(&s - e.segs.data());
        g_replayHash = s.vhash;
        if (SoftInstEmit(s.prim, s.layoutMask, s.count, e.verts.data() + s.offset)) { // (W5 SKIN soft instancing: taken from the entry, no arena copy)
            g_replayList = nullptr;
            anySoft = anySoft || LastDrawNeedsArena(); // (as below: a culled draw that would not be transformed on the CPU leaves the list calm)
            ++fr.primsDL;
            fr.vertsDL += s.count;
            continue;
        }
        g_replayList = nullptr;
        uint8_t* out = BeginVertices(s.layoutMask, s.count);
        std::memcpy(out, e.verts.data() + s.offset, static_cast<size_t>(s.count) * stride);
        SetPendingPosMtxMax(s.posMtxMax);
        g_streamCensusList = true; // (MSC_VGX_STREAMCENSUS: this draw is a replayed list)
        g_replayList = e.list;     // (MSC_VGX_SOFTCENSUS, MSC_VGX_SOFTINST)
        g_replaySeg = static_cast<uint32_t>(&s - e.segs.data());
        g_replayHash = s.vhash;
        EmitDraw(s.prim, s.layoutMask, s.count);
        g_streamCensusList = false;
        g_replayList = nullptr;
        anySoft = anySoft || LastDrawNeedsArena();
        ++fr.primsDL;
        fr.vertsDL += s.count;
    }
    if (g_promoSmall) e.calm = anySoft ? 0 : (e.calm < 255 ? static_cast<uint8_t>(e.calm + 1) : e.calm);
}

void Free(Entry* e) {
    ReleaseBuffer(*e);
    g_ctr.liveBytes -= e->verts.capacity();
    delete e;
}

// Drops entries that were not used for a long time (called once in a while).
void Sweep(uint64_t frame) {
    for (auto it = g_map.begin(); it != g_map.end();) {
        Entry** link = &it->second.head;
        while (*link) {
            Entry* e = *link;
            if (frame - e->lastUse > kRetainFrames) {
                *link = e->next;
                Free(e);
                ++g_ctr.evicted;
            } else {
                link = &e->next;
            }
        }
        if (!it->second.head) { it = g_map.erase(it); MapFrontClear(); }
        else ++it;
    }
    for (auto it = g_bypass.begin(); it != g_bypass.end();) {
        if (it->second.until + kRetainFrames < frame) it = g_bypass.erase(it);
        else ++it;
    }
}

void FreeAll(ListInfo& li) {
    while (li.head) {
        Entry* e = li.head;
        li.head = e->next;
        Free(e);
        ++g_ctr.evicted;
    }
}
void Unlink(ListInfo& li, Entry* e) {
    Entry** link = &li.head;
    while (*link != e) link = &(*link)->next;
    *link = e->next;
    Free(e);
}

} // namespace

uint32_t AllocStaticBufferId() { return AllocBufId(nullptr); }
void FreeStaticBufferId(uint32_t id) {
    if (!id || id >= g_bufOwner.size()) return;
    Pending().cmds.push_back(Cmd{CMD_BUF_FREE, id});
    g_bufOwner[id] = nullptr;
    g_freeIds.push_back(id);
}

bool StaticBufferData(uint32_t bufId, const uint8_t*& vtx, const uint16_t*& idx) {
    if (bufId >= g_bufOwner.size() || !g_bufOwner[bufId] || g_bufOwner[bufId]->verts.empty()) return false;
    vtx = g_bufOwner[bufId]->verts.data();
    idx = g_bufOwner[bufId]->idx.data();
    return true;
}

void DlCacheReport(FILE* f) {
    std::fprintf(f,
                 "display-list cache: %llu hits (%llu verts), %llu builds (%llu verts), %llu re-validated by hash, %llu changed, %llu bypassed calls, "
                 "%llu uncacheable, %llu evicted, %llu changed-without-notification (verify mode), %.1f MB of decoded vertices live in the front end, "
                 "%llu promotions to static buffers (%llu of them lists with small draws, MSC_VGX_PROMOSMALL), %llu static buffers / %.1f MB live in the back end\n",
                 (unsigned long long)g_ctr.hits, (unsigned long long)g_ctr.hitVerts, (unsigned long long)g_ctr.builds,
                 (unsigned long long)g_ctr.builtVerts, (unsigned long long)g_ctr.rehashOk, (unsigned long long)g_ctr.rehashChanged,
                 (unsigned long long)g_ctr.bypassed, (unsigned long long)g_ctr.uncacheable, (unsigned long long)g_ctr.evicted,
                 (unsigned long long)g_ctr.staleNoNotify, g_ctr.liveBytes / 1048576.0, (unsigned long long)g_ctr.promoted,
                 (unsigned long long)g_ctr.promotedSmall, (unsigned long long)g_ctr.staticBufs, g_ctr.staticBytes / 1048576.0);
}

void CallList(const uint8_t* list, uint32_t nbytes) {
    Init();
    const uint64_t frame = CurrentFrame();
    if (g_mode == 0 || nbytes < 16 || !IsGuestRam(list, nbytes) || TraceFrame()) {
        ProcessStream(list, nbytes);
        return;
    }
    {
        static uint64_t lastSweep = 0;
        if (frame - lastSweep >= 120) { lastSweep = frame; Sweep(frame); }
    }
    if (!g_bypass.empty() && (!Gxd(GXD_DLFAST) || BypassMaybe(list))) {
        auto b = g_bypass.find(list);
        if (b != g_bypass.end()) {
            if (frame < b->second.until) {
                ++g_ctr.bypassed;
                ++stats::Cur().dlBypass;
                ProcessStream(list, nbytes);
                return;
            }
            // expired: try caching again (the entry stays, it remembers the back-off level)
        }
    }
    MapFront& mf = g_mapFront[MapFrontIndex(list)];
    if (!(mf.li && mf.list == list && mf.stamp == g_mapFrontStamp)) {
        mf.li = &g_map[list];
        mf.list = list;
        mf.stamp = g_mapFrontStamp;
    }
    ListInfo& li = *mf.li;
    Entry* e = li.head;
    while (e && !(e->nbytes == nbytes && SameState(*e))) e = e->next;
    if (e && e->raced) {
        // Built while the game thread was flushing something: not trusted, build again.
    } else if (e) {
        uint64_t gl = 0, ga[12];
        bool tracked, same;
        // GXD_DLFAST: no write notification at all since the generations were last read and found unchanged: nothing the entry depends on can
        // have changed (the count is read before the generations, so a write that lands in between shows at the next call).
        const bool fast = Gxd(GXD_DLFAST);
        const uint64_t ep = fast ? GuestWriteEpoch() : 0;
        if (fast && e->okEpoch == ep) {
            tracked = same = true;
            if (Gxd(GXD_VERIFY)) {
                uint64_t gl2, ga2[12];
                bool same2 = ReadGenerations(*e, gl2, ga2) && gl2 == e->genList;
                for (int a = 9; a <= 20 && same2; ++a)
                    if (e->idxMask & (1u << a)) same2 = ga2[a - 9] == e->genArr[a - 9];
                if (!same2) {
                    static int fails = 0;
                    if (++fails <= 8) std::fprintf(stderr, "[vgx] GXD VERIFY FAILED: dl cache shortcut trusted a list (%p, %u bytes) whose generations moved\n", (const void*)list, nbytes);
                }
            }
        } else {
            tracked = ReadGenerations(*e, gl, ga);
            same = tracked && gl == e->genList;
            for (int a = 9; a <= 20 && same; ++a)
                if (e->idxMask & (1u << a)) same = ga[a - 9] == e->genArr[a - 9];
            if (fast && same) e->okEpoch = ep;
        }
        if (same && g_mode == 2 && ContentHash(*e) != e->hash) {
            ++g_ctr.staleNoNotify;
            VGX_LOG_N(32, "dl cache: list %p (%u bytes, %u verts) changed without a write notification\n", (const void*)list, nbytes, e->totalVerts);
            same = false;
        }
        if (same) {
            e->lastUse = frame;
            e->fails = 0;
            ++g_ctr.hits;
            ++stats::Cur().dlHit;
            g_ctr.hitVerts += e->totalVerts;
            // Used again unchanged: worth a static buffer (no per-frame copy, no index generation).
            if (!e->bufId && g_static) {
                if (e->promotable) Promote(*e);
                else if (g_promoSmall && e->smallOk && e->calm >= g_promoSmall) Promote(*e);
            }
            Emit(*e, nbytes);
            return;
        }
        if (tracked && ContentHash(*e) == e->hash) {
            // A write landed in the same 4 KiB as something this entry uses, but not on it.
            e->genList = gl;
            std::memcpy(e->genArr, ga, sizeof ga);
            e->okEpoch = ~0ull;
            e->lastUse = frame;
            e->fails = 0;
            ++g_ctr.rehashOk;
            ++stats::Cur().dlRehash;
            g_ctr.hitVerts += e->totalVerts;
            Emit(*e, nbytes);
            return;
        }
        ++g_ctr.rehashChanged;
        if (++e->fails >= 3) {
            // Rewritten every time it is drawn (dynamic geometry): hashing and
            // storing it is wasted work. Decode directly for the next two seconds.
            SetBypass(BypassFor(list), frame);
            Unlink(li, e);
            if (!li.head) { g_map.erase(list); MapFrontClear(); }
            ProcessStream(list, nbytes);
            return;
        }
    } else {
        // A new variant. One every frame means the bindings never repeat.
        int n = 0;
        Entry* lru = nullptr;
        for (Entry* v = li.head; v; v = v->next) {
            ++n;
            if (!lru || v->lastUse < lru->lastUse) lru = v;
        }
        if (n) {
            if (li.lastNewVariant + 1 == frame) ++li.streak;
            else if (li.lastNewVariant != frame) li.streak = 1;
            li.lastNewVariant = frame;
            if (li.streak >= 4) {
                SetBypass(BypassFor(list), frame);
                FreeAll(li);
                g_map.erase(list);
                MapFrontClear();
                ++g_ctr.bypassed;
                ++stats::Cur().dlBypass;
                ProcessStream(list, nbytes);
                return;
            }
            if (n >= kMaxVariants) {
                Unlink(li, lru);
                ++g_ctr.evicted;
            }
        } else {
            li.lastNewVariant = frame;
            li.streak = 1;
        }
        e = new Entry();
        e->next = li.head;
        li.head = e;
    }
    // (Re)build. The array ranges are only known after the decode, so a write
    // that races with it cannot be pinned to this entry: any write notification
    // between here and the end of the build makes the entry untrusted.
    const uint64_t epoch0 = GuestWriteEpoch();
    g_ctr.liveBytes -= e->verts.capacity();
    e->list = list;
    e->nbytes = nbytes;
    std::memcpy(e->vcd, g.vcd, 24);
    e->okEpoch = ~0ull;
    e->skin = g_skin.on;
    e->skinW = g_skin.weights;
    e->skinI = g_skin.indices;
    e->skinBones = g_skin.nBones;
    e->skinVerts = g_skin.nVerts;
    const bool pure = Build(*e, list, nbytes);
    if (pure && SoftInstOn()) // (only the segments soft instancing could take: small, not skinned)
        for (Seg& sg : e->segs)
            if (sg.count <= SoftXformLimit() && !(sg.layoutMask & VL_SKIN))
                sg.vhash = XXH3_64bits(e->verts.data() + sg.offset, static_cast<size_t>(sg.count) * MakeLayout(sg.layoutMask).stride);
    g_ctr.liveBytes += e->verts.capacity();
    bool ok = pure;
    if (ok) {
        for (int a = 9; a <= 20; ++a)
            if (e->idxMask & (1u << a)) {
                e->arr[a - 9] = g.arrays[a].data;
                e->arrStride[a - 9] = g.arrays[a].stride;
                if (g.arrays[a].le) e->arrLe = static_cast<uint16_t>(e->arrLe | (1u << (a - 9)));
                else e->arrLe = static_cast<uint16_t>(e->arrLe & ~(1u << (a - 9)));
            }
        uint64_t gl, ga[12];
        ok = ReadGenerations(*e, gl, ga);
        if (ok) {
            e->genList = gl;
            std::memcpy(e->genArr, ga, sizeof ga);
            e->hash = ContentHash(*e);
            e->raced = GuestWriteEpoch() != epoch0;
        }
    }
    if (!ok) {
        // State commands inside, or data outside tracked guest RAM: the general
        // parser handles it, and the question is not asked again for a while.
        ++g_ctr.uncacheable;
        BypassFor(list).until = frame + 600;
        Unlink(li, e);
        if (!li.head) { g_map.erase(list); MapFrontClear(); }
        ProcessStream(list, nbytes);
        return;
    }
    e->lastUse = frame;
    ++g_ctr.builds;
    ++stats::Cur().dlBuild;
    g_ctr.builtVerts += e->totalVerts;
    Emit(*e, nbytes);
}

} // namespace vgx
