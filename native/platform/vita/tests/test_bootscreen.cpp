// Pictures of the player build's loading screen (vita_bootscreen.cpp) on the host, for review without a console:
//   g++ -std=gnu++17 -O2 -Isrc tests/test_bootscreen.cpp src/vita_bootscreen.cpp src/vita_bootart.cpp -o /tmp/tb && /tmp/tb <loading.bin or -> <out dir>
// writes <out>/boot_<n>.ppm at several bar values and moments, and the time one full frame and one bar-region frame take.
#include "np_vita_internal.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace VitaConsole {
void ShowText() {}
bool OwnsDisplay() { return true; }
const uint32_t* Pixels() { return nullptr; }
uint32_t* PresentScreen() { return nullptr; }
} // namespace VitaConsole
namespace VitaLog {
void Note(const char*) noexcept {}
} // namespace VitaLog

static void WritePpm(const char* path, const std::vector<uint32_t>& fb) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", VitaConsole::kWidth, VitaConsole::kHeight);
    for (uint32_t p : fb) {
        const unsigned char rgb[3] = {static_cast<unsigned char>(p), static_cast<unsigned char>(p >> 8), static_cast<unsigned char>(p >> 16)};
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
}

int main(int argc, char** argv) {
    const char* art = argc > 1 && std::strcmp(argv[1], "-") != 0 ? argv[1] : nullptr;
    const char* out = argc > 2 ? argv[2] : ".";
    const bool haveArt = VitaBootScreen::TestLoad(art);
    std::printf("picture: %s\n", haveArt ? "the package's" : "plain");
    std::vector<uint32_t> fb(static_cast<size_t>(VitaConsole::kWidth) * VitaConsole::kHeight);
    struct Shot { float shown; double t; float bright; };
    const Shot shots[] = {{0.03f, 0.10, 1.0f}, {0.18f, 0.55, 1.0f}, {0.40f, 1.30, 1.0f}, {0.66f, 2.05, 1.0f}, {0.90f, 3.20, 1.0f}, {1.0f, 3.60, 1.0f}, {1.0f, 3.80, 0.45f}};
    int n = 0;
    for (const Shot& s : shots) {
        VitaBootScreen::TestRender(fb.data(), s.shown, s.t, s.bright);
        char path[512];
        std::snprintf(path, sizeof path, "%s/boot_%d.ppm", out, n++);
        WritePpm(path, fb);
    }
    // 1.3: the milestone line under the bar, and the slow-start line that takes its place: the pixels below the ball change only with the line, and stay in the animated region.
    {
        const auto band = [&fb](int y0, int y1) {
            unsigned long long sum = 0;
            for (int y = y0; y < y1; ++y)
                for (int x = 0; x < VitaConsole::kWidth; ++x) sum = sum * 31 + fb[y * VitaConsole::kWidth + x];
            return sum;
        };
        int bad = 0;
        VitaBootScreen::TestRender(fb.data(), 0.0f, 1.0, 1.0f);
        const unsigned long long plain = band(530, 543);
        VitaBootScreen::Stage("the game's start-up");
        VitaBootScreen::TestRender(fb.data(), 0.0f, 1.0, 1.0f);
        const unsigned long long loading = band(530, 543);
        VitaBootScreen::Stage("vitaGL start-up");
        VitaBootScreen::TestRender(fb.data(), 0.0f, 1.0, 1.0f);
        const unsigned long long graphics = band(530, 543);
        VitaBootScreen::TestRender(fb.data(), 0.0f, 25.0, 1.0f);
        const unsigned long long slow = band(530, 543);
        if (plain == loading || loading == graphics || slow == graphics) ++bad;
        VitaBootScreen::Stage("the game's start-up");
        VitaBootScreen::TestRender(fb.data(), 0.0f, 1.0, 1.0f);
        if (band(530, 543) != loading) ++bad;   // (no state left behind by the other lines)
        WritePpm((std::string(out) + "/boot_slow.ppm").c_str(), fb);
        std::printf("milestone line: %s\n", bad ? "FAILED" : "ok");
        if (bad) return 1;
    }
    // art25: the picture made from the game files fades in over the plain background: at the start the plain one, halfway a mix, at the end the picture; then it is the background.
    {
        constexpr int W = VitaConsole::kWidth, H = VitaConsole::kHeight;
        std::vector<unsigned char> image(12 + static_cast<size_t>(W) * H * 2);
        std::memcpy(image.data(), "NPBS", 4);
        for (size_t i = 0; i < static_cast<size_t>(W) * H; ++i) {   // a made-up gradient: no art in the test
            const unsigned v = (((i % W) * 31 / W) << 11) | (((i / W) * 63 / H) << 5) | 17u;
            image[12 + 2 * i] = static_cast<unsigned char>(v & 255);
            image[13 + 2 * i] = static_cast<unsigned char>(v >> 8);
        }
        std::vector<uint32_t> plainFb(fb.size()), mid(fb.size()), done(fb.size()), after(fb.size());
        VitaBootScreen::TestLoad(nullptr);   // the plain background
        VitaBootScreen::TestFade(image.data(), 0.0, plainFb.data(), false);
        VitaBootScreen::TestFade(image.data(), 0.25, mid.data(), false);
        VitaBootScreen::TestFade(image.data(), 0.5, done.data(), true);   // (the fade lasts 0.5 s)
        VitaBootScreen::TestRender(after.data(), 0.0f, 1.0, 1.0f);       // the background itself, with the bar at 0
        int bad = 0;
        const size_t px = static_cast<size_t>(300) * W + 700;
        const auto ch = [](uint32_t p, int s) { return static_cast<int>((p >> s) & 255); };
        const uint32_t art = done[px];
        if (art == plainFb[px] || mid[px] == plainFb[px] || mid[px] == art) ++bad;
        for (int s : {0, 8, 16}) {
            const int a = ch(plainFb[px], s), b = ch(art, s), m = ch(mid[px], s);
            if (m < std::min(a, b) - 1 || m > std::max(a, b) + 1) ++bad;
        }
        const unsigned v = (((700u) * 31 / W) << 11) | (((300u) * 63 / H) << 5) | 17u;   // the picture's pixel there, expanded the way the tool's decoders do
        const int r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
        if (ch(art, 0) != ((r << 3) | (r >> 2)) || ch(art, 8) != ((g << 2) | (g >> 4)) || ch(art, 16) != ((b << 3) | (b >> 2))) ++bad;
        if (after[px] != art) ++bad;   // the picture is now the background (the bar is drawn from y 497 down, this pixel is above it)
        std::printf("made picture fade: %s\n", bad ? "FAILED" : "ok");
        if (bad) return 1;
    }
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 20; ++i) VitaBootScreen::TestRender(fb.data(), 0.5f, i * 0.033, 1.0f);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 20.0;
    std::printf("%d pictures in %s; one full frame %.2f ms on this host\n", n, out, ms);
    return 0;
}
