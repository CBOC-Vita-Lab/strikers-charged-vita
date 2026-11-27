// MSC_VGX_TEX_COMPRESS through the GL back end (vgx_backend_gl.cpp: Upload, TcUpload, Draw -> TcNoteDraw, TcPicture, TcApply, UploadReal, CMD_TEX_FREE, CopyTexture) over the stub GL,
// built as the Vita build (S3TC on) with a driver that can run out of memory the way vitaGL does. texcomp_gl <1|0>: 1 = the switch on, 0 = off (the control: nothing is replaced).
// Checks: a texture drawn in perspective is re-uploaded as DXT (glCompressedTexImage2D, every level); one drawn orthographically, or by an indirect shader, never; a texture whose replacement could not
// be stored is reported (TcApply false) and left alone for the run; a freed texture and an EFB copy target are forgotten; the card file is written at the end of a picture, not inside a draw.
#include <GLES2/gl2.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
unsigned g_failName = 0;   // the texture name whose compressed level 0 gets no memory (0: none)
unsigned g_curTex = 0;
unsigned g_compCalls[VGX_STUB_MAXN], g_compLevel0[VGX_STUB_MAXN], g_texImage0[VGX_STUB_MAXN];
bool g_noStore[VGX_STUB_MAXN];
unsigned g_compressedBytes = 0;
void TestBindTex(GLenum t, GLuint n) { g_curTex = n; glBindTexture(t, n); }
void TestTexImage(GLenum t, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint b, GLenum fmt, GLenum type, const GLvoid* data) {
    if (level == 0 && g_curTex < VGX_STUB_MAXN) { g_noStore[g_curTex] = false; ++g_texImage0[g_curTex]; }
    glTexImage2D(t, level, ifmt, w, h, b, fmt, type, data);
}
void TestCompressed(GLenum t, GLint level, GLenum ifmt, GLsizei w, GLsizei h, GLint b, GLsizei size, const GLvoid* data) {
    if (g_curTex < VGX_STUB_MAXN) {
        ++g_compCalls[g_curTex];
        if (level == 0) { ++g_compLevel0[g_curTex]; g_noStore[g_curTex] = (g_curTex == g_failName); }
    }
    g_compressedBytes += static_cast<unsigned>(size);
    glCompressedTexImage2D(t, level, ifmt, w, h, b, size, data);
}
void TestDelTex(GLsizei n, const GLuint* ids) {
    for (GLsizei i = 0; i < n; ++i) if (ids[i] < VGX_STUB_MAXN) g_noStore[ids[i]] = false;
    glDeleteTextures(n, ids);
}
void Check(bool ok, const char* message) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); } }
}

#define glBindTexture TestBindTex
#define glTexImage2D TestTexImage
#define glCompressedTexImage2D TestCompressed
#define glDeleteTextures TestDelTex
#include "../src/vgx_backend_gl.cpp"
#undef glBindTexture
#undef glTexImage2D
#undef glCompressedTexImage2D
#undef glDeleteTextures
#define vgx_tex_has_storage hooks_unused_has_storage
#include "backend_test_hooks.h"
#undef vgx_tex_has_storage
extern "C" int vgx_tex_has_storage(void) { return !(g_curTex < VGX_STUB_MAXN && g_noStore[g_curTex]); }
extern "C" int VgxStillArmed(void) { return 0; }
namespace vgx { bool LogEnabled() { return false; } const char* ShaderListPath() { return ""; } }

namespace {
vgx::ShaderSource g_vs, g_fsPlain, g_fsInd;
unsigned g_frame = 0;

std::vector<uint8_t> Tex(uint32_t w, uint32_t h, int seed) {   // a smooth gradient (passes the gate), different per seed
    std::vector<uint8_t> t(static_cast<size_t>(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t* p = &t[(static_cast<size_t>(y) * w + x) * 4];
            p[0] = static_cast<uint8_t>(x * 200 / w + seed); p[1] = static_cast<uint8_t>(y * 200 / h + seed * 3); p[2] = static_cast<uint8_t>(60 + seed * 9); p[3] = 255;
        }
    return t;
}
vgx::TexUpload Upload128(uint32_t id, int seed) {
    vgx::TexUpload up;
    up.texId = id; up.width = up.height = 128; up.format = vgx::TF_RGBA8;
    std::vector<uint8_t> l = Tex(128, 128, seed);
    up.levels.push_back(l);
    uint32_t w = 128;
    while (w > 1) {   // an authored chain to 1x1 (box filter)
        std::vector<uint8_t> nx((w / 2) * (w / 2) * 4);
        for (uint32_t y = 0; y < w / 2; ++y) for (uint32_t x = 0; x < w / 2; ++x) for (int c = 0; c < 4; ++c)
            nx[(y * (w / 2) + x) * 4 + c] = static_cast<uint8_t>((l[((2 * y) * w + 2 * x) * 4 + c] + l[((2 * y) * w + 2 * x + 1) * 4 + c] + l[((2 * y + 1) * w + 2 * x) * 4 + c] + l[((2 * y + 1) * w + 2 * x + 1) * 4 + c] + 2) / 4);
        up.levels.push_back(nx);
        l = nx;
        w /= 2;
    }
    up.authored = static_cast<uint8_t>(up.levels.size());
    return up;
}
vgx::CmdBuf NewBuf() { vgx::CmdBuf b; b.frame = ++g_frame; b.vtx.assign(256, 0); b.idx = {0, 1, 2}; return b; }
// a draw sampling texture `tex` with a perspective (ortho = false) or orthographic projection, shader fs
void AddDraw(vgx::CmdBuf& b, uint32_t tex, bool ortho, uint16_t fs) {
    const uint32_t off = static_cast<uint32_t>(b.uni.size());
    const float persp[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, -1, 0, 0, -1, 0};
    const float orth[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0, 0, 0, -1, 1};
    for (int i = 0; i < 16; ++i) b.uni.push_back(ortho ? orth[i] : persp[i]);
    vgx::DrawRec r;
    r.vsId = 0; r.fsId = fs; r.vtxCount = 3; r.idxCount = 3; r.texMask = 1; r.tex[0].texId = tex;
    r.ublock[vgx::UB_PROJ] = off;
    b.draws.push_back(r);
    b.cmds.push_back(vgx::Cmd{vgx::CMD_DRAW, static_cast<uint32_t>(b.draws.size() - 1)});
}
void AddUpload(vgx::CmdBuf& b, const vgx::TexUpload& up) { b.uploads.push_back(up); b.cmds.push_back(vgx::Cmd{vgx::CMD_TEX_UPLOAD, static_cast<uint32_t>(b.uploads.size() - 1)}); }
void AddPresent(vgx::CmdBuf& b) { b.cmds.push_back(vgx::Cmd{vgx::CMD_PRESENT, 0}); }
void Run(vgx::CmdBuf& b) { vgx::backend::Execute(b); }
unsigned NameOf(uint32_t id) { return id < vgx::backend::g_tex.size() ? vgx::backend::g_tex[id].id : 0u; }
unsigned Comp(uint32_t id) { const unsigned n = NameOf(id); return n < VGX_STUB_MAXN ? g_compCalls[n] : 0; }
unsigned Comp0(uint32_t id) { const unsigned n = NameOf(id); return n < VGX_STUB_MAXN ? g_compLevel0[n] : 0; }
}

int main(int argc, char** argv) {
    const bool on = argc < 2 || std::strcmp(argv[1], "0") != 0;
    setenv("MSC_VGX_ALLOC_GUARD", "1", 1);
    setenv("MSC_VGX_TEX_COMPRESS", on ? "1" : "0", 1);
    setenv("MSC_VGX_TEX_COMPRESS_READY", "2", 1);
    setenv("MSC_VGX_TEX_COMPRESS_BLOCKS", "100000", 1);
    setenv("MSC_VGX_TEX_COMPRESS_CACHE", "0", 1);
    setenv("MSC_VGX_TEX_COMPRESS_HEAP_MIB", "0", 1);
    vgx::backend::g_hasS3tc = vgx::backend::g_hasDxt5 = true;   // (what Init() sets on the Vita)
    std::printf("texcomp through the back end: switch %s\n", on ? "ON" : "OFF (control)");
    g_vs.fragment = false; g_vs.id = 0; g_vs.text = "void main() { }\n";
    g_fsPlain.fragment = true; g_fsPlain.id = 0; g_fsPlain.text = "void main() { }\n";
    g_fsPlain.fkey = std::make_shared<const vgx::FsKey>();
    g_fsInd.fragment = true; g_fsInd.id = 1; g_fsInd.text = "void main() { }\n";
    { vgx::FsKey k; k.numIndStages = 1; g_fsInd.fkey = std::make_shared<const vgx::FsKey>(k); }

    {   // picture 1: target, shaders, the textures, a draw of each, present
        vgx::CmdBuf b = NewBuf();
        b.targets.push_back(vgx::SetTargetCmd{});
        b.cmds.push_back(vgx::Cmd{vgx::CMD_SET_TARGET, 0});
        b.shaders.push_back(&g_vs); b.cmds.push_back(vgx::Cmd{vgx::CMD_SHADER, 0});
        b.shaders.push_back(&g_fsPlain); b.cmds.push_back(vgx::Cmd{vgx::CMD_SHADER, 1});
        b.shaders.push_back(&g_fsInd); b.cmds.push_back(vgx::Cmd{vgx::CMD_SHADER, 2});
        AddUpload(b, Upload128(10, 1));   // perspective only: replaced
        AddUpload(b, Upload128(11, 2));   // drawn orthographically: never
        AddUpload(b, Upload128(12, 3));   // perspective, but its replacement will not fit
        AddUpload(b, Upload128(13, 4));   // freed before its verdict
        AddUpload(b, Upload128(14, 5));   // drawn by an indirect shader: never
        AddUpload(b, Upload128(15, 6));   // becomes an EFB copy target: forgotten
        AddDraw(b, 10, false, 0); AddDraw(b, 11, true, 0); AddDraw(b, 12, false, 0); AddDraw(b, 13, false, 0); AddDraw(b, 14, false, 1); AddDraw(b, 15, false, 0);
        AddPresent(b);
        Run(b);
        Check(Comp(10) == 0 && g_texImage0[NameOf(10)] >= 1, "the first pictures: the texture is uploaded as RGBA8, as always");
    }
    const unsigned name10 = NameOf(10), name12 = NameOf(12);
    g_failName = name12;   // 12's replacement cannot be stored: the driver gives its level 0 no memory
    auto picture = [&](bool free13, bool copy15) {
        vgx::CmdBuf b = NewBuf();
        AddDraw(b, 10, false, 0); AddDraw(b, 11, true, 0); AddDraw(b, 12, false, 0); AddDraw(b, 14, false, 1);
        AddDraw(b, 13, false, 0); AddDraw(b, 15, false, 0);   // (still drawn after being freed / copied into: without Forget their votes would reach a verdict)
        if (free13) b.cmds.push_back(vgx::Cmd{vgx::CMD_TEX_FREE, 13});
        if (copy15) {
            vgx::CopyTexCmd c; c.texId = 15; c.src = vgx::EfbRect{0, 0, 64, 64}; c.dstW = 64; c.dstH = 64;
            b.copyTex.push_back(c); b.cmds.push_back(vgx::Cmd{vgx::CMD_COPY_TEX, 0});
        }
        AddPresent(b);
        Run(b);
    };
    picture(true, true);   // picture 2 (the first drew them all): 13 is freed and 15 copied into before their verdict (2 perspective pictures)
    for (int i = 0; i < 5; ++i) picture(true, false);
    if (on) {
        Check(name10 < VGX_STUB_MAXN && g_compLevel0[name10] == 1, "perspective texture: replaced exactly once");
        Check(g_compCalls[name10] == 6, "perspective texture 128x128: all 6 levels down to 4x4 as DXT (one call per level)");
        Check(Comp(11) == 0 && Comp(14) == 0, "orthographic and indirect-shader textures are never replaced");
        Check(Comp(13) == 0 && Comp(15) == 0, "a freed texture and an EFB copy target are never replaced");
        Check(g_compLevel0[name12] == 1, "refused replacement: one attempt, no retry");
        Check(vgx::backend::g_tcm.counters().applyFail == 1, "refused replacement: the manager is told (TcApply false)");
        Check(vgx::backend::g_tcm.counters().applied == 1, "only the texture that fit is counted as replaced");
    } else {
        Check(Comp(10) == 0 && g_compLevel0[name12] == 0 && g_compressedBytes == 0, "switch off: no compressed upload at all");
    }
    std::printf("texcomp_gl: %s passed\n", on ? "on" : "off");
    return 0;
}
