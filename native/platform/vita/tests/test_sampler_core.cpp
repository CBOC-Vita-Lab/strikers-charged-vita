// diag13: host test of np_sampler_core.h (the table, the top-N pick, the context layout finder), with mutants: -DSAMPLER_MUT=1 (top pick ignores the order), 2 (layout takes the last word, not the first).
#include "np_sampler_core.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
int main() {
    static NpSampler::Table<256> t;
    t.Reset();
    for (int i = 0; i < 100; ++i) t.Add(0x81001000u);
    for (int i = 0; i < 40; ++i) t.Add(0x81002000u);
    for (int i = 0; i < 40; ++i) t.Add(0x81000F00u);
    for (int i = 0; i < 7; ++i) t.Add(0x81003000u);
    size_t idx[3];
    const size_t n = t.Top(idx, 3);
    CHECK(n == 3 && t.total == 187 && t.used == 4);
#if defined(SAMPLER_MUT) && SAMPLER_MUT == 1
    std::swap(idx[0], idx[1]);
#endif
    CHECK(t.key[idx[0]] == 0x81001000u && t.cnt[idx[0]] == 100);
    CHECK(t.key[idx[1]] == 0x81000F00u && t.cnt[idx[1]] == 40);   // the tie goes to the lower key
    CHECK(t.key[idx[2]] == 0x81002000u);
    // a full table counts overflow and never loops
    t.Reset();
    for (uint32_t k = 1; k <= 1000; ++k) t.Add(k * 4u);
    CHECK(t.used == 192 && t.overflow == 1000 - 192 && t.total == 1000);
    size_t big[40];
    CHECK(t.Top(big, 40) == 40);
    for (size_t i = 1; i < 40; ++i) CHECK(t.cnt[big[i - 1]] >= t.cnt[big[i]]);
    // layout: pc at word 15 (thumb bit set), lr sentinel at 14
    uint32_t ctx[128] = {};
    for (int i = 0; i < 13; ++i) ctx[i + 1] = 0xC0DE0000u + i;
    ctx[14] = 0xC0DE00FEu;
    ctx[15] = 0x81234569u;
    ctx[16] = 0x81234569u;   // a second word in the function (a copy): the first one is the pc
    NpSampler::Layout l = NpSampler::FindLayout(ctx, 128, 0x81234560u, 0x812345A0u, 0xC0DE00FEu);
#if defined(SAMPLER_MUT) && SAMPLER_MUT == 2
    l.pc = 16;
#endif
    CHECK(l.Ok() && l.pc == 15 && l.lr == 14);
    ctx[14] = 0;
    CHECK(!NpSampler::FindLayout(ctx, 128, 0x81234560u, 0x812345A0u, 0xC0DE00FEu).Ok());   // no sentinel: not calibrated
    std::printf(fails ? "sampler core: %d FAILED\n" : "sampler core: ok\n", fails);
    return fails ? 1 : 0;
}
