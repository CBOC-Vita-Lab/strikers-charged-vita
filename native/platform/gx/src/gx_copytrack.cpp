// GX glue, part 4: guest-memory notifications and the bookkeeping of EFB copy textures. Standard headers allowed
// (no SDK headers in this unit).
//
// An EFB copy (GXCopyTex) leaves its result GPU-side; the renderer keys that texture by the destination address.
// When the game later reuses the destination memory (it flushes the data cache over bytes it wrote there), the GPU
// texture is stale and must not be found by a texture bind of the new contents: the copy is retired
// (GXDestroyCopyTex). The data-cache calls of the SDK layer (DCFlushRange, DCStoreRange, DCInvalidateRange,
// DCFlushRangeNoSync, disc reads) report here (np_gx_notify_write). This is the native equivalent of the
// recompiled build's RememberEfbCopyDestination / InvalidateEfbCopyDestinationsForRange (hle/gx/gx_copy.cpp).
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#if defined(__GLIBC__)
#include <execinfo.h>
#endif

#include "np_gx.h"
#include "vgx_native.h"
#include "gx_internal.h"

namespace {

// The map's nodes come from malloc, not from the game heap (operator new is the game's allocator in the native build): RememberCopy runs inside the frame-send job on the
// worker (GXCopyTex), and a worker that allocates from the game heap would take its blocks at times the game thread's own allocations do not see (docs/wave5-PIPE.md 1.6).
template <class T>
struct MallocAllocator {
    typedef T value_type;
    MallocAllocator() {}
    template <class U> MallocAllocator(const MallocAllocator<U>&) {}
    T* allocate(size_t n) {
        void* p = std::malloc(n * sizeof(T));
        if (!p) std::abort();
        return static_cast<T*>(p);
    }
    void deallocate(T* p, size_t) { std::free(p); }
    template <class U> bool operator==(const MallocAllocator<U>&) const { return true; }
    template <class U> bool operator!=(const MallocAllocator<U>&) const { return false; }
};

std::mutex g_mtx;
std::map<uintptr_t, uint32_t, std::less<uintptr_t>, MallocAllocator<std::pair<const uintptr_t, uint32_t> > > g_copies;   // destination -> bytes
uint32_t g_largest = 0;                   // largest destination ever remembered
std::vector<uintptr_t> g_retired;         // destinations to destroy on the game thread
std::atomic<uint32_t> g_retiredCount{0};
std::atomic<std::thread::id> g_gameThread{};

// Every 4 KiB page any copy ever covered, hashed into 65536 bits: a notification over pages none of which is set
// cannot overlap a copy, so the map is not even looked at (hundreds of DCStoreRange calls a frame, almost all
// over memory that never held an EFB copy). Bits are only set.
std::atomic<uint32_t> g_bits[65536 / 32];
inline uint32_t Hash(uintptr_t page) { return static_cast<uint32_t>((page * 2654435761u) >> 16) & 0xFFFFu; }
void AddPages(uintptr_t a, uint32_t n) {
    for (uintptr_t p = a >> 12, e = (a + n - 1) >> 12; p <= e; ++p) {
        const uint32_t h = Hash(p);
        g_bits[h >> 5].fetch_or(1u << (h & 31), std::memory_order_relaxed);
    }
}
bool MayOverlap(uintptr_t a, uint32_t n) {
    for (uintptr_t p = a >> 12, e = (a + n - 1) >> 12; p <= e; ++p) {
        const uint32_t h = Hash(p);
        if (g_bits[h >> 5].load(std::memory_order_relaxed) & (1u << (h & 31))) return true;
    }
    return false;
}

} // namespace

namespace np_gx {

void ResetCopyTracking() {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_copies.clear();
    g_retired.clear();
    g_retiredCount.store(0);
    g_largest = 0;
    g_gameThread.store(std::this_thread::get_id());
}

void RememberCopy(void* dest, unsigned int size) {
    if (!dest || !size) return;
    std::lock_guard<std::mutex> lk(g_mtx);
    AddPages(reinterpret_cast<uintptr_t>(dest), size);
    g_copies[reinterpret_cast<uintptr_t>(dest)] = size;
    if (size > g_largest) g_largest = size;
}

void DrainRetiredCopies() {
    if (g_retiredCount.load(std::memory_order_acquire) == 0) return;
    std::vector<uintptr_t> list;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        list.swap(g_retired);
        g_retiredCount.store(0, std::memory_order_release);
    }
    // FIFO order: the destroy record goes in before any later texture load that can consume the new bytes.
    for (uintptr_t d : list) GXDestroyCopyTex(reinterpret_cast<void*>(d));
}

} // namespace np_gx

// The frame-send worker (platform/vita/src/np_framesend.cpp) records GX calls on a second core while the game thread runs the next
// simulation step; retiring an EFB copy is a recorder call (GXDestroyCopyTex), so a flush over a copy destination waits for a pending send.
extern "C" {
extern volatile int NpFsBusy __attribute__((weak));
void NpFsGateSlow(const char* why) __attribute__((weak));
}

extern "C" void np_gx_notify_write(const void* p, unsigned int n) {
    if (!n) return;
    vgxn_notify_write(p, n);
    const uintptr_t start = reinterpret_cast<uintptr_t>(p), end = start + n;
    if (!MayOverlap(start, n)) return;
    if (&NpFsBusy != nullptr && NpFsBusy != 0) NpFsGateSlow("texinval");   // (a no-op on the worker itself)
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        auto it = g_copies.lower_bound(start > g_largest ? start - g_largest : 0);
        while (it != g_copies.end() && it->first < end) {
            if (it->first + it->second <= start) { ++it; continue; }
            g_retired.push_back(it->first);
            it = g_copies.erase(it);
        }
        g_retiredCount.store(static_cast<uint32_t>(g_retired.size()), std::memory_order_release);
    }
    // The recorder has one producer, the game thread: from another thread the retirement waits for its next GX call.
    if (std::this_thread::get_id() == g_gameThread.load()) np_gx::DrainRetiredCopies();
}

namespace np_gx {
// GXInit starts the renderer when the platform code has not; if that fails every later call would block or draw nothing
// (the recorder waits for a player that does not exist): stop with the reason instead of hanging in the first frame.
void StartFailed() {
    std::fprintf(stderr, "np_gx: the renderer failed to start (see the [vgx] lines above); GXInit cannot continue.\n"
                         "np_gx: host32: run the program through vgx/native/test/run32.sh (private 32-bit Mesa); armhf / Vita: link the matching library.\n");
    std::fflush(stderr);
    std::abort();
}
} // namespace np_gx

// Diagnostic (LOOK): the call stack of GXBegin calls with a given vertex count in a range of presented frames (glue GXBegin / GXCopyDisp).
namespace np_gx {
namespace {
unsigned g_tfA = 0, g_tfB = ~0u, g_tfCur = 0;
unsigned InitTrace() {
    if (const char* e = std::getenv("NP_GX_TRACE_FRAMES")) { if (std::sscanf(e, "%u-%u", &g_tfA, &g_tfB) < 2) g_tfB = g_tfA; }
    const char* n = std::getenv("NP_GX_TRACE_BEGIN");
    return n ? static_cast<unsigned>(std::atoi(n)) : 0u;
}
}
unsigned g_traceBegin = InitTrace();
void TraceFrameTick() { ++g_tfCur; }
void TraceBegin(unsigned prim, int fmt, unsigned verts) {
    if (g_tfCur < g_tfA || g_tfCur > g_tfB) return;
    std::fprintf(stderr, "[gxbegin] frame %u prim=0x%x fmt=%d n=%u\n", g_tfCur, prim, fmt, verts);
#if defined(__GLIBC__)
    void* stack[12];
    const int n = backtrace(stack, 12);
    backtrace_symbols_fd(stack, n, 2);
#endif
}
} // namespace np_gx
