#include "vgx_texdecode.h"
#include "vgx_common.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
namespace vgx { uint32_t g_gxd = 0x3FFFFFFF; }
static uint32_t rs = 99;
static uint32_t rnd() { rs = rs * 1664525u + 1013904223u; return rs >> 8; }
int main() {
    int bad = 0, n = 0;
    for (int it = 0; it < 400; ++it) {
        const uint32_t w = 8 * (1 + rnd() % 40), h = 4 * (1 + rnd() % 40);
        std::vector<uint8_t> src(w * h + 7), a(w * h * 4 + 3), b(w * h * 4 + 3);
        for (auto& x : src) x = static_cast<uint8_t>(rnd());
        vgx::g_gxd = 0x3FFFFFFF; vgx::DecodeTexture(src.data() + (it & 1), w, h, 1, nullptr, 0, 0, a.data() + (it & 3)); // (source and destination at odd addresses too)
        vgx::g_gxd = 0x3FFFFFFF & ~(1u << 8); vgx::DecodeTexture(src.data() + (it & 1), w, h, 1, nullptr, 0, 0, b.data() + (it & 3));
        ++n;
        if (std::memcmp(a.data(), b.data(), a.size()) != 0) ++bad;
    }
    std::printf("%d sizes, %d bad\n", n, bad);
    return bad != 0;
}
