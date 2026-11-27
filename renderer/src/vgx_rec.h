// The game-thread side of vgx: the "recorder".
//
// GX API calls made by the runtime's HLE layer are not executed where they are
// made. Each is turned into a compact record appended to a stream; a "player"
// (vgx_play.cpp) reads the stream, applies the records to the front end's
// register file and runs vertex decoding, state resolution and shader lookup.
// The player normally runs on the render thread and consumes the stream while
// the game thread is still producing it, the way the console's graphics
// processor consumed its FIFO. The game thread therefore pays only for
// encoding a call into a register value (which needs a shadow copy of the
// registers it read-modify-writes) and for copying bytes.
//
// What a record may refer to:
//  - by value: register writes, matrices, immediate-mode vertex bytes, texture
//    object descriptions, command streams that live in transient host buffers;
//  - by reference: display lists, vertex arrays and texture/TLUT data that live
//    in guest RAM. They are read by the player some time later, but always
//    before the game thread returns from presenting the frame (rec::Present
//    waits for the player to have consumed the whole frame), which is the same
//    guarantee the console gave: data handed to the graphics processor had to
//    stay unchanged until it had drawn the frame.
//
//   MSC_VGX_PLAY=thread     (default) player on the render thread, streaming
//   MSC_VGX_PLAY=frame      player on the game thread, a frame at a time, at present
//   MSC_VGX_PLAY=immediate  player on the game thread after every record: the
//                           reference behaviour (nothing is deferred), used to
//                           check that deferring changes nothing (MSC_VGX_HASH)
#pragma once
#include "vgx_common.h"
#include "vgx_state.h"

#include <atomic>

namespace vgx::rec {

enum Op : uint32_t {
    R_NOP = 0,
    R_JUMP,         // the stream continues in the next chunk
    R_RESET,        // power-on register state
    R_BP,           // [1] reg << 24 | final 24-bit value
    R_XF,           // aux = count; [1] addr, [count] words
    R_VCD,          // aux = attr | type << 8
    R_VAT,          // aux = fmt | attr << 4 | cnt << 12 | type << 16 ; [1] frac
    R_ARRAY,        // aux = attr | le << 8; [1] size, [1] stride, [2] pointer (by reference)
    R_ARRAY_COPY,   // aux = attr | le << 8; [1] size, [1] stride, [n] bytes (by value)
    R_VTXSTATE,     // aux = fmt; [15] vcd[21] + 12 x {cnt, type, frac} (see vgx_hle.h)
    R_TEX,          // aux = texmap; [n] TexObj, [1] load serial
    R_TLUT,         // aux = index; [n] TlutObj
    R_INVAL_TEX,
    R_STREAM_REF,   // [1] nbytes, [2] pointer
    R_STREAM_COPY,  // [1] nbytes, [n] bytes
    R_DRAW_RAW,     // aux = prim | fmt << 8; [1] vertex count, [1] nbytes, [n] big-endian vertex bytes as the game wrote them
    R_DRAW_DEC,     // aux = prim; [1] layout mask, [1] vertex count, [n] decoded vertices
    R_COPY_TEX,     // aux = clear | half << 1 | clearOnly << 2 (nxr4b MSC_GLOW_L0OFF=2: the game says nothing reads this copy, only its clear is wanted); [1] fmt, [2] src x,y / w,h (s16 pairs), [1] dst w | h << 16, [2] dest pointer
    R_COPY_DISP,    // aux = clear; [2] src x,y / w,h
    R_DESTROY_COPY, // [2] dest pointer
    R_SET_TARGET,   // [1] w | h << 16
    R_FLAGS,        // aux = capture | skipDraws << 1: back-end flags for the frame that follows
    R_PRESENT,
    R_EXT,          // aux = kind | nwords << 8; [nwords] payload: extension records of the native port (vgx_ext.h)
    R_OP_COUNT
};

// ------------------------------------------------------------------ stream
// Single producer (game thread), single consumer (player). Records are whole
// numbers of 32-bit words and never straddle chunks.
struct Chunk {
    std::atomic<Chunk*> next{nullptr};
    std::atomic<uint32_t> committed{0}; // words the consumer may read
    uint32_t cap = 0;
    // (diagnostics for the abort on a corrupt stream: which chunk it was, and whether the consumer had already handed it back to the free list)
    std::atomic<uint32_t> serial{0};    // the n-th chunk made or taken from the free list
    std::atomic<uint32_t> state{0};     // 0 in use by the stream, 1 handed back (in the free list)
    uint32_t data[1];
};

struct Producer {
    Chunk* cur = nullptr;
    // GXD (wave 5): the write position and the fit limit as addresses, so that Reserve is a compare of two loaded words (it was a test of g_immOpen, of cur, and
    // an add / compare / load of the chunk's capacity: 24 instructions per record on the ARM, 13 000 records per frame).
    uint32_t* wp = nullptr;    // the next free word of `cur`
    uintptr_t limit = 0;       // a record of n words fits while wp + n <= limit (one word of the chunk stays free for the jump marker); 0: the slow path decides
                               // (no chunk yet, or a primitive is open and must be closed first)
    uint64_t wordsBefore = 0;  // words written into the chunks before `cur` (statistics only)
};
extern Producer g_prod;
extern int g_mode;                 // 0 immediate, 1 frame, 2 thread
enum { MODE_IMMEDIATE = 0, MODE_FRAME = 1, MODE_THREAD = 2 };

void Grow(uint32_t words);  // makes room for a record of `words` words
void AfterCommitSlow();     // immediate mode: play now; thread mode: wake the player
void WakePlayerOnce();      // int28: wake an idle thread-mode player on the first record of a new frame
extern std::atomic<bool> g_playerIdle;
extern bool g_wakeCheckArmed;      // int31 (producer-only): armed after Present() so commits 1..4 immediately check g_playerIdle
extern uint32_t g_commitSinceWake; // int31 (producer-only): periodic mid-frame wake cadence (1 in 32 commits)
void CloseImm();            // finishes the open primitive (immediate-mode or raw), see vgx_rec.cpp
extern bool g_immOpen;      // a primitive's record is reserved but not committed yet

#if defined(VGX_PRODUCER_CHECK)
// W5 PIPE (a census build only, -DVGX_PRODUCER_CHECK): called by every record before it is reserved, so that the platform layer can see a second thread recording
// while the frame-send job is running (np/docs/wave5-PIPE.md). Not compiled into the normal libraries: they pay nothing.
extern void (*g_producerHook)();
#endif
uint32_t* ReserveSlow(uint32_t words); // closes an open primitive, starts a chunk when the record does not fit
inline uint32_t Wpos() { return static_cast<uint32_t>(g_prod.wp - g_prod.cur->data); }
inline void RefreshLimit() { // (not while a primitive is open: it is closed by the slow path of the next Reserve)
    g_prod.limit = g_immOpen || !g_prod.cur ? 0 : reinterpret_cast<uintptr_t>(g_prod.cur->data + g_prod.cur->cap - 1u);
}
extern bool g_prefetchRec; // MSC_PREFETCH bit 4 (1.3 armcpu9, default off): the line 96 bytes ahead of the write pointer is requested for writing
inline uint32_t* Reserve(uint32_t words) {
#if defined(VGX_PRODUCER_CHECK)
    if (g_producerHook) g_producerHook();
#endif
    uint32_t* const p = g_prod.wp;
    if (g_prefetchRec) __builtin_prefetch(p + 24, 1);
    if (__builtin_expect(reinterpret_cast<uintptr_t>(p) + (static_cast<uintptr_t>(words) << 2) > g_prod.limit, 0)) return ReserveSlow(words);
    return p;
}
inline void Commit(uint32_t words) {
    g_prod.wp += words;
    g_prod.cur->committed.store(Wpos(), std::memory_order_release);
    if (__builtin_expect(g_mode == MODE_THREAD, 1)) {
        const uint32_t c = ++g_commitSinceWake;
        if (__builtin_expect(g_wakeCheckArmed || ((c & 31u) == 0u), 0)) {
            std::atomic_thread_fence(std::memory_order_seq_cst);
            if (__builtin_expect(g_playerIdle.load(std::memory_order_relaxed), 0)) {
                WakePlayerOnce();
                g_wakeCheckArmed = false;
            } else if (c >= 4u) {
                g_wakeCheckArmed = false;
            }
        }
    }
    if (g_mode == MODE_IMMEDIATE) AfterCommitSlow();
}
// State records (registers, matrices, descriptors, array and texture binds: 11 000 of the
// 13 000 records of a match frame) are not published one by one: the player can do nothing
// useful with them before the draw, copy or present that follows, and that record's Commit
// publishes everything written before it (so does a change of chunk, Grow). What is saved is
// the release store per record, which on ARMv7 is a `dmb` barrier plus the store.
// g_publishEach: immediate mode (the reference: play after every record) or
// MSC_VGX_LAZYPUB=0 (publish every record as before).
extern bool g_publishEach;
void PublishSlow();
inline void CommitState(uint32_t words) {
    g_prod.wp += words;
    if (g_publishEach) PublishSlow();
}
inline uint32_t Hdr(Op op, uint32_t aux = 0) { return static_cast<uint32_t>(op) | (aux << 8); }
inline void Emit0(Op op, uint32_t aux = 0) {
    uint32_t* r = Reserve(1);
    r[0] = Hdr(op, aux);
    Commit(1);
}
inline void Emit1(Op op, uint32_t aux, uint32_t a) {
    uint32_t* r = Reserve(2);
    r[0] = Hdr(op, aux);
    r[1] = a;
    Commit(2);
}
inline void Emit0State(Op op, uint32_t aux = 0) {
    uint32_t* r = Reserve(1);
    r[0] = Hdr(op, aux);
    CommitState(1);
}
inline void Emit1State(Op op, uint32_t aux, uint32_t a) {
    uint32_t* r = Reserve(2);
    r[0] = Hdr(op, aux);
    r[1] = a;
    CommitState(2);
}
inline uint32_t WordsFor(uint32_t bytes) { return (bytes + 3u) >> 2; }
inline void PutPtr(uint32_t* w, const void* p) {
    const uint64_t v = reinterpret_cast<uintptr_t>(p);
    std::memcpy(w, &v, 8);
}
inline const void* GetPtr(const uint32_t* w) {
    uint64_t v;
    std::memcpy(&v, w, 8);
    return reinterpret_cast<const void*>(static_cast<uintptr_t>(v));
}

// ------------------------------------------------------------------ shadow
// What the API encoders read back on the game thread. Mirrors the SDK's own
// shadow registers: it sees API calls only, never the contents of display
// lists.
struct Shadow {
    uint32_t bp[256];
    uint32_t bpMask;
    uint32_t xfReg[0x58];
    uint32_t matIdxA, matIdxB;
    uint8_t vcd[26];
    VtxAttrFmt vat[8][26];
    bool layoutDirty;
    ArrayRef arrays[26]; // by-reference bindings last sent (exact: only records change the player's)
    EfbRect dispCopySrc, texCopySrc;
    uint16_t dispCopyDstW, dispCopyDstH, texCopyDstW, texCopyDstH;
    uint32_t texCopyFmt;
    bool texCopyHalf;
    uint16_t efbW, efbH;
};
extern Shadow gs;

// ---- recording entry points used by vgx_api.cpp
void Reset();
void BPSlow(uint32_t regAndValue);              // raw write (mask register honoured): the general path
void BPFieldSlow(uint8_t reg, int shift, int count, uint32_t value);
// MSC_REC_FAST (wave 9, default 1; 0 = the calls as they were): the two register writes of every GXSet* call inline. The records and the shadow are the same words as
// BPSlow / BPFieldSlow make (MSC_REC_HASH proves the stream): the common case (no mask pending, a register that is neither the mask, a token register nor the EFB-copy trigger)
// skips the dispatch of the general path.
extern bool g_recFast;
extern uint64_t g_recCnt[4];  // MSC_REC_FAST_STATS=1 prints at exit: [0] BPSlow calls, [1] unused, [2] fog memo hits, [3] fog computed
extern int g_recBreak;   // MSC_REC_FAST_BREAK=1|2 (mutants of the proof, never on): 1 the inline BP does not update the shadow, 2 the fog memo ignores its key
inline void BP(uint32_t rv) {
    const uint32_t reg = rv >> 24;
    if (__builtin_expect(g_recFast && gs.bpMask == 0x00FFFFFFu && reg != 0xFE && reg != 0x45 && reg != 0x47 && reg != 0x48 && reg != 0x52, 1)) {
        const uint32_t nv = rv & 0x00FFFFFFu;
        if (__builtin_expect(g_recBreak != 1, 1)) gs.bp[reg] = nv;
        uint32_t* r = Reserve(2);
        r[0] = Hdr(R_BP, 0);
        r[1] = rv;
        CommitState(2);
        return;
    }
    BPSlow(rv);
}
inline void BP(uint8_t reg, uint32_t v) { BP((static_cast<uint32_t>(reg) << 24) | (v & 0x00FFFFFFu)); }
inline void BPField(uint8_t reg, int shift, int count, uint32_t value) {
    if (__builtin_expect(!g_recFast, 0)) { BPFieldSlow(reg, shift, count, value); return; }
    const uint32_t mask = ((1u << count) - 1u) << shift;
    const uint32_t nv = ((gs.bp[reg] & ~mask) | ((value << shift) & mask)) & 0x00FFFFFFu;
    gs.bp[reg] = nv;
    uint32_t* r = Reserve(2);
    r[0] = Hdr(R_BP, 0);
    r[1] = (static_cast<uint32_t>(reg) << 24) | nv;
    CommitState(2);
}
void XF(uint32_t addr, uint32_t count, const uint32_t* hostWords);
// GXD_RECFAST: the same records XF() makes (tools/gxd/tests/test_recxf.cpp compares the streams word for word), encoded without the call, and the matrix
// words copied once, from the caller's matrix into the record (the API functions made a stack copy first and XF() copied it again).
inline void XFReg(uint32_t addr, uint32_t v) {
    if (__builtin_expect(!Gxd(GXD_RECFAST), 0)) { XF(addr, 1, &v); return; }
    if (addr >= 0x1000 && addr < 0x1058) { // the shadow of the XF registers, as in XF()
        if (addr == 0x1018) gs.matIdxA = v;
        else if (addr == 0x1019) gs.matIdxB = v;
        gs.xfReg[addr - 0x1000] = v;
    }
    uint32_t* r = Reserve(3);
    r[0] = Hdr(R_XF, 1);
    r[1] = addr;
    r[2] = v;
    CommitState(3);
}
// `n` words (12: position / 3x4 texture matrix, 8: 2x4 texture matrix) from `m`, which holds floats in host order.
inline void XFMtx(uint32_t addr, const void* m, uint32_t n) {
    if (__builtin_expect(!Gxd(GXD_RECFAST) || addr + n > 0x1000, 0)) {
        uint32_t w[12];
        std::memcpy(w, m, n * 4);
        XF(addr, n, w);
        return;
    }
    uint32_t* r = Reserve(2 + n);
    r[0] = Hdr(R_XF, n);
    r[1] = addr;
    if (n == 12) std::memcpy(r + 2, m, 48);
    else std::memcpy(r + 2, m, n * 4);
    CommitState(2 + n);
}
// The upper-left 3x3 of a 3x4 matrix (normal matrix memory, 9 words).
inline void XFNrm(uint32_t addr, const void* m) {
    const uint8_t* s = static_cast<const uint8_t*>(m);
    if (__builtin_expect(!Gxd(GXD_RECFAST) || addr + 9 > 0x1000, 0)) {
        uint32_t w[9];
        for (int row = 0; row < 3; ++row) std::memcpy(w + row * 3, s + row * 16, 12);
        XF(addr, 9, w);
        return;
    }
    uint32_t* r = Reserve(2 + 9);
    r[0] = Hdr(R_XF, 9);
    r[1] = addr;
    std::memcpy(r + 2, s, 12);
    std::memcpy(r + 5, s + 16, 12);
    std::memcpy(r + 8, s + 32, 12);
    CommitState(2 + 9);
}
void Vcd(int attr, int type);
void Vat(int fmt, int attr, int cnt, int type, int frac);
// `guestRam`: the caller knows data is guest RAM (binding by reference without asking).
void Array(int attr, const void* data, uint32_t size, uint32_t stride, bool le, bool guestRam = false);
void Tex(int texMap, const TexObj& obj, uint32_t loadSerial);
void Tlut(int idx, const TlutObj& obj);
void Stream(const uint8_t* data, uint32_t nbytes, bool mayWriteCp = true);
void DrawRaw(uint8_t prim, int fmt, const uint8_t* data, uint32_t count, uint32_t nbytes);
void CopyTex(const void* dest, bool clear);
void NextCopyClearOnly();   // nxr4b: the next CopyTex is marked clear-only (consumed by it)
void CopyDisp(bool clear);
void DestroyCopy(const void* dest);
void SetTarget(uint16_t w, uint16_t h);
void BackendFlags(bool capture, bool skipDraws);
// An extension record (vgx_ext.h): `n` payload words (at most 0xFFFF), played in command order.
void Ext(uint32_t kind, const uint32_t* words, uint32_t n);
// Ends the frame. Returns when the player has consumed every record of the
// frame (so guest memory the frame referred to may change again); with
// `waitExecuted` also when the back end has drawn it.
void Present(bool waitExecuted, uint64_t latencyTag = 0);
// immediate mode
void ImmBegin(uint8_t prim, int fmt, uint32_t count);
void ImmEnd();
void ImmPos(float x, float y, float z, bool raw);
void ImmNrm(float x, float y, float z, int rawFracBits); // rawFracBits < 0: already float
void ImmColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void ImmTex(float s, float t, bool raw);
void ImmMtxIdx(int attr, uint8_t idx);

// Is [p, p+n) guest RAM (stable until the end of the frame) rather than a
// transient host buffer?
bool IsGuestMemory(const void* p, uint32_t n);
void SetGuestMemoryProbe(uint64_t (*fn)(const void*, size_t));

// ------------------------------------------------------------------ player
// vgx_play.cpp. PlayPending() consumes everything committed so far and returns
// the number of records played; PlayerThreadMain() is the render thread's loop
// in thread mode.
uint32_t PlayPending();
void PlayerThreadMain();
// Producer stopped at a frame boundary: wait for all submitted frames, without adding a frame.
bool WaitPresented(unsigned timeoutMs);
// With MSC_VGX_BACKEND_THREAD=1 the player runs on a thread PlayerThreadMain creates. `fn` is
// called first thing on that thread (the host pins it to a core there). Set it before
// PlayerThreadMain.
void SetPlayerThreadInit(void (*fn)());
void InitMode(); // reads MSC_VGX_PLAY (once)

} // namespace vgx::rec
