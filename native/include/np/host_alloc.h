// 1.3: storage of the port's own long-lived containers on the host heap (malloc), never in the game's heaps. Ported from the Galaxy
// port's r67 crash fix (platform/include/port/host_vector.h: PortHostVector, PortHostAllocScope, GXV_HEAP_GUARD).
//
// On the game thread the global operator new is the game's allocator (NL/nlMemory.cpp: CurrentAllocator, the arenas the game was
// sized for). A std::vector, std::string or map of the port that grows on the game thread therefore takes its blocks from those
// arenas. In Galaxy a scene's heap was freed under such lists and the next scene's objects were overwritten (r67). Strikers' game
// never frees an arena (StandardAllocator and VirtualAllocator live for the whole run, nothing else is ever made current), so that
// crash cannot happen here as such; what remains is:
//  - port blocks interleaved with the game's own in arenas sized for the game (fragmentation, a smaller largest free block late in
//    a session, where the game's own allocations then fail);
//  - a game-heap layout that depends on port-side work. The simulation must not depend on heap addresses, and the determinism
//    tests check it does not (NP_HEAP_FLIP), but a layout that moves with what was drawn, logged or rendered is one more way two
//    consoles could differ. Port storage on the host heap keeps the game's heap exactly the game's.
//
//   np::HostAllocator<T>  an allocator for std containers: malloc/free, whatever thread grows them.
//   np::HostVector<T>     std::vector<T, np::HostAllocator<T>>.
//   np::HostString        std::basic_string on the host heap.
//   NpHostAllocScope      RAII: inside it the calling thread's operator new comes from the host heap (for code that builds
//                         std:: objects it does not own the allocator of: std::string temporaries, library calls). Frees are unchanged.
//   NpGameAllocScope      RAII, inside a host scope: a call into game code that makes game objects goes back to the game heap.
//   NpHeapGuardRegister   MSC_HEAP_GUARD=1: registered long-lived storage is checked at every stadium destroy; any block of it
//                         inside a game arena is logged ("[heapguard]"). MSC_HEAP_GUARD=2 (host builds with <execinfo.h>):
//                         also a census of the game thread's operator new calls by call stack, printed at exit, to find port
//                         code that allocates from the game heap (tools/heapguard/census.py symbolises it).
#pragma once
// (the functions are weak: tools that link port code without NL/nlMemory.cpp keep plain malloc storage and no guard)
// (C++ containers bring <new>, whose placement new clashes with NL/nlMemory.h's: include this in port files, not in a file that includes NL/nlMemory.h)

#include <cstddef>
#include <cstdlib>
#include <string>
#include <vector>

// MSC_HEAP_GUARD (NL/nlMemory.cpp). data(obj) returns the storage's first block (0 when it has none).
extern "C" void NpHeapGuardRegister(const char* name, const void* (*data)(const void* obj), const void* obj) __attribute__((weak));
extern "C" int NpHeapGuardCheck(const char* when) __attribute__((weak));   // returns how many registered blocks lie in a game arena (logged)
extern "C" int NpHeapGuardInGameHeap(const void* p) __attribute__((weak)); // 1 when p lies inside StandardAllocator's or VirtualAllocator's arena
extern "C" int NpHeapGuardMode(void) __attribute__((weak));                // 0 off, 1 guard, 2 guard + census
extern "C" void NpHostHeapExhausted(std::size_t size) __attribute__((weak, noreturn));   // the FATAL line of a failed host-heap request

extern "C" int NpMemHostScope(int delta) __attribute__((weak));        // NL/nlMemory.cpp: NpHostAllocScope's per-thread count (returns it before the change)
// (only operator new changes inside a scope; frees keep the game thread's rules, the frame-send deferral included)
class NpHostAllocScope {
public:
    NpHostAllocScope() { if (NpMemHostScope) NpMemHostScope(1); }
    ~NpHostAllocScope() { if (NpMemHostScope) NpMemHostScope(-1); }
    NpHostAllocScope(const NpHostAllocScope&) = delete;
    NpHostAllocScope& operator=(const NpHostAllocScope&) = delete;
};
// Inside an NpHostAllocScope: a call into game code that allocates game objects (which the game may later free with nlFree) leaves the scope for its length.
class NpGameAllocScope {
public:
    NpGameAllocScope() : saved_(NpMemHostScope ? NpMemHostScope(0) : 0) { if (saved_) NpMemHostScope(-saved_); }
    ~NpGameAllocScope() { if (saved_) NpMemHostScope(saved_); }
    NpGameAllocScope(const NpGameAllocScope&) = delete;
    NpGameAllocScope& operator=(const NpGameAllocScope&) = delete;
private:
    int saved_;
};

namespace np {

template <class T> struct HostAllocator {
    typedef T value_type;
    HostAllocator() noexcept {}
    template <class U> HostAllocator(const HostAllocator<U>&) noexcept {}
    T* allocate(std::size_t n) {
        void* p = std::malloc(n * sizeof(T) ? n * sizeof(T) : 1);
        if (p == nullptr) {
            if (NpHostHeapExhausted) NpHostHeapExhausted(n * sizeof(T));
            std::abort();
        }   // (as operator new on the host heap: one FATAL line, then the end)
        return static_cast<T*>(p);
    }
    void deallocate(T* p, std::size_t) noexcept { std::free(p); }
    template <class U> struct rebind { typedef HostAllocator<U> other; };
};
template <class T, class U> inline bool operator==(const HostAllocator<T>&, const HostAllocator<U>&) { return true; }
template <class T, class U> inline bool operator!=(const HostAllocator<T>&, const HostAllocator<U>&) { return false; }

template <class T> using HostVector = std::vector<T, HostAllocator<T>>;
typedef std::basic_string<char, std::char_traits<char>, HostAllocator<char>> HostString;

// (MSC_HEAP_GUARD=1: register a long-lived container by its address once, e.g. from its owner's first use)
template <class C> inline const void* HeapGuardData(const void* obj) { return static_cast<const C*>(obj)->data(); }
template <class C> inline void HeapGuard(const char* name, const C& c) { if (NpHeapGuardRegister) NpHeapGuardRegister(name, &HeapGuardData<C>, &c); }

} // namespace np
