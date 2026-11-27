// galaxyport18 (Galaxy replay36, vgx_galaxy.cpp DanglingCmds): a CMD_COPY_TEX whose arg is not an index into the buffer's copyTex vector (a buffer copied without its sibling vectors,
// a held scene or a replay that lost one) was read past the end of the vector on the render thread. At() is one compare when the record is there; when it is not, the command reads a
// dead copy record (skip set, no texture: CopyTex does nothing but its empty clear), the miss is counted and the first few are logged. Invisible when nothing is dangling.
#pragma once
#include "vgx_cmd.h"
#include <atomic>
#include <cstdio>

namespace vgx::copyguard {

inline std::atomic<unsigned>& Count() { static std::atomic<unsigned> n{0}; return n; }

#if defined(__GNUC__)
__attribute__((noinline, cold))
#endif
inline const CopyTexCmd& Bad(size_t records, size_t arg) {
    static const CopyTexCmd dead = [] { CopyTexCmd c; c.skip = true; return c; }();
    const unsigned n = Count().fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 4) std::fprintf(stderr, "[copyguard] a CMD_COPY_TEX names record %u of %u: skipped (miss %u; the buffer lost its copyTex records)\n", static_cast<unsigned>(arg), static_cast<unsigned>(records), n);
    return dead;
}

inline const CopyTexCmd& At(const CmdBuf& buf, size_t arg) {
    return __builtin_expect(arg < buf.copyTex.size(), 1) ? buf.copyTex[arg] : Bad(buf.copyTex.size(), arg);
}

} // namespace vgx::copyguard
