// 1.3 (wave 10, warm10) the shader warm-up's decision, as plain code so that a host test can run them without a GL:
//
//  Stop(): may the warm-up compile / link one more thing now? The old rule was an absolute figure (MSC_VGX_WARM_HEAP_MIB, 44 MiB of C heap in use), written for a 64 MiB heap. The heap is
//     96 MiB now and the game's own start-up leaves 48-52 MiB in use before the first picture, so the figure was passed at picture 0 on every build and the warm-up never ran. The rule is
//     now relative: stop only when the FREE C heap (heap size minus use) is under a floor (MSC_VGX_WARM_FLOOR_MIB). An explicit MSC_VGX_WARM_HEAP_MIB keeps its old meaning (> 0: stop at
//     that use; 0: never stop).
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vgx::warm {

enum class Why : uint8_t { Go = 0, Absolute = 1, Floor = 2 };

// absMib: MSC_VGX_WARM_HEAP_MIB (negative: not set, 0: never stop, > 0: stop at that many MiB in use); floorMib: MSC_VGX_WARM_FLOOR_MIB (<= 0: no floor).
// haveUsed / haveTotal: the platform has the accessors (a host build has neither and never stops).
inline Why Stop(long absMib, long floorMib, unsigned usedKiB, unsigned totalKiB, bool haveUsed, bool haveTotal) {
    if (absMib == 0 || !haveUsed) return Why::Go;
    if (absMib > 0) {
#if defined(VGX_WARM_BREAK) && VGX_WARM_BREAK == 1
        return Why::Go;   // deliberate break: the absolute figure is ignored
#else
        return usedKiB >= static_cast<uint64_t>(absMib) * 1024u ? Why::Absolute : Why::Go;
#endif
    }
    if (floorMib <= 0 || !haveTotal) return Why::Go;
    const uint64_t freeKiB = totalKiB > usedKiB ? totalKiB - usedKiB : 0;
#if defined(VGX_WARM_BREAK) && VGX_WARM_BREAK == 2
    return freeKiB <= static_cast<uint64_t>(floorMib) * 1024u / 2u ? Why::Floor : Why::Go;   // deliberate break: the floor is half what it says
#else
    return freeKiB < static_cast<uint64_t>(floorMib) * 1024u ? Why::Floor : Why::Go;
#endif
}

}  // namespace vgx::warm
