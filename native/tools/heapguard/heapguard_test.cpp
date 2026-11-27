// 1.3 np/host_alloc.h + MSC_HEAP_GUARD: the real NL/nlMemory.cpp and NL/MemAlloc.cpp over two small arenas (nlInitMemory below stands in for the platform's).
// On a thread marked as the game thread a plain std::vector takes game-heap storage (the hazard the Galaxy port's r67 crash came from); np::HostVector,
// np::HostString and anything made inside NpHostAllocScope take the host heap, on any thread. NpHeapGuardCheck finds exactly the registered storage that lies
// in a game arena. Frees work across the two heaps in both directions. BREAK=1: HostAllocator uses operator new (the check must then fail).
#include "NL/MemAlloc.h"   // (not NL/nlMemory.h: its placement new clashes with <new>, which the containers bring)
#include "np/host_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

extern "C" void NpMemSetGameThread(int on);
extern u8 gMemoryInitialized;
extern MemoryAllocator VirtualAllocator;
MemoryAllocator StandardAllocator;   // (src/NL/plat/nlMemory.cpp's, which needs the OS layer)
// MemAlloc.cpp's debug output, not reached here
void nlBreak() {}
void* nlOpenFileDebug(const char*, bool, bool) { return 0; }
void nlCloseFileDebug(void*) {}
bool nlDebugFileIsValid(void*) { return false; }
void nlWriteLineDebug(void*, const char*, bool) {}
int nlSNPrintf(char*, unsigned long, const char*, ...) { return 0; }
void nlInitMemory();
static unsigned char sMem1[1 << 20] __attribute__((aligned(32))), sMem2[1 << 20] __attribute__((aligned(32)));
void nlInitMemory()
{
    gMemoryInitialized = 1;
    StandardAllocator.Initialize(sMem1, sizeof sMem1);
    VirtualAllocator.Initialize(sMem2, sizeof sMem2);
}

#if defined(BREAK) && BREAK == 1
template <class T> struct BrokenAlloc {
    typedef T value_type;
    BrokenAlloc() {}
    template <class U> BrokenAlloc(const BrokenAlloc<U>&) {}
    T* allocate(size_t n) { return static_cast<T*>(::operator new(n * sizeof(T))); }
    void deallocate(T* p, size_t) { ::operator delete(p); }
};
template <class T, class U> bool operator==(const BrokenAlloc<T>&, const BrokenAlloc<U>&) { return true; }
template <class T, class U> bool operator!=(const BrokenAlloc<T>&, const BrokenAlloc<U>&) { return false; }
template <class T> using TestHostVector = std::vector<T, BrokenAlloc<T> >;
#else
template <class T> using TestHostVector = np::HostVector<T>;
#endif

static int sFail;
#define EXPECT(c, what) do { if (!(c)) { printf("FAIL: %s\n", what); ++sFail; } } while (0)

int main()
{
    nlInitMemory();
    setenv("MSC_HEAP_GUARD", "1", 1);
    NpMemSetGameThread(1);
    std::vector<int> plain;
    TestHostVector<int> host;
    np::HostString text;
    for (int i = 0; i < 1000; ++i) { plain.push_back(i); host.push_back(i); text += 'x'; }
    EXPECT(NpHeapGuardInGameHeap(plain.data()) == 1, "a plain vector grown on the game thread is in a game heap");
    EXPECT(NpHeapGuardInGameHeap(host.data()) == 0, "np::HostVector is on the host heap");
    EXPECT(NpHeapGuardInGameHeap(text.data()) == 0, "np::HostString is on the host heap");
    std::vector<int>* scoped;
    {
        NpHostAllocScope scope;
        scoped = new std::vector<int>(500, 7);
        {
            NpHostAllocScope nested;
            int* q = new int[4];
            EXPECT(NpHeapGuardInGameHeap(q) == 0, "nested scope: host heap");
            delete[] q;
        }
        int* r = new int[4];
        EXPECT(NpHeapGuardInGameHeap(r) == 0, "after a nested scope ends the outer one still holds");
        delete[] r;
    }
    EXPECT(NpHeapGuardInGameHeap(scoped) == 0 && NpHeapGuardInGameHeap(scoped->data()) == 0, "made inside NpHostAllocScope: host heap");
    int* after = new int[4];
    EXPECT(NpHeapGuardInGameHeap(after) == 1, "after the scope: the game heap again");
    // frees across heaps: a host block freed on the game thread, a game block freed inside a scope and on another thread's flag
    const unsigned freeBefore = StandardAllocator.TotalFreeMemory();
    delete scoped;
    {
        NpHostAllocScope scope;
        delete[] after;
    }
    EXPECT(StandardAllocator.TotalFreeMemory() > freeBefore, "a game block freed inside a scope returns to its arena");
    // the guard
    np::HeapGuard("plain", plain);
    np::HeapGuard("host", host);
    np::HeapGuard("text", text);
    np::HeapGuard("host", host);   // (registered twice: one entry)
    EXPECT(NpHeapGuardCheck("test") == 1, "the guard finds exactly the one registered container in a game heap");
    NpMemSetGameThread(0);
    std::vector<int> other(100, 1);
    EXPECT(NpHeapGuardInGameHeap(other.data()) == 0, "another thread's operator new is the host heap");
    if (sFail) { printf("%d failure(s)\n", sFail); return 1; }
    puts("heap guard: game-thread containers in the game heap, host containers and scopes on the host heap, frees across both");
    return 0;
}

// Symbols the real nlMemory.cpp / MemAlloc.cpp name since the overlap, sim-thread census and gsplit waves; the units link no player code, so they are off and inert here.
extern "C" {
int NpAllocMask(void) { return 0; }
void NpSimtNoteHostAlloc(void) {}
void NpSimtNoteGameAlloc(void) {}
}
