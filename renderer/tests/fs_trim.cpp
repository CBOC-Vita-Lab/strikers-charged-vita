// MSC_VGX_FS_TRIM: the clamps the range analysis leaves out cannot act, and the ordered alpha tests without floor decide as the old text does.
// Random keys and values, run through a model of the generated arithmetic twice, with every clamp and with the analysed ones, in float as the shader does it.
// Run by run_fs_trim_test.sh. `fs_trim mutate` weakens the analysis: the test must then fail (the deliberate break).
#include "../src/vgx_shadergen.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>

using namespace vgx;
static std::mt19937 rng(12345);
static uint32_t R(uint32_t n) { return rng() % n; }
static float Val() {   // the corners as often as the inside
    switch (R(4)) { case 0: return 0.0f; case 1: return 1.0f; default: return static_cast<float>(R(256)) / 255.0f; }
}
static uint32_t ColorReg() {
    uint32_t r = 0;
    r |= R(16) << 12 | R(16) << 8 | R(16) << 4 | R(16);
    r |= R(4) << 16 | R(2) << 18 | R(2) << 19 | R(4) << 20 | R(4) << 22;
    return r;
}
static uint32_t AlphaReg() {
    uint32_t r = 0;
    r |= R(8) << 13 | R(8) << 10 | R(8) << 7 | R(8) << 4;
    r |= R(4) << 16 | R(2) << 18 | R(2) << 19 | R(4) << 20 | R(4) << 22;
    return r;
}
static FsKey Key(bool wide) {
    FsKey k;
    std::memset(static_cast<void*>(&k), 0, sizeof k);
    k.numStages = static_cast<uint8_t>(1 + R(6));
    for (int s = 0; s < k.numStages; ++s) {
        FsKey::Stage& st = k.stage[s];
        st.color = ColorReg(); st.alpha = AlphaReg();
        st.texEnable = static_cast<uint8_t>(R(2)); st.rasChan = static_cast<uint8_t>(R(3) == 0 ? 7 : R(2));
        st.kcsel = static_cast<uint8_t>(R(32)); st.kasel = static_cast<uint8_t>(R(32));
    }
    if (wide) k.pad = FSK_WIDE;
    return k;
}
static inline float Bits(uint32_t v, int p, int n) { return static_cast<float>((v >> p) & ((1u << n) - 1)); }
struct V4 { float c[3], a; };
static float Stage1(float a, float b, float c, float d, uint32_t reg, bool colour, const FsTrimClamp* act, float (*peek)(float)) {
    const uint32_t bias = (reg >> 16) & 3, minus = (reg >> 18) & 1, clampOn = (reg >> 19) & 1, scale = (reg >> 20) & 3;
    float r;
    if (bias == 3) {
        // the compare tests are unknown here: either side of them, chosen at random, is what the range must hold
        r = d + (R(2) ? c : 0.0f);
    } else {
        const float m = a * (1.0f - c) + b * c;
        r = minus ? d - m : d + m;
        if (bias) r += bias == 1 ? 0.5f : -0.5f;
        static const float kS[4] = {1.0f, 2.0f, 4.0f, 0.5f};
        r *= kS[scale];
    }
    const float lo = clampOn ? 0.0f : -4.0157f, hi = clampOn ? 1.0f : 4.0118f;
    (void)colour; (void)peek;
    if (!act || act->hi) r = r > hi ? hi : r;
    if (!act || act->lo) r = r < lo ? lo : r;
    return r;
}
// One evaluation. `seed` makes the two runs see the same values and the same compare outcomes.
static void Eval(const FsKey& k, const FsTrimInfo* ti, uint32_t seed, float out[4]) {
    rng.seed(seed);
    V4 reg[4];
    const bool wide = (k.pad & FSK_WIDE) != 0;
    for (int i = 0; i < 4; ++i) {
        for (int c = 0; c < 3; ++c) reg[i].c[c] = wide ? (static_cast<float>(R(2048)) - 1024.0f) / 255.0f : Val();
        reg[i].a = wide ? (static_cast<float>(R(2048)) - 1024.0f) / 255.0f : Val();
    }
    V4 tex{{1, 1, 1}, 1};
    float kc[3] = {1, 1, 1}, ka = 1;
    for (int s = 0; s < k.numStages; ++s) {
        const FsKey::Stage& st = k.stage[s];
        if (st.texEnable) { for (float& c : tex.c) c = Val(); tex.a = Val(); }
        V4 ras{{0, 0, 0}, 0};
        if (st.rasChan < 2) { for (float& c : ras.c) c = Val(); ras.a = Val(); }
        const uint32_t tc = st.color, ta = st.alpha;
        bool kcu = false, kau = false;
        for (int i = 0; i < 4; ++i) { if (((tc >> (i * 4)) & 15) == 14) kcu = true; if (((ta >> (4 + i * 3)) & 7) == 6) kau = true; }
        if (kcu) {
            if (st.kcsel < 8) kc[0] = kc[1] = kc[2] = (8 - st.kcsel) / 8.0f;
            else if (st.kcsel < 12) kc[0] = kc[1] = kc[2] = 0.0f;
            else { kc[0] = Val(); kc[1] = Val(); kc[2] = Val(); }
        }
        if (kau) ka = st.kasel < 8 ? (8 - st.kasel) / 8.0f : (st.kasel < 16 ? 0.0f : Val());
        auto CA = [&](uint32_t a, int c) -> float {
            a &= 15;
            if (a < 8) return (a & 1) ? reg[a / 2].a : reg[a / 2].c[c];
            switch (a) { case 8: return tex.c[c]; case 9: return tex.a; case 10: return ras.c[c]; case 11: return ras.a; case 12: return 1.0f; case 13: return 0.5f; case 14: return kc[c]; default: return 0.0f; }
        };
        auto AA = [&](uint32_t a) -> float {
            a &= 7;
            if (a < 4) return reg[a].a;
            return a == 4 ? tex.a : (a == 5 ? ras.a : (a == 6 ? ka : 0.0f));
        };
        float nc[3];
        for (int c = 0; c < 3; ++c) nc[c] = Stage1(CA(tc >> 12, c), CA(tc >> 8, c), CA(tc >> 4, c), CA(tc, c), tc, true, ti ? &ti->c[s] : nullptr, nullptr);
        const float na = Stage1(AA(ta >> 13), AA(ta >> 10), AA(ta >> 7), AA(ta >> 4), ta, false, ti ? &ti->a[s] : nullptr, nullptr);
        for (int c = 0; c < 3; ++c) reg[(tc >> 22) & 3].c[c] = nc[c];
        reg[(ta >> 22) & 3].a = na;
    }
    const FsKey::Stage& last = k.stage[k.numStages - 1];
    for (int c = 0; c < 3; ++c) {
        float v = reg[(last.color >> 22) & 3].c[c];
        if (!ti || ti->finC.hi) v = v > 1.0f ? 1.0f : v;
        if (!ti || ti->finC.lo) v = v < 0.0f ? 0.0f : v;
        out[c] = v;
    }
    float v = reg[(last.alpha >> 22) & 3].a;
    if (!ti || ti->finA.hi) v = v > 1.0f ? 1.0f : v;
    if (!ti || ti->finA.lo) v = v < 0.0f ? 0.0f : v;
    out[3] = v;
}
static bool OldCmp(int c, float ai, float ref) {
    switch (c) { case 1: return ai < ref - 0.5f; case 3: return ai < ref + 0.5f; case 4: return ai > ref + 0.5f; case 6: return ai > ref - 0.5f; default: return false; }
}
static bool NewCmp(int c, float ax, float ref) {
    switch (c) { case 1: return ax < ref; case 3: return ax < ref + 1.0f; case 4: return ax >= ref + 1.0f; case 6: return ax >= ref; default: return false; }
}
int main(int argc, char** argv) {
    const bool mutate = argc > 1 && !std::strcmp(argv[1], "mutate");
    uint64_t runs = 0, bad = 0, flips = 0, elided = 0, kept = 0, sides = 0;
    float worst = 0;
    for (int n = 0; n < 200000; ++n) {
        rng.seed(n + 1);
        const bool wide = (n % 4) == 3;
        const FsKey k = Key(wide);
        FsTrimInfo ti;
        FsTrimAnalyze(k, ti);
        if (mutate) for (int s = 0; s < k.numStages; ++s) { ti.c[s].hi = false; ti.a[s].lo = false; }
        for (int s = 0; s < k.numStages; ++s) {
            elided += (!ti.c[s].lo && !ti.c[s].hi) + (!ti.a[s].lo && !ti.a[s].hi);
            sides += (ti.c[s].lo != ti.c[s].hi) + (ti.a[s].lo != ti.a[s].hi);
            kept += (ti.c[s].lo && ti.c[s].hi) + (ti.a[s].lo && ti.a[s].hi);
        }
        for (uint32_t t = 0; t < 8; ++t) {
            float a[4], b[4];
            const uint32_t seed = n * 31u + t + 7u;
            Eval(k, nullptr, seed, a);
            Eval(k, &ti, seed, b);
            ++runs;
            for (int c = 0; c < 4; ++c) {
                const float d = std::fabs(a[c] - b[c]);
                worst = d > worst ? d : worst;
                if (d > 1e-5f) ++bad;
                else if (std::floor(a[c] * 255.0f + 0.5f) != std::floor(b[c] * 255.0f + 0.5f)) ++flips;   // (one float ulp across an 8-bit rounding edge)
            }
        }
    }
    uint64_t ab = 0;
    for (int ref = 0; ref < 256; ++ref)
        for (int i = 0; i <= 25500; ++i) {
            const float a = static_cast<float>(i) / 25500.0f;
            const float ax = a * 255.0f + 0.5f, ai = std::floor(ax);
            for (int c : {1, 3, 4, 6})
                if (OldCmp(c, ai, static_cast<float>(ref)) != NewCmp(c, ax, static_cast<float>(ref))) ++ab;
        }
    std::printf("%llu evaluations: %llu differ, worst difference %g, 8-bit rounding flips %llu; clamps gone %llu, one side %llu, kept %llu; alpha tests differing %llu\n", static_cast<unsigned long long>(runs), static_cast<unsigned long long>(bad), worst, static_cast<unsigned long long>(flips),
                static_cast<unsigned long long>(elided), static_cast<unsigned long long>(sides), static_cast<unsigned long long>(kept), static_cast<unsigned long long>(ab));
    // The final clamp (colour and alpha) is in every trimmed text (the Galaxy port's console: static on shadows and decals where it was dropped). Every key above, both forms
    // of precision; built with -DVGX_FS_TRIM_FINAL_BREAK (run_fs_trim_test.sh BREAK=1) the generator drops it and this must fail.
    uint64_t texts = 0, noFinal = 0;
    for (int n = 0; n < 20000 && !mutate; ++n) {
        rng.seed(n + 1);
        FsKey k = Key((n % 4) == 3);
        for (const uint8_t prec : {static_cast<uint8_t>(FSP_TRIM), static_cast<uint8_t>(FSP_TRIM | FSP_HALF)}) {
            k.prec = prec;
            ShaderSource src;
            GenerateFragmentShader(k, src);
            ++texts;
            if (src.text.find("vec4 fin = clamp(") == std::string::npos && src.text.find("vec4 fin = vec4(clamp(") == std::string::npos) ++noFinal;
        }
    }
    if (!mutate) std::printf("%llu trimmed texts: %llu without the final clamp\n", static_cast<unsigned long long>(texts), static_cast<unsigned long long>(noFinal));
    if (noFinal) { std::printf("FAILED: a trimmed text lost its final clamp\n"); return 1; }
    if (mutate) { if (bad) { std::printf("the mutated analysis is caught\n"); return 0; } std::printf("FAILED: the mutated analysis passed\n"); return 1; }
    return bad || ab ? 1 : 0;
}
