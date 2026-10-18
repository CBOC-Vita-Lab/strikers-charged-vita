// Build/run (x86-64 or armhf under qemu-arm):
//   clang++ -std=c++20 -O2 -DTARGET_PC -DNOMINMAX -DVGX_NATIVE -I<vgx>/src -I<vgx>/native -I<vgx>/include -I<vgx>/third_party test_recxf.cpp <vgx>/src/vgx_rec.cpp -o t_xf -lpthread
//   MSC_VGX_PLAY=frame ./t_xf 300000 0x3FFFFFFF   and   ./t_xf 300000 0x3FFFEFFF   (GXD_RECFAST on / off): the line printed must be equal
// Stream test of the recorder's XF fast paths (GXD_RECFAST): register writes, position / texture / normal matrix loads from unaligned-by-8 sources,
// out-of-range addresses (the fallback), mixed with other records; prints a checksum of every word the player could read and of the XF register shadow.
#include "vgx_rec.h"
#include "vgx_hle.h"
#include "vgx_state.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
namespace vgx {
uint32_t g_gxd = 0x3FFFFFFF;
bool IsGuestRam(const void*, uint32_t) { return false; }
bool HaveGuestRam() { return false; }
void GxdInit() {}
void InitState(State&) {}
const Plan& GetPlanIn(PlanCache& c, bool&, const uint8_t*, const VtxAttrFmt (*)[26], int, bool) { return c.p[0]; }
namespace rec { uint32_t PlayPending() { return 0; } Chunk* FirstChunk(); }
}
using namespace vgx;
static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd() { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return static_cast<uint32_t>(rng >> 16); }
int main(int argc, char** argv) {
    rec::InitMode();
    const int N = argc > 1 ? std::atoi(argv[1]) : 300000;
    if (argc > 2) vgx::g_gxd = static_cast<uint32_t>(std::strtoul(argv[2], nullptr, 0));
    static uint32_t src[64 * 4 + 8];
    for (int i = 0; i < N; ++i) {
        for (auto& w : src) w = rnd();
        const uint32_t k = rnd() % 100;
        const void* m = src + (rnd() % 8); // 4-byte aligned, not always 8
        if (k < 30) rec::XFReg((rnd() % 3 == 0) ? 0x0FF0 + rnd() % 0x80 : 0x1000 + rnd() % 0x70, rnd());
        else if (k < 40) rec::XFReg(rnd() % 3 == 0 ? 0x1018 : 0x1019, rnd());
        else if (k < 60) rec::XFMtx((rnd() % 20 == 0) ? 0x0FF8 + rnd() % 16 : (rnd() % 64) * 4, m, 12);
        else if (k < 70) rec::XFMtx((rnd() % 20 == 0) ? 0x0FFC + rnd() % 8 : 0x500 + (rnd() % 32) * 4, m, 8);
        else if (k < 85) rec::XFNrm((rnd() % 20 == 0) ? 0x0FF8 + rnd() % 16 : 0x400 + (rnd() % 20) * 3, m);
        else if (k < 92) rec::BP(static_cast<uint8_t>(rnd() % 255), rnd());
        else if (k < 96) rec::Vcd(static_cast<int>(rnd() % 21), static_cast<int>(rnd() % 4));
        else rec::Present(false);
    }
    rec::Emit0(rec::R_PRESENT);
    uint64_t h = 1469598103934665603ull, total = 0; uint32_t chunks = 0;
    for (rec::Chunk* c = rec::FirstChunk(); c; c = c->next.load()) {
        const uint32_t n = c->committed.load();
        for (uint32_t i = 0; i < n; ++i) h = (h ^ c->data[i]) * 1099511628211ull;
        total += n; ++chunks;
    }
    uint64_t sh = 1469598103934665603ull;
    for (uint32_t v : rec::gs.xfReg) sh = (sh ^ v) * 1099511628211ull;
    sh = (sh ^ rec::gs.matIdxA) * 1099511628211ull; sh = (sh ^ rec::gs.matIdxB) * 1099511628211ull;
    std::printf("words %llu chunks %u hash %016llx shadow %016llx\n", (unsigned long long)total, chunks, (unsigned long long)h, (unsigned long long)sh);
    return 0;
}
