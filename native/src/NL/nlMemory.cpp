#include "NL/nlMemory.h"

#include "NL/MemAlloc.h"

MemoryAllocator VirtualAllocator;
MemoryAllocator* AllocatorStack[16] = { &StandardAllocator };
MemoryAllocator* CurrentAllocator = &StandardAllocator;
unsigned int AllocatorStackDepth = 1;
u8 gMemoryInitialized;

static inline bool IsInAllocator(const MemoryAllocator& allocator, const void* ptr)
{
    const u8* base = (const u8*)allocator.m_memory;
    return base != 0 && (const u8*)ptr >= base && (const u8*)ptr < base + allocator.m_memory_size;
}

#ifdef NP_NATIVE
// NP: the native port is multi-threaded (renderer thread, job pool, SDK workers, libstdc++), the console build was not. The
// game's allocators are not thread safe and the arenas are sized for the game, so:
//  - the thread that runs the game's main() ("game thread") takes its operator new from the game heaps;
//  - every other thread's operator new comes from the host heap (malloc), with a 16-byte header that tells operator delete
//    apart (the game thread may free such a block, and any thread may free a game-heap block);
//  - everything that touches the game heaps (nlMalloc, nlFree, operator new on the game thread, operator delete of an arena
//    block) runs under one lock (uncontended: two atomic operations).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include "np/framesend.h"
#include "np/gsplit.h"   // gt27 (MSC_GSPLIT): game-heap calls on the game thread are the 'mem' phase
#include "np/lockwait.h"
// NP: host32 runs under valgrind (tools/det/vg.sh): every game-heap allocation is announced as undefined memory, so memcheck reports
// the reads of memory that no one initialised (fresh or recycled; the arenas are mmap'ed, which memcheck would call defined).
#if defined(NP_TARGET_HOST32) && defined(__has_include)
#if __has_include(<valgrind/memcheck.h>)
#include <valgrind/memcheck.h>
#define NP_VG_ANNOTATE 1
#endif
#endif
static __thread unsigned char tNpGameThread;
extern "C" volatile int gNpSimtCensus __attribute__((weak));   // block17 (src/Game/NpSimRun17.cpp)
extern "C" void NpSimtNoteHostAlloc(void);
extern "C" void NpSimtNoteGameAlloc(void);
static volatile int sNpHeapLock;
static unsigned sNpHeapLockSlow;   // acquisitions that ran out of spins (np/lockwait.h; 0 while one thread uses the heap)
extern "C" unsigned long long NpMemHeapLockSlow(void) { return (unsigned long long)__atomic_load_n(&sNpHeapLockSlow, __ATOMIC_RELAXED); }
static const unsigned kNpHostMagic = 0x4E504831u; // 'NPH1'
static inline void NpHeapLock()
{
    int spins = 0;
    while (__atomic_exchange_n(&sNpHeapLock, 1, __ATOMIC_ACQUIRE))
    {
        NpLockBackoff(spins, &sNpHeapLockSlow);
    }
}
static inline void NpHeapUnlock() { __atomic_store_n(&sNpHeapLock, 0, __ATOMIC_RELEASE); }
void NpMemMarkGameThread() { tNpGameThread = 1; }
// tools/dtlive: the harness runs in the game thread but must not allocate from the (full) game heaps while it forks and compares
extern "C" void NpMemSetGameThread(int on) { tNpGameThread = on ? 1 : 0; }
extern "C" int NpMemGetGameThread() { return tNpGameThread; }
static inline void* NpHostNew(size_t size)
{
    unsigned* p = (unsigned*)malloc(size + 16);
    if (p == 0)
    {
        return 0;
    }
    p[0] = kNpHostMagic;
    p[1] = (unsigned)size;
    return (u8*)p + 16;
}
static inline bool NpHostDelete(void* ptr)
{
    unsigned* p = (unsigned*)((u8*)ptr - 16);
    if (p[0] != kNpHostMagic)
    {
        return false;
    }
    p[0] = 0;
    free(p);
    return true;
}
#endif

static inline void FreeMemoryLocked(void* ptr);

#ifdef NP_NATIVE
// the frees the frame-send module kept until the join (np_framesend.h: MSC_FS_FREEGATE=2)
static void NpFsFreeNow(void* ptr)
{
    NpHeapLock();
    FreeMemoryLocked(ptr);
    NpHeapUnlock();
}
static bool sNpFsFreeFnSet;
#endif

static inline void FreeMemory(void* ptr)
{
#ifdef NP_NATIVE
    if (ptr == 0)
    {
        return;
    }
    NpGsScope npGsMem(NPGS_MEM);
    if (__builtin_expect(npGsMem.p >= 0, 0)) ++g_npGsCnt[NPGC_MEM_FREE];
    // (frame-send worker: before the lock, a join may have to wait for the job; or the free is kept until the join: NpFsHeapFree returns 1)
    if (&NpFsBusy != 0 && NpFsBusy != 0)
    {
        if (tNpGameThread)
        {
            if (!sNpFsFreeFnSet)
            {
                sNpFsFreeFnSet = true;
                NpFsSetFreeFn(NpFsFreeNow);
            }
            if (NpFsHeapFree(ptr) != 0)
            {
                return;
            }
        }
        else if (NpFsOnWorker())
        {
            NpFsWorkerHeapCall();
        }
    }
    NpHeapLock();
    FreeMemoryLocked(ptr);
    NpHeapUnlock();
#else
    FreeMemoryLocked(ptr);
#endif
}

static inline void FreeMemoryLocked(void* ptr)
{
    if (CurrentAllocator != &StandardAllocator && CurrentAllocator != &VirtualAllocator)
    {
        CurrentAllocator->Free(ptr);
        return;
    }

    // NP: the console told MEM1 (0x80..) from MEM2 (0x90..) by the top nibble of the address. On a
    // native host the two arenas live wherever the platform layer got them, so ask each allocator
    // whether the pointer is inside the block it was initialised with.
    if (IsInAllocator(StandardAllocator, ptr))
    {
        StandardAllocator.Free(ptr);
    }
    else if (IsInAllocator(VirtualAllocator, ptr))
    {
        VirtualAllocator.Free(ptr);
    }
}

#ifdef NP_NATIVE
extern "C" void NpHarnessAtExit(void (*fn)(void)) __attribute__((weak));   // platform/harness/src/np_harness.cpp
static unsigned long long sFillCalls, sFillBytes;
static void NpHeapFillReport()
{
    fprintf(stderr, "[heapfill] calls=%llu bytes=%llu\n", sFillCalls, sFillBytes);
}
#endif

static inline void* AllocateMemory(unsigned long size, unsigned int alignment, bool fromEnd)
{
    if (gMemoryInitialized == 0)
    {
        nlInitMemory();
    }
#ifdef NP_NATIVE
    NpGsScope npGsMem(NPGS_MEM);
    if (__builtin_expect(npGsMem.p >= 0, 0)) { ++g_npGsCnt[NPGC_MEM_ALLOC]; g_npGsCnt[NPGC_MEM_BYTES] += (unsigned)size; }
    // NP_HEAP_FLIP=1 (determinism tests, NPB3): every allocation is taken from the other end of its heap, which reverses the address order of almost
    // all objects; a simulation that depends on the order of heap addresses (pointer-keyed trees, sorts by address) changes with it.
    static int sFlip = -2;
    if (sFlip == -2)
    {
        const char* f = getenv("NP_HEAP_FLIP");
        sFlip = (f != 0 && *f != 0 && *f != '0') ? 1 : 0;
    }
    if (sFlip == 1)
    {
        fromEnd = !fromEnd;
    }
    NpHeapLock();
    void* result = CurrentAllocator->Allocate(size, alignment, fromEnd);
    NpHeapUnlock();
    // NP_HEAP_POISON=<byte> (determinism tests, NPB3): every game-heap allocation starts filled with that byte instead of
    // whatever the arena held. A simulation that reads memory before writing it changes with the byte; one that does not is
    // the same match for every value (tools/det/).
    static int sPoison = -2;
    if (sPoison == -2)
    {
        const char* e = getenv("NP_HEAP_POISON");
        // NP_HEAP_ZERO_DEFAULT (make HEAPZERO=1, VRS): without the variable every allocation starts zeroed, as mkwiita's zeroed operator new
        // does; NP_HEAP_POISON=off (or any byte) overrides. The cost is one memset per allocation (docs/wave4-VRS.md).
        if (e != 0 && strcmp(e, "off") == 0)
        {
            sPoison = -1;
        }
        else
        {
#ifdef NP_HEAP_ZERO_DEFAULT
            sPoison = (e != 0 && *e != 0) ? (int)(strtol(e, 0, 0) & 0xFF) : 0;
#else
            sPoison = (e != 0 && *e != 0) ? (int)(strtol(e, 0, 0) & 0xFF) : -1;
#endif
        }
    }
    if (sPoison >= 0 && result != 0 && size < 0x4000000)
    {
        memset(result, sPoison, size);
        // NP_HEAP_POISON_STATS=1 (VRS): how much the fill costs: calls and bytes, printed at exit ("[heapfill]").
        static int sStats = -2;
        if (sStats == -2)
        {
            const char* st = getenv("NP_HEAP_POISON_STATS");
            sStats = (st != 0 && *st != 0 && *st != '0') ? 1 : 0;
            if (sStats == 1)
            {
                // (the harness ends the process without exit handlers: it runs the hooks given to it)
                if (NpHarnessAtExit != 0)
                {
                    NpHarnessAtExit(NpHeapFillReport);
                }
                else
                {
                    atexit(NpHeapFillReport);
                }
            }
        }
        if (sStats == 1)
        {
            __atomic_fetch_add(&sFillCalls, 1ull, __ATOMIC_RELAXED);
            __atomic_fetch_add(&sFillBytes, (unsigned long long)size, __ATOMIC_RELAXED);
        }
    }
    if (&NpFsBusy != 0 && NpFsBusy != 0)
    {
        if (tNpGameThread)
        {
            NpFsHeapAlloc(result);
        }
        else if (NpFsOnWorker())
        {
            NpFsWorkerHeapCall();
        }
    }
#ifdef NP_VG_ANNOTATE
    if (result != 0 && RUNNING_ON_VALGRIND)
    {
        VALGRIND_MAKE_MEM_UNDEFINED(result, size);
    }
#endif
    return result;
#else
    return CurrentAllocator->Allocate(size, alignment, fromEnd);
#endif
}

#ifdef NP_NATIVE
static inline void NpOperatorDelete(void* ptr)
{
    if (ptr == 0)
    {
        return;
    }
    if (IsInAllocator(StandardAllocator, ptr) || IsInAllocator(VirtualAllocator, ptr) || !NpHostDelete(ptr))
    {
        FreeMemory(ptr);
    }
}
// 1.2: a request the host heap (malloc) cannot meet. operator new does not throw in the native build; it used to return null, and the caller
// (a std::vector of the renderer's frame arenas, a std::string) then wrote through it: a crash that said nothing about memory. The platform
// writes one FATAL line with the heap's state to the player's basic log and flushes it (Vita: NpPlatformHostAllocFailed); then the program ends.
extern "C" void NpPlatformHostAllocFailed(size_t size) __attribute__((weak));
static void NpHostAllocFailed(size_t size)
{
    if (NpPlatformHostAllocFailed)
    {
        NpPlatformHostAllocFailed(size);
    }
    else
    {
        fprintf(stderr, "FATAL: host heap exhausted: a request for %lu KiB found no room\n", (unsigned long)(size / 1024u));
        fflush(stderr);
    }
    abort();
}
// 1.3 (renderer vgx_arena.h, MSC_VGX_ARENA_SOFT): a host-heap block that may be refused. Same header as every host block, so operator delete takes it
// from any thread; null when malloc has no room, with nothing logged and nothing ended: the caller degrades.
extern "C" void* NpHostTryNew(size_t size)
{
    return NpHostNew(size);
}
extern "C" void NpHostTryDelete(void* ptr)
{
    NpOperatorDelete(ptr);
}

extern "C" void NpHostHeapExhausted(size_t size)
{
    NpHostAllocFailed(size);
    abort();
}

// ---- MSC_HEAP_GUARD (np/host_alloc.h; the Galaxy port's GXV_HEAP_GUARD). 0 (default): nothing. 1: the port's registered long-lived
// storage is checked at every stadium destroy and any of it inside a game arena is logged. 2: also, in host builds that have
// <execinfo.h>, a census of every game-thread operator new by call stack (static tables, no allocation), printed at exit as
// "[heapcensus]" lines for tools/heapguard/census.py. Neither touches the game's heaps.
static int sNpHeapGuard = -1;   // (a plain word: read once, then the same value; no guarded static on operator new's path)
extern "C" int NpHeapGuardMode(void)
{
    if (sNpHeapGuard < 0)
    {
        const char* e = getenv("MSC_HEAP_GUARD");
        sNpHeapGuard = (e != 0 && *e != 0) ? (int)strtol(e, 0, 0) : 0;
    }
    return sNpHeapGuard;
}
extern "C" int NpHeapGuardInGameHeap(const void* p)
{
    return p != 0 && (IsInAllocator(StandardAllocator, p) || IsInAllocator(VirtualAllocator, p)) ? 1 : 0;
}
struct NpHeapGuardEnt
{
    const char* name;
    const void* (*data)(const void*);
    const void* obj;
};
static NpHeapGuardEnt sNpHeapGuardEnt[64];
static int sNpHeapGuardCount;
extern "C" void NpHeapGuardRegister(const char* name, const void* (*data)(const void* obj), const void* obj)
{
    if (NpHeapGuardMode() == 0)
    {
        return;
    }
    NpHeapLock();
    bool known = false;
    for (int i = 0; i < sNpHeapGuardCount; ++i)
    {
        known = known || sNpHeapGuardEnt[i].obj == obj;
    }
    if (!known && sNpHeapGuardCount < (int)(sizeof(sNpHeapGuardEnt) / sizeof(sNpHeapGuardEnt[0])))
    {
        NpHeapGuardEnt& g = sNpHeapGuardEnt[sNpHeapGuardCount++];
        g.name = name;
        g.data = data;
        g.obj = obj;
    }
    NpHeapUnlock();
}
extern "C" int NpHeapGuardCheck(const char* when)
{
    if (NpHeapGuardMode() == 0)
    {
        return 0;
    }
    int bad = 0;
    const int n = sNpHeapGuardCount;
    for (int i = 0; i < n; ++i)
    {
        const void* p = sNpHeapGuardEnt[i].data(sNpHeapGuardEnt[i].obj);
        if (NpHeapGuardInGameHeap(p))
        {
            ++bad;
            fprintf(stderr, "[heapguard] %s: %s has its storage at %p, inside a game heap\n", when, sNpHeapGuardEnt[i].name, p);
        }
    }
    fprintf(stderr, "[heapguard] %s: %d registered, %d in a game heap\n", when, n, bad);
    return bad;
}

#if defined(__has_include) && !defined(NP_TARGET_VITA)
#if __has_include(<execinfo.h>) && __has_include(<dlfcn.h>)
#include <execinfo.h>
#include <dlfcn.h>
#define NP_HEAP_CENSUS 1
#endif
#endif
#ifdef NP_HEAP_CENSUS
enum { kNpCensusSlots = 8192, kNpCensusDepth = 10 };
struct NpCensusEnt
{
    unsigned hash, count, n;
    unsigned long long bytes;
    void* pc[kNpCensusDepth];
};
static NpCensusEnt sNpCensus[kNpCensusSlots];
static unsigned sNpCensusUsed, sNpCensusLost;
static __thread int tNpCensusBusy;
static void NpCensusReport()
{
    Dl_info info;
    const unsigned long base = dladdr((void*)&NpHeapGuardMode, &info) != 0 ? (unsigned long)info.dli_fbase : 0;
    unsigned long long total = 0, calls = 0;
    for (unsigned i = 0; i < kNpCensusSlots; ++i)
    {
        const NpCensusEnt& e = sNpCensus[i];
        if (e.count == 0)
        {
            continue;
        }
        calls += e.count;
        total += e.bytes;
        fprintf(stderr, "[heapcensus] %u %llu", e.count, e.bytes);
        for (unsigned k = 0; k < e.n; ++k)
        {
            fprintf(stderr, " %lx", (unsigned long)e.pc[k] - base);
        }
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "[heapcensus] total: %llu game-thread operator new calls, %llu bytes, %u stacks, %u not kept (table full)\n", calls, total,
        sNpCensusUsed, sNpCensusLost);
    fflush(stderr);
}
static void NpCensusNote(size_t size)
{
    if (tNpCensusBusy)
    {
        return;
    }
    tNpCensusBusy = 1;
    static bool sHooked;
    if (!sHooked)
    {
        sHooked = true;
        if (NpHarnessAtExit != 0)
        {
            NpHarnessAtExit(NpCensusReport);
        }
        else
        {
            atexit(NpCensusReport);
        }
    }
    void* pc[kNpCensusDepth + 2];
    const int got = backtrace(pc, kNpCensusDepth + 2);   // [0] this function, [1] operator new: the callers follow
    const int n = got > 2 ? got - 2 : 0;
    unsigned h = 2166136261u;
    for (int k = 0; k < n; ++k)
    {
        h = (h ^ (unsigned)(unsigned long)pc[k + 2]) * 16777619u;
    }
    h |= 1u;
    for (unsigned probe = 0; probe < kNpCensusSlots; ++probe)
    {
        NpCensusEnt& e = sNpCensus[(h + probe) & (kNpCensusSlots - 1)];
        if (e.hash == 0)
        {
            e.hash = h;
            e.n = (unsigned)n;
            for (int k = 0; k < n; ++k)
            {
                e.pc[k] = pc[k + 2];
            }
            ++sNpCensusUsed;
        }
        else if (e.hash != h)
        {
            continue;
        }
        ++e.count;
        e.bytes += size;
        tNpCensusBusy = 0;
        return;
    }
    ++sNpCensusLost;
    tNpCensusBusy = 0;
}
#endif

// NpHostAllocScope (np/host_alloc.h): while this thread's count is above 0 its operator new takes the host heap even on the game thread. Only the
// allocation changes: frees keep the game thread's rules (the frame-send deferral of NpFsHeapFree above), so a game-heap block freed inside a scope
// is still kept until the join when a send is running.
static __thread int tNpHostScope;
extern "C" int NpMemHostScope(int delta)
{
    const int was = tNpHostScope;
    tNpHostScope += delta;
    return was;
}

static inline void* NpOperatorNew(size_t size)
{
    if (!tNpGameThread || tNpHostScope > 0)
    {
        if (&gNpSimtCensus != 0 && gNpSimtCensus != 0) NpSimtNoteHostAlloc();   // block17 (MSC_SIMT=1): counted when np_sim asks for the host heap (it must not)
        void* p = NpHostNew(size);
        if (p == 0)
        {
            NpHostAllocFailed(size);
        }
        return p;
    }
#ifdef NP_HEAP_CENSUS
    if (__builtin_expect(sNpHeapGuard != 0, 0) && NpHeapGuardMode() >= 2)
    {
        NpCensusNote(size);
    }
#endif
    if (&gNpSimtCensus != 0 && gNpSimtCensus != 0) NpSimtNoteGameAlloc();   // block17 (MSC_SIMT=1): game-heap requests made on np_sim (a count, for the log line)
    return AllocateMemory(size, 8, false);
}
void operator delete(void* ptr) { NpOperatorDelete(ptr); }
void operator delete[](void* ptr) { NpOperatorDelete(ptr); }
// C++14 sized deallocation, used by the platform code (built as gnu++17): without these the calls bind to libstdc++'s, which forward to the
// operator above on a normal run but are replaced by valgrind's allocator under valgrind (an "Invalid free" of every game-heap block)
void operator delete(void* ptr, size_t) { NpOperatorDelete(ptr); }
void operator delete[](void* ptr, size_t) { NpOperatorDelete(ptr); }
void* operator new(size_t size) { return NpOperatorNew(size); }
void* operator new[](size_t size) { return NpOperatorNew(size); }
#else
void operator delete(void* ptr)
{
    FreeMemory(ptr);
}

void operator delete[](void* ptr)
{
    FreeMemory(ptr);
}

void* operator new(size_t size)
{
    return AllocateMemory(size, 8, false);
}

void* operator new[](size_t size)
{
    return AllocateMemory(size, 8, false);
}
#endif

void* nlMalloc(unsigned long size)
{
    return AllocateMemory(size, 8, false);
}

void* nlMalloc(unsigned long size, unsigned int alignment, bool fromEnd)
{
    return AllocateMemory(size, alignment, fromEnd);
}

void nlFree(void* ptr)
{
    FreeMemory(ptr);
}
