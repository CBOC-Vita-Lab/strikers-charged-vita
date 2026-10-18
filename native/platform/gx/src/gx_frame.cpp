// GX glue, part 3: frames and EFB copies (SDK headers, builtins only). The copy-destination bookkeeping that needs
// standard containers is in gx_copytrack.cpp.
//
// A frame ends with GXCopyDisp (the game calls it from its swap code with the XFB it is drawing to): the renderer's
// recorder gets the display copy and the frame is handed over (vgxn_end_frame). That is the same place the recompiled
// build's GX HLE presents (GX__CopyDisp_803A4930: GXDrawDone, GXCopyDisp, VI_HLE_PresentFrame). Pacing to the video
// retrace belongs to the SDK layer: the game calls VIWaitForRetrace itself after VISetNextFrameBuffer / VIFlush.
#include <revolution/gx.h>
#include <revolution/vi/viPublicTypes.h>

#include "vgx_native.h"
#include "gx_internal.h"

extern "C" {

// The game sets the display-copy source rectangle to the whole EFB (fbWidth x efbHeight of its render mode) right after
// it configures the video mode; the recompiled build learned the EFB size from VIConfigure. When the SDK layer does not
// report it (np_gx_set_efb_size), the rectangle at the top-left corner is taken for it: the renderer's render target
// has the EFB's size.
void GXSetDispCopySrc(u16 left, u16 top, u16 wd, u16 ht) {
    vgxapi_GXSetDispCopySrc(left, top, wd, ht);
    if (!np_gx::EfbSizeReported() && left == 0 && top == 0 && wd > 0 && ht > 0) vgxn_set_efb_size(wd, ht);
}

void GXCopyDisp(void* dest, GXBool clear) {
    np_gx::TraceFrameTick();
    np_gx::DrainRetiredCopies();
    vgxapi_GXCopyDisp(dest, clear);
    vgxn_end_frame();
    np_gx_present_hook hook = np_gx::PresentHook();
    if (hook) hook(dest);
}

// An EFB copy into texture memory: the texture the renderer makes of it lives GPU-side, keyed by the destination
// address; the game's later GXLoadTexObj of that address samples it. The destination range is remembered so that a
// data-cache flush over it (the game reusing the memory for something else) retires the GPU texture.
void GXCopyTex(void* dest, GXBool clear) {
    np_gx::DrainRetiredCopies();
    vgxapi_GXCopyTex(dest, clear);
    unsigned int w = 0, h = 0, f = 0;
    vgxn_tex_copy_dst(&w, &h, &f);
    np_gx::RememberCopy(dest, GXGetTexBufferSize(static_cast<u16>(w), static_cast<u16>(h), f, 0, 0));
}

// The copy's clear colour and depth: BP registers 0x4F (alpha, red), 0x50 (green, blue), 0x51 (depth, 24 bits).
// (vgx's GXApplyBPReg writes a BP register through the recorder.)
void GXSetCopyClear(GXColor color, u32 z) {
    GXApplyBPReg(0x4F, (static_cast<unsigned int>(color.a) << 8) | color.r);
    GXApplyBPReg(0x50, (static_cast<unsigned int>(color.g) << 8) | color.b);
    GXApplyBPReg(0x51, z & 0xFFFFFFu);
}

// The vertical filter: the picture is drawn into the renderer's own frame buffer without a scaling pass, so the
// scale only has to come out the way the SDK computes it (the game reads the XFB height it implies).
static u32 NumXfbLines(u32 efbHt, u32 iScale) {
    u32 realHt = ((efbHt - 1) * 0x100) / iScale + 1;
    u32 iScaleD = iScale;
    if (iScaleD > 0x80 && iScaleD < 0x100) {
        while (iScaleD % 2 == 0) iScaleD /= 2;
        if (efbHt % iScaleD == 0) ++realHt;
    }
    if (realHt > 0x400) realHt = 0x400;
    return realHt;
}

u32 GXSetDispCopyYScale(f32 scale) {
    const u32 iScale = static_cast<u32>(256.0f / scale) & 0x1FF;
    return NumXfbLines(vgxn_disp_copy_height(), iScale ? iScale : 256);
}

f32 GXGetYScaleFactor(u16 efbHeight, u16 xfbHeight) {
    f32 yScale = static_cast<f32>(xfbHeight) / static_cast<f32>(efbHeight);
    u32 tgtHt = xfbHeight;
    u32 iScale = static_cast<u32>(256.0f / yScale) & 0x1FF;
    u32 realHt = NumXfbLines(efbHeight, iScale);
    while (realHt > xfbHeight) {
        --tgtHt;
        yScale = static_cast<f32>(tgtHt) / static_cast<f32>(efbHeight);
        iScale = static_cast<u32>(256.0f / yScale) & 0x1FF;
        realHt = NumXfbLines(efbHeight, iScale);
    }
    f32 fScale = yScale;
    while (realHt < xfbHeight) {
        fScale = yScale;
        ++tgtHt;
        yScale = static_cast<f32>(tgtHt) / static_cast<f32>(efbHeight);
        iScale = static_cast<u32>(256.0f / yScale) & 0x1FF;
        realHt = NumXfbLines(efbHeight, iScale);
    }
    return fScale;
}

void GXAdjustForOverscan(GXRenderModeObj* in, GXRenderModeObj* out, u16 horiz, u16 vert) {
    const u16 hor2 = static_cast<u16>(horiz * 2);
    const u16 ver2 = static_cast<u16>(vert * 2);
    if (in != out) *out = *in;
    const u32 viTVMode = in->viTVmode & 3;
    out->fbWidth = static_cast<u16>(in->fbWidth - hor2);
    const u32 verf = in->efbHeight;
    out->efbHeight = static_cast<u16>(verf - ((ver2 * verf) / in->xfbHeight));
    if (in->xfbMode == VI_XFB_MODE_SF && viTVMode == 0) out->xfbHeight = static_cast<u16>(in->xfbHeight - (ver2 / 2));
    else out->xfbHeight = static_cast<u16>(in->xfbHeight - ver2);
    out->viWidth = static_cast<u16>(in->viWidth - hor2);
    if (viTVMode == 1) out->viHeight = static_cast<u16>(in->viHeight - (ver2 * 2));
    else out->viHeight = static_cast<u16>(in->viHeight - ver2);
    out->viXOrigin = static_cast<u16>(in->viXOrigin + horiz);
    out->viYOrigin = static_cast<u16>(in->viYOrigin + vert);
}

} // extern "C"
