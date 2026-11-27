// MSC_VGX_FOGNOOP (nxgpu25): the decision of vgx_fognoop.h against the fog code the shader generator emits (vgx_shadergen.cpp "if (k.fog)"), evaluated in IEEE single precision.
// For every (type, ortho, A, B, C, z, colour) drawn from a seeded sweep that includes the edges: when Inert() says yes, the fogged colour must be bit-identical to the fog-free
// colour; when it says no it must not be claimed for a case that changes the colour (a count of cases where Inert() refused but the colour did not change is only reported).
// "mutate N" breaks the decision on purpose (each must fail): 1 types 6 and 7 count as inert, 2 a negative C counts as inert, 3 any A below 2^-100 counts as zero (a denormal or tiny A),
// 4 ortho draws are left to the perspective rule only (A' * z read as a quotient), 5 NaN C counts as inert (4 is not used).
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "vgx_fognoop.h"
using namespace vgx;
static int g_mut = 0;
static bool Inert(uint32_t ee, uint32_t f1) {
    const uint32_t fsel = (f1 >> 21) & 7u;
    const float c = fognoop::FogFloat(f1);
    if (g_mut == 0) return fognoop::Inert(ee, f1);
    if (fsel == 0) return false;
    if (g_mut != 1 && fsel >= 6) return false;
    if (g_mut == 3) { float a = fognoop::FogFloat(ee); if (std::fabs(a) >= 0x1p-100f) return false; }
    else if ((ee & 0x7FFFFu) != 0) return false;
    if (g_mut == 2) return true;
    if (g_mut == 5) return !(c < 0.0f);
    return c >= 0.0f;
}
// the generated fragment code, one pixel: returns the colour after the fog (rgb channel c, fog colour fc, depth z)
static float Fogged(uint32_t ee, uint32_t eF, uint32_t f0, uint32_t f1, float z, float c, float fc, float bShiftScale) {
    const uint32_t fsel = (f1 >> 21) & 7u, ortho = (f1 >> 20) & 1u;
    const float a = fognoop::FogFloat(ee);
    const float sh = std::ldexp(1.0f, static_cast<int>(f0 & 31u));
    float x, y, zc = fognoop::FogFloat(f1);
    if (ortho) { x = a; y = 0; }
    else { x = a * sh; y = static_cast<float>(eF) * sh / 16777216.0f * bShiftScale; }
    float fg = ortho ? x * z : x / std::fmax(y - z, 0.000001f);
    if (std::isnan(fg - zc)) return std::nanf("");   // clamp() of a NaN is undefined in GLSL: modelled as a NaN colour (never equal)
    fg = std::fmin(std::fmax(fg - zc, 0.0f), 1.0f);
    switch (fsel) {
    case 4: fg = 1.0f - std::exp2(-8.0f * fg); break;
    case 5: fg = 1.0f - std::exp2(-8.0f * fg * fg); break;
    case 6: fg = std::exp2(-8.0f * (1.0f - fg)); break;
    case 7: fg = 1.0f - fg; fg = std::exp2(-8.0f * fg * fg); break;
    default: break;
    }
    return c * (1.0f - fg) + fc * fg;   // mix(): x * (1 - a) + y * a
}
static uint64_t g_s = 0x9E3779B97F4A7C15ull;
static uint32_t R() { g_s ^= g_s << 13; g_s ^= g_s >> 7; g_s ^= g_s << 17; return static_cast<uint32_t>(g_s >> 16); }
int main(int argc, char** argv) {
    if (argc > 2 && std::string(argv[1]) == "mutate") g_mut = std::atoi(argv[2]);
    long inert = 0, refusedSame = 0, bad = 0, total = 0, dropped[8] = {};
    const uint32_t aSet[] = {0x00000, 0x80000, 0x00001, 0x7FF80000u & 0xFFFFF, 0x00800, 0x3F000, 0x40000, 0x12345, 0xC0000, 0x00400};
    for (int it = 0; it < 400000; ++it) {
        const uint32_t ee = (it % 3 == 0) ? aSet[R() % 10] : ((R() & 3) ? 0u : (R() & 0xFFFFFu));
        uint32_t cbits = R() & 0xFFFFFu;
        if (it % 4 == 0) cbits = (cbits & 0x80000u) | (cbits & 0x7FFu) | ((R() % 3 == 0 ? 0xFFu : (R() % 2 ? 0x7Fu : 0u)) << 11); // zero, +-1.x, inf / NaN exponents
        const uint32_t f1 = cbits | ((R() & 1u) << 20) | ((R() % 8u) << 21);
        if (((f1 >> 21) & 7u) == 0) continue;
        const uint32_t eF = R() & 0xFFFFFFu, f0 = R() & 31u;
        const float zs[] = {0.0f, 1.0f, 0.5f, static_cast<float>(R() % 65536) / 65535.0f, 0x1p-24f, 0.99999994f};
        for (float z : zs) {
            const float c = static_cast<float>(R() % 256) / 255.0f, fc = static_cast<float>(R() % 256) / 255.0f;
            const float want = c; // the fog-free colour
            const float got = Fogged(ee, eF, f0, f1, z, c, fc, 1.0f);
            const bool inertSays = Inert(ee, f1);
            ++total;
            uint32_t a, b; std::memcpy(&a, &got, 4); std::memcpy(&b, &want, 4);
            if (inertSays) { ++inert; ++dropped[(f1 >> 21) & 7u]; if (a != b) ++bad; }
            else if (a == b) ++refusedSame;
        }
    }
    std::printf("fognoop: %ld cases, %ld called inert (by type 1..5: %ld %ld %ld %ld %ld), %ld refused although the colour did not change (allowed: the rule is sufficient, not necessary), %ld WRONG\n",
                total, inert, dropped[1], dropped[2], dropped[3], dropped[4], dropped[5], refusedSame, bad);
    if (g_mut == 0 && inert < 1000) { std::printf("fognoop: the sweep proved nothing\n"); return 1; }
    return bad ? 1 : 0;
}
