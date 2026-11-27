// be24 MSC_VGX_DYNCACHE=0|1|2 (default 0): the frame's big dynamic vertex ranges that were the same bytes in an earlier picture are not written to the stream again.
// Included by vgx_backend_gl.cpp only, before Draw() (it uses that file's MakeLayout, C, BindArrayBuf and the draw classes).
//
// Why (host counters, Vice live play, ESTIMATES): a picture writes 1.03 MiB of GL_STREAM_DRAW data into vitaGL's circular pool, which is UNCACHED RAM where the CPU's writes ran at 0.25 GB/s
// (3.9 ms a MB, 5 Oct), and 0.36 MiB more into the uniform ring. 0.5 MiB of the stream is the six player draws (a GPU-skinned layout, 92 bytes a vertex: the mesh in its bind pose, the
// bones come in the uniforms) and 59 % of all 64 byte blocks of the stream occur somewhere in the previous picture's stream (MSC_VGX_STREAMDIFF). The decoded bytes are the same every picture;
// only their place in the arena moves.
//
// What it does: a dynamic draw whose vertices are >= MSC_VGX_DYNCACHE_MIN bytes (default 8192; not the crowd, triangles only) is looked up by content (XXH3 128 bit of its vertex bytes; a cheap
// sample of 192 bytes finds the candidate first). A hit draws from a buffer of its own that holds exactly those bytes (made once, by the second sighting of the sample), through
// vglDrawElementsDirect with the stream address shifted back by vtxFirst * stride (the draw's indices count from the chunk's start, vgx_draw.cpp "vertex addressing"); UploadStream then
// allocates the chunk without copying those ranges and copies only the rest (glBufferData(NULL) + glBufferSubData). The GPU reads the same bytes as before, so the picture is the same.
// Vita only with MSC_VGX_GXM_DIRECT (the shifted address is a stream pointer, which the ordinary attribute path would turn into 16-bit offsets). On the host the plan is made and CHECKED
// (=2 keeps a copy of every cached range and compares every hit: "mismatch" must stay 0) but nothing is applied.
//   =1 on, =2 on with the copy compare (the host proof; on the console only for a short check: it doubles the memory), MSC_VGX_DYNCACHE_KIB (default 1536) the memory the cached buffers may hold,
//   MSC_VGX_BREAK bit 128 (test): a hit needs only the sample to match (the compare must catch it).
namespace dyncache {
struct Entry {
    GLuint vbo = 0;
    uint32_t bytes = 0, layout = 0, stride = 0;
    uint32_t last = 0, hits = 0;
    uint64_t sample = 0;
    std::vector<uint8_t> copy;   // =2
};
struct Plan { GLuint vbo = 0; uint32_t base = 0; uint32_t s = 0, e = 0, chunk = 0; };   // per draw record: vbo 0 = not cached; s..e the bytes in the arena
struct Stats { uint64_t considered = 0, hits = 0, promoted = 0, fullMiss = 0, mismatch = 0, evicted = 0, bytesSaved = 0, pics = 0, demoted = 0, refused = 0; uint64_t hashBytes = 0; };
inline int g_mode = -1;
inline bool g_apply = false;
inline uint32_t g_min = 8192, g_capBytes = 1536u * 1024u, g_pic = 0, g_held = 0;
inline bool g_break = false;
inline std::unordered_map<uint64_t, Entry> g_byFull;      // key: the 128 bit hash folded to 64
inline std::unordered_map<uint64_t, uint64_t> g_bySample; // sample -> key of the entry
inline std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> g_seen;   // sample -> (picture last seen, full-hash mismatches)
inline std::unordered_set<uint32_t> g_proven;             // vsId << 16 | layout mask of draws that went through vglDrawElementsDirect
inline std::vector<Plan> g_plan;
inline const DrawRec* g_draws = nullptr;
inline Stats g_st;
inline bool g_broken = false;
inline bool On() { return g_mode > 0; }
inline void Clear() { g_plan.clear(); g_draws = nullptr; }
inline void Setup(bool direct) {
    if (g_mode >= 0) return;
    const long v = EnvInt("MSC_VGX_DYNCACHE", 0);
    g_mode = v < 0 ? 0 : (v > 2 ? 2 : static_cast<int>(v));
    g_min = static_cast<uint32_t>(EnvInt("MSC_VGX_DYNCACHE_MIN", 8192));
    if (g_min < 256) g_min = 256;
    g_capBytes = static_cast<uint32_t>(EnvInt("MSC_VGX_DYNCACHE_KIB", 1536)) * 1024u;
    g_break = (EnvInt("MSC_VGX_BREAK", 0) & 128) != 0;
#ifdef __VGX_VITA__
    g_apply = g_mode > 0 && direct;
#else
    g_apply = false;
    (void)direct;
#endif
    if (g_mode > 0) std::fprintf(stderr, "[dyncache] on (MSC_VGX_DYNCACHE=%d): ranges of %u bytes or more, memory %u KiB, %s\n", g_mode, g_min, g_capBytes / 1024u,
                                 g_apply ? "applied: hits are drawn from their own buffer and left out of the stream" : "census only (this build has no direct draw path to apply it to)");
}
inline uint64_t Fold(const XXH128_hash_t& h) { return h.low64 ^ (h.high64 * 0x9E3779B97F4A7C15ull); }
inline uint64_t Sample(const uint8_t* p, uint32_t n, uint32_t layout) {
    XXH64_state_t st;
    XXH64_reset(&st, layout);
    XXH64_update(&st, &n, 4);
    XXH64_update(&st, p, 64);
    XXH64_update(&st, p + (n / 2 & ~63u), 64);
    XXH64_update(&st, p + n - 64, 64);
    return XXH64_digest(&st);
}
inline void NoteDirect(uint16_t vsId, uint32_t layout) {   // a draw of this pair went through vglDrawElementsDirect
    if (g_mode > 0) g_proven.insert(static_cast<uint32_t>(vsId) << 16 | (layout & 0xFFFFu));
}
inline void Drop(uint64_t key) {
    auto it = g_byFull.find(key);
    if (it == g_byFull.end()) return;
    if (g_held >= it->second.bytes) g_held -= it->second.bytes;
    if (it->second.vbo) { GLuint b = it->second.vbo; G(glDeleteBuffers(1, &b)); }
    g_bySample.erase(it->second.sample);
    g_byFull.erase(it);
    ++g_st.evicted;
}
inline void Evict(uint32_t need) {   // oldest first until `need` fits
    while (g_held + need > g_capBytes && !g_byFull.empty()) {
        uint64_t oldest = 0;
        uint32_t oldestPic = ~0u;
        for (const auto& kv : g_byFull)
            if (kv.second.last < oldestPic) { oldestPic = kv.second.last; oldest = kv.first; }
        if (oldestPic + 1 >= g_pic) break;   // everything was used in the last picture: no room
        Drop(oldest);
    }
}
// Plans the picture: which dynamic draws are served from a cached buffer. `vtx` is the arena the stream upload copies from.
inline void Prepare(const CmdBuf& buf, const uint8_t* vtx, bool direct) {
    Setup(direct);
    g_plan.clear();
    g_draws = nullptr;
    if (g_mode <= 0 || g_broken) return;
    ++g_pic;
    ++g_st.pics;
    const size_t n = buf.draws.size();
    if (n == 0 || buf.vtx.empty()) return;
    g_plan.assign(n, Plan{});
    g_draws = buf.draws.data();
    // entries not used for a while go
    if ((g_pic & 15u) == 0) {
        std::vector<uint64_t> old;
        for (const auto& kv : g_byFull) if (g_pic - kv.second.last > 120u) old.push_back(kv.first);
        for (uint64_t k : old) Drop(k);
        if ((g_pic & 255u) == 0)
            for (auto it = g_seen.begin(); it != g_seen.end();) it = (g_pic - it->second.first > 600u) ? g_seen.erase(it) : std::next(it);
    }
    struct Span { uint32_t s, e, idx; bool hit; };
    std::vector<Span> spans;
    spans.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const DrawRec& r = buf.draws[i];
        if (r.vbuf || r.vtxCount == 0) continue;
        const uint32_t stride = MakeLayout(r.layoutMask).stride;
        const uint32_t chunkA = buf.vtxChunks.empty() ? 0 : buf.vtxChunks[r.vchunk < buf.vtxChunks.size() ? r.vchunk : 0];
        const uint32_t s = chunkA + r.vtxFirst * stride, e = s + r.vtxCount * stride;
        if (e > buf.vtx.size() || r.vtxOffset != 0) continue;
        spans.push_back({s, e, static_cast<uint32_t>(i), false});
        const uint32_t bytes = r.vtxCount * stride;
        if (bytes < g_min || r.prim != PRIM_TRIANGLES || r.cls == CLS_CROWD || CrowdEligible(r)) continue;
        if (g_apply && !g_proven.count(static_cast<uint32_t>(r.vsId) << 16 | (r.layoutMask & 0xFFFFu))) continue;
        ++g_st.considered;
        const uint8_t* p = vtx + s;
        const uint64_t sk = Sample(p, bytes, r.layoutMask);
        auto bs = g_bySample.find(sk);
        uint64_t fullKey = 0;
        bool hit = false;
        if (bs != g_bySample.end()) {
            auto fe = g_byFull.find(bs->second);
            if (fe != g_byFull.end() && fe->second.bytes == bytes && fe->second.layout == r.layoutMask) {
                g_st.hashBytes += bytes;
                fullKey = Fold(XXH3_128bits(p, bytes));
                if (g_break || fullKey == bs->second) {
                    hit = true;
                    ++g_st.hits;
                    Entry& en = fe->second;
                    if (g_mode == 2 && !en.copy.empty() && std::memcmp(en.copy.data(), p, bytes) != 0) ++g_st.mismatch;
                    en.last = g_pic;
                    ++en.hits;
                    Plan& pl = g_plan[i];
                    pl.vbo = en.vbo;
                    pl.base = static_cast<uint32_t>(0u - r.vtxFirst * stride);
                    pl.s = s;
                    pl.e = e;
                    pl.chunk = r.vchunk;
                    spans.back().hit = true;
                } else {
                    ++g_st.fullMiss;
                    ++g_seen[sk].second;
                    // a changed range: the old entry goes, a new one may be made from the second sighting
                    Drop(bs->second);
                }
            }
        }
        if (hit) continue;
        // not cached: seen before (within 16 pictures, and not a range that keeps changing)? then it gets a buffer now
        auto sn = g_seen.find(sk);
        if (sn == g_seen.end()) { g_seen[sk] = {g_pic, 0}; continue; }
        const bool recent = g_pic - sn->second.first <= 16u;
        sn->second.first = g_pic;
        if (!recent || sn->second.second >= 3) continue;
        if (g_held + bytes > g_capBytes) Evict(bytes);
        if (g_held + bytes > g_capBytes) { ++g_st.refused; continue; }
        g_st.hashBytes += bytes;
        fullKey = Fold(XXH3_128bits(p, bytes));
        if (g_byFull.count(fullKey)) continue;
        Entry en;
        en.bytes = bytes;
        en.layout = r.layoutMask;
        en.stride = stride;
        en.last = g_pic;
        en.sample = sk;
        if (g_mode == 2) en.copy.assign(p, p + bytes);
        if (g_apply) {
            GLuint b = 0;
            G(glGenBuffers(1, &b));
            BindArrayBuf(b);
            G(glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(bytes), p, g_staticUsage));
            en.vbo = b;
        }
        g_held += bytes;
        g_bySample[sk] = fullKey;
        ++g_st.promoted;
        const Entry& ref = g_byFull.emplace(fullKey, std::move(en)).first->second;
        if (g_apply && ref.vbo) {   // the draw of the promoting picture already uses it
            Plan& pl = g_plan[i];
            pl.vbo = ref.vbo;
            pl.base = static_cast<uint32_t>(0u - r.vtxFirst * stride);
            pl.s = s;
            pl.e = e;
            pl.chunk = r.vchunk;
            spans.back().hit = true;
        }
    }
    // a cached range that touches another draw's range is not left out of the stream (that draw reads it)
    std::sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) { return a.s < b.s || (a.s == b.s && a.e < b.e); });
    for (size_t k = 0; k < spans.size(); ++k) {
        if (!spans[k].hit) continue;
        const bool overlapPrev = k > 0 && spans[k - 1].e > spans[k].s, overlapNext = k + 1 < spans.size() && spans[k + 1].s < spans[k].e;
        if (overlapPrev || overlapNext) { g_plan[spans[k].idx] = Plan{}; spans[k].hit = false; ++g_st.demoted; }
    }
    for (const Span& sp : spans)
        if (sp.hit) g_st.bytesSaved += sp.e - sp.s;
}
// The plan of draw record `r` (the reference into the frame's records): a cached buffer, or null.
inline const Plan* For(const DrawRec& r) {
    if (!g_apply || g_plan.empty()) return nullptr;
    const ptrdiff_t i = &r - g_draws;
    if (i < 0 || static_cast<size_t>(i) >= g_plan.size()) return nullptr;
    return g_plan[static_cast<size_t>(i)].vbo ? &g_plan[static_cast<size_t>(i)] : nullptr;
}
// What the stream upload of chunk [a, b) must still copy: the chunk minus the cached ranges, as (offset, length) pairs inside the chunk. False: nothing is cached in it (copy it whole).
inline bool Gaps(uint32_t chunk, uint32_t a, uint32_t b, std::vector<std::pair<uint32_t, uint32_t>>& out) {
    out.clear();
    if (!g_apply || g_plan.empty()) return false;
    std::vector<std::pair<uint32_t, uint32_t>> skip;
    for (const Plan& p : g_plan)
        if (p.vbo && p.chunk == chunk && p.s >= a && p.e <= b) skip.emplace_back(p.s - a, p.e - a);
    if (skip.empty()) return false;
    std::sort(skip.begin(), skip.end());
    uint32_t at = 0;
    for (const auto& sk : skip) {
        if (sk.first > at) out.emplace_back(at, sk.first - at);
        if (sk.second > at) at = sk.second;
    }
    if (at < b - a) out.emplace_back(at, b - a - at);
    // small gaps between ranges are copied with their neighbours (a call costs more than 256 bytes of copy)
    std::vector<std::pair<uint32_t, uint32_t>> merged;
    for (const auto& g : out) {
        if (!merged.empty() && g.first - (merged.back().first + merged.back().second) < 256) merged.back().second = g.first + g.second - merged.back().first;
        else merged.push_back(g);
    }
    out.swap(merged);
    return true;
}
inline void Report() {
    if (g_mode <= 0 || g_st.pics == 0 || (g_st.pics % 300) != 0) return;
    const double n = static_cast<double>(g_st.pics);
    std::fprintf(stderr, "[dyncache] %llu pictures: per picture %.1f ranges looked at, %.1f hits (%.1f KiB left out of the stream), %.2f made, %.2f changed (full compare failed), %.2f demoted (overlap), %.2f refused (memory); "
                         "%.1f KiB hashed; held %u KiB in %u buffers; evicted %llu; mismatch %llu%s\n",
                 static_cast<unsigned long long>(g_st.pics), g_st.considered / n, g_st.hits / n, g_st.bytesSaved / n / 1024.0, g_st.promoted / n, g_st.fullMiss / n, g_st.demoted / n, g_st.refused / n, g_st.hashBytes / n / 1024.0,
                 g_held / 1024u, static_cast<unsigned>(g_byFull.size()), static_cast<unsigned long long>(g_st.evicted), static_cast<unsigned long long>(g_st.mismatch), g_apply ? "" : " (census: nothing applied)");
    g_st = Stats{};
}
}   // namespace dyncache
