// W5 SEED: the union of two shader warm-up lists (vgx_shaderlist.bin, format in src/vgx_draw.cpp EnsureShaderList), in memory.
//
// Why: the list on a console grows by itself (every key a session meets is appended), and a new package brings a larger one. Copying the package's
// list over the console's would lose the keys this console learned; copying it only when the console has none (the code before W5 SEED) leaves a console
// that has an older list without the new keys for ever. The union keeps both: the package's keys first (in the package's order: the warm-up order),
// then the console's own.
//
// Header only and free of console includes, so a host program can test it (tests/listmerge_test.cpp; tools/seed/shl.py merge is the reference).
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace vgx_listmerge {

struct List {
    uint32_t vsSize = 0, fsSize = 0, gen = 0;
    std::vector<std::string> vs, fs;
    std::vector<std::pair<uint32_t, uint32_t>> prog;          // ids into vs / fs
    std::vector<std::array<uint32_t, 4>> var;                  // vs, fs, layout, blend
    bool ok = false;
};

inline uint32_t Rd32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }

// Parses a list file's bytes. ok = false when the magic is wrong, a record is cut off, or an id is out of range.
inline List Parse(const uint8_t* d, size_t n) {
    List L;
    if (n < 24 || std::memcmp(d, "VGXSHL01", 8) != 0) return L;
    L.vsSize = Rd32(d + 8);
    L.fsSize = Rd32(d + 12);
    L.gen = Rd32(d + 16);
    if (L.vsSize == 0 || L.fsSize == 0 || L.vsSize > 4096 || L.fsSize > 4096) return L;
    size_t p = 24;
    while (p + 4 <= n) {
        const uint32_t t = Rd32(d + p);
        p += 4;
        if (t == 1) {
            if (p + L.vsSize > n) return L;
            L.vs.emplace_back(reinterpret_cast<const char*>(d + p), L.vsSize);
            p += L.vsSize;
        } else if (t == 2) {
            if (p + L.fsSize > n) return L;
            L.fs.emplace_back(reinterpret_cast<const char*>(d + p), L.fsSize);
            p += L.fsSize;
        } else if (t == 3) {
            if (p + 8 > n) return L;
            const uint32_t v = Rd32(d + p), f = Rd32(d + p + 4);
            if (v >= L.vs.size() || f >= L.fs.size()) return L;
            L.prog.emplace_back(v, f);
            p += 8;
        } else if (t == 4) {
            if (p + 16 > n) return L;
            const uint32_t v = Rd32(d + p), f = Rd32(d + p + 4);
            if (v >= L.vs.size() || f >= L.fs.size()) return L;
            L.var.push_back({v, f, Rd32(d + p + 8), Rd32(d + p + 12)});
            p += 16;
        } else {
            return L;
        }
    }
    L.ok = (p == n);
    return L;
}

inline void Put32(std::string& s, uint32_t v) { s.append(reinterpret_cast<const char*>(&v), 4); }

// The union of `first` and `second` (first's keys keep their order and come first). Lists from another generator (key sizes or generation differ)
// cannot be merged: the result is then `first` alone. Duplicates are dropped, ids renumbered, so the result is what the reader accepts without a rewrite.
inline std::string Merge(const List& first, const List& second, unsigned* added = nullptr) {
    const bool useSecond = second.ok && first.ok && first.vsSize == second.vsSize && first.fsSize == second.fsSize && first.gen == second.gen;
    const List* in[2] = {&first, useSecond ? &second : nullptr};
    std::unordered_map<std::string, uint32_t> vsId, fsId;
    std::unordered_map<uint64_t, bool> progSeen;
    std::vector<std::string> vs, fs;
    std::vector<std::pair<uint32_t, uint32_t>> prog;
    std::vector<std::array<uint32_t, 4>> var;
    std::unordered_map<std::string, bool> varSeen;
    unsigned fromSecond = 0;
    for (int w = 0; w < 2; ++w) {
        const List* L = in[w];
        if (!L) continue;
        std::vector<uint32_t> mapV(L->vs.size()), mapF(L->fs.size());
        for (size_t i = 0; i < L->vs.size(); ++i) {
            auto it = vsId.emplace(L->vs[i], static_cast<uint32_t>(vs.size()));
            if (it.second) { vs.push_back(L->vs[i]); fromSecond += (w == 1); }
            mapV[i] = it.first->second;
        }
        for (size_t i = 0; i < L->fs.size(); ++i) {
            auto it = fsId.emplace(L->fs[i], static_cast<uint32_t>(fs.size()));
            if (it.second) { fs.push_back(L->fs[i]); fromSecond += (w == 1); }
            mapF[i] = it.first->second;
        }
        for (const auto& pr : L->prog) {
            const uint32_t v = mapV[pr.first], f = mapF[pr.second];
            if (progSeen.emplace((static_cast<uint64_t>(v) << 32) | f, true).second) { prog.emplace_back(v, f); fromSecond += (w == 1); }
        }
        for (const auto& t : L->var) {
            std::array<uint32_t, 4> m = {mapV[t[0]], mapF[t[1]], t[2], t[3]};
            std::string key(reinterpret_cast<const char*>(m.data()), 16);
            if (varSeen.emplace(key, true).second) { var.push_back(m); fromSecond += (w == 1); }
        }
    }
    if (added) *added = fromSecond;
    std::string out("VGXSHL01", 8);
    Put32(out, first.vsSize);
    Put32(out, first.fsSize);
    Put32(out, first.gen);
    Put32(out, 0);
    for (const auto& k : vs) { Put32(out, 1); out += k; }
    for (const auto& k : fs) { Put32(out, 2); out += k; }
    for (const auto& pr : prog) { Put32(out, 3); Put32(out, pr.first); Put32(out, pr.second); }
    for (const auto& t : var) { Put32(out, 4); for (int i = 0; i < 4; ++i) Put32(out, t[i]); }
    return out;
}

}  // namespace vgx_listmerge
