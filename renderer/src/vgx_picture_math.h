// Picture arithmetic of the 1.3 no-cost picture work (header-only so a test can run it on its own; no GL, no state).
#pragma once
#include <cmath>
#include <cstdlib>

namespace vgx {

// MSC_VGX_LODBIAS: the LOD bias of mipmapped textures, in sceGxm's raw units of 1/8 of a mip level (the field is 6 bits, 31 is 0; Vita3K reads it as (raw - 31) / 8).
// Only sharper is offered: 0 to -8, one level at most. Anything else, an empty value or a positive number, is 0.
inline int ParseLodBias(const char* e) {
    if (!e || !*e) return 0;
    char* end = nullptr;
    const long v = std::strtol(e, &end, 10);
    if (end == e || *end != 0 || v > 0) return 0;
    return v < -8 ? -8 : static_cast<int>(v);
}

// The bias a bind asks for: `base` on every mipmapped texture that is not a render target, plus the dynamic resolution's background step on background draws.
inline int TextureBiasRaw(bool mipSampled, bool renderTarget, int base, int fgRaw, bool background) {
    if (!mipSampled || renderTarget) return 0;
    const int sum = base + (background ? fgRaw : 0);
    return sum < -31 ? -31 : (sum > 32 ? 32 : sum);   // (the field is 6 bits: vitaGL adds 31 to the parameter, so -32 would wrap into the neighbouring fields of the texture's control word)
}

// MSC_HDTEX_FADE: an HD replacement texture (2x, vgx_hdtex.cpp) arrives a few pictures after its object is first seen; until then the object is drawn with the original. Drawn at once at its
// own sharpness the object pops; the replacement is therefore sampled with a mip bias that starts at `startRaw` (8 raw = one mip level = the original's detail on a 2x texture) and falls to
// 0 over `frames` pictures, in whole raw steps. `age`: pictures since the replacement was uploaded. 0 when over, or when either setting is 0.
inline int HdFadeRaw(long age, int startRaw, int frames) {
    if (startRaw <= 0 || frames <= 0 || age < 0 || age >= frames) return 0;
    const int v = static_cast<int>((static_cast<long>(startRaw) * (frames - age) + frames - 1) / frames);   // (rounded up: the first picture shows the full bias)
    return v > 32 ? 32 : v;
}

// MSC_VITA_SHAPE=1: the same pixels as efbW x efbH at scale s, in the display's shape. 640x448 on 960x544 is scaled 1.50 across and 1.21 down: the picture spends its pixels where
// the screen needs them least. The result is whole multiples of 8 (what the ladder's sizes are) and never has more pixels than the plain size, so the fill cannot grow; both
// sides stay on the screen.
inline void ShapedSize(int efbW, int efbH, float s, int dispW, int dispH, int* w, int* h) {
    const float area = static_cast<float>(efbW) * static_cast<float>(efbH) * s * s;
    const float aspect = static_cast<float>(dispW) / static_cast<float>(dispH);
    int ww = (static_cast<int>(std::sqrt(area * aspect) + 4.0f)) & ~7;
    if (ww > dispW) ww = dispW;
    if (ww < 16) ww = 16;
    int hh = static_cast<int>(area / static_cast<float>(ww)) & ~7;   // (down: never more pixels)
    if (hh > dispH) hh = dispH;
    if (hh < 16) hh = 16;
    *w = ww;
    *h = hh;
}

// 1.3 MSC_VGX_LODBIAS_RUNG=<percent> (render-size aware texture detail): the mip bias, in raw 1/8 levels (the GL parameter vitaGL takes), that gives a picture drawn rtW x rtH and
// stretched to picW x picH the texture detail of a picture drawn at picW x picH: percent/100 x log2 of the geometric mean of the two axis ratios (a surface's mip level follows its
// longer footprint, which is across the screen on walls and down it on the pitch, so the mean of the two is the middle of what a native picture would pick). 0 at or above the
// native size; never below -31 (the field is 6 bits: raw 0 is -31, GL_MAX_TEXTURE_LOD_BIAS).
inline int RungLodBiasRaw(int rtW, int rtH, int picW, int picH, int percent) {
    if (percent <= 0 || rtW <= 0 || rtH <= 0 || picW <= 0 || picH <= 0) return 0;
    const float s = std::sqrt(static_cast<float>(rtW) / static_cast<float>(picW) * static_cast<float>(rtH) / static_cast<float>(picH));
    if (s >= 0.999f) return 0;
    const long raw = std::lround(std::log2(s) * 8.0f * static_cast<float>(percent) / 100.0f);
    return raw < -31 ? -31 : static_cast<int>(raw);
}

// 1.3 MSC_VITA_SHARPEN_SCALE: how much the picture is stretched on the screen, the geometric mean of the two axes (1: the picture is the screen's size).
inline float PictureUpscale(int w, int h, int dw, int dh) {
    if (w <= 0 || h <= 0) return 1.0f;
    return std::sqrt(static_cast<float>(dw) / static_cast<float>(w) * static_cast<float>(dh) / static_cast<float>(h));
}
// The present's sharpening as a function of the stretch f: nothing below kPlainCopyUpscale (the picture is copied as it is), then k x sqrt(f - 1), at most `cap`. The root is the fit: on
// real frames (host picture sheets, see docs) the amount of the combined picture that is closest to a native picture is about 0.3 at a stretch of 1.13, 0.45 at 1.35 and 0.65 at 2.
// A picture drawn larger than the screen (f < 0.97, the supersampled rungs) is not covered: the host keeps its own rule there.
constexpr float kPlainCopyUpscale = 1.03f;
inline bool PictureIsPlainCopy(float f) { return f < kPlainCopyUpscale && f > 0.97f; }
inline float SharpenForUpscale(float f, float k, float cap) {
    if (f < kPlainCopyUpscale) return 0.0f;
    const float a = k * std::sqrt(f - 1.0f);
    return a > cap ? cap : a;
}

} // namespace vgx
