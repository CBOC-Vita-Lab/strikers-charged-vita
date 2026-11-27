// Host test of the Settings screen with the player's own game art (np_brand_art.cpp): draws the list and the controls page with and without the art.
//   c++ -std=c++17 -O1 -Isrc tests/test_brand_art.cpp src/np_brand_art.cpp src/np_settings.cpp src/vita_plan.cpp -o /tmp/tba
//   /tmp/tba <game root: the folder that holds files/ or "-" for none> <out dir>
// Writes plain.ppm (no art: the original look), art.ppm, art-row.ppm (a row selected), controls-art.ppm. With "-" the art is absent, as when the disc
// files are missing, and the screen must be the plain one. Without a disc the test checks only that.
#include "np_brand_art.h"
#include "np_settings.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static void WritePpm(const std::string& path, const uint32_t* px) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { std::perror(path.c_str()); std::exit(2); }
    std::fprintf(f, "P6\n960 544\n255\n");
    for (int i = 0; i < 960 * 544; ++i) {
        const unsigned char c[3] = {static_cast<unsigned char>(px[i]), static_cast<unsigned char>(px[i] >> 8), static_cast<unsigned char>(px[i] >> 16)};
        std::fwrite(c, 1, 3, f);
    }
    std::fclose(f);
}

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: %s <game root or -> <out dir>\n", argv[0]); return 2; }
    const std::string root = argv[1], out = argv[2];
    std::vector<uint32_t> px(960 * 544);
    NpSettings::State state;
    NpSettings::Load(state, nullptr);

    NpSettings::SetArt(nullptr);
    NpSettings::Draw(px.data(), state, 0, 0, "", false);
    WritePpm(out + "/plain.ppm", px.data());
    std::vector<uint32_t> plain = px;

    NpBrandArt::Art art;
    const bool loaded = root != "-" && art.Load(root.c_str());
    if (root != "-" && !loaded) { std::fprintf(stderr, "FAIL: the art did not load from %s\n", root.c_str()); return 1; }
    if (root == "-" && art.Load("/nonexistent")) { std::fprintf(stderr, "FAIL: loaded art from nowhere\n"); return 1; }
    NpSettings::Art screenArt;
    screenArt.backdrop = art.Backdrop();
    screenArt.heading = [&art](uint32_t* p, int x, int y, int h, const char* t, bool draw) { return art.Heading(p, x, y, h, t, draw); };
    NpSettings::SetArt(&screenArt);
    // an art without a picture is the plain screen, pixel for pixel
    if (!loaded) {
        NpSettings::Draw(px.data(), state, 0, 0, "", false);
        if (std::memcmp(px.data(), plain.data(), plain.size() * 4) != 0) { std::fprintf(stderr, "FAIL: the fallback differs from the plain screen\n"); return 1; }
        std::printf("fallback ok (no art: the plain screen)\n");
        return 0;
    }
    if (!art.HasFont() || art.Heading(nullptr, 0, 0, 40, "SETTINGS", false) < 100) { std::fprintf(stderr, "FAIL: the heading font did not load\n"); return 1; }
    NpSettings::Draw(px.data(), state, 0, 0, "", false);
    WritePpm(out + "/art.ppm", px.data());
    NpSettings::Draw(px.data(), state, 3, 0, "Saved to ux0:data/mscharged/np-player.txt", false);
    WritePpm(out + "/art-row.ppm", px.data());
    NpSettings::DrawControls(px.data(), state);
    WritePpm(out + "/controls-art.ppm", px.data());
    NpSettings::SetArt(nullptr);
    NpSettings::DrawControls(px.data(), state);
    WritePpm(out + "/controls-plain.ppm", px.data());
    art.Release();
    if (art.Backdrop()) { std::fprintf(stderr, "FAIL: Release kept the picture\n"); return 1; }
    std::printf("art ok\n");
    return 0;
}
