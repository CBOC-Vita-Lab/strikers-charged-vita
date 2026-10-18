// Z-freeze on the real GL back end (Mesa llvmpipe through EGL, or any GLES2 driver): the generated fragment shader that writes its
// depth from the reference plane, and the plane's journey through the back end (EFB pixels -> render-target pixels).
//   1. position: a triangle drawn the ordinary way leaves its depth in the buffer; a frozen probe at that triangle's plane, moved
//      nearer by `delta`, passes everywhere inside the triangle, moved farther it passes nowhere (delta = 4e-5 of the depth range);
//   2. the effect: a flattened mesh of overlapping coplanar triangles with blending and a strict LESS depth test, drawn the ordinary
//      way and frozen: pixels covered by more than one triangle are darkened again (unfrozen) or not (frozen).
// Usage: zfreeze_gl [RTWxRTH]   (the render target; default 640x528 = the EFB, so 1:1)
#include <GLES2/gl2.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>

#include "../src/vgx_backend_gl.cpp"
#include "../src/vgx_zfreeze.h"

namespace vgx { bool LogEnabled() { return false; } }

using namespace vgx;

static int g_failures = 0;
static void Check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
}

namespace {

constexpr int kEfbW = 640, kEfbH = 528;
float g_sx = 1, g_sy = 1; // render target / EFB

struct Vtx { float x, y, z, w; uint8_t r, g, b, a; };
static_assert(sizeof(Vtx) == 20, "layout of VL_COL0");

ShaderSource g_vs, g_fsPlain, g_fsFrozen;
uint64_t g_frame = 1;

struct Mesh {
    std::vector<Vtx> v;
    std::vector<uint16_t> idx;
};
struct Job {
    Mesh mesh;
    float proj[16];
    float mv[24];
    bool frozen = false;
    zfreeze::Plane plane;
    RenderState rs;
};

void Identity(float proj[16], float mv[24]) {
    std::memset(proj, 0, 16 * sizeof(float));
    proj[0] = proj[5] = proj[10] = proj[15] = 1;
    std::memset(mv, 0, 24 * sizeof(float));
    mv[0] = mv[5] = mv[10] = 1;
    mv[15] = mv[20] = 1; // (normal rows: unused)
}

// UB_PROJ as BuildProj makes it for a perspective projection.
void MakeProj(float fovScale, float aspect, float n, float f, float out[16]) {
    std::memset(out, 0, 16 * sizeof(float));
    const float p0 = fovScale / aspect, p2 = fovScale, p4 = -n / (f - n), p5 = -f * n / (f - n);
    out[0] = p0;
    out[5] = -p2;
    out[10] = 2.0f * p4 - 1.0f;
    out[14] = 2.0f * p5;
    out[11] = -1.0f;
}

RenderState BaseState() {
    RenderState rs;
    rs.scX = rs.scY = 0;
    rs.scW = kEfbW;
    rs.scH = kEfbH;
    rs.vpX = rs.vpY = 0;
    rs.vpW = kEfbW;
    rs.vpH = kEfbH;
    rs.zNear = 0;
    rs.zFar = 1;
    return rs;
}

// Runs the jobs in order on a cleared (white, depth 1) target and returns the picture (top row first, RGBA).
std::vector<uint8_t> Render(const std::vector<Job>& jobs) {
    CmdBuf buf;
    buf.frame = g_frame++;
    buf.shaders = {&g_vs, &g_fsPlain, &g_fsFrozen};
    buf.cmds = {{CMD_SHADER, 0}, {CMD_SHADER, 1}, {CMD_SHADER, 2}};
    CopyDispCmd clear;
    clear.src = EfbRect{0, 0, kEfbW, kEfbH};
    clear.clear.enable = true;
    clear.clear.rgba[0] = clear.clear.rgba[1] = clear.clear.rgba[2] = clear.clear.rgba[3] = 1.0f;
    clear.clear.z = 1.0f;
    buf.copyDisp.push_back(clear);
    buf.cmds.push_back({CMD_COPY_DISP, 0}); // (not captured: only clears)
    for (const Job& j : jobs) {
        DrawRec d;
        d.vsId = 1;
        d.fsId = j.frozen ? 2 : 1;
        d.prim = PRIM_TRIANGLES;
        d.layoutMask = VL_COL0;
        d.vtxOffset = static_cast<uint32_t>(buf.vtx.size());
        d.vtxCount = static_cast<uint32_t>(j.mesh.v.size());
        d.idxOffset = static_cast<uint32_t>(buf.idx.size());
        d.idxCount = static_cast<uint32_t>(j.mesh.idx.size());
        const size_t at = buf.vtx.size();
        buf.vtx.resize(at + j.mesh.v.size() * sizeof(Vtx));
        std::memcpy(buf.vtx.data() + at, j.mesh.v.data(), j.mesh.v.size() * sizeof(Vtx));
        buf.idx.insert(buf.idx.end(), j.mesh.idx.begin(), j.mesh.idx.end());
        d.ublock[UB_PROJ] = static_cast<uint32_t>(buf.uni.size());
        buf.uni.insert(buf.uni.end(), j.proj, j.proj + 16);
        d.ublock[UB_MV] = static_cast<uint32_t>(buf.uni.size());
        buf.uni.insert(buf.uni.end(), j.mv, j.mv + 24);
        d.ublock[UB_TEV] = static_cast<uint32_t>(buf.uni.size());
        float tev[112] = {};
        buf.uni.insert(buf.uni.end(), tev, tev + 112);
        if (j.frozen) {
            // the plane goes where the generated shader reads it (the front end writes it in BuildTev)
            float* t = buf.uni.data() + d.ublock[UB_TEV] + g_fsFrozen.zfVec * 4;
            t[0] = j.plane.a;
            t[1] = j.plane.b;
            t[2] = j.plane.c;
        }
        d.rs = j.rs;
        buf.draws.push_back(d);
        buf.cmds.push_back({CMD_DRAW, static_cast<uint32_t>(buf.draws.size() - 1)});
    }
    CopyDispCmd read = clear;
    buf.copyDisp.push_back(read);
    buf.cmds.push_back({CMD_COPY_DISP, 1});
    backend::RequestCapture(true);
    backend::Execute(buf);
    backend::RequestCapture(false);
    int w = 0, h = 0;
    const std::vector<uint8_t>& px = backend::LastDisplayCopy(w, h);
    Check(w > 0 && h > 0 && px.size() == static_cast<size_t>(w) * h * 4, "the display copy came back");
    return px;
}

struct Tri { float p[3][3]; };

// A triangle's clip coordinates and window coordinates (EFB pixels) under proj / mv.
void ToWindow(const Tri& t, const float proj[16], const float mv[24], float clip[3][4], float win[3][3]) {
    const zfreeze::Viewport vp{0, 0, kEfbW, kEfbH, 0, 1};
    for (int i = 0; i < 3; ++i) {
        zfreeze::ModelToClip(t.p[i], mv, proj, clip[i]);
        zfreeze::ClipToWindow(clip[i], vp, win[i]);
    }
}

Mesh MeshOf(const Tri* t, int n, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    Mesh m;
    for (int k = 0; k < n; ++k)
        for (int i = 0; i < 3; ++i) {
            m.v.push_back(Vtx{t[k].p[i][0], t[k].p[i][1], t[k].p[i][2], 0, r, g, b, a});
            m.idx.push_back(static_cast<uint16_t>(m.v.size() - 1));
        }
    return m;
}

Mesh ScreenQuad(uint8_t r, uint8_t g, uint8_t b, uint8_t a) { // covers the screen under an identity projection
    Mesh m;
    const float c[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    for (auto& p : c) m.v.push_back(Vtx{p[0], p[1], 0, 0, r, g, b, a});
    m.idx = {0, 1, 2, 0, 2, 3};
    return m;
}

bool Inside(const float w[3][3], float x, float y, float margin) { // pixel centre inside the triangle by at least `margin` pixels
    float d[3];
    for (int i = 0; i < 3; ++i) {
        const float* a = w[i];
        const float* b = w[(i + 1) % 3];
        const float ex = b[0] - a[0], ey = b[1] - a[1];
        const float len = std::sqrt(ex * ex + ey * ey);
        d[i] = ((x - a[0]) * ey - (y - a[1]) * ex) / (len > 0 ? len : 1);
    }
    const bool allPos = d[0] >= margin && d[1] >= margin && d[2] >= margin;
    const bool allNeg = d[0] <= -margin && d[1] <= -margin && d[2] <= -margin;
    return allPos || allNeg;
}

} // namespace

int main(int argc, char** argv) {
    int rtW = kEfbW, rtH = kEfbH;
    if (argc > 1) std::sscanf(argv[1], "%dx%d", &rtW, &rtH);
    setenv("MSC_VGX_ZFREEZE", "1", 1);
    if (!host::CreateContext()) { std::fprintf(stderr, "no GL context\n"); return 2; }
    if (!backend::Init(rtW, rtH)) { std::fprintf(stderr, "back end init failed\n"); return 2; }
    g_sx = static_cast<float>(rtW) / kEfbW;
    g_sy = static_cast<float>(rtH) / kEfbH;
    const char* ext = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    std::printf("GL %s / %s, render target %dx%d for EFB %dx%d, GL_EXT_frag_depth %s\n", reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
                reinterpret_cast<const char*>(glGetString(GL_VERSION)), rtW, rtH, kEfbW, kEfbH, ext && std::strstr(ext, "GL_EXT_frag_depth") ? "yes" : "NO");

    // shaders: vertex colour straight through, plain and frozen
    VsKey vk{};
    FsKey fk{};
    fk.numStages = 1;
    fk.stage[0].rasChan = 0;
    fk.stage[0].color = (1u << 19) | (15u << 12) | (15u << 8) | (15u << 4) | 10u;                                      // d = RASC, clamped
    fk.stage[0].alpha = (1u << 19) | (7u << 13) | (7u << 10) | (7u << 7) | (5u << 4);                                  // d = RASA, clamped
    GenerateVertexShader(vk, g_vs);
    g_vs.id = 1; g_vs.fragment = false; g_vs.keyHash = 1;
    GenerateFragmentShader(fk, g_fsPlain);
    g_fsPlain.id = 1; g_fsPlain.fragment = true; g_fsPlain.keyHash = 2;
    fk.zfreeze = 1;
    GenerateFragmentShader(fk, g_fsFrozen);
    g_fsFrozen.id = 2; g_fsFrozen.fragment = true; g_fsFrozen.keyHash = 3;
    Check(g_fsFrozen.zfVec == FsZFreezeVec(fk) && g_fsFrozen.zfVec != 0, "the shader says where its plane is");
    Check(g_fsFrozen.text.find("VGX_FRAGDEPTH") != std::string::npos && g_fsPlain.text.find("VGX_FRAGDEPTH") == std::string::npos,
          "only the frozen shader writes the depth (the plain one is unchanged)");

    // camera: 30 m above the ground plane y = 0.1, looking down at 45 degrees, turned a little
    float proj[16], mv[24] = {};
    MakeProj(1.7f, static_cast<float>(kEfbW) / kEfbH, 0.5f, 800.0f, proj);
    const float c = std::cos(0.8f), s = std::sin(0.8f);
    const float mv12[12] = {1, 0, 0, 3, 0, c, -s, -2, 0, s, c, -30};
    std::memcpy(mv, mv12, sizeof mv12);

    // ---- 1. position of the frozen depth
    {
        Tri t = {{{-6, 0.1f, -4}, {7, 0.1f, -3}, {1, 0.1f, 6}}};
        float clip[3][4], win[3][3];
        ToWindow(t, proj, mv, clip, win);
        zfreeze::Plane pl;
        Check(zfreeze::PlaneFromClip(clip, zfreeze::Viewport{0, 0, kEfbW, kEfbH, 0, 1}, pl), "the test triangle has a plane");
        for (int sign = -1; sign <= 1; sign += 2) {
            const float delta = 4e-5f * static_cast<float>(sign); // negative: the probe is nearer than the triangle
            std::vector<Job> jobs(2);
            std::memcpy(jobs[0].proj, proj, sizeof proj);
            std::memcpy(jobs[0].mv, mv, sizeof mv);
            jobs[0].mesh = MeshOf(&t, 1, 0, 0, 0, 255);
            jobs[0].rs = BaseState();
            jobs[0].rs.depthTest = 1; jobs[0].rs.depthFunc = CMP_ALWAYS; jobs[0].rs.depthWrite = 1;
            Identity(jobs[1].proj, jobs[1].mv);
            jobs[1].mesh = ScreenQuad(255, 0, 0, 255);
            jobs[1].frozen = true;
            jobs[1].plane = zfreeze::Plane{pl.a, pl.b, pl.c + delta};
            jobs[1].rs = BaseState();
            jobs[1].rs.depthTest = 1; jobs[1].rs.depthFunc = CMP_LESS; jobs[1].rs.depthWrite = 0;
            const std::vector<uint8_t> px = Render(jobs);
            long inside = 0, black = 0, red = 0, otherIn = 0, outside = 0, outsideRed = 0;
            float winRt[3][3];
            for (int i = 0; i < 3; ++i) { winRt[i][0] = win[i][0] * g_sx; winRt[i][1] = win[i][1] * g_sy; winRt[i][2] = win[i][2]; }
            for (int y = 0; y < rtH; ++y)
                for (int x = 0; x < rtW; ++x) {
                    const uint8_t* p = &px[(static_cast<size_t>(y) * rtW + x) * 4];
                    if (Inside(winRt, x + 0.5f, y + 0.5f, 2.0f)) {
                        ++inside;
                        if (p[0] < 16 && p[1] < 16 && p[2] < 16) ++black;
                        else if (p[0] > 240 && p[1] < 16 && p[2] < 16) ++red;
                        else ++otherIn;
                    } else if (!Inside(winRt, x + 0.5f, y + 0.5f, -2.0f)) {
                        ++outside;
                        if (p[0] > 240 && p[1] < 16 && p[2] < 16) ++outsideRed;
                    }
                }
            std::printf("position test, probe %s by %.0e: %ld pixels inside the triangle: %ld black (probe rejected), %ld red (probe passed), %ld other; "
                        "%ld clearly outside, %ld of them red\n",
                        sign < 0 ? "nearer" : "farther", std::fabs(delta), inside, black, red, otherIn, outside, outsideRed);
            Check(inside > 5000, "the triangle covers enough pixels");
            Check(otherIn == 0, "every pixel inside is black or red");
            if (sign < 0) Check(red == inside, "a probe nearer than the triangle's own depth passes everywhere inside it");
            else Check(black == inside, "a probe farther than the triangle's own depth is rejected everywhere inside it");
            Check(outsideRed == outside, "outside the triangle (depth 1.0) the probe always passes");
        }
    }

    // ---- 2. the effect: a flattened mesh of overlapping coplanar triangles, blended with a strict depth test
    {
        std::mt19937 rng(777);
        std::uniform_real_distribution<float> U(-1.0f, 1.0f);
        std::vector<Tri> mesh;
        for (int k = 0; k < 80; ++k) {
            Tri t;
            const float ox = U(rng) * 2.5f, oz = U(rng) * 2.5f;
            for (auto& p : t.p) { p[0] = ox + U(rng) * 2.0f; p[1] = 0.1f; p[2] = oz + U(rng) * 2.0f; }
            mesh.push_back(t);
        }
        // the reference: the game draws a culled quad at the shadow height first; here, a triangle of the same plane
        Tri ref = {{{-30, 0.1f, -30}, {30, 0.1f, -30}, {0, 0.1f, 40}}};
        float clip[3][4], win[3][3];
        ToWindow(ref, proj, mv, clip, win);
        zfreeze::Plane pl;
        Check(zfreeze::PlaneFromClip(clip, zfreeze::Viewport{0, 0, kEfbW, kEfbH, 0, 1}, pl), "the reference triangle has a plane");
        long cover[2][3] = {}; // [frozen][0: none, 1: single darkening, 2: darker]
        for (int frozen = 0; frozen < 2; ++frozen) {
            std::vector<Job> jobs(1);
            std::memcpy(jobs[0].proj, proj, sizeof proj);
            std::memcpy(jobs[0].mv, mv, sizeof mv);
            jobs[0].mesh = MeshOf(mesh.data(), static_cast<int>(mesh.size()), 0, 0, 0, 77);
            jobs[0].frozen = frozen != 0;
            jobs[0].plane = pl;
            jobs[0].rs = BaseState();
            jobs[0].rs.blend = 1; jobs[0].rs.blendSrc = BF_SRC_ALPHA; jobs[0].rs.blendDst = BF_INV_SRC_ALPHA;
            jobs[0].rs.depthTest = 1; jobs[0].rs.depthFunc = CMP_LESS; jobs[0].rs.depthWrite = 1;
            const std::vector<uint8_t> px = Render(jobs);
            for (int y = 0; y < rtH; ++y)
                for (int x = 0; x < rtW; ++x) {
                    const int v = px[(static_cast<size_t>(y) * rtW + x) * 4];
                    ++cover[frozen][v > 250 ? 0 : (v > 170 ? 1 : 2)]; // 255 untouched; 255 * (1 - 77/255) = 178 once; 124 twice
                }
            std::printf("flattened mesh of %zu overlapping triangles, %s: %ld pixels untouched, %ld darkened once, %ld darkened more than once (%.2f %% of the covered)\n",
                        mesh.size(), frozen ? "frozen " : "plain  ", cover[frozen][0], cover[frozen][1], cover[frozen][2],
                        100.0 * cover[frozen][2] / static_cast<double>(cover[frozen][1] + cover[frozen][2]));
        }
        Check(cover[1][1] > 5000, "the frozen mesh drew");
        Check(cover[1][2] == 0, "frozen: no pixel is darkened twice");
        Check(cover[0][1] + cover[0][2] == cover[1][1] + cover[1][2], "plain and frozen cover the same pixels");
        if (cover[0][2] == 0) std::printf("(note: this driver's plain depth interpolation did not let any pixel through twice; the unit test shows the effect with 24-bit quantisation)\n");
    }
    if (g_failures) return 1;
    std::puts("zfreeze GL test: passed");
    return 0;
}
