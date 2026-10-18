// Standalone drawing test, part 2: the driver. Runs the scene of scene.cpp through the native GX glue and libvgx-native
// (no recompilation runtime, no game), writes the picture as a PNG and checks pixels and the numeric helpers.
//
//   gx_native_test [--no-ram] [--frames N] [--png out.png] [--null]
//     --no-ram   do not register the scene's arena as guest RAM (display lists copied, textures re-hashed per bind)
//     --frames   frames to draw (default 3: the later ones run the display-list cache and texture trust paths)
//     --null     expect the null back end (no picture): only the call paths and the numeric checks run
// Exit status 0 when every check passed.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "np_gx.h"
#include "vgx_native.h"

extern "C" {
void* np_test_arena(unsigned int* size);
void np_test_draw_frame(int frame);
unsigned int np_test_numeric();
void np_test_set_fog();
}

namespace {
struct Expect { int x, y; int r, g, b; const char* what; };
const Expect kExpect[] = {
    {70, 70, 255, 0, 0, "direct vertices (WGPIPE floats + colours)"},
    {210, 70, 0, 255, 0, "indexed, big-endian arrays, indices through WGPIPE"},
    {350, 70, 0, 0, 255, "indexed, little-endian arrays (NP_GX_SETARRAY le=1)"},
    {490, 70, 255, 255, 0, "indexed, legacy 3-argument GXSetArray, default little-endian"},
    {70, 210, 255, 0, 255, "big-endian display list"},
    {185, 185, 255, 0, 0, "RGB565 texture, tile 0 (red)"},
    {235, 185, 0, 255, 0, "RGB565 texture, tile 1 (green)"},
    {185, 235, 0, 0, 255, "RGB565 texture, tile 2 (blue)"},
    {235, 235, 255, 255, 255, "RGB565 texture, tile 3 (white)"},
    {325, 210, 255, 130, 0, "CI8 + TLUT, index 0 (RGB565 0xFC00)"},
    {375, 210, 0, 255, 255, "CI8 + TLUT, index 1 (RGB565 0x07FF)"},
    {490, 210, 255, 0, 0, "EFB copy texture of the red block"},
    {70, 350, 0, 255, 255, "TEV konst colour"},
    {210, 350, 255, 128, 0, "display list called twice"},
    {350, 350, 255, 128, 0, "second display list"},
    {490, 350, 128, 255, 0, "display list over little-endian arrays, called twice"},
    {600, 450, 32, 48, 64, "background (GXSetCopyClear colour)"},
};
const char* const kNumeric[] = {
    "GXInitLightDistAttn", "GXInitSpecularDir", "GXInitLightAttnA", "GXGetProjectionv (perspective)", "GXSetProjectionv round trip (orthographic)",
    "GXGetViewportv", "GXProject", nullptr, "GXSetDrawDone / GXSetDrawDoneCallback", "GXGetGPStatus",
    "texture object accessors", "GXAdjustForOverscan", "dynamic aspect (widened full-screen perspective, raw read-back, small viewport, off)",
};
int Abs(int v) { return v < 0 ? -v : v; }
} // namespace

int main(int argc, char** argv) {
    bool ram = true, nullBackend = false;
    int frames = 3;
    std::string png = "gx_native_test.png";
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--no-ram")) ram = false;
        else if (!std::strcmp(argv[i], "--null")) nullBackend = true;
        else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--png") && i + 1 < argc) png = argv[++i];
    }
    np_gx_config cfg;
    std::memset(&cfg, 0, sizeof cfg);
    cfg.size = sizeof cfg;
    if (!np_gx_start(&cfg)) { std::fprintf(stderr, "np_gx_start failed\n"); return 2; }
    unsigned int arenaSize = 0;
    void* arena = np_test_arena(&arenaSize);
    if (ram) np_gx_register_ram(arena, arenaSize);

    int failures = 0;
    unsigned char* firstPicture = nullptr;
    int pw = 0, ph = 0;
    for (int f = 0; f < frames; ++f) {
        // the last frame is dumped (and waited for)
        if (f == frames - 1 && !nullBackend) np_gx_request_dump(png.c_str());
        np_test_draw_frame(f);
        if (nullBackend) continue;
        if (f == 0 || f == frames - 1) {
            int w = 0, h = 0;
            const unsigned char* px = np_gx_last_picture(&w, &h);
            if (!px) { std::printf("frame %d: no picture\n", f); if (f == frames - 1) ++failures; continue; }
            if (f == 0) {
                // (frame 0 was not waited for: its picture may be the previous one; only compare later frames with it when valid)
                (void)px;
            }
            if (f == frames - 1) {
                pw = w; ph = h;
                std::printf("picture %dx%d (%s guest RAM)\n", w, h, ram ? "registered" : "unregistered");
                if (w != 640 || h != 480) { std::printf("  FAIL picture size: the display copy is 640 x 480\n"); ++failures; }
                for (const Expect& e : kExpect) {
                    const unsigned char* c = px + (static_cast<size_t>(e.y) * w + e.x) * 4;
                    const bool ok = Abs(c[0] - e.r) <= 8 && Abs(c[1] - e.g) <= 8 && Abs(c[2] - e.b) <= 8;
                    std::printf("  %-4s (%3d,%3d) = %3u %3u %3u  want %3d %3d %3d  %s\n", ok ? "ok" : "FAIL", e.x, e.y, c[0], c[1], c[2], e.r, e.g, e.b, e.what);
                    if (!ok) ++failures;
                }
                firstPicture = static_cast<unsigned char*>(std::malloc(static_cast<size_t>(w) * h * 4));
                std::memcpy(firstPicture, px, static_cast<size_t>(w) * h * 4);
            }
        }
    }
    (void)pw; (void)ph;
    std::free(firstPicture);

    const unsigned int bad = np_test_numeric();
    for (unsigned i = 0; i < sizeof(kNumeric) / sizeof(kNumeric[0]); ++i) {
        if (!kNumeric[i]) continue;
        const bool ok = !(bad & (1u << i));
        std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", kNumeric[i]);
        if (!ok) ++failures;
    }
    np_test_set_fog();
    {
        const unsigned int fog = vgxn_bp_reg(0xF2);
        const bool ok = fog == ((10u << 16) | (20u << 8) | 30u);
        std::printf("  %-4s GXSetFog colour reaches BP 0xF2 (SDK argument order): 0x%06X\n", ok ? "ok" : "FAIL", fog);
        if (!ok) ++failures;
    }
    const unsigned int stray = np_gx_stray_pipe_writes();
    std::printf("  %-4s no stray gather-pipe writes (%u)\n", stray == 0 ? "ok" : "FAIL", stray);
    if (stray) ++failures;
    std::printf("%s: %d failure(s), %llu frame(s) presented\n", failures ? "FAILED" : "PASSED", failures, np_gx_frame_count());
    return failures ? 1 : 0;
}
