// MSC_VGX_DISCARD_BLEND (Galaxy r99 GXV_DISCARD_BLEND): a draw whose alpha test fails writes vec4(0.0) instead of being discarded, only where its blend makes that a no-op.
// 1. The blend side (DiscardBlendKeeps): every GX blend-mode / z-mode / destination-alpha combination is drawn the way BuildRenderState maps it (a copy of that mapping
//    below), in 8-bit fixed point with every destination value of a grid: a source of (0, 0, 0, 0) must leave each one as it is, and no depth may be written.
// 2. The text: for random alpha-tested keys, the key with FSK_NODISCARD has no discard, keeps the same test expression as the discarding text, and writes vec4(0.0)
//    when the test fails; the text without the bit is the discarding one. Built with -DVGX_DISCARD_BLEND_BREAK (run_discard_blend_test.sh BREAK=1) the test is lost
//    and this must fail.
#include "../src/vgx_shadergen.h"
#include <cstdio>
#include <cstring>
#include <random>
#include <string>

using namespace vgx;

namespace {
enum { Z, ONE, SC, ISC, SA, ISA, DA, IDA, DC, IDC };
// BuildRenderState's mapping (vgx_draw.cpp MapSrcFactor / MapDstFactor and the subtract / logic-op branches)
int SrcF(uint32_t f, bool a) { static const int t[8] = {Z, ONE, DC, IDC, SA, ISA, DA, IDA}; return f == 6 && !a ? ONE : f == 7 && !a ? Z : t[f]; }
int DstF(uint32_t f, bool a) { static const int t[8] = {Z, ONE, SC, ISC, SA, ISA, DA, IDA}; return f == 6 && !a ? ONE : f == 7 && !a ? Z : t[f]; }
// one channel c of a factor, 8-bit values (0..255) for source s (rgba) and destination d (rgba)
int Fac(int f, int c, const int* s, const int* d) {
    switch (f) {
    case Z: return 0; case ONE: return 255;
    case SC: return s[c]; case ISC: return 255 - s[c];
    case SA: return s[3]; case ISA: return 255 - s[3];
    case DA: return d[3]; case IDA: return 255 - d[3];
    case DC: return d[c]; default: return 255 - d[c];
    }
}
int Mul(int a, int b) { return (a * b + 127) / 255; }
// What the target holds after the draw (blend == false: the source is written as it is)
void Draw(uint32_t cm0, bool hasAlpha, const int* s, const int* d, int* out) {
    bool blend = false; int sf = ONE, df = Z; bool revsub = false;
    if ((cm0 >> 11) & 1u) { blend = true; revsub = true; sf = ONE; df = ONE; }
    else if (cm0 & 1u) { blend = true; sf = SrcF((cm0 >> 8) & 7u, hasAlpha); df = DstF((cm0 >> 5) & 7u, hasAlpha); if (sf == ONE && df == Z) blend = false; }
    else if ((cm0 >> 1) & 1u) {
        static const int lo[16][2] = {{Z, Z}, {DC, Z}, {ONE, ISC}, {ONE, Z}, {DC, ONE}, {Z, ONE}, {IDC, ISC}, {IDC, ONE}, {ISC, IDC}, {ISC, SC}, {IDC, IDC}, {ONE, IDA}, {ISC, ISC}, {ISC, ONE}, {IDC, ISC}, {ONE, ONE}};
        const uint32_t op = (cm0 >> 12) & 15u;
        if (op != 3) { blend = true; sf = lo[op][0]; df = lo[op][1]; }
    }
    for (int c = 0; c < 4; ++c) {
        if (!blend) { out[c] = s[c]; continue; }
        const int a = Mul(s[c], Fac(sf, c, s, d)), b = Mul(d[c], Fac(df, c, s, d));
        int v = revsub ? b - a : a + b;
        out[c] = v < 0 ? 0 : v > 255 ? 255 : v;
    }
}
std::mt19937 rng(7);
uint32_t R(uint32_t n) { return rng() % n; }
} // namespace

int main() {
    // 1. the blend side
    uint64_t keeps = 0, combos = 0, bad = 0, missed = 0;
    const int zero[4] = {0, 0, 0, 0};
    for (uint32_t cm0 = 0; cm0 < (1u << 16); ++cm0) {
        if (cm0 & 0x18u) continue;   // (bits 3 and 4 are the colour / alpha update: not the blend)
        for (uint32_t zm = 0; zm < 32; zm += 1)
            for (uint32_t da = 0; da < 2; ++da)
                for (int hasAlpha = 0; hasAlpha < 2; ++hasAlpha) {
                    ++combos;
                    const bool k = DiscardBlendKeeps(cm0, zm, da << 8, hasAlpha != 0);
                    const bool zWrite = (zm & 1u) && (zm & 0x10u);
                    bool same = !zWrite && !da;
                    for (int dv = 0; dv < 256 && same; dv += 15)
                        for (int av = 0; av < 256 && same; av += 51) {
                            const int d[4] = {dv, 255 - dv, (dv * 7) & 255, av};
                            int out[4];
                            Draw(cm0, hasAlpha != 0, zero, d, out);
                            for (int c = 0; c < 4; ++c) if (out[c] != d[c]) same = false;
                        }
                    if (k) { ++keeps; if (!same) ++bad; }
                    else if (same && (cm0 & 1u) && !((cm0 >> 1) & 1u)) ++missed;   // (exact but not taken: allowed, counted)
                }
    }
    std::printf("blend: %llu combinations, %llu taken, %llu of them change the target (must be 0), %llu exact ones not taken\n", static_cast<unsigned long long>(combos),
                static_cast<unsigned long long>(keeps), static_cast<unsigned long long>(bad), static_cast<unsigned long long>(missed));
    // 2. the text
    uint64_t texts = 0, textBad = 0;
    for (int n = 0; n < 20000; ++n) {
        rng.seed(n + 1);
        FsKey k;
        std::memset(static_cast<void*>(&k), 0, sizeof k);
        k.numStages = static_cast<uint8_t>(1 + R(4));
        for (int s = 0; s < k.numStages; ++s) {
            k.stage[s].color = R(1u << 24); k.stage[s].alpha = R(1u << 24) & 0xFFFFF0u;
            k.stage[s].texEnable = static_cast<uint8_t>(R(2)); k.stage[s].rasChan = static_cast<uint8_t>(R(3) == 0 ? 7 : R(2));
        }
        k.alphaTestOn = 1;
        k.alphaTest = static_cast<uint8_t>(R(256));
        k.fog = static_cast<uint8_t>(R(3) == 0 ? 1 + R(7) : 0);
        static const uint8_t kPrec[4] = {0, FSP_TRIM, FSP_TRIM | FSP_HALF, FSP_TEX2};
        k.prec = kPrec[R(4)];
        ShaderSource a, b;
        GenerateFragmentShader(k, a);
        k.pad |= FSK_NODISCARD;
        GenerateFragmentShader(k, b);
        ++texts;
        const size_t p = a.text.find("  if (!("), q = a.text.find(")) discard;\n", p);
        const std::string expr = (p != std::string::npos && q != std::string::npos) ? a.text.substr(p + 8, q - p - 8) : std::string("?");
        const bool ok = expr != "?" && b.text.find("discard") == std::string::npos && b.text.find("  bool vgx_ok = " + expr + ";\n") != std::string::npos &&
                        b.text.find("  gl_FragColor = vgx_ok ? vec4(fin) : vec4(0.0);\n") != std::string::npos && a.text.find("vgx_ok") == std::string::npos;
        // everything before the test is the same text
        const bool prefix = b.text.compare(0, p, a.text, 0, p) == 0;
        if (!ok || !prefix) {
            if (++textBad <= 3) std::printf("text MISMATCH at key %d:\n--- discarding\n%s--- without discard\n%s", n, a.text.c_str(), b.text.c_str());
        }
    }
    std::printf("text: %llu keys, %llu wrong\n", static_cast<unsigned long long>(texts), static_cast<unsigned long long>(textBad));
    if (bad || textBad || !keeps) { std::printf("discard_blend: FAILED\n"); return 1; }
    return 0;
}
