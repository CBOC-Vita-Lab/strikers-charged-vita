// cpu60-25: MSC_VGX_TEXSIZE_MEMO. TexDataSize (a 64-entry memo, shifts instead of divisions) must return what TexDataSizeRef (the plain loop) returns,
// for every width / height / format / level count the game can ask and for the ones the memo's key does not cover; mutated slots are caught.
#include "vgx_common.h"
#include "vgx_texdecode.h"

#include <cstdio>
#include <cstdlib>

namespace vgx { uint32_t g_gxd = 0x3FFFFFFFu; }

int main() {
    unsigned long checked = 0, wrong = 0;
    auto check = [&](uint32_t w, uint32_t h, uint32_t f, uint32_t l) {
        ++checked;
        const uint32_t a = vgx::TexDataSize(w, h, f, l), b = vgx::TexDataSizeRef(w, h, f, l);
        if (a != b) {
            if (++wrong <= 10) std::fprintf(stderr, "MISMATCH %ux%u fmt %u levels %u: %u against %u\n", w, h, f, l, a, b);
        }
    };
    // every size up to 1100 on both axes in steps that hit odd, even, power-of-two and clipped sizes, every format 0..20, level counts 0..17 (each asked twice: miss, then hit)
    const uint32_t dims[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 12, 15, 16, 17, 24, 31, 32, 33, 40, 48, 63, 64, 65, 96, 100, 127, 128, 129, 192, 255, 256, 257, 320, 384, 480, 511, 512, 513, 640, 768, 1000, 1023, 1024, 1025, 1100, 2048, 4096, 0};
    for (uint32_t f = 0; f < 21; ++f)
        for (uint32_t w : dims)
            for (uint32_t h : dims)
                for (uint32_t l = 0; l < 18; ++l) { check(w, h, f, l); check(w, h, f, l); }
    // a dense sweep of small sizes (the collisions of the 64-entry table are exercised: many keys share a slot)
    for (uint32_t f = 0; f < 16; ++f)
        for (uint32_t w = 1; w <= 130; ++w)
            for (uint32_t h = 1; h <= 70; h += 3)
                for (uint32_t l = 1; l <= 9; l += 2) check(w, h, f, l);
    // pseudo-random
    uint32_t seed = 987654321u;
    auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
    for (int i = 0; i < 2000000; ++i) check(rnd() % 1100, rnd() % 1100, rnd() % 18, rnd() % 14);
    std::printf("texsize_memo: %lu checked, %lu wrong\n", checked, wrong);
    return wrong == 0 ? 0 : 1;
}
