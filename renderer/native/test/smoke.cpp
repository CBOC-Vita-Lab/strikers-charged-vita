// Smoke test of the native library through vgx's own (Dolphin-named) API: one triangle in immediate mode, one
// frame, PNG dump. Built with native/vgx_native_rename.h force-included (GXInit etc. are vgxapi_*).
//   make -f native/native.mk smoke [NBACKEND=gl|null]  ->  <out>/smoke  (run it in an empty directory)
#include <dolphin/gx.h>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/gx/GXExtra.h>
#include <dolphin/vi.h>

#define VGXN_NO_VGXAPI_DECLS 1
#include "vgx_native.h"

#include <cstdio>
#include <cstring>

int main() {
    vgxn_config cfg{};
    cfg.size = sizeof cfg;
    if (!vgxn_init(&cfg)) { std::fprintf(stderr, "vgxn_init failed\n"); return 2; }
    GXInit(nullptr, 0);
    GXRenderModeObj rm{};
    rm.fbWidth = 640; rm.efbHeight = 480; rm.xfbHeight = 480;
    VIConfigure(&rm);
    vgxn_set_efb_size(640, 480);
    GXSetViewport(0, 0, 640, 480, 0, 1);
    GXSetScissor(0, 0, 640, 480);
    GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GXSetCullMode(GX_CULL_NONE);
    float proj[4][4] = {{2.0f / 640, 0, 0, -1}, {0, -2.0f / 480, 0, 1}, {0, 0, -1, 0}, {0, 0, 0, 1}};
    GXSetProjection(proj, GX_ORTHOGRAPHIC);
    float id[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    GXLoadPosMtxImm(id, GX_PNMTX0);
    GXSetCurrentMtx(GX_PNMTX0);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
    GXSetNumTexGens(0);
    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    vgxn_request_dump("smoke.png");
    GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
    GXPosition3f32(100, 400, 0); GXColor4u8(255, 0, 0, 255);
    GXPosition3f32(320, 60, 0); GXColor4u8(0, 255, 0, 255);
    GXPosition3f32(540, 400, 0); GXColor4u8(0, 0, 255, 255);
    GXEnd();
    GXCopyDisp(nullptr, GX_TRUE);
    vgxn_end_frame();
    int w = 0, h = 0;
    const uint8_t* px = vgxn_last_picture(&w, &h);
    std::printf("picture %dx%d%s\n", w, h, px ? "" : " (none)");
    if (px && w == 640) {
        const uint8_t* c = px + (static_cast<size_t>(300) * w + 320) * 4;
        std::printf("centre pixel (320,300) = %u %u %u %u\n", c[0], c[1], c[2], c[3]);
        const uint8_t* b = px + (static_cast<size_t>(10) * w + 10) * 4;
        std::printf("corner pixel (10,10) = %u %u %u %u\n", b[0], b[1], b[2], b[3]);
    }
    return 0;
}
