// 1.3 MSC_VGX_GXM_DIRECT (vgx_backend_gl.cpp) and MSC_FX_LOWRES_RUNS on the real GL back end (the Vita build) over the stub GL, with a stand-in for vitaGL's direct call.
//   gxm_direct <on|off> | gxm_direct fxruns
// A line draw takes the ordinary path, a triangle draw the direct one, both in one process: for each layout the table given to vitaGL must say what the ordinary path
// said with glVertexAttribPointer (offsets, sizes, types, stride, base), and the constants of the other attributes must be the ones it set. Then: base and indices reach the
// call; the next ordinary draw points its arrays again; a refused draw is drawn the ordinary way; a warm-up variant goes the same way as the real draw; constants are made
// once per frame. Off: no direct call at all. fxruns: at most N effect runs go to the reduced target, the largest, Mega Strikes included.
#include <GLES2/gl2.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

namespace {
struct Ptr { GLint size; GLenum type; GLboolean norm; GLsizei stride; size_t off; };
std::map<GLuint, Ptr> ptrs;              // what glVertexAttribPointer last said, per location
uint32_t enabled = 0;
std::array<std::array<float, 4>, 16> constants{};
uint32_t constSet = 0;
unsigned attrPointerCalls = 0, constCalls = 0, elementDraws = 0;
GLsizei lastCount = 0;
size_t lastIdx = 0;
void TestPointer(GLuint i, GLint size, GLenum type, GLboolean norm, GLsizei stride, const GLvoid* off) {
    ptrs[i] = Ptr{size, type, norm, stride, reinterpret_cast<size_t>(off)};
    ++attrPointerCalls;
}
void TestEnable(GLuint i) { enabled |= 1u << i; }
void TestDisable(GLuint i) { enabled &= ~(1u << i); }
void TestConst(GLuint i, GLfloat a, GLfloat b, GLfloat c, GLfloat d) { constants[i] = {a, b, c, d}; constSet |= 1u << i; ++constCalls; }
void TestDrawElements(GLenum, GLsizei n, GLenum, const GLvoid* off) { ++elementDraws; lastCount = n; lastIdx = reinterpret_cast<size_t>(off); }
void Check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
}
#define glVertexAttribPointer TestPointer
#define glEnableVertexAttribArray TestEnable
#define glDisableVertexAttribArray TestDisable
#define glVertexAttrib4f TestConst
#define glDrawElements TestDrawElements
#include "../src/vgx_backend_gl.cpp"
#undef glVertexAttribPointer
#undef glEnableVertexAttribArray
#undef glDisableVertexAttribArray
#undef glVertexAttrib4f
#undef glDrawElements
#include "backend_test_hooks.h"

namespace vgx { bool LogEnabled() { return false; } const char* ShaderListPath() { return ""; } }   // (ShaderListPath, VgxStillArmed: vgx_draw.cpp, not linked here)
extern "C" int VgxStillArmed(void) { return 0; }

namespace {
struct Call { GLenum mode; GLsizei count; uint32_t idx; GLuint vbo; uint32_t base; vgx::backend::VgxDirectLayout layout; };
std::vector<Call> calls;
bool refuse = false;
}
extern "C" GLboolean vglDrawElementsDirect(GLenum mode, GLsizei count, uint32_t idxOffset, GLuint vboName, uint32_t base, const vgx::backend::VgxDirectLayout* layout) {
    calls.push_back(Call{mode, count, idxOffset, vboName, base, *layout});
    return refuse ? GL_FALSE : GL_TRUE;
}

using namespace vgx;
namespace {
std::vector<ShaderSource> shaders(4);
uint64_t frameNo = 1;
void Frame(const std::vector<DrawRec>& draws, const std::vector<VariantCmd>& variants = {}) {
    CmdBuf b;
    b.frame = ++frameNo;
    b.vtx.assign(4096, 0);
    b.idx.assign(64, 0);
    b.variants = variants;
    for (size_t i = 0; i < variants.size(); ++i) b.cmds.push_back(Cmd{CMD_VARIANT, static_cast<uint32_t>(i)});
    b.draws = draws;
    for (size_t i = 0; i < draws.size(); ++i) b.cmds.push_back(Cmd{CMD_DRAW, static_cast<uint32_t>(i)});
    backend::Execute(b);
}
DrawRec Rec(uint32_t mask, uint8_t prim, uint32_t vtxOffset = 0, uint32_t idxOffset = 0, uint32_t idxCount = 3) {
    DrawRec r;
    r.prim = prim;
    r.layoutMask = mask;
    r.vtxOffset = vtxOffset;
    r.idxOffset = idxOffset;
    r.idxCount = idxCount;
    r.vtxCount = 3;
    return r;
}
void Boot() {
    shaders[0].fragment = false; shaders[0].id = 0; shaders[0].text = "void main() { }\n";
    shaders[1].fragment = false; shaders[1].id = 1; shaders[1].text = "void main() { }\n"; shaders[1].nMv = 2;
    shaders[2].fragment = true; shaders[2].id = 0; shaders[2].text = "void main() { }\n";
    shaders[3].fragment = true; shaders[3].id = 1; shaders[3].text = "void main() { }\n";
    CmdBuf b;
    b.frame = 1;
    b.targets.push_back(SetTargetCmd{});
    b.cmds.push_back(Cmd{CMD_SET_TARGET, 0});
    for (uint32_t i = 0; i < shaders.size(); ++i) { b.shaders.push_back(&shaders[i]); b.cmds.push_back(Cmd{CMD_SHADER, i}); }
    backend::Execute(b);
}
int Off() {
    Boot();
    Frame({Rec(VL_NRM | VL_COL0, PRIM_TRIANGLES)}, {VariantCmd{0, 0, VL_COL0, 0}});
    Check(calls.empty() && elementDraws == 2, "off: the ordinary path only");
    std::puts("PASS: MSC_VGX_GXM_DIRECT off: no direct call");
    return 0;
}
void CompareLayout(uint32_t mask) {
    // the ordinary path's word for this layout, at a base of 64 bytes
    Frame({Rec(mask, PRIM_LINES, 64)});
    const auto ordinary = ptrs;
    const uint32_t ordEnabled = enabled;
    const auto ordConst = constants;
    calls.clear();
    Frame({Rec(mask, PRIM_TRIANGLES, 128, 6, 9)});
    Check(calls.size() == 1, "one direct call");
    const Call& c = calls[0];
    Check(c.mode == GL_TRIANGLES && c.count == 9 && c.idx == 12 && c.base == 128, "count, indices and base reach the call");
    const VtxLayout L = MakeLayout(mask);
    Check(c.layout.stride == L.stride && (c.layout.key & 0x1FFFu) == mask, "stride and key");
    Check(c.layout.enabled == ordEnabled, "the enabled set is the ordinary path's");
    for (int loc = 0; loc < 16; ++loc) {
        if (!(c.layout.enabled & (1u << loc))) {
            Check(!(enabled & (1u << loc)), "an attribute the layout lacks has its array off");
            Check((constSet & (1u << loc)) != 0 || loc >= AT_BIX, "its constant was set");
            if (loc < AT_BIX) Check(constants[loc] == ordConst[loc], "its constant is the ordinary path's");
            continue;
        }
        const Ptr& p = ordinary.at(loc);
        Check(p.stride == c.layout.stride, "stride");
        Check(p.size == c.layout.size[loc], "component count");
        Check(p.off == 64u + c.layout.offset[loc], "offset");
        const uint8_t type = p.type == GL_FLOAT ? backend::VGL_DIRECT_F32 : (p.norm ? backend::VGL_DIRECT_U8N : backend::VGL_DIRECT_U8);
        Check(type == c.layout.type[loc], "format");
    }
}
int On() {
    Boot();
    const uint32_t masks[] = {0u, VL_COL0, VL_NRM | VL_COL0, VL_NRM | VL_COL0 | VL_COL1 | VL_TEX0, VL_TEX0 | (VL_TEX0 << 3) | VL_TMI, VL_NRM | VL_SKIN | VL_TEX0,
                              VL_NRM | VL_COL0 | VL_COL1 | (0xFFu * VL_TEX0) | VL_TMI | VL_SKIN, VL_TEX0};
    for (uint32_t m : masks) CompareLayout(m);
    // after a direct draw the ordinary path points its arrays again even with the same buffer, layout and base
    Frame({Rec(VL_COL0, PRIM_LINES), Rec(VL_COL0, PRIM_TRIANGLES), Rec(VL_COL0, PRIM_LINES)});
    Check(ptrs.count(AT_COL0) != 0, "arrays set");
    unsigned before = attrPointerCalls;
    calls.clear();
    Frame({Rec(VL_COL0, PRIM_LINES), Rec(VL_COL0, PRIM_LINES)});
    const unsigned oneSet = attrPointerCalls - before;
    Check(oneSet == 2u, "two consecutive ordinary draws: one layout (position, colour)");
    before = attrPointerCalls;
    Frame({Rec(VL_COL0, PRIM_LINES), Rec(VL_COL0, PRIM_TRIANGLES), Rec(VL_COL0, PRIM_LINES)});
    Check(attrPointerCalls - before == 2u * oneSet, "an ordinary draw after a direct one sets its layout again");
    // direct draws in a row make no attribute calls, and constants are made once per frame
    Frame({Rec(0, PRIM_TRIANGLES)});
    unsigned c0 = constCalls, p0 = attrPointerCalls;
    Frame({Rec(0, PRIM_TRIANGLES), Rec(0, PRIM_TRIANGLES, 256), Rec(0, PRIM_TRIANGLES, 512)});
    Check(attrPointerCalls == p0, "direct draws set no arrays");
    Check(constCalls - c0 == 13u, "the thirteen constants once in the frame (vitaGL keeps them in a ring)");
    // a refused draw is drawn the ordinary way, with its arrays
    refuse = true;
    const unsigned e0 = elementDraws;
    p0 = attrPointerCalls;
    calls.clear();
    Frame({Rec(VL_COL0, PRIM_TRIANGLES, 32, 3, 6)});
    refuse = false;
    Check(calls.size() == 1 && elementDraws == e0 + 1 && lastCount == 6 && lastIdx == 6, "the refused draw went to glDrawElements");
    Check(attrPointerCalls == p0 + 2 && ptrs.at(AT_POS).off == 32u, "with its layout at its base");
    // a warm-up variant takes the direct path: the first real draw must not patch another vertex program
    calls.clear();
    p0 = attrPointerCalls;
    const unsigned e1 = elementDraws;
    Frame({}, {VariantCmd{0, 1, VL_NRM, 0}});
    Check(calls.size() == 1 && calls[0].base == 0 && calls[0].count == 3 && calls[0].idx == 0 && (calls[0].layout.key & 0x1FFFu) == VL_NRM, "warm-up variant: direct");
    Check(elementDraws == e1 && attrPointerCalls == p0, "warm-up variant: no ordinary draw");
    std::puts("PASS: MSC_VGX_GXM_DIRECT: the table is the ordinary path's, and the path keeps the back end's state");
    return 0;
}
// Three runs of effect draws (quads of 110 vertices' worth of count) separated by another draw. Quad sizes in pixels through an identity projection set the ranking.
int FxRuns() {
    Boot();
    CmdBuf b;
    b.frame = 2;
    const uint32_t stride = MakeLayout(0).stride;
    auto quad = [&](float half) {
        const uint32_t first = static_cast<uint32_t>(b.vtx.size() / stride);
        const float xy[4][2] = {{-half, -half}, {half, -half}, {half, half}, {-half, half}};
        for (auto& v : xy) {
            const float w[4] = {v[0], v[1], 0.0f, 1.0f};
            const size_t at = b.vtx.size();
            b.vtx.resize(at + stride);
            std::memcpy(b.vtx.data() + at, w, sizeof w);
        }
        const uint16_t f = static_cast<uint16_t>(first);
        const uint32_t idx = static_cast<uint32_t>(b.idx.size());
        for (uint16_t i : {f, static_cast<uint16_t>(f + 1), static_cast<uint16_t>(f + 2), f, static_cast<uint16_t>(f + 2), static_cast<uint16_t>(f + 3)}) b.idx.push_back(i);
        return idx;
    };
    const uint32_t proj = 0, mv = 16;
    const float I4[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const float I3[24] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    b.uni.assign(I4, I4 + 16);
    b.uni.insert(b.uni.end(), I3, I3 + 24);
    auto fx = [&](float half) {
        DrawRec r = Rec(0, PRIM_TRIANGLES);
        r.vsId = 1;
        r.fx = 1;
        r.idxOffset = quad(half);
        r.idxCount = 6;
        r.vtxCount = 500;
        r.ublock[UB_PROJ] = proj;
        r.ublock[UB_MV] = mv;
        r.rs.blend = 1; r.rs.blendEq = BE_ADD; r.rs.blendSrc = BF_ONE; r.rs.blendDst = BF_ONE; r.rs.depthWrite = 0;
        r.rs.vpW = 640; r.rs.vpH = 448; r.rs.scW = 640; r.rs.scH = 448;
        return r;
    };
    const float sizes[] = {0.1f, 0.8f, 0.3f, 0.5f};
    for (float s : sizes) {
        b.draws.push_back(fx(s));
        b.cmds.push_back(Cmd{CMD_DRAW, static_cast<uint32_t>(b.draws.size() - 1)});
        DrawRec other = Rec(0, PRIM_TRIANGLES);
        b.draws.push_back(other);
        b.cmds.push_back(Cmd{CMD_DRAW, static_cast<uint32_t>(b.draws.size() - 1)});
    }
    backend::FxDecide(b, b.draws.data(), true, backend::g_lvNative, false);
    const char* want = std::getenv("FX_WANT");
    std::string got;
    for (size_t i = 0; i < b.cmds.size(); i += 2) got += backend::FxCmd(static_cast<int>(i)) ? '1' : '0';
    std::printf("runs drawn small (sizes 0.1 0.8 0.3 0.5): %s, want %s\n", got.c_str(), want);
    Check(got == want, "the largest runs, whatever their order");
    std::puts("PASS: MSC_FX_LOWRES_RUNS");
    return 0;
}
}

int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "on";
    if (!std::strcmp(mode, "fxruns")) return FxRuns();
    if (!std::strcmp(mode, "off")) return Off();
    return On();
}
