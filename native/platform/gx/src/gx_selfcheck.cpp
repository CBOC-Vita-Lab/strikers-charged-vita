// Self-check of the glue's restated interfaces: this unit includes the renderer library's real headers next to the
// restatements (no SDK headers, so the standard headers are fine). A drift between them fails the build here.
#include <cstddef>
#include <cstdint>

#include "np_gx.h"
#include "vgx_native.h"   // restated vgx_native.h: redeclarations with different types would not compile together
#include "vgx_hle.h"      // vgx::hle::RawCursor, g_rawCur, FinishRaw
#include "vgx_native.h"   // the real declarations of the vgxn_* functions

namespace restated {
#include "np_gx_pipe.h"   // the WGPIPE stand-in's restatement of vgx::hle (now restated::vgx::hle)
}

static_assert(sizeof(restated::vgx::hle::RawCursor) == sizeof(vgx::hle::RawCursor), "RawCursor size");
static_assert(offsetof(restated::vgx::hle::RawCursor, ptr) == offsetof(vgx::hle::RawCursor, ptr), "RawCursor::ptr");
static_assert(offsetof(restated::vgx::hle::RawCursor, end) == offsetof(vgx::hle::RawCursor, end), "RawCursor::end");
