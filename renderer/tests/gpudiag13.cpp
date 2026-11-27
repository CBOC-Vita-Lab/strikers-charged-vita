// gpudiag13 (vgx_gpudiag13.h): checks of the pure parts.   gpudiag13 [--break=N]
//   N=1: a destination-alpha blend is not an alpha reader (the negative control of MASKFULL must then FAIL);  N=2: a copy whose matrix takes alpha is not a reader;
//   N=3: a whole-target alpha clear does not kill (liveness never ends) -> the "dead" cases must FAIL;  N=4: a read-modify-write alpha blend is not a reader.
// Pure: no GL.
#include "vgx_gpudiag13.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {
int g_fail = 0, g_pass = 0, g_break = 0;
void Check(bool ok, const char* what) {
    if (ok) { ++g_pass; std::printf("ok   %s\n", what); }
    else { ++g_fail; std::printf("FAIL %s\n", what); }
}
using vgx::RenderState;
bool DrawReads(const RenderState& rs) {
    if (g_break == 1 && rs.blend && (gd13::DstAlphaFactor(rs.blendSrc) || gd13::DstAlphaFactor(rs.blendDst))) return false;
    if (g_break == 4 && rs.blend && rs.alphaWrite && rs.blendDst != vgx::BF_ZERO && !gd13::DstAlphaFactor(rs.blendSrc) && !gd13::DstAlphaFactor(rs.blendDst)) return false;
    return gd13::DrawReadsAlpha(rs);
}
bool CopyReads(const float m[4][4]) { return g_break == 2 ? false : gd13::CopyReadsAlpha(m); }
}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (!std::strncmp(argv[i], "--break=", 8)) g_break = std::atoi(argv[i] + 8);
    // ---- drop classes
    Check(gd13::ParseDropClass("T") == gd13::DC_T && gd13::ParseDropClass("B") == gd13::DC_B && gd13::ParseDropClass("D") == gd13::DC_D, "class letters");
    Check(gd13::ParseDropClass("tb") == (gd13::DC_T | gd13::DC_B) && gd13::ParseDropClass("") == 0 && gd13::ParseDropClass(nullptr) == 0 && gd13::ParseDropClass("x") == 0, "class combinations and junk");
    {
        RenderState opaque;            // colour + alpha, no blend
        RenderState blended; blended.blend = 1;
        RenderState depthOnly; depthOnly.colorWrite = 0; depthOnly.alphaWrite = 0;
        Check(gd13::DrawClass(opaque, false) == 0, "an opaque draw is in no class");
        Check(gd13::DrawClass(opaque, true) == gd13::DC_T, "an alpha-tested unblended draw is T");
        Check(gd13::DrawClass(blended, true) == gd13::DC_B && gd13::DrawClass(blended, false) == gd13::DC_B, "a blended draw is B, tested or not");
        Check(gd13::DrawClass(depthOnly, false) == gd13::DC_D, "a draw with no colour and no alpha write is D");
    }
    // ---- bisect texts
    {
        const std::string fs =
            "precision mediump float;\nuniform sampler2D u_tex0;\nuniform vec4 u_tev[28];\nvarying vec4 v_col0;\nvarying vec3 v_tex0;\nvoid main() {\n  vec4 tex = vec4(1.0);\n"
            "  vec2 uv0 = v_tex0.xy;\n  tex = texture2D(u_tex0, uv0 * (vec2(1.0) + vec2(0.0))).rgba;\n  vec3 ind = texture2D(u_tex0, uv0, u_tev[3].x).abg * 255.0;\n  gl_FragColor = tex * v_col0;\n}\n";
        std::string out;
        const char* why = "";
        Check(gd13::BisectText(fs, 1, out, &why) && out.find("gl_FragColor = vec4(v_col0);") != std::string::npos && out.find("texture2D") == std::string::npos && out.find("uniform sampler2D u_tex0;") != std::string::npos, "bisect 1: raster colour only, declarations kept");
        Check(gd13::BisectText(fs, 2, out, &why) && out.find("texture2D") == std::string::npos && out.find("vec4(0.5).rgba") != std::string::npos && out.find("vec4(0.5).abg * 255.0") != std::string::npos &&
                  out.find("gl_FragColor = tex * v_col0;") != std::string::npos && out.find("(vec2(1.0) + vec2(0.0))") == std::string::npos, "bisect 2: both reads (nested parentheses, bias form) are constants, the arithmetic stays");
        Check(!gd13::BisectText("void main() { gl_FragColor = vec4(1.0); }", 2, out, &why) && std::strcmp(why, "no texture read") == 0, "bisect 2 refuses a program with no read");
        Check(!gd13::BisectText("void main() { gl_FragDepth = 0.5; }", 1, out, &why) && std::strcmp(why, "writes depth") == 0, "bisect refuses a program that writes depth");
        Check(!gd13::BisectText(fs, 3, out, &why), "bisect refuses an unknown mode");
        Check(gd13::BisectText("varying vec3 v_tex0;\nvoid main() { }\n", 1, out, &why) && out.find("vec4(0.5)") != std::string::npos, "bisect 1 without a vertex colour writes a constant");
    }
    // ---- alpha readers
    {
        RenderState plain;
        Check(!DrawReads(plain), "an unblended draw reads no alpha");
        RenderState shadow; shadow.blend = 1; shadow.blendSrc = vgx::BF_DST_ALPHA; shadow.blendDst = vgx::BF_INV_DST_ALPHA;
        Check(DrawReads(shadow), "NEGATIVE CONTROL: a blend by the destination alpha reads it");
        RenderState src; src.blend = 1; src.blendSrc = vgx::BF_SRC_ALPHA; src.blendDst = vgx::BF_INV_SRC_ALPHA; src.alphaWrite = 0;
        Check(!DrawReads(src), "a source-alpha blend with alpha write off reads no destination alpha");
        RenderState chain = src; chain.alphaWrite = 1;
        Check(DrawReads(chain), "the same blend with alpha write on keeps part of the destination alpha (a chain): a reader");
        RenderState add; add.blend = 1; add.blendSrc = vgx::BF_SRC_ALPHA; add.blendDst = vgx::BF_ONE; add.alphaWrite = 0;
        Check(!DrawReads(add), "an additive blend with alpha write off is no reader");
        float id[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
        float noA[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 0}};
        Check(CopyReads(id), "a copy with the identity matrix reads alpha");
        Check(!CopyReads(noA), "a copy whose matrix ignores alpha reads none");
    }
    // ---- liveness: the plan of a picture, as MaskFullScan builds it
    //  0 draw A (candidate)  1 copy that ignores alpha  2 draw B (candidate)  3 shadow blend (reads)  4 display copy that clears alpha (kills)  5 draw C (candidate, next picture's)
    {
        uint8_t rd[6] = {0, 0, 0, 1, 0, 0}, kl[6] = {0, 0, 0, 0, 1, 0};
        if (g_break == 3) kl[4] = 0;
        std::vector<uint8_t> live;
        gd13::LiveBackward(rd, kl, 6, 1, live);
        Check(live[0] == 1 && live[2] == 1 && live[3] == 1, "draws before the shadow blend see the alpha live");
        Check(live[4] == 0, "after the read, the whole-target alpha clear ends the liveness (the display copy)");
        Check(live[5] == 1, "after the last kill the buffer's end is conservative (live)");
        uint8_t rd2[4] = {0, 0, 0, 0}, kl2[4] = {0, 0, 1, 0};
        if (g_break == 3) kl2[2] = 0;
        gd13::LiveBackward(rd2, kl2, 4, 1, live);
        Check(live[0] == 0 && live[1] == 0 && live[2] == 0, "no reader before a kill: the alpha of the draws before it is dead");
        uint8_t rd3[3] = {0, 1, 0}, kl3[3] = {0, 1, 0};
        gd13::LiveBackward(rd3, kl3, 3, 0, live);
        Check(live[1] == 1 && live[0] == 1 && live[2] == 0, "a command that reads and clears reads first");
    }
    // ---- candidates
    {
        RenderState a; a.alphaWrite = 0;
        Check(gd13::MaskCandidate(a), "colour on, alpha off, no blend: a candidate");
        RenderState b = a; b.blend = 1;
        RenderState c = a; c.colorWrite = 0;
        RenderState d = a; d.dstAlphaPass = 1;
        RenderState e; // alpha on
        Check(!gd13::MaskCandidate(b) && !gd13::MaskCandidate(c) && !gd13::MaskCandidate(d) && !gd13::MaskCandidate(e), "blended, no colour, alpha-only pass and alpha-on draws are not");
    }
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
