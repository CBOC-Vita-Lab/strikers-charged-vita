// nxboot25 R2-9: the vertex arenas in memory blocks of their own (renderer/src/vgx_cmd.h NoInitAlloc, MSC_VGX_VTX_BLOCK), on the host with a capped C heap and a fake block provider.
//   vgx_vtx_block on|off [fail]       (MSC_VGX_VTX_BLOCK is set here; "fail": the provider refuses every block)
// Model of the console (6124.log): the C heap holds 44.6 MiB of other things (76.9 MiB in use with the two 16 MiB vertex arenas, 2 x 16 MiB = "vertex arenas 32768 KiB"); the numbers printed are
// the C heap's in use and peak with the switch on and off: on, the heap holds 32 MiB less and the provider holds them (same total).
#include "vgx_arena.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

static const size_t MiB = 1u << 20;
static size_t g_heapHeld = 0, g_heapPeak = 0, g_blockHeld = 0, g_blockCalls = 0, g_blockFrees = 0, g_blockMax = 0;
struct Blk { void* p; size_t n; };
static Blk g_heap[512];
static size_t g_nheap = 0;
static Blk g_blocks[16];
static size_t g_nblocks = 0;
static bool g_refuse = false;
static void Note(size_t* peak, size_t v) { if (v > *peak) *peak = v; }

static void* HeapNew(size_t n) {
    void* p = std::malloc(n);
    if (!p) return nullptr;
    g_heap[g_nheap++] = Blk{p, n};
    g_heapHeld += n; Note(&g_heapPeak, g_heapHeld);
    return p;
}
static void HeapDelete(void* p) {
    if (!p) return;
    for (size_t i = 0; i < g_nheap; ++i) if (g_heap[i].p == p) { g_heapHeld -= g_heap[i].n; g_heap[i] = g_heap[--g_nheap]; std::free(p); return; }
    std::printf("FAIL: heap delete of a pointer the heap never gave (a block freed as heap?)\n"); std::exit(1);
}
extern "C" void* NpHostTryNew(size_t n) { return HeapNew(n); }
extern "C" void NpHostTryDelete(void* p) { HeapDelete(p); }
extern "C" void NpPlatformNote(const char*) {}
extern "C" void* NpHostBlockNew(size_t n) {
    ++g_blockCalls;
    if (g_refuse || g_nblocks == 16) return nullptr;
    void* p = std::malloc(n);
    g_blocks[g_nblocks++] = Blk{p, n};
    g_blockHeld += n; Note(&g_blockMax, g_blockHeld);
    return p;
}
extern "C" int NpHostBlockFree(void* p) {
    for (size_t i = 0; i < g_nblocks; ++i) if (g_blocks[i].p == p) { g_blockHeld -= g_blocks[i].n; g_blocks[i] = g_blocks[--g_nblocks]; ++g_blockFrees; std::free(p); return 1; }
    return 0;
}
void* operator new(size_t n) { void* p = HeapNew(n); if (!p) throw std::bad_alloc(); return p; }
void operator delete(void* p) noexcept { HeapDelete(p); }
void operator delete(void* p, size_t) noexcept { HeapDelete(p); }

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)
using Vtx = std::vector<uint8_t, vgx::NoInitAlloc<uint8_t>>;
using Idx = std::vector<uint16_t, vgx::NoInitAlloc<uint16_t>>;

int main(int argc, char** argv) {
    const bool on = argc > 1 && !std::strcmp(argv[1], "on");
    g_refuse = argc > 2 && !std::strcmp(argv[2], "fail");
    setenv("MSC_VGX_VTX_BLOCK", on ? "1" : "0", 1);
    const vgx::arena::Floors f = vgx::arena::MakeFloors(16384);
    // the rest of the heap: 44.6 MiB in many pieces, held to the end
    std::vector<void*> other;
    for (int i = 0; i < 446; ++i) other.push_back(HeapNew(100 * 1024));
    const size_t base = g_heapHeld;
    {
        Vtx a, b;
        Idx idx;
        vgx::arena::VtxMakeRoom(a, 1 * MiB, f);   // the renderer's start: both buffers at their floors
        vgx::arena::VtxMakeRoom(b, 1 * MiB, f);
        a.resize(12 * MiB); b.resize(3 * MiB);
        for (size_t i = 0; i < a.size(); i += 4096) a[i] = static_cast<uint8_t>(i >> 12);
        idx.reserve(3 * MiB);                      // 6 MiB of uint16: never a block (the switch is for the vertex arena's byte vectors)
        const size_t heapAfter = g_heapHeld - base;
        std::printf("vertex arenas at their floors: C heap +%zu KiB, blocks %zu KiB (switch %s%s)\n", heapAfter >> 10, g_blockHeld >> 10, on ? "on" : "off", g_refuse ? ", provider refuses" : "");
        if (on && !g_refuse) {
            CHECK(g_blockHeld == 32 * MiB && g_nblocks == 2);
            CHECK(heapAfter == 6 * MiB);           // only the index vector
        } else {
            CHECK(g_blockHeld == 0 && g_nblocks == 0);
            CHECK(heapAfter == 38 * MiB);
        }
        CHECK(on || g_blockCalls == 0);            // switch off: the provider is never asked
        CHECK(a.capacity() == 16 * MiB && b.capacity() == 16 * MiB);
        // growth past the floor (the soft path: a heap block first, the vector's reserve takes it; the old arena goes back where it came from)
        const size_t blocksBefore = g_nblocks, frees0 = g_blockFrees;
        CHECK(vgx::arena::VtxTryRoom(a, 20 * MiB, f));
        CHECK(a.capacity() >= 20 * MiB);
        a.resize(20 * MiB);
        bool same = true;
        for (size_t i = 0; i < 12 * MiB; i += 4096) same = same && a[i] == static_cast<uint8_t>(i >> 12);
        CHECK(same);                               // the bytes moved with it
        if (on && !g_refuse) CHECK(g_nblocks == blocksBefore - 1 && g_blockFrees == frees0 + 1);   // a's old block is freed as a block, once
        // a small arena (under 4 MiB) never asks for a block
        const size_t calls = g_blockCalls;
        { Vtx small; small.reserve(3 * MiB); small.reserve(3 * MiB + 4096); }
        CHECK(g_blockCalls == calls);
        // swap: the circulating buffers trade places, nothing is allocated or freed
        const size_t h = g_heapHeld, bl = g_blockHeld;
        a.swap(b);
        CHECK(g_heapHeld == h && g_blockHeld == bl);
        // a copy that grows from the heap and a move
        Vtx c(std::move(b));
        CHECK(g_heapHeld == h && g_blockHeld == bl);
    }
    // every block back, nothing freed twice (HeapDelete stops the run on a pointer it never gave), no leak
    CHECK(g_nblocks == 0 && g_blockHeld == 0);
    CHECK(g_heapHeld == base);
    std::printf("C heap peak in this model: %zu KiB (other %zu KiB); blocks peak %zu KiB; block calls %zu, frees %zu\n", g_heapPeak >> 10, base >> 10, g_blockMax >> 10, g_blockCalls, g_blockFrees);
    for (void* p : other) HeapDelete(p);
    std::printf("vgx_vtx_block (%s%s): %s\n", on ? "on" : "off", g_refuse ? ", refused" : "", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
