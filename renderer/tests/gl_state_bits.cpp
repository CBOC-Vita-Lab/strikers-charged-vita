// glStateBits.h against the bit-by-bit loops it replaced, on the real field table of glState.cpp.
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <random>
#include "NL/gl/glStateBits.h"

static unsigned long GetLoop(unsigned long raster, int startBit, int numBits) {
    unsigned long out = 0;
    for (int i = 0; i < numBits; i++)
        if (raster & (1u << (i + startBit))) out |= (1u << i);
    return out;
}
static unsigned long PutLoop(unsigned long state, int startBit, int numBits, unsigned long value) {
    for (int i = 0; i < numBits; i++) {
        if (value & (1u << i)) state = state | (1u << (i + startBit));
        else state = state & ~(1u << (i + startBit));
    }
    return state;
}
int main() {
    // (packed_raster's widths, in glState.cpp's order)
    static const int kWidths[] = {1, 1, 2, 1, 8, 3, 2, 2, 1, 1, 1, 2, 2, 2, 1, 2};
    std::mt19937 rng(1);
    unsigned long checks = 0;
    int start = 0;
    for (int width : kWidths) {
        for (int n = 0; n < 200000; ++n) {
            const unsigned long s = static_cast<uint32_t>(rng()), v = n & 1 ? static_cast<uint32_t>(rng()) : rng() & 7;
            if (GetLoop(s, start, width) != glStateBitsGet(s, start, width)) { std::printf("FAIL get %d %d\n", start, width); return 1; }
            if (static_cast<uint32_t>(PutLoop(s, start, width, v)) != static_cast<uint32_t>(glStateBitsPut(s, start, width, v))) { std::printf("FAIL put %d %d\n", start, width); return 1; }
            ++checks;
        }
        start += width;
    }
    if (start != 32) { std::printf("FAIL table is %d bits\n", start); return 1; }
    // every position a 32-bit word has, for fields of up to 8 bits
    for (int s = 0; s < 32; ++s)
        for (int w = 0; w <= 8 && s + w <= 32; ++w)
            for (int n = 0; n < 2000; ++n) {
                const unsigned long a = static_cast<uint32_t>(rng()), v = static_cast<uint32_t>(rng());
                if (GetLoop(a, s, w) != glStateBitsGet(a, s, w) || static_cast<uint32_t>(PutLoop(a, s, w, v)) != static_cast<uint32_t>(glStateBitsPut(a, s, w, v))) {
                    std::printf("FAIL %d %d\n", s, w);
                    return 1;
                }
            }
    std::printf("gl_state_bits ok (%lu checks)\n", checks);
}
