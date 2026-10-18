// Build/run (x86-64, once per Reserve variant; ASLR must be off because array pointers are in the stream):
//   clang++ -std=c++20 -O2 -DTARGET_PC -DNOMINMAX -DVGX_NATIVE -I<vgx>/src -I<vgx>/native -I<vgx>/include -I<vgx>/third_party test_recptr.cpp <vgx>/src/vgx_rec.cpp -o t_x -lpthread
//   MSC_VGX_PLAY=frame setarch x86_64 -R ./t_x 200000      (the line printed must be equal for the two variants)
// Stream test of the recorder: the same pseudo-random sequence of record calls against the old and the new Reserve; prints a checksum of every word the player could read.
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
    static uint32_t words[5000];
    static uint8_t big[900000];
    for (auto& b : big) b = static_cast<uint8_t>(rnd());
    uint64_t pub = 0;
    for (int i = 0; i < N; ++i) {
        const uint32_t k = rnd() % 100;
        if (k < 30) { const uint32_t cnt = (rnd() % 4 == 0) ? 12 : (rnd() % 3 == 0 ? 9 : 1 + rnd() % 16); for (uint32_t j = 0; j < cnt; ++j) words[j] = rnd(); rec::XF(rnd() % 0x1000, cnt, words); }
        else if (k < 55) rec::BP(static_cast<uint8_t>(rnd() % 255), rnd());
        else if (k < 62) rec::BPField(static_cast<uint8_t>(rnd() % 8), static_cast<int>(rnd() % 16), 1 + static_cast<int>(rnd() % 4), rnd());
        else if (k < 68) rec::Vcd(static_cast<int>(rnd() % 21), static_cast<int>(rnd() % 4));
        else if (k < 76) rec::Array(static_cast<int>(rnd() % 12) + 9, big + (rnd() % 1000), 64 + rnd() % 4096, 12, false, true);
        else if (k < 80) { TexObj t; std::memset(&t, static_cast<int>(rnd()), sizeof t); rec::Tex(static_cast<int>(rnd() % 8), t, rnd()); }
        else if (k < 84) rec::Stream(big + (rnd() % 100), (rnd() % 3 == 0) ? 1 + rnd() % 700000 : 1 + rnd() % 3000, false); // sizes straddle chunk ends; > 64 bytes: by reference only for guest RAM -> copies here
        else if (k < 93) { // a raw primitive, written by the cursor, closed by the next record or by FinishRaw
            const uint32_t cnt = 1 + rnd() % 40, vs = 12 + 4 * (rnd() % 4);
            if (hle::BeginRaw(static_cast<uint8_t>(rnd() % 8), static_cast<int>(rnd() % 8), cnt, vs)) {
                const uint32_t n = 1 + rnd() % cnt;
                for (uint32_t v = 0; v < n; ++v) { std::memset(hle::g_rawCur.ptr, static_cast<int>(rnd()), vs); hle::g_rawCur.ptr += vs; }
                if (rnd() % 2) hle::FinishRaw();
            }
        }
        else if (k < 96) { for (uint32_t j = 0; j < 12; ++j) words[j] = rnd(); rec::XF(0x100 + rnd() % 64, 12, words); }
        else rec::Present(false); // MODE_THREAD would wait for the player; the test uses MSC_VGX_PLAY=frame (PlayPending is a stub)
        pub += rec::g_prod.cur ? rec::g_prod.cur->committed.load() : 0;
    }
    hle::FinishRaw();
    rec::Emit0(rec::R_PRESENT); // commits
    // walk the stream as the player does
    uint64_t h = 1469598103934665603ull, total = 0; uint32_t chunks = 0;
    for (rec::Chunk* c = rec::FirstChunk(); c; c = c->next.load()) {
        const uint32_t n = c->committed.load();
        for (uint32_t i = 0; i < n; ++i) { h = (h ^ c->data[i]) * 1099511628211ull; }
        total += n; ++chunks;
    }
    std::printf("words %llu chunks %u hash %016llx pubsum %llu\n", (unsigned long long)total, chunks, (unsigned long long)h, (unsigned long long)pub);
}
