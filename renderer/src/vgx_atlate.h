// MSC_VGX_ATLATE: alpha-tested draws behind the opaque draws of their window (header-only so a host test runs the shipped code).
//
// On the tile GPU an alpha-tested (discard) draw cannot take part in hidden-surface removal, and the opaque draws that follow it in the command order lose it too: the tile has to
// shade them in submission order. Galaxy measured 30.2 -> 23.6 ms in its worst stage by drawing the alpha-tested draws after the opaque ones. Here the back end gives each command a
// kind and this plan returns the new order:
//   kNeutral  stays where it is (shader, program and variant commands, an upload that no pending late draw reads)
//   kOpaque   an opaque draw: depth test and write, LESS or LEQUAL, no blend, no test: it stays in order
//   kLate     a draw that may sink: it is held back to the end of its window, in its own order
//   kBarrier  anything else (a blended draw, a draw without depth write, a copy, a target change, a free, an upload a pending late draw reads ...): the window ends before it
// Exact apart from fragments whose depth ties exactly (a late draw and an opaque one at the same depth: the last drawn wins with LEQUAL), and, for a blended late draw, the pixels
// where it is nearer than an opaque draw that came after it in the game's order and its soft rim (alpha below 1) now blends with that draw instead of what lay there before.
#pragma once
#include <cstdint>
#include <vector>

namespace vgx {
namespace atlate {

enum Kind : uint8_t { kNeutral = 0, kOpaque = 1, kLate = 2, kBarrier = 3 };

// order[i] = the old index of the command that now stands at position i. Returns the number of late commands that moved (changed position).
inline uint32_t Plan(const std::vector<uint8_t>& kind, std::vector<uint32_t>& order) {
    const uint32_t n = static_cast<uint32_t>(kind.size());
    order.clear();
    order.reserve(n);
    std::vector<uint32_t> late;
    uint32_t moved = 0;
    auto flush = [&]() {
        for (uint32_t k : late) order.push_back(k);
        late.clear();
    };
    for (uint32_t i = 0; i < n; ++i) {
        switch (kind[i]) {
            case kLate:
                late.push_back(i);
                break;
            case kBarrier:
                flush();
                order.push_back(i);
                break;
            case kOpaque:
                order.push_back(i);
                break;
            default:
                order.push_back(i);
                break;
        }
    }
    flush();
    for (uint32_t p = 0; p < n; ++p) if (kind[order[p]] == kLate && order[p] != p) ++moved;
    return moved;
}

}  // namespace atlate
}  // namespace vgx
