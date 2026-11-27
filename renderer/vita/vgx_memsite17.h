// memsite17 (wave 17): who holds the video memory, by allocation site, at the moments that matter. Vita only; with MSC_VGL_MEMSITES unset or 0 (the default) every function here
// returns after one test of a value read once, and vitaGL (patch 0041, utils/mem_utils.c and gpu_utils.c) keeps no table and takes one predictable branch in its allocator wrappers.
// With MSC_VGL_MEMSITES=1 vitaGL keeps every pool block by address (size, the call site that asked, the kind of routine) and the "[memsite]" lines are written at
//   - kick-off of a match (the first two, before the hold's commit lets targets go),
//   - the first held cut-scene and the first replay of the run,
//   - the first refused allocation and the first time every pool refused (vitaGL, utils/mem_utils.c),
//   - the end of a scripted run (vita_memreport.cpp LogPoolTables),
// at most Gate::kMax from here. NpVitaMemSites (vita_memreport.cpp) prints vitaGL's lines and a check of each pool's site sum against the allocator counters of the [mem] lines.
// Both symbols are weak: a build without them (or the host) does nothing.
#pragma once

#include <cstdlib>

#if defined(__vita__)
extern "C" void NpVitaMemSites(const char* tag) __attribute__((weak));
#endif

namespace vgx_memsite17 {

// Which events dump (pure logic, no I/O).
struct Gate {
    static constexpr int kMax = 6;       // dumps from the events below (vitaGL's two refusal dumps and the end-of-run one come on top)
    static constexpr int kKicks = 2;     // kick-offs dumped (the first two matches of a run)
    int dumps = 0, kicks = 0;
    bool cut = false, replay = false;
    // event: 0 kick-off, 1 held cut-scene (lock60 kCut), 2 replay (lock60 kReplay); returns the tag to dump under, or nullptr.
    const char* Take(int event) {
        if (dumps >= kMax) return nullptr;
        const char* tag = nullptr;
        if (event == 0 && kicks < kKicks) { ++kicks; tag = kicks == 1 ? "kick-off" : "kick-off (second match)"; }
        else if (event == 1 && !cut) { cut = true; tag = "first held cut-scene"; }
        else if (event == 2 && !replay) { replay = true; tag = "first replay"; }
        if (tag) ++dumps;
        return tag;
    }
};

inline bool On() {
    static const bool on = [] { const char* e = std::getenv("MSC_VGL_MEMSITES"); return e && *e && std::atoi(e) != 0; }();
    return on;
}

inline void Event(int event) {
    if (!On()) return;
    static Gate gate;
    const char* tag = gate.Take(event);
#if defined(__vita__)
    if (tag && NpVitaMemSites) NpVitaMemSites(tag);
#else
    (void)tag;
#endif
}

}  // namespace vgx_memsite17
