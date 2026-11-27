// MSC_VGX_NODEPTH_PASS (wave 11, vitaGL patch 0038): no depth or stencil memory for pass objects whose scenes never test or write depth.
// vitaGL gave every framebuffer object a DF32M depth surface (4 bytes a sample, aligned to 32x32 tiles) at its first scene, whatever the GL side attached; a copy target or a filter
// pass of 990x1139 held 4.3 MiB it never used. With the switch on, such objects are made with vglFramebufferNoDepth(fb, 1): their scenes run with a surface that has no memory (tile
// memory only, nothing loaded or stored). Header-only and pure: the decisions of the pass-object pool (vgx_backend_gl.cpp AcquireFboOnce) so a host test (tests/nodepth.cpp) runs them.
//   level    0 off (code default and Vita plan), 1 passes and copy targets that never use depth. (The plan's level 2, depth used inside a scene but never loaded or stored, is not built.)
//   qualify  a copy target (wanted without depth on the GL side), or a pass whose PassCmd::planes has no depth bit (the planes the pass uses at all).
//   match    a pooled object may serve an acquire unless it has no depth memory and the acquire needs it: an object with depth memory serves any acquire; one without serves only
//            qualifying ones. A qualifying acquire takes an object without depth memory first.
#pragma once
#include <cstdint>

namespace vgx::nodepth {

inline int LevelFromSetting(long v) { return v <= 0 ? 0 : 1; }
inline bool QualifiesPass(uint8_t planes) { return (planes & 4u) == 0; }               // PassCmd::planes: 1 colour, 2 alpha, 4 depth
inline bool Qualifies(int level, bool wantDepth, bool passHasNoDepthPlane) { return level >= 1 && (!wantDepth || passHasNoDepthPlane); }
inline bool Accepts(bool objNoDepth, bool acquireNoDepthOk) { return acquireNoDepthOk || !objNoDepth; }
inline bool Preferred(bool objNoDepth, bool acquireNoDepthOk) { return acquireNoDepthOk && objNoDepth; }
// the memory an object's depth surface takes on the console: 4 bytes a sample (5 with stencil), width and height aligned to the 32x32 tile, times the samples
inline uint64_t DepthBytes(int w, int h, int samples, bool stencil) {
    const uint64_t aw = (static_cast<uint64_t>(w) + 31u) & ~31ull, ah = (static_cast<uint64_t>(h) + 31u) & ~31ull;
    return aw * ah * static_cast<uint64_t>(samples < 1 ? 1 : samples) * (stencil ? 5u : 4u);
}

} // namespace vgx::nodepth
