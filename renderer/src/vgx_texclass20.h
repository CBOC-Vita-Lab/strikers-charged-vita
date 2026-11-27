// swz20: a census of texture uploads by class (format, shape, size, mip levels, re-upload) and what MSC_VGX_TEXSWIZZLE did with each class. Diagnostic only: [texclass] lines, nothing
// else changes. Host and console both count (the host cannot swizzle, so there "swizzled" reads as the class it WOULD have been: outcome "ok" = eligible).
#pragma once
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <map>
#include <string>
#include <vector>
namespace vgx {
struct TexClassStat {
    std::map<std::string, std::pair<uint64_t, uint64_t>> cls;   // key -> uploads, bytes
    uint64_t total = 0;
    static const char* FmtName(uint8_t f) {
        static const char* n[] = {"RGBA8", "DXT1", "LA88", "DXT5", "RGB565", "RGBA5551", "RGBA4444", "I8"};
        return f < 8 ? n[f] : "?";
    }
    void Note(uint8_t fmt, uint32_t w, uint32_t h, size_t levels, bool genMips, bool again, const char* outcome, uint64_t bytes) {
        const bool p2 = w && h && !(w & (w - 1)) && !(h & (h - 1));
        const uint32_t m = w > h ? w : h;
        char k[160];
        std::snprintf(k, sizeof k, "%-9s %-6s %-6s side%-5s %s%s%s -> %s", FmtName(fmt), w == h ? "square" : w > h ? "wide" : "tall", p2 ? "pow2" : "npot",
                      m < 32 ? "<32" : m < 64 ? "32" : m < 128 ? "64" : m < 256 ? "128" : m < 512 ? "256" : "512+", levels > 1 ? "mips(authored) " : genMips ? "mips(gen) " : "nomips ", again ? "again " : "first ", "", outcome);
        auto& e = cls[k];
        ++e.first;
        e.second += bytes;
        if (!(++total & 255u)) Print();
    }
    void Print() const {
        std::fprintf(stderr, "[texclass] %llu uploads, class (format shape pow2 size mips first/again -> outcome): uploads, MB\n", static_cast<unsigned long long>(total));
        std::vector<std::pair<uint64_t, std::string>> v;
        for (const auto& e : cls) v.emplace_back(e.second.second, e.first);
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (size_t i = 0; i < v.size() && i < 40; ++i) {
            const auto& e = cls.at(v[i].second);
            std::fprintf(stderr, "[texclass]   %-80s %6llu %8.2f\n", v[i].second.c_str(), static_cast<unsigned long long>(e.first), e.second / 1048576.0);
        }
    }
};
}   // namespace vgx
