// gpudiag13 (wave 13): the pure parts of five GPU switches of the back end, kept apart from GL so that a host test (renderer/tests/gpudiag13.cpp) can check them.
//   MSC_VGX_SCENE_PROBE=<n>  (diagnostic, default 0, never shipped on) n empty 64x64 scenes a picture, 0..16: the price of one more scene on the console, from the [gpu scenes] line
//                            (a "64x64 xN/frame" kind) and the picture's GPU time. Galaxy's GXV_SCENE_PROBE.
//   MSC_VGX_DROP_CLASS=T|B|D (diagnostic, default off, the picture is WRONG on purpose) leave out the alpha-tested (T), blended (B) or depth-only (D) draws. Letters may be combined (TB).
//   MSC_VGX_GPU_BISECT=1|2   (diagnostic, default 0, picture wrong on purpose) opaque textured draws of the main scene: 1 = the fragment program writes the rasterised vertex colour only (no texture
//                            read, no TEV arithmetic); 2 = the program is kept but every texture read is a constant (no fetch).
//   MSC_VGX_MASKFULL=0|1|2   see the liveness helpers below (an unblended RGB draw with alpha write off writes all four channels where the EFB's alpha is dead).
//   MSC_VGX_TAILFOLD=0|2     (2 = census only) the tail of a picture after its last EFB copy: could the present take it into its own scene.
// Included in vgx_backend_gl.cpp (inside its anonymous namespace, before Draw) and by the unit test; includes nothing of GL.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "vgx_cmd.h"

namespace gd13 {

inline long EnvNum(const char* name, long def) {
    const char* e = std::getenv(name);
    return e && *e ? std::atol(e) : def;
}

// ---- MSC_VGX_DROP_CLASS
enum : uint8_t { DC_T = 1, DC_B = 2, DC_D = 4 };
inline uint8_t ParseDropClass(const char* e) {
    uint8_t m = 0;
    for (; e && *e; ++e) {
        if (*e == 'T' || *e == 't') m |= DC_T;
        else if (*e == 'B' || *e == 'b') m |= DC_B;
        else if (*e == 'D' || *e == 'd') m |= DC_D;
    }
    return m;
}
// The class of one draw: alpha-tested without blending (T), blended (B), colour and alpha writes both off (D, depth / stencil only). `alphaTested` = the fragment program discards.
// 0: not in any class. A blended alpha-tested draw is B.
inline uint8_t DrawClass(const vgx::RenderState& rs, bool alphaTested) {
    if (rs.blend) return DC_B;
    if (!rs.colorWrite && !rs.alphaWrite) return DC_D;
    if (alphaTested) return DC_T;
    return 0;
}

// ---- MSC_VGX_GPU_BISECT: transforms of a fragment program's text (GLSL ES 1.00 as vgx_shadergen.cpp writes it)
// Mode 1: everything from `void main() {` on is replaced by one statement. The vertex colour when the program declares it, else a constant.
// Mode 2: every `texture2D(...)` call (the whole call, with its nested parentheses) becomes `vec4(0.5)`.
// false when the text cannot be changed safely (a program that writes the depth, no `void main`, no texture read in mode 2): `why` names the reason.
inline bool BisectText(const std::string& in, int mode, std::string& out, const char** why) {
    *why = "";
    if (mode != 1 && mode != 2) { *why = "mode"; return false; }
    if (in.find("VGX_FRAGDEPTH") != std::string::npos || in.find("gl_FragDepth") != std::string::npos) { *why = "writes depth"; return false; }
    if (mode == 1) {
        const size_t m = in.find("void main()");
        if (m == std::string::npos) { *why = "no main"; return false; }
        const bool col0 = in.find("varying vec4 v_col0;") != std::string::npos || in.find("varying HV4 v_col0;") != std::string::npos;
        out = in.substr(0, m);
        out += col0 ? "void main() {\n  gl_FragColor = vec4(v_col0);\n}\n" : "void main() {\n  gl_FragColor = vec4(0.5);\n}\n";
        return true;
    }
    out.clear();
    out.reserve(in.size());
    size_t pos = 0;
    int n = 0;
    static const char kCall[] = "texture2D(";
    for (;;) {
        const size_t at = in.find(kCall, pos);
        if (at == std::string::npos) break;
        out.append(in, pos, at - pos);
        size_t i = at + sizeof(kCall) - 1;
        int depth = 1;
        while (i < in.size() && depth > 0) {
            if (in[i] == '(') ++depth;
            else if (in[i] == ')') --depth;
            ++i;
        }
        if (depth != 0) { *why = "unbalanced"; return false; }
        out += "vec4(0.5)";
        pos = i;
        ++n;
    }
    if (n == 0) { *why = "no texture read"; return false; }
    out.append(in, pos, std::string::npos);
    return true;
}

// ---- MSC_VGX_MASKFULL: where may an unblended draw write all four channels?
// The EFB's alpha plane is "read" by: a blended draw of the EFB whose factors use the destination alpha (BF_DST_ALPHA, BF_INV_DST_ALPHA), or that writes alpha and keeps part of the destination
// (alpha blended with a destination factor other than ZERO: a read-modify-write chain), and a copy of the EFB whose matrix takes alpha into any output row. It is killed by a clear of the alpha
// plane over the whole target. The alpha before command i is live when some command at or after i reads it before a kill.
inline bool DstAlphaFactor(uint8_t f) { return f == vgx::BF_DST_ALPHA || f == vgx::BF_INV_DST_ALPHA; }
inline bool DrawReadsAlpha(const vgx::RenderState& rs) {
    if (!rs.blend) return false;
    if (DstAlphaFactor(rs.blendSrc) || DstAlphaFactor(rs.blendDst)) return true;
    if (rs.alphaWrite && rs.blendDst != vgx::BF_ZERO) return true;   // (the new alpha holds part of the old one)
    if (rs.dstAlphaPass) return true;
    return false;
}
inline bool CopyReadsAlpha(const float mat[4][4]) {
    for (int r = 0; r < 4; ++r)
        if (mat[r][3] != 0.0f) return true;
    return false;
}
// live[i] for i in [0, n]: live[n] = endLive; live[i] = read[i] ? 1 : (kill[i] ? 0 : live[i+1]).
inline void LiveBackward(const uint8_t* read, const uint8_t* kill, int n, uint8_t endLive, std::vector<uint8_t>& live) {
    live.assign(static_cast<size_t>(n) + 1, 0);
    live[static_cast<size_t>(n)] = endLive;
    for (int i = n - 1; i >= 0; --i) live[static_cast<size_t>(i)] = read[i] ? 1 : (kill[i] ? 0 : live[static_cast<size_t>(i) + 1]);
}
// Reasons a candidate draw is refused (and the order they are tested in).
enum MaskWhy : uint8_t { MW_TAKEN, MW_NO_PLAN, MW_NOT_EFB, MW_STENCIL, MW_ALPHA_LIVE, MW_COUNT };
inline const char* const* MaskWhyNames() {
    static const char* const n[MW_COUNT] = {"taken", "no plan for this command", "not the EFB (pass, effects target, display)", "alpha-only pass", "alpha live later"};
    return n;
}
// A candidate: colour written, alpha not, no blend, no alpha-only second pass.
inline bool MaskCandidate(const vgx::RenderState& rs) { return rs.colorWrite && !rs.alphaWrite && !rs.blend && !rs.dstAlphaPass; }

struct MaskStat {
    uint64_t pictures = 0, buffers = 0, candidates = 0, taken = 0, why[MW_COUNT] = {};
    double candShare = 0, takenShare = 0;   // sums of the screen share (pixels of the draws' triangles / pixels of the picture) over the window, mode 2
    uint64_t unmeasured = 0;                 // candidates whose pixels could not be measured (mode 2)
    uint64_t readsBlend = 0, readsCopy = 0, kills = 0;   // alpha readers and whole-target alpha clears found by the scans
};

}  // namespace gd13
