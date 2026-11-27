// vgx_heldscenes.h (1.3 wave 11, scenes16): fewer offscreen scenes per held picture (cut-scene, celebration, replay), and the census that says which ones there are.
//
// Pure logic, no GL: the back end (vgx_backend_gl.cpp, "scenes16" hooks) feeds it one record per scene and one call per picture.
//
// MSC_HELD_SCENES=<bits> (code default and Vita plan default: see TECHNICAL.md), acts only in held scenes (replay, cut-scene incl. celebration and walk-out):
//   1  held scenes keep their depth only where something reads it: the EFB scene's depth is stored/loaded as the RT hints already say (MSC_VGX_RT);
//      this bit extends the rule to what the hints leave conservative in held scenes (see HeldDepthRule in the back end)
//   2  the pass memo (MSC_VGX_PASSMEMO) and the effects run merge (MSC_FX_RUNMERGE) are on in held scenes if they are off there
//   4  MSC_VGX_PASSHALF (blur passes at half scale) in held scenes only (visible: softer glow)
// MSC_HELD_CENSUS=<n>: one [scenes] line per held picture class every n pictures (n=1: every held picture, 0: off); =-1: each scene of each held picture too.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace vgx {
namespace heldscenes {

constexpr unsigned kBitDepth = 1u, kBitMemo = 2u, kBitHalf = 4u, kBitsAll = 7u;

// MSC_HELD_SCENES text -> bits (decimal, 0x hex; anything else 0). Pure.
inline unsigned ParseBits(const char* s) {
    if (!s || !*s) return 0;
    char* end = nullptr;
    const unsigned long v = std::strtoul(s, &end, 0);
    if (end == s) return 0;
    return static_cast<unsigned>(v) & kBitsAll;
}
// The bits in force for a picture: only in held scenes (scene 2 replay, 3 nis = cut-scene/celebration/walk-out), never otherwise. Pure.
inline unsigned BitsFor(unsigned configured, int bufScene, bool replayBuffer) {
    (void)replayBuffer;
    return (bufScene == 2 || bufScene == 3) ? configured : 0u;
}

enum SceneKind : uint8_t { kEfb = 0, kPass = 1, kCopy = 2, kDisp = 3, kOther = 4 };
struct SceneRec {
    int16_t w = 0, h = 0;
    uint8_t kind = kOther, samples = 1, op = 0, cmdType = 0;
    bool noLoad = false, noStore = false;
    uint32_t draws = 0, depthDraws = 0, copies = 0;
    uint32_t dsKib = 0;     // depth KiB the scene loads + stores with no hint (2 x surface)
    uint32_t cmdIndex = 0;
    uint32_t touches = 0;   // held24: draws + clears + copies + quads the scene saw (1 with a clear op: a clear and nothing else)
    char why[16] = {};      // what asked for it (from the command or the back end's tag)
};

struct Census {
    int every = -2;         // MSC_HELD_CENSUS; -2: not read yet
    std::vector<SceneRec> cur;
    uint32_t pics[4] = {};          // held pictures by buf.scene
    uint64_t scenes[4] = {};        // their scenes
    uint64_t kinds[4][5] = {};      // by SceneKind
    uint64_t noLoad[4] = {}, noStore[4] = {}, loadStore[4] = {};   // scenes with neither hint: they load AND store
    uint64_t dsKib[4] = {}, dsSkipKib[4] = {};
    uint64_t emptyS[4] = {}, noDepthDraw[4] = {};   // scenes with no draw / with draws but none depth-tested
    uint64_t loadNoDepthRead[4] = {};               // scenes that load and store depth though no draw in them tests depth (candidates for bit 1)
    uint64_t clearOnly[4] = {};     // held24: scenes that began with a clear op and saw nothing else (a dead clear unless a later scene reads the target)
    uint32_t maxScenes[4] = {};
};

inline const char* SceneName(int s) { static const char* const n[4] = {"other", "play", "replay", "cut-scene"}; return (s >= 0 && s < 4) ? n[s] : "?"; }
inline const char* KindName(uint8_t k) { static const char* const n[5] = {"efb", "pass", "copy", "disp", "other"}; return k < 5 ? n[k] : "?"; }

inline void Add(Census& C, int bufScene, uint64_t frame, std::vector<SceneRec>& scenes, int logEvery, uint64_t& state) {
    (void)state;
    if (bufScene < 0 || bufScene > 3) return;
    const int s = bufScene;
    ++C.pics[s];
    C.scenes[s] += scenes.size();
    if (scenes.size() > C.maxScenes[s]) C.maxScenes[s] = static_cast<uint32_t>(scenes.size());
    uint32_t nk[5] = {}, nl = 0, ns = 0, nls = 0, nodd = 0;
    for (const SceneRec& r : scenes) {
        ++nk[r.kind < 5 ? r.kind : 4];
        ++C.kinds[s][r.kind < 5 ? r.kind : 4];
        if (r.noLoad) ++nl;
        if (r.noStore) ++ns;
        if (!r.noLoad && !r.noStore) { ++nls; if (r.depthDraws == 0 && r.kind != kDisp) ++nodd; }
        C.dsKib[s] += r.dsKib;
        C.dsSkipKib[s] += (r.noLoad ? r.dsKib / 2 : 0) + (r.noStore ? r.dsKib / 2 : 0);
        if (!r.draws && !r.copies) ++C.emptyS[s];
        if (r.touches == 1 && (r.op == 1 || r.op == 2 || r.op == 3)) ++C.clearOnly[s];
        if (r.draws && !r.depthDraws) ++C.noDepthDraw[s];
    }
    C.noLoad[s] += nl; C.noStore[s] += ns; C.loadStore[s] += nls; C.loadNoDepthRead[s] += nodd;
    if (logEvery != 0 && (s >= 2 || logEvery >= 30) && (logEvery < 0 || C.pics[s] % static_cast<uint32_t>(logEvery) == 1u || logEvery == 1)) {
        std::fprintf(stderr, "[scenes] f%llu %s: %u scenes (efb %u, pass %u, copy %u, disp %u, other %u), depth hints noload %u nostore %u, loads+stores %u (of which no depth-tested draw %u)\n",
                     static_cast<unsigned long long>(frame), SceneName(s), static_cast<unsigned>(scenes.size()), nk[0], nk[1], nk[2], nk[3], nk[4], nl, ns, nls, nodd);
        if (logEvery < 0) {
            uint32_t i = 0;
            for (const SceneRec& r : scenes)
                std::fprintf(stderr, "[scenes]   %2u %-5s %dx%d x%u cmd %u(%u) op %u why %s draws %u depth %u copies %u touches %u %s%s\n", i++, KindName(r.kind), r.w, r.h, r.samples, r.cmdIndex, r.cmdType, r.op, r.why[0] ? r.why : "-",
                             r.draws, r.depthDraws, r.copies, r.touches, r.noLoad ? "NOLOAD " : "", r.noStore ? "NOSTORE" : "");
        }
    }
    scenes.clear();
}
inline void Summary(const Census& C, uint64_t frame) {
    for (int s = 2; s <= 3; ++s) {
        if (!C.pics[s]) continue;
        const double n = static_cast<double>(C.pics[s]);
        std::fprintf(stderr, "[scenes-total] f%llu %s: %u pictures, %.2f scenes a picture (max %u): efb %.2f pass %.2f copy %.2f disp %.2f, noload %.2f nostore %.2f load+store %.2f (no depth test in %.2f), empty %.2f (clear only %.2f), depth %.0f KiB a picture of which %.0f skipped\n",
                     static_cast<unsigned long long>(frame), SceneName(s), C.pics[s], C.scenes[s] / n, C.maxScenes[s], C.kinds[s][0] / n, C.kinds[s][1] / n, C.kinds[s][2] / n, C.kinds[s][3] / n,
                     C.noLoad[s] / n, C.noStore[s] / n, C.loadStore[s] / n, C.loadNoDepthRead[s] / n, C.emptyS[s] / n, C.clearOnly[s] / n, C.dsKib[s] / n, C.dsSkipKib[s] / n);
    }
}

} // namespace heldscenes
} // namespace vgx
