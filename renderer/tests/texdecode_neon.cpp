// GXD_TEXNEON: the NEON LA88 tile decoders must write the same bytes as the scalar loops (I4, I8, IA4, IA8; whole-tile and clipped sizes).
// Built for ARM (or anywhere with VGX_FORCE_NEON undefined: then both runs are scalar and the test is trivially true).
#include "vgx_common.h"
#include "vgx_texdecode.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace vgx { uint32_t g_gxd = 0x3FFFFFFFu; }

int main() {
    unsigned checked = 0, neonSizes = 0;
    uint32_t seed = 12345;
    auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return static_cast<uint8_t>(seed >> 24); };
    const unsigned sizes[][2] = {{8, 8}, {16, 8}, {8, 16}, {32, 32}, {64, 64}, {128, 32}, {256, 256}, {16, 16}, {24, 8}, {40, 24},
                                 {4, 4}, {4, 8}, {12, 12}, {20, 10}, {30, 6}, {1, 1}, {2, 2}, {7, 5}, {33, 17}, {65, 3}};
    for (unsigned fmt = 0; fmt < 4; ++fmt)
        for (const auto& sz : sizes) {
            const unsigned w = sz[0], h = sz[1];
            std::vector<uint8_t> src(vgx::TexDataSize(w, h, fmt, 1) + 64);
            for (int pass = 0; pass < 3; ++pass) {
                for (auto& b : src) b = pass == 0 ? rnd() : (pass == 1 ? 0xFF : 0x00);
                const size_t n = static_cast<size_t>(w) * h * 2;
                std::vector<uint8_t> a(n + 16, 0xA5), b(n + 16, 0xA5);
                vgx::g_gxd = 0x3FFFFFFFu;
                const bool okA = vgx::DecodeTextureLA88(src.data(), w, h, fmt, a.data());
                vgx::g_gxd = 0x3FFFFFFFu & ~vgx::GXD_TEXNEON;
                const bool okB = vgx::DecodeTextureLA88(src.data(), w, h, fmt, b.data());
                if (okA != okB || a != b) {
                    std::fprintf(stderr, "FAIL: fmt %u %ux%u pass %d differs (ok %d/%d)\n", fmt, w, h, pass, okA, okB);
                    return 1;
                }
                ++checked;
            }
            neonSizes += (w % 8 == 0 && h % 8 == 0);
        }
    std::printf("texdecode neon: %u cases identical (%u whole-tile sizes)\n", checked, neonSizes);
    return 0;
}
