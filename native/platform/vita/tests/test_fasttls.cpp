// Host test of np_fasttls.h (MSC_TLS_FAST): the pthread-key list walk and the emutls array peek, against line-by-line copies of the library code as read from the Vita's
// libraries' disassembly (libpthread vita_osal.o pte_osTlsGetValue + libc threading.o vitasdk_get_tls_data; libgcc 15.2.0 emutls.o __emutls_get_address). Random lists (0..24 nodes,
// duplicate keys, keys 0 and 0xFFFFFFFF), random arrays, every offset around the size. Mutant: -DNP_FASTTLS_KEYEQ(a,b)='(a)<=(b)' must fail.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "../src/np_fasttls.h"
using namespace npfasttls;

// the original, as the disassembly reads: r0 = vitasdk_get_tls_data() = reent - 8; the first load takes the list head, then compare [node+4] with the key, [node+0] is next, [node+8] the value
static void* RefPte(void* reent, uint32_t key) {
    char* p = static_cast<char*>(reent) - 2 * sizeof(void*);   // (8 on the Vita)
    TlsNode* node = *reinterpret_cast<TlsNode**>(p);
    while (node != nullptr) {
        if (node->key == key) return node->value;
        node = node->next;
    }
    return nullptr;
}
// libgcc emutls_get_address after __gthread_active_p: offset = obj->loc.offset; arr = getspecific(key); if (arr == 0 -> alloc) ; if (arr->size < offset) grow; ptr = arr->data[offset-1]; if (!ptr) alloc.
// Returns 0 where the library would allocate or grow.
static void* RefEmutls(void* arrv, size_t offset) {
    if (offset == 0) return nullptr;                    // not assigned yet: emutls_alloc / init path
    if (arrv == nullptr) return nullptr;                // no array yet: calloc
    size_t* arr = static_cast<size_t*>(arrv);
    if (arr[0] < offset) return nullptr;                // array too small: realloc
    void** data = reinterpret_cast<void**>(arr + 1);
    return data[offset - 1];                            // null: emutls_alloc
}

static uint64_t s = 0x9E3779B97F4A7C15ull;
static uint32_t Rnd() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return static_cast<uint32_t>(s >> 16); }

int main() {
    long checks = 0, bad = 0;
    for (int iter = 0; iter < 200000; ++iter) {
        // a thread's reent block: [head ptr][pad][reent...]; nodes allocated separately
        std::vector<TlsNode> nodes(Rnd() % 25);
        for (size_t i = 0; i < nodes.size(); ++i) {
            const uint32_t k = Rnd() % 4 == 0 ? (Rnd() % 2 ? 0u : 0xFFFFFFFFu) : Rnd() % 40;
            nodes[i].key = k;
            nodes[i].value = reinterpret_cast<void*>(static_cast<uintptr_t>(Rnd()) | 1);
            nodes[i].next = i + 1 < nodes.size() ? &nodes[i + 1] : nullptr;
        }
        std::vector<void*> block(8, nullptr);   // block[0..1] = the 8 bytes in front of the reent, then the reent
        block[0] = nodes.empty() ? nullptr : &nodes[0];
        void* reent = &block[2];
        for (int q = 0; q < 50; ++q) {
            const uint32_t key = Rnd() % 8 == 0 ? (Rnd() % 2 ? 0u : 0xFFFFFFFFu) : Rnd() % 45;
            ++checks;
            if (PteGet(reent, key) != RefPte(reent, key)) ++bad;
        }
        // emutls arrays
        const size_t size = Rnd() % 20;
        std::vector<size_t> arr(1 + 24, 0);
        arr[0] = size;
        for (size_t i = 1; i <= 24; ++i) arr[i] = Rnd() % 3 == 0 ? 0 : static_cast<size_t>(Rnd()) | 4;
        for (int q = 0; q < 30; ++q) {
            const size_t off = Rnd() % 24;
            void* a = Rnd() % 20 == 0 ? nullptr : static_cast<void*>(arr.data());
            ++checks;
            if (EmutlsPeek(a, off) != RefEmutls(a, off)) ++bad;
        }
    }
    printf("fasttls: %ld checks, %ld mismatches\n", checks, bad);
    return bad == 0 ? 0 : 1;
}
