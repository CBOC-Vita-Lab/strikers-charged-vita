// vgx native interface: the C API of the standalone renderer library (libvgx-native) that the native
// port's GX glue (np/platform/gx) and its host programs call.
//
// Why a second API next to the Dolphin-SDK-named one (src/vgx_api.cpp)?
//  - The game is compiled against the Wii SDK headers (revolution/gx.h); vgx_api.cpp implements the
//    Dolphin/Aurora header set. Both have the same function names, the same enumerator values (checked by
//    np/platform/gx/tools/gx_abi.py) and, for most functions, the same ABI, so those are linked straight
//    through. Everything that differs (texture object layout, GXSetArray, GXSetFog, raw FIFO writes through
//    WGPIPE, display-list and guest-RAM bookkeeping, frame present) goes through this header, whose
//    signatures use plain C types only: the glue includes it next to the SDK headers without clashes.
//  - It is the whole surface the standalone test and the glue need from the renderer; nothing in it refers to
//    the recompilation runtime.
//
// Threading: everything here is called by the game thread unless stated. vgxn_init starts the render thread
// (which owns the GL / vitaGL context and runs the player).
#pragma once
// (No #include: builtin types only, so that the glue can include this next to the Wii SDK headers, whose types.h
// redefines uintptr_t / size_t / char16_t. unsigned int is what uint32_t is on every target we build for.)

#ifdef __cplusplus
extern "C" {
#endif

#define VGXN_API_VERSION 1

typedef struct vgxn_config {
    unsigned int size;          // sizeof(vgxn_config), for forward compatibility
    int render_width;       // internal picture size; 0, 0 = follow the game's EFB size (VIConfigure)
    int render_height;
    int surface_width;      // size reported by vgxn_surface_size (the runtime's dynamic aspect); 0 = 640x480
    int surface_height;
    const char* dump_dir;   // where vgxn_request_dump / MSC_DUMP_* write PNG files; NULL = ./vgx_frames
    unsigned int flags;         // VGXN_F_*
} vgxn_config;

enum {
    VGXN_F_NO_RENDER_THREAD_PIN = 1, // Vita: do not pin the render thread to a core
};

// Starts the render thread, creates the context, initialises the back end. Returns 1 on success.
// Reads the renderer's environment switches (MSC_VGX_*, see vgx/NOTES.md "Switches").
int vgxn_init(const vgxn_config* config);
void vgxn_shutdown(void);

// ---------------------------------------------------------------- frames
// The game has queued its display copy for this frame (GXCopyDisp): hands the frame to the player. Blocks
// until the player has consumed everything the frame refers to in memory (the back end may still be drawing
// it), or, when a dump was requested for this frame, until it has been drawn and written.
void vgxn_end_frame(void);
unsigned long long vgxn_frame_count(void);           // frames presented so far
// The next presented frame is also written as a PNG (RGB). Returns 0 when the dump directory is unusable.
int vgxn_request_dump(const char* png_path);
// Size of the last display copy (the picture), 0 before the first one; pixels are RGBA8, top row first.
// Valid until the next frame is presented. Only meaningful after a frame was drawn (vgxn_request_dump waits).
const unsigned char* vgxn_last_picture(int* width, int* height);
void vgxn_surface_size(unsigned int* width, unsigned int* height);

// ---------------------------------------------------------- guest memory
// The game's heap(s): memory the game fills for the graphics hardware (display lists, vertex arrays, textures,
// palettes) and tells the renderer about by flushing the data cache. Registering enables the display-list
// cache and write-generation tracking of textures; without it everything is still correct, but display lists
// are copied on every call and textures are re-hashed on every bind. At most 4 ranges. `base`..`base+size`
// must be one contiguous mapping (the game's arena).
void vgxn_register_ram(const void* base, unsigned int size);
// DCFlushRange / DCStoreRange / DCInvalidateRange / disc reads over [p, p+n): contents may have changed.
void vgxn_notify_write(const void* p, unsigned int n);

// ------------------------------------------------------------- state access
// Destination size / format of the last GXSetTexCopyDst (for the glue's bookkeeping of EFB copy textures).
void vgxn_tex_copy_dst(unsigned int* width, unsigned int* height, unsigned int* format);
unsigned int vgxn_disp_copy_height(void);   // height of the last GXSetDispCopySrc rectangle
unsigned int vgxn_xf_reg(unsigned int addr);       // shadow of XF registers 0x1000..0x1057 (the API's own writes)
unsigned int vgxn_bp_reg(unsigned int reg);        // shadow of BP register `reg` (24-bit)
void vgxn_set_efb_size(int width, int height); // VIConfigure: logical EFB size (fbWidth x efbHeight)
// W5 PIPE: only in a library built with -DVGX_PRODUCER_CHECK (a census build): `fn` is called before every record of the recorder (any thread).
void vgxn_set_producer_hook(void (*fn)(void));

// ---------------------------------------------------------------- geometry
// Binds a vertex array. `size` bounds what may be read from `p` (reads beyond see zeros); `le` != 0: the array
// holds little-endian (host-order) values, otherwise big-endian as on the console. The memory is read later
// by the player thread, before vgxn_end_frame returns.
void vgxn_set_array(int attr, const void* p, unsigned int size, unsigned int stride, int le);
// Opens a raw primitive: the game then writes count * (bytes per vertex) bytes through the gather-pipe
// stand-in (vgxn_pipe_write*); returns the bytes per vertex (0: not drawable under the current vertex
// descriptor, nothing is recorded).
unsigned int vgxn_begin_raw(int prim, int fmt, unsigned int count);

// Gather-pipe stand-in used by the WGPIPE proxy (platform/gx/include/np_gx_pipe.h). The inline versions live
// in vgx_hle.h (vgx::hle::RawWrite*); these exist for callers that cannot include it. Return 0 when no
// primitive is open (the write is dropped).
int vgxn_pipe_write8(unsigned int v);
int vgxn_pipe_write16(unsigned int v);
int vgxn_pipe_write32(unsigned int v);

// ------------------------------------------------- extension records (W5 SKIN, src/vgx_ext.h)
// A command the GX API has not got, recorded in command order: `kind` (EXT_*), `n` payload words.
void vgxn_ext(unsigned int kind, const unsigned int* words, unsigned int n);

// -------------------------------------------------------------- textures
typedef struct vgxn_tex {
    const void* data;          // image data, console tiled format and byte order (big-endian), all mip levels
    unsigned short width, height;
    unsigned int format;           // GXTexFmt / GXCITexFmt value
    unsigned char wrap_s, wrap_t;    // GXTexWrapMode
    unsigned char min_filter, mag_filter; // GXTexFilter
    unsigned char mipmap;
    unsigned char pad[3];
    float min_lod, max_lod, lod_bias;
    unsigned int tlut;             // palette name for CI formats
} vgxn_tex;
void vgxn_load_tex(int tex_map, const vgxn_tex* t);
// Palette: `entries` 16-bit big-endian values in `fmt` (GXTlutFmt). The palette is re-validated on every load.
void vgxn_load_tlut(int index, const void* data, int fmt, int entries);

// ------------------------------------------------------- vgx_api.cpp (Dolphin-SDK-named API) functions the glue uses
// In the native library these carry the prefix vgxapi_ (native/vgx_native_rename.h) where the glue redefines the SDK
// name; the others keep their names and are linked straight into the game.
#ifndef VGXN_NO_VGXAPI_DECLS   // (defined by programs that include vgx's own Dolphin-style headers, which declare these with their types)
void* vgxapi_GXInit(void* fifo_base, unsigned int fifo_size);
void vgxapi_GXSetProjection(const float m[4][4], unsigned int type);
void vgxapi_GXSetViewport(float left, float top, float width, float height, float nearz, float farz);
void vgxapi_GXCopyDisp(void* dest, unsigned char clear);
void vgxapi_GXCopyTex(void* dest, unsigned char clear);
void vgxapi_GXSetDispCopySrc(unsigned short left, unsigned short top, unsigned short wd, unsigned short ht);
void GXDestroyCopyTex(void* dest);
void GXApplyBPReg(unsigned char reg, unsigned int value); // a raw BP register write through the recorder
#endif

// ------------------------------------------------------------ misc
// Number of times a GX call reached the renderer with something it cannot honour (diagnostics for the glue).
unsigned int vgxn_dropped_pipe_writes(void);

#ifdef __cplusplus
}
#endif
