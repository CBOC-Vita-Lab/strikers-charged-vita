// How a frame's arenas (CmdBuf::vtx, idx, uni, draws, cmds) hold the C heap.
//
// Every frame buffer keeps the capacity its largest frame needed (a cleared vector keeps its memory), and a std::vector grows by
// doubling. The player builds each frame in the front end's buffer (draw.cpp g_buf) and swaps it with the buffer the back end is
// drawing (play.cpp g_slot): two buffers circulate, and each doubled its vertex arena on its own, at whatever moment it first
// carried a large frame (a replay, a Mega Strike, the stadium intro). Each doubling asked the C heap (newlib's, a fixed block on
// the console) for one block twice the old one while the old one was still held, and left the old one as a hole that small
// blocks fill. The Galaxy port ran out of heap this way after a dozen levels (abort in BeginVertices); a Strikers session of many
// matches back to back is the same shape. So:
//   - floors: when the renderer starts, while the heap is still in one piece, both buffers get arenas big enough for the biggest
//     frames seen in play (see kVtxKiB below). They are never given back, so they never move and leave no holes;
//   - past its floor the vertex arena grows in steps of at most 2 MiB, not by doubling: the largest single request is the
//     frame's size plus at most 2 MiB.
// None of this changes what a frame holds (sizes, offsets, bytes): only how much room is behind it.
//   MSC_VGX_ARENA_KIB=<n>   the vertex arena's floor in KiB; 0: the old behaviour (no floors, doubling)
//   MSC_VGX_ARENA_LOG=1     the peaks of the frames and the arenas' capacities, every 600 frames (diagnostic)
//   MSC_VGX_ARENA_SOFT=1    (1.3, default on) growth past the floor fails soft: when the C heap has no block for it, the draws that do not fit
//                           that frame are dropped (one line in the log, a counter in the arena line) and the program goes on; the next frame
//                           tries again. 0: the old way (operator new ends the program with a FATAL line)
//   MSC_VGX_ARENA_SIM=<KiB> (test) a growth to a capacity of this many KiB or more is refused as if the heap had no room (with
//                           MSC_VGX_ARENA_KIB=<small floor>: the soft path on a machine with plenty of memory)
#pragma once
#include "vgx_cmd.h"
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>

namespace vgx {
namespace arena {

struct Floors {
    bool on = false;
    size_t vtxBytes = 0, idxBytes = 0, uniBytes = 0; // the arenas' floors
    size_t draws = 0, cmds = 0;                      // records
};

// The floors, from the largest frames of the host routes (nav_match60 and the Mega Strike routes): 7.3 MiB of vertices (the stadium intro and the
// menus' 3D scenes), 920 KiB of indices, 430 KiB of uniforms, 770 draw calls. Open play is 0.5 to 1.5 MiB of vertices. Both buffers reach 8 MiB
// in any session that gets as far as a match, so the floor holds nothing that doubling would not hold soon after. The vertex arena is the one that matters.
constexpr long kVtxKiB = 16384;   // 1.3: was 8192. Stadium intros at Crystal Canyon reach 12 MiB; growing past the floor needs the new block beside the old one, which a cut-up heap refused (abort)
constexpr size_t kIdxKiB = 1024, kUniKiB = 512;
constexpr size_t kDraws = 1024, kCmds = 2048;

inline Floors MakeFloors(long vtxKiB) {
    Floors f;
    if (vtxKiB <= 0) return f;
    f.on = true;
    f.vtxBytes = static_cast<size_t>(vtxKiB) << 10;
    f.idxBytes = kIdxKiB << 10;
    f.uniBytes = kUniKiB << 10;
    f.draws = kDraws;
    f.cmds = kCmds;
    return f;
}
inline const Floors& Get() {
    static const Floors f = MakeFloors(EnvInt("MSC_VGX_ARENA_KIB", kVtxKiB)); // read once
    return f;
}

// The capacity to give the vertex arena when it must hold `need` bytes and has `cap`: at least the floor, and past it half again its
// size, but never more than 2 MiB at a time (rounded to 64 KiB). Without floors: exactly `need` (the vector then doubles as before).
inline size_t VtxGrowTo(const Floors& f, size_t cap, size_t need) {
    if (!f.on) return need;
    size_t step = cap / 2;
    if (step < (256u << 10)) step = 256u << 10;
    if (step > (2u << 20)) step = 2u << 20;
    size_t to = cap + step;
    if (to < need) to = need;
    if (to < f.vtxBytes) to = f.vtxBytes;
    return (to + 0xFFFFu) & ~static_cast<size_t>(0xFFFFu);
}
// Room for `need` bytes in a vertex arena that has less (called before the resize that needs it; without floors the resize
// doubles as before).
template <typename V> inline void VtxMakeRoom(V& vtx, size_t need, const Floors& f) {
    if (f.on && need > vtx.capacity()) vtx.reserve(VtxGrowTo(f, vtx.capacity(), need));
}

template <typename V> inline void ReserveBytes(V& v, size_t bytes) {
    const size_t n = bytes / sizeof(typename V::value_type);
    if (v.capacity() < n) v.reserve(n);
}
template <typename V> inline size_t CapBytes(const V& v) { return v.capacity() * sizeof(typename V::value_type); }

// ---- soft growth (MSC_VGX_ARENA_SOFT)
// The game's operator new does not throw: on a thread other than the game's it ends the program when malloc has no block (nlMemory.cpp
// NpHostAllocFailed; in 1.1 it returned null and the caller wrote through it). The platform offers NpHostTryNew, which returns a block
// that operator delete knows (null when there is none); without it (the host tests, the harness) a nothrow new is used.
extern "C" void* NpHostTryNew(size_t) __attribute__((weak));
extern "C" void NpHostTryDelete(void*) __attribute__((weak));
extern "C" void NpPlatformNote(const char*) __attribute__((weak));
struct SoftStats { unsigned long fails = 0, dropsThisFrame = 0, dropsTotal = 0; size_t lastNeed = 0, lastCap = 0; bool noted = false; };
inline SoftStats& Soft() { static SoftStats s; return s; }
inline bool SoftOn() {
    static const bool on = EnvInt("MSC_VGX_ARENA_SOFT", 1) != 0;
    return on;
}
inline size_t SimLimit() {
    static const size_t kib = static_cast<size_t>(EnvInt("MSC_VGX_ARENA_SIM", 0));
    return kib << 10;
}
inline void* TryAlloc(size_t bytes) {
    const size_t sim = SimLimit();
    if (sim && bytes >= sim) return nullptr;
    if (NpHostTryNew) return NpHostTryNew(bytes);
    return ::operator new(bytes, std::nothrow);
}
inline void TryFree(void* p) {
    if (!p) return;
    if (NpHostTryDelete) NpHostTryDelete(p); else ::operator delete(p);
}
// Room for `need` bytes in `vtx` without ending the program when the heap cannot give it: true when the vector can hold `need` now.
// On false the vector is untouched. The block is taken first and handed to the vector's own reserve() through the allocator, so no other
// thread can take its place in between and nothing is held twice beyond the copy itself. A growth step that does not fit is tried again at the
// bare size of the request before giving up.
template <typename V> inline bool VtxTryRoom(V& vtx, size_t need, const Floors& f) {
    if (need <= vtx.capacity()) return true;
    if (!f.on || !SoftOn()) { VtxMakeRoom(vtx, need, f); return true; }
    const size_t step = VtxGrowTo(f, vtx.capacity(), need);
    size_t to = step;
    void* blk = TryAlloc(to);
    if (!blk && step > need) { to = (need + 0xFFFFu) & ~static_cast<size_t>(0xFFFFu); blk = TryAlloc(to); }
    if (!blk) {
        SoftStats& s = Soft();
        ++s.fails;
        s.lastNeed = need;
        s.lastCap = vtx.capacity();
        return false;
    }
    NoInitPending& p = NoInitPendingBlock();
    p.block = blk;
    p.bytes = to;
    vtx.reserve(to);
    if (p.block) { TryFree(p.block); p.block = nullptr; p.bytes = 0; } // (not taken: the vector found its capacity enough)
    return true;
}
// One line the first time a frame loses draws to it (the platform's own record when it has one), the rest counted.
inline void SoftNoteDrop(uint64_t frame) {
    SoftStats& s = Soft();
    ++s.dropsThisFrame;
    ++s.dropsTotal;
    if (s.noted) return;
    s.noted = true;
    char line[200];
    std::snprintf(line, sizeof line, "[arena] vertex arena could not grow (frame %llu): %lu KiB wanted over %lu KiB held; draws that do not fit are dropped until the heap has room",
                  static_cast<unsigned long long>(frame), static_cast<unsigned long>(s.lastNeed >> 10), static_cast<unsigned long>(s.lastCap >> 10)); // (%lu: the Vita's libc does not know %zu)
    if (NpPlatformNote) NpPlatformNote(line);
    std::fprintf(stderr, "%s\n", line);
}

// A frame buffer's arenas at their floors (at the renderer's start, before the heap is cut up).
inline void Reserve(CmdBuf& b, const Floors& f) {
    if (!f.on) return;
    ReserveBytes(b.vtx, f.vtxBytes);
    ReserveBytes(b.idx, f.idxBytes);
    ReserveBytes(b.uni, f.uniBytes);
    if (b.draws.capacity() < f.draws) b.draws.reserve(f.draws);
    if (b.cmds.capacity() < f.cmds) b.cmds.reserve(f.cmds);
}

// Host memory the buffer's arenas hold (capacity, not use).
inline size_t HostBytes(const CmdBuf& b) {
    return CapBytes(b.vtx) + CapBytes(b.idx) + CapBytes(b.uni) + CapBytes(b.draws) + CapBytes(b.cmds);
}

// ---- census (the circulating buffers register; the front end reports the frames' peaks)
void Track(const CmdBuf* b);                  // a buffer whose capacity the census adds up
void NoteFrame(const CmdBuf& b, uint64_t n);  // at the end of every frame (front end): peaks, and the log line when MSC_VGX_ARENA_LOG is set
size_t TrackedBytes(unsigned* buffers, size_t* vtxBytes);

} // namespace arena
} // namespace vgx

// For the platform's periodic memory line: "frame buffers 5120 KiB in 2 (vertex arenas 4096 KiB), largest frame 1534 KiB of vertices".
extern "C" size_t vgx_host_memory_describe(char* out, size_t cap);
