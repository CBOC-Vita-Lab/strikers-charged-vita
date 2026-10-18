// The Dolphin-SDK-named C API the runtime's GX HLE calls (declared in
// aurora-main/include/dolphin/gx/*.h). Each setter is encoded into the
// hardware register it controls, exactly like the SDK does, so that the API
// and raw command streams share one state.
//
// Register encodings follow the public GX documentation; the fork-specific
// entry points (GXSetArray with a host pointer, GXInitTexObjData,
// GXApplyBPReg, GXSetSourceVtxDesc, GXCallDisplayList with a host pointer,
// GXDestroyCopyTex, aurora::gx::fifo::submit_raw_draw) follow the semantics
// of Aurora's implementation (MIT), which was used as the reference.
#include <dolphin/gx.h>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/gx/GXExtra.h>
#include <dolphin/vi.h>

#include "vgx_rec.h"

#include <cmath>

using namespace vgx;

static_assert(sizeof(TexObj) <= sizeof(GXTexObj), "host texobj must fit the public struct");
static_assert(sizeof(TlutObj) <= sizeof(GXTlutObj), "host tlutobj must fit the public struct");
static_assert(sizeof(LightObj) == sizeof(GXLightObj), "light object layout");

namespace {
GXFifoObj g_fifo{};
constexpr uint32_t kTexMagic = 0x76677854u; // "vgxT"
uint32_t g_texLoadSerial = 0;
uint32_t g_tlutVersion = 0;

inline TexObj* T(GXTexObj* o) { return reinterpret_cast<TexObj*>(o); }
inline TlutObj* TL(GXTlutObj* o) { return reinterpret_cast<TlutObj*>(o); }
inline LightObj* L(GXLightObj* o) { return reinterpret_cast<LightObj*>(o); }

inline uint32_t XfReg(uint32_t addr) { return rec::gs.xfReg[addr - 0x1000]; }
inline uint32_t PackColor(GXColor c) { return (uint32_t(c.r) << 24) | (uint32_t(c.g) << 16) | (uint32_t(c.b) << 8) | c.a; }

void SetMatIdx(bool b, int shift, uint32_t value) {
    // Always written, like the SDK does: a display list may have changed the
    // real register behind the shadow's back.
    const uint32_t reg = b ? rec::gs.matIdxB : rec::gs.matIdxA;
    const uint32_t nv = (reg & ~(0x3Fu << shift)) | ((value & 0x3Fu) << shift);
    rec::XFReg(b ? 0x1019 : 0x1018, nv);
}

void InitTex(TexObj* o, const void* data, u16 w, u16 h, u32 fmt, GXTexWrapMode ws, GXTexWrapMode wt, GXBool mip) {
    const uint32_t version = (o->magic == kTexMagic) ? o->dataVersion + 1 : 1;
    std::memset(o, 0, sizeof(GXTexObj));
    o->data = data;
    o->width = w;
    o->height = h;
    o->format = fmt;
    o->wrapS = static_cast<uint8_t>(ws);
    o->wrapT = static_cast<uint8_t>(wt);
    o->mipmap = mip ? 1 : 0;
    o->minFilter = mip ? GX_LIN_MIP_LIN : GX_LINEAR;
    o->magFilter = GX_LINEAR;
    o->minLod = 0;
    uint32_t maxDim = w > h ? w : h, lod = 0;
    while (maxDim > 1) { maxDim >>= 1; ++lod; }
    o->maxLod = mip ? static_cast<float>(lod) : 0.0f;
    o->dataVersion = version;
    o->magic = kTexMagic;
}
} // namespace

extern "C" {

// ------------------------------------------------------------ management
// The runtime replaces the game's GXInit wholesale and never runs the SDK's
// __GXInitGX, so the documented power-on defaults have to be established here
// (the game only sets what it wants different from them).
static void ApplyInitDefaults() {
    static const float ident[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    const GXColor black = {0, 0, 0, 0}, white = {255, 255, 255, 255};
    rec::BP(0x4F, 0xFF40); // copy clear colour (0x40,0x40,0x40,0xFF): AR
    rec::BP(0x50, 0x4040); // GB
    rec::BP(0x51, 0xFFFFFF);
    for (int i = 0; i < 8; ++i)
        GXSetTexCoordGen2(static_cast<GXTexCoordID>(i), GX_TG_MTX2x4, static_cast<GXTexGenSrc>(GX_TG_TEX0 + i), GX_IDENTITY, GX_FALSE, GX_PTIDENTITY);
    GXSetNumTexGens(1);
    GXClearVtxDesc();
    GXSetLineWidth(6, GX_TO_ZERO);
    GXSetPointSize(6, GX_TO_ZERO);
    GXLoadPosMtxImm(ident, GX_PNMTX0);
    GXLoadNrmMtxImm(ident, GX_PNMTX0);
    GXSetCurrentMtx(GX_PNMTX0);
    GXLoadTexMtxImm(ident, GX_IDENTITY, GX_MTX3x4);
    GXLoadTexMtxImm(ident, GX_PTIDENTITY, GX_MTX3x4);
    rec::XFReg(0x1012, 1); // dual texture transform enabled
    GXSetViewport(0.0f, 0.0f, 640.0f, 480.0f, 0.0f, 1.0f);
    GXSetCoPlanar(GX_DISABLE);
    GXSetCullMode(GX_CULL_BACK);
    GXSetClipMode(GX_CLIP_ENABLE);
    GXSetScissor(0, 0, 640, 480);
    GXSetScissorBoxOffset(0, 0);
    GXSetNumChans(0);
    GXSetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
    GXSetChanAmbColor(GX_COLOR0A0, black);
    GXSetChanMatColor(GX_COLOR0A0, white);
    GXSetChanCtrl(GX_COLOR1A1, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
    GXSetChanAmbColor(GX_COLOR1A1, black);
    GXSetChanMatColor(GX_COLOR1A1, white);
    GXSetNumIndStages(0);
    for (int i = 0; i < 4; ++i) GXSetIndTexCoordScale(static_cast<GXIndTexStageID>(i), GX_ITS_1, GX_ITS_1);
    for (int i = 0; i < 16; ++i) {
        const GXTevStageID st = static_cast<GXTevStageID>(i);
        GXSetTevDirect(st);
        if (i < 8) GXSetTevOrder(st, static_cast<GXTexCoordID>(i), static_cast<GXTexMapID>(i), GX_COLOR0A0);
        else GXSetTevOrder(st, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR_NULL);
        GXSetTevKColorSel(st, GX_TEV_KCSEL_1_4);
        GXSetTevKAlphaSel(st, GX_TEV_KASEL_1);
        GXSetTevSwapMode(st, GX_TEV_SWAP0, GX_TEV_SWAP0);
    }
    GXSetNumTevStages(1);
    GXSetTevOp(GX_TEVSTAGE0, GX_REPLACE);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GXSetZTexture(GX_ZT_DISABLE, GX_TF_Z8, 0);
    GXSetTevSwapModeTable(GX_TEV_SWAP0, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_ALPHA);
    GXSetTevSwapModeTable(GX_TEV_SWAP1, GX_CH_RED, GX_CH_RED, GX_CH_RED, GX_CH_ALPHA);
    GXSetTevSwapModeTable(GX_TEV_SWAP2, GX_CH_GREEN, GX_CH_GREEN, GX_CH_GREEN, GX_CH_ALPHA);
    GXSetTevSwapModeTable(GX_TEV_SWAP3, GX_CH_BLUE, GX_CH_BLUE, GX_CH_BLUE, GX_CH_ALPHA);
    GXSetFog(GX_FOG_NONE, 0.0f, 1.0f, 0.1f, 1.0f, black);
    GXSetBlendMode(GX_BM_NONE, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    GXSetColorUpdate(GX_TRUE);
    GXSetAlphaUpdate(GX_TRUE);
    GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    GXSetZCompLoc(GX_TRUE);
    GXSetDither(GX_TRUE);
    GXSetDstAlpha(GX_FALSE, 0);
    GXSetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
    GXSetDispCopySrc(0, 0, 640, 480);
    GXSetDispCopyDst(640, 480);
}

GXFifoObj* GXInit(void*, u32) {
    rec::Reset();
    ApplyInitDefaults();
    return &g_fifo;
}
GXFifoObj* GXGetCPUFifo(void) { return &g_fifo; }
void GXInitFifoBase(GXFifoObj*, void*, u32) {}
void GXSetCPUFifo(GXFifoObj*) {}
void GXSetGPFifo(GXFifoObj*) {}
void GXSaveCPUFifo(GXFifoObj*) {}
void GXFlush(void) {}
void GXDrawDone(void) {} // the frame's commands are handed over at present time (QueuePresent)
void GXPixModeSync(void) {}
void __GXSetSUTexRegs(void) {} // texcoord scale is derived from the bound textures at draw time

// ------------------------------------------------------------- geometry
void GXClearVtxDesc(void) {
    for (int i = 0; i <= 20; ++i)
        if (i != GX_VA_POS) rec::Vcd(i, GX_NONE);
    rec::Vcd(GX_VA_POS, GX_DIRECT);
}
void GXSetVtxDesc(GXAttr attr, GXAttrType type) { rec::Vcd(attr, type); }
void GXSetSourceVtxDesc(GXAttr, GXAttrType) {} // Aurora-internal hint (direct vs indexed source); not needed here
void GXGetVtxDesc(GXAttr attr, GXAttrType* type) {
    if (!type) return;
    *type = (static_cast<unsigned>(attr) <= GX_VA_TEX7) ? static_cast<GXAttrType>(rec::gs.vcd[attr]) : GX_NONE;
}
void GXSetVtxAttrFmt(GXVtxFmt fmt, GXAttr attr, GXCompCnt cnt, GXCompType type, u8 frac) { rec::Vat(fmt, attr, cnt, type, frac); }
void GXSetArray(GXAttr attr, const void* data, u32 size, u8 stride, bool le) {
    int a = attr == GX_VA_NBT ? GX_VA_NRM : attr;
    if (a < GX_VA_POS || a > GX_LIGHT_ARRAY) return;
    rec::Array(a, data, size, stride, le);
}
void GXSetNumTexGens(u8 n) {
    rec::BPField(0x00, 0, 4, n);
    rec::XFReg(0x103F, n);
}
void GXSetTexCoordGen2(GXTexCoordID dst, GXTexGenType func, GXTexGenSrc src, u32 mtx, GXBool normalize, u32 postMtx) {
    if (static_cast<unsigned>(dst) > 7) return;
    uint32_t row = 5, form = 0;
    switch (src) {
    case GX_TG_POS: row = 0; form = 1; break;
    case GX_TG_NRM: row = 1; form = 1; break;
    case GX_TG_BINRM: row = 3; form = 1; break;
    case GX_TG_TANGENT: row = 4; form = 1; break;
    case GX_TG_COLOR0: case GX_TG_COLOR1: row = 2; break;
    default:
        if (src >= GX_TG_TEX0 && src <= GX_TG_TEX7) row = 5 + (src - GX_TG_TEX0);
        break;
    }
    uint32_t reg = 0;
    switch (func) {
    case GX_TG_MTX2x4: reg = (form << 2) | (row << 7); break;
    case GX_TG_MTX3x4: reg = (1u << 1) | (form << 2) | (row << 7); break;
    case GX_TG_SRTG: reg = (form << 2) | ((src == GX_TG_COLOR0 ? 2u : 3u) << 4) | (2u << 7); break;
    default:
        if (func >= GX_TG_BUMP0 && func <= GX_TG_BUMP7) {
            reg = (form << 2) | (1u << 4) | (row << 7) | ((static_cast<uint32_t>(src) - 12u) << 12) |
                  ((static_cast<uint32_t>(func) - GX_TG_BUMP0) << 15);
        }
        break;
    }
    rec::XFReg(0x1040 + dst, reg);
    rec::XFReg(0x1050 + dst, ((postMtx - 64u) & 0x3F) | (normalize ? 0x100u : 0u));
    if (dst < 4) SetMatIdx(false, 6 + 6 * dst, mtx);
    else SetMatIdx(true, 6 * (dst - 4), mtx);
}
void GXSetLineWidth(u8 width, GXTexOffset offs) { rec::BPField(0x22, 0, 8, width); rec::BPField(0x22, 16, 3, offs); }
void GXSetPointSize(u8 size, GXTexOffset offs) { rec::BPField(0x22, 8, 8, size); rec::BPField(0x22, 19, 3, offs); }
void GXEnableTexOffsets(GXTexCoordID, GXBool, GXBool) {}
void GXSetCullMode(GXCullMode mode) {
    // The hardware field swaps front and back relative to the API enum.
    static const uint32_t hw[4] = {0, 2, 1, 3};
    rec::BPField(0x00, 14, 2, hw[mode & 3]);
}
void GXSetCoPlanar(GXBool enable) { rec::BPField(0x00, 19, 1, enable ? 1 : 0); }

void GXBegin(GXPrimitive prim, GXVtxFmt fmt, u16 nverts) { rec::ImmBegin(static_cast<uint8_t>(prim), fmt, nverts); }
void GXEnd(void) { rec::ImmEnd(); }
void GXPosition3f32(f32 x, f32 y, f32 z) { rec::ImmPos(x, y, z, false); }
void GXPosition3u16(u16 x, u16 y, u16 z) { rec::ImmPos(x, y, z, true); }
void GXPosition3s16(s16 x, s16 y, s16 z) { rec::ImmPos(x, y, z, true); }
void GXPosition3u8(u8 x, u8 y, u8 z) { rec::ImmPos(x, y, z, true); }
void GXPosition3s8(s8 x, s8 y, s8 z) { rec::ImmPos(x, y, z, true); }
void GXPosition2f32(f32 x, f32 y) { rec::ImmPos(x, y, 0, false); }
void GXPosition2u16(u16 x, u16 y) { rec::ImmPos(x, y, 0, true); }
void GXPosition2s16(s16 x, s16 y) { rec::ImmPos(x, y, 0, true); }
void GXPosition2u8(u8 x, u8 y) { rec::ImmPos(x, y, 0, true); }
void GXPosition2s8(s8 x, s8 y) { rec::ImmPos(x, y, 0, true); }
void GXNormal3f32(f32 x, f32 y, f32 z) { rec::ImmNrm(x, y, z, -1); }
void GXNormal3u16(u16 x, u16 y, u16 z) { rec::ImmNrm(x, y, z, 15); }
void GXNormal3s16(s16 x, s16 y, s16 z) { rec::ImmNrm(x, y, z, 14); }
void GXNormal3u8(u8 x, u8 y, u8 z) { rec::ImmNrm(x, y, z, 7); }
void GXNormal3s8(s8 x, s8 y, s8 z) { rec::ImmNrm(x, y, z, 6); }
void GXColor4u8(u8 r, u8 gr, u8 b, u8 a) { rec::ImmColor(r, gr, b, a); }
void GXColor3u8(u8 r, u8 gr, u8 b) { rec::ImmColor(r, gr, b, 255); }
void GXTexCoord2f32(f32 s, f32 t) { rec::ImmTex(s, t, false); }
void GXTexCoord2u16(u16 s, u16 t) { rec::ImmTex(s, t, true); }
void GXTexCoord2s16(s16 s, s16 t) { rec::ImmTex(s, t, true); }
void GXTexCoord2u8(u8 s, u8 t) { rec::ImmTex(s, t, true); }
void GXTexCoord2s8(s8 s, s8 t) { rec::ImmTex(s, t, true); }
void GXTexCoord1f32(f32 s) { rec::ImmTex(s, 0, false); }
void GXTexCoord1u16(u16 s) { rec::ImmTex(s, 0, true); }
void GXTexCoord1s16(s16 s) { rec::ImmTex(s, 0, true); }
void GXTexCoord1u8(u8 s) { rec::ImmTex(s, 0, true); }
void GXTexCoord1s8(s8 s) { rec::ImmTex(s, 0, true); }
void GXMatrixIndex1u8(GXAttr attr, u8 index) { rec::ImmMtxIdx(attr, index); }

void GXCallDisplayList(const void* list, u32 nbytes) {
    if (list && nbytes) rec::Stream(static_cast<const uint8_t*>(list), nbytes);
}
void GXApplyBPReg(u8 reg, u32 value) { rec::BP(reg, value); }

// ------------------------------------------------------------ transform
void GXSetProjection(const void* mtx_, GXProjectionType type) {
    const float (*m)[4] = static_cast<const float (*)[4]>(mtx_);
    uint32_t p[7];
    p[0] = f2u(m[0][0]); p[2] = f2u(m[1][1]); p[4] = f2u(m[2][2]); p[5] = f2u(m[2][3]);
    if (type == GX_ORTHOGRAPHIC) { p[1] = f2u(m[0][3]); p[3] = f2u(m[1][3]); }
    else { p[1] = f2u(m[0][2]); p[3] = f2u(m[1][2]); }
    p[6] = type == GX_ORTHOGRAPHIC ? 1 : 0;
    rec::XF(0x1020, 7, p);
}
void GXLoadPosMtxImm(const void* mtx, u32 id) { rec::XFMtx(id * 4, mtx, 12); }
void GXLoadNrmMtxImm(const void* mtx_, u32 id) { rec::XFNrm(0x400 + id * 3, mtx_); }
void GXLoadTexMtxImm(const void* mtx, u32 id, GXTexMtxType type) {
    const uint32_t n = type == GX_MTX2x4 ? 8 : 12;
    if (id >= GX_PTTEXMTX0) rec::XFMtx(0x500 + (id - GX_PTTEXMTX0) * 4, mtx, n);
    else rec::XFMtx(id * 4, mtx, n);
}
void GXSetCurrentMtx(u32 id) { SetMatIdx(false, 0, id); }
void GXSetViewport(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz) {
    uint32_t w[6];
    w[0] = f2u(wd * 0.5f);
    w[1] = f2u(-ht * 0.5f);
    w[2] = f2u((farz - nearz) * 16777215.0f);
    w[3] = f2u(left + wd * 0.5f + 342.0f);
    w[4] = f2u(top + ht * 0.5f + 342.0f);
    w[5] = f2u(farz * 16777215.0f);
    rec::XF(0x101A, 6, w);
}
void GXSetScissor(u32 left, u32 top, u32 wd, u32 ht) {
    const uint32_t x0 = left + 342, y0 = top + 342, x1 = x0 + wd - 1, y1 = y0 + ht - 1;
    rec::BP(0x20, (y0 & 0x7FF) | ((x0 & 0x7FF) << 12));
    rec::BP(0x21, (y1 & 0x7FF) | ((x1 & 0x7FF) << 12));
}
void GXSetScissorBoxOffset(s32 x, s32 y) {
    const uint32_t hx = static_cast<uint32_t>(x + 342) >> 1, hy = static_cast<uint32_t>(y + 342) >> 1;
    rec::BP(0x59, (hx & 0x3FF) | ((hy & 0x3FF) << 10));
}
void GXSetZScaleOffset(f32, f32) {}
void GXSetClipMode(GXClipMode mode) { rec::XFReg(0x1005, mode); }

// ------------------------------------------------------------- lighting
void GXSetNumChans(u8 n) {
    rec::BPField(0x00, 4, 3, n);
    rec::XFReg(0x1009, n);
}
static void SetChanColor(uint32_t baseReg, GXChannelID chan, GXColor c) {
    uint32_t reg, v;
    switch (chan) {
    case GX_COLOR0: reg = baseReg; v = (XfReg(reg) & 0xFF) | (PackColor(c) & 0xFFFFFF00u); break;
    case GX_COLOR1: reg = baseReg + 1; v = (XfReg(reg) & 0xFF) | (PackColor(c) & 0xFFFFFF00u); break;
    case GX_ALPHA0: reg = baseReg; v = (XfReg(reg) & 0xFFFFFF00u) | c.a; break;
    case GX_ALPHA1: reg = baseReg + 1; v = (XfReg(reg) & 0xFFFFFF00u) | c.a; break;
    case GX_COLOR0A0: reg = baseReg; v = PackColor(c); break;
    case GX_COLOR1A1: reg = baseReg + 1; v = PackColor(c); break;
    default: return;
    }
    rec::XFReg(reg, v);
}
void GXSetChanAmbColor(GXChannelID chan, GXColor c) { SetChanColor(0x100A, chan, c); }
void GXSetChanMatColor(GXChannelID chan, GXColor c) { SetChanColor(0x100C, chan, c); }
void GXSetChanCtrl(GXChannelID chan, GXBool enable, GXColorSrc ambSrc, GXColorSrc matSrc, u32 lightMask, GXDiffuseFn diffFn,
                   GXAttnFn attnFn) {
    uint32_t reg = 0;
    reg |= (matSrc & 1);
    reg |= (enable ? 1u : 0u) << 1;
    reg |= (lightMask & 0xF) << 2;
    reg |= (ambSrc & 1) << 6;
    reg |= ((attnFn == GX_AF_SPEC) ? 0u : (static_cast<uint32_t>(diffFn) & 3)) << 7;
    reg |= (attnFn != GX_AF_NONE ? 1u : 0u) << 9;
    reg |= (attnFn != GX_AF_SPEC ? 1u : 0u) << 10;
    reg |= ((lightMask >> 4) & 0xF) << 11;
    switch (chan) {
    case GX_COLOR0: rec::XFReg(0x100E, reg); break;
    case GX_COLOR1: rec::XFReg(0x100F, reg); break;
    case GX_ALPHA0: rec::XFReg(0x1010, reg); break;
    case GX_ALPHA1: rec::XFReg(0x1011, reg); break;
    case GX_COLOR0A0: rec::XFReg(0x100E, reg); rec::XFReg(0x1010, reg); break;
    case GX_COLOR1A1: rec::XFReg(0x100F, reg); rec::XFReg(0x1011, reg); break;
    default: break;
    }
}
void GXInitLightColor(GXLightObj* o, GXColor c) { L(o)->color = PackColor(c); }
void GXInitLightAttn(GXLightObj* o, f32 a0, f32 a1, f32 a2, f32 k0, f32 k1, f32 k2) {
    LightObj* l = L(o);
    l->a[0] = a0; l->a[1] = a1; l->a[2] = a2;
    l->k[0] = k0; l->k[1] = k1; l->k[2] = k2;
}
void GXInitLightPos(GXLightObj* o, f32 x, f32 y, f32 z) { LightObj* l = L(o); l->pos[0] = x; l->pos[1] = y; l->pos[2] = z; }
void GXInitLightDir(GXLightObj* o, f32 nx, f32 ny, f32 nz) {
    // The hardware light stores the negated direction.
    LightObj* l = L(o);
    l->dir[0] = -nx; l->dir[1] = -ny; l->dir[2] = -nz;
}
void GXLoadLightObjImm(GXLightObj* o, GXLightID light) {
    int idx = 0;
    for (uint32_t m = static_cast<uint32_t>(light); idx < 7 && !(m & 1); m >>= 1) ++idx;
    uint32_t w[16];
    std::memcpy(w, o, 64);
    rec::XF(0x600 + idx * 16, 16, w);
}

// ------------------------------------------------------------------ TEV
void GXSetNumTevStages(u8 n) { rec::BPField(0x00, 10, 4, static_cast<uint32_t>(n ? n - 1 : 0)); }
void GXSetTevColorIn(GXTevStageID s, GXTevColorArg a, GXTevColorArg b, GXTevColorArg c, GXTevColorArg d) {
    const uint8_t reg = static_cast<uint8_t>(0xC0 + 2 * (s & 15));
    rec::BPField(reg, 0, 16, (static_cast<uint32_t>(a) << 12) | (static_cast<uint32_t>(b) << 8) | (static_cast<uint32_t>(c) << 4) | d);
}
void GXSetTevAlphaIn(GXTevStageID s, GXTevAlphaArg a, GXTevAlphaArg b, GXTevAlphaArg c, GXTevAlphaArg d) {
    const uint8_t reg = static_cast<uint8_t>(0xC1 + 2 * (s & 15));
    rec::BPField(reg, 4, 12, (static_cast<uint32_t>(a) << 9) | (static_cast<uint32_t>(b) << 6) | (static_cast<uint32_t>(c) << 3) | d);
}
static void SetTevOp(uint8_t reg, GXTevOp op, GXTevBias bias, GXTevScale scale, bool clamp, GXTevRegID out) {
    uint32_t v = 0;
    if (op <= 1) v = (static_cast<uint32_t>(bias) & 3) | ((op & 1u) << 2) | ((static_cast<uint32_t>(scale) & 3) << 4);
    else v = 3u | ((op & 1u) << 2) | (((static_cast<uint32_t>(op) >> 1) & 3) << 4); // compare modes
    v |= (clamp ? 1u : 0u) << 3;
    v |= (static_cast<uint32_t>(out) & 3) << 6;
    rec::BPField(reg, 16, 8, v);
}
void GXSetTevColorOp(GXTevStageID s, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp, GXTevRegID out) {
    SetTevOp(static_cast<uint8_t>(0xC0 + 2 * (s & 15)), op, bias, scale, clamp, out);
}
void GXSetTevAlphaOp(GXTevStageID s, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp, GXTevRegID out) {
    SetTevOp(static_cast<uint8_t>(0xC1 + 2 * (s & 15)), op, bias, scale, clamp, out);
}
void GXSetTevOp(GXTevStageID id, GXTevMode mode) {
    const GXTevColorArg inC = id == GX_TEVSTAGE0 ? GX_CC_RASC : GX_CC_CPREV;
    const GXTevAlphaArg inA = id == GX_TEVSTAGE0 ? GX_CA_RASA : GX_CA_APREV;
    switch (mode) {
    case GX_MODULATE:
        GXSetTevColorIn(id, GX_CC_ZERO, GX_CC_TEXC, inC, GX_CC_ZERO);
        GXSetTevAlphaIn(id, GX_CA_ZERO, GX_CA_TEXA, inA, GX_CA_ZERO);
        break;
    case GX_DECAL:
        GXSetTevColorIn(id, inC, GX_CC_TEXC, GX_CC_TEXA, GX_CC_ZERO);
        GXSetTevAlphaIn(id, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, inA);
        break;
    case GX_BLEND:
        GXSetTevColorIn(id, inC, GX_CC_ONE, GX_CC_TEXC, GX_CC_ZERO);
        GXSetTevAlphaIn(id, GX_CA_ZERO, GX_CA_TEXA, inA, GX_CA_ZERO);
        break;
    case GX_REPLACE:
        GXSetTevColorIn(id, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
        GXSetTevAlphaIn(id, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
        break;
    default: // GX_PASSCLR
        GXSetTevColorIn(id, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, inC);
        GXSetTevAlphaIn(id, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, inA);
        break;
    }
    GXSetTevColorOp(id, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaOp(id, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
}
void GXSetTevOrder(GXTevStageID s, GXTexCoordID coord, GXTexMapID map, GXChannelID color) {
    const int stage = s & 15;
    const uint8_t reg = static_cast<uint8_t>(0x28 + (stage >> 1));
    const int sh = (stage & 1) * 12;
    const uint32_t m = static_cast<uint32_t>(map), c = static_cast<uint32_t>(coord);
    const bool texOn = m != 0xFF && !(m & 0x100);
    uint32_t ras = 7;
    switch (color) {
    case GX_COLOR0: case GX_ALPHA0: case GX_COLOR0A0: ras = 0; break;
    case GX_COLOR1: case GX_ALPHA1: case GX_COLOR1A1: ras = 1; break;
    case GX_ALPHA_BUMP: ras = 5; break;
    case GX_ALPHA_BUMPN: ras = 6; break;
    default: ras = 7; break;
    }
    const uint32_t v = (m == 0xFF ? 0u : (m & 7)) | ((c >= 8 ? 0u : c) << 3) | ((texOn ? 1u : 0u) << 6) | (ras << 7);
    rec::BPField(reg, sh, 10, v);
}
void GXSetTevColor(GXTevRegID id, GXColor c) {
    rec::BP(static_cast<uint8_t>(0xE0 + 2 * (id & 3)), c.r | (static_cast<uint32_t>(c.a) << 12));
    rec::BP(static_cast<uint8_t>(0xE1 + 2 * (id & 3)), c.b | (static_cast<uint32_t>(c.g) << 12));
}
void GXSetTevColorS10(GXTevRegID id, GXColorS10 c) {
    rec::BP(static_cast<uint8_t>(0xE0 + 2 * (id & 3)), (c.r & 0x7FF) | (static_cast<uint32_t>(c.a & 0x7FF) << 12));
    rec::BP(static_cast<uint8_t>(0xE1 + 2 * (id & 3)), (c.b & 0x7FF) | (static_cast<uint32_t>(c.g & 0x7FF) << 12));
}
void GXSetTevKColor(GXTevKColorID id, GXColor c) {
    rec::BP(static_cast<uint8_t>(0xE0 + 2 * (id & 3)), c.r | (static_cast<uint32_t>(c.a) << 12) | 0x800000u);
    rec::BP(static_cast<uint8_t>(0xE1 + 2 * (id & 3)), c.b | (static_cast<uint32_t>(c.g) << 12) | 0x800000u);
}
void GXSetTevKColorSel(GXTevStageID s, GXTevKColorSel sel) {
    rec::BPField(static_cast<uint8_t>(0xF6 + ((s & 15) >> 1)), (s & 1) ? 14 : 4, 5, sel);
}
void GXSetTevKAlphaSel(GXTevStageID s, GXTevKAlphaSel sel) {
    rec::BPField(static_cast<uint8_t>(0xF6 + ((s & 15) >> 1)), (s & 1) ? 19 : 9, 5, sel);
}
void GXSetTevSwapMode(GXTevStageID s, GXTevSwapSel ras, GXTevSwapSel tex) {
    rec::BPField(static_cast<uint8_t>(0xC1 + 2 * (s & 15)), 0, 4, (static_cast<uint32_t>(ras) & 3) | ((static_cast<uint32_t>(tex) & 3) << 2));
}
void GXSetTevSwapModeTable(GXTevSwapSel table, GXTevColorChan r, GXTevColorChan gr, GXTevColorChan b, GXTevColorChan a) {
    const uint8_t reg = static_cast<uint8_t>(0xF6 + 2 * (table & 3));
    rec::BPField(reg, 0, 4, (static_cast<uint32_t>(r) & 3) | ((static_cast<uint32_t>(gr) & 3) << 2));
    rec::BPField(static_cast<uint8_t>(reg + 1), 0, 4, (static_cast<uint32_t>(b) & 3) | ((static_cast<uint32_t>(a) & 3) << 2));
}
void GXSetAlphaCompare(GXCompare c0, u8 ref0, GXAlphaOp op, GXCompare c1, u8 ref1) {
    rec::BP(0xF3, ref0 | (static_cast<uint32_t>(ref1) << 8) | ((static_cast<uint32_t>(c0) & 7) << 16) |
                      ((static_cast<uint32_t>(c1) & 7) << 19) | ((static_cast<uint32_t>(op) & 3) << 22));
}
void GXSetZTexture(GXZTexOp op, GXTexFmt fmt, u32 bias) {
    uint32_t type = 0;
    switch (fmt) { case GX_TF_Z8: type = 0; break; case GX_TF_Z16: type = 1; break; default: type = 2; break; }
    rec::BP(0xF4, bias & 0xFFFFFF);
    rec::BP(0xF5, type | ((static_cast<uint32_t>(op) & 3) << 2));
}

// ------------------------------------------------------------- indirect
void GXSetNumIndStages(u8 n) { rec::BPField(0x00, 16, 3, n); }
void GXSetIndTexOrder(GXIndTexStageID stage, GXTexCoordID coord, GXTexMapID map) {
    rec::BPField(0x27, (stage & 3) * 6, 6, (static_cast<uint32_t>(map) & 7) | ((static_cast<uint32_t>(coord) & 7) << 3));
}
void GXSetIndTexCoordScale(GXIndTexStageID stage, GXIndTexScale ss, GXIndTexScale ts) {
    rec::BPField(static_cast<uint8_t>(0x25 + ((stage & 3) >> 1)), (stage & 1) * 8, 8,
               (static_cast<uint32_t>(ss) & 0xF) | ((static_cast<uint32_t>(ts) & 0xF) << 4));
}
void GXSetIndTexMtx(GXIndTexMtxID id, const void* offset, s8 scaleExp) {
    const float (*m)[3] = static_cast<const float (*)[3]>(offset);
    int idx;
    switch (id) {
    case GX_ITM_0: case GX_ITM_S0: case GX_ITM_T0: idx = 0; break;
    case GX_ITM_1: case GX_ITM_S1: case GX_ITM_T1: idx = 1; break;
    case GX_ITM_2: case GX_ITM_S2: case GX_ITM_T2: idx = 2; break;
    default: return;
    }
    const uint32_t scale = static_cast<uint32_t>(scaleExp + 17);
    for (int c = 0; c < 3; ++c) {
        const uint32_t a = static_cast<uint32_t>(static_cast<int>(m[0][c] * 1024.0f)) & 0x7FF;
        const uint32_t b = static_cast<uint32_t>(static_cast<int>(m[1][c] * 1024.0f)) & 0x7FF;
        rec::BP(static_cast<uint8_t>(0x06 + idx * 3 + c), a | (b << 11) | (((scale >> (2 * c)) & 3) << 22));
    }
}
void GXSetTevIndirect(GXTevStageID s, GXIndTexStageID indStage, GXIndTexFormat fmt, GXIndTexBiasSel bias, GXIndTexMtxID mtx,
                      GXIndTexWrap wrapS, GXIndTexWrap wrapT, GXBool addPrev, GXBool utcLod, GXIndTexAlphaSel alphaSel) {
    rec::BP(static_cast<uint8_t>(0x10 + (s & 15)),
            (static_cast<uint32_t>(indStage) & 3) | ((static_cast<uint32_t>(fmt) & 3) << 2) | ((static_cast<uint32_t>(bias) & 7) << 4) |
                ((static_cast<uint32_t>(alphaSel) & 3) << 7) | ((static_cast<uint32_t>(mtx) & 15) << 9) |
                ((static_cast<uint32_t>(wrapS) & 7) << 13) | ((static_cast<uint32_t>(wrapT) & 7) << 16) | ((utcLod ? 1u : 0u) << 19) |
                ((addPrev ? 1u : 0u) << 20));
}
void GXSetTevDirect(GXTevStageID s) {
    GXSetTevIndirect(s, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_NONE, GX_ITM_OFF, GX_ITW_OFF, GX_ITW_OFF, GX_FALSE, GX_FALSE, GX_ITBA_OFF);
}
void GXSetTevIndWarp(GXTevStageID s, GXIndTexStageID indStage, GXBool signedOffsets, GXBool replace, GXIndTexMtxID mtx) {
    const GXIndTexWrap wrap = replace ? GX_ITW_0 : GX_ITW_OFF;
    GXSetTevIndirect(s, indStage, GX_ITF_8, signedOffsets ? GX_ITB_STU : GX_ITB_NONE, mtx, wrap, wrap, GX_FALSE, GX_FALSE, GX_ITBA_OFF);
}

// ---------------------------------------------------------------- pixel
void GXSetBlendMode(GXBlendMode type, GXBlendFactor src, GXBlendFactor dst, GXLogicOp op) {
    uint32_t v = rec::gs.bp[0x41];
    v &= ~(0x1u | 0x2u | 0x800u | 0xF000u | 0x7E0u | 0x700u);
    if (type == GX_BM_BLEND || type == GX_BM_SUBTRACT) v |= 1u;
    if (type == GX_BM_LOGIC) v |= 2u;
    if (type == GX_BM_SUBTRACT) v |= 0x800u;
    v |= (static_cast<uint32_t>(dst) & 7) << 5;
    v |= (static_cast<uint32_t>(src) & 7) << 8;
    v |= (static_cast<uint32_t>(op) & 15) << 12;
    rec::BP(0x41, v);
}
void GXSetColorUpdate(GXBool e) { rec::BPField(0x41, 3, 1, e ? 1 : 0); }
void GXSetAlphaUpdate(GXBool e) { rec::BPField(0x41, 4, 1, e ? 1 : 0); }
void GXSetDither(GXBool e) { rec::BPField(0x41, 2, 1, e ? 1 : 0); }
void GXSetZMode(GXBool enable, GXCompare func, GXBool update) {
    rec::BP(0x40, (enable ? 1u : 0u) | ((static_cast<uint32_t>(func) & 7) << 1) | ((update ? 1u : 0u) << 4));
}
void GXSetZCompLoc(GXBool beforeTex) { rec::BPField(0x43, 6, 1, beforeTex ? 1 : 0); }
void GXSetPixelFmt(GXPixelFmt fmt, GXZFmt16 zfmt) {
    static const uint32_t hw[8] = {0, 1, 2, 3, 4, 4, 4, 5};
    rec::BPField(0x43, 0, 3, hw[fmt & 7]);
    rec::BPField(0x43, 3, 3, static_cast<uint32_t>(zfmt) & 7);
}
void GXSetDstAlpha(GXBool enable, u8 alpha) { rec::BP(0x42, alpha | ((enable ? 1u : 0u) << 8)); }
void GXSetFog(GXFogType type, f32 startz, f32 endz, f32 nearz, f32 farz, GXColor color) {
    const uint32_t fsel = static_cast<uint32_t>(type) & 7;
    const bool ortho = (static_cast<uint32_t>(type) & 8) != 0;
    float a, c;
    uint32_t bMant = 0, bShift = 0;
    if (ortho) {
        if (farz == nearz || endz == startz) { a = 0.0f; c = 0.0f; }
        else {
            const float A = 1.0f / (endz - startz);
            a = A * (farz - nearz);
            c = A * (startz - nearz);
        }
    } else {
        float A, B, C;
        if (farz == nearz || endz == startz) { A = 0.0f; B = 0.5f; C = 0.0f; }
        else {
            A = (farz * nearz) / ((farz - nearz) * (endz - startz));
            B = farz / (farz - nearz);
            C = startz / (endz - startz);
        }
        float bm = B;
        int expn = 0;
        while (bm > 1.0f) { bm *= 0.5f; ++expn; }
        while (bm > 0.0f && bm < 0.5f) { bm *= 2.0f; --expn; }
        a = A / static_cast<float>(1 << (expn + 1));
        bMant = static_cast<uint32_t>(bm * 8388638.0f);
        bShift = static_cast<uint32_t>(expn + 1);
        c = C;
    }
    rec::BP(0xEE, f2u(a) >> 12);
    rec::BP(0xEF, bMant & 0xFFFFFF);
    rec::BP(0xF0, bShift & 0x1F);
    rec::BP(0xF1, (f2u(c) >> 12) | ((ortho ? 1u : 0u) << 20) | (fsel << 21));
    rec::BP(0xF2, color.b | (static_cast<uint32_t>(color.g) << 8) | (static_cast<uint32_t>(color.r) << 16));
}

// -------------------------------------------------------------- textures
void GXInitTexObj(GXTexObj* obj, const void* data, u16 w, u16 h, GXTexFmt fmt, GXTexWrapMode ws, GXTexWrapMode wt, GXBool mip) {
    InitTex(T(obj), data, w, h, fmt, ws, wt, mip);
}
void GXInitTexObjCI(GXTexObj* obj, const void* data, u16 w, u16 h, GXCITexFmt fmt, GXTexWrapMode ws, GXTexWrapMode wt, GXBool mip,
                    u32 tlut) {
    InitTex(T(obj), data, w, h, fmt, ws, wt, mip);
    T(obj)->tlut = tlut;
}
void GXInitTexObjData(GXTexObj* obj, const void* data) {
    T(obj)->data = data;
    ++T(obj)->dataVersion;
}
void GXInitTexObjLOD(GXTexObj* obj, GXTexFilter minFilt, GXTexFilter magFilt, f32 minLod, f32 maxLod, f32 lodBias, GXBool, GXBool,
                     GXAnisotropy) {
    TexObj* o = T(obj);
    o->minFilter = static_cast<uint8_t>(minFilt);
    o->magFilter = static_cast<uint8_t>(magFilt);
    o->minLod = minLod;
    o->maxLod = maxLod;
    o->lodBias = lodBias;
}
void GXInitTexObjWrapMode(GXTexObj* obj, GXTexWrapMode s, GXTexWrapMode t) {
    T(obj)->wrapS = static_cast<uint8_t>(s);
    T(obj)->wrapT = static_cast<uint8_t>(t);
}
void GXInitTexObjTlut(GXTexObj* obj, u32 tlut) { T(obj)->tlut = tlut; }
void GXInitTexObjUserData(GXTexObj* obj, void* userData) { T(obj)->userData = userData; }
void GXLoadTexObj(GXTexObj* obj, GXTexMapID id) {
    if (static_cast<unsigned>(id) > 7 || !obj) return;
    rec::Tex(id, *T(obj), ++g_texLoadSerial);
}
void GXDestroyTexObj(GXTexObj*) {}
void GXInitTlutObj(GXTlutObj* obj, const void* data, GXTlutFmt fmt, u16 entries) {
    TlutObj* t = TL(obj);
    t->data = data;
    t->format = fmt;
    t->entries = entries;
    t->version = ++g_tlutVersion;
}
void GXLoadTlut(const GXTlutObj* obj, u32 idx) {
    if (idx >= 20 || !obj) return;
    TlutObj t = *reinterpret_cast<const TlutObj*>(obj);
    // The palette bytes may have changed under the same object; re-hash on next use.
    t.version = ++g_tlutVersion;
    rec::Tlut(static_cast<int>(idx), t);
}
void GXDestroyTlutObj(GXTlutObj*) {}
void GXInvalidateTexAll(void) { rec::Emit0(rec::R_INVAL_TEX); }
void GXSetTexCoordScaleManually(GXTexCoordID coord, GXBool enable, u16 ss, u16 ts) {
    // Texture coordinates are kept normalised here, so the hardware's texel
    // scale registers (and manual overrides of them) have no effect.
    (void)coord; (void)enable; (void)ss; (void)ts;
}
u32 GXGetTexBufferSize(u16 width, u16 height, u32 fmt, GXBool mips, u8 maxLod) {
    s32 shiftX = 0, shiftY = 0;
    switch (fmt) {
    case GX_TF_I4: case GX_TF_C4: case GX_TF_CMPR: case GX_CTF_R4: case GX_CTF_Z4:
        shiftX = 3; shiftY = 3; break;
    case GX_TF_I8: case GX_TF_IA4: case GX_TF_C8: case GX_TF_Z8: case GX_CTF_RA4: case GX_CTF_A8:
    case GX_CTF_R8: case GX_CTF_G8: case GX_CTF_B8: case GX_CTF_Z8M: case GX_CTF_Z8L:
        shiftX = 3; shiftY = 2; break;
    case GX_TF_IA8: case GX_TF_RGB565: case GX_TF_RGB5A3: case GX_TF_RGBA8: case GX_TF_C14X2:
    case GX_TF_Z16: case GX_TF_Z24X8: case GX_CTF_RA8: case GX_CTF_RG8: case GX_CTF_GB8: case GX_CTF_Z16L:
        shiftX = 2; shiftY = 2; break;
    default: break;
    }
    const u32 bitSize = (fmt == GX_TF_RGBA8 || fmt == GX_TF_Z24X8) ? 64u : 32u;
    u32 bufLen = 0;
    if (mips) {
        while (maxLod != 0) {
            const u32 tilesX = (width + (1u << shiftX) - 1) >> shiftX;
            const u32 tilesY = (height + (1u << shiftY) - 1) >> shiftY;
            bufLen += bitSize * tilesX * tilesY;
            if (width == 1 && height == 1) return bufLen;
            width = width < 2 ? 1 : width / 2;
            height = height < 2 ? 1 : height / 2;
            --maxLod;
        }
    } else {
        const u32 tilesX = (width + (1u << shiftX) - 1) >> shiftX;
        const u32 tilesY = (height + (1u << shiftY) - 1) >> shiftY;
        bufLen = bitSize * tilesX * tilesY;
    }
    return bufLen;
}

// ---------------------------------------------------------- frame buffer
void GXSetDispCopySrc(u16 left, u16 top, u16 wd, u16 ht) { rec::gs.dispCopySrc = EfbRect{(int16_t)left, (int16_t)top, (int16_t)wd, (int16_t)ht}; }
void GXSetDispCopyDst(u16 wd, u16 ht) { rec::gs.dispCopyDstW = wd; rec::gs.dispCopyDstH = ht; }
void GXSetTexCopySrc(u16 left, u16 top, u16 wd, u16 ht) { rec::gs.texCopySrc = EfbRect{(int16_t)left, (int16_t)top, (int16_t)wd, (int16_t)ht}; }
void GXSetTexCopyDst(u16 wd, u16 ht, GXTexFmt fmt, GXBool mipmap) {
    rec::gs.texCopyDstW = wd; rec::gs.texCopyDstH = ht;
    rec::gs.texCopyFmt = static_cast<uint32_t>(fmt);
    rec::gs.texCopyHalf = mipmap != GX_FALSE;
}
void GXSetCopyFilter(GXBool, u8[12][2], GXBool, u8[7]) {}
void GXSetDispCopyGamma(GXGamma) {}
void GXSetDispCopyFrame2Field(u32) {}
void GXSetCopyClamp(GXFBClamp) {}
void GXClearBoundingBox(void) {}
void GXCopyDisp(void*, GXBool clear) { rec::CopyDisp(clear != GX_FALSE); }
void GXCopyTex(void* dest, GXBool clear) { rec::CopyTex(dest, clear != GX_FALSE); }
void GXDestroyCopyTex(void* dest) { rec::DestroyCopy(dest); }

// -------------------------------------------------------------------- VI
void VIConfigure(const GXRenderModeObj* rm) {
    if (!rm || rm->fbWidth == 0 || rm->efbHeight == 0) return;
    if (rec::gs.efbW != rm->fbWidth || rec::gs.efbH != rm->efbHeight) {
        rec::gs.efbW = rm->fbWidth;
        rec::gs.efbH = rm->efbHeight;
        VGX_LOG("VIConfigure: EFB %ux%u, XFB height %u, VI %ux%u\n", rm->fbWidth, rm->efbHeight, rm->xfbHeight, rm->viWidth, rm->viHeight);
        rec::SetTarget(rec::gs.efbW, rec::gs.efbH);
    }
}
void VISetFrameBufferScale(float) {}
void VILockAspectRatio(int, int) {}
void VIUnlockAspectRatio(void) {}

} // extern "C"

namespace aurora::gx::fifo {
// One complete draw packet whose vertex components are all direct, big-endian.
bool submit_raw_draw(GXPrimitive prim, GXVtxFmt fmt, const uint8_t* vertices, uint16_t vtxCount, uint32_t vertexBytes) {
    rec::DrawRaw(static_cast<uint8_t>(prim), fmt, vertices, vtxCount, vertexBytes);
    return true;
}
}
