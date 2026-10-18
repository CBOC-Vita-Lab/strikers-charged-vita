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
// NP: host32 runs under valgrind (tools/det/vg.sh): every game-heap allocation is announced as undefined memory, so memcheck reports
// the reads of memory that no one initialised (fresh or recycled; the arenas are mmap'ed, which memcheck would call defined).
#if defined(NP_TARGET_HOST32) && defined(__has_include)
#if __has_include(<valgrind/memcheck.h>)
#include <valgrind/memcheck.h>
#define NP_VG_ANNOTATE 1
#endif
#endif
static __thread unsigned char tNpGameThread;
static volatile int sNpHeapLock;
static const unsigned kNpHostMagic = 0x4E504831u; // 'NPH1'
static inline void NpHeapLock()
{
    int spins = 0;
    while (__atomic_exchange_n(&sNpHeapLock, 1, __ATOMIC_ACQUIRE))
    {
        if (++spins > 64)
        {
            sched_yield();
        }
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
static inline void* NpOperatorNew(size_t size)
{
    if (!tNpGameThread)
    {
        return NpHostNew(size);
    }
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
