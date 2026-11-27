// np_fasttls.h (1.3 cpuexact13, MSC_TLS_FAST): the pieces of the Vita's emulated thread-local lookup that do not need the Vita, so a host test can run them against a copy of
// the original code. np_fasttls.cpp wires them to the link wraps (--wrap=__getreent, --wrap=pte_osTlsGetValue, --wrap=__emutls_get_address).
//
// The chain that one `thread_local` read costs on the Vita (VitaSDK 2024+ newlib, libgcc 15.2.0, read from the libraries' disassembly):
//   __emutls_get_address(obj)           libgcc emutls.o: __gthread_active_p, dmb, then
//     -> pthread_getspecific(emutls_key) libpthread: one tail call
//       -> pte_osTlsGetValue(key)        libpthread vita_osal.o
//         -> vitasdk_get_tls_data()      libc threading.o: __getreent_for_thread(0) - 8
//           -> __getreent_for_thread(0)  8 registers saved, 136 bytes of stack, sceKernelGetTLSAddr(0x89), the slot's value
//             -> sceKernelGetTLSAddr(0x89)
// The reent structure of the thread is `*(slot 0x89)`. The 8 bytes in front of it are the head of a singly linked list of {next, key, value} nodes: the pthread-key values of the thread
// (pte_osTlsGetValue walks that list). libgcc's emutls keeps ONE pthread key whose value is the thread's array {size, data[size]}; an object's slot is data[offset-1] = arr[offset].
// Everything below returns the same pointer as the library code for the same memory, or 0 meaning "ask the library" (an unset slot, a short array, an empty entry: the library
// allocates there).
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifndef NP_FASTTLS_KEYEQ
#define NP_FASTTLS_KEYEQ(a, b) ((a) == (b))   // (the host test's mutant compiles it as <=)
#endif

namespace npfasttls {

struct TlsNode {   // vitasdk's per-thread key node: next @0, key @4, value @8 on the Vita
    TlsNode* next;
    uint32_t key;
    void* value;
};

// pte_osTlsGetValue(key) given the thread's reent pointer (the original: vitasdk_get_tls_data = reent - 8, then the loop).
static inline void* PteGet(void* reent, uint32_t key) {
    const TlsNode* n = *reinterpret_cast<TlsNode* const*>(static_cast<char*>(reent) - 2 * sizeof(void*));
    for (; n; n = n->next)
        if (NP_FASTTLS_KEYEQ(n->key, key)) return n->value;
    return nullptr;
}

// libgcc emutls_get_address's fast path for a thread whose array exists and holds the object: `arrv` is the pthread-key value (the thread's array), `offset` the object's
// (obj->loc.offset, 0 until the first thread touched it). 0 = take the library's path (it allocates the array, grows it, or fills the entry).
static inline void* EmutlsPeek(void* arrv, size_t offset) {
    if (offset == 0 || arrv == nullptr) return nullptr;
    void* const* arr = static_cast<void* const*>(arrv);
    if (reinterpret_cast<size_t>(arr[0]) < offset) return nullptr;
    return arr[offset];
}

}  // namespace npfasttls

#if defined(__vita__)
#include <psp2/kernel/threadmgr.h>
extern "C" volatile uintptr_t np_tls_fast_word;
// The address of this thread's TLS slot 0x89 (what sceKernelGetTLSAddr(0x89) returns; a thread's identity token): two instructions once np_tls_fast_init() proved the offset.
static inline void* NpTlsSlot89() {
    const uintptr_t f = np_tls_fast_word;
    if (f != 0) {
        uint32_t tp;
        __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(tp));
        return reinterpret_cast<void*>(static_cast<uintptr_t>(tp) + (f & ~static_cast<uintptr_t>(1)));
    }
    return sceKernelGetTLSAddr(0x89);
}
#endif
