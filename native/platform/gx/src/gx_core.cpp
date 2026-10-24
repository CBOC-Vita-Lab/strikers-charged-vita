// GX glue, part 1: start-up, vertex arrays, primitives, draw synchronisation, misc entry points.
//
// Compiled against the Wii SDK headers (so every definition below is checked against the game's declaration) and
// without any standard header (the SDK's types.h clashes with them): builtins only.
//
// What is NOT here: functions whose Wii signature equals vgx's own (about 100 of the 114 the game calls) are
// served by vgx_api.cpp directly; textures in gx_texture.cpp, frames and copies in gx_frame.cpp, lights in
// gx_light.cpp, matrices in gx_transform.cpp. (TPL is NPS's: ref tpl/TPL.c with the structure words swapped in place.)
#include <revolution/gx.h>

#include "np_gx.h"
#include "np_gx_pipe.h"
#include "vgx_native.h"
#include "gx_internal.h"

NpGatherPipe g_npGatherPipe;

namespace np_gx {

static int g_started = 0;
static int g_arrayLe = 0;
static GXDrawDoneCallback g_drawDoneCb = 0;
static unsigned short g_drawSyncToken = 0;
static unsigned int g_strayPipeWrites = 0;
static np_gx_present_hook g_presentHook = 0;
static int g_efbW = 0, g_efbH = 0;   // the last render mode the SDK layer reported (VIConfigure runs before GXInit in the game)

// The SDK's GXSetArray takes no size: NP_GX_ARRAY_UNBOUNDED (np_gx.h) bounds nothing a valid array can reach.
static const unsigned int kUnboundedArray = NP_GX_ARRAY_UNBOUNDED;

int Started() { return g_started; }
void MarkStarted() { g_started = 1; }
int ArrayDefaultLe() { return g_arrayLe; }
int EfbSizeReported() { return g_efbW > 0 && g_efbH > 0; }
GXDrawDoneCallback DrawDoneCallback() { return g_drawDoneCb; }
np_gx_present_hook PresentHook() { return g_presentHook; }

} // namespace np_gx

using namespace np_gx;

// ------------------------------------------------------------------------------------------------ platform API
extern "C" {

int np_gx_start(const np_gx_config* config) {
    if (g_started) return 1;
    vgxn_config c = {};
    c.size = sizeof(c);
    if (config) {
        c.render_width = config->render_width;
        c.render_height = config->render_height;
        c.surface_width = config->surface_width;
        c.surface_height = config->surface_height;
        c.dump_dir = config->dump_dir;
        g_arrayLe = config->array_default_le != 0;
    }
    if (!vgxn_init(&c)) return 0;
    g_started = 1;
    return 1;
}
void np_gx_shutdown(void) { vgxn_shutdown(); }
void np_gx_register_ram(const void* base, unsigned int size) { vgxn_register_ram(base, size); }
void np_gx_set_array_default_le(int le) { g_arrayLe = le != 0; }
void np_gx_set_efb_size(int fb_width, int efb_height) {
    g_efbW = fb_width;
    g_efbH = efb_height;
    vgxn_set_efb_size(fb_width, efb_height);
}
void np_gx_set_present_hook(np_gx_present_hook hook) { g_presentHook = hook; }
unsigned long long np_gx_frame_count(void) { return vgxn_frame_count(); }
void np_gx_surface_size(unsigned int* width, unsigned int* height) { vgxn_surface_size(width, height); }
int np_gx_request_dump(const char* png_path) { return vgxn_request_dump(png_path); }
const unsigned char* np_gx_last_picture(int* width, int* height) { return vgxn_last_picture(width, height); }
unsigned int np_gx_stray_pipe_writes(void) { return g_strayPipeWrites + vgxn_dropped_pipe_writes(); }
void np_gx_pipe_stray(unsigned int) { ++g_strayPipeWrites; }

void np_GXSetArray(int attr, const void* data, unsigned int size, unsigned int stride, int le) {
    vgxn_set_array(attr, data, size, stride, le);
}

// W5 SKIN: extension records (np_gx.h; vgx src/vgx_ext.h: EXT_TAG = 1, EXT_SKIN = 2, EXT_SKIN_OFF = 3).
void np_gx_skin_begin(const float* mats, unsigned int nBones, const void* weightsBE, const void* indices, unsigned int nVerts) {
    if (nBones == 0 || nBones > 32) return;
    unsigned int w[6 + 32 * 12];
    w[0] = nBones;
    w[1] = nVerts;
    const unsigned long long pw = (unsigned long long)(__SIZE_TYPE__)weightsBE, pi = (unsigned long long)(__SIZE_TYPE__)indices;
    __builtin_memcpy(w + 2, &pw, 8);
    __builtin_memcpy(w + 4, &pi, 8);
    __builtin_memcpy(w + 6, mats, nBones * 48u);
    vgxn_ext(2, w, 6 + nBones * 12);
}
void np_gx_skin_end(void) { vgxn_ext(3, 0, 0); }
void np_gx_fx_lowres(unsigned int on) {
    const unsigned int w[1] = {on ? 1u : 0u};
    vgxn_ext(4, w, 1);
}
void np_gx_diag_tag(unsigned int tag, unsigned int flags) {
    const unsigned int w[2] = {tag, flags};
    vgxn_ext(1, w, 2);
}

// ------------------------------------------------------------------------------------------------ SDK entry points
// GXInit: the SDK's takes the FIFO memory the hardware would read commands from; there is none here (commands go
// into the renderer's record stream), so the memory is not used. The returned FIFO object is vgx's dummy.
GXFifoObj* GXInit(void*, u32) {
    if (!g_started && !np_gx_start(0)) StartFailed();
    GXFifoObj* fifo = static_cast<GXFifoObj*>(vgxapi_GXInit(0, 0));
    // GXInit puts the renderer's registers back to power-on values, the EFB size among them: the game configures the video mode
    // (VIConfigure) before it initialises GX, so say it again.
    if (g_efbW > 0 && g_efbH > 0) vgxn_set_efb_size(g_efbW, g_efbH);
    ResetCopyTracking();
    return fifo;
}

// The FIFO's watermarks mean nothing without a hardware FIFO.
void GXInitFifoLimits(GXFifoObj*, u32, u32) {}
void GXInvalidateVtxCache(void) {}
void GXSetMisc(GXMiscToken, u32) {}
void GXAbortFrame(void) {}

// Vertex arrays. The SDK's GXSetArray has no size and no byte order. The game's arrays are plain memory it owns, bound
// by reference and read by the player thread before the frame is presented; the size is only a bound against bad
// indices, which a valid array never reaches (kUnboundedArray). The byte order is the glue's default for this
// 3-argument form; the per-call form is NP_GX_SETARRAY.
void GXSetArray(GXAttr attr, const void* base, u8 stride) {
    vgxn_set_array(static_cast<int>(attr), base, kUnboundedArray, stride, g_arrayLe);
}

// Primitives. The game follows GXBegin with raw writes to WGPIPE (index bytes, or direct vertex data): the renderer
// opens a raw primitive sized from the current vertex descriptor and formats, and the WGPIPE stand-in (np_gx_pipe.h)
// appends to it. A format that describes no vertex bytes opens nothing; the writes that follow are counted as stray.
void GXBegin(GXPrimitive prim, GXVtxFmt fmt, u16 verts) {
    if (np_gx::g_traceBegin != 0 && verts == np_gx::g_traceBegin) np_gx::TraceBegin(static_cast<unsigned>(prim), static_cast<int>(fmt), verts);
    vgxn_begin_raw(static_cast<int>(prim), static_cast<int>(fmt), verts);
}

} // extern "C"

// Fog: the SDK's argument order is (type, colour, start, end, near, far); vgx implements the Dolphin SDK's
// (type, start, end, near, far, colour).
extern "C" void vgxapi_GXSetFog(int type, float startz, float endz, float nearz, float farz, GXColor color);
extern "C" void GXSetFog(GXFogType type, GXColor color, f32 start, f32 end, f32 near, f32 far) {
    vgxapi_GXSetFog(static_cast<int>(type), start, end, near, far, color);
}

// ------------------------------------------------------------------------------------------------ draw sync
// The hardware's draw-done token machinery. Everything the game has handed to GX is consumed by the renderer's
// player when the frame is presented (GXCopyDisp), so the notion of "the GPU has finished" collapses: the SDK's
// GXSetDrawDone then completes at once and delivers the callback the way the PE finish interrupt would (the
// recompiled build's HLE does the same: MSC_GXSetDrawDone), and GXWaitDrawDone has nothing to wait for.
extern "C" {

void GXSetDrawDone(void) {
    GXDrawDone();
    GXDrawDoneCallback cb = g_drawDoneCb;
    if (cb) cb();
}
GXDrawDoneCallback GXSetDrawDoneCallback(GXDrawDoneCallback callback) {
    GXDrawDoneCallback old = g_drawDoneCb;
    g_drawDoneCb = callback;
    return old;
}
void GXWaitDrawDone(void) {}
void GXSetDrawSync(u16 token) { g_drawSyncToken = token; }
u16 GXReadDrawSync(void) { return g_drawSyncToken; }

// GPU status: nothing is ever over- or underflowing, the command processor is idle, no break point.
void GXGetGPStatus(GXBool* overhi, GXBool* underlow, GXBool* readIdle, GXBool* cmdIdle, GXBool* brkpt) {
    if (overhi) *overhi = 0;
    if (underlow) *underlow = 0;
    if (readIdle) *readIdle = 1;
    if (cmdIdle) *cmdIdle = 1;
    if (brkpt) *brkpt = 0;
}

// EFB peeks and pokes: the recompiled build's HLE stubs these out too (the game uses them for a debug screen border,
// the Home Button menu's frame counter and the disc-lid message); a poke draws nothing, a peek reads black.
void GXPokeARGB(u16, u16, u32) {}
void GXPokeBlendMode(GXBlendMode, GXBlendFactor, GXBlendFactor, GXLogicOp) {}
void GXPokeColorUpdate(GXBool) {}
void GXPeekARGB(unsigned short, unsigned short, unsigned long* color) {
    if (color) *color = 0;
}

} // extern "C"
