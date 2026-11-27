// 1.2 CLEARS: the planner for the clears of one flush (vgx_backend_gl.cpp, MSC_VGX_CLEARDEAD / MSC_VGX_CLEARMERGE), kept apart from GL so that a test
// (native/tools/render/run_clearplan_test.sh) can check it against a plain pixel simulation.
#pragma once
#include <cstdint>
#include <vector>
#include "vgx_cmd.h"

namespace vgx::backend {

enum : uint8_t { PL_C = 1, PL_A = 2, PL_Z = 4 }; // colour (rgb), alpha, depth
inline uint8_t ClearPlanes(const ClearSpec& c) { return static_cast<uint8_t>((c.color ? PL_C : 0) | (c.alpha ? PL_A : 0) | (c.depth ? PL_Z : 0)); }
// the same planes of two clears hold the same values
inline bool ClearSameValues(const ClearSpec& a, const ClearSpec& b, uint8_t planes) {
    if ((planes & PL_C) && (a.rgba[0] != b.rgba[0] || a.rgba[1] != b.rgba[1] || a.rgba[2] != b.rgba[2])) return false;
    if ((planes & PL_A) && a.rgba[3] != b.rgba[3]) return false;
    if ((planes & PL_Z) && a.z != b.z) return false;
    return true;
}
struct ClrEnt {
    ClearSpec c;
    EfbRect r;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0; // the pixels it clears in the render target (ClearPixRect)
    uint8_t planes = 0;
    uint8_t fate = 0; // 0 made, 1 dead (a later clear covers it), 2 repeat (an earlier clear left the same), 3 folded into the clear before it
};
inline bool EntCovers(const ClrEnt& a, const ClrEnt& b) { return a.x0 <= b.x0 && a.y0 <= b.y0 && a.x1 >= b.x1 && a.y1 >= b.y1; }
inline bool EntOverlaps(const ClrEnt& a, const ClrEnt& b) { return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1; }
// The planner: what the clears of one flush (the EFB, in order, nothing else between them) can be reduced to. Each rule keeps every pixel of every plane
// exactly as the clears made one after the other leave it.
void PlanClears(std::vector<ClrEnt>& e, bool dead, bool merge, uint32_t* nDead, uint32_t* nRepeat, uint32_t* nFold) {
    const size_t n = e.size();
    if (dead) {
        for (size_t i = 0; i < n; ++i) {
            for (size_t j = i + 1; j < n; ++j) {
                // (j may itself be dead: what covers j covers i as well, rectangles and planes are transitive)
                if (EntCovers(e[j], e[i]) && (e[j].planes & e[i].planes) == e[i].planes) { e[i].fate = 1; ++*nDead; break; }
            }
        }
    }
    if (merge) {
        for (size_t j = 1; j < n; ++j) {
            if (e[j].fate) continue;
            for (size_t i = j; i-- > 0;) {
                if (e[i].fate) continue;
                // the nearest earlier clear that touches j's pixels and planes decides: it either left exactly these values (j repeats it), or not
                const bool touches = EntOverlaps(e[i], e[j]) && (e[i].planes & e[j].planes);
                if (!touches) continue;
                if (EntCovers(e[i], e[j]) && (e[i].planes & e[j].planes) == e[j].planes && ClearSameValues(e[i].c, e[j].c, e[j].planes)) {
                    // (every clear between i and j either misses j's pixels and planes, or was skipped just above: the pixels of j hold i's values)
                    e[j].fate = 2;
                    ++*nRepeat;
                }
                break;
            }
        }
        // neighbours (the live clears in order) with the same rectangle and the same values on the planes both clear
        size_t prev = n;
        for (size_t j = 0; j < n; ++j) {
            if (e[j].fate) continue;
            if (prev < n && e[prev].x0 == e[j].x0 && e[prev].y0 == e[j].y0 && e[prev].x1 == e[j].x1 && e[prev].y1 == e[j].y1 &&
                ClearSameValues(e[prev].c, e[j].c, static_cast<uint8_t>(e[prev].planes & e[j].planes))) {
                ClrEnt& a = e[prev];
                const ClrEnt& b = e[j];
                if (b.c.color) { a.c.rgba[0] = b.c.rgba[0]; a.c.rgba[1] = b.c.rgba[1]; a.c.rgba[2] = b.c.rgba[2]; a.c.color = true; }
                if (b.c.alpha) { a.c.rgba[3] = b.c.rgba[3]; a.c.alpha = true; }
                if (b.c.depth) { a.c.z = b.c.z; a.c.depth = true; }
                a.planes = static_cast<uint8_t>(a.planes | b.planes);
                e[j].fate = 3;
                ++*nFold;
                continue; // (prev stays: the next clear may fold into the merged one as well)
            }
            prev = j;
        }
    }
}


} // namespace vgx::backend
