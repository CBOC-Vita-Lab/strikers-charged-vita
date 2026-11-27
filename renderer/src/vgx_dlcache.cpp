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
//   MSC_VGX_DLFLUSH_STADIUM=0  keep the entries a torn-down stadium left behind for the full kRetainFrames (default: free them, see FlushStep)
// Counters: per frame in the CSV (dlHit, dlBuild, dlRehash, dlBypass), totals in vgx_stats.txt.
#include <algorithm>
#include "vgx_guestmem.h"
#include "vgx_hle.h"
#include "vgx_state.h"
#include "vgx_perf_diag.h"
#include "vgx_ext.h"
#include "vgx_workload.h"
#include "vgx_housekeep.h"

#define XXH_INLINE_ALL
#include <xxhash.h>
#include "vgx_dlweld.h"
#include "vgx_dlweldbudget.h"   // nxfly25: MSC_VGX_DLWELD_BUDGET_US / _LISTS

#include <atomic>
#include <unordered_map>

namespace vgx {

void DynBypassNote(const char* why, const uint8_t* list, uint32_t nbytes);   // (below: BypassNote without an entry)

namespace {

struct Seg {
    uint8_t prim;       // GX draw opcode (primitive type bits)
    uint8_t fmt;
    uint32_t layoutMask;
    uint32_t count;
    uint32_t offset;    // into Entry::verts (bytes)
    uint32_t idxOffset; // into the static index buffer (elements), once promoted
    uint32_t vtxFirst;  // once promoted: what the primitive's indices in the static buffer are offset by
    uint32_t srcOff = 0; // MSC_VGX_DLREFRESH: where the primitive's vertex stream starts in the list (bytes)
    uint32_t wOff = 0;  // MSC_VGX_DLWELD: byte offset of the welded vertex block of the primitive's group in the static buffer (Entry::welded)
    uint32_t posMtxMax; // highest position-matrix row its vertices name
    uint32_t posMtxMask = 0; // MSC_VGX_PALCOMPACT: bit k = the matrix at row 3k is named by a vertex (0: unknown: a row that is no multiple of 3, or not below 30)
    bool boundsOk = false; // MSC_VGX_CULL: bounds holds the box of the decoded positions (not skinned, all finite)
    float bounds[6] = {};  // min xyz, max xyz
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
    uint64_t okWatch = ~0ull;      // MSC_VGX_DLWATCH: the watch epoch read just before the generations last found this entry valid (its pages are watched)
    bool watched = false;
    uint64_t watchMask = 0;        // gem2b MSC_VGX_DLWATCH_BUCKETS: the buckets of the entry's watched pages (WatchRangeMask); 0 = none known
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
    uint8_t solo = 0;              // MSC_VGX_PROMOSOLO: consecutive replays whose CPU-transformed draw merged with nothing
    // ---- policy
    uint64_t lastUse = 0;
    uint8_t fails = 0;             // consecutive content changes
    bool raced = false;            // a write notification arrived while this was being built: rebuild on next use
    bool skinCtr = false;          // MSC_VGX_SKINMERGE: palKept is counted in skinKeptBytes (a skinned list), not palKeptBytes
    bool welded = false;           // MSC_VGX_DLWELD: the static buffer holds each distinct vertex of a group once (draws use Seg::wOff)
    bool palKept = false;          // MSC_VGX_PALMERGE: the decoded vertices were kept after the promotion (palette runs gather them into combined buffers)
    bool sbKept = false;           // MSC_VGX_SBATCH: kept after the promotion for static batches (vgx_draw.cpp SbFrame), counted in sbKeptBytes
    bool sbNoted = false;          // MSC_VGX_SBATCH: a draw of it was a batch candidate (kept copies of lists never noted are dropped after a while)
    uint64_t sbKeptAt = 0;         // MSC_VGX_SBATCH: cache-use frame of the promotion
    bool sbWant = false;           // MSC_VGX_SBATCH: a candidate draw found no copy: decode the list again once (SBatchWant)
    // MSC_VGX_DLREFRESH: an entry of the dynamic store (g_dyn): the parts it was decoded from are hashed (see DynRefresh)
    uint64_t hashList = 0, hashSkin = 0, hashArr[12] = {};   // XXH3 of the list bytes, of the skin arrays, of each array range the decode read
    bool evictMark = false;        // MSC_DL_KEEP: chosen for eviction by the current Sweep
    uint8_t sbWhy = 0;             // MSC_VGX_SBATCH diagnostics: why the copy is not kept (1 hash mode, 2 palette keep, 3 off, 4 skin, 5 matrix index, 6 cap, 7 trimmed idle, 8 trimmed unnoted)
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
// Entries age by frames that call cacheable lists, not by wall frames, so a
// loading screen, pause or movie does not throw the match's lists away.
uint64_t g_useClock = 0, g_useClockFrame = ~0ull;
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
int g_flushOn = -1; // MSC_VGX_DLFLUSH_STADIUM (see FlushStep)
bool g_static = true;   // MSC_VGX_STATICBUF=0: never promote (decoded vertices are copied into the arena every time)
bool g_keepCopy = false; // MSC_VGX_HASH=2: keep the bytes of promoted entries for hashing
// MSC_VGX_PROMOSMALL=<n> (default 0 = off): a list with draws of at most MSC_VGX_SOFTXFORM vertices is normally never promoted, because such draws may be
// transformed on the CPU and merged with their neighbours. Those the front end did NOT transform (palette-matrix and model-space texgen draws) stay in the
// stream every frame and are not merged more than any other draw: after n consecutive replays without a CPU-transformed draw the list gets a static
// buffer like a big one. Never undone (a promoted list is drawn on the GPU path whatever its state turns out to be).
uint32_t g_promoSmall = 0;
// MSC_VGX_PROMOSOLO=<n> (default 0 = off): the only thing a CPU transform buys is a merge with the draw before or after; a rigid mesh that merges with nothing
// costs a draw record all the same, plus the vertex copy, the transform and the index generation of every picture. After n consecutive replays in which a
// one-primitive list's CPU-transformed draw joined nothing it gets a static buffer like a big one, so the limit (MSC_VGX_SOFTXFORM) stays high for what does merge
// (crowd rows, the menus' instanced meshes: a batch counts as a merge) without a size having to tell them apart. Every list of the game is one primitive
// (glxDisplayList.cpp dlMakeDisplayList). As with PROMOSMALL it is never undone, and pixels move at most by the same rounding as a change of the limit.
// At most kSoloPerFrame such promotions per frame: the upload of a whole stadium's small meshes in one picture is what MSC_VGX_SOFTXFORM=0 did to the Galaxy port.
uint32_t g_promoSolo = 0;
constexpr uint32_t kSoloPerFrame = 16;
uint32_t g_soloLeft = 0;
uint32_t g_sbRekeepLeft = 0; // MSC_VGX_SBATCH: re-decodes left in this frame (SBatchWant)
Entry* g_soloOpen = nullptr; // the list whose draw made the newest record, until a draw joins that record or another record is made
// MSC_VGX_DL_TINY=<bytes> (default 15 = the 1.2 test, off): lists up to this size go straight to the parser, no cache entry. The Galaxy port's default is 64: a list of
// a few dozen bytes is decoded faster than its entry is checked. A list's size is 3 + 2 x streams x vertices rounded up to 32, so 64 is at most 30 vertices here.
uint32_t g_tinyBytes = 15;
struct Counters {
    uint64_t hits = 0, builds = 0, rehashOk = 0, rehashChanged = 0, bypassed = 0, uncacheable = 0, staleNoNotify = 0, evicted = 0;
    uint64_t flushed = 0;
    uint64_t keepSaved = 0;    // 1.3 MSC_DL_KEEP: lists found again that the 600-frame rule would have freed (each one is a list not decoded again)
    uint64_t hitVerts = 0, builtVerts = 0, liveBytes = 0, staticBytes = 0, promoted = 0, staticBufs = 0, promotedSmall = 0;
    uint64_t boundsRefused = 0; // 1.2: builds refused because an index lay outside its array (MSC_VGX_DLCACHE_BOUNDS_GUARD)
    uint64_t promotedSolo = 0;  // 1.3: of the promotions, those of MSC_VGX_PROMOSOLO
    uint64_t skinKeptBytes = 0; // 1.3 MSC_VGX_SKINMERGE: decoded vertices kept after promotion (GPU-skinned lists)
    uint64_t palKeptBytes = 0;  // 1.3 MSC_VGX_PALMERGE: decoded vertices kept after promotion (lists with a matrix index per vertex)
    uint64_t sbKeptBytes = 0;   // 1.3 MSC_VGX_SBATCH: decoded vertices kept after promotion (rigid background lists)
    uint64_t sbRekept = 0;      // 1.3 MSC_VGX_SBATCH: lists decoded again to keep a copy
    uint64_t refreshed = 0;     // MSC_VGX_DLREFRESH: calls that decoded only the arrays that had changed
    uint64_t refreshSame = 0, refreshKept = 0, refreshGaveUp = 0, refreshAttrVerts = 0;   // calls of a kept list that found nothing changed; lists kept; calls that fell back to the bypass; attribute x vertices decoded again
} g_ctr;

// Static buffer ids: small, reused, owner known (for MSC_VGX_HASH=2).
std::vector<uint32_t> g_freeIds;
std::vector<Entry*> g_bufOwner(1, nullptr);
// 1.3 MSC_VGX_PALMERGE: per id, a serial that changes whenever the id is given out (a combined buffer names its members by id and serial, so a
// reused id is never taken for the buffer it used to be), and the frame it was last freed in (AllocStaticBufferIdFresh).
std::vector<uint32_t> g_bufSerial(1, 0);
std::vector<uint64_t> g_bufFreedAt(1, ~0ull);
uint32_t g_serialNext = 0;
uint32_t AllocBufId(Entry* e) {
    uint32_t id;
    if (!g_freeIds.empty()) { id = g_freeIds.back(); g_freeIds.pop_back(); }
    else { id = static_cast<uint32_t>(g_bufOwner.size()); g_bufOwner.push_back(nullptr); g_bufSerial.push_back(0); g_bufFreedAt.push_back(~0ull); }
    g_bufOwner[id] = e;
    g_bufSerial[id] = ++g_serialNext;
    return id;
}
void ReleaseBuffer(Entry& e) {
    if (!e.bufId) return;
    PalMergeForget(e.bufId); // (MSC_VGX_PALMERGE: combined buffers made from this one go with it)
    SBatchForget(e.bufId);   // (MSC_VGX_SBATCH: likewise the static batches that hold its geometry)
    if (e.palKept) { (e.skinCtr ? g_ctr.skinKeptBytes : g_ctr.palKeptBytes) -= e.verts.capacity(); e.palKept = false; e.skinCtr = false; }
    if (e.sbKept) { g_ctr.sbKeptBytes -= e.verts.capacity(); e.sbKept = false; }
    Pending().cmds.push_back(Cmd{CMD_BUF_FREE, e.bufId});
    g_bufOwner[e.bufId] = nullptr;
    g_bufFreedAt[e.bufId] = CurrentFrame();
    g_freeIds.push_back(e.bufId);
    g_ctr.staticBytes -= e.bufBytes;
    --g_ctr.staticBufs;
    e.bufId = 0;
    e.bufBytes = 0;
    e.welded = false;
}
// MSC_VGX_DLWELD=0|1 (code default 0, Vita plan 0): see vgx_dlweld.h
bool DlWeldOn() {
    static const bool on = static_cast<bool>(VGX_KNOB(EnvInt("MSC_VGX_DLWELD", 0) != 0));
    return on;
}
void DlWeldLog() {
    const dlweld::Totals& t = dlweld::g_t;
    std::fprintf(stderr, "[dlweld] %llu lists welded (%llu groups), vertices %llu -> %llu (%.1f%%), %.1f KiB saved, %.2f ms spent welding, buffer sum %016llx\n", (unsigned long long)t.lists, (unsigned long long)t.groups,
                 (unsigned long long)t.vertsIn, (unsigned long long)t.vertsOut, 100.0 * (double)t.vertsOut / (double)(t.vertsIn ? t.vertsIn : 1), t.bytesSaved / 1024.0, t.micros / 1000.0, (unsigned long long)t.sig);
}
// nxfly25: MSC_VGX_DLWELD_BUDGET_US=<us> / MSC_VGX_DLWELD_LISTS=<n> (code default 3000 us a picture, ON when MSC_VGX_DLWELD=1; 0 = no limit): per picture, no promotion (weld) starts once the picture has spent that many
// microseconds welding / welded that many lists (vgx_dlweldbudget.h). Timing only: the lists left over are promoted at a later picture, the welded buffers are the same ([dlweld] "buffer sum").
// The weld runs inline in the first repeat call of a list, i.e. on the thread that runs the front end (the record player), in whatever picture a list is called a second time: the first
// pictures of a stadium's fly-in. [dlweld] picture lines name the pictures that spent 4 ms or more welding (or put promotions off), at most 60 lines.
dlweldbudget::Budget g_wb;
bool g_wbInit = false;
void DlWeldPictureLine() {
    static int logged = 0;
    if ((g_wb.usedUs >= 4000 || g_wb.deferredPic) && ++logged <= 60)
        std::fprintf(stderr, "[dlweld] picture %llu: %u lists welded in %.1f ms (limit %u us / %u lists), %u promotions put off to a later picture (%llu in all)\n", (unsigned long long)g_wb.picture, g_wb.lists,
                     g_wb.usedUs / 1000.0, g_wb.budgetUs, g_wb.maxLists, g_wb.deferredPic, (unsigned long long)g_wb.deferred);
}
void DlWeldInit() {
    if (g_wbInit) return;
    g_wbInit = true;
    const long us = EnvInt("MSC_VGX_DLWELD_BUDGET_US", 3000), n = EnvInt("MSC_VGX_DLWELD_LISTS", 0);
    g_wb.budgetUs = us > 0 ? static_cast<uint32_t>(us) : 0u;
    g_wb.maxLists = n > 0 ? static_cast<uint32_t>(n) : 0u;
    if ((g_wb.budgetUs || g_wb.maxLists) && DlWeldOn())
        std::fprintf(stderr, "[dlweld] MSC_VGX_DLWELD_BUDGET_US=%u MSC_VGX_DLWELD_LISTS=%u: a picture starts no weld after that much time / that many lists; the rest wait for a later picture\n", g_wb.budgetUs, g_wb.maxLists);
}
// The first list call of a picture: the previous picture's line, then a fresh count.
void DlWeldNewPicture(uint64_t frame) {
    DlWeldInit();
    if (g_wb.picture != ~0ull && g_wb.picture != frame) DlWeldPictureLine();
    g_wb.NewPicture(frame);
}
// May a promotion start now? (Applies to every promotion while the weld is on: a promotion that turns out not to weld costs no weld time and is not counted.)
bool DlWeldMayPromote() {
    if (!DlWeldOn() || g_keepCopy || g_wb.Allow()) return true;
    g_wb.Deferred();
    return false;
}
// Moves the entry's geometry into a static buffer of the back end.
void Promote(Entry& e) {
    // With absolute indices every primitive of the buffer is addressed from the buffer's
    // start: that needs one stride for all of them and 16 bits to be enough. (Lists with
    // more than one primitive are rare in this game; one that does not qualify stays in
    // the per-frame arena.)
    const bool abs = AbsoluteIndices();
    const uint32_t stride = MakeLayout(e.segs[0].layoutMask).stride;
    if (abs && stride == 0) { e.promotable = false; e.smallOk = false; return; }   // (a layout without vertex data: no start index to divide out)
    if (abs) {
        bool ok = e.totalVerts <= 0x10000;
        for (const Seg& sg : e.segs) ok = ok && MakeLayout(sg.layoutMask).stride == stride;
        if (!ok) { e.promotable = false; e.smallOk = false; return; }
    }
    BufUpload up;
    up.bufId = e.bufId = AllocBufId(&e);
    g_ctr.liveBytes -= e.verts.capacity();
    // MSC_VGX_PALMERGE: a list whose vertices name their position matrix (a skinned mesh drawn with the matrix palette) keeps its decoded vertices,
    // from which palette runs are gathered into combined buffers (vgx_draw.cpp PmClose)
    bool palKeep = !g_keepCopy && PalMergeMode() != 0 && g.vcd[0] != 0 && g_ctr.palKeptBytes + e.verts.capacity() <= PalMergeCapBytes();
    // MSC_VGX_SKINMERGE: a GPU-skinned list keeps them too (runs of skinned draws that differ in the bone block), counted apart
    const bool skinKeep = !palKeep && !g_keepCopy && e.skin && SkinMergeOn() && g_ctr.skinKeptBytes + e.verts.capacity() <= SkinMergeCapBytes();
    palKeep = palKeep || skinKeep;
    // MSC_VGX_SBATCH: a rigid list (no matrix index per vertex, not skinned) drawn in a background class keeps them for static batches
    bool sbKeep = !g_keepCopy && !palKeep && SBatchKeepWanted() && !e.skin;
    e.sbWhy = g_keepCopy ? 1 : palKeep ? 2 : !SBatchKeepWanted() ? 3 : e.skin ? 4 : 0;
    for (int i = 0; i <= 8 && sbKeep; ++i) sbKeep = g.vcd[i] == 0;
    if (!e.sbWhy && !sbKeep) e.sbWhy = 5;
    if (sbKeep && g_ctr.sbKeptBytes + e.verts.capacity() > SBatchKeepCapBytes()) SBatchTrimKept(0); // (full: the copies of lists never seen as a candidate go)
    if (sbKeep && g_ctr.sbKeptBytes + e.verts.capacity() > SBatchKeepCapBytes()) { sbKeep = false; e.sbWhy = 6; }
    // MSC_VGX_DLWELD: not for a list that keeps its decoded copy (those consumers read e.verts + sg.offset)
    const bool weld = DlWeldOn() && !g_keepCopy && !palKeep && !sbKeep;
    const uint64_t weldT0 = weld ? dlweld::NowUs() : 0;
    static dlweld::Welder welder;
    static std::vector<uint16_t> slotOf;
    uint64_t weldOutVerts = 0;
    size_t weldOutPos = 0;
    uint8_t* weldOutData = nullptr;
    if (weld) {
        welder.Init(e.totalVerts);
        up.vtx.resize(e.verts.size());
        weldOutData = up.vtx.data();
    }
    size_t block = 0;      // (welding) where the group's block of distinct vertices starts in the new buffer, bytes
    uint32_t base = 0;     // (welding) the number its first vertex has (absolute indices: from the buffer's start; relative: 0)
    uint32_t groupRaw = 0; // (welding) vertices of the group before welding (groups are cut so that this stays below 0x10000)
    uint32_t groupMask = 0;
    bool haveGroup = false;
    for (Seg& sg : e.segs) {
        sg.idxOffset = static_cast<uint32_t>(up.idx.size());
        if (weld) {
            const uint32_t segStride = MakeLayout(sg.layoutMask).stride;
            if (!haveGroup || sg.layoutMask != groupMask || groupRaw + sg.count > 0xFFFF) {
                haveGroup = true;
                groupMask = sg.layoutMask;
                groupRaw = 0;
                block = weldOutPos;
                base = abs ? static_cast<uint32_t>(block / stride) : 0u;
                welder.NewGroup();
                ++dlweld::g_t.groups;
            }
            sg.wOff = static_cast<uint32_t>(block);
            sg.vtxFirst = base;
            const uint32_t n = PrimIndices(sg.prim, sg.count, 0, nullptr);
            up.idx.resize(up.idx.size() + n);
            uint16_t* const ix = up.idx.data() + sg.idxOffset;
            PrimIndices(sg.prim, sg.count, 0, ix);
            slotOf.resize(sg.count);
            const uint8_t* v = e.verts.data() + sg.offset;
            const uint32_t beforeSlots = welder.nextSlot;
            for (uint32_t i = 0; i < sg.count; ++i, v += segStride) slotOf[i] = static_cast<uint16_t>(base + welder.Find(weldOutData, weldOutPos, block, v, segStride));
            weldOutVerts += welder.nextSlot - beforeSlots;
            for (uint32_t i = 0; i < n; ++i) ix[i] = slotOf[ix[i]];
#ifndef __VGX_VITA__
            static const long brk = EnvInt("MSC_VGX_DLWELD_BREAK", 0); // host mutant: the remap of one index of every brk-th list is wrong
            if (brk > 0 && n > 2 && static_cast<long>(dlweld::g_t.lists + 1) % brk == 0 && &sg == &e.segs[0]) ix[n - 1] = ix[0] == ix[n - 1] ? static_cast<uint16_t>(ix[0] + 1) : ix[0];
#endif
            groupRaw += sg.count;
        } else {
            sg.vtxFirst = abs ? sg.offset / stride : 0;
            const uint32_t n = PrimIndices(sg.prim, sg.count, 0, nullptr);
            up.idx.resize(up.idx.size() + n);
            PrimIndices(sg.prim, sg.count, sg.vtxFirst, up.idx.data() + sg.idxOffset);
        }
    }
    if (weld) {
        up.vtx.resize(weldOutPos);
        e.welded = true;
        dlweld::Totals& t = dlweld::g_t;
        t.vertsIn += e.totalVerts;
        t.vertsOut += weldOutVerts;
        t.bytesSaved += e.verts.size() - up.vtx.size();
        ++t.lists;
#if defined(__VGX_VITA__)
        // int28: skip ~10+ MiB XXH3 diagnostic buffer sum on Vita production unless explicitly requested by MSC_VGX_DLWELD_SIG=1.
        static const bool sigOn = EnvInt("MSC_VGX_DLWELD_SIG", 0) != 0;
        if (sigOn) t.sig += XXH3_64bits(up.vtx.data(), up.vtx.size()) * 3u + XXH3_64bits(up.idx.data(), up.idx.size() * 2u);
#else
        // nxr6a: the buffer sum (the A/B proof that a budget changes no buffer: [dlweld] line) is hashed INSIDE the timed span, so the budget and the "ms spent welding" count it
        t.sig += XXH3_64bits(up.vtx.data(), up.vtx.size()) * 3u + XXH3_64bits(up.idx.data(), up.idx.size() * 2u);
#endif
        const uint32_t weldUs = static_cast<uint32_t>(dlweld::NowUs() - weldT0); // (32-bit wrap safe on the console's timer)
        t.micros += weldUs;
        g_wb.Spent(weldUs);
        if ((t.lists & 255u) == 1u) DlWeldLog();
    }
    e.bufBytes = static_cast<uint32_t>((weld ? up.vtx.size() : e.verts.size()) + up.idx.size() * 2);
    if (weld) {
        e.verts = std::vector<uint8_t>();
    } else if (g_keepCopy || palKeep || sbKeep) {
        up.vtx = e.verts;
        if (g_keepCopy) e.idx = up.idx;
        g_ctr.liveBytes += e.verts.capacity();
        if (skinKeep) { e.palKept = true; e.skinCtr = true; g_ctr.skinKeptBytes += e.verts.capacity(); }
        else if (palKeep) { e.palKept = true; g_ctr.palKeptBytes += e.verts.capacity(); }
        if (sbKeep) { e.sbKept = true; e.sbNoted = false; e.sbKeptAt = g_useClock; g_ctr.sbKeptBytes += e.verts.capacity(); }
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

// MSC_VGX_BUILDFAST=0|1 (code default 0; on in the Vita's list): the cost of building an entry (the first call of a list): the buffer reserved once, the box of the
// positions by NEON. The decoded bytes are the same; the box differs only for a denormal extreme (read as 0).
bool BuildFast() {
    static int on = -1;
    if (on < 0) on = static_cast<bool>(VGX_KNOB(EnvInt("MSC_VGX_BUILDFAST", 0) != 0)) ? 1 : 0;
    return on != 0;
}

void Init() {
    if (g_mode >= 0) return;
    const char* m = std::getenv("MSC_VGX_DLCACHE");
    if (m && !std::strcmp(m, "verify")) g_mode = 2;
    else g_mode = (m && *m == '0') ? 0 : 1;
    g_static = EnvInt("MSC_VGX_STATICBUF", 1) != 0;
    g_keepCopy = EnvInt("MSC_VGX_HASH", 0) >= 2;
    g_promoSmall = static_cast<uint32_t>(EnvInt("MSC_VGX_PROMOSMALL", 0));
    g_promoSolo = static_cast<uint32_t>(EnvInt("MSC_VGX_PROMOSOLO", 0));
    if (g_promoSolo > 255) g_promoSolo = 255; // (the count is a byte)
    {
        const long tiny = EnvInt("MSC_VGX_DL_TINY", 15);
        g_tinyBytes = tiny < 15 ? 15u : static_cast<uint32_t>(tiny);
    }
    g_flushOn = EnvInt("MSC_VGX_DLFLUSH_STADIUM", 1) != 0 ? 1 : 0;
}

// The box of a segment's positions (decoded vertices, position first). Not for skinned segments (the stored positions are the bind pose) or when a position is
// not finite or absurdly large.
bool BuildFast(); // MSC_VGX_BUILDFAST
void SegBounds(Seg& s, const uint8_t* v) {
    s.boundsOk = false;
    if (!CullBoundsWanted()) return;
    const uint32_t stride = MakeLayout(s.layoutMask).stride;
    float lo[3], hi[3];
    for (int a = 0; a < 3; ++a) { lo[a] = 3.0e38f; hi[a] = -3.0e38f; }
    uint32_t i0 = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    if (BuildFast() && s.count >= 4 && stride >= 16) {
        // MSC_VGX_BUILDFAST: the same box with one load, one minimum and one maximum per vertex (the position is the first three words of the
        // 16 the layout reserves, so the load stays inside the vertex; lane 3 is the matrix word and is not read out). A NaN gives a NaN lane (the
        // test below fails), an infinity or an absurd value a lane outside the limits: the vertex that makes the scalar loop give up makes this one give up.
        // (The one difference: the NEON minimum and maximum flush a denormal to zero, so a box whose extreme is a denormal reads 0 there; the box
        // only feeds culling tests with a margin of pixels.)
        float32x4_t vlo = vdupq_n_f32(3.0e38f), vhi = vdupq_n_f32(-3.0e38f);
        const uint8_t* q = v;
        for (uint32_t i = 0; i < s.count; ++i, q += stride) {
            const float32x4_t p = vld1q_f32(reinterpret_cast<const float*>(q));
            vlo = vminq_f32(vlo, p);
            vhi = vmaxq_f32(vhi, p);
        }
        float l4[4], h4[4];
        vst1q_f32(l4, vlo);
        vst1q_f32(h4, vhi);
        for (int a = 0; a < 3; ++a) {
            if (!(l4[a] > -1.0e18f && h4[a] < 1.0e18f && l4[a] < 1.0e18f && h4[a] > -1.0e18f)) return;
            s.bounds[a] = l4[a];
            s.bounds[3 + a] = h4[a];
        }
        s.boundsOk = true;
        return;
    }
#endif
    for (uint32_t i = i0; i < s.count; ++i) {
        float p[3];
        std::memcpy(p, v + static_cast<size_t>(i) * stride, 12);
        for (int a = 0; a < 3; ++a) {
            if (!(p[a] > -1.0e18f && p[a] < 1.0e18f)) return; // (NaN, infinity, absurd)
            if (p[a] < lo[a]) lo[a] = p[a];
            if (p[a] > hi[a]) hi[a] = p[a];
        }
    }
    for (int a = 0; a < 3; ++a) { s.bounds[a] = lo[a]; s.bounds[3 + a] = hi[a]; }
    s.boundsOk = true;
}

// Walks a list that consists of draw commands (and padding) only. Fills
// e.segs / e.verts / spans; false if the list holds anything else.
bool Build(Entry& e, const uint8_t* p, uint32_t nbytes) {
    const uint8_t* const end = p + nbytes;
    ReleaseBuffer(e);
    e.segs.clear();
    e.verts.clear();
    e.welded = false;
    e.idx.clear();
    e.totalVerts = 0;
    e.promotable = true;
    e.smallOk = true;
    e.calm = 0;
    e.fmtMask = 0;
    e.idxMask = 0;
    for (int i = 0; i < 12; ++i) { e.lo[i] = 0xFFFFFFFFu; e.hi[i] = 0; }
    if (BuildFast()) {
        // MSC_VGX_BUILDFAST: the vertex buffer's final size is known from the commands: one allocation, no copy of what was decoded when a primitive grows it
        // (a list of a dozen primitives was copied a dozen times).
        size_t total = 0;
        for (const uint8_t* q = p; q < end;) {
            const uint8_t cmd = *q++;
            if (cmd == 0x00) continue;
            if (!(cmd & 0x80) || end - q < 2) break;
            const uint32_t count = be16(q);
            q += 2;
            const Plan& plan = CurrentPlan(cmd & 7);
            if (plan.streamSize == 0) break;
            const uint64_t used = static_cast<uint64_t>(count) * plan.streamSize;
            if (used > static_cast<uint64_t>(end - q)) break;
            total += static_cast<size_t>(count) * MakeLayout((e.skin && SkinPosIndexed(plan)) ? (plan.layoutMask | VL_SKIN) : plan.layoutMask).stride;
            q += used;
        }
        if (total) e.verts.reserve(total);
    }
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
            s.srcOff = static_cast<uint32_t>(p - e.list);   // (e.list is the list being built: set by the caller)
            s.idxOffset = 0;
            s.vtxFirst = 0;
            if (count <= SoftXformLimit() || count > 0xFFFF) e.promotable = false;
            if (count > 0xFFFF) e.smallOk = false;
            const bool sk = e.skin && SkinPosIndexed(plan); // (W5 SKIN)
            if (e.skin && !sk) SkinNoteMissed(count);
            if (sk) s.layoutMask |= VL_SKIN;
            e.verts.resize(e.verts.size() + static_cast<size_t>(count) * MakeLayout(s.layoutMask).stride);
            ArraySpans sp;
            DecodeVertices(plan, p, count, e.verts.data() + s.offset, &sp, sk && SkinDirect() ? MakeLayout(s.layoutMask).stride : 0);
            // 1.2 (Galaxy GAL-091): an index outside its array decoded as zeros, and the spans (what the entry's identity and hash cover) leave that
            // element out: a later, valid binding of the array would keep drawing the cached zeros. Such a list is not cached (MSC_VGX_DLCACHE_BOUNDS_GUARD=0: as before).
            const bool boundsGuard = static_cast<bool>(VGX_KNOB(EnvInt("MSC_VGX_DLCACHE_BOUNDS_GUARD", 1) != 0));
            if (boundsGuard && sp.incomplete) { ++g_ctr.boundsRefused; return false; }
            if (sk) { if (SkinDirect()) SkinFill(plan, p, count, e.verts.data() + s.offset); else SkinExpand(plan, p, count, e.verts.data() + s.offset); }
            s.posMtxMax = LastDecodedPosMtxMax();
            if (!sk && g.vcd[0] != 0 && PalCompactOn()) { // (the matrices the vertices name: the w of each position, a whole number)
                const uint32_t stride = MakeLayout(s.layoutMask).stride;
                const uint8_t* q = e.verts.data() + s.offset + 12;
                uint32_t mk = 0;
                for (uint32_t v = 0; v < count && mk != ~0u; ++v, q += stride) {
                    float f;
                    std::memcpy(&f, q, 4);
                    const int r = static_cast<int>(f);
                    if (static_cast<float>(r) != f || r < 0 || r >= 30 || r % 3 != 0) mk = ~0u;
                    else mk |= 1u << (r / 3);
                }
                s.posMtxMask = mk == ~0u ? 0u : mk;
            }
            if (!sk) SegBounds(s, e.verts.data() + s.offset);
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

// MSC_VGX_DLWATCH (0 off, 1 default on, 2 verify): see GuestWatchEpoch (vgx_guestmem.h). The pages of the list, of the array ranges the decode read and of the
// skin arrays are watched once; a hit whose generations were found unchanged at the current watch epoch is valid without reading them again.
int DlWatchMode() {
    const int m = static_cast<int>(VGX_KNOB([] { const long v = EnvInt("MSC_VGX_DLWATCH", 1); return (v < 0 || v > 2) ? 1 : static_cast<int>(v); }()));
    return m;
}
uint64_t g_watchFast = 0, g_watchSlow = 0, g_watchChecked = 0, g_watchBad = 0;
// gem2b MSC_VGX_DLWATCH_BUCKETS=0|1 (default 1; =0 is the single global watch epoch): the stamp an entry's "found valid" is tied to is the sum of the counters of the 64 KiB buckets its
// watched pages are in, not the one epoch of every watched write.
bool DlWatchBuckets() {
    static const bool on = EnvInt("MSC_VGX_DLWATCH_BUCKETS", 1) != 0;
    return on;
}
inline uint64_t WatchStamp(const Entry& e) { return DlWatchBuckets() ? GuestWatchSum(e.watchMask) : GuestWatchEpoch(); }
void WatchEntry(Entry& e) {
    uint64_t m = WatchRangeMask(e.list, e.nbytes);
    for (int a = 9; a <= 20; ++a)
        if (e.idxMask & (1u << a)) m |= WatchRangeMask(e.arr[a - 9] + e.lo[a - 9], e.hi[a - 9] - e.lo[a - 9]);
    if (e.skin) {
        m |= WatchRangeMask(e.skinW, static_cast<uint32_t>(e.skinVerts) * 16u);
        m |= WatchRangeMask(e.skinI, static_cast<uint32_t>(e.skinVerts) * 4u);
    }
    e.watchMask = m;
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

// ---- MSC_VGX_DLREFRESH=0|1|2 (code default 0, Vita plan 1; =0: as before; =2: verify) -----------------------------------------------------------------------------------
// A character mesh whose position array the game writes again before every draw (the walk-out at Vice: 8-11 lists a picture of 736..46464 bytes, MSC_VGX_BYPASSLOG) fails the
// content hash three calls in a row and is then left to the general parser at every call for 120 frames and more (the bypass window, back-off doubling to 1920): every
// attribute of every vertex, the skin attributes included, decoded again for a change in one array. The WINDOW is unchanged by this switch (same start, same length, same
// lists in it, same static buffers of the other variants of the list: only the lists that are drawn in it differ in how their vertices are made). With the switch on a call
// inside the window finds (DynCall) the list's entry of the dynamic store g_dyn (same descriptor, formats, array addresses and strides, skin arrays: SameState) or decodes the
// list into one (Build, what the parser's decode gives), and draws the entry's vertices (EmitDyn: the arena copy and EmitDraw that DrawStream does). A later call whose
// generations moved hashes the parts whose generation moved: when the list bytes and the skin arrays are what they were, the attributes of the arrays that differ are decoded
// again over the kept vertices by the same decoder at the same stride (DecodeVertices onlyAttrs) and nothing else of a vertex is touched; the vertices are then what a full
// decode gives (host test tests/dlrefresh.cpp; =2 compares every call with a full decode). Any other change (list bytes: the index data; the skin arrays; more than half
// of the attributes changed; an index outside its array) gives the entry up and the call is the parser's. The entries are freed when the window ends.
long RefreshMode() {   // 0 off, 1 on, 2 verify (every refreshed or unchanged call is checked against a full decode, mismatches counted and logged)
    static const long m = static_cast<long>(VGX_KNOB(EnvInt("MSC_VGX_DLREFRESH", 0)));
    return m;
}
bool RefreshOn() { return RefreshMode() != 0; }
uint64_t g_refreshBad = 0, g_refreshChecked = 0;
// (MSC_VGX_DLREFRESH=2) the kept vertices against a fresh full decode of the same list at the same strides
void RefreshVerify(const Entry& e, const char* what, uint32_t changed) {
    std::vector<uint8_t> tmp;
    for (const Seg& sg : e.segs) {
        const Plan& plan = CurrentPlan(sg.fmt);
        const bool sk = (sg.layoutMask & VL_SKIN) != 0;
        const uint32_t stride = MakeLayout(sg.layoutMask).stride;
        tmp.assign(static_cast<size_t>(sg.count) * stride, 0);
        DecodeVertices(plan, e.list + sg.srcOff, sg.count, tmp.data(), nullptr, sk && SkinDirect() ? stride : 0);
        if (sk) { if (SkinDirect()) SkinFill(plan, e.list + sg.srcOff, sg.count, tmp.data()); else SkinExpand(plan, e.list + sg.srcOff, sg.count, tmp.data()); }
        ++g_refreshChecked;
        const uint8_t* kept = e.verts.data() + sg.offset;
        if (std::memcmp(tmp.data(), kept, tmp.size()) != 0) {
            ++g_refreshBad;
            size_t at = 0;
            while (tmp[at] == kept[at]) ++at;
            VGX_LOG_N(40, "dl refresh VERIFY: %s: list %p (%u bytes) seg fmt %u count %u stride %u layout %x: first differing byte %lu (vertex %lu, byte %lu of it), changed arrays %x\n",
                      what, (const void*)e.list, e.nbytes, sg.fmt, sg.count, stride, sg.layoutMask, (unsigned long)at, (unsigned long)(at / stride), (unsigned long)(at % stride), changed);
        }
    }
}
inline uint64_t SkinHash(const Entry& e) {
    return XXH3_64bits_withSeed(e.skinI, static_cast<size_t>(e.skinVerts) * 4u, XXH3_64bits(e.skinW, static_cast<size_t>(e.skinVerts) * 16u));
}
void PartHashes(Entry& e) {
    e.hashList = XXH3_64bits(e.list, e.nbytes);
    e.hashSkin = e.skin ? SkinHash(e) : 0;
    for (int a = 9; a <= 20; ++a)
        if (e.idxMask & (1u << a)) e.hashArr[a - 9] = XXH3_64bits(e.arr[a - 9] + e.lo[a - 9], e.hi[a - 9] - e.lo[a - 9]);
}
void SegBounds(Seg& s, const uint8_t* v);
// A kept entry whose generations moved (gl, ga: as read now). 0: not refreshable (`why` says it; the caller takes the path of before), 1: every part is what it was,
// 2: the arrays that differ were decoded again.
int DynRefresh(Entry& e, uint64_t gl, const uint64_t ga[12], const char*& why) {
    if (e.bufId || e.verts.empty() || e.segs.empty()) { why = "no decoded vertices kept"; return 0; }
    if (gl != e.genList) {
        if (XXH3_64bits(e.list, e.nbytes) != e.hashList) { why = "list bytes changed"; return 0; }
        if (e.skin && SkinHash(e) != e.hashSkin) { why = "skin arrays changed"; return 0; }
    }
    uint32_t changed = 0, nChanged = 0;
    uint64_t h[12];
    for (int a = 9; a <= 20; ++a)
        if ((e.idxMask & (1u << a)) && ga[a - 9] != e.genArr[a - 9]) {
            h[a - 9] = XXH3_64bits(e.arr[a - 9] + e.lo[a - 9], e.hi[a - 9] - e.lo[a - 9]);
            if (h[a - 9] != e.hashArr[a - 9]) { changed |= 1u << a; ++nChanged; }
        }
    if (!changed) { if (RefreshMode() == 2) RefreshVerify(e, "found unchanged", 0); return 1; }
    // every primitive is still decodable in place: the plan of its format gives the layout it was decoded to and its stream lies inside the list
    uint32_t minAttrs = ~0u;
    for (const Seg& sg : e.segs) {
        const Plan& plan = CurrentPlan(sg.fmt);
        const bool sk = (sg.layoutMask & VL_SKIN) != 0;
        if (plan.streamSize == 0 || (sk ? (plan.layoutMask | VL_SKIN) : plan.layoutMask) != sg.layoutMask || (sk && !e.skin) ||
            static_cast<uint64_t>(sg.srcOff) + static_cast<uint64_t>(sg.count) * plan.streamSize > e.nbytes ||
            static_cast<uint64_t>(sg.offset) + static_cast<uint64_t>(sg.count) * MakeLayout(sg.layoutMask).stride > e.verts.size()) { why = "layout no longer the decoded one"; return 0; }
        const uint32_t n = static_cast<uint32_t>(plan.n) + (sk ? 1u : 0u);
        if (n < minAttrs) minAttrs = n;
    }
    if (nChanged * 2 > minAttrs) { why = "more than half of the attributes changed"; return 0; }
    for (Seg& sg : e.segs) {
        const Plan& plan = CurrentPlan(sg.fmt);
        const uint32_t stride = MakeLayout(sg.layoutMask).stride;
        uint32_t count = sg.count;
#ifndef __VGX_VITA__
        static const long brk = EnvInt("MSC_VGX_DLREFRESH_BREAK", 0); // host mutant: the last vertex of every primitive is not decoded again
        if (brk == 1 && count) --count;
#endif
        {
            workload::FeScope feDecode(workload::FeDecode);
            DecodeVertices(plan, e.list + sg.srcOff, count, e.verts.data() + sg.offset, nullptr, stride, changed);
        }
        g_ctr.refreshAttrVerts += static_cast<uint64_t>(nChanged) * sg.count;
        // what a build derives from the decoded vertices (the matrix rows come from the stream: as they were)
        if ((changed & (1u << 9)) && !(sg.layoutMask & VL_SKIN)) SegBounds(sg, e.verts.data() + sg.offset);
        if (SoftInstOn() && sg.count <= SoftXformLimit() && !(sg.layoutMask & VL_SKIN))
            sg.vhash = XXH3_64bits(e.verts.data() + sg.offset, static_cast<size_t>(sg.count) * stride);
    }
    for (int a = 9; a <= 20; ++a)
        if (changed & (1u << a)) e.hashArr[a - 9] = h[a - 9];
    if (RefreshMode() == 2) RefreshVerify(e, "refreshed", changed);
    return 2;
}
// A kept entry's draw: what the bypass did for the list (ProcessStream / DrawStream) without the decode.
void EmitDyn(const Entry& e, uint32_t nbytes) {
    stats::Frame& fr = stats::Cur();
    ++fr.streams;
    fr.streamBytes += nbytes;
    for (const Seg& s : e.segs) {
        uint8_t* out = BeginVertices(s.layoutMask, s.count);
        if (out) {
            std::memcpy(out, e.verts.data() + s.offset, static_cast<size_t>(s.count) * MakeLayout(s.layoutMask).stride);
            SetPendingPosMtxMax(s.posMtxMax);
            EmitDraw(s.prim, s.layoutMask, s.count);
        }
        ++fr.primsDL;
        fr.vertsDL += s.count;
    }
}

void Free(Entry* e);
// The dynamic store: the entries of the lists inside a bypass window (MSC_VGX_DLREFRESH). Freed when the window of the list ends (DynDrop at the first call after it, DynSweep).
struct DynSet {
    std::vector<Entry*> v;
    uint64_t noBuildUntil = 0;   // frame: a build of the list failed (state inside, an index outside its array): the parser has it until then
};
std::unordered_map<const uint8_t*, DynSet> g_dyn;
constexpr size_t kDynVariants = 4;
constexpr size_t kDynMaxBytes = 8u << 20;   // decoded bytes of the whole store
size_t g_dynBytes = 0;
void DynDrop(const uint8_t* list) {
    auto it = g_dyn.find(list);
    if (it == g_dyn.end()) return;
    for (Entry* e : it->second.v) { g_dynBytes -= e->verts.capacity(); Free(e); }
    g_dyn.erase(it);
}
void DynSweep(uint64_t frame) {
    for (auto it = g_dyn.begin(); it != g_dyn.end();) {
        auto b = g_bypass.find(it->first);
        if (b == g_bypass.end() || b->second.until <= frame) {
            for (Entry* e : it->second.v) { g_dynBytes -= e->verts.capacity(); Free(e); }
            it = g_dyn.erase(it);
        } else ++it;
    }
}
HostRange DynRaceRanges(const Entry& e, HostRange used[15], int& n) {
    n = 0;
    used[n++] = {e.list, e.nbytes};
    if (e.skin) {
        used[n++] = {e.skinW, e.skinVerts * 16u};
        used[n++] = {e.skinI, e.skinVerts * 4u};
    }
    for (int a = 9; a <= 20; ++a)
        if ((e.idxMask & (1u << a)) && e.arr[a - 9] && e.hi[a - 9] > e.lo[a - 9])
            used[n++] = {static_cast<const uint8_t*>(e.arr[a - 9]) + e.lo[a - 9], e.hi[a - 9] - e.lo[a - 9]};
    return used[0];
}
Entry* BuildDyn(const uint8_t* list, uint32_t nbytes) {
    Entry* e = new Entry();
    const uint64_t epoch0 = GuestWriteEpoch();
    e->list = list;
    e->nbytes = nbytes;
    std::memcpy(e->vcd, g.vcd, 24);
    e->skin = g_skin.on;
    e->skinW = g_skin.weights;
    e->skinI = g_skin.indices;
    e->skinBones = g_skin.nBones;
    e->skinVerts = g_skin.nVerts;
    bool ok;
    {
        workload::FeScope feDecode(workload::FeDecode);
        ok = Build(*e, list, nbytes);
    }
    g_ctr.liveBytes += e->verts.capacity();
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
            PartHashes(*e);
            const uint64_t epoch1 = GuestWriteEpoch();
            if (epoch1 != epoch0) {
                HostRange used[15];
                int nUsed = 0;
                DynRaceRanges(*e, used, nUsed);
                ok = !GuestWritesTouch(epoch0, epoch1, used, nUsed);   // (a write over what was read while it was read: not trusted)
            }
        }
    }
    if (!ok) { Free(e); return nullptr; }
    return e;
}
// A call of a list inside its bypass window (MSC_VGX_DLREFRESH on). True: drawn here; false: the caller gives it to the parser, as with the switch off.
bool DynCall(const uint8_t* list, uint32_t nbytes, uint64_t frame) {
    workload::FeScope feStream(workload::FeStream);
    DynSet& ds = g_dyn[list];
    Entry* e = nullptr;
    size_t at = 0;
    for (; at < ds.v.size(); ++at)
        if (ds.v[at]->nbytes == nbytes && SameState(*ds.v[at])) { e = ds.v[at]; break; }
    if (e) {
        uint64_t gl = 0, ga[12];
        const bool tracked = ReadGenerations(*e, gl, ga);
        bool same = tracked && gl == e->genList;
        for (int a = 9; a <= 20 && same; ++a)
            if (e->idxMask & (1u << a)) same = ga[a - 9] == e->genArr[a - 9];
        int r = 0;
        const char* why = nullptr;
        if (same) {
            r = 1;
            if (RefreshMode() == 2) {
                // (verify: nothing was notified, so every part must be what it was: the vertices are a full decode of what is there now)
                RefreshVerify(*e, "no write notified", 0);
            }
            ++g_ctr.refreshSame;
        } else if (tracked) {
            const uint64_t epoch0 = GuestWriteEpoch();
            r = DynRefresh(*e, gl, ga, why);
            if (r) {
                e->genList = gl;
                std::memcpy(e->genArr, ga, sizeof ga);
                if (r == 2) ++g_ctr.refreshed; else ++g_ctr.refreshSame;
                const uint64_t epoch1 = GuestWriteEpoch();
                if (epoch1 != epoch0) {
                    HostRange used[15];
                    int nUsed = 0;
                    DynRaceRanges(*e, used, nUsed);
                    if (GuestWritesTouch(epoch0, epoch1, used, nUsed)) r = 0;   // (a write landed over what was hashed and decoded: not trusted)
                }
            }
        }
        if (r) {
            EmitDyn(*e, nbytes);
            return true;
        }
        if (EnvInt("MSC_VGX_BYPASSLOG", 0)) {
            char w[120];
            std::snprintf(w, sizeof w, "kept list not refreshable (%s)", why ? why : "not tracked");
            DynBypassNote(w, list, nbytes);
        }
        ++g_ctr.refreshGaveUp;
        g_dynBytes -= e->verts.capacity();
        Free(e);
        ds.v.erase(ds.v.begin() + static_cast<std::ptrdiff_t>(at));
        e = nullptr;
    }
    if (ds.noBuildUntil > frame || ds.v.size() >= kDynVariants || g_dynBytes >= kDynMaxBytes) return false;
    e = BuildDyn(list, nbytes);
    if (!e) { ds.noBuildUntil = frame + 30; return false; }
    g_dynBytes += e->verts.capacity();
    ds.v.push_back(e);
    ++g_ctr.refreshKept;
    EmitDyn(*e, nbytes);
    return true;
}

void Emit(Entry& e, uint32_t nbytes) {
    stats::Frame& fr = stats::Cur();
    ++fr.streams;
    fr.streamBytes += nbytes;
    if (e.bufId) {
        for (const Seg& s : e.segs) {
            SetPendingPosMtxMax(s.posMtxMax);
            g_staticSegBounds = s.boundsOk ? s.bounds : nullptr;
            g_staticSegVerts = (e.palKept || e.sbKept) ? e.verts.data() + s.offset : nullptr; // (MSC_VGX_PALMERGE, MSC_VGX_SBATCH)
            g_staticSegSerial = g_bufSerial[e.bufId];
            g_staticSegPalMask = s.posMtxMask;
            EmitStaticDraw(s.prim, s.layoutMask, s.count, e.bufId, AbsoluteIndices() ? 0 : (e.welded ? s.wOff : s.offset), s.idxOffset, s.vtxFirst);
            g_staticSegBounds = nullptr;
            g_staticSegPalMask = 0;
            g_staticSegVerts = nullptr;
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
            e.solo = 0; // (in a batch, or culled: nothing to learn from it)
            ++fr.primsDL;
            fr.vertsDL += s.count;
            continue;
        }
        g_replayList = nullptr;
        uint8_t* out = BeginVertices(s.layoutMask, s.count);
        if (!out) { e.solo = 0; continue; }   // (MSC_VGX_ARENA_SOFT: the arena could not grow; this segment is dropped)
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
        if (g_promoSolo) {
            const EmitOutcome o = LastEmitOutcome();
            if (e.segs.size() != 1 || !e.smallOk || (o != kEmitDropped && !LastDrawSoftOnly()) || o == kEmitJoined || o == kEmitBatch) {
                e.solo = 0;
            } else if (o == kEmitNew) {
                if (e.solo < 255) ++e.solo;
                g_soloOpen = &e; // (the next draw may still join it)
            }
        }
        ++fr.primsDL;
        fr.vertsDL += s.count;
    }
    if (g_promoSmall) e.calm = anySoft ? 0 : (e.calm < 255 ? static_cast<uint8_t>(e.calm + 1) : e.calm);
}

void Free(Entry* e) {
    if (g_soloOpen == e) g_soloOpen = nullptr;
    ReleaseBuffer(*e);
    g_ctr.liveBytes -= e->verts.capacity();
    delete e;
}

// MSC_DL_KEEP (wave 7 follow-up, default 0 in the code, the Vita plan sets 6 MiB): the lists of a goal's celebration and replay were let go 600 use-frames after their last use and built again at
// the next goal (168 lists in the first picture of the celebration). With a cap above 0 the entries that are due are kept while they add up to no more than the cap in MiB (decoded bytes in the front end plus the
// bytes of their static buffers in the back end; the cache of the entries in use is not counted); over the cap the oldest due entries go, at most kKeepEvictPerSweep a sweep (a sweep is every 120 frames), never all at once. An entry kept is
// judged by its generations on its next call exactly like any other, so what is drawn does not change, only whether a list is decoded at the call or found.
constexpr int kKeepEvictPerSweep = 24;
int g_keepOn = -1;
uint64_t g_keepCap = 0, g_keepSeen = 0;
inline bool KeepOn() {
    if (g_keepOn < 0) {
        const long mb = EnvInt("MSC_DL_KEEP", 0);
        g_keepOn = mb > 0 ? 1 : 0;
        g_keepCap = mb > 0 ? static_cast<uint64_t>(mb) << 20 : 0;
    }
    return g_keepOn != 0;
}
inline uint64_t EntryBytes(const Entry& e) {
    return static_cast<uint64_t>(e.verts.capacity()) + e.segs.capacity() * sizeof(Seg) + sizeof(Entry) + e.bufBytes;
}

// Drops entries that were not used for a long time (called once in a while).
uint64_t g_dbgSweepFreed = 0;   // (MSC_DL_SEEN: entries the sweep let go, in all)
void Sweep(uint64_t frame, uint64_t use) {
    const uint64_t begin=NpDiagActive&&NpDiagActive()?NpDiagNow():0;
    bool keepAll = false;
    if (KeepOn()) {
        uint64_t total = 0, dueBytes = 0, dueHeap = 0, dueBuf = 0;
        unsigned due = 0;
        std::vector<std::pair<uint64_t, Entry*>> cand;
        for (auto& kv : g_map)
            for (Entry* e = kv.second.head; e; e = e->next) {
                total += EntryBytes(*e);
                e->evictMark = false;
                if (use - e->lastUse > kRetainFrames) { cand.push_back(std::make_pair(e->lastUse, e)); dueBytes += EntryBytes(*e); dueBuf += e->bufBytes; dueHeap += EntryBytes(*e) - e->bufBytes; ++due; }
            }
        g_keepSeen = total;
        if (dueBytes > g_keepCap && !cand.empty()) {
            std::sort(cand.begin(), cand.end(), [](const std::pair<uint64_t, Entry*>& a, const std::pair<uint64_t, Entry*>& b) { return a.first < b.first; });
            uint64_t over = dueBytes - g_keepCap;
            for (size_t i = 0; i < cand.size() && i < static_cast<size_t>(kKeepEvictPerSweep) && over > 0; ++i) {
                cand[i].second->evictMark = true;
                const uint64_t b = EntryBytes(*cand[i].second);
                over = b >= over ? 0 : over - b;
            }
        }
        keepAll = true;
        static unsigned logged = 0;
        if (due && logged < 40 && EnvInt("MSC_DL_KEEP_LOG", 0) != 0) {
            ++logged;
            std::fprintf(stderr, "[dlkeep] frame %llu: %lu entries due (%.2f MB: %.2f MB decoded in the heap + %.2f MB in static buffers; the whole cache %.2f MB), cap %.2f MB\n", static_cast<unsigned long long>(frame),
                         static_cast<unsigned long>(due), dueBytes / 1048576.0, dueHeap / 1048576.0, dueBuf / 1048576.0, total / 1048576.0, g_keepCap / 1048576.0);
        }
    }
    for (auto it = g_map.begin(); it != g_map.end();) {
        Entry** link = &it->second.head;
        while (*link) {
            Entry* e = *link;
            if (keepAll ? e->evictMark : (use - e->lastUse > kRetainFrames)) {
                *link = e->next;
                Free(e);
                ++g_ctr.evicted;
                ++g_dbgSweepFreed;
            } else {
                link = &e->next;
            }
        }
        if (!it->second.head) { it = g_map.erase(it); MapFrontClear(); }
        else ++it;
    }
    DynSweep(frame);
    for (auto it = g_bypass.begin(); it != g_bypass.end();) {
        if (it->second.until + kRetainFrames < frame) it = g_bypass.erase(it);
        else ++it;
    }
    if(begin)NpDiagEvent(NPD_GEOMETRY,1,frame,begin,NpDiagNow(),g_ctr.evicted,g_map.size(),g_bypass.size());
}

// MSC_VGX_DLFLUSH_STADIUM (1.2, default on; =0 off): when the game tears a stadium down (VgxStadiumChanged, from DestroyStadium) the lists of the old stadium are never called
// again, but the plain eviction keeps them for kRetainFrames use-frames (a stadium A -> B change then holds both stadiums' static buffers in VRAM and both
// decoded copies in RAM for ten seconds of play). kFlushDelay use-frames after the change, every entry that was not called since the change is freed,
// kFlushPerFrame entries per frame. What the new match calls again (menus, players, ball, shared models) has been called by then and stays. Dropping an
// entry is what the eviction does too, so the picture cannot change: a list called again later is built again.
std::atomic<uint32_t> g_stadiumSignal{0};
uint32_t g_flushSeen = 0;
int g_flushState = 0;                   // 0 idle, 1 waiting for kFlushDelay, 2 freeing
uint64_t g_flushMark = 0, g_flushDue = 0;
std::vector<const uint8_t*> g_flushQueue;
size_t g_flushPos = 0;
constexpr uint64_t kFlushDelay = 240;
constexpr int kFlushPerFrame = 24;
void FlushStep(uint64_t use) {
    const uint32_t sig = g_stadiumSignal.load(std::memory_order_relaxed);
    if (sig != g_flushSeen) {
        g_flushSeen = sig;
        g_flushMark = use;
        g_flushDue = use + kFlushDelay;
        g_flushState = 1;
        g_flushQueue.clear();
    }
    if (g_flushState == 1 && use >= g_flushDue) {
        g_flushQueue.clear();
        g_flushPos = 0;
        for (const auto& kv : g_map)
            for (const Entry* e = kv.second.head; e; e = e->next)
                if (e->lastUse <= g_flushMark) { g_flushQueue.push_back(kv.first); break; }
        g_flushState = 2;
    }
    if (g_flushState != 2) return;
    int budget = kFlushPerFrame;
    while (budget > 0 && g_flushPos < g_flushQueue.size()) {
        auto it = g_map.find(g_flushQueue[g_flushPos++]);
        if (it == g_map.end()) continue;
        Entry** link = &it->second.head;
        while (*link) {
            Entry* e = *link;
            if (e->lastUse <= g_flushMark) {
                *link = e->next;
                Free(e);
                ++g_ctr.evicted;
                ++g_ctr.flushed;
                --budget;
            } else {
                link = &e->next;
            }
        }
        if (!it->second.head) { g_map.erase(it); MapFrontClear(); }
    }
    if (g_flushPos >= g_flushQueue.size()) { g_flushState = 0; g_flushQueue.clear(); g_flushQueue.shrink_to_fit(); }
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

// MSC_VGX_PROMOSOLO: a draw that joins a record makes that record's list one that merges.
void DlNoteRecord(bool joined) {
    if (joined && g_soloOpen) g_soloOpen->solo = 0;
    g_soloOpen = nullptr;
}

uint32_t AllocStaticBufferId() { return AllocBufId(nullptr); }
// An id that was not freed in this frame: a buffer whose upload is put in front of draws already recorded (vgx_draw.cpp PmClose) must not take an id
// whose CMD_BUF_FREE comes after those draws in the same frame (the old buffer's draws would see the new contents).
uint32_t AllocStaticBufferIdFresh() {
    if (!g_freeIds.empty() && g_bufFreedAt[g_freeIds.back()] == CurrentFrame()) {
        const uint32_t id = static_cast<uint32_t>(g_bufOwner.size());
        g_bufOwner.push_back(nullptr);
        g_bufSerial.push_back(++g_serialNext);
        g_bufFreedAt.push_back(~0ull);
        return id;
    }
    return AllocBufId(nullptr);
}
void FreeStaticBufferId(uint32_t id) {
    if (!id || id >= g_bufOwner.size()) return;
    Pending().cmds.push_back(Cmd{CMD_BUF_FREE, id});
    g_bufOwner[id] = nullptr;
    g_bufFreedAt[id] = CurrentFrame();
    g_freeIds.push_back(id);
}
// MSC_VGX_PALMERGE: the kept decoded vertices of a promoted list, [byteOff, byteOff + bytes), while buffer `bufId` is still the one with this serial.
const uint8_t* StaticSegVerts(uint32_t bufId, uint32_t serial, uint32_t byteOff, uint32_t bytes) {
    if (bufId >= g_bufOwner.size() || g_bufSerial[bufId] != serial) return nullptr;
    const Entry* e = g_bufOwner[bufId];
    if (!e || !(e->palKept || e->sbKept) || static_cast<uint64_t>(byteOff) + bytes > e->verts.size()) return nullptr;
    return e->verts.data() + byteOff;
}
uint64_t PalKeptBytes() { return g_ctr.palKeptBytes; }
uint64_t SBatchKeptBytes() { return g_ctr.sbKeptBytes; }
void SBatchWant(uint32_t bufId) {
    if (bufId < g_bufOwner.size() && g_bufOwner[bufId] && g_bufOwner[bufId]->sbWhy != 3) g_bufOwner[bufId]->sbWant = true;
}
uint64_t SBatchRekept() { return g_ctr.sbRekept; }
uint8_t SBatchWhyNotKept(uint32_t bufId) {
    if (bufId >= g_bufOwner.size() || !g_bufOwner[bufId]) return 9;
    return g_bufOwner[bufId]->sbWhy;
}
void SBatchNoted(uint32_t bufId) {
    if (bufId < g_bufOwner.size() && g_bufOwner[bufId]) g_bufOwner[bufId]->sbNoted = true;
}
// MSC_VGX_SBATCH: kept copies of lists not called for `idle` cache-use frames are dropped (a menu's or the last stadium's lists must not hold the cap
// when the next stadium's lists are promoted). A dropped list's members are not admitted again (SbBuild finds no vertices).
// idle 0: instead, the copies of lists never noted as a candidate (made before this cache frame).
void SBatchTrimKept(uint64_t idle) {
    if (!g_ctr.sbKeptBytes) return;
    for (auto& kv : g_map)
        for (Entry* e = kv.second.head; e; e = e->next)
            if (e->sbKept && (g_useClock - e->lastUse > idle || (idle == 0 && !e->sbNoted && e->sbKeptAt != g_useClock))) {
                e->sbWhy = idle ? 7 : 8;
                if (e->palKept) { (e->skinCtr ? g_ctr.skinKeptBytes : g_ctr.palKeptBytes) -= e->verts.capacity(); e->palKept = false; e->skinCtr = false; }   // (the same vertices: the palette merge's count and flag go with them)
                g_ctr.sbKeptBytes -= e->verts.capacity();
                g_ctr.liveBytes -= e->verts.capacity();
                e->verts = std::vector<uint8_t>();
                e->sbKept = false;
            }
}

bool StaticBufferData(uint32_t bufId, const uint8_t*& vtx, const uint16_t*& idx) {
    if (bufId >= g_bufOwner.size() || !g_bufOwner[bufId] || g_bufOwner[bufId]->verts.empty()) return false;
    vtx = g_bufOwner[bufId]->verts.data();
    idx = g_bufOwner[bufId]->idx.data();
    return true;
}

// 1.3 (wave 8) per-match switch line: MSC_DL_KEEP on, and how many lists it kept that the old rule would have freed and a call then found again
bool DlKeepActive() { return KeepOn(); }
uint64_t DlKeepSaved() { return g_ctr.keepSaved; }
void DlCacheDiagnostic(uint64_t tag){
    if(NpDiagActive&&NpDiagActive())NpDiagEvent(NPD_GEOMETRY,0,tag,g_ctr.liveBytes,g_ctr.staticBytes,g_ctr.staticBufs,g_ctr.evicted,g_map.size());
    // MSC_VGX_DLLIVE=<every N presents> (diagnostic, 0/unset: off): what the geometry cache holds now, for stadium-to-stadium comparisons
    const long every = static_cast<long>(VGX_KNOB(EnvInt("MSC_VGX_DLLIVE", 0)));
    if (every > 0 && CurrentFrame() % static_cast<uint64_t>(every) == 0)
        std::fprintf(stderr, "[dllive] frame %llu: %lu lists, %llu static buffers / %.2f MB in the back end, %.2f MB decoded in the front end, %llu evicted (%llu by the stadium flush), %llu builds, %llu hits, %llu bypassed\n",
                     static_cast<unsigned long long>(CurrentFrame()), (unsigned long)g_map.size(), static_cast<unsigned long long>(g_ctr.staticBufs), g_ctr.staticBytes / 1048576.0,
                     g_ctr.liveBytes / 1048576.0, static_cast<unsigned long long>(g_ctr.evicted), static_cast<unsigned long long>(g_ctr.flushed), static_cast<unsigned long long>(g_ctr.builds),
                     static_cast<unsigned long long>(g_ctr.hits), static_cast<unsigned long long>(g_ctr.bypassed));
}

// (MSC_VGX_FE_SPLIT: the branch counters, for the [lane-dl] line of vgx_workload.h)
void DlCacheBranchCounts(uint64_t out[9]) {
    out[0] = g_ctr.hits; out[1] = g_ctr.rehashOk; out[2] = g_ctr.rehashChanged; out[3] = g_ctr.bypassed;
    out[4] = g_ctr.builds; out[5] = g_ctr.uncacheable; out[6] = g_ctr.promoted; out[7] = g_ctr.promotedSolo;
    out[8] = g_ctr.refreshed;
}

void DlCacheReport(FILE* f) {
    if (dlweld::g_t.lists) DlWeldLog();
    if (g_watchFast || g_watchSlow)
        std::fprintf(f, "display-list cache watch (MSC_VGX_DLWATCH=%d, buckets %d): %llu hits validated by the watch epoch, %llu by reading generations; verify: %llu checked, %llu MISMATCHING\n", DlWatchMode(), DlWatchBuckets() ? 1 : 0,
                     (unsigned long long)g_watchFast, (unsigned long long)g_watchSlow, (unsigned long long)g_watchChecked, (unsigned long long)g_watchBad);
    std::fprintf(f,
                 "display-list cache: %llu hits (%llu verts), %llu builds (%llu verts), %llu re-validated by hash, %llu changed, %llu bypassed calls, "
                 "%llu uncacheable, %llu evicted, %llu changed-without-notification (verify mode), %.1f MB of decoded vertices live in the front end, "
                 "%llu promotions to static buffers (%llu of them lists with small draws, MSC_VGX_PROMOSMALL), %llu static buffers / %.1f MB live in the back end, %llu builds refused for an index outside its array\n",
                 (unsigned long long)g_ctr.hits, (unsigned long long)g_ctr.hitVerts, (unsigned long long)g_ctr.builds,
                 (unsigned long long)g_ctr.builtVerts, (unsigned long long)g_ctr.rehashOk, (unsigned long long)g_ctr.rehashChanged,
                 (unsigned long long)g_ctr.bypassed, (unsigned long long)g_ctr.uncacheable, (unsigned long long)g_ctr.evicted,
                 (unsigned long long)g_ctr.staleNoNotify, g_ctr.liveBytes / 1048576.0, (unsigned long long)g_ctr.promoted,
                 (unsigned long long)g_ctr.promotedSmall, (unsigned long long)g_ctr.staticBufs, g_ctr.staticBytes / 1048576.0, (unsigned long long)g_ctr.boundsRefused);
    if (RefreshOn() || g_ctr.refreshKept)
        std::fprintf(f, "display-list cache refresh (MSC_VGX_DLREFRESH=%d): %llu lists kept instead of bypassed, %llu calls decoded only the changed arrays (%llu attribute-vertices), %llu found nothing changed, %llu gave up (bypass as before)\n",
                     (int)RefreshMode(), (unsigned long long)g_ctr.refreshKept, (unsigned long long)g_ctr.refreshed, (unsigned long long)g_ctr.refreshAttrVerts, (unsigned long long)g_ctr.refreshSame, (unsigned long long)g_ctr.refreshGaveUp);
    if (RefreshMode() == 2) std::fprintf(f, "display-list cache refresh verify: %llu primitives checked, %llu MISMATCHING\n", (unsigned long long)g_refreshChecked, (unsigned long long)g_refreshBad);
    if (g_promoSolo) std::fprintf(f, "display-list cache: %llu promotions of lists whose CPU-transformed draw merged with nothing (MSC_VGX_PROMOSOLO=%u)\n", (unsigned long long)g_ctr.promotedSolo, g_promoSolo);
}

// MSC_VGX_BYPASSLOG=1 (diagnostic): why a list is decoded every frame. Names what differs between the new binding and the newest variant.
void BypassNote(const char* why, const uint8_t* list, uint32_t nbytes, const Entry* v) {
    const int on = static_cast<int>(VGX_KNOB(EnvInt("MSC_VGX_BYPASSLOG", 0) ? 1 : 0));
    if (!on) return;
    char diff[160] = "";
    if (v) {
        size_t n = 0;
        auto add = [&](const char* t) { n += std::snprintf(diff + n, sizeof diff - n, "%s ", t); };
        if (std::memcmp(v->vcd, g.vcd, 21) != 0) add("vcd");
        for (int f = 0; f < 8; ++f)
            if ((v->fmtMask & (1u << f)) && std::memcmp(v->vat[f], &g.vat[f][9], sizeof v->vat[f]) != 0) { add("vat"); break; }
        static const char* kNames[12] = {"pos", "nrm", "clr0", "clr1", "tex0", "tex1", "tex2", "tex3", "tex4", "tex5", "tex6", "tex7"};
        for (int a = 9; a <= 20; ++a)
            if ((v->idxMask & (1u << a)) && v->arr[a - 9] != g.arrays[a].data) add(kNames[a - 9]);
        if (v->skin != g_skin.on) add("skin-on");
        else if (v->skin && (v->skinW != g_skin.weights || v->skinI != g_skin.indices)) add("skin-arrays");
        if (v->nbytes != nbytes) add("size");
    }
    char vd[40];
    for (int a = 9; a <= 20; ++a) vd[a - 9] = "-dbw"[g.vcd[a] & 3];
    vd[12] = 0;
    std::fprintf(stderr, "[bypass] frame %llu list %p %u bytes: %s%s%s (vcd pos..tex7 %s, skin %d)\n", static_cast<unsigned long long>(CurrentFrame()), (const void*)list, nbytes, why,
                 v ? "; differs: " : "", diff, vd, g_skin.on ? 1 : 0);
}
void DynBypassNote(const char* why, const uint8_t* list, uint32_t nbytes) { BypassNote(why, list, nbytes, nullptr); }

namespace {
// ---- MSC_DL_WARM (wave 7): the lists a goal's scenes call for the first time are decoded ahead, a few a picture ---------------------------------------------------------------------------
// The first picture of a goal celebration builds ~150 display lists that no picture of open play called (120k vertices, ~20 M front-end instructions: five times a normal picture, about one
// long picture at the cut), and the entries of a goal's scenes are evicted 600 use-frames (10 s) after the last use, so the NEXT goal builds them again. The front end remembers, per scene
// class (the celebration after a goal, the replay), the identity of every list built in the first 120 frames of the scene (list, vertex descriptor, array bindings, skin arrays: what an
// Entry's identity is made of, ~0.6 KB each, at most 512 a class). When a goal is scored in play (VgxWarmGoal) the entries of those lists that are not in the cache are built again, at most
// MSC_DL_WARM_VERTS (8000) vertices a picture, by the same code that builds them at a call: the state a decode reads (descriptor, formats, array bindings, skin arrays) is set to the
// remembered one for that call and put back. The entry made is the one the real call makes (same identity; the generation counters and the content hash validate it as ever); nothing is
// recorded for the frame (no draw, no state, no texture), a list that is no longer a pure geometry list is dropped from the memory without marking it uncacheable. Draw-side only.
// The very first goal of a session has nothing remembered and builds as before.
bool g_warm = false;                 // a warm call is running: CallListBody records no draw
int g_wOn = -1;                      // MSC_DL_WARM
uint32_t g_wVerts = 8000;
struct WarmCtx {
    const uint8_t* list; uint32_t nbytes;
    uint8_t vcd[24]; uint8_t fmtMask; uint32_t idxMask;
    VtxAttrFmt vat[8][12];
    const uint8_t* arr[12]; uint32_t arrStride[12]; uint32_t hi[12]; uint16_t arrLe;
    bool skin; const uint8_t* skinW; const uint8_t* skinI; uint32_t skinBones, skinVerts;
};
constexpr size_t kWarmMax = 512;
std::vector<WarmCtx> g_wSet[2];      // 0: the celebration after a goal, 1: the replay
std::vector<WarmCtx> g_wQueue;
size_t g_wPos = 0;
int g_wLearn = -1;                   // the set being learned, -1 none
uint64_t g_wLearnUntil = 0;
bool g_wGoal = false;                // a goal was scored in play: the next cut-scene is the celebration
uint64_t g_wDone = 0, g_wDropped = 0, g_wLearned = 0, g_wFrames = 0;
bool WarmOn() {
    if (g_wOn < 0) {
        g_wOn = EnvInt("MSC_DL_WARM", 0) != 0 ? 1 : 0;
        const long v = EnvInt("MSC_DL_WARM_VERTS", 8000);
        g_wVerts = v < 500 ? 500u : static_cast<uint32_t>(v);
    }
    return g_wOn != 0;
}
void WarmLearn(const Entry& e) {
    if (g_wLearn < 0 || CurrentFrame() > g_wLearnUntil) return;
    std::vector<WarmCtx>& set = g_wSet[g_wLearn];
    if (set.size() >= kWarmMax) return;
    for (const WarmCtx& c : set)
        if (c.list == e.list && c.arr[0] == e.arr[0] && c.skinW == e.skinW && c.nbytes == e.nbytes) return;
    WarmCtx c;
    std::memset(&c, 0, sizeof c);
    c.list = e.list; c.nbytes = e.nbytes;
    std::memcpy(c.vcd, e.vcd, sizeof c.vcd);
    c.fmtMask = e.fmtMask; c.idxMask = e.idxMask;
    for (int f = 0; f < 8; ++f) for (int i = 0; i < 12; ++i) c.vat[f][i] = e.vat[f][i];
    for (int i = 0; i < 12; ++i) { c.arr[i] = e.arr[i]; c.arrStride[i] = e.arrStride[i]; c.hi[i] = e.hi[i]; }
    c.arrLe = e.arrLe;
    c.skin = e.skin; c.skinW = e.skinW; c.skinI = e.skinI; c.skinBones = e.skinBones; c.skinVerts = e.skinVerts;
    set.push_back(c);
    ++g_wLearned;
}
// Decodes one remembered list as the real call would. Returns the vertices decoded (0: already cached, or dropped).
uint32_t WarmOne(const WarmCtx& c);
void WarmSceneImpl(int cls) {
    if (cls == 3) { g_wLearn = g_wGoal ? 0 : -1; g_wGoal = false; }   // a cut-scene after a goal in play is the celebration; any other is not learned
    else if (cls == 2) g_wLearn = 1;
    else { g_wLearn = -1; if (cls == 1) g_wGoal = false; }
    g_wLearnUntil = CurrentFrame() + 120;
}
void WarmGoalImpl() {
    g_wGoal = true;
    g_wQueue.clear();
    g_wPos = 0;
    g_wFrames = 0;
    g_wQueue.insert(g_wQueue.end(), g_wSet[0].begin(), g_wSet[0].end());
    g_wQueue.insert(g_wQueue.end(), g_wSet[1].begin(), g_wSet[1].end());
    static int logged = 0;
    if (++logged <= 40)
        std::fprintf(stderr, "[dlwarm] goal at frame %llu: %lu remembered lists (celebration %lu, replay %lu) to build ahead, %lu vertices a picture\n", static_cast<unsigned long long>(CurrentFrame()),
                     static_cast<unsigned long>(g_wQueue.size()), static_cast<unsigned long>(g_wSet[0].size()), static_cast<unsigned long>(g_wSet[1].size()), static_cast<unsigned long>(g_wVerts));
}
void WarmStep() {
    if (g_wPos >= g_wQueue.size()) return;
    ++g_wFrames;
    uint32_t budget = g_wVerts;
    while (g_wPos < g_wQueue.size() && budget > 0) {
        const uint32_t n = WarmOne(g_wQueue[g_wPos++]);
        budget = n >= budget ? 0 : budget - n;
    }
    if (g_wPos >= g_wQueue.size()) {
        std::fprintf(stderr, "[dlwarm] queue done at frame %llu: %llu warmed, %llu dropped, over %llu pictures\n", static_cast<unsigned long long>(CurrentFrame()), static_cast<unsigned long long>(g_wDone),
                     static_cast<unsigned long long>(g_wDropped), static_cast<unsigned long long>(g_wFrames));
        g_wQueue.clear();
        g_wPos = 0;
    }
}

} // namespace (MSC_DL_WARM)

// MSC_DL_SEEN=1 (wave 7, diagnostic, front end only): of the lists built in a picture, how many no picture ever built before (first sight) and how many were built, let go and are built again (with
// the age since their previous build). One line per picture with 20 or more builds. Reads nothing the game or the draw sees.
namespace {
std::unordered_map<const uint8_t*, uint64_t> g_seenAt;
uint64_t g_seenFrame = 0; unsigned g_seenFirst = 0, g_seenAgain = 0, g_seenVerts = 0; uint64_t g_seenAgeSum = 0, g_seenAgeMin = ~0ull, g_seenAgeMax = 0;
int g_seenOn = -1;
void SeenFlush() {
    if (g_seenFirst + g_seenAgain >= 20)
        std::fprintf(stderr, "[dlseen] frame %llu: %u builds, %u first sight, %u built again (age since the previous build min %llu mean %llu max %llu pictures), %u vertices; entries let go so far: sweep %llu, flush %llu, other %llu\n", static_cast<unsigned long long>(g_seenFrame),
                     g_seenFirst + g_seenAgain, g_seenFirst, g_seenAgain, static_cast<unsigned long long>(g_seenAgain ? g_seenAgeMin : 0), static_cast<unsigned long long>(g_seenAgain ? g_seenAgeSum / g_seenAgain : 0),
                     static_cast<unsigned long long>(g_seenAgeMax), g_seenVerts, static_cast<unsigned long long>(g_dbgSweepFreed), static_cast<unsigned long long>(g_ctr.flushed),
                     static_cast<unsigned long long>(g_ctr.evicted - g_dbgSweepFreed - g_ctr.flushed));
    g_seenFirst = g_seenAgain = g_seenVerts = 0; g_seenAgeSum = 0; g_seenAgeMin = ~0ull; g_seenAgeMax = 0;
}
void SeenNote(const uint8_t* list, uint32_t nbytes, uint32_t verts, uint64_t frame) {
    if (g_seenOn < 0) g_seenOn = static_cast<int>(EnvInt("MSC_DL_SEEN", 0));
    if (!g_seenOn) return;
    if (frame != g_seenFrame) { SeenFlush(); g_seenFrame = frame; }
    auto it = g_seenAt.find(list);
    const bool firstSight = it == g_seenAt.end();
    if (firstSight) { ++g_seenFirst; g_seenAt[list] = frame; }
    else { const uint64_t age = frame - it->second; ++g_seenAgain; g_seenAgeSum += age; if (age < g_seenAgeMin) g_seenAgeMin = age; if (age > g_seenAgeMax) g_seenAgeMax = age; it->second = frame; }
    g_seenVerts += verts;
    if (g_seenOn >= 2) std::fprintf(stderr, "[dlseen1] %llu %p %u %u %s\n", static_cast<unsigned long long>(frame), static_cast<const void*>(list), nbytes, verts, firstSight ? "first" : "again");
}
} // namespace

static void CallListBody(const uint8_t* list, uint32_t nbytes, workload::FeScope& cls);
void CallList(const uint8_t* list, uint32_t nbytes) {
    // (MSC_VGX_FE_SPLIT: the branch taken decides the slot; list-other until one is known)
    workload::FeScope cls(workload::FeOther);
    CallListBody(list, nbytes, cls);
}
static void CallListBody(const uint8_t* list, uint32_t nbytes, workload::FeScope& cls) {
    Init();
    const uint64_t frame = CurrentFrame();
#if VGX_WORKLOAD_BUILD
    const auto slot = [&cls](unsigned s) { if (cls.slot >= 0) cls.slot = static_cast<int>(s); };
#else
    const auto slot = [](unsigned) {};
    (void)cls;
#endif
    if (g_mode == 0 || nbytes <= g_tinyBytes || !IsGuestRam(list, nbytes) || TraceFrame()) {
        slot(workload::FeTiny);
        if (!g_warm) ProcessStream(list, nbytes);
        return;
    }
    if (frame != g_useClockFrame) {
        g_useClockFrame = frame;
        ++g_useClock;
        g_sbRekeepLeft = 8;
        g_soloLeft = kSoloPerFrame;
        DlWeldNewPicture(frame);
        if (g_flushOn > 0) FlushStep(g_useClock);
        if (!g_warm && WarmOn()) WarmStep();   // (MSC_DL_WARM: a few remembered lists decoded ahead, here, at the first list call of a picture)
    }
    const uint64_t use = g_useClock;
    {
        // 1.3 MSC_VGX_HOUSEKEEP_BACKOFF (vgx_housekeep.h): a sweep that let nothing go (and nothing else was freed since: the stadium flush) makes the next one wait twice as long, up to 480 frames
        static const bool backoff = EnvInt("MSC_VGX_HOUSEKEEP_BACKOFF", 0) != 0;
        static uint64_t lastSweep = 0, gap = 120, evictedAtSweep = 0, sweeps = 0, emptySweeps = 0, skippedFrames = 0, noteAt = 3600;
        if (frame - lastSweep >= (backoff ? gap : 120)) {
            skippedFrames += frame - lastSweep > 120 ? frame - lastSweep - 120 : 0;
            lastSweep = frame;
            const bool changed = g_ctr.evicted != evictedAtSweep;   // (something was freed since the last sweep, by anyone: look again soon)
            Sweep(frame, use);
            ++sweeps;
            const bool nothing = g_ctr.evicted == evictedAtSweep && !changed;
            if (nothing) ++emptySweeps;
            evictedAtSweep = g_ctr.evicted;
            gap = backoff ? vgx::housekeep::SweepGap(120, gap, nothing, 480) : 120;
        }
        if (frame >= noteAt) {   // [housekeep] line: every 3600 frames while the counters moved
            noteAt = frame + 3600;
            static uint64_t noteSum = 0;
            if (sweeps != noteSum) {
                noteSum = sweeps;
                std::fprintf(stderr, "[housekeep] list cache sweep: %lu passes (%lu freed nothing), %lu frames spared by the back-off (MSC_VGX_HOUSEKEEP_BACKOFF=%d), gap now %lu frames; %lu lists cached\n", (unsigned long)sweeps,
                             (unsigned long)emptySweeps, (unsigned long)skippedFrames, backoff ? 1 : 0, (unsigned long)gap, (unsigned long)g_map.size());
            }
        }
    }
    if (!g_bypass.empty() && (!Gxd(GXD_DLFAST) || BypassMaybe(list))) {
        auto b = g_bypass.find(list);
        if (b != g_bypass.end()) {
            if (frame < b->second.until) {
                ++g_ctr.bypassed;
                ++stats::Cur().dlBypass;
                if (!g_warm && !(RefreshOn() && DynCall(list, nbytes, frame))) ProcessStream(list, nbytes);   // (MSC_VGX_DLREFRESH: the list's own entry, refreshed)
                return;
            }
            // expired: try caching again (the entry stays, it remembers the back-off level)
            if (!g_dyn.empty()) DynDrop(list);
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
    if (g_warm && (e || li.head)) return;   // (MSC_DL_WARM: only a list with no entry at all is built ahead; an entry that exists, valid or not, is left to the real call)
    if (e && e->raced) {
        // Built while the game thread was flushing something: not trusted, build again.
    } else if (e) {
        uint64_t gl = 0, ga[12];
        bool tracked, same;
        // GXD_DLFAST: no write notification at all since the generations were last read and found unchanged: nothing the entry depends on can
        // have changed (the count is read before the generations, so a write that lands in between shows at the next call).
        const bool fast = Gxd(GXD_DLFAST);
        {   // MSC_VGX_DLWATCH_POKE=<n> (diagnostic, 0): every n-th lookup of a cached list reports a write over its first bytes (no content changes: a generation moves, as when another
            // object shares the page). Normal runs must give the same frames; the BREAK=2 mutant must then fail the verify run.
            static const long poke = EnvInt("MSC_VGX_DLWATCH_POKE", 0);
            static uint64_t pokeN = 0;
            if (__builtin_expect(poke != 0, 0) && ++pokeN % static_cast<uint64_t>(poke) == 0) hle::NotifyGuestWrite(list, 16);
        }
        const uint64_t ep = fast ? GuestWriteEpoch() : 0;
        const int dw = fast ? DlWatchMode() : 0;
        // (read before the generations are: a write that lands in between moves it, and shows at the next call)
        const uint64_t wep = (dw && e->watched) ? WatchStamp(*e) : 0;
        const bool watchOk = dw && e->watched && e->okWatch == wep && (e->watchMask != 0 || !DlWatchBuckets());
        if (fast && (e->okEpoch == ep || watchOk)) {
            tracked = same = true;
            if (watchOk && e->okEpoch != ep) {
                ++g_watchFast;
                if (dw == 2) {
                    uint64_t gl2, ga2[12];
                    bool same2 = ReadGenerations(*e, gl2, ga2) && gl2 == e->genList;
                    for (int a = 9; a <= 20 && same2; ++a)
                        if (e->idxMask & (1u << a)) same2 = ga2[a - 9] == e->genArr[a - 9];
                    ++g_watchChecked;
                    if (!same2) {
                        ++g_watchBad;
                        VGX_LOG_N(16, "dlwatch: MISMATCH list %p (%u bytes): generations moved with no watched write\n", (const void*)list, nbytes);
                    }
                }
            }
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
            // (watch first, then the epoch, then the generations)
            if (dw && !e->watched) { WatchEntry(*e); e->watched = true; }
            const uint64_t wep2 = dw ? WatchStamp(*e) : 0;
            tracked = ReadGenerations(*e, gl, ga);
            same = tracked && gl == e->genList;
            for (int a = 9; a <= 20 && same; ++a)
                if (e->idxMask & (1u << a)) same = ga[a - 9] == e->genArr[a - 9];
            if (fast && same) { e->okEpoch = ep; if (dw) e->okWatch = wep2; }
            if (dw) ++g_watchSlow;
        }
        if (same && g_mode == 2 && ContentHash(*e) != e->hash) {
            ++g_ctr.staleNoNotify;
            VGX_LOG_N(32, "dl cache: list %p (%u bytes, %u verts) changed without a write notification\n", (const void*)list, nbytes, e->totalVerts);
            same = false;
        }
        if (same && e->sbWant && e->bufId && !e->sbKept && g_sbRekeepLeft) {
            // MSC_VGX_SBATCH: decoded again so that the promotion keeps a copy (a few per frame)
            --g_sbRekeepLeft;
            e->sbWant = false;
            ++g_ctr.sbRekept;
            goto rebuild;
        }
        if (same) {
            if (g_keepOn > 0 && use - e->lastUse > kRetainFrames) ++g_ctr.keepSaved;
            e->lastUse = use;
            e->fails = 0;
            ++g_ctr.hits;
            ++stats::Cur().dlHit;
            g_ctr.hitVerts += e->totalVerts;
            // Used again unchanged: worth a static buffer (no per-frame copy, no index generation).
            if (!e->bufId && g_static) {
                // (nxfly25 MSC_VGX_DLWELD_BUDGET_US / _LISTS: this picture has welded enough; the list is drawn from the arena once more. nxr6a: the small and solo paths ask the budget too: vgx_dlweldbudget.h ChoosePromotion)
                switch (dlweldbudget::ChoosePromotion(e->promotable, e->smallOk, e->calm, e->solo, g_promoSmall, g_promoSolo, g_soloLeft, [] { return DlWeldMayPromote(); })) {
                case dlweldbudget::Route::kPromote: Promote(*e); break;
                case dlweldbudget::Route::kPromoteSolo:
                    --g_soloLeft;
                    Promote(*e);
                    if (e->bufId) ++g_ctr.promotedSolo;
                    break;
                case dlweldbudget::Route::kNone: break;
                }
            }
            slot(workload::FeGeomHit);
            if (!g_warm) Emit(*e, nbytes);
            return;
        }
        if (tracked && ContentHash(*e) == e->hash) {
            // A write landed in the same 4 KiB as something this entry uses, but not on it.
            e->genList = gl;
            std::memcpy(e->genArr, ga, sizeof ga);
            e->okEpoch = ~0ull;
            e->lastUse = use;
            e->fails = 0;
            ++g_ctr.rehashOk;
            ++stats::Cur().dlRehash;
            g_ctr.hitVerts += e->totalVerts;
            if (!g_warm) Emit(*e, nbytes);
            return;
        }
        ++g_ctr.rehashChanged;
        if (++e->fails >= 3) {
            // Rewritten every time it is drawn (dynamic geometry): hashing and
            // storing it is wasted work. Decode directly for the next two seconds.
            if (EnvInt("MSC_VGX_BYPASSLOG", 0)) {
                static const char* kNames[12] = {"pos", "nrm", "clr0", "clr1", "tex0", "tex1", "tex2", "tex3", "tex4", "tex5", "tex6", "tex7"};
                char w[200];
                size_t n = std::snprintf(w, sizeof w, "contents rewritten each draw: written %s", tracked && gl != e->genList ? "list " : "");
                for (int a = 9; a <= 20; ++a)
                    if ((e->idxMask & (1u << a)) && (!tracked || ga[a - 9] != e->genArr[a - 9]))
                        n += std::snprintf(w + n, sizeof w - n, "%s@%p+%u ", kNames[a - 9], (const void*)(e->arr[a - 9] + e->lo[a - 9]), e->hi[a - 9] - e->lo[a - 9]);
                BypassNote(w, list, nbytes, nullptr);
            }
            SetBypass(BypassFor(list), frame);
            Unlink(li, e);
            if (!li.head) { g_map.erase(list); MapFrontClear(); }
            if (!g_warm) ProcessStream(list, nbytes);
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
                BypassNote("a new binding every frame", list, nbytes, li.head);
                SetBypass(BypassFor(list), frame);
                FreeAll(li);
                g_map.erase(list);
                MapFrontClear();
                ++g_ctr.bypassed;
                ++stats::Cur().dlBypass;
                if (!g_warm) ProcessStream(list, nbytes);
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
rebuild:
    // (Re)build. The array ranges are only known after the decode, so writes
    // notified during it are checked against them afterwards (GuestWritesTouch).
    const uint64_t epoch0 = GuestWriteEpoch();
    g_ctr.liveBytes -= e->verts.capacity();
    e->list = list;
    e->nbytes = nbytes;
    std::memcpy(e->vcd, g.vcd, 24);
    e->okEpoch = ~0ull;
    e->okWatch = ~0ull;
    e->watched = false; e->watchMask = 0; // (the ranges are set below, after the decode: watched at the next call)
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
            // Raced only if a write notified during the build overlaps the
            // list or an array range it read (other game writes are common).
            const uint64_t epoch1 = GuestWriteEpoch();
            if (epoch1 == epoch0) {
                e->raced = false;
            } else {
                HostRange used[15];
                int nUsed = 0;
                used[nUsed++] = {e->list, e->nbytes};
                if (e->skin) {
                    used[nUsed++] = {e->skinW, e->skinVerts * 16u};
                    used[nUsed++] = {e->skinI, e->skinVerts * 4u};
                }
                for (int a = 9; a <= 20; ++a)
                    if ((e->idxMask & (1u << a)) && e->arr[a - 9] && e->hi[a - 9] > e->lo[a - 9])
                        used[nUsed++] = {static_cast<const uint8_t*>(e->arr[a - 9]) + e->lo[a - 9], e->hi[a - 9] - e->lo[a - 9]};
                e->raced = GuestWritesTouch(epoch0, epoch1, used, nUsed);
            }
        }
    }
    if (!ok) {
        // State commands inside, or data outside tracked guest RAM: the general
        // parser handles it, and the question is not asked again for a while.
        ++g_ctr.uncacheable;
        if (!g_warm) BypassFor(list).until = frame + 600;
        else ++g_wDropped;
        Unlink(li, e);
        if (!li.head) { g_map.erase(list); MapFrontClear(); }
        if (!g_warm) ProcessStream(list, nbytes);
        return;
    }
    e->lastUse = use;
    ++g_ctr.builds;
    ++stats::Cur().dlBuild;
    g_ctr.builtVerts += e->totalVerts;
    if (!g_warm) { SeenNote(list, nbytes, e->totalVerts, frame); WarmLearn(*e); Emit(*e, nbytes); }
}


namespace {
uint32_t WarmOne(const WarmCtx& c) {
    if (!IsGuestRam(c.list, c.nbytes)) { ++g_wDropped; return 0; }
    // the state a decode reads: set to the remembered one for this call, put back after it
    uint8_t vcd[26];
    std::memcpy(vcd, g.vcd, sizeof vcd);
    VtxAttrFmt vat[8][12];
    for (int f = 0; f < 8; ++f) for (int i = 0; i < 12; ++i) vat[f][i] = g.vat[f][9 + i];
    ArrayRef arr[12];
    for (int i = 0; i < 12; ++i) arr[i] = g.arrays[9 + i];
    const bool skinOn = g_skin.on;
    const uint32_t skinBones = g_skin.nBones, skinVerts = g_skin.nVerts;
    const uint8_t* const skinW = g_skin.weights;
    const uint8_t* const skinI = g_skin.indices;
    std::memset(g.vcd, 0, sizeof g.vcd);
    std::memcpy(g.vcd, c.vcd, sizeof c.vcd);
    for (int f = 0; f < 8; ++f) if (c.fmtMask & (1u << f)) for (int i = 0; i < 12; ++i) g.vat[f][9 + i] = c.vat[f][i];
    for (int i = 0; i < 12; ++i) {
        ArrayRef a;
        if (c.idxMask & (1u << (9 + i))) { a.data = c.arr[i]; a.stride = c.arrStride[i]; a.size = c.hi[i]; a.le = ((c.arrLe >> i) & 1u) != 0; }
        g.arrays[9 + i] = a;
    }
    g_skin.on = c.skin; g_skin.nBones = c.skinBones; g_skin.nVerts = c.skinVerts; g_skin.weights = c.skinW; g_skin.indices = c.skinI;
    g.layoutDirty = true;
    const uint64_t before = g_ctr.builtVerts;
    g_warm = true;
    {
        workload::FeScope cls(workload::FeOther);
        CallListBody(c.list, c.nbytes, cls);
    }
    g_warm = false;
    std::memcpy(g.vcd, vcd, sizeof vcd);
    for (int f = 0; f < 8; ++f) for (int i = 0; i < 12; ++i) g.vat[f][9 + i] = vat[f][i];
    for (int i = 0; i < 12; ++i) g.arrays[9 + i] = arr[i];
    g_skin.on = skinOn; g_skin.nBones = skinBones; g_skin.nVerts = skinVerts; g_skin.weights = skinW; g_skin.indices = skinI;
    g.layoutDirty = true;
    const uint32_t n = static_cast<uint32_t>(g_ctr.builtVerts - before);
    if (n) ++g_wDone;
    return n ? n : 64u;   // (an entry that existed costs a lookup: a little of the budget)
}
} // namespace
} // namespace vgx

// The game tore a stadium down (DestroyStadium): see FlushStep. Called on the game thread; the work happens on the next frames' list calls.
extern "C" void VgxStadiumChanged(void) { vgx::g_stadiumSignal.fetch_add(1, std::memory_order_relaxed); }
namespace vgx { uint32_t StadiumSignal() { return g_stadiumSignal.load(std::memory_order_relaxed); } } // (MSC_DYNRES_NEWSTADIUM, vgx_backend_gl.cpp)

// MSC_DL_WARM (wave 7), game thread. VgxWarmScene: the scene class at its cut (0 other, 1 open play, 2 replay, 3 cut-scene): what the front end learns. VgxWarmGoal: a goal was scored in play:
// the lists remembered for the celebration and the replay are built ahead, a few a picture.
extern "C" void VgxWarmScene(int cls) {
    if (!vgx::WarmOn()) return;
    vgx::WarmSceneImpl(cls);
}
extern "C" void VgxWarmGoal(void) {
    if (!vgx::WarmOn()) return;
    vgx::WarmGoalImpl();
}
