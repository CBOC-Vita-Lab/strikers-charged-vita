// Names in generated code: vitaGL's GLSL-to-Cg translator #defines the
// identifiers s, t, st, ts, ss and tt (texture-coordinate swizzles), so none
// of them is used as a variable name, and mod() is avoided (it becomes Cg's
// truncating fmod there).
//
// GLSL ES 1.00 generation from the structural keys. TEV runs in floats (0..1
// for 0..255); everything that is only a value comes from uniform arrays that
// mirror the uniform blocks in vgx_cmd.h, so one glUniform4fv per block is
// enough on the back end.
#include "vgx_shadergen.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace vgx {

uint64_t HashKey(const void* key, size_t size) { return XXH3_64bits(key, size); }

bool UniformPacking() {
    const bool on = static_cast<bool>(VGX_KNOB(EnvInt("MSC_VGX_UNIPACK", 1) != 0));
    return on;
}

bool AlphaTrivialOn() {
    const bool on = static_cast<bool>(VGX_KNOB(EnvInt("MSC_VGX_ATRIVIAL", 1) != 0));
    return on;
}

int FsHalfMode() {
    const int m = static_cast<int>(VGX_KNOB([] { const long v = EnvInt("MSC_VGX_FS_HALF", 0); return v == 1 || v == 2 ? static_cast<int>(v) : 0; }()));
    return m;
}
bool Tex2dOn() {
    const bool on = static_cast<bool>(VGX_KNOB(EnvInt("MSC_VGX_TEX2D", 0) != 0));
    return on;
}

bool FsTrimOn() {
    const bool on = static_cast<bool>(VGX_KNOB(EnvInt("MSC_VGX_FS_TRIM", 0) != 0));
    return on;
}

int TevCanonMode() {
    const int m = static_cast<int>(VGX_KNOB([] { const long v = EnvInt("MSC_VGX_TEV_CANON", 0); return v == 1 || v == 2 ? static_cast<int>(v) : 0; }()));
    return m;
}

int FinClampMode() {
    const int m = static_cast<int>(VGX_KNOB([] { const long v = EnvInt("MSC_VGX_FINCLAMP", 0); return v >= 1 && v <= 3 ? static_cast<int>(v) : 0; }()));
    return m;
}

bool LeanVaryOn() {
    const bool on = static_cast<bool>(VGX_KNOB(EnvInt("MSC_VGX_LEANVARY", 0) != 0));
    return on;
}

// MSC_VGX_VS_NOBRANCH (default 0, see vgx_shadergen.h)
bool VsNoBranchOn() {
    const bool on = static_cast<bool>(VGX_KNOB(EnvInt("MSC_VGX_VS_NOBRANCH", 0) != 0));
    return on;
}

// MSC_VGX_VS_NOBRANCH_LIGHT (default 0, see vgx_shadergen.h): the per-light part of VS_NOBRANCH (direction guard, spot gate). Needs VS_NOBRANCH.
bool VsNoBranchLightOn() {
    const bool on = static_cast<bool>(VGX_KNOB(EnvInt("MSC_VGX_VS_NOBRANCH_LIGHT", 0) != 0));
    return on && VsNoBranchOn();
}

// MSC_VGX_FS_PROJ (default 0, see vgx_shadergen.h)
int FsProjMode() {
    const int m = static_cast<int>(VGX_KNOB([] { const long v = EnvInt("MSC_VGX_FS_PROJ", 0); return v == 1 || v == 2 ? static_cast<int>(v) : 0; }()));
    return m;
}

int VsTrimMode() {
    const int m = static_cast<int>(VGX_KNOB([] { const long v = EnvInt("MSC_VGX_VS_TRIM", 0); return v == 1 || v == 2 ? static_cast<int>(v) : 0; }()));
    return m;
}

int FsZFreezeVec(const FsKey& k) {
    if (!k.zfreeze) return 0;
    return (!UniformPacking() || k.numIndStages) ? 25 : (k.fog ? 11 : 9); // after everything else the block holds
}
int FsTevCount(const FsKey& k) {
    if (!UniformPacking() || k.numIndStages) return 28;
    return (k.fog ? 11 : 9) + (k.zfreeze ? 1 : 0); // registers, konst, misc (+ fog parameters and colour) (+ Z-freeze plane)
}
int VsLightCount(const VsKey& k) {
    int maxLight = -1;
    for (int i = 0; i < 4; ++i) {
        const VsKey::Chan& c = k.chan[i];
        if ((i & 1) >= k.numChans || !c.enable || !c.lightMask) continue;
        for (int l = 0; l < 8; ++l)
            if ((c.lightMask & (1u << l)) && l > maxLight) maxLight = l;
    }
    return 4 + (maxLight + 1) * 5;
}

namespace {

struct Out {
    std::string s;
    void operator()(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        char buf[1024];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        s += buf;
    }
};

const char* const kSwz = "rgba";
std::string Swizzle(uint8_t swap) {
    std::string r;
    for (int i = 0; i < 4; ++i) r += kSwz[(swap >> (i * 2)) & 3];
    return r;
}

// UB_TEV as vec4 indices
enum { V_REG = 0, V_KONST = 4, V_MISC = 8, V_FOG = 9, V_FOGCOL = 10, V_INDMTX = 11, V_INDSCALE = 17, V_TEXSIZE = 21 };

const char* ColorArg(uint32_t a) {
    static const char* const t[16] = {"prev.rgb", "prev.aaa", "c0.rgb", "c0.aaa", "c1.rgb", "c1.aaa", "c2.rgb", "c2.aaa",
                                      "tex.rgb", "tex.aaa", "ras.rgb", "ras.aaa", "vec3(1.0)", "vec3(0.5)", "kc", "vec3(0.0)"};
    return t[a & 15];
}
const char* AlphaArg(uint32_t a) {
    static const char* const t[8] = {"prev.a", "c0.a", "c1.a", "c2.a", "tex.a", "ras.a", "ka", "0.0"};
    return t[a & 7];
}
const char* RegName(uint32_t r) {
    static const char* const t[4] = {"prev", "c0", "c1", "c2"};
    return t[r & 3];
}
std::string KonstColor(uint8_t sel) {
    char b[64];
    if (sel < 8) { std::snprintf(b, sizeof b, "vec3(%.3f)", (8 - sel) / 8.0); return b; }
    if (sel < 12) return "vec3(0.0)";
    const int k = sel & 3;
    static const char* const comp[5] = {"rgb", "rrr", "ggg", "bbb", "aaa"};
    std::snprintf(b, sizeof b, "u_tev[%d].%s", V_KONST + k, comp[(sel - 12) >> 2]);
    return b;
}
std::string KonstAlpha(uint8_t sel) {
    char b[64];
    if (sel < 8) { std::snprintf(b, sizeof b, "%.3f", (8 - sel) / 8.0); return b; }
    if (sel < 16) return "0.0";
    const int k = sel & 3;
    static const char* const comp[4] = {"r", "g", "b", "a"};
    std::snprintf(b, sizeof b, "u_tev[%d].%s", V_KONST + k, comp[(sel - 16) >> 2]);
    return b;
}
const char* CompareExpr(uint32_t c, const char* ref, char* buf, size_t n) {
    switch (c) {
    case 0: return "false";
    case 1: std::snprintf(buf, n, "(ai < %s - 0.5)", ref); break;
    case 2: std::snprintf(buf, n, "(abs(ai - %s) < 0.5)", ref); break;
    case 3: std::snprintf(buf, n, "(ai < %s + 0.5)", ref); break;
    case 4: std::snprintf(buf, n, "(ai > %s + 0.5)", ref); break;
    case 5: std::snprintf(buf, n, "(abs(ai - %s) > 0.5)", ref); break;
    case 6: std::snprintf(buf, n, "(ai > %s - 0.5)", ref); break;
    default: return "true";
    }
    return buf;
}


// MSC_VGX_GENSTAT=1: at exit, how many shaders were generated and how big their text is (bytes, and an operation count: arithmetic operators,
// texture reads, discards and the built-in calls that cost instructions), for before / after comparisons of a generator change.
struct GenTotals { unsigned n[2] = {0, 0}; unsigned long long bytes[2] = {0, 0}, ops[2] = {0, 0}, tex[2] = {0, 0}, discards = 0, lines[2] = {0, 0}; std::atomic<unsigned long long> noBranchVs{0}, projFs{0}; };   // (noBranchVs / projFs: MSC_VGX_VS_NOBRANCH / FS_PROJ action counters)
GenTotals g_gen;
void GenStatReport() {
    std::fprintf(stderr, "[genstat] fragment: %u shaders, %llu bytes, %llu lines, %llu operators, %llu texture reads, %llu discards\n", g_gen.n[1], g_gen.bytes[1], g_gen.lines[1],
                 g_gen.ops[1], g_gen.tex[1], g_gen.discards);
    if (g_gen.noBranchVs.load() || g_gen.projFs.load()) std::fprintf(stderr, "[genstat] MSC_VGX_VS_NOBRANCH: %llu vertex shaders without branches; MSC_VGX_FS_PROJ: %llu fragment shaders with texture2DProj\n", g_gen.noBranchVs.load(), g_gen.projFs.load());
    std::fprintf(stderr, "[genstat] vertex:   %u shaders, %llu bytes, %llu lines, %llu operators, %llu texture reads\n", g_gen.n[0], g_gen.bytes[0], g_gen.lines[0], g_gen.ops[0], g_gen.tex[0]);
}
void GenStat(const std::string& t, bool fragment) {
    const bool on = static_cast<bool>(VGX_KNOB(EnvInt("MSC_VGX_GENSTAT", 0) != 0));
    if (!on) return;
    static bool hooked = false;
    if (!hooked) { hooked = true; std::atexit(GenStatReport); }
    const int f = fragment ? 1 : 0;
    ++g_gen.n[f];
    g_gen.bytes[f] += t.size();
    for (size_t i = 0; i < t.size(); ++i) {
        const char c = t[i];
        if (c == '\n') ++g_gen.lines[f];
        else if (c == '*' || c == '/' || c == '+' || c == '-') ++g_gen.ops[f];
    }
    for (size_t p = 0; (p = t.find("texture2D", p)) != std::string::npos; p += 9) ++g_gen.tex[f];
    for (size_t p = 0; (p = t.find("discard", p)) != std::string::npos; p += 7) ++g_gen.discards;
    for (const char* fn : {"clamp(", "mix(", "exp2(", "floor(", "dot(", "max(", "min(", "abs(", "step(", "length(", "sqrt(", "normalize("})
        for (size_t p = 0; (p = t.find(fn, p)) != std::string::npos; p += 3) ++g_gen.ops[f];
}

// Abstract value of one alpha register, for the analyses below: unknown, a constant (a multiple of 1/16 only, so every operation is exact at any
// precision the GPU may use), or exactly the alpha of a texture map.
struct AlphaVal {
    int kind = 0; // 0 unknown, 1 constant, 2 texture alpha
    float v = 0.0f;
    uint8_t map = 0;
    bool operator==(const AlphaVal& o) const { return kind == o.kind && (kind == 0 ? false : (kind == 1 ? v == o.v : map == o.map)); }
};
AlphaVal ConstVal(float v) {
    AlphaVal r;
    const float q = v * 16.0f;
    if (q != std::floor(q) || v > 64.0f || v < -64.0f) return r;
    r.kind = 1;
    r.v = v;
    return r;
}
bool IsConst(const AlphaVal& a, float v) { return a.kind == 1 && a.v == v; }

// The alpha value a TEV alpha stage leaves in its destination register, as far as it can be said without knowing any uniform or texel.
AlphaVal EvalAlphaStage(uint32_t ta, const AlphaVal in[8]) {
    AlphaVal none;
    const uint32_t bias = bits(ta, 16, 2), op = bits(ta, 18, 1), clampOn = bits(ta, 19, 1), scale = bits(ta, 20, 2);
    if (bias == 3) return none;
    const AlphaVal A = in[bits(ta, 13, 3)], B = in[bits(ta, 10, 3)], C = in[bits(ta, 7, 3)], D = in[bits(ta, 4, 3)];
    AlphaVal m;
    if (IsConst(C, 0.0f)) m = A;
    else if (IsConst(C, 1.0f)) m = B;
    else if (A == B) m = A;
    else if (A.kind == 1 && B.kind == 1 && C.kind == 1) m = ConstVal(A.v * (1.0f - C.v) + B.v * C.v);
    if (m.kind == 0) return none;
    AlphaVal r;
    if (IsConst(m, 0.0f)) r = D;
    else if (IsConst(D, 0.0f) && !op) r = m;
    else if (D.kind == 1 && m.kind == 1) r = ConstVal(op ? D.v - m.v : D.v + m.v);
    else if (IsConst(D, 0.0f) && op && m.kind == 1) r = ConstVal(-m.v);
    if (r.kind == 0) return none;
    if (bias) {
        if (r.kind != 1) return none;
        r = ConstVal(r.v + (bias == 1 ? 0.5f : -0.5f));
        if (r.kind == 0) return none;
    }
    if (scale) {
        if (r.kind != 1) return none;
        r = ConstVal(r.v * (scale == 1 ? 2.0f : (scale == 2 ? 4.0f : 0.5f)));
        if (r.kind == 0) return none;
    }
    if (r.kind == 1) {
        const float lo = clampOn ? 0.0f : -4.0157f, hi = clampOn ? 1.0f : 4.0118f;
        r.v = r.v < lo ? lo : (r.v > hi ? hi : r.v);
        r = ConstVal(r.v);
    }
    return r;
}

// Does the final alpha of the key stay known? (see AnalyzeFs)
AlphaVal FinalAlpha(const FsKey& k) {
    AlphaVal reg[4]; // prev, c0, c1, c2: whatever the uniforms hold
    AlphaVal tex = ConstVal(1.0f), ka = ConstVal(1.0f);
    for (int s = 0; s < k.numStages; ++s) {
        const FsKey::Stage& st = k.stage[s];
        if (st.texEnable) {
            tex = AlphaVal{};
            if (((st.texSwap >> 6) & 3) == 3) { tex.kind = 2; tex.map = st.texMap; }
        }
        AlphaVal ras; // vertex colour: unknown; no channel: zero
        if (st.rasChan >= 2) ras = ConstVal(0.0f);
        bool kaUsed = false;
        for (int i = 0; i < 4; ++i)
            if (bits(st.alpha, 4 + i * 3, 3) == 6) kaUsed = true;
        if (kaUsed) {
            if (st.kasel < 8) ka = ConstVal((8 - st.kasel) / 8.0f);
            else if (st.kasel < 16) ka = ConstVal(0.0f);
            else ka = AlphaVal{};
        }
        AlphaVal in[8] = {reg[0], reg[1], reg[2], reg[3], tex, ras, ka, ConstVal(0.0f)};
        reg[bits(st.alpha, 22, 2)] = EvalAlphaStage(st.alpha, in);
    }
    return reg[bits(k.stage[k.numStages - 1].alpha, 22, 2)];
}

bool CompareHolds(uint32_t c, int ai, int ref) {
    switch (c) {
    case 0: return false;
    case 1: return ai < ref;
    case 2: return ai == ref;
    case 3: return ai <= ref;
    case 4: return ai > ref;
    case 5: return ai != ref;
    case 6: return ai >= ref;
    default: return true;
    }
}

} // namespace


bool GenStatOn() {
    const bool on = static_cast<bool>(VGX_KNOB(EnvInt("MSC_VGX_GENSTAT", 0) != 0));
    return on;
}
void GenStatPrint() { GenStatReport(); }

bool AlphaTestPassesAt(uint8_t alphaTest, uint32_t ref0, uint32_t ref1, int ai) {
    const uint32_t c0 = alphaTest & 7, c1 = (alphaTest >> 3) & 7, op = (alphaTest >> 6) & 3;
    const bool t0 = CompareHolds(c0, ai, static_cast<int>(ref0)), t1 = CompareHolds(c1, ai, static_cast<int>(ref1));
    return op == 0 ? (t0 && t1) : (op == 1 ? (t0 || t1) : (op == 2 ? (t0 != t1) : (t0 == t1)));
}

namespace {
// Zero-ness of the final colour and alpha under assumptions: colour 0's rgb (rasC) / alpha (rasA) is zero; with `tev` (the draw's u_tev block), the
// registers and konst values it holds are looked at as well (otherwise unknown, but konst selects that are constants count).
void ZeroRun(const FsKey& k, bool rasC, bool rasA, const float* tev, bool& colZero, bool& alphaZero) {
    bool rc[4] = {false, false, false, false}, ra[4] = {false, false, false, false};
    if (tev)
        for (int i = 0; i < 4; ++i) {
            rc[i] = tev[V_REG * 4 + i * 4] == 0.0f && tev[V_REG * 4 + i * 4 + 1] == 0.0f && tev[V_REG * 4 + i * 4 + 2] == 0.0f;
            ra[i] = tev[V_REG * 4 + i * 4 + 3] == 0.0f;
        }
    bool kcZero = false, kaZero = false;   // kc / ka as the last stage that used them left them (the shader starts them at 1)
    for (int s = 0; s < k.numStages; ++s) {
        const FsKey::Stage& st = k.stage[s];
        bool zC = false, zA = false;   // ras.rgb / ras.a zero in this stage
        if (st.rasChan >= 2 && st.rasChan != 5 && st.rasChan != 6) zC = zA = true;   // (no channel: ras is zero)
        else if (st.rasChan == 0 && st.rasSwap == 0xE4) { zC = rasC; zA = rasA; }
        const uint32_t tc = st.color, ta = st.alpha;
        bool kcUsed = false, kaUsed = false;
        for (int i = 0; i < 4; ++i) {
            if (bits(tc, i * 4, 4) == 14) kcUsed = true;
            if (bits(ta, 4 + i * 3, 3) == 6) kaUsed = true;
        }
        if (kcUsed) {
            const uint8_t sel = st.kcsel;
            if (sel < 8) kcZero = false;
            else if (sel < 12) kcZero = true;
            else if (tev) {
                const int kr = V_KONST + (sel & 3), comp = (sel - 12) >> 2;   // 0 rgb, 1..3 r g b, 4 a
                const float* v = tev + kr * 4;
                kcZero = comp == 0 ? (v[0] == 0.0f && v[1] == 0.0f && v[2] == 0.0f) : (v[comp == 4 ? 3 : comp - 1] == 0.0f);
            } else kcZero = false;
        }
        if (kaUsed) {
            const uint8_t sel = st.kasel;
            if (sel < 8) kaZero = false;
            else if (sel < 16) kaZero = true;
            else if (tev) kaZero = tev[(V_KONST + (sel & 3)) * 4 + ((sel - 16) >> 2)] == 0.0f;
            else kaZero = false;
        }
        auto cz = [&](uint32_t a) -> bool {
            switch (a & 15) {
            case 0: return rc[0]; case 1: return ra[0]; case 2: return rc[1]; case 3: return ra[1];
            case 4: return rc[2]; case 5: return ra[2]; case 6: return rc[3]; case 7: return ra[3];
            case 10: return zC; case 11: return zA; case 14: return kcZero; case 15: return true;
            default: return false;
            }
        };
        auto az = [&](uint32_t a) -> bool {
            switch (a & 7) {
            case 0: return ra[0]; case 1: return ra[1]; case 2: return ra[2]; case 3: return ra[3];
            case 5: return zA; case 6: return kaZero; case 7: return true;
            default: return false;
            }
        };
        auto stageZero = [&](uint32_t reg, bool color) -> bool {
            if (bits(reg, 16, 2) != 0) return false;   // a bias (0.5), or the compare modes (3)
            bool A, B, C, D;
            if (color) { A = cz(bits(reg, 12, 4)); B = cz(bits(reg, 8, 4)); C = cz(bits(reg, 4, 4)); D = cz(bits(reg, 0, 4)); }
            else { A = az(bits(reg, 13, 3)); B = az(bits(reg, 10, 3)); C = az(bits(reg, 7, 3)); D = az(bits(reg, 4, 3)); }
            return D && A && (B || C);   // d +- ((1 - c) a + c b) is zero when d, a are and one of b, c is
        };
        const bool newC = stageZero(tc, true), newA = stageZero(ta, false);
        rc[bits(tc, 22, 2)] = newC;
        ra[bits(ta, 22, 2)] = newA;
    }
    const FsKey::Stage& last = k.stage[k.numStages - 1];
    colZero = rc[bits(last.color, 22, 2)];
    alphaZero = ra[bits(last.alpha, 22, 2)];
}
} // namespace

uint8_t ZeroFacts(const FsKey& k) {
    uint8_t out = 0;
    bool c, a;
    if (k.numIndStages == 0 && k.numStages >= 1) {
        ZeroRun(k, false, true, nullptr, c, a);
        if (a && !k.dstAlpha) out |= ZF_ALPHA;
        ZeroRun(k, true, false, nullptr, c, a);
        if (c && !k.fog) out |= ZF_COLOR;
        ZeroRun(k, true, true, nullptr, c, a);
        if (c && !k.fog) out |= ZF_COLOR_BOTH;
        static const float kZeros[32] = {};
        ZeroRun(k, false, false, kZeros, c, a);   // (every register and konst at zero: the best case)
        if (a && !k.dstAlpha) out |= ZF_DYN;
    }
    return out;
}

bool AlphaZeroDyn(const FsKey& k, const float* tev) {
    if (k.numIndStages != 0 || k.numStages < 1 || k.dstAlpha) return false;
    bool c, a;
    ZeroRun(k, false, false, tev, c, a);
    return a;
}

uint8_t VsZeroFacts(const VsKey& k) {
    uint8_t out = 0;
    // (a channel that is not produced passes the attribute through; one that is produced multiplies the material, from the vertex or not, by
    // its lighting: zero only when the material comes from the vertex)
    if (k.numChans < 1 || k.chan[2].matSrcVtx) out |= 1;
    if (k.numChans < 1 || k.chan[0].matSrcVtx) out |= 2;
    return out;
}

bool BlendNoDiscardOn() {
    const bool on = static_cast<bool>(VGX_KNOB(EnvInt("MSC_VGX_BLENDNODISCARD", 0) != 0));
    return on;
}
bool DiscardBlendOn() {
    const bool on = static_cast<bool>(VGX_KNOB(EnvInt("MSC_VGX_DISCARD_BLEND", 0) != 0));
    return on;
}
bool AlphaTestPassesFromOne(const FsKey& k, uint32_t ref0, uint32_t ref1) {
    if (!k.alphaTestOn) return true;
    const uint32_t c0 = k.alphaTest & 7, c1 = (k.alphaTest >> 3) & 7, op = (k.alphaTest >> 6) & 3;
    for (int ai = 1; ai <= 255; ++ai) {
        const bool t0 = CompareHolds(c0, ai, static_cast<int>(ref0)), t1 = CompareHolds(c1, ai, static_cast<int>(ref1));
        const bool pass = op == 0 ? (t0 && t1) : (op == 1 ? (t0 || t1) : (op == 2 ? (t0 != t1) : (t0 == t1)));
        if (!pass) return false;
    }
    return true;
}

bool AlphaTestAlwaysPasses(const FsKey& k, uint32_t ref0, uint32_t ref1) {
    if (!k.alphaTestOn) return true;
    // The alphas the shader can produce: ai = floor(clamp(a) * 255 + 0.5). A constant alpha is taken with one step of slack either way
    // (a half-way value such as 0.5 * 255 + 0.5 sits exactly on a rounding edge).
    int lo = 0, hi = 255;
    const AlphaVal fa = FinalAlpha(k);
    if (fa.kind == 1) {
        const float c = fa.v < 0.0f ? 0.0f : (fa.v > 1.0f ? 1.0f : fa.v);
        const int ai = static_cast<int>(std::floor(c * 255.0f + 0.5f));
        lo = ai > 0 ? ai - 1 : 0;
        hi = ai < 255 ? ai + 1 : 255;
    }
    const uint32_t c0 = k.alphaTest & 7, c1 = (k.alphaTest >> 3) & 7, op = (k.alphaTest >> 6) & 3;
    for (int ai = lo; ai <= hi; ++ai) {
        const bool t0 = CompareHolds(c0, ai, static_cast<int>(ref0)), t1 = CompareHolds(c1, ai, static_cast<int>(ref1));
        const bool pass = op == 0 ? (t0 && t1) : (op == 1 ? (t0 || t1) : (op == 2 ? (t0 != t1) : (t0 == t1)));
        if (!pass) return false;
    }
    return true;
}

// A one-stage key whose final alpha is exactly zero wherever the texture alpha is: d = 0 and either the compare "tex > 0 ? c : 0", or a
// mix of {zero, tex} (no bias): zero times any finite weight is zero. Returns the texture map, or 0xFF.
uint8_t TexZeroMap(const FsKey& k) {
    if (k.numStages != 1) return 0xFF;
    const FsKey::Stage& st = k.stage[0];
    if (!st.texEnable || ((st.texSwap >> 6) & 3) != 3 || st.ind) return 0xFF;
    const uint32_t ta = st.alpha, bias = bits(ta, 16, 2), op = bits(ta, 18, 1), A = bits(ta, 13, 3), B = bits(ta, 10, 3), D = bits(ta, 4, 3);
    if (D != 7) return 0xFF;
    if (bias == 3) return (op == 0 && bits(ta, 20, 2) == 3 && A == 4 && B == 7) ? st.texMap : 0xFF;
    if (bias != 0 || op != 0) return 0xFF;
    const bool aOk = A == 7 || A == 4, bOk = B == 7 || B == 4;
    return (aOk && bOk && !(A == 7 && B == 7)) ? st.texMap : 0xFF;
}

FsFacts AnalyzeFs(const FsKey& k) {
    FsFacts f;
    f.texZeroMap = TexZeroMap(k);
    for (int s = 0; s < k.numStages; ++s) {
        const FsKey::Stage& st = k.stage[s];
        const uint32_t tc = st.color, ta = st.alpha;
        // Colour: PREV = clamp(PREV + 0) with no bias and no scale (the "+ 0" being ZERO as C, or ZERO as both A and B).
        const bool cOp = bits(tc, 22, 2) == 0 && bits(tc, 0, 4) == 0 && bits(tc, 16, 2) == 0 && !bits(tc, 18, 1) && bits(tc, 20, 2) == 0 &&
                         (bits(tc, 4, 4) == 15 || (bits(tc, 12, 4) == 15 && bits(tc, 8, 4) == 15));
        const bool aOp = bits(ta, 22, 2) == 0 && bits(ta, 4, 3) == 0 && bits(ta, 16, 2) == 0 && !bits(ta, 18, 1) && bits(ta, 20, 2) == 0 &&
                         (bits(ta, 7, 3) == 7 || (bits(ta, 13, 3) == 7 && bits(ta, 10, 3) == 7));
        if (cOp && aOp) ++f.nopStages;
        else if (cOp || aOp) ++f.nopColorOnly;
    }
    const AlphaVal fa = FinalAlpha(k);
    if (fa.kind == 1) {
        const float c = fa.v < 0.0f ? 0.0f : (fa.v > 1.0f ? 1.0f : fa.v);
        f.constAlpha = 1;
        f.constVal = static_cast<uint8_t>(std::floor(c * 255.0f + 0.5f));
    } else if (fa.kind == 2) {
        f.texAlphaMap = fa.map;
    }
    return f;
}

namespace {
// One value of a TEV register in the half analysis: the range it can have, and the largest error against the same value computed in full precision.
struct HalfVal { float lo = 0, hi = 0, e = 0; };
inline float HalfTop(const HalfVal& v) { const float a = v.lo < 0.0f ? -v.lo : v.lo, b = v.hi < 0.0f ? -v.hi : v.hi; return a > b ? a : b; }
// Half's rounding error of a result of magnitude up to m: half a unit in its last place. 11 significant bits: 2^-12 for anything up to 1 (1 itself is exact), doubling per binade above.
float HalfUlp(float m) {
    float r = 1.0f / 4096.0f;
    for (float top = 1.0f; m > top; top *= 2.0f) r *= 2.0f;
    return r;
}
HalfVal HalfRead(float lo, float hi) { HalfVal v{lo, hi, 0.0f}; v.e = HalfUlp(HalfTop(v)); return v; } // a float value (texel, varying, uniform) taken into half
HalfVal HalfExact(float v) { return HalfVal{v, v, 0.0f}; }                                             // a constant half holds exactly
// d +- mix(a, b, c) with the stage's bias, scale and clamp. mix is a + c (b - a) or a (1 - c) + b c, as the compiler likes; five roundings bound either (four operations, b - a one binade up).
HalfVal HalfStage(const HalfVal& a, const HalfVal& b, const HalfVal& c, const HalfVal& d, bool minus, uint32_t bias, uint32_t clampOn, uint32_t scale) {
    HalfVal m;
    if (c.lo >= 0.0f && c.hi <= 1.0f) {
        m.lo = a.lo < b.lo ? a.lo : b.lo;
        m.hi = a.hi > b.hi ? a.hi : b.hi;
        m.e = (a.e > b.e ? a.e : b.e) + (m.hi - m.lo) * c.e;
    } else {
        const float w = HalfTop(c);
        const float reach = (1.0f + w) * HalfTop(a) + w * HalfTop(b);
        m.lo = -reach;
        m.hi = reach;
        m.e = (1.0f + w) * a.e + w * b.e + (HalfTop(a) + HalfTop(b)) * c.e;
    }
    m.e += 5.0f * HalfUlp(HalfTop(m));
    HalfVal r;
    r.lo = minus ? d.lo - m.hi : d.lo + m.lo;
    r.hi = minus ? d.hi - m.lo : d.hi + m.hi;
    r.e = d.e + m.e + HalfUlp(HalfTop(r));
    if (bias) {
        const float b5 = bias == 1 ? 0.5f : -0.5f;
        r.lo += b5;
        r.hi += b5;
        r.e += HalfUlp(HalfTop(r));
    }
    static const float kScale[4] = {1.0f, 2.0f, 4.0f, 0.5f};
    r.lo *= kScale[scale];
    r.hi *= kScale[scale];
    r.e *= kScale[scale]; // (scaling by a power of two rounds nothing)
    const float lo = clampOn ? 0.0f : -4.0157f, hi = clampOn ? 1.0f : 4.0118f;
    r.lo = r.lo < lo ? lo : (r.lo > hi ? hi : r.lo);
    r.hi = r.hi < lo ? lo : (r.hi > hi ? hi : r.hi);
    return r;
}
} // namespace

float FsHalfBound(const FsKey& k, float regLo, float regHi) {
    if (k.numIndStages) return -1.0f;
    for (int s = 0; s < k.numStages; ++s)
        if (k.stage[s].ind || bits(k.stage[s].color, 16, 2) == 3 || bits(k.stage[s].alpha, 16, 2) == 3) return -1.0f;
    HalfVal rc[4], ra[4];
    for (int i = 0; i < 4; ++i) rc[i] = ra[i] = HalfRead(regLo, regHi);
    HalfVal tc = HalfExact(1.0f), ta = HalfExact(1.0f), kc = HalfExact(1.0f), ka = HalfExact(1.0f);
    for (int s = 0; s < k.numStages; ++s) {
        const FsKey::Stage& st = k.stage[s];
        if (st.texEnable) tc = ta = HalfRead(0.0f, 1.0f);
        const HalfVal ras = st.rasChan < 2 ? HalfRead(0.0f, 1.0f) : HalfExact(0.0f);
        const uint32_t cc = st.color, ca = st.alpha;
        bool kcUsed = false, kaUsed = false;
        for (int i = 0; i < 4; ++i) {
            if (bits(cc, i * 4, 4) == 14) kcUsed = true;
            if (bits(ca, 4 + i * 3, 3) == 6) kaUsed = true;
        }
        if (kcUsed) kc = st.kcsel < 8 ? HalfExact((8 - st.kcsel) / 8.0f) : (st.kcsel < 12 ? HalfExact(0.0f) : HalfRead(0.0f, 1.0f));
        if (kaUsed) ka = st.kasel < 8 ? HalfExact((8 - st.kasel) / 8.0f) : (st.kasel < 16 ? HalfExact(0.0f) : HalfRead(0.0f, 1.0f));
        auto colourArg = [&](uint32_t a) -> HalfVal {
            switch (a & 15) {
            case 0: case 2: case 4: case 6: return rc[(a & 15) / 2];
            case 1: case 3: case 5: case 7: return ra[(a & 15) / 2];
            case 8: return tc;
            case 9: return ta;
            case 10: case 11: return ras;
            case 12: return HalfExact(1.0f);
            case 13: return HalfExact(0.5f);
            case 14: return kc;
            default: return HalfExact(0.0f);
            }
        };
        auto alphaArg = [&](uint32_t a) -> HalfVal {
            switch (a & 7) {
            case 0: case 1: case 2: case 3: return ra[a & 7];
            case 4: return ta;
            case 5: return ras;
            case 6: return ka;
            default: return HalfExact(0.0f);
            }
        };
        const HalfVal nc = HalfStage(colourArg(bits(cc, 12, 4)), colourArg(bits(cc, 8, 4)), colourArg(bits(cc, 4, 4)), colourArg(bits(cc, 0, 4)), bits(cc, 18, 1) != 0, bits(cc, 16, 2), bits(cc, 19, 1), bits(cc, 20, 2));
        const HalfVal na = HalfStage(alphaArg(bits(ca, 13, 3)), alphaArg(bits(ca, 10, 3)), alphaArg(bits(ca, 7, 3)), alphaArg(bits(ca, 4, 3)), bits(ca, 18, 1) != 0, bits(ca, 16, 2), bits(ca, 19, 1), bits(ca, 20, 2));
        rc[bits(cc, 22, 2)] = nc;
        ra[bits(ca, 22, 2)] = na;
    }
    const FsKey::Stage& last = k.stage[k.numStages - 1];
    const float ec = rc[bits(last.color, 22, 2)].e, ea = ra[bits(last.alpha, 22, 2)].e;
    return ec > ea ? ec : ea;
}

bool FsHalfEligible(const FsKey& k) {
    const float usual = FsHalfBound(k, 0.0f, 1.0f);
    if (usual < 0.0f || usual > 1.0f / 255.0f) return false;
    return FsHalfBound(k, -4.0157f, 4.0118f) <= 4.0f / 255.0f;
}

namespace {
struct Rng { float lo, hi; };
inline Rng Rg(float lo, float hi) { return Rng{lo, hi}; }
inline Rng Hull(const Rng& a, const Rng& b) { return Rng{a.lo < b.lo ? a.lo : b.lo, a.hi > b.hi ? a.hi : b.hi}; }
inline float Lerp(float a, float b, float c) { return a * (1.0f - c) + b * c; }
Rng TrimStage(const Rng& a, const Rng& b, const Rng& c, const Rng& d, uint32_t reg, FsTrimClamp& out) {
    const uint32_t bias = bits(reg, 16, 2), minus = bits(reg, 18, 1), clampOn = bits(reg, 19, 1), scale = bits(reg, 20, 2);
    Rng r;
    if (bias == 3) {
        // Compare modes: d + (test ? c : 0).
        const Rng m = Hull(c, Rg(0.0f, 0.0f));
        r = Rng{d.lo + m.lo, d.hi + m.hi};
    } else {
        Rng m;
        if (c.lo >= 0.0f && c.hi <= 1.0f) m = Hull(a, b);
        else {
            // (bilinear in a, b and c: the extremes are at the corners)
            m = Rng{1e30f, -1e30f};
            for (int i = 0; i < 8; ++i) {
                const float v = Lerp((i & 1) ? a.hi : a.lo, (i & 2) ? b.hi : b.lo, (i & 4) ? c.hi : c.lo);
                m.lo = v < m.lo ? v : m.lo;
                m.hi = v > m.hi ? v : m.hi;
            }
        }
        r = minus ? Rng{d.lo - m.hi, d.hi - m.lo} : Rng{d.lo + m.lo, d.hi + m.hi};
        if (bias) { const float b5 = bias == 1 ? 0.5f : -0.5f; r.lo += b5; r.hi += b5; }
        static const float kScale[4] = {1.0f, 2.0f, 4.0f, 0.5f};
        r.lo *= kScale[scale];
        r.hi *= kScale[scale];
    }
    const float lo = clampOn ? 0.0f : -4.0157f, hi = clampOn ? 1.0f : 4.0118f;
    out.lo = r.lo < lo;
    out.hi = r.hi > hi;
    if (r.lo < lo) r.lo = lo;
    if (r.hi > hi) r.hi = hi;
    if (r.lo > hi) r.lo = hi;
    if (r.hi < lo) r.hi = lo;
    return r;
}
// What the analysis knows of a stage, for the canon form of the text: the range of each operand of the colour and the alpha part, and of what each part leaves in its register.
struct StagePlan { Rng ca[4], aa[4], resC, resA; };
} // namespace

static void TrimRun(const FsKey& k, FsTrimInfo& out, StagePlan* plan);
void FsTrimAnalyze(const FsKey& k, FsTrimInfo& out) { TrimRun(k, out, nullptr); }
static void TrimRun(const FsKey& k, FsTrimInfo& out, StagePlan* plan) {
    const Rng unit = Rg(0.0f, 1.0f), one = Rg(1.0f, 1.0f), zero = Rg(0.0f, 0.0f), half = Rg(0.5f, 0.5f);
    const Rng regStart = (k.pad & FSK_WIDE) ? Rg(-4.0157f, 4.0118f) : unit;
    Rng rc[4] = {regStart, regStart, regStart, regStart}, ra[4] = {regStart, regStart, regStart, regStart};
    Rng tex = one, kc = one, ka = one;
    for (int s = 0; s < k.numStages; ++s) {
        const FsKey::Stage& st = k.stage[s];
        if (st.texEnable) tex = unit;
        const Rng ras = st.rasChan < 2 ? unit : zero;
        const uint32_t cc = st.color, ca = st.alpha;
        bool kcUsed = false, kaUsed = false;
        for (int i = 0; i < 4; ++i) {
            if (bits(cc, i * 4, 4) == 14) kcUsed = true;
            if (bits(ca, 4 + i * 3, 3) == 6) kaUsed = true;
        }
        if (kcUsed) kc = st.kcsel < 8 ? Rg((8 - st.kcsel) / 8.0f, (8 - st.kcsel) / 8.0f) : (st.kcsel < 12 ? zero : unit);
        if (kaUsed) ka = st.kasel < 8 ? Rg((8 - st.kasel) / 8.0f, (8 - st.kasel) / 8.0f) : (st.kasel < 16 ? zero : unit);
        auto colourArg = [&](uint32_t a) -> Rng {
            a &= 15;
            if (a < 8) return (a & 1) ? ra[a / 2] : rc[a / 2];
            switch (a) {
            case 8: case 9: return tex;
            case 10: case 11: return ras;
            case 12: return one;
            case 13: return half;
            case 14: return kc;
            default: return zero;
            }
        };
        auto alphaArg = [&](uint32_t a) -> Rng {
            a &= 7;
            if (a < 4) return ra[a];
            return a == 4 ? tex : (a == 5 ? ras : (a == 6 ? ka : zero));
        };
        const Rng ca4[4] = {colourArg(bits(cc, 12, 4)), colourArg(bits(cc, 8, 4)), colourArg(bits(cc, 4, 4)), colourArg(bits(cc, 0, 4))};
        const Rng aa4[4] = {alphaArg(bits(ca, 13, 3)), alphaArg(bits(ca, 10, 3)), alphaArg(bits(ca, 7, 3)), alphaArg(bits(ca, 4, 3))};
        const Rng nc = TrimStage(ca4[0], ca4[1], ca4[2], ca4[3], cc, out.c[s]);
        const Rng na = TrimStage(aa4[0], aa4[1], aa4[2], aa4[3], ca, out.a[s]);
        if (plan) {
            for (int i = 0; i < 4; ++i) { plan[s].ca[i] = ca4[i]; plan[s].aa[i] = aa4[i]; }
            plan[s].resC = nc;
            plan[s].resA = na;
        }
        rc[bits(cc, 22, 2)] = nc;
        ra[bits(ca, 22, 2)] = na;
    }
    const FsKey::Stage& last = k.stage[k.numStages - 1];
    const Rng fc = rc[bits(last.color, 22, 2)], fa = ra[bits(last.alpha, 22, 2)];
    if (plan) { plan[16].resC = fc; plan[16].resA = fa; }
    out.finC.lo = fc.lo < 0.0f; out.finC.hi = fc.hi > 1.0f;   // (what the ranges say; the generator keeps the final clamp regardless)
    out.finA.lo = fa.lo < 0.0f; out.finA.hi = fa.hi > 1.0f;
}

// MSC_VGX_HSR_PROVEN: the range of the final alpha (0..1, after the output clamp) for one draw, from the key's stages and the draw's own u_tev block (`tev`: registers, then konst
// colours; texels and vertex colours are 0..1, a konst select that is a constant is that constant). The same interval arithmetic as FsTrimAnalyze, with the numbers the draw holds.
bool HsrFactsOn() {
    const bool on = static_cast<bool>(VGX_KNOB(EnvInt("MSC_VGX_HSR_FACTS", 0) != 0));
    return on;
}
bool HsrRasAlpha(const VsKey& vk, int ch, const float* lt, float& lo, float& hi) {
    if (ch < 0 || ch > 1 || ch >= vk.numChans) return false;   // (no lit channel: the colour is the vertex's own)
    const VsKey::Chan& c = vk.chan[ch + 2];
    if (c.matSrcVtx) return false;
    const float mat = lt[(2 + ch) * 4 + 3];
    float la = 1.0f;
    if (c.enable) {
        if (c.ambSrcVtx || c.lightMask) return false;
        la = lt[ch * 4 + 3];
        la = la < 0.0f ? 0.0f : (la > 1.0f ? 1.0f : la);
    }
    const float v = mat * la;
    lo = hi = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    return true;
}
bool AlphaRangeDyn(const FsKey& k, const float* tev, float& lo, float& hi, const HsrFacts* facts, float* cLo, float* cHi) {
    if (k.numIndStages != 0 || k.numStages < 1) return false;
    const Rng unit = Rg(0.0f, 1.0f), one = Rg(1.0f, 1.0f), zero = Rg(0.0f, 0.0f);
    Rng rc[4], ra[4];
    for (int i = 0; i < 4; ++i) {
        const float* r = tev + V_REG * 4 + i * 4;
        const float mn = std::fmin(r[0], std::fmin(r[1], r[2])), mx = std::fmax(r[0], std::fmax(r[1], r[2]));
        rc[i] = Rg(mn, mx);
        ra[i] = Rg(r[3], r[3]);
    }
    Rng tex = one, texA = one, kc = one, ka = one;
    for (int s = 0; s < k.numStages; ++s) {
        const FsKey::Stage& st = k.stage[s];
        if (st.texEnable) { tex = unit; texA = (facts && ((facts->texAOne >> st.texMap) & 1u) && st.texSwap == 0xE4) ? one : unit; }
        const Rng ras = st.rasChan < 2 ? unit : zero;
        Rng rasA = ras;
        if (facts && st.rasChan < 2 && st.rasSwap == 0xE4 && facts->rasA[st.rasChan][0] <= facts->rasA[st.rasChan][1]) rasA = Rg(facts->rasA[st.rasChan][0], facts->rasA[st.rasChan][1]);
        const uint32_t cc = st.color, ca = st.alpha;
        bool kcUsed = false, kaUsed = false;
        for (int i = 0; i < 4; ++i) {
            if (bits(cc, i * 4, 4) == 14) kcUsed = true;
            if (bits(ca, 4 + i * 3, 3) == 6) kaUsed = true;
        }
        if (kcUsed) {
            if (st.kcsel < 8) kc = Rg((8 - st.kcsel) / 8.0f, (8 - st.kcsel) / 8.0f);
            else if (st.kcsel < 12) kc = zero;
            else {
                const float* v = tev + (V_KONST + (st.kcsel & 3)) * 4;
                const int comp = (st.kcsel - 12) >> 2;   // 0 rgb, 1..3 r g b, 4 a
                kc = comp == 0 ? Rg(std::fmin(v[0], std::fmin(v[1], v[2])), std::fmax(v[0], std::fmax(v[1], v[2]))) : Rg(v[comp == 4 ? 3 : comp - 1], v[comp == 4 ? 3 : comp - 1]);
            }
        }
        if (kaUsed) {
            if (st.kasel < 8) ka = Rg((8 - st.kasel) / 8.0f, (8 - st.kasel) / 8.0f);
            else if (st.kasel < 16) ka = zero;
            else { const float v = tev[(V_KONST + (st.kasel & 3)) * 4 + ((st.kasel - 16) >> 2)]; ka = Rg(v, v); }
        }
        auto colourArg = [&](uint32_t a) -> Rng {
            a &= 15;
            if (a < 8) return (a & 1) ? ra[a / 2] : rc[a / 2];
            switch (a) {
            case 8: return tex;
            case 9: return texA;
            case 10: return ras;
            case 11: return rasA;
            case 12: return one;
            case 13: return Rg(0.5f, 0.5f);
            case 14: return kc;
            default: return zero;
            }
        };
        auto alphaArg = [&](uint32_t a) -> Rng {
            a &= 7;
            if (a < 4) return ra[a];
            return a == 4 ? texA : (a == 5 ? rasA : (a == 6 ? ka : zero));
        };
        FsTrimClamp sink;
        const Rng nc = TrimStage(colourArg(bits(cc, 12, 4)), colourArg(bits(cc, 8, 4)), colourArg(bits(cc, 4, 4)), colourArg(bits(cc, 0, 4)), cc, sink);
        const Rng na = TrimStage(alphaArg(bits(ca, 13, 3)), alphaArg(bits(ca, 10, 3)), alphaArg(bits(ca, 7, 3)), alphaArg(bits(ca, 4, 3)), ca, sink);
        rc[bits(cc, 22, 2)] = nc;
        ra[bits(ca, 22, 2)] = na;
    }
    const Rng fa = ra[bits(k.stage[k.numStages - 1].alpha, 22, 2)];
    if (cLo && cHi) {
        const Rng fc = rc[bits(k.stage[k.numStages - 1].color, 22, 2)];
        *cLo = fc.lo < 0.0f ? 0.0f : (fc.lo > 1.0f ? 1.0f : fc.lo);
        *cHi = fc.hi < 0.0f ? 0.0f : (fc.hi > 1.0f ? 1.0f : fc.hi);
    }
    lo = fa.lo < 0.0f ? 0.0f : (fa.lo > 1.0f ? 1.0f : fa.lo);
    hi = fa.hi < 0.0f ? 0.0f : (fa.hi > 1.0f ? 1.0f : fa.hi);
    return true;
}
bool HsrProvenOn() {
    const bool on = static_cast<bool>(VGX_KNOB(EnvInt("MSC_VGX_HSR_PROVEN", 0) != 0));
    return on;
}

FsKey ShaderSwitchKey(const FsKey& k) {
    FsKey r = k;
    const int half = FsHalfMode();
    if (half && FsHalfEligible(k)) r.prec = static_cast<uint8_t>(r.prec | FSP_HALF | (half == 2 ? FSP_HALFVAR : 0));
    if (Tex2dOn()) r.prec |= FSP_TEX2;
    if (FsTrimOn()) r.prec |= FSP_TRIM;
    const int canon = TevCanonMode();
    if (canon) r.prec = static_cast<uint8_t>(r.prec | (canon == 2 ? FSP_CANON2 : FSP_CANON));
    if (FinClampMode() == 2) r.prec = static_cast<uint8_t>(r.prec | FSP_NOFC);
    if (FinClampMode() == 3) r.prec = static_cast<uint8_t>(r.prec | FSP_NOFC3);
    return r;
}
VsKey ShaderSwitchKey(const VsKey& k) {
    VsKey r = k;
    if (Tex2dOn()) r.chan[0].pad |= VSP_TEX2;
    return r;
}


namespace {
// ---- MSC_VGX_TEV_CANON: the TEV chain as short expressions. A value is a number the ranges prove (c) or the text that computes it. Colour values are vec3, alpha values float; a
// number is written as vec3(x) where a vec3 is needed and as a bare literal beside a vec3 operand (as the text has always done with `+ 0.5` and `* 2.0`).
struct CV { bool c = false; float v = 0.0f; std::string s; };
std::string CLit(float v) {
    char b[40];
    std::snprintf(b, sizeof b, "%.9g", static_cast<double>(v));
    std::string r = b;
    if (r.find_first_of(".en") == std::string::npos) r += ".0";
    if (v < 0.0f) r = "(" + r + ")";
    return r;
}
CV CNum(float v) { CV r; r.c = true; r.v = v; return r; }
CV CVar(const std::string& s) { CV r; r.s = s; return r; }
std::string CText(const CV& a, bool col) { return a.c ? (col ? "vec3(" + CLit(a.v) + ")" : CLit(a.v)) : a.s; }
CV CMul(const CV& x, const CV& y, bool col) {
    if ((x.c && x.v == 0.0f) || (y.c && y.v == 0.0f)) return CNum(0.0f);
    if (x.c && y.c) return CNum(x.v * y.v);
    if (x.c && x.v == 1.0f) return y;
    if (y.c && y.v == 1.0f) return x;
    if (x.c) return CVar("(" + y.s + " * " + CLit(x.v) + ")");
    if (y.c) return CVar("(" + x.s + " * " + CLit(y.v) + ")");
    (void)col;
    return CVar("(" + x.s + " * " + y.s + ")");
}
CV CAdd(const CV& x, const CV& y, bool col) {
    if (x.c && x.v == 0.0f) return y;
    if (y.c && y.v == 0.0f) return x;
    if (x.c && y.c) return CNum(x.v + y.v);
    if (x.c) return CVar("(" + y.s + " + " + CLit(x.v) + ")");
    if (y.c) return CVar("(" + x.s + " + " + CLit(y.v) + ")");
    (void)col;
    return CVar("(" + x.s + " + " + y.s + ")");
}
CV CSub(const CV& x, const CV& y, bool col) {   // x - y
    if (y.c && y.v == 0.0f) return x;
    if (x.c && y.c) return CNum(x.v - y.v);
    if (y.c) return CVar("(" + x.s + " - " + CLit(y.v) + ")");
    if (x.c && x.v == 0.0f) return CVar("(-" + y.s + ")");
    if (x.c) return CVar("(" + CText(x, col) + " - " + y.s + ")");
    return CVar("(" + x.s + " - " + y.s + ")");
}
bool CSame(const CV& x, const CV& y) { return x.c ? (y.c && x.v == y.v) : (!y.c && x.s == y.s); }
CV CMix(const CV& a, const CV& b, const CV& c, bool col) {
    if (c.c && c.v == 0.0f) return a;
    if (c.c && c.v == 1.0f) return b;
    if (CSame(a, b)) return a;
#ifdef VGX_TEV_CANON_BREAK
    if (a.c && a.v == 0.0f) return CMul(a, c, col);   // (the deliberate break of run_tev_canon_test.sh: mix(0, b, c) taken for 0 * c)
#endif
    if (a.c && a.v == 0.0f) return CMul(b, c, col);
    if (b.c && b.v == 0.0f) return CMul(a, CSub(CNum(1.0f), c, col), col);
    if (a.c && b.c) return CAdd(a, CMul(c, CNum(b.v - a.v), col), col);
    return CVar("mix(" + CText(a, col) + ", " + CText(b, col) + ", " + CText(c, col) + ")");
}
} // namespace

void GenerateFragmentShader(const FsKey& k, ShaderSource& src) {
    Out o;
    uint8_t texMask = 0;
    bool usesCol[2] = {false, false};
    const bool canonOn = (k.prec & (FSP_CANON | FSP_CANON2)) != 0 && (k.prec & FSP_HALF) == 0;   // (the half forms keep their text)
    const int canonMode = !canonOn ? 0 : ((k.prec & FSP_CANON2) ? 2 : 1);
    const bool trim = (k.prec & (FSP_TRIM | FSP_CANON | FSP_CANON2)) != 0;   // (the canon form includes the trim)
    FsTrimInfo ti;
    StagePlan plan[17];
    bool liveC[16] = {}, liveA[16] = {};   // canon: the stage's colour / alpha part is read by a later stage or by the output
    if (canonOn) {
        TrimRun(k, ti, plan);
        bool rc[4] = {}, ra[4] = {};
        const FsKey::Stage& lst = k.stage[k.numStages - 1];
        rc[bits(lst.color, 22, 2)] = true;
        ra[bits(lst.alpha, 22, 2)] = true;
        for (int s = k.numStages - 1; s >= 0; --s) {
            const uint32_t cc = k.stage[s].color, ca = k.stage[s].alpha;
            const int dc = static_cast<int>(bits(cc, 22, 2)), da = static_cast<int>(bits(ca, 22, 2));
            liveC[s] = rc[dc];
            liveA[s] = ra[da];
            // (both parts read the registers as they were before the stage; each part's write is then a kill)
            const bool wc = rc[dc], wa = ra[da];
            if (wc) rc[dc] = false;
            if (wa) ra[da] = false;
            if (wc)
                for (int i = 0; i < 4; ++i) {
                    const uint32_t a = bits(cc, i * 4, 4);
                    if (a < 8) { if (a & 1) ra[a / 2] = true; else rc[a / 2] = true; }
                }
            if (wa)
                for (int i = 0; i < 4; ++i) {
                    const uint32_t a = bits(ca, 4 + i * 3, 3);
                    if (a < 4) ra[a] = true;
                }
        }
    } else if (trim) FsTrimAnalyze(k, ti);
    const bool noDiscard = k.alphaTestOn && (k.pad & FSK_NODISCARD) != 0;   // MSC_VGX_DISCARD_BLEND (BuildFsKey decides; the bit is part of the key)
    for (int s = 0; s < k.numStages; ++s) {
        if (k.stage[s].texEnable) texMask |= static_cast<uint8_t>(1u << k.stage[s].texMap);
        if (k.stage[s].rasChan < 2) usesCol[k.stage[s].rasChan] = true;
    }
    for (int i = 0; i < k.numIndStages; ++i) texMask |= static_cast<uint8_t>(1u << k.ind[i].texMap);

    // Z-freeze (MSC_VGX_ZFREEZE): the fragment's depth is the reference plane at the pixel. GLES2 names the output gl_FragDepthEXT
    // (extension GL_EXT_frag_depth); vitaGL's translator knows gl_FragDepth and drops extension lines, so the macro is only
    // defined where the extension exists.
    if (k.zfreeze) o("#ifdef GL_EXT_frag_depth\n#extension GL_EXT_frag_depth : enable\n#define VGX_FRAGDEPTH gl_FragDepthEXT\n#else\n#define VGX_FRAGDEPTH gl_FragDepth\n#endif\n");
    // MSC_VGX_FS_HALF (key bit FSP_HALF): the TEV colour values are half; texture coordinates, the alpha test's value, fog, depth and uniforms stay float. Sony's
    // compiler takes `half` where vitaGL's translator leaves it, which its own preprocessor (the first pass, it defines VITAGL) lets through. Every change of type is
    // written out, in the form the picture shaders brought (half3(x), vec3(h)); no float literal meets a half operand and no half operand a `*`, so no overload has to be
    // guessed. Other GLs get vec types.
    const bool hf = (k.prec & FSP_HALF) != 0;
    const char* const V3 = hf ? "HV3" : "vec3";
    const char* const V4 = hf ? "HV4" : "vec4";
    const char* const F1 = hf ? "HF" : "float";
    if (hf) o("#ifdef VITAGL\n#define HF half\n#define HV3 half3\n#define HV4 half4\n#else\n#define HF float\n#define HV3 vec3\n#define HV4 vec4\n#endif\n");
    o("#ifdef GL_FRAGMENT_PRECISION_HIGH\nprecision highp float;\n#else\nprecision mediump float;\n#endif\n");
    for (int m = 0; m < 8; ++m)
        if (texMask & (1u << m)) o("uniform sampler2D u_tex%d;\n", m);
    o("uniform vec4 u_tev[%d];\n", FsTevCount(k));
    const char* const colVar = (k.prec & FSP_HALFVAR) ? "HV4" : "vec4";
    if (usesCol[0]) o("varying %s v_col0;\n", colVar);
    if (usesCol[1]) o("varying %s v_col1;\n", colVar);
    if (k.fog & FOG_LINEAR) o("varying float v_fz;\n"); // MSC_VGX_FOGVTX: the eye-space depth, interpolated perspective-correctly
    // MSC_VGX_TEX2D (FSP_TEX2): the projective bit says whether the third component is read. The vertex shader decides from the same register bit (VsKey::TexGen::proj).
    for (int i = 0; i < k.numTexGens; ++i) o("varying %s v_tex%d;\n", ((k.prec & FSP_TEX2) && !(k.texProjMask & (1u << i))) ? "vec2" : "vec3", i);
    o("void main() {\n");
    if (hf) {
        o("  HV4 prev = HV4(u_tev[0]), c0 = HV4(u_tev[1]), c1 = HV4(u_tev[2]), c2 = HV4(u_tev[3]);\n");
        o("  HV4 tex = HV4(1.0), ras = HV4(0.0);\n  HV3 kc = HV3(1.0);\n  HF ka = HF(1.0);\n");
    } else {
        o("  vec4 prev = u_tev[0], c0 = u_tev[1], c1 = u_tev[2], c2 = u_tev[3];\n");
        o("  vec4 tex = vec4(1.0), ras = vec4(0.0);\n  vec3 kc = vec3(1.0);\n  float ka = 1.0;\n");
    }
    // Texture coordinates (normalised).
    // MSC_VGX_FS_PROJ: a projective coordinate that only plain (not indirect) reads use is not divided here: the read is texture2DProj(sampler, vec3), the divide in the texture unit.
    // Mode 1 keeps the z == 0 rule (the third component is 1 there) with z + step(|z|, 0); mode 2 hands the varying over as it is (a z of exactly 0 then reads at infinity).
    uint8_t projRead = 0;   // coordinates whose reads are all projective reads
    if (FsProjMode()) {
        uint8_t other = 0;
        for (int i = 0; i < k.numIndStages; ++i) if (k.ind[i].texCoord < 8) other |= static_cast<uint8_t>(1u << k.ind[i].texCoord);
        for (int s = 0; s < k.numStages; ++s) if (k.stage[s].ind && k.stage[s].texCoord < 8) other |= static_cast<uint8_t>(1u << k.stage[s].texCoord);
        for (int i = 0; i < k.numTexGens && i < 8; ++i) {
            bool direct = false;
            for (int s = 0; s < k.numStages; ++s) if (k.stage[s].texEnable && !k.stage[s].ind && k.stage[s].texCoord == i) direct = true;
            if ((k.texProjMask & (1u << i)) && direct && !(other & (1u << i))) projRead |= static_cast<uint8_t>(1u << i);
        }
    }
    for (int i = 0; i < k.numTexGens; ++i) {
        if (projRead & (1u << i)) {
            if (FsProjMode() == 1) o("  vec3 pq%d = vec3(v_tex%d.xy, v_tex%d.z + step(abs(v_tex%d.z), 0.0));\n", i, i, i, i);
        }
        else if (k.texProjMask & (1u << i)) o("  vec2 uv%d = v_tex%d.xy / (v_tex%d.z == 0.0 ? 1.0 : v_tex%d.z);\n", i, i, i, i);
        else o("  vec2 uv%d = v_tex%d.xy;\n", i, i);
    }
    auto Uv = [&](int c) -> std::string {
        if (c >= k.numTexGens) return "vec2(0.0)";
        return "uv" + std::to_string(c);
    };
    // Indirect lookups: three 8-bit offsets per indirect stage, taken from A, B, G.
    for (int i = 0; i < k.numIndStages; ++i) {
        o("  vec3 ind%d = texture2D(u_tex%d, %s * u_tev[%d].xy).abg * 255.0;\n", i, k.ind[i].texMap, Uv(k.ind[i].texCoord).c_str(),
          V_INDSCALE + i);
    }
    if (k.numIndStages) o("  vec2 itc = vec2(0.0);\n");

    for (int s = 0; s < k.numStages; ++s) {
        const FsKey::Stage& st = k.stage[s];
        o("  // stage %d\n", s);
        std::string coord = Uv(st.texCoord);
        if (st.ind) {
            // Indirect texture coordinate computation, in texels of this stage's texture.
            const uint32_t ind = st.ind;
            const uint32_t bt = bits(ind, 0, 2), fmt = bits(ind, 2, 2), bias = bits(ind, 4, 3), mid = bits(ind, 9, 4);
            const uint32_t sw = bits(ind, 13, 3), tw = bits(ind, 16, 3), addPrev = bits(ind, 20, 1);
            const int map = st.texMap;
            o("  {\n    vec2 tsz = u_tev[%d].%s;\n", V_TEXSIZE + map / 2, (map & 1) ? "zw" : "xy");
            o("    vec2 tcx = %s * tsz;\n", coord.c_str());
            if (bt < k.numIndStages) o("    vec3 iv = ind%u;\n", bt);
            else o("    vec3 iv = vec3(0.0);\n");
            static const float kMod[4] = {256.0f, 32.0f, 16.0f, 8.0f};
            // Not mod(): vitaGL's GLSL translator maps it to Cg fmod, which truncates instead of flooring.
            if (fmt) o("    iv = iv - %.1f * floor(iv / %.1f);\n", kMod[fmt], kMod[fmt]);
            const float bv = fmt == 0 ? -128.0f : 1.0f;
            o("    iv += vec3(%.1f, %.1f, %.1f);\n", (bias & 1) ? bv : 0.0f, (bias & 2) ? bv : 0.0f, (bias & 4) ? bv : 0.0f);
            if (mid == 0) o("    vec2 io = vec2(0.0);\n");
            else if (mid <= 3) o("    vec2 io = vec2(dot(u_tev[%d].xyz, iv), dot(u_tev[%d].xyz, iv));\n", V_INDMTX + (mid - 1) * 2, V_INDMTX + (mid - 1) * 2 + 1);
            else if (mid >= 5 && mid <= 7) o("    vec2 io = tcx * (iv.x / 256.0) * u_tev[%d].w;\n", V_INDMTX + (mid - 5) * 2);
            else if (mid >= 9 && mid <= 11) o("    vec2 io = tcx * (iv.y / 256.0) * u_tev[%d].w;\n", V_INDMTX + (mid - 9) * 2);
            else o("    vec2 io = vec2(0.0);\n");
            static const float kWrap[8] = {0, 256, 128, 64, 32, 16, 0, 0};
            if (sw == 6) o("    tcx.x = 0.0;\n");
            else if (sw) o("    tcx.x = tcx.x - %.1f * floor(tcx.x / %.1f);\n", kWrap[sw], kWrap[sw]);
            if (tw == 6) o("    tcx.y = 0.0;\n");
            else if (tw) o("    tcx.y = tcx.y - %.1f * floor(tcx.y / %.1f);\n", kWrap[tw], kWrap[tw]);
            if (addPrev) o("    itc = tcx + io + itc;\n");
            else o("    itc = tcx + io;\n");
            o("    itc = itc / tsz;\n  }\n");
            coord = "itc";
        }
        if (st.texEnable) {
            const bool pr = !st.ind && st.texCoord < 8 && (projRead & (1u << st.texCoord));
            char pc[16];
            std::snprintf(pc, sizeof pc, FsProjMode() == 1 ? "pq%d" : "v_tex%d", static_cast<int>(st.texCoord));
            const char* const fn = pr ? "texture2DProj" : "texture2D";
            const char* const cs = pr ? pc : coord.c_str();
            if (hf) o("  tex = HV4(%s(u_tex%d, %s).%s);\n", fn, st.texMap, cs, Swizzle(st.texSwap).c_str());
            else o("  tex = %s(u_tex%d, %s).%s;\n", fn, st.texMap, cs, Swizzle(st.texSwap).c_str());
        }
        if (st.rasChan < 2) o(hf ? "  ras = HV4(v_col%d.%s);\n" : "  ras = v_col%d.%s;\n", st.rasChan, Swizzle(st.rasSwap).c_str());
        else o("  ras = %s(0.0);\n", V4);
        const uint32_t tc = st.color, ta = st.alpha;
        if (canonOn && bits(tc, 16, 2) != 3 && bits(ta, 16, 2) != 3) {
            // MSC_VGX_TEV_CANON: the stage as expressions of what it reads (compare stages keep the text below: their tests need the operands as variables)
            auto colArg = [&](int i) -> CV {
                const Rng r = plan[s].ca[i];
                if (r.lo == r.hi) return CNum(r.lo);
                const uint32_t a = bits(tc, 12 - 4 * i, 4);
                if (a < 8) return CVar(std::string(RegName(a / 2)) + ((a & 1) ? ".aaa" : ".rgb"));
                switch (a) {
                case 8: return CVar("tex.rgb");
                case 9: return CVar("tex.aaa");
                case 10: return CVar("ras.rgb");
                case 11: return CVar("ras.aaa");
                case 14: return CVar(KonstColor(st.kcsel));
                default: return CNum(0.0f);
                }
            };
            auto alpArg = [&](int i) -> CV {
                const Rng r = plan[s].aa[i];
                if (r.lo == r.hi) return CNum(r.lo);
                const uint32_t a = bits(ta, 13 - 3 * i, 3);
                if (a < 4) return CVar(std::string(RegName(a)) + ".a");
                if (a == 4) return CVar("tex.a");
                if (a == 5) return CVar("ras.a");
                if (a == 6) return CVar(KonstAlpha(st.kasel));
                return CNum(0.0f);
            };
            auto Part = [&](uint32_t reg, bool color, const Rng& res, const FsTrimClamp& act) {
                const char* const name = color ? "rc" : "ra";
                const char* const type = color ? "vec3" : "float";
                if (res.lo == res.hi) { o("    %s %s = %s;\n", type, name, CText(CNum(res.lo), color).c_str()); return; }
                const uint32_t bias = bits(reg, 16, 2), minus = bits(reg, 18, 1), clampOn = bits(reg, 19, 1), scale = bits(reg, 20, 2);
                CV A, B, C, D;
                if (color) { A = colArg(0); B = colArg(1); C = colArg(2); D = colArg(3); }
                else { A = alpArg(0); B = alpArg(1); C = alpArg(2); D = alpArg(3); }
                const CV m = CMix(A, B, C, color);
                CV r = minus ? CSub(D, m, color) : CAdd(D, m, color);
                if (bias) r = CAdd(r, CNum(bias == 1 ? 0.5f : -0.5f), color);
                static const float kS[4] = {1.0f, 2.0f, 4.0f, 0.5f};
                r = CMul(r, CNum(kS[scale]), color);
                o("    %s %s = %s;\n", type, name, CText(r, color).c_str());
                if (!(act.lo && act.hi)) {
                    if (act.lo) o("    %s = max(%s, %s(%s));\n", name, name, type, clampOn ? "0.0" : "-4.0157");
                    else if (act.hi) o("    %s = min(%s, %s(%s));\n", name, name, type, clampOn ? "1.0" : "4.0118");
                } else if (clampOn) o("    %s = clamp(%s, 0.0, 1.0);\n", name, name);
                else o("    %s = clamp(%s, -4.0157, 4.0118);\n", name, name);
            };
            if (liveC[s] || liveA[s]) {
                o("  {\n");
                if (liveC[s]) Part(tc, true, plan[s].resC, ti.c[s]);
                if (liveA[s]) Part(ta, false, plan[s].resA, ti.a[s]);
                if (liveC[s]) o("    %s.rgb = rc;\n", RegName(bits(tc, 22, 2)));
                if (liveA[s]) o("    %s.a = ra;\n", RegName(bits(ta, 22, 2)));
                o("  }\n");
            }
            continue;
        }
        bool kcUsed = false, kaUsed = false;
        for (int i = 0; i < 4; ++i) {
            if (bits(tc, i * 4, 4) == 14) kcUsed = true;
            if (bits(ta, 4 + i * 3, 3) == 6) kaUsed = true;
        }
        if (kcUsed) o(hf ? "  kc = HV3(%s);\n" : "  kc = %s;\n", KonstColor(st.kcsel).c_str());
        if (kaUsed) o(hf ? "  ka = HF(%s);\n" : "  ka = %s;\n", KonstAlpha(st.kasel).c_str());
        o("  {\n");
        auto CArg = [&](uint32_t a) -> const char* { return !hf ? ColorArg(a) : (a & 15) == 12 ? "HV3(1.0)" : (a & 15) == 13 ? "HV3(0.5)" : (a & 15) == 15 ? "HV3(0.0)" : ColorArg(a); };
        auto AArg = [&](uint32_t a) -> const char* { return hf && (a & 7) == 7 ? "HF(0.0)" : AlphaArg(a); };
        o("    %s a = %s, b = %s, c = %s, d = %s;\n", V3, CArg(bits(tc, 12, 4)), CArg(bits(tc, 8, 4)), CArg(bits(tc, 4, 4)), CArg(bits(tc, 0, 4)));
        o("    %s aa = %s, ab = %s, ac = %s, ad = %s;\n", F1, AArg(bits(ta, 13, 3)), AArg(bits(ta, 10, 3)), AArg(bits(ta, 7, 3)), AArg(bits(ta, 4, 3)));
        static const char* const kBias[3] = {"", " + 0.5", " - 0.5"};
        static const char* const kScale[4] = {"", " * 2.0", " * 4.0", " * 0.5"};
        auto Combine = [&](uint32_t reg, bool color) {
            const uint32_t bias = bits(reg, 16, 2), op = bits(reg, 18, 1), clampOn = bits(reg, 19, 1), scale = bits(reg, 20, 2);
            const char* res = color ? "rc" : "ra";
            const char* type = color ? V3 : F1;
            const char* A = color ? "a" : "aa"; const char* B = color ? "b" : "ab";
            const char* C = color ? "c" : "ac"; const char* D = color ? "d" : "ad";
            if (bias != 3 && hf) {
                // (vitaGL's translator turns every `*` into its vglMul, which has no half overloads; so a scale is an addition, and the half lerp of mix)
                const char* const cast = color ? "HV3" : "HF";
                if (bias) o("    %s %s = (%s %s mix(%s, %s, %s) %s %s(0.5));\n", type, res, D, op ? "-" : "+", A, B, C, bias == 1 ? "+" : "-", cast);
                else o("    %s %s = (%s %s mix(%s, %s, %s));\n", type, res, D, op ? "-" : "+", A, B, C);
                if (scale == 1 || scale == 2) o("    %s = %s + %s;\n", res, res, res);
                if (scale == 2) o("    %s = %s + %s;\n", res, res, res);
                if (scale == 3) o("    %s = mix(%s(0.0), %s, %s(0.5));\n", res, cast, res, cast);
            } else if (bias != 3) {
                o("    %s %s = (%s %s mix(%s, %s, %s)%s)%s;\n", type, res, D, op ? "-" : "+", A, B, C, kBias[bias], kScale[scale]);
            } else {
                // Compare modes: d + ((a OP b) ? c : 0). scale selects the operand width.
                const bool eq = op != 0;
                std::string cond;
                const float e = 0.25f / 255.0f;
                char buf[256];
                switch (scale) {
                case 0:
                    if (eq) std::snprintf(buf, sizeof buf, "(abs(a.r - b.r) < %.6f)", e * 2);
                    else std::snprintf(buf, sizeof buf, "(a.r > b.r + %.6f)", e);
                    break;
                case 1:
                    if (eq) std::snprintf(buf, sizeof buf, "(abs(dot(a.rg - b.rg, vec2(255.0, 65280.0))) < 0.5)");
                    else std::snprintf(buf, sizeof buf, "(dot(a.rg - b.rg, vec2(255.0, 65280.0)) > 0.25)");
                    break;
                case 2:
                    if (eq) std::snprintf(buf, sizeof buf, "(abs(dot(a.rgb - b.rgb, vec3(255.0, 65280.0, 16711680.0))) < 0.5)");
                    else std::snprintf(buf, sizeof buf, "(dot(a.rgb - b.rgb, vec3(255.0, 65280.0, 16711680.0)) > 0.25)");
                    break;
                default:
                    if (color) {
                        // (step, not lessThan / greaterThan: vitaGL's translator maps those to the wrong comparisons)
                        if (eq) std::snprintf(buf, sizeof buf, "(vec3(1.0) - step(vec3(%.6f), abs(a - b)))", e * 2);
                        else std::snprintf(buf, sizeof buf, "(vec3(1.0) - step(a, b + vec3(%.6f)))", e);
                    } else {
                        if (eq) std::snprintf(buf, sizeof buf, "(abs(aa - ab) < %.6f)", e * 2);
                        else std::snprintf(buf, sizeof buf, "(aa > ab + %.6f)", e);
                    }
                    break;
                }
                if (scale == 3 && color) o("    vec3 rc = d + c * %s;\n", buf);
                else o("    %s %s = %s + (%s ? %s : %s(0.0));\n", type, res, D, buf, C, type);
            }
            const FsTrimClamp act = trim ? (color ? ti.c[s] : ti.a[s]) : FsTrimClamp{};
            if (trim && !(act.lo && act.hi)) {
                // (MSC_VGX_FS_TRIM: one side costs one instruction, none costs none. The bound is written as the operand's type: Cg has no min(float3, float).)
                const char* const cast = hf ? (color ? "HV3" : "HF") : (color ? "vec3" : "float");
                if (act.lo) o("    %s = max(%s, %s(%s));\n", res, res, cast, clampOn ? "0.0" : "-4.0157");
                else if (act.hi) o("    %s = min(%s, %s(%s));\n", res, res, cast, clampOn ? "1.0" : "4.0118");
            } else if (hf) {
                const char* const cast = color ? "HV3" : "HF";
                if (clampOn) o("    %s = clamp(%s, %s(0.0), %s(1.0));\n", res, res, cast, cast);
                else o("    %s = clamp(%s, %s(-4.0157), %s(4.0118));\n", res, res, cast, cast);
            } else if (clampOn) o("    %s = clamp(%s, 0.0, 1.0);\n", res, res);
            else o("    %s = clamp(%s, -4.0157, 4.0118);\n", res, res);
        };
        Combine(tc, true);
        Combine(ta, false);
        o("    %s.rgb = rc;\n    %s.a = ra;\n  }\n", RegName(bits(tc, 22, 2)), RegName(bits(ta, 22, 2)));
    }
    const FsKey::Stage& last = k.stage[k.numStages - 1];
    // (MSC_VGX_FS_TRIM keeps the final clamp, colour and alpha, whatever the ranges say: the Galaxy port's console turned shadows and decals into static where its trim
    // dropped it (r86 follow-up). The ranges are real numbers; a value an ulp past 1 is harmless in GL, but the console's output conversion is not to be trusted with it.)
#ifdef VGX_FS_TRIM_FINAL_BREAK
    if (trim && !ti.finC.lo && !ti.finC.hi && !ti.finA.lo && !ti.finA.hi) o("  vec4 fin = vec4(%s.rgb, %s.a);\n", RegName(bits(last.color, 22, 2)), RegName(bits(last.alpha, 22, 2)));   // (the deliberate break of run_fs_trim_test.sh)
    else
#endif
    if (hf) o("  vec4 fin = vec4(clamp(HV4(%s.rgb, %s.a), HV4(0.0), HV4(1.0)));\n", RegName(bits(last.color, 22, 2)), RegName(bits(last.alpha, 22, 2)));
    else if (trim && (k.prec & FSP_NOFC3) && !k.zfreeze) {
        // MSC_VGX_FINCLAMP=3: per part the side(s) the ranges leave open. Exact by the range proof (inputs in 0..1), so a draw of any blend keeps what it had
        auto part = [&](const char* n, const char* sw, bool lo, bool hi, const char* ty) {
            char b[96];
            if (lo && hi) std::snprintf(b, sizeof b, "clamp(%s%s, 0.0, 1.0)", n, sw);
            else if (lo) std::snprintf(b, sizeof b, "max(%s%s, %s(0.0))", n, sw, ty);
            else if (hi) std::snprintf(b, sizeof b, "min(%s%s, %s(1.0))", n, sw, ty);
            else std::snprintf(b, sizeof b, "%s%s", n, sw);
            return std::string(b);
        };
        o("  vec4 fin = vec4(%s, %s);\n", part(RegName(bits(last.color, 22, 2)), ".rgb", ti.finC.lo, ti.finC.hi, "vec3").c_str(), part(RegName(bits(last.alpha, 22, 2)), ".a", ti.finA.lo, ti.finA.hi, "float").c_str());
    }
    else if (trim && ((k.prec & FSP_NOFC) || ((k.pad & FSK_NOFC) && FinClampMode() == 1)) && !k.zfreeze) {
        // MSC_VGX_FINCLAMP: the part the ranges prove inside 0..1 is not clamped (colour and alpha apart)
        const char* const cn = RegName(bits(last.color, 22, 2));
        const char* const an = RegName(bits(last.alpha, 22, 2));
        const bool cIdle = !ti.finC.lo && !ti.finC.hi, aIdle = !ti.finA.lo && !ti.finA.hi;
        o("  vec4 fin = vec4(%s%s.rgb%s, %s%s.a%s);\n", cIdle ? "" : "clamp(", cn, cIdle ? "" : ", 0.0, 1.0)", aIdle ? "" : "clamp(", an, aIdle ? "" : ", 0.0, 1.0)");
    }
    else o("  vec4 fin = clamp(vec4(%s.rgb, %s.a), 0.0, 1.0);\n", RegName(bits(last.color, 22, 2)), RegName(bits(last.alpha, 22, 2)));
    if (k.alphaTestOn) {
        char b0[96], b1[96], r0[32], r1[32];
        std::snprintf(r0, sizeof r0, "u_tev[%d].x", V_MISC);
        std::snprintf(r1, sizeof r1, "u_tev[%d].y", V_MISC);
        const char* e0 = CompareExpr(k.alphaTest & 7, r0, b0, sizeof b0);
        const char* e1 = CompareExpr((k.alphaTest >> 3) & 7, r1, b1, sizeof b1);
        static const char* const kOp[4] = {"&&", "||", "!=", "=="};
        const char* ThrTest = nullptr;   // MSC_VGX_TEV_CANON=2: the whole test as one comparison (see below)
        if (trim) {
            // (MSC_VGX_FS_TRIM: floor(x) against a whole number is x against that number, without the floor; only == and != need it)
            auto ordered = [](uint32_t c, const char* ref, char* buf, size_t n, const char* have) -> const char* {
                switch (c) {
                case 1: std::snprintf(buf, n, "(ax < %s)", ref); break;
                case 3: std::snprintf(buf, n, "(ax < %s + 1.0)", ref); break;
                case 4: std::snprintf(buf, n, "(ax >= %s + 1.0)", ref); break;
                case 6: std::snprintf(buf, n, "(ax >= %s)", ref); break;
                default: return have;
                }
                return buf;
            };
            e0 = ordered(k.alphaTest & 7, r0, b0, sizeof b0, e0);
            e1 = ordered((k.alphaTest >> 3) & 7, r1, b1, sizeof b1, e1);
            const uint32_t c0 = k.alphaTest & 7, c1 = (k.alphaTest >> 3) & 7, aop = (k.alphaTest >> 6) & 3;
            // MSC_VGX_TEV_CANON=2: tests that are lower bounds on the alpha (GREATER, GEQUAL, or ALWAYS beside one) are one comparison of fin.a against a threshold built from the
            // references (uniform arithmetic: the compiler's once-per-draw program): `ax >= r + 1` is `fin.a >= (r + 0.5) / 255`. The same decision except for an alpha within float
            // rounding of half a step of the reference; the colour is not touched.
#ifdef VGX_TEV_CANON_BREAK2
#define VGX_THR_HALF "0.0"   // (the second deliberate break of run_tev_canon_test.sh, BREAK=2: the threshold is half a step off, no colour changes, the discard decision does)
#else
#define VGX_THR_HALF "0.5"
#endif
            auto lowerD = [](uint32_t c) -> int { return c == 4 ? 1 : (c == 6 ? 0 : -1); };
            const int d0 = lowerD(c0), d1 = lowerD(c1);
            if (canonMode == 2 && aop <= 1 && d0 >= 0 && d1 >= 0) {
                std::snprintf(b0, sizeof b0, "(fin.a >= (%s(%s + %s, %s + %s) - " VGX_THR_HALF ") * 0.00392156862745098)", aop == 0 ? "max" : "min", r0, d0 ? "1.0" : "0.0", r1, d1 ? "1.0" : "0.0");
                ThrTest = b0;
            } else if (canonMode == 2 && aop == 0 && ((d0 >= 0 && c1 == 7) || (d1 >= 0 && c0 == 7))) {
                std::snprintf(b0, sizeof b0, "(fin.a >= (%s + %s - " VGX_THR_HALF ") * 0.00392156862745098)", d0 >= 0 ? r0 : r1, (d0 >= 0 ? d0 : d1) ? "1.0" : "0.0");
                ThrTest = b0;
            }
            if (!ThrTest) o("  float ax = fin.a * 255.0 + 0.5;\n");
            if (!ThrTest && (c0 == 2 || c0 == 5 || c1 == 2 || c1 == 5)) o("  float ai = floor(ax);\n");
        } else
        o("  float ai = floor(fin.a * 255.0 + 0.5);\n");
        if (noDiscard) {
            // (MSC_VGX_DISCARD_BLEND: the same test, kept for the output; the key's blend makes vec4(0.0) a no-op)
#ifdef VGX_DISCARD_BLEND_BREAK
            o("  bool vgx_ok = true;\n");   // (the deliberate break of run_discard_blend_test.sh: the test is lost)
#else
            if (ThrTest) o("  bool vgx_ok = %s;\n", ThrTest);
            else o("  bool vgx_ok = %s %s %s;\n", e0, kOp[(k.alphaTest >> 6) & 3], e1);
#endif
        } else if (ThrTest) o("  if (!%s) discard;\n", ThrTest);
        else
        o("  if (!(%s %s %s)) discard;\n", e0, kOp[(k.alphaTest >> 6) & 3], e1);
    }
    if (k.fog) {
        const int fsel = k.fog & 7;
        if (k.fog & FOG_LINEAR) {
            // MSC_VGX_FOGVTX: the draw's fog is a linear function of the eye-space depth (FogLinearOf in vgx_draw.cpp proves it for the draw's registers):
            // fa * ze + fb, both from the TEV block, with the start offset already in fb. Only the clamp is left per pixel.
            o("  float fg = clamp(u_tev[%d].x * v_fz + u_tev[%d].y, 0.0, 1.0);\n", V_FOG, V_FOG);
        } else {
        if (k.fog & 8) o("  float fg = u_tev[%d].x * gl_FragCoord.z;\n", V_FOG);
        else o("  float fg = u_tev[%d].x / max(u_tev[%d].y - gl_FragCoord.z, 0.000001);\n", V_FOG, V_FOG);
        o("  fg = clamp(fg - u_tev[%d].z, 0.0, 1.0);\n", V_FOG);
        }
        switch (fsel) {
        case 4: o("  fg = 1.0 - exp2(-8.0 * fg);\n"); break;
        case 5: o("  fg = 1.0 - exp2(-8.0 * fg * fg);\n"); break;
        case 6: o("  fg = exp2(-8.0 * (1.0 - fg));\n"); break;
        case 7: o("  fg = 1.0 - fg; fg = exp2(-8.0 * fg * fg);\n"); break;
        default: break;
        }
        o("  fin.rgb = mix(fin.rgb, u_tev[%d].rgb, fg);\n", V_FOGCOL);
    }
    if (k.dstAlpha) o("  fin.a = u_tev[%d].z;\n", V_MISC);
    if (k.zfreeze) o("  VGX_FRAGDEPTH = clamp(dot(vec3(gl_FragCoord.xy, 1.0), u_tev[%d].xyz), 0.0, 1.0);\n", FsZFreezeVec(k));
    if (noDiscard) {
        o("  gl_FragColor = vgx_ok ? vec4(fin) : vec4(0.0);\n}\n");
        static unsigned made = 0;
        if (++made <= 64) std::fprintf(stderr, "[vgx] MSC_VGX_DISCARD_BLEND: fragment program %u without discard (alpha test 0x%02X, %u stages)\n", made, static_cast<unsigned>(k.alphaTest), static_cast<unsigned>(k.numStages));
    }
    else o("  gl_FragColor = fin;\n}\n");
    if (projRead) ++g_gen.projFs;
    GenStat(o.s, true);
    src.text = std::move(o.s);
    src.texMask = texMask;
    src.numTexGens = k.numTexGens;
    src.nTev = static_cast<uint8_t>(FsTevCount(k));
    src.zfVec = static_cast<uint8_t>(FsZFreezeVec(k));
    {
        const FsFacts f = AnalyzeFs(k);
        src.cAlphaOn = k.alphaTestOn;
        src.cAlphaTest = k.alphaTest;
        src.cFog = k.fog;
        src.cStages = k.numStages;
        src.cProjMask = k.texProjMask;
        src.cNop = f.nopStages;
        src.cNopHalf = f.nopColorOnly;
        src.cConstAlpha = f.constAlpha;
        src.cConstVal = f.constVal;
        src.cTexAlphaMap = f.texAlphaMap;
        src.cTexZeroMap = f.texZeroMap;
        src.cZero = ZeroFacts(k);
        src.fkey = std::make_shared<const FsKey>(k);
    }
}

namespace {
// The palette row a vertex names (a_pos.w, a_tmi*: whole numbers stored as floats) as an int.
//
// The cast must be given a WHOLE number. The first console run drew every skinned character
// as a head with streaks below it (hw/run3-gl) with `int(a_pos.w + 0.5)` here: Sony's compiler
// turns a float-to-int cast into pack instructions (VPCKU16F32 / VPCKS16F32, no scaling)
// WITHOUT truncating first, and on the console's GPU a pack rounds to the nearest integer
// (Imagination's assembler: "Only nearest rounding is supported on this processor" for every
// core but the SGX545; its own compiler front end puts a floor / truncate in front of the
// conversion for that reason, usc2/icvt_core.c ExpandMOV). An exact x.5 therefore became
// x + 1 for some or all x: rows of the NEXT matrix element. The emulator translates the same
// pack as a C cast (truncation), so there it was right. With a whole number in the cast every
// rounding gives the same result; floor(w + 0.5) also absorbs any error below a half.
//   MSC_VGX_PALIDX=1  (default) int(floor(w + 0.5))
//   MSC_VGX_PALIDX=0  the old text, int(w + 0.5): wrong on the console (to see the fault again)
//   MSC_VGX_PALIDX=2  int(w): the form the THUG port's skinning shader uses on consoles
//   MSC_VGX_PALIDX=3 / 4  (PC only, to compare pictures with a console's) what the old text is
//                     believed to do on the console: 3 = nearest, ties to even; 4 = ties up
int PalIndexMode() {
    const int m = static_cast<int>(VGX_KNOB(static_cast<int>(EnvInt("MSC_VGX_PALIDX", 1))));
    return m;
}
void RowIndex(Out& o, const char* ind, const char* var, const char* w) {
    switch (PalIndexMode()) {
    case 0: o("%sint %s = int(%s + 0.5);\n", ind, var, w); break;
    case 2: o("%sint %s = int(%s);\n", ind, var, w); break;
    case 3:
        // (fract(x * 0.5) is 0.5 exactly for an odd whole x)
        o("%sfloat %sf = floor(%s + 0.5);\n%sif (fract(%sf * 0.5) > 0.25) %sf += 1.0;\n%sint %s = int(%sf);\n", ind, var, w, ind, var, var, ind, var, var);
        break;
    case 4: o("%sint %s = int(floor(%s + 0.5) + 1.0);\n", ind, var, w); break;
    default: o("%sint %s = int(floor(%s + 0.5));\n", ind, var, w); break;
    }
}

// MSC_VGX_VS_NOBRANCH: the row index with its upper clamp as a min() on the whole-numbered float before the cast, in place of `int v = ...; if (v > top) v = top;`. The same value for every
// finite input (floor() is whole, so min(floor(x), top) is floor(x) or top exactly; only an index past 2^31, which the cast does not define, differs). Only the default index form (PALIDX=1)
// has this text; the other forms keep the branch. Returns false when it wrote nothing (the caller writes the old text).
#ifndef VGX_NOBRANCH_BREAK
#define VGX_NOBRANCH_BREAK 0   // run_shader_text_test.sh BREAK=1..3: deliberately wrong texts the proof must catch (1: a clamp one row low, 2: a guard that also zeroes lengths below 1e-3 (Mesa flushes denormals, so a 1e-20 threshold is not observable there), 3: no guard on the light direction)
#endif
bool RowIndexClamped(Out& o, const char* ind, const char* var, const char* w, int top) {
    if (!VsNoBranchOn() || PalIndexMode() != 1) return false;
    if (VGX_NOBRANCH_BREAK == 1) --top;
    o("%sfloat %sf = min(floor(%s + 0.5), %d.0);\n%sint %s = int(%sf);\n", ind, var, w, top, ind, var, var);
    return true;
}

} // namespace

void GenerateVertexShader(const VsKey& k, ShaderSource& src) {
    Out o;
    // (MSC_VGX_VS_TRIM: see vgx_shadergen.h. All 0 with the switch off.)
    const uint8_t cosOne = k.chan[1].pad, distOne = k.chan[2].pad, farDir = k.chan[3].pad;
    const bool col1Unread = (k.tg[7].pad[0] & 1) != 0;
    const bool col0Unread = (k.tg[7].pad[0] & 2) != 0;   // MSC_VGX_LEANVARY: the fragment shader of the draw does not read colour 0 either
    bool needNrm = false, needLights = false, needCol[2] = {false, false}, needPal = k.perVtxPos != 0;
    bool needTm = false, needPtm = false, needTmi = false;
    uint8_t texSrcMask = 0;
    int maxLight = -1;
    for (int i = 0; i < 4; ++i) {
        const VsKey::Chan& c = k.chan[i];
        if ((i & 1) >= k.numChans) continue;
        if (c.matSrcVtx || (c.enable && c.ambSrcVtx)) needCol[i & 1] = true;
        if (c.enable && c.lightMask) {
            needLights = true;
            if (c.diffuse || c.attn == 1) needNrm = true;
            for (int l = 0; l < 8; ++l)
                if (c.lightMask & (1u << l)) maxLight = l > maxLight ? l : maxLight;
        }
    }
    if (k.numChans < 1) needCol[0] = true;
    if (k.numChans < 2) needCol[1] = true;
    bool col1Vary = !col1Unread;   // (a colour texgen reads colour 1 as well)
    for (int i = 0; i < k.numTexGens; ++i)
        if (k.tg[i].type == 3) col1Vary = true;
    bool col0Vary = !col0Unread;   // (a colour texgen reads colour 0 as well)
    for (int i = 0; i < k.numTexGens; ++i)
        if (k.tg[i].type == 2) col0Vary = true;
    if (k.numChans < 2 && !col1Vary) needCol[1] = false;
    if (k.numChans < 1 && !col0Vary) needCol[0] = false;
    for (int i = 0; i < k.numTexGens; ++i) {
        const VsKey::TexGen& t = k.tg[i];
        if (t.type >= 2) continue;
        if (t.src == 1) needNrm = true;
        if (t.src >= 5 && t.src <= 12) texSrcMask |= static_cast<uint8_t>(1u << (t.src - 5));
        if (t.perVtxMtx) { needPal = true; needTmi = true; }
        else needTm = true;
        if (t.post) needPtm = true;
    }

    // W5 SKIN: the position and normal the transform below starts from are the skinned ones (sum over four bones of weight x bone matrix x
    // the vertex as stored), in model space like the CPU skinning's output; every use of the stored ones (texgens included) takes them.
    const bool skin = k.chan[0].skin != 0;
    const char* const posXY = skin ? "p.xy" : "a_pos.xy";
    const char* const posZ = skin ? "p.z" : "a_pos.z";
    const char* const nrmV = skin ? "sn" : "a_nrm";
    o("attribute vec4 a_pos;\n");
    if (needNrm) o("attribute vec3 a_nrm;\n");
    if (skin) o("attribute vec4 a_bix;\nattribute vec4 a_bw;\nuniform vec4 u_bone[%u];\n", SkinBones(k.chan[0].skin) * 3u);
    if (needCol[0]) o("attribute vec4 a_col0;\n");
    if (needCol[1]) o("attribute vec4 a_col1;\n");
    for (int t = 0; t < 8; ++t)
        if (texSrcMask & (1u << t)) o("attribute vec2 a_tex%d;\n", t);
    if (needTmi) o("attribute vec4 a_tmi0;\nattribute vec4 a_tmi1;\n");
    o("uniform mat4 u_proj;\n");
    const bool shortPal = k.perVtxPos == 2;
    const int palPos = PalPosRows(k); // position rows of a short palette (the normal rows follow them)
    if (needPal) o("uniform vec4 u_pal[%d];\n", shortPal ? 2 * palPos : kPalRowsFull);
    if (!k.perVtxPos) o("uniform vec4 u_mv[6];\n");
    if (needTm) o("uniform vec4 u_tm[%d];\n", k.numTexGens * 3);
    if (needPtm) o("uniform vec4 u_ptm[%d];\n", k.numTexGens * 3);
    o("uniform vec4 u_lt[%d];\n", 4 + (maxLight + 1) * 5);
    if (!col0Unread) o("varying vec4 v_col0;\n");
    if (!col1Unread) o("varying vec4 v_col1;\n");
    // MSC_VGX_TEX2D (VSP_TEX2): a coordinate without the projective bit writes two components, the two the fragment shader reads (FSP_TEX2).
    // (A colour texgen stays vec3: its key has no projective bit to read, and a fragment shader that divides by the third component needs it. The other way round, three
    // components written and two read, is the same as a varying that a fragment shader does not declare at all, which every draw without colour 1 has.)
    auto Tex2 = [&](int i) { return (k.chan[0].pad & VSP_TEX2) && k.tg[i].type < 2 && !k.tg[i].proj; };
    for (int i = 0; i < k.numTexGens; ++i) o("varying %s v_tex%d;\n", Tex2(i) ? "vec2" : "vec3", i);
    if (k.tg[7].pad[1] & VSK_FOGZ) o("varying float v_fz;\n"); // MSC_VGX_FOGVTX
    if (!skin) {
        o("void main() {\n  vec4 p = vec4(a_pos.xyz, 1.0);\n");
    } else {
        o("void main() {\n  vec4 p0 = vec4(a_pos.xyz, 1.0);\n");
        static const char* const lane = "xyzw";
        for (int b = 0; b < 4; ++b) {
            char var[4] = {'b', static_cast<char>('0' + b), 0, 0};
            char w[10];
            std::snprintf(w, sizeof w, "a_bix.%c", lane[b]);
            RowIndex(o, "  ", var, w);
        }
        o("  vec4 p = vec4(a_bw.x * vec3(dot(u_bone[b0], p0), dot(u_bone[b0 + 1], p0), dot(u_bone[b0 + 2], p0))"
          " + a_bw.y * vec3(dot(u_bone[b1], p0), dot(u_bone[b1 + 1], p0), dot(u_bone[b1 + 2], p0))"
          " + a_bw.z * vec3(dot(u_bone[b2], p0), dot(u_bone[b2 + 1], p0), dot(u_bone[b2 + 2], p0))"
          " + a_bw.w * vec3(dot(u_bone[b3], p0), dot(u_bone[b3 + 1], p0), dot(u_bone[b3 + 2], p0)), 1.0);\n");
        if (needNrm)
            o("  vec3 sn = a_bw.x * vec3(dot(u_bone[b0].xyz, a_nrm), dot(u_bone[b0 + 1].xyz, a_nrm), dot(u_bone[b0 + 2].xyz, a_nrm))"
              " + a_bw.y * vec3(dot(u_bone[b1].xyz, a_nrm), dot(u_bone[b1 + 1].xyz, a_nrm), dot(u_bone[b1 + 2].xyz, a_nrm))"
              " + a_bw.z * vec3(dot(u_bone[b2].xyz, a_nrm), dot(u_bone[b2 + 1].xyz, a_nrm), dot(u_bone[b2 + 2].xyz, a_nrm))"
              " + a_bw.w * vec3(dot(u_bone[b3].xyz, a_nrm), dot(u_bone[b3 + 1].xyz, a_nrm), dot(u_bone[b3 + 2].xyz, a_nrm));\n");
    }
    if (k.perVtxPos) {
        const bool nbPi = RowIndexClamped(o, "  ", "pi", "a_pos.w", shortPal ? palPos - 3 : 61);
        if (!nbPi) {
            RowIndex(o, "  ", "pi", "a_pos.w");
            if (shortPal) o("  if (pi > %d) pi = %d;\n", palPos - 3, palPos - 3);
            else o("  if (pi > 61) pi = 61;\n");
        }
        o("  vec3 eye = vec3(dot(u_pal[pi], p), dot(u_pal[pi + 1], p), dot(u_pal[pi + 2], p));\n");
        if (needNrm) {
            if (shortPal) o("  int ni = %d + pi;\n", palPos); // (position rows 0..palPos-1, then the normal rows)
            else if (nbPi) o("  int ni = 64 + int(min(pif, 29.0));\n");   // (NOBRANCH: pi > 29 ? 93 : 64 + pi; pi is already clamped to 61)
            else o("  int ni = pi > 29 ? 93 : 64 + pi;\n");
            o("  vec3 n = vec3(dot(u_pal[ni].xyz, %s), dot(u_pal[ni + 1].xyz, %s), dot(u_pal[ni + 2].xyz, %s));\n", nrmV, nrmV, nrmV);
        }
    } else {
        o("  vec3 eye = vec3(dot(u_mv[0], p), dot(u_mv[1], p), dot(u_mv[2], p));\n");
        if (needNrm) o("  vec3 n = vec3(dot(u_mv[3].xyz, %s), dot(u_mv[4].xyz, %s), dot(u_mv[5].xyz, %s));\n", nrmV, nrmV, nrmV);
    }
    if (needNrm) {
        if (VsNoBranchOn()) o(VGX_NOBRANCH_BREAK == 2 ? "  float nl = length(n);\n  n = n / (nl + step(nl, 1.0e-3));\n" : "  float nl = length(n);\n  n = n / (nl + step(nl, 0.0));\n");   // (nl is 0 or more: step(nl, 0.0) is 1 only for nl == 0, which divides by exactly 1)
        else o("  float nl = length(n);\n  if (nl > 0.0) n = n / nl;\n");
    }
    o("  gl_Position = u_proj * vec4(eye, 1.0);\n");
    if (k.tg[7].pad[1] & VSK_FOGZ) o("  v_fz = eye.z;\n");

    // Lighting channels.
    for (int ch = 0; ch < 2; ++ch) {
        if (ch == 0 && !col0Vary) continue;   // (MSC_VGX_LEANVARY: nobody reads it)
        if (ch >= k.numChans) {
            if (ch == 1 && !col1Vary) continue;
            o("  vec4 col%d = a_col%d;\n", ch, ch);
            continue;
        }
        o("  vec4 col%d;\n  {\n", ch);
        for (int part = 0; part < 2; ++part) { // 0 colour, 1 alpha
            const VsKey::Chan& c = k.chan[ch + part * 2];
            const char* sw = part ? "a" : "rgb";
            const char* ty = part ? "float" : "vec3";
            const char* nm = part ? "la" : "lc";
            if (c.enable) {
                if (c.ambSrcVtx) o("    %s %s = a_col%d.%s;\n", ty, nm, ch, sw);
                else o("    %s %s = u_lt[%d].%s;\n", ty, nm, ch, sw);
                for (int l = 0; l < 8; ++l) {
                    if (!(c.lightMask & (1u << l))) continue;
                    const int b = 4 + l * 5; // pos, dir, col, cosatt, distatt
                    const bool one = (cosOne >> l) & 1, dOne = (distOne >> l) & 1;
                    if (c.attn == 2 && ((farDir >> l) & 1)) o("    {\n      vec3 ld = u_lt[%d].xyz;\n      ld = ld / sqrt(dot(ld, ld));\n      float at = 1.0;\n", b);
                    else
                    o("    {\n      vec3 ld = u_lt[%d].xyz - eye;\n      float d2 = dot(ld, ld);\n      float d1 = sqrt(d2);\n"
                      "%s      float at = 1.0;\n", b, VsNoBranchLightOn() ? (VGX_NOBRANCH_BREAK == 3 ? "      ld = ld / d1;\n" : "      ld = ld / (d1 + step(d1, 0.0));\n") : "      if (d1 > 0.0) ld = ld / d1;\n");
                    if (c.attn == 2 && one && dOne) {
                        // (max(0, 1 + 0 ca + 0 ca ca) / (1 + 0 d1 + 0 d2) is exactly 1 for every finite ca, d1, d2)
                    } else if (c.attn == 2 && one) {
                        o("      at = 1.0 / dot(u_lt[%d].xyz, vec3(1.0, d1, d2));\n", b + 4);
                    } else if (c.attn == 2 && dOne) {
                        o("      float ca = max(0.0, dot(ld, u_lt[%d].xyz));\n", b + 1);
                        o("      at = max(0.0, u_lt[%d].x + u_lt[%d].y * ca + u_lt[%d].z * ca * ca);\n", b + 3, b + 3, b + 3);
                    } else if (c.attn == 2) {
                        o("      float ca = max(0.0, dot(ld, u_lt[%d].xyz));\n", b + 1);
                        o("      at = max(0.0, u_lt[%d].x + u_lt[%d].y * ca + u_lt[%d].z * ca * ca) / dot(u_lt[%d].xyz, vec3(1.0, d1, d2));\n", b + 3, b + 3,
                          b + 3, b + 4);
                    } else if (c.attn == 1) {
                        if (VsNoBranchLightOn()) o("      float ca = step(0.0, dot(n, ld)) * max(0.0, dot(n, u_lt[%d].xyz));\n", b + 1);   // (the gate as 0 or 1: the same for finite dot products)
                        else o("      float ca = dot(n, ld) >= 0.0 ? max(0.0, dot(n, u_lt[%d].xyz)) : 0.0;\n", b + 1);
                        if (c.diffuse == 0) o("      vec3 da = u_lt[%d].xyz;\n", b + 4);
                        else o("      vec3 da = normalize(u_lt[%d].xyz);\n", b + 4);
                        o("      at = max(0.0, dot(u_lt[%d].xyz, vec3(1.0, ca, ca * ca))) / dot(da, vec3(1.0, ca, ca * ca));\n", b + 3);
                    }
                    if (c.diffuse == 1) o("      at *= dot(ld, n);\n");
                    else if (c.diffuse == 2) o("      at *= max(0.0, dot(ld, n));\n");
                    o("      %s += at * u_lt[%d].%s;\n    }\n", nm, b + 2, sw);
                }
                // (MSC_VGX_VS_TRIM: no light can take away: ambient and light colours are 8-bit, so the sum stays at or above 0 when every `at` does)
                bool sumNonNeg = (cosOne | distOne) != 0;
                for (int l = 0; l < 8 && sumNonNeg; ++l)
                    if ((c.lightMask >> l) & 1)
                        sumNonNeg = c.diffuse != 1 && (c.attn == 0 || (c.attn == 2 && ((distOne >> l) & 1)));
                if (sumNonNeg) o("    %s = min(%s, %s(1.0));\n", nm, nm, ty);
                else
                o("    %s = clamp(%s, 0.0, 1.0);\n", nm, nm);
            } else {
                o("    %s %s = %s(1.0);\n", ty, nm, ty);
            }
            if (c.matSrcVtx) o("    col%d.%s = a_col%d.%s * %s;\n", ch, sw, ch, sw, nm);
            else o("    col%d.%s = u_lt[%d].%s * %s;\n", ch, sw, 2 + ch, sw, nm);
        }
        o("  }\n");
    }
    if (!col0Unread) o("  v_col0 = col0;\n");
    if (!col1Unread) o("  v_col1 = col1;\n");

    // Texture coordinate generation.
    for (int i = 0; i < k.numTexGens; ++i) {
        const VsKey::TexGen& t = k.tg[i];
        if (t.type >= 2) {
            o("  v_tex%d = vec3(col%d.rg, 1.0);\n", i, t.type - 2);
            continue;
        }
        o("  {\n");
        if (t.src == 0) o("    vec4 srcv = vec4(%s, %s, 1.0);\n", posXY, t.inputAbc1 ? posZ : "1.0");
        else if (t.src == 1) o("    vec4 srcv = vec4(%s.xy, %s, 1.0);\n", nrmV, t.inputAbc1 ? (skin ? "sn.z" : "a_nrm.z") : "1.0");
        else if (t.src >= 5 && t.src <= 12) o("    vec4 srcv = vec4(a_tex%d, 1.0, 1.0);\n", t.src - 5);
        else o("    vec4 srcv = vec4(0.0, 0.0, 1.0, 1.0);\n");
        if (t.perVtxMtx) {
            char tmi[16];
            std::snprintf(tmi, sizeof tmi, "a_tmi%d.%c", i / 4, "xyzw"[i & 3]);
            if (!RowIndexClamped(o, "    ", "ti", tmi, 61)) {
                RowIndex(o, "    ", "ti", tmi);
                o("    if (ti > 61) ti = 61;\n");
            }
            if (t.proj) o("    vec3 tc = vec3(dot(u_pal[ti], srcv), dot(u_pal[ti + 1], srcv), dot(u_pal[ti + 2], srcv));\n");
            else o("    vec3 tc = vec3(dot(u_pal[ti], srcv), dot(u_pal[ti + 1], srcv), 1.0);\n");
        } else {
            if (t.proj) o("    vec3 tc = vec3(dot(u_tm[%d], srcv), dot(u_tm[%d], srcv), dot(u_tm[%d], srcv));\n", i * 3, i * 3 + 1, i * 3 + 2);
            else o("    vec3 tc = vec3(dot(u_tm[%d], srcv), dot(u_tm[%d], srcv), 1.0);\n", i * 3, i * 3 + 1);
        }
        if (t.post) {
            if (t.post == 2) {
                if (VsNoBranchOn()) o("    float tl = length(tc);\n    tc = tc / (tl + step(tl, 0.0));\n");
                else o("    float tl = length(tc);\n    if (tl > 0.0) tc = tc / tl;\n");
            }
            o("    vec4 q = vec4(tc, 1.0);\n    tc = vec3(dot(u_ptm[%d], q), dot(u_ptm[%d], q), dot(u_ptm[%d], q));\n", i * 3, i * 3 + 1, i * 3 + 2);
        }
        o(Tex2(i) ? "    v_tex%d = tc.xy;\n  }\n" : "    v_tex%d = tc;\n  }\n", i);
    }
    o("}\n");
    if (VsNoBranchOn()) ++g_gen.noBranchVs;
    GenStat(o.s, false);
    src.text = std::move(o.s);
    src.usesPalette = needPal;
    src.usesLights = needLights;
    src.numTexGens = k.numTexGens;
    src.nMv = k.perVtxPos ? 0 : 6;
    src.nTm = needTm ? static_cast<uint8_t>(k.numTexGens * 3) : 0;
    src.nPtm = needPtm ? static_cast<uint8_t>(k.numTexGens * 3) : 0;
    src.nPal = needPal ? static_cast<uint8_t>(shortPal ? 2 * palPos : kPalRowsFull) : 0;
    src.nLt = static_cast<uint8_t>(4 + (maxLight + 1) * 5);
    src.nBone = skin ? static_cast<uint8_t>(SkinBones(k.chan[0].skin) * 3u) : 0;
    src.cZero = VsZeroFacts(k);
    src.vkey = std::make_shared<const VsKey>(k);
    for (int i = 0; i < k.numTexGens; ++i) {
        const VsKey::TexGen& t = k.tg[i];
        if (t.type < 2 && t.proj && !t.perVtxMtx && !t.post) {
            src.cProjPlain |= static_cast<uint8_t>(1u << i);
            if (t.src >= 2 || !t.inputAbc1) src.cProjZ1 |= static_cast<uint8_t>(1u << i);
        }
    }
}

} // namespace vgx
