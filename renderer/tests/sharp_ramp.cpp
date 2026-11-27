// MSC_SHARPEN_EASE: the ramp of the display pass's sharpening (vgx_sharpramp.h).
#include "../src/vgx_sharpramp.h"
#include <cstdio>
#include <cstdlib>
using namespace vgx::sharpramp;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
int main() {
    State s;
    // first picture: no ramp
    CHECK(Step(s, 0.5f, 20) == 0.5f);
    CHECK(!Active(s));
    for (int i = 0; i < 50; ++i) CHECK(Step(s, 0.5f, 20) == 0.5f);
    // a step from 0.5 to 0.8: 20 pictures, monotone, ends exactly on the target, never overshoots, the first picture moves by little
    float prev = 0.5f, firstStep = 0, maxStep = 0;
    for (int i = 1; i <= 20; ++i) {
        const float v = Step(s, 0.8f, 20);
        CHECK(v >= prev && v <= 0.8f);
        if (i == 1) firstStep = v - prev;
        if (v - prev > maxStep) maxStep = v - prev;
        prev = v;
    }
    CHECK(prev == 0.8f);
    CHECK(!Active(s));
    CHECK(firstStep < 0.3f / 20.0f);        // slower than a linear ramp at the start
    CHECK(maxStep < 0.3f * 0.1f);           // no picture takes a tenth of the step (a switch would take all of it)
    CHECK(Step(s, 0.8f, 20) == 0.8f);
    // a step interrupted half way turns from where it is, with no jump
    for (int i = 0; i < 10; ++i) Step(s, 0.4f, 20);
    const float mid = s.cur;
    CHECK(mid < 0.8f && mid > 0.4f);
    const float next = Step(s, 0.6f, 20);   // a new target
    CHECK(std::fabs(next - mid) < 0.05f);
    int n = 0;
    while (Active(s) && n < 100) { Step(s, 0.6f, 20); ++n; }
    CHECK(n == 19 && s.cur == 0.6f);
    // a target that moves by less than kSame is the same target
    const float c0 = s.cur;
    Step(s, 0.6f + kSame * 0.5f, 20);
    CHECK(s.cur == c0 && !Active(s));
    // frames 0: off, every step is a switch
    State o;
    Step(o, 0.2f, 0);
    CHECK(Step(o, 0.7f, 0) == 0.7f && !Active(o));
    // the plain copy: easing down to nothing keeps the picture program until it is nothing
    State p;
    Step(p, 0.4f, 20);
    Step(p, 0.0f, 20);
    CHECK(StillSharpening(p));
    int k = 0;
    while (StillSharpening(p) && k < 100) { Step(p, 0.0f, 20); ++k; }
    CHECK(k <= 20 && !StillSharpening(p) && p.cur == 0.0f);
    State q;
    Step(q, 0.0f, 20);
    CHECK(!StillSharpening(q));   // at the screen's size from the start: the plain copy, as before
    // frames limits
    CHECK(FramesFromSetting(-3) == 0 && FramesFromSetting(20) == 20 && FramesFromSetting(9999) == 120);
    std::printf(fails ? "FAILED %d\n" : "sharp_ramp: all passed\n", fails);
    return fails ? 1 : 0;
}
