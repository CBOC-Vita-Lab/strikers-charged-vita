// Shared between the glue's translation units (not a public header). Builtin types only; included after the SDK headers.
#ifndef NP_GX_INTERNAL_H
#define NP_GX_INTERNAL_H
#include "np_gx.h"

namespace np_gx {
int Started();
void MarkStarted();
int ArrayDefaultLe();
int EfbSizeReported();      // np_gx_set_efb_size was called (the SDK layer reports the render mode)
np_gx_present_hook PresentHook();
void ResetCopyTracking();   // GXInit: forget EFB copy destinations
void RememberCopy(void* dest, unsigned int size);  // GXCopyTex: the texture made of the EFB lives at [dest, dest + size)
void StartFailed();         // the implicit renderer start in GXInit failed: say so and stop (nothing could ever be presented)
void DrainRetiredCopies();  // game thread: tell the renderer about copy textures whose memory was reused
// Diagnostic (LOOK, gx_copytrack.cpp): NP_GX_TRACE_BEGIN=<vertex count> NP_GX_TRACE_FRAMES=<a>-<b> print the call stack of every GXBegin with that
// count in those presented frames (finding the game code behind one draw of a vgx draw log). Off: g_traceBegin is 0.
extern unsigned g_traceBegin;
void TraceBegin(unsigned prim, int fmt, unsigned verts);
void TraceFrameTick();      // GXCopyDisp: one presented frame
}

#endif
