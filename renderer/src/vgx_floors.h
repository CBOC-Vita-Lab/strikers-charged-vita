// The smallest picture the renderer may draw (owner, 17 Nov 2026: "448x320 is unacceptable"). One named constant per mode; every ladder (the Quality hold ladder, the held-scene ladder, the
// start tables, the Smooth 60 presets, the dynamic-resolution levels) is cut at it, and renderer/tests/floors.cpp fails when a rung below it is reachable.
//   Quality (MSC_PICTURE_HOLD): never below 864x488 (0.90 of 960x544) in live play, replays, cut-scenes, Mega Strikes and menus.
//   Smooth 60 (MSC_LOCK60):     never below 800x456 (0.8333 of 960x544), the very lowest; when 60 does not hold there the frame-rate controller goes to a steady 30 and the picture goes back up.
// Pure, no platform includes. Render only: the size of the picture reaches nothing the simulation or the network reads.
#pragma once
#include <algorithm>
#include <cmath>

namespace vgx {
namespace floors {

constexpr int kScreenW = 960, kScreenH = 544;
// (the mutants of renderer/tests/floors.cpp lower these with -DVGX_FLOOR_QUALITY_W=... and the test must then fail)
#ifndef VGX_FLOOR_QUALITY_W
#define VGX_FLOOR_QUALITY_W 864
#define VGX_FLOOR_QUALITY_H 488
#endif
#ifndef VGX_FLOOR_SMOOTH_W
#define VGX_FLOOR_SMOOTH_W 800
#define VGX_FLOOR_SMOOTH_H 456
#endif
constexpr int kQualityMinW = VGX_FLOOR_QUALITY_W, kQualityMinH = VGX_FLOOR_QUALITY_H;   // MSC_PICTURE_HOLD
constexpr int kSmoothMinW = VGX_FLOOR_SMOOTH_W, kSmoothMinH = VGX_FLOOR_SMOOTH_H;       // MSC_LOCK60

enum Mode { kQuality = 0, kSmooth = 1 };
inline int MinW(int mode) { return mode == kSmooth ? kSmoothMinW : kQualityMinW; }
inline int MinH(int mode) { return mode == kSmooth ? kSmoothMinH : kQualityMinH; }
// A size is allowed when it has at least the floor's PIXELS: the ladders of the Smooth 60 mode draw in the game's own 4:3-ish shape (640x448 times a scale) and stretch it, so 728x512 holds
// the pixels of 800x456; the Quality ladder is in the screen's shape, where the pixel test and the sides agree (864x488, 912x520).
inline long MinPixels(int mode) { return static_cast<long>(MinW(mode)) * MinH(mode); }
inline bool SizeAllowed(int mode, int w, int h) { return static_cast<long>(w) * h >= MinPixels(mode); }
// How many of the leading entries of a ladder (sizes w[0], h[0] ... in the order they are taken, each smaller than the one before) are at or above the floor.
inline int LeadingAllowed(int mode, const int* w, const int* h, int n) {
    int k = 0;
    while (k < n && SizeAllowed(mode, w[k], h[k])) ++k;
    return k;
}
// The size of a dynamic-resolution level (the renderer's DynLevelSize without the display shape): the game's own size times s per side, up to a multiple of 8, never past the screen.
inline void LevelSize(int ownW, int ownH, float s, int* w, int* h) {
    if (s > 0.999f && s < 1.001f) { *w = ownW; *h = ownH; return; }
    int ww = (static_cast<int>(static_cast<float>(ownW) * s + 0.5f) + 7) & ~7, hh = (static_cast<int>(static_cast<float>(ownH) * s + 0.5f) + 7) & ~7;
    if (ww > kScreenW) ww = kScreenW;
    if (hh > kScreenH) hh = kScreenH;
    if (s < 1.0f) { ww = std::min(ww, ownW); hh = std::min(hh, ownH); }
    *w = std::max(ww, 16);
    *h = std::max(hh, 16);
}
// The per-side scale of the game's own size (ownW x ownH) whose picture has at least the mode's pixels (rounded up to the table's two decimals).
inline float ScaleFloor(int mode, int ownW, int ownH) {
    const float s = std::sqrt(static_cast<float>(MinPixels(mode)) / (static_cast<float>(ownW) * static_cast<float>(ownH)));
    return std::ceil(s * 100.0f - 0.001f) / 100.0f;
}
// The Smooth 60 level table (MSC_LOCK60 with the floors): scale, and samples a pixel.
// Rung 0 is the steady 30's FULL picture (960x544 2x when multisampling is on); rung 1 is full native 960x544 1x (zero upscale blur when GPU fits); rungs 2-3 are 768x544 and 728x512.
struct SmoothRung { float s; int ms; };
constexpr int kSmoothRungs = 4;
inline void SmoothTable(int msaaTop, SmoothRung out[kSmoothRungs]) {
    out[0] = {1.50f, msaaTop >= 2 ? 2 : 1};
    out[1] = {1.50f, 1};
    out[2] = {1.20f, 1};
    out[3] = {1.13f, 1};
}
// The Quality level table of the dynamic ladder when the hold ladder is not in force (MSC_FPS=30, DYNAMIC RESOLUTION off, MSC_VITA_MSAA named, MSC_PICTURE_HOLD=0, LOCKED 30, PERFORMANCE; 18 Nov, F1 of
// fable-review25): the game's own size was the old table's start (640x448, 286,720 pixels) and its floor went down to 0.86 / 0.70 / 0.50 (552x384 ... 320x224). Four rungs, best first, in the game's own
// shape: 1.50 (960x544, the full size, 2x edge smoothing when the build has multisampling), 1.38, 1.30 and the floor's own scale (ScaleFloor, 1.22 = 784x544, 426,496 pixels against 421,632). Every named floor
// (ordinary scenes, 60, paced, SEVERE, heavy stadium, Mega Strike) is an index into this table, so none can reach a size below the floor. A scene that does not fit at the bottom rung has no smaller size to
// go to: a 30 stays a 30 (the frame-rate controller already falls back from 60 to 30; there is no rate below 30).
constexpr int kQualityRungs = 4;
inline void QualityTable(int msaaTop, SmoothRung out[kQualityRungs]) {
#ifdef VGX_FLOORS_TABLE_BREAK   // (mutant of renderer/tests/floors.cpp: the pre-floor table, the clamp removed)
    out[0] = {1.00f, msaaTop >= 2 ? 2 : 1};
    out[1] = {0.93f, 1};
    out[2] = {0.86f, 1};
    out[3] = {0.50f, 1};
#else
    out[0] = {1.50f, msaaTop >= 2 ? 2 : 1};
    out[1] = {1.38f, 1};
    out[2] = {1.30f, 1};
    out[3] = {ScaleFloor(kQuality, 640, 448), 1};
#endif
}
// Which table a configuration gets (the renderer's DynBuildLevels): a fixed size or a held picture has no table; with the floors on, LOCKED 60 gets the Smooth table, every other ladder the Quality one;
// only MSC_PICTURE_FLOORS=0 (a developer's switch) keeps the old table.
enum Ladder { kLadderNone = 0, kLadderLegacy = 1, kLadderSmooth = 2, kLadderQuality = 3 };
inline int LadderFor(bool floorsOn, bool userFixed, bool lock60) {
    if (userFixed) return kLadderNone;
    if (!floorsOn) return kLadderLegacy;
    return lock60 ? kLadderSmooth : kLadderQuality;
}
// A fixed picture size a player's file names (the Settings screen, np-player.txt): a size under the Quality floor goes up to the floor's own size, 864x488. (The Vita plan applies it; a size set by hand in a host run is not clamped.)
inline void ClampFixedSize(int w, int h, int* ow, int* oh) {
    if (SizeAllowed(kQuality, w, h)) { *ow = w; *oh = h; return; }
    *ow = kQualityMinW; *oh = kQualityMinH;
}
// How many of the leading levels of a table of per-side scales are allowed (the table is ordered best first).
inline int LevelsAllowed(int mode, int ownW, int ownH, const float* scale, int n) {
    int k = 0;
    for (; k < n; ++k) { int w, h; LevelSize(ownW, ownH, scale[k], &w, &h); if (!SizeAllowed(mode, w, h)) break; }
    return k;
}

}  // namespace floors
}  // namespace vgx
