// GX glue, part 2: texture and palette objects.
//
// The game keeps its GXTexObj / GXTlutObj by value inside its own structures (PlatTexture holds 8 and 3 words), copies
// them around with memcpy and hands the copies to GXInitTexObjWrapMode / GXLoadTexObj; the SDK's objects are 32 and
// 12 bytes. vgx's own (Dolphin / Aurora) objects are larger (96 / 48 bytes in its TARGET_PC build), so the glue
// defines the object functions itself: the 32 / 12 bytes hold a compact description in a layout of our own, and
// GXLoadTexObj / GXLoadTlut expand it to what the renderer's recorder takes (vgxn_load_tex / vgxn_load_tlut). The
// image data stays where the game put it, in console format and byte order (tiled, big-endian); only the object's
// description is host-endian.
//
// Quantisation follows the SDK's GXInitTexObjLOD (the recompiled build ran the SDK's own code and decoded the
// hardware bits: LOD limits in 1/16, bias in 1/32), so pictures match.
#include <revolution/gx.h>

#include "vgx_native.h"
#include "gx_internal.h"

static_assert(sizeof(void*) == 4, "the native port is ILP32 (the game's GXTexObj holds 32 bits of pointer)");

namespace {

struct NpTexObj { // 32 bytes
    const void* data;       // 0   image data (all mip levels), console tiled format, big-endian
    unsigned short width;   // 4
    unsigned short height;  // 6
    unsigned char format;   // 8   GXTexFmt / GXCITexFmt
    unsigned char wrapS;    // 9
    unsigned char wrapT;    // 10
    unsigned char minFilter; // 11  GXTexFilter
    unsigned char magFilter; // 12
    unsigned char mipmap;   // 13
    unsigned char ci;       // 14  a CI format (initialised by GXInitTexObjCI)
    unsigned char pad;      // 15
    unsigned char minLod;   // 16  1/16 units, as the SDK stores it
    unsigned char maxLod;   // 17  1/16 units
    signed char lodBias;    // 18  1/32 units
    unsigned char pad2;     // 19
    unsigned int tlut;      // 20  palette name (CI formats)
    void* userData;         // 24
    unsigned int reserved;  // 28
};
static_assert(sizeof(NpTexObj) == 32, "GXTexObj is 32 bytes");
static_assert(sizeof(GXTexObj) >= sizeof(NpTexObj), "compact texture object must fit the SDK's GXTexObj");

struct NpTlutObj { // 12 bytes
    const void* data;       // 0   entries 16-bit big-endian
    unsigned short entries; // 4
    unsigned char format;   // 6   GXTlutFmt
    unsigned char pad;      // 7
    unsigned int reserved;  // 8
};
static_assert(sizeof(NpTlutObj) == 12, "GXTlutObj is 12 bytes");
static_assert(sizeof(GXTlutObj) >= sizeof(NpTlutObj), "compact palette object must fit the SDK's GXTlutObj");

inline NpTexObj* T(const GXTexObj* o) { return reinterpret_cast<NpTexObj*>(const_cast<GXTexObj*>(o)); }
inline NpTlutObj* TL(const GXTlutObj* o) { return reinterpret_cast<NpTlutObj*>(const_cast<GXTlutObj*>(o)); }

inline float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

void InitTex(NpTexObj* t, void* image, unsigned short w, unsigned short h, unsigned int fmt, unsigned int wrapS, unsigned int wrapT, unsigned char mipmap, bool ci) {
    __builtin_memset(t, 0, sizeof(*t));
    t->data = image;
    t->width = w;
    t->height = h;
    t->format = static_cast<unsigned char>(fmt);
    t->wrapS = static_cast<unsigned char>(wrapS);
    t->wrapT = static_cast<unsigned char>(wrapT);
    t->mipmap = mipmap ? 1 : 0;
    t->ci = ci ? 1 : 0;
    // SDK defaults: magnify linear; minify linear (mip-mapped: linear between levels, nearest level for CI), maximum
    // LOD = floor(log2(larger side)) when mip-mapped.
    t->magFilter = GX_LINEAR;
    if (mipmap) {
        t->minFilter = ci ? GX_LIN_MIP_NEAR : GX_LIN_MIP_LIN;
        unsigned int m = w > h ? w : h, lod = 0;
        while (m > 1) { m >>= 1; ++lod; }
        t->maxLod = static_cast<unsigned char>(16.0f * lod);
    } else {
        t->minFilter = GX_LINEAR;
    }
}

} // namespace

extern "C" {

void GXInitTexObj(const GXTexObj* obj, void* image, u16 w, u16 h, GXTexFmt fmt, GXTexWrapMode wrapS, GXTexWrapMode wrapT, GXBool mipmap) {
    InitTex(T(obj), image, w, h, fmt, wrapS, wrapT, mipmap, false);
}

void GXInitTexObjCI(GXTexObj* obj, void* image, u16 w, u16 h, GXCITexFmt fmt, GXTexWrapMode wrapS, GXTexWrapMode wrapT, GXBool mipmap, u32 tlut) {
    InitTex(T(obj), image, w, h, fmt, wrapS, wrapT, mipmap, true);
    T(obj)->tlut = tlut;
}

void GXInitTexObjLOD(GXTexObj* obj, GXTexFilter minFilt, GXTexFilter magFilt, f32 minLod, f32 maxLod, f32 lodBias, GXBool, GXBool, GXAnisotropy) {
    NpTexObj* t = T(obj);
    t->minFilter = static_cast<unsigned char>(minFilt);
    t->magFilter = static_cast<unsigned char>(magFilt);
    if (lodBias < -4.0f) lodBias = -4.0f;
    else if (lodBias >= 4.0f) lodBias = 3.99f;
    t->lodBias = static_cast<signed char>(static_cast<unsigned char>(32.0f * lodBias));
    t->minLod = static_cast<unsigned char>(16.0f * Clamp(minLod, 0.0f, 10.0f));
    t->maxLod = static_cast<unsigned char>(16.0f * Clamp(maxLod, 0.0f, 10.0f));
}

void GXInitTexObjWrapMode(const GXTexObj* obj, GXTexWrapMode wrapS, GXTexWrapMode wrapT) {
    T(obj)->wrapS = static_cast<unsigned char>(wrapS);
    T(obj)->wrapT = static_cast<unsigned char>(wrapT);
}

void GXInitTexObjTlut(const GXTexObj* obj, u32 tlut) { T(obj)->tlut = tlut; }


void GXInitTexObjUserData(const GXTexObj* obj, void* userData) { T(obj)->userData = userData; }
void* GXGetTexObjUserData(const GXTexObj* obj) { return T(obj)->userData; }
u32 GXGetTexObjTlut(GXTexObj* obj) { return T(obj)->tlut; }
void GXGetTexObjAll(GXTexObj* obj, void* image, u16* width, u16* height, GXTexFmt* fmt, GXTexWrapMode* wrapS, GXTexWrapMode* wrapT, GXBool* mipmap) {
    const NpTexObj* t = T(obj);
    *(const void**)image = t->data;
    *width = t->width;
    *height = t->height;
    *fmt = static_cast<GXTexFmt>(t->format);
    *wrapS = static_cast<GXTexWrapMode>(t->wrapS);
    *wrapT = static_cast<GXTexWrapMode>(t->wrapT);
    *mipmap = t->mipmap;
}
void GXGetTexObjLODAll(GXTexObj* obj, GXTexFilter* minFilt, GXTexFilter* magFilt, f32* minLod, f32* maxLod, f32* lodBias, GXBool* biasClamp, GXBool* edgeLod,
                       GXAnisotropy* aniso) {
    const NpTexObj* t = T(obj);
    *minFilt = static_cast<GXTexFilter>(t->minFilter);
    *magFilt = static_cast<GXTexFilter>(t->magFilter);
    *minLod = static_cast<f32>(t->minLod) / 16.0f;
    *maxLod = static_cast<f32>(t->maxLod) / 16.0f;
    *lodBias = static_cast<f32>(t->lodBias) / 32.0f;
    *biasClamp = 0;
    *edgeLod = 0;
    *aniso = GX_ANISO_1;
}
u16 GXGetTexObjWidth(const GXTexObj* obj) { return T(obj)->width; }
u16 GXGetTexObjHeight(const GXTexObj* obj) { return T(obj)->height; }
GXTexFmt GXGetTexObjFmt(const GXTexObj* obj) { return static_cast<GXTexFmt>(T(obj)->format); }
GXTexWrapMode GXGetTexObjWrapS(const GXTexObj* obj) { return static_cast<GXTexWrapMode>(T(obj)->wrapS); }
GXTexWrapMode GXGetTexObjWrapT(const GXTexObj* obj) { return static_cast<GXTexWrapMode>(T(obj)->wrapT); }
GXBool GXGetTexObjMipMap(const GXTexObj* obj) { return T(obj)->mipmap; }

void GXLoadTexObj(const GXTexObj* obj, GXTexMapID map) {
    const NpTexObj* t = T(obj);
    if (!t->data) return;
    vgxn_tex d;
    d.data = t->data;
    d.width = t->width;
    d.height = t->height;
    d.format = t->format;
    d.wrap_s = t->wrapS;
    d.wrap_t = t->wrapT;
    d.min_filter = t->minFilter;
    d.mag_filter = t->magFilter;
    d.mipmap = t->mipmap;
    d.pad[0] = d.pad[1] = d.pad[2] = 0;
    d.min_lod = static_cast<float>(t->minLod) / 16.0f;
    d.max_lod = static_cast<float>(t->maxLod) / 16.0f;
    d.lod_bias = static_cast<float>(t->lodBias) / 32.0f;
    d.tlut = t->tlut;
    vgxn_load_tex(static_cast<int>(map), &d);
}

void GXInitTlutObj(const GXTlutObj* obj, void* palette, GXTlutFmt fmt, u16 entries) {
    NpTlutObj* t = TL(obj);
    t->data = palette;
    t->entries = entries;
    t->format = static_cast<unsigned char>(fmt);
    t->pad = 0;
    t->reserved = 0;
}

void GXLoadTlut(const GXTlutObj* obj, u32 name) {
    const NpTlutObj* t = TL(obj);
    if (!t->data) return;
    vgxn_load_tlut(static_cast<int>(name), t->data, t->format, t->entries);
}

} // extern "C"
