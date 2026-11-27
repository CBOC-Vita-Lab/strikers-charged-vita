// cache25 (vitaGL 0060): the cached stream arena in SCENE mode (one write-back per scene submit); based on vbuf_cached.cpp, the vbuf15 test, whose fake cache and shims it keeps.
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
#include "macros.inc"   // the two macros of shared.h (VGL_STREAM_FLUSH_IF_NEEDED, VGL_STREAM_SCENE_SUBMIT), cut out of the real text by run_stream_scene_test.sh
#define DRAW_ENTRY() do { VGL_STREAM_FLUSH_IF_NEEDED() } while (0)
#define SCENE_SUBMIT() do { VGL_STREAM_SCENE_SUBMIT() } while (0)

// The model. A scene records draws (buffer references); the draw entry macro runs per draw; the scene submit macro runs before the "sceGxmEndScene". The GPU reads the RAM view of every buffer a scene
// drew, `latency` frames after the submit (0 = at the submit; 2 = the last scene of a frame is read two frames later, like a swap chain of three). What it reads must be what the CPU wrote.
struct Buf { uint8_t* ptr; uint32_t size; std::vector<uint8_t> want; int slice; };
struct Job { int frame; int slice; std::vector<Buf> bufs; };
static int g_fail = 0, g_run = 0;
static void check(bool ok, const char* what, int frame) { if (!ok && g_fail++ < 6) std::printf("FAIL (run %d, frame %d): %s\n", g_run, frame, what); }
static std::mt19937 rng(4321);
static void evictSome() {
    for (int i = 0; i < 4; ++i) {
        const size_t l = rng() % (g_size / kLine);
        if (g_dirty[l]) { std::memcpy(&g_ram[g_base - g_cpu.data() + l * kLine], g_base + l * kLine, kLine); g_dirty[l] = 0; }
    }
}
static std::vector<Job> g_pending;
static int g_slicePending[8];
static uint64_t g_reservedBytes = 0, g_gpuReads = 0, g_draws = 0;
static void gpuRun(const Job& j) {
    for (const Buf& b : j.bufs) {
        ++g_gpuReads;
        const bool same = std::memcmp(gpuRead(b.ptr), b.want.data(), b.size) == 0;
        check(same, "the GPU read stale or unwritten bytes of a submitted scene", j.frame);
    }
    --g_slicePending[j.slice];
}
static bool linesClean(const uint8_t* from, const uint8_t* to) {   // no dirty (CPU-only) line left in [from, to)
    for (size_t l = (from - g_base) / kLine; l * kLine < static_cast<size_t>(to - g_base); ++l) if (g_dirty[l]) return false;
    return true;
}

static void runOne(int latency, int slices, int frames, bool rewriteOld, bool sceneMode = true, bool useFn = true) {
    ++g_run;
    g_spillOff = 0; g_spills = 0; g_syncCalls = 0; vgl_circular_idx = 0; g_pending.clear();
    for (int& c : g_slicePending) c = 0;
    stream_uid = -1; vgl_stream_cached = GL_FALSE; vgl_stream_unflushed = GL_FALSE; vgl_stream_scene_mode = sceneMode ? GL_TRUE : GL_FALSE;
    stream_flushes = stream_flushed_bytes = 0; vgl_stream_scene_flushes = 0; g_reservedBytes = 0;
    vglSetupStreamArena(2048 * 1024, 0, useFn ? +[](void* a, uint32_t n) { writeback(a, n); } : nullptr);
    vgl_stream_init(slices);
    check(vgl_stream_cached, "arena up", 0);
    uint64_t expectFlushes = 0, drawFlushesBefore = 0;
    (void)drawFlushesBefore;
    for (int f = 0; f < frames; ++f) {
        vgl_circular_idx = f % slices;
        // the GPU finishes the scenes that are due (the swap's fence): everything older than `latency` frames
        for (size_t i = 0; i < g_pending.size();) if (f - g_pending[i].frame >= latency && latency > 0) { gpuRun(g_pending[i]); g_pending.erase(g_pending.begin() + i); } else ++i;
        check(stream_ptr[vgl_circular_idx] == stream_base[vgl_circular_idx] && stream_flushed[vgl_circular_idx] == stream_base[vgl_circular_idx], "a slice does not start empty", f);
        // the fence rule: a slice is reserved into again only when nothing of its last use is still to be read
        check(g_slicePending[vgl_circular_idx] == 0, "a slice was reused while the GPU still had to read it", f);
        std::vector<Buf> all;   // buffers of the frame
        const int scenes = 1 + rng() % 6;
        bool dirtySinceFlush = false;
        for (int sc = 0; sc < scenes; ++sc) {
            Job job; job.frame = f; job.slice = vgl_circular_idx;
            std::vector<size_t> drawn;
            const int n = 1 + rng() % 14;
            std::vector<Buf> created; created.reserve(64);
            for (int i = 0; i < n; ++i) {
                int op = rng() % 10;
                if (op < 5 || all.empty()) {
                    const uint32_t sz = (rng() % 4 == 0) ? 2 + rng() % 600 : 64 + rng() % 40000;
                    Buf b; b.size = sz; b.slice = vgl_circular_idx; b.ptr = vgl_reserve_stream_pool(sz); b.want.resize(sz);
                    for (auto& x : b.want) x = static_cast<uint8_t>(rng());
                    cpuWrite(b.ptr, b.want.data(), sz);
                    if (b.ptr >= g_base && b.ptr < g_base + g_size) { g_reservedBytes += (sz + 63u) & ~63u; dirtySinceFlush = true; }
                    all.push_back(b);
                } else if (op < 7 && rewriteOld) {          // glBufferSubData in place into any older buffer of the frame (only in runs where the GPU has read every submitted scene already)
                    Buf& b = all[rng() % all.size()];
                    const uint32_t off = rng() % b.size, len = 1 + rng() % (b.size - off);
                    for (uint32_t k = 0; k < len; ++k) b.want[off + k] = static_cast<uint8_t>(rng());
                    vgl_stream_touch(b.ptr + off);
                    cpuWrite(b.ptr + off, &b.want[off], len);
                    dirtySinceFlush = dirtySinceFlush || (b.ptr >= g_base && b.ptr < g_base + g_size);
                } else if (op < 8 && rewriteOld) {          // map write + unmap (0054: touch at both)
                    Buf& b = all[rng() % all.size()];
                    vgl_stream_touch(b.ptr);
                    const uint32_t len = 1 + rng() % b.size;
                    for (uint32_t k = 0; k < len; ++k) b.want[k] = static_cast<uint8_t>(rng());
                    cpuWrite(b.ptr, b.want.data(), len);
                    vgl_stream_touch(b.ptr);
                    dirtySinceFlush = dirtySinceFlush || (b.ptr >= g_base && b.ptr < g_base + g_size);
                } else {                                    // draws: the draw entry runs, nothing is read until the submit
                    const int nd = 1 + rng() % 4;
                    for (int d = 0; d < nd; ++d) {
                        const uint64_t before = stream_flushes;
                        DRAW_ENTRY();
                        if (sceneMode) check(stream_flushes == before, "a draw entry wrote back in scene mode", f);
                        else if (vgl_stream_unflushed == GL_FALSE && dirtySinceFlush) dirtySinceFlush = false;
                        ++g_draws;
                        drawn.push_back((rng() % 2) ? all.size() - 1 : rng() % all.size());
                    }
                }
                if (rng() % 3 == 0) evictSome();
            }
            if (drawn.empty()) drawn.push_back(all.size() - 1);   // every scene draws something
            for (size_t k : drawn) job.bufs.push_back(all[k]);   // what the GPU must see: the bytes as written up to the submit
            // sceGxmEndScene: the macro first
            const uint64_t fl = stream_flushes;
            SCENE_SUBMIT();
            if (sceneMode && dirtySinceFlush) ++expectFlushes;
            if (sceneMode) { check(stream_flushes - fl == (dirtySinceFlush ? 1u : 0u), "the scene submit wrote back once (or not at all with nothing new)", f);
                             check(linesClean(stream_base[vgl_circular_idx], stream_ptr[vgl_circular_idx]), "a dirty line is left in the used range after the submit (clean does not cover every written byte)", f); }
            dirtySinceFlush = false;
            // the buffers the scene's draws read must not be rewritten while still unread (the circular pool's own rule): with latency this run does no rewrites
            if (latency == 0) gpuRun(Job{f, vgl_circular_idx, job.bufs}), (void)0;
            else { ++g_slicePending[vgl_circular_idx]; g_pending.push_back(job); }
            if (latency == 0) ++g_slicePending[vgl_circular_idx];   // balanced by gpuRun's decrement
        }
        vgl_glb_frame_end();
        vgl_circular_idx = (f + 1) % slices;
        vgl_stream_next_slice();
    }
    for (Job& j : g_pending) gpuRun(j);
    g_pending.clear();
    if (sceneMode) check(vgl_stream_scene_flushes == expectFlushes, "the scene-submit counter counts scenes with new bytes only", -1);
    if (sceneMode) check(stream_flushes == expectFlushes, "write-back calls are one per scene that had new bytes (none for draws)", -1);
    if (sceneMode && !rewriteOld) check(stream_flushed_bytes == g_reservedBytes, "no rewrites: every reserved byte written back exactly once", -1);
}

int main() {
    g_pool.resize(8u << 20);
    for (int i = 0; i < DISPLAY_MAX_BUFFER_COUNT; ++i) circular_data_pool_ptr[i] = circular_data_pool[i] = g_pool.data();
    runOne(0, 3, 60, true);        // the GPU reads at the submit; in-place rewrites of older buffers (touch) must reach the next scene
    runOne(0, 3, 60, false);       // exact-once accounting
    runOne(1, 3, 60, false);       // one frame of latency
    runOne(2, 3, 60, false);       // two frames (a swap chain of three): the slice rule holds
    runOne(2, 4, 60, false);
    runOne(0, 3, 40, true, true, false);   // write-back by sceKernelSyncVMDomain
    runOne(2, 3, 40, false, true, false);
    check(g_syncCalls > 0, "the SyncVMDomain path ran", -1);
    runOne(0, 3, 40, true, false); // the per-draw mode (0054) is unchanged: draws write back, the model reads at the submit
    std::printf("%s (GPU reads %llu, draws %llu)\n", g_fail ? "FAILED" : "stream_scene ok", (unsigned long long)g_gpuReads, (unsigned long long)g_draws);
    return g_fail ? 1 : 0;
}
