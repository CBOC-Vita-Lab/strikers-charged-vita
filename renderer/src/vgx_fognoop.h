// vgx_fognoop.h: MSC_VGX_FOGNOOP (nxgpu25): when the fog registers make the fog a no-op, the fragment key has no fog (the shader has no fog code and its TEV block is
// two vectors shorter). Header only, so the host test (tests/fognoop.cpp) runs the very decision the front end uses.
//
// The generated fragment code (vgx_shadergen.cpp, "if (k.fog)") is
//     perspective:  fg = A' / max(B' - z, 1e-6)        ortho:  fg = A' * z            (A' = a * 2^shift, the TEV block's fog vector .x)
//     fg = clamp(fg - C, 0.0, 1.0)                                                    (C = FogFloat(BP 0xF1), the block's .z)
//     type 4: fg = 1 - exp2(-8 fg)   type 5: fg = 1 - exp2(-8 fg fg)   type 6: fg = exp2(-8 (1 - fg))   type 7: fg = 1 - fg; fg = exp2(-8 fg fg)   (2, and the
//     reserved 1 and 3: fg stays)
//     fin.rgb = mix(fin.rgb, fogColour, fg)
// With A = +0 or -0 (BP 0xEE: sign bit 19, exponent bits 11..18, mantissa bits 0..10; exponent and mantissa 0), A' = a * 2^shift is a zero too, and the perspective quotient is
// 0 / (a divisor of at least 1e-6) = 0 (no NaN, no infinity: z is a finite depth), the ortho product 0 * z = 0. Then fg = clamp(0 - C, 0, 1) = 0 for every C >= 0 (C = +inf too;
// a NaN C fails the test below, as it would make the clamp's argument NaN). Types 2, 4, 5 map fg = 0 to 0 (1 - exp2(-0) = 1 - 1 = 0), and mix(c, f, 0) = c * (1 - 0) + f * 0 = c
// bit for bit (a finite fog colour: it is an 8-bit register). Types 6 and 7 map 0 to exp2(-8) = 1/256: NOT inert, they keep their fog.
#ifndef VGX_FOGNOOP_H
#define VGX_FOGNOOP_H
#include <cstdint>
#include <cstring>
namespace vgx {
namespace fognoop {
inline float FogFloat(uint32_t r) { // sign(1) exp(8) mantissa(11) packed in the low 20 bits (GXSetFog's A and C)
    const uint32_t u = (((r >> 19) & 1u) << 31) | (((r >> 11) & 0xFFu) << 23) | ((r & 0x7FFu) << 12);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}
// bpEE: BP 0xEE (A); bpF1: BP 0xF1 (C, bit 20 ortho, bits 21..23 the type).
inline bool Inert(uint32_t bpEE, uint32_t bpF1) {
    const uint32_t fsel = (bpF1 >> 21) & 7u;
    if (fsel == 0 || fsel >= 6) return false;   // no fog at all (nothing to drop), or a curve that does not map 0 to 0
    if ((bpEE & 0x7FFFFu) != 0) return false;   // A is not +-0 (a denormal A is not zero either)
    return FogFloat(bpF1) >= 0.0f;              // false for a NaN
}
}  // namespace fognoop
}  // namespace vgx
#endif
