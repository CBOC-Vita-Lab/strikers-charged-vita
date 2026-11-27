// MSC_VGX_HSRCENSUS=<seconds> (diagnostic, off; nothing is changed, no reorder is made): how many alpha-tested draws sit before the last opaque depth-writing draw of
// a scene, which is the order that keeps a tile GPU's hidden-surface removal from working (Galaxy, on a console: 6.6 to 9.8 ms in one stage). A scene is the run of
// draws between two commands that end one (copy, pass begin, target change). Per scene kind (other/play/replay/nis) and per window of <seconds> (1 means 10; also
// MSC_VGX_HSRCENSUS_FRAMES=<n> to count frames instead, for a run whose clock is not real time) one line:
//   scenes, draws, opaque (depth-writing, no blend, no test), tested (alpha test, no blend), before = tested before the scene's last opaque draw: draws and index counts,
//   inrun = the part of those that no barrier (blended draw, draw without depth write or test or colour write, HUD, any other command) lies between the draw and that
//   last opaque draw: the draws an exact reorder could look at, by the classes that carry them (needs the layer marks: MSC_DYNRES_FG=1 or MSC_VGX_FGCENSUS=1).
// Read by both back ends once per executed buffer (replays excluded), so it works in the armhf null harness. Per scene work: one pass over its draws.
#pragma once
#include "vgx_cmd.h"

#include <chrono>
#include <cstdio>
#include <vector>

namespace vgx {
namespace hsrcensus {

inline long Setting() {
    static const long v = EnvInt("MSC_VGX_HSRCENSUS", 0);
    return v;
}
struct Acc {
    uint64_t scenes = 0, draws = 0, opaque = 0, tested = 0, testedIdx = 0, before = 0, beforeIdx = 0, inrun = 0, inrunIdx = 0, blendedBefore = 0;
    uint64_t byCls[CLS_COUNT] = {}, byClsIdx[CLS_COUNT] = {};
};
inline void Note(const CmdBuf& buf) {
    if (Setting() <= 0) return;
    static std::vector<uint8_t> atest; // by fragment shader id: the alpha test is in the shader (shaders are immutable, ids never reused)
    static Acc acc[4];
    static uint64_t frames = 0;
    static std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    static const long winFrames = EnvInt("MSC_VGX_HSRCENSUS_FRAMES", 0);
    static const char* const sc[4] = {"other", "play", "replay", "nis"};
    static const char* const cn[CLS_COUNT] = {"unk", "player", "item", "fx", "hud", "npc", "6?", "7?", "stadium", "stadium-a", "crowd", "sky", "far", "crowd-cap"};
    for (const Cmd& c : buf.cmds)
        if (c.type == CMD_SHADER && c.arg < buf.shaders.size() && buf.shaders[c.arg] && buf.shaders[c.arg]->fragment) {
            const ShaderSource* s = buf.shaders[c.arg];
            if (s->id >= atest.size()) atest.resize(static_cast<size_t>(s->id) + 1, 0);
            atest[s->id] = s->cAlphaOn ? 1 : 0;
        }
    Acc& a = acc[buf.scene & 3];
    // Pass 1 over one scene: the draws as (kind, index count, class, barrier-before) and where the last opaque draw is. kind 0 opaque, 1 tested, 2 other.
    struct D { uint8_t kind, cls, barrier; uint32_t idx; };
    static std::vector<D> sd;
    auto flush = [&]() {
        if (sd.empty()) return;
        int last = -1;
        for (int i = static_cast<int>(sd.size()) - 1; i >= 0; --i)
            if (sd[i].kind == 0) { last = i; break; }
        ++a.scenes;
        a.draws += sd.size();
        for (const D& d : sd) { a.opaque += d.kind == 0; a.tested += d.kind == 1; a.testedIdx += d.kind == 1 ? d.idx : 0; }
        for (int i = 0; i < last; ++i) {
            if (sd[i].kind == 1) { ++a.before; a.beforeIdx += sd[i].idx; }
            else if (sd[i].kind == 2 && sd[i].barrier) ++a.blendedBefore;
        }
        // in-run: walk back from the last opaque draw; a barrier ends the run (the draws before it cannot be looked at across it)
        for (int i = last - 1; i >= 0; --i) {
            if (sd[i].barrier) break;
            if (sd[i].kind == 1) { ++a.inrun; a.inrunIdx += sd[i].idx; a.byCls[sd[i].cls < CLS_COUNT ? sd[i].cls : 0]++; a.byClsIdx[sd[i].cls < CLS_COUNT ? sd[i].cls : 0] += sd[i].idx; }
        }
        sd.clear();
    };
    for (const Cmd& c : buf.cmds) {
        if (c.type == CMD_DRAW) {
            const DrawRec& d = buf.draws[c.arg];
            const RenderState& rs = d.rs;
            const bool at = d.fsId < atest.size() && atest[d.fsId];
            const bool plain = !rs.blend && rs.depthWrite && rs.depthTest && rs.colorWrite && rs.alphaWrite && !rs.dstAlphaPass && d.prim == PRIM_TRIANGLES;
            D e;
            e.kind = plain ? (at ? 1 : 0) : 2;
            e.barrier = plain ? 0 : 1;
            if (d.cls == CLS_HUD) e.barrier = 1;
            e.cls = d.cls;
            e.idx = d.idxCount;
            sd.push_back(e);
        } else if (c.type == CMD_COPY_TEX || c.type == CMD_COPY_DISP || c.type == CMD_PASS_BEGIN || c.type == CMD_SET_TARGET) {
            flush();
        }
    }
    flush();
    ++frames;
    const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const bool due = winFrames > 0 ? frames >= static_cast<uint64_t>(winFrames) : el >= (Setting() == 1 ? 10.0 : static_cast<double>(Setting()));
    if (!due) return;
    for (int s = 0; s < 4; ++s) {
        const Acc& x = acc[s];
        if (!x.scenes) continue;
        const double n = static_cast<double>(frames);
        std::fprintf(stderr, "[hsrcensus] %s: %.0f frames, per frame: scenes %.2f draws %.1f opaque %.1f tested %.1f (%.0f idx) | tested before the last opaque: %.1f (%.0f idx), %.1f blended/other barriers before it; in-run (no barrier between): %.1f (%.0f idx) | in-run by class:",
                     sc[s], n, x.scenes / n, x.draws / n, x.opaque / n, x.tested / n, x.testedIdx / n, x.before / n, x.beforeIdx / n, x.blendedBefore / n, x.inrun / n, x.inrunIdx / n);
        for (int k = 0; k < CLS_COUNT; ++k)
            if (x.byCls[k]) std::fprintf(stderr, " %s %.1f (%.0f idx)", cn[k], x.byCls[k] / n, x.byClsIdx[k] / n);
        std::fprintf(stderr, "\n");
        acc[s] = Acc{};
    }
    frames = 0;
    t0 = std::chrono::steady_clock::now();
}

} // namespace hsrcensus
} // namespace vgx
