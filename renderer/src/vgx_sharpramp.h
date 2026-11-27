// MSC_SHARPEN_EASE: the display pass's sharpening strength follows the picture size, and a size step used to switch it in one picture. The ramp moves it from the strength in force to the new
// one over `frames` pictures (a smoothstep), so a step of the dynamic resolution changes the softness of the picture gradually and is not seen as a change of sharpness. Header-only and pure
// (no GL, no clock) so a host test (renderer/tests/sharp_ramp.cpp) runs the shipped arithmetic. The strength is a uniform of the picture program: no shader text.
#pragma once
#include <cmath>

namespace vgx::sharpramp {

struct State {
    float cur = 0.0f, from = 0.0f, to = 0.0f;
    int n = 0, frames = 0;
    bool have = false;
};
constexpr float kSame = 0.004f;   // a new target this close to the last one is the same target (the amount is a function of the size: it moves by whole steps, never in small drifts)

inline int FramesFromSetting(long v) { return v < 0 ? 0 : (v > 120 ? 120 : static_cast<int>(v)); }
inline bool Active(const State& s) { return s.have && s.n < s.frames; }
// the strength to draw this picture with; call once per picture with the strength the size asks for
inline float Step(State& s, float target, int frames) {
    s.frames = frames;
    if (!s.have || frames <= 0) {
        s.have = true;
        s.cur = s.from = s.to = target;
        s.n = frames;
        return target;
    }
    if (std::fabs(target - s.to) > kSame) {
        s.from = s.cur;
        s.to = target;
        s.n = 0;
    }
    if (s.n < frames) {
        ++s.n;
        const float t = static_cast<float>(s.n) / static_cast<float>(frames);
        s.cur = s.from + (s.to - s.from) * (t * t * (3.0f - 2.0f * t));
    } else {
        s.cur = s.to;
    }
    return s.cur;
}
// a picture the size of the screen is copied as it is (no picture program) when nothing is easing and the strength is nothing
inline bool StillSharpening(const State& s) { return s.have && (Active(s) || std::fabs(s.cur) > kSame); }

} // namespace vgx::sharpramp
