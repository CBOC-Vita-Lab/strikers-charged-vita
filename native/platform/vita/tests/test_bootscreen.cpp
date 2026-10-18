// Pictures of the player build's loading screen (vita_bootscreen.cpp) on the host, for review without a console:
//   g++ -std=gnu++17 -O2 -Isrc tests/test_bootscreen.cpp src/vita_bootscreen.cpp -o /tmp/tb && /tmp/tb <loading.bin or -> <out dir>
// writes <out>/boot_<n>.ppm at several bar values and moments, and the time one full frame and one bar-region frame take.
#include "np_vita_internal.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

namespace VitaConsole {
void ShowText() {}
bool OwnsDisplay() { return true; }
const uint32_t* Pixels() { return nullptr; }
void PresentScreen() {}
} // namespace VitaConsole

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
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 20; ++i) VitaBootScreen::TestRender(fb.data(), 0.5f, i * 0.033, 1.0f);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 20.0;
    std::printf("%d pictures in %s; one full frame %.2f ms on this host\n", n, out, ms);
    return 0;
}
