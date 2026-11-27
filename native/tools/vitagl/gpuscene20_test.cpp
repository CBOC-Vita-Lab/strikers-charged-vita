// renderer/src/vgx_gpuscene20.h: the [gpu scenes] line (kinds, display, "other"), the parameter-buffer estimate and the texture census.   Run: run_rt_kinds_test.sh
#include <cstdio>
#include <cstring>
#include "../../../renderer/src/vgx_gpuscene20.h"
static int fails;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
int main() {
    gs20::Kind a[17], b[17];
    b[0].used = 1; b[0].w = 704; b[0].h = 400; b[0].scenes = 600; b[0].tiling = 300 * 3100; b[0].pixel = 300 * 12400;
    b[1].used = 1; b[1].w = 960; b[1].h = 544; b[1].display = 1; b[1].scenes = 300; b[1].pixel = 300 * 2000;
    b[16].used = 1; b[16].scenes = 30; b[16].pixel = 300 * 500;
    const std::string s = gs20::FormatScenes(a, b, 17, 300);
    CHECK(s.find("[gpu scenes] 300 frames: 704x400 x2.0/frame tiling 3.10 pixel 12.40 ms") != std::string::npos);
    CHECK(s.find("| display 960x544 x1.0 tiling 0.00 pixel 2.00 ms (includes the wait") != std::string::npos);
    CHECK(s.find("| other x0.1 tiling 0.00 pixel 0.50 ms") != std::string::npos);
    CHECK(gs20::FormatScenes(a, a, 17, 300).find("no timed scene") != std::string::npos);
    CHECK(gs20::PbBytes(1000, 300, true, 2) == 1000u * 36u + 100u * 12u);
    CHECK(gs20::PbBytes(10, 30, false, 0) == 200u);
    gs20::TcUpload(1, 256, 256, 1, 9, false, 1000);   // DXT1 with mips
    gs20::TcUpload(2, 32, 32, 0, 1, true, 3000);      // RGBA8, generated mips: 4000 bytes
    gs20::TcUpload(3, 128, 64, 0, 1, false, 500);     // RGBA8 linear, 128 on one side
    const std::string t = gs20::TcLine("x", gs20::TcSt().tex);
    CHECK(t.find("3 live textures") != std::string::npos && t.find("DXT 1 /") != std::string::npos && t.find("uncompressed linear 2 /") != std::string::npos);
    CHECK(t.find("with mips 2 /") != std::string::npos && t.find("128 texels or more a side 2 (DXT 1, uncompressed 1; with mips, the anisotropic ones: 1)") != std::string::npos);
    gs20::TcFree(1);
    CHECK(gs20::TcSt().tex.size() == 2 && gs20::TcSt().liveBytes == 4500);
    std::printf(fails ? "gpuscene20_test: %d FAILED\n" : "gpuscene20_test: ok\n", fails);
    return fails != 0;
}
