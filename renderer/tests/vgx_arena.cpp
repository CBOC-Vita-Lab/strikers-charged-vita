// The frame arenas' hold on the C heap (renderer/src/vgx_arena.h), on the host.
//
// Two frame buffers circulate as in the player (the front end's own and the back end's slot, swapped every frame) through a scripted session
// of matches: each starts with a few frames of 4.4 to 7.3 MiB of vertices (the stadium intro and the menus' 3D scenes; up to 920 KiB of indices),
// as the host routes measured, and goes on with ordinary play of 0.5 to 1.5 MiB; one stage has a frame of 9.5 MiB, past the floor. The vertex
// arena grows the way the front end grows it (VtxMakeRoom, then resize). Every allocation is seen by a replacement operator new and mirrored
// into a capped first-fit heap, in which the game's small blocks come and go between the stages (what cuts the console's heap into pieces).
// Checked, with the floors on (the default) against the old behaviour (MSC_VGX_ARENA_KIB=0, doubling):
//   - once the floors are reserved, only the frame past the floor asks for a large block, within its size + 2 MiB;
//   - the buffers hold no more than the old way at the end;
//   - a capped heap the old way runs out of (ARENA_TIGHT_MIB=<n> to try others) still meets every request with the floors, and so does the
//     console's 64 MiB.
#include "vgx_arena.h"
#include <cstdio>
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete" // (the replacement operator new below is malloc's)
#endif
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <new>
#include <vector>

namespace {
// ---- every allocation of the test passes here
size_t g_largest = 0, g_bigRequests = 0, g_bigInPlay = 0;
bool g_inPlay = false, g_measure = false; // (only the frame buffers' requests are measured, not the stand-in for the game's)
struct ToyHeap; ToyHeap* g_toy = nullptr;
void ToyNote(void* p, size_t n, bool alloc);
}

void* operator new(size_t n) {
    if (g_measure) {
        if (n > g_largest) g_largest = n;
        if (n >= (1u << 20)) ++g_bigRequests;
        if (n >= (1u << 20) && g_inPlay) ++g_bigInPlay;
    }
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    ToyNote(p, n, true);
    return p;
}
void operator delete(void* p) noexcept { ToyNote(p, 0, false); std::free(p); }
void operator delete(void* p, size_t) noexcept { ToyNote(p, 0, false); std::free(p); }

namespace {
// ---- a first-fit heap of fixed size (address bookkeeping only), standing for newlib's 64 MiB arena on the console. The test's
// blocks of 4 KiB and more are mirrored into it; the game's long-lived small blocks are placed in it between the stages, so
// the free space is cut up as the console's was (thousands of pieces).
struct ToyHeap {
    size_t size;
    std::map<size_t, size_t> freeList; // start -> length
    std::map<void*, std::pair<size_t, size_t>> placed; // real pointer -> (start, length)
    bool failed = false, smallFailed = false; // a request of 1 MiB or more found no room / a smaller one (the game's) found none
    size_t failSize = 0;
    explicit ToyHeap(size_t s) : size(s) { freeList[0] = s; }
    bool Alloc(size_t n, size_t& at) {
        n = (n + 15) & ~size_t(15);
        for (auto it = freeList.begin(); it != freeList.end(); ++it) {
            if (it->second < n) continue;
            at = it->first;
            const size_t rest = it->second - n;
            freeList.erase(it);
            if (rest) freeList[at + n] = rest;
            return true;
        }
        return false;
    }
    void Free(size_t at, size_t n) {
        n = (n + 15) & ~size_t(15);
        auto next = freeList.lower_bound(at);
        if (next != freeList.end() && at + n == next->first) { n += next->second; next = freeList.erase(next); }
        if (next != freeList.begin()) {
            auto prev = std::prev(next);
            if (prev->first + prev->second == at) { prev->second += n; return; }
        }
        freeList[at] = n;
    }
};
bool g_toyBusy = false;
void ToyNote(void* p, size_t n, bool alloc) {
    if (!g_toy || g_toyBusy) return;
    g_toyBusy = true; // (the map's own nodes come through operator new too)
    if (alloc) {
        if (n >= 4096) {
            size_t at = 0;
            if (g_toy->Alloc(n, at)) g_toy->placed[p] = {at, n};
            else if (n >= (1u << 20)) { if (!g_toy->failed) { g_toy->failed = true; g_toy->failSize = n; } } // (a large request: the frame buffers')
            else g_toy->smallFailed = true;
        }
    } else {
        auto it = g_toy->placed.find(p);
        if (it != g_toy->placed.end()) { g_toy->Free(it->second.first, it->second.second); g_toy->placed.erase(it); }
    }
    g_toyBusy = false;
}

using namespace vgx;

uint32_t g_rng = 12345;
uint32_t Rand() { g_rng = g_rng * 1664525u + 1013904223u; return g_rng >> 8; }

struct Result {
    size_t largest = 0, bigRequests = 0, bigInPlay = 0, held = 0, maxHeld = 0;
    bool toyFailed = false, toySmallFailed = false;
    size_t toyFailSize = 0, toyPieces = 0;
};

size_t Held(const std::vector<CmdBuf*>& bufs) {
    size_t n = 0;
    for (CmdBuf* b : bufs) n += arena::CapBytes(b->vtx) + arena::CapBytes(b->idx) + arena::CapBytes(b->uni);
    return n;
}

// One frame into `b`: vertices in pieces of a few KiB as BeginVertices asks for them, indices and uniforms alike.
void Fill(CmdBuf& b, size_t vtxBytes, size_t idxBytes, size_t uniBytes, const arena::Floors& f) {
    for (size_t at = 0; at < vtxBytes;) {
        const size_t piece = 1024 + Rand() % 8192;
        const size_t n = at + piece < vtxBytes ? at + piece : vtxBytes;
        arena::VtxMakeRoom(b.vtx, n, f);
        b.vtx.resize(n);
        b.vtx[n - 1] = 1;
        at = n;
    }
    for (size_t i = 0; i < idxBytes / 2; ++i) b.idx.push_back(static_cast<uint16_t>(i));
    for (size_t i = 0; i < uniBytes / 4; ++i) b.uni.push_back(1.0f);
}

Result Session(const arena::Floors& f, int stages, size_t heapBytes, bool past) {
    Result r;
    ToyHeap toy(heapBytes);
    g_toy = &toy;
    std::vector<void*> game; // the game's long-lived small blocks
    std::vector<CmdBuf*> bufs;
    // The game's own heap use at the start (in the toy heap: 16 MiB in one block), then the renderer's start.
    void* boot = ::operator new(5u << 20);
    for (int i = 0; i < 2; ++i) {
        bufs.push_back(new CmdBuf());
        arena::Reserve(*bufs.back(), f);
    }
    g_largest = g_bigRequests = g_bigInPlay = 0;
    unsigned frame = 0;
    const size_t burst[] = {7333u << 10, 6976u << 10, 6475u << 10, 6094u << 10, 4756u << 10, 4444u << 10};
    for (int s = 0; s < stages; ++s) {
        // Between stages: the game frees half of what it had and takes 500 new blocks of 4-48 KiB, in between whatever is free
        // now (what cuts the heap up; about 26 MiB of them live at a time).
        for (size_t i = 0; i < game.size(); ++i)
            if (Rand() % 2 == 0) { ::operator delete(game[i]); game[i] = game.back(); game.pop_back(); --i; }
        for (int i = 0; i < 500; ++i) game.push_back(::operator new(4096 + (Rand() % 44) * 1024));
        g_inPlay = false;
        g_measure = true;
        for (int k = 0; k < 8; ++k, ++frame) { // the stage's first frames: the big ones
            CmdBuf& b = *bufs[frame % 2];
            size_t v = k < 6 ? burst[(k + s) % 6] : (300u << 10);
            if (past && s == stages - 3 && k == 2) v = 9500u << 10; // (one frame past the floor)
            Fill(b, v, k < 6 ? (v / 8) : (30u << 10), 430u << 10, f);
            if (Held(bufs) > r.maxHeld) r.maxHeld = Held(bufs);
            b.clear();
        }
        g_inPlay = true;
        for (int k = 0; k < 600; ++k, ++frame) { // ordinary play: 0.6 to 1.25 MiB of vertices
            CmdBuf& b = *bufs[frame % 2];
            const size_t v = (500u << 10) + Rand() % (1000u << 10);
            Fill(b, v, v / 10, 100u << 10, f);
            b.clear();
        }
        g_inPlay = false;
        g_measure = false;
    }
    r.held = Held(bufs);
    r.largest = g_largest;
    r.bigRequests = g_bigRequests;
    r.bigInPlay = g_bigInPlay;
    r.toyFailed = toy.failed;
    r.toySmallFailed = toy.smallFailed;
    r.toyFailSize = toy.failSize;
    r.toyPieces = toy.freeList.size();
    for (void* p : game) ::operator delete(p);
    ::operator delete(boot);
    for (CmdBuf* b : bufs) delete b;
    g_toy = nullptr;
    return r;
}

int g_fail = 0;
void Check(bool ok, const char* what) {
    std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fail = 1;
}
} // namespace

int main() {
    const size_t MiB = 1u << 20;
    // The growth rule itself.
    {
        const arena::Floors f = arena::MakeFloors(8192);
        Check(arena::VtxGrowTo(f, 0, 100) == 8 * MiB, "an empty arena grows to its floor");
        Check(arena::VtxGrowTo(f, 2 * MiB, 2 * MiB + 1) == 8 * MiB, "an arena below its floor grows to it");
        Check(arena::VtxGrowTo(f, 8 * MiB, 8 * MiB + 1) == 10 * MiB, "past the floor: at most 2 MiB at a time");
        Check(arena::VtxGrowTo(arena::MakeFloors(512), 2 * MiB, 2 * MiB + 1) == 3 * MiB, "below 4 MiB: half again");
        Check(arena::VtxGrowTo(f, 8 * MiB, 13 * MiB + 5) == 13 * MiB + 0x10000, "a need beyond the step is met (rounded to 64 KiB)");
        const arena::Floors off = arena::MakeFloors(0);
        Check(!off.on && arena::VtxGrowTo(off, 2 * MiB, 3 * MiB) == 3 * MiB, "MSC_VGX_ARENA_KIB=0: off");
        CmdBuf b;
        arena::Reserve(b, f);
        Check(arena::CapBytes(b.vtx) == 8 * MiB && arena::CapBytes(b.idx) == 1024u << 10 && arena::CapBytes(b.uni) == 512u << 10 &&
                  b.draws.capacity() == 1024 && b.cmds.capacity() == 2048,
              "floors reserved");
        const uint8_t* const base = b.vtx.data();
        arena::VtxMakeRoom(b.vtx, 7 * MiB, f);
        b.vtx.resize(7 * MiB);
        b.clear();
        Check(b.vtx.data() == base && arena::CapBytes(b.vtx) == 8 * MiB, "a frame within the floor does not move the arena, and clear() keeps it");
    }
    // The session, both ways, in the console's heap and in a tighter one.
    const arena::Floors on = arena::MakeFloors(8192), off = arena::MakeFloors(0);
    const int stages = 14;
    for (size_t heapMiB : {static_cast<size_t>(64), static_cast<size_t>(std::getenv("ARENA_TIGHT_MIB") ? std::atoi(std::getenv("ARENA_TIGHT_MIB")) : 48)}) {
        const Result a = Session(off, stages, heapMiB * MiB, heapMiB == 64);
        const Result b = Session(on, stages, heapMiB * MiB, heapMiB == 64);
        std::printf("%zu MiB heap, old (doubling): largest request %zu KiB, %zu requests of 1 MiB or more (%zu in ordinary play), held at the end %zu KiB; "
                    "capped heap: %s%zu KiB, %zu free pieces\n",
                    heapMiB, a.largest >> 10, a.bigRequests, a.bigInPlay, a.held >> 10, a.toyFailed ? "FAILED a request of " : "ok ", a.toyFailSize >> 10,
                    a.toyPieces);
        std::printf("%zu MiB heap, new (floors, steps): largest request %zu KiB, %zu requests of 1 MiB or more (%zu in ordinary play), held at the end %zu KiB; "
                    "capped heap: %s%zu KiB, %zu free pieces\n",
                    heapMiB, b.largest >> 10, b.bigRequests, b.bigInPlay, b.held >> 10, b.toyFailed ? "FAILED a request of " : "ok ", b.toyFailSize >> 10,
                    b.toyPieces);
        if (heapMiB == 64) {
            Check(b.bigRequests <= 2 && b.largest == 10 * MiB,
                  "after the floors only the frame past them asks for large blocks (its vertices: 9.5 MiB + at most 2 MiB)");
            Check(b.largest < a.largest, "smaller than doubling's largest");
            Check(b.bigInPlay == 0, "ordinary play asks for nothing of 1 MiB or more");
            Check(b.held <= a.held, "no more held at the end than the old way");
            Check(!b.toyFailed, "with the floors the console's 64 MiB meets every request");
        } else {
            // (a narrow window with two buffers: the old way ran out at 48 MiB and less here, both fit from 50 MiB up)
            Check(a.toyFailed, "the old way runs out of the tighter heap (the console's pattern)");
            Check(!b.toyFailed, "the floors do not");
        }
    }
    return g_fail;
}
