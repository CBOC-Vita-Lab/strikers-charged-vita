// Direct submission interface for the runtime's GX HLE layer
// (runtime/src/hle/gx, built with -DMSC_VGX). It bypasses the per-component
// GX API (GXPosition3f32, ...): the bytes the game writes to the gather pipe
// between GXBegin and the end of the primitive go straight into a record of
// the stream the render thread consumes, and the vertex descriptor / formats
// and array bindings the primitive needs travel as two compact records.
// Nothing here includes runtime or GX headers.
#pragma once
#include <cstdint>

namespace vgx::hle {

struct VtxFmtRow { uint8_t cnt, type, frac; }; // one attribute of a vertex format (GXCompCnt, GXCompType, frac)

// MSC_VGX_FAST=0 turns the fast paths off (the HLE layer then uses its
// original per-component paths; pictures must not change: MSC_VGX_HASH).
bool Enabled();

// Guest RAM, as host address ranges (MEM1, MEM2): registered once at start-up.
void RegisterGuestRam(const void* hostBase, uint32_t size);
// The game made [hostPtr, hostPtr + size) visible to the graphics hardware
// (data-cache flush / store / invalidate) or the runtime bulk-wrote it (disc
// read, ...). Cached decodes of that range are re-validated.
void NotifyGuestWrite(const void* hostPtr, uint32_t size);

// The complete vertex descriptor (attributes 0..20 = PNMTXIDX..TEX7; values
// 0 none, 1 direct, 2 index8, 3 index16, real types: indexed stays indexed)
// and the format row of `fmt` for attributes 9..20 (POS..TEX7).
void SetVertexState(int fmt, const uint8_t vcd[21], const VtxFmtRow row[12]);

// A vertex array in guest RAM, by reference. `sizeBound`: bytes that may be
// read from hostPtr on (reads beyond it see zeros).
void SetArray(int attr, const void* hostPtr, uint32_t sizeBound, uint32_t stride);

// A display list in guest RAM that contains no CP register writes and no
// nested lists (the caller knows from its scan cache), by reference.
void CallList(const void* hostPtr, uint32_t nbytes);

// Raw primitive. BeginRaw reserves count * vertexBytes bytes in the stream;
// the caller stores the big-endian vertex bytes exactly as the game writes
// them (indices stay indices) through g_rawCur.ptr and calls FinishRaw when
// it reaches g_rawCur.end. Any other GX call that arrives while a
// primitive is open ends it with the whole vertices written so far.
// (One object for both pointers: the game writes 15 000 indices a frame through RawWrite16, one
// call each, and in position-independent code every global costs a load of its address. The
// stores go through a local pointer of another type than the object's members, so the compiler
// need not read the members again after them.)
struct RawCursor { uint8_t* ptr; uint8_t* end; };
extern RawCursor g_rawCur;
bool BeginRaw(uint8_t prim, int fmt, uint32_t count, uint32_t vertexBytes);
void FinishRaw();

inline bool RawWrite8(uint32_t v) {
    RawCursor& c = g_rawCur;
    uint8_t* const p = c.ptr;
    uint8_t* const e = c.end;
    if (p == e) return false;
    p[0] = static_cast<uint8_t>(v);
    c.ptr = p + 1;
    if (p + 1 == e) FinishRaw();
    return true;
}
inline bool RawWrite16(uint32_t v) {
    RawCursor& c = g_rawCur;
    uint8_t* const p = c.ptr;
    uint8_t* const e = c.end;
    if (e - p < 2) return false;
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
    c.ptr = p + 2;
    if (p + 2 == e) FinishRaw();
    return true;
}
inline bool RawWrite32(uint32_t v) {
    RawCursor& c = g_rawCur;
    uint8_t* const p = c.ptr;
    uint8_t* const e = c.end;
    if (e - p < 4) return false;
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
    c.ptr = p + 4;
    if (p + 4 == e) FinishRaw();
    return true;
}

// Matrix loads straight from guest memory (GXLoadPosMtxImm / GXLoadTexMtxImm: `count` = 12 or 8
// big-endian words to XF address `addr`; GXLoadNrmMtxImm: the 3x3 part of a big-endian 3x4
// matrix, nine words to `addr`). The record is the one the GX API path writes (R_XF with the
// words in host order); only the two intermediate copies are gone. Matrix / light memory only:
// addr + count must not reach the XF registers at 0x1000 (those have a shadow on this side).
void LoadXFBE(uint32_t addr, uint32_t count, const void* be);
void LoadNrmMtxBE(uint32_t addr, const void* be);

// Bytes per vertex of the stream a raw primitive of format `fmt` carries under the game thread's current
// vertex descriptor / formats (the API's shadow of them, the same plan the immediate-mode path uses);
// 0 when the descriptor describes nothing drawable. Added for the native port, whose game code writes
// the vertex bytes through the gather-pipe stand-in and has no HLE state tables of its own.
uint32_t ShadowStreamVertexBytes(int fmt);

// For the HLE layer's own "is this interface version there" checks: bumped whenever a function
// is added here (2: RawCursor, LoadXFBE, LoadNrmMtxBE; 3: ShadowStreamVertexBytes).
#define VGX_HLE_INTERFACE 3

} // namespace vgx::hle
