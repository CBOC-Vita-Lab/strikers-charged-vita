// Front-end texture manager: decides which back-end texture a texmap resolves
// to and queues uploads when the guest bytes are new or have changed.
#include "vgx_guestmem.h"
#include "vgx_cutlearn.h"
#include "vgx_state.h"
#include "vgx_texdecode.h"
#include "vgx_i8churn.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <algorithm>
#include <cmath>
#include <array>
#include <atomic>
#include <mutex>
#include <cstdio>
#include <map>
#include <unordered_map>
#include <vector>

namespace vgx {

const char* ShaderListPath();   // (vgx_draw.cpp)

namespace {

// ---- 1.3 (wave 11, learn16) MSC_CUT_LEARN bit 1: textures first uploaded after kick-off, learned per stadium and shown to the renderer again at the next match of it (vgx_cutlearn.h).
// Front end only (this file's thread): the list, its file and the replay are here; the back end owns the variants. Phase and stadium come from the back end's match ledger.
int CutLearnBits() { static const int b = static_cast<int>(VGX_KNOB(EnvInt("MSC_CUT_LEARN", 0))); return b; }
bool CutLearnTexOn() { return (CutLearnBits() & cutlearn::kBitTex) != 0; }
struct CutLearnTex {
    cutlearn::List list;
    cutlearn::Limits lim;
    cutlearn::Plan plan;
    std::string path;
    uint32_t seenMatch = 0, seenPhase = 0, cursor = 0, touched = 0, there = 0, noAddr = 0, uploaded = 0, recorded = 0, loaded = 0;
    uint64_t kib = 0;
    bool replaying = false;
} g_clt;
std::unordered_map<uint32_t, std::array<uint32_t, 10>> g_cltShown;   // address -> the words of the last pretouch touch (this run: the game's own textures); filled in the loading scene, before the ledger starts the match
std::string CutLearnTexPath(uint32_t stadium) {
    std::string base;
    if (const char* f = std::getenv("MSC_CUT_LEARN_FILE"); f && *f) base = f;
    else if (const char* lp = vgx::ShaderListPath(); lp && *lp) base = std::string(lp) + ".cutlearn";
    if (base.empty() || stadium == 0xFFFFFFFFu) return std::string();
    return base + "." + std::to_string(stadium) + ".tex";
}
// Called from ResolveTextureImpl's upload branch (once per upload, after the texels are known): after kick-off the upload is a first use in a held scene or in play.
void CutLearnTexRecord(const void* data, uint16_t w, uint16_t h, uint32_t fmt, uint64_t hash, uint32_t bytes) {
    if (!CutLearnTexOn() || cutlearn::g_phase.load(std::memory_order_acquire) != 2 || g_clt.path.empty()) return;
    const cutlearn::TexRec r{hash, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(data)), w, h, static_cast<uint8_t>(fmt & 255u), (bytes + 1023u) / 1024u};
    if (!g_clt.list.AddTex(r, g_clt.lim)) return;
    ++g_clt.recorded;
    if (FILE* f = std::fopen(g_clt.path.c_str(), "a")) { const std::string ln = cutlearn::FormatTex(r); std::fwrite(ln.data(), 1, ln.size(), f); std::fclose(f); }
    std::fprintf(stderr, "[cutlearn] texture %ux%u fmt 0x%X (%u KiB) first used after kick-off: written for the next match of stadium %u (%u in the list)\n", w, h, fmt, r.kib, cutlearn::g_stadium.load(), static_cast<unsigned>(g_clt.list.tex.size()));
}

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
    bool hd = false;       // 1.1: the back end holds an HD replacement (SetTextureReplaceHook), `bytes` of it
    uint32_t hdWait = 0;   // 1.1: 1 + the replacement being read for this texture (0: none): uploaded again once it is in
    i8churn::State churn;  // walk15 (MSC_VGX_I8CHURN): how often this texture was uploaded in a row
    uint8_t imFmt = 0xFF;  // tile24 census: GX format 0..3 (I4, I8, IA4, IA8) of a mipmapped upload, else 0xFF
    uint32_t imRgbaEq = 0; // tile24 census: what the texture would hold as RGBA8 with its mip chain
    bool pinned = false;   // 1.3 (MSC_TEX_PRETOUCH=2): uploaded ahead of its first use for the match; the budget's trim leaves it until the match ends (TexTouch mode 2)
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
CacheFront g_cacheFront[1024] = {};   // (1.1, from the Galaxy port: 1024 classes with a multiplicative hash; 128 collided in a match)
inline Entry& CacheEntry(const Key& key) {
    CacheFront& f = g_cacheFront[(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(key.data) >> 5) * 2654435761u) >> 22];
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
int (*g_replaceHook)(const TexReplaceQuery&, TexUpload&, uint32_t*) = nullptr;   // 1.1: HD character textures (vgx_hdtex.cpp)
bool (*g_replaceReady)(uint32_t) = nullptr;
uint8_t g_replaceMipFilter = FILT_LIN_MIP_NEAR;
// 1.3 mball24 MSC_BALL_FINS=0|1 (default 1; render only): the white spikes round a Mega Strike ball (the "charge" glow, nis_ball/charge4 and its kin) are ONE 32x32 CMPR texture with
// on/off-like alpha, drawn alpha-tested and magnified 5 to 8 times on the console's 960x544 screen: its contour is a staircase of texels, which is what "the ball looks low resolution"
// was (the ball's colour map is 256x256, and the HD pack leaves the charge glow out). The texture is recognised by the hash of its texels and given 128x128 (4x, Catmull-Rom on
// premultiplied colour, alpha kept soft so the alpha test draws a smooth contour), with its own mip chain to 1x1. Only that texture: nothing else in the game is touched.
// MSC_BALL_FINS_LOG=1 logs the hash of every 32x32 CMPR upload (to add a texture to the list).
const uint64_t kFinHashes[] = {0xa43cbd668e5f274bull};   // (the 32x32 spike texture of the Mega Strike ball: MSC_BALL_FINS_LOG=1 lists the hashes of every 32x32 CMPR upload)
inline int BallFinsMode() { static const int m = static_cast<int>(EnvInt("MSC_BALL_FINS", 1)); return m; }   // 0 off, 1 the 128x128 upscale, 2 also the soft edge (the back end draws that texture in two passes; its alpha is a steep ramp)
inline bool BallFinsOn() { return BallFinsMode() != 0; }
bool IsFinHash(uint64_t h) { for (uint64_t k : kFinHashes) if (k && k == h) return true; return false; }
inline float CatmullRom(float t, float p0, float p1, float p2, float p3) {
    return 0.5f * ((2.0f * p1) + (-p0 + p2) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t * t + (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t * t * t);
}
// level 0 of a 32x32 RGBA8 -> 128x128 RGBA8 (4x). The texture's alpha is on/off (an ellipse drawn in whole texels): the alpha is low-passed (binomial 5 taps, about one texel) so the
// alpha test's contour is a smooth curve at the old edge, then Catmull-Rom; the colour of transparent texels is bled in from their opaque neighbours (no dark fringe), then Catmull-Rom.
void FinUpscale(const uint8_t* src, uint32_t w, uint32_t h, std::vector<uint8_t>& dst, uint32_t scale, float steep) {
    const uint32_t W = w * scale, H = h * scale;
    const size_t n = static_cast<size_t>(w) * h;
    std::vector<float> col(n * 3), al(n), alb(n), tmp(n);
    std::vector<uint8_t> have(n);
    for (size_t i = 0; i < n; ++i) {
        al[i] = src[i * 4 + 3] / 255.0f;
        have[i] = src[i * 4 + 3] >= 128;
        for (int c = 0; c < 3; ++c) col[i * 3 + c] = src[i * 4 + c] / 255.0f;
    }
    for (int pass = 0; pass < 4; ++pass) {   // bleed the colour outwards
        std::vector<uint8_t> hv2 = have;
        std::vector<float> c2 = col;
        for (int y = 0; y < static_cast<int>(h); ++y)
            for (int x = 0; x < static_cast<int>(w); ++x) {
                const size_t i = static_cast<size_t>(y) * w + x;
                if (have[i]) continue;
                float sum[3] = {0, 0, 0};
                int cnt = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int xx = x + dx, yy = y + dy;
                        if (xx < 0 || yy < 0 || xx >= static_cast<int>(w) || yy >= static_cast<int>(h)) continue;
                        const size_t j = static_cast<size_t>(yy) * w + xx;
                        if (!have[j]) continue;
                        for (int c = 0; c < 3; ++c) sum[c] += col[j * 3 + c];
                        ++cnt;
                    }
                if (cnt) { for (int c = 0; c < 3; ++c) c2[i * 3 + c] = sum[c] / cnt; hv2[i] = 1; }
            }
        have.swap(hv2);
        col.swap(c2);
    }
    static const float k5[5] = {1.0f / 16, 4.0f / 16, 6.0f / 16, 4.0f / 16, 1.0f / 16};
    auto cl = [&](int v, int m) { return v < 0 ? 0 : (v >= m ? m - 1 : v); };
    for (int y = 0; y < static_cast<int>(h); ++y)
        for (int x = 0; x < static_cast<int>(w); ++x) {
            float s2 = 0;
            for (int k = -2; k <= 2; ++k) s2 += k5[k + 2] * al[static_cast<size_t>(y) * w + cl(x + k, static_cast<int>(w))];
            tmp[static_cast<size_t>(y) * w + x] = s2;
        }
    for (int y = 0; y < static_cast<int>(h); ++y)
        for (int x = 0; x < static_cast<int>(w); ++x) {
            float s2 = 0;
            for (int k = -2; k <= 2; ++k) s2 += k5[k + 2] * tmp[static_cast<size_t>(cl(y + k, static_cast<int>(h))) * w + x];
            alb[static_cast<size_t>(y) * w + x] = s2;
        }
    for (int rep = 0; rep < 1; ++rep) {   // (a second pass of the same blur: the spikes stretch the texture, so one texel is 6 to 10 screen pixels along them)
        for (int y = 0; y < static_cast<int>(h); ++y)
            for (int x = 0; x < static_cast<int>(w); ++x) {
                float s2 = 0;
                for (int k = -2; k <= 2; ++k) s2 += k5[k + 2] * alb[static_cast<size_t>(y) * w + cl(x + k, static_cast<int>(w))];
                tmp[static_cast<size_t>(y) * w + x] = s2;
            }
        for (int y = 0; y < static_cast<int>(h); ++y)
            for (int x = 0; x < static_cast<int>(w); ++x) {
                float s2 = 0;
                for (int k = -2; k <= 2; ++k) s2 += k5[k + 2] * tmp[static_cast<size_t>(cl(y + k, static_cast<int>(h))) * w + x];
                alb[static_cast<size_t>(y) * w + x] = s2;
            }
    }
    auto at = [&](int x, int y, int c) { return c < 3 ? col[(static_cast<size_t>(cl(y, static_cast<int>(h))) * w + cl(x, static_cast<int>(w))) * 3 + c] : alb[static_cast<size_t>(cl(y, static_cast<int>(h))) * w + cl(x, static_cast<int>(w))]; };
    dst.assign(static_cast<size_t>(W) * H * 4, 0);
    for (uint32_t Y = 0; Y < H; ++Y) {
        const float fy = (Y + 0.5f) / scale - 0.5f;
        const int y0 = static_cast<int>(std::floor(fy));
        const float ty = fy - y0;
        for (uint32_t X = 0; X < W; ++X) {
            const float fx = (X + 0.5f) / scale - 0.5f;
            const int x0 = static_cast<int>(std::floor(fx));
            const float tx = fx - x0;
            uint8_t* o = &dst[(static_cast<size_t>(Y) * W + X) * 4];
            for (int c = 0; c < 4; ++c) {
                float cv[4];
                for (int j = 0; j < 4; ++j) cv[j] = CatmullRom(tx, at(x0 - 1, y0 - 1 + j, c), at(x0, y0 - 1 + j, c), at(x0 + 1, y0 - 1 + j, c), at(x0 + 2, y0 - 1 + j, c));
                float v = CatmullRom(ty, cv[0], cv[1], cv[2], cv[3]);
                if (c == 3 && steep > 0.0f) v = (v - 0.5f) * steep + 0.5f;   // (MSC_BALL_FINS=2: the smooth contour as a steep ramp about a screen pixel or two wide: the coverage of the soft pass)
                v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
                o[c] = static_cast<uint8_t>(v * 255.0f + 0.5f);
            }
        }
    }
}
uint64_t g_hdLive = 0, g_hdUploads = 0, g_hdBytes = 0, g_hdFreed = 0;
uint64_t g_hdBudget = 0;   // the replacement hook's budget: past half of it, replacements unused for 10 s are freed
uint64_t g_texStale = 0;
uint64_t g_liveBytes = 0;          // bytes of all guest textures held by the back end
uint64_t g_budget = 0;             // 0 = keep everything
bool g_budgetSet = false;
uint64_t g_evicted = 0, g_evictedBytes = 0, g_overBudgetFrames = 0;
// Uploads by what the back end is handed (totals over the run, for the report).
uint64_t g_dxtUploads = 0, g_dxtBytes = 0, g_rgbaUploads = 0, g_rgbaBytes = 0, g_cmprDecoded = 0, g_cmprRgbaBytes = 0;
uint64_t g_laUploads = 0, g_laBytes = 0, g_i8Uploads = 0, g_i8Bytes = 0;   // (1.3: TF_I8, MSC_VGX_LA88=2)
uint64_t g_dxt5Uploads = 0, g_dxt5Bytes = 0, g_t16Uploads = 0, g_t16Bytes = 0;   // 1.3: MSC_VGX_CMPRA, MSC_VGX_TEX16
uint64_t g_cmpraKept = 0, g_t16Small = 0;   // 1.3: MSC_VGX_CMPRA=2: textures left decoded because the middle colour would move too far (and the bytes they hold); TEX16 candidates under MSC_VGX_TEX16_MIN
uint32_t g_cmpraWorst = 0;   // the largest middle-colour error among the textures sent as DXT5
uint64_t g_laMipUploads = 0;   // tile24: mipmapped I/IA textures sent as LA88 / one byte with a generated chain (MSC_VGX_LA88=3)
uint64_t g_imUploads[4] = {}, g_imBytes[4] = {}, g_imNoMipUploads[4] = {}, g_imNoMipBytes[4] = {};   // tile24 census: I4, I8, IA4, IA8 uploads that are mipmapped (wantMips) / carry the mip flag but sample without levels, and their RGBA8 bytes as held now
uint64_t g_imLive[4] = {}, g_imLiveN[4] = {};                  // tile24: live (held) such textures and the bytes held, by GX format, mipmapped only
bool g_laSelfDone = false, g_laSelfOk = true;
uint64_t g_fmtUploads[16] = {}, g_fmtBytes[16] = {};   // by GX format: uploads and what the back end holds for them (TexCacheReport)
// tile24 (as Galaxy's startup self-test): the first time an I/IA texture would take the LA88 / one-byte path, the two decoders are run over synthetic data and compared texel by
// texel: the LA88 pair must be (R, A) of the RGBA8 decode with R = G = B (GX replicates the intensity into the three colours; I4 and I8 into alpha too, which is what lets
// TF_I8 hold one byte). Sizes: whole tiles and a clipped one, every GX format 0..3. Any difference turns the path off (RGBA8 again) with one line on stderr.
bool LaSelfTestOk() {
    if (g_laSelfDone) return g_laSelfOk;
    g_laSelfDone = true;
    static const uint32_t kSize[2][2] = {{16, 16}, {10, 6}};
    uint32_t seed = 0x2545F491u;
    unsigned bad = 0, texels = 0;
    for (uint32_t fmt = 0; fmt < 4; ++fmt)
        for (const auto& sz : kSize) {
            const uint32_t w = sz[0], h = sz[1];
            std::vector<uint8_t> src(TexLevelSize(w, h, fmt) + 64), rgba(static_cast<size_t>(w) * h * 4), la(static_cast<size_t>(w) * h * 2);
            for (uint8_t& b : src) { seed = seed * 1664525u + 1013904223u; b = static_cast<uint8_t>(seed >> 24); }
            DecodeTexture(src.data(), w, h, fmt, nullptr, 0, 0, rgba.data());
            if (!DecodeTextureLA88(src.data(), w, h, fmt, la.data())) { ++bad; continue; }
            for (size_t i = 0, n = static_cast<size_t>(w) * h; i < n; ++i, ++texels) {
                const uint8_t* c = &rgba[i * 4];
                if (c[0] != c[1] || c[0] != c[2] || la[i * 2] != c[0] || la[i * 2 + 1] != c[3] || (fmt <= 1 && c[3] != c[0])) ++bad;
            }
        }
    g_laSelfOk = bad == 0;
    std::fprintf(stderr, "[vgx] MSC_VGX_LA88 self-test: %u texels over I4, I8, IA4, IA8: %s\n", texels, bad ? "MISMATCH, intensity textures stay RGBA8" : "exact");
    return g_laSelfOk;
}
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
// (MSC_VGX_TEXMEMO, see ResolveTexture below) Bumped when an entry of g_cache goes: the binding memo holds pointers to entries.
uint32_t g_cacheEraseGen = 1;
// What ResolveTextureImpl used for its answer, for the binding memo: 0 none, 1 an EFB copy, 2 the cache entry g_implEntry.
Entry* g_implEntry = nullptr;
uint64_t g_uploadSerial = 0; // uploads made so far (TexTouch tells its own from a texture that was already in)
bool g_pinNew = false;   // the entry TexTouch just resolved was pinned by it (not already pinned)
bool g_touchPin = false; // TexTouch is resolving with the pin on: the entry ResolveTextureImpl finds or makes is pinned
int g_implKind = 0;
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
std::atomic<uint32_t> g_ballFinTexIds[4] = {{0}, {0}, {0}, {0}};   // (MSC_BALL_FINS) up to 4 live texture ids of the spike texture, read by the back end
std::atomic<uint32_t> g_ballFinTexNext{0};
void BallFinNoteId(uint32_t id, bool isFin) {
    if (!id) return;
    if (!isFin) {
        for (int i = 0; i < 4; ++i)
            if (g_ballFinTexIds[i].load(std::memory_order_relaxed) == id) g_ballFinTexIds[i].store(0, std::memory_order_relaxed);
        return;
    }
    for (int i = 0; i < 4; ++i)
        if (g_ballFinTexIds[i].load(std::memory_order_relaxed) == id) return;
    for (int i = 0; i < 4; ++i)
        if (g_ballFinTexIds[i].load(std::memory_order_relaxed) == 0) { g_ballFinTexIds[i].store(id, std::memory_order_relaxed); return; }
    g_ballFinTexIds[g_ballFinTexNext.fetch_add(1, std::memory_order_relaxed) & 3u].store(id, std::memory_order_relaxed);
}

void SetGuestWriteGenerationHook(uint64_t (*fn)(const void*, size_t)) { g_genHook = fn; }
void SetTextureDumpHook(void (*fn)(const TexUpload&, uint32_t, uint64_t)) { g_texDumpHook = fn; }
void SetTextureReplaceHook(int (*fn)(const TexReplaceQuery&, TexUpload&, uint32_t*), uint8_t mipFilter, bool (*ready)(uint32_t)) {
    g_replaceHook = fn;
    g_replaceReady = ready;
    g_replaceMipFilter = mipFilter;
}
uint64_t ReplacedTextureBytes() { return g_hdLive; }
void SetReplacedTextureBudget(uint64_t bytes) { g_hdBudget = bytes; }

// GXInvalidateTexAll. On the console it empties the texture cache in front of
// the texture units, and the game calls it every frame. It carries no
// information the write generations do not already give: texture data only
// reached the hardware through RAM, so a changed texture was always flushed
// from the data cache as well, and the runtime reports those flushes. With
// tracked guest RAM the call is therefore not a reason to hash every bound
// texture again (108 textures / 2.8 MB per frame in a match).
//   MSC_VGX_TEXTRUST=0       re-hash after every GXInvalidateTexAll (the old behaviour)
//   MSC_VGX_TEXTRUST=verify  hash anyway and report a texture that changed without a write notification
int TexSameMode() {
    const int m = static_cast<int>(VGX_KNOB(EnvInt("MSC_VGX_TEXSAME", 0)));
    return m;
}
TexSameCounters g_texSame;
void InvalidateAllTextures() {
    const int trust = static_cast<int>(VGX_KNOB([] {
        const char* m = std::getenv("MSC_VGX_TEXTRUST");
        return (m && !std::strcmp(m, "verify")) ? 2 : ((m && *m == '0') ? 0 : 1);
    }()));
    g_texVerify = trust == 2;
    ++g_texMemoGen;
    if (trust == 0 || !(HaveGuestRam() || g_genHook)) {
        // Contents are re-validated (generation counter / hash) on the next bind.
        for (auto& e : g_cache) { e.second.generation = ~0ull; e.second.checkedEpoch = ~0ull; }
    }
    for (uint32_t& s : g_tlutSeen) s = 0xFFFFFFFFu;
    g.texDirty = true;
    g.texSlotDirty = 0xFF; g_texSame.verifySlots = 0;
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
    const bool on = static_cast<bool>(VGX_KNOB([] {
        const bool b = EnvInt("MSC_VGX_COPYCHECK", 0) != 0;
        return b;
    }()));
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
    g.texSlotDirty = 0xFF; g_texSame.verifySlots = 0;
}

static bool ResolveTextureImpl(int texMap, TexBind& out, uint16_t& width, uint16_t& height) {
    out = TexBind{};
    g_implKind = 0;
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
        g_implKind = 1;
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
    const bool noMip = static_cast<bool>(VGX_KNOB(EnvInt("MSC_VGX_NOMIP", 0) != 0));
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
    g_implKind = 2;
    g_implEntry = &e;
    if (g_touchPin && !e.pinned) { e.pinned = true; g_pinNew = true; }
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
    // 1.1: an HD replacement that was being read is in: the same texture again, with it.
    if (!upload && e.hdWait && g_replaceReady && g_replaceReady(e.hdWait - 1)) upload = true;
    if (upload) {
        ++g_uploadSerial;
        { char nm[48]; std::snprintf(nm, sizeof nm, "tex:%ux%u/f%X", o.width, o.height, fmt); FirstUseNote(nm); }
        const bool texLog = static_cast<bool>(VGX_KNOB(EnvInt("MSC_VGX_TEXLOG", 0) != 0));   // diagnostic: what every texture upload was asked for
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
        // walk15 MSC_VGX_I8CHURN: noted at every upload of an I8 texture (a plain one: no mip levels, no palette); it takes the one-byte form from its minRun-th upload in a row
        const bool churnI8 = caps.i8churn && fmt == 1 && !o.mipmap && levels == 1 && i8churn::Shape(o.width, o.height) &&
                             i8churn::Note(e.churn, CurrentFrame(), static_cast<uint32_t>(VGX_KNOB(EnvInt("MSC_VGX_I8CHURN_RUN", 3))), static_cast<uint32_t>(VGX_KNOB(EnvInt("MSC_VGX_I8CHURN_WINDOW", 4))));
        // The levels to send. Unless the back end takes a short chain (mipClamp), a
        // mipmapped texture is completed down to 1x1 below (no GL_TEXTURE_MAX_LEVEL in GLES2).
        uint32_t authored = levels;
        // A replacement image (vgx_hdtex.cpp: HD character textures), asked only here, at upload.
        if (e.hd) { g_hdLive -= e.bytes; e.hd = false; }
        bool hd = false;
        if (g_replaceHook) {
            const TexReplaceQuery q{data, fmt, o.width, o.height, tlut, tlut ? (tlutEntries == 0xFFFF ? 32768u : tlutEntries * 2u) : 0u, caps.mipClamp, g_hdLive};
            uint32_t wait = 0;
            const bool arrival = e.hdWait != 0;   // (MSC_HDTEX_FADE: the replacement was being read when the object was first seen)
            const int r = g_replaceHook(q, up, &wait);
            hd = r == 1;
            up.hdArrive = hd && arrival;
            e.hdWait = r == 2 ? wait + 1 : 0;
            if (hd) authored = static_cast<uint32_t>(up.levels.size());
        }
        bool fin = false;   // (mball24 MSC_BALL_FINS)
        if (!hd && fmt == 0xE && o.width == 32 && o.height == 32) {
            static const bool logHash = EnvInt("MSC_BALL_FINS_LOG", 0) != 0;
            if (logHash) std::fprintf(stderr, "[ballfins] frame %llu 32x32 CMPR upload hash %016llx levels %u\n", static_cast<unsigned long long>(CurrentFrame()), static_cast<unsigned long long>(e.contentHash), levels);
            if (BallFinsOn() && IsFinHash(e.contentHash)) {
                std::vector<uint8_t> base(32u * 32u * 4u);
                DecodeTexture(data, 32, 32, fmt, nullptr, 0, 0, base.data());
                std::vector<uint8_t> big;
                FinUpscale(base.data(), 32, 32, big, 4, BallFinsMode() >= 2 ? 10.0f : 0.0f);
                BallFinNoteId(e.texId, true);
                up.levels.clear();
                up.levels.push_back(std::move(big));
                uint32_t lw = 128, lh = 128;
                while (lw > 1 || lh > 1) {
                    std::vector<uint8_t> next;
                    uint32_t nw, nh;
                    Downsample(up.levels.back().data(), lw, lh, next, nw, nh);
                    up.levels.push_back(std::move(next));
                    lw = nw; lh = nh;
                }
                up.width = 128;
                up.height = 128;
                up.format = TF_RGBA8;
                authored = static_cast<uint32_t>(up.levels.size());
                fin = true;
                static std::atomic<int> logged{0};
                if (logged.fetch_add(1) < 4) std::fprintf(stderr, "[ballfins] the Mega Strike ball's 32x32 spike texture (hash %016llx) is given as 128x128 (MSC_BALL_FINS=0 turns it off)\n", static_cast<unsigned long long>(e.contentHash));
            }
        }
        if (!fin) BallFinNoteId(e.texId, false);
        // CMPR goes to the back end as DXT1 (an eighth of the memory, no decode) unless
        // a level uses the one thing DXT1 renders differently (see CmprToDxt1).
        //   MSC_VGX_DXT1=0  always decode;  =2  DXT1 for those textures too (their transparent
        //   texels become transparent black: dark fringes where they are filtered)
        const bool dxtAll = static_cast<bool>(VGX_KNOB(EnvInt("MSC_VGX_DXT1", 1) == 2));
        // MSC_VGX_CMPRA=1 (1.3, default 0: colours can move a little): those textures go as DXT5 (CmprToDxt5) instead of being decoded: 8 bits per texel instead of 32.
        // Only where the chain needs no completing (Caps::mipClamp: the console) or there is no chain.
        // MSC_VGX_CMPRA=2 (1.3): the same, but a texture goes as DXT5 only when no texel of any authored level moves by more than MSC_VGX_CMPRA_TOL steps of 255 in a colour channel
        // (default 12; 0: only textures whose three-colour blocks never show their middle colour), the others are decoded as before. Alpha is exact either way.
        const long cmpraMode = static_cast<long>(VGX_KNOB(EnvInt("MSC_VGX_CMPRA", 0)));
        const bool cmprA = cmpraMode != 0;
        bool dxt5 = false;
        bool dxt = !hd && !fin && fmt == 0xE && caps.dxt1;
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
                if (!CmprToDxt1(src, w, h, up.levels.back().data(), dxtAll)) {
                    dxt = false;
                    dxt5 = cmprA && (caps.mipClamp || !wantMips);
                    if (dxt5 && cmpraMode >= 2) {
                        const uint32_t tol = static_cast<uint32_t>(VGX_KNOB(EnvInt("MSC_VGX_CMPRA_TOL", 12)));
                        uint32_t worst = 0, lw = o.width, lh = o.height;
                        const uint8_t* ls = data;
                        for (uint32_t k = 0; k < authored; ++k) {
                            const uint32_t e = CmprDxt5MaxError(ls, lw, lh);
                            if (e > worst) worst = e;
                            ls += TexLevelSize(lw, lh, fmt);
                            lw = lw > 1 ? lw / 2 : 1;
                            lh = lh > 1 ? lh / 2 : 1;
                        }
                        if (worst > tol) { dxt5 = false; ++g_cmpraKept; }
                        else if (worst > g_cmpraWorst) g_cmpraWorst = worst;
                    }
                    break;
                }
                src += TexLevelSize(w, h, fmt);
                w = w > 1 ? w / 2 : 1;
                h = h > 1 ? h / 2 : 1;
            }
            if (dxt5) {
                up.levels.clear();
                src = data;
                w = o.width; h = o.height;
                for (uint32_t l = 0; l < authored; ++l) {
                    up.levels.emplace_back(Dxt5Size(w, h));
                    CmprToDxt5(src, w, h, up.levels.back().data());
                    src += TexLevelSize(w, h, fmt);
                    w = w > 1 ? w / 2 : 1;
                    h = h > 1 ? h / 2 : 1;
                }
            } else if (!dxt) {
                up.levels.clear();
                authored = levels;
                ++g_cmprDecoded;
            }
        }
        if (hd || fin) {
            // (up was filled by the hook: its own mip chain, sampled with a mip filter below)
        } else if (dxt5) {
            up.format = TF_DXT5;
        } else if (dxt) {
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
        } else if (churnI8) {
            // walk15 (MSC_VGX_I8CHURN): the one-byte form, written straight from the tiles (no LA88 pair in between)
            up.levels.emplace_back(static_cast<size_t>(o.width) * o.height);
            DecodeTextureI8(data, o.width, o.height, up.levels.back().data());
            up.format = TF_I8;
        } else if (caps.la88 && LaSelfTestOk() && fmt <= 3 && (!wantMips || (caps.laMips && caps.mipGen))) {
            // (tile24: `!wantMips` is any texture sampled without mip levels, also one whose mipmap flag is set; before 1.3 wave 24 only !o.mipmap took this path.
            //  MSC_VGX_LA88=3, a back end that makes its own chain (Caps::mipGen, the console): a mipmapped I/IA texture is sent as level 0 in LA88/one byte and the chain is
            //  generated by the driver, as the RGBA8 one is today.)
            up.genMips = wantMips;
            if (wantMips) { authored = 1; ++g_laMipUploads; }
            up.levels.emplace_back(static_cast<size_t>(o.width) * o.height * 2);
            DecodeTextureLA88(data, o.width, o.height, fmt, up.levels.back().data());
            if (caps.i8 && fmt <= 1) {
                // 1.3 (MSC_VGX_LA88=2): GX I4 and I8 have luminance = alpha: one byte a texel, sampled as (I,I,I,I) (vitaGL's U8_RRRR): the same four numbers as the RGBA8 decode, a quarter of its bytes.
                std::vector<uint8_t>& lv = up.levels.back();
                const size_t n = static_cast<size_t>(o.width) * o.height;
                for (size_t i = 0; i < n; ++i) lv[i] = lv[i * 2];
                lv.resize(n);
                up.format = TF_I8;
            } else {
                up.format = TF_LA88;
            }
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
            // MSC_VGX_TEX16 (1.3, default 0): a decoded texture whose texels all fit a 16-bit format goes in that format, half the memory and half the texture reads' traffic
            // (RGB565 always, RGB5A3 without translucent texels or with only such, I4 and small palettes often: decided from the texels, whatever the GX format).
            // 1 = textures without mipmaps; 2 = mipmapped too (their generated levels are then rounded to the same bits). The GPU expands 5 bits to c / 31, not to
            // the 8-bit replica over 255: under half a step of 255, so an output can differ by one level at most.
            const long tex16 = static_cast<long>(VGX_KNOB(EnvInt("MSC_VGX_TEX16", 0)));
            // MSC_VGX_TEX16_MIN=<texels> (default 16384, 128 x 128): smaller textures (HUD pieces, icons, glyph pages, most faces) stay 32 bits whatever their texels say: they hold few bytes.
            const uint32_t tex16Min = static_cast<uint32_t>(VGX_KNOB(EnvInt("MSC_VGX_TEX16_MIN", 16384)));
            if (tex16 && static_cast<uint32_t>(o.width) * o.height < tex16Min) ++g_t16Small;
            else if (tex16 && (tex16 >= 2 || !wantMips)) {
                const uint8_t f16 = Rgba16Format(up.levels, !up.genMips);
                if (f16) {
                    for (std::vector<uint8_t>& lv : up.levels) PackRgba16(lv, f16);
                    up.format = f16;
                }
            }
        }
        up.authored = static_cast<uint8_t>(authored < up.levels.size() ? authored : up.levels.size());
        e.mipmapped = wantMips || hd || fin;
        {
            // Bytes the back end will hold for this texture.
            uint32_t bytes = 0;
            for (const auto& lv : up.levels) bytes += static_cast<uint32_t>(lv.size());
            if (up.genMips) bytes += bytes / 3;
            if (hd) { ++g_hdUploads; g_hdBytes += bytes; g_hdLive += bytes; e.hd = true; }
            else if (up.format == TF_DXT1) { ++g_dxtUploads; g_dxtBytes += bytes; }
            else if (up.format == TF_DXT5) { ++g_dxt5Uploads; g_dxt5Bytes += bytes; }
            else if (up.format == TF_I8) { ++g_i8Uploads; g_i8Bytes += bytes; }
            else if (up.format >= TF_RGB565) { ++g_t16Uploads; g_t16Bytes += bytes; }
            else if (up.format == TF_LA88) { ++g_laUploads; g_laBytes += bytes; }
            else { ++g_rgbaUploads; g_rgbaBytes += bytes; if (fmt == 0xE) g_cmprRgbaBytes += bytes; }
            ++g_fmtUploads[fmt & 15u];
            e.imFmt = 0xFF;
            if (fmt <= 3 && !hd) {   // tile24 census: intensity textures, mipmapped or not sampled with levels
                const uint64_t eq = static_cast<uint64_t>(o.width) * o.height * 4u * (wantMips ? 4u : 3u) / 3u;   // (RGBA8, with the chain when mipmapped)
                if (wantMips) { ++g_imUploads[fmt]; g_imBytes[fmt] += eq; e.imFmt = static_cast<uint8_t>(fmt); e.imRgbaEq = static_cast<uint32_t>(eq); }
                else if (o.mipmap) { ++g_imNoMipUploads[fmt]; g_imNoMipBytes[fmt] += static_cast<uint64_t>(o.width) * o.height * 4u; }
            }
            g_fmtBytes[fmt & 15u] += bytes;
            ++stats::Cur().texUploads;
            stats::Cur().texUploadBytes += bytes;
            stats::TexLive(e.bytes ? 0 : 1, static_cast<int64_t>(bytes) - e.bytes);
            g_liveBytes += bytes;
            g_liveBytes -= e.bytes;
            e.bytes = bytes;
            CutLearnTexRecord(o.data, o.width, o.height, fmt, e.contentHash, bytes);   // (MSC_CUT_LEARN bit 1)
        }
        // Diagnostic hook of the host layer (MSC_VGX_TEXDUMP on the desktop harness): sees every uploaded texture.
        if (g_texDumpHook) g_texDumpHook(up, fmt, CurrentFrame());
        CmdBuf& buf = Pending();
        buf.uploads.push_back(std::move(up));
        buf.cmds.push_back(Cmd{CMD_TEX_UPLOAD, static_cast<uint32_t>(buf.uploads.size() - 1)});
        stats::TexFormat(fmt, o.width, o.height, wantMips);
    }
    out.texId = e.texId;
    // A replacement is twice the original's size with a mip chain whose second level is the original's size: sampled with mips, so that
    // wherever the game draws the texture at or below its original resolution (most of the time) no more texels are fetched than before,
    // and only close-ups read the larger level.
    if (e.hd && out.minFilter < FILT_NEAR_MIP_NEAR) out.minFilter = out.minFilter == FILT_NEAR ? FILT_NEAR_MIP_NEAR : g_replaceMipFilter;
    return true;
}

// GXD_TEXMEMO: a texture bound again exactly as it was resolved earlier in the same frame (the game binds the same few textures for every draw: 545 of the 1030
// texture loads of an open-play frame are such repeats, 852 resolutions a frame at about 220 instructions) gives the same answer: the result depends on the
// object's words, on the guest writes (the write-notification count, read BEFORE anything else is read), on the set of EFB copies, on the palette (indexed
// formats are not memoised) and on what the cache holds (a new frame, eviction and GXInvalidateTexAll bump the generation).

// ------------------------------------------------------------------ binding memo (MSC_VGX_TEXMEMO)
// (from the Galaxy port's GXV_TEXMEMO) The memo above (GXD_TEXMEMO) holds one answer per texture unit for the length of a frame, and the game
// binds unit 0 with A, B, A, B: it caught next to nothing (measured neutral). This one is a table of 512 answers keyed by the texture object's
// words the resolution reads (address, size, format, wrap, filters, mip levels, palette number), good across frames while the set of EFB
// copies and the set of cache entries are unchanged (generation counters). What a bind used to cost the full path (an EFB-copy test, the
// cache lookup, the level and wrap arithmetic: ~220 instructions, 850 times a frame) is a hash, a compare and the same check of the
// texture's contents the full path makes on every use (the write epoch, else the write generation of its bytes): a texture whose bytes
// changed, one that is not uploaded yet, a replacement being read, a colour-indexed one (its palette) and anything the verification modes of
// the cache ask for take the full path, which also remembers the answer.
//   MSC_VGX_TEXMEMO=1 (default) this memo, =0 off (GXD_TEXMEMO's memo as in 1.1), =2 verify: the full path runs as well and must give the same binding.
namespace {
struct MemoKey {
    const void* data;
    uint32_t dims, fmt, modes, maxLod, tlut;
};
inline MemoKey MakeMemoKey(const TexObj& o) {
    MemoKey k;
    k.data = o.data;
    k.dims = static_cast<uint32_t>(o.width) | (static_cast<uint32_t>(o.height) << 16);
    k.fmt = o.format | (static_cast<uint32_t>(o.mipmap) << 8);
    k.modes = static_cast<uint32_t>(o.wrapS) | (static_cast<uint32_t>(o.wrapT) << 8) | (static_cast<uint32_t>(o.minFilter) << 16) | (static_cast<uint32_t>(o.magFilter) << 24);
    k.maxLod = f2u(o.maxLod);
    k.tlut = o.tlut;
    return k;
}
inline bool SameMemoKey(const MemoKey& a, const MemoKey& b) {
    return a.data == b.data && a.dims == b.dims && a.fmt == b.fmt && a.modes == b.modes && a.maxLod == b.maxLod && a.tlut == b.tlut;
}
inline uint32_t MemoSlot(const MemoKey& k) {
    const uint32_t x = (static_cast<uint32_t>(reinterpret_cast<uintptr_t>(k.data) >> 4) * 2654435761u) ^ (k.dims * 0x85EBCA77u) ^ (k.modes * 0xC2B2AE3Du) ^ (k.fmt * 0x27D4EB2Fu);
    return (x ^ (x >> 15)) & 511u;
}
struct TexMemo2 {
    MemoKey key;
    uint32_t copiesGen = 0, eraseGen = 0;
    TexBind out;
    uint16_t width = 0, height = 0;
    Entry* e = nullptr; // nullptr: an EFB copy
    const void* data = nullptr;
    uint32_t size = 0;
    bool valid = false, hd = false;
};
TexMemo2 g_tm2[512];
uint64_t g_tm2Hits = 0, g_tm2Calls = 0, g_tm2Checked = 0, g_tm2Bad = 0;
int TexMemo2Mode() {
    const int m = static_cast<int>(VGX_KNOB([] {
        long v = EnvInt("MSC_VGX_TEXMEMO", 1);
        return (v < 0 || v > 2) ? 1 : static_cast<int>(v);
    }()));
    return m;
}
// The memo's answer, or false when the full path has to run (nothing has been changed then except the entry's check stamps the full path would set).
inline bool TexMemo2Hit(const TexMemo2& t, const MemoKey& k, TexBind& out, uint16_t& width, uint16_t& height) {
    if (!t.valid || t.copiesGen != g_copiesGen || t.eraseGen != g_cacheEraseGen || !SameMemoKey(t.key, k)) return false;
    if (t.e) {
        Entry& e = *t.e;
        if (e.hd != t.hd || e.hdWait) return false;
        // (ResolveTextureImpl's check of the contents: read the write epoch before the generation, so that a write in between is seen next time)
        const uint64_t epoch = GuestWriteEpoch();
        if (!(epoch != ~0ull && e.checkedEpoch == epoch)) {
            const uint64_t gen = Generation(t.data, t.size);
            if (gen == ~0ull || gen != e.generation) return false; // (hashed and maybe uploaded by the full path)
            e.checkedEpoch = epoch;
        }
        e.lastUsed = CurrentFrame();
    }
    out = t.out;
    width = t.width;
    height = t.height;
    return true;
}
} // namespace

uint64_t TexSameGen(const TexObj& o) {
    if (!o.data || o.width == 0 || o.height == 0 || TexFormatIsCI(o.format)) return ~0ull;
    uint32_t levels = 1;
    if (o.mipmap && IsPow2(o.width) && IsPow2(o.height)) levels = 32u - static_cast<uint32_t>(__builtin_clz(o.width > o.height ? o.width : o.height));
    return Generation(o.data, TexDataSize(o.width, o.height, o.format, levels));
}
uint64_t TexSameStamp() { return (static_cast<uint64_t>(g_copiesGen) << 32) | g_cacheEraseGen; }
bool TexSameOk() { return HaveGuestRam() && !g_replaceReady && !g_texVerify && !CopyCheckOn(); }
bool ResolveTexture(int texMap, TexBind& out, uint16_t& width, uint16_t& height) {
    const int memo2 = static_cast<int>(VGX_KNOB(TexMemo2Mode()));
    if (memo2 != 0 && !g_texVerify && HaveGuestRam() && !CopyCheckOn()) {
        const TexSlot& slot = g.tex[texMap & 7];
        if (!slot.valid || !slot.obj.data || slot.obj.width == 0 || slot.obj.height == 0 || TexFormatIsCI(slot.obj.format)) return ResolveTextureImpl(texMap, out, width, height);
        const MemoKey k = MakeMemoKey(slot.obj);
        TexMemo2& t = g_tm2[MemoSlot(k)];
        ++g_tm2Calls;
        if (TexMemo2Hit(t, k, out, width, height)) {
            ++g_tm2Hits;
            if (memo2 == 2) {
                TexBind full;
                uint16_t fw = 0, fh = 0;
                const bool r = ResolveTextureImpl(texMap, full, fw, fh);
                ++g_tm2Checked;
                if (!r || fw != width || fh != height || full.texId != out.texId || full.wrapS != out.wrapS || full.wrapT != out.wrapT || full.minFilter != out.minFilter ||
                    full.magFilter != out.magFilter) {
                    ++g_tm2Bad;
                    VGX_LOG_N(16, "texmemo: MISMATCH (texmap %d, %ux%u fmt 0x%X at %p: texture %u against %u)\n", texMap, slot.obj.width, slot.obj.height, slot.obj.format, slot.obj.data,
                              out.texId, full.texId);
                }
                out = full;
                width = fw;
                height = fh;
                return r;
            }
            return true;
        }
        const bool r = ResolveTextureImpl(texMap, out, width, height);
        t.valid = false;
        if (r && (g_implKind == 1 || (g_implKind == 2 && g_implEntry && g_implEntry->size))) {
            t.key = k;
            t.copiesGen = g_copiesGen;
            t.eraseGen = g_cacheEraseGen;
            t.out = out;
            t.width = width;
            t.height = height;
            t.e = g_implKind == 2 ? g_implEntry : nullptr;
            t.data = slot.obj.data;
            t.size = t.e ? t.e->size : 0;
            t.hd = t.e && t.e->hd;
            t.valid = true;
        }
        return r;
    }
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

// 1.3 first-use manifest (the game's NpFirstUseStep reads it): what was uploaded or made since the last query, by name, for a phase that had something. Cold events only.
static std::mutex g_fuMu;
static char g_fuText[480];
static size_t g_fuLen = 0;
static unsigned g_fuDropped = 0;
void FirstUseNote(const char* text) {
    std::lock_guard<std::mutex> lock(g_fuMu);
    const size_t n = std::strlen(text);
    if (g_fuLen + n + 2 >= sizeof g_fuText) { ++g_fuDropped; return; }
    std::memcpy(g_fuText + g_fuLen, text, n);
    g_fuLen += n;
    g_fuText[g_fuLen++] = ' ';
    g_fuText[g_fuLen] = 0;
}
uint64_t TexUploadSerial() { return g_uploadSerial; }

// 1.3 MSC_TEX_PRETOUCH (EXT_TOUCH_TEX, the game's glx layer): the texture described by the record is resolved without a draw, so it is uploaded now and not at its first draw
// in play. Words: [mode, data, 0, width | height << 16, format, wrapS | wrapT << 8 | minFilter << 16 | magFilter << 24, mipmap, minLod, maxLod, lodBias, tlut] (n = 10; n = 1 for mode 2).
// mode 0: upload only; 1: upload and pin (the budget's trim leaves it); 2: every pin is released (the match is over). No texmap's binding changes: texmap 7 is lent for the
// resolve and given back.
static uint64_t g_touchCalls = 0, g_touchUploads = 0, g_touchHave = 0, g_touchPinnedBytes = 0, g_touchCapped = 0;
void TexTouch(const uint32_t* w, uint32_t n) {
    const uint32_t mode = n >= 1 ? w[0] : 0;
    if (mode == 2) {
        g_cltShown.clear();
        uint64_t k = 0;
        for (auto& kv : g_cache)
            if (kv.second.pinned) { kv.second.pinned = false; ++k; }
        g_touchPinnedBytes = 0;
        if (k) std::fprintf(stderr, "[pretouch] frame %llu: %llu pinned textures released (match over); %llu touches, %llu uploads by touch, %llu already resident\n", static_cast<unsigned long long>(CurrentFrame()),
                            static_cast<unsigned long long>(k), static_cast<unsigned long long>(g_touchCalls), static_cast<unsigned long long>(g_touchUploads), static_cast<unsigned long long>(g_touchHave));
        return;
    }
    if (n < 10) return;
    TexObj o{};
    o.data = reinterpret_cast<const void*>(static_cast<uintptr_t>(w[1]));
    o.width = static_cast<uint16_t>(w[2] & 0xFFFFu);
    o.height = static_cast<uint16_t>(w[2] >> 16);
    o.format = w[3];
    o.wrapS = static_cast<uint8_t>(w[4] & 0xFFu);
    o.wrapT = static_cast<uint8_t>((w[4] >> 8) & 0xFFu);
    o.minFilter = static_cast<uint8_t>((w[4] >> 16) & 0xFFu);
    o.magFilter = static_cast<uint8_t>(w[4] >> 24);
    o.mipmap = static_cast<uint8_t>(w[5]);
    std::memcpy(&o.minLod, &w[6], 4);
    std::memcpy(&o.maxLod, &w[7], 4);
    std::memcpy(&o.lodBias, &w[8], 4);
    o.tlut = w[9];
    o.dataVersion = 1;
    o.magic = 0x76677854u;
    if (!o.data || o.width == 0 || o.height == 0 || TexFormatIsCI(o.format)) return;
    if (CutLearnTexOn() && !g_clt.replaying) { std::array<uint32_t, 10> a; std::memcpy(a.data(), w, sizeof a); g_cltShown[w[1]] = a; }   // (MSC_CUT_LEARN: the game's own addresses of this run)
    ++g_touchCalls;
    TexSlot& slot = g.tex[7];
    const TexSlot saved = slot;
    std::memcpy(&slot.obj, &o, sizeof(TexObj));
    slot.valid = true;
    const uint64_t serial0 = g_uploadSerial;
    TexBind b;
    uint16_t bw = 0, bh = 0;
    // What a pin may take: the pinned set can never be freed by the trim, so it is kept under the budget with a slack (MSC_TEX_PINSLACK_MIB, default 6) for what a match binds besides it; over that,
    // a texture is uploaded without a pin and may be trimmed (and uploaded again at its first draw): the budget wins over the pre-upload, never the other way round.
    static const uint64_t slack = static_cast<uint64_t>(EnvInt("MSC_TEX_PINSLACK_MIB", 6)) << 20;
    const uint64_t bud = g_budgetSet ? g_budget : (static_cast<uint64_t>(EnvInt("MSC_VGX_TEXBUDGET", 0)) << 20);
    const bool pinOk = mode == 1 && (bud == 0 || g_touchPinnedBytes + slack < bud);
    if (mode == 1 && !pinOk) {
        static bool said = false;
        ++g_touchCapped;
        if (!said) { said = true; std::fprintf(stderr, "[pretouch] frame %llu: pinned set %.1f MB reached the budget %.1f MB less %.1f MB slack: later textures are shown without a pin\n", static_cast<unsigned long long>(CurrentFrame()), g_touchPinnedBytes / 1048576.0, bud / 1048576.0, slack / 1048576.0); }
    }
    g_pinNew = false;
    g_touchPin = pinOk;
    const bool ok = ResolveTextureImpl(7, b, bw, bh);
    g_touchPin = false;
    slot = saved;
    if (!ok) return;
    if (g_implKind == 2 && g_implEntry) {
        if (g_uploadSerial != serial0) ++g_touchUploads; else ++g_touchHave;
        if (g_pinNew) g_touchPinnedBytes += g_implEntry->bytes;
    }
    static const bool lg = EnvInt("MSC_TEX_PRETOUCH_LOG", 0) != 0;
    if (lg && (g_touchCalls % 64) == 0)
        std::fprintf(stderr, "[pretouch] frame %llu: %llu touches, %llu uploads by touch, %llu already resident, %.1f MB pinned, %.1f MB live\n", static_cast<unsigned long long>(CurrentFrame()),
                     static_cast<unsigned long long>(g_touchCalls), static_cast<unsigned long long>(g_touchUploads), static_cast<unsigned long long>(g_touchHave), g_touchPinnedBytes / 1048576.0, g_liveBytes / 1048576.0);
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

// 1.2 (MSC_VGX_UPLOAD_RETRY=<frames>, default 120; 0 off): the back end could not give an uploaded texture any memory (vitaGL reports nothing) and left
// its slot empty, which samples as white. The entry would be taken for uploaded until it was evicted, so the texture stayed white; instead the entry
// is forgotten once <frames> frames have passed, and the texture's next use uploads it again. One retry per entry per <frames> frames at most: a
// memory that stays full costs one upload attempt every two seconds a texture is used, not one per draw. Both threads that can reach this (the
// player's and the back end's) are one thread in every play mode; the mailbox is locked all the same.
namespace {
std::mutex g_lostMu;
std::vector<uint32_t> g_lostIn;           // texture ids the back end gave up (mailbox)
std::atomic<bool> g_lostPending{false};
struct LostEntry {
    Key key;
    uint32_t texId;
    uint64_t retryAt;
};
std::vector<LostEntry> g_lostWait;
long UploadRetryFrames() {
    const long n = static_cast<long>(VGX_KNOB(EnvInt("MSC_VGX_UPLOAD_RETRY", 120)));
    return n;
}
void RetryLostUploads() {
    if (g_lostPending.load(std::memory_order_acquire)) {
        std::vector<uint32_t> in;
        {
            std::lock_guard<std::mutex> lock(g_lostMu);
            in.swap(g_lostIn);
            g_lostPending.store(false, std::memory_order_release);
        }
        const long frames = UploadRetryFrames();
        for (uint32_t id : in) {
            if (frames <= 0 || !id) continue;
            bool waiting = false;
            for (const LostEntry& w : g_lostWait) waiting = waiting || w.texId == id;
            if (waiting) continue;
            for (const auto& kv : g_cache)
                if (kv.second.texId == id) { g_lostWait.push_back(LostEntry{kv.first, id, CurrentFrame() + static_cast<uint64_t>(frames)}); break; }
        }
    }
    if (g_lostWait.empty()) return;
    const uint64_t now = CurrentFrame();
    bool erased = false;
    for (size_t i = 0; i < g_lostWait.size();) {
        if (g_lostWait[i].retryAt > now) { ++i; continue; }
        const LostEntry w = g_lostWait[i];
        g_lostWait.erase(g_lostWait.begin() + static_cast<std::ptrdiff_t>(i));
        auto it = g_cache.find(w.key);
        if (it == g_cache.end() || it->second.texId != w.texId) continue;   // (evicted or uploaded again meanwhile)
        const uint32_t bytes = it->second.bytes;
        Pending().cmds.push_back(Cmd{CMD_TEX_FREE, w.texId});   // (the slot is empty: nothing to free there, but the same command stream as an eviction)
        stats::TexLive(-1, -static_cast<int64_t>(bytes));
        g_liveBytes -= bytes;
        if (it->second.hd) g_hdLive -= bytes;
        g_cache.erase(it);
        erased = true;
    }
    if (erased) { CacheFrontClear(); ++g_cacheEraseGen; }
}
} // namespace

// The back end's call (weak there: tools that link it without the cache do not have it).
void TexCacheLost(uint32_t texId) {
    std::lock_guard<std::mutex> lock(g_lostMu);
    if (g_lostIn.size() < 256) g_lostIn.push_back(texId);
    g_lostPending.store(true, std::memory_order_release);
}

// MSC_CUT_LEARN bit 1, once a frame from TexCacheEndFrame: at a match's load the list of its stadium is read and made ahead (two touches a frame, inside the KiB budget), and what is uploaded after kick-off is written.
void CutLearnTexTick() {
    if (!CutLearnTexOn()) return;
    CutLearnTex& c = g_clt;
    const uint32_t phase = cutlearn::g_phase.load(std::memory_order_acquire), idx = cutlearn::g_matchIdx.load(std::memory_order_acquire);
    if (phase == 1 && (c.seenMatch != idx || c.seenPhase != 1)) {   // a match has loaded: its stadium's list
        c = CutLearnTex{};
        c.seenMatch = idx;
        c.seenPhase = 1;
        c.lim.texKib = static_cast<uint32_t>(EnvInt("MSC_CUT_LEARN_KIB", 2048));
        c.path = CutLearnTexPath(cutlearn::g_stadium.load());
        if (!c.path.empty()) {
            if (FILE* f = std::fopen(c.path.c_str(), "rb")) {
                std::string text;
                char buf[4096];
                size_t n;
                while ((n = std::fread(buf, 1, sizeof buf, f)) > 0 && text.size() < (1u << 20)) text.append(buf, n);
                std::fclose(f);
                c.loaded = cutlearn::ParseText(text.data(), text.size(), c.list, c.lim);
            }
        }
        c.plan = cutlearn::MakePlan(c.list, cutlearn::kBitTex, c.lim);
        std::fprintf(stderr, "[cutlearn] match #%u stadium %u: texture list %s: %u entries, %u made ahead (%u KiB of the %u KiB budget), %u over the budget\n", idx, cutlearn::g_stadium.load(),
                     c.path.empty() ? "(no file: MSC_VGX_SHADERLIST unset)" : c.path.c_str(), c.loaded, static_cast<unsigned>(c.plan.tex.size()), c.plan.texKib, c.lim.texKib, c.plan.texSkipped);
        return;
    }
    if (phase == 1 && c.seenMatch == idx) {
        // two a frame; a listed texture is touched only at an address this run's pretouch showed the renderer, with the size and format that were listed
        for (int k = 0; k < 2 && c.cursor < c.plan.tex.size(); ++k) {
            const cutlearn::TexRec& r = c.list.tex[c.plan.tex[c.cursor++]];
            auto it = g_cltShown.find(r.addr);
            if (it == g_cltShown.end() || (it->second[2] & 0xFFFFu) != r.w || (it->second[2] >> 16) != r.h || (it->second[3] & 255u) != r.fmt) { ++c.noAddr; continue; }
            const uint64_t up0 = g_touchUploads;
            c.replaying = true;
            std::array<uint32_t, 10> w = it->second;
            w[0] = 0;   // (no pin: the budget of the pinned set is the pretouch's; this is a plain touch)
            TexTouch(w.data(), 10);
            c.replaying = false;
            ++c.touched;
            if (g_touchUploads != up0) { ++c.uploaded; c.kib += r.kib; } else ++c.there;
        }
        return;
    }
    if (phase == 2 && c.seenMatch == idx && c.seenPhase == 1) {
        c.seenPhase = 2;
        std::fprintf(stderr, "[cutlearn] kick-off of match #%u: textures made ahead: %u touched, %u uploaded now (%llu KiB), %u already there, %u not shown by this run's pretouch (address, size or format differ); %u of %u planned\n", idx, c.touched, c.uploaded,
                     static_cast<unsigned long long>(c.kib), c.there, c.noAddr, static_cast<unsigned>(static_cast<size_t>(c.cursor)), static_cast<unsigned>(c.plan.tex.size()));
    }
}

void TexCacheEndFrame() {
    CutLearnTexTick();
    RetryLostUploads();
    ++g_texMemoGen; // (GXD_TEXMEMO: nothing resolved in the frame that ended is trusted in the next)
    // 1.1: HD replacements of characters no longer on screen (an earlier match's teams) are freed, so the next ones fit in the budget.
    // Every 120 frames, only past half the budget; texture id order keeps the command stream independent of the hash map.
    if (g_hdBudget && g_hdLive > g_hdBudget / 2 && CurrentFrame() % 120 == 0) {
        const uint64_t now = CurrentFrame();
        static std::vector<std::pair<uint32_t, Key>> old;
        old.clear();
        for (const auto& kv : g_cache)
            if (kv.second.hd && kv.second.texId && !kv.second.pinned && kv.second.lastUsed + 600 < now) old.emplace_back(kv.second.texId, kv.first);
        std::sort(old.begin(), old.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        CmdBuf& buf = Pending();
        for (const auto& o : old) {
            auto it = g_cache.find(o.second);
            const uint32_t bytes = it->second.bytes;
            buf.cmds.push_back(Cmd{CMD_TEX_FREE, o.first});
            stats::TexLive(-1, -static_cast<int64_t>(bytes));
            g_liveBytes -= bytes;
            g_hdLive -= bytes;
            ++g_hdFreed;
            g_cache.erase(it);
        }
        if (!old.empty()) { CacheFrontClear(); ++g_cacheEraseGen; }
    }
    const uint64_t envBudget = static_cast<uint64_t>(VGX_KNOB(static_cast<uint64_t>(EnvInt("MSC_VGX_TEXBUDGET", 0)) << 20));
    const uint64_t budget = g_budgetSet ? g_budget : envBudget;
    if (!budget || g_liveBytes <= budget) return;
    constexpr uint64_t kMinIdle = 30;
    const uint64_t now = CurrentFrame();
    // 1.3 MSC_VGX_HOUSEKEEP_BACKOFF (the Vita plan sets 1, code 0 = as 1.2): over the budget with every texture used in the last 30 frames, each frame walked the whole cache and sorted nothing to delete
    // (a working set larger than the budget). After such a pass the next waits kTrimRest (4) frames: a texture becomes deletable by age alone, so only the time of its delete moves.
    static const bool backoff = EnvInt("MSC_VGX_HOUSEKEEP_BACKOFF", 0) != 0;
    constexpr uint64_t kTrimRest = 4;
    static uint64_t nextScan = 0, scans = 0, empty = 0, skipped = 0, noteAt = 1800, noteSum = 0;
    if (now >= noteAt) {   // [housekeep] line: every 1800 frames, while the counters moved
        noteAt = now + 1800;
        if (scans + skipped != noteSum) {
            noteSum = scans + skipped;
            std::fprintf(stderr, "[housekeep] texture trim over budget: %lu passes (%lu deleted nothing), %lu skipped by the back-off (MSC_VGX_HOUSEKEEP_BACKOFF=%d); %lu textures freed in all\n", static_cast<unsigned long>(scans),
                         static_cast<unsigned long>(empty), static_cast<unsigned long>(skipped), backoff ? 1 : 0, static_cast<unsigned long>(g_evicted));
        }
    }
    if (now < nextScan) { ++skipped; ++g_overBudgetFrames; return; }
    ++scans;
    struct Cand {
        uint64_t lastUsed;
        uint32_t texId;
        Key key;
    };
    static std::vector<Cand> cand;
    cand.clear();
    for (const auto& kv : g_cache)
        if (kv.second.texId && !kv.second.pinned && kv.second.lastUsed + kMinIdle < now) cand.push_back(Cand{kv.second.lastUsed, kv.second.texId, kv.first});
    // Oldest first; the texture id breaks ties so that the order (and with it the
    // command stream) does not depend on the hash map's iteration order.
    std::sort(cand.begin(), cand.end(), [](const Cand& a, const Cand& b) { return a.lastUsed != b.lastUsed ? a.lastUsed < b.lastUsed : a.texId < b.texId; });
    if (cand.empty()) { ++empty; if (backoff) nextScan = now + kTrimRest; }
    CmdBuf& buf = Pending();
    for (const Cand& c : cand) {
        if (g_liveBytes <= budget) break;
        auto it = g_cache.find(c.key);
        const uint32_t bytes = it->second.bytes;
        buf.cmds.push_back(Cmd{CMD_TEX_FREE, c.texId});
        stats::TexLive(-1, -static_cast<int64_t>(bytes));
        g_liveBytes -= bytes;
        if (it->second.hd) g_hdLive -= bytes;
        ++g_evicted;
        g_evictedBytes += bytes;
        ++stats::Cur().texEvicted;
        stats::Cur().texEvictBytes += bytes;
        g_cache.erase(it);
        CacheFrontClear();
        ++g_cacheEraseGen;
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
    if (g_i8Uploads) std::fprintf(f, "texture uploads: %llu as one byte a texel (I4, I8; %.1f MB, MSC_VGX_LA88=2)\n", static_cast<unsigned long long>(g_i8Uploads), g_i8Bytes / 1048576.0);
    {   // tile24: mipmapped I4/I8/IA4/IA8 (they are RGBA8 unless MSC_VGX_LA88=3): uploads and bytes as RGBA8 with a chain; live now (held bytes); what 16 / 8 bits would hold
        static const char* const kN[4] = {"I4", "I8", "IA4", "IA8"};
        uint64_t live[4] = {}, liveN[4] = {};
        for (const auto& kv : g_cache)
            if (kv.second.imFmt < 4 && kv.second.texId) { live[kv.second.imFmt] += kv.second.bytes; ++liveN[kv.second.imFmt]; }
        for (int i = 0; i < 4; ++i)
            if (g_imUploads[i] || g_imNoMipUploads[i])
                std::fprintf(f, "texture census (tile24) %s: %llu mipmapped uploads (%.2f MB as RGBA8 with chain; as LA88 %.2f MB, one byte %.2f MB), %llu live holding %.2f MB; %llu more carry the mip flag but sample without levels (%.2f MB RGBA8)\n", kN[i],
                             static_cast<unsigned long long>(g_imUploads[i]), g_imBytes[i] / 1048576.0, g_imBytes[i] / 2 / 1048576.0, g_imBytes[i] / 4 / 1048576.0, static_cast<unsigned long long>(liveN[i]), live[i] / 1048576.0,
                             static_cast<unsigned long long>(g_imNoMipUploads[i]), g_imNoMipBytes[i] / 1048576.0);
        if (g_laMipUploads) std::fprintf(f, "texture uploads: %llu mipmapped I/IA textures as LA88 / one byte with a generated chain (MSC_VGX_LA88=3)\n", static_cast<unsigned long long>(g_laMipUploads));
    }
    if (g_dxt5Uploads || g_cmpraKept) std::fprintf(f, "texture uploads: %llu CMPR as DXT5 (%.1f MB, MSC_VGX_CMPRA; largest middle-colour error %u of 255), %llu left decoded (MSC_VGX_CMPRA=2, over MSC_VGX_CMPRA_TOL)\n", static_cast<unsigned long long>(g_dxt5Uploads), g_dxt5Bytes / 1048576.0, g_cmpraWorst, static_cast<unsigned long long>(g_cmpraKept));
    if (g_t16Small) std::fprintf(f, "texture uploads: %llu under MSC_VGX_TEX16_MIN texels kept at 32 bits\n", static_cast<unsigned long long>(g_t16Small));
    if (g_t16Uploads) std::fprintf(f, "texture uploads: %llu in 16 bits (%.1f MB, MSC_VGX_TEX16)\n", static_cast<unsigned long long>(g_t16Uploads), g_t16Bytes / 1048576.0);
    {
        static const char* const kFmt[16] = {"I4", "I8", "IA4", "IA8", "RGB565", "RGB5A3", "RGBA8", "?7", "C4", "C8", "C14X2", "?B", "?C", "?D", "CMPR", "?F"};
        for (int i = 0; i < 16; ++i)
            if (g_fmtUploads[i]) std::fprintf(f, "texture uploads by GX format: %s %llu (%.1f MB held)\n", kFmt[i], static_cast<unsigned long long>(g_fmtUploads[i]), g_fmtBytes[i] / 1048576.0);
    }
    if (g_hdUploads) std::fprintf(f, "HD textures: %llu uploads (%.1f MB), %.1f MB alive at the end, %llu freed when unused\n", static_cast<unsigned long long>(g_hdUploads),
                                  g_hdBytes / 1048576.0, g_hdLive / 1048576.0, static_cast<unsigned long long>(g_hdFreed));
    if (g_tm2Calls) std::fprintf(f, "texture binding memo (MSC_VGX_TEXMEMO): %llu of %llu resolutions answered by the memo%s\n", static_cast<unsigned long long>(g_tm2Hits), static_cast<unsigned long long>(g_tm2Calls),
                                 g_tm2Checked ? "" : "");
    if (g_tm2Checked) std::fprintf(f, "texture binding memo verify: %llu hits checked against the full path, %llu MISMATCHING\n", static_cast<unsigned long long>(g_tm2Checked), static_cast<unsigned long long>(g_tm2Bad));
    if (g_texMemoCalls) std::fprintf(f, "texture memo: %llu of %llu resolutions repeated a bind of the same frame (GXD_TEXMEMO)\n", static_cast<unsigned long long>(g_texMemoHits), static_cast<unsigned long long>(g_texMemoCalls));
    const uint64_t budget = g_budgetSet ? g_budget : (static_cast<uint64_t>(EnvInt("MSC_VGX_TEXBUDGET", 0)) << 20);
    if (budget)
        std::fprintf(f, "texture budget %.0f MB: %llu textures freed (%.1f MB), %llu frames ended over budget (working set larger than the budget)\n",
                     budget / 1048576.0, static_cast<unsigned long long>(g_evicted), g_evictedBytes / 1048576.0,
                     static_cast<unsigned long long>(g_overBudgetFrames));
}

bool IsBallFinTexId(unsigned int id) {
    if (!id) return false;
    for (int i = 0; i < 4; ++i)
        if (g_ballFinTexIds[i].load(std::memory_order_relaxed) == id) return true;
    return false;
}
} // namespace vgx
