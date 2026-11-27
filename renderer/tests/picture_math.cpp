// The no-cost picture switches: MSC_VGX_LODBIAS and MSC_VITA_SHAPE (vgx_picture_math.h).
#include "../src/vgx_picture_math.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

int main() {
    using namespace vgx;
    // Only sharper, one level at most; anything that is not a number is the default.
    CHECK(ParseLodBias(nullptr) == 0);
    CHECK(ParseLodBias("") == 0);
    CHECK(ParseLodBias("0") == 0);
    CHECK(ParseLodBias("-2") == -2);
    CHECK(ParseLodBias("-8") == -8);
    CHECK(ParseLodBias("-40") == -8);
    CHECK(ParseLodBias("3") == 0);
    CHECK(ParseLodBias("x") == 0);
    CHECK(ParseLodBias("-2x") == 0);
    // Never on a texture without a mip chain in use or on a render target; the background step adds to it on background draws only.
    CHECK(TextureBiasRaw(true, false, -2, 0, false) == -2);
    CHECK(TextureBiasRaw(false, false, -2, 4, true) == 0);
    CHECK(TextureBiasRaw(true, true, -2, 4, true) == 0);
    CHECK(TextureBiasRaw(true, false, -2, 4, true) == 2);
    CHECK(TextureBiasRaw(true, false, -2, 4, false) == -2);
    CHECK(TextureBiasRaw(true, false, 0, 0, false) == 0);
    CHECK(TextureBiasRaw(true, false, -39, 0, false) == -31);   // (a fixed bias and the render-size bias together stay in the field)
    CHECK(TextureBiasRaw(true, false, 30, 20, true) == 32);
    // MSC_HDTEX_FADE: a replacement that has just come in starts at the full bias, falls in whole steps, never rises, is 0 at the end and with either setting 0.
    CHECK(HdFadeRaw(0, 8, 8) == 8);
    CHECK(HdFadeRaw(7, 8, 8) == 1);
    CHECK(HdFadeRaw(8, 8, 8) == 0);
    CHECK(HdFadeRaw(100, 8, 8) == 0);
    CHECK(HdFadeRaw(0, 0, 8) == 0 && HdFadeRaw(0, 8, 0) == 0 && HdFadeRaw(-1, 8, 8) == 0);
    for (int frames = 1; frames <= 20; ++frames) {
        int prev = 99;
        for (long a = 0; a < frames + 3; ++a) {
            const int v = HdFadeRaw(a, 8, frames);
            CHECK(v <= prev && v >= 0 && v <= 8);
            if (a < frames) CHECK(v >= 1);
            prev = v;
        }
        CHECK(HdFadeRaw(frames, 8, frames) == 0);
    }
    CHECK(HdFadeRaw(0, 99, 8) == 32);
    // The picture's shape: at every ladder scale no more pixels than the plain size, multiples of 8, inside the screen, and closer to the same scale on both axes.
    const float scales[] = {1.20f, 1.12f, 1.06f, 1.00f, 0.93f, 0.86f, 0.78f, 0.70f, 0.62f, 0.56f, 0.50f, 0.25f};
    for (float s : scales) {
        int w = 0, h = 0;
        ShapedSize(640, 448, s, 960, 544, &w, &h);
        const float plain = 640.0f * 448.0f * s * s;
        CHECK(static_cast<float>(w) * static_cast<float>(h) <= plain * 1.0001f);
        CHECK(static_cast<float>(w) * static_cast<float>(h) >= plain * 0.90f);
        CHECK(w % 8 == 0 && h % 8 == 0);
        CHECK(w <= 960 && h <= 544 && w >= 16 && h >= 16);
        const float ax = 960.0f / static_cast<float>(w), ay = 544.0f / static_cast<float>(h);
        const float r = ax > ay ? ax / ay : ay / ax;
        CHECK(r < 1.15f);
        CHECK(r < (1.5f / 1.2143f));
    }
    int w = 0, h = 0;
    ShapedSize(640, 448, 1.0f, 960, 544, &w, &h);
    CHECK(w == 712 && h == 400);
    ShapedSize(640, 448, 2.0f, 960, 544, &w, &h);   // (the screen caps it)
    CHECK(w == 960 && h == 544);
    // MSC_VGX_LODBIAS_RUNG: nothing at or above the screen's size or when off; the geometric mean of the axes; 100 % is the whole difference; never past the 6-bit field.
    CHECK(RungLodBiasRaw(960, 544, 960, 544, 100) == 0);
    CHECK(RungLodBiasRaw(960, 544, 960, 544, 0) == 0);
    CHECK(RungLodBiasRaw(640, 448, 960, 544, 0) == 0);
    CHECK(RungLodBiasRaw(768, 538, 960, 544, 100) == -1);   // log2(0.89) = -0.17 level = -1.4/8
    CHECK(RungLodBiasRaw(848, 480, 960, 544, 100) == -1);   // 0.883: -0.18 level -> -1.4 -> -1
    CHECK(RungLodBiasRaw(480, 272, 960, 544, 100) == -8);   // half: one level
    CHECK(RungLodBiasRaw(480, 272, 960, 544, 60) == -5);    // 60 %: -4.8
    CHECK(RungLodBiasRaw(16, 16, 960, 544, 150) == -31);    // the field's end
    CHECK(RungLodBiasRaw(726, 544, 726, 544, 100) == 0);    // the 4:3 box at its own size
    CHECK(RungLodBiasRaw(640, 448, 726, 544, 100) < 0 && RungLodBiasRaw(640, 448, 726, 544, 100) > RungLodBiasRaw(640, 448, 960, 544, 100));
    // MSC_VITA_SHARPEN_SCALE
    CHECK(PictureIsPlainCopy(PictureUpscale(960, 544, 960, 544)));
    CHECK(PictureIsPlainCopy(PictureUpscale(944, 536, 960, 544)));
    CHECK(!PictureIsPlainCopy(PictureUpscale(848, 480, 960, 544)));
    CHECK(!PictureIsPlainCopy(PictureUpscale(1000, 600, 960, 544)));   // (drawn larger than the screen: the host's own rule)
    CHECK(SharpenForUpscale(1.0f, 0.8f, 0.8f) == 0.0f);
    CHECK(SharpenForUpscale(1.02f, 0.8f, 0.8f) == 0.0f);
    CHECK(SharpenForUpscale(1.13f, 0.8f, 0.8f) > 0.27f && SharpenForUpscale(1.13f, 0.8f, 0.8f) < 0.31f);
    CHECK(SharpenForUpscale(1.35f, 0.8f, 0.8f) > 0.45f && SharpenForUpscale(1.35f, 0.8f, 0.8f) < 0.50f);
    CHECK(SharpenForUpscale(3.0f, 0.8f, 0.8f) == 0.8f);
    float prev = -1.0f;
    for (float f = 1.0f; f < 3.0f; f += 0.05f) { const float a = SharpenForUpscale(f, 0.8f, 0.8f); CHECK(a >= prev); prev = a; }
    if (g_fail) return 1;
    std::printf("picture_math: all passed\n");
    return 0;
}
