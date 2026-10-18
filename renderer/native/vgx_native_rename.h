// Force-included (-include) when src/vgx_api.cpp is compiled for the native library.
//
// vgx_api.cpp implements the Dolphin-SDK-named GX API. The native game links against the Wii SDK's names and
// layouts; for almost every function the two agree and the game's call reaches vgx_api.cpp unchanged (see
// np/platform/gx/tools/gx_abi.py). The functions below are the ones the native glue (np/platform/gx) defines
// itself, with the game's signature and semantics, on top of the C interface in vgx_native.h: their vgx_api.cpp
// versions are kept under the prefix vgxapi_ (still reachable, e.g. for the standalone tests) so that the
// two definitions do not clash at link time.
#pragma once
#define GXInit vgxapi_GXInit
#define GXSetArray vgxapi_GXSetArray
#define GXBegin vgxapi_GXBegin
#define GXSetFog vgxapi_GXSetFog
#define GXSetProjection vgxapi_GXSetProjection
#define GXSetViewport vgxapi_GXSetViewport
#define GXCopyDisp vgxapi_GXCopyDisp
#define GXCopyTex vgxapi_GXCopyTex
#define GXSetDispCopySrc vgxapi_GXSetDispCopySrc
#define GXInitTexObj vgxapi_GXInitTexObj
#define GXInitTexObjCI vgxapi_GXInitTexObjCI
#define GXInitTexObjData vgxapi_GXInitTexObjData
#define GXInitTexObjLOD vgxapi_GXInitTexObjLOD
#define GXInitTexObjWrapMode vgxapi_GXInitTexObjWrapMode
#define GXInitTexObjTlut vgxapi_GXInitTexObjTlut
#define GXInitTexObjUserData vgxapi_GXInitTexObjUserData
#define GXLoadTexObj vgxapi_GXLoadTexObj
#define GXDestroyTexObj vgxapi_GXDestroyTexObj
#define GXInitTlutObj vgxapi_GXInitTlutObj
#define GXLoadTlut vgxapi_GXLoadTlut
#define GXDestroyTlutObj vgxapi_GXDestroyTlutObj
// VI belongs to the SDK layer (NPS); the render-mode hook is np_gx_vi_configure.
#define VIConfigure vgxapi_VIConfigure
#define VISetFrameBufferScale vgxapi_VISetFrameBufferScale
#define VILockAspectRatio vgxapi_VILockAspectRatio
#define VIUnlockAspectRatio vgxapi_VIUnlockAspectRatio
