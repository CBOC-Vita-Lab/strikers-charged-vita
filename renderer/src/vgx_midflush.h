// MSC_VGX_MIDFLUSH (wave 14, from Galaxy's midflush31): the scheduling rule of sceGxmMidSceneFlush for the EFB scene. Pure, standard library only (tests/midflush.cpp).
//
// sceGxm starts a scene's vertex phase (all vertex shading and binning) only when the scene ends or a ring buffer fills. The back end records the EFB scene for many ms
// of CPU, so that phase then runs in series with the scene's pixel phase. A mid-scene flush hands the GPU the vertex work recorded so far: it runs while the CPU still records,
// and only the rest is left after sceGxmEndScene. Nothing is drawn differently (the pixel phase still begins at the scene's end); what a flush costs is the call and the
// state sceGxm forgets, which the next draw gives again.
//   MSC_VGX_MIDFLUSH=<mode * 10000 + n>
//     n (1..4095)     mode 0: after every n-th draw of an EFB scene, wherever that is
//     10000 + n       mode 1: once n draws were recorded since the scene's start (or the last flush) the flush waits for the first draw whose program differs from the one
//                     bound (that draw gives its program again anyway), or goes in front of the next draw if none comes in max(8, n / 2) draws
//     30000 + k       mode 3: k flushes (1..8) in the scene, in chunks of (the draws of the previous picture's largest EFB scene) / (k + 1), at least 24 draws, mode 1's boundary rule
//     (mode 2 of Galaxy, chunks by indices, is not ported)
#pragma once
#include <algorithm>
#include <cstdint>

namespace vgx {
namespace midflush {

struct Cfg {
    uint8_t mode = 0;     // 0, 1, 3; 255 = off
    uint32_t n = 0;       // 0 = off
    bool On() const { return n != 0 && mode != 255; }
};
inline Cfg Parse(long v) {
    Cfg c;
    if (v <= 0) return c;
    v = std::min<long>(v, 39999);
    const long m = v / 10000;
    long n = v % 10000;
    if (m == 2) return c;   // (Galaxy's index mode is not ported: off)
    c.mode = static_cast<uint8_t>(m);
    if (m == 0) n = std::min<long>(n, 4095);
    else if (m == 3) n = n ? std::min<long>(n, 8) : 2;
    else if (n == 0) n = 64;
    c.n = static_cast<uint32_t>(n);
    return c;
}

struct Sched {
    Cfg cfg;
    uint32_t draws = 0;          // EFB draws since the scene's start or the last flush
    uint32_t sceneDraws = 0, sceneFlushes = 0, picBig = 0, bigPrev = 0, chunk = 0, late = 0;
    bool pending = false;
    // A scene begins (nowEfb: it is the EFB's) after `wasEfb` ended.
    void SceneBoundary(bool wasEfb, bool nowEfb) {
        if (wasEfb && sceneDraws > picBig) picBig = sceneDraws;
        draws = sceneDraws = sceneFlushes = late = 0;
        pending = false;
        chunk = (cfg.mode == 3 && nowEfb && bigPrev) ? std::max<uint32_t>(24u, bigPrev / (cfg.n + 1)) : 0;
    }
    void PictureEnd() {
        if (sceneDraws > picBig) picBig = sceneDraws;
        bigPrev = picBig;
        picBig = 0;
    }
    // After an EFB draw. Returns true when a flush is due NOW (mode 0); modes 1 and 3 set `pending` for the next boundary.
    bool AfterDraw() {
        ++draws;
        ++sceneDraws;
        if (pending) return false;
        switch (cfg.mode) {
        case 0: return draws >= cfg.n;
        case 1: if (draws >= cfg.n) pending = true; return false;
        default: if (chunk && sceneFlushes < cfg.n && draws >= chunk) pending = true; return false;
        }
    }
    // In front of an EFB draw (its program known). 0 no flush; 1 flush at a boundary (the program differs); 2 forced (no boundary came in time).
    int BeforeDraw(bool programDiffers) {
        if (!pending) return 0;
        [[maybe_unused]] const uint32_t cap = cfg.mode == 3 ? std::max<uint32_t>(8u, chunk / 2) : std::max<uint32_t>(8u, cfg.n / 2);
        if (programDiffers) return 1;
#ifdef VGX_MIDFLUSH_BREAK   // (test mutant: a flush never goes in when no boundary comes)
        return 0;
#else
        return ++late > cap ? 2 : 0;
#endif
    }
    void Flushed() { draws = 0; late = 0; pending = false; ++sceneFlushes; }
};

}  // namespace midflush
}  // namespace vgx
