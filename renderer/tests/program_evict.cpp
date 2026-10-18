// W5 SEED: MSC_VGX_MAXPROGRAMS (vgx_backend_gl.cpp: EvictPrograms, WarmRoom, the no-free-slot net) on the real GL back end over the stub GL,
// with a stand-in for vitaGL's program table: 1,024 slots, handles are the slot numbers and are handed out again, glCreateProgram returns
// 0xFFFFFFFF when the table is full (custom_shaders.c with SKIP_ERROR_HANDLING), and every use of a program that is not live is a GL error.
//   program_evict <cap> [programs] [frames]     cap = MSC_VGX_MAXPROGRAMS (0 = none; "default" leaves it unset: 900 with -D__VGX_VITA__, none otherwise)
// Frame 1 is the warm-up of `programs` programs (default 1054 = the catalogue) and some variants, then `frames` frames of play: six working sets of
// 176 programs, each drawn for 700 frames (every program of the set every 22 frames), so the sets before it go stale (600 frames) and are evicted when
// the count passes the cap. A draw of a program that was deleted links it again. Exit 0 = every check passed; with cap 0 the checks that need a
// cap are skipped and the run reports the failure mode instead (a control: 1,054 programs overflow the table by 30).
#include <GLES2/gl2.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
constexpr unsigned kSlots = 1024;
bool live[kSlots + 1];
unsigned liveCount = 0, peakLive = 0, overflow = 0, glErrors = 0, creates = 0, deletes = 0, cur = 0, drawsOnDead = 0, draws = 0;
void Err(const char* what) {
    if (glErrors < 5) std::fprintf(stderr, "  GL error: %s\n", what);
    ++glErrors;
}
bool Live(GLuint p) { return p >= 1 && p <= kSlots && live[p]; }
GLuint TestCreateProgram() {
    for (unsigned i = 1; i <= kSlots; ++i)
        if (!live[i]) {
            live[i] = true;
            ++liveCount; ++creates;
            if (liveCount > peakLive) peakLive = liveCount;
            return i;
        }
    ++overflow;
    return 0xFFFFFFFFu;
}
void TestDeleteProgram(GLuint p) {
    if (!Live(p)) { Err("glDeleteProgram of a program that is not live"); return; }
    live[p] = false; --liveCount; ++deletes;
    if (cur == p) cur = 0;
}
void TestAttachShader(GLuint p, GLuint) { if (!Live(p)) Err("glAttachShader on a dead program"); }
void TestLinkProgram(GLuint p) { if (!Live(p)) Err("glLinkProgram on a dead program"); }
void TestUseProgram(GLuint p) { if (p && !Live(p)) Err("glUseProgram on a dead program"); cur = p; }
void TestGetProgramiv(GLuint p, GLenum name, GLint* out) {
    if (!Live(p)) Err("glGetProgramiv on a dead program");
    *out = name == GL_ACTIVE_UNIFORMS ? 0 : 1;
}
void TestDrawElements(GLenum, GLsizei, GLenum, const GLvoid*) {
    ++draws;
    if (!Live(cur)) { ++drawsOnDead; Err("draw with no live program in use"); }
}
void Check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
}

#define glCreateProgram TestCreateProgram
#define glDeleteProgram TestDeleteProgram
#define glAttachShader TestAttachShader
#define glLinkProgram TestLinkProgram
#define glUseProgram TestUseProgram
#define glGetProgramiv TestGetProgramiv
#define glDrawElements TestDrawElements
#include "../src/vgx_backend_gl.cpp"
#undef glCreateProgram
#undef glDeleteProgram
#undef glAttachShader
#undef glLinkProgram
#undef glUseProgram
#undef glGetProgramiv
#undef glDrawElements

namespace vgx { bool LogEnabled() { return false; } }
namespace vgx::rec { double TakePlayerIdleMs() { return 0; } bool TakePipeWaitMs(double*, double*) { return false; } }

int main(int argc, char** argv) {
    const char* capArg = argc > 1 ? argv[1] : "200";
    const unsigned nProg = argc > 2 ? static_cast<unsigned>(std::atoi(argv[2])) : 1054;
    const unsigned frames = argc > 3 ? static_cast<unsigned>(std::atoi(argv[3])) : 4300;
    unsigned cap = 0;
    if (std::strcmp(capArg, "default") != 0) { cap = static_cast<unsigned>(std::atoi(capArg)); setenv("MSC_VGX_MAXPROGRAMS", capArg, 1); }
    else {
#ifdef __VGX_VITA__
        cap = 900;
#endif
    }
    constexpr unsigned kVs = 50, kFs = 60;
    std::vector<vgx::ShaderSource> shaders(kVs + kFs);
    for (unsigned i = 0; i < kVs; ++i) { shaders[i].fragment = false; shaders[i].id = static_cast<uint16_t>(i); shaders[i].text = "void main() { }\n"; }
    for (unsigned i = 0; i < kFs; ++i) { shaders[kVs + i].fragment = true; shaders[kVs + i].id = static_cast<uint16_t>(i); shaders[kVs + i].text = "void main() { }\n"; }
    auto pairOf = [&](unsigned n, uint16_t& v, uint16_t& f) { v = static_cast<uint16_t>(n % kVs); f = static_cast<uint16_t>(n / kVs); };

    // frame 1: shaders, the warm-up programs, a variant of every fifth
    {
        vgx::CmdBuf b;
        b.frame = 1;
        b.targets.push_back(vgx::SetTargetCmd{});   // the 640x528 EFB: without a render target every draw's scissor comes out empty and nothing is drawn
        b.cmds.push_back(vgx::Cmd{vgx::CMD_SET_TARGET, 0});
        for (unsigned i = 0; i < kVs + kFs; ++i) { b.shaders.push_back(&shaders[i]); b.cmds.push_back(vgx::Cmd{vgx::CMD_SHADER, i}); }
        for (unsigned n = 0; n < nProg; ++n) {
            uint16_t v, f; pairOf(n, v, f);
            b.cmds.push_back(vgx::Cmd{vgx::CMD_PROGRAM, (static_cast<uint32_t>(v) << 16) | f});
            if (n % 5 == 0) {
                b.variants.push_back(vgx::VariantCmd{v, f, 0, 0});
                b.cmds.push_back(vgx::Cmd{vgx::CMD_VARIANT, static_cast<uint32_t>(b.variants.size() - 1)});
            }
        }
        vgx::backend::Execute(b);
    }
    std::printf("warm-up of %u programs, cap %s: %u linked, %u slots live, %u overflowed, %u GL errors\n", nProg, capArg, vgx::backend::GetStats().programs, liveCount, overflow, glErrors);
    if (cap) {
        Check(overflow == 0, "the warm-up must not overflow the program table");
        Check(liveCount <= cap, "the warm-up links no more than the cap");
        Check(glErrors == 0, "no GL error in the warm-up");
    } else if (nProg > kSlots) {
        // No cap: the table runs out during the warm-up (1,054 programs: the field log of a Vita3K run without any of this had 30 programs on the
        // invalid handle, 75 patching failures and GL error 0x501). The no-free-slot net frees a hundred programs and goes on: no GL error.
        std::printf("no cap: %u programs asked for, table of %u: glCreateProgram failed %u time(s), the net deleted %u programs, %u GL errors\n", nProg, kSlots, overflow, deletes, glErrors);
        Check(overflow >= 1 && deletes >= 100, "without a cap the table runs out and the net frees programs");
        Check(glErrors == 0 && liveCount <= kSlots, "the net leaves no GL error");
    }

    // play: frames 2..frames+1, six working sets of 176 programs, 700 frames each
    unsigned maxLive = 0;
    std::vector<unsigned char> everLinked(nProg, 0);
    for (unsigned f = 2; f < 2 + frames; ++f) {
        const unsigned set = ((f - 2) / 700) % 6;
        vgx::CmdBuf b;
        b.frame = f;
        b.vtx.assign(256, 0);
        b.idx = {0, 1, 2};
        // 8 programs of the set per frame, the set is covered every 22 frames
        for (unsigned k = 0; k < 8; ++k) {
            const unsigned n = (set * 176 + ((f * 8 + k) % 176)) % nProg;
            uint16_t v, fs; pairOf(n, v, fs);
            vgx::DrawRec r;
            r.vsId = v; r.fsId = fs; r.vtxCount = 3; r.idxCount = 3;
            b.draws.push_back(r);
            b.cmds.push_back(vgx::Cmd{vgx::CMD_DRAW, static_cast<uint32_t>(b.draws.size() - 1)});
        }
        vgx::backend::Execute(b);
        for (unsigned k = 0; k < 8; ++k) everLinked[(set * 176 + ((f * 8 + k) % 176)) % nProg] = 1;
        if (liveCount > maxLive) maxLive = liveCount;
    }
    // programs the cycle came back to after they had been deleted are the extra creates beyond the number of distinct programs ever used
    unsigned distinct = 0;
    for (unsigned char c : everLinked) distinct += c;
    std::printf("play of %u frames: %u draws, %u programs used, peak %u slots live (cap %s), %u deleted in %u passes, %u linked in all (%u over the distinct ones: re-links), %u overflowed, %u GL errors, %u draws on a dead program\n",
                frames, draws, distinct, peakLive, capArg, deletes, vgx::backend::g_evictions, creates, creates > distinct ? creates - distinct : 0, overflow, glErrors, drawsOnDead);
    if (cap) Check(overflow == 0, "no overflow of the program table while playing");
    Check(glErrors == 0 && drawsOnDead == 0, "no GL error, every draw had a live program");
    Check(draws >= frames * 8, "every draw was issued");
    if (cap) {
        Check(deletes > 0 && vgx::backend::g_evictions > 0, "programs were deleted");
        // what a draw used in the last 600 frames cannot be deleted: at a change of working set the old set (176) is still that recent while the new one links
        Check(maxLive <= (cap > 2 * 176 ? cap : 2 * 176) + 8, "the live count stays at the cap, or at two working sets when the cap is lower than that");
        Check(creates > distinct, "deleted programs were linked again when drawn");
    }
    std::puts("PASS: program eviction and warm-up cap over a stand-in for vitaGL's 1,024-program table");
    return 0;
}
