#pragma once
#include <stddef.h>
#include <stdlib.h>
#include <vector>
// MSC_DM_COMPACT (nxvis25, default ON, =0 the plain vectors): the shader tables are indexed by the shader id, and the derived programs of MSC_VGX_GPU_BISECT / the Dump material (dumpgpu25) have ids from 0xE000 / 0xF000,
// so one derived program made the plain vectors grow to 57k-61k entries (g_fs 4 + g_fsSrc 4 + g_fsAttempts 16 bytes each on the Vita: about 1.4 MiB of the C heap). The same ids, the same programs and the same
// reads: only the storage of an index from 0xE000 up is a second small vector. An element never written reads as the zero value, as in the plain vector.
template <class T> class ShaderTab {
public:
    static size_t Split() {   // (the test builds with SHADERTAB_SPLIT)
#ifdef SHADERTAB_SPLIT
        return SHADERTAB_SPLIT;
#else
        static const size_t v = [] { const char* e = getenv("MSC_DM_COMPACT"); return (e && *e == '0') ? ~static_cast<size_t>(0) : static_cast<size_t>(0xE000); }(); return v;
#endif
    }
    // int26b (real-Vita 6407 sampler: ShaderTab::operator[] was 2.3% of Core 1 due to Split()'s __cxa_guard_acquire + Grow() on every read):
    // lo_ never stores indices >= Split(), so i < lo_.size() is a 2-instruction fast path with zero Split() or Grow() calls.
    size_t size() const { return __builtin_expect(hi_.empty(), 1) ? lo_.size() : splitCache_ + hi_.size(); }
    void resize(size_t n, const T& v = T()) { if (n > 0) GrowSlow(n - 1, v); }
    T& operator[](size_t i) {
        if (__builtin_expect(i < lo_.size(), 1)) return lo_[i];
        return SlowMut(i);
    }
    const T& operator[](size_t i) const {
        if (__builtin_expect(i < lo_.size(), 1)) return lo_[i];
        return SlowConst(i);
    }
    const T& get(size_t i) const {
        if (__builtin_expect(i < lo_.size(), 1)) return lo_[i];
        return SlowConst(i);
    }
    template <class F> void each(F f) const { for (const T& x : lo_) f(x); for (const T& x : hi_) f(x); }
    void clear() { lo_.clear(); hi_.clear(); }
    size_t stored() const { return lo_.size() + hi_.size(); }
private:
    size_t ResolveSplit() {
        if (!splitInit_) { splitCache_ = Split(); splitInit_ = true; }
        return splitCache_;
    }
    __attribute__((noinline)) void GrowSlow(size_t i, const T& v) {
        const size_t sp = ResolveSplit();
        if (i < sp) { if (lo_.size() <= i) lo_.resize(i + 1, v); }
        else if (hi_.size() <= i - sp) hi_.resize(i - sp + 1, v);
    }
    __attribute__((noinline)) T& SlowMut(size_t i) {
        if (splitInit_ && i >= splitCache_ && i - splitCache_ < hi_.size()) return hi_[i - splitCache_];
        GrowSlow(i, T());
        return i < splitCache_ ? lo_[i] : hi_[i - splitCache_];
    }
    __attribute__((noinline)) const T& SlowConst(size_t i) const {
        if (hi_.empty()) return zero_;
        return (i >= splitCache_ && i - splitCache_ < hi_.size()) ? hi_[i - splitCache_] : zero_;
    }
    std::vector<T> lo_, hi_;
    size_t splitCache_ = 0;
    bool splitInit_ = false;
    T zero_{};
};
