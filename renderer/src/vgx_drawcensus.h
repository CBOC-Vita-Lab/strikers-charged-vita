// MSC_VGX_DRAWCENSUS=<window> (diagnostic, off; nothing is changed): what the draw records of a command buffer are, per scene class and draw class
// (EXT_LAYER; needs the layer marks: MSC_DYNRES_FG=1 or MSC_VGX_FGCENSUS=1), and why each one did not join the record before it. Read by both back
// ends (called once per executed buffer, before it is drawn), so it works in the armhf null harness and in the glnull harness.
//   one line per window of <window> frames (default 600 when the value is 1) and per scene/class: records, static ones, vertices, and the first
//   test that kept a record from joining its predecessor: nonadj (another command between them), tag (class, layer or fx mark), static, layout
//   (primitive or vertex layout), vs, fs, state (render state or textures), skin, mvonly / texmonly / tevonly / blocks (only those uniform blocks
//   differ: mvonly is what a rigid merge could take), same (should not be: they would have merged).
//   MSC_VGX_DRAWCENSUS_ORDER=<frame>: the records of that frame in order, one line each.
#pragma once
#include "vgx_cmd.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

namespace vgx {
namespace drawcensus {

inline long Window() {
    static const long w = [] {
        const long v = EnvInt("MSC_VGX_DRAWCENSUS", 0);
        return v == 1 ? 600L : v;
    }();
    return w;
}
// MSC_VGX_DRAWCENSUS_FRAMES="a,b-c": with RICH, dump the records of those frames (like ORDER, for several frames at once)
inline bool InFrames(uint64_t f) {
    static const std::string list = [] { const char* e = std::getenv("MSC_VGX_DRAWCENSUS_FRAMES"); return std::string(e ? e : ""); }();
    if (list.empty()) return false;
    const char* p = list.c_str();
    while (*p) {
        char* e = nullptr;
        const unsigned long a = std::strtoul(p, &e, 10);
        if (e == p) break;
        unsigned long b = a;
        p = e;
        if (*p == '-') { b = std::strtoul(p + 1, &e, 10); p = e; }
        if (f >= a && f <= b) return true;
        if (*p == ',') ++p;
    }
    return false;
}
inline bool RichOrder() {
    static const bool on = EnvInt("MSC_VGX_DRAWCENSUS_RICH", 0) != 0;
    return on;
}
struct Bucket {
    uint64_t recs = 0, stat = 0, verts = 0, tris = 0, noCull = 0, noCullTris = 0, opaqueNoCull = 0;
    uint64_t why[48] = {};
};
inline const char* const kWhy[] = {"nonadj", "tag", "static", "layout", "vs", "fs", "state", "skin", "mvonly", "texmonly", "tevonly", "blocks", "same", "first", "palonly", "s-layout", "s-vs", "s-fs", "s-state", "s-skin", "s-mv", "s-pal", "s-texm", "s-blk", "s-same", "g-tag", "g-lay", "g-vs", "g-fs", "g-state", "g-skin", "g-mv", "g-pal", "g-oth", "g-same", "b-up", "b-copy", "b-oth"};
enum { W_NONADJ, W_TAG, W_STATIC, W_LAYOUT, W_VS, W_FS, W_STATE, W_SKIN, W_MV, W_TEXM, W_TEV, W_BLOCKS, W_SAME, W_FIRST, W_PAL, S_LAYOUT, S_VS, S_FS, S_STATE, S_SKIN, S_MV, S_PAL, S_TEXM, S_BLK, S_SAME, G_TAG, G_LAY, G_VS, G_FS, G_STATE, G_SKIN, G_MV, G_PAL, G_OTH, G_SAME, B_UP, B_COPY, B_OTH, W_COUNT };

struct StateDetail { uint64_t texId = 0, texParam = 0, texMask = 0, blend = 0, depth = 0, cull = 0, cmask = 0, scissor = 0, viewport = 0, other = 0, multi = 0, pairs = 0; };
inline void NoteState(StateDetail& sd, const DrawRec& p, const DrawRec& d) {
    unsigned n = 0;
    ++sd.pairs;
    auto hit = [&](uint64_t& c, bool f) { if (f) { ++c; ++n; } };
    bool tid = false, tpar = false;
    for (int i = 0; i < 8; ++i) { tid |= p.tex[i].texId != d.tex[i].texId; tpar |= std::memcmp(&p.tex[i], &d.tex[i], sizeof(TexBind)) != 0 && p.tex[i].texId == d.tex[i].texId; }
    const RenderState &a = p.rs, &b = d.rs;
    hit(sd.texId, tid);
    hit(sd.texParam, tpar);
    hit(sd.texMask, p.texMask != d.texMask);
    hit(sd.blend, a.blend != b.blend || a.blendSrc != b.blendSrc || a.blendDst != b.blendDst || a.blendEq != b.blendEq || a.dstAlphaPass != b.dstAlphaPass);
    hit(sd.depth, a.depthTest != b.depthTest || a.depthFunc != b.depthFunc || a.depthWrite != b.depthWrite);
    hit(sd.cull, a.cull != b.cull);
    hit(sd.cmask, a.colorWrite != b.colorWrite || a.alphaWrite != b.alphaWrite);
    hit(sd.scissor, a.scX != b.scX || a.scY != b.scY || a.scW != b.scW || a.scH != b.scH);
    hit(sd.viewport, a.vpX != b.vpX || a.vpY != b.vpY || a.vpW != b.vpW || a.vpH != b.vpH || a.zNear != b.zNear || a.zFar != b.zFar);
    hit(sd.other, a.lineWidth6 != b.lineWidth6);
    if (n > 1) ++sd.multi;
}
// MSC_VGX_DRAWCENSUS_SHADERS=<file>: every shader source that arrives (vertex "V<id>", fragment "F<id>") with its text, to that file
inline void NoteShaders(const CmdBuf& buf) {
    static FILE* f = [] { const char* e = std::getenv("MSC_VGX_DRAWCENSUS_SHADERS"); return e ? std::fopen(e, "w") : nullptr; }();
    if (!f) return;
    for (const ShaderSource* s : buf.shaders) {
        if (!s) continue;
        std::fprintf(f, "#### %c%u texMask=%u nPal=%u nBone=%u nMv=%u nTm=%u attr=%x\n%s\n", s->fragment ? 'F' : 'V', static_cast<unsigned>(s->id), static_cast<unsigned>(s->texMask), static_cast<unsigned>(s->nPal),
                     static_cast<unsigned>(s->nBone), static_cast<unsigned>(s->nMv), static_cast<unsigned>(s->nTm), static_cast<unsigned>(s->attrMask), s->text.c_str());
    }
    std::fflush(f);
}
inline void Frame(const CmdBuf& buf) {
    NoteShaders(buf);
    static StateDetail sdet;
    static std::map<std::string, Bucket> win;
    static uint64_t frames = 0, segs = 0, allRecs = 0;
    static const char* const sc[4] = {"other", "play", "replay", "nis"};
    static const char* const cn[CLS_COUNT] = {"unknown", "player", "item", "fx", "hud", "npc", "6?", "7?", "stadium", "stadium-a", "crowd", "sky", "far", "crowd-cap"};
    static const long orderFrame = EnvInt("MSC_VGX_DRAWCENSUS_ORDER", -1);
    const bool order = orderFrame == -2 || (orderFrame >= 0 && static_cast<long>(buf.frame) == orderFrame) || InFrames(buf.frame); // (-2: every buffer executed)
    const DrawRec* prev = nullptr;
    const DrawRec* last = nullptr; // the last draw, across any command between
    unsigned breaker = B_OTH;
    for (const Cmd& c : buf.cmds) {
        if (c.type != CMD_DRAW && order && RichOrder() && c.type != CMD_SHADER && c.type != CMD_PROGRAM && c.type != CMD_VARIANT)
            std::fprintf(stderr, "[dcr] %llu cmd %u %u\n", static_cast<unsigned long long>(buf.frame), static_cast<unsigned>(c.type), c.arg);
        if (c.type != CMD_DRAW) {
            if (c.type == CMD_COPY_TEX || c.type == CMD_COPY_DISP || c.type == CMD_PASS_BEGIN || c.type == CMD_SET_TARGET) breaker = B_COPY;
            else if (c.type == CMD_TEX_UPLOAD || c.type == CMD_BUF_UPLOAD || c.type == CMD_BUF_FREE || c.type == CMD_TEX_FREE) { if (breaker != B_COPY) breaker = B_UP; }
            else if (breaker != B_COPY) breaker = B_OTH;
            if (c.type == CMD_COPY_TEX || c.type == CMD_COPY_DISP || c.type == CMD_PASS_BEGIN || c.type == CMD_SET_TARGET) ++segs;
            if (c.type != CMD_SHADER && c.type != CMD_TEX_UPLOAD && c.type != CMD_BUF_UPLOAD && c.type != CMD_PROGRAM && c.type != CMD_VARIANT && c.type != CMD_TEX_FREE &&
                c.type != CMD_BUF_FREE)
                prev = nullptr;
            else if (prev) prev = nullptr; // (an upload between two draws is a command between them)
            continue;
        }
        const DrawRec& d = buf.draws[c.arg];
        unsigned why = W_FIRST;
        if (prev) {
            const DrawRec& p = *prev;
            if (p.cls != d.cls || p.layer != d.layer || p.fx != d.fx) why = W_TAG;
            else if (p.prim != d.prim || p.layoutMask != d.layoutMask) why = W_LAYOUT;
            else if (p.vsId != d.vsId) why = W_VS;
            else if (p.fsId != d.fsId) why = W_FS;
            else if (p.texMask != d.texMask || std::memcmp(&p.rs, &d.rs, sizeof p.rs) != 0 || std::memcmp(p.tex, d.tex, sizeof p.tex) != 0) { why = W_STATE; NoteState(sdet, p, d); }
            else if (p.skinBlock != d.skinBlock) why = W_SKIN;
            else {
                uint32_t m = 0;
                for (int k = 0; k < UB_COUNT; ++k)
                    if (p.ublock[k] != d.ublock[k]) m |= 1u << k;
                why = m == 0 ? W_SAME : m == (1u << UB_MV) ? W_MV : m == (1u << UB_TEXM) ? W_TEXM : m == (1u << UB_TEV) ? W_TEV : W_BLOCKS;
                if (m == (1u << UB_PAL)) why = W_PAL;
            }
            if ((p.vbuf != 0 || d.vbuf != 0) && why != W_FIRST && why != W_TAG) {
                // static buffers cannot join a record: say what else would have kept them apart
                const bool pal = why == W_PAL;
                why = why == W_LAYOUT ? S_LAYOUT : why == W_VS ? S_VS : why == W_FS ? S_FS : why == W_STATE ? S_STATE : why == W_SKIN ? S_SKIN : why == W_MV ? S_MV : pal ? S_PAL
                      : why == W_TEXM || why == W_TEV || why == W_BLOCKS ? S_BLK : S_SAME;
            }
        } else {
            why = W_NONADJ;
        }
        unsigned gwhy = G_OTH, gb = 0;
        if (!prev && last) { // what would the gap's neighbours have been (same tests, any static buffer allowed)
            const DrawRec& p = *last;
            gb = breaker;
            if (p.cls != d.cls || p.layer != d.layer || p.fx != d.fx) gwhy = G_TAG;
            else if (p.prim != d.prim || p.layoutMask != d.layoutMask) gwhy = G_LAY;
            else if (p.vsId != d.vsId) gwhy = G_VS;
            else if (p.fsId != d.fsId) gwhy = G_FS;
            else if (p.texMask != d.texMask || std::memcmp(&p.rs, &d.rs, sizeof p.rs) != 0 || std::memcmp(p.tex, d.tex, sizeof p.tex) != 0) gwhy = G_STATE;
            else if (p.skinBlock != d.skinBlock) gwhy = G_SKIN;
            else {
                uint32_t m = 0;
                for (int k = 0; k < UB_COUNT; ++k)
                    if (p.ublock[k] != d.ublock[k]) m |= 1u << k;
                gwhy = m == 0 ? G_SAME : m == (1u << UB_MV) ? G_MV : m == (1u << UB_PAL) ? G_PAL : G_OTH;
            }
        }
        auto add = [&](const std::string& k) {
            Bucket& b = win[k];
            ++b.recs;
            b.stat += d.vbuf != 0;
            b.verts += d.vtxCount;
            b.tris += d.prim == PRIM_TRIANGLES ? d.idxCount / 3 : 0;
            if (d.prim == PRIM_TRIANGLES && d.rs.cull == CULL_NONE) { ++b.noCull; b.noCullTris += d.idxCount / 3; b.opaqueNoCull += d.rs.blend == 0; }
            ++b.why[why];
            if (gb) { ++b.why[gwhy]; ++b.why[gb]; }
        };
        const uint8_t cl = d.cls < CLS_COUNT ? d.cls : 0;
        add(std::string(sc[buf.scene & 3]) + "/" + cn[cl]);
        add(std::string(sc[buf.scene & 3]) + "/TOTAL");
        ++allRecs;
        if (order && RichOrder()) {
            // MSC_VGX_DRAWCENSUS_RICH=1: one machine-readable line per record (offline batching analysis): geometry, state and block contents
            auto bh = [&](uint32_t off, uint32_t n) -> unsigned long long {
                if (off == kNoBlock || off + n > buf.uni.size()) return 0ull;
                uint64_t h = 1469598103934665603ull;
                const uint8_t* p = reinterpret_cast<const uint8_t*>(buf.uni.data() + off);
                for (uint32_t i = 0; i < n * 4; ++i) h = (h ^ p[i]) * 1099511628211ull;
                return static_cast<unsigned long long>(h);
            };
            const float* mv = d.ublock[UB_MV] != kNoBlock && d.ublock[UB_MV] + 24 <= buf.uni.size() ? buf.uni.data() + d.ublock[UB_MV] : nullptr;
            std::fprintf(stderr, "[dcr] %llu %u %u %u %u %u %u %u %03X %u %u %u %u %u %u", static_cast<unsigned long long>(buf.frame), c.arg, buf.scene & 3u, d.cls, d.layer, d.fx,
                         d.vsId, d.fsId, d.layoutMask, d.prim, d.vbuf, d.vtxOffset, d.vtxFirst, d.idxOffset, d.idxCount);
            std::fprintf(stderr, " %u %u", d.vtxCount, d.texMask);
            for (int t = 0; t < 8; ++t) std::fprintf(stderr, " %u:%u%u%u%u", d.tex[t].texId, d.tex[t].wrapS, d.tex[t].wrapT, d.tex[t].minFilter, d.tex[t].magFilter);
            const RenderState& r = d.rs;
            std::fprintf(stderr, " rs %u %u %u %u %u %u %u %u %u %u %u %d %d %d %d %g %g %g %g", r.blend, r.blendSrc, r.blendDst, r.blendEq, r.depthTest, r.depthFunc, r.depthWrite, r.cull,
                         r.colorWrite, r.alphaWrite, r.dstAlphaPass, r.scX, r.scY, r.scW, r.scH, r.vpX, r.vpY, r.vpW, r.vpH);
            std::fprintf(stderr, " ub %llx %llx %llx %llx %llx %llx sk %u", bh(d.ublock[UB_PROJ], kUbSize[UB_PROJ]), bh(d.ublock[UB_MV], 24), bh(d.ublock[UB_TEXM], 96),
                         bh(d.ublock[UB_PAL], 60), bh(d.ublock[UB_LIGHT], kUbSize[UB_LIGHT]), bh(d.ublock[UB_TEV], kUbSize[UB_TEV]), d.skinBlock != kNoBlock ? 1u : 0u);
            // the bone matrices of a GPU-skinned draw: "row:hash" of every nonzero 12-float bone row (same hash = same matrix)
            std::fprintf(stderr, " bones");
            if (d.skinBlock != kNoBlock && d.skinBlock + kSkinBlockFloats <= buf.uni.size()) {
                const float* sk = buf.uni.data() + d.skinBlock;
                int nb = 0;
                for (int r = 0; r < 32; ++r) {
                    bool nz = false;
                    for (int k = 0; k < 12; ++k) nz |= sk[r * 12 + k] != 0.0f;
                    if (!nz) continue;
                    uint32_t h = 2166136261u;
                    const uint8_t* q = reinterpret_cast<const uint8_t*>(sk + r * 12);
                    for (int k = 0; k < 48; ++k) h = (h ^ q[k]) * 16777619u;
                    std::fprintf(stderr, "%c%d:%x", nb++ ? ',' : ' ', r, h & 0xFFFFFF);
                }
                if (!nb) std::fprintf(stderr, " -");
            } else std::fprintf(stderr, " -");
            {
                // the position rows a palette draw fills (last nonzero row of the 64 position rows + 1) and the TEV block floats
                int lastRow = 0;
                if (d.ublock[UB_PAL] != kNoBlock && d.ublock[UB_PAL] + 256 <= buf.uni.size()) {
                    const float* pl = buf.uni.data() + d.ublock[UB_PAL];
                    for (int r = 0; r < 64; ++r)
                        if (pl[r * 4] != 0.0f || pl[r * 4 + 1] != 0.0f || pl[r * 4 + 2] != 0.0f || pl[r * 4 + 3] != 0.0f) lastRow = r + 1;
                }
                std::fprintf(stderr, " palrows %d tev", lastRow);
                if (d.ublock[UB_TEV] != kNoBlock && d.ublock[UB_TEV] + kUbSize[UB_TEV] <= buf.uni.size())
                    for (uint32_t k = 0; k < kUbSize[UB_TEV]; ++k) std::fprintf(stderr, " %.6g", buf.uni[d.ublock[UB_TEV] + k]);
                else std::fprintf(stderr, " -");
            }
            std::fprintf(stderr, " mv");
            for (int k = 0; k < 24; ++k) std::fprintf(stderr, " %.9g", mv ? mv[k] : 0.0f);
            std::fprintf(stderr, "\n");
        }
        if (order && !RichOrder())
            std::fprintf(stderr, "[drawcensus] f=%llu #%u %s cls=%s layer=%u fx=%u vs=%u fs=%u %s lay=%03X v=%u i=%u tex=%u/%u blend=%u z=%u%u why=%s\n", static_cast<unsigned long long>(buf.frame), c.arg,
                         sc[buf.scene & 3], cn[cl], d.layer, d.fx, d.vsId, d.fsId, d.vbuf ? "static" : "arena", d.layoutMask, d.vtxCount, d.idxCount, d.tex[0].texId, d.tex[1].texId, d.rs.blend, d.rs.depthTest, d.rs.depthWrite,
                         kWhy[why]);
        prev = &d;
        last = &d;
        breaker = B_OTH;
    }
    ++frames;
    const long w = Window();
    if (w > 0 && frames >= static_cast<uint64_t>(w)) {
        for (const auto& kv : win) {
            const Bucket& b = kv.second;
            std::fprintf(stderr, "[drawcensus] %llu frames, %-18s per frame: %6.1f recs (%5.1f static) %8.0f verts %7.0f tris | why:", static_cast<unsigned long long>(frames), kv.first.c_str(),
                         static_cast<double>(b.recs) / static_cast<double>(frames), static_cast<double>(b.stat) / static_cast<double>(frames), static_cast<double>(b.verts) / static_cast<double>(frames),
                         static_cast<double>(b.tris) / static_cast<double>(frames));
            for (int k = 0; k < W_COUNT; ++k)
                if (b.why[k]) std::fprintf(stderr, " %s %.1f", kWhy[k], static_cast<double>(b.why[k]) / static_cast<double>(frames));
            std::fprintf(stderr, " | cull-none recs %.1f (%.0f tris, opaque %.1f)\n", static_cast<double>(b.noCull) / static_cast<double>(frames), static_cast<double>(b.noCullTris) / static_cast<double>(frames), static_cast<double>(b.opaqueNoCull) / static_cast<double>(frames));
        }
        std::fprintf(stderr, "[drawcensus]   state-differs pairs (same shaders, layout; per frame): %.1f of which texId %.1f texParam %.1f texMask %.1f blend %.1f depth %.1f cull %.1f cmask %.1f scissor %.1f viewport %.1f other %.1f, several %.1f\n",
                     static_cast<double>(sdet.pairs) / static_cast<double>(frames), static_cast<double>(sdet.texId) / static_cast<double>(frames), static_cast<double>(sdet.texParam) / static_cast<double>(frames),
                     static_cast<double>(sdet.texMask) / static_cast<double>(frames), static_cast<double>(sdet.blend) / static_cast<double>(frames), static_cast<double>(sdet.depth) / static_cast<double>(frames),
                     static_cast<double>(sdet.cull) / static_cast<double>(frames), static_cast<double>(sdet.cmask) / static_cast<double>(frames), static_cast<double>(sdet.scissor) / static_cast<double>(frames),
                     static_cast<double>(sdet.viewport) / static_cast<double>(frames), static_cast<double>(sdet.other) / static_cast<double>(frames), static_cast<double>(sdet.multi) / static_cast<double>(frames));
        sdet = StateDetail{};
        std::fprintf(stderr, "[drawcensus]   segments (copies, passes, targets) per frame %.1f\n", static_cast<double>(segs) / static_cast<double>(frames));
        win.clear();
        frames = segs = allRecs = 0;
    }
}
inline void Note(const CmdBuf& buf) {
    static const bool any = Window() > 0 || EnvInt("MSC_VGX_DRAWCENSUS_ORDER", -1) != -1 || std::getenv("MSC_VGX_DRAWCENSUS_FRAMES") || std::getenv("MSC_VGX_DRAWCENSUS_SHADERS");
    if (any) Frame(buf);
}

} // namespace drawcensus
} // namespace vgx
