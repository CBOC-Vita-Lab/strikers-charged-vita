// The part of the native C interface (vgx_native.h) that is independent of the host platform: state access,
// vertex arrays, raw primitives, textures. Everything here is a thin layer over the recorder (vgx_rec.h) and the
// direct-submission interface (vgx_hle.h) that the runtime's GX HLE uses in the recompiled build; what reaches
// the record stream is exactly what the HLE sends for the same game call.
#include "vgx_native.h"

#include "vgx_guestmem.h"
#include "vgx_hle.h"
#include "vgx_rec.h"

using namespace vgx;

namespace {
constexpr uint32_t kTexMagic = 0x76677854u; // "vgxT": TexObj::magic, which the player tests (vgx_play.cpp)
uint32_t g_texLoadSerial = 0;
uint32_t g_tlutVersion = 0;
uint32_t g_droppedPipeWrites = 0;
} // namespace

// Backward compatibility with glue versions that do not define the projection / viewport entry points themselves: the
// SDK names resolve to vgx's own (renamed) implementations unless the glue's strong definitions are linked (np/platform/gx
// gx_transform.cpp, which widens full-screen perspective projections to the output surface).
extern "C" {
void vgxapi_GXSetViewport(float left, float top, float wd, float ht, float nearz, float farz);
void vgxapi_GXSetProjection(const float m[4][4], unsigned int type);
__attribute__((weak)) void GXSetViewport(float left, float top, float wd, float ht, float nearz, float farz) { vgxapi_GXSetViewport(left, top, wd, ht, nearz, farz); }
__attribute__((weak)) void GXSetProjection(const float m[4][4], unsigned int type) { vgxapi_GXSetProjection(m, type); }
}

extern "C" {

void vgxn_register_ram(const void* base, uint32_t size) { hle::RegisterGuestRam(base, size); }
#if defined(VGX_PRODUCER_CHECK)
void vgxn_set_producer_hook(void (*fn)(void)) { rec::g_producerHook = fn; }
#endif
void vgxn_notify_write(const void* p, uint32_t n) { hle::NotifyGuestWrite(p, n); }

void vgxn_tex_copy_dst(uint32_t* width, uint32_t* height, uint32_t* format) {
    if (width) *width = rec::gs.texCopyDstW;
    if (height) *height = rec::gs.texCopyDstH;
    if (format) *format = rec::gs.texCopyFmt;
}

uint32_t vgxn_disp_copy_height(void) { return static_cast<uint32_t>(rec::gs.dispCopySrc.h); }

uint32_t vgxn_xf_reg(uint32_t addr) {
    const uint32_t i = addr - 0x1000u;
    return i < 0x58u ? rec::gs.xfReg[i] : 0u;
}
uint32_t vgxn_bp_reg(uint32_t reg) { return rec::gs.bp[reg & 0xFFu] & 0x00FFFFFFu; }

void vgxn_set_efb_size(int width, int height) {
    if (width <= 0 || height <= 0) return;
    if (rec::gs.efbW != width || rec::gs.efbH != height) {
        rec::gs.efbW = static_cast<uint16_t>(width);
        rec::gs.efbH = static_cast<uint16_t>(height);
        VGX_LOG("EFB size %dx%d\n", width, height);
        rec::SetTarget(rec::gs.efbW, rec::gs.efbH);
    }
}

void vgxn_set_array(int attr, const void* p, uint32_t size, uint32_t stride, int le) {
    // GX_VA_NBT (25) shares the normal array; the XF arrays up to GX_LIGHT_ARRAY (24) are accepted as is.
    const int a = attr == 25 ? 10 : attr;
    if (a < 9 || a > 24) return;
    // The game's memory is stable until the frame is presented (vgxn_end_frame waits for the player), so the
    // binding is by reference: never ask whether the pointer is "guest" memory (and never copy `size` bytes).
    rec::Array(a, p, size, stride, le != 0, /*guestRam=*/true);
}

void vgxn_ext(uint32_t kind, const uint32_t* words, uint32_t n) { rec::Ext(kind, words, n); }

uint32_t vgxn_begin_raw(int prim, int fmt, uint32_t count) {
    const uint32_t bytes = hle::ShadowStreamVertexBytes(fmt);
    if (bytes == 0 || count == 0) return 0;
    if (!hle::BeginRaw(static_cast<uint8_t>(prim), fmt, count, bytes)) return 0;
    return bytes;
}

int vgxn_pipe_write8(uint32_t v) {
    if (hle::RawWrite8(v)) return 1;
    ++g_droppedPipeWrites;
    return 0;
}
int vgxn_pipe_write16(uint32_t v) {
    if (hle::RawWrite16(v)) return 1;
    ++g_droppedPipeWrites;
    return 0;
}
int vgxn_pipe_write32(uint32_t v) {
    if (hle::RawWrite32(v)) return 1;
    ++g_droppedPipeWrites;
    return 0;
}
uint32_t vgxn_dropped_pipe_writes(void) { return g_droppedPipeWrites; }

void vgxn_load_tex(int tex_map, const vgxn_tex* t) {
    if (static_cast<unsigned>(tex_map) > 7 || !t) return;
    TexObj o{};
    o.data = t->data;
    o.width = t->width;
    o.height = t->height;
    o.format = t->format;
    o.wrapS = t->wrap_s;
    o.wrapT = t->wrap_t;
    o.minFilter = t->min_filter;
    o.magFilter = t->mag_filter;
    o.mipmap = t->mipmap;
    o.minLod = t->min_lod;
    o.maxLod = t->max_lod;
    o.lodBias = t->lod_bias;
    o.tlut = t->tlut;
    o.userData = nullptr;
    o.dataVersion = 1;
    o.magic = kTexMagic;
    rec::Tex(tex_map, o, ++g_texLoadSerial);
}

void vgxn_load_tlut(int index, const void* data, int fmt, int entries) {
    if (static_cast<unsigned>(index) >= 20u || !data) return;
    TlutObj t{};
    t.data = data;
    t.format = static_cast<uint32_t>(fmt);
    t.entries = static_cast<uint16_t>(entries);
    // The palette bytes may have changed under the same pointer: the texture cache re-hashes on a new version.
    t.version = ++g_tlutVersion;
    rec::Tlut(index, t);
}

} // extern "C"
