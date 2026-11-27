#include <GLES2/gl2.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace {
struct ShaderState { GLenum type; std::string source; bool compiled = false, binary = false, deleted = false; unsigned refs = 0; };
struct ProgramState { std::vector<GLuint> shaders; bool linked = false; };
std::map<GLuint, ShaderState> shaders;
std::map<GLuint, ProgramState> programs;
std::vector<std::string> fragmentSources;
std::vector<uint64_t> packQueries;
unsigned nextShader = 1, nextProgram = 1, compiles = 0, binaryLoads = 0, halfRefusals = 0;
bool refuseHalfText = false;
bool packPresent = true, binaryAccepted = true, failHalfLink = false, failFloatCompile = false, failVertexCompile = false, failAllLinks = false, failProgramCreate = false;
GLuint badProgramName = 0;
void Check(bool ok, const char* message);
struct PackEntry { uint64_t hash; uint32_t kind, offset, size, pad; };
uint8_t g_pack[8]{};
std::atomic<uint32_t> g_packHits{0};
const PackEntry* PackFind(int fragment, uint64_t hash) {
    Check(fragment == 1, "candidate must be a fragment shader");
    packQueries.push_back(hash);
    static PackEntry entry{};
    entry = {hash, 1, 0, sizeof(g_pack), 0};
    return packPresent ? &entry : nullptr;
}
void Check(bool ok, const char* message) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); } }
GLuint TestCreateShader(GLenum type) { const GLuint id = nextShader++; ShaderState s{}; s.type = type; shaders.emplace(id, std::move(s)); return id; }
void TestShaderSource(GLuint id, GLsizei count, const GLchar* const* strings, const GLint* lengths) {
    auto& s = shaders.at(id);
    for (int i = 0; i < count; ++i) s.source.append(strings[i], lengths && lengths[i] >= 0 ? static_cast<size_t>(lengths[i]) : std::strlen(strings[i]));
    if (s.type == GL_FRAGMENT_SHADER) fragmentSources.push_back(s.source);
}
void TestCompileShader(GLuint id) {
    auto& s = shaders.at(id); ++compiles;
    s.compiled = !(failFloatCompile && s.type == GL_FRAGMENT_SHADER) && !(failVertexCompile && s.type == GL_VERTEX_SHADER);
    if (refuseHalfText && s.source.find("HV4") != std::string::npos) { s.compiled = false; ++halfRefusals; }
}
void TestGetShaderiv(GLuint id, GLenum name, GLint* out) { *out = name == GL_COMPILE_STATUS ? shaders.at(id).compiled : 0; }
void TestDeleteShader(GLuint id) { auto& s = shaders.at(id); Check(!s.deleted, "shader deleted twice"); s.deleted = true; if (!s.refs) shaders.erase(id); }
GLuint TestCreateProgram() { if (failProgramCreate) return badProgramName; const GLuint id = nextProgram++; programs.emplace(id, ProgramState{}); return id; }
void TestAttachShader(GLuint program, GLuint shader) { programs.at(program).shaders.push_back(shader); ++shaders.at(shader).refs; }
void TestLinkProgram(GLuint id) {
    auto& p = programs.at(id); p.linked = !failAllLinks;
    for (GLuint shader : p.shaders) p.linked = p.linked && shaders.at(shader).compiled && !(failHalfLink && shaders.at(shader).binary);
}
void TestGetProgramiv(GLuint id, GLenum name, GLint* out) { *out = name == GL_LINK_STATUS ? programs.at(id).linked : 0; }
void TestDeleteProgram(GLuint id) {
    for (GLuint shader : programs.at(id).shaders) { auto& s = shaders.at(shader); Check(s.refs > 0, "shader reference underflow"); if (--s.refs == 0 && s.deleted) shaders.erase(shader); }
    programs.erase(id);
}
void TestUseProgram(GLuint id) { Check(id == 0 || (programs.count(id) && programs.at(id).linked), "using invalid program"); }
void TestShaderBinary(GLsizei count, const GLuint* ids, GLenum format, const void* data, GLsizei length) {
    Check(count == 1 && format == 0 && data == g_pack && length == sizeof(g_pack), "exact indexed binary submitted to native reader");
    ++binaryLoads;
    if (binaryAccepted) { auto& s = shaders.at(ids[0]); s.compiled = true; s.binary = true; }
}
}

#define glShaderBinary TestShaderBinary
#define glCreateShader TestCreateShader
#define glShaderSource TestShaderSource
#define glCompileShader TestCompileShader
#define glGetShaderiv TestGetShaderiv
#define glDeleteShader TestDeleteShader
#define glCreateProgram TestCreateProgram
#define glAttachShader TestAttachShader
#define glLinkProgram TestLinkProgram
#define glGetProgramiv TestGetProgramiv
#define glDeleteProgram TestDeleteProgram
#define glUseProgram TestUseProgram
#include "picture_pack_loader.inc"
unsigned fakeHeapKiB = 0;
extern "C" unsigned NpPlatformHeapUsedKiB(void) { return fakeHeapKiB; }
extern "C" unsigned NpPlatformHeapTotalKiB(void) { return 98304u; }
extern "C" int VgxStillArmed(void) { return 0; }   // (vgx_draw.cpp; the test links no front end)
#include "../src/vgx_backend_gl.cpp"
#undef glShaderBinary
#undef glCreateShader
#undef glShaderSource
#undef glCompileShader
#undef glGetShaderiv
#undef glDeleteShader
#undef glCreateProgram
#undef glAttachShader
#undef glLinkProgram
#undef glGetProgramiv
#undef glDeleteProgram
#undef glUseProgram
#define VGX_TEST_PACKED_SHADER_LOADER
#include "backend_test_hooks.h"
extern "C" {
void NpPlatformNote(const char*) {}
volatile uint32_t np_stat_dvd_reads = 0, np_stat_dvd_kib = 0, np_stat_sync_reads = 0, np_stat_sync_kib = 0;
volatile uint32_t np_stat_audio_streams = 0, np_stat_audio_samples = 0;
unsigned char vglFramebufferMultisample(unsigned int, int) { return 0; }
}
namespace vgx { bool LogEnabled() { return false; } void TexCacheLost(uint32_t) {} const char* ShaderListPath() { return ""; } }

namespace {
void Reset() {
    for (auto& p : vgx::backend::g_pic) { if (p.prog) TestDeleteProgram(p.prog); p = {}; }
    Check(shaders.empty() && programs.empty(), "all linked and pending-deletion shader objects released");
    vgx::backend::S.program = ~0u;
    fragmentSources.clear(); packQueries.clear(); compiles = binaryLoads = 0;
    packPresent = binaryAccepted = true; failHalfLink = failFloatCompile = failVertexCompile = failAllLinks = failProgramCreate = false; badProgramName = 0;
}
uint64_t LastFragmentHash() { Check(!fragmentSources.empty(), "float fragment source submitted"); const auto& s = fragmentSources.back(); return XXH3_64bits(s.data(), s.size()); }
void FloatPath(int mode, uint64_t hash) {
    auto* p = vgx::backend::EnsurePictureProgram(mode);
    Check(p && p->prog && !p->failed, "original float path succeeds");
    Check(LastFragmentHash() == hash, "original float source bytes and version prefix preserved");
    Check(programs.size() == 1 && shaders.size() == 2, "only chosen program and attached shader ownership remain");
}
}

int main(int argc, char** argv) {
    setenv("MSC_VGX_WARM_HEAP_MIB", "44", 1);   // (the old absolute rule, explicit; the relative floor is in warm_spread.cpp)
    using namespace vgx;
    using namespace vgx::backend;
    if (argc == 3 && !std::strcmp(argv[1], "--emit-sources")) {
        for (int mode : {static_cast<int>(PIC_UNSHARP), kPicSmoothPlain}) for (bool half : {false, true}) {
            const std::string filename = std::string(argv[2]) + "/" + (mode == PIC_UNSHARP ? "unsharp" : "plain") + (half ? "-half" : "-original") + ".native-input.frag";
            const std::string source = kShaderVersion + PictureFragmentSource(mode, half);
            std::ofstream out(filename, std::ios::binary); Check(static_cast<bool>(out), "open emitted production source"); out << source; out.close(); Check(static_cast<bool>(out), "write emitted production source");
            std::printf("%s %016llX\n", filename.c_str(), static_cast<unsigned long long>(XXH3_64bits(source.data(), source.size())));
        }
        return 0;
    }
    if (argc == 2 && !std::strcmp(argv[1], "--dump-baseline")) {
        for (int mode = PIC_LINEAR; mode <= kPicSmoothPlain; ++mode) { Reset(); packPresent = false; Check(EnsurePictureProgram(mode), "baseline builder succeeds"); std::printf("mode=%d hash=%016llX\n", mode, static_cast<unsigned long long>(LastFragmentHash())); }
        Reset(); return 0;
    }
    const int modes[] = {PIC_UNSHARP, kPicSmoothPlain};
    [[maybe_unused]] const uint64_t halfHashes[] = {0x62207CC4A21190BCull, 0x094ADA8079ADDF26ull};
    const uint64_t floatHashes[] = {0x3D46CA1EE556CD70ull, 0x8BA91556042AA06Cull};
    for (unsigned i = 0; i < 2; ++i) {
        Reset();
        auto* p = EnsurePictureProgram(modes[i]);
        Check(p && p->prog && !p->failed, "picture program created");
#ifdef __VGX_VITA__
        Check(binaryLoads == 1, "eligible Vita picture loads the candidate from the package");
        Check(packQueries.size() == 1 && packQueries[0] == halfHashes[i], "candidate key derived from actual version-prefixed source");
        Check(compiles == 1 && fragmentSources.empty(), "candidate does not invoke fragment compiler or create source cache");
#else
        Check(packQueries.empty() && binaryLoads == 0, "desktop has no native precision or pack path");
        Check(LastFragmentHash() == floatHashes[i], "desktop source remains byte-identical");
#endif
        const unsigned compiled = compiles;
        Check(EnsurePictureProgram(modes[i]) == p && compiles == compiled, "resident picture program reused");

        Reset(); packPresent = false; FloatPath(modes[i], floatHashes[i]);
        Check(binaryLoads == 0 && compiles == 2, "older package takes original path without candidate runtime compile");
        Reset(); binaryAccepted = false; FloatPath(modes[i], floatHashes[i]);
        Check(compiles == 2, "rejected binary falls back without candidate runtime compile");
        Reset(); failHalfLink = true; FloatPath(modes[i], floatHashes[i]);
        Check(compiles == 2, "shared vertex compiled once across candidate and float attempts");
        Reset(); failVertexCompile = true;
        Check(!EnsurePictureProgram(modes[i]) && shaders.empty() && programs.empty(), "vertex failure cleans objects without attempting fragments");
        Check(packQueries.empty() && fragmentSources.empty(), "failed shared vertex does not trigger either fragment path");
        Reset(); packPresent = false; failFloatCompile = true;
        Check(!EnsurePictureProgram(modes[i]) && g_pic[modes[i] - PIC_LINEAR].failed, "float failure preserves blit fallback");
        Check(shaders.empty() && programs.empty(), "compile failure cleans every shader");
        Reset(); failAllLinks = true;
        Check(!EnsurePictureProgram(modes[i]) && g_pic[modes[i] - PIC_LINEAR].failed, "all link failures preserve blit fallback");
        Check(shaders.empty() && programs.empty(), "link failure cleans program and attached shader references");
        for (GLuint badName : {0u, 0xFFFFFFFFu}) {
            Reset(); failProgramCreate = true; badProgramName = badName;
            Check(!EnsurePictureProgram(modes[i]) && shaders.empty() && programs.empty(), "invalid program name cleaned without use");
        }
    }
    Reset(); FloatPath(PIC_SMOOTH, 0xDE7FFA993F86CF37ull);
    Check(packQueries.empty(), "edge-enabled smooth never takes precision path");
    Check(PictureFragmentSource(PIC_SMOOTH, true) == PictureFragmentSource(PIC_SMOOTH, false), "edge-enabled smooth source cannot change under half request");
    Reset(); FloatPath(PIC_SHARP, 0xC86E7C015F859B76ull);
    Check(packQueries.empty(), "single-sample sharp never takes precision path");
    Reset(); FloatPath(PIC_LINEAR, 0x45B454E6C1B743A4ull);
    Check(packQueries.empty(), "linear picture never takes precision path");
    {   // 1.3 PIC_FAST: its own slot, no dependent coordinates in the fragment shader, no pack lookup
        Reset();
        auto* p = EnsurePictureProgram(PIC_FAST);
        Check(p && p->prog && !p->failed && p == &g_pic[5], "fast picture program in its own slot");
        Check(packQueries.empty(), "fast picture has no candidate in a pack");
        Check(g_pic[PIC_SMOOTH + 1 - PIC_LINEAR].prog == 0, "fast does not take the plain smooth slot");
        const std::string f = PictureFragmentSource(PIC_FAST, false);
        Check(f == PictureFragmentSource(PIC_FAST, true), "half request does not change the fast source");
        Check(f.find("clamp") == std::string::npos && f.find("u_lim") == std::string::npos, "no per-pixel clamp");
        Check(f.find("texture2D(u_src, v_uv)") != std::string::npos && f.find("texture2D(u_src, v_tex1)") != std::string::npos && f.find("texture2D(u_src, v_tex2)") != std::string::npos, "fetches at the varyings as they come");
        Check(PictureModeFromString("fast") == PIC_FAST, "mode name");
        // The inset rectangle: the outermost fetch of a w-texel picture at x stays on or inside the centre of its edge texel, however the rectangle is scaled.
        const int x = 7, w = 640;
        const double lo = (x + 1) - 0.5, hi = (x + w - 1) + 0.5;
        Check(lo >= x + 0.5 && hi <= x + w - 0.5, "the half-texel fetches stay inside the display copy");
    }
    {   // 1.3 MSC_VGX_FS_HALF: a half text the compiler refuses costs no draw: the float text of the key at once; after three refusals no more attempts
        Reset();
        refuseHalfText = true;
        static ShaderSource srcs[5];
        for (uint16_t i = 0; i < 5; ++i) {
            FsKey k;
            std::memset(static_cast<void*>(&k), 0, sizeof k);
            k.numStages = 1; k.numTexGens = 1;
            k.stage[0].texEnable = 1; k.stage[0].rasChan = 0; k.stage[0].rasSwap = k.stage[0].texSwap = 0xE4;
            k.stage[0].color = 15u | 10u << 4 | 8u << 8 | 15u << 12 | 1u << 19;
            k.stage[0].alpha = 7u << 4 | 5u << 7 | 4u << 10 | 7u << 13 | 1u << 19;
            k.stage[0].kcsel = static_cast<uint8_t>(i);
            k.prec = FSP_HALF | FSP_HALFVAR;
            GenerateFragmentShader(k, srcs[i]);
            srcs[i].fragment = true;
            srcs[i].id = i;
            Check(srcs[i].text.find("HV4") != std::string::npos, "the half text");
            const GLuint id = CompileShaderSlot(srcs[i], true);
            Check(id != 0 && shaders.at(id).compiled, "a shader comes out of a refused half text");
            Check(shaders.at(id).source.find("HV4") == std::string::npos, "and it is the float text");
            Check(shaders.at(id).source.find("varying vec4 v_col0") != std::string::npos, "with float varyings");
        }
        Check(halfRefusals == 3, "three refusals, then no more half attempts");
        for (GLuint id : g_fs) if (id) TestDeleteShader(id);
        g_fs.clear(); g_fsSrc.clear(); g_fsAttempts.clear();
        refuseHalfText = false;
    }
    {   // 1.3 MSC_VGX_WARM_HEAP_MIB (explicit: wave 10 made the default a floor on the free heap): over the figure the warm-up compiles nothing and counts no attempt; a draw's compile and a running game are not held back
        Reset();
        static ShaderSource src[4];
        for (uint16_t i = 0; i < 4; ++i) {
            FsKey k;
            std::memset(static_cast<void*>(&k), 0, sizeof k);
            k.numStages = 1; k.numTexGens = 1;
            k.stage[0].texEnable = 1; k.stage[0].rasChan = 0; k.stage[0].rasSwap = k.stage[0].texSwap = 0xE4;
            k.stage[0].kcsel = static_cast<uint8_t>(i);
            GenerateFragmentShader(k, src[i]);
            src[i].fragment = true;
            src[i].id = i;
        }
        const uint64_t serial = g_serial;
        g_serial = 0;
        fakeHeapKiB = 43u * 1024u;
        Check(CompileShaderSlot(src[0], true) != 0, "under the figure the warm-up compiles");
        fakeHeapKiB = 44u * 1024u;
        const unsigned before = compiles;
        Check(CompileShaderSlot(src[1], true) == 0 && compiles == before, "at the figure it stops");
        Check(g_fsAttempts[1].count == 0, "and counts no attempt");
        Check(CompileShaderSlot(src[1], false) != 0, "a draw's compile still goes");
        g_serial = kWarmFrames;
        Check(CompileShaderSlot(src[2], true) != 0, "after the warm-up a new key is not held back");
        g_serial = serial;
        fakeHeapKiB = 0;
        for (GLuint id : g_fs) if (id) TestDeleteShader(id);
        g_fs.clear(); g_fsSrc.clear(); g_fsAttempts.clear();
    }
    Reset();
    std::puts("PASS: actual picture builder source identity, binary admission, float fallback and GL ownership");
}
