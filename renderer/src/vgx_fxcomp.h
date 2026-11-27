// MSC_FX_COMPOSITE: the rules of the effects compositor, header-only so a host test can run them on their own (renderer/tests/fx_comp.cpp).
#pragma once
#include <cstdint>

namespace vgx {
namespace fxcomp {

// Size rule. A draw (one emitter's packet) goes to the shared low-resolution accumulation target when its sprites are large: the mean fill of one triangle is at
// least `minTriPx` pixels of the 640 x 448 picture (a quad is two triangles, so 900 is a sprite of about 42 x 42 pixels, 0.6 % of the picture). The small crisp
// ones (sparks, rain streaks, dust, the ball's trail) stay at full size, where half or quarter resolution would show. A draw nobody can measure (px < 0, or no
// triangles) is large only when the caller says so (`unknownBig`: a marked additive mesh of a Mega Strike always was).
inline bool Big(double px, uint32_t tris, double minTriPx, bool unknownBig) {
    if (px < 0.0 || tris == 0) return unknownBig;
    return px >= minTriPx * static_cast<double>(tris);
}

// Divisor (per side) of the accumulation target for a scene whose eligible draws fill `fill` screens: the smallest of half and quarter whose fill
// (fill / div^2 screens, which is what the GPU rasterises) is at most `budget` screens (about 2 full-screen equivalents). `step` is the governor's: each step
// doubles the divisor again (the effects governor lowers the accumulation size before the picture), at most `maxDiv`. fill <= 0 (not measurable) gives half.
inline int Div(double fill, int step, double budget = 2.0, int maxDiv = 8) {
    int d = (fill > 4.0 * budget) ? 4 : 2;
    for (int s = 0; s < step && d < maxDiv; ++s) d *= 2;
    return d;
}

// What the accumulation costs against drawing the same sprites in the scene, in screens of the picture: the sprites at div^2, plus the depth copy
// (a quad at the target's size) and the composite (a quad of the full picture's size). The pass pays when this is below `fill`. `share` (MSC_FX_COMPOSITE_BOX, 1.0 when it
// is not used or the draws cannot be placed): the part of the picture that the depth copy and the composite cover, the rectangle the run's sprites land in.
inline double Cost(double fill, int div, double share = 1.0) {
    const double d2 = static_cast<double>(div) * static_cast<double>(div);
    return fill / d2 + share / d2 + share;
}

// The divisor the game is told (VgxFxPassDiv: the tight polygons of MSC_FX_TIGHT come in levels up to 4).
inline int GameDiv(int div) { return div > 4 ? 4 : div; }

} // namespace fxcomp
} // namespace vgx
