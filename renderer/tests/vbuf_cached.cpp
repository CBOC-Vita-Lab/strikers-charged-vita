// vbuf15: the cached stream arena of vitaGL (vgl.c, "GLB: cached stream arena": reserve / flush / touch / frame end / next slice) against a fake data cache.
// The arena code under test is the real text of vgl.c (cut out between its two markers by run_vbuf_cached_test.sh into arena_under_test.inc, no copy kept here); only the sceKernel/sceGxm
// calls and the pool variables around it are shims. The fake cache: CPU writes land in `cpu` and mark their 64 byte lines dirty; a write-back (stream_flush_fn or sceKernelSyncVMDomain)
// copies the dirty lines it touches to `ram`; the "GPU" reads `ram`. A draw reads a buffer's bytes from ram and must see what the CPU wrote.
//   run_vbuf_cached_test.sh builds the real text and the mutants (each must fail).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <random>

typedef uint8_t GLboolean;
#define GL_FALSE 0
#define GL_TRUE 1
#define DISPLAY_MAX_BUFFER_COUNT 4
typedef int SceUID;
typedef int SceKernelMemBlockType;
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW 1
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE 2
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RW 3
#define SCE_GXM_MEMORY_ATTRIB_READ 1
typedef void (*vglCacheFlushFn)(void *addr, uint32_t size);

static const uint32_t kLine = 64;
static std::vector<uint8_t> g_cpu, g_ram, g_pool;       // the arena as the CPU sees it, as RAM (the GPU) holds it, and the spill pool (uncached: both views at once)
static std::vector<uint8_t> g_dirty;                    // one byte per line
static uint8_t* g_base = nullptr;                       // 4 KiB aligned like a memblock (the arena code rounds addresses to lines)
static size_t g_size = 0;
static int g_circular_idx_dummy = 0;
int vgl_circular_idx = 0;
uint32_t vgl_glb_pool_peak = 0, vgl_glb_pool_frame_peak = 0, vgl_glb_ring_peak = 0, vgl_glb_ring_frame_bytes = 0, vgl_glb_ring_frame_peak = 0;
uint8_t* circular_data_pool_ptr[DISPLAY_MAX_BUFFER_COUNT];
uint8_t* circular_data_pool[DISPLAY_MAX_BUFFER_COUNT];
GLboolean vgl_stream_wants_scratch = GL_FALSE;
static size_t g_spillOff = 0;
static int g_spills = 0;

#define vgl_log(...) ((void)0)
static SceUID sceKernelAllocMemBlock(const char*, SceKernelMemBlockType, uint32_t size, void*) {
    g_size = size; g_cpu.assign(size + 4096, 0xEE); g_ram.assign(size + 4096, 0xEE); g_dirty.assign(size / kLine + 2, 0);
    g_base = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(g_cpu.data()) + 4095) & ~uintptr_t(4095));
    return 7;
}
static int sceKernelGetMemBlockBase(SceUID, void** base) { *base = g_base; return 0; }
static void sceGxmMapMemory(void*, uint32_t, int) {}
static int g_syncCalls = 0;
static void writeback(void* addr, uint32_t n);
static int sceKernelSyncVMDomain(SceUID, void* addr, uint32_t n) { ++g_syncCalls; writeback(addr, n); return 0; }
static uint8_t* vgl_reserve_data_pool(uint32_t size) {   // the uncached circular pool: no cache in between
    ++g_spills;
    if (g_spillOff + size > g_pool.size()) g_spillOff = 0;   // wrap (the pool is only a sink; every frame's buffers are checked before it can wrap onto them)
    uint8_t* p = g_pool.data() + g_spillOff; g_spillOff += size; return p;
}

// the fake cache
static void writeback(void* addr, uint32_t n) {
    const size_t off = static_cast<uint8_t*>(addr) - g_base;
    if (static_cast<uint8_t*>(addr) < g_base || off + n > g_size) { std::printf("FAIL: write-back outside the arena\n"); std::exit(1); }
    for (size_t l = off / kLine; l <= (off + n - 1) / kLine; ++l) {
        if (g_dirty[l]) { std::memcpy(&g_ram[g_base - g_cpu.data() + l * kLine], g_base + l * kLine, kLine); g_dirty[l] = 0; }
    }
}
static void cpuWrite(uint8_t* dst, const uint8_t* src, uint32_t n) {
    if (dst >= g_base && dst < g_base + g_size) {
        std::memcpy(dst, src, n);
        const size_t off = dst - g_base;
        for (size_t l = off / kLine; l <= (off + n - 1) / kLine; ++l) g_dirty[l] = 1;
    } else std::memcpy(dst, src, n);          // pool memory
}
static const uint8_t* gpuRead(const uint8_t* p) { return (p >= g_base && p < g_base + g_size) ? &g_ram[g_base - g_cpu.data() + (p - g_base)] : p; }

#include "arena_under_test.inc"

// what a draw entry does in vitaGL (shared.h VGL_STREAM_FLUSH_IF_NEEDED)
#ifndef VBUF_MUTANT_DRAW_NO_FLUSH
#define DRAW_ENTRY() do { if (vgl_stream_unflushed) vgl_stream_flush(); } while (0)
#else
#define DRAW_ENTRY() do { } while (0)
#endif

struct Buf { uint8_t* ptr; uint32_t size; std::vector<uint8_t> want; };
static int g_fail = 0;
static int g_run = 0;
static void check(bool ok, const char* what, int frame) { if (!ok && g_fail++ < 5) std::printf("FAIL (run %d, frame %d): %s\n", g_run, frame, what); }
static void drawBuf(const Buf& b, int frame, const char* what) {
    DRAW_ENTRY();
    const bool same = std::memcmp(gpuRead(b.ptr), b.want.data(), b.size) == 0;
    if (!same && g_fail < 2) { uint32_t k = 0; while (gpuRead(b.ptr)[k] == b.want[k]) ++k; std::printf("  buffer at +%ld size %u: first bad byte %u (has %02x wants %02x)\n", (long)(b.ptr - g_base), b.size, k, gpuRead(b.ptr)[k], b.want[k]); }
    check(same, what, frame);
}

// fault injection of the cache: a line may be written back by itself at any time (eviction); that must never matter
static std::mt19937 rng(1234);
static void evictSome() {
    for (int i = 0; i < 3; ++i) {
        const size_t l = rng() % (g_size / kLine);
        if (g_dirty[l]) { std::memcpy(&g_ram[g_base - g_cpu.data() + l * kLine], g_base + l * kLine, kLine); g_dirty[l] = 0; }
    }
}

static uint64_t g_reservedBytes = 0;
static void runOne(int memMode, bool useCustomFlush, uint32_t sliceReq, int frames, bool rewrites = true) {
    ++g_run;
    g_spillOff = 0; g_spills = 0; g_syncCalls = 0; vgl_circular_idx = 0;
    stream_uid = -1; vgl_stream_cached = GL_FALSE; vgl_stream_unflushed = GL_FALSE;
    vglSetupStreamArena(sliceReq, memMode, useCustomFlush ? +[](void* a, uint32_t n) { writeback(a, n); } : nullptr);
    const int slices = 3;
    vgl_stream_init(slices);
    check(vgl_stream_cached, "arena up", 0);
    for (int f = 0; f < frames; ++f) {
        vgl_circular_idx = f % slices;
        std::vector<Buf> bufs;
        const int n = 1 + rng() % 30;
        for (int i = 0; i < n; ++i) {
            int op = rng() % 10;
            if (!rewrites && op >= 5 && op < 8) op = 9;
            if (op < 5 || bufs.empty()) {                              // glBufferData(GL_STREAM_DRAW, data): reserve and copy (sizes from 2 bytes of indices to 40 KB of vertices)
                const uint32_t sz = (rng() % 4 == 0) ? 2 + rng() % 600 : 64 + rng() % 40000;
                Buf b; b.size = sz; b.ptr = vgl_reserve_stream_pool(sz); b.want.resize(sz);
                if (b.ptr >= g_base && b.ptr < g_base + g_size) g_reservedBytes += (sz + 63u) & ~63u;
                for (auto& x : b.want) x = static_cast<uint8_t>(rng());
                cpuWrite(b.ptr, b.want.data(), sz);
                bufs.push_back(b);
            } else if (op < 7) {                                       // glBufferSubData in place into an older buffer (after a draw may have flushed it), through vgl_stream_touch
                Buf& b = bufs[rng() % bufs.size()];
                const uint32_t off = rng() % b.size, len = 1 + rng() % (b.size - off);
                for (uint32_t k = 0; k < len; ++k) b.want[off + k] = static_cast<uint8_t>(rng());
                vgl_stream_touch(b.ptr + off);
                cpuWrite(b.ptr + off, &b.want[off], len);
            } else if (op < 8) {                                       // glMapBuffer (patch 0054: touch at map) write, glUnmapBuffer (touch again)
                Buf& b = bufs[rng() % bufs.size()];
                vgl_stream_touch(b.ptr);
                const uint32_t len = 1 + rng() % b.size;
                for (uint32_t k = 0; k < len; ++k) b.want[k] = static_cast<uint8_t>(rng());
                cpuWrite(b.ptr, b.want.data(), len);
                vgl_stream_touch(b.ptr);
            } else {                                                   // draws, most recent or any buffer of the frame
                const int nd = 1 + rng() % 4;
                for (int d = 0; d < nd; ++d) drawBuf(bufs[(rng() % 2) ? bufs.size() - 1 : rng() % bufs.size()], f, "draw read stale or unwritten bytes");
            }
            if (rng() % 3 == 0) evictSome();
        }
        // the last draws of the frame read every buffer; then the frame ends (flush at frame end, next slice)
        for (auto& b : bufs) drawBuf(b, f, "last draw of the frame read stale bytes");
        vgl_glb_frame_end();
        vgl_circular_idx = (f + 1) % slices;   // gxm.c: the index moves on, then the next slice starts empty
        vgl_stream_next_slice();
    }
}

int main() {
    // spill pool rows need circular_data_pool_ptr set for frame_end's peak lines
    for (int i = 0; i < DISPLAY_MAX_BUFFER_COUNT; ++i) circular_data_pool_ptr[i] = circular_data_pool[i] = g_pool.data();
    g_pool.resize(8u << 20);
    for (int i = 0; i < DISPLAY_MAX_BUFFER_COUNT; ++i) circular_data_pool_ptr[i] = circular_data_pool[i] = g_pool.data();
    runOne(0, true, 2048 * 1024, 60);          // cached, write-back by kubridge-like function
    runOne(0, false, 2048 * 1024, 60);         // cached, write-back by sceKernelSyncVMDomain
    check(g_syncCalls > 0, "the SyncVMDomain path ran", -1);
    // each byte reserved is written back exactly once when nothing is rewritten (a flush that forgets what it did writes back again and again: correct but slow)
    g_reservedBytes = 0; stream_flushed_bytes = 0;
    runOne(0, true, 2048 * 1024, 40, false);
    check(stream_flushed_bytes == g_reservedBytes, "no rewrites: every reserved byte written back once", -1);
    runOne(0, true, 96 * 1024, 60);            // small slice: frames overrun into the pool (the spill is uncached, correct, counted)
    check(g_spills > 0, "the overrun path ran", -1);
    check(stream_overruns > 0, "overruns counted", -1);
    // the reservation is 64 byte aligned and the slices do not overlap
    {
        runOne(0, true, 256 * 1024, 1);
        uint8_t* a = vgl_reserve_stream_pool(3); uint8_t* b = vgl_reserve_stream_pool(3);
        check(((uintptr_t)a & 63) == 0 && ((uintptr_t)b & 63) == 0 && b - a == 64, "reservations are whole lines", -1);
    }
    std::printf("%s\n", g_fail ? "FAILED" : "vbuf_cached ok");
    return g_fail ? 1 : 0;
}
