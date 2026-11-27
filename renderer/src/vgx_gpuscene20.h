// gpuscene20: three diagnostics for the question "where do the GPU milliseconds of The Dump go" (all default OFF, none changes a pixel or the simulation).
//  MSC_VITA_RT_SCENES=1   (console; H sets it up) vitaGL 0043 keeps GPU time per kind of scene (target size, display or not); every MSC_VITA_RT_LOG presents `[gpu scenes]`. Implies the split
//                         and the GPU timers. FormatScenes() below makes the line from two cumulative copies of vitaGL's table.
//  MSC_VGX_PBEST=1        per picture: the sum over draws of vertices x (16 + 8 per texture coordinate + 4 colour) bytes + 12 bytes a triangle, `[pb est]` every MSC_VGX_PBEST_WINDOW (300) pictures.
//                         ESTIMATE of how much of the parameter buffer a picture takes; it ranks scenes, MSC_VITA_PB_MIB runs decide. Pictures in flight: MSC_VGX_PBEST_FLIGHT (2, assumed).
//  MSC_VGX_TEXCENSUS20=1  `[texlayout20]` at the first play picture, when most textures are freed (a stadium ends) and every MSC_VGX_TEXCENSUS20_WINDOW (3600) pictures: live textures by DXT /
//                         uncompressed (linear unless MSC_VGX_TEXSWIZZLE moved them), with mips, 128 texels or more on a side (the anisotropic ones); with MSC_VGX_FGCENSUS on also the 8
//                         textures with most fetches (pixels x textures sampled, frame-buffer draws).
// Included in vgx_backend_gl.cpp (inside its anonymous namespace, before Draw) and by the unit test; includes nothing of vitaGL.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace gs20 {

inline bool EnvOn(const char* name) {
    const char* e = std::getenv(name);
    return e && *e && std::atoi(e) != 0;
}
inline long EnvNum(const char* name, long def) {
    const char* e = std::getenv(name);
    return e && *e ? std::atol(e) : def;
}

// ---- [gpu scenes]: the table of vitaGL's vglRtSceneKind, as plain numbers (the console host copies it over)
struct Kind { uint64_t busy = 0, tiling = 0, pixel = 0; uint32_t scenes = 0; uint16_t w = 0, h = 0; uint8_t display = 0, used = 0; };
// `window` = frames in the window; a and b are the table at its start and end (n entries, the last is "other"; ms a frame)
inline std::string FormatScenes(const Kind* a, const Kind* b, int n, uint32_t frames) {
    char buf[1536];
    std::string out;
    std::snprintf(buf, sizeof buf, "[gpu scenes] %u frames:", frames);
    out = buf;
    const double f = frames ? static_cast<double>(frames) : 1.0;
    bool any = false;
    for (int i = 0; i < n; ++i) {
        const Kind& e = b[i];
        if (!e.used) continue;
        const double ds = static_cast<double>(e.scenes - a[i].scenes), dp = static_cast<double>(e.pixel - a[i].pixel), dt = static_cast<double>(e.tiling - a[i].tiling);
        if (ds <= 0) continue;
        const bool other = i == n - 1;
        if (other) std::snprintf(buf, sizeof buf, "%s other x%.1f tiling %.2f pixel %.2f ms", any ? " |" : "", ds / f, dt / 1000.0 / f, dp / 1000.0 / f);
        else if (e.display) std::snprintf(buf, sizeof buf, "%s display %ux%u x%.1f tiling %.2f pixel %.2f ms (includes the wait for a display buffer)", any ? " |" : "", e.w, e.h, ds / f, dt / 1000.0 / f, dp / 1000.0 / f);
        else std::snprintf(buf, sizeof buf, "%s %ux%u x%.1f/frame tiling %.2f pixel %.2f ms", any ? " |" : "", e.w, e.h, ds / f, dt / 1000.0 / f, dp / 1000.0 / f);
        out += buf;
        any = true;
    }
    if (!any) out += " no timed scene (needs MSC_VITA_RT_SCENES=1 and the GPU timers)";
    out += " (ms a frame; tiling = submission to vertex job done, a lower bound; pixel = after it)\n";
    return out;
}

// ---- [pb est]
struct Pb {
    int on = -1;
    uint64_t picBytes = 0, sumBytes = 0, maxBytes = 0, pics = 0, draws = 0;
    long window = 300, flight = 2;
};
inline Pb& PbSt() { static Pb p; return p; }
inline bool PbOn() {
    Pb& p = PbSt();
    if (p.on < 0) { p.on = EnvOn("MSC_VGX_PBEST") ? 1 : 0; p.window = EnvNum("MSC_VGX_PBEST_WINDOW", 300); p.flight = EnvNum("MSC_VGX_PBEST_FLIGHT", 2); if (p.window < 1) p.window = 300; if (p.flight < 1) p.flight = 1; }
    return p.on != 0;
}
inline uint64_t PbBytes(uint32_t vtxCount, uint32_t idxCount, bool triangles, unsigned texCoords) {
    return static_cast<uint64_t>(vtxCount) * (16u + 8u * texCoords + 4u) + (triangles ? static_cast<uint64_t>(idxCount / 3) * 12u : 0u);
}
inline void PbDraw(uint32_t vtxCount, uint32_t idxCount, bool triangles, unsigned texCoords) {
    Pb& p = PbSt();
    p.picBytes += PbBytes(vtxCount, idxCount, triangles, texCoords);
    ++p.draws;
}
// one picture ends; pbMib: the live MSC_VITA_PB_MIB (16 when unset)
inline void PbPresent(long pbMib) {
    Pb& p = PbSt();
    if (!p.draws) return;
    p.sumBytes += p.picBytes;
    p.maxBytes = std::max(p.maxBytes, p.picBytes);
    ++p.pics;
    p.picBytes = 0; p.draws = 0;
    if (static_cast<long>(p.pics) < p.window) return;
    const double mib = 1.0 / (1024.0 * 1024.0), mean = p.sumBytes * mib / p.pics, mx = p.maxBytes * mib;
    std::fprintf(stderr, "[pb est] %llu pictures: mean %.2f MiB / max %.2f MiB a picture, x%ld pictures in flight (assumed) = mean %.2f / max %.2f MiB of %ld MiB (ESTIMATE: vertices x (16 + 8 per texcoord + 4) + 12 a triangle; ranks scenes, a MSC_VITA_PB_MIB run decides)\n",
                 static_cast<unsigned long long>(p.pics), mean, mx, p.flight, mean * p.flight, mx * p.flight, pbMib);
    p.sumBytes = p.maxBytes = p.pics = 0;
}

// ---- [texlayout20]
struct TexInfo { uint16_t w = 0, h = 0; uint8_t format = 0, levels = 1; bool genMips = false; uint32_t bytes = 0; uint64_t fetch = 0; };
struct TexCen {
    int on = -1;
    std::unordered_map<uint32_t, TexInfo> tex;
    uint64_t liveBytes = 0;
    size_t peakCount = 0;
    std::string peakLine;   // the census line of the moment with most live textures
    bool kickoff = false;
    uint64_t pics = 0;
    long window = 3600;
};
inline TexCen& TcSt() { static TexCen t; return t; }
inline bool TcOn() {
    TexCen& t = TcSt();
    if (t.on < 0) { t.on = EnvOn("MSC_VGX_TEXCENSUS20") ? 1 : 0; t.window = EnvNum("MSC_VGX_TEXCENSUS20_WINDOW", 3600); if (t.window < 1) t.window = 3600; }
    return t.on != 0;
}
inline bool IsDxt(uint8_t fmt) { return fmt == 1 || fmt == 3; }   // TF_DXT1, TF_DXT5
// bytes: all levels given; fmt: TexUpload::format
inline void TcUpload(uint32_t id, uint16_t w, uint16_t h, uint8_t fmt, size_t nLevels, bool genMips, uint64_t bytes) {
    TexCen& t = TcSt();
    TexInfo& i = t.tex[id];
    t.liveBytes -= i.bytes;
    const bool mipped = nLevels > 1 || genMips;
    i.w = w; i.h = h; i.format = fmt; i.levels = static_cast<uint8_t>(std::min<size_t>(nLevels, 255)); i.genMips = genMips;
    i.bytes = static_cast<uint32_t>(genMips && nLevels == 1 ? bytes * 4 / 3 : bytes);
    (void)mipped;
    t.liveBytes += i.bytes;
}
inline void TcFree(uint32_t id) {
    TexCen& t = TcSt();
    auto it = t.tex.find(id);
    if (it == t.tex.end()) return;
    t.liveBytes -= it->second.bytes;
    t.tex.erase(it);
}
inline std::string TcLine(const char* when, const std::unordered_map<uint32_t, TexInfo>& tex) {
    uint64_t dxtN = 0, dxtB = 0, linN = 0, linB = 0, mipN = 0, mipB = 0, bigN = 0, bigDxt = 0, bigLin = 0, bigMip = 0;
    for (const auto& kv : tex) {
        const TexInfo& i = kv.second;
        const bool dxt = IsDxt(i.format), mip = i.levels > 1 || i.genMips, big = std::max(i.w, i.h) >= 128;
        if (dxt) { ++dxtN; dxtB += i.bytes; } else { ++linN; linB += i.bytes; }
        if (mip) { ++mipN; mipB += i.bytes; }
        if (big) { ++bigN; if (dxt) ++bigDxt; else ++bigLin; if (mip) ++bigMip; }
    }
    const double mb = 1.0 / (1024.0 * 1024.0);
    char b[900];
    std::snprintf(b, sizeof b, "[texlayout20] %s: %u live textures, %.1f MiB: DXT %llu / %.1f MiB, uncompressed linear %llu / %.1f MiB (MSC_VGX_TEXSWIZZLE moves some), with mips %llu / %.1f MiB, 128 texels or more a side %llu (DXT %llu, uncompressed %llu; with mips, the anisotropic ones: %llu)\n",
                  when, static_cast<unsigned>(tex.size()), (dxtB + linB) * mb, static_cast<unsigned long long>(dxtN), dxtB * mb, static_cast<unsigned long long>(linN), linB * mb, static_cast<unsigned long long>(mipN), mipB * mb,
                  static_cast<unsigned long long>(bigN), static_cast<unsigned long long>(bigDxt), static_cast<unsigned long long>(bigLin), static_cast<unsigned long long>(bigMip));
    return b;
}
inline void TcPrint(const char* when) {
    TexCen& t = TcSt();
    std::fputs(TcLine(when, t.tex).c_str(), stderr);
    std::vector<std::pair<uint64_t, uint32_t>> top;
    for (const auto& kv : t.tex) if (kv.second.fetch) top.emplace_back(kv.second.fetch, kv.first);
    std::sort(top.begin(), top.end(), [](const auto& x, const auto& y) { return x.first != y.first ? x.first > y.first : x.second < y.second; });
    for (size_t k = 0; k < top.size() && k < 8; ++k) {
        const TexInfo& i = t.tex[top[k].second];
        std::fprintf(stderr, "[texlayout20]   top %u: tex %u %ux%u %s%s fetches %.2f M so far (frame-buffer draws, MSC_VGX_FGCENSUS)\n", static_cast<unsigned>(k + 1), top[k].second, i.w, i.h, IsDxt(i.format) ? "DXT" : "linear", (i.levels > 1 || i.genMips) ? " mips" : "", top[k].first / 1e6);
    }
}
// a draw's fetches: pixels x one fetch per texture sampled, for each texture
inline void TcFetch(const uint32_t* ids, int n, double px) {
    TexCen& t = TcSt();
    if (px <= 0) return;
    for (int k = 0; k < n; ++k) { auto it = t.tex.find(ids[k]); if (it != t.tex.end()) it->second.fetch += static_cast<uint64_t>(px); }
}
// one picture ends; play: it was a play scene
inline void TcPresent(bool play) {
    TexCen& t = TcSt();
    ++t.pics;
    if (t.tex.size() > t.peakCount) { t.peakCount = t.tex.size(); t.peakLine = TcLine("stadium end (the most textures live since the last such line)", t.tex); }
    else if (t.peakCount > 32 && t.tex.size() * 2 < t.peakCount) {   // most of the textures were freed: a stadium ended
        std::fputs(t.peakLine.c_str(), stderr);
        t.peakCount = t.tex.size(); t.peakLine.clear();
    }
    if (play && !t.kickoff) { t.kickoff = true; TcPrint("kick-off"); }
    if (static_cast<long>(t.pics % static_cast<uint64_t>(t.window)) == 0) TcPrint("periodic");
}

}   // namespace gs20
