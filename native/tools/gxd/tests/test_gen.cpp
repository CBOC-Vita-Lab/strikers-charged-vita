#include "vgx_guestmem.h"
#include "vgx_hle.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
using namespace vgx;
namespace vgx { uint32_t g_gxd = 0x3FFFFFFF; }
static uint32_t rs = 777;
static uint32_t rnd() { rs = rs * 1664525u + 1013904223u; return rs >> 8; }
int main() {
    const uint32_t SZ = 4u << 20;
    std::vector<uint8_t> mem(SZ);
    hle::RegisterGuestRam(mem.data(), SZ);
    std::vector<uint64_t> shadow(SZ >> 12, 1);
    int bad = 0;
    for (int it = 0; it < 200000; ++it) {
        if (rnd() % 3 == 0) { // a write notification
            const uint32_t o = rnd() % (SZ - 5000), n = 1 + rnd() % 5000;
            hle::NotifyGuestWrite(mem.data() + o, n);
            for (uint32_t p = o >> 12; p <= (o + n - 1) >> 12; ++p) shadow[p]++;
        }
        const uint32_t o = rnd() % (SZ - 200000), n = 1 + rnd() % ((rnd() % 4 == 0) ? 200000 : 30000);
        uint64_t want = 0;
        for (uint32_t p = o >> 12; p <= (o + n - 1) >> 12; ++p) want += shadow[p];
        const uint64_t got = GuestWriteGeneration(mem.data() + o, n);
        if (got != want) { if (++bad < 5) std::printf("mismatch o=%u n=%u got %llu want %llu\n", o, n, (unsigned long long)got, (unsigned long long)want); }
    }
    std::printf("bad %d\n", bad);
    return bad != 0;
}
