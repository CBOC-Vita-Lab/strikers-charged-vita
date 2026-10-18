// Standalone drawing test, part 1: a scene drawn through the game's own GX API (the Wii SDK headers + the glue's
// WGPIPE stand-in), the way the game's material programs do it. No standard headers here (see gx_core.cpp);
// the driver (main.cpp) runs it, saves the picture and checks pixels.
//
// The scene is a grid of 100 x 100 blocks on a 640 x 480 EFB (orthographic, pixel coordinates, y down). Every block
// exercises one path the native port needs, and has one flat colour (or four, for the textures) the driver checks:
//
//   row 0: direct vertices (GXPosition3f32 / GXColor4u8 through WGPIPE)             red
//          indexed arrays, big-endian data, index bytes through WGPIPE (DrawIndexed)  green
//          indexed arrays, little-endian data, per-call flag (NP_GX_SETARRAY)         blue
//          indexed arrays, legacy 3-argument GXSetArray, glue default little-endian   yellow
//   row 1: a display list built the way dlMakeDisplayList builds it (big-endian)       magenta
//          textured block, RGB565 2 x 2 tiles, big-endian texture data, compact object four colours
//          paletted block, CI8 + TLUT, big-endian                                      two colours
//          an EFB copy (GXCopyTex) sampled as a texture                                the copy of the red block
//   row 2: TEV konst colour with GXSetTevKColor / GXSetTevKColorSel                    cyan
//          a display list called twice (display-list cache), and a second list         orange, orange
//          a display list called twice over little-endian arrays                       lime
#include <cstdio>
#include <revolution/gx.h>
#include <revolution/tpl.h>

#include "np_gx.h"
#include "vgx_native.h"

extern "C" void GXProject(f32 x, f32 y, f32 z, const f32 mtx[3][4], const f32* pm, const f32* vp, f32* sx, f32* sy, f32* sz);   // (declared by the game itself, depthoffield.cpp)

namespace {

alignas(32) unsigned char g_arena[1 << 18]; // everything the scene gives to GX lives here (registered as guest RAM)
unsigned int g_arenaUsed = 0;

void* Alloc(unsigned int bytes, unsigned int align = 32) {
    g_arenaUsed = (g_arenaUsed + align - 1) & ~(align - 1);
    void* p = g_arena + g_arenaUsed;
    g_arenaUsed += bytes;
    return p;
}

inline unsigned int BeF(float f) { return __builtin_bswap32(__builtin_bit_cast(unsigned int, f)); }
inline unsigned short Be16(unsigned short v) { return __builtin_bswap16(v); }

void PutBeF(unsigned char* p, float f) {
    const unsigned int v = BeF(f);
    __builtin_memcpy(p, &v, 4);
}
void PutLeF(unsigned char* p, float f) { __builtin_memcpy(p, &f, 4); }

void ColorState(bool indexed, bool ci = false) {
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, indexed ? GX_INDEX16 : GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, indexed ? GX_INDEX16 : GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
    GXSetNumTexGens(0);
    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
}

void TexState() {
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GXSetNumChans(0);
    GXSetNumTexGens(1);
    GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY, GX_FALSE, GX_PTIDENTITY);
    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
    GXSetTevOp(GX_TEVSTAGE0, GX_REPLACE);
}

// A block of four vertices at (x, y) .. (x + 100, y + 100) as a quad, colour given.
struct Block { float x, y; unsigned char r, g, b; };
const float kS = 100.0f;

// ---- block geometry for the indexed paths: 4 positions, 4 colours (RGBA bytes), indices 0..3
void FillArrays(unsigned char* pos, unsigned char* col, const Block& b, bool le) {
    const float xs[4] = {b.x, b.x + kS, b.x + kS, b.x}, ys[4] = {b.y, b.y, b.y + kS, b.y + kS};
    for (int i = 0; i < 4; ++i) {
        if (le) { PutLeF(pos + 12 * i, xs[i]); PutLeF(pos + 12 * i + 4, ys[i]); PutLeF(pos + 12 * i + 8, 0.0f); }
        else { PutBeF(pos + 12 * i, xs[i]); PutBeF(pos + 12 * i + 4, ys[i]); PutBeF(pos + 12 * i + 8, 0.0f); }
        col[4 * i] = b.r; col[4 * i + 1] = b.g; col[4 * i + 2] = b.b; col[4 * i + 3] = 255;
    }
}

void DrawDirectBlock(const Block& b) {
    ColorState(false);
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    const float xs[4] = {b.x, b.x + kS, b.x + kS, b.x}, ys[4] = {b.y, b.y, b.y + kS, b.y + kS};
    for (int i = 0; i < 4; ++i) {
        GXPosition3f32(xs[i], ys[i], 0.0f);
        GXColor4u8(b.r, b.g, b.b, 255);
    }
}

// The material programs' DrawIndexed: GXSetArray for each stream, GXBegin, two index writes per vertex.
void DrawIndexedBlock(const Block& b, int mode) {
    unsigned char* pos = static_cast<unsigned char*>(Alloc(48));
    unsigned char* col = static_cast<unsigned char*>(Alloc(16));
    const bool le = mode != 0;
    FillArrays(pos, col, b, le);
    np_gx_notify_write(pos, 48);
    np_gx_notify_write(col, 16);
    ColorState(true);
    if (mode == 0) {
        NP_GX_SETARRAY(GX_VA_POS, pos, 48, 12, 0);
        NP_GX_SETARRAY(GX_VA_CLR0, col, 16, 4, 0);
    } else if (mode == 1) {
        NP_GX_SETARRAY(GX_VA_POS, pos, 48, 12, 1);
        NP_GX_SETARRAY(GX_VA_CLR0, col, 16, 4, 1);
    } else {
        np_gx_set_array_default_le(1);
        GXSetArray(GX_VA_POS, pos, 12);
        GXSetArray(GX_VA_CLR0, col, 4);
        np_gx_set_array_default_le(0);
    }
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    for (unsigned short i = 0; i < 4; ++i) {
        WGPIPE.us = i;
        WGPIPE.us = i;
    }
}

// What dlMakeDisplayList writes (src/NL/glx/glxDisplayList.cpp), but big-endian as the native port needs it:
// opcode, vertex count, then per vertex one 16-bit index per indexed stream.
unsigned int MakeDisplayList(unsigned char* p, unsigned short count, unsigned char prim) {
    p[0] = prim;
    p[1] = static_cast<unsigned char>(count >> 8);
    p[2] = static_cast<unsigned char>(count);
    unsigned char* q = p + 3;
    for (unsigned short i = 0; i < count; ++i) {
        for (int s = 0; s < 2; ++s) {
            const unsigned short v = i & 3;   // (the block's four corners again and again: a list long enough to be taken by reference, > 64 bytes)
            *q++ = static_cast<unsigned char>(v >> 8);
            *q++ = static_cast<unsigned char>(v);
        }
    }
    unsigned int size = static_cast<unsigned int>(q - p);
    size = (size + 31) & ~31u;
    return size;
}

void DrawListBlock(const Block& b, int times, bool le = false) {
    unsigned char* pos = static_cast<unsigned char*>(Alloc(48));
    unsigned char* col = static_cast<unsigned char*>(Alloc(16));
    FillArrays(pos, col, b, le);
    unsigned char* list = static_cast<unsigned char*>(Alloc(256));
    for (int i = 0; i < 256; ++i) list[i] = 0;
    const unsigned int size = MakeDisplayList(list, 20, GX_QUADS | GX_VTXFMT0);
    np_gx_notify_write(pos, 48);
    np_gx_notify_write(col, 16);
    np_gx_notify_write(list, size);
    ColorState(true);
    NP_GX_SETARRAY(GX_VA_POS, pos, 48, 12, le ? 1 : 0);
    NP_GX_SETARRAY(GX_VA_CLR0, col, 16, 4, le ? 1 : 0);
    for (int i = 0; i < times; ++i) GXCallDisplayList(list, size);
}

// ---- textures ------------------------------------------------------------------------------------
// 8 x 8 RGB565, tiled 4 x 4, big-endian: tile order is row-major over the picture: red, green / blue, white.
unsigned char* MakeRgb565Quad() {
    unsigned char* t = static_cast<unsigned char*>(Alloc(128));
    const unsigned short colour[4] = {0xF800, 0x07E0, 0x001F, 0xFFFF};
    for (int tile = 0; tile < 4; ++tile)
        for (int i = 0; i < 16; ++i) {
            const unsigned short v = Be16(colour[tile]);
            __builtin_memcpy(t + tile * 32 + i * 2, &v, 2);
        }
    return t;
}

void DrawTexturedBlock(const Block& b) {
    unsigned char* img = MakeRgb565Quad();
    np_gx_notify_write(img, 128);
    alignas(8) GXTexObj tex;
    GXInitTexObj(&tex, img, 8, 8, GX_TF_RGB565, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GXInitTexObjLOD(&tex, GX_NEAR, GX_NEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
    // (as glx_BindTexture: the object is copied around by value, then bound)
    unsigned long copy[8];
    __builtin_memcpy(copy, &tex, sizeof(copy));
    GXInitTexObjWrapMode(reinterpret_cast<GXTexObj*>(copy), GX_CLAMP, GX_CLAMP);
    GXLoadTexObj(reinterpret_cast<GXTexObj*>(copy), GX_TEXMAP0);
    TexState();
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    const float xs[4] = {b.x, b.x + kS, b.x + kS, b.x}, ys[4] = {b.y, b.y, b.y + kS, b.y + kS};
    const float us[4] = {0, 1, 1, 0}, vs[4] = {0, 0, 1, 1};
    for (int i = 0; i < 4; ++i) {
        GXPosition3f32(xs[i], ys[i], 0.0f);
        GXTexCoord2f32(us[i], vs[i]);
    }
}

// 8 x 4 CI8 (one 8 x 4 tile): left four columns index 0, right four index 1; palette RGB565: orange, teal.
void DrawPalettedBlock(const Block& b) {
    unsigned char* img = static_cast<unsigned char*>(Alloc(32));
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 8; ++col) img[row * 8 + col] = col < 4 ? 0 : 1;
    unsigned short* pal = static_cast<unsigned short*>(Alloc(512));
    for (int i = 0; i < 256; ++i) pal[i] = 0;
    pal[0] = Be16(0xFC00);   // R 31, G 32, B 0: orange-ish (255, 130, 0)
    pal[1] = Be16(0x07FF);   // G 63, B 31: cyan (0, 255, 255)
    np_gx_notify_write(img, 32);
    np_gx_notify_write(pal, 512);
    alignas(8) GXTexObj tex;
    alignas(8) GXTlutObj tlut;
    GXInitTlutObj(&tlut, pal, GX_TL_RGB565, 256);
    GXInitTexObjCI(&tex, img, 8, 4, GX_TF_C8, GX_CLAMP, GX_CLAMP, GX_FALSE, 0);
    GXInitTexObjLOD(&tex, GX_NEAR, GX_NEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
    GXInitTexObjTlut(&tex, GX_TEXMAP1);
    GXLoadTlut(&tlut, GX_TEXMAP1);
    GXLoadTexObj(&tex, GX_TEXMAP1);
    TexState();
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP1, GX_COLOR_NULL);
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    const float xs[4] = {b.x, b.x + kS, b.x + kS, b.x}, ys[4] = {b.y, b.y, b.y + kS, b.y + kS};
    const float us[4] = {0, 1, 1, 0}, vs[4] = {0, 0, 1, 1};
    for (int i = 0; i < 4; ++i) {
        GXPosition3f32(xs[i], ys[i], 0.0f);
        GXTexCoord2f32(us[i], vs[i]);
    }
}

void DrawKonstBlock(const Block& b) {
    // TEV stage 0: output the konst colour 0 selected for the stage (cyan, via GXSetTevKColor / GXSetTevKColorSel).
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetNumChans(0);
    GXSetNumTexGens(0);
    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR_NULL);
    GXColor k = {0, 255, 255, 255};
    GXSetTevKColor(GX_KCOLOR0, k);
    GXSetTevKColorSel(GX_TEVSTAGE0, GX_TEV_KCSEL_K0);
    GXSetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_K0_A);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_KONST);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    const float xs[4] = {b.x, b.x + kS, b.x + kS, b.x}, ys[4] = {b.y, b.y, b.y + kS, b.y + kS};
    for (int i = 0; i < 4; ++i) GXPosition3f32(xs[i], ys[i], 0.0f);
}

} // namespace

extern "C" {

void* np_test_arena(unsigned int* size) {
    *size = sizeof(g_arena);
    return g_arena;
}

// One-time set-up the way glplatStartup / glx_InitGX do it, then one frame. `frame` counts from 0.
void np_test_draw_frame(int frame) {
    if (frame == 0) {
        static unsigned char fifo[0x10000];
        np_gx_set_efb_size(640, 480);   // (the game's VIConfigure runs before its GXInit: the size must survive GXInit)
        GXInit(fifo, sizeof(fifo));
        GXSetViewport(0, 0, 640, 480, 0, 1);
        GXSetScissor(0, 0, 640, 480);
        GXColor clear = {32, 48, 64, 255};   // (the picture's background: 32, 48, 64)
        GXSetCopyClear(clear, 0xFFFFFF);
        GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
        GXSetCullMode(GX_CULL_NONE);
        GXSetBlendMode(GX_BM_NONE, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
        GXSetPixelFmt(GX_PF_RGBA6_Z24, GX_ZC_LINEAR);
    }
    // Orthographic projection in pixels, y down (what the game's ortho views come to).
    float proj[4][4] = {{2.0f / 640, 0, 0, -1}, {0, -2.0f / 480, 0, 1}, {0, 0, -1, 0}, {0, 0, 0, 1}};
    GXSetProjection(proj, GX_ORTHOGRAPHIC);
    float id[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    GXLoadPosMtxImm(id, GX_PNMTX0);
    GXSetCurrentMtx(GX_PNMTX0);

    g_arenaUsed = 0; // (the arena is reused every frame, like the game's per-frame allocator)

    // row 0
    DrawDirectBlock({20, 20, 255, 0, 0});
    DrawIndexedBlock({160, 20, 0, 255, 0}, 0);
    DrawIndexedBlock({300, 20, 0, 0, 255}, 1);
    DrawIndexedBlock({440, 20, 255, 255, 0}, 2);
    // row 1
    DrawListBlock({20, 160, 255, 0, 255}, 1);
    DrawTexturedBlock({160, 160, 0, 0, 0});
    DrawPalettedBlock({300, 160, 0, 0, 0});
    // the EFB copy: copy the red block (20,20)..(120,120) into a texture, draw it at (440, 160)
    {
        unsigned char* copyBuf = static_cast<unsigned char*>(Alloc(8192, 32));
        GXSetTexCopySrc(20, 20, 100, 100);
        GXSetTexCopyDst(50, 50, GX_TF_RGB565, GX_TRUE);   // (half size: a mip-map style reduction)
        GXCopyTex(copyBuf, GX_FALSE);
        alignas(8) GXTexObj tex;
        GXInitTexObj(&tex, copyBuf, 50, 50, GX_TF_RGB565, GX_CLAMP, GX_CLAMP, GX_FALSE);
        GXInitTexObjLOD(&tex, GX_NEAR, GX_NEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
        GXLoadTexObj(&tex, GX_TEXMAP2);
        TexState();
        GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP2, GX_COLOR_NULL);
        GXBegin(GX_QUADS, GX_VTXFMT0, 4);
        const float xs[4] = {440, 540, 540, 440}, ys[4] = {160, 160, 260, 260};
        const float us[4] = {0, 1, 1, 0}, vs[4] = {0, 0, 1, 1};
        for (int i = 0; i < 4; ++i) {
            GXPosition3f32(xs[i], ys[i], 0.0f);
            GXTexCoord2f32(us[i], vs[i]);
        }
    }
    // row 2
    DrawKonstBlock({20, 300, 0, 0, 0});
    DrawListBlock({160, 300, 255, 128, 0}, 2);
    DrawListBlock({300, 300, 255, 128, 0}, 1);   // a second list, at another address
    DrawListBlock({440, 300, 128, 255, 0}, 2, true);   // a display list over little-endian arrays (the display-list cache's LE path)

    // the end of the frame, as swap_Pre / swap_Post do it
    GXSetCopyClear({32, 48, 64, 255}, 0xFFFFFF);
    GXCopyDisp(0, GX_TRUE);
    GXSetDrawDone();
    GXFlush();
    GXWaitDrawDone();
}

} // extern "C"

// ---------------------------------------------------------------------------------------------------- numeric tests
namespace {
bool Near(float a, float b, float tol = 1e-4f) { return (a > b ? a - b : b - a) <= tol; }
void PutBe32(unsigned char* base, unsigned int off, unsigned int v) {
    const unsigned int b = __builtin_bswap32(v);
    __builtin_memcpy(base + off, &b, 4);
}
void PutBe16(unsigned char* base, unsigned int off, unsigned int v) {
    const unsigned short b = __builtin_bswap16(static_cast<unsigned short>(v));
    __builtin_memcpy(base + off, &b, 2);
}
int g_cbCalls = 0;
void CbA() { ++g_cbCalls; }
void CbB() { g_cbCalls += 10; }
} // namespace

extern "C" {

// Calls GXSetFog the way glxFog.cpp does; the driver then reads BP register 0xF2 (the fog colour).
void np_test_set_fog() { GXSetFog(GX_FOG_PERSP_LIN, GXColor{10, 20, 30, 255}, 1.0f, 100.0f, 0.1f, 1000.0f); }

// Returns a bit mask of failed checks (bit i = check i, see main.cpp for the names).
unsigned int np_test_numeric() {
    unsigned int fail = 0;
    // 0..2: light helpers (SDK maths, hardware light layout: 3 pad words, colour, a[3], k[3], pos[3], dir[3])
    {
        alignas(8) GXLightObj l;
        __builtin_memset(&l, 0, sizeof(l));
        float* w = reinterpret_cast<float*>(&l);
        GXInitLightDistAttn(&l, 100.0f, 0.5f, GX_DA_GENTLE);
        if (!(Near(w[7], 1.0f) && Near(w[8], 0.01f) && Near(w[9], 0.0f))) fail |= 1u << 0;
        GXInitSpecularDir(&l, 0.0f, 0.0f, 1.0f);   // dir (0, 0, 0)+(0,0,0): mag 0
        GXInitSpecularDir(&l, 0.6f, 0.0f, 0.0f);   // d = (-0.6, 0, 1): |d| = 1.1662
        const float m = 1.0f / __builtin_sqrtf(0.36f + 1.0f);
        if (!(Near(w[13], -0.6f * m, 1e-4f) && Near(w[15], 1.0f * m, 1e-4f) && w[10] < -1e17f)) fail |= 1u << 1;
        GXInitLightAttnA(&l, 1.0f, 2.0f, 3.0f);
        if (!(Near(w[4], 1.0f) && Near(w[5], 2.0f) && Near(w[6], 3.0f))) fail |= 1u << 2;
    }
    // 3, 4: projection round trip through the register shadow, perspective and orthographic
    {
        float p[4][4] = {{1.5f, 0, 0.25f, 0}, {0, 2.0f, -0.125f, 0}, {0, 0, -1.002f, -0.2002f}, {0, 0, -1, 0}};
        GXSetProjection(p, GX_PERSPECTIVE);
        float v[GX_PROJECTION_SZ];
        GXGetProjectionv(v);
        if (!(v[0] == 0.0f && Near(v[1], 1.5f) && Near(v[2], 0.25f) && Near(v[3], 2.0f) && Near(v[4], -0.125f) && Near(v[5], -1.002f) && Near(v[6], -0.2002f))) fail |= 1u << 3;
        float o[4][4] = {{0.5f, 0, 0, -0.5f}, {0, 0.25f, 0, 0.75f}, {0, 0, -0.1f, -1.1f}, {0, 0, 0, 1}};
        GXSetProjection(o, GX_ORTHOGRAPHIC);
        GXGetProjectionv(v);
        float v2[GX_PROJECTION_SZ];
        GXSetProjectionv(v);
        GXGetProjectionv(v2);
        bool same = true;
        for (int i = 0; i < GX_PROJECTION_SZ; ++i) same = same && v[i] == v2[i];
        if (!(same && v[0] == 1.0f && Near(v[2], -0.5f) && Near(v[4], 0.75f))) fail |= 1u << 4;
    }
    // 5: viewport
    {
        GXSetViewport(10.0f, 20.0f, 300.0f, 200.0f, 0.25f, 0.75f);
        float vp[GX_VIEWPORT_SZ];
        GXGetViewportv(vp);
        if (!(Near(vp[0], 10.0f, 1e-3f) && Near(vp[1], 20.0f, 1e-3f) && Near(vp[2], 300.0f, 1e-3f) && Near(vp[3], 200.0f, 1e-3f) && Near(vp[4], 0.25f, 1e-3f) && Near(vp[5], 0.75f, 1e-3f))) fail |= 1u << 5;
        GXSetViewport(0, 0, 640, 480, 0, 1);
    }
    // 6: GXProject of the viewport centre
    {
        float m[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, -10}};
        float pm[7] = {0.0f, 1.0f, 0.0f, 1.0f, 0.0f, -1.0f, -1.0f};   // perspective-style coefficients: x, y pass through
        float vp[6] = {0, 0, 640, 480, 0, 1};
        float sx, sy, sz;
        GXProject(0.0f, 0.0f, 0.0f, m, pm, vp, &sx, &sy, &sz);
        if (!(Near(sx, 320.0f, 1e-2f) && Near(sy, 240.0f, 1e-2f))) fail |= 1u << 6;
    }
    // 7: (TPLBind: NPS owns TPL, ref tpl/TPL.c with the structure words swapped in place; not part of the glue)
    // 8: draw-done callback: GXSetDrawDone delivers it at once, GXSetDrawDoneCallback returns the previous one
    {
        g_cbCalls = 0;
        GXDrawDoneCallback old = GXSetDrawDoneCallback(CbA);
        GXSetDrawDone();
        GXDrawDoneCallback prev = GXSetDrawDoneCallback(CbB);
        GXSetDrawDone();
        GXSetDrawDoneCallback(old);
        if (!(g_cbCalls == 11 && prev == CbA)) fail |= 1u << 8;
    }
    // 9: GP status as an idle processor
    {
        GXBool a = 7, b = 7, c = 0, d = 0, e = 7;
        GXGetGPStatus(&a, &b, &c, &d, &e);
        if (!(a == 0 && b == 0 && c == 1 && d == 1 && e == 0)) fail |= 1u << 9;
    }
    // 10: texture object accessors (compact object), LOD quantisation
    {
        alignas(8) GXTexObj tex;
        static unsigned char img[64];
        GXInitTexObj(&tex, img, 16, 8, GX_TF_RGB565, GX_REPEAT, GX_MIRROR, GX_TRUE);
        GXInitTexObjLOD(&tex, GX_LIN_MIP_LIN, GX_LINEAR, 0.0f, 3.0f, 0.5f, GX_FALSE, GX_FALSE, GX_ANISO_1);
        void* image = 0;
        u16 w = 0, h = 0;
        GXTexFmt fmt;
        GXTexWrapMode ws, wt;
        GXBool mip;
        GXGetTexObjAll(&tex, &image, &w, &h, &fmt, &ws, &wt, &mip);
        GXTexFilter minF, magF;
        f32 minLod, maxLod, bias;
        GXBool biasClamp, edge;
        GXAnisotropy an;
        GXGetTexObjLODAll(&tex, &minF, &magF, &minLod, &maxLod, &bias, &biasClamp, &edge, &an);
        if (!(GXGetTexObjWidth(&tex) == 16 && GXGetTexObjHeight(&tex) == 8 && GXGetTexObjFmt(&tex) == GX_TF_RGB565 && GXGetTexObjWrapS(&tex) == GX_REPEAT &&
              GXGetTexObjWrapT(&tex) == GX_MIRROR && GXGetTexObjMipMap(&tex) && image == img && w == 16 && h == 8 && fmt == GX_TF_RGB565 && ws == GX_REPEAT && wt == GX_MIRROR && mip &&
              minF == GX_LIN_MIP_LIN && magF == GX_LINEAR && Near(minLod, 0.0f) && Near(maxLod, 3.0f) && Near(bias, 0.5f)))
            fail |= 1u << 10;
    }
    // 11: render-mode helper: GXAdjustForOverscan against the SDK's arithmetic
    {
        GXRenderModeObj in = GXNtsc480IntDf, out;
        GXAdjustForOverscan(&in, &out, 0, 16);
        if (!(out.fbWidth == in.fbWidth && out.efbHeight == in.efbHeight - ((32u * in.efbHeight) / in.xfbHeight) && out.viYOrigin == in.viYOrigin + 16)) fail |= 1u << 11;
    }
    // 12: dynamic aspect (np_gx.h): a full-screen perspective projection is widened to the surface (640 x 480 here: Vert+, row 1 of the
    // projection divided by (16/9)/(640/480) = 4/3), the game reads back its own matrix, an orthographic projection and a projection under
    // a small viewport are left alone, and a projection set before its viewport is issued again.
    {
        auto reg = [](unsigned int addr) { return __builtin_bit_cast(float, vgxn_xf_reg(addr)); };
        float p[4][4] = {{1.5f, 0, 0.25f, 0}, {0, 2.0f, -0.125f, 0}, {0, 0, -1.002f, -0.2002f}, {0, 0, -1, 0}};
        float v[GX_PROJECTION_SZ];
        bool c[8];
        np_gx_set_dynamic_aspect(1);
        GXSetViewport(0, 0, 640, 448, 0, 1);
        GXSetProjection(p, GX_PERSPECTIVE);
        c[0] = Near(reg(0x1020), 1.5f) && Near(reg(0x1021), 0.25f) && Near(reg(0x1022), 1.5f) && Near(reg(0x1023), -0.125f * 0.75f);   // row 1 / (4/3), row 0 as it was
        GXGetProjectionv(v);
        c[1] = Near(v[1], 1.5f) && Near(v[3], 2.0f) && Near(v[4], -0.125f);                                  // the game reads back its own matrix
        GXSetViewport(0, 0, 320, 224, 0, 1);                                                                 // the scene's own matrix under a small viewport (heat haze): stays widened
        c[2] = Near(reg(0x1022), 1.5f);
        float q[4][4] = {{1.0f, 0, 0, 0}, {0, 3.0f, 0, 0}, {0, 0, -1.002f, -0.2002f}, {0, 0, -1, 0}};
        GXSetProjection(q, GX_PERSPECTIVE);                                                                  // another matrix under the small viewport (a shadow pass): as the game set it
        c[2] = c[2] && Near(reg(0x1022), 3.0f);
        GXSetViewport(0, 0, 640, 448, 0, 1);                                                                 // that projection was set before this (full-screen) viewport: issued again, widened
        c[3] = Near(reg(0x1022), 2.25f, 1e-3f);
        GXSetProjection(p, GX_PERSPECTIVE);
        float o[4][4] = {{0.5f, 0, 0, -0.5f}, {0, 0.25f, 0, 0.75f}, {0, 0, -0.1f, -1.1f}, {0, 0, 0, 1}};
        GXSetProjection(o, GX_ORTHOGRAPHIC);                                                                 // orthographic: never
        c[4] = Near(reg(0x1022), 0.25f) && Near(reg(0x1023), 0.75f);
        np_gx_set_dynamic_aspect(0);
        GXSetProjection(p, GX_PERSPECTIVE);                                                                  // switched off
        c[5] = Near(reg(0x1022), 2.0f);
        np_gx_set_dynamic_aspect(-1);                                                                        // follow the game (no SDK layer here: on)
        GXSetProjection(p, GX_PERSPECTIVE);
        c[6] = Near(reg(0x1022), 1.5f);
        GXSetViewport(0, 0, 640, 480, 0, 1);
        bool ok = true;
        for (int i = 0; i < 7; ++i) {
            ok = ok && c[i];
            if (!c[i]) std::fprintf(stderr, "dynamic aspect check %d failed (registers %g %g %g %g)\n", i, reg(0x1020), reg(0x1021), reg(0x1022), reg(0x1023));
        }
        if (!ok) fail |= 1u << 12;
    }
    return fail;
}

} // extern "C"
