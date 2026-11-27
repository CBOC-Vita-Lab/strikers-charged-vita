// 1.3 (wave 11, learn16) MSC_CUT_LEARN: the learned list of first uses at a cut (pure logic, no GL; host test renderer/tests/cutlearn.cpp).
//
// What a held scene (goal cut-scene, replay, Mega Strike cut-in) uses for the first time after kick-off is written down with the stadium it was seen in, on this console's own card, and the
// next match of that stadium makes it ahead, behind the loading picture and before kick-off, inside a KiB budget and a count budget:
//   bit 1  textures: a texture the renderer had to upload after kick-off (key: content hash, size, format; the address the game had it at). Made ahead only if the pretouch of this run showed the
//          renderer a texture at that address (so the address is the game's own, not a stale one) and the texels there still hash to the listed value.
//   bit 2  pipeline variants: a (vertex shader, fragment shader, vertex layout, blend, target class) the patcher had to build after kick-off. Shaders are named by their key hash (the ids
//          change from run to run). Made ahead as the empty triangle of CMD_VARIANT.
// The bits are separate: a texture costs memory when made early, a variant costs patcher memory but no texels.
// Render only: nothing here reads or writes the game's state.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

namespace vgx::cutlearn {

enum : int { kBitTex = 1, kBitVar = 2 };

// What the back end's match ledger tells the front end (and itself): 0 no match, 1 a match is loaded and kick-off has not come, 2 kicked off. The index counts matches of the process.
inline std::atomic<uint32_t> g_phase{0}, g_matchIdx{0}, g_stadium{0};

struct TexRec {
    uint64_t hash = 0;     // content hash of the texels (XXH3 of the bytes the renderer read)
    uint32_t addr = 0;     // where the game had it (checked against what the pretouch showed this run)
    uint16_t w = 0, h = 0;
    uint8_t fmt = 0;
    uint32_t kib = 0;      // bytes the upload held, in KiB (rounded up)
};
struct VarRec {
    uint64_t vsHash = 0, fsHash = 0;   // ShaderSource::keyHash
    uint16_t layout = 0, blend = 0;
    uint8_t cls = 0;                   // bit 0: made in a pass target, bits 1..: the target's sample index at the time
};

inline uint64_t TexKey(const TexRec& t) { return t.hash ^ (static_cast<uint64_t>(t.w) << 48) ^ (static_cast<uint64_t>(t.h) << 32) ^ (static_cast<uint64_t>(t.fmt) << 24); }
inline uint64_t VarKey(const VarRec& v) {
    uint64_t k = v.vsHash * 0x9E3779B97F4A7C15ull;
    k ^= (v.fsHash + 0x7F4A7C15ull) * 0xBF58476D1CE4E5B9ull;
    k ^= (static_cast<uint64_t>(v.layout) << 20) ^ (static_cast<uint64_t>(v.blend) << 4) ^ v.cls;
    k ^= k >> 29;
    return k * 0x94D049BB133111EBull;
}

struct Limits {
    uint32_t maxTex = 256, maxVar = 1024;   // entries kept in the file (the list never grows past them)
    uint32_t texKib = 2048;                 // KiB made ahead at one kick-off
    uint32_t varPerMatch = 96;              // variants made ahead at one kick-off
};

struct List {
    std::vector<TexRec> tex;
    std::vector<VarRec> var;
    std::unordered_set<uint64_t> knownTex, knownVar;   // every key in the list
    uint32_t dropped = 0;                 // records refused because the list was full
    bool AddTex(const TexRec& t, const Limits& lim) {
        if (!knownTex.insert(TexKey(t)).second) return false;
        if (tex.size() >= lim.maxTex) { ++dropped; return false; }
        tex.push_back(t);
        return true;
    }
    bool AddVar(const VarRec& v, const Limits& lim) {
        if (!knownVar.insert(VarKey(v)).second) return false;
        if (var.size() >= lim.maxVar) { ++dropped; return false; }
        var.push_back(v);
        return true;
    }
    bool HasTex(const TexRec& t) const { return knownTex.count(TexKey(t)) != 0; }
    bool HasVar(const VarRec& v) const { return knownVar.count(VarKey(v)) != 0; }
};

// ---- text form: one record a line, appended as found. "T <hash> <addr> <w> <h> <fmt> <kib>" and "V <vsHash> <fsHash> <layout> <blend> <cls>" (hex for hashes and the address).
inline std::string FormatTex(const TexRec& t) {
    char b[128];
    std::snprintf(b, sizeof b, "T %llx %x %u %u %u %u\n", static_cast<unsigned long long>(t.hash), t.addr, t.w, t.h, t.fmt, t.kib);
    return b;
}
inline std::string FormatVar(const VarRec& v) {
    char b[128];
    std::snprintf(b, sizeof b, "V %llx %llx %u %u %u\n", static_cast<unsigned long long>(v.vsHash), static_cast<unsigned long long>(v.fsHash), v.layout, v.blend, v.cls);
    return b;
}
// One line into the list; false for a line that is not a record, is damaged or is a duplicate (a damaged card line is skipped, never trusted).
inline bool ParseLine(const char* line, List& l, const Limits& lim) {
    char c = 0;
    if (std::sscanf(line, " %c", &c) != 1) return false;
    if (c == 'T') {
        unsigned long long h = 0; unsigned a = 0, w = 0, hh = 0, f = 0, k = 0;
        if (std::sscanf(line, " T %llx %x %u %u %u %u", &h, &a, &w, &hh, &f, &k) != 6 || !w || !hh || w > 4096 || hh > 4096 || f > 255 || k > 65536) return false;
        return l.AddTex(TexRec{h, a, static_cast<uint16_t>(w), static_cast<uint16_t>(hh), static_cast<uint8_t>(f), k}, lim);
    }
    if (c == 'V') {
        unsigned long long a = 0, b = 0; unsigned lay = 0, bl = 0, cl = 0;
        if (std::sscanf(line, " V %llx %llx %u %u %u", &a, &b, &lay, &bl, &cl) != 5 || lay > 0x1FFFu || bl > 0xFFFu || cl > 15 || !a || !b) return false;
        return l.AddVar(VarRec{a, b, static_cast<uint16_t>(lay), static_cast<uint16_t>(bl), static_cast<uint8_t>(cl)}, lim);
    }
    return false;
}
inline uint32_t ParseText(const char* text, size_t n, List& l, const Limits& lim) {
    uint32_t got = 0;
    size_t i = 0;
    while (i < n) {
        size_t j = i;
        while (j < n && text[j] != '\n') ++j;
        char line[160];
        const size_t len = j - i < sizeof line - 1 ? j - i : sizeof line - 1;
        std::memcpy(line, text + i, len);
        line[len] = 0;
        if (ParseLine(line, l, lim)) ++got;
        i = j + 1;
    }
    return got;
}

// ---- the budget: what is made ahead at one kick-off. Textures are taken in the file's order while their KiB fit (one that does not fit is skipped, the next may); variants are the first N.
struct Plan {
    std::vector<uint32_t> tex, var;   // indexes into the list
    uint32_t texKib = 0, texSkipped = 0, varSkipped = 0;
};
inline Plan MakePlan(const List& l, int bits, const Limits& lim) {
    Plan p;
    if (bits & kBitTex)
        for (uint32_t i = 0; i < l.tex.size(); ++i) {
            if (p.texKib + l.tex[i].kib > lim.texKib) { ++p.texSkipped; continue; }
            p.texKib += l.tex[i].kib;
            p.tex.push_back(i);
        }
    if (bits & kBitVar)
        for (uint32_t i = 0; i < l.var.size(); ++i) {
            if (p.var.size() >= lim.varPerMatch) { ++p.varSkipped; continue; }
            p.var.push_back(i);
        }
    return p;
}

}  // namespace vgx::cutlearn
