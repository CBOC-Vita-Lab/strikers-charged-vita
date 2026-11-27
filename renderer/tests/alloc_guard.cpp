// 1.2 MSC_VGX_ALLOC_GUARD (vgx_backend_gl.cpp: AcquireFbo, CopyTexture, BeginPass, CopyTex, BeginFx, Upload): the GL back end over the stub GL
// with a driver that can run out the way vitaGL does: glGenFramebuffers / glGenTextures leave their output alone (0) when the name table is full,
// and glTexImage2D gives a texture no memory with no error (vgx_tex_has_storage() says so). The checks:
//   guard on  : a target or copy texture that could not be made is skipped; nothing is drawn into framebuffer 0 (the display), nothing is
//               attached that has no storage, and once the driver has room again the same commands work.
//   guard off : (a control) the 1.1 behaviour, which does all of those things; the test shows that it is the guard that prevents them.
//   alloc_guard <1|0>
#include <GLES2/gl2.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
bool g_failFbo = false, g_failTex = false, g_noStorage = false;
unsigned g_curFbo = 0, g_curTex = 0;
unsigned g_draws = 0, g_drawsOnFbo0 = 0, g_attachOnFbo0 = 0, g_attachNoStorage = 0, g_attachOk = 0, g_notes = 0;
bool g_noStore[VGX_STUB_MAXN];

void TestGenFbo(GLsizei n, GLuint* ids) { if (!g_failFbo) glGenFramebuffers(n, ids); }
void TestGenTex(GLsizei n, GLuint* ids) { if (!g_failTex) glGenTextures(n, ids); }
void TestBindFbo(GLenum t, GLuint f) { g_curFbo = f; glBindFramebuffer(t, f); }
void TestBindTex(GLenum t, GLuint n) { g_curTex = n; glBindTexture(t, n); }
void TestTexImage(GLenum t, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint b, GLenum fmt, GLenum type, const GLvoid* data) {
    if (level == 0 && g_curTex < VGX_STUB_MAXN) g_noStore[g_curTex] = g_noStorage;
    glTexImage2D(t, level, ifmt, w, h, b, fmt, type, data);
}
void TestDelTex(GLsizei n, const GLuint* ids) {
    for (GLsizei i = 0; i < n; ++i) if (ids[i] < VGX_STUB_MAXN) g_noStore[ids[i]] = false;
    glDeleteTextures(n, ids);
}
void TestAttach(GLenum t, GLenum a, GLenum tt, GLuint tex, GLint l) {
    if (g_curFbo == 0) ++g_attachOnFbo0;
    if (tex && tex < VGX_STUB_MAXN && g_noStore[tex]) ++g_attachNoStorage;
    else if (tex && g_curFbo) ++g_attachOk;
    glFramebufferTexture2D(t, a, tt, tex, l);
}
void CountDraw() { ++g_draws; if (g_curFbo == 0) ++g_drawsOnFbo0; }
void TestDrawElements(GLenum, GLsizei, GLenum, const GLvoid*) { CountDraw(); }
void TestDrawArrays(GLenum, GLint, GLsizei) { CountDraw(); }
void Check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
}

#define glGenFramebuffers TestGenFbo
#define glGenTextures TestGenTex
#define glBindFramebuffer TestBindFbo
#define glBindTexture TestBindTex
#define glTexImage2D TestTexImage
#define glDeleteTextures TestDelTex
#define glFramebufferTexture2D TestAttach
#define glDrawElements TestDrawElements
#define glDrawArrays TestDrawArrays
#include "../src/vgx_backend_gl.cpp"
#undef glGenFramebuffers
#undef glGenTextures
#undef glBindFramebuffer
#undef glBindTexture
#undef glTexImage2D
#undef glDeleteTextures
#undef glFramebufferTexture2D
#undef glDrawElements
#undef glDrawArrays
#define vgx_tex_has_storage hooks_unused_has_storage
#include "backend_test_hooks.h"
#undef vgx_tex_has_storage
extern "C" int vgx_tex_has_storage(void) { return !(g_curTex < VGX_STUB_MAXN && g_noStore[g_curTex]); }
extern "C" void NpPlatformNote(const char*) { ++g_notes; }

namespace vgx { bool LogEnabled() { return false; } const char* ShaderListPath() { return ""; } }   // (vgx_draw.cpp, not linked here)
extern "C" int VgxStillArmed(void) { return 0; }

namespace {
vgx::ShaderSource g_vs, g_fs;
unsigned g_frame = 0;

struct Snap {
    unsigned draws, onFbo0, attach0, attachNs, attachOk, notes;
};
Snap Take() { return Snap{g_draws, g_drawsOnFbo0, g_attachOnFbo0, g_attachNoStorage, g_attachOk, g_notes}; }

vgx::CmdBuf NewBuf() {
    vgx::CmdBuf b;
    b.frame = ++g_frame;
    b.vtx.assign(256, 0);
    b.idx = {0, 1, 2};
    return b;
}
void AddDraw(vgx::CmdBuf& b, bool fx = false) {
    vgx::DrawRec r;
    r.vsId = 0; r.fsId = 0; r.vtxCount = 3; r.idxCount = 3;
    if (fx) { r.fx = 1; r.rs.blend = 1; r.rs.blendSrc = vgx::BF_ONE; r.rs.blendDst = vgx::BF_ONE; }
    b.draws.push_back(r);
    b.cmds.push_back(vgx::Cmd{vgx::CMD_DRAW, static_cast<uint32_t>(b.draws.size() - 1)});
}
void AddCopy(vgx::CmdBuf& b, uint32_t texId, int srcW, int srcH, int dstW, int dstH, bool fromPass) {
    vgx::CopyTexCmd c;
    c.texId = texId;
    c.src = vgx::EfbRect{0, 0, static_cast<int16_t>(srcW), static_cast<int16_t>(srcH)};
    c.dstW = static_cast<uint16_t>(dstW);
    c.dstH = static_cast<uint16_t>(dstH);
    c.fromPass = fromPass;
    b.copyTex.push_back(c);
    b.cmds.push_back(vgx::Cmd{vgx::CMD_COPY_TEX, static_cast<uint32_t>(b.copyTex.size() - 1)});
}
void AddPass(vgx::CmdBuf& b, int w, int h) {
    vgx::PassCmd p;
    p.rect = vgx::EfbRect{0, 0, static_cast<int16_t>(w), static_cast<int16_t>(h)};
    b.passes.push_back(p);
    b.cmds.push_back(vgx::Cmd{vgx::CMD_PASS_BEGIN, static_cast<uint32_t>(b.passes.size() - 1)});
}
void Run(vgx::CmdBuf& b) { vgx::backend::Execute(b); }
void Reset() { g_failFbo = g_failTex = g_noStorage = false; }
}

int main(int argc, char** argv) {
    const bool guard = argc < 2 || std::strcmp(argv[1], "0") != 0;
    setenv("MSC_VGX_ALLOC_GUARD", guard ? "1" : "0", 1);
    setenv("MSC_FX_LOWRES", "always", 1);
    std::printf("allocation guard %s\n", guard ? "ON (default)" : "OFF (control: the 1.1 behaviour)");
    g_vs.fragment = false; g_vs.id = 0; g_vs.text = "void main() { }\n";
    g_fs.fragment = true; g_fs.id = 0; g_fs.text = "void main() { }\n";

    {   // frame 1: the render target, the shaders, a plain draw
        vgx::CmdBuf b = NewBuf();
        b.targets.push_back(vgx::SetTargetCmd{});
        b.cmds.push_back(vgx::Cmd{vgx::CMD_SET_TARGET, 0});
        b.shaders.push_back(&g_vs); b.cmds.push_back(vgx::Cmd{vgx::CMD_SHADER, 0});
        b.shaders.push_back(&g_fs); b.cmds.push_back(vgx::Cmd{vgx::CMD_SHADER, 1});
        AddDraw(b);
        Run(b);
        Check(g_draws == 1 && g_drawsOnFbo0 == 0 && g_attachOnFbo0 == 0, "the plain frame draws into the render target");
    }

    // 1. a copy texture that gets no memory (no GL error, no storage)
    {
        g_noStorage = true;
        Snap a = Take();
        vgx::CmdBuf b = NewBuf();
        AddCopy(b, 5, 640, 528, 300, 250, false);
        AddDraw(b);
        Run(b);
        Snap z = Take();
        std::printf("copy texture without memory: %u attached without storage, %u notes\n", z.attachNs - a.attachNs, z.notes - a.notes);
        if (guard) {
            Check(z.attachNs == a.attachNs, "guard: a texture without storage is never attached");
            Check(z.notes > a.notes, "guard: the failure is noted");
            Check(z.draws == a.draws + 1, "guard: the next draw is still made (into the render target)");
        } else {
            Check(z.attachNs > a.attachNs, "control: without the guard the texture is attached with no storage");
        }
        Reset();
        // the driver has room again: the same copy works
        a = Take();
        b = NewBuf();
        AddCopy(b, 5, 640, 528, 300, 250, false);
        Run(b);
        z = Take();
        Check(!guard || z.attachOk > a.attachOk, "after the failure the same copy is made (the copy texture attached)");   // (the control keeps the storage-less texture for good)
    }

    // 2. no framebuffer name for the copy's object (the output stays 0 = the display)
    {
        g_failFbo = true;
        Snap a = Take();
        vgx::CmdBuf b = NewBuf();
        AddCopy(b, 6, 640, 528, 200, 100, false);
        Run(b);
        Snap z = Take();
        std::printf("copy without a framebuffer name: %u attaches on framebuffer 0, %u notes\n", z.attach0 - a.attach0, z.notes - a.notes);
        if (guard) {
            Check(z.attach0 == a.attach0 && z.onFbo0 == a.onFbo0, "guard: nothing is attached to or drawn into framebuffer 0");
            Check(z.notes > a.notes, "guard: the failure is noted");
        } else {
            Check(z.attach0 > a.attach0, "control: without the guard the copy texture is attached to framebuffer 0");
        }
        Reset();
        a = Take();
        b = NewBuf();
        AddCopy(b, 6, 640, 528, 200, 100, false);
        Run(b);
        z = Take();
        Check(!guard || z.attachOk > a.attachOk, "after the failure the same copy is made");
    }

    // 3. an offscreen pass whose target cannot be made (no name; then no memory): its draws are dropped, the copy that ends it is skipped,
    //    and the frame after it is normal
    for (int variant = 0; variant < 2; ++variant) {
        const int side = variant ? 90 : 100;
        if (variant) g_noStorage = true; else g_failFbo = true;
        Snap a = Take();
        vgx::CmdBuf b = NewBuf();
        AddPass(b, side, side);
        AddDraw(b);
        AddCopy(b, 7, side, side, side, side, true);
        Run(b);
        Snap z = Take();
        std::printf("pass %s: %u draws, %u on framebuffer 0, %u attached without storage, %u notes\n", variant ? "without memory" : "without a name",
                    z.draws - a.draws, z.onFbo0 - a.onFbo0, z.attachNs - a.attachNs, z.notes - a.notes);
        if (guard) {
            Check(z.draws == a.draws, "guard: the draws of a pass with no target are dropped");
            Check(z.onFbo0 == a.onFbo0 && z.attach0 == a.attach0 && z.attachNs == a.attachNs, "guard: nothing reaches framebuffer 0 or a texture without storage");
            Check(z.notes > a.notes, "guard: the failure is noted");
        } else {
            Check(z.onFbo0 > a.onFbo0 || z.attachNs > a.attachNs, "control: without the guard the pass draws into framebuffer 0 or a texture without storage");
        }
        Reset();
        a = Take();
        b = NewBuf();
        AddDraw(b);
        Run(b);
        z = Take();
        Check(z.draws == a.draws + 1, "the frame after a skipped pass draws");
        // and the same pass with room: it draws into its own object
        a = Take();
        b = NewBuf();
        AddPass(b, side, side);
        AddDraw(b);
        AddCopy(b, 7, side, side, side, side, true);
        Run(b);
        z = Take();
        Check(!guard || (z.draws >= a.draws + 1 && z.attachOk > a.attachOk), "with room again the pass draws into its own target (and its copy is made)");
    }

    // 4. texture uploads: no name, no memory
    for (int variant = 0; variant < 2; ++variant) {
        vgx::TexUpload up;
        up.texId = 20 + variant;
        up.width = up.height = 4;
        up.levels.push_back(std::vector<uint8_t>(64, 0x80));
        if (variant) g_noStorage = true; else g_failTex = true;
        const unsigned before = vgx::backend::GetStats().textures;
        const unsigned notes = g_notes;
        vgx::CmdBuf b = NewBuf();
        b.uploads.push_back(up);
        b.cmds.push_back(vgx::Cmd{vgx::CMD_TEX_UPLOAD, 0});
        AddDraw(b);
        Run(b);
        const unsigned after = vgx::backend::GetStats().textures;
        std::printf("upload %s: textures %u -> %u, %u notes\n", variant ? "without memory" : "without a name", before, after, g_notes - notes);
        if (guard) {
            Check(after == before, "guard: a texture that could not be made is not counted");
            Check(g_notes > notes, "guard: the failure is noted");
        }
        Reset();
        b = NewBuf();
        b.uploads.push_back(up);
        b.cmds.push_back(vgx::Cmd{vgx::CMD_TEX_UPLOAD, 0});
        Run(b);
        Check(vgx::backend::GetStats().textures == after + 1 || !guard, "with room again the upload is made");
    }

#ifdef __VGX_VITA__
    // 5. the reduced-size effects target: no name for its object. The effects are drawn at full size instead.
    {
        g_failFbo = true;
        Snap a = Take();
        vgx::CmdBuf b = NewBuf();
        AddDraw(b, true);
        Run(b);
        Snap z = Take();
        std::printf("effects without a target: %u draws, %u on framebuffer 0, %u notes\n", z.draws - a.draws, z.onFbo0 - a.onFbo0, z.notes - a.notes);
        if (guard) {
            Check(z.draws > a.draws, "guard: the effects are still drawn");
            Check(z.onFbo0 == a.onFbo0 && z.attach0 == a.attach0, "guard: nothing reaches framebuffer 0");
        } else {
            Check(z.onFbo0 > a.onFbo0 || z.attach0 > a.attach0, "control: without the guard the effects go to framebuffer 0");
        }
        Reset();
    }
#endif
    std::puts("PASS: allocation guard");
    return 0;
}
