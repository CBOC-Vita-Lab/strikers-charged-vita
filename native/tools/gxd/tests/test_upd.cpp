#include "vgx_common.h"
#include <cstdio>
#include <cstdlib>
using namespace vgx;
static uint32_t rs = 12345;
static uint32_t rnd() { rs = rs * 1664525u + 1013904223u; return rs >> 8; }
int main() {
    int bad = 0, n = 0;
    alignas(16) uint8_t A[200], B[200], R[200];
    for (int it = 0; it < 200000; ++it) {
        const int off = it % 5; // unaligned destination and source
        for (int i = 0; i < 200; ++i) A[i] = static_cast<uint8_t>(rnd());
        for (int i = 0; i < 200; ++i) B[i] = A[i];
        const int words = (it & 1) ? 12 : 9;
        uint8_t* d = A + off + 16; const uint8_t* s = B + 64 + off;
        std::memcpy(const_cast<uint8_t*>(s), d, words * 4);
        const int mode = rnd() % 4;
        if (mode == 1) { const int w = rnd() % words; const_cast<uint8_t*>(s)[w * 4 + (rnd() % 4)] ^= static_cast<uint8_t>(1 + rnd() % 255); }
        else if (mode == 2) { for (int w = 0; w < words; ++w) const_cast<uint8_t*>(s)[w * 4] = static_cast<uint8_t>(rnd()); }
        std::memcpy(R, A, 200);
        const bool differ = std::memcmp(d, s, words * 4) != 0;
        if (differ) std::memcpy(R + off + 16, s, words * 4);
        const bool got = words == 12 ? UpdateWords12(d, s) : UpdateWords9(d, s);
        ++n;
        if (got != differ || std::memcmp(R, A, 200) != 0) ++bad;
    }
    std::printf("%d cases, %d bad\n", n, bad);
    return bad != 0;
}
