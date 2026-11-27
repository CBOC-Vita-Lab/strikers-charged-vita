// nxgl25 (vitaGL 0061): host test of the copy selection of wrcopy.c (mode 3's fallback). The text under test is the real text of vglWriteCopyVariant, vgl_wr_copy and the fallback switch,
// cut out of dependencies/vitaGL/source/utils/wrcopy.c by run_wrcopy_pick_test.sh; the SDK calls are fakes that count and the two NEON loops are emulations that need the same 32 byte alignment.
// The oracle below is written from the rules, not from the code: bytes exact; DMA only in mode 3, only from 8 KiB, only with both addresses below 0x81000000; a copy the DMA engine does not take goes
// to the 64 byte stores when the fallback is on (and there is a whole block after the alignment head) and to sceClibMemcpy when it is off; counters.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

static uint32_t g_dstBase, g_srcBase;     // the fake addresses of dst[0] and src[0]
static const uint8_t *g_srcP; static uint8_t *g_dstP; static size_t g_srcLen, g_dstLen;
static uint32_t fake_addr(const void *p) {
    const uint8_t *b = static_cast<const uint8_t *>(p);
    if (b >= g_srcP && b < g_srcP + g_srcLen + 1) return g_srcBase + static_cast<uint32_t>(b - g_srcP);
    if (b >= g_dstP && b < g_dstP + g_dstLen + 1) return g_dstBase + static_cast<uint32_t>(b - g_dstP);
    std::fprintf(stderr, "address of a pointer outside both buffers\n"); std::abort();
}
#define VGL_WR_ADDR(p) fake_addr(p)

static int c_plain, c_dma, c_n64, c_v128; static uint64_t b_plain, b_dma, b_n64;
static int g_dmaRet = 0, g_misalign = 0;
extern "C" void *sceClibMemcpy(void *d, const void *s, unsigned n) { ++c_plain; b_plain += n; std::memmove(d, s, n); return d; }
extern "C" int sceDmacMemcpy(void *d, const void *s, unsigned n) { ++c_dma; b_dma += n; if (g_dmaRet < 0) return g_dmaRet; std::memcpy(d, s, n); return 0; }
static inline void wr_neon64(uint8_t *d, const uint8_t *s, uint32_t blocks) {
    if (reinterpret_cast<uintptr_t>(d) & 31u) ++g_misalign;   // vst1.64 {..}, [r:256] faults on the console
    ++c_n64; b_n64 += blocks; std::memcpy(d, s, static_cast<size_t>(blocks) * 64u);
}
static inline void wr_vstm128(uint8_t *d, const uint8_t *s, uint32_t blocks) { ++c_v128; std::memcpy(d, s, static_cast<size_t>(blocks) * 128u); }
#define VGL_WRCOPY_MIN 192u
uint32_t vgl_wr_stat[8];
static volatile int vgl_wr_mode = 0;
#include "under_test.inc"

static int g_bad = 0, g_cases = 0;
#define CHECK(c, ...) do { if (!(c)) { if (g_bad < 12) { std::fprintf(stderr, "FAIL %s:%d ", __FILE__, __LINE__); std::fprintf(stderr, __VA_ARGS__); std::fprintf(stderr, "\n"); } ++g_bad; } } while (0)

int main() {
    const size_t kN = 70000;
    std::vector<uint8_t> srcv(kN + 256), dstv(kN + 512), ref(kN + 512);
    alignas(64) static uint8_t srcA[70000 + 256], dstA[70000 + 512];
    (void)srcv; (void)dstv;
    g_srcP = srcA; g_dstP = dstA; g_srcLen = sizeof srcA; g_dstLen = sizeof dstA;
    uint32_t x = 12345;
    for (size_t i = 0; i < sizeof srcA; ++i) { x = x * 1664525u + 1013904223u; srcA[i] = static_cast<uint8_t>(x >> 24); }
    const uint32_t addrs[] = {0x40200000u, 0x60000000u, 0x80fe0000u, 0x80ffe000u, 0x80ffff00u, 0x81000000u, 0x8286d8a8u};   // below, below, fits 70k, fits 8192 exactly, crosses 0x81000000, at it, heap
    const uint32_t sizes[] = {0, 1, 63, 64, 100, 191, 192, 193, 255, 256, 768, 4096, 8191, 8192, 8193, 9000, 65536, 70000 - 200};
    const int doffs[] = {0, 1, 8, 31, 32, 33, 63, 64, 70};
    const int soffs[] = {0, 1, 7, 16};
    for (int mode = -1; mode <= 4; ++mode)
    for (int fb = 0; fb <= 1; ++fb)
    for (int dmaRet = 0; dmaRet >= -1; --dmaRet)
    for (uint32_t sa : addrs) for (uint32_t da : addrs)
    for (uint32_t n : sizes) for (int dof : doffs) for (int sof : soffs) {
        if (dof + n + 8 > sizeof dstA || sof + n > sizeof srcA) continue;
        for (int viaCopy = 0; viaCopy <= 1; ++viaCopy) {
            if (mode < 0 && viaCopy) continue;
            g_srcBase = sa; g_dstBase = da; g_dmaRet = dmaRet; g_misalign = 0;
            c_plain = c_dma = c_n64 = c_v128 = 0; b_plain = b_dma = b_n64 = 0;
            std::memset(vgl_wr_stat, 0, sizeof vgl_wr_stat);
            vgl_wr_fb = fb;
            std::memset(dstA, 0xA5, sizeof dstA);
            std::memcpy(ref.data(), dstA, sizeof dstA);
            std::memcpy(ref.data() + dof, srcA + sof, n);
            const int m = mode < 0 ? 0 : mode;
            vgl_wr_mode = m;
            if (viaCopy) vgl_wr_copy(dstA + dof, srcA + sof, n); else vglWriteCopyVariant(m, dstA + dof, srcA + sof, n);
            ++g_cases;
            // full-span address check: both [sA, sA + n) and [dA, dA + n) must lie strictly below 0x81000000
            const uint32_t sA = sa + static_cast<uint32_t>(sof), dA = da + static_cast<uint32_t>(dof);
            const bool lo = sA < 0x81000000u && dA < 0x81000000u && n <= 0x81000000u - sA && n <= 0x81000000u - dA;
            // 1: the bytes
            CHECK(std::memcmp(dstA, ref.data(), sizeof dstA) == 0, "bytes differ: mode %d fb %d n %u dof %d sof %d dmaRet %d src 0x%08X dst 0x%08X via %d", m, fb, n, dof, sof, dmaRet, sA, dA, viaCopy);
            CHECK(g_misalign == 0, "unaligned 32 byte store: mode %d n %u dof %d", m, n, dof);
            // 2: who made the copy
            const bool direct = viaCopy && (m == 0 || n < VGL_WRCOPY_MIN);   // vgl_wr_copy keeps these on sceClibMemcpy
            const bool dmaTry = !direct && m == 3 && n >= 0x2000u && lo;
            const bool dmaOk = dmaTry && dmaRet >= 0;
            CHECK(c_dma == (dmaTry ? 1 : 0), "DMA calls %d, expected %d: mode %d fb %d n %u src 0x%08X dst 0x%08X via %d", c_dma, dmaTry ? 1 : 0, m, fb, n, sA, dA, viaCopy);
            if (direct) { CHECK(c_n64 == 0 && c_v128 == 0 && c_plain == 1 && b_plain == n, "direct copy not plain: mode %d n %u", m, n);
                CHECK(vgl_wr_stat[0] == 0 && vgl_wr_stat[1] == 0 && vgl_wr_stat[2] == 1 && vgl_wr_stat[3] == n, "plain-copy counters: mode %d n %u", m, n); continue; }
            if (dmaOk) { CHECK(c_plain == 0 && c_n64 == 0 && c_v128 == 0, "a DMA copy also copied by the CPU: n %u", n); }
            else {
                // the CPU's copy: mode 3 -> mode 1 when the fallback is on, plain when off; mode 1: 64 byte stores; mode 2: 128 byte stores; mode 0 (and any other): plain
                int eff = m;
                if (m == 3) eff = fb ? 1 : 0;
                if (m < 0 || m > 3) eff = 0;
                const uint32_t unit = eff == 1 ? 64u : 128u;
                const uint32_t head = static_cast<uint32_t>(-static_cast<intptr_t>(reinterpret_cast<uintptr_t>(dstA + dof))) & 63u;
                const bool stores = (eff == 1 || eff == 2) && n >= head + unit;
                CHECK(c_v128 == (stores && eff == 2 ? 1 : 0), "128 byte store calls %d: mode %d n %u", c_v128, m, n);
                CHECK(c_n64 == (stores && eff == 1 ? 1 : 0), "64 byte store calls %d, expected %d: mode %d fb %d n %u dof %d (src 0x%08X dst 0x%08X dmaRet %d)", c_n64, stores && eff == 1 ? 1 : 0, m, fb, n, dof, sA, dA, dmaRet);
                if (stores && eff == 1) { const uint32_t blocks = (n - head) / 64u; CHECK(b_n64 == blocks, "blocks %llu expected %u", (unsigned long long)b_n64, blocks); }
                if (!stores) CHECK(c_plain == 1 && b_plain == n, "plain copy missing: mode %d fb %d n %u", m, fb, n);
            }
            // 2b: nxr6a, the NEON / plain / DMA counters follow the branch the copy took: [0]/[1] a copy whose bulk is NEON stores (64 or 128 byte), [2]/[3] a copy made wholly by sceClibMemcpy (the FB=0 fallback included),
            //     [4]/[5] the DMA engine; every copy is in exactly one (before, vgl_wr_copy counted [0]/[1] ahead of the dispatch: the FB=0 plain fallbacks as NEON, every DMA copy twice)
            {
                uint32_t e0 = 0, e1 = 0, e2 = 0, e3 = 0;
                if (dmaOk) { /* only [4]/[5], checked below */ }
                else {
                    int eff = m; if (m == 3) eff = fb ? 1 : 0; if (m < 0 || m > 3) eff = 0;
                    const uint32_t unit = eff == 1 ? 64u : 128u;
                    const uint32_t head = static_cast<uint32_t>(-static_cast<intptr_t>(reinterpret_cast<uintptr_t>(dstA + dof))) & 63u;
                    if ((eff == 1 || eff == 2) && n >= head + unit) { e0 = 1; e1 = n; } else { e2 = 1; e3 = n; }
                }
                CHECK(vgl_wr_stat[0] == e0 && vgl_wr_stat[1] == e1 && vgl_wr_stat[2] == e2 && vgl_wr_stat[3] == e3,
                      "branch counters %u/%u %u/%u, expected %u/%u %u/%u: mode %d fb %d n %u dmaRet %d src 0x%08X dst 0x%08X via %d", vgl_wr_stat[0], vgl_wr_stat[1], vgl_wr_stat[2], vgl_wr_stat[3], e0, e1, e2, e3, m, fb, n, dmaRet, sA, dA, viaCopy);
            }
            // 3: counters (mode 3 only; the others leave [4..7] alone)
            if (m == 3 && n >= 0x2000u) {
                CHECK(vgl_wr_stat[4] == (dmaOk ? 1u : 0u) && vgl_wr_stat[5] == (dmaOk ? n : 0u), "dma counters %u/%u n %u", vgl_wr_stat[4], vgl_wr_stat[5], n);
                CHECK(vgl_wr_stat[6] == (lo ? 0u : 1u), "address-refusal counter %u, lo %d", vgl_wr_stat[6], lo);
                CHECK(vgl_wr_stat[7] == (dmaTry && dmaRet < 0 ? 1u : 0u), "error counter %u", vgl_wr_stat[7]);
            } else CHECK(vgl_wr_stat[6] == 0 && vgl_wr_stat[7] == 0 && (vgl_wr_stat[4] == 0 || viaCopy), "counters moved: mode %d n %u", m, n);
        }
    }
    std::printf("%d cases, %d failed checks\n", g_cases, g_bad);
    if (g_bad) return 1;
    std::puts("OK");
    return 0;
}
