// Extension records of the native port (vgx_ext.h): the player's side. W5 SKIN.
#include "vgx_ext.h"
#include "vgx_state.h"

#include <cstdio>
#include <cstring>
#include <unordered_set>

#define XXH_INLINE_ALL
#include <xxhash.h>

namespace vgx {

uint32_t g_diagTag = 0, g_diagFlags = 0;
SkinState g_skin;

bool SkinEnabled() {
    static const bool on = EnvInt("MSC_VGX_SKIN", 1) != 0;
    return on;
}

// ------------------------------------------------------------------ tag census
// MSC_VGX_TAGCENSUS=1: per frame and tag (the native port tags each packet draw with its material program's hash,
// flags bit 0 = the packet was skinned on the CPU, bit 1 = it is skinned on the GPU; tag 0 = draws outside packets):
// draws and vertices that went into the per-frame stream, their vertex and index bytes, draws and vertices from static
// buffers, and vertices decoded (what the decode D did for them). One line per tag with anything in it:
//   [tagcensus] f=<frame> tag=<hex> fl=<flags> sd=<stream draws> sv=<stream vertices> sb=<vertex bytes> ib=<index bytes> td=<static draws> tv=<static vertices> dec=<decoded vertices>
namespace {
struct TagRec {
    uint32_t tag = 0, flags = 0;
    uint32_t sd = 0, sv = 0, sb = 0, ib = 0, td = 0, tv = 0, dec = 0;
};
constexpr int kTagMax = 96;
TagRec g_tags[kTagMax];
int g_nTags = 0;
TagRec& TagFor(uint32_t tag, uint32_t flags) {
    for (int i = 0; i < g_nTags; ++i)
        if (g_tags[i].tag == tag && g_tags[i].flags == flags) return g_tags[i];
    if (g_nTags == kTagMax) return g_tags[kTagMax - 1];
    TagRec& t = g_tags[g_nTags++];
    t = TagRec{};
    t.tag = tag;
    t.flags = flags;
    return t;
}
} // namespace

bool TagCensusOn() {
    static const bool on = EnvInt("MSC_VGX_TAGCENSUS", 0) != 0;
    return on;
}
void TagCensusDraw(uint32_t verts, uint32_t vtxBytes, uint32_t idxCount, bool isStatic) {
    TagRec& t = TagFor(g_diagTag, g_diagFlags);
    if (isStatic) { ++t.td; t.tv += verts; return; }
    ++t.sd;
    t.sv += verts;
    t.sb += vtxBytes;
    t.ib += idxCount * 2u;
}
void TagCensusDecode(uint32_t verts) { TagFor(g_diagTag, g_diagFlags).dec += verts; }
void TagCensusFrame(uint64_t frame) {
    for (int i = 0; i < g_nTags; ++i) {
        const TagRec& t = g_tags[i];
        if (!(t.sd | t.td | t.dec)) continue;
        std::fprintf(stderr, "[tagcensus] f=%llu tag=%08X fl=%u sd=%u sv=%u sb=%u ib=%u td=%u tv=%u dec=%u\n", static_cast<unsigned long long>(frame), t.tag, t.flags,
                     t.sd, t.sv, t.sb, t.ib, t.td, t.tv, t.dec);
    }
    g_nTags = 0;
}

// ------------------------------------------------------------------ soft census
const void* g_replayList = nullptr;
uint32_t g_replaySeg = 0;
uint64_t g_replayHash = 0;
namespace {
struct SoftCensus {
    std::unordered_set<uint64_t> prevKeys, curKeys, prevLists, curLists;
    uint32_t n = 0, v = 0, same = 0, sameV = 0, sameList = 0, noList = 0;
} g_sc2;
}
bool SoftCensusOn() {
    static const bool on = EnvInt("MSC_VGX_SOFTCENSUS", 0) != 0;
    return on;
}
void SoftCensusDraw(uint32_t count, uint32_t layoutMask, const float* mv12, const float* nrm9) {
    SoftCensus& c = g_sc2;
    ++c.n;
    c.v += count;
    if (!g_replayList) { ++c.noList; return; }
    const uint64_t lk = XXH3_64bits_withSeed(&g_replayList, sizeof g_replayList, g_replaySeg);
    uint64_t k = XXH3_64bits_withSeed(mv12, 48, lk ^ (static_cast<uint64_t>(count) << 32) ^ layoutMask);
    if (layoutMask & VL_NRM) k = XXH3_64bits_withSeed(nrm9, 36, k);
    if (c.prevLists.count(lk)) ++c.sameList;
    if (c.prevKeys.count(k)) { ++c.same; c.sameV += count; }
    c.curKeys.insert(k);
    c.curLists.insert(lk);
}
void SoftCensusFrame(uint64_t frame) {
    SoftCensus& c = g_sc2;
    if (c.n)
        std::fprintf(stderr, "[softcensus] f=%llu n=%u v=%u same=%u sameV=%u sameList=%u lists=%u nolist=%u\n", static_cast<unsigned long long>(frame), c.n, c.v, c.same,
                     c.sameV, c.sameList, static_cast<unsigned>(c.curLists.size()), c.noList);
    std::swap(c.prevKeys, c.curKeys);
    std::swap(c.prevLists, c.curLists);
    c.curKeys.clear();
    c.curLists.clear();
    c.n = c.v = c.same = c.sameV = c.sameList = c.noList = 0;
}

// ------------------------------------------------------------------ the records
void PlayExt(uint32_t kind, const uint32_t* w, uint32_t n) {
    switch (kind) {
    case EXT_TAG:
        if (n >= 2) { g_diagTag = w[0]; g_diagFlags = w[1]; }
        break;
    case EXT_SKIN: {
        if (n < 6 || !SkinEnabled()) break;
        const uint32_t nb = w[0] & 0xFFFFu;
        if (nb == 0 || nb > kSkinMaxBones || n < 6 + nb * 12) { g_skin.on = false; break; }
        uint64_t pw, pi;
        std::memcpy(&pw, w + 2, 8);
        std::memcpy(&pi, w + 4, 8);
        g_skin.on = true;
        g_skin.nBones = nb;
        g_skin.nVerts = w[1];
        g_skin.weights = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(pw));
        g_skin.indices = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(pi));
        std::memcpy(g_skin.mats, w + 6, nb * 48u);
        ++g_skin.serial;
        break;
    }
    case EXT_SKIN_OFF: g_skin.on = false; break;
    default: break;
    }
}

} // namespace vgx
