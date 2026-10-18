// Wrapper over the decompilation's revolution/gx/GXHardware.h for the native port (found first on the include
// path, then includes the real header): the SDK declares WGPIPE as a variable at the gather pipe's hardware address;
// here it is the gather-pipe stand-in of np_gx_pipe.h. Define NP_GX_NO_PIPE_PROXY to get the SDK's declaration.
#ifndef NP_GX_WRAPPER_GXHARDWARE_H
#define NP_GX_WRAPPER_GXHARDWARE_H
#if defined(__cplusplus) && !defined(NP_GX_NO_PIPE_PROXY)
#define WGPIPE NP_SDK_WGPIPE_DECL_UNUSED
#include_next <revolution/gx/GXHardware.h>
#undef WGPIPE
#include "np_gx_pipe.h"
#else
#include_next <revolution/gx/GXHardware.h>
#endif
// (every unit that draws also gets the glue's array helpers: NP_GX_SETARRAY_STREAM)
#include "np_gx.h"
#endif
