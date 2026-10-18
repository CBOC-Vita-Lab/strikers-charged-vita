// np_gx_pipe.h: the gather-pipe stand-in. Defines WGPIPE for C++ code.
//
// On the console the game writes the vertex data of a primitive (after GXBegin) to the write-gather pipe at
// 0xCC008000: `WGPIPE.us = index;` Natively that address does not exist, and a plain variable would swallow the
// writes. Here WGPIPE is an object whose members' assignment operators append big-endian bytes to the renderer's
// open raw primitive (opened by GXBegin, see gx_core.cpp), exactly as the recompiled build's GX HLE does with the
// guest's pipe writes. Everything is inline and header-only apart from the stray-write path; only builtin types are
// used so that this can be included next to the Wii SDK headers.
//
// Included by platform/gx/include/revolution/gx/GXHardware.h (the wrapper that replaces the SDK's declaration of
// WGPIPE); game code includes <revolution/gx.h> as before.
#ifndef NP_GX_PIPE_H
#define NP_GX_PIPE_H

// (The SDK includes GXHardware.h inside its own extern "C" block: everything here, templates included, has C++ linkage.)
extern "C++" {

// The renderer's cursor (vgx/src/vgx_hle.h: vgx::hle::g_rawCur / FinishRaw): restated here so that game code does
// not need vgx's headers. gx_selfcheck.cpp compiles this next to vgx_hle.h: a drift is an error there.
namespace vgx {
namespace hle {
struct RawCursor {
    unsigned char* ptr;
    unsigned char* end;
};
extern RawCursor g_rawCur;
void FinishRaw();
} // namespace hle
} // namespace vgx

extern "C" void np_gx_pipe_stray(unsigned int bytes);

static inline void np_gx_raw_write8(unsigned int v) {
    vgx::hle::RawCursor& c = vgx::hle::g_rawCur;
    unsigned char* const p = c.ptr;
    unsigned char* const e = c.end;
    if (p == e) { np_gx_pipe_stray(1); return; }
    p[0] = static_cast<unsigned char>(v);
    c.ptr = p + 1;
    if (p + 1 == e) vgx::hle::FinishRaw();
}
static inline void np_gx_raw_write16(unsigned int v) {
    vgx::hle::RawCursor& c = vgx::hle::g_rawCur;
    unsigned char* const p = c.ptr;
    unsigned char* const e = c.end;
    if (e - p < 2) { np_gx_pipe_stray(2); return; }
    p[0] = static_cast<unsigned char>(v >> 8);
    p[1] = static_cast<unsigned char>(v);
    c.ptr = p + 2;
    if (p + 2 == e) vgx::hle::FinishRaw();
}
static inline void np_gx_raw_write32(unsigned int v) {
    vgx::hle::RawCursor& c = vgx::hle::g_rawCur;
    unsigned char* const p = c.ptr;
    unsigned char* const e = c.end;
    if (e - p < 4) { np_gx_pipe_stray(4); return; }
    p[0] = static_cast<unsigned char>(v >> 24);
    p[1] = static_cast<unsigned char>(v >> 16);
    p[2] = static_cast<unsigned char>(v >> 8);
    p[3] = static_cast<unsigned char>(v);
    c.ptr = p + 4;
    if (p + 4 == e) vgx::hle::FinishRaw();
}

struct NpPipe8 {
    template <class T> void operator=(T v) const { np_gx_raw_write8(static_cast<unsigned int>(v)); }
};
struct NpPipe16 {
    template <class T> void operator=(T v) const { np_gx_raw_write16(static_cast<unsigned int>(v)); }
};
struct NpPipe32 {
    template <class T> void operator=(T v) const { np_gx_raw_write32(static_cast<unsigned int>(v)); }
};
struct NpPipeF32 {
    void operator=(float v) const { np_gx_raw_write32(__builtin_bit_cast(unsigned int, v)); }
};
struct NpPipePtr {
    // (a display-list address as GXFastCallDisplayList writes it: a FIFO command, not vertex data)
    void operator=(const void*) const { np_gx_pipe_stray(4); }
};
struct NpGatherPipe {
    NpPipe8 c, uc;
    NpPipe16 s, us;
    NpPipe32 i, ui;
    NpPipePtr p;
    NpPipeF32 f;
};
extern NpGatherPipe g_npGatherPipe;

} // extern "C++"

#define WGPIPE g_npGatherPipe

#endif
