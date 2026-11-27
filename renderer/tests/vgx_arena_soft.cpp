// Soft growth of the vertex arena (renderer/src/vgx_arena.h, MSC_VGX_ARENA_SOFT), on the host.
// A capped heap stands in for the console's C heap: NpHostTryNew (which the platform provides in the game) refuses a block that does not fit
// beside what is held. Checked:
//   1. growth that fits: the vector holds the bytes it held, its capacity is the planned one, and nothing is left pending in the allocator;
//   2. growth that does not fit (also at the bare size of the request): VtxTryRoom says false and the vector is exactly as it was (size, capacity,
//      data pointer, bytes) -- the old behaviour was an abort;
//   3. a refused growth does not leak: after it the heap holds what it held;
//   4. a later growth, once room is given back, succeeds (the next frame tries again);
//   5. a request that fits in the capacity never asks the heap;
//   6. with the switch off (MSC_VGX_ARENA_SOFT=0) VtxTryRoom is the old VtxMakeRoom (it asks operator new, which here throws as a stand-in for the
//      game's abort) and the soft path is not taken.
// Run it twice: no argument (switch on) and "off" (as the harness sets MSC_VGX_ARENA_SOFT=0).
#include "vgx_arena.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

static size_t g_cap = 0, g_held = 0, g_asks = 0;
struct Blk { void* p; size_t n; };
static Blk g_blk[256];   // (a fixed table: a map would allocate through the operator new below)
static size_t g_nblocks = 0;
static bool g_throwOnNew = false;

extern "C" void* NpHostTryNew(size_t n) {
    ++g_asks;
    if (g_held + n > g_cap) return nullptr;
    void* p = std::malloc(n);
    if (p) { g_blk[g_nblocks++] = Blk{p, n}; g_held += n; }
    return p;
}
extern "C" void NpHostTryDelete(void* p) {
    if (!p) return;
    for (size_t i = 0; i < g_nblocks; ++i)
        if (g_blk[i].p == p) { g_held -= g_blk[i].n; g_blk[i] = g_blk[--g_nblocks]; break; }
    std::free(p);
}
extern "C" void NpPlatformNote(const char* line) { std::printf("  note: %s\n", line); }

// the allocator's operator new / delete go through the same capped heap, as the game's host blocks do
void* operator new(size_t n) {
    ++g_asks;
    if (g_throwOnNew) throw std::bad_alloc();
    void* p = NpHostTryNew(n);
    if (!p) throw std::bad_alloc();
    return p;
}
void operator delete(void* p) noexcept { NpHostTryDelete(p); }
void operator delete(void* p, size_t) noexcept { NpHostTryDelete(p); }

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

int main(int argc, char** argv) {
    const bool off = argc > 1 && !std::strcmp(argv[1], "off");
    if (off) setenv("MSC_VGX_ARENA_SOFT", "0", 1);
    using Vtx = std::vector<uint8_t, vgx::NoInitAlloc<uint8_t>>;
    const vgx::arena::Floors f = vgx::arena::MakeFloors(8192);   // 8 MiB floor
    const size_t MiB = 1u << 20;

    g_cap = 64 * MiB;
    Vtx v;
    vgx::arena::VtxMakeRoom(v, 8 * MiB, f);   // the floor, as the renderer reserves it at start-up
    v.resize(8 * MiB);
    for (size_t i = 0; i < v.size(); i += 4096) v[i] = static_cast<uint8_t>(i >> 12);
    const size_t held0 = g_held;

    if (off) {
        // 6. the soft path is not taken: the old growth asks operator new
        CHECK(!vgx::arena::SoftOn());
        g_cap = held0 + MiB;   // no room for the next step
        g_throwOnNew = true;
        bool threw = false;
        try { vgx::arena::VtxTryRoom(v, 10 * MiB, f); } catch (const std::bad_alloc&) { threw = true; }
        CHECK(threw);
        CHECK(vgx::arena::Soft().fails == 0);
        std::printf("vgx_arena_soft (off): %s\n", g_fail ? "FAIL" : "PASS");
        return g_fail ? 1 : 0;
    }
    CHECK(vgx::arena::SoftOn());

    // 5. fits in the capacity: no ask
    g_asks = 0;
    CHECK(vgx::arena::VtxTryRoom(v, 8 * MiB, f));
    CHECK(g_asks == 0);

    // 1. growth that fits
    g_cap = 64 * MiB;
    CHECK(vgx::arena::VtxTryRoom(v, 9 * MiB, f));
    CHECK(v.capacity() >= 9 * MiB && v.capacity() <= 8 * MiB + 2 * MiB + 65536);
    CHECK(v.size() == 8 * MiB);
    bool same = true;
    for (size_t i = 0; i < 8 * MiB; i += 4096) same = same && v[i] == static_cast<uint8_t>(i >> 12);
    CHECK(same);
    CHECK(vgx::NoInitPendingBlock().block == nullptr);
    CHECK(g_nblocks == 1);   // the old block was given back by the vector

    // 2. growth that does not fit, step and bare size alike
    const size_t held1 = g_held, cap1 = v.capacity(), size1 = v.size();
    const uint8_t* data1 = v.data();
    g_cap = held1 + MiB / 2;   // half a MiB free: nothing of 1 MiB+ fits, not even the bare request
    CHECK(!vgx::arena::VtxTryRoom(v, cap1 + 3 * MiB, f));
    CHECK(v.capacity() == cap1 && v.size() == size1 && v.data() == data1);
    same = true;
    for (size_t i = 0; i < 8 * MiB; i += 4096) same = same && v[i] == static_cast<uint8_t>(i >> 12);
    CHECK(same);
    // 3. no leak, nothing pending
    CHECK(g_held == held1);
    CHECK(vgx::NoInitPendingBlock().block == nullptr);
    CHECK(vgx::arena::Soft().fails == 1);
    vgx::arena::SoftNoteDrop(7);
    vgx::arena::SoftNoteDrop(7);   // the line is written once
    CHECK(vgx::arena::Soft().dropsTotal == 2 && vgx::arena::Soft().dropsThisFrame == 2);

    // a step that does not fit but the bare request does: taken at the bare size
    g_cap = held1 + (cap1 + 1 * MiB) - cap1 + 2 * MiB;   // room for a block of cap1 + 3 MiB... but not the 2 MiB step on top of cap1 plus the bare
    g_cap = held1 + (cap1 + MiB / 4);                      // room for (cap1 + 256 KiB) only
    const size_t need = cap1 + MiB / 8;
    CHECK(vgx::arena::VtxTryRoom(v, need, f));
    CHECK(v.capacity() >= need && v.capacity() < cap1 + MiB / 2);
    CHECK(v.size() == size1);

    // 4. room again: a later growth works
    g_cap = 64 * MiB;
    CHECK(vgx::arena::VtxTryRoom(v, 14 * MiB, f));
    CHECK(v.capacity() >= 14 * MiB);
    same = true;
    for (size_t i = 0; i < 8 * MiB; i += 4096) same = same && v[i] == static_cast<uint8_t>(i >> 12);
    CHECK(same);

    // the simulated refusal of the harness (MSC_VGX_ARENA_SIM) is read once; not exercised here (the environment is read at first use)
    v = Vtx();
    CHECK(g_nblocks == 0 && g_held == 0);
    std::printf("vgx_arena_soft: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
