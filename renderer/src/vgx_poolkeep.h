// MSC_POOL_KEEP / MSC_POOL_TRIM_ONE: the rules of TrimPool (vgx_backend_gl.cpp) that decide WHEN an unused pass object or kept render target is let go. Header-only and pure so a host test
// (tests/pool_keep.cpp) runs the shipped decisions. Everything is counted in presents.
//   age     how long an object may stay unused: 2 presents after a size change when the video memory is not roomy (the old fast trim), 60 when it is not roomy, and with room (24 MiB free)
//           `keep` presents (the old rule was a fixed 600, ten seconds at 60: the size a replay or the next goal used was gone by then and was made again, 80-190 ms on the console).
//   quota   how many objects (and, separately, kept targets) one present may let go: one unless the memory is tight and MSC_POOL_TRIM_ONE is on, so a trim is never a lump of deletions in
//           one frame; unlimited when it is tight (memory pressure wants them back at once).
#pragma once
#include <cstdint>

namespace vgx::poolkeep {

inline uint32_t Age(bool fastTrim, bool roomy, bool keepSizes, uint32_t keep) {
    return (fastTrim && !roomy) ? 2u : (roomy && keepSizes ? keep : 60u);
}
// int28/int29: smooth tiered retention by free VRAM (KiB) with 2 MiB hysteresis band so dipping slightly under 16/12 MiB never snaps or thrashes idle keep every frame.
//   tier 2 (roomy): enter >= 18 MiB (or unknown < 0), stay until < 16 MiB -> full `keep` (18000)
//   tier 1 (mid):   enter >= 14 MiB, stay until < 12 MiB -> 1800 presents (60 s at 30 FPS), or 60 on fastTrim
//   tier 0 (tight): < 12 MiB -> 60 presents, or 2 on fastTrim (PoolEmergencyTrim still guards actual OOM immediately)
inline uint32_t AgeByFreeKibHyst(bool fastTrim, int freeKib, bool keepSizes, uint32_t keep, int& tier) {
    if (freeKib < 0) { tier = 2; return keepSizes ? keep : 60u; }
    if (tier < 0) tier = freeKib >= 16384 ? 2 : (freeKib >= 12288 ? 1 : 0);
    if (tier == 2 && freeKib < 16384) tier = freeKib < 12288 ? 0 : 1;
    else if (tier == 1) {
        if (freeKib >= 18432) tier = 2;
        else if (freeKib < 12288) tier = 0;
    } else if (tier == 0 && freeKib >= 14336) {
        tier = freeKib >= 18432 ? 2 : 1;
    }
    if (tier == 2) return keepSizes ? keep : 60u;
    if (tier == 1) {
        if (fastTrim || !keepSizes) return 60u;
        return keep < 1800u ? keep : 1800u;
    }
    return fastTrim ? 2u : 60u;
}
inline uint32_t AgeByFreeKib(bool fastTrim, int freeKib, bool keepSizes, uint32_t keep) {
    int tier = -1;
    return AgeByFreeKibHyst(fastTrim, freeKib, keepSizes, keep, tier);
}
inline bool Due(uint32_t now, uint32_t lastUse, uint32_t age) { return now - lastUse > age; }
// tight: the video memory is short (under 12 MiB free): then everything due goes at once. Between 12 and 24 MiB (not roomy, not tight) the old ages apply but one object a present is
// enough: keeping sizes (MSC_POOL_KEEP) can bring the free figure under 24 MiB, and the first present to see it must not delete everything idle in one lump.
inline uint32_t Quota(bool tight, bool trimOne) { return (!tight && trimOne) ? 1u : 0xFFFFFFFFu; }
// the kept render targets of the EFB table: with room they stay at least as long as the old rule's 1800 presents
inline uint32_t KeptTargetAge(uint32_t keep) { return keep > 1800u ? keep : 1800u; }
// the keep setting: 0 or less means the old 600
inline uint32_t KeepFromSetting(long v) { return v <= 0 ? 600u : static_cast<uint32_t>(v); }

// A target could not be made (no memory, no name): every pooled object that is alive and was not used in this present's own frame goes at once, whatever its age and
// the quota, and the allocation is tried again in the same frame. Objects used this present stay: the scene being recorded may still name them.
inline bool EmergencyDue(uint32_t now, uint32_t lastUse, bool alive) { return alive && lastUse != now; }

} // namespace vgx::poolkeep
