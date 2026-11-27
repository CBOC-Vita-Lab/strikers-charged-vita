// MSC_PKT_SORT: np/pktsort.h (stable merge sort, later repeats dropped) against the game's real balanced tree (src/NL/nlAVLTree.cpp, the
// GLPacketTree's AddAVLNode and in-order walk), on random key sets with repeats, sorted and reversed input, and the sizes around the run
// and merge boundaries. Every case must give the same (key, value) sequence. armhf under qemu-arm (run_pktsort_test.py).
#include "NL/nlAVLTree.h"
#include "np/pktsort.h"
#include <stdio.h>
#include <stdlib.h>

namespace {
typedef unsigned long long Key;
struct Ent { Key key; unsigned long seq; unsigned long value; };
template <class T> struct HeapAllocator {
    T* Allocate() { return (T*)calloc(1, sizeof(T)); }
    void Allocate(T*& out) { out = Allocate(); }
    void Delete(T* e) { free(e); }
};
typedef AVLTreeEntry<Key, unsigned long> TreeEntry;
class Tree : public AVLTreeBase<Key, unsigned long, HeapAllocator<TreeEntry>, DefaultKeyCompare<Key> > {};

unsigned long long sRng = 0x9E3779B97F4A7C15ull;
unsigned long long Rand() { sRng ^= sRng << 13; sRng ^= sRng >> 7; sRng ^= sRng << 17; return sRng; }

int sFailures = 0;
void Check(const char* what, Ent* in, unsigned long n) {
    Tree tree;
    for (unsigned long i = 0; i < n; ++i) tree.Add(in[i].key, in[i].value);   // as GLTreePacketSorter::AttachPacket
    Ent* a = (Ent*)malloc((2 * n + 1) * sizeof(Ent));
    for (unsigned long i = 0; i < n; ++i) a[i] = in[i];
    Ent* e = NpPktStableSort(a, a + n, n);
    const unsigned long kept = NpPktDropRepeats(e, n);
    nlAVLTreeIterator<Key, unsigned long, DefaultKeyCompare<Key> > it;
    it.Initialize(tree.m_Root);
    unsigned long k = 0;
    bool ok = true;
    while (it.IsValid()) {
        const TreeEntry* t = it.Current();
        if (k >= kept || t->key != e[k].key || t->value != e[k].value) { ok = false; break; }
        ++k;
        it.Next();
    }
    if (k != kept) ok = false;
    if (!ok) { ++sFailures; printf("FAIL %s n=%lu at %lu (kept %lu)\n", what, n, k, kept); }
    free(a);
}
} // namespace

// What the tree needs from the game's allocator interface (AllocateEntry / DeleteEntry are in the template).
int main() {
    static Ent in[5000];
    unsigned long cases = 0;
    const unsigned long sizes[] = {0, 1, 2, 7, 8, 9, 15, 16, 17, 63, 64, 65, 200, 1000, 4999};
    for (unsigned long si = 0; si < sizeof sizes / sizeof sizes[0]; ++si)
        for (int mode = 0; mode < 6; ++mode)
            for (int rep = 0; rep < 20; ++rep) {
                const unsigned long n = sizes[si];
                for (unsigned long i = 0; i < n; ++i) {
                    Key k;
                    switch (mode) {
                    case 0: k = Rand(); break;                                  // unique (packet addresses differ)
                    case 1: k = Rand() % (n / 4 + 1); break;                    // many repeats (a packet attached again)
                    case 2: k = (Key)i << 32 | (Rand() & 0xFFFF0); break;       // ascending sort keys
                    case 3: k = (Key)(n - i) << 32; break;                      // descending
                    case 4: k = (Rand() % 3) << 32 | (Rand() % 5); break;       // a few keys, heavy repeats
                    default: k = (Key)(Rand() % 16) << 32 | (0x80000000u + (Rand() % 64) * 32); break;   // texture key + address
                    }
                    in[i].key = k;
                    in[i].seq = i;
                    in[i].value = i * 7 + 1;   // tells the first insert of a key from a later one
                }
                Check("random", in, n);
                ++cases;
            }
    printf("pktsort: %lu cases, %d failures\n", cases, sFailures);
    return sFailures != 0;
}
