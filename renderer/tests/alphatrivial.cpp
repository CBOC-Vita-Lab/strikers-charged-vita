// Unit test of AlphaTestAlwaysPasses / AnalyzeFs (vgx_shadergen.cpp, W12 SHADER): an alpha test that provably cannot fail is not emitted.
// No GL, no game data. The brute-force check evaluates the shader's own compare text semantics for every 8-bit alpha.
#include "../src/vgx_shadergen.h"
#include <cstdio>
#include <cstdlib>

using namespace vgx;

static int g_failures = 0;
static void Check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
}

// TEV alpha word: a b c d (3 bits each at 13/10/7/4), bias 16, op 18, clamp 19, scale 20, dest 22. Inputs: 0 prev 1 c0 2 c1 3 c2 4 tex 5 ras 6 ka 7 zero.
static uint32_t Alpha(int a, int b, int c, int d, int bias = 0, int op = 0, int clamp = 1, int scale = 0, int dest = 0) {
    return (uint32_t(a) << 13) | (uint32_t(b) << 10) | (uint32_t(c) << 7) | (uint32_t(d) << 4) | (uint32_t(bias) << 16) | (uint32_t(op) << 18) |
           (uint32_t(clamp) << 19) | (uint32_t(scale) << 20) | (uint32_t(dest) << 22);
}

static bool Brute(uint32_t c0, uint32_t c1, uint32_t op, int ref0, int ref1, int lo, int hi) {
    auto cmp = [](uint32_t c, int a, int r) {
        switch (c) { case 0: return false; case 1: return a < r; case 2: return a == r; case 3: return a <= r; case 4: return a > r; case 5: return a != r; case 6: return a >= r; default: return true; }
    };
    for (int a = lo; a <= hi; ++a) {
        const bool t0 = cmp(c0, a, ref0), t1 = cmp(c1, a, ref1);
        const bool p = op == 0 ? (t0 && t1) : (op == 1 ? (t0 || t1) : (op == 2 ? (t0 != t1) : (t0 == t1)));
        if (!p) return false;
    }
    return true;
}

int main() {
    // 1. Unknown alpha (one stage that passes the rasterised alpha on): the answer is the brute force over 0..255, for every compare pair and reference.
    int checked = 0;
    for (uint32_t c0 = 0; c0 < 8; ++c0)
        for (uint32_t c1 = 0; c1 < 8; ++c1)
            for (uint32_t op = 0; op < 4; ++op)
                for (int ref : {0, 1, 2, 127, 128, 254, 255}) {
                    FsKey k;
                    k.numStages = 1;
                    k.stage[0].alpha = Alpha(7, 7, 7, 5); // d = ras.a
                    k.stage[0].rasChan = 0;
                    k.alphaTestOn = 1;
                    k.alphaTest = uint8_t(c0 | (c1 << 3) | (op << 6));
                    const bool got = AlphaTestAlwaysPasses(k, uint32_t(ref), uint32_t(255 - ref));
                    Check(got == Brute(c0, c1, op, ref, 255 - ref, 0, 255), "unknown alpha agrees with brute force");
                    ++checked;
                }
    // 2. GEQUAL 0 and LEQUAL 255 always pass; GREATER 0 does not.
    {
        FsKey k;
        k.numStages = 1;
        k.stage[0].alpha = Alpha(7, 7, 7, 5);
        k.stage[0].rasChan = 0;
        k.alphaTestOn = 1;
        k.alphaTest = uint8_t(6 | (7 << 3) | (0 << 6));
        Check(AlphaTestAlwaysPasses(k, 0, 0), "GEQUAL 0 AND ALWAYS passes");
        k.alphaTest = uint8_t(4 | (7 << 3));
        Check(!AlphaTestAlwaysPasses(k, 0, 0), "GREATER 0 can fail");
        k.alphaTest = uint8_t(3 | (3 << 3));
        Check(AlphaTestAlwaysPasses(k, 255, 255), "LEQUAL 255 AND LEQUAL 255 passes");
    }
    // 3. A constant alpha chain: d = ka (konst alpha select 0 = 8/8 = 1.0): alpha 255; GREATER 128 passes, GREATER 255 does not, GREATER 254 passes.
    {
        FsKey k;
        k.numStages = 1;
        k.stage[0].alpha = Alpha(7, 7, 7, 6);
        k.stage[0].kasel = 0;
        k.alphaTestOn = 1;
        k.alphaTest = uint8_t(4 | (4 << 3));
        Check(AlphaTestAlwaysPasses(k, 128, 128), "constant alpha 1.0 passes GREATER 128");
        Check(AlphaTestAlwaysPasses(k, 200, 100), "constant alpha 1.0 passes GREATER 200 AND GREATER 100");
        Check(!AlphaTestAlwaysPasses(k, 255, 0), "constant alpha 1.0 fails GREATER 255");
        Check(!AlphaTestAlwaysPasses(k, 254, 0), "(one step of slack) alpha 255 vs GREATER 254 is not taken as passing");
        // constant 4/8 = 0.5 = 127.5 -> 128: slack covers 127..129
        k.stage[0].kasel = 4;
        k.alphaTest = uint8_t(4 | (7 << 3));
        Check(AlphaTestAlwaysPasses(k, 100, 0), "constant 0.5 passes GREATER 100");
        Check(!AlphaTestAlwaysPasses(k, 127, 0), "constant 0.5 is not taken as passing GREATER 127 (rounding edge)");
        const FsFacts f = AnalyzeFs(k);
        Check(f.constAlpha == 1 && f.constVal == 128, "AnalyzeFs: constant 0.5 -> 128");
    }
    // 4. Two stages: stage 0 writes ka to PREV, stage 1 multiplies by ras alpha (unknown) -> not constant.
    {
        FsKey k;
        k.numStages = 2;
        k.stage[0].alpha = Alpha(7, 7, 7, 6);
        k.stage[1].alpha = Alpha(7, 0, 5, 7); // mix(0, prev, ras) = prev * ras
        k.stage[1].rasChan = 0;
        k.alphaTestOn = 1;
        k.alphaTest = uint8_t(4 | (7 << 3));
        Check(!AlphaTestAlwaysPasses(k, 0, 0), "alpha times an unknown is unknown");
        Check(AnalyzeFs(k).constAlpha == 0, "AnalyzeFs: not constant");
    }
    // 5. Texture alpha is reported, not assumed.
    {
        FsKey k;
        k.numStages = 1;
        k.stage[0].alpha = Alpha(7, 7, 7, 4);
        k.stage[0].texEnable = 1;
        k.stage[0].texMap = 3;
        k.stage[0].texSwap = 0xE4;
        const FsFacts f = AnalyzeFs(k);
        Check(f.texAlphaMap == 3 && !f.constAlpha, "AnalyzeFs: alpha is texture 3's alpha");
        k.stage[0].texSwap = 0x1B; // a swap table that puts another channel into alpha
        Check(AnalyzeFs(k).texAlphaMap == 0xFF, "AnalyzeFs: swapped alpha is not claimed");
    }
    // 6. W12 CROWD, AlphaTestPassesFromOne (MSC_VGX_BLENDNODISCARD): the test passes for every alpha from 1 to 255, for every compare pair and reference.
    int fromOne = 0;
    for (uint32_t c0 = 0; c0 < 8; ++c0)
        for (uint32_t c1 = 0; c1 < 8; ++c1)
            for (uint32_t op = 0; op < 4; ++op)
                for (int ref : {0, 1, 2, 127, 128, 254, 255}) {
                    FsKey k;
                    k.numStages = 1;
                    k.stage[0].alpha = Alpha(7, 7, 7, 5);
                    k.stage[0].rasChan = 0;
                    k.alphaTestOn = 1;
                    k.alphaTest = uint8_t(c0 | (c1 << 3) | (op << 6));
                    Check(AlphaTestPassesFromOne(k, uint32_t(ref), uint32_t(255 - ref)) == Brute(c0, c1, op, ref, 255 - ref, 1, 255), "from one agrees with brute force");
                    ++fromOne;
                }
    {
        FsKey k;
        k.numStages = 1;
        k.stage[0].alpha = Alpha(7, 7, 7, 5);
        k.stage[0].rasChan = 0;
        k.alphaTestOn = 1;
        k.alphaTest = uint8_t(4 | (7 << 3));   // GREATER 0 OR/AND ALWAYS: rejects alpha 0 only
        Check(AlphaTestPassesFromOne(k, 0, 0) && !AlphaTestAlwaysPasses(k, 0, 0), "GREATER 0 passes from 1 on and can fail at 0");
        k.alphaTest = uint8_t(6 | (7 << 3));   // GEQUAL 128 AND ALWAYS: rejects 1..127 as well
        Check(!AlphaTestPassesFromOne(k, 128, 0), "GEQUAL 128 rejects more than alpha 0");
        k.alphaTestOn = 0;
        Check(AlphaTestPassesFromOne(k, 128, 0), "no test passes");
    }
    if (g_failures) { std::fprintf(stderr, "%d failures\n", g_failures); return 1; }
    std::printf("alphatrivial: %d + %d brute-force cases and the targeted ones passed\n", checked, fromOne);
    return 0;
}
