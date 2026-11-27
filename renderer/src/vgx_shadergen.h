// Structural shader keys and GLSL ES 1.00 generation. Front-end side: no GL.
#pragma once
#include "vgx_cmd.h"

namespace vgx {

// Everything that changes the *code* of a shader goes into a key; everything
// that is only a value (colours, matrices, references, fog parameters) is a
// uniform. Keys are plain zero-initialised structs compared with memcmp.
enum : uint8_t { FOG_LINEAR = 16 };   // FsKey::fog bit
enum : uint8_t { VSK_FOGZ = 1 };      // VsKey::tg[7].pad[1]: the vertex shader writes the eye-space depth to v_fz (a key bit like the VS_TRIM ones: set from the registers, so the key says what the text is)
struct FsKey {
    uint8_t numStages = 1;    // 1..16
    uint8_t numTexGens = 0;   // texcoord varyings declared
    uint8_t texProjMask = 0;  // texcoord k is projective (divide by q)
    uint8_t alphaTestOn = 0;
    uint8_t alphaTest = 0;    // comp0 | comp1 << 3 | op << 6
    uint8_t fog = 0;          // fsel | ortho << 3 | FOG_LINEAR (0 = off). FOG_LINEAR (MSC_VGX_FOGVTX): the factor before the clamp is fa * eye depth + fb, from a varying (the vertex key has VSK_FOGZ)
    uint8_t dstAlpha = 0;     // write the constant destination alpha
    uint8_t numIndStages = 0;
    uint8_t ztex = 0;         // op << 2 | fmt (0 = off)
    uint8_t zfreeze = 0;      // GX Z-freeze (see vgx_zfreeze.h): the depth comes from a reference plane in u_tev (MSC_VGX_ZFREEZE, default off)
    uint8_t prec = 0;         // FSP_* (1.3): the text's precision and varying forms. 0 in everything that is built, looked up or kept in a list: only the generator's copy has it (ShaderSwitchKey)
    uint8_t pad = 0;
    struct Stage {
        uint32_t color = 0; // BP 0xC0+2n bits 0..23
        uint32_t alpha = 0; // BP 0xC1+2n bits 4..23 (swap selects stripped)
        uint32_t ind = 0;   // BP 0x10+n (0 when the stage is direct)
        uint8_t texEnable = 0, texMap = 0, texCoord = 0, rasChan = 7; // 0, 1, 5/6 (bump alpha), 7 = zero
        uint8_t kcsel = 0, kasel = 0;
        uint8_t rasSwap = 0xE4, texSwap = 0xE4; // 2 bits per output component, r in bits 0..1
        uint8_t pad[4] = {};
    } stage[16];
    struct Ind {
        uint8_t texMap = 0, texCoord = 0, pad[2] = {};
    } ind[4];
};

struct VsKey {
    uint8_t numTexGens = 0;
    uint8_t numChans = 0;   // 0..2 lit colour channels produced
    uint8_t perVtxPos = 0;  // position/normal matrix row comes from a_pos.w (2: no row above 27 is used, the palette is the short one)
    uint8_t palRows = 0;    // perVtxPos 2 only: position rows of the palette (the normal rows follow); 0 = kPalRowsShort / 2 (what lists written before the buckets hold)
    // Per channel control: [0] colour0, [1] colour1, [2] alpha0, [3] alpha1.
    struct Chan {
        uint8_t enable = 0, matSrcVtx = 0, ambSrcVtx = 0;
        uint8_t diffuse = 0; // 0 none, 1 sign, 2 clamp
        uint8_t attn = 0;    // 0 none, 1 spec, 2 spot
        uint8_t lightMask = 0;
        uint8_t skin = 0;    // W5 SKIN, chan[0] only: the draw is skinned on the GPU (VL_SKIN attributes, u_bone; vgx_ext.h). In a byte that was padding:
                             // every key of the older lists and seeds stays what it was.
        uint8_t pad = 0;     // chan[0]: VSP_* (1.3), as FsKey::prec; a byte that was padding, as `skin`
    } chan[4];
    struct TexGen {
        uint8_t type = 0;      // 0 regular, 1 emboss, 2 colour0, 3 colour1
        uint8_t src = 5;       // XF source row: 0 pos, 1 nrm, 2 colours, 3 binrm, 4 tangent, 5.. tex0..
        uint8_t proj = 0;      // 1 = STQ (3x4 matrix, projective)
        uint8_t inputAbc1 = 0; // input form (a,b,c,1) instead of (a,b,1,1)
        uint8_t post = 0;      // 0 none, 1 post matrix, 2 normalise + post matrix
        uint8_t perVtxMtx = 0; // matrix row from the per-vertex TEXnMTXIDX
        uint8_t pad[2] = {};
    } tg[8];
};

// 1.3: forms of the text that two test switches choose. They live in the keys' former padding and are set on the generator's copy of a key only (ShaderSwitchKey), so:
//  * a key that is built, looked up, or read from or written to a warm-up list has them 0: lists, tools/seed and the registry ids do not depend on the switches;
//  * with the switches off every key, text and hash is what it was, and the package's pack and the console's cache stay valid (no kShaderGenVersion bump);
//  * with one on the text differs, and vitaGL's caches are keyed by a hash of the text, so a cached or packed program of the other form can never be taken for it.
enum : uint8_t {
    FSP_HALF = 1,    // MSC_VGX_FS_HALF=1: the TEV colour arithmetic of this shader is in half (only a shader FsHalfEligible admits)
    FSP_HALFVAR = 2, // MSC_VGX_FS_HALF=2: as well, the colour varyings are declared half4
    FSP_TEX2 = 4,    // MSC_VGX_TEX2D=1: a texture coordinate without the projective bit is a vec2 varying
    FSP_TRIM = 8,    // MSC_VGX_FS_TRIM=1: no clamp the ranges prove idle, no floor in an ordered alpha test
    FSP_CANON = 16,  // MSC_VGX_TEV_CANON=1: the TEV chain in its simplest exact form (see TevCanonMode); implies the FS_TRIM analysis
    FSP_CANON2 = 32, // MSC_VGX_TEV_CANON=2: as well the forms that may move a colour by one level
    FSP_NOFC3 = 128, // MSC_VGX_FINCLAMP=3: per part, only the side of the output clamp the ranges leave open (max, min, clamp or nothing), every draw
    FSP_NOFC = 64,   // MSC_VGX_FINCLAMP=2: no final clamp of the output where the ranges prove it idle, for every draw
};
enum : uint8_t { VSP_TEX2 = 1 }; // MSC_VGX_TEX2D=1: the same in the vertex shader (chan[0].pad)
int FsHalfMode();  // MSC_VGX_FS_HALF: 0 off (default), 1 arithmetic, 2 arithmetic and colour varyings
bool Tex2dOn();    // MSC_VGX_TEX2D
// MSC_VGX_FS_TRIM. The Vita's compiler keeps every clamp of the text, each a VMIN and a VMAX: about a fifth of a typical shader. FsTrimAnalyze follows the range of every TEV value through
// a key and says which side of which clamp can act: texels, konst colours, vertex colours and the registers are 0..1, unless FSK_WIDE says a register holds a value GX's 8-bit calls
// cannot give (GXSetTevColorS10, the movie shader); the key's own bit, because the text depends on it. Bounds are real numbers: float rounding may leave a value an ulp past a bound.
enum : uint8_t { FSK_WIDE = 1, FSK_NODISCARD = 2, FSK_NOFC = 4 };   // FsKey::pad; only with the switch on, so with it off every key is what it was
// MSC_VGX_DISCARD_BLEND (Galaxy r99 GXV_DISCARD_BLEND; default 0): FSK_NODISCARD on an alpha-tested key whose draw is blended so that a source of (0, 0, 0, 0) leaves
// the target as it is (reverse subtract, or a destination factor of 1, 1 - source alpha or 1 - source colour after the pixel format's mapping), writes no depth (no z
// update, or no z compare: GX writes no depth then) and no constant destination alpha. Its text has no `discard`: a fragment that fails the test writes vec4(0.0), which
// that blend makes a no-op, colour and alpha. The tile GPU keeps its hidden-surface removal for the draw (a shader with discard is a punch-through object on the SGX).
// Exact by the blend's algebra (0 * s + d * 1 = d). The test's text and result are unchanged; only what a failed test does. Set in BuildFsKey, so the key (and the text)
// differs from the discarding draw's: the shader pack has to be seeded with the switch on before it can be a default.
bool DiscardBlendOn();
// The draw's side of it, from GX registers: blend mode (BP 0x41), z mode (0x40), destination alpha (0x42), and whether the pixel format has alpha (EfbHasAlpha:
// a DSTALPHA factor is then 1). Mirrors BuildRenderState's mapping: subtract is dst - src with factors 1, 1; blend source 1 / destination 0 is drawn unblended.
inline bool DiscardBlendKeeps(uint32_t cm0, uint32_t zm, uint32_t da, bool hasAlpha) {
    if ((zm & 1u) && (zm & 0x10u)) return false;   // depth is written
    if ((da >> 8) & 1u) return false;              // a constant destination alpha
    if ((cm0 >> 11) & 1u) return true;             // subtract: dst - 0
    if (!(cm0 & 1u)) return false;                 // no blend (a logic op, or none)
    const uint32_t sf = (cm0 >> 8) & 7u, df = (cm0 >> 5) & 7u;
    if (sf == 1u && df == 0u) return false;        // (drawn unblended)
    return df == 1u || df == 3u || df == 5u || (df == 6u && !hasAlpha);
}
bool FsTrimOn();
// MSC_VGX_TEV_CANON (default 0): 0 off, 1 exact, 2 at most one colour level. The generator's TEV chain is written from the ranges the key proves: an operand whose range is one
// number is that number (it folds through the stages that read it), mix() of a zero or equal operand is a product or the operand, an add of zero or a scale of one is nothing, a
// stage result no later stage and not the output reads is not computed, and every stage clamp the ranges prove idle goes (what FS_TRIM does; the canon form includes it). The final
// clamp of the output stays. Mode 2 adds the forms that are not the same real number (see GenerateFragmentShader). Only the text changes: keys, uniforms and draws are as they were.
int TevCanonMode();
// MSC_VGX_FINCLAMP (default 0): the output clamp `fin = clamp(...)` that every text keeps (2 of the 7 instructions of the hottest shader, 2 of 11 of the next) is dropped where the ranges
// prove the value inside 0..1 (colour and alpha apart; the analysis of FS_TRIM / TEV_CANON, so one of them must be on). 1: only for a draw that is not blended (key bit FSK_NOFC from
// BuildFsKey; the output conversion saturates an opaque write, whereas a blended write goes through the blend unclamped); 2: every draw (the Galaxy port's console drew blended shadows
// and decals as static with that: a console A/B decides). Changes shader text: new keys, pack 15 misses.
int FinClampMode();
// MSC_VGX_LEANVARY (default 0): the vertex shader writes only what the fragment shader declares: colour 0 as well (VS_TRIM does colour 1; the texture varyings and the fog depth are
// already lean). VsKey::tg[7].pad[0] bit 2. Changes the text of the vertex shaders of draws whose TEV stages read no vertex colour 0: new keys, pack 15 misses.
bool LeanVaryOn();
// MSC_VGX_VS_TRIM. Facts about the light values at the draw, which the generator turns into shorter text (lighting is what makes a vertex shader long: the game's lights are mostly
// directional, with attenuation terms of exactly 1). They live in padding of VsKey and are 0 with the switch off. Unlike the forms above they are part of the key, as the text depends on the state:
//   chan[1].pad  bit l: light l's cos attenuation is exactly (1, 0, 0), so the spot term is 1
//   chan[2].pad  bit l: light l's distance attenuation is exactly (1, 0, 0), so the divisor is 1
//   chan[3].pad  bit l (mode 2 only, NOT exact): as both above, and the light is so far (|pos| >= 1e5) that its direction is taken as the same at every vertex
//   tg[7].pad[0] 1: the fragment shader of the draw does not read colour 1, so the vertex shader does not write it
int VsTrimMode();   // 0 off (default), 1 exact, 2 far lights too
// MSC_VGX_VS_NOBRANCH (default 0): the vertex shaders without data-dependent `if`: the normalise guards (`if (nl > 0.0) n = n / nl;`, the light direction's and the texgen's) divide by
// `l + step(l, 0.0)` (exactly l for l > 0, exactly 1 for a zero length: the same value as the branch for every non-NaN input), the palette row clamps (`if (pi > 61) pi = 61;`, the
// short palette's, the texture-matrix row's) are a min() on the whole-numbered float before the integer cast, `pi > 29 ? 93 : 64 + pi` is 64 + min(pi, 29), and the spot gate
// `dot(n, ld) >= 0.0 ? x : 0.0` is a step() product. Only the text changes (a pack miss for each changed program); proof: tests/vs_nobranch_gl.cpp (Mesa, bit patterns).
bool VsNoBranchOn();
// MSC_VGX_VS_NOBRANCH_LIGHT (default 0, needs VS_NOBRANCH): the per-light part of it, the light direction's normalise guard and the spot gate. Off by default: with it a vertex program of
// several lights (5 light blocks: Vita3K's shader 4efa3e4a) comes out of Sony's compiler with 85 temporary registers (the same programs with the branches: at most 52 over the whole pack list),
// which Vita3K cannot translate (its r[] holds 80: "array index must be < 20", the Dump gate failed at boot) and which costs a real SGX its thread occupancy as well.
bool VsNoBranchLightOn();
// MSC_VGX_FS_PROJ (default 0): a projective texture coordinate that only plain reads use is read with texture2DProj instead of a per-pixel divide into a vec2. 1 keeps the z == 0
// rule (z + step(|z|, 0) is 1 there), 2 passes the varying as it is. Exact up to the rounding of the divide (host: tests/fs_proj_gl.cpp). Changes the text of the fragment programs with a projective coordinate.
int FsProjMode();
struct FsTrimClamp { bool lo = true, hi = true; };   // the side acts somewhere in the key's ranges
struct FsTrimInfo { FsTrimClamp c[16], a[16], finC, finA; };
void FsTrimAnalyze(const FsKey& k, FsTrimInfo& out);
// What the generator is given for a key: a copy with the bits above, as the switches of this run say. A pure function of the key once the switches are read.
FsKey ShaderSwitchKey(const FsKey& k);
VsKey ShaderSwitchKey(const VsKey& k);
// The worst error half arithmetic can add to this key's final colour or alpha, in units of 1 (one 8-bit step is 1 / 255), for TEV registers within regLo..regHi, or a negative
// number when the key has what half must not touch: indirect texturing (texel coordinates, offsets up to 255) and compare stages (they test a difference of a quarter
// step, and weigh colours by up to 16711680, above half's range). It bounds every rounding of half's 11 significant bits, in either shape of mix.
float FsHalfBound(const FsKey& k, float regLo, float regHi);
// An admitted key: at most one 8-bit step of difference with registers in 0..1 (what materials hold; the same limit Galaxy's GXV_FS_HALF notes state), at most four at the registers' full range.
bool FsHalfEligible(const FsKey& k);
uint64_t HashKey(const void* key, size_t size);
// Bump when the key structs or what is generated from them change: shader
// warm-up lists written by an older generator are then discarded.
constexpr uint32_t kShaderGenVersion = 2;
// Uniform arrays are declared (and their blocks built) only as long as the shader needs
// them: u_tev[9 / 11 / 28], u_lt[4 + 5 per light], u_pal[60 / 96], u_tm per texgen. On the
// console every draw copies the program's whole uniform buffers once more.
//   MSC_VGX_UNIPACK=0   the old fixed sizes (what the reference frame hashes were made with)
bool UniformPacking();
// Sizes in vec4 of what a shader with this key declares.
int FsTevCount(const FsKey& k);  // u_tev
int FsZFreezeVec(const FsKey& k); // index (vec4) of the Z-freeze plane in u_tev (a, b, c in EFB pixels; the back end adapts it to the target)
int VsLightCount(const VsKey& k); // u_lt
constexpr int kPalRowsFull = 96, kPalRowsShort = 60, kPalShortMaxRow = 27;
// Palette buckets (W5 LOOK2, MSC_VGX_PALBUCKET): the short palette of a draw whose vertices use position matrix rows up to R holds P >= R + 3 position rows and as
// many normal rows; P = 30 is the whole short palette (kPalRowsShort rows) and the only size before the buckets. The console copies a program's whole uniform
// buffer per draw, so a skinned mesh of a few bones is cheaper in a smaller one.
constexpr int kPalShortPos = kPalRowsShort / 2;
inline int PalPosRows(const VsKey& k) { return k.perVtxPos == 2 ? (k.palRows ? k.palRows : kPalShortPos) : 0; }
// ---- shader economy (W12 SHADER)
//   MSC_VGX_ATRIVIAL=0  alpha tests that provably cannot fail are kept (the old text, with its discard)
// A draw whose alpha test passes for every possible alpha emits no discard, and its key is the key of the draw without a test (one program,
// not two). The reference values are part of the same BP register as the compare codes, so nothing new has to be tracked.
bool AlphaTrivialOn();
// True when `if (!(cmp0 op cmp1)) discard;` cannot discard for any alpha the shader can produce: ref0 / ref1 are the 8-bit references
// (BP 0xF3 bits 0..7 and 8..15). Looks at the compare codes and references, and at the TEV alpha chain where it provably ends in a constant.
bool AlphaTestAlwaysPasses(const FsKey& k, uint32_t ref0, uint32_t ref1);
// W12 CROWD (MSC_VGX_BLENDNODISCARD, default off): a draw that is blended so that a fragment of alpha 0 changes nothing (source alpha times the colour, destination
// kept), writes no depth and no destination alpha pass, and whose alpha test fails for alpha 0 only, is made without the test: its key is the key of a draw without one
// (no discard). The only difference is a fragment whose alpha is below half a step (it rounds to 0 in the test, 0.4% at most in the blend): sub-level.
bool BlendNoDiscardOn();
// True when the alpha test (FsKey::alphaTest codes) with these references passes for every 8-bit alpha from 1 to 255.
bool AlphaTestPassesFromOne(const FsKey& k, uint32_t ref0, uint32_t ref1);
// MSC_VGX_HSR_PROVEN (default 0): hidden-surface removal from the draw's own numbers. The tile GPU shades an opaque fragment only if nothing opaque covers it, but a draw with a blend is
// shaded in full. A draw blended by its source alpha (factors SRC_ALPHA / ONE against INV_SRC_ALPHA / ZERO, add) whose final alpha is PROVEN 1 for the values its u_tev block holds
// (AlphaRangeDyn: interval arithmetic over the stages, konst colours and registers as they are, texels and vertex colours 0..1) is source 1, destination 0: drawn without the blend.
// Exact (x * 1 + d * 0 = x). Changes no shader text, only the draw's blend function (the program variant).
bool HsrProvenOn();
// MSC_VGX_HSR_FACTS (default 0): what the draw's other numbers add: a texture whose texels are all opaque (texAOne: bit per texture map) has alpha 1, and a vertex colour made from the
// material and ambient registers (no per-vertex source) is the number the light block holds (rasA[chan]: lo, hi; chan 0 colour 0, 1 colour 1; lo > hi: unknown).
struct HsrFacts { uint8_t texAOne = 0; float rasA[2][2] = {{1.0f, 0.0f}, {1.0f, 0.0f}}; };
bool HsrFactsOn();
// Colour channel `ch`'s alpha of a vertex shader key from the light block `lt` (u_lt rows: amb 0..1, mat 2..3), as an interval; false when per-vertex data (or lights) make it unknown.
bool HsrRasAlpha(const VsKey& vk, int ch, const float* lt, float& lo, float& hi);
// tile24: cLo/cHi (when given) receive the range of the final colour (any channel, after the output clamp), same arithmetic; fog is NOT in it (a caller that needs the colour must check k.fog == 0).
bool AlphaRangeDyn(const FsKey& k, const float* tev, float& lo, float& hi, const HsrFacts* facts = nullptr, float* cLo = nullptr, float* cHi = nullptr);
// Census facts about a fragment key (diagnostics only; MSC_VGX_SHCENSUS).
struct FsFacts {
    uint8_t nopStages = 0;     // stages that pass PREV through unchanged (colour and alpha)
    uint8_t nopColorOnly = 0;  // colour part is a pass-through, alpha part is not (or the other way round)
    uint8_t constAlpha = 0;    // 1: the final alpha is a constant (constVal, 0..255)
    uint8_t constVal = 0;
    uint8_t texAlphaMap = 0xFF; // the final alpha is exactly the alpha of this texture map (0xFF: not)
    uint8_t texZeroMap = 0xFF;  // the final alpha is exactly zero wherever the alpha of this texture map is zero (0xFF: not shown)
};
FsFacts AnalyzeFs(const FsKey& k);
// W12 OVERDRAW: what a fragment key's output is when the interpolated colour 0 is zero (so that a draw whose vertices all carry a zero
// can be skipped). Bit 0: the final alpha is zero when colour 0's alpha is; bit 1: the final colour is zero when colour 0's rgb is; bit 2: the
// final colour is zero when both are. Conservative (unknown konst / texture / register values count as not zero; fog and a destination-alpha
// write rule bit 1 / 2 and bit 0 out).
enum : uint8_t { ZF_ALPHA = 1, ZF_COLOR = 2, ZF_COLOR_BOTH = 4, ZF_DYN = 8 /* the final alpha can be provably zero from the draw's u_tev values (AlphaZeroDyn): worth looking per draw */ };
uint8_t ZeroFacts(const FsKey& k);
// The vertex shader's colour 0 is zero where the vertex attribute is: bit 0 for alpha, bit 1 for rgb (lit and unlit channels alike, as long as the
// material comes from the vertex).
uint8_t VsZeroFacts(const VsKey& k);
// The final alpha of the key is exactly zero for this draw, whatever the vertices and textures are, given the values in the draw's u_tev block
// (registers and konst colours; `tev` is its first float): a fade at zero, a konst alpha of 0.
bool AlphaZeroDyn(const FsKey& k, const float* tev);
// An alpha test (FsKey::alphaTest codes) with these references, for the 8-bit alpha `ai`.
bool AlphaTestPassesAt(uint8_t alphaTest, uint32_t ref0, uint32_t ref1, int ai);
// MSC_VGX_GENSTAT=1: totals of the generated shader text (printed by GenStatPrint, which the back end calls every 600 frames and the process at normal exit)
bool GenStatOn();
void GenStatPrint();
void GenerateFragmentShader(const FsKey& key, ShaderSource& out);
void GenerateVertexShader(const VsKey& key, ShaderSource& out);

} // namespace vgx
