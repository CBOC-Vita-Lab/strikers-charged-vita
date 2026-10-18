// Integration test with the game's own drawing code: the test links real game translation units, patched the way NPB applies
// the patches platform/gx/patches/0002 and 0003 (main already carries NPD's display-list patch), against libnpgx.a and libvgx-native.a:
//
//   GLColourMeshWriter (src/Game/GL)             the mesh writer that particles / effects / UI use, fills host-order arrays
//   glModelBuilder (src/Game/GL)                  glCreateModel / glSetModelStream: marks those streams host-order (patch 0002)
//   GXVertexColourMaterialProgram (+ Render)      BindVertexArrays (NP_GX_SETARRAY_STREAM, patch 0003), DrawIndexed / DrawDirect (WGPIPE)
//   glxDisplayList (dlMakeDisplayList)            builds the display list, big-endian (NPD's patch, applied in main)
//   glxGX (gxInit, gxSet*)                        the game's GX state cache
//   glxModel (glplatFinalizePacket)
//
// Only the allocators, the data-cache hooks (what NPS provides: DC*RangeNoSync call np_gx_notify_write), the material
// program registry and a few string helpers are stand-ins. Four models, drawn in a 640 x 480 picture:
//
//   (20,20)   red      writer-built, per frame, host-order arrays          DrawDirect: sequential indices through WGPIPE
//   (160,20)  green    disc-style packet: big-endian arrays, host indices  DrawIndexed through WGPIPE
//   (300,20)  blue     the same data in a big-endian display list          GXCallDisplayList (display-list cache); in the last
//                                                                          frame the colours are rewritten and announced with
//                                                                          DCStoreRangeNoSync: the picture must follow (magenta)
//   (440,20)  yellow   writer-built once, persistent (resource allocator)  drawn every frame
//
//   gx_game_test [--no-ram] [--frames N] [--png out.png] [--null] [--efb-h N] [--report-efb]
//     --efb-h N       the game's render mode has an EFB of 640 x N (default 480; the game's PAL-style modes use 448); the test then
//                     sets the display-copy rectangle to the whole EFB, as glx_InitGX does, and the renderer's render target must
//                     follow (check with MSC_VGX_LOG=1: "render target ... for EFB 640xN"; test/matrix.sh does)
//     --report-efb    also report the size the way NPS's VIConfigure may (np_gx_set_efb_size); without it the glue takes it
//                     from GXSetDispCopySrc
#include <revolution/gx.h>
#include <revolution/os/OSCache.h>
#include <revolution/base/PPCArch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "np_gx.h"
#include "vgx_native.h"

#include "Game/GL/GLColourMeshWriter.h"
#include "Game/GL/glModelBuilder.h"
#include "NL/gl/glMatrix.h"
#include "NL/gl/glMaterialProgram.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glModel.h"
#include "NL/gl/glState.h"
#include "NL/glx/GXVertexColourMaterialProgram.h"
#include "NL/glx/glxDisplayList.h"
#include "NL/glx/glxGX.h"
#include "NL/nlString.h"

// ------------------------------------------------------------------------------------------------ stand-ins
namespace {
alignas(32) unsigned char g_frameArena[1 << 18];   // glFrameAlloc: reset every frame
alignas(32) unsigned char g_resArena[1 << 18];     // glResourceAlloc: persistent
unsigned int g_frameUsed = 0, g_resUsed = 0;
void* g_program = 0;

void* Bump(unsigned char* arena, unsigned int& used, unsigned long size) {
    used = (used + 31) & ~31u;
    void* p = arena + used;
    used += (size + 31) & ~31ul;
    memset(p, 0, size);
    return p;
}
} // namespace

void* glFrameAlloc(unsigned long size, eGLMemory) { return Bump(g_frameArena, g_frameUsed, size); }
void* glResourceAlloc(unsigned long size, eGLMemory, void*) { return Bump(g_resArena, g_resUsed, size); }
u32 glGetCurrentMatrix() { return 0; }
u32 glGetCurrentRasterState() { return 0; }
void glRegisterMaterialProgram(void* program, unsigned long) { g_program = program; }
void* glGetMaterialProgram(unsigned long) { return g_program; }
void nlZeroMemory(void* ptr, unsigned long numBytes) { memset(ptr, 0, numBytes); }
u32 nlStringLowerHash(const char*) { return 0; }
void GLMatrix::Set(const nlMatrix4&) {}
void GLMatrix::Get(nlMatrix4&) const {}
unsigned long glx_SkinMatrixSlots[10];
GXRenderModeObj glx_rmode;

// What NPS's cache functions do for the renderer (docs/render.md, "What the other workstreams need").
extern "C" {
void DCFlushRangeNoSync(const void* address, unsigned long length) { np_gx_notify_write(address, length); }
void DCStoreRangeNoSync(const void* address, unsigned long length) { np_gx_notify_write(address, length); }
void PPCSync(void) {}
}

// ------------------------------------------------------------------------------------------------ models
namespace {
const unsigned long kProgramHash = 0xD701656B;

struct Quad { float x, y; unsigned char r, g, b; };

void PutBeF(unsigned char* p, float f) {
    unsigned int v;
    memcpy(&v, &f, 4);
    v = __builtin_bswap32(v);
    memcpy(p, &v, 4);
}

// A disc-style packet: big-endian position and colour arrays (what the converted .rlg holds), host-order u16 indices (patch
// NPA 0001..), 4 unique vertices, 6 indices, two streams with id 1 (position) and 3 (colour), unknown07 = 0.
struct DiscModel {
    glModelPacket packet;
    glModelStream streams[2];
    unsigned char* pos;     // 4 x 12 bytes, big-endian floats
    unsigned char* col;     // 4 x 4 bytes, r g b a
    unsigned short* index;
};

void MakeDiscModel(DiscModel& m, const Quad& q) {
    memset(&m, 0, sizeof m);
    m.pos = static_cast<unsigned char*>(glResourceAlloc(48, GLM_VertexData, 0));
    m.col = static_cast<unsigned char*>(glResourceAlloc(16, GLM_VertexData, 0));
    m.index = static_cast<unsigned short*>(glResourceAlloc(12, GLM_VertexData, 0));
    const float xs[4] = {q.x, q.x + 100, q.x + 100, q.x}, ys[4] = {q.y, q.y, q.y + 100, q.y + 100};
    for (int i = 0; i < 4; ++i) {
        PutBeF(m.pos + 12 * i, xs[i]);
        PutBeF(m.pos + 12 * i + 4, ys[i]);
        PutBeF(m.pos + 12 * i + 8, 0.0f);
        m.col[4 * i] = q.r; m.col[4 * i + 1] = q.g; m.col[4 * i + 2] = q.b; m.col[4 * i + 3] = 255;
    }
    static const unsigned short idx[6] = {0, 1, 2, 0, 2, 3};
    memcpy(m.index, idx, sizeof idx);
    m.streams[0].address = m.pos; m.streams[0].index = 0; m.streams[0].stride = 12; m.streams[0].id = 1; m.streams[0].unknown07 = 0;
    m.streams[1].address = m.col; m.streams[1].index = 1; m.streams[1].stride = 4;  m.streams[1].id = 3; m.streams[1].unknown07 = 0;
    m.packet.indexBuffer = m.index;
    m.packet.numVertices = 6;
    m.packet.numUniqueVertices = 4;
    m.packet.primType = 0;   // triangles
    m.packet.numStreams = 2;
    m.packet.streams = m.streams;
    m.packet.materialProgram = g_program;
}

// The mesh writer, the way ParticleSystem / glDraw use it: Begin, per vertex Colour + Vertex, End.
glModel* WriteQuad(GLColourMeshWriter& w, const Quad& q, void* allocator) {
    w.Begin(6, 0, allocator);
    const float xs[4] = {q.x, q.x + 100, q.x + 100, q.x}, ys[4] = {q.y, q.y, q.y + 100, q.y + 100};
    static const int order[6] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < 6; ++i) {
        w.Colour(q.r, q.g, q.b, 255);
        w.Vertex(xs[order[i]], ys[order[i]], 0.0f);
    }
    w.End();
    return w.GetModel();
}

GXVertexColourMaterialProgram* g_prog = 0;
DiscModel g_discPlain, g_discList;
GLColourMeshWriter g_persist;
bool g_built = false;
} // namespace

extern "C" {

void* np_game_test_arenas(unsigned int* size0, void** second, unsigned int* size1) {
    *size0 = sizeof g_frameArena;
    *second = g_resArena;
    *size1 = sizeof g_resArena;
    return g_frameArena;
}

// `last` = the final frame (the blue model's colours are rewritten there).
void np_game_test_frame(int frame, int last, int efbHeight, int reportEfb) {
    if (frame == 0) {
        static unsigned char fifo[0x10000];
        if (reportEfb) np_gx_set_efb_size(640, efbHeight);
        GXInit(fifo, sizeof fifo);
        GXSetDispCopySrc(0, 0, 640, efbHeight);   // (glx_InitGX, right after the game's GXInit)
        GXSetDispCopyDst(640, efbHeight);
        GXSetViewport(0, 0, 640, efbHeight, 0, 1);
        GXSetScissor(0, 0, 640, efbHeight);
        gxInit();                       // the game's own initial state (z test on, blend off, ...)
        gxSetCullMode(GX_CULL_NONE);
        GXSetPixelFmt(GX_PF_RGBA6_Z24, GX_ZC_LINEAR);
        static unsigned char progStorage[sizeof(GXVertexColourMaterialProgram)] __attribute__((aligned(8)));
        g_prog = new (progStorage) GXVertexColourMaterialProgram();
        g_prog->Initialize();
        MakeDiscModel(g_discPlain, {160, 20, 0, 255, 0});
        MakeDiscModel(g_discList, {300, 20, 0, 0, 255});
        g_discList.packet.displayList = dlMakeDisplayList(&g_discList.packet, 0, true);
        WriteQuad(g_persist, {440, 20, 255, 255, 0}, &g_resUsed);   // (any non-null allocator: the persistent path)
        g_built = true;
    }
    float proj[4][4] = {{2.0f / 640, 0, 0, -1}, {0, -2.0f / 480, 0, 1}, {0, 0, -1, 0}, {0, 0, 0, 1}};
    GXSetProjection(proj, GX_ORTHOGRAPHIC);
    float id[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    GXLoadPosMtxImm(id, GX_PNMTX0);
    gxSetCurrentMtx(GX_PNMTX0, true);
    g_frameUsed = 0;                    // the per-frame allocator is reset, as at the start of the game's frame

    if (last) {
        // the converted file data changed (a vertex-colour edit): announce it the way the game announces any CPU write for the GPU
        for (int i = 0; i < 4; ++i) { g_discList.col[4 * i] = 255; g_discList.col[4 * i + 1] = 0; g_discList.col[4 * i + 2] = 255; }
        DCStoreRangeNoSync(g_discList.col, 16);
    }

    g_prog->Activate(0);
    GLColourMeshWriter w;
    glModel* red = WriteQuad(w, {20, 20, 255, 0, 0}, 0);
    g_prog->Draw(&red->packets[0]);
    g_prog->Draw(&g_discPlain.packet);
    g_prog->Draw(&g_discList.packet);
    g_prog->Draw(&g_persist.GetModel()->packets[0]);
    g_prog->Deactivate();

    GXColor clear = {32, 48, 64, 255};
    GXSetCopyClear(clear, 0xFFFFFF);
    GXCopyDisp(0, GX_TRUE);
    GXSetDrawDone();
    GXFlush();
    GXWaitDrawDone();
}

} // extern "C"

// ------------------------------------------------------------------------------------------------ driver
namespace {
struct Expect { int x, y; int r, g, b; const char* what; };
const Expect kExpect[] = {
    {70, 70, 255, 0, 0, "GLColourMeshWriter model (host-order arrays), DrawDirect through WGPIPE"},
    {210, 70, 0, 255, 0, "disc-style packet (big-endian arrays), DrawIndexed through WGPIPE"},
    {350, 70, 255, 0, 255, "big-endian display list over big-endian arrays; rewritten colours seen after DCStoreRangeNoSync"},
    {490, 70, 255, 255, 0, "persistent GLColourMeshWriter model"},
    {600, 440, 32, 48, 64, "background"},
};
int Abs(int v) { return v < 0 ? -v : v; }
} // namespace

int main(int argc, char** argv) {
    bool ram = true, nullBackend = false, reportEfb = false;
    int frames = 3, efbHeight = 480;
    const char* png = "gx_game_test.png";
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--no-ram")) ram = false;
        else if (!strcmp(argv[i], "--null")) nullBackend = true;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--efb-h") && i + 1 < argc) efbHeight = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--report-efb")) reportEfb = true;
        else if (!strcmp(argv[i], "--png") && i + 1 < argc) png = argv[++i];
    }
    np_gx_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.size = sizeof cfg;
    if (!np_gx_start(&cfg)) { fprintf(stderr, "np_gx_start failed\n"); return 2; }
    unsigned int s0 = 0, s1 = 0;
    void* second = 0;
    void* first = np_game_test_arenas(&s0, &second, &s1);
    if (ram) { np_gx_register_ram(first, s0); np_gx_register_ram(second, s1); }

    int failures = 0;
    for (int f = 0; f < frames; ++f) {
        const int last = f == frames - 1;
        if (last && !nullBackend) np_gx_request_dump(png);
        np_game_test_frame(f, last && frames > 1, efbHeight, reportEfb);
        if (!last || nullBackend) continue;
        int w = 0, h = 0;
        const unsigned char* px = np_gx_last_picture(&w, &h);
        if (!px) { printf("no picture\n"); ++failures; continue; }
        printf("picture %dx%d (%s guest RAM)\n", w, h, ram ? "registered" : "unregistered");
        for (const Expect& e : kExpect) {
            const unsigned char* c = px + (static_cast<size_t>(e.y) * w + e.x) * 4;
            const bool ok = Abs(c[0] - e.r) <= 8 && Abs(c[1] - e.g) <= 8 && Abs(c[2] - e.b) <= 8;
            printf("  %-4s (%3d,%3d) = %3u %3u %3u  want %3d %3d %3d  %s\n", ok ? "ok" : "FAIL", e.x, e.y, c[0], c[1], c[2], e.r, e.g, e.b, e.what);
            if (!ok) ++failures;
        }
    }
    const unsigned int stray = np_gx_stray_pipe_writes();
    printf("  %-4s no stray gather-pipe writes (%u)\n", stray == 0 ? "ok" : "FAIL", stray);
    if (stray) ++failures;
    printf("%s: %d failure(s), %llu frame(s) presented\n", failures ? "FAILED" : "PASSED", failures, np_gx_frame_count());
    return failures ? 1 : 0;
}
