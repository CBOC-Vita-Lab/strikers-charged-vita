// gpueff18: two widenings of the crowd trim (MSC_VGX_CROWDTRIM, vgx_backend_gl.cpp "W12 CROWDTRIM") for the held scenes, pure functions (host test
// renderer/tests/run_ctskew_test.sh, mutants VGX_CTSKEW_BREAK=1..4).
//  IndexRange (MSC_VGX_CROWDTRIM_WELD): a display list welded at its promotion (MSC_VGX_DLWELD) keeps each distinct vertex of a group once, but its draw
//    records still name the group's unwelded vertex count (vtxFirst = the group's first vertex, vtxCount = the segment's count before welding), and a
//    later segment of a group can name vertices an earlier one wrote (past its own vtxFirst + vtxCount). The record's range ran past the end of the
//    buffer's CPU copy and the figure pass was not measured ("range"), so in a held scene no crowd quad was cut. With the switch the static draw's
//    vertices are the range its own indices name (the index copy is kept beside the vertex copy): every vertex it can draw, so the trim stays exact.
//  SkewOk / SkewPos (MSC_VGX_CROWDTRIM_SKEW): the held scenes' crowd quads are not parallelograms in position (the fourth corner is off the plane of the
//    other three by up to ~1 % of a side); their texture coordinates are one picture's cell. The quad is drawn as the triangles 0 1 2 and 0 2 3 of its
//    index pattern, each affine. A cut corner is placed on the triangle of the original quad that holds its texture fractions (barycentric), so every
//    cut corner lies on the drawn surface; inside, the new pair of triangles folds along its own diagonal (at most about a quarter of the corner's
//    offset away). NOT exact: single pixels can differ where depth ties or alpha sits at the test's threshold.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#ifndef VGX_CTSKEW_BREAK
#define VGX_CTSKEW_BREAK 0
#endif

namespace vgx {
namespace ctskew {

// The vertices a draw's indices name: idx[off .. off + count) of an index copy of `size` entries -> [first, first + n). False: outside the copy or empty.
inline bool IndexRange(const uint16_t* idx, size_t size, uint32_t off, uint32_t count, uint32_t& first, uint32_t& n) {
    if (count == 0 || static_cast<size_t>(off) + count > size) return false;
    uint32_t mn = 0xFFFFFFFFu, mx = 0;
#if VGX_CTSKEW_BREAK == 1
    if (count > 1) --count;   // mutant: the last index is not looked at
#endif
    for (uint32_t k = 0; k < count; ++k) { const uint32_t v = idx[off + k]; mn = v < mn ? v : mn; mx = v > mx ? v : mx; }
    first = mn;
    n = mx - mn + 1;
    return true;
}
// Without an index copy: the vertices from `first` to the end of a copy of `bytes` (0: none there). A welded group's distinct vertices are one block from
// its first vertex on, and later segments of the group name only those, so this holds every vertex the draw can name (a superset: the box only grows).
inline uint32_t ToEnd(size_t off, uint32_t first, size_t stride, size_t bytes) {
    const size_t start = off + static_cast<size_t>(first) * stride;
    if (stride == 0 || start >= bytes) return 0;
#if VGX_CTSKEW_BREAK == 1
    return static_cast<uint32_t>((bytes - start) / stride) - 1;   // mutant: the copy's last vertex left out
#else
    return static_cast<uint32_t>((bytes - start) / stride);
#endif
}
// The fourth corner a11 is off the parallelogram of a00, a10, a01 by at most tol of the longest side.
inline bool SkewOk(const float* a00, const float* a10, const float* a01, const float* a11, float tol) {
    float side = 0, off = 0;
    for (int j = 0; j < 3; ++j) {
        side = std::max(side, std::max(std::fabs(a10[j] - a00[j]), std::fabs(a01[j] - a00[j])));
        off = std::max(off, std::fabs(a11[j] - (a10[j] + a01[j] - a00[j])));
    }
#if VGX_CTSKEW_BREAK == 2
    return side > 0.0f;   // mutant: any quad, however bent
#else
    return side > 0.0f && std::isfinite(off) && off <= tol * side;
#endif
}
// The position at texture fractions (fx, fy) of the quad's cell (vertex i sits at corner (xi[i], yi[i]) of the unit square) on the triangle of the
// original quad (0 1 2 or 0 2 3) that holds it.
inline void SkewPos(const float pos[4][3], const int* xi, const int* yi, float fx, float fy, float* out) {
#if VGX_CTSKEW_BREAK == 4
    std::swap(fx, fy);   // mutant: the fractions crossed
#endif
    auto bary = [&](int a, int b, int c, float* w) {
        const float ax = static_cast<float>(xi[a]), ay = static_cast<float>(yi[a]), bx = static_cast<float>(xi[b]), by = static_cast<float>(yi[b]), cx = static_cast<float>(xi[c]), cy = static_cast<float>(yi[c]);
        const float d = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy);
        if (d == 0.0f) { w[0] = w[1] = -1.0f; w[2] = 3.0f; return; }
        w[0] = ((by - cy) * (fx - cx) + (cx - bx) * (fy - cy)) / d;
        w[1] = ((cy - ay) * (fx - cx) + (ax - cx) * (fy - cy)) / d;
        w[2] = 1.0f - w[0] - w[1];
    };
    float w[3];
    int t[3] = {0, 1, 2};
    bary(0, 1, 2, w);
#if VGX_CTSKEW_BREAK != 3   // (mutant 3: always the first triangle)
    if (!(w[0] >= -1e-5f && w[1] >= -1e-5f && w[2] >= -1e-5f)) { t[1] = 2; t[2] = 3; bary(0, 2, 3, w); }
#endif
    for (int j = 0; j < 3; ++j) out[j] = w[0] * pos[t[0]][j] + w[1] * pos[t[1]][j] + w[2] * pos[t[2]][j];
}

}  // namespace ctskew
}  // namespace vgx
