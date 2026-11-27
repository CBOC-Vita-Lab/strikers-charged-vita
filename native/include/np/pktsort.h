// MSC_PKT_SORT (src/NL/gl/glRenderList.cpp): the sort that gives a packet sorter's list the order of the balanced tree it replaces.
// The tree (NL/nlAVLTree.h, AddAVLNode) walks in key order and, for a key inserted twice, keeps the first insert. Here: a stable merge
// sort by key (equal keys stay in insertion order), then the later repeats of a key are dropped. E has a `key` member with < and !=.
// Plain code (no <algorithm>: in the game's sources its <new> clashes with the game's own operator new). Test: tools/gamefast/pktsort_test.cpp.
#pragma once
#include <string.h>

// a[0..n) is sorted using b[0..n) as the second half; returns the buffer (a or b) that holds the result.
template <class E>
E* NpPktStableSort(E* a, E* b, unsigned long n)
{
    const unsigned long kRun = 8;
    for (unsigned long lo = 0; lo < n; lo += kRun)   // runs of 8 by insertion
    {
        const unsigned long hi = lo + kRun < n ? lo + kRun : n;
        for (unsigned long i = lo + 1; i < hi; ++i)
        {
            const E v = a[i];
            unsigned long j = i;
            while (j > lo && v.key < a[j - 1].key)
            {
                a[j] = a[j - 1];
                --j;
            }
            a[j] = v;
        }
    }
    for (unsigned long w = kRun; w < n; w *= 2)   // merges, from one half of the buffer into the other
    {
        for (unsigned long lo = 0; lo < n; lo += 2 * w)
        {
            const unsigned long mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n;
            if (mid == hi || !(a[mid].key < a[mid - 1].key))
            {
                memcpy(b + lo, a + lo, (hi - lo) * sizeof(E));   // (already in order)
                continue;
            }
            unsigned long i = lo, j = mid, k = lo;
            while (i < mid && j < hi)
                b[k++] = a[j].key < a[i].key ? a[j++] : a[i++];   // (equal keys: the left one first)
            while (i < mid)
                b[k++] = a[i++];
            while (j < hi)
                b[k++] = a[j++];
        }
        E* const t = a;
        a = b;
        b = t;
    }
    return a;
}

// e[0..n) sorted by NpPktStableSort: keeps the first of each run of equal keys, returns the count kept.
template <class E>
unsigned long NpPktDropRepeats(E* e, unsigned long n)
{
    unsigned long kept = 0;
    for (unsigned long i = 0; i < n; ++i)
        if (kept == 0 || e[i].key != e[kept - 1].key)
            e[kept++] = e[i];
    return kept;
}
