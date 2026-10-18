// np_gx.h: the native port's GX glue, as seen by the platform code (NPS: OS / VI / DVD, NPV: Vita, harness)
// and by host programs. Plain C types only (no SDK headers, no standard headers: it can be included next to the
// Wii SDK headers, whose typedefs clash with <stdint.h>).
//
// The glue is the layer between the game, which is compiled against the Wii SDK's GX API (revolution/gx.h) and
// writes raw vertex bytes through the gather-pipe stand-in WGPIPE, and the renderer vgx (libvgx-native). Most of
// the SDK's GX functions are served by vgx's own implementation directly; the glue defines the ones whose
// signature, object layout or meaning differs for native code. See docs/render.md.
#ifndef NP_GX_H
#define NP_GX_H
#ifdef __cplusplus
extern "C" {
#endif

typedef struct np_gx_config {
    unsigned int size;                  // sizeof(np_gx_config)
    int render_width, render_height;    // internal picture size; 0, 0 = the game's EFB size
    int surface_width, surface_height;  // what np_gx_surface_size reports (dynamic aspect); 0 = 640x480
    const char* dump_dir;               // PNG dumps (np_gx_request_dump, MSC_DUMP_*); NULL = ./vgx_frames
    int array_default_le;               // 3-argument GXSetArray: arrays are little-endian (1) or big-endian (0)
} np_gx_config;

// Starts the renderer (render thread, GL / vitaGL context). Optional: GXInit starts it with defaults when
// the platform code has not. Returns 1 on success. Call it before the game thread touches GX.
int np_gx_start(const np_gx_config* config);
void np_gx_shutdown(void);

// The game's heap(s): the memory display lists, vertex arrays, textures and palettes live in. Registering lets the
// renderer cache decoded display lists and track writes per 4 KiB instead of copying display lists on every call and
// hashing textures on every bind. At most 4 contiguous ranges; call before the first GX call that uses them.
void np_gx_register_ram(const void* base, unsigned int size);
// The data cache over [p, p+n) was flushed / stored / invalidated (DCFlushRange, DCStoreRange, DCInvalidateRange,
// DCFlushRangeNoSync), or a disc read / decompression wrote there: cached decodes and EFB copy textures that
// overlap are retired. Cheap when nothing overlaps; thread-safe.
void np_gx_notify_write(const void* p, unsigned int n);

// Vertex array byte order. Two kinds of array reach GXSetArray in the native game:
//   * disc data (the model files' vertex arrays, vertex-animation frames) and the CPU-skinned output stay big-endian,
//     the console's order (NPA's contract, docs/assets.md): the renderer decodes them as it always has;
//   * arrays the game fills itself with typed stores (every mesh writer: particles, effects, UI, shadows, the movie
//     quad) hold host-order (little-endian) values.
// The model stream says which: bit 0 of byte 7 of the game's glModelStream (`unknown07`, 0 in all 4762 streams of the
// 185 .rlg files on the disc) is NP_GL_STREAM_HOST_ORDER; glSetModelStream sets it, the two CPU-skinning calls clear it
// (patches/0002), and the material programs call NP_GX_SETARRAY_STREAM (patches/0003) instead of GXSetArray(attr,
// stream.address, stride). Colour bytes are r, g, b, a in memory order in both kinds (the writers copy an nlColour as
// a word), so only 16- and 32-bit components differ.
#define NP_GL_STREAM_HOST_ORDER 1
#define NP_GX_ARRAY_UNBOUNDED 0x01000100u   /* an index is 16 bits and a stride at most 255 bytes: no valid array reaches this */
// Per-call form: `size` bounds the array (bytes readable from `data`; reads beyond see zeros), `le` != 0 for
// host-order (little-endian) arrays, 0 for console-order data.
void np_GXSetArray(int attr, const void* data, unsigned int size, unsigned int stride, int le);
#define NP_GX_SETARRAY(attr, data, size, stride, le) np_GXSetArray((int)(attr), (data), (unsigned int)(size), (unsigned int)(stride), (int)(le))
// The array of a glModelStream (`stream` is the struct itself: an lvalue with `address` and `unknown07`).
#define NP_GX_SETARRAY_STREAM(attr, stream, stride) \
    np_GXSetArray((int)(attr), (stream).address, NP_GX_ARRAY_UNBOUNDED, (unsigned int)(stride), ((stream).unknown07 & NP_GL_STREAM_HOST_ORDER) != 0)
// W5 SKIN: GPU skinning (vgx extension records, vgx src/vgx_ext.h). The draws between np_gx_skin_begin and np_gx_skin_end are skinned by
// the renderer: every vertex's position and normal (as read from the bound arrays: the bind pose) become the sum over its four slots of
// weight * (bone matrix * vertex), the matrices `mats` (nBones x 3 x 4 floats, GX row layout: row r = out_r = m[r][0..2] . v + m[r][3])
// as the game's CPU skinning reads them, the weights (`weightsBE`: 4 big-endian floats per vertex) and bone numbers (`indices`: 4 bytes
// per vertex) indexed by the vertex's POSITION index (nVerts entries). Slots after the first zero weight and bone numbers >= nBones do
// not count (the CPU loop's rules). nBones <= 32. Recorded in command order (the matrices by value, the arrays by reference: model data
// that stays unchanged until the frame is presented).
void np_gx_skin_begin(const float* mats, unsigned int nBones, const void* weightsBE, const void* indices, unsigned int nVerts);
void np_gx_skin_end(void);
// Diagnostics (MSC_VGX_TAGCENSUS=1 in the renderer): the draws that follow are counted under (tag, flags) in the per-frame tag census.
void np_gx_diag_tag(unsigned int tag, unsigned int flags);

// What the 3-argument SDK form GXSetArray(attr, base, stride) assumes (default big-endian, as on the console). Only code
// that has no stream to ask (the SDK's own libraries) uses it.
void np_gx_set_array_default_le(int le);

// The SDK layer's VIConfigure(rm) may report the render mode here (logical EFB size = fbWidth x efbHeight; the renderer's
// render target has this size). Optional: without it the glue takes the size from the game's GXSetDispCopySrc(0, 0, w, h),
// which the game calls with the whole EFB right after configuring the video mode. A reported size wins.
void np_gx_set_efb_size(int fb_width, int efb_height);

// Dynamic aspect (port/runtime/src/dynamic_aspect.cpp in the recompiled build): full-screen perspective projections are widened
// to the shape of the output surface (np_gx_surface_size: Hor+ wider than 16:9, Vert+ narrower), so that the 16:9 picture the
// game draws in its widescreen mode fills any window without bars or squeezing (src/gx_transform.cpp). mode: -1 follow the
// game's widescreen setting (SCGetAspectRatio of the SDK layer; the default), 0 off, 1 on.
void np_gx_set_dynamic_aspect(int mode);

// Called on the game thread after every GXCopyDisp, once the frame has been handed to the renderer: `xfb` is the
// destination the game passed (the SDK layer's VISetNextFrameBuffer bookkeeping may want it).
typedef void (*np_gx_present_hook)(void* xfb);
void np_gx_set_present_hook(np_gx_present_hook hook);

unsigned long long np_gx_frame_count(void);              // frames presented (GXCopyDisp calls)
void np_gx_surface_size(unsigned int* width, unsigned int* height);
// Writes the next presented frame as a PNG (waits for it to be drawn); 1 on success.
int np_gx_request_dump(const char* png_path);
// The last display copy: RGBA8, top row first, valid until the next present; NULL before the first.
const unsigned char* np_gx_last_picture(int* width, int* height);
// Gather-pipe writes that arrived while no primitive was open (a bug in the caller or an unsupported FIFO command).
unsigned int np_gx_stray_pipe_writes(void);

// Big-endian stores for data the game generates for the graphics hardware (display lists, textures): the renderer reads
// console-order bytes there. (Arrays have the per-call `le` flag instead; see NP_GX_SETARRAY.)
static inline void np_gx_put_be16(void* p, unsigned int v) {
    unsigned char* b = (unsigned char*)p;
    b[0] = (unsigned char)(v >> 8);
    b[1] = (unsigned char)v;
}
static inline void np_gx_put_be32(void* p, unsigned int v) {
    unsigned char* b = (unsigned char*)p;
    b[0] = (unsigned char)(v >> 24);
    b[1] = (unsigned char)(v >> 16);
    b[2] = (unsigned char)(v >> 8);
    b[3] = (unsigned char)v;
}

#ifdef __cplusplus
}
#endif
#endif
