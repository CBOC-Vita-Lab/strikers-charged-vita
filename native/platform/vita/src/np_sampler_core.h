// diag13 (MSC_SAMPLER): the pure parts of the on-console pc sampler (vita_sampler.cpp), so a host test can run them: a fixed-size pc histogram (no allocation), the top-N pick, and the finder of
// the register words in the raw thread context that sceKernelGetThreadContextForVM returns (undocumented layout; calibrated at start with a thread spinning in a known function, the way the
// NFS Underground 2 Vita port does it, danos-x5/NFSUG2_VITA src/platform/sampler.cpp, MIT licence: see THIRD_PARTY_NOTICES.md).
#pragma once
#include <cstddef>
#include <cstdint>

namespace NpSampler {

// Open-addressing table of (key, count): N a power of two. No allocation, no locks: one writer (the sampler thread).
template <size_t N>
struct Table {
    static_assert((N & (N - 1)) == 0, "N must be a power of two");
    uint32_t key[N];
    uint32_t cnt[N];   // 0: slot free
    uint32_t used = 0, overflow = 0, total = 0;
    void Reset() { for (size_t i = 0; i < N; ++i) { key[i] = 0; cnt[i] = 0; } used = overflow = total = 0; }
    // Count one sample of `k`. A full table (3/4 used) counts the sample in `overflow` only.
    void Add(uint32_t k) {
        ++total;
        size_t i = (k * 2654435761u) >> 7 & (N - 1);
        for (size_t probe = 0; probe < N; ++probe, i = (i + 1) & (N - 1)) {
            if (cnt[i] == 0) {
                if (used >= N / 4 * 3) { ++overflow; return; }
                key[i] = k; cnt[i] = 1; ++used;
                return;
            }
            if (key[i] == k) { ++cnt[i]; return; }
        }
        ++overflow;
    }
    // The `n` largest counts, largest first, into idx[] (slot numbers). Returns how many were found (fewer when the table holds fewer keys). Ties: the lower key first, so a run is repeatable.
    size_t Top(size_t* idx, size_t n) const {
        size_t got = 0;
        for (size_t i = 0; i < N; ++i) {
            if (cnt[i] == 0) continue;
            size_t at = got;
            while (at > 0 && (cnt[idx[at - 1]] < cnt[i] || (cnt[idx[at - 1]] == cnt[i] && key[idx[at - 1]] > key[i]))) --at;
            if (at >= n) continue;
            const size_t last = got < n ? got : n - 1;
            for (size_t j = last; j > at; --j) idx[j] = idx[j - 1];
            idx[at] = i;
            if (got < n) ++got;
        }
        return got;
    }
};

struct Layout {
    int pc = -1, lr = -1;
    bool Ok() const { return pc >= 0 && lr >= 0 && pc != lr; }
};

// ctx: the words the kernel wrote. The calibration thread was suspended inside the function [spinLo, spinHi) with its lr register holding `lrSentinel`. The pc word is the first word whose value
// (thumb bit cleared) lies in the function; the lr word is the one holding the sentinel.
inline Layout FindLayout(const uint32_t* ctx, size_t words, uintptr_t spinLo, uintptr_t spinHi, uint32_t lrSentinel) {
    Layout l;
    for (size_t i = 0; i < words; ++i) {
        const uintptr_t w = static_cast<uintptr_t>(ctx[i] & ~1u);
        if (l.pc < 0 && w >= spinLo && w < spinHi) l.pc = static_cast<int>(i);
        if (l.lr < 0 && ctx[i] == lrSentinel) l.lr = static_cast<int>(i);
    }
    return l;
}

}  // namespace NpSampler
