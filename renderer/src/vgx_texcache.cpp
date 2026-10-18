// Front-end texture manager: decides which back-end texture a texmap resolves
// to and queues uploads when the guest bytes are new or have changed.
#include "vgx_guestmem.h"
#include "vgx_state.h"
#include "vgx_texdecode.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <unordered_map>
#include <vector>

namespace vgx {

namespace {

struct Key {
    const void* data;
    uint32_t dims; // w | h << 16
    uint32_t fmt;  // format | levels << 8
    uint64_t tlutHash;
    bool operator==(const Key& o) const { return data == o.data && dims == o.dims && fmt == o.fmt && tlutHash == o.tlutHash; }
};
struct KeyHash {
    size_t operator()(const Key& k) const {
        return static_cast<size_t>(reinterpret_cast<uintptr_t>(k.data) * 0x9E3779B97F4A7C15ull ^ (static_cast<uint64_t>(k.dims) << 20) ^
                                   k.fmt ^ k.tlutHash);
    }
};
struct Entry {
    uint32_t texId = 0;
    uint64_t contentHash = 0;
    uint64_t generation = ~0ull;
    bool mipmapped = false;
    uint32_t bytes = 0; // bytes held by the back-end texture (RGBA8, DXT1 or LA88, all levels)
    uint64_t lastUsed = 0; // frame of the last bind
    uint32_t size = 0;     // guest bytes of all levels
    uint64_t checkedEpoch = ~0ull; // GuestWriteEpoch() when the contents were last found unchanged
};
struct CopyEntry {
    uint32_t texId = 0;
    uint16_t w = 0, h = 0;
    uint32_t fmt = 0;
};

std::unordered_map<Key, Entry, KeyHash> g_cache;
// In front of g_cache: the entry last looked up for each of 128 address classes (a lookup
// in the hash table costs a division on the console's processor, and a frame binds the same
// textures hundreds of times). Entries of an unordered_map do not move; the front is
// emptied whenever one is erased.
struct CacheFront { Key key; Entry* e; };
CacheFront g_cacheFront[128] = {};
inline Entry& CacheEntry(const Key& key) {
    CacheFront& f = g_cacheFront[(reinterpret_cast<uintptr_t>(key.data) >> 5) & 127u];
    if (f.e && f.key == key) return *f.e;
    Entry& e = g_cache[key];
    f.key = key;
    f.e = &e;
    return e;
}
inline void CacheFrontClear() {
    for (CacheFront& f : g_cacheFront) f.e = nullptr;
}
bool g_texVerify = false;
void (*g_texDumpHook)(const TexUpload&, uint32_t, uint64_t) = nullptr;
uint64_t g_texStale = 0;
uint64_t g_liveBytes = 0;          // bytes of all guest textures held by the back end
uint64_t g_budget = 0;             // 0 = keep everything
bool g_budgetSet = false;
uint64_t g_evicted = 0, g_evictedBytes = 0, g_overBudgetFrames = 0;
// Uploads by what the back end is handed (totals over the run, for the report).
uint64_t g_dxtUploads = 0, g_dxtBytes = 0, g_rgbaUploads = 0, g_rgbaBytes = 0, g_cmprDecoded = 0, g_cmprRgbaBytes = 0;
uint64_t g_laUploads = 0, g_laBytes = 0;
std::map<const void*, CopyEntry> g_copies;
// In front of g_copies: every texture bind asks whether its address is an EFB copy's (a walk
// down a tree of some 300 nodes, 800 times a frame), and almost always the answer is the
// one given last time. An answer is good while g_copies has not gained or lost an entry.
struct CopyFront { const void* p; const CopyEntry* e; uint32_t gen; };
CopyFront g_copyFront[256] = {};
uint32_t g_copiesGen = 1;
// (GXD_TEXMEMO, see ResolveTexture below)
struct TexMemo {
    bool ok = false;
    uint32_t gen = 0, copies = 0;
    uint64_t epoch = 0;
    uint16_t w = 0, h = 0;
    TexBind out;
    TexObj obj;
};
TexMemo g_texMemo[8];
uint32_t g_texMemoGen = 1;
uint64_t g_texMemoHits = 0, g_texMemoCalls = 0;
static_assert(sizeof(TexObj) % 4 == 0, "the texture object compares as words");
inline const CopyEntry* FindCopy(const void* p) {
    CopyFront& f = g_copyFront[(reinterpret_cast<uintptr_t>(p) >> 5) & 255u];
    if (f.gen == g_copiesGen && f.p == p) return f.e;
    auto it = g_copies.find(p);
    f.p = p;
    f.e = it == g_copies.end() ? nullptr : &it->second;
    f.gen = g_copiesGen;
    return f.e;
}
uint32_t g_nextTexId = 1;
uint64_t (*g_genHook)(const void*, size_t) = nullptr;
uint64_t g_tlutHash[20] = {};
uint32_t g_tlutSeen[20] = {};

inline bool IsPow2(uint32_t v) { return v && !(v & (v - 1)); }

// "Has this guest range been written since?" counter; ~0 = unknown (always
// compare the bytes). vgx's own table (4 KiB granules, fed by the HLE layer in
// MSC_VGX builds) when the runtime registered guest RAM, else the runtime's
// hook (64 KiB granules: a per-frame flush anywhere near a texture forces a
// hash of the whole texture every frame).
inline uint64_t Generation(const void* p, uint32_t n) {
    if (HaveGuestRam()) return GuestWriteGeneration(p, n);
    return g_genHook ? g_genHook(p, n) : ~0ull;
}

uint64_t TlutHash(int idx) {
    if (idx < 0 || idx >= 20 || !g.tlut[idx].valid || !g.tlut[idx].obj.data) return 0;
    const TlutObj& t = g.tlut[idx].obj;
    if (g_tlutSeen[idx] != t.version) {
        g_tlutSeen[idx] = t.version;
        g_tlutHash[idx] = XXH3_64bits(t.data, static_cast<size_t>(t.entries) * 2) ^ (static_cast<uint64_t>(t.format) << 60) ^ 1;
    }
    return g_tlutHash[idx];
}

} // namespace

void SetGuestWriteGenerationHook(uint64_t (*fn)(const void*, size_t)) { g_genHook = fn; }
void SetTextureDumpHook(void (*fn)(const TexUpload&, uint32_t, uint64_t)) { g_texDumpHook = fn; }

// GXInvalidateTexAll. On the console it empties the texture cache in front of
// the texture units, and the game calls it every frame. It carries no
// information the write generations do not already give: texture data only
// reached the hardware through RAM, so a changed texture was always flushed
// from the data cache as well, and the runtime reports those flushes. With
// tracked guest RAM the call is therefore not a reason to hash every bound
// texture again (108 textures / 2.8 MB per frame in a match).
//   MSC_VGX_TEXTRUST=0       re-hash after every GXInvalidateTexAll (the old behaviour)
//   MSC_VGX_TEXTRUST=verify  hash anyway and report a texture that changed without a write notification
void InvalidateAllTextures() {
    static const int trust = [] {
        const char* m = std::getenv("MSC_VGX_TEXTRUST");
        return (m && !std::strcmp(m, "verify")) ? 2 : ((m && *m == '0') ? 0 : 1);
    }();
    g_texVerify = trust == 2;
    ++g_texMemoGen;
    if (trust == 0 || !(HaveGuestRam() || g_genHook)) {
        // Contents are re-validated (generation counter / hash) on the next bind.
        for (auto& e : g_cache) { e.second.generation = ~0ull; e.second.checkedEpoch = ~0ull; }
    }
    for (uint32_t& s : g_tlutSeen) s = 0xFFFFFFFFu;
    g.texDirty = true;
    g.texSlotDirty = 0xFF;
}

uint32_t AllocTextureId() { return g_nextTexId++; }

// MSC_VGX_COPYCHECK=1 (diagnostic, see CopyCheck in vgx_draw.cpp): which addresses have been EFB-copy destinations, whether
// the copy was destroyed since (GXDestroyCopyTex: the runtime saw the guest flush or invalidate the range), and how often a
// texture bound at such an address was then taken from guest RAM, where the GPU copy's pixels never are.
namespace {
struct CopyHist { uint64_t lastCopy = 0, destroyedAt = 0, copies = 0; uint16_t w = 0, h = 0; uint32_t fmt = 0; bool live = false; };
std::unordered_map<const void*, CopyHist> g_copyHist;
uint64_t g_ccDestroys = 0, g_ccFallbacks = 0, g_ccHeldDestroys = 0;
inline bool CopyCheckOn() {
    static const bool on = [] {
        const bool b = EnvInt("MSC_VGX_COPYCHECK", 0) != 0;
        return b;
    }();
    return on;
}
} // namespace

void CopyCheckTexReport() {
    std::fprintf(stderr, "[copycheck] %llu copy destinations, %llu destroyed (%llu of them had been copied to in an earlier frame), %llu binds of a destroyed destination fell back to guest RAM\n",
                 static_cast<unsigned long long>(g_copyHist.size()), static_cast<unsigned long long>(g_ccDestroys),
                 static_cast<unsigned long long>(g_ccHeldDestroys), static_cast<unsigned long long>(g_ccFallbacks));
}

uint32_t AcquireCopyTexture(const void* dest, uint16_t w, uint16_t h, uint32_t fmt) {
    CopyEntry& c = g_copies[dest];
    if (!c.texId) { c.texId = g_nextTexId++; ++g_copiesGen; }
    c.w = w; c.h = h; c.fmt = fmt;
    if (CopyCheckOn()) {
        CopyHist& hh = g_copyHist[dest];
        hh.live = true; hh.lastCopy = CurrentFrame(); ++hh.copies; hh.w = w; hh.h = h; hh.fmt = fmt;
    }
    return c.texId;
}

void DestroyCopyTexture(const void* dest) {
    auto it = g_copies.find(dest);
    if (it == g_copies.end()) return;
    if (CopyCheckOn()) {
        CopyHist& hh = g_copyHist[dest];
        ++g_ccDestroys;
        if (hh.lastCopy < CurrentFrame()) ++g_ccHeldDestroys;
        VGX_LOG_N(80, "[copycheck] frame %llu: copy destination %p (%ux%u fmt 0x%X, texture %u) destroyed; last copied to in frame %llu, %llu copies so far\n",
                  static_cast<unsigned long long>(CurrentFrame()), dest, hh.w, hh.h, hh.fmt, it->second.texId, static_cast<unsigned long long>(hh.lastCopy),
                  static_cast<unsigned long long>(hh.copies));
        hh.live = false; hh.destroyedAt = CurrentFrame();
    }
    Pending().cmds.push_back(Cmd{CMD_TEX_FREE, it->second.texId});
    g_copies.erase(it);
    ++g_copiesGen;
    g.texDirty = true;
    g.texSlotDirty = 0xFF;
}

static bool ResolveTextureImpl(int texMap, TexBind& out, uint16_t& width, uint16_t& height) {
    out = TexBind{};
    const TexSlot& slot = g.tex[texMap & 7];
    if (!slot.valid || !slot.obj.data || slot.obj.width == 0 || slot.obj.height == 0) return false;
    const TexObj& o = slot.obj;
    width = o.width;
    height = o.height;
    out.wrapS = o.wrapS > 2 ? WRAP_CLAMP : o.wrapS;
    out.wrapT = o.wrapT > 2 ? WRAP_CLAMP : o.wrapT;
    out.magFilter = o.magFilter ? FILT_LINEAR : FILT_NEAR;
    out.minFilter = o.minFilter > 5 ? FILT_LINEAR : o.minFilter;

    // An EFB copy made to this address wins over the bytes in guest RAM.
    if (const CopyEntry* ce = FindCopy(o.data)) {
        out.texId = ce->texId;
        if (out.minFilter >= FILT_NEAR_MIP_NEAR) out.minFilter = (out.minFilter == FILT_NEAR_MIP_NEAR || out.minFilter == FILT_NEAR_MIP_LIN) ? FILT_NEAR : FILT_LINEAR;
        // Render-target textures are never power-of-two sized in general: clamp only.
        out.wrapS = out.wrapT = WRAP_CLAMP;
        return true;
    }

    if (CopyCheckOn()) {
        auto h = g_copyHist.find(o.data);
        if (h != g_copyHist.end() && !h->second.live) {
            ++g_ccFallbacks;
            VGX_LOG_N(80, "[copycheck] frame %llu: texture %ux%u fmt 0x%X at %p was an EFB copy (%ux%u, last copied to in frame %llu, destroyed in frame %llu): taken from guest RAM\n",
                      static_cast<unsigned long long>(CurrentFrame()), o.width, o.height, o.format, o.data, h->second.w, h->second.h,
                      static_cast<unsigned long long>(h->second.lastCopy), static_cast<unsigned long long>(h->second.destroyedAt));
        }
    }

    const uint32_t fmt = o.format;
    // Mip levels present in guest memory. GLES2 can only mipmap power-of-two
    // textures, and needs the full chain (completed in software below).
    uint32_t levels = 1;
    if (o.mipmap && IsPow2(o.width) && IsPow2(o.height)) {
        const uint32_t full = 32u - static_cast<uint32_t>(__builtin_clz(o.width > o.height ? o.width : o.height)); // 1 + log2 of the longer side
        const float ml = o.maxLod < 0 ? 0 : o.maxLod;
        levels = static_cast<uint32_t>(ml) + 1u;
        if (levels > full) levels = full;
    }
    // MSC_VGX_NOMIP=1 (diagnostic): never use mip levels.
    static const bool noMip = EnvInt("MSC_VGX_NOMIP", 0) != 0;
    const bool wantMips = levels > 1 && out.minFilter >= FILT_NEAR_MIP_NEAR && !noMip;
    if (!wantMips) {
        levels = 1;
        if (out.minFilter >= FILT_NEAR_MIP_NEAR) out.minFilter = (out.minFilter == FILT_NEAR_MIP_NEAR || out.minFilter == FILT_NEAR_MIP_LIN) ? FILT_NEAR : FILT_LINEAR;
    }
    if (!IsPow2(o.width) || !IsPow2(o.height)) {
        // GLES2 / the Vita GPU: non-power-of-two textures only clamp.
        if (out.wrapS != WRAP_CLAMP || out.wrapT != WRAP_CLAMP) stats::Note("non-power-of-two texture with repeat/mirror wrap (clamped)");
        out.wrapS = out.wrapT = WRAP_CLAMP;
    }

    Key key{o.data, static_cast<uint32_t>(o.width) | (static_cast<uint32_t>(o.height) << 16), fmt | (levels << 8), 0};
    const uint8_t* tlut = nullptr;
    uint32_t tlutFmt = 0, tlutEntries = 0;
    if (TexFormatIsCI(fmt)) {
        const uint32_t ti = o.tlut;
        if (ti < 20 && g.tlut[ti].valid) {
            tlut = static_cast<const uint8_t*>(g.tlut[ti].obj.data);
            tlutFmt = g.tlut[ti].obj.format;
            tlutEntries = g.tlut[ti].obj.entries;
            key.tlutHash = TlutHash(static_cast<int>(ti));
        } else {
            VGX_LOG_N(8, "CI texture bound without a loaded TLUT (slot %u)\n", ti);
        }
    }
    Entry& e = CacheEntry(key);
    if (!e.size) e.size = TexDataSize(o.width, o.height, fmt, levels);
    const uint32_t size = e.size;
    e.lastUsed = CurrentFrame();
    bool upload = e.texId == 0;
    // Cheapest path: no write notification at all has arrived since the contents were last
    // checked (read before the generation, so a write that lands in between is seen next time).
    const uint64_t epoch = HaveGuestRam() ? GuestWriteEpoch() : ~0ull;
    if (!upload && !(epoch != ~0ull && e.checkedEpoch == epoch && !g_texVerify)) {
        // Cheap path: the runtime's write-generation counters say nothing in the range changed.
        const uint64_t gen = Generation(o.data, size);
        e.checkedEpoch = gen != ~0ull ? epoch : ~0ull;
        if (g_texVerify && gen != ~0ull && gen == e.generation && XXH3_64bits(o.data, size) != e.contentHash) {
            ++g_texStale;
            VGX_LOG_N(32, "texture cache: %ux%u fmt 0x%X at %p changed without a write notification\n", o.width, o.height, fmt, o.data);
            e.generation = ~0ull;
        }
        if (gen == ~0ull || gen != e.generation) {
            const uint64_t h = XXH3_64bits(o.data, size);
            ++stats::Cur().texHashes;
            stats::Cur().texHashBytes += size;
            if (gen == ~0ull) ++stats::Cur().texUntracked;
            if (h != e.contentHash) { e.contentHash = h; upload = true; }
            e.generation = gen;
        }
    } else if (upload) {
        e.texId = g_nextTexId++;
        e.contentHash = XXH3_64bits(o.data, size);
        e.generation = Generation(o.data, size);
    }
    if (upload) {
        static const bool texLog = EnvInt("MSC_VGX_TEXLOG", 0) != 0;   // diagnostic: what every texture upload was asked for
        if (texLog)
            std::fprintf(stderr, "[texlog] frame %llu upload %ux%u fmt=0x%X mip=%d maxLod=%g minLod=%g bias=%g minFilter=%d wrap=%d,%d levels=%u data=%p\n", static_cast<unsigned long long>(CurrentFrame()),
                         o.width, o.height, fmt, o.mipmap ? 1 : 0, static_cast<double>(o.maxLod), static_cast<double>(o.minLod), static_cast<double>(o.lodBias), static_cast<int>(out.minFilter),
                         static_cast<int>(out.wrapS), static_cast<int>(out.wrapT), levels, o.data);
        const backend::Caps& caps = backend::GetCaps();
        TexUpload up;
        up.texId = e.texId;
        up.width = o.width;
        up.height = o.height;
        const uint8_t* const data = static_cast<const uint8_t*>(o.data);
        // The levels to send. Unless the back end takes a short chain (mipClamp), a
        // mipmapped texture is completed down to 1x1 below (no GL_TEXTURE_MAX_LEVEL in GLES2).
        uint32_t authored = levels;
        // CMPR goes to the back end as DXT1 (an eighth of the memory, no decode) unless
        // a level uses the one thing DXT1 renders differently (see CmprToDxt1).
        //   MSC_VGX_DXT1=0  always decode;  =2  DXT1 for those textures too (their transparent
        //   texels become transparent black: dark fringes where they are filtered)
        static const bool dxtAll = EnvInt("MSC_VGX_DXT1", 1) == 2;
        bool dxt = fmt == 0xE && caps.dxt1;
        // (vitaGL stores compressed textures swizzled; for sizes that are not powers of two it
        // takes another path that has not been read through: those are decoded on the console)
        if (dxt && caps.mipClamp && !(IsPow2(o.width) && IsPow2(o.height))) dxt = false;
        if (dxt) {
            if (caps.mipClamp) { // (a back end with short chains cannot take compressed levels below one block)
                uint32_t lw = o.width, lh = o.height, n = 1;
                while (n < authored && (lw >> 1) >= 4 && (lh >> 1) >= 4) { lw >>= 1; lh >>= 1; ++n; }
                authored = n;
            }
            const uint8_t* src = data;
            uint32_t w = o.width, h = o.height;
            for (uint32_t l = 0; l < authored; ++l) {
                up.levels.emplace_back(Dxt1Size(w, h));
                if (!CmprToDxt1(src, w, h, up.levels.back().data(), dxtAll)) { dxt = false; break; }
                src += TexLevelSize(w, h, fmt);
                w = w > 1 ? w / 2 : 1;
                h = h > 1 ? h / 2 : 1;
            }
            if (!dxt) {
                up.levels.clear();
                authored = levels;
                ++g_cmprDecoded;
            }
        }
        if (dxt) {
            up.format = TF_DXT1;
            if (wantMips && !caps.mipClamp) {
                // Complete the chain: decode the last authored level, halve, encode.
                uint32_t lw = o.width, lh = o.height;
                const uint8_t* src = data;
                for (uint32_t l = 1; l < authored; ++l) { src += TexLevelSize(lw, lh, fmt); lw = lw > 1 ? lw / 2 : 1; lh = lh > 1 ? lh / 2 : 1; }
                std::vector<uint8_t> cur(static_cast<size_t>(lw) * lh * 4), next;
                DecodeTexture(src, lw, lh, fmt, nullptr, 0, 0, cur.data());
                while (lw > 1 || lh > 1) {
                    uint32_t nw, nh;
                    Downsample(cur.data(), lw, lh, next, nw, nh);
                    up.levels.emplace_back(Dxt1Size(nw, nh));
                    EncodeDxt1(next.data(), nw, nh, up.levels.back().data());
                    cur.swap(next);
                    lw = nw; lh = nh;
                }
            }
        } else if (caps.la88 && !o.mipmap && fmt <= 3) {
            up.format = TF_LA88;
            up.levels.emplace_back(static_cast<size_t>(o.width) * o.height * 2);
            DecodeTextureLA88(data, o.width, o.height, fmt, up.levels.back().data());
        } else {
            // RGBA8. A back end that makes its own mip levels (mipGen) only gets level 0.
            up.genMips = wantMips && caps.mipGen;
            if (up.genMips) authored = 1;
            const uint8_t* src = data;
            uint32_t w = o.width, h = o.height;
            for (uint32_t l = 0; l < authored; ++l) {
                up.levels.emplace_back(static_cast<size_t>(w) * h * 4);
                DecodeTexture(src, w, h, fmt, tlut, tlutFmt, tlutEntries, up.levels.back().data());
                src += TexLevelSize(w, h, fmt);
                if (w == 1 && h == 1) break;
                w = w > 1 ? w / 2 : 1;
                h = h > 1 ? h / 2 : 1;
            }
            if (wantMips && !up.genMips && !caps.mipClamp) {
                // Complete the chain down to 1x1.
                uint32_t lw = o.width, lh = o.height;
                for (size_t l = 1; l < up.levels.size(); ++l) { lw = lw > 1 ? lw / 2 : 1; lh = lh > 1 ? lh / 2 : 1; }
                while (lw > 1 || lh > 1) {
                    std::vector<uint8_t> next;
                    uint32_t nw, nh;
                    Downsample(up.levels.back().data(), lw, lh, next, nw, nh);
                    up.levels.push_back(std::move(next));
                    lw = nw; lh = nh;
                }
            }
        }
        up.authored = static_cast<uint8_t>(authored < up.levels.size() ? authored : up.levels.size());
        e.mipmapped = wantMips;
        {
            // Bytes the back end will hold for this texture.
            uint32_t bytes = 0;
            for (const auto& lv : up.levels) bytes += static_cast<uint32_t>(lv.size());
            if (up.genMips) bytes += bytes / 3;
            if (up.format == TF_DXT1) { ++g_dxtUploads; g_dxtBytes += bytes; }
            else if (up.format == TF_LA88) { ++g_laUploads; g_laBytes += bytes; }
            else { ++g_rgbaUploads; g_rgbaBytes += bytes; if (fmt == 0xE) g_cmprRgbaBytes += bytes; }
            ++stats::Cur().texUploads;
            stats::Cur().texUploadBytes += bytes;
            stats::TexLive(e.bytes ? 0 : 1, static_cast<int64_t>(bytes) - e.bytes);
            g_liveBytes += bytes;
            g_liveBytes -= e.bytes;
            e.bytes = bytes;
        }
        // Diagnostic hook of the host layer (MSC_VGX_TEXDUMP on the desktop harness): sees every uploaded texture.
        if (g_texDumpHook) g_texDumpHook(up, fmt, CurrentFrame());
        CmdBuf& buf = Pending();
        buf.uploads.push_back(std::move(up));
        buf.cmds.push_back(Cmd{CMD_TEX_UPLOAD, static_cast<uint32_t>(buf.uploads.size() - 1)});
        stats::TexFormat(fmt, o.width, o.height, wantMips);
    }
    out.texId = e.texId;
    return true;
}

// GXD_TEXMEMO: a texture bound again exactly as it was resolved earlier in the same frame (the game binds the same few textures for every draw: 545 of the 1030
// texture loads of an open-play frame are such repeats, 852 resolutions a frame at about 220 instructions) gives the same answer: the result depends on the
// object's words, on the guest writes (the write-notification count, read BEFORE anything else is read), on the set of EFB copies, on the palette (indexed
// formats are not memoised) and on what the cache holds (a new frame, eviction and GXInvalidateTexAll bump the generation).

bool ResolveTexture(int texMap, TexBind& out, uint16_t& width, uint16_t& height) {
    if (!Gxd(GXD_TEXMEMO) || g_texVerify || !HaveGuestRam() || CopyCheckOn()) return ResolveTextureImpl(texMap, out, width, height);
    const TexSlot& slot = g.tex[texMap & 7];
    TexMemo& tm = g_texMemo[texMap & 7];
    const uint64_t ep = GuestWriteEpoch();
    ++g_texMemoCalls;
    if (tm.ok && tm.epoch == ep && tm.gen == g_texMemoGen && tm.copies == g_copiesGen && SameWords(&tm.obj, &slot.obj, sizeof(TexObj) / 4)) {
        ++g_texMemoHits;
        if (Gxd(GXD_VERIFY)) {
            TexBind o2;
            uint16_t w2 = 0, h2 = 0;
            const bool r2 = ResolveTextureImpl(texMap, o2, w2, h2);
            if (!r2 || w2 != tm.w || h2 != tm.h || o2.texId != tm.out.texId || o2.wrapS != tm.out.wrapS || o2.wrapT != tm.out.wrapT ||
                o2.minFilter != tm.out.minFilter || o2.magFilter != tm.out.magFilter) {
                static int fails = 0;
                if (++fails <= 8) std::fprintf(stderr, "[vgx] GXD VERIFY FAILED: texture memo returned texture %u for a bind that now resolves to %u\n", tm.out.texId, o2.texId);
            }
        }
        out = tm.out;
        width = tm.w;
        height = tm.h;
        return true;
    }
    const bool r = ResolveTextureImpl(texMap, out, width, height);
    tm.ok = r && ep != ~0ull && !TexFormatIsCI(slot.obj.format);
    if (tm.ok) {
        tm.epoch = ep;
        tm.gen = g_texMemoGen;
        tm.copies = g_copiesGen;
        tm.w = width;
        tm.h = height;
        tm.out = out;
        std::memcpy(&tm.obj, &slot.obj, sizeof(TexObj));
    }
    return r;
}

// A memory budget for guest textures. The cache otherwise only grows: every
// texture the game has ever bound stays in the back end (menus, both teams'
// models, every stadium visited), which a 128 MB graphics memory cannot
// afford over a session. When the live total exceeds the budget at the end of
// a frame, the least recently bound textures are freed, oldest first, until
// it fits; a texture bound in the last `kMinIdle` frames is never freed (if
// the working set itself exceeds the budget, the budget loses: thrashing
// uploads would be worse). A freed texture that is bound again is decoded
// from guest RAM again like a new one.
//   MSC_VGX_TEXBUDGET=<MiB>   (default 0: keep everything)   or SetTextureBudget() from the host layer
void SetTextureBudget(uint64_t bytes) {
    g_budget = bytes;
    g_budgetSet = true;
}

void TexCacheEndFrame() {
    ++g_texMemoGen; // (GXD_TEXMEMO: nothing resolved in the frame that ended is trusted in the next)
    static const uint64_t envBudget = static_cast<uint64_t>(EnvInt("MSC_VGX_TEXBUDGET", 0)) << 20;
    const uint64_t budget = g_budgetSet ? g_budget : envBudget;
    if (!budget || g_liveBytes <= budget) return;
    constexpr uint64_t kMinIdle = 30;
    const uint64_t now = CurrentFrame();
    struct Cand {
        uint64_t lastUsed;
        uint32_t texId;
        Key key;
    };
    static std::vector<Cand> cand;
    cand.clear();
    for (const auto& kv : g_cache)
        if (kv.second.texId && kv.second.lastUsed + kMinIdle < now) cand.push_back(Cand{kv.second.lastUsed, kv.second.texId, kv.first});
    // Oldest first; the texture id breaks ties so that the order (and with it the
    // command stream) does not depend on the hash map's iteration order.
    std::sort(cand.begin(), cand.end(), [](const Cand& a, const Cand& b) { return a.lastUsed != b.lastUsed ? a.lastUsed < b.lastUsed : a.texId < b.texId; });
    CmdBuf& buf = Pending();
    for (const Cand& c : cand) {
        if (g_liveBytes <= budget) break;
        auto it = g_cache.find(c.key);
        const uint32_t bytes = it->second.bytes;
        buf.cmds.push_back(Cmd{CMD_TEX_FREE, c.texId});
        stats::TexLive(-1, -static_cast<int64_t>(bytes));
        g_liveBytes -= bytes;
        ++g_evicted;
        g_evictedBytes += bytes;
        ++stats::Cur().texEvicted;
        stats::Cur().texEvictBytes += bytes;
        g_cache.erase(it);
        CacheFrontClear();
    }
    if (g_liveBytes > budget) ++g_overBudgetFrames;
}

void TexCacheReport(FILE* f) {
    std::fprintf(f, "texture cache: %u entries, %.1f MB live; %llu changed without a write notification (MSC_VGX_TEXTRUST=verify)\n", static_cast<unsigned>(g_cache.size()),
                 g_liveBytes / 1048576.0, static_cast<unsigned long long>(g_texStale));
    std::fprintf(f, "texture uploads: %llu as DXT1 (%.1f MB), %llu as RGBA8 (%.1f MB); of the RGBA8 ones %llu are CMPR textures with transparent "
                    "texels (%.1f MB), which DXT1 would render with dark fringes\n",
                 static_cast<unsigned long long>(g_dxtUploads), g_dxtBytes / 1048576.0, static_cast<unsigned long long>(g_rgbaUploads),
                 g_rgbaBytes / 1048576.0, static_cast<unsigned long long>(g_cmprDecoded), g_cmprRgbaBytes / 1048576.0);
    if (g_laUploads) std::fprintf(f, "texture uploads: %llu as LA88 (%.1f MB)\n", static_cast<unsigned long long>(g_laUploads), g_laBytes / 1048576.0);
    if (g_texMemoCalls) std::fprintf(f, "texture memo: %llu of %llu resolutions repeated a bind of the same frame (GXD_TEXMEMO)\n", static_cast<unsigned long long>(g_texMemoHits), static_cast<unsigned long long>(g_texMemoCalls));
    const uint64_t budget = g_budgetSet ? g_budget : (static_cast<uint64_t>(EnvInt("MSC_VGX_TEXBUDGET", 0)) << 20);
    if (budget)
        std::fprintf(f, "texture budget %.0f MB: %llu textures freed (%.1f MB), %llu frames ended over budget (working set larger than the budget)\n",
                     budget / 1048576.0, static_cast<unsigned long long>(g_evicted), g_evictedBytes / 1048576.0,
                     static_cast<unsigned long long>(g_overBudgetFrames));
}

} // namespace vgx
