// Memory accounting for a Vita build (workstream V, session 4): what the process uses in
// each of the budgets a console enforces, measured while it runs.
//
// A console gives an application three separate budgets (the packages ask for the largest
// main one, ATTRIBUTE2=12): about 365 MiB of main memory (the executable image, every
// user memory block, every thread stack), 112 MiB of video memory (CDRAM) and 26 MiB of
// physically contiguous memory. Nothing warns before one is exhausted: an allocation fails
// and, inside a library, usually only a missing texture or a crash shows it. So the port
// keeps its own figures:
//   - what the SYSTEM says is free in each budget (sceKernelGetFreeMemorySize), at every
//     checkpoint, with the lowest value seen;
//   - the C heap (newlib's, one block of fixed size taken at start-up): bytes in use and
//     the most that was ever in use, sampled every 30 frames;
//   - vitaGL's pools. vitaGL takes its memory in a few large blocks at start-up (one per
//     budget) and serves textures, vertex data, render targets and its own state from them
//     through the system's sceClibMspace* allocator. Those calls are wrapped at link time
//     (-Wl,--wrap=..., Makefile.vita) and every allocation is counted against its pool:
//     in use now, the most ever in use, the largest single allocation, failed allocations.
//     (The system's own statistics call gives the current figure only, and Vita3K does not
//     implement it or the usable-size call at all, hence the table of sizes kept here.)
//     MSC_VITA_MEMSTATS=0 in env.txt turns the counting off (the wraps then only forward);
//     =2 also logs every allocation of 1 MiB or more with its caller's address;
//   - thread stacks of the threads this layer knows: size, and how deep each was used
//     (the first word from the low end that is not a new stack's fill: zero, or 0xCC in Vita3K);
//   - the game's memory arenas (NpAllocArena: MEM1, MEM2 ...), per 4 KiB page: holding data,
//     holding only the game allocator's 0xCD fill, or zero; and the highest page with data
//     (how much of MEM2 the game really uses).
// A short line goes to the log every 600 frames ("[mem] ..."), the full table when the
// test harness ends a run (NpVitaExitReport, called from np_harness.cpp) and on request
// (ReportNow). Budgets and what the figures mean for them: np/docs/vita.md, "Memory".
#if defined(__vita__)

#include "np_vita_internal.h"

#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>

extern "C" {
SceClibMspace __real_sceClibMspaceCreate(void* memblock, SceSize size);
void __real_sceClibMspaceDestroy(SceClibMspace mspace);
void* __real_sceClibMspaceMalloc(SceClibMspace mspace, SceSize size);
void* __real_sceClibMspaceCalloc(SceClibMspace mspace, SceSize num, SceSize size);
void* __real_sceClibMspaceRealloc(SceClibMspace mspace, void* ptr, SceSize size);
void* __real_sceClibMspaceMemalign(SceClibMspace mspace, SceSize alignment, SceSize size);
void __real_sceClibMspaceFree(SceClibMspace mspace, void* ptr);
extern unsigned int _newlib_heap_size_user;
}

namespace VitaMem {
namespace {

// Arenas the game asked for through NpAllocArena (np_vita_process.cpp calls RegisterArena).
constexpr int kMaxArenas = 8;
struct ArenaRecord { char name[24]; uintptr_t base; size_t size; };
ArenaRecord g_arenas[kMaxArenas];
std::atomic<int> g_arenaCount{0};

// ---- one small lock for the tables (allocations come from the render thread, frees also
// from vitaGL's collector thread; the critical sections are a few dozen instructions)
std::atomic_flag g_lock = ATOMIC_FLAG_INIT;
struct Locked {
    Locked() {
        int spins = 0;
        while (g_lock.test_and_set(std::memory_order_acquire))
            if (++spins > 200) { sceKernelDelayThread(50); spins = 0; }
    }
    ~Locked() { g_lock.clear(std::memory_order_release); }
};

// ---- vitaGL's pools
struct Pool {
    void* space = nullptr;      // the mspace handle
    uintptr_t base = 0;
    uint32_t capacity = 0;
    uint32_t blockType = 0;     // kernel memory block type of the pool's memory (0 = not known)
    uint64_t inUse = 0, peak = 0;
    uint32_t live = 0, livePeak = 0, largest = 0;
    uint64_t allocs = 0, frees = 0, failed = 0, failedBytes = 0, untracked = 0;
};
constexpr int kMaxPools = 8;
Pool g_pools[kMaxPools];
int g_poolCount = 0;

// pointer -> size. Open addressing, linear probing, tombstones; 32768 slots (vitaGL holds a
// few thousand allocations in a match). When it is full, further allocations are counted as
// "untracked" and their frees are not seen: the figures then say so.
constexpr uint32_t kSlots = 32768;
constexpr uintptr_t kEmpty = 0, kTomb = 1;
struct Slot {
    uintptr_t ptr;
    uint32_t size;
};
Slot g_slots[kSlots];
uint32_t g_slotUsed = 0;   // occupied + tombstones
int g_enabled = -1;        // -1 not decided, 0 off, 1 on

bool g_logLarge = false;   // MSC_VITA_MEMSTATS=2: a log line for every allocation of 1 MiB or more
bool Enabled() {
    if (g_enabled < 0) {
        const char* e = std::getenv("MSC_VITA_MEMSTATS");
        g_logLarge = e && std::atoi(e) == 2;
        g_enabled = (e && *e && std::atoi(e) == 0) ? 0 : 1;
    }
    return g_enabled == 1;
}
uint32_t HashOf(uintptr_t p) { return static_cast<uint32_t>((p >> 3) * 2654435761u) >> 17; }   // 15 bits

Pool* PoolOf(void* space) {
    for (int i = 0; i < g_poolCount; ++i)
        if (g_pools[i].space == space) return &g_pools[i];
    return nullptr;
}
// (Outside the lock.) `caller` is a return address inside vitaGL: look it up in the link map.
void LogLarge(const char* what, void* space, uint32_t size, void* result, void* caller) {
    if (!g_logLarge || size < 1024u * 1024u) return;
    int index = -1;
    for (int i = 0; i < g_poolCount; ++i)
        if (g_pools[i].space == space) index = i;
    std::fprintf(stderr, "[mem] %s %u KiB in pool %d -> %p (called from %p, frame %llu)\n", what, size / 1024u, index, result, caller,
                 static_cast<unsigned long long>(VitaPlatform::FrameCount()));
}
// (lock held) Deleted entries leave tombstones; when they and the live entries together fill
// three quarters of the table it is rebuilt from the live ones.
Slot g_rebuild[kSlots];
uint32_t g_liveSlots = 0, g_staleEntries = 0;
void Rebuild() {
    std::memcpy(g_rebuild, g_slots, sizeof(g_slots));
    std::memset(g_slots, 0, sizeof(g_slots));
    g_slotUsed = 0;
    for (uint32_t k = 0; k < kSlots; ++k) {
        if (g_rebuild[k].ptr <= kTomb) continue;
        uint32_t i = HashOf(g_rebuild[k].ptr);
        while (g_slots[i].ptr != kEmpty) i = (i + 1) & (kSlots - 1);
        g_slots[i] = g_rebuild[k];
        ++g_slotUsed;
    }
}
// (lock held)
void Insert(Pool& pool, void* ptr, uint32_t size) {
    ++pool.allocs;
    if (g_slotUsed >= kSlots / 4 * 3) {
        if (g_liveSlots < kSlots / 2) Rebuild();
        if (g_slotUsed >= kSlots / 4 * 3) { ++pool.untracked; return; }
    }
    const uintptr_t key = reinterpret_cast<uintptr_t>(ptr);
    uint32_t i = HashOf(key);
    int32_t reuse = -1;
    for (; g_slots[i].ptr != kEmpty; i = (i + 1) & (kSlots - 1)) {
        if (g_slots[i].ptr == kTomb) { if (reuse < 0) reuse = static_cast<int32_t>(i); continue; }
        if (g_slots[i].ptr != key) continue;
        // The allocator handed out an address this table still holds: its free was not seen
        // (a call that is not wrapped). The old entry is replaced.
        pool.inUse -= g_slots[i].size;
        --pool.live;
        --g_liveSlots;
        ++g_staleEntries;
        reuse = static_cast<int32_t>(i);
        g_slots[i].ptr = kTomb;
        break;
    }
    if (reuse >= 0) i = static_cast<uint32_t>(reuse);
    else ++g_slotUsed;
    g_slots[i].ptr = key;
    g_slots[i].size = size;
    ++g_liveSlots;
    pool.inUse += size;
    if (pool.inUse > pool.peak) pool.peak = pool.inUse;
    if (++pool.live > pool.livePeak) pool.livePeak = pool.live;
    if (size > pool.largest) pool.largest = size;
}
// (lock held) Returns the recorded size, 0 when the pointer is not in the table.
uint32_t Remove(Pool& pool, void* ptr) {
    const uintptr_t key = reinterpret_cast<uintptr_t>(ptr);
    uint32_t i = HashOf(key);
    for (uint32_t probes = 0; probes < kSlots && g_slots[i].ptr != kEmpty; ++probes, i = (i + 1) & (kSlots - 1)) {
        if (g_slots[i].ptr != key) continue;
        const uint32_t size = g_slots[i].size;
        g_slots[i].ptr = kTomb;
        --g_liveSlots;
        pool.inUse -= size;
        --pool.live;
        ++pool.frees;
        return size;
    }
    return 0;
}
void NoteFailure(void* space, uint32_t size) {
    Locked lock;
    if (Pool* pool = PoolOf(space)) { ++pool->failed; pool->failedBytes += size; }
}

const char* TypeName(uint32_t type) {
    switch (type) {
    case SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW: return "CDRAM";
    case SCE_KERNEL_MEMBLOCK_TYPE_USER_RW: return "main";
    case SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE: return "main, uncached";
    case SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW: return "PHYCONT";
    case SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW: return "PHYCONT, uncached";
    default: return "type not known";
    }
}

// ---- the C heap and the system's own figures
std::atomic<uint32_t> g_heapPeak{0};
int g_minFree[3] = {0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF};   // main, cdram, phycont
int g_firstFree[3] = {-1, -1, -1};

bool SystemFree(int out[3]) {
    SceKernelFreeMemorySizeInfo info;
    std::memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (sceKernelGetFreeMemorySize(&info) < 0) return false;
    out[0] = info.size_user;
    out[1] = info.size_cdram;
    out[2] = info.size_phycont;
    for (int i = 0; i < 3; ++i) {
        if (out[i] < g_minFree[i]) g_minFree[i] = out[i];
        if (g_firstFree[i] < 0) g_firstFree[i] = out[i];
    }
    return true;
}

// ---- threads
struct ThreadRecord {
    char name[24];
    uintptr_t stackLo;
    uint32_t stackSize;
};
constexpr int kMaxThreads = 16;
ThreadRecord g_threads[kMaxThreads];
std::atomic<int> g_threadCount{0};

// Bytes of a stack that were ever used: from the first word that is not the fill a new stack
// starts with (searching up from the low end, where the stack ends) to the top. The fill is
// zero or, in Vita3K, 0xCC bytes (kernel/src/thread.cpp); what a console's kernel leaves in a
// new stack is not known here, so a result equal to the whole stack means "cannot tell".
uint32_t StackUsed(const ThreadRecord& t) {
    const uint32_t* p = reinterpret_cast<const uint32_t*>(t.stackLo);
    const uint32_t words = t.stackSize / 4u;
    uint32_t i = 0;
    while (i < words && (p[i] == 0 || p[i] == 0xCCCCCCCCu)) ++i;
    return (words - i) * 4u;
}

} // namespace

void SampleHeap() {
    const struct mallinfo heap = mallinfo();
    const uint32_t used = static_cast<uint32_t>(heap.uordblks);
    uint32_t peak = g_heapPeak.load(std::memory_order_relaxed);
    while (used > peak && !g_heapPeak.compare_exchange_weak(peak, used, std::memory_order_relaxed)) {}
}

void RegisterThread(const char* name) {
    SceKernelThreadInfo info;
    std::memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) < 0 || !info.stack || !info.stackSize) return;
    const uintptr_t lo = reinterpret_cast<uintptr_t>(info.stack);
    const int count = g_threadCount.load(std::memory_order_acquire);
    for (int i = 0; i < count; ++i)
        if (g_threads[i].stackLo == lo) return;   // registered already (a role pinned twice)
    const int index = g_threadCount.fetch_add(1, std::memory_order_acq_rel);
    if (index >= kMaxThreads) { g_threadCount.store(kMaxThreads); return; }
    std::snprintf(g_threads[index].name, sizeof(g_threads[index].name), "%s", name ? name : "?");
    g_threads[index].stackSize = static_cast<uint32_t>(info.stackSize);
    g_threads[index].stackLo = lo;   // last: a reader takes a record with a stack address as complete
}

void Checkpoint(const char* when) {
    SampleHeap();
    int free[3] = {0, 0, 0};
    SystemFree(free);
    const struct mallinfo heap = mallinfo();
    char pools[256] = "";
    size_t n = 0;
    {
        Locked lock;
        for (int i = 0; i < g_poolCount; ++i) {
            const Pool& p = g_pools[i];
            const int wrote = std::snprintf(pools + n, sizeof(pools) - n, "; pool %d (%s) %u/%u KiB, peak %u", i, TypeName(p.blockType),
                                            static_cast<unsigned>(p.inUse / 1024u), p.capacity / 1024u, static_cast<unsigned>(p.peak / 1024u));
            if (wrote < 0 || static_cast<size_t>(wrote) >= sizeof(pools) - n) break;
            n += static_cast<size_t>(wrote);
        }
    }
    std::fprintf(stderr, "[mem] %s: system free main %d, cdram %d, phycont %d KiB; C heap %u KiB in use, peak %u, of %u%s\n", when ? when : "now",
                 free[0] / 1024, free[1] / 1024, free[2] / 1024, static_cast<unsigned>(heap.uordblks / 1024),
                 static_cast<unsigned>(g_heapPeak.load() / 1024u), _newlib_heap_size_user / 1024u, pools);
}

void ReportNow(const char* why) {
    SampleHeap();
    int free[3] = {0, 0, 0};
    const bool haveFree = SystemFree(free);
    const struct mallinfo heap = mallinfo();
    std::fprintf(stderr, "[mem] ===== memory report (%s), frame %llu =====\n", why ? why : "", static_cast<unsigned long long>(VitaPlatform::FrameCount()));
    if (haveFree)
        std::fprintf(stderr, "[mem] system free now: main %d KiB, cdram %d KiB, phycont %d KiB; lowest seen: %d / %d / %d KiB; at the first reading: %d / %d / %d KiB\n",
                     free[0] / 1024, free[1] / 1024, free[2] / 1024, g_minFree[0] / 1024, g_minFree[1] / 1024, g_minFree[2] / 1024,
                     g_firstFree[0] / 1024, g_firstFree[1] / 1024, g_firstFree[2] / 1024);
    std::fprintf(stderr, "[mem] C heap: block %u KiB (taken whole at start-up); in use now %u KiB, most ever in use %u KiB (sampled every 30 frames), extent reached %u KiB\n",
                 _newlib_heap_size_user / 1024u, static_cast<unsigned>(heap.uordblks / 1024), static_cast<unsigned>(g_heapPeak.load() / 1024u),
                 static_cast<unsigned>(heap.arena / 1024));
    {
        Pool copy[kMaxPools];
        int count;
        uint32_t slots;
        {
            Locked lock;
            count = g_poolCount;
            slots = g_slotUsed;
            for (int i = 0; i < count; ++i) copy[i] = g_pools[i];
        }
        if (!Enabled()) std::fprintf(stderr, "[mem] vitaGL pools: not counted (MSC_VITA_MEMSTATS=0)\n");
        for (int i = 0; i < count; ++i) {
            const Pool& p = copy[i];
            std::fprintf(stderr, "[mem] vitaGL pool %d (%s) at 0x%08lX: %u KiB; in use now %u KiB in %u allocations; most ever %u KiB (%u allocations at once); largest single %u KiB; %llu allocations, %llu frees, %llu FAILED (%llu KiB asked)%s\n",
                         i, TypeName(p.blockType), static_cast<unsigned long>(p.base), p.capacity / 1024u, static_cast<unsigned>(p.inUse / 1024u), p.live,
                         static_cast<unsigned>(p.peak / 1024u), p.livePeak, p.largest / 1024u, static_cast<unsigned long long>(p.allocs),
                         static_cast<unsigned long long>(p.frees), static_cast<unsigned long long>(p.failed),
                         static_cast<unsigned long long>(p.failedBytes / 1024u), p.untracked ? " (table full: figures are too low)" : "");
        }
        if (count) std::fprintf(stderr, "[mem] (pool figures are requested sizes; the allocator adds 8-16 bytes each and alignment; table %u of %u slots, %u entries replaced without a free seen)\n", slots, kSlots, g_staleEntries);
    }
    const int threads = g_threadCount.load(std::memory_order_acquire);
    uint32_t stackTotal = 0;
    for (int i = 0; i < threads && i < kMaxThreads; ++i) {
        const ThreadRecord& t = g_threads[i];
        if (!t.stackLo) continue;
        stackTotal += t.stackSize;
        std::fprintf(stderr, "[mem] thread \"%s\": stack %u KiB at 0x%08lX, deepest use %u KiB\n", t.name, t.stackSize / 1024u,
                     static_cast<unsigned long>(t.stackLo), (StackUsed(t) + 1023u) / 1024u);
    }
    std::fprintf(stderr, "[mem] stacks of the %d threads above: %u KiB in all (threads the libraries create are not listed)\n", threads, stackTotal / 1024u);
    // Arenas, per 4 KiB page: all zero (never written), all 0xCDCDCDCD (the fill the game's own
    // allocator gives memory it has not handed out: seen as the value read through an
    // uninitialised pointer), or anything else = really used at some time.
    const int arenas = g_arenaCount.load(std::memory_order_acquire);
    for (int b = 0; b < arenas && b < kMaxArenas; ++b) {
        const ArenaRecord& block = g_arenas[b];
        const uint32_t* host = reinterpret_cast<const uint32_t*>(block.base);
        const uint32_t pages = static_cast<uint32_t>(block.size / 4096u);
        uint32_t used = 0, fill = 0, highest = 0;
        for (uint32_t page = 0; page < pages; ++page) {
            const uint32_t* p = host + page * 1024u;
            const uint32_t first = p[0];
            bool same = first == 0u || first == 0xCDCDCDCDu;
            for (int w = 1; w < 1024 && same; ++w) same = p[w] == first;
            if (same) { if (first != 0u) ++fill; continue; }
            ++used;
            highest = page + 1;
        }
        std::fprintf(stderr, "[mem] arena %s (%p, %u KiB): %u KiB in pages with data, %u KiB in pages of the 0xCD fill, %u KiB zero; the highest page with data ends at +%u KiB\n",
                     block.name, reinterpret_cast<void*>(block.base), static_cast<unsigned>(block.size / 1024u), used * 4u, fill * 4u, (pages - used - fill) * 4u, highest * 4u);
    }
    std::fprintf(stderr, "[mem] ===== end of memory report =====\n");
}

} // namespace VitaMem

namespace VitaMem {
void RegisterArena(const char* name, void* base, size_t size) {
    const int i = g_arenaCount.load(std::memory_order_relaxed);
    if (i >= kMaxArenas) return;
    std::snprintf(g_arenas[i].name, sizeof(g_arenas[i].name), "%s", name ? name : "?");
    g_arenas[i].base = reinterpret_cast<uintptr_t>(base);
    g_arenas[i].size = size;
    g_arenaCount.store(i + 1, std::memory_order_release);
}
} // namespace VitaMem

// The SDK layer's own account (mixer mode line, path counters...): platform/sdk may define this.
extern "C" void NpSdkPrintStats(void) __attribute__((weak));
// End of a harness run (np_harness.cpp Finish): memory table, audio, job pool, the SDK layer's stats.
extern "C" void NpVitaExitReport(void) {
    VitaMem::ReportNow("end of a scripted run");
    VitaAudio::LogReport("the end of the run");
    if (NpSdkPrintStats) NpSdkPrintStats();
}

// ---- the wraps
extern "C" {

SceClibMspace __wrap_sceClibMspaceCreate(void* memblock, SceSize size) {
    using namespace VitaMem;
    const SceClibMspace space = __real_sceClibMspaceCreate(memblock, size);
    if (space && Enabled()) {
        uint32_t type = 0;
        SceKernelMemBlockInfo info;
        std::memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelGetMemBlockInfoByAddr(memblock, &info) >= 0) type = static_cast<uint32_t>(info.type);
        Locked lock;
        if (g_poolCount < kMaxPools) {
            Pool& p = g_pools[g_poolCount++];
            p = Pool{};
            p.space = space;
            p.base = reinterpret_cast<uintptr_t>(memblock);
            p.capacity = size;
            p.blockType = type;
        }
    }
    return space;
}
void __wrap_sceClibMspaceDestroy(SceClibMspace mspace) {
    using namespace VitaMem;
    {
        Locked lock;
        if (Pool* p = PoolOf(mspace)) p->space = nullptr;   // (its figures stay in the report)
    }
    __real_sceClibMspaceDestroy(mspace);
}
void* __wrap_sceClibMspaceMalloc(SceClibMspace mspace, SceSize size) {
    using namespace VitaMem;
    void* ptr = __real_sceClibMspaceMalloc(mspace, size);
    if (!Enabled()) return ptr;
    LogLarge(ptr ? "allocation of" : "REFUSED allocation of", mspace, size, ptr, __builtin_return_address(0));
    if (!ptr) { NoteFailure(mspace, size); return ptr; }
    Locked lock;
    if (Pool* p = PoolOf(mspace)) Insert(*p, ptr, size);
    return ptr;
}
void* __wrap_sceClibMspaceCalloc(SceClibMspace mspace, SceSize num, SceSize size) {
    using namespace VitaMem;
    void* ptr = __real_sceClibMspaceCalloc(mspace, num, size);
    if (!Enabled()) return ptr;
    LogLarge(ptr ? "allocation of" : "REFUSED allocation of", mspace, num * size, ptr, __builtin_return_address(0));
    if (!ptr) { NoteFailure(mspace, num * size); return ptr; }
    Locked lock;
    if (Pool* p = PoolOf(mspace)) Insert(*p, ptr, num * size);
    return ptr;
}
void* __wrap_sceClibMspaceMemalign(SceClibMspace mspace, SceSize alignment, SceSize size) {
    using namespace VitaMem;
    void* ptr = __real_sceClibMspaceMemalign(mspace, alignment, size);
    if (!Enabled()) return ptr;
    LogLarge(ptr ? "allocation of" : "REFUSED allocation of", mspace, size, ptr, __builtin_return_address(0));
    if (!ptr) { NoteFailure(mspace, size); return ptr; }
    Locked lock;
    if (Pool* p = PoolOf(mspace)) Insert(*p, ptr, size);
    return ptr;
}
void* __wrap_sceClibMspaceRealloc(SceClibMspace mspace, void* ptr, SceSize size) {
    using namespace VitaMem;
    if (!Enabled()) return __real_sceClibMspaceRealloc(mspace, ptr, size);
    // The old block is taken out of the table first: once the real call has returned, another
    // thread may already have been given its address.
    uint32_t old = 0;
    if (ptr) {
        Locked lock;
        if (Pool* p = PoolOf(mspace)) { old = Remove(*p, ptr); if (old) --p->frees; }
    }
    void* result = __real_sceClibMspaceRealloc(mspace, ptr, size);
    Locked lock;
    Pool* p = PoolOf(mspace);
    if (!p) return result;
    if (result) {
        Insert(*p, result, size);
        if (ptr) --p->allocs;   // (a move or a resize, not a new allocation)
    } else {
        ++p->failed;
        p->failedBytes += size;
        if (ptr && old && size != 0) { Insert(*p, ptr, old); --p->allocs; }   // a failed realloc leaves the old block
    }
    return result;
}
void __wrap_sceClibMspaceFree(SceClibMspace mspace, void* ptr) {
    using namespace VitaMem;
    if (ptr && Enabled()) {
        Locked lock;
        if (Pool* p = PoolOf(mspace)) Remove(*p, ptr);
    }
    __real_sceClibMspaceFree(mspace, ptr);
}

} // extern "C"

#endif // __vita__
