// MSC_VGX_DLWELD (code default 0, Vita plan 0 until a console run; port of Galaxy's GXV_DLWELD): at the promotion of a display list each distinct
// vertex (the same decoded bytes) of a group of consecutive primitives with one layout is written once into the static buffer and the strip / fan
// indices are remapped to it. Exact: the vertices are byte for byte those of the unwelded buffer, the primitives, their order and winding are the
// same, so the same triangles reach the rasteriser; only the vertex shader runs fewer times. Once per list at the promotion, never per picture.
// Not for lists that keep their decoded copy (MSC_VGX_PALMERGE / SKINMERGE / SBATCH read e.verts + sg.offset) nor MSC_VGX_HASH=2.
#pragma once
#include <algorithm>
#include <ctime>
#if defined(__vita__)
extern "C" unsigned int sceKernelGetProcessTimeLow(void);
#endif
#include <cstdint>
#include <cstring>
#include <vector>
#ifndef XXH_INLINE_ALL
#define XXH_INLINE_ALL
#endif
#include <xxhash.h>

namespace vgx {
namespace dlweld {
struct Totals {
    uint64_t lists = 0, vertsIn = 0, vertsOut = 0, bytesSaved = 0, micros = 0, groups = 0;
    uint64_t sig = 0;   // nxfly25: sum over the welded lists of a hash of their welded vertex and index bytes (order does not matter: the same lists welded at other pictures give the same sum)
};
inline Totals g_t;
// The distinct vertices of one group: an open-addressing table of slots in the new buffer, emptied by a stamp per group.
struct Welder {
    std::vector<uint32_t> stamp, slot;
    uint32_t mask = 0, cur = 0, nextSlot = 0;
    void Init(uint32_t maxVerts) {
        uint32_t cap = 256;
        while (cap < maxVerts * 2u) cap <<= 1;
        if (stamp.size() < cap) { stamp.assign(cap, 0); slot.assign(cap, 0); cur = 0; }
        // int28: cap-sized mask keeps small display lists inside L1/L2 cache instead of scattering across a 512 KiB table grown by a giant list.
        mask = cap - 1u;
    }
    void NewGroup() {
        if (++cur == 0) { std::fill(stamp.begin(), stamp.end(), 0u); cur = 1; }
        nextSlot = 0;
    }
    static inline uint32_t HashVert(const uint8_t* v, uint32_t stride) {
        // int28: fast 32-bit FNV1a + finalizer over 32-bit words (MakeLayout strides are 4-byte aligned >= 16; tail loop handles any odd stride).
        uint32_t h = 2166136261u ^ stride;
        const uint32_t words = stride >> 2;
        for (uint32_t i = 0; i < words; ++i) {
            uint32_t w;
            std::memcpy(&w, v + (i << 2), 4);
            h ^= w;
            h *= 16777619u;
        }
        for (uint32_t i = words << 2; i < stride; ++i) {
            h ^= v[i];
            h *= 16777619u;
        }
        h ^= h >> 16;
        h *= 0x85ebca6bu;
        h ^= h >> 13;
        return h;
    }
    // Fast pre-sized cursor path (int28): zero software __aeabi_uidiv and zero per-vertex vector::insert().
    uint32_t Find(uint8_t* outData, size_t& outPos, size_t block, const uint8_t* v, uint32_t stride) {
        uint32_t h = HashVert(v, stride) & mask;
        for (;;) {
            if (stamp[h] != cur) {
                const uint32_t n = nextSlot++;
                std::memcpy(outData + outPos, v, stride);
                outPos += stride;
                stamp[h] = cur;
                slot[h] = n;
                return n;
            }
            if (std::memcmp(outData + block + static_cast<size_t>(slot[h]) * stride, v, stride) == 0) return slot[h];
            h = (h + 1u) & mask;
        }
    }
    // Legacy vector-append overload kept for unit tests.
    uint32_t Find(std::vector<uint8_t>& out, size_t block, const uint8_t* v, uint32_t stride) {
        uint32_t h = HashVert(v, stride) & mask;
        for (;;) {
            if (stamp[h] != cur) {
                const uint32_t n = nextSlot++;
                out.insert(out.end(), v, v + stride);
                stamp[h] = cur;
                slot[h] = n;
                return n;
            }
            if (std::memcmp(out.data() + block + static_cast<size_t>(slot[h]) * stride, v, stride) == 0) return slot[h];
            h = (h + 1u) & mask;
        }
    }
};
// Microseconds: the console's process timer (steady_clock costs a system call there); on the host the thread's CPU time (the virtual clock of the test runs does not move).
inline uint64_t NowUs() {
#if defined(__vita__)
    return sceKernelGetProcessTimeLow();
#else
    struct timespec t;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t) != 0) return 0;
    return static_cast<uint64_t>(t.tv_sec) * 1000000ull + static_cast<uint64_t>(t.tv_nsec) / 1000u;
#endif
}
}  // namespace dlweld
}  // namespace vgx
