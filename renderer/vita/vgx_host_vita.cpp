// PS Vita platform layer of vgx: the counterpart of src/vgx_host.cpp and
// src/vgx_host_egl.cpp.
//
//   - a render thread that owns the vitaGL context. In the renderer's default
//     mode it is the "player" (src/vgx_rec.h): it decodes the record stream the
//     game thread writes and executes the resulting command buffers itself. In
//     the two other modes it executes command buffers handed over
//     synchronously by the game thread;
//   - vitaGL start-up with explicit memory pool sizes, shader cache directory
//     ux0:data/mscharged/shadercache and fixed varying semantics;
//   - present: the back end calls PresentDisplayCopy (its present hook) at
//     every display copy, while the finished picture is still in the EFB
//     framebuffer object; it is blitted to the display and the buffers are
//     swapped;
//   - renderer settings a console needs: a texture budget and the shader
//     warm-up list (see EnsureThread);
//   - shader start-up: vitaGL's on-disk cache of compiled shaders lives in a
//     folder whose name carries a key of everything that decides what a
//     compiled shader looks like (ShaderCacheFolder), and while shaders are
//     being compiled the back end draws a progress bar that ShowProgressBar
//     puts on the display;
//   - vitaGL's own threads and pools set up as the one GameCube port known to
//     run on consoles has them (docs/acgc-vita-port-findings.md): a larger
//     shader patcher, the collector thread on core 2, sceGxm's display-queue
//     thread on core 1;
//   - a compiled-shader cache that came inside the package (app0:shadercache)
//     is copied to the writable cache folder once (SeedShaderCache), so that a
//     first start need not compile: both reference ports that run on consoles
//     ship their compiled shaders (ACGC as GXP files, melee-Vita as a warm
//     cache next to a writable one);
//     (vitaGL's cache layout is <key>/v<n>/{v,f}/<HH>/<hash>.gxp: n and the
//     spelling of the hash follow the library, VglCacheVersion; what an older
//     layout or an older key left on a console is moved once, MoveOlderCache);
//   - the desktop graphics-library entry points the runtime links against
//     (aurora_*, PAD).
//
// Nothing in vgx/src is modified for the Vita. This file and include/GLES2
// adapt vitaGL to what the back end expects; see include/GLES2/gl2.h for the
// two GL calls routed through here.
//
// NOT RUN YET: this file has been compiled and linked, never executed (no
// Vita or emulator in the build environment). Everything it prints is meant
// to make the first run diagnosable: look for "[vgx-vita]" in log.txt.
//
// Debug controls (hold SELECT+START, then press):
//   D-pad down   show the text console (log tail) instead of the picture, and back
//                (it is copied over each presented frame, so it only updates
//                while the game presents frames)
//   D-pad up     flip the presented picture vertically
//   D-pad right  stretch to the whole screen / 4:3 with side bars
// Marker files in ux0:data/mscharged (looked at once, at start-up):
//   vgx_verbose          the renderer's verbose log
//   vgx_widescreen       forces full screen (see DecideWidescreen() below; since round 6 full
//                        screen is the default whenever the game's widescreen setting is on)
//   vgx_play_frame       the game thread decodes each frame at present and
//   vgx_play_immediate   ... after every call (the renderer's MSC_VGX_PLAY
//                        modes "frame" and "immediate"; default "thread")
// The renderer's own switches are environment variables (MSC_VGX_*); on the
// Vita they are set through ux0:data/mscharged/env.txt (vita_main.cpp), e.g.
//   MSC_VGX_TEXBUDGET=48     texture budget in MiB (default here: 32)
//   MSC_VGX_SHADERLIST=path  another warm-up list file
//   MSC_VGX_PASSISO=1        the renderer's pass isolation
//   MSC_VITA_WIDE=0|1        4:3 with side bars / full screen, whatever the game's setting
//   MSC_VITA_SHOT=<frames>   save the presented picture of these presents as PNG files in
//                            ux0:data/mscharged/shots/ (workstream REL: "---- frame capture")
//   MSC_VITA_RENDER=WxH      internal picture size (default: the game's own, 640x448 in a
//                            match), scaled to the screen at present. 480x272 or 640x362 if
//                            the GPU ever becomes the limit; 960x544 for a sharper picture
//                            (costs video memory and fill rate: see RenderSize() below)
//   MSC_VITA_UPSCALE=smooth|off|linear|sharp|sharp+
//                            how the internal picture is scaled to the screen at present (LOOK,
//                            wave 5): smooth = bilinear plus an unsharp mask (default since 23 Oct;
//                            five fetches per screen pixel), off = glBlitFramebuffer (no shader),
//                            linear = the blit through a shader, sharp = "sharp bilinear" (one
//                            fetch per pixel), sharp+ = sharp plus an unsharp mask. Needs the
//                            picture shader (first use compiles it: libshacccg, or a shader cache
//                            seed that has it); without it the blit is used.
//                            combined (1.3) = SMOOTH's look from three fetches, no dependent read (the cheap default candidate); cas (1.3) = contrast-adaptive, five fetches
//                            fast (1.3) = bilinear plus an unsharp mask from three fetches at coordinates the vertex shader computed, no edge smoothing
//   MSC_VITA_SHARPEN=0.5     the unsharp mask's amount (smooth 0.5, sharp+ 0.35; 0.2 to 0.8). Unset,
//                            smooth uses 0.7 / 0.8 in Mega Strike scenes (drawn smaller)
//   MSC_VITA_MSAA=0|2|4      multisampling of every render target (vglInit's last argument; the
//                            depth buffers grow 2x / 4x, and with STORE_DEPTH_STENCIL the tile
//                            load / store traffic with them): see StartGl
//   MSC_VITA_TRIPLE=1|0      (1.2, console A/B) vglUseTripleBuffering(): 3 / 2 display buffers; vitaGL's default is already 3 (=0 is the same as
//                            MSC_VITA_DISPLAY_BUFFERS=2, which wins when both are set)
//   MSC_VITA_DISPLAY_SCENES=1..8   (1.2, console A/B) scenes per frame of the display's own render target (vglSetupRenderTargetScenesNum's
//                            first argument; default 1)
//   MSC_VITA_PB_MIB=16       (1.3, console A/B) sceGxm's parameter buffer in MiB (4 to 64; default 16, vitaGL's): the video memory reserve follows it. See StartGl
//   MSC_VGL_PATCHER_BUF_KB=8192 (vram19) the patcher's bufferMem alone (video memory), see StartGl; MSC_VITA_PB_LPDDR, MSC_VGX_DISPDEPTH (vram19, console A/B)
//   MSC_VITA_PATCHER_MIB=8   shader patcher pools: this many MiB of host memory and half of it for each of vertex
//                            and fragment code (default 8 / 4 / 4; see StartGl)
//   MSC_VGX_MAXPROGRAMS=900  (back end, src/vgx_backend_gl.cpp) linked programs above which a frame start deletes the
//                            ones longest without a draw (none drawn in the last 600 frames), a hundred at a time, and
//                            the warm-up stops linking new ones; vitaGL's table has 1,024 slots. 0 = no limit
#if defined(__vita__)

#define VGX_VITA_NO_GL_ROUTING
#include <cstring>
#include <GLES2/gl2.h>

#if defined(VGX_NATIVE)
// The native library (np branch, native/native-vita.mk): no recompilation runtime. The services the host takes from the
// platform layer (console, thread cores, debug pad, boot log) come from native/vita_shim, whose headers carry the
// runtime's names; vgxn_* below replaces the aurora_* entry points.
#include "vgx_hdtex.h"
#include "vgx_native.h"
#else
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/gfx.h>
#include <dolphin/gx.h>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/pad.h>
#endif

#include <psp2/display.h>
#include <psp2/gxm.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include "plat_frame_stats.h"
#include "plat_split.h"
#include "vgx_present.h"
#include "vgx_pacelog.h"   // wave 9 (MSC_PACE_LOG): where a picture's time goes, per window
#include "vgx_rec.h"
#include "vgx_shadergen.h"
#include "vgx_scenefresh.h"
#include "vgx_state.h"
#include "vita_platform.h"
#include "vgx_display_rect.h"
#include "vgx_gpubusy.h"
#include "vgx_picture_math.h"
#include "vgx_sharpramp.h"
#include "vgx_listmerge.h"
#include "vgx_split17.h"
#include "vgx_gpuscene20.h"   // gpuscene20 (MSC_VITA_RT_SCENES: the [gpu scenes] line)
#include "vgx_worker_alloc.h"  // wave 14 (MSC_VGX_ASYNC_HEAPONLY): the shader worker's allocation rule

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <malloc.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdlib>
#include <mutex>
#include <pthread.h>
#include <string>
#include <utility>
#include <algorithm>
#include <vector>

// The renderer's dynamic resolution checks the free video memory before it draws a larger picture (vgx_backend_gl.cpp).
// (vitaGL puts a GPU allocation in VRAM first and then falls back to its RAM pool: both count.)
extern "C" int vgx_vram_free_kib(void) { return static_cast<int>((vglMemFree(VGL_MEM_VRAM) + vglMemFree(VGL_MEM_RAM)) / 1024); }
// The texture bound to the active unit got its memory (vitaGL is built with SKIP_ERROR_HANDLING: a failed allocation is silent and
// a framebuffer object with that texture attached crashes the next scene).
extern "C" int vgx_tex_has_storage(void) { return vglGetTexDataPointer(GL_TEXTURE_2D) != nullptr ? 1 : 0; }
extern "C" int vgx_tex_swizzle(void) { return vglSwizzleBoundTexture() ? 1 : 0; }   // MSC_VGX_TEXSWIZZLE (src/vgx_backend_gl.cpp)

// Multiplayer (platform layer, vita_multiplayer.cpp): the system NetCheck dialog of the ad hoc connection is
// drawn by vitaGL's swap, which must be told that a CommonDialog is on screen. Weak: absent in an older platform
// snapshot (then every swap is a plain one).
extern "C" {
void np_vita_radio_dialog_render(void) __attribute__((weak));
bool np_vita_radio_dialog_active(void) __attribute__((weak));
static unsigned g_patcherBufKb = 0;          // 1.3 (vram19) the shader patcher's bufferMem asked for (KiB), MSC_VGL_PATCHER_BUF_KB
static unsigned g_patcherBufHigh = 0;        // the highest sceGxmShaderPatcherGetBufferMemAllocated seen at the [vgx] vitaGL: lines (KiB)
static unsigned g_patcherBufSamples = 0;
static bool g_dispDepthNoMem = false;        // 1.3 (vram19) MSC_VGX_DISPDEPTH=nomem was asked for
void np_vita_radio_set_dialog_ready(bool) __attribute__((weak));
int np_vita_radio_dialog_prepare(bool) __attribute__((weak));
extern "C" void NpLogMarkNoCardWrite(void) __attribute__((weak));   // wave 14: vita_log.cpp, this thread never writes the log to the card
// (W5 FPS, platform/compat/np_fps.cpp) how many vblanks a picture is shown for: 2 at MSC_FPS=30, 1 at 60, 0 = no gate (unlocked, the build's own pacing).
// Weak: absent in an older platform snapshot (no gate).
int NpFpsDisplayInterval(void) __attribute__((weak));
int NpFps45Due(uint64_t tag, uint32_t earliest, uint32_t prevShown, int havePrev, uint32_t* due) __attribute__((weak));   // helper f45 (MSC_FPS=45): np_fps.cpp, the 1, 1, 2 pattern's gate
void NpFps45Shown(uint64_t tag, uint32_t shown) __attribute__((weak));
}

#ifdef VITAGL_GLB
// kubridge.skprx (taiHEN plug-in), optional: the port links its WEAK stubs, so the address is null without the plug-in.
extern "C" void kuKernelFlushCaches(const void* ptr, SceSize len) __attribute__((weak));
// W5 SEED: the read-through patch of vitaGL (np tools/seed/vitagl/0002-seed-readthrough.patch). A vitaGL without it does not have this function (weak: null).
extern "C" void vglSetShaderSeedPath(const char* path) __attribute__((weak));
extern "C" void vglSetWriteCopyFallback(int on) __attribute__((weak));       // nxgl25: vitaGL 0061 (MSC_VITA_WRCOPY_FB); null in an older library
extern "C" void vglWriteCopyStats8(uint32_t* out8) __attribute__((weak));
extern "C" void vglSetFramebufferDetachKeep(GLboolean on) __attribute__((weak));   // nxgl25: vitaGL 0062 (MSC_VITA_FBO_KEEP); null in an older library
extern "C" void vglSetTextureCopyDma(int mode) __attribute__((weak));        // gxvita14: vitaGL 0051 (MSC_VITA_TEXCOPY); null in an older library
extern "C" void vglTextureCopyStats(uint32_t* out6) __attribute__((weak));
extern "C" void vglSetLazyClear(GLboolean on) __attribute__((weak)); // 1.2: vitaGL 0015 (lazy glClear); null in an older library
extern "C" uint32_t vglSetStateCache(uint32_t mask) __attribute__((weak)); // 1.2: vitaGL 0013 (GXM state cache); null in an older library
extern "C" int vglRtWaitScenesDone(uint32_t target, uint32_t timeout_us) __attribute__((weak)); // 1.3: vitaGL 0022 (MSC_VITA_QUEUE_WAIT=event); null in an older library
#endif
// W5 BOOT: the in-memory shader pack (np platform/vita/vitagl/0004-boot-shader-pack.patch). Weak: null without the patch (any vitaGL build).
extern "C" void vglSetShaderPackLookup(const void* (*fn)(int fragment, uint64_t hash, uint32_t* size)) __attribute__((weak));
extern "C" unsigned vglStrikersShaderCacheCaps(void) __attribute__((weak));
// The cache version folder this vitaGL writes ("v<n>", its SHADER_CACHE_MAGIC): 2 up to the cdbba42 base, 3 since the move to 9a14d17 (30 Oct 2026), which also
// writes file names with 16 hex digits. Weak: a library without the function is the old one (2, short names).
extern "C" unsigned vglStrikersShaderCacheVersion(void) __attribute__((weak));
// W5 RT: the input-delay record (np include/np/latency.h, platform/compat/np_latency.cpp) and the VI thread's last vblank (np np_plat_vita.cpp)
extern "C" uint64_t NpLatTakeSendTag(void) __attribute__((weak));
#include "../src/vgx_perf_diag.h"

extern "C" float VgxRenderScale(void); // vgx_backend_gl.cpp (DYNRES): the scene's render scale in force
extern "C" void NpPlatformNote(const char* line) __attribute__((weak)); // 1.2: the platform's one-line record that survives a quiet build (log.txt)
extern "C" void NpLatDecoded(uint64_t tag) __attribute__((weak));
extern "C" void NpLatPresented(uint64_t tag) __attribute__((weak));
extern "C" void NpLatShown(uint64_t tag, uint64_t shownNs, int handedToo) __attribute__((weak));
extern "C" void NpLatEntered(uint64_t tag) __attribute__((weak));   // wave 11 (press16)
extern "C" int np_display_last_vblank(uint32_t* vcount, uint64_t* ns) __attribute__((weak));
extern "C" uint32_t VgxGpuSceneEpoch(void) __attribute__((weak));   // nxr3a MSC_SCENE_GPU_RESET (vgx_backend_gl.cpp): a count that changes when play begins after another kind of scene
extern "C" uint32_t VgxGpuSceneSkip(void) __attribute__((weak));   // nxr6a MSC_SCENE_SKIP (vgx_backend_gl.cpp): timed frames of the old scene still in flight when play begins
extern "C" int VgxGpuSampleIdle(void) __attribute__((weak));        // nxr3a MSC_GPU_IDLE_SKIP: the newest GPU sample may belong to a picture that drew nothing
extern "C" int VgxBackendLivePlay(void) __attribute__((weak));   // vgx_backend_gl.cpp: the scene being executed is live match play (wave 9 MSC_PACE_LOG)

namespace {

// MSC_GPU_BUSY_FIX (default 1): the GPU time the ladder, LOCKED 60 and the auto queue depth read leaves out the display scene's wait for a display buffer
// (vgx_gpubusy.h; vitaGL 0030 display_us). MSC_GPU_BUSY_BLIT_US (2000): what the display scene really costs. Raw figures stay in the [rt] lines.
uint32_t GpuBusyUs(const vglRtGpuFrame& f) {
    static const vgx::gpubusy::Config cfg = [] {
        vgx::gpubusy::Config c;
        const char* e = std::getenv("MSC_GPU_BUSY_FIX");
        c.on = !(e && *e && std::atoi(e) == 0);
        if (const char* b = std::getenv("MSC_GPU_BUSY_BLIT_US"); b && *b) c.blitUs = static_cast<uint32_t>(std::max(0, std::atoi(b)));
        std::fprintf(stderr, "[vgx-vita] GPU busy time: %s (MSC_GPU_BUSY_FIX=0|1; the display scene's wait for a buffer is %s, its blit counted as %u us, MSC_GPU_BUSY_BLIT_US)\n",
                     c.on ? "display scene wait left out" : "raw", c.on ? "subtracted" : "kept", c.blitUs);
        return c;
    }();
    return vgx::gpubusy::CorrectedUs(cfg, f.busy_us, f.display_us, f.scenes);
}

// W5 BOOT: the platform's loading screen (np platform/vita/src/vita_bootscreen.cpp, through the renderer services): it draws itself into
// every buffer vitaGL is about to show until the game's first picture, and follows the seed copy, the shader warm-up and the presents.
#if defined(VGX_NATIVE)
inline bool BootCompose(void* framebuffer) { return VitaBootScreen::Compose(framebuffer); }
inline void BootProgress(int kind, unsigned done, unsigned total) { VitaBootScreen::Progress(kind, done, total); }
#else
enum { VGXN_BOOT_SEED_COPY = 1, VGXN_BOOT_WARMUP = 2, VGXN_BOOT_PRESENT = 3 };
inline bool BootCompose(void*) { return false; }
inline void BootProgress(int, unsigned, unsigned) {}
#endif
// Milliseconds since the process started, for the boot lines ("[+N ms]").
inline unsigned long BootMs() { return static_cast<unsigned long>(sceKernelGetProcessTimeWide() / 1000u); }

constexpr int kScreenW = 960, kScreenH = 544;
constexpr const char* kShaderCacheDir = "ux0:data/mscharged/shadercache";
// What decides the compiled form of a shader besides its GLSL text (which is vitaGL's own
// key inside the folder): the vitaGL build (its translator, its serialisation, its options)
// and the varying semantics fixed in StartGl. The build system can pass the first as
// MSC_VGL_ID ("<commit> <options>", the line vita/Makefile.vitadeps writes to
// obj/vitagl/options.txt); without it every compile of this file gets a key of its own,
// which is safe (a new package compiles its shaders again once) but invalidates more often
// than necessary. Bump kHostShaderAbi when the semantic bindings in StartGl change.
#ifndef MSC_VGL_ID
#define MSC_VGL_ID "unidentified vitaGL, host built " __DATE__ " " __TIME__
#endif
constexpr const char* kHostShaderAbi = "tex0-7 on TEXCOORD0-7, col0/1 on TEXCOORD8/9, uv on TEXCOORD0; mode GLOBAL; 1";
// vitaGL's collector thread at its default priority, but on the audio core: the game
// thread's core is the one that has no time to spare.
constexpr int kGcPriority = 0x10000100;
// Every shader key the renderer has needed so far; a run that finds the file
// generates, compiles and links all of them in its first frame instead of in
// the middle of play. Derived from playing, so it lives next to vitaGL's
// cache of the compiled shaders and is not part of the package.
constexpr const char* kShaderListFile = "ux0:data/mscharged/shadercache/vgx_shaderlist.bin";
// Decoded guest textures the renderer keeps (RGBA8 bytes). Its own
// measurement: a match frame's working set is about 20 MiB, and without a
// budget the cache grows to 52-61 MiB over a match; video memory here is
// 109 MiB for everything (display buffers, EFB, copy targets, textures).
constexpr uint64_t kTextureBudget = 32ull * 1024 * 1024;
constexpr const char* kVerboseMarker = "ux0:data/mscharged/vgx_verbose";
constexpr const char* kWideMarker = "ux0:data/mscharged/vgx_widescreen";
constexpr size_t kRenderStack = 4u * 1024u * 1024u;   // the Cg compiler runs on this thread

// ---- what the routed GL calls record (render thread only)
bool g_scissorOn = false;
void ParkScissor();     // (defined with vgxvita_glScissor below)
void UnparkScissor();

// ---- render thread hand-over (the renderer's "frame" and "immediate" modes)
std::mutex g_mtx;
std::condition_variable g_cv;
const vgx::CmdBuf* g_jobBuf = nullptr;
bool g_started = false, g_startOk = false, g_threadCreated = false;
char g_startError[512] = "";
int g_startCause = 0;   // (1.3) why the start failed, as a VGXN_START_* code the platform can name in the player's language (vgxn_vita_start_failure)

// ---- present state. The flags are set by the game thread (debug controls).
std::atomic<bool> g_flipY{false}, g_stretch{false}, g_showConsole{false};
int g_vglBuffers = 3, g_vglFboScenes = 8, g_vglDisplayScenes = 1;   // what StartGl gave vitaGL (the run header of the logs)
int g_upscaleMode = 4;       // vgx::backend::PictureMode; 0 = glBlitFramebuffer, 4 = PIC_SMOOTH (default)
bool g_sharpenSet = false;   // MSC_VITA_SHARPEN given: Mega Strike scenes keep it too
float g_sharpen = 0.5f;      // MSC_VITA_SHARPEN (default for smooth; sharp+ was tuned at 0.35)
float g_sharpenScale = 0.0f, g_sharpenCap = 0.8f;   // MSC_VITA_SHARPEN_SCALE / _CAP (1.3)
// MSC_SHARPEN_EASE=<pictures> (wave 7, 20; 0 = off): a step of the picture size moves the sharpening strength over this many pictures instead of in one (vgx_sharpramp.h)
vgx::sharpramp::State g_sharpRamp;
int g_sharpEase = -1;
int SharpEase() {
    if (g_sharpEase < 0) {
        const char* e = std::getenv("MSC_SHARPEN_EASE");
        g_sharpEase = vgx::sharpramp::FramesFromSetting((e && *e) ? std::atol(e) : 0);
    }
    return g_sharpEase;
}
int g_texCopyMode = 0;          // MSC_VITA_TEXCOPY (set at start-up, read by the periodic stats line)
static bool g_streamSceneFlush = false;   // MSC_VITA_STREAM_CACHED in force (cache25, vitaGL 0060)
uint64_t g_presented = 0;       // render thread
unsigned long long g_nativeFrames = 0;   // vgxn_end_frame calls (native build; unused otherwise)
uint64_t g_lastPresentUs = 0;
vgx::backend::Stats g_lastStats;

#ifdef VITAGL_GLB
// be24 MSC_VITA_WRCOPY=0|1|2|3|auto (vitaGL patch 0045): how the frame's data is copied INTO the uncached pools (the stream buffers, 1 MB of an open-play picture, and the uniform ring,
// 0.3 MB). The CPU's writes there ran at 0.25 GB/s (5 Oct), which is the whole of the back end's ~6 ms CPU in open play if it still holds. 0 = sceClibMemcpy, 1 / 2 = whole 64 / 128 byte
// NEON stores, 3 = the DMA engine (sceDmacMemcpy, 8 KiB and more). auto: a start-up benchmark on a block of the same memory type (USER_RW_UNCACHE), every variant read back and compared with the source; the fastest correct one that
// is at least 1.2 x the plain copy is used, else 0. =bench logs the numbers and keeps 0.
static int g_wrCopyMode = 0;     // MSC_VITA_WRCOPY in force (the [wrcopy] counters line reads it)
static int g_wrCopyFb = 0;       // MSC_VITA_WRCOPY_FB in force (0061)
static uint32_t g_wrBase[8] = {};   // the counters after the start-up benchmark (its copies are not the game's)
static void WriteCopySetup() {
    const char* e = std::getenv("MSC_VITA_WRCOPY");
    const bool bench = e && (std::strcmp(e, "auto") == 0 || std::strcmp(e, "bench") == 0);
    int mode = 0;
    if (e && *e && !bench) mode = std::atoi(e);
    if (mode < 0 || mode > 3) mode = 0;
    // nxgl25 (vitaGL 0061) MSC_VITA_WRCOPY_FB=0|1 (default 1): what mode 3 does with a copy the DMA engine does not take. 1 = mode 1's NEON stores (exact, the same bytes), 0 = sceClibMemcpy as before.
    // nxr6a (NEON r6 #6): set BEFORE the benchmark: it times mode 3 with the fallback in force (the start-up benchmark ran with the library's default 1 and FB=0 was only set afterwards, so an FB=0 run could
    // pick mode 3 / 1 on numbers the plain fallback does not have).
    {
        const char* f = std::getenv("MSC_VITA_WRCOPY_FB");
        const int on = (f && *f) ? (std::atoi(f) != 0) : 1;
        if (vglSetWriteCopyFallback) { vglSetWriteCopyFallback(on); g_wrCopyFb = on; }
        else if (f && *f) std::fprintf(stderr, "[wrcopy] MSC_VITA_WRCOPY_FB=%s ignored: this vitaGL has no patch 0061\n", f);
    }
    if (bench) {
        const uint32_t kBytes = 256u * 1024u;
        std::vector<uint8_t> src(kBytes);
        for (uint32_t i = 0; i < kBytes; ++i) src[i] = static_cast<uint8_t>((i * 2654435761u) >> 24);
        const SceUID blk = sceKernelAllocMemBlock("wrcopy_bench", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 1024u * 1024u, nullptr);
        void* base = nullptr;
        if (blk >= 0 && sceKernelGetMemBlockBase(blk, &base) >= 0 && base) {
            uint8_t* dst = static_cast<uint8_t*>(base);
            double mbs[4] = {0, 0, 0, 0}, smallMbs[4] = {0, 0, 0, 0};
            bool ok[4] = {true, true, true, true};
            bool dmaTaken = false;   // the DMA engine took the benchmark's large copy (both addresses below 0x81000000)
            for (int v = 0; v < 4; ++v) {
                uint32_t st0[8] = {};
                if (v == 3 && vglWriteCopyStats8) vglWriteCopyStats8(st0);
                uint64_t best = ~0ull;
                for (int rep = 0; rep < 3; ++rep) {
                    std::memset(dst, 0xA5, 4096);
                    const uint64_t t0 = sceKernelGetProcessTimeWide();
                    vglWriteCopyVariant(v, dst + 4096 + 32, src.data(), kBytes - 4096);   // (an unaligned start: the head is copied by hand)
                    const uint64_t dt = sceKernelGetProcessTimeWide() - t0;
                    if (dt < best) best = dt;
                }
                if (v == 3 && vglWriteCopyStats8) { uint32_t st1[8] = {}; vglWriteCopyStats8(st1); dmaTaken = st1[4] > st0[4]; }
                if (std::memcmp(dst + 4096 + 32, src.data(), kBytes - 4096) != 0) ok[v] = false;
                mbs[v] = best ? static_cast<double>(kBytes - 4096) / static_cast<double>(best) : 0.0;   // bytes per us = MB/s
                // 768-byte copies (the size of the uniform ring's blocks), 128 of them
                uint64_t bestS = ~0ull;
                for (int rep = 0; rep < 3; ++rep) {
                    const uint64_t t0 = sceKernelGetProcessTimeWide();
                    for (int i = 0; i < 128; ++i) vglWriteCopyVariant(v, dst + 512u * 1024u + static_cast<uint32_t>(i) * 784u, src.data() + static_cast<uint32_t>(i) * 16u, 768);
                    const uint64_t dt = sceKernelGetProcessTimeWide() - t0;
                    if (dt < bestS) bestS = dt;
                }
                for (int i = 0; i < 128; ++i)
                    if (std::memcmp(dst + 512u * 1024u + static_cast<uint32_t>(i) * 784u, src.data() + static_cast<uint32_t>(i) * 16u, 768) != 0) ok[v] = false;
                smallMbs[v] = bestS ? 128.0 * 768.0 / static_cast<double>(bestS) : 0.0;
            }
            int pick = 0;
            double top = mbs[0] * 1.2;
            for (int v = 1; v < 4; ++v)
                if (ok[v] && mbs[v] > top && smallMbs[v] >= smallMbs[0] * 0.9) { pick = v; top = mbs[v]; }
            // nxgl25: mode 3 is "the DMA engine where it is allowed, else mode 1's stores". When the benchmark's own large copy was not given to the DMA engine (a source address at or above 0x81000000), 3 is mode 1 under another name: say 1.
            if (pick == 3 && vglWriteCopyStats8 && !dmaTaken) pick = g_wrCopyFb ? 1 : 0;   // (FB=0: mode 3 without the DMA engine is the plain copy, which is mode 0)
            if (e && std::strcmp(e, "auto") == 0) mode = pick;
            std::fprintf(stderr, "[wrcopy] benchmark addresses: source %p (the C heap, as the frame's vertex and index vectors), destination %p (USER_RW_UNCACHE); the DMA engine takes a copy of 8 KiB+ only below 0x81000000 on both: %s\n",
                         static_cast<const void*>(src.data()), base, !vglWriteCopyStats8 ? "not counted (library without 0061)" : dmaTaken ? "TAKEN" : "NOT taken, mode 3 ran as mode 1 for the large copy");
            std::fprintf(stderr, "[wrcopy] uncached write benchmark, 252 KiB: plain %.0f MB/s (768 B copies %.0f), neon64 %.0f (%.0f)%s, vstm128 %.0f (%.0f)%s, dma %.0f (%.0f)%s; chosen %d (MSC_VITA_WRCOPY=%s)\n", mbs[0], smallMbs[0],
                         mbs[1], smallMbs[1], ok[1] ? "" : " MISMATCH", mbs[2], smallMbs[2], ok[2] ? "" : " MISMATCH", mbs[3], smallMbs[3], ok[3] ? "" : " MISMATCH", mode, e);
            sceKernelFreeMemBlock(blk);
        } else {
            std::fprintf(stderr, "[wrcopy] benchmark block not made (%08X): plain copy kept\n", static_cast<unsigned>(blk));
        }
    }
    vglSetWriteCopy(mode);
    g_wrCopyMode = mode;
    if (vglWriteCopyStats8) vglWriteCopyStats8(g_wrBase);   // (after the benchmark: its copies are not the game's)
    std::fprintf(stderr, "[wrcopy] copies into the uncached pools: %s (MSC_VITA_WRCOPY=0|1|2|3|auto|bench)\n", mode == 0 ? "plain (sceClibMemcpy)" : mode == 1 ? "neon64" : mode == 2 ? "vstm128" : "dma (8 KiB and more)");
    if (mode == 3) std::fprintf(stderr, "[wrcopy] mode 3: copies the DMA engine does not take go to %s (MSC_VITA_WRCOPY_FB=%d)\n", g_wrCopyFb ? "neon64 (vitaGL 0061)" : "sceClibMemcpy (as 0045)", g_wrCopyFb);
}
#endif

uint64_t NowUs() { return sceKernelGetProcessTimeWide(); }

bool Exists(const char* path) {
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}
// The size of a file, -1 when it is not there.
long long FileSizeOf(const char* path) {
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0 ? static_cast<long long>(st.st_size) : -1;
}
// A cached shader is written whole or not at all (review 12 Nov): to <path>.tmp first, then renamed over the name vitaGL reads. A power cut in the middle
// leaves only a .tmp, which no reader opens and the next start writes again.
bool RenameInto(const char* tmp, const char* path) {
    if (Exists(path)) sceIoRemove(path);
    if (sceIoRename(tmp, path) < 0) { sceIoRemove(tmp); return false; }
    return true;
}

// FULL SCREEN (the owner's requirement after the first console runs, which showed the picture
// in a 4:3 box). With `widescreen = true` in Config.toml (the default) the game runs in its own
// 16:9 mode: it draws an ANAMORPHIC 16:9 picture into its 640-pixel-wide frame buffer, lays out
// 2D for a 16:9 screen, and the runtime fits the 3D projections to the shape of the surface we
// report (dynamic_aspect.cpp). That picture is only right when it is stretched over a 16:9
// screen: in a 4:3 box the HUD and menus are squeezed to three quarters of their width. So:
//   game's widescreen on  -> surface reported as 960x544, picture stretched over the whole
//                            screen (the Vita's 960x544 is 0.7 % taller than 16:9; the runtime
//                            gives the 3D that much more vertical field of view, 2D is
//                            stretched by it, which nobody can see);
//   game's widescreen off -> surface 640x480, the game's 4:3 picture in a 725x544 box with bars.
// The game's setting is taken from the window the runtime asks for in aurora_initialize (854x480
// with widescreen or "force 16:9" on, 640x480 without: runtime/src/main.cpp), so this file needs
// nothing from the runtime's configuration headers. MSC_VITA_WIDE=0|1 in env.txt overrides it
// (1 with the game's setting off stretches a 4:3 picture: everything too wide); the marker file
// vgx_widescreen forces it on, as before round 6. Checked on the PC build with
// MSC_VGX_SURFACE=960x544 (vgx/NOTES.md, round 6, "FULL SCREEN").
std::atomic<bool> g_wide{false};
bool Widescreen() { return g_wide.load(std::memory_order_relaxed); }
void DecideWidescreen(unsigned windowWidth, unsigned windowHeight) {
    bool wide = windowHeight != 0 && windowWidth * 3u > windowHeight * 4u;
    const char* why = wide ? "the game's widescreen setting is on" : "the game's widescreen setting is off";
    if (const char* e = std::getenv("MSC_VITA_WIDE"); e && *e) {
        wide = std::atoi(e) != 0;
        why = "MSC_VITA_WIDE in env.txt";
    } else if (!wide && Exists(kWideMarker)) {
        wide = true;
        why = "marker file vgx_widescreen";
    }
    g_wide.store(wide);
    std::fprintf(stderr, "[vgx-vita] full screen: %s (%s; the runtime asked for a %ux%u window)\n", wide ? "ON, 960x544 surface, picture stretched" : "off, 640x480 surface in a 4:3 box",
                 why, windowWidth, windowHeight);
}

// MSC_VITA_RENDER=WxH (env.txt): the size of the picture the renderer draws into, instead of
// the game's own frame-buffer size (640x448 in a match, 640x480 / 640x528 in some screens).
// Every pass target and copy scales with it (the back end's fixed-size mode, the PC harness's
// MSC_RENDER_SIZE). SMALLER (480x272 = a quarter of the screen's pixels, 640x362) is the lever
// if a console turns out to be limited by the GPU: 480x272 draws 43 % of the default's pixels;
// the picture is visibly soft at 480x272 and close to the default at 640x362 (pictures:
// build/host-gl/checks/r6/wide_lowres_sheet.png). LARGER (960x544) is sharper and costs 1.8
// times the pixels and about 3 MiB more video memory for the main target alone.
int g_vitaMsaa = 0;     // MSC_VITA_MSAA in force at start-up (0: none), the run header
int g_anisoLevel = 0;   // MSC_VITA_ANISO in force (0: off)
void RenderSize(int& w, int& h) {
    w = h = 0;
    const char* e = std::getenv("MSC_VITA_RENDER");
    if (!e || !*e) return;
    int rw = 0, rh = 0;
    if (std::sscanf(e, "%dx%d", &rw, &rh) == 2 && rw >= 160 && rh >= 120 && rw <= 960 && rh <= 544) {
        w = rw;
        h = rh;
        std::fprintf(stderr, "[vgx-vita] MSC_VITA_RENDER: internal picture %dx%d, scaled to the screen at present\n", w, h);
    } else {
        std::fprintf(stderr, "[vgx-vita] MSC_VITA_RENDER=%s ignored (WxH from 160x120 to 960x544)\n", e);
    }
}

void LogMemory(const char* when) {
    SceKernelFreeMemorySizeInfo free;
    std::memset(&free, 0, sizeof(free));
    free.size = sizeof(free);
    sceKernelGetFreeMemorySize(&free);
    std::fprintf(stderr, "[vgx-vita] memory %s: system free main %d KiB, cdram %d KiB, phycont %d KiB\n", when, free.size_user / 1024,
                 free.size_cdram / 1024, free.size_phycont / 1024);
}

void LogPools(const char* when) {
    std::fprintf(stderr, "[vgx-vita] vitaGL pools %s (free/total KiB): vram %u/%u, ram %u/%u, phycont %u/%u, newlib heap %u/%u\n", when,
                 static_cast<unsigned>(vglMemFree(VGL_MEM_VRAM) / 1024), static_cast<unsigned>(vglMemTotal(VGL_MEM_VRAM) / 1024),
                 static_cast<unsigned>(vglMemFree(VGL_MEM_RAM) / 1024), static_cast<unsigned>(vglMemTotal(VGL_MEM_RAM) / 1024),
                 static_cast<unsigned>(vglMemFree(VGL_MEM_PHYCONT) / 1024), static_cast<unsigned>(vglMemTotal(VGL_MEM_PHYCONT) / 1024),
                 static_cast<unsigned>(vglMemFree(VGL_MEM_EXTERNAL) / 1024), static_cast<unsigned>(vglMemTotal(VGL_MEM_EXTERNAL) / 1024));
}

// W5 RT: the input-delay tag of every buffer handed to the display queue, in order (one producer: the render thread, SwapDisplay; one consumer: the display-queue
// thread's DisplayCallback, which vitaGL calls once per vglSwapBuffers, in the same order).
struct LatFifo {
    uint64_t tag[32] = {};
    std::atomic<uint32_t> head{0}, tail{0};
} g_latFifo;
std::atomic<uint32_t> g_diagFifoGeneration{0};
// The renderer's frame number -> input-delay tag (written by the thread that sends the frame, read by the render thread at its display copy).
// STR-102 (1.2): one entry is ONE 64-bit word, the frame number's low 24 bits above the tag's low 40 bits, stored with release and loaded with
// acquire: a reader sees either the whole entry of a frame or an earlier one, never a new frame number with an old tag (the two-field entry it
// replaces could be read half written), and everything the sender stamped before the store (the decode time) is visible after the load.
// Zero: empty (tag 0 is "no record").
std::atomic<uint64_t> g_latMap[256];
void LatMapPut(uint64_t frame, uint64_t tag) {
    g_latMap[frame % 256].store(((frame & 0xFFFFFFull) << 40) | (tag & 0xFFFFFFFFFFull), std::memory_order_release);
}
uint64_t LatTagOf(uint64_t frame) {
    const uint64_t v = g_latMap[frame % 256].load(std::memory_order_acquire);
    return v != 0 && (v >> 40) == (frame & 0xFFFFFFull) ? (v & 0xFFFFFFFFFFull) : 0;
}

// wave 9 MSC_PACE_LOG=<seconds> (default 0, off; the Vita plan sets 5): two lines per window, one from the render thread (the swap call split into the display queue's and the GPU limiter's wait, the GPU's
// time) and one from the display-queue thread (vblanks waited, the flip against the first possible vblank, holds, frame ids shown, long holds by picture % 600): vgx_pacelog.h. Clock reads: three per swap
// and one per callback, only with the switch on. Each thread keeps its own window: nothing is shared but the last GPU time (one relaxed atomic).
const double g_paceWindowMs = [] { const char* e = std::getenv("MSC_PACE_LOG"); const double v = e && *e ? std::atof(e) : 0.0; return v > 0.0 ? v * 1000.0 : 0.0; }();
std::atomic<float> g_paceLastGpuMs{-1.0f};
vgx::pacelog::RenderWin g_paceRender;      // render thread
vgx::pacelog::DisplayWin g_paceDisplay;    // display-queue thread
std::atomic<int> g_paceInterval{0};

// ---- the picture is shown at the vblank it is due at (W5 FPS) -------------------------------------------------------------------------------------------------
// vitaGL's display callback hands the finished buffer to sceDisplaySetFrameBuf(NEXTFRAME), which shows it at the first vblank after the call, i.e. whenever
// the GPU happened to finish: a 30 fps game whose frames finish a little earlier or later in the refresh period would be shown 2,2,1,3,2 refreshes
// now and then. The gate below (in the display-queue thread, just before that call) holds the buffer until the vblank that is `interval` after the
// previous picture's, so every picture of a steady 30 fps game is shown for exactly two refreshes (60: for one). A picture that is late is shown at
// the first vblank after it is ready and the next is measured from there (no catch-up). The thread only waits for vblanks, which vitaGL's own
// vsync does in the same place. MSC_VITA_DISPLAY_GATE=0: no waiting, the statistics only.
// The log line says how long each picture was shown (vblanks between two pictures): the console's own evidence of an even pace.
struct DisplayGate {
    bool have = false;
    uint32_t lastShown = 0;        // the vcount of the vblank the previous picture appeared at
    uint64_t total = 0, waited = 0, late = 0, held[5] = {0, 0, 0, 0, 0};   // 1, 2, 3, 4+ vblanks, 0 (two pictures in one vblank)
    uint64_t winTotal = 0, winHeld[5] = {0, 0, 0, 0, 0}, every = 120;   // a log line after 120 pictures, then every 600
    int lastInterval = -1;
    int enabled = -1;
} g_gate;

std::atomic<uint32_t> g_lastHeld{0};   // vblanks the last picture was shown for (0: not known), published by GateDisplay on the display-queue thread

extern "C" int NpFpsPictureStamp(uint64_t tag, uint16_t* vcount) __attribute__((weak));   // (np_fps.cpp: the vblank a frame began at)

// wave 9 (MSC_PACE_LOG): one picture's figures for the display thread's window line, and a line for each of the first long holds in live play.
void PaceDisplayNote(uint32_t earliest, uint32_t shown, uint32_t lastShown, bool haveLast, uint32_t waits, uint64_t id, int interval) {
    static uint64_t picture = 0;
    ++picture;
    const double nowMs = static_cast<double>(NowUs()) * 0.001;
    const bool live = VgxBackendLivePlay ? VgxBackendLivePlay() != 0 : false;
    g_paceInterval.store(interval, std::memory_order_relaxed);
    vgx::pacelog::DisplayWin& w = g_paceDisplay;
    w.Shown(nowMs, earliest, shown, lastShown, haveLast, waits, id, picture, live);
    { uint16_t vc = 0; if (id && NpFpsPictureStamp && NpFpsPictureStamp(id, &vc)) w.Delay(shown, vc); }   // wave 10: frame start to screen
    if (haveLast && live && shown - lastShown >= 3 && shown - lastShown < 1000 && w.LongHoldLine())
        std::fprintf(stderr, "[pace] long hold: %u vblanks before picture %llu (frame id %llu, picture %% 600 = %llu), interval %d, the callback waited %u vblanks, first possible vblank +%d, GPU busy of the last timed frame %.1f ms\n",
                     shown - lastShown, static_cast<unsigned long long>(picture), static_cast<unsigned long long>(id), static_cast<unsigned long long>(picture % 600), interval, waits,
                     static_cast<int>(shown - earliest), static_cast<double>(g_paceLastGpuMs.load(std::memory_order_relaxed)));
    if (w.Due(nowMs, g_paceWindowMs)) {
        // gxvita14 (after Galaxy stackgate31): this runs on sceGxm's display-queue thread, whose stack is 4 KiB. A 900-byte local here made DisplayCallback's frame 1152 bytes
        // on EVERY callback (the function is inlined into it); a static buffer is the same text with a ~250-byte frame. Only this thread ever gets here, one callback at a time.
        static char line[900];
        w.Print(line, sizeof line, nowMs, picture, interval);
        std::fprintf(stderr, "%s\n", line);
    }
}

void GateDisplay() {
    uint64_t diagTag=0;
    if (NpLatEntered) { const uint32_t et = g_latFifo.tail.load(std::memory_order_relaxed); if (et != g_latFifo.head.load(std::memory_order_acquire)) NpLatEntered(g_latFifo.tag[et % 32]); }   // wave 11 (press16): entered, before any wait
    DisplayGate& g = g_gate;
    if (g.enabled < 0) {
        const char* e = std::getenv("MSC_VITA_DISPLAY_GATE");
        g.enabled = (e && *e == '0') ? 0 : 1;
    }
    const int interval = NpFpsDisplayInterval ? NpFpsDisplayInterval() : 0;
    if (interval != g.lastInterval) {
        if (g.lastInterval >= 0) std::fprintf(stderr, "[vgx-vita] display: pictures are now shown for %d vblank(s) (was %d)\n", interval, g.lastInterval);
        g.lastInterval = interval;
        g.have = false;   // the first picture after a change is the new reference
    }
    uint32_t shown = static_cast<uint32_t>(sceDisplayGetVcount()) + 1u;   // sceDisplaySetFrameBuf(NEXTFRAME) shows it at the next vblank
    const uint32_t paceEarliest = shown, paceLast = g.lastShown;   // (MSC_PACE_LOG: the first vblank this picture could be on; the previous picture's)
    const bool paceHadLast = g.have;
    uint32_t paceWaits = 0;
    // 1.3 (MSC_PRESENT_SYNC, vgx_present.h): the vblank this picture's game time asks for; `shown` itself when the switch is off, the picture has no stamp, or policy 30's gate below keeps the cadence.
    const uint32_t syncTail = g_latFifo.tail.load(std::memory_order_relaxed);
    const uint32_t syncTarget = vgx::present::SyncTarget(shown, syncTail != g_latFifo.head.load(std::memory_order_acquire) ? g_latFifo.tag[syncTail % 32] : 0);
    bool syncWaited = false;
    // helper f45 (MSC_FPS=45, np_fps45.h): a picture whose frame was made at 45 goes up on its slot of the 1, 1, 2 pattern (its next slot, the slot its game time asks for, or the first slot
    // it can make when it is late: never a vblank off the pattern); the 30 rule below is for every other picture.
    uint32_t f45Due = 0;
    const uint64_t f45Tag = syncTail != g_latFifo.head.load(std::memory_order_acquire) ? g_latFifo.tag[syncTail % 32] : 0;
    const bool f45 = g.enabled && NpFps45Due && NpFps45Due(f45Tag, shown, g.lastShown, g.have ? 1 : 0, &f45Due) != 0;
    if (f45) {
        int guard = 0;
        if (static_cast<int32_t>(f45Due - shown) > 0) ++g.waited;
        while (static_cast<int32_t>(f45Due - shown) > 0 && guard++ < 8) {
            if (sceDisplayWaitVblankStart() < 0) break;
            ++paceWaits;
            shown = static_cast<uint32_t>(sceDisplayGetVcount()) + 1u;
        }
        if (static_cast<int32_t>(shown - f45Due) > 0) ++g.late;
        if (NpFps45Shown) NpFps45Shown(f45Tag, shown);
    } else if (interval > 0 && g.enabled && g.have) {
        const uint32_t due = g.lastShown + static_cast<uint32_t>(interval);
        int guard = 0;
        if (static_cast<int32_t>(due - shown) > 0) ++g.waited;
        while (static_cast<int32_t>(due - shown) > 0 && guard++ < 8) {
            if (sceDisplayWaitVblankStart() < 0) break;
            ++paceWaits;
            shown = static_cast<uint32_t>(sceDisplayGetVcount()) + 1u;
        }
        if (static_cast<int32_t>(shown - due) > 0) ++g.late;
    }
    for (int guard = 0; !f45 && static_cast<int32_t>(syncTarget - shown) > 0 && guard < 9; ++guard) {
        syncWaited = true;
        if (sceDisplayWaitVblankStart() < 0) break;
        ++paceWaits;
        shown = static_cast<uint32_t>(sceDisplayGetVcount()) + 1u;
    }
    vgx::present::SyncShown(shown, syncWaited);
    if (g.have) {
        const uint32_t held = shown - g.lastShown;
        const int bin = held == 0 ? 0 : held >= 4 ? 4 : static_cast<int>(held);
        if (held < 1000) {   // (a stall of seconds is not a pace)
            ++g.total; ++g.held[bin]; ++g.winTotal; ++g.winHeld[bin];
            g_lastHeld.store(held, std::memory_order_relaxed);   // 1.2: for MSC_PACE_VBLANK (np_framepace.cpp)
        }
    }
    g.lastShown = shown;
    g.have = true;
    {
        // W5 RT: the input-delay record of this picture: it appears at vblank `shown`, timed from the last vblank the VI thread saw
        const uint32_t t = g_latFifo.tail.load(std::memory_order_relaxed);
        if (t != g_latFifo.head.load(std::memory_order_acquire)) {
            const uint64_t tag = g_latFifo.tag[t % 32];
            diagTag=tag;
            g_latFifo.tail.store(t + 1, std::memory_order_release);
            if (tag && NpLatShown) {
                uint32_t v = 0;
                uint64_t ns = 0;
                uint64_t shownNs = 0;
                if (np_display_last_vblank && np_display_last_vblank(&v, &ns) && static_cast<int32_t>(shown - v) >= 0 && static_cast<int32_t>(shown - v) < 8)
                    shownNs = ns + static_cast<uint64_t>(shown - v) * 16683333ull;   // (59.94 Hz)
                NpLatShown(tag, shownNs, 1);
            }
        }
    }
    if (g_paceWindowMs > 0.0) PaceDisplayNote(paceEarliest, shown, paceLast, paceHadLast, paceWaits, diagTag, interval);
    if(NpDiagActive&&NpDiagActive())NpDiagEvent(NPD_GATE,interval|(g_diagFifoGeneration.load()<<16),diagTag,shown,g.total,g.waited,g.late,0);
    if (g.winTotal >= g.every) {
        g.every = 600;
        std::fprintf(stderr, "[vgx-vita] display: interval %d vblank(s), the last %llu pictures were shown for 0/1/2/3/4+ vblanks: %llu/%llu/%llu/%llu/%llu (all so far %llu: %llu/%llu/%llu/%llu/%llu; "
                     "the gate waited %llu times, %llu pictures were late)\n", interval, static_cast<unsigned long long>(g.winTotal),
                     static_cast<unsigned long long>(g.winHeld[0]), static_cast<unsigned long long>(g.winHeld[1]), static_cast<unsigned long long>(g.winHeld[2]),
                     static_cast<unsigned long long>(g.winHeld[3]), static_cast<unsigned long long>(g.winHeld[4]), static_cast<unsigned long long>(g.total),
                     static_cast<unsigned long long>(g.held[0]), static_cast<unsigned long long>(g.held[1]), static_cast<unsigned long long>(g.held[2]),
                     static_cast<unsigned long long>(g.held[3]), static_cast<unsigned long long>(g.held[4]), static_cast<unsigned long long>(g.waited),
                     static_cast<unsigned long long>(g.late));
        g.winTotal = 0;
        for (int i = 0; i < 5; ++i) g.winHeld[i] = 0;
    }
}

// Called by vitaGL on sceGxm's display-queue thread with the buffer that is
// about to be shown (960 x 544, A8B8G8R8, 960 pixels per row): the place to
// put the text console on screen while the renderer keeps the display, and
// (W5 FPS) to hold the picture until the vblank it is due at.
void DisplayCallback(void* framebuffer) {
    { static bool marked = false; if (!marked) { marked = true; if (NpLogMarkNoCardWrite) NpLogMarkNoCardWrite(); } }
    if(NpDiagActive&&NpDiagActive()){
        const uint32_t tail=g_latFifo.tail.load(std::memory_order_relaxed),head=g_latFifo.head.load(std::memory_order_acquire);
        const uint64_t tag=tail!=head?g_latFifo.tag[tail%32]:0;
        static bool empty=false;
        if(tail==head&&!empty){++g_diagFifoGeneration;NpDiagEvent(NPD_FIFO_ERROR,2,0,head,tail,0,0,0);}
        empty=tail==head;
        NpDiagEvent(NPD_CALLBACK,g_diagFifoGeneration.load()<<16,tag,(uintptr_t)framebuffer,(uint32_t)sceDisplayGetVcount(),head,tail,0);
    }
    if (BootCompose(framebuffer)) {
        // (the loading screen: nothing else goes on top)
    } else if (g_showConsole.load(std::memory_order_relaxed)) {
        const uint32_t* console = VitaConsole::Pixels();
        if (console) std::memcpy(framebuffer, console, static_cast<size_t>(kScreenW) * kScreenH * 4u);
    }
    GateDisplay();
}

// A trivial GLSL pair through the whole chain (GLSL -> Cg translation, the
// shader compiler module, program link). If this fails nothing the renderer
// generates can work, and the reason is in the returned text.
bool ShaderCompilerSelfTest(char* error, size_t errorSize) {
    static const char* vs = "attribute vec4 a_pos;\nvarying vec2 v_uv;\nvoid main() { v_uv = a_pos.xy; gl_Position = a_pos; }\n";
    static const char* fs = "precision mediump float;\nvarying vec2 v_uv;\nuniform vec4 u_add;\nvoid main() { gl_FragColor = vec4(v_uv, 0.0, 1.0) + u_add; }\n";
    const uint64_t t0 = NowUs();
    char log[1024] = "";
    GLuint shaders[2] = {0, 0};
    for (int i = 0; i < 2; ++i) {
        // (through the host's entry points, like the back end's shaders: they make the cache's
        // bucket folder for this text if it is not there yet, see vgxvita_glShaderSource)
        shaders[i] = vgxvita_glCreateShader(i == 0 ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER);
        const char* text = i == 0 ? vs : fs;
        vgxvita_glShaderSource(shaders[i], 1, &text, nullptr);
        glCompileShader(shaders[i]);
        GLint ok = 0;
        glGetShaderiv(shaders[i], GL_COMPILE_STATUS, &ok);
        if (!ok) {
            glGetShaderInfoLog(shaders[i], sizeof(log), nullptr, log);
            std::snprintf(error, errorSize, "the %s test shader did not compile:\n%s", i == 0 ? "vertex" : "fragment", log);
            return false;
        }
    }
    const GLuint program = glCreateProgram();
    glAttachShader(program, shaders[0]);
    glAttachShader(program, shaders[1]);
    glBindAttribLocation(program, 0, "a_pos");
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        std::snprintf(error, errorSize, "the test program did not link:\n%s", log);
        return false;
    }
    const GLint location = glGetUniformLocation(program, "u_add");
    std::fprintf(stderr, "[vgx-vita] shader compiler self-test passed in %u ms (u_add at location %d)\n",
                 static_cast<unsigned>((NowUs() - t0) / 1000u), static_cast<int>(location));
    glDeleteProgram(program);
    glDeleteShader(shaders[0]);
    glDeleteShader(shaders[1]);
    return true;
}

void PresentDisplayCopy(unsigned efb, int x, int y, int w, int h);
bool PresentSplitBegin(unsigned efb, int x, int y, int w, int h, int* displayX, int* displayW, int* displayH);
void PresentSplitFinish();
#ifdef VITAGL_RT
void StartRt();
#endif

bool CacheWriterOwnsBuckets() {
    static const bool owns = [] {
        auto caps = vglStrikersShaderCacheCaps;
        __asm__("" : "+r"(caps));
        return caps && caps() == 1u;
    }();
    return owns;
}

// vitaGL's cache layout, <cache>/v<version>/{v,f}/<HH>/<hash>.gxp. Version 2 (vitaGL cdbba42 and before) names the file with the hash printed without
// leading zeros; version 3 (upstream, 13 Oct 2026) prints 16 digits. The package's pack does not care (it is indexed by hash), but everything here that
// builds or reads a cache file name does.
unsigned VglCacheVersion() {
    static const unsigned version = [] {
        auto fn = vglStrikersShaderCacheVersion;
        __asm__("" : "+r"(fn));
        return fn ? fn() : 2u;
    }();
    return version;
}
const char* VglCacheVersionDir() {
    static char dir[12] = "";
    if (!dir[0]) std::snprintf(dir, sizeof(dir), "v%u", VglCacheVersion());
    return dir;
}
// "<folder>/<hash>.gxp" in the spelling of the given cache version.
void ShaderFileName(char* out, size_t n, const char* folder, uint64_t hash, unsigned version) {
    if (version >= 3) std::snprintf(out, n, "%s/%016llX.gxp", folder, static_cast<unsigned long long>(hash));
    else std::snprintf(out, n, "%s/%llX.gxp", folder, static_cast<unsigned long long>(hash));
}

// ux0:data/mscharged/shadercache/<8 hex digits>: the folder vitaGL keeps compiled shaders in.
// A package whose key differs starts with an empty folder (the old one is left behind; it is
// some hundred small files and can be deleted by hand).
uint32_t CacheKeyOf(const char* vglId) {
    char id[512];
    std::snprintf(id, sizeof(id), "%s|%s|gen %u", vglId, kHostShaderAbi, static_cast<unsigned>(vgx::kShaderGenVersion));
    uint32_t h = 2166136261u; // FNV-1a
    for (const char* c = id; *c; ++c) h = (h ^ static_cast<uint8_t>(*c)) * 16777619u;
    return h;
}
// The key of the 1.0 and 1.1 builds (vitaGL cdbba42, cache version 2). The move to vitaGL 9a14d17 changes the library's id string and so the key, but the
// compiled shaders of the old key are good for this build: the shader text the renderer generates avoids every GLSL built-in whose translation upstream
// corrected between the two (mod, lessThan, lessThanEqual, boolean equal; vgx_shadergen.cpp), and the compiler, the serialisation and the host's
// bindings are unchanged. So the package's pack and the console's own files of the old key are used, but only while the id differs from the old one in the
// commit alone (the same options) and the host's own ABI and shader generator version are still the old ones (they are part of the key, so a bump gives a
// different old key, which no existing folder or pack has). Returns null when there is nothing to look for.
constexpr const char* kLegacyVglId = "vitaGL cdbba42-glb built with: -DHAVE_SHADER_CACHE -DSTORE_DEPTH_STENCIL -DSKIP_SPLASHSCREEN -DHAVE_SHARK_LOG -DLOG_ERRORS "
                                     "-DHAVE_SCRATCH_MEMORY -DSKIP_ERROR_HANDLING -DDRAW_SPEEDHACK uniform-ring-8MiB";
const char* LegacyCacheKey() {
    static char key[12] = "";
    static bool done = false;
    if (done) return key[0] ? key : nullptr;
    done = true;
    const char* a = std::strstr(MSC_VGL_ID, " built with: ");
    const char* b = std::strstr(kLegacyVglId, " built with: ");
    if (!a || !b || std::strcmp(a, b) != 0) return nullptr;
    const uint32_t legacy = CacheKeyOf(kLegacyVglId);
    if (legacy == CacheKeyOf(MSC_VGL_ID)) return nullptr;
    std::snprintf(key, sizeof(key), "%08x", static_cast<unsigned>(legacy));
    return key;
}
const char* ShaderCacheFolder() {
    static char path[160] = "";
    if (path[0]) return path;
    char id[512];
    std::snprintf(id, sizeof(id), "%s|%s|gen %u", MSC_VGL_ID, kHostShaderAbi, static_cast<unsigned>(vgx::kShaderGenVersion));
    std::snprintf(path, sizeof(path), "%s/%08x", kShaderCacheDir, static_cast<unsigned>(CacheKeyOf(MSC_VGL_ID)));
    std::fprintf(stderr, "[vgx-vita] compiled-shader cache: %s (key of: %s)\n", path, id);
    std::fprintf(stderr, "[vgx-vita] vitaGL cache layout: %s/{v,f}/<HH>/<hash as %s hex digits>.gxp%s%s\n", VglCacheVersionDir(), VglCacheVersion() >= 3 ? "16" : "0-16",
                 LegacyCacheKey() ? "; older key of the same shaders: " : "", LegacyCacheKey() ? LegacyCacheKey() : "");
    std::fprintf(stderr, "[vgx-vita] shader cache buckets: %s\n", CacheWriterOwnsBuckets()
        ? "library-managed (host bucket counters inactive)" : "legacy host workaround");
    return path;
}

// Every swap of the display goes through here: a pending dialog operation is processed before the swap, and the
// swap is told while the dialog is shown.
// tag: the input-delay record of the frame being swapped (0: a picture that is not a game frame: start-up, progress bar)
// ---- adaptive GPU queue depth (23 Oct; the Super Mario Galaxy port's thp32 change) -------------------------------------------------------------------------
// In a scene the GPU cannot finish within the frame budget, every frame queued ahead of the GPU is a frame of input delay that buys nothing (Codex
// measured ~340 ms from a pad read to the screen in the 15 fps Mega Saves of the 18 Oct build). After each swap the render thread waits until at most
// `limit` frames are unfinished on the GPU (vitaGL's GPU timers: vglRtSceneProgress counts the scenes ended and the scenes seen completed). The limit
// adapts: 1 while the mean GPU time of the last 24 frames is at least 78 % of the frame budget (16.7 ms at 60, 33.4 ms at 30), 2 again at or below
// 60 %. With 1 the CPU still builds the next frame while the GPU draws the current one, so the frame rate holds (Galaxy: the slow plaza went from
// 124 to 62 ms median delay, frame rate unchanged). MSC_VITA_QUEUE=auto (default) | 1 | 2 | off; nothing without the GPU timers.
struct GpuQueue {
    int mode = -1;               // -1 unread, 0 off, 1 / 2 fixed, 3 auto
    int limit = 2;
    uint32_t ring[8] = {};       // scenes ended at each of the last swaps
    uint32_t swaps = 0;
    float busy[24] = {};
    int bi = 0, bn = 0;
    uint32_t lastGpuSeq = 0;
    uint32_t skipNew = 0;   // nxr6a: new timed frames still to drop after a scene reset (they are the old scene's, finishing on the GPU)
    uint64_t waits = 0, waitedUs = 0, changes = 0, timeouts = 0;
    int eventWait = 0;           // MSC_VITA_QUEUE_WAIT=event (1.3, vitaGL 0022): block on the GPU watcher's event flag instead of polling every 200 us
    uint64_t eventFallbacks = 0;
    int fixed60 = 0;             // plan24 X5, MSC_VITA_QUEUE_FIXED60 (default 0, a timing change: console run first): 1 depth 2 whenever the frame rate is 60, 2 the same but depth 1 once the GPU alone needs 105 % of the budget
    int fixed30 = 0;             // MSC_VITA_QUEUE_FIXED30 (auto mode): 0 as before, 1 depth 2 whenever the frame rate is held at 30, 2 the same but depth 1 when the GPU alone needs the whole budget
} g_queue;
void LimitGpuQueue() {
    GpuQueue& q = g_queue;
    if (q.mode < 0) {
        const char* e = std::getenv("MSC_VITA_QUEUE");
        q.mode = 3;
        if (e && *e) {
            if (std::strcmp(e, "off") == 0 || std::strcmp(e, "0") == 0) q.mode = 0;
            else if (std::strcmp(e, "1") == 0) q.mode = 1;
            else if (std::strcmp(e, "2") == 0) q.mode = 2;
        }
        q.limit = q.mode == 1 ? 1 : 2;
        // 1.2 (the Galaxy port's FIXED_DEPTH, which found the queue collapsing to one frame under a 30 cap and suspects it of a 17-19 fps scene): the auto rule
        // below goes to depth 1 once the GPU needs 78 % of the budget. At 30 the budget is 33.4 ms, so 26 ms of GPU time (the dynamic resolution's own aim:
        // it raises the picture until 80 % of the budget is used) already holds the queue at one frame, and with one frame the CPU's build of the next
        // frame and the GPU's drawing of this one no longer overlap through any jitter: a late frame costs a whole refresh. Not tried on a console.
        //   MSC_VITA_QUEUE_FIXED30=1: while the frame rate is held at 30 the depth stays 2.
        //   MSC_VITA_QUEUE_FIXED30=2: the same, except depth 1 while the GPU alone needs the whole 30 budget (nothing to overlap then, only latency to save).
        if (const char* f = std::getenv("MSC_VITA_QUEUE_FIXED30"); f && *f) q.fixed30 = std::atoi(f) >= 2 ? 2 : (std::atoi(f) == 1 ? 1 : 0);
        if (const char* w = std::getenv("MSC_VITA_QUEUE_WAIT"); w && std::strcmp(w, "event") == 0) {
            q.eventWait = 1;
            std::fprintf(stderr, "[vgx-vita] GPU queue: waits block on the GPU watcher's event (MSC_VITA_QUEUE_WAIT=event)\n");
        }
        if (const char* f6 = std::getenv("MSC_VITA_QUEUE_FIXED60"); f6 && *f6) q.fixed60 = std::atoi(f6) >= 2 ? 2 : (std::atoi(f6) == 1 ? 1 : 0);
        if (q.mode == 3 && q.fixed60) std::fprintf(stderr, "[vgx-vita] GPU queue: depth 2 held at 60 fps (MSC_VITA_QUEUE_FIXED60=%d); without it the depth falls to 1 once the GPU takes 78 %% of 16.7 ms (13.0 ms)\n", q.fixed60);
        if (q.mode == 3 && q.fixed30) std::fprintf(stderr, "[vgx-vita] GPU queue: depth 2 held under a 30 fps cap (MSC_VITA_QUEUE_FIXED30=%d)\n", q.fixed30);
        std::fprintf(stderr, "[vgx-vita] GPU queue depth: %s (MSC_VITA_QUEUE=auto|1|2|off)\n", q.mode == 0 ? "off" : q.mode == 3 ? "auto (2, 1 in slow scenes)" : (q.mode == 1 ? "1" : "2"));
    }
    if (q.mode == 0) return;
    uint32_t submitted = 0, done = 0;
    vglRtSceneProgress(&submitted, &done);
    if (!submitted) return;   // no GPU timers
    q.ring[q.swaps % 8] = submitted;
    ++q.swaps;
    if (q.mode == 3) {
        vglRtGpuFrame f;
        const uint32_t seq = vglRtLastGpuFrame(&f);
        if (&VgxGpuSceneEpoch != nullptr) {   // nxr3a MSC_SCENE_GPU_RESET: play begins after another kind of scene: the other scene's frames are forgotten and the depth starts at 2 as at start-up (6122.log:752: 28.9 ms of a walk-out took 2 -> 1 at the kick-off)
            static uint32_t epochSeen = 0;
            const uint32_t ep = VgxGpuSceneEpoch();
            if (ep != epochSeen) {
                epochSeen = ep;
                q.skipNew = &VgxGpuSceneSkip != nullptr ? VgxGpuSceneSkip() : 0u;
                std::fprintf(stderr, "[vgx-vita] GPU queue: %u timed frame(s) of the scene before forgotten (play begins), depth %d -> 2, the next %u new timed frames (still the old scene's) not taken (MSC_SCENE_GPU_RESET, MSC_SCENE_SKIP)\n", q.bn, q.limit, q.skipNew);
                q.bn = 0; q.bi = 0; if (q.mode == 3) q.limit = 2;
            }
        }
        const bool idleSample = &VgxGpuSampleIdle != nullptr && VgxGpuSampleIdle() != 0;   // nxr3a MSC_GPU_IDLE_SKIP: not a measurement
        if (seq && seq != q.lastGpuSeq && f.timed) {
            q.lastGpuSeq = seq;
            if (vgx::scenefresh::DropSample(q.skipNew)) {}   // (nxr6a: an old scene's frame finishing: not a sample of play)
            else if (!idleSample) {
                q.busy[q.bi] = static_cast<float>(GpuBusyUs(f)) * 0.001f;
                q.bi = (q.bi + 1) % 24;
                if (q.bn < 24) ++q.bn;
            }
        }
        if (q.bn == 24) {
            float sum = 0;
            for (float b : q.busy) sum += b;
            const float mean = sum / 24.0f;
            const int interval = NpFpsDisplayInterval ? NpFpsDisplayInterval() : 2;
            const float budget = (interval == 1 ? 1.0f : 2.0f) * 16.683f;
            int limit = q.limit;
            if (q.fixed30 && interval != 1) {   // a 30 cap (or no policy yet, which the 30 budget stands for)
                if (q.fixed30 == 2 && mean >= 1.0f * budget) limit = 1;
                else if (q.fixed30 == 1 || mean <= 0.92f * budget) limit = 2;
            } else if (q.fixed60 && interval == 1) {   // plan24 X5: the 60 rule of the same shape (the 78 % line is 13.0 ms at 60, which live play's GPU time reaches: one frame in flight leaves no overlap for the CPU's next build)
                if (q.fixed60 == 2 && mean >= 1.05f * budget) limit = 1;
                else if (q.fixed60 == 1 || mean <= 0.92f * budget) limit = 2;
            } else if (mean >= 0.78f * budget) limit = 1;
            else if (mean <= 0.60f * budget) limit = 2;
            if (limit != q.limit) {
                ++q.changes;
                if (q.changes <= 100)
                    std::fprintf(stderr, "[vgx-vita] GPU queue depth %d -> %d: the GPU takes %.1f ms a frame (budget %.1f ms)\n", q.limit, limit, mean, budget);
                q.limit = limit;
            }
        }
    }
    if (q.swaps <= static_cast<uint32_t>(q.limit)) return;
    // the frame `limit` swaps back must be finished on the GPU
    const uint32_t target = q.ring[(q.swaps - 1 - static_cast<uint32_t>(q.limit)) % 8];
    if (static_cast<int32_t>(done - target) >= 0) return;
    const uint64_t t0 = NowUs();
    ++q.waits;
    // MSC_VITA_QUEUE_WAIT=event: the same condition and the same 100 ms limit, but woken by the watcher when it finishes a scene instead of at the next
    // 200 us poll (no extra wake-ups on the render core while the GPU is busy). -1 (no event): the poll below, as without the switch.
    if (q.eventWait) {
        const int r = vglRtWaitScenesDone ? vglRtWaitScenesDone(target, 100000) : -1;
        if (r == 1) ++q.timeouts;
        if (r >= 0) done = target;   // (done or given up: the loop below is skipped)
        else if (++q.eventFallbacks == 1) std::fprintf(stderr, "[vgx-vita] GPU queue: no watcher event, polling (MSC_VITA_QUEUE_WAIT)\n");
    }
    while (static_cast<int32_t>(done - target) < 0) {
        if (NowUs() - t0 > 100000) { ++q.timeouts; break; }   // (a stuck watcher must not stop the game)
        sceKernelDelayThread(200);
        vglRtSceneProgress(&submitted, &done);
    }
    q.waitedUs += NowUs() - t0;
    if ((q.waits % 1800) == 0)
        std::fprintf(stderr, "[vgx-vita] GPU queue: depth %d, %llu waits so far (%.2f ms each on average), %llu depth changes, %llu timeouts\n", q.limit,
                     static_cast<unsigned long long>(q.waits), static_cast<double>(q.waitedUs) / 1000.0 / static_cast<double>(q.waits),
                     static_cast<unsigned long long>(q.changes), static_cast<unsigned long long>(q.timeouts));
}

// 1.3 render13 MSC_VITA_DQ_TIMING: sceGxmDisplayQueueAddEntry's time (vitaGL 0049), for the [hitch] line (dq=) and a window line.
bool g_dqAddTiming = false;   // (render13; diag13's MSC_DQ_TIMING is g_dqTiming)
void DisplayQueueNote() {
    if (!g_dqAddTiming) return;
    static uint32_t lastAdds = 0, lastSlow = 0; static uint64_t lastTotal = 0;
    uint32_t last = 0, mx = 0, adds = 0, slow = 0, pend = 0; uint64_t total = 0;
    vglDisplayQueueStats(&last, &mx, &adds, &slow, &pend, &total);
    if (adds - lastAdds >= 600) {
        std::fprintf(stderr, "[vgx-vita] display queue: limit %u, %u adds, mean %.2f ms, longest %.1f ms so far, %u of 2 ms or more (%u in this window)\n", pend, adds - lastAdds,
                     static_cast<double>(total - lastTotal) / 1000.0 / static_cast<double>(adds - lastAdds), mx / 1000.0, slow, slow - lastSlow);
        lastAdds = adds; lastSlow = slow; lastTotal = total;
    }
}
// 1.3 MSC_HITCH_MS: how long the last swap took (the display queue when it is full, the collector's credit, the GPU queue limit), for the [hitch] lines; off: no clock read.
const bool g_hitchClock = [] { const char* e = std::getenv("MSC_HITCH_MS"); return e && std::atoi(e) > 0; }();
std::atomic<float> g_lastSwapMs{0.0f};
// diag13 MSC_DQ_TIMING=1 (default 0, off; diagnostic, changes no picture): the time vglSwapBuffers alone takes per picture (the display queue taking the buffer; it blocks while the queue is full), two clock
// reads a picture on the render thread (the swap's own start read is shared with MSC_HITCH_MS / MSC_PACE_LOG / MSC_VGX_BEPH when one of them is on). Cumulative counters, read as windows by the [hitch] and [late] lines
// through vgx_host_dq_window (a cursor each). Bins by ms: <1, <4, <8, <16, <33, 33 and over.
const bool g_dqTiming = [] { const char* e = std::getenv("MSC_DQ_TIMING"); return e && std::atoi(e) > 0; }();
struct DqStats {
    std::atomic<uint64_t> n{0}, sumUs{0}, bin[6]{};
    std::atomic<uint32_t> maxUs[2]{}, lastUs{0};   // (a longest-of-window each for the two readers)
};
DqStats g_dq;
void DqNote(uint64_t us) {
    g_dq.n.fetch_add(1, std::memory_order_relaxed);
    g_dq.sumUs.fetch_add(us, std::memory_order_relaxed);
    const unsigned b = us < 1000 ? 0u : us < 4000 ? 1u : us < 8000 ? 2u : us < 16000 ? 3u : us < 33000 ? 4u : 5u;
    g_dq.bin[b].fetch_add(1, std::memory_order_relaxed);
    const uint32_t u = us > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<uint32_t>(us);
    g_dq.lastUs.store(u, std::memory_order_relaxed);
    for (int i = 0; i < 2; ++i) {
        uint32_t m = g_dq.maxUs[i].load(std::memory_order_relaxed);
        while (u > m && !g_dq.maxUs[i].compare_exchange_weak(m, u, std::memory_order_relaxed)) {}
    }
}

const bool g_sweepBg = [] { const char* e = std::getenv("MSC_VITA_SWEEP_BG"); return e && *e == '1'; }();
// wave 9 (MSC_PACE_LOG): one swap's figures for the render thread's window line.
void PaceRenderNote(uint64_t t0, uint64_t t1) {
    const uint64_t t2 = NowUs();
    float gpuMs = -1.0f;
#ifdef VITAGL_RT
    static uint32_t lastSeq = 0;
    vglRtGpuFrame f;
    const uint32_t seq = vglRtLastGpuFrame(&f);
    if (seq && seq != lastSeq && f.timed) {
        lastSeq = seq;
        gpuMs = static_cast<float>(GpuBusyUs(f)) * 0.001f;
        g_paceLastGpuMs.store(gpuMs, std::memory_order_relaxed);
    }
#endif
    static uint64_t picture = 0;
    ++picture;
    const double nowMs = static_cast<double>(t2) * 0.001;
    g_paceRender.Swap(nowMs, static_cast<float>(t1 - t0) * 0.001f, static_cast<float>(t2 - t1) * 0.001f, gpuMs);
    if (g_paceRender.Due(nowMs, g_paceWindowMs)) {
        char line[700];
        g_paceRender.Print(line, sizeof line, nowMs, picture);
        std::fprintf(stderr, "%s\n", line);
    }
}
// be25 MSC_VGX_BEPH: the time the render thread spends inside the swap (display queue, collector credit) and the GPU queue limit, cumulative us; read by the back end's profiler line.
const bool g_bephWait = [] { const char* e = std::getenv("MSC_VGX_BEPH"); return e && std::atoi(e) > 0; }();
std::atomic<uint64_t> g_bephWaitUs{0};
void SwapDisplay(uint64_t latTag = 0) {
    if (np_vita_radio_dialog_render) np_vita_radio_dialog_render();
    {
        const uint32_t h = g_latFifo.head.load(std::memory_order_relaxed);
        if (h - g_latFifo.tail.load(std::memory_order_acquire) < 32) {
            g_latFifo.tag[h % 32] = latTag;
            g_latFifo.head.store(h + 1, std::memory_order_release);
        }else{
            if(NpDiagActive&&NpDiagActive()){
                ++g_diagFifoGeneration;NpDiagEvent(NPD_FIFO_ERROR,1,latTag,h,g_latFifo.tail.load(),0,0,0);
            }
        }
    }
    if(NpDiagActive&&NpDiagActive())NpDiagEvent(NPD_SWAP,g_diagFifoGeneration.load()<<16,latTag,(uint32_t)sceDisplayGetVcount(),g_latFifo.head.load(),g_latFifo.tail.load(),0,0);
    const uint64_t swapT0 = g_hitchClock || g_paceWindowMs > 0.0 || g_bephWait || g_dqTiming ? NowUs() : 0;
    vglSwapBuffers(np_vita_radio_dialog_active && np_vita_radio_dialog_active() ? GL_TRUE : GL_FALSE);
    const uint64_t swapT1 = g_paceWindowMs > 0.0 || g_dqTiming ? NowUs() : 0;
    if (g_dqTiming) DqNote(swapT1 - swapT0);
#ifdef VITAGL_RT
    LimitGpuQueue();
#endif
    if (g_bephWait) g_bephWaitUs.fetch_add(NowUs() - swapT0, std::memory_order_relaxed);   // (be25 MSC_VGX_BEPH: the swap and the queue limit wait for the display and the GPU)
    DisplayQueueNote();
    if (g_hitchClock) g_lastSwapMs.store(static_cast<float>(NowUs() - swapT0) * 0.001f, std::memory_order_relaxed);
    if (g_paceWindowMs > 0.0) PaceRenderNote(swapT0, swapT1);
    if(NpDiagActive&&NpDiagActive())NpDiagEvent(NPD_SWAP_END,g_diagFifoGeneration.load()<<16,latTag,(uint32_t)sceDisplayGetVcount(),g_latFifo.head.load(),g_latFifo.tail.load(),0,0);
}


// The back end's progress hook (vgx::backend::SetProgressDisplay): a bar has been drawn into
// the display's back buffer in the middle of a frame; show it.
void ShowProgressBar(uint32_t done, uint32_t total) {
    static uint32_t shown = 0;
    BootProgress(VGXN_BOOT_WARMUP, done, total);
    if (shown++ == 0) std::fprintf(stderr, "[vgx-vita] shaders are being compiled: progress bar on screen (%u of %u units)\n", done, total);
    if (VitaConsole::OwnsDisplay()) return;
    SwapDisplay();
}

// ---- a cache that came with the package
// Layout inside the package (the build adds it when it has one; none of today's packages do):
//   app0:shadercache/<key>/v<n>/{v,f}/<HH>/<hash>.gxp   vitaGL's cache folder of a finished session,
//                                                        copied from ux0:data/mscharged/shadercache/<key>
//                                                        (empty bucket folders need not be packed)
//   app0:shadercache/vgx_shaderlist.bin             that session's warm-up list
// <key> is the folder name ShaderCacheFolder computes, so a seed made with another vitaGL
// build, other bindings or another shader generator is simply not found. Files that already
// exist in the writable folder are kept (they are this console's own compiles). The copy
// happens once: the marker file "seeded" is written when it has run to the end.
constexpr const char* kSeedDir = "app0:shadercache";
constexpr unsigned kSeedMaxFiles = 4096;
// (The folder vitaGL puts under the cache path, "v<SHADER_CACHE_MAGIC>", is VglCacheVersionDir() above. The host makes <cache>/<it>/v and /f before vitaGL starts
// only so that no start-up path leaves them to the library; with another vitaGL the only effect of a wrong value is a folder that stays empty.)

// What the first console run showed (hw/run2-gl): "123 files, 133 KiB, 0 failed, 55272 ms" for
// the seed copy, i.e. 55 s of black screen on the first start of a package. The copy made, on
// the memory card: 123 small files, their ~110 bucket folders, and then ALL 512 bucket folders
// (EnsureBuckets, because that vitaGL only creates them when <cache>/v2/v/00 does not exist yet),
// with one more stat per file. That is about 750 directory operations for 133 KiB of data; a
// card creates a directory entry in tens of milliseconds. (Not measurable in the emulator,
// where the whole copy takes 24 ms.) Since round 6:
//   - only the bucket folders that hold seeded files are made here, plus v/00 and f/00;
//   - every other bucket is made when a shader that hashes into it is first handed to vitaGL
//     (vgxvita_glShaderSource below computes vitaGL's file name), i.e. just before the one
//     compile whose result it stores; a seeded start makes none;
//   - no stat for files that go into a folder this copy has just made; one read per file;
//   - the log line gives the time spent making folders and copying files separately.
// Expected on the first start: ~130 folders + ~145 files instead of ~750 + 123 operations.
// MSC_VITA_SEED=app0 (env.txt) copies nothing: vitaGL reads the packaged folder in place
// (read-only: what this console compiles itself is then not kept for the next start).
struct SeedCopyStats {
    unsigned files = 0, bytes = 0, failed = 0, folders = 0, kept = 0;
    uint64_t folderUs = 0, fileUs = 0;
};
bool CopyFile(const char* from, const char* to, bool mayExist, SeedCopyStats& st) {
    if (mayExist) { const long long have = FileSizeOf(to); if (have >= 0 && have == FileSizeOf(from)) { ++st.kept; return true; } }   // (a file of another size is a cut copy: written again)
    static char buffer[64 * 1024];
    char tmp[336];
    if (std::snprintf(tmp, sizeof(tmp), "%s.tmp", to) >= static_cast<int>(sizeof(tmp))) return false;
    const SceUID in = sceIoOpen(from, SCE_O_RDONLY, 0);
    if (in < 0) return false;
    const SceUID out = sceIoOpen(tmp, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (out < 0) { sceIoClose(in); return false; }
    bool ok = true;
    for (;;) {
        const int n = sceIoRead(in, buffer, sizeof(buffer));
        if (n < 0) ok = false;
        if (n <= 0) break;
        if (sceIoWrite(out, buffer, static_cast<SceSize>(n)) != n) { ok = false; break; }
        st.bytes += static_cast<unsigned>(n);
        if (n < static_cast<int>(sizeof(buffer))) break;   // (a short read is the end of the file)
    }
    sceIoClose(in);
    sceIoClose(out);
    if (!ok) { sceIoRemove(tmp); return false; }   // never leave half a program behind: vitaGL would load it
    return RenameInto(tmp, to);
}
// `fresh`: the destination folder was made by this copy, so nothing in it can exist yet.
void CopyTree(const char* from, const char* to, int depth, bool fresh, SeedCopyStats& st) {
    const SceUID dir = sceIoDopen(from);
    if (dir < 0) return;
    SceIoDirent entry;
    for (;;) {
        std::memset(&entry, 0, sizeof(entry));
        if (sceIoDread(dir, &entry) <= 0 || st.files + st.failed >= kSeedMaxFiles) break;
        if (entry.d_name[0] == '.') continue;
        // (a seed taken from a finished session may hold that session's own marker)
        if (std::strcmp(entry.d_name, "seeded") == 0) continue;
        char a[320], b[320];
        if (std::snprintf(a, sizeof(a), "%s/%s", from, entry.d_name) >= static_cast<int>(sizeof(a)) ||
            std::snprintf(b, sizeof(b), "%s/%s", to, entry.d_name) >= static_cast<int>(sizeof(b)))
            continue;
        const uint64_t t0 = NowUs();
        if (SCE_S_ISDIR(entry.d_stat.st_mode)) {
            if (depth >= 3) continue;
            const bool made = sceIoMkdir(b, 0777) >= 0;
            if (made) ++st.folders;
            st.folderUs += NowUs() - t0;
            CopyTree(a, b, depth + 1, made, st);
        } else {
            if (CopyFile(a, b, !fresh, st)) ++st.files; else ++st.failed;
            st.fileUs += NowUs() - t0;
            if ((st.files & 15u) == 0) BootProgress(VGXN_BOOT_SEED_COPY, st.files, 0);
        }
    }
    sceIoDclose(dir);
}
// Older libraries: with <cache>/v2/v/00 present vitaGL does not create its 512
// bucket folders at start-up (source/vgl.c: a failed mkdir of ".../v/00" means "they are all
// there"). Every bucket a shader needs is made by vgxvita_glShaderSource instead.
void MakeFirstBuckets(const char* folder, SeedCopyStats& st) {
    static const char* const kParts[] = {"", "/v", "/v/00", "/f", "/f/00"};
    const uint64_t t0 = NowUs();
    for (const char* part : kParts) {
        if (CacheWriterOwnsBuckets() && std::strstr(part, "/00")) continue;
        char path[320];
        if (std::snprintf(path, sizeof(path), "%s/%s%s", folder, VglCacheVersionDir(), part) >= static_cast<int>(sizeof(path))) return;
        if (sceIoMkdir(path, 0777) >= 0) ++st.folders;
    }
    st.folderUs += NowUs() - t0;
}
// The marker file <cache>/seeded holds the size and date of the package's eboot.bin: a newly
// installed package (which may bring a larger seed under the same key, as the one of round 6
// did: 35 vertex shaders changed their text) is looked through once more, and only the files
// this console does not have yet are written. (Markers written before round 6 are empty.)
void PackageStamp(char* out, size_t n) {
    SceIoStat st;
    std::memset(&st, 0, sizeof(st));
    if (sceIoGetstat("app0:eboot.bin", &st) < 0) { std::snprintf(out, n, "no eboot"); return; }
    std::snprintf(out, n, "%lld %04d-%02d-%02d %02d:%02d:%02d", static_cast<long long>(st.st_size), static_cast<int>(st.st_mtime.year),
                  static_cast<int>(st.st_mtime.month), static_cast<int>(st.st_mtime.day), static_cast<int>(st.st_mtime.hour),
                  static_cast<int>(st.st_mtime.minute), static_cast<int>(st.st_mtime.second));
}
bool MarkerHas(const char* marker, const char* stamp) {
    const SceUID f = sceIoOpen(marker, SCE_O_RDONLY, 0);
    if (f < 0) return false;
    char text[80];
    const int n = sceIoRead(f, text, sizeof(text) - 1);
    sceIoClose(f);
    if (n < 0) return false;
    text[n] = 0;
    return std::strcmp(text, stamp) == 0;
}
// ---- W5 BOOT: the package's compiled shaders as ONE file, app0:shadercache/<key>.pack (np platform/vita/tools/make_shader_pack.py)
// What the maintainer first console start showed (17 Oct, hw/w5/play1): every file operation on his memory card costs about 7 ms (645 stats of
// files that were already there: 4.4 s), a new folder about 310 ms (35 folders: 10.9 s), and the frame-1 warm-up opened 226 shader files
// (1,535 ms, 6.8 ms per shader). So the package carries one file: it is read once (one open, a few MiB in large reads), vitaGL asks the
// in-memory index before any cache file (vglSetShaderPackLookup, patch 0004), and nothing is copied at an install or an update. Shaders
// that are not in the pack are compiled and kept in the writable folder as before.
//   pack: "VGXPACK1", u32 count, u32 data start; count x {u64 hash, u32 kind (0 vertex, 1 fragment), u32 offset, u32 size, u32 0},
//         sorted by (kind, hash); the cache files' bytes at their offsets (16-byte aligned).
//   MSC_VITA_SEED=pack (default when the package has a pack) | link | app0 | copy (the folder modes of a package with loose files)
struct PackEntry { uint64_t hash; uint32_t kind, offset, size, pad; };
static_assert(sizeof(PackEntry) == 24, "pack index entry");
uint8_t* g_pack = nullptr;
const PackEntry* g_packIndex = nullptr;
uint32_t g_packCount = 0;
std::atomic<uint32_t> g_packHits{0};
const PackEntry* PackFind(int fragment, uint64_t hash) {
    if (!g_packIndex) return nullptr;
    const uint32_t kind = fragment ? 1u : 0u;
    uint32_t lo = 0, hi = g_packCount;
    while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2;
        const PackEntry& e = g_packIndex[mid];
        if (e.kind < kind || (e.kind == kind && e.hash < hash)) lo = mid + 1;
        else hi = mid;
    }
    if (lo < g_packCount && g_packIndex[lo].kind == kind && g_packIndex[lo].hash == hash) return &g_packIndex[lo];
    return nullptr;
}
// 1.3 [packmiss]: one line for each distinct shader text (by vitaGL's hash: the pack's) that the pack did not answer, and the totals in the periodic shader line. vitaGL asks at glCompileShader
// and again at glLinkProgram, from the render thread and the shader worker, so the table is lock-free (open addressing, 512 places a kind; a full table only stops the lines) and the lines are
// printed by the present (PackMissDrain). A miss is not always a compile: the folder cache may still hold the shader (then it costs a file read, not a compile).
struct PackMissLine { std::atomic<uint32_t> ready{0}; uint64_t hash = 0; uint32_t kind = 0, present = 0; };
std::atomic<uint64_t> g_missTable[2][512];
PackMissLine g_missLines[256];
std::atomic<uint32_t> g_missLookups{0}, g_missDistinct{0}, g_missInPlay{0};
uint32_t g_missPrinted = 0;
void PackMissNote(int fragment, uint64_t hash) {
    if (!g_packIndex) return;
    g_missLookups.fetch_add(1, std::memory_order_relaxed);
    const uint32_t kind = fragment ? 1u : 0u;
    const uint64_t key = hash ? hash : 1u;
    for (uint32_t probe = 0; probe < 512u; ++probe) {
        std::atomic<uint64_t>& slot = g_missTable[kind][(static_cast<uint32_t>(key) + probe) & 511u];
        uint64_t cur = slot.load(std::memory_order_relaxed);
        if (cur == key) return;   // seen before
        if (cur != 0) continue;
        if (!slot.compare_exchange_strong(cur, key, std::memory_order_relaxed)) {
            if (cur == key) return;
            continue;
        }
        const uint32_t n = g_missDistinct.fetch_add(1, std::memory_order_relaxed);
        const uint32_t present = static_cast<uint32_t>(__atomic_load_n(&g_presented, __ATOMIC_RELAXED));
        if (present >= 3) g_missInPlay.fetch_add(1, std::memory_order_relaxed);
        if (n < 256u) {
            g_missLines[n].hash = hash;
            g_missLines[n].kind = kind;
            g_missLines[n].present = present;
            g_missLines[n].ready.store(1, std::memory_order_release);
        }
        return;
    }
}
// Called at every present (render thread): up to 4 pending lines, in the order the misses were found.
void PackMissDrain() {
    uint32_t shown = 0;
    while (shown < 4 && g_missPrinted < 256u && g_missPrinted < g_missDistinct.load(std::memory_order_relaxed)) {
        PackMissLine& l = g_missLines[g_missPrinted];
        if (!l.ready.load(std::memory_order_acquire)) break;
        std::fprintf(stderr, "[vgx-vita] shader pack MISS #%u: %s shader %016llX asked for at present %u is not in the pack (compiled now, or read from the folder cache)\n", g_missPrinted + 1u,
                     l.kind ? "fragment" : "vertex", static_cast<unsigned long long>(l.hash), l.present);
        ++g_missPrinted;
        ++shown;
    }
}
const void* PackLookup(int fragment, uint64_t hash, uint32_t* size) {
    const PackEntry* e = PackFind(fragment, hash);
    if (!e) { PackMissNote(fragment, hash); return nullptr; }
    if (size) *size = e->size;
    g_packHits.fetch_add(1, std::memory_order_relaxed);
    return g_pack + e->offset;
}
bool LoadPack(const char* path, unsigned* ms) {
    const uint64_t t0 = NowUs();
    SceUID f = sceIoOpen(path, SCE_O_RDONLY, 0);
    // gxvita14 (after Galaxy bootio, whose start-ups died about once in 20-200 launches on a single failed open of a file that was there): a package pack that EXISTS but does not open is
    // tried again every 50 ms for up to 1 s, the first failure logged with its code. Only the error path runs it; an open that works costs what it did. A pack that is not there is not retried.
    if (f < 0 && Exists(path)) {
        std::fprintf(stderr, "[vgx-vita] %s exists but did not open (0x%08X): trying again every 50 ms for up to 1 s\n", path, static_cast<unsigned>(f));
        for (int i = 0; i < 20 && f < 0; ++i) {
            sceKernelDelayThread(50 * 1000);
            f = sceIoOpen(path, SCE_O_RDONLY, 0);
        }
        std::fprintf(stderr, "[vgx-vita] %s: %s after the retry\n", path, f >= 0 ? "opened" : "still not open");
    }
    if (f < 0) return false;
    const SceOff size = sceIoLseek(f, 0, SCE_SEEK_END);
    sceIoLseek(f, 0, SCE_SEEK_SET);
    if (size < 16 || size > 64 * 1024 * 1024) { sceIoClose(f); return false; }
    uint8_t* data = static_cast<uint8_t*>(std::malloc(static_cast<size_t>(size)));
    if (!data) { sceIoClose(f); return false; }
    size_t got = 0;
    while (got < static_cast<size_t>(size)) {
        const size_t want = static_cast<size_t>(size) - got > 512u * 1024u ? 512u * 1024u : static_cast<size_t>(size) - got;
        int n = sceIoRead(f, data + got, static_cast<SceSize>(want));
        if (n < 0) {   // gxvita14: an error (not the end of the file) is tried again at the same offset, 5 times 50 ms
            for (int i = 0; i < 5 && n < 0; ++i) {
                sceKernelDelayThread(50 * 1000);
                sceIoLseek(f, static_cast<SceOff>(got), SCE_SEEK_SET);
                n = sceIoRead(f, data + got, static_cast<SceSize>(want));
            }
        }
        if (n <= 0) break;
        got += static_cast<size_t>(n);
    }
    sceIoClose(f);
    uint32_t count = 0, start = 0;
    std::memcpy(&count, data + 8, 4);
    std::memcpy(&start, data + 12, 4);
    bool ok = got == static_cast<size_t>(size) && std::memcmp(data, "VGXPACK1", 8) == 0 && start >= 16 && 16u + static_cast<uint64_t>(count) * sizeof(PackEntry) <= start &&
              start <= static_cast<uint64_t>(size);
    const PackEntry* index = reinterpret_cast<const PackEntry*>(data + 16);
    for (uint32_t i = 0; ok && i < count; ++i)
        ok = (index[i].offset & 3u) == 0 && index[i].offset >= start && static_cast<uint64_t>(index[i].offset) + index[i].size <= static_cast<uint64_t>(size) &&
             (i == 0 || index[i - 1].kind < index[i].kind || (index[i - 1].kind == index[i].kind && index[i - 1].hash < index[i].hash));
    if (!ok) { std::free(data); return false; }
    g_pack = data;
    g_packIndex = index;
    g_packCount = count;
    if (ms) *ms = static_cast<unsigned>((NowUs() - t0) / 1000u);
    return true;
}
// Link mode (W5 SEED's read-through): the package's loose seed folder that vitaGL reads in place ("" = none).
char g_seedLinkPath[220] = "";

// ---- the shaders an older build kept, moved to this build's cache layout (once)
// vitaGL's cache folder version and file names changed with the move to vitaGL 9a14d17 (2 -> 3, short -> 16-digit names), and the key folder changed with its id
// string (LegacyCacheKey). The compiled shaders on a console that ran 1.0/1.1 are still good, but vitaGL looks only in <key>/v3 for names of 16 digits, so
// without this they would be compiled again one by one in play (a console compile is 100 ms and more). Two places are looked through:
//   <this key>/v<other>     a folder of the same key with another cache version (a build that changed only the layout, a seed copied from an older package)
//   <older key>/v2          the folder of the 1.0/1.1 builds (LegacyCacheKey)
// Files are renamed into place (no copy), except those the package's pack already holds (those are read from the pack; moving them would cost a card
// operation each for nothing) and those that exist already. A file that cannot be renamed is copied and the original removed. The old folders are left
// (empty bucket folders and the pack-covered files stay; a card operation costs ~7 ms, deleting hundreds of them is not worth a start-up). Marker
// <this key>/cache-moved: written when the pass ran to the end (not when the file limit stopped it: the next start goes on).
constexpr unsigned kMoveMaxFiles = 3000;
struct MoveStats { unsigned moved = 0, copied = 0, packed = 0, there = 0, failed = 0, folders = 0, scanned = 0; bool limit = false; };
bool ParseShaderFileName(const char* name, uint64_t* hash) {
    const size_t n = std::strlen(name);
    if (n < 5 || n > 20 || std::strcmp(name + n - 4, ".gxp") != 0) return false;   // (a ".gxp.tmp" of an interrupted save is not one)
    uint64_t h = 0;
    for (size_t i = 0; i < n - 4; ++i) {
        const char c = name[i];
        const unsigned v = c >= '0' && c <= '9' ? static_cast<unsigned>(c - '0') : (c >= 'a' && c <= 'f' ? static_cast<unsigned>(c - 'a' + 10) : (c >= 'A' && c <= 'F' ? static_cast<unsigned>(c - 'A' + 10) : 16u));
        if (v > 15) return false;
        h = (h << 4) | v;
    }
    *hash = h;
    return true;
}
void MoveCacheTree(const char* from, const char* to, MoveStats& st) {
    for (int kind = 0; kind < 2 && !st.limit; ++kind) {
        char kindDir[320];
        std::snprintf(kindDir, sizeof(kindDir), "%s/%s", from, kind ? "f" : "v");
        const SceUID kd = sceIoDopen(kindDir);
        if (kd < 0) continue;
        SceIoDirent be;
        for (;;) {
            std::memset(&be, 0, sizeof(be));
            if (sceIoDread(kd, &be) <= 0) break;
            if (be.d_name[0] == '.' || !SCE_S_ISDIR(be.d_stat.st_mode) || std::strlen(be.d_name) != 2) continue;
            char bucketFrom[320], bucketTo[320];
            std::snprintf(bucketFrom, sizeof(bucketFrom), "%s/%s", kindDir, be.d_name);
            std::snprintf(bucketTo, sizeof(bucketTo), "%s/%s/%s", to, kind ? "f" : "v", be.d_name);
            const SceUID bd = sceIoDopen(bucketFrom);
            if (bd < 0) continue;
            bool madeBucket = false;
            SceIoDirent fe;
            for (;;) {
                std::memset(&fe, 0, sizeof(fe));
                if (sceIoDread(bd, &fe) <= 0) break;
                uint64_t hash;
                if (fe.d_name[0] == '.' || SCE_S_ISDIR(fe.d_stat.st_mode) || !ParseShaderFileName(fe.d_name, &hash)) continue;
                if (st.scanned >= kMoveMaxFiles) { st.limit = true; break; }
                ++st.scanned;
                if ((st.scanned & 15u) == 0) BootProgress(VGXN_BOOT_SEED_COPY, st.scanned, 0);
                if (PackFind(kind, hash)) { ++st.packed; continue; }
                char src[360], dst[360];
                std::snprintf(src, sizeof(src), "%s/%s", bucketFrom, fe.d_name);
                ShaderFileName(dst, sizeof(dst), bucketTo, hash, VglCacheVersion());
                if (!madeBucket) { if (sceIoMkdir(bucketTo, 0777) >= 0) ++st.folders; madeBucket = true; }
                if (Exists(dst)) { ++st.there; continue; }
                if (sceIoRename(src, dst) >= 0) { ++st.moved; continue; }
                SeedCopyStats cs;
                if (CopyFile(src, dst, false, cs)) { sceIoRemove(src); ++st.copied; } else ++st.failed;
            }
            sceIoDclose(bd);
            if (st.limit) break;
        }
        sceIoDclose(kd);
    }
}
void MoveOlderCache(const char* folder) {
    char marker[200];
    std::snprintf(marker, sizeof(marker), "%s/cache-moved", folder);
    if (Exists(marker)) return;
    const uint64_t t0 = NowUs();
    char to[200];
    std::snprintf(to, sizeof(to), "%s/%s", folder, VglCacheVersionDir());
    MoveStats st;
    unsigned looked = 0;
    // (they exist already when MakeFirstBuckets ran, or when the library made them in an earlier session; if not, the buckets below could not be made)
    sceIoMkdir(to, 0777);
    for (const char* kind : {"/v", "/f"}) {
        char kindDir[220];
        std::snprintf(kindDir, sizeof(kindDir), "%s%s", to, kind);
        sceIoMkdir(kindDir, 0777);
    }
    // (the same key, another version folder: only the versions that existed before this one)
    for (unsigned v = 2; v < VglCacheVersion() && !st.limit; ++v) {
        char from[200];
        std::snprintf(from, sizeof(from), "%s/v%u", folder, v);
        if (!Exists(from)) continue;
        ++looked;
        MoveCacheTree(from, to, st);
    }
    if (const char* legacy = LegacyCacheKey(); legacy && !st.limit) {
        char from[200];
        std::snprintf(from, sizeof(from), "%s/%s/v2", kShaderCacheDir, legacy);
        if (Exists(from)) { ++looked; MoveCacheTree(from, to, st); }
    }
    if (looked)
        std::fprintf(stderr, "[vgx-vita] shaders of an older cache layout moved to %s: %u renamed, %u copied, %u already there, %u in the package's pack (left), %u failed; "
                             "%u folders made, %u files looked at, %lu ms%s  [+%lu ms]\n", to, st.moved, st.copied, st.there, st.packed, st.failed, st.folders, st.scanned,
                     static_cast<unsigned long>((NowUs() - t0) / 1000u), st.limit ? " (file limit reached: the rest follows at the next start)" : "", BootMs());
    if (st.limit) return;
    const SceUID f = sceIoOpen(marker, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (f >= 0) {
        sceIoWrite(f, "moved\n", 6);
        sceIoClose(f);
    }
}

// Which key folder of the package holds this build's compiled shaders: its own, else (a package made for 1.0/1.1 and not re-keyed) the older one
// LegacyCacheKey names. `key` is "/<8 hex digits>" as strrchr finds it in the cache folder path; returns a string of the same shape.
const char* PackageKeyPath(const char* key) {
    static char chosen[16];
    std::snprintf(chosen, sizeof(chosen), "%s", key ? key : "/none");
    const char* legacy = LegacyCacheKey();
    if (!legacy) return chosen;
    char probe[200];
    std::snprintf(probe, sizeof(probe), "%s%s.pack", kSeedDir, chosen);
    if (Exists(probe)) return chosen;
    std::snprintf(probe, sizeof(probe), "%s%s", kSeedDir, chosen);
    if (Exists(probe)) return chosen;
    std::snprintf(probe, sizeof(probe), "%s/%s.pack", kSeedDir, legacy);
    bool older = Exists(probe);
    if (!older) { std::snprintf(probe, sizeof(probe), "%s/%s", kSeedDir, legacy); older = Exists(probe); }
    if (older) {
        std::snprintf(chosen, sizeof(chosen), "/%s", legacy);
        std::fprintf(stderr, "[vgx-vita] the package's compiled shaders are those of the older key %s (this build's own key has none): used as they are\n", legacy);
    }
    return chosen;
}

// Returns the folder vitaGL is to use as its cache.
const char* SeedShaderCacheInner(const char* folder) {
    const char* key = std::strrchr(folder, '/');
    static char seed[200];
    char marker[200];
    key = PackageKeyPath(key);
    std::snprintf(seed, sizeof(seed), "%s%s", kSeedDir, key);
    std::snprintf(marker, sizeof(marker), "%s/seeded", folder);
    SeedCopyStats st;
    const char* seedMode = std::getenv("MSC_VITA_SEED");
    if (!(seedMode && *seedMode && std::strcmp(seedMode, "pack") != 0)) {
        char packPath[200];
        std::snprintf(packPath, sizeof(packPath), "%s%s.pack", kSeedDir, key);
        void (*setPack)(const void* (*)(int, uint64_t, uint32_t*)) = vglSetShaderPackLookup;
        __asm__("" : "+r"(setPack));   // (a weak function: the compiler must not fold the test away)
        unsigned ms = 0;
        if (!setPack) {
            // A vitaGL without patch 0004 (another VDEPS): the pack's shaders are written to the writable cache once per package instead (the old
            // copy's cost, from one file), so that nothing is compiled on the console.
            if (Exists(packPath) && LoadPack(packPath, &ms)) {
                char stamp[80], marker[200];
                PackageStamp(stamp, sizeof(stamp));
                std::snprintf(marker, sizeof(marker), "%s/seeded", folder);
                MakeFirstBuckets(folder, st);
                if (!MarkerHas(marker, stamp)) {
                    const uint64_t t0 = NowUs();
                    unsigned written = 0, failed = 0;
                    for (uint32_t i = 0; i < g_packCount; ++i) {
                        const PackEntry& e = g_packIndex[i];
                        char dir[320], path[320];
                        std::snprintf(dir, sizeof(dir), "%s/%s/%s/%02X", folder, VglCacheVersionDir(), e.kind ? "f" : "v", static_cast<unsigned>(e.hash >> 56));
                        ShaderFileName(path, sizeof(path), dir, e.hash, VglCacheVersion());
                        if (FileSizeOf(path) == static_cast<long long>(e.size)) continue;   // (whole already; another size is a cut write: written again)
                        sceIoMkdir(dir, 0777);
                        char tmp[336];
                        std::snprintf(tmp, sizeof(tmp), "%s.tmp", path);
                        const SceUID f = sceIoOpen(tmp, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
                        bool ok = f >= 0 && sceIoWrite(f, g_pack + e.offset, e.size) == static_cast<int>(e.size);
                        if (f >= 0) sceIoClose(f);
                        ok = ok ? RenameInto(tmp, path) : (sceIoRemove(tmp), false);
                        if (ok) ++written; else ++failed;
                        if ((written & 15u) == 0) BootProgress(VGXN_BOOT_SEED_COPY, written, g_packCount);
                    }
                    if (failed == 0) {
                        const SceUID f = sceIoOpen(marker, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
                        if (f >= 0) { sceIoWrite(f, stamp, static_cast<SceSize>(std::strlen(stamp))); sceIoClose(f); }
                    }
                    std::fprintf(stderr, "[vgx-vita] the package has a shader pack but this vitaGL has no vglSetShaderPackLookup (patch 0004): %u shaders written to %s (%u failed) in %u ms  [+%lu ms]\n",
                                 written, folder, failed, static_cast<unsigned>((NowUs() - t0) / 1000u), BootMs());
                }
                std::free(g_pack);
                g_pack = nullptr; g_packIndex = nullptr; g_packCount = 0;
                return folder;
            }
        } else if (LoadPack(packPath, &ms)) {
            MakeFirstBuckets(folder, st);
            setPack(PackLookup);
            std::fprintf(stderr, "[vgx-vita] MSC_VITA_SEED=pack: %u compiled shaders of the package read from %s in %u ms (one file, nothing copied); "
                                 "shaders compiled on this console are kept in %s  [+%lu ms]\n", g_packCount, packPath, ms, folder, BootMs());
            return folder;
        } else if (Exists(packPath)) {
            std::fprintf(stderr, "[vgx-vita] %s could not be read or is damaged: not used\n", packPath);
        }
    }
    if (!Exists(seed)) {
        MakeFirstBuckets(folder, st);
        std::fprintf(stderr, "[vgx-vita] no compiled shaders in the package for this key (%s): they are compiled on this console as needed\n", seed);
        return folder;
    }
    if (const char* e = std::getenv("MSC_VITA_SEED"); e && std::strcmp(e, "link") == 0) {
        // W5 SEED: vitaGL (patched, 0002-seed-readthrough.patch) reads the package's compiled shaders in place AND keeps the shaders this console compiles
        // in the writable folder: nothing is copied (a full catalogue is ~560 files and ~330 folders: tens of seconds on a memory card), nothing is lost.
        void (*setSeed)(const char*) = vglSetShaderSeedPath;
        __asm__("" : "+r"(setSeed));   // (a weak function: the compiler must not fold the test away)
        if (setSeed) {
            static char seedPath[220];
            // (the version folder this library writes if the package has it, else the one of an older seed: the library tries both spellings of a file name)
            std::snprintf(seedPath, sizeof(seedPath), "%s/%s", seed, VglCacheVersionDir());
            if (!Exists(seedPath)) {
                char other[220];
                std::snprintf(other, sizeof(other), "%s/v%u", seed, VglCacheVersion() >= 3 ? 2u : 3u);
                if (Exists(other)) std::snprintf(seedPath, sizeof(seedPath), "%s", other);
            }
            MakeFirstBuckets(folder, st);
            setSeed(seedPath);
            std::snprintf(g_seedLinkPath, sizeof(g_seedLinkPath), "%s", seedPath);
            std::fprintf(stderr, "[vgx-vita] MSC_VITA_SEED=link: vitaGL reads the package's compiled shaders in place (%s), new ones are kept in %s; nothing is copied\n", seedPath, folder);
            return folder;
        }
        std::fprintf(stderr, "[vgx-vita] MSC_VITA_SEED=link ignored: this vitaGL has no vglSetShaderSeedPath (the read-through patch is not in it); copying as usual\n");
    }
    if (const char* e = std::getenv("MSC_VITA_SEED"); e && std::strcmp(e, "app0") == 0) {
        std::fprintf(stderr, "[vgx-vita] MSC_VITA_SEED=app0: vitaGL reads the package's compiled shaders in place (%s); nothing is copied, and shaders "
                             "compiled on this console are not kept\n", seed);
        return seed;
    }
    char stamp[80];
    PackageStamp(stamp, sizeof(stamp));
    if (MarkerHas(marker, stamp)) return folder;
    const uint64_t t0 = NowUs();
    CopyTree(seed, folder, 0, false, st);
    MakeFirstBuckets(folder, st);
    std::fprintf(stderr, "[vgx-vita] compiled shaders from the package copied to %s: %u files (%u KiB, %u ms), %u already there, %u failed; %u folders (%u ms); %u ms in all  [+%lu ms]\n",
                 folder, st.files - st.kept, st.bytes / 1024u, static_cast<unsigned>(st.fileUs / 1000u), st.kept, st.failed, st.folders,
                 static_cast<unsigned>(st.folderUs / 1000u), static_cast<unsigned>((NowUs() - t0) / 1000u), BootMs());
    if (st.failed == 0) {
        const SceUID f = sceIoOpen(marker, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
        if (f >= 0) {
            sceIoWrite(f, stamp, static_cast<SceSize>(std::strlen(stamp)));
            sceIoClose(f);
        }
    }
    return folder;
}
// What vitaGL gets as its cache folder, after the package's shaders are in reach and the older cache layout's files are in place.
const char* SeedShaderCache(const char* folder) {
    const char* result = SeedShaderCacheInner(folder);
    if (result == folder) MoveOlderCache(folder);   // (not for MSC_VITA_SEED=app0, where the cache is the package's read-only folder)
    return result;
}
// The warm-up list of the package (the catalogue of W5 SEED: every shader the scripted runs through the game needed).
// A console without a list gets a copy. A console that has one (every start appends the keys it meets, and an earlier
// package may have put its own) gets the union of the two once per installed package: the package's keys first, then
// the console's own; before W5 SEED the package's list was ignored when the console had any, so a bigger catalogue in a
// new package never reached a console that had been started before. The stamp file next to the list holds the size and
// date of the package's eboot.bin, as the shader folder's marker does.
constexpr const char* kShaderListStamp = "ux0:data/mscharged/shadercache/vgx_shaderlist.stamp";
bool ReadWhole(const char* path, std::string& out) {
    const SceUID f = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (f < 0) return false;
    out.clear();
    static char buffer[64 * 1024];
    for (;;) {
        const int n = sceIoRead(f, buffer, sizeof(buffer));
        if (n < 0) { sceIoClose(f); return false; }
        if (n == 0) break;
        out.append(buffer, static_cast<size_t>(n));
    }
    sceIoClose(f);
    return true;
}
bool WriteWhole(const char* path, const std::string& data) {
    const SceUID f = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (f < 0) return false;
    bool ok = true;
    for (size_t at = 0; at < data.size() && ok;) {
        const size_t n = data.size() - at > 65536 ? 65536 : data.size() - at;
        ok = sceIoWrite(f, data.data() + at, static_cast<SceSize>(n)) == static_cast<int>(n);
        at += n;
    }
    sceIoClose(f);
    return ok;
}
void SeedShaderList() {
    char seed[200];
    std::snprintf(seed, sizeof(seed), "%s/vgx_shaderlist.bin", kSeedDir);
    if (!Exists(seed)) return;
    char stamp[80];
    PackageStamp(stamp, sizeof(stamp));
    const bool have = Exists(kShaderListFile);
    if (have && MarkerHas(kShaderListStamp, stamp)) return;
    // nxboot25 R2-11: the parser knows record 5 (the match marks both lists hold since MSC_VGX_WARM_ORDER=1). MSC_SEED_MARKS=0 gives back the old parser (every merge refused: "WRITE FAILED, 0 bytes now").
    const bool marks = !(std::getenv("MSC_SEED_MARKS") && std::getenv("MSC_SEED_MARKS")[0] == '0');
    const uint64_t t0 = NowUs();
    std::string pk;
    if (!ReadWhole(seed, pk)) { std::fprintf(stderr, "[vgx-vita] warm-up list from the package: cannot read %s\n", seed); return; }
    std::string out = pk;
    unsigned added = 0;
    const char* how = "copied";
    if (have) {
        std::string con;
        const vgx_listmerge::List P = vgx_listmerge::Parse(reinterpret_cast<const uint8_t*>(pk.data()), pk.size(), marks);
        const vgx_listmerge::List C = ReadWhole(kShaderListFile, con) ? vgx_listmerge::Parse(reinterpret_cast<const uint8_t*>(con.data()), con.size(), marks) : vgx_listmerge::List();
        if (P.ok && C.ok) {
            out = vgx_listmerge::Merge(P, C, &added);
            how = "merged with this console's own list";
        } else {
            how = P.ok ? "copied over this console's list (unreadable)" : "NOT merged (the package's list is damaged)";
            if (!P.ok) out.clear();
        }
    }
    bool ok = !out.empty();
    if (ok) {
        char tmp[200];
        std::snprintf(tmp, sizeof(tmp), "%s.new", kShaderListFile);
        ok = WriteWhole(tmp, out);
        if (ok) {
            if (have) sceIoRemove(kShaderListFile);
            ok = sceIoRename(tmp, kShaderListFile) >= 0;
        }
        if (!ok) sceIoRemove(tmp);
    }
    if (ok) WriteWhole(kShaderListStamp, std::string(stamp));
    // (nxboot25: a list that was refused is said so; "WRITE FAILED" is for a write that failed. MSC_SEED_MARKS=0 shows the old line for the old parser: "NOT merged".)
    std::fprintf(stderr, "[vgx-vita] warm-up list from the package (%u bytes): %s%s, %u records of this console's list added, %u bytes now, %u ms (MSC_SEED_MARKS %s)\n",
                 static_cast<unsigned>(pk.size()), ok ? how : (out.empty() ? how : "WRITE FAILED"), "", added, static_cast<unsigned>(out.size()), static_cast<unsigned>((NowUs() - t0) / 1000u), marks ? "1: record 5 read" : "0: old parser");
}

// sceGxm's display-queue thread and vitaGL's collector are created inside the libraries with
// affinity "any", at priorities that can take core 0 from the game thread. They are found by
// name in the process's thread ids (the walk the ACGC port does, vita_platform.c
// vita_pin_hidden_threads) and moved: the display queue to the render thread's core, the
// collector to the audio core (it is created there already, see StartGl; this covers a
// vitaGL that ignores the request). Threads can appear late, so this runs again at a few
// presents (PresentDisplayCopy).
void PinLibraryThreads(bool log) {
    SceKernelThreadInfo info;
    int missesInRow = 0, moved = 0, seen = 0;
    for (SceUID uid = 0x40010001; uid < 0x40020000 && missesInRow < 500; ++uid) {
        if (sceKernelGetThreadmgrUIDClass(uid) != SCE_KERNEL_TMID_Thread) { ++missesInRow; continue; }
        std::memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelGetThreadInfo(uid, &info) != 0) { ++missesInRow; continue; }
        missesInRow = 0;
        ++seen;
        int target = 0;
        static bool gcRefused = false;   // (1.3 MSC_VITA_CORE3_SEND=2: the system refused the collector the fourth core once: its plan core from then on)
        const bool gc = std::strstr(info.name, "Garbage Collector") != nullptr;
        const bool dq = !gc && !std::strcmp(info.name, "SceGxmDisplayQueue");
        if (gc) target = VitaThreads::CoreMask(gcRefused ? (VitaThreads::kCollector | VitaThreads::kPlan) : VitaThreads::kCollector);
        else if (dq) target = VitaThreads::CoreMask(VitaThreads::kDisplayQueue);
        // int26 (Carbon #7): keep SceGxmDisplayQueue strictly above RenderThread (148), frame send (144) and np_jobs (144) on Core 1 so buffer flips are never preempted.
        static bool dqPrioTried = false;
        if (dq && !dqPrioTried && info.currentPriority > 132) {
            dqPrioTried = true;
            const int rp = sceKernelChangeThreadPriority(uid, 0x10000100 - 28);   // relative -28 (abs 132)
            std::fprintf(stderr, "[vgx-vita] thread \"SceGxmDisplayQueue\" priority %d -> 132: %s\n", info.currentPriority, rp >= 0 ? "ok" : "refused");
        }
        if (target && info.currentCpuAffinityMask != static_cast<SceUInt32>(target)) {
            int res = sceKernelChangeThreadCpuAffinityMask(uid, target);
            if (res < 0 && gc && (target & 0x80000) && !gcRefused) {   // (SCE_KERNEL_CPU_MASK_SYSTEM)
                gcRefused = true;
                target = VitaThreads::CoreMask(VitaThreads::kCollector | VitaThreads::kPlan);
                std::fprintf(stderr, "[vgx-vita] the fourth core was refused for \"%s\" (0x%X): back to its plan core\n", info.name, static_cast<unsigned>(res));
                res = sceKernelChangeThreadCpuAffinityMask(uid, target);
            }
            ++moved;
            std::fprintf(stderr, "[vgx-vita] thread \"%s\" (priority 0x%X) moved to core mask 0x%X: %s\n", info.name, static_cast<unsigned>(info.currentPriority),
                         static_cast<unsigned>(target), res >= 0 ? "ok" : "refused");
        }
    }
    // (Vita3K does not implement sceKernelGetThreadmgrUIDClass: there the scan sees nothing,
    // and only a console exercises it. Found by the emulator workstream.)
    if (log) std::fprintf(stderr, "[vgx-vita] library threads: %d threads seen, %d moved%s\n", seen, moved,
                          seen == 0 ? " (none seen: expected in the emulator, a fault on a console)" : "");
}

bool StartGl() {
    if (np_vita_radio_set_dialog_ready) np_vita_radio_set_dialog_ready(false);
    // The shader compiler module is the user's own copy of a system file; all
    // of the renderer's shaders are compiled with it on first use.
    const bool compilerA = Exists("ur0:data/libshacccg.suprx"), compilerB = Exists("ur0:data/external/libshacccg.suprx");
    std::fprintf(stderr, "[vgx-vita] shader compiler module: ur0:data/libshacccg.suprx %s, ur0:data/external/libshacccg.suprx %s\n",
                 compilerA ? "present" : "absent", compilerB ? "present" : "absent");
    if (!compilerA && !compilerB) {
        g_startCause = 1;
        std::snprintf(g_startError, sizeof(g_startError),
                      "The shader compiler module is missing.\n"
                      "Put libshacccg.suprx into ur0:data/ (on a console: extract it with\n"
                      "ShaRKBR33D or the PSM runtime guide; in Vita3K: copy the same file into\n"
                      "the emulator's ur0/data folder).");
        return false;
    }

    LogMemory("before vitaGL");
    SceKernelFreeMemorySizeInfo free;
    std::memset(&free, 0, sizeof(free));
    free.size = sizeof(free);
    sceKernelGetFreeMemorySize(&free);
    // vitaGL's general pool (its rings and uniform ring, vertex and index data, everything it
    // allocates "for the CPU"). Leave 40 MiB of main memory to the system for sceGxm's own
    // buffers (16 MiB parameter buffer), thread stacks and the shader compiler; take at most
    // 96 MiB (MSC_VITA_VGL_RAM_MIB in env.txt changes the limit).
    // Measured in Vita3K over a whole match and the start of the next (v3k/NOTES.md session 4,
    // run c1full, with the limit at 64 MiB as it was until then): the pool was FULL (65.3 MiB
    // at its peak, 35 allocations of 3-5 MiB refused) and vitaGL put the overflow, up to
    // 19.2 MiB at once, into the physically contiguous pool. A console in the 365 MiB mode may
    // have no such memory to give, and the overflow would then go to video memory, which was
    // at 91.6 of 109 MiB itself. Main memory is the budget with room: about 85 MiB unassigned.
    const int64_t reserve = 40ll * 1024 * 1024;
    int64_t ramLimitMib = 96;
    if (const char* e = std::getenv("MSC_VITA_VGL_RAM_MIB"); e && *e) ramLimitMib = std::atoi(e);
    if (ramLimitMib < 16) ramLimitMib = 16;
    if (ramLimitMib > 192) ramLimitMib = 192;   // (vitaGL caps an uncached pool at 200 MiB)
    int64_t ram = static_cast<int64_t>(free.size_user) - reserve;
    if (ram > ramLimitMib * 1024 * 1024) ram = ramLimitMib * 1024 * 1024;
    if (ram < 16ll * 1024 * 1024) ram = 16ll * 1024 * 1024;
    // Video memory (display buffers, render targets, textures): what is free MINUS what sceGxm
    // takes for itself when vitaGL initialises it, and whatever physically contiguous memory
    // the system offers. FIRST CONSOLE RUN (5 Oct, hw/run2-gl): this asked for all free video
    // memory but 1 MiB (111.6 of 112.6 MiB); sceGxmInitialize had taken its 16 MiB parameter
    // buffer from the same budget by the time vitaGL allocated its pool, the allocation failed,
    // vitaGL carried on without a video-memory pool, and the first query of that pool
    // dereferenced a null mspace in SceLibKernel (data abort at 0x1BC). Vita3K does not limit
    // memory, so the emulator never showed it. MSC_VITA_CDRAM_RESERVE_MIB in env.txt changes
    // the reserve (default 18 = parameter buffer 16 + 2).
    // MSC_VITA_PB_MIB=<4..64> (1.3, console A/B only): the parameter buffer holds the transformed geometry of the scenes the GPU has not finished. When it fills up in a
    // scene the GPU renders what it has (a partial render: tiles written out and read back) and goes on; that is invisible in a picture, shows only as a GPU time
    // spike, and no counter of it exists on a retail console (sceRazorGpuLive's partial_render flag needs a devkit). So this is the test: if heavy scenes get
    // steadier at 32, partial renders are real here; if 8 changes nothing, 8 MiB of video memory are free for textures. The reserve follows (parameter buffer + 2).
    int pbMib = 16;
    if (const char* e = std::getenv("MSC_VITA_PB_MIB"); e && *e) {
        pbMib = std::atoi(e);
        if (pbMib < 4) pbMib = 4;
        if (pbMib > 64) pbMib = 64;
        // 1.3 (w11/vram19, review item 10): a smaller parameter buffer makes partial renders likelier, and a scene on a surface with no depth memory (patch 0038:
        // MSC_VGX_NODEPTH_PASS, 0042: MSC_VGX_DISPDEPTH=nomem) "would have nothing to spill to". The two are never run together: the smaller buffer is refused.
        const char* nd = std::getenv("MSC_VGX_NODEPTH_PASS");
        const char* dd = std::getenv("MSC_VGX_DISPDEPTH");
        const bool noDepthOn = (nd && std::atol(nd) > 0) || (dd && (!std::strcmp(dd, "nomem") || !std::strcmp(dd, "1")));
        if (pbMib < 16 && noDepthOn) {
            std::fprintf(stderr, "[vgx-vita] MSC_VITA_PB_MIB=%d REFUSED: a parameter buffer smaller than 16 MiB with a depth-less surface (MSC_VGX_NODEPTH_PASS=%s MSC_VGX_DISPDEPTH=%s) "
                                 "leaves a partial render nothing to spill to; the buffer stays at 16 MiB\n", pbMib, nd ? nd : "0", dd ? dd : "0");
            pbMib = 16;
        } else {
            vglSetParamBufferSize(static_cast<uint32_t>(pbMib) * 1024u * 1024u);
            std::fprintf(stderr, "[vgx-vita] MSC_VITA_PB_MIB: parameter buffer %d MiB\n", pbMib);
        }
    }
    // 1.3 (vram19, review item 8) MSC_VITA_PB_LPDDR=0|1 (default 0, console only): ask sceGxm for the parameter buffer in main memory (SCE_GXM_INITIALIZE_FLAG_PB_LPDDR, which vitaGL
    // sets only in system-app mode). Whether a normal application may have it, and what it costs the GPU, only a console tells: the next line then shows the video memory request
    // about the buffer's size larger, and the [vgx-vita] PB_LPDDR line after vglInit says whether sceGxm took it (a refusal falls back and the pool request is cut back by vitaGL).
    const bool pbLpddr = [] { const char* e = std::getenv("MSC_VITA_PB_LPDDR"); return e && *e && std::atoi(e) != 0; }();
    if (pbLpddr) vglSetParamBufferLpddr(1);
    // gxvita14 (after Galaxy bowser24, GXV_VITA_VDM_KIB / _VERTEX_KIB / _FRAGMENT_KIB / _USSE_KIB): the other command rings of the sceGxm context, default 0 = vitaGL's own sizes (VDM 128, vertex 2048,
    // fragment 512, USSE 16 KiB) and no call made, so nothing changes unless a key is set. A ring that is too small makes sceGxmReserve*/Draw wait for the GPU to drain it. Console only: a
    // GPU-bound scene (The Dump) could show it in the [be-split]-style reserve / draw times first. Galaxy's own A/B at a locked 30 found no gain (the lock hides it); at 60 it was never run.
    {
        long vdm = 0, vtx = 0, frag = 0, usse = 0;
        const auto env_int = [](const char* name, long hi) { const char* e = std::getenv(name); long v = e && *e ? std::atol(e) : 0; return v < 0 ? 0 : (v > hi ? hi : v); };
        vdm = env_int("MSC_VITA_VDM_KIB", 4096); vtx = env_int("MSC_VITA_VERTEX_KIB", 16384); frag = env_int("MSC_VITA_FRAGMENT_KIB", 8192); usse = env_int("MSC_VITA_USSE_KIB", 1024);
        if (vdm) vglSetVDMBufferSize(static_cast<uint32_t>(vdm) * 1024u);
        if (vtx) vglSetVertexBufferSize(static_cast<uint32_t>(vtx) * 1024u);
        if (frag) vglSetFragmentBufferSize(static_cast<uint32_t>(frag) * 1024u);
        if (usse) vglSetUSSEBufferSize(static_cast<uint32_t>(usse) * 1024u);
        if (vdm || vtx || frag || usse)
            std::fprintf(stderr, "[vgx-vita] sceGxm rings: VDM %ld KiB, vertex %ld KiB, fragment %ld KiB, USSE %ld KiB (0 = vitaGL's default; MSC_VITA_VDM_KIB / _VERTEX_KIB / _FRAGMENT_KIB / _USSE_KIB)\n", vdm, vtx, frag, usse);
    }
    int64_t cdramReserveMib = pbLpddr ? 2 : pbMib + 2;
    if (const char* e = std::getenv("MSC_VITA_CDRAM_RESERVE_MIB"); e && *e) cdramReserveMib = std::atoi(e);
    if (cdramReserveMib < 1) cdramReserveMib = 1;
    int64_t cdram = static_cast<int64_t>(free.size_cdram) - cdramReserveMib * 1024 * 1024;
    cdram &= ~static_cast<int64_t>(256 * 1024 - 1);
    if (cdram < 0) cdram = 0;
    const int64_t phycont = free.size_phycont > 0 ? free.size_phycont : 0;
    std::fprintf(stderr, "[vgx-vita] vitaGL pools requested: ram %u KiB, cdram %u KiB, phycont %u KiB\n",
                 static_cast<unsigned>(ram / 1024), static_cast<unsigned>(cdram / 1024), static_cast<unsigned>(phycont / 1024));

    // Varyings get fixed semantics. vgx compiles vertex and fragment shaders
    // independently and links them in many combinations, so the translator's
    // name-order assignment (which depends on which shader of a pair it sees
    // first) cannot be used. v_uv is the back end's EFB-copy program, which
    // never shares a shader with the generated ones.
    vglSetSemanticBindingMode(VGL_MODE_GLOBAL);
    static const char* const kTex[8] = {"v_tex0", "v_tex1", "v_tex2", "v_tex3", "v_tex4", "v_tex5", "v_tex6", "v_tex7"};
    for (int i = 0; i < 8; ++i) vglAddSemanticBinding(reinterpret_cast<const GLchar* const*>(kTex[i]), i, VGL_TYPE_TEXCOORD);
    vglAddSemanticBinding(reinterpret_cast<const GLchar* const*>("v_col0"), 8, VGL_TYPE_TEXCOORD);
    vglAddSemanticBinding(reinterpret_cast<const GLchar* const*>("v_col1"), 9, VGL_TYPE_TEXCOORD);
    vglAddSemanticBinding(reinterpret_cast<const GLchar* const*>("v_uv"), 0, VGL_TYPE_TEXCOORD);
    vglAddSemanticBinding(reinterpret_cast<const GLchar* const*>("v_fz"), 0, VGL_TYPE_FOG); // int32: bind MSC_VGX_FOGVTX eye-depth varying to hardware FOGC (no effect on shaders without v_fz)

    // sceGxm sizes a render target for a number of scenes per frame (1 to 8);
    // a further scene on the same target in the same frame waits for the GPU
    // to finish an earlier one. vitaGL's default is 1 for framebuffer objects.
    // The renderer returns to the EFB after every EFB copy (43 copies in an
    // open-play frame), so its framebuffer objects get the maximum; the
    // display itself is drawn to once per frame.
    int fboScenes = 8;
    if (const char* e = std::getenv("MSC_VITA_FBO_SCENES")) fboScenes = std::atoi(e);
    if (fboScenes < 1) fboScenes = 1;
    if (fboScenes > 8) fboScenes = 8;
    // MSC_VITA_DISPLAY_SCENES=<1..8> (1.2, console A/B only): the same for the display's own render target (the picture to the screen: the sharpening
    // pass and the overlays). Default 1 (vitaGL's); the display is drawn to once per frame, so more is expected to change nothing, but a second
    // scene (the console text layer, the boot screen) on the same target within a frame waits for the first.
    int displayScenes = 1;
    if (const char* e = std::getenv("MSC_VITA_DISPLAY_SCENES"); e && *e) displayScenes = std::atoi(e);
    if (displayScenes < 1) displayScenes = 1;
    if (displayScenes > 8) displayScenes = 8;
    vglSetupRenderTargetScenesNum(static_cast<uint8_t>(displayScenes), static_cast<uint8_t>(fboScenes));
    g_vglFboScenes = fboScenes;
    g_vglDisplayScenes = displayScenes;
    std::fprintf(stderr, "[vgx-vita] render targets: %d scene(s) per frame for framebuffer objects (MSC_VITA_FBO_SCENES in env.txt changes it), %d for the display (MSC_VITA_DISPLAY_SCENES)\n", fboScenes, displayScenes);

    // The shader patcher holds every linked program and every patched variant of one (blend
    // state, vertex layout). vitaGL's default is 1 MiB for each of its three pools; the ACGC
    // port needed 4 / 2 / 2 MiB for some 240 small programs, and ours are 78 programs with
    // 155 variants of much larger shaders. When a pool is exhausted vitaGL logs "shader
    // patching failed" and draws with a stale program.
    // W5 SEED (Galaxy 98a0d5e uses 8 / 4 / 4 for its 736-program warm-up): the catalogue of this
    // game is 1,054 programs / 1,663 variants (690 distinct (fragment shader, blend) pairs, 234
    // (vertex shader, layout) pairs; the patcher keys a fragment program on its vertex shader too:
    // 1,659 (vs, fs, blend) combinations). At about 1.3 KB of code per patched fragment program
    // [E, from the 543 pairs of the first union: 0.7 MB] all of it would be 2.2 MB of fragment
    // code, more than the 2 MiB pool of the 4 / 2 / 2 setting; with MSC_VGX_MAXPROGRAMS (default
    // 900) a long session holds up to ~1,400 combinations = 1.8 MB, 90 % of that pool. So the
    // default is 8 / 4 / 4 MiB (the 8 in video memory, 4 + 4 of main memory, which the 96 MiB vitaGL pool limit leaves
    // room for: about 85 MiB unassigned). The 27 + 48 "patching failed" lines of the Vita3K compile
    // run of the whole catalogue were NOT pool exhaustion (all SCE_GXM_ERROR_INVALID_POINTER on
    // shader 0x0: the 30 programs past vitaGL's 1,024-slot table); Vita3K does not model the pools.
    // MSC_VITA_USSE_CDRAM=1 (1.3, default 0, console A/B): the code of every shader (the patcher's vertex and fragment pools and the fragment USSE ring) in video memory, the GPU's own
    // memory path, instead of main memory, the one place in the renderer where something the GPU reads for every vertex and fragment did not go to the fast memory (vitaGL's default,
    // vglUseVramForUSSE). Costs the two code pools (8 MiB at the default, MSC_VITA_PATCHER_MIB; about 2 MB of them used) of video memory that textures could have had.
    if (const char* e = std::getenv("MSC_VITA_USSE_CDRAM"); e && *e && *e != '0') {
        vglUseVramForUSSE(GL_TRUE);
        std::fprintf(stderr, "[vgx-vita] MSC_VITA_USSE_CDRAM: shader code in video memory\n");
    }
    unsigned patcherMib = 8;
    if (const char* e = std::getenv("MSC_VITA_PATCHER_MIB")) patcherMib = static_cast<unsigned>(std::atoi(e));
    if (patcherMib < 1) patcherMib = 1;
    if (patcherMib > 16) patcherMib = 16;
    // 1.3 (vram19, review item 6): the first argument of vglSetupShaderPatcher is the patcher's bufferMem (gxm.c start_shader_patcher: gpu_alloc_mapped_aligned_for_gpu = VIDEO memory
    // first); only the second and third (the vertex and fragment code pools) are main memory (unless MSC_VITA_USSE_CDRAM). So one knob gave 8 MiB of video memory for a buffer that the
    // [vgx] vitaGL: line reports at "patcher ... buffer=N KiB" (vitaGL's own default 1 MiB, ACGC needed 4 MiB with ~240 small programs). MSC_VGL_PATCHER_BUF_KB (256..16384) is that
    // buffer alone; the code pools stay (MSC_VITA_PATCHER_MIB + 1) / 2 each. DEFAULT 8192 KiB (as before): no console figure of the buffer's high-water exists yet (Vita3K does not model the
    // patcher: buffer=0 KiB), so the size is not lowered until the console run "patcherbuf" (high-water line "[vgx-vita] patcher buffer high-water") shows it. Suggested size from
    // the figure: 2 x the high-water, rounded up to 512 KiB, at least 1024.
    unsigned patcherBufKb = patcherMib * 1024u;
    if (const char* e = std::getenv("MSC_VGL_PATCHER_BUF_KB"); e && *e) {
        const long v = std::atol(e);
        if (v >= 256 && v <= 16384) patcherBufKb = static_cast<unsigned>(v);
        else std::fprintf(stderr, "[vgx-vita] MSC_VGL_PATCHER_BUF_KB=%s ignored (256..16384)\n", e);
    }
    g_patcherBufKb = patcherBufKb;
    vglSetupShaderPatcher(patcherBufKb * 1024u, (patcherMib + 1) / 2 * 1024u * 1024u, (patcherMib + 1) / 2 * 1024u * 1024u);
    std::fprintf(stderr, "[vgx-vita] shader patcher buffer (video memory) %u KiB (MSC_VGL_PATCHER_BUF_KB)\n", patcherBufKb);
    vglSetupGarbageCollector(kGcPriority, VitaThreads::CoreMask(VitaThreads::kCollector | VitaThreads::kPlan));   // (MSC_VITA_CORE3_SEND=2: created on its plan core, moved by PinLibraryThreads)
    std::fprintf(stderr, "[vgx-vita] shader patcher pools %u / %u / %u MiB (MSC_VITA_PATCHER_MIB in env.txt changes the first), collector thread core mask 0x%X\n",
                 patcherMib, (patcherMib + 1) / 2, (patcherMib + 1) / 2, static_cast<unsigned>(VitaThreads::CoreMask(VitaThreads::kCollector)));

    // Per-frame vertex and index data through vitaGL's scratch memory (VRS, 16 Oct; the idea is
    // Mario Kart Wii Vita's vglAllocFromScratch use and strikersVita's scratch for stream
    // buffers, both measured on consoles; here it is vitaGL's own switch). The renderer
    // re-specifies its GL_STREAM_DRAW buffers (the frame's vertex arena chunks and the index
    // buffer, src/vgx_backend_gl.cpp UploadStream) once per frame, before any draw that reads
    // them; with HAVE_SCRATCH_MEMORY vitaGL then cuts them from the circular data pool (a bump
    // pointer, no malloc/free, no garbage-collector entry per buffer per frame) instead of its
    // general pool. A scratch buffer lives 3 frames: that is NEVER right for the static
    // buffers (GL_DYNAMIC_DRAW hint, src/vgx_backend_gl.cpp g_staticUsage; they live for the
    // whole run), and a vitaGL built with HAVE_SCRATCH_MEMORY defaults BOTH hints to scratch,
    // so the call below is made on every start: dynamic always off, stream on unless
    // MSC_VITA_VGL_SCRATCH=0. In a vitaGL built without HAVE_SCRATCH_MEMORY (the strict deps
    // of the first console builds) the call does nothing. The pool is vitaGL's 32 MiB split in
    // three slices, one per display buffer; MSC_VITA_VGL_CIRC_MIB changes its size (an
    // overrun falls back to the general pool, [vitaGL] "Circular pool overrun" in log.txt).
    {
        int scratch = 1;
        if (const char* e = std::getenv("MSC_VITA_VGL_SCRATCH"); e && *e) scratch = std::atoi(e) != 0;
        int circMib = 0;
        if (const char* e = std::getenv("MSC_VITA_VGL_CIRC_MIB"); e && *e) circMib = std::atoi(e);
        if (circMib > 0) {
            if (circMib < 6) circMib = 6;
            if (circMib > 64) circMib = 64;
            vglSetCircularPoolSize(static_cast<uint32_t>(circMib) * 1024u * 1024u);
        }
        vglSetupScratchMemory(GL_FALSE, scratch ? GL_TRUE : GL_FALSE);
        std::fprintf(stderr, "[vgx-vita] vitaGL scratch memory for stream buffers: %s (MSC_VITA_VGL_SCRATCH=0 turns it off; effective only in a vitaGL built with HAVE_SCRATCH_MEMORY), "
                     "circular pool %s (MSC_VITA_VGL_CIRC_MIB changes it)\n", scratch ? "on" : "off",
                     circMib > 0 ? (std::to_string(circMib) + " MiB").c_str() : "vitaGL default 32 MiB");
    }

#ifdef VITAGL_GLB
    // W5 GLB: the patched vitaGL (np platform/vita/vitagl, tools/vitadeps_glb.sh). Everything here is OFF unless set in env.txt.
    //   MSC_VITA_VGL_STREAM_KIB=<n>   the frame's GL_STREAM_DRAW buffers (vertex chunks, indices; 0.5 to 1.3 MB per open-play frame, 8.3 MB at the start of
    //                                 a match) go to an ARENA of n KiB per display buffer (2048 to 4096 is plenty) instead of the circular pool in uncached
    //                                 RAM, where CPU writes run at 0.25 GB/s (3.9 ms per MB measured on 5 Oct). Needs scratch memory for streams
    //                                 (MSC_VITA_VGL_SCRATCH, default on). More than n KiB in one frame spills into the circular pool (counted, correct).
    //   MSC_VITA_VGL_STREAM_MEM=cdram|cached|uncached   what the arena is made of (default cdram):
    //        cdram     CDRAM, write-combined; the GPU reads it fastest; nothing to write back (the safe choice)
    //        cached    cached RAM: CPU writes at cache speed, written back once per frame before the first draw with MSC_VITA_VGL_FLUSH
    //        uncached  uncached RAM like the pool (diagnostic: the arena mechanism alone)
    //   MSC_VITA_VGL_FLUSH=kubridge|syncvm|none   how a cached arena is written back: kuKernelFlushCaches of kubridge.skprx (default when the plug-in is
    //                                 installed; it writes back through L2), sceKernelSyncVMDomain (documented for VM-domain blocks; whether it does anything for
    //                                 a plain block, and reaches RAM past the L2, is unknown: a stale picture means it does not), or nothing (diagnostic)
    //   MSC_VITA_VGL_RING_MEM=cdram    the uniform ring (every draw copies its uniform buffers into it: ~330 KB per open-play frame) in CDRAM (write-combined)
    //                                 instead of the pool's uncached RAM
    //   MSC_VITA_VGL_SKIP=1           do not forward sceGxm calls that set what the context already has (vertex/fragment program, stream, texture)
    //   MSC_VGX_GXMCACHE=1|<mask>     the same for the state setters (depth, stencil, cull/polygon, viewport/clip, vertex textures, uniform buffers); needs a GXM_STATE_CACHE library
    {
        // MSC_VITA_VBUF_CACHED=0|1 (vbuf15, after the Azahar Vita port's cached vertex pages; default 0 = no change): 1 is the two keys below set to a cached arena of 2048 KiB per display
        // buffer, written back once per run of writes before the draw (vitaGL 0054 covers every draw entry and the map calls). A key that is set itself (STREAM_KIB, STREAM_MEM) wins. The
        // choice is made at allocation, so it is a separate run, not an in-run A/B. The uniform ring stays in the uncached pool (MSC_VITA_WRCOPY serves it).
        int vbufCached = 0;
        if (const char* e = std::getenv("MSC_VITA_VBUF_CACHED"); e && *e) vbufCached = std::atoi(e) != 0;
        // MSC_VITA_STREAM_CACHED=0|1 (cache25, vitaGL 0060; default 0 = no change): the same cached arena as MSC_VITA_VBUF_CACHED=1 (2048 KiB a display buffer, written by the CPU at cache speed instead of
        // 0.34 to 1 GB/s into uncached RAM) but written back ONCE per scene submit (before sceGxmEndScene, vitaGL scene_end) instead of before every draw (VBUF_CACHED measured 13.6 -> 17.4 ms back end on
        // 23 Nov, ~330 write-back calls a picture is the suspect). It wins over MSC_VITA_VBUF_CACHED. Chosen at allocation: a separate run. A wrong picture (shards) = the GPU read before the scene submit: stop.
        int streamCached = 0;
        if (const char* e = std::getenv("MSC_VITA_STREAM_CACHED"); e && *e) streamCached = std::atoi(e) != 0;
        if (streamCached) vbufCached = 1;
        int streamKib = vbufCached ? 2048 : 0;
        if (const char* e = std::getenv("MSC_VITA_VGL_STREAM_KIB"); e && *e) streamKib = std::atoi(e);
        if (streamKib < 0) streamKib = 0;
        if (streamKib > 16384) streamKib = 16384;
        int memKind = vbufCached ? VGL_STREAM_CACHED : VGL_STREAM_CDRAM;
        const char* memName = vbufCached ? "cached" : "cdram";
        if (const char* e = std::getenv("MSC_VITA_VGL_STREAM_MEM"); e && *e) {
            if (std::strcmp(e, "cached") == 0) { memKind = VGL_STREAM_CACHED; memName = "cached"; }
            else if (std::strcmp(e, "uncached") == 0) { memKind = VGL_STREAM_UNCACHED; memName = "uncached"; }
        }
        const char* flushEnv = std::getenv("MSC_VITA_VGL_FLUSH");
        const bool explicitFlush = flushEnv && *flushEnv;
        const char* mode = kuKernelFlushCaches ? "kubridge" : "syncvm";
        if (explicitFlush) mode = flushEnv;
        // int31 (Carbon B1): plain USER_RW cached RAM requires kubridge.skprx (kuKernelFlushCaches) to write back ARM L1/L2 D-cache;
        // sceKernelSyncVMDomain does not flush plain USER_RW blocks, so without kubridge SGX would read stale vertices/indices (shards).
        // Fall back safely to the uncached circular pool + wrcopy unless the user explicitly overrides MSC_VITA_VGL_FLUSH.
        bool noKubridgeFallback = false;
        if (memKind == VGL_STREAM_CACHED && streamKib > 0 && !kuKernelFlushCaches && !explicitFlush) {
            std::fprintf(stderr, "[streamc] kubridge.skprx not loaded (kuKernelFlushCaches unavailable): disabling cached stream arena (falling back to uncached circular pool + wrcopy)\n");
            streamKib = 0;
            memName = "uncached-fallback";
            noKubridgeFallback = true;
        }
        vglCacheFlushFn fn = nullptr; // vitaGL's own: sceKernelSyncVMDomain on the arena
        if (std::strcmp(mode, "none") == 0) fn = [](void*, uint32_t) {};
        else if (std::strcmp(mode, "kubridge") == 0 && kuKernelFlushCaches) fn = [](void* a, uint32_t n) { kuKernelFlushCaches(a, n); };
        else mode = "syncvm";
        vglSetupStreamArena(static_cast<uint32_t>(streamKib) * 1024u, memKind, fn);
        g_streamSceneFlush = streamCached && memKind == VGL_STREAM_CACHED && streamKib > 0;
        vglSetStreamFlushAtScene(g_streamSceneFlush ? GL_TRUE : GL_FALSE);
        if (streamCached && !noKubridgeFallback) std::fprintf(stderr, "[streamc] MSC_VITA_STREAM_CACHED=1: stream arena %d KiB per display buffer, %s, write-back %s once per scene submit%s\n", streamKib, memName, mode,
                                       g_streamSceneFlush ? "" : " (NOT in force: the arena is not cached RAM or has no size)");
        int ringCdram = 0;
        if (const char* e = std::getenv("MSC_VITA_VGL_RING_MEM"); e && *e) ringCdram = std::strcmp(e, "cdram") == 0;
        vglSetUniformRingMem(ringCdram);
        int skip = 0;
        if (const char* e = std::getenv("MSC_VITA_VGL_SKIP"); e && *e) skip = std::atoi(e) != 0;
        vglSetSkipRedundantSets(skip ? GL_TRUE : GL_FALSE);
        // MSC_VGX_GXMCACHE (1.2, vitaGL 0013 GXM_STATE_CACHE): the state setters of the draw path and of GL's state calls (depth function and write mode, stencil, polygon
        // and cull mode, depth bias, viewport, region clip, vertex textures, default uniform buffers) are not forwarded to sceGxm when they repeat the last forwarded call
        // of the scene. 0 = off (default), 1 = every group, else the group mask (hex ok: 0x01 depth, 0x02 stencil, 0x04 cull/polygon/bias, 0x08 viewport/clip, 0x10 vertex
        // textures, 0x20 uniform buffers). Does nothing in a library built without GXM_STATE_CACHE (the log line says so). MSC_VITA_VGL_SKIP stays the switch of the
        // program / stream / fragment texture part.
        uint32_t stateMask = 0;
        if (const char* e = std::getenv("MSC_VGX_GXMCACHE"); e && *e) {
            const unsigned long v = std::strtoul(e, nullptr, 0);
            stateMask = v == 1 ? 0x3Fu : static_cast<uint32_t>(v & 0x3Fu);
        }
        const uint32_t stateIn = vglSetStateCache ? vglSetStateCache(stateMask) : 0u;
        // MSC_VGX_LAZYCLEAR (1.2, vitaGL 0015 LAZY_CLEAR): glClear leaves the sceGxm state it set for its quad in place and sets it back only when something needs it
        // (the next draw, a scene change, a setter of that state); clears that follow each other set and restore nothing between them. The picture is the same.
        // Default 1 (the player build's kSpeed list); =0 is the classic glClear. Does nothing in a library built without LAZY_CLEAR (the log line says so).
        int lazyClear = 0;
        if (const char* e = std::getenv("MSC_VGX_LAZYCLEAR"); e && *e) lazyClear = std::atoi(e) != 0;
        if (vglSetLazyClear) vglSetLazyClear(lazyClear ? GL_TRUE : GL_FALSE);
        // MSC_VGX_ONESTREAM=1 (STR-076): glDrawElements of an interleaved layout gets one hardware stream instead of one per attribute (vitaGL
        // vglSetOneStream; needs a vitaGL built with DRAW_SPEEDHACK, otherwise it does nothing). Default off until a console A/B says otherwise.
        int oneStream = 0;
        if (const char* e = std::getenv("MSC_VGX_ONESTREAM"); e && *e) oneStream = std::atoi(e) != 0;
        vglSetOneStream(oneStream ? GL_TRUE : GL_FALSE);
        if (vbufCached) std::fprintf(stderr, "[vbuf] MSC_VITA_VBUF_CACHED=1: stream arena %d KiB, %s, write-back %s\n", streamKib, memName, memKind == VGL_STREAM_CACHED ? mode : "-");
        std::fprintf(stderr, "[vgx-vita] vitaGL GLB: stream arena %d KiB per display buffer (MSC_VITA_VGL_STREAM_KIB; 0 = off), memory %s (MSC_VITA_VGL_STREAM_MEM), write-back %s "
                     "(MSC_VITA_VGL_FLUSH; cached only), uniform ring %s (MSC_VITA_VGL_RING_MEM), skip redundant sceGxm sets %s (MSC_VITA_VGL_SKIP), one stream per draw %s (MSC_VGX_ONESTREAM), sceGxm state cache 0x%02X%s (MSC_VGX_GXMCACHE), lazy glClear %s%s (MSC_VGX_LAZYCLEAR)\n", streamKib, memName,
                     memKind == VGL_STREAM_CACHED ? mode : "-", ringCdram ? "cdram" : "pool memory", skip ? "on" : "off", oneStream ? "on" : "off", stateIn,
                     !vglSetStateCache ? " (library without it)" : (stateMask && !stateIn) ? " (library built without GXM_STATE_CACHE)" : "", lazyClear ? "on" : "off",
                     !vglSetLazyClear ? " (library without it)" : "");
        WriteCopySetup();
        // nxgl25 (vitaGL 0062) MSC_VITA_FBO_KEEP=0|1 (default 0): detaching a framebuffer object's colour texture (glFramebufferTexture2D, texture 0) keeps the object's render target and depth surface, which a later attach of a
        // texture of the same size takes back (no sceGxmCreateRenderTarget, 46-55 ms on a picture-size step, 6122). The cost is video memory: a detached object keeps its depth surface until it is deleted or re-sized.
        if (const char* e = std::getenv("MSC_VITA_FBO_KEEP"); e && *e && std::atoi(e) != 0) {
            if (vglSetFramebufferDetachKeep) { vglSetFramebufferDetachKeep(GL_TRUE); std::fprintf(stderr, "[fbkeep] MSC_VITA_FBO_KEEP=1: a detached colour texture keeps the object's render target and depth surface (vitaGL 0062)\n"); }
            else std::fprintf(stderr, "[fbkeep] MSC_VITA_FBO_KEEP=%s ignored: this vitaGL has no patch 0062\n", e);
        }
        // gxvita14 (after Galaxy dma31, GXV_DMA_LOAD) MSC_VITA_TEXCOPY=0|1|2 (default 0 = the library's own copy): the contiguous level-0 copy of a texture upload (8 KiB and more) by the DMA engine
        // (1), or by it and compared with its source, made again by the CPU on a difference (2). Needs vitaGL patch 0051. Stage-load time is where it shows ([texcopy] line with the counts).
        if (const char* e = std::getenv("MSC_VITA_TEXCOPY"); e && *e && std::atoi(e) != 0) {
            g_texCopyMode = std::atoi(e) == 2 ? 2 : 1;
            if (vglSetTextureCopyDma) vglSetTextureCopyDma(g_texCopyMode);
            else std::fprintf(stderr, "[texcopy] MSC_VITA_TEXCOPY=%d ignored: this vitaGL has no patch 0051\n", g_texCopyMode);
        }
    }
#endif

    sceIoMkdir(kShaderCacheDir, 0777);
    sceIoMkdir(ShaderCacheFolder(), 0777);
    vglSetShaderCachePath(SeedShaderCache(ShaderCacheFolder()));
    // The game paces itself to the display rate; a second wait for vertical
    // blank inside the swap would make the two beat against each other.
    vglWaitVblankStart(GL_FALSE);
    vglSetDisplayCallback(DisplayCallback);

    VitaPlatform::BootStage("vitaGL start-up (takes over the display; first run creates the shader cache folders)");
    VitaConsole::SetDisplayOwner(false);
    // The return value is NOT "initialised": vitaGL returns GL_TRUE only when the display could
    // not take the requested size and it fell back to a smaller one (source/vgl.c: `return
    // res_fallback`), i.e. GL_FALSE is the normal result. (Until the first run in Vita3K, 5 Oct,
    // this file treated GL_FALSE as failure: every start ended here with "vglInitWithCustomSizes
    // failed", on a console as in the emulator.) vitaGL reports nothing else from its start-up;
    // what can go wrong in it shows in the lines below (pools, GL strings) and in the shader
    // compiler self-test.
    // MSC_VITA_MSAA=2|4 (LOOK, wave 5): vitaGL's multisampling is one setting for the display and every
    // framebuffer object (vgl.c msaa_mode): the colour surfaces are 2x / 4x with a hardware down-scale, the depth
    // and stencil surfaces 2x / 4x the size (gxm.c init_depth_stencil_buffer; with STORE_DEPTH_STENCIL they are
    // loaded and stored at every scene, so the traffic grows with them). The cost is the GPU's and the memory's,
    // not the CPU's, and has not been measured on a console: a first run reads the video memory lines at start.
    SceGxmMultisampleMode msaaMode = SCE_GXM_MULTISAMPLE_NONE;
    g_vitaMsaa = 0;
    if (const char* e = std::getenv("MSC_VITA_MSAA"); e && *e) {
        const int n = std::atoi(e);
        if (n == 2) { msaaMode = SCE_GXM_MULTISAMPLE_2X; g_vitaMsaa = 2; }
        else if (n == 4) { msaaMode = SCE_GXM_MULTISAMPLE_4X; g_vitaMsaa = 4; }
        else if (n != 0) std::fprintf(stderr, "[vgx-vita] MSC_VITA_MSAA=%s ignored (0, 2 or 4)\n", e);
        std::fprintf(stderr, "[vgx-vita] MSC_VITA_MSAA: %dx multisampling\n", n == 2 ? 2 : (n == 4 ? 4 : 0));
    }
    // 1.3 (MSC_VITA_MSAA_GUARD=0 turns the downgrade off): what the render targets of this set-up take of video memory, written down at start so a first console run says it. sceGxm gives a
    // multisampled surface 2x / 4x the depth and stencil samples (5 bytes a sample), colour stays one sample (down-scaled when stored). The share of the display, the main target (the fixed
    // MSC_VITA_RENDER size, else the highest dynamic-resolution rung, 768 x 538) and the pass objects (about 1.2 million pixels of them in open play: the crowd atlases, shadows, effects; an
    // ESTIMATE) with MSC_VITA_MSAA is what 1.2's "every target multisampled" costs. When the targets alone are more than the video pool less 16 MiB, even textures spilling to main memory
    // cannot make room: the multisampling steps down (4x to 2x to none) with a line saying so. Less than that is only a warning: the targets that do not fit go to main memory, slower.
    {
        int rw = 768, rh = 538;
        if (const char* e = std::getenv("MSC_VITA_RENDER"); e && *e) {
            int a = 0, b = 0;
            if (std::sscanf(e, "%dx%d", &a, &b) == 2 && a >= 160 && b >= 120 && a <= 960 && b <= 544) { rw = a; rh = b; }
        }
        const int buffers = (std::getenv("MSC_VITA_TRIPLE") && std::atoi(std::getenv("MSC_VITA_TRIPLE")) == 0) ? 2 :
                            ((std::getenv("MSC_VITA_DISPLAY_BUFFERS") && std::getenv("MSC_VITA_DISPLAY_BUFFERS")[0] == '2') ? 2 : 3);
        const double disp = static_cast<double>(kScreenW) * kScreenH, efb = static_cast<double>(rw) * rh, passes = 1.2e6;
        const auto rtMib = [&](int n, double* d, double* e, double* p) {
            *d = (disp * 4.0 * buffers + disp * 5.0 * n) / 1048576.0;
            *e = efb * (4.0 + 5.0 * n) / 1048576.0;
            *p = passes * (4.0 + 5.0 * n) / 1048576.0;
            return *d + *e + *p;
        };
        const int want = g_vitaMsaa ? g_vitaMsaa : 1;
        double d = 0, e = 0, p = 0;
        const double poolMib = static_cast<double>(cdram) / 1048576.0;
        int use = want;
        if (g_vitaMsaa && cdram > 0 && free.size_cdram <= 160u * 1024u * 1024u && !(std::getenv("MSC_VITA_MSAA_GUARD") && std::atoi(std::getenv("MSC_VITA_MSAA_GUARD")) == 0)) {
            while (use > 1 && rtMib(use, &d, &e, &p) > poolMib - 16.0) use = use == 4 ? 2 : 1;
            if (use != want) {
                std::fprintf(stderr, "[vgx-vita] MSC_VITA_MSAA=%d: render targets would take about %.0f MiB of the %.0f MiB video pool: using %s instead (MSC_VITA_MSAA_GUARD=0 keeps it)\n",
                             want, rtMib(want, &d, &e, &p), poolMib, use > 1 ? "2x" : "no multisampling");
                g_vitaMsaa = use > 1 ? use : 0;
                msaaMode = use == 2 ? SCE_GXM_MULTISAMPLE_2X : SCE_GXM_MULTISAMPLE_NONE;
                // (the back end reads MSC_VITA_MSAA itself, GlobalFboSamples and MsaaProbe: after a step down it must see what vitaGL is started with, or the per-target
                // multisampling believes in 4x that is not there, and with "none" the ladder's own multisampled rungs stay off as if the global one covered them)
                setenv("MSC_VITA_MSAA", g_vitaMsaa ? "2" : "0", 1);
            }
        }
        const double tot = rtMib(use, &d, &e, &p);
        std::fprintf(stderr, "[vgx-vita] [budget] video memory for render targets (estimate): display %dx%d x %d buffers %.1f MiB, main target %dx%d %.1f MiB, pass objects %.1f MiB = %.1f MiB of the %.1f MiB pool "
                             "(%s); textures (budget 32 MiB), shader code (%u MiB) and rings come on top and spill to main memory when the pool is full\n",
                     kScreenW, kScreenH, buffers, d, rw, rh, e, p, tot, poolMib, use > 1 ? (use == 4 ? "4x multisampled" : "2x multisampled") : "one sample", 8u);
    }
    if (const char* e = std::getenv("MSC_VITA_UPSCALE"); e && *e) {
        const int mode = vgx::backend::PictureModeFromString(e);
        if (mode < 0) std::fprintf(stderr, "[vgx-vita] MSC_VITA_UPSCALE=%s ignored (off, linear, sharp, sharp+, smooth, fast, cas, combined)\n", e);
        else g_upscaleMode = mode;
        if (mode == 3) g_sharpen = 0.35f;
    }
    if (const char* e = std::getenv("MSC_VITA_SHARPEN"); e && *e) { g_sharpen = static_cast<float>(std::atof(e)); g_sharpenSet = true; }
    if (g_sharpen < 0.0f) g_sharpen = 0.0f;
    if (g_sharpen > 1.5f) g_sharpen = 1.5f;
    // 1.3 MSC_VITA_SHARPEN_SCALE=<k> (default 0: the rule below, by the ladder's scale): the sharpening of the smooth / combined / fast pictures follows how much the picture is stretched on the screen,
    // k x sqrt(stretch - 1) (vgx_picture_math.h SharpenForUpscale), at most MSC_VITA_SHARPEN_CAP (default 0.8); a picture within 3 % of the screen's size is copied as it is.
    if (const char* e = std::getenv("MSC_VITA_SHARPEN_SCALE"); e && *e) g_sharpenScale = static_cast<float>(std::atof(e));
    if (const char* e = std::getenv("MSC_VITA_SHARPEN_CAP"); e && *e) g_sharpenCap = std::min(1.5f, std::max(0.0f, static_cast<float>(std::atof(e))));
    if (g_sharpenScale > 0.0f)
        std::fprintf(stderr, "[vgx-vita] sharpening follows the stretch of the picture: %.2f x sqrt(stretch - 1), at most %.2f, none within 3 %% of the screen's size (MSC_VITA_SHARPEN_SCALE)\n", g_sharpenScale, g_sharpenCap);
    std::fprintf(stderr, "[vgx-vita] picture to the screen: %s%s\n",
                 g_upscaleMode == 0 ? "glBlitFramebuffer (MSC_VITA_UPSCALE=off)" : (g_upscaleMode == 1 ? "linear, through the picture shader" : (g_upscaleMode == 2 ? "sharp bilinear" : (g_upscaleMode == 3 ? "sharp bilinear + unsharp mask" : (g_upscaleMode == 6 ? "bilinear + three-fetch unsharp mask (fast)" : "bilinear + unsharp mask (smooth)")))),
                 g_upscaleMode >= 3 ? (" amount " + std::to_string(g_sharpen)).c_str() : "");
    // MSC_VITA_DISPLAY_BUFFERS=2 (23 Oct, console A/B only): double instead of vitaGL's triple buffering. One frame fewer can
    // wait in the display queue, so a GPU-bound scene shows each frame about one frame time sooner (input feels quicker), at the
    // risk of a lower frame rate when frame times hover near the 33 ms budget. Unset or 3: vitaGL's default (3).
    // MSC_VITA_TRIPLE=1|0 (1.2, console A/B only): vglUseTripleBuffering(). It only sets vitaGL's display buffer count (3 / 2) and has to come before
    // vglInit* (the count sizes the sceGxm display queue, the swap chain and the slices of the circular pool). vitaGL's own default is already 3, so
    // =1 changes nothing and =0 is MSC_VITA_DISPLAY_BUFFERS=2; an explicit MSC_VITA_DISPLAY_BUFFERS below wins over it.
    if (const char* e = std::getenv("MSC_VITA_TRIPLE"); e && *e) {
        const bool on = std::atoi(e) != 0;
        vglUseTripleBuffering(on ? GL_TRUE : GL_FALSE);
        g_vglBuffers = on ? 3 : 2;
        std::fprintf(stderr, "[vgx-vita] MSC_VITA_TRIPLE=%s: vglUseTripleBuffering(%s), %d display buffers\n", e, on ? "true" : "false", on ? 3 : 2);
    }
    if (const char* e = std::getenv("MSC_VITA_DISPLAY_BUFFERS"); e && (e[0] == '2' || e[0] == '3') && e[1] == '\0') {
        vglSetDisplayBufferCount(e[0] - '0');
        g_vglBuffers = e[0] - '0';
        std::fprintf(stderr, "[vgx-vita] MSC_VITA_DISPLAY_BUFFERS: %c display buffers\n", e[0]);
    }
    // 1.3 render13 (R1, MH3's presentation: 3 display buffers, each with its own sync object, display queue limit = buffers + 1 = 4; at 30.5 -> 42 fps in a GPU-bound fight):
    // vitaGL already makes one sync object per display buffer and, with 3 buffers, lets sceGxm hold 2 pending flips (buffers - 1). MSC_VITA_DQ_PENDING=n (1..8; unset = vitaGL's
    // buffers - 1, nothing changes) sets sceGxm's displayQueueMaxPendingCount, so sceGxmDisplayQueueAddEntry blocks the render thread later (or never) when the display is behind.
    // Pacing change, off. The frames in flight stay bounded by the GPU queue limit (MSC_VITA_QUEUE, 2 by default; =off with a limit above buffers - 1 can overtake the circular
    // pool's per-buffer slices, so a limit is refused above buffers + 1 and a warning names the unbounded case). MSC_VITA_DQ_TIMING=1 times AddEntry: a line every 600 swaps
    // ("[vgx-vita] display queue: ...") and dq= in the [hitch] line.
    if (const char* e = std::getenv("MSC_VITA_DQ_PENDING"); e && *e) {
        const int n = std::atoi(e);
        if (n >= 1 && n <= 8) {
            const int b = g_vglBuffers;
            const int lim = n > b + 1 ? b + 1 : n;
            vglSetDisplayQueuePending(lim);
            std::fprintf(stderr, "[vgx-vita] MSC_VITA_DQ_PENDING=%d: display queue limit %d with %d display buffers (vitaGL's own: %d)%s\n", n, lim, b, b - 1, n != lim ? " (cut to buffers + 1)" : "");
            const char* q = std::getenv("MSC_VITA_QUEUE");
            if (q && (!std::strcmp(q, "off") || !std::strcmp(q, "0")) && lim > b - 1)
                std::fprintf(stderr, "[vgx-vita] WARNING: MSC_VITA_DQ_PENDING above buffers - 1 with MSC_VITA_QUEUE=off: nothing bounds the frames in flight\n");
        } else std::fprintf(stderr, "[vgx-vita] MSC_VITA_DQ_PENDING=%s ignored (1..8)\n", e);
    }
    if (const char* e = std::getenv("MSC_VITA_DQ_TIMING"); e && *e == '1') { vglSetDisplayQueueTiming(1); g_dqAddTiming = true; }
    // 1.3 (vram19, review item 7) MSC_VGX_DISPDEPTH=real|nomem (default real, console A/B after nodepth1): the display's depth + stencil surface (960x544, 2.49 MiB of video memory)
    // is never used by the present (NOLOAD|NOSTORE, vglRtSceneHint above) but sceCommonDialogUpdate takes its depth address (system keyboard, ad hoc NetCheck). nomem: the surface
    // points at no memory, vitaGL (patch 0042) makes a real one on the first swap with a dialog up and gives it back 90 swaps after the last dialog frame. Scenes on a surface with no
    // memory load and store nothing (0038's path); a partial render of one would have nothing to spill to, so MSC_VITA_PB_MIB below 16 is refused with it (see above).
    if (const char* e = std::getenv("MSC_VGX_DISPDEPTH"); e && (!std::strcmp(e, "nomem") || !std::strcmp(e, "1"))) {
        vglSetDisplayNoDepth(1);
        g_dispDepthNoMem = true;
        std::fprintf(stderr, "[vgx-vita] MSC_VGX_DISPDEPTH=nomem: the display depth/stencil surface has no memory except while a system dialog is up\n");
    }
    const GLboolean fellBack = vglInitWithCustomSizes(0, kScreenW, kScreenH, static_cast<int>(ram), static_cast<int>(cdram),
                                                      static_cast<int>(phycont), 0, msaaMode);
    if (fellBack)
        std::fprintf(stderr, "[vgx-vita] vitaGL could not set %dx%d and fell back to the display's largest mode; the picture's placement will be off\n",
                     kScreenW, kScreenH);
    if (vglMemTotal(VGL_MEM_VRAM) == 0 && vglMemTotal(VGL_MEM_RAM) == 0) {
        g_startCause = 2;
        std::snprintf(g_startError, sizeof(g_startError), "vitaGL started without any memory pool (see the [vitaGL] lines in log.txt).");
        return false;
    }
    if (pbLpddr) {
        int rc = 0; uint32_t shrink = 0;
        const int st = vglParamBufferLpddrState(&rc, &shrink);
        std::fprintf(stderr, "[vgx-vita] PB_LPDDR: %s (sceGxm rc 0x%08X), video memory request cut back by %u KiB, parameter buffer %d MiB; vitaGL pools now: cdram %u KiB, ram %u KiB (compare the 'pools requested' line above: "
                             "with the flag taken the request is about %d MiB larger than a run without it)\n",
                     st > 0 ? "TAKEN (parameter buffer in main memory)" : (st < 0 ? "REFUSED, fell back to video memory" : "not asked"), static_cast<unsigned>(rc), shrink >> 10, pbMib,
                     static_cast<unsigned>(vglMemTotal(VGL_MEM_VRAM) >> 10), static_cast<unsigned>(vglMemTotal(VGL_MEM_RAM) >> 10), pbMib);
    }
    LogMemory("after vitaGL");
    // MSC_VITA_ANISO=3|2 (23 Oct, experimental, off by default): trilinear textures (the pitch, the stands, most of the world) get the GPU's
    // anisotropic minification filter, which keeps surfaces seen at a slant (the far half of the pitch) sharp. vitasdk names the two values
    // MIPMAP_POINT / MIPMAP_LINEAR; they are believed to be the anisotropic modes (vitaGL's vglSetAnisotropicFilter). Unmeasured on a console:
    // look at the far half of the pitch with it on and off, and the frame rate.
    if (const char* e = std::getenv("MSC_VITA_ANISO"); e && (e[0] == '2' || e[0] == '3') && e[1] == 0) {
        vglSetAnisotropicFilter(static_cast<uint32_t>(e[0] - '0'));
        g_anisoLevel = e[0] - '0';
        std::fprintf(stderr, "[vgx-vita] MSC_VITA_ANISO=%s: trilinear textures use minification filter %s\n", e, e);
        // 1.3 MSC_VITA_ANISO_MIN=<texels> (vitaGL 0031; default 0: every trilinear texture): textures whose larger side is below this keep plain trilinear filtering (Galaxy's console runs left the small ones out).
        if (const char* m = std::getenv("MSC_VITA_ANISO_MIN"); m && *m) {
            const long v = std::atol(m);
            if (v > 0 && v <= 4096) {
                vglSetAnisotropicMinSize(static_cast<uint32_t>(v));
                std::fprintf(stderr, "[vgx-vita] MSC_VITA_ANISO_MIN=%ld: textures under %ld texels on their larger side stay trilinear\n", v, v);
            }
        }
    }
    {
        // A pool that vitaGL could not allocate leaves the budget untouched; say so instead of
        // crashing at its first use (see the note at the pool sizes above).
        SceKernelFreeMemorySizeInfo after;
        std::memset(&after, 0, sizeof(after));
        after.size = sizeof(after);
        sceKernelGetFreeMemorySize(&after);
        // (An emulator reports more video memory than any console has and may not count
        // allocations at all: no verdict there.)
        const bool realBudget = free.size_cdram > 0 && free.size_cdram <= 160u * 1024u * 1024u;
        const int64_t taken = static_cast<int64_t>(free.size_cdram) - static_cast<int64_t>(after.size_cdram);
        if (realBudget && cdram > 0 && taken < cdram / 2) {
            g_startCause = 3;
            std::snprintf(g_startError, sizeof(g_startError),
                          "vitaGL could not allocate its video memory pool (%u KiB asked, %u KiB of video memory still free).\n"
                          "Set MSC_VITA_CDRAM_RESERVE_MIB in env.txt to a larger value than %d.",
                          static_cast<unsigned>(cdram / 1024), static_cast<unsigned>(after.size_cdram / 1024),
                          static_cast<int>(cdramReserveMib));
            return false;
        }
    }
    LogPools("after start-up");
    {
        // (glGetString can return null: the console's libc crashes on a null %s where glibc prints "(null)")
        const char* vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
        const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
        std::fprintf(stderr, "[vgx-vita] GL: %s / %s / %s\n", vendor ? vendor : "?", renderer ? renderer : "?", version ? version : "?");
    }

    // Every buffer of the swap chain starts black (the side bars of the 4:3
    // picture are never drawn again), and the first swap is a sign of life.
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    // The display is cleared and swapped three times; after the FIRST of these, one clear with
    // the scissor on (MSC_VITA_PRIME=0 leaves it out). Both the scissored clear and its place
    // are there for Vita3K (v0.2.1-4120); a console needs neither and loses nothing by them.
    //
    // (1) Why a scissored clear at all. vitaGL implements the scissor with sceGxm's mask bit
    // and writes that mask at every scene start. The emulator's OpenGL renderer takes the
    // value such a mask draw writes from the last stencil function it was SENT while the mask
    // program was current (renderer/src/state_set.cpp), starts with 0
    // (renderer/include/renderer/types.h, writing_mask) and is not sent a function that
    // repeats the previous one (modules/SceGxm/SceGxm.cpp sceGxmSetFrontStencilFunc). With
    // the scissor off vitaGL's mask draw repeats "always", so until the first scissored draw
    // of a session every mask is written as 0 and every later draw is discarded: the four
    // boot screens were black (v3k/NOTES.md, session 3, V2). A scissored draw sends "never"
    // then "always", after which the emulator's value stays 1.
    //
    // (2) Why NOT as the very first draw of the session. vitaGL draws its mask quads with the
    // same vertex program as its clear quad. The emulator translates a guest shader when it
    // is first used and keeps the translation on disk under the shader's hash; when that
    // first use is a mask draw, it writes the mask-update body into the VERTEX shader as well
    // ("out_color = vec4(screen_width)", no gl_Position: build/v3k/emucache-poisoned against
    // emucache-good). From then on no clear and no mask draw of any session reaches a pixel:
    // the OpenGL renderer shows nothing at all (runs gl17, gl19, gl20a-d, gl21a, gl22a), and
    // on the Vulkan renderer nothing is ever cleared (menus with piled-up text, the match on
    // black: every Vulkan run from gl13V to gl19V). So the first draw is a plain clear.
    static const bool prime = [] { const char* e = std::getenv("MSC_VITA_PRIME"); return !(e && *e && std::atoi(e) == 0); }();
    for (int i = 0; i < 3; ++i) {
        glClear(GL_COLOR_BUFFER_BIT);
        SwapDisplay();
        if (i == 0 && prime) {
            glEnable(GL_SCISSOR_TEST);
            glScissor(0, 0, 16, 16);
            glClear(GL_COLOR_BUFFER_BIT);
            glDisable(GL_SCISSOR_TEST);
        }
    }
    std::fprintf(stderr, "[vgx-vita] display cleared and swapped three times (%s)\n", prime ? "with one scissored clear after the first" : "MSC_VITA_PRIME=0: no scissored clear");
    if (const char* e = std::getenv("MSC_VITA_MASK_GUARD"); e && std::atoi(e) != 0) {
        vgxvita_maskGuardOn = 1;
        std::fprintf(stderr, "[vgx-vita] MSC_VITA_MASK_GUARD=1: every draw and clear is preceded by a mask update under depth function NEVER "
                             "(for Vita3K's Vulkan renderer, which draws mask updates into the picture; never needed on a console)\n");
    }

    char error[400] = "";
    if (!ShaderCompilerSelfTest(error, sizeof(error))) {
        g_startCause = 4;
        std::snprintf(g_startError, sizeof(g_startError), "Run-time shader compilation does not work:\n%s", error);
        return false;
    }
    // Static geometry goes to vitaGL's MAIN-memory pool (the hint GL_DYNAMIC_DRAW; see
    // g_staticUsage in vgx_backend_gl.cpp): 18.5 MiB less video memory in a match.
    // MSC_VGX_STATIC_USAGE=static in env.txt puts it back into video memory.
    setenv("MSC_VGX_STATIC_USAGE", "dynamic", 0);
    {
        const char* usage = std::getenv("MSC_VGX_STATIC_USAGE");
        std::fprintf(stderr, "[vgx-vita] static vertex buffers: %s\n", usage && std::strcmp(usage, "dynamic") == 0 ? "main-memory pool first (GL_DYNAMIC_DRAW hint)"
                                                                                                                     : "video memory first (MSC_VGX_STATIC_USAGE)");
    }
    int renderW = 0, renderH = 0;   // 0 = the game's own frame-buffer size
    RenderSize(renderW, renderH);
    // 1.3 MSC_VITA_SHAPE=1 (default off): only the full-screen picture; in the 4:3 box the game's own shape already fits.
    if (const char* e = std::getenv("MSC_VITA_SHAPE"); e && e[0] == '1' && e[1] == 0 && g_stretch.load(std::memory_order_relaxed)) {
        vgx::backend::SetRenderShape(kScreenW, kScreenH);
        std::fprintf(stderr, "[vgx-vita] MSC_VITA_SHAPE=1: picture sizes take the screen's shape, same pixels\n");
    }
    if (!vgx::backend::Init(renderW, renderH)) {
        g_startCause = 5;
        std::snprintf(g_startError, sizeof(g_startError), "the vgx GL back end failed to initialise");
        return false;
    }
#ifndef MSC_VGX_COMMIT
#define MSC_VGX_COMMIT "unknown"
#endif
    if (g_upscaleMode > 0) vgx::backend::PrewarmPicturePrograms(g_upscaleMode);   // 1.3 MSC_PIC_PREWARM: the picture programs at start-up, not at the first step to a smaller picture
    vgx::backend::SetPresentHook(PresentDisplayCopy);
    vgx::backend::SetPresentSplit(PresentSplitBegin, PresentSplitFinish);
    vgx::backend::SetProgressDisplay(0, kScreenW, kScreenH, ShowProgressBar);
#ifdef VITAGL_RT
    StartRt();
#endif
    PinLibraryThreads(true);
    if (VitaMem::Checkpoint) VitaMem::Checkpoint("after vitaGL start-up and the back end's own allocations");
    std::fprintf(stderr, "[vgx-vita] back end ready; renderer sources: %s\n", MSC_VGX_COMMIT);
    // From here the swap can show a CommonDialog (multiplayer's NetCheck): allow the platform layer to ask for one.
    if (np_vita_radio_dialog_prepare && np_vita_radio_dialog_prepare(true) != 0)
        std::fprintf(stderr, "[vgx-vita] CommonDialog configuration failed; the multiplayer connection dialog is unavailable\n");
    return true;
}

// ---- frame capture (workstream REL, 6 Oct): pictures of a console run without the owner
// MSC_VITA_SHOT=<list> in env.txt saves the finished picture of the listed presents as
// ux0:data/mscharged/shots/shot_<present>.png, where the lab client can fetch them
// (vitalab.py ls shots ; get shots/shot_001800.png x.png). <list> = comma-separated items
// N, A-B or A-B/S (every S-th from A to B), e.g. "600,1200,1800-2400/200"; at most 64 files per
// run. The number is this file's present count (the "[vgx-vita] present N" of the log): the
// display copies the render thread has shown, which is the harness's frame number f of the
// [watch] lines give or take the frames in flight. MSC_VITA_SHOT_SIZE=WxH (default 480x272,
// up to the picture's own size) and MSC_VITA_SHOT_FLIP=0|1 (row order, see below).
// What is saved is the EFB picture the display copy is made from (the game's 640x448 unless
// MSC_VITA_RENDER says otherwise), averaged down: what the GPU drew, before the blit to the
// screen. A captured present costs a GPU flush, a 1.1 MiB read-back and a file of about
// 390 KiB (the PNG is stored, not compressed: no library needed): tens of milliseconds, so
// the frame times around a captured present are not measurements.
// Runs on the render thread, inside the present hook, with the EFB bound.
struct ShotItem { uint32_t first, last, step; };
ShotItem g_shotItems[16];
int g_shotCount = -1;            // -1: env.txt not read yet
int g_shotW = 480, g_shotH = 272;
bool g_shotFlip = true;
unsigned g_shotsWritten = 0;
constexpr unsigned kShotMaxFiles = 64;
void ShotInit() {
    g_shotCount = 0;
    const char* e = std::getenv("MSC_VITA_SHOT");
    if (!e || !*e) return;
    const char* p = e;
    while (*p && g_shotCount < 16) {
        char* end = nullptr;
        const unsigned long a = std::strtoul(p, &end, 10);
        if (end == p) break;
        unsigned long b = a, st = 1;
        if (*end == '-') { const char* q = end + 1; b = std::strtoul(q, &end, 10); if (end == q) break; }
        if (*end == '/') { const char* q = end + 1; st = std::strtoul(q, &end, 10); if (end == q) break; }
        if (b >= a && st >= 1) g_shotItems[g_shotCount++] = ShotItem{static_cast<uint32_t>(a), static_cast<uint32_t>(b), static_cast<uint32_t>(st)};
        if (*end != ',') break;
        p = end + 1;
    }
    if (const char* z = std::getenv("MSC_VITA_SHOT_SIZE")) {
        int w = 0, h = 0;
        if (std::sscanf(z, "%dx%d", &w, &h) == 2 && w >= 64 && h >= 48 && w <= 960 && h <= 544) { g_shotW = w; g_shotH = h; }
    }
    if (const char* f = std::getenv("MSC_VITA_SHOT_FLIP")) g_shotFlip = std::atoi(f) != 0;
    sceIoMkdir("ux0:data/mscharged/shots", 0777);
    std::fprintf(stderr, "[vgx-vita] MSC_VITA_SHOT=%s: %d item(s) understood; pictures of those presents go to ux0:data/mscharged/shots/ "
                         "(at most %dx%d, %u files; row order %s)\n", e, g_shotCount, g_shotW, g_shotH, kShotMaxFiles, g_shotFlip ? "reversed" : "as read");
}
std::vector<uint8_t> g_shotPx, g_shotRgb, g_shotPng;
void PngStored(std::vector<uint8_t>& o, const uint8_t* rgb, int w, int h);
bool ShotWanted(uint64_t present) {
    if (g_shotCount < 0) {
        ShotInit();
        if (g_shotCount > 0) {   // (a large shot in a match does not find 5 MiB of C heap: taken at the first present, while there is room)
            const size_t n = static_cast<size_t>(g_shotW) * g_shotH;
            g_shotPx.reserve((n > kScreenW * kScreenH ? n : static_cast<size_t>(kScreenW) * kScreenH) * 4);
            g_shotRgb.reserve(n * 3);
            g_shotPng.reserve(n * 3 + n / 8 + 4096);
            g_shotRgb.assign(n * 3, 0);
            PngStored(g_shotPng, g_shotRgb.data(), g_shotW, g_shotH);
        }
    }
    if (g_shotCount == 0 || g_shotsWritten >= kShotMaxFiles) return false;
    for (int i = 0; i < g_shotCount; ++i) {
        const ShotItem& it = g_shotItems[i];
        if (present >= it.first && present <= it.last && (it.step <= 0 || (present - it.first) % it.step == 0)) return true;
    }
    return false;
}
// A PNG with stored (uncompressed) deflate blocks: signature, IHDR, one IDAT, IEND.
void PngPut32(std::vector<uint8_t>& o, uint32_t v) { o.push_back(v >> 24); o.push_back(v >> 16); o.push_back(v >> 8); o.push_back(v); }
uint32_t PngCrc(const uint8_t* d, size_t n) {
    static uint32_t table[256];
    static bool made = false;
    if (!made) {
        for (uint32_t i = 0; i < 256; ++i) { uint32_t c = i; for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1; table[i] = c; }
        made = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ d[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
void PngChunk(std::vector<uint8_t>& o, const char* type, const uint8_t* data, size_t n) {
    PngPut32(o, static_cast<uint32_t>(n));
    const size_t at = o.size();
    o.insert(o.end(), type, type + 4);
    if (n) o.insert(o.end(), data, data + n);
    PngPut32(o, PngCrc(o.data() + at, n + 4));
}
// rgb: w*h*3 bytes, top row first.
void PngStored(std::vector<uint8_t>& o, const uint8_t* rgb, int w, int h) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    o.assign(sig, sig + 8);
    uint8_t ihdr[13] = {0, 0, 0, 0, 0, 0, 0, 0, 8, 2, 0, 0, 0};   // 8 bits, colour type 2 (RGB)
    ihdr[0] = w >> 24; ihdr[1] = w >> 16; ihdr[2] = w >> 8; ihdr[3] = w;
    ihdr[4] = h >> 24; ihdr[5] = h >> 16; ihdr[6] = h >> 8; ihdr[7] = h;
    PngChunk(o, "IHDR", ihdr, 13);
    // The raw stream: per row a filter byte 0 and the row.
    static std::vector<uint8_t> raw;   // (kept from shot to shot: ShotWanted makes them at the first present)
    raw.clear();
    raw.reserve(static_cast<size_t>(h) * (w * 3 + 1));
    for (int y = 0; y < h; ++y) { raw.push_back(0); raw.insert(raw.end(), rgb + static_cast<size_t>(y) * w * 3, rgb + static_cast<size_t>(y + 1) * w * 3); }
    static std::vector<uint8_t> z;
    z.clear();
    z.reserve(raw.size() + raw.size() / 65535u * 5u + 16u);
    z.push_back(0x78); z.push_back(0x01);
    uint32_t s1 = 1, s2 = 0;
    for (size_t at = 0; at < raw.size();) {
        const size_t n = raw.size() - at < 65535u ? raw.size() - at : 65535u;
        z.push_back(at + n == raw.size() ? 1 : 0);
        z.push_back(n & 0xFF); z.push_back(n >> 8); z.push_back(~n & 0xFF); z.push_back((~n >> 8) & 0xFF);
        z.insert(z.end(), raw.begin() + at, raw.begin() + at + n);
        for (size_t i = 0; i < n; ++i) { s1 += raw[at + i]; if (s1 >= 65521u) s1 -= 65521u; s2 += s1; if (s2 >= 65521u) s2 -= 65521u; }
        at += n;
    }
    PngPut32(z, (s2 << 16) | s1);
    PngChunk(o, "IDAT", z.data(), z.size());
    PngChunk(o, "IEND", nullptr, 0);
}
void TakeShot(uint64_t present, int x, int y, int w, int h) {
    const uint64_t t0 = NowUs();
    std::vector<uint8_t>& px = g_shotPx; std::vector<uint8_t>& rgb = g_shotRgb; std::vector<uint8_t>& png = g_shotPng;
    px.resize(static_cast<size_t>(w) * h * 4);
    // (vitaGL copies whole rows of 4-byte pixels whatever the pack alignment is; with the EFB
    // bound it first ends the scene and waits for the GPU: source/framebuffers.c.)
    glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    const int ow = g_shotW < w ? g_shotW : w, oh = g_shotH < h ? g_shotH : h;
    rgb.resize(static_cast<size_t>(ow) * oh * 3);
    // Row order of the read-back: desktop GL returns the rectangle's lowest row first, which for
    // the EFB (stored top row first) is the picture's top row. vitaGL copies a framebuffer
    // object's rows in the opposite order (source/framebuffers.c glReadPixels: the first row
    // returned is the rectangle's highest), hence "reversed" by default; MSC_VITA_SHOT_FLIP=0
    // if a picture ever comes out upside down.
    for (int oy = 0; oy < oh; ++oy) {
        const int sy0 = oy * h / oh, sy1 = (oy + 1) * h / oh > sy0 ? (oy + 1) * h / oh : sy0 + 1;
        for (int ox = 0; ox < ow; ++ox) {
            const int sx0 = ox * w / ow, sx1 = (ox + 1) * w / ow > sx0 ? (ox + 1) * w / ow : sx0 + 1;
            unsigned r = 0, g = 0, b = 0, n = 0;
            for (int sy = sy0; sy < sy1; ++sy) {
                const int row = g_shotFlip ? h - 1 - sy : sy;
                const uint8_t* src = px.data() + (static_cast<size_t>(row) * w + sx0) * 4;
                for (int sx = sx0; sx < sx1; ++sx, src += 4) { r += src[0]; g += src[1]; b += src[2]; ++n; }
            }
            uint8_t* dst = rgb.data() + (static_cast<size_t>(oy) * ow + ox) * 3;
            dst[0] = static_cast<uint8_t>(r / n); dst[1] = static_cast<uint8_t>(g / n); dst[2] = static_cast<uint8_t>(b / n);
        }
    }
    PngStored(png, rgb.data(), ow, oh);
    char path[96];
    std::snprintf(path, sizeof(path), "ux0:data/mscharged/shots/shot_%06llu.png", static_cast<unsigned long long>(present));
    sceIoRemove(path);
    const SceUID f = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    int wrote = -1;
    if (f >= 0) {
        wrote = sceIoWrite(f, png.data(), static_cast<SceSize>(png.size()));
        sceIoClose(f);
    }
    ++g_shotsWritten;
    std::fprintf(stderr, "[vgx-vita] shot: present %llu (harness frame about %llu) %dx%d of the %dx%d picture -> %s, %d of %u bytes written, %u ms\n",
                 static_cast<unsigned long long>(present), static_cast<unsigned long long>(PlatFrameStats::FrameCount()), ow, oh, w, h, path, wrote,
                 static_cast<unsigned>(png.size()), static_cast<unsigned>((NowUs() - t0) / 1000u));
}

// The back end's present hook: a display copy, on the render thread, with the
// EFB framebuffer object `efb` bound and the finished picture in it. (x, y, w,
// h) is the copy rectangle in framebuffer pixels; the EFB is stored top row
// first (its projection flips Y), so the picture's top row is the
// framebuffer's y. The back end binds the EFB again and re-applies its render
// state after this returns.
// ---- W5 RT: depth load/store per scene, scene and GPU timers (vitaGL patch platform/vita/vitagl/0003-rt-*.patch, source/rt.c) ----
//   MSC_VGX_RT=1             the back end's per-scene hints (vgx_backend_gl.cpp "W5 RT") reach vitaGL, and vitaGL skips the depth load of a scene that
//                            begins with a whole depth clear; the display's scene (the present) neither loads nor stores depth
//   MSC_VITA_RT_AUTO=0|1     vitaGL's own whole-clear rule (default: as MSC_VGX_RT)
//   MSC_VITA_RT_TIMERS=0..3  1: CPU time of the scene changes in vitaGL; 2: GPU completion of every scene (a fragment notification each, watcher
//                            thread on MSC_VITA_RT_CORE, default core 2); 3: both (lab builds: VitaPlan sets 3)
//   MSC_VGX_RTTIME=1         the back end times the draws that begin a scene and every 32nd other draw (lab builds: VitaPlan sets 1)
//   MSC_VITA_RT_LOG=N        an [rt] line every N presents (default 300 when anything above is on; 0: none)
#ifdef VITAGL_RT
namespace {
bool g_rtDisplayHint = false;
int g_rtLogEvery = 0;
vglRtStats g_rtNow, g_rtPrev;
extern "C" void vglGetMaskKeepStats(uint32_t* drawn, uint32_t* kept) __attribute__((weak)); // (vitaGL patch 0026; weak: an older vitaGL has none)
extern "C" void vglGetMaskBackgroundStats(uint32_t* armed, uint32_t* kept) __attribute__((weak)); // (vitaGL patch 0029)
extern "C" void vglGetMaskClipStats(uint32_t* narrowed, uint32_t* kpxSaved) __attribute__((weak)); // (vitaGL patch 0055)
extern "C" void vglSceneCensusSetup(int on) __attribute__((weak));                    // (vitaGL patch 0033; weak: an older vitaGL has none)
extern "C" void vglGetSceneCensus(vglSceneCensus* out, int reset) __attribute__((weak));
extern "C" void vglGetDualRTStats(uint32_t* made, uint32_t* kib, uint32_t* oddScenes, uint32_t* failed) __attribute__((weak));   // vitaGL 0063
extern "C" void np_env_lock_stats(unsigned* calls, unsigned* waits, unsigned* timeouts) __attribute__((weak));   // platform/vita/src/np_vita_envlock.cpp (the lock under getenv / setenv)
extern "C" void vglSetFreeGuard(int presents) __attribute__((weak));   // (vitaGL patch 0034; weak: an older vitaGL has none)
extern "C" void vglSetFreeGuardReferencedOnly(int on) __attribute__((weak));   // (vitaGL patch 0047, MSC_VGX_FREE_GUARD_REF)
extern "C" void vglGetFreeGuardStats(vglFreeGuardStats* out) __attribute__((weak));
extern "C" int vglFreeGuardTake(vglFreeGuardEvent* out) __attribute__((weak));
int g_freeGuard = 0;   // MSC_VGX_FREE_GUARD=N (lab, default 0): every texture / framebuffer delete waits N presents in vitaGL; deletes of a block a scene drew into are reported
int g_sceneCountEvery = 0;   // MSC_VITA_SCENE_COUNT=N (lab / logging, default 0): a [scenes] line every N presents (1 means 300)
extern "C" void vglGetMemFreeStats(uint32_t* fast, uint32_t* walked, uint32_t* checks, uint32_t* mismatches, uint32_t* raced, int32_t* worst, uint32_t* pools) __attribute__((weak)); // (vitaGL patch 0028)
uint32_t g_mkPrev[2];
uint64_t g_rtPrevDraw[4] = {};
bool g_rtHavePrev = false;
bool g_rtScenes = false;   // gpuscene20: MSC_VITA_RT_SCENES
constexpr uint32_t RT_KIND_SLOTS_N = 17;   // vitaGL 0043 rt_kinds.h: 16 kinds and "other"
long EnvL(const char* name, long dflt) {
    const char* e = std::getenv(name);
    return (e && *e) ? std::atol(e) : dflt;
}
template <typename T> void Pct(std::vector<T>& v, T& p50, T& p95) {
    if (v.empty()) { p50 = p95 = 0; return; }
    std::sort(v.begin(), v.end());
    p50 = v[v.size() / 2];
    p95 = v[(v.size() * 95) / 100 < v.size() ? (v.size() * 95) / 100 : v.size() - 1];
}
} // namespace
void StartRt() {
    // 1.2 (the failure handling of the patched vitaGL; all of it only changes what happens after something has already failed):
    //   MSC_VITA_ROBUST=0     scene entry and shader patching as in 1.1 (a failed depth/target allocation, begin or program is not skipped)
    //   MSC_VITA_RT_RING_GUARD=0   the GPU timer ring is overwritten when the watcher is 256 scenes behind, as in 1.1
    if (EnvL("MSC_VITA_ROBUST", 1) == 0) vglSetRobustScenes(0);
    if (EnvL("MSC_VITA_RT_RING_GUARD", 1) == 0) vglRtSetRingGuard(0);
    const long hints = EnvL("MSC_VGX_RT", 0);
    const long autoNoLoad = EnvL("MSC_VITA_RT_AUTO", hints);
    const long timers = EnvL("MSC_VITA_RT_TIMERS", 0);
    uint32_t flags = 0;
    if (hints > 0) flags |= VGL_RT_HINTS;
    if (autoNoLoad > 0) flags |= VGL_RT_AUTO_NOLOAD;
    if (timers & 1) flags |= VGL_RT_CPU_TIMERS;
    if (timers & 2) flags |= VGL_RT_GPU_TIMERS;
    const int core = static_cast<int>(EnvL("MSC_VITA_RT_CORE", 2));
    const int affinity = (core >= 0 && core <= 2) ? (SCE_KERNEL_CPU_MASK_USER_0 << core) : 0;
    // MSC_VITA_RT_SPLIT=1 (1.3, diagnostic, default 0): a vertex notification per scene as well, and a `[gpu split]` line: the GPU time after tiling ended (what the picture size acts on) and the rest.
    if (EnvL("MSC_VITA_RT_SPLIT", 0) > 0) vglRtSetSplit(1);
    if (EnvL("MSC_VITA_RT_SCENES", 0) > 0) { vglRtSetSplit(1); flags |= VGL_RT_GPU_TIMERS; g_rtScenes = true; }   // gpuscene20 (vitaGL 0043): GPU time per kind of scene, `[gpu scenes]`; implies the split and the GPU timers
    vglRtSetup(flags, static_cast<int>(EnvL("MSC_VITA_RT_PRIO", 0x10000100 - 16)), affinity);
    g_rtDisplayHint = hints > 0;
    if (hints > 0) vgx::backend::SetSceneHintHook([](unsigned fb, unsigned h) { vglRtSceneHint(fb, h); });
    vgx::backend::SetMicroClock([]() -> uint32_t { return sceKernelGetProcessTimeLow(); });
    // MSC_VGX_FREE_GUARD=N (default 0 = off, exactly as before): the Galaxy port's guard against freeing video memory a queued scene still writes (our r9 boot crash: glDeleteTextures
    // freed the colour block of a target drawn into at once). With N (3 is the Galaxy value) vitaGL holds every glDeleteTextures / glDeleteFramebuffers N presents and reports, once per call
    // site, a delete of a texture that a scene drew into within the last 4 presents or that is the colour of the bound framebuffer: "[free-guard] FREE WHILE REFERENCED".
    // MSC_VGX_FREE_GUARD_REF=N (stab13, vitaGL 0047; the Vita plan sets 3): the same guard, but only a delete of a block a scene drew into within the last four presents (or the colour of the bound
    // framebuffer) is held back N presents; every other delete is immediate, as with no guard. Galaxy 2.3's crash (graphics memory handed back while the GPU still drew into it) is that one case.
    // MSC_VGX_FREE_GUARD=N (every delete held) wins when both are set. =0 for both: no guard.
    const long guardAll = EnvL("MSC_VGX_FREE_GUARD", 0);
    const long guardRef = guardAll ? 0 : EnvL("MSC_VGX_FREE_GUARD_REF", 0);   // (getenv through EnvL)
    if (const long n = guardAll ? guardAll : guardRef) {
        if (vglSetFreeGuard && vglFreeGuardTake && vglGetFreeGuardStats && (guardAll || vglSetFreeGuardReferencedOnly)) {
            vglSetFreeGuard(static_cast<int>(n));
            if (guardRef) vglSetFreeGuardReferencedOnly(1);
            g_freeGuard = static_cast<int>(n);
            std::fprintf(stderr, "[free-guard] on: %s wait %d presents (%s); a delete of a block a scene drew into recently is reported once per site\n",
                         guardRef ? "only the deletes of a texture / framebuffer a scene drew into lately" : "texture and framebuffer deletes", g_freeGuard, guardRef ? "MSC_VGX_FREE_GUARD_REF" : "MSC_VGX_FREE_GUARD");
        } else {
            std::fprintf(stderr, "[free-guard] MSC_VGX_FREE_GUARD is set but this vitaGL has no free guard (patch 0034)\n");
        }
    }
    // MSC_VITA_SCENE_COUNT=1|N (default 0, logging only): scenes begun per render target in one picture against the scenesPerFrame the target was made with; a [scenes] line every
    // 300 (or N) presents with the worst count of each kind of target and its capacity, flagged when a picture went past it (a scene past the capacity waits for the GPU).
    if (const long n = EnvL("MSC_VITA_SCENE_COUNT", 0)) {
        if (vglSceneCensusSetup && vglGetSceneCensus) {
            vglSceneCensusSetup(1);
            g_sceneCountEvery = static_cast<int>(n == 1 ? 300 : n);
        } else {
            std::fprintf(stderr, "[scenes] MSC_VITA_SCENE_COUNT is set but this vitaGL has no scene census (patch 0033)\n");
        }
    }
    g_rtLogEvery = static_cast<int>(EnvL("MSC_VITA_RT_LOG", (flags || EnvL("MSC_VGX_RTTIME", 0) > 0) ? 300 : 0));
    vglRtGetStats(&g_rtNow);
    std::fprintf(stderr, "[rt] depth load/store per scene: hints %s, whole-clear rule %s; timers: CPU %s, GPU %s (core %d), draws %s; clock read %.2f us; [rt] line every %d presents "
                         "(MSC_VGX_RT, MSC_VITA_RT_AUTO, MSC_VITA_RT_TIMERS, MSC_VGX_RTTIME, MSC_VITA_RT_LOG)\n",
                 hints > 0 ? "on" : "off", autoNoLoad > 0 ? "on" : "off", (g_rtNow.flags & VGL_RT_CPU_TIMERS) ? "on" : "off", (g_rtNow.flags & VGL_RT_GPU_TIMERS) ? "on" : "off", core,
                 EnvL("MSC_VGX_RTTIME", 0) > 0 ? "on" : "off", g_rtNow.clock_cost_us, g_rtLogEvery);
    // DYNRES: the renderer's dynamic resolution reads the GPU time of every frame (the player build turns the GPU timers on for it: vita_plan.cpp)
    if (g_rtNow.flags & VGL_RT_GPU_TIMERS) {
        vgx::backend::SetGpuTimeSource([](uint32_t* seq, float* ms) -> bool {
            vglRtGpuFrame f;
            const uint32_t n = vglRtLastGpuFrame(&f);
            if (!n || !f.timed) return false;
            *seq = n;
            const uint32_t busy = GpuBusyUs(f);
            *ms = static_cast<float>(busy) * 0.001f;
            {   // heldgpu24: the span (first submission to last completion) with the same display-wait correction as the busy time
                const uint32_t hold = f.busy_us > busy ? f.busy_us - busy : 0;
                vgx::backend::NoteGpuSpanMs(static_cast<float>(f.span_us > hold ? f.span_us - hold : f.span_us) * 0.001f);
            }
            return true;
        });
    }
}
// "[env]" every g_rtLogEvery presents: how many times libc's environment lock was taken since the last line (every getenv, setenv and unsetenv takes it once), per picture: a
// getenv on a hot path shows here as a count per picture far above the handful of lazy first reads. Waits and timeouts are other threads that met the lock held.
void LogEnvLock() {
    static unsigned prevCalls = 0, prevWaits = 0, prevTo = 0;
    static unsigned long long prevPresent = 0;
    if (!np_env_lock_stats || g_rtLogEvery <= 0 || g_presented % static_cast<unsigned long long>(g_rtLogEvery) != 0) return;
    unsigned c = 0, w = 0, t = 0;
    np_env_lock_stats(&c, &w, &t);
    const unsigned long long pics = g_presented - prevPresent;
    if (prevPresent && pics)
        std::fprintf(stderr, "[env] %llu pictures: %u environment lock takes (%.2f per picture), %u waits for another thread, %u timeouts; %u takes in all\n", pics, c - prevCalls,
                     static_cast<double>(c - prevCalls) / static_cast<double>(pics), w - prevWaits, t - prevTo, c);
    prevCalls = c; prevWaits = w; prevTo = t; prevPresent = g_presented;
}
// Exit quiesce, the GPU half (vgx_play.cpp VgxQuiesceForExit): wait, at most `ms`, until the scenes vitaGL has submitted are finished (its scene counters; needs the GPU timers the
// player turns on, MSC_VITA_RT_TIMERS=2, else there is nothing to wait for). Called from the exiting thread; reads two counters, calls no GL.
extern "C" void VgxHostQuiesceGpu(unsigned ms) {
    uint32_t submitted = 0, done = 0;
    vglRtSceneProgress(&submitted, &done);
    const uint64_t t0 = NowUs();
    while (submitted && static_cast<int32_t>(done - submitted) < 0 && NowUs() - t0 < static_cast<uint64_t>(ms) * 1000u) {
        sceKernelDelayThread(500);
        vglRtSceneProgress(&submitted, &done);
    }
}
void LogFreeGuard() {
    if (!g_freeGuard) return;
    vglFreeGuardEvent e;
    while (vglFreeGuardTake(&e))
        std::fprintf(stderr, "[free-guard] FREE WHILE REFERENCED %s %u (%s%s) by 0x%08x at present %llu (arm-vita-eabi-addr2line -e <image>.elf 0x%08x)\n", e.kind ? "framebuffer" : "texture", e.id,
                     (e.reason & 1) ? "a scene drew into its colour block" : "", (e.reason & 2) ? ((e.reason & 1) ? " and it is the bound framebuffer's colour" : "colour of the bound framebuffer") : "",
                     e.caller, static_cast<unsigned long long>(g_presented), e.caller);
    if (g_presented % 300 == 0) {
        vglFreeGuardStats st;
        vglGetFreeGuardStats(&st);
        std::fprintf(stderr, "[free-guard] %u deletes held back %d presents, %u of them referenced (a scene drew into the block within 4 presents, or the bound framebuffer's colour), %u call sites reported, %u not held (ring full); "
                     "most collector-list elements in one picture %u of 16384, render targets %u of 128%s\n",
                     st.deletes, g_freeGuard, st.referenced, st.sites, st.ring_full, st.purge_peak, st.rt_purge_peak,
                     (st.purge_peak >= 16384 || st.rt_purge_peak >= 128) ? "  ** COLLECTOR LIST OVERFLOW: the extra entries land in the next picture's list **" : "");
    }
}
void LogSceneCensus() {
    if (g_sceneCountEvery <= 0 || g_presented % static_cast<unsigned long long>(g_sceneCountEvery) != 0) return;
    vglSceneCensus c;
    vglGetSceneCensus(&c, 1);
    if (!c.frames) return;
    if (vglGetDualRTStats) {   // nxr4b (MSC_VITA_FBO_DUALRT): second targets, their driver memory (sceGxmGetRenderTargetMemSize), scenes begun on them
        uint32_t made = 0, kib = 0, odd = 0, failed = 0;
        vglGetDualRTStats(&made, &kib, &odd, &failed);
        if (made || odd || failed) std::fprintf(stderr, "[dualrt] second render targets made %u, driver memory %u KiB, scenes begun on them %u, makes that failed %u (MSC_VITA_FBO_DUALRT)\n", made, kib, odd, failed);
    }
    std::fprintf(stderr, "[scenes] %u pictures: worst scenes on the display target %u of %u, worst on one framebuffer target %u of %u (most targets in a picture %u); "
                         "pictures past capacity: display %u, framebuffer targets %u%s%s\n",
                 c.frames, c.worst_display, c.cap_display, c.worst_fbo, c.cap_fbo, c.worst_fbo_targets, c.over_display, c.over_fbo,
                 (c.over_display || c.over_fbo) ? "  ** OVER CAPACITY: the extra scenes wait for the GPU **" : "", c.slots_full ? "  (more than 32 targets in a picture: not all counted)" : "");
}
// gpuscene20: the window's GPU time per kind of scene (vitaGL 0043's cumulative table, minus the copy taken at the last window). The first window only takes the copy.
static void LogGpuScenes(uint32_t frames) {
    static gs20::Kind prev[RT_KIND_SLOTS_N];
    static bool have = false;
    vglRtSceneKind raw[RT_KIND_SLOTS_N];
    const uint32_t n = vglRtSceneKinds(raw, RT_KIND_SLOTS_N);
    if (!n) return;
    gs20::Kind now[RT_KIND_SLOTS_N];
    for (uint32_t i = 0; i < n; ++i) { now[i].busy = raw[i].busy_us; now[i].tiling = raw[i].tiling_us; now[i].pixel = raw[i].pixel_us; now[i].scenes = raw[i].scenes; now[i].w = raw[i].w; now[i].h = raw[i].h; now[i].display = raw[i].display; now[i].used = raw[i].used; }
    if (have) std::fputs(gs20::FormatScenes(prev, now, static_cast<int>(n), frames).c_str(), stderr);
    for (uint32_t i = 0; i < n; ++i) prev[i] = now[i];
    have = true;
}
void LogRt() {
    if (g_rtLogEvery <= 0 || g_presented % static_cast<unsigned long long>(g_rtLogEvery) != 0) return;
    vglRtGetStats(&g_rtNow);
    uint64_t dt[4];
    vgx::backend::GetDrawTimers(dt);
    if (!g_rtHavePrev) {
        g_rtPrev = g_rtNow;
        for (int i = 0; i < 4; ++i) g_rtPrevDraw[i] = dt[i];
        if (vglGetMaskKeepStats) vglGetMaskKeepStats(&g_mkPrev[0], &g_mkPrev[1]);
        g_rtHavePrev = true;
        return;
    }
    const vglRtStats& a = g_rtPrev;
    const vglRtStats& b = g_rtNow;
    const uint32_t frames = b.cpu_frame_head - a.cpu_frame_head;
    if (!frames) return;
    const double f = static_cast<double>(frames);
    // the last frames of the window (the rings keep 128)
    std::vector<uint32_t> sw, busy, span, tail;
    uint64_t splitPixel = 0, splitTiling = 0, splitBusy = 0, splitN = 0;   // (MSC_VITA_RT_SPLIT)
    for (uint32_t i = (frames > 128 ? b.cpu_frame_head - 128 : a.cpu_frame_head); i != b.cpu_frame_head; ++i) sw.push_back(b.cpu_frames[i % 128].switch_us);
    const uint32_t gf = b.gpu_frame_head - a.gpu_frame_head;
    for (uint32_t i = (gf > 128 ? b.gpu_frame_head - 128 : a.gpu_frame_head); i != b.gpu_frame_head; ++i) {
        const vglRtGpuFrame& g = b.gpu_frames[i % 128];
        if (!g.timed) continue;
        busy.push_back(g.busy_us);
        splitPixel += g.pixel_us; splitTiling += g.tiling_us; splitBusy += g.busy_us; ++splitN;
        span.push_back(g.span_us);
        tail.push_back(g.tail_us);
    }
    uint32_t sw50, sw95, b50, b95, s50, s95, t50, t95;
    Pct(sw, sw50, sw95);
    Pct(busy, b50, b95);
    Pct(span, s50, s95);
    Pct(tail, t50, t95);
    const double mb = 1.0 / (1024.0 * 1024.0);
    if (splitN && splitPixel)
        std::fprintf(stderr, "[gpu split] %llu frames: busy %.2f ms a frame, of that after tiling (pixels) %.2f, the rest %.2f; tiling seen after submission %.2f (lower bound)\n", static_cast<unsigned long long>(splitN), splitBusy * 0.001 / splitN, splitPixel * 0.001 / splitN,
                     (splitBusy - splitPixel) * 0.001 / splitN, splitTiling * 0.001 / splitN);
    if (g_rtScenes) LogGpuScenes(frames);   // gpuscene20 (MSC_VITA_RT_SCENES)
    const uint64_t firstN = dt[0] - g_rtPrevDraw[0], firstUs = dt[1] - g_rtPrevDraw[1], sampN = dt[2] - g_rtPrevDraw[2], sampUs = dt[3] - g_rtPrevDraw[3];
    // vitaGL 0065: the sceGxmBeginScene calls alone (the "begin" above also holds the depth and render target creation before them); only with the CPU timers (MSC_VITA_RT_TIMERS), else nothing is printed
    char beginSplit[128];
    beginSplit[0] = 0;
    if (b.flags & VGL_RT_CPU_TIMERS)
        std::snprintf(beginSplit, sizeof beginSplit, " | begin_disp %.2f (max %.1f) begin_fbo %.2f (max %.1f)", (b.begin_disp_us - a.begin_disp_us) / 1000.0 / f, b.begin_disp_max_us / 1000.0,
                      (b.begin_fbo_us - a.begin_fbo_us) / 1000.0 / f, b.begin_fbo_max_us / 1000.0);
    std::fprintf(stderr,
                 "[rt] %u frames: scenes %.1f/frame (display %.1f), depth not loaded %.1f (by the whole-clear rule %.1f), not stored %.1f, hints %.1f given %.1f used; "
                 "depth MB/frame loaded %.2f stored %.2f, not loaded %.2f, not stored %.2f, mask-clear quads saved %.1f | CPU scene change %.3f ms/frame (end %.3f begin %.3f set-up %.3f; frame p50 %.3f p95 %.3f, max scene %.3f ms) | "
                 "draw that begins a scene %.1f us (x%.1f/frame), other draws %.1f us | GPU (%lu of the last frames timed) busy p50 %.2f p95 %.2f ms, first submit to last done p50 %.2f p95 %.2f ms, "
                 "last submit to done p50 %.2f p95 %.2f ms; scenes timed %u untimed %u already done when looked at %u, pending %u%s\n",
                 frames, (b.scenes - a.scenes) / f, (b.scenes_display - a.scenes_display) / f, (b.scenes_noload - a.scenes_noload) / f,
                 (b.scenes_noload_auto - a.scenes_noload_auto) / f, (b.scenes_nostore - a.scenes_nostore) / f, (b.hints_given - a.hints_given) / f,
                 (b.hints_used - a.hints_used) / f, (b.ds_loaded - a.ds_loaded) * mb / f, (b.ds_stored - a.ds_stored) * mb / f,
                 (b.ds_load_skipped - a.ds_load_skipped) * mb / f, (b.ds_store_skipped - a.ds_store_skipped) * mb / f,
                 (b.mask_invalidations_skipped - a.mask_invalidations_skipped) / f,
                 ((b.cpu_end_us - a.cpu_end_us) + (b.cpu_begin_us - a.cpu_begin_us) + (b.cpu_setup_us - a.cpu_setup_us)) / 1000.0 / f,
                 (b.cpu_end_us - a.cpu_end_us) / 1000.0 / f, (b.cpu_begin_us - a.cpu_begin_us) / 1000.0 / f, (b.cpu_setup_us - a.cpu_setup_us) / 1000.0 / f, sw50 / 1000.0,
                 sw95 / 1000.0, b.cpu_switch_max_us / 1000.0, firstN ? static_cast<double>(firstUs) / static_cast<double>(firstN) : 0.0, firstN / f,
                 sampN ? static_cast<double>(sampUs) / static_cast<double>(sampN) : 0.0, static_cast<unsigned long>(busy.size()), b50 / 1000.0, b95 / 1000.0, s50 / 1000.0, s95 / 1000.0, t50 / 1000.0,
                 t95 / 1000.0, b.gpu_scenes_timed - a.gpu_scenes_timed, b.gpu_scenes_untimed - a.gpu_scenes_untimed, b.gpu_scenes_late - a.gpu_scenes_late, b.gpu_pending, beginSplit);
    {   // 1.3 MSC_VGX_VGL_MASKKEEP (vitaGL patch 0026): scissor-mask rebuilds per frame, drawn (two full-target quads each, one when the scene began unloaded) and kept
        uint32_t drawn = 0, kept = 0;
        uint32_t& prevDrawn = g_mkPrev[0];
        uint32_t& prevKept = g_mkPrev[1];
        if (vglGetMaskKeepStats) {
            vglGetMaskKeepStats(&drawn, &kept);
            std::fprintf(stderr, "[rt] %u frames: scissor mask rebuilds %.2f/frame drawn, %.2f/frame kept (not drawn: the surface held that mask; MSC_VGX_VGL_MASKKEEP)\n", frames,
                         (drawn - prevDrawn) / f, (kept - prevKept) / f);
            prevDrawn = drawn;
            prevKept = kept;
        }
    }
    if (vglGetMaskBackgroundStats) {   // 1.3 MSC_VGX_VGL_MASKBG (vitaGL patch 0029): scenes begun with the background mask set (= full-target quads left out, one each), cumulative
        uint32_t armed = 0, kept = 0;
        vglGetMaskBackgroundStats(&armed, &kept);
        std::fprintf(stderr, "[rt] scissor mask background (MSC_VGX_VGL_MASKBG): %u scenes begun with the mask set, %u full-target quads left out so far\n", armed, kept);
    }
    if (vglGetMaskClipStats) {   // 1.3 MSC_VGX_VGL_MASKCLIP (vitaGL patch 0055): mask clears drawn over the old region only, and the kilopixels of whole-target clear fill that left out, cumulative
        uint32_t narrowed = 0, kpx = 0;
        vglGetMaskClipStats(&narrowed, &kpx);
        std::fprintf(stderr, "[rt] scissor mask clear clip (MSC_VGX_VGL_MASKCLIP): %u clears over the old region only, %u kilopixels of clear fill left out so far\n", narrowed, kpx);
    }
    if (vglGetMemFreeStats) {   // 1.3 MSC_VGX_VGL_MEMFREE (vitaGL patch 0028): free-figure questions answered from the counters / by a walk, and in mode 2 the comparison
        uint32_t fast = 0, walked = 0, checks = 0, bad = 0, raced = 0, pools = 0;
        int32_t worst = 0;
        vglGetMemFreeStats(&fast, &walked, &checks, &bad, &raced, &worst, &pools);
        std::fprintf(stderr, "[mem] free-figure questions so far: %u answered from counters, %u by a heap walk; counted pools mask 0x%X; check (MSC_VGX_VGL_MEMFREE=2): %u made, %u differ (worst %d bytes), %u skipped (another thread allocated meanwhile)\n",
                     fast, walked, pools, checks, bad, static_cast<int>(worst), raced);
        // smooth (11 Nov): mask 0x0 with every answer a walk means vitaGL's start-up probes did not accept any pool. Vita3K never does (its sceClibMspaceMallocStats is "Unimplemented":
        // no statistics, so the counters have nothing to start from and the walk stays, silently); on a console a mask of 0 is a finding: look for "free figure kept on the heap walk" in the vitaGL log.
        static bool saidNoCounters = false;
        if (pools == 0 && fast == 0 && walked > 0 && !saidNoCounters) {
            saidNoCounters = true;
            std::fprintf(stderr, "[mem] NOTE: no pool is counted, so every free-figure question walks the heap (a few ms each on the render thread, one in 30 presents and at each size-step check). Expected in Vita3K "
                                 "(sceClibMspaceMallocStats is a stub there); on a console it means the allocator probes failed (MSC_VGX_VGL_MEMFREE=2 compares, the vitaGL log names the pool)\n");
        }
    }
    g_rtPrev = g_rtNow;
    for (int i = 0; i < 4; ++i) g_rtPrevDraw[i] = dt[i];
}
#endif

#ifdef VITAGL_RT
// 1.2: the driver's survived failures (a scene that could not be started, a shader program that could not be made, a timer record that was not
// written) go to the basic record once they happen: a line when the count first moves and then when it has doubled, at most 5 lines in a run.
void PollRobust() {
    static uint32_t reported = 0, lines = 0;
    uint32_t scenes = 0, shaders = 0;
    const uint32_t total = vglRobustFailures(&scenes, &shaders) + vglRtRingDrops();
    if (total == reported || (reported && total < 2 * reported) || lines >= 5) return;
    reported = total;
    ++lines;
    char text[160];
    std::snprintf(text, sizeof(text), "[vitaGL] survived failures so far: %u scene(s) not started, %u shader program(s) not made, %u timer record(s) not written",
                  static_cast<unsigned>(scenes), static_cast<unsigned>(shaders), static_cast<unsigned>(vglRtRingDrops()));
    std::fprintf(stderr, "%s\n", text);
    if (NpPlatformNote) NpPlatformNote(text);
}
#endif

// MSC_VITA_SHOT_HOLD=1 (Vita3K only, for comparing the screen itself): the display cannot be read back in the emulator (its guest memory lags), so a listed present stays on the screen 4 s for a
// capture from outside; the shot list then names the frames to hold and no file is written. With MSC_VIRTUAL_TIME the game does not notice.
bool ShotHold() {
    static const bool on = [] { const char* e = std::getenv("MSC_VITA_SHOT_HOLD"); return e && e[0] == '1'; }();
    return on;
}
void ShotHoldNow() {
    std::fprintf(stderr, "[vgx-vita] held present %llu for a capture\n", static_cast<unsigned long long>(g_presented));
    if (g_shotsWritten < 4096) ++g_shotsWritten;
    sceKernelDelayThread(4000000);
}
bool g_presentHold = false, g_presentHeld = false;   // MSC_VGX_HUDNATIVE
unsigned g_heldEfb = 0;
int g_heldRect[2] = {0, 0};
void PresentSwap(unsigned efb) {
    const uint64_t execTag = vgx::backend::ExecutingLatencyTag();
    const uint64_t latTag = execTag ? execTag : LatTagOf(vgx::backend::ExecutingFrame());
    if (latTag && NpLatPresented) NpLatPresented(latTag);
    SwapDisplay(latTag);
    // Back to what the back end had: it clears the EFB next.
    glBindFramebuffer(GL_FRAMEBUFFER, efb);
    if (g_scissorOn) { UnparkScissor(); glEnable(GL_SCISSOR_TEST); }
}
void PresentDisplayCopy(unsigned efb, int x, int y, int w, int h) {
    ++g_presented;
    if (g_presented <= 1000) BootProgress(VGXN_BOOT_PRESENT, static_cast<unsigned>(g_presented), 0);
    if (g_presented <= 2)   // W5 BOOT: the start's timeline (present 1 is the first frame; present 2 follows its shader warm-up)
        std::fprintf(stderr, "[boot] renderer: present %u%s  [+%lu ms]\n", static_cast<unsigned>(g_presented), g_presented == 2 ? " (the warm-up of frame 1 is done)" : "", BootMs());
#ifdef VITAGL_RT
    PollRobust();
    LogRt();
    LogSceneCensus();
    LogFreeGuard();
    LogEnvLock();
#endif
    const uint64_t now = NowUs();
    const vgx::backend::Stats stats = vgx::backend::GetStats();
    if (g_presented == 1)
        std::fprintf(stderr, "[vgx-vita] first present: display copy %d,%d %dx%d from framebuffer object %u\n", x, y, w, h, efb);
    PackMissDrain();   // (1.3: one line for each shader text the pack did not answer)
    // A long gap between two presents is almost always shader compilation: say so.
    if (g_lastPresentUs != 0 && now - g_lastPresentUs >= 100000u &&
        (stats.vertexShaders != g_lastStats.vertexShaders || stats.fragmentShaders != g_lastStats.fragmentShaders ||
         stats.programs != g_lastStats.programs))
        std::fprintf(stderr, "[vgx-vita] present %llu came %u ms after the previous one: +%u vertex shaders, +%u fragment shaders, +%u programs, +%u textures\n",
                     static_cast<unsigned long long>(g_presented), static_cast<unsigned>((now - g_lastPresentUs) / 1000u),
                     stats.vertexShaders - g_lastStats.vertexShaders, stats.fragmentShaders - g_lastStats.fragmentShaders,
                     stats.programs - g_lastStats.programs, stats.textures - g_lastStats.textures);
    if (g_presented == 1 || g_presented % 600 == 0) {
        std::fprintf(stderr, "[vgx-vita] present %llu: %u draw calls in the last buffer, %u vertex + %u fragment shaders and %u programs so far, %u textures, %u KiB of texels\n",
                     static_cast<unsigned long long>(g_presented), stats.drawCallsThisFrame, stats.vertexShaders, stats.fragmentShaders,
                     stats.programs, stats.textures, static_cast<unsigned>(stats.textureBytes / 1024u));
        // (A start that found its shaders in vitaGL's cache shows a few hundred milliseconds
        // here; one that compiled them, 0.3 to 1 s per shader.)
        std::fprintf(stderr, "[vgx-vita] shader work so far: %u compiles, %u links, %u ms in all, progress bar shown %u times\n", stats.shaderCompiles,
                     stats.programLinks, static_cast<unsigned>(stats.shaderWorkMs), stats.progressFrames);
        if (g_packCount != 0)
            std::fprintf(stderr, "[vgx-vita] shader pack: %u of the shaders asked for came from the package's pack  [+%lu ms]; %u asks it did not answer, %u distinct texts (%u of them first asked at present 3 or later, in play); %u compiles in all%s\n",
                         g_packHits.load(std::memory_order_relaxed), BootMs(), g_missLookups.load(std::memory_order_relaxed), g_missDistinct.load(std::memory_order_relaxed), g_missInPlay.load(std::memory_order_relaxed),
                         stats.shaderCompiles, g_missDistinct.load(std::memory_order_relaxed) > 256u ? " (only the first 256 misses have a line)" : "");
        {
            unsigned made = 0, ms = 0;
            vgxvita_cacheFolderStats(&made, &ms);
            if (made != 0) std::fprintf(stderr, "[vgx-vita] shader cache: %u bucket folders made as shaders needed them, %u ms in all\n", made, ms);
        }
        LogPools("now");
    }
    // 1.3 (MSC_VITA_SWEEP_BG, vita_threads.cpp): the 600-present sweep on the watch thread instead of here; logging builds time a few
    if (g_presented == 5 || g_presented == 30 || g_presented == 120 || (g_presented % 600 == 0 && !g_sweepBg)) {
        const uint64_t t0 = g_presented <= 1800 ? NowUs() : 0;
        PinLibraryThreads(false);
        if (t0) std::fprintf(stderr, "[vita] thread sweep at present %llu took %u us on the render thread\n", static_cast<unsigned long long>(g_presented), static_cast<unsigned>(NowUs() - t0));
    }
    g_lastPresentUs = now;
    g_lastStats = stats;

    // (With the text console selected the frame is still drawn and swapped,
    // so vitaGL's per-frame bookkeeping goes on; DisplayCallback then puts the
    // console's picture over it.)
    // A fatal-error screen has taken the display back: leave it alone.
    if (VitaConsole::OwnsDisplay()) return;
    if (efb == 0 || w <= 0 || h <= 0) return;
    if (!g_presentHold && !ShotHold() && ShotWanted(g_presented)) TakeShot(g_presented, x, y, w, h);
    const auto display = vgxvita::GetDisplayRect(g_stretch.load(std::memory_order_relaxed));   // 117..842 of 960 in the 4:3 box
    const int dx0 = display.x, dx1 = display.x + display.width;
    // Source rows are given bottom-up so the picture comes out upright.
    int sy0 = y + h, sy1 = y;
    if (g_flipY.load(std::memory_order_relaxed)) std::swap(sy0, sy1);
    if (g_scissorOn) { glDisable(GL_SCISSOR_TEST); ParkScissor(); }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, efb);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
#ifdef VITAGL_RT
    if (g_rtDisplayHint) vglRtSceneHint(0, VGL_RT_NOLOAD | VGL_RT_NOSTORE);   // (W5 RT: the present never uses the display's depth)
#endif
    if (vgxvita_maskGuardOn) vgxvita_maskGuard();   // (the display's scene starts with a mask update too)
    if (g_upscaleMode > 0) {
        // LOOK: the back end's picture shader (vgx_backend_gl.cpp DrawPicture). It binds the display itself, and the
        // back end trusts nothing it cached afterwards (CopyDisp). If it did not build (a console without the shader
        // compiler: the owner's has libshacccg, a clean one does not) the picture is blitted as before, for good.
        // The dynamic resolution (DYNRES in vgx_backend_gl.cpp) may draw the scene smaller: the smooth filter sharpens more the smaller it is
        // (0.5 at the game's own size, 0.59 at 86 % per side, 0.68 at 70 %, 0.8 at 50 %), and a little less when the scene is drawn larger.
        float amount = g_sharpen;
        const int pw = dx1 - dx0;
        vgx::backend::SetPictureSize(pw, kScreenH);   // (MSC_VGX_LODBIAS_RUNG reads it)
        const float stretch = vgx::PictureUpscale(w, h, pw, kScreenH);
        if (g_sharpenScale > 0.0f && (g_upscaleMode == 4 || g_upscaleMode >= 6) && !g_sharpenSet && stretch >= 0.97f) {
            // 1.3 MSC_VITA_SHARPEN_SCALE: the stretch decides (none at the screen's size: then the picture is copied as it is, the blit, with no picture shader)
            // (only for the modes with no edge smoothing of their own: SMOOTH and COMBINED keep their pass at the screen's size, with no sharpening,
            //  so a Quality picture never loses its edge smoothing when it is not multisampled)
            if (vgx::PictureIsPlainCopy(stretch) && !g_vitaMsaa && g_upscaleMode != 4 && g_upscaleMode != 8) {
                // (MSC_SHARPEN_EASE: a step up to the screen's size lets the strength fade out first, with the picture program)
                if (SharpEase() <= 0 || !vgx::sharpramp::StillSharpening(g_sharpRamp) ) {
                    if (SharpEase() > 0) vgx::sharpramp::Step(g_sharpRamp, 0.0f, SharpEase());
                    glBlitFramebuffer(x, sy0, x + w, sy1, dx0, 0, dx1, kScreenH, GL_COLOR_BUFFER_BIT, GL_LINEAR);
                    goto presented;
                }
            }
            amount = vgx::SharpenForUpscale(stretch, g_sharpenScale, g_sharpenCap);
        } else if ((g_upscaleMode == 4 || g_upscaleMode >= 6) && !g_sharpenSet) {
            const float s = VgxRenderScale();
            if (s < 0.99f) amount = std::min(0.8f, g_sharpen + (1.0f - s) * 0.6f);
            else if (s > 1.01f) amount = 0.4f;
        }
        if (SharpEase() > 0) amount = vgx::sharpramp::Step(g_sharpRamp, amount, SharpEase());
        if (!vgx::backend::DrawPicture(0, x, y, w, h, dx0, 0, dx1 - dx0, kScreenH, g_upscaleMode, amount, !g_flipY.load(std::memory_order_relaxed))) {
            std::fprintf(stderr, "[vgx-vita] picture shader unavailable: MSC_VITA_UPSCALE off\n");
            g_upscaleMode = 0;
            glBlitFramebuffer(x, sy0, x + w, sy1, dx0, 0, dx1, kScreenH, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        }
    } else {
        glBlitFramebuffer(x, sy0, x + w, sy1, dx0, 0, dx1, kScreenH, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }
presented:
    if (g_presentHold) {   // (MSC_VGX_HUDNATIVE: the back end draws on the display, then PresentSplitFinish)
        g_presentHeld = true;
        g_heldEfb = efb;
        g_heldRect[0] = dx0;
        g_heldRect[1] = dx1 - dx0;
        return;
    }
    PresentSwap(efb);
    if (ShotHold() && ShotWanted(g_presented)) ShotHoldNow();
}
// MSC_VGX_HUDNATIVE: the two steps of a present (vgx_cmd.h SetPresentSplit).
bool PresentSplitBegin(unsigned efb, int x, int y, int w, int h, int* displayX, int* displayW, int* displayH) {
    if (g_upscaleMode <= 0 || g_vitaMsaa) return false;   // (the blit ends the display's scene; a multisampled display is a different target)
    // int26 (Carbon #3): at native display dimensions (e.g. Smooth 60 960x544 1x) the HUD is already full resolution; skip the split-present pass even under COMBINED.
    const int dispW = vgxvita::GetDisplayRect(g_stretch.load(std::memory_order_relaxed)).width;
    if (w >= dispW && h >= kScreenH) return false;
    // 1.3 MSC_VITA_SHARPEN_SCALE: a picture at the screen's size has its HUD at the screen's size already, and is copied by the blit, which ends the display's scene
    if (g_sharpenScale > 0.0f && !g_sharpenSet && g_upscaleMode != 4 && g_upscaleMode != 8 && vgx::PictureIsPlainCopy(vgx::PictureUpscale(w, h, dispW, kScreenH))
        && (SharpEase() <= 0 || !vgx::sharpramp::StillSharpening(g_sharpRamp))) return false;
    g_presentHold = true;
    g_presentHeld = false;
    PresentDisplayCopy(efb, x, y, w, h);
    g_presentHold = false;
    if (!g_presentHeld) return false;
    *displayX = g_heldRect[0];
    *displayW = g_heldRect[1];
    *displayH = kScreenH;
    vgx::split17::Begin();
    return true;
}
void PresentSplitFinish() {
    PresentSwap(g_heldEfb);
    vgx::split17::Finish();
    if (ShotWanted(g_presented)) {
        if (ShotHold()) ShotHoldNow();
        else std::fprintf(stderr, "[vgx-vita] MSC_VGX_HUDNATIVE: present %llu has no EFB picture to save, MSC_VITA_SHOT_HOLD=1 holds it for a screen capture\n", static_cast<unsigned long long>(g_presented));
    }
}

// MSC_VGX_BACKEND_THREAD=1 (env.txt): the renderer's player gets a thread of its own and the
// render thread only executes finished command buffers, i.e. game / decode / GL on three
// threads, which is how both reference ports that run on consoles are laid out (ACGC: main,
// display-list worker on core 1, a third on core 2; melee-Vita: game core 0, submit worker
// core 2, render thread core 1). That thread is created inside the renderer with no
// affinity, so it is pinned here: core 2, next to the audio thread, never the game's core.
// The default stays two threads until a console run shows the render thread is the limit.
// (Cores: vita/src/vita_threads.cpp, MSC_VITA_CORE_PLAYER in env.txt.)
void PinPlayerThread() {
    std::fprintf(stderr, "[vgx-vita] record player on a thread of its own (MSC_VGX_BACKEND_THREAD)\n");
    VitaThreads::PinCurrentThread(VitaThreads::kPlayer);
}

// MSC_VGX_FEPAR (vgx_fepar.inc): the decode worker. Core 1 beside the back end by default (core 0 is the game's, core 2 the record player's and the audio's);
// MSC_VGX_FEPAR_CORE=<0..3> overrides (a test). Priority 0x10000100-16 (the job pool's, np_jobs.cpp; more urgent than the render thread, so a job is not queued behind
// a back-end burst: the player waits for it at the end of the picture), MSC_VGX_FEPAR_PRIO overrides.
// (core3x15, MSC_VITA_CORE3_WORK bit 2) With the proven, open system core the worker goes there instead (and parks at once, vgx_fepar.inc); the system refusing the mask, an unproven core or an online
// session: core 1 as before. FeparRepin: the lateness rule retired the placement (3 late joins in 48): back to the render core, off the online guard's list.
extern "C" int NpVitaCore3FeparOpen(void) __attribute__((weak));   // (native/platform/vita/src/vita_threads.cpp)
extern "C" void NpVitaCore3Placed(void) __attribute__((weak));
extern "C" void NpVitaCore3Unplaced(void) __attribute__((weak));
// (core60 / int26, MSC_VGX_FEPAR_KEEP) bit 1: relative priority 0x10000100 = 160 (legal range 0x100000E0..0x1000011F = abs 128..191).
// Run the core-3 decode worker at 0x10000100-24 = 0x100000E8 (abs 136): below audio (128) and np_core3watch (132, Carbon #6), ahead of np_jobs (144).
constexpr int kFeparPrioPlan = 0x10000100 - 16, kFeparPrioCore3Keep = 0x10000100 - 24;
int FeparPrio(bool onCore3) {
    int prio = (onCore3 && (vgx::FeparKeepBits() & 1)) ? kFeparPrioCore3Keep : kFeparPrioPlan;
    if (const char* t = std::getenv("MSC_VGX_FEPAR_PRIO")) prio = static_cast<int>(std::strtoul(t, nullptr, 0));
    return prio;
}
void FeparRepin() {
    VitaThreads::PinCurrentThread(VitaThreads::kRender);
    if (NpVitaCore3Unplaced) NpVitaCore3Unplaced();
    sceKernelChangeThreadPriority(sceKernelGetThreadId(), FeparPrio(false));
    std::fprintf(stderr, "[vgx-vita] decode worker: back on the render core (it kept the player waiting while on the system core)\n");
}
// (core60, KEEP bit 4) The worker asks to go back on the system core: only while the core is open (proven, not lost, no online session). Listed for the online guard first, then the
// check again: a retreat that ran in between has not seen this thread, so it leaves at once.
bool FeparReplace() {
    if (!NpVitaCore3FeparOpen || !NpVitaCore3FeparOpen() || std::getenv("MSC_VGX_FEPAR_CORE")) return false;
    const SceUID self = sceKernelGetThreadId();
    if (sceKernelChangeThreadCpuAffinityMask(self, 0x00080000) < 0) return false;
    if (NpVitaCore3Placed) NpVitaCore3Placed();
    if (!NpVitaCore3FeparOpen()) {
        VitaThreads::PinCurrentThread(VitaThreads::kRender);
        if (NpVitaCore3Unplaced) NpVitaCore3Unplaced();
        return false;
    }
    sceKernelChangeThreadPriority(self, FeparPrio(true));
    std::fprintf(stderr, "[vgx-vita] decode worker: back on the system core, priority 0x%X (MSC_VGX_FEPAR_KEEP)\n", static_cast<unsigned>(FeparPrio(true)));
    return true;
}
void PinFeparThread() {
    const SceUID self = sceKernelGetThreadId();
    int rc = 0;
    bool core3 = false;
    if (NpVitaCore3FeparOpen && NpVitaCore3FeparOpen() && !std::getenv("MSC_VGX_FEPAR_CORE")) {
        rc = sceKernelChangeThreadCpuAffinityMask(self, 0x00080000);   // (SCE_KERNEL_CPU_MASK_SYSTEM)
        core3 = rc >= 0;
        if (core3) { if (NpVitaCore3Placed) NpVitaCore3Placed(); vgx::FeparPlacedOnCore3(true); vgx::SetFeparRepin(FeparRepin); vgx::SetFeparReplace(FeparReplace); }
    }
    if (!core3) {
        VitaThreads::PinCurrentThread(VitaThreads::kRender);
        if (const char* c = std::getenv("MSC_VGX_FEPAR_CORE")) rc = sceKernelChangeThreadCpuAffinityMask(self, 0x00010000 << (std::strtol(c, nullptr, 0) & 3));
    }
    const int prio = FeparPrio(core3);
    const int rp = sceKernelChangeThreadPriority(self, prio);
    std::fprintf(stderr, "[vgx-vita] decode worker (MSC_VGX_FEPAR): %saffinity %s, priority 0x%X (%s)\n", core3 ? "on the system core (MSC_VITA_CORE3_WORK=2), " : "", rc >= 0 ? "ok" : "REFUSED", static_cast<unsigned>(prio), rp >= 0 ? "ok" : "REFUSED");
}

void* RenderThread(void*) {
    // The core plan is the platform layer's (vita/src/vita_threads.cpp): core 1 while the game
    // thread is the only heavy thread beside this one, core 2 (with audio) when the two-core
    // split's worker has core 1 (MSC_SPLIT=thread); MSC_VITA_CORE_RENDER in env.txt overrides.
    VitaThreads::PinCurrentThread(VitaThreads::kRender);
    {
        // int26: raise render thread above OS/UI/main/collector/shader threads (156..191) while staying below np_jobs and frame send (144); MSC_VITA_PRIO_RENDER overrides for A/B.
        int prio = 0x100000F4;   // relative -12 (abs 148)
        if (const char* p = std::getenv("MSC_VITA_PRIO_RENDER"); p && *p) prio = static_cast<int>(std::strtoul(p, nullptr, 0));
        if (prio > 0) {
            const int rp = sceKernelChangeThreadPriority(sceKernelGetThreadId(), prio);
            std::fprintf(stderr, "[vgx-vita] render thread priority 0x%X (%s; MSC_VITA_PRIO_RENDER)\n", static_cast<unsigned>(prio), rp >= 0 ? "ok" : "REFUSED");
        }
    }
    const bool ok = StartGl();
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_started = true;
        g_startOk = ok;
    }
    g_cv.notify_all();
    if (!ok) return nullptr;
    // Streaming mode: this thread is the player (front end and back end).
    if (vgx::rec::g_mode == vgx::rec::MODE_THREAD) vgx::rec::PlayerThreadMain();
    for (;;) {
        std::unique_lock<std::mutex> lk(g_mtx);
        while (g_jobBuf == nullptr) g_cv.wait_for(lk, std::chrono::milliseconds(20));   // (W5 PIPE: timed rounds, see EnsureThread)
        const vgx::CmdBuf* buf = g_jobBuf;
        lk.unlock();
        vgx::backend::Execute(*buf);
        lk.lock();
        g_jobBuf = nullptr;
        lk.unlock();
        g_cv.notify_all();
    }
    return nullptr;
}

bool EnsureThread() {
    if (!g_threadCreated) {
        g_threadCreated = true;
        // The renderer picks its mode from the environment.
        if (Exists("ux0:data/mscharged/vgx_play_frame")) setenv("MSC_VGX_PLAY", "frame", 1);
        else if (Exists("ux0:data/mscharged/vgx_play_immediate")) setenv("MSC_VGX_PLAY", "immediate", 1);
        // Settings a console needs, unless env.txt chose otherwise (the
        // renderer reads those two variables itself).
        sceIoMkdir(kShaderCacheDir, 0777);
        if (!std::getenv("MSC_VGX_SHADERLIST")) {
            SeedShaderList();
            vgx::SetShaderListFile(kShaderListFile);
        }
        if (!std::getenv("MSC_VGX_TEXBUDGET")) vgx::SetTextureBudget(kTextureBudget);
        vgx::hdtex::Install(nullptr);   // 1.1: HD character textures from ux0:data/mscharged/hdtex.pack, when it is there (MSC_HDTEX=0: off)
        std::fprintf(stderr, "[vgx-vita] shader warm-up list: %s (%s); texture budget: %s\n",
                     std::getenv("MSC_VGX_SHADERLIST") ? std::getenv("MSC_VGX_SHADERLIST") : kShaderListFile,
                     Exists(std::getenv("MSC_VGX_SHADERLIST") ? std::getenv("MSC_VGX_SHADERLIST") : kShaderListFile)
                         ? "found: all known shaders are compiled during the first frame" : "not there yet: shaders are compiled as the game needs them",
                     std::getenv("MSC_VGX_TEXBUDGET") ? "MSC_VGX_TEXBUDGET (MiB) from env.txt" : "32 MiB");
        vgx::rec::InitMode();
        vgx::rec::SetPlayerThreadInit(PinPlayerThread);
        vgx::SetFeparThreadInit(PinFeparThread);
        // Frame presenter (MSC_VGX_PRESENT=30|60, default off): its ticks start at display vblanks, so every picture
        // appears the same number of vblanks after its tick (vgx_present.h). A repeated picture needs nothing here:
        // not swapping leaves the last one on the screen.
        if (vgx::present::Enabled()) {
            vgx::present::SetVblankSource([]() -> uint32_t { return static_cast<uint32_t>(sceDisplayGetVcount()); });
            std::fprintf(stderr, "[vgx-vita] frame presenter on: display ticks on sceDisplayGetVcount\n");
        }
        static const char* const kModes[3] = {"immediate (game thread, after every call)", "frame (game thread, at present)",
                                              "thread (render thread, streaming)"};
        std::fprintf(stderr, "[vgx-vita] record player mode: %s\n", kModes[vgx::rec::g_mode % 3]);
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, kRenderStack);
        pthread_t thread;
        const int res = pthread_create(&thread, &attr, RenderThread, nullptr);
        pthread_attr_destroy(&attr);
        if (res != 0) {
            g_startCause = 6;
            std::snprintf(g_startError, sizeof(g_startError), "could not create the render thread (error %d)", res);
            return false;
        }
        pthread_detach(thread);
        std::unique_lock<std::mutex> lk(g_mtx);
        // (W5 PIPE: a timed round instead of an untimed wait: a signal sent between the test and the block would otherwise hang the start-up for ever)
        while (!g_started) g_cv.wait_for(lk, std::chrono::milliseconds(50));
    }
    return g_startOk;
}

// SELECT+START+D-pad: see the header comment.
void DebugControls() {
    static uint32_t last = 0;
    VitaInput::Raw raw;
    if (!VitaInput::Read(raw)) return;
    const uint32_t chord = VitaInput::kSelect | VitaInput::kStart;
    const uint32_t now = (raw.buttons & chord) == chord ? (raw.buttons & (VitaInput::kUp | VitaInput::kDown | VitaInput::kRight)) : 0;
    const uint32_t pressed = now & ~last;
    last = now;
    if (pressed & VitaInput::kDown) {
        const bool console = !g_showConsole.load();
        g_showConsole.store(console);
        std::fprintf(stderr, "[vgx-vita] display: %s\n", console ? "text console over the picture" : "picture");
    }
    if (pressed & VitaInput::kUp) {
        const bool flip = !g_flipY.load();
        g_flipY.store(flip);
        std::fprintf(stderr, "[vgx-vita] present: vertical flip %s\n", flip ? "on" : "off");
    }
    if (pressed & VitaInput::kRight) {
        const bool stretch = !g_stretch.load();
        g_stretch.store(stretch);
        std::fprintf(stderr, "[vgx-vita] present: %s\n", stretch ? "stretched to 960x544" : "4:3 with side bars");
    }
}

} // namespace
extern "C" void vgxvita_pin_library_threads(void) { PinLibraryThreads(false); }
// diag13 MSC_DQ_TIMING: the display-queue call's figures since this cursor's last call (id 0: the [hitch] window, 1: the [late] line); 0 when the switch is off or no swap was made.
// n swaps, their mean and longest ms, and how many took 8 ms or more and 33 ms or more (a full queue holds the render thread for whole refreshes).
extern "C" int vgx_host_dq_window(int id, unsigned* n, float* meanMs, float* maxMs, unsigned* over8, unsigned* over33) {
    if (!g_dqTiming || id < 0 || id > 1) return 0;
    static uint64_t lastN[2], lastSum[2], lastBin[2][6];
    const uint64_t cn = g_dq.n.load(std::memory_order_relaxed), cs = g_dq.sumUs.load(std::memory_order_relaxed);
    uint64_t cb[6];
    for (int i = 0; i < 6; ++i) cb[i] = g_dq.bin[i].load(std::memory_order_relaxed);
    const uint64_t dn = cn - lastN[id], ds = cs - lastSum[id];
    const uint64_t o8 = (cb[3] - lastBin[id][3]) + (cb[4] - lastBin[id][4]) + (cb[5] - lastBin[id][5]);   // (bins 3-5: 8 ms and over)
    const uint64_t o33 = cb[5] - lastBin[id][5];
    lastN[id] = cn; lastSum[id] = cs;
    for (int i = 0; i < 6; ++i) lastBin[id][i] = cb[i];
    if (dn == 0) return 0;
    *n = static_cast<unsigned>(dn);
    *meanMs = static_cast<float>(ds) / static_cast<float>(dn) * 0.001f;
    *maxMs = static_cast<float>(g_dq.maxUs[id].exchange(0, std::memory_order_relaxed)) * 0.001f;
    *over8 = static_cast<unsigned>(o8);
    *over33 = static_cast<unsigned>(o33);
    return 1;
}
// diag13: the display-queue call of the newest picture, ms (0 when MSC_DQ_TIMING is off).
extern "C" float vgx_host_last_dq_ms(void) { return static_cast<float>(g_dq.lastUs.load(std::memory_order_relaxed)) * 0.001f; }
// render13 (MSC_VITA_DQ_TIMING): sceGxmDisplayQueueAddEntry alone, ms of the last swap; -1 when off.
extern "C" float vgx_host_last_dqadd_ms(void) {
    if (!g_dqAddTiming) return -1.0f;
    uint32_t last = 0; vglDisplayQueueStats(&last, nullptr, nullptr, nullptr, nullptr, nullptr);
    return static_cast<float>(last) * 0.001f;
}
extern "C" float vgx_host_last_swap_ms(void) { return g_lastSwapMs.load(std::memory_order_relaxed); }
// 1.3 wave 10 (vitaGL 0035): what the patcher and the render target creates cost since the last call (the back end asks once a picture): calls[3] / us[3] / maxUs[3] for vertex program creates, fragment program creates
// and sceGxmCreateRenderTarget, and the patch failures so far (vglRobustFailures: programs the patcher refused, e.g. a full code pool).
extern "C" uint64_t VgxHostWaitUs(void) { return g_bephWaitUs.load(std::memory_order_relaxed); }   // be25 (MSC_VGX_BEPH): see SwapDisplay
extern "C" void vgx_host_patch_take(uint32_t* calls, uint64_t* us, uint32_t* maxUs, uint32_t* failures) {
    vglPatchTimingTake(calls, us, maxUs);
    uint32_t scenes = 0, shaders = 0;
    vglRobustFailures(&scenes, &shaders);
    *failures = shaders;
}
// 1.3 wave 11 (logs16, MSC_HITCH_MS only): free KiB per vitaGL pool (VRAM, RAM, PHYCONT = cdram), so a [hitch] line can show which pool a picture's new pass surfaces came out of (the change since the last picture).
extern "C" void vgx_host_pool_free_kib(int32_t* out3) {
    out3[0] = static_cast<int32_t>(vglMemFree(VGL_MEM_VRAM) / 1024);
    out3[1] = static_cast<int32_t>(vglMemFree(VGL_MEM_RAM) / 1024);
    out3[2] = static_cast<int32_t>(vglMemFree(VGL_MEM_PHYCONT) / 1024);
}
// (vitaGL 0036: GPU memory recoveries so far, the time they took in total, and the longest one; the back end takes the difference since the last picture)
extern "C" void vglGetGcRecoveryStats(uint32_t* recoveries, uint32_t* resolved, uint32_t* failed, uint32_t* pauses, uint32_t* legacy, uint32_t* cycles, uint32_t* maxUs, uint64_t* totalUs, uint64_t* pauseUs, uint64_t* bytes) __attribute__((weak));
extern "C" int vgx_host_gc_stats(uint32_t* recoveries, uint64_t* totalUs) {
    if (!vglGetGcRecoveryStats) return 0;
    uint32_t ok, bad, waits, legacy, cycles, maxUs;
    uint64_t pauseUs, bytes;
    vglGetGcRecoveryStats(recoveries, &ok, &bad, &waits, &legacy, &cycles, &maxUs, totalUs, &pauseUs, &bytes);
    return 1;
}
extern "C" uint32_t vgx_host_pack_hits(void) { return g_packHits.load(std::memory_order_relaxed); }
extern "C" uint32_t vgx_host_pack_misses(uint32_t* inPlay) { if (inPlay) *inPlay = g_missInPlay.load(std::memory_order_relaxed); return g_missDistinct.load(std::memory_order_relaxed); }   // (distinct shaders the pack did not answer; of them first asked in play)

extern "C" int vgx_vita_load_packed_shader(unsigned int shader, int fragment, uint64_t hash) {
    const PackEntry* entry = PackFind(fragment, hash);
    if (!entry || !entry->size || entry->size > 0x7FFFFFFFu) return 0;
    glShaderBinary(1, &shader, 0, g_pack + entry->offset, static_cast<int>(entry->size));
    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled) g_packHits.fetch_add(1, std::memory_order_relaxed);
    return compiled != 0;
}

// ---- GL calls routed here from the back end (include/GLES2/gl2.h)
// MSC_VITA_MASK_GUARD=1 (default 0): FOR VITA3K'S VULKAN RENDERER ONLY, never needed on a
// console. vitaGL implements the scissor with sceGxm's mask bit and redraws the mask (one
// quad over the target, or two when the scissor is on) inside the first draw or clear after
// every scene start and every scissor change. Vita3K's Vulkan renderer has no mask ("Mask
// not implemented in the vulkan renderer!") and draws those quads into the COLOUR surface,
// white or transparent black, at depth 1.0 under whatever depth test is current: with a
// clean emulator shader cache the menus stand on white and the 3D backgrounds are painted
// over (run gl24V; v3k/NOTES.md, session 3). With the guard on, every draw and clear of the
// back end is preceded by a clear of nothing under depth function NEVER: vitaGL does its
// mask update inside that call (scene_reset() is the first statement of its glClear), the
// emulator's quads fail the depth test, and the draw that follows finds the mask clean.
// The clear of nothing itself is a quad with the fragment program disabled and depth
// writes off. The scissor still works there through the region clip vitaGL sets beside
// the mask. Run gl26V with it: boot screens, menus in their dark room, holograms, stadium
// with its crowd, kick-off and open play as in the PC pictures.
extern "C" { unsigned char vgxvita_maskGuardOn = 0; }
namespace {
GLenum g_depthFunc = GL_LESS;   // GL's initial values
bool g_depthTestOn = false;
} // namespace
extern "C" uint64_t NpDiagDisplayTrace(unsigned stage, uint64_t token, void* framebuffer, uint32_t interval, int32_t result) {
    if (!NpDiagActive || !NpDiagEvent || !NpDiagActive() || stage > 5) return 0;
    if (stage == 0) {
        constexpr size_t capacity = sizeof(g_latFifo.tag) / sizeof(g_latFifo.tag[0]);
        static_assert(capacity == 32, "Display FIFO producer and gate use 32 slots");
        const uint32_t tail = g_latFifo.tail.load(std::memory_order_relaxed);
        const uint32_t head = g_latFifo.head.load(std::memory_order_acquire);
        if (head == tail || uint32_t(head - tail) > capacity) return 0;
        const uint64_t tag = g_latFifo.tag[tail % capacity];
        if (!tag || tag > UINT32_MAX) return 0;
        // Peek only. GateDisplay owns consumption; later stages use this copied identity.
        token = (uint64_t(g_diagFifoGeneration.load()) << 32) | tag;
    }
    if (!token) return 0;
    NpDiagEvent(NPD_LIBRARY_DISPLAY, stage, uint32_t(token), token >> 32,
                reinterpret_cast<uintptr_t>(framebuffer), uint32_t(sceDisplayGetVcount()), interval, uint32_t(result));
    return token;
}

extern "C" void vgxvita_glDepthFunc(GLenum func) {
    g_depthFunc = func;
    glDepthFunc(func);
}
extern "C" void vgxvita_maskGuard(void) {
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_NEVER);
    glClear(0);
    glDepthFunc(g_depthFunc);
    if (!g_depthTestOn) glDisable(GL_DEPTH_TEST);
}

// The scissor rectangle while the scissor is OFF. GL ignores it then; vitaGL does not quite:
// update_scissor_test() (source/tests.c) clips the rectangle to the bound target whether the
// test is on or not, and a rectangle that starts right of a narrower target (an atlas cell at
// x = 594, then a 512x256 pass target) leaves a NEGATIVE width in vitaGL's `region`, which
// scene_reset() (source/gxm.c) hands to sceGxmSetRegionClip at the start of the next scene
// with the scissor on, before the update that replaces it. No draw happens in between, so on
// a console that is at worst an odd clip for no pixels. Vita3K's OpenGL renderer turns it
// into glScissor with a negative size (GL_INVALID_VALUE, seen with MESA_DEBUG, 63 times on
// the way to kick-off), and the emulator blames the left-over error on whatever host object
// it creates next: "Failed to initialise color surface texture!", and once a failed
// sceGxmCreateRenderTarget followed by a crash at the post-match scene (run gl14).
// So the rectangle the back end set is kept here, vitaGL's copy is parked at a rectangle
// inside any target while the scissor is off, and put back when it is switched on. Free:
// glEnable/glDisable(GL_SCISSOR_TEST) mark vitaGL's scissor stale anyway.
namespace {
GLint g_scRect[4] = {0, 0, 1, 1};
void ParkScissor() { glScissor(0, 0, 1, 1); }
void UnparkScissor() { glScissor(g_scRect[0], g_scRect[1], g_scRect[2], g_scRect[3]); }
} // namespace
extern "C" void vgxvita_glScissor(GLint x, GLint y, GLsizei width, GLsizei height) {
    g_scRect[0] = x; g_scRect[1] = y; g_scRect[2] = width; g_scRect[3] = height;
    if (g_scissorOn) glScissor(x, y, width, height);   // (off: it is given when the scissor is switched on)
}

extern "C" void vgxvita_glEnable(GLenum cap) {
    if (cap == GL_DITHER) return;
    if (cap == GL_SCISSOR_TEST) { g_scissorOn = true; UnparkScissor(); }
    if (cap == GL_DEPTH_TEST) g_depthTestOn = true;
    glEnable(cap);
}
extern "C" void vgxvita_glDisable(GLenum cap) {
    if (cap == GL_DITHER) return;   // vitaGL has no dithering to switch off
    if (cap == GL_SCISSOR_TEST) g_scissorOn = false;
    if (cap == GL_DEPTH_TEST) g_depthTestOn = false;
    glDisable(cap);
    if (cap == GL_SCISSOR_TEST) ParkScissor();
}

// The constant a disabled attribute array supplies. In GL the value set with
// glVertexAttrib4f stays until it is set again, and the back end relies on
// that: it sets a location's constant when its array is switched off, not
// before every draw. vitaGL (source/custom_shaders.c, reserve_attrib_pool)
// stores each value in the next slot of a ring of 16,384 four-float slots and
// keeps only a pointer to it per location, so once the ring has gone round, a
// location whose constant was set long ago reads whatever a later call wrote
// into its slot (a texture-matrix index of 60 turning into 0 or 1, say),
// silently and only after some seconds of play. The values are therefore
// shadowed here and all of them are issued again every 2,048 calls, long
// before a slot can come up for reuse.
extern "C" void vgxvita_glVertexAttrib4f(GLuint index, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3) {
    static float shadow[16][4];
    static uint32_t known = 0, sinceRefresh = 0;
    glVertexAttrib4f(index, v0, v1, v2, v3);
    if (index >= 16) return;
    shadow[index][0] = v0;
    shadow[index][1] = v1;
    shadow[index][2] = v2;
    shadow[index][3] = v3;
    known |= 1u << index;
    if (++sinceRefresh < 2048) return;
    sinceRefresh = 0;
    for (GLuint i = 0; i < 16; ++i)
        if (i != index && (known & (1u << i))) glVertexAttrib4f(i, shadow[i][0], shadow[i][1], shadow[i][2], shadow[i][3]);
}

// Texture data that goes through the GPU's transfer unit. vitaGL converts a compressed level
// (and generates mip levels) with sceGxmTransferCopy / sceGxmTransferDownscale and returns
// without waiting for the transfer (utils/gpu_utils.c gpu_alloc_compressed_texture,
// gpu_alloc_mipmaps); it only waits, with sceGxmTransferFinish, where it reads a transfer's
// result itself (framebuffers.c, glReadPixels). But the upload of the NEXT level of the same
// texture reallocates the texture's memory (vgl_realloc in gpu_alloc_compressed_texture):
// if the block moves while the previous level's transfer is still queued, the transfer
// writes into the freed block and the copy made by the reallocation holds whatever was
// there before. In Vita3K, which runs transfers later on its render thread (SceGxm.cpp
// sceGxmTransferCopy -> renderer::transfer_copy, sent without waiting), that happened in
// every session at the title screen, the first scene with mip-mapped DXT1 textures: three
// runs ended in the emulator's allocator (a null link in the freed block) or with the game
// thread reading garbage, the run with MSC_VGX_DXT1=0 did not (5 Oct, v3k/NOTES.md). On a
// console the window is a transfer of a few kilobytes against a memcpy, so it probably never
// shows, but nothing orders the two. Waiting for the transfer unit after each such call
// closes it on both; the wait covers our own pending transfers only, not rendering.
// MSC_VITA_UPLOAD_SYNC=0 in env.txt turns the wait off (to measure what it costs).
static bool UploadSync() {
    static const bool on = [] {
        const char* e = std::getenv("MSC_VITA_UPLOAD_SYNC");
        const bool v = !(e && *e && std::atoi(e) == 0);
        std::fprintf(stderr, "[vgx-vita] texture transfers: %s (MSC_VITA_UPLOAD_SYNC)\n",
                     v ? "waited for after every compressed level and mip generation" : "NOT waited for");
        return v;
    }();
    return on;
}
extern "C" void vgxvita_glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border,
                                               GLsizei imageSize, const void* data) {
    glCompressedTexImage2D(target, level, internalformat, width, height, border, imageSize, data);
    if (UploadSync()) sceGxmTransferFinish();
}
extern "C" void vgxvita_glGenerateMipmap(GLenum target) {
    glGenerateMipmap(target);
    if (UploadSync()) sceGxmTransferFinish();
}

// vitaGL's own diagnostics (the library is built with LOG_ERRORS and with its
// print function renamed to this, see vita/Makefile.vitadeps). The first 400
// messages are logged in full; after that one in 500, so an error raised by
// every draw call cannot flood the log.
// ---- Legacy-library fallback: cache folders made when they are needed
// A library with vglStrikersShaderCacheCaps owns this work on actual cache misses.
// Its host wrappers forward directly, without duplicate source hashing or seed I/O.
// vitaGL stores a compiled shader as <cache>/v<n>/{v,f}/<HH>/<hash>.gxp (VglCacheVersion), hash = XXH3_64bits of
// the source text as handed to glShaderSource (all strings joined), <HH> = its top byte
// (source/custom_shaders.c, glCompileShader and glLinkProgram). It never creates <HH> when it
// saves: it expects all 512 of them from its start-up, and a save into a missing folder fails
// without a word (the shader is then compiled again at every start). Those 512 folders are
// what made the first start slow on a console (SeedShaderCache above), so they are made here,
// one at a time, for the shaders that exist. The file names this computes were checked
// against a cache written by vitaGL in the emulator: 141 of 141 generated shaders, the copy
// pair and the self-test pair (vgx/NOTES.md, round 6, "SEED").
extern "C" { extern char vgl_shader_cache_path[256] __attribute__((weak)); }
namespace {
uint8_t g_bucketSeen[2][32];           // [vertex, fragment][bucket / 8]
std::vector<uint8_t> g_shaderKind;     // by shader handle: 1 vertex, 2 fragment
unsigned g_bucketsMade = 0;
uint64_t g_bucketUs = 0;
void EnsureBucket(int kind, unsigned bucket) {
    uint8_t& bits = g_bucketSeen[kind][bucket >> 3];
    const uint8_t bit = static_cast<uint8_t>(1u << (bucket & 7u));
    if (bits & bit) return;
    bits |= bit;
    if (&vgl_shader_cache_path[0] == nullptr || vgl_shader_cache_path[0] == 0) return;
    char path[320];
    if (std::snprintf(path, sizeof(path), "%s/%s/%02X", vgl_shader_cache_path, kind ? "f" : "v", bucket) >= static_cast<int>(sizeof(path))) return;
    const uint64_t t0 = NowUs();
    if (sceIoMkdir(path, 0777) >= 0) ++g_bucketsMade;   // (an error = it is there already, or the cache is read-only)
    g_bucketUs += NowUs() - t0;
}
} // namespace
extern "C" GLuint vgxvita_glCreateShader(GLenum type) {
    const GLuint shader = glCreateShader(type);
    if (!CacheWriterOwnsBuckets() && shader < 65536u) {
        if (g_shaderKind.size() <= shader) g_shaderKind.resize(shader + 64u, 0);
        g_shaderKind[shader] = type == GL_VERTEX_SHADER ? 1 : 2;
    }
    return shader;
}
extern "C" void vgxvita_glShaderSource(GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length) {
    if (!CacheWriterOwnsBuckets() && shader < g_shaderKind.size() && g_shaderKind[shader] != 0 && count > 0 && string) {
        uint64_t hash;
        if (count == 1 && !(length && length[0] >= 0)) {
            hash = XXH3_64bits(string[0], std::strlen(string[0]));
        } else {
            static std::string joined;   // (render thread only)
            joined.clear();
            for (GLsizei i = 0; i < count; ++i) {
                if (length && length[i] >= 0) joined.append(string[i], static_cast<size_t>(length[i]));
                else joined.append(string[i]);
            }
            hash = XXH3_64bits(joined.data(), joined.size());
        }
        const int kind = g_shaderKind[shader] == 1 ? 0 : 1;
        // W5 BOOT: a shader the package has (pack or linked seed folder) is never compiled or saved here: no folder for it (a new folder costs
        // about 310 ms on a console's memory card; link mode made one per seeded shader).
        bool packaged = PackFind(kind, hash) != nullptr;
        if (!packaged && g_seedLinkPath[0]) {
            char path[320];
            char dir[320];
            std::snprintf(dir, sizeof(dir), "%s/%s/%02X", g_seedLinkPath, kind ? "f" : "v", static_cast<unsigned>(hash >> 56));
            ShaderFileName(path, sizeof(path), dir, hash, 3);
            packaged = Exists(path);
            if (!packaged && (hash >> 60) == 0) { ShaderFileName(path, sizeof(path), dir, hash, 2); packaged = Exists(path); }   // (a seed of the older layout)
        }
        if (!packaged) EnsureBucket(kind, static_cast<unsigned>(hash >> 56));
    }
    glShaderSource(shader, count, string, length);
}
extern "C" void vgxvita_cacheFolderStats(unsigned* made, unsigned* ms) {
    if (made) *made = g_bucketsMade;
    if (ms) *ms = static_cast<unsigned>(g_bucketUs / 1000u);
}

extern "C" int msc_vgl_printf(const char* format, ...) __attribute__((format(printf, 1, 2)));
extern "C" int msc_vgl_printf(const char* format, ...) {
    static uint32_t count = 0, compilerWarnings = 0;
    char text[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    // The shader compiler's warnings (vitaGL is built with HAVE_SHARK_LOG): the generated
    // shaders declare locals they do not always use, which gives four or five "local variable
    // ... is unreferenced" lines per compile, some five hundred in a first session. They
    // would use up the 400 full messages before anything that matters is said, so only the
    // first six are logged and the rest are neither logged nor counted. Errors ("E]") and
    // everything else go on as before.
    if (std::strstr(text, "Shader Compiler: W]")) {
        if (++compilerWarnings > 6) return 0;
        if (compilerWarnings == 6) std::fprintf(stderr, "[vitaGL] (further shader compiler warnings are not logged)\n");
    }
    ++count;
    // After the first 400 messages only every 500th is kept, which hid exactly the lines a long session needs (found
    // by Codex's log audit, 7 Oct: "Scene reset failed", "Failed to create a rendertarget", forced garbage collection
    // of video memory, a uniform pool overrun). Those stay, up to 300 more.
    static uint32_t serious = 0;
    if (count > 400 && count % 500 != 0) {
        const bool important = std::strstr(text, "ailed") || std::strstr(text, "rror") || std::strstr(text, "E]") || std::strstr(text, "ERROR") ||
                               std::strstr(text, "outage") || std::strstr(text, "overrun") || std::strstr(text, "forcefully") || std::strstr(text, "incomplete") ||
                               std::strstr(text, "VGL_RING") /* (cx/renderer/vitagl-patches/0001: uniform ring wrap log) */;
        if (!important || ++serious > 300) return 0;
    }
    size_t length = std::strlen(text);
    while (length != 0 && (text[length - 1] == '\n' || text[length - 1] == '\r')) text[--length] = '\0';
    // 1.2: the driver's own account of an allocation, scene or program that failed reaches the basic record too (a quiet build has no stderr). At most
    // 10 lines of 200 characters in a run; the counts of the survivable kinds come separately (PollRobust).
    if (NpPlatformNote && (std::strstr(text, "ailed") || std::strstr(text, "failed")) && !std::strstr(text, "Shader Compiler")) {
        static uint32_t noted = 0;
        if (noted < 10) {
            ++noted;
            char brief[200];
            std::snprintf(brief, sizeof(brief), "[vitaGL] %.190s", text);
            NpPlatformNote(brief);
        }
    }
    if (count > 400) std::fprintf(stderr, "[vitaGL] (message %u) %s\n", count, text);
    else std::fprintf(stderr, "[vitaGL] %s\n", text);
    return static_cast<int>(length);
}

namespace vgx {

bool LogEnabled() {
    static const bool on = Exists(kVerboseMarker);
    return on;
}

// Used in the "frame" and "immediate" modes only: the game thread decoded
// the records itself and hands the command buffer over; returns when it has
// been executed. (In "thread" mode the player executes buffers directly.)
void SubmitToBackend(CmdBuf& buf) {
    if (!EnsureThread()) return;
    std::unique_lock<std::mutex> lk(g_mtx);
    g_jobBuf = &buf;
    g_cv.notify_all();
    while (g_jobBuf != nullptr) g_cv.wait_for(lk, std::chrono::milliseconds(20));   // (W5 PIPE: timed rounds)
}

} // namespace vgx

using namespace vgx;

#ifdef VITAGL_GLB
// W5 GLB: one line of the patched vitaGL's own counters, printed with the MSC_VGX_PROF line (src/vgx_backend_gl.cpp).
// 1.2 (MSC_PACE_VBLANK, np_framepace.cpp): how many real vblanks the picture before last was on the screen, as the display itself counted them (0: unknown).
extern "C" int NpDisplayLastHeld(void) { return static_cast<int>(g_lastHeld.load(std::memory_order_relaxed)); }
// 1.2: one line for the run header of the logs: how the picture gets to the screen and how the frames are queued.
extern "C" int vgx_vita_describe(char* out, int size) {
    static const char* const picture[9] = {"blit", "linear", "sharp", "sharp+", "smooth", "smooth", "fast", "cas", "combined"};
    const char* const queue = g_queue.mode == 0 ? "off" : g_queue.mode == 1 ? "1" : g_queue.mode == 2 ? "2" : g_queue.mode == 3 ? "auto" : "auto (not read yet)";
    return std::snprintf(out, static_cast<size_t>(size), "picture=%s sharpen=%.2f%s aniso=%d startup-msaa=%d queue=%s%s display-buffers=%d fbo-scenes=%d display-scenes=%d display-gate=%s",
                         picture[g_upscaleMode >= 0 && g_upscaleMode <= 8 ? g_upscaleMode : 0], g_sharpen, g_sharpenSet ? " (set)" : "", g_anisoLevel, g_vitaMsaa, queue, g_queue.fixed30 ? (g_queue.fixed30 == 2 ? " (depth 2 at 30, 1 when overloaded)" : " (depth 2 at 30)") : "", g_vglBuffers, g_vglFboScenes,
                         g_vglDisplayScenes, g_gate.enabled == 0 ? "off" : "on");
}
// 1.2: the state-cache counters as per-frame means since the last line: issued/skipped per setter. Printed only when the library has the cache (state_built) and
// it is switched on, or when the calls are being counted for comparison (MSC_VGX_GXMCACHE=0 in a library that has it prints the issued counts alone).
static void LogGxmStateCounters(const vglGlbStats& s) {
    static const char* const kName[VGL_GXS_KINDS] = {"fdepthfunc", "bdepthfunc", "fdepthwrite", "bdepthwrite", "fstencilfunc", "bstencilfunc", "fstencilref", "bstencilref",
        "fpolymode", "bpolymode", "cull", "twosided", "fdepthbias", "bdepthbias", "viewport", "regionclip", "vtex0", "vtex1", "vtex2", "vtex3", "vdefubuf", "fdefubuf", "vubo", "fubo"};
    static uint32_t prevCalls[VGL_GXS_KINDS], prevSkipped[VGL_GXS_KINDS], prevFrame = 0;
    if (s.version < 2 || !s.state_built) return;
    const uint32_t frames = s.frame > prevFrame ? s.frame - prevFrame : 1u;
    char line[768];
    size_t n = 0;
    uint64_t totCalls = 0, totSkipped = 0;
    for (int i = 0; i < VGL_GXS_KINDS; ++i) {
        const uint32_t c = s.state_calls[i] - prevCalls[i], k = s.state_skipped[i] - prevSkipped[i];
        totCalls += c;
        totSkipped += k;
        if (c && n + 40 < sizeof(line)) n += static_cast<size_t>(std::snprintf(line + n, sizeof(line) - n, " %s %.1f/%.1f", kName[i], static_cast<double>(c - k) / frames, static_cast<double>(k) / frames));
        prevCalls[i] = s.state_calls[i];
        prevSkipped[i] = s.state_skipped[i];
    }
    prevFrame = s.frame;
    std::fprintf(stderr, "[vgx] vitaGL gxm state cache (mask 0x%02X): per frame over %u frames, sceGxm calls issued %.1f skipped %.1f of %.1f |%s (issued/skipped per setter)\n", s.state_mask, frames,
                 static_cast<double>(totCalls - totSkipped) / frames, static_cast<double>(totSkipped) / frames, static_cast<double>(totCalls) / frames, n ? line : " none");
}
extern "C" void vgx_vita_stream_line(unsigned pictures) {   // cache25: once per [lane] window, MSC_VITA_STREAM_CACHED=1 only (nxgl25: also the [wrcopy] counters line, MSC_VITA_WRCOPY on)
#ifdef VITAGL_GLB
    if (pictures && g_wrCopyMode && vglWriteCopyStats8) {
        static uint32_t prev[8] = {};
        uint32_t now[8];
        vglWriteCopyStats8(now);
        uint32_t d[8];
        for (int i = 0; i < 8; ++i) { d[i] = now[i] - g_wrBase[i] - prev[i]; prev[i] = now[i] - g_wrBase[i]; }
        const double p = static_cast<double>(pictures);
        std::fprintf(stderr, "[wrcopy] per picture over %u pictures (mode %d, fallback %s): made by NEON stores %.1f copies (%.1f KiB), made wholly by sceClibMemcpy %.1f (%.1f KiB: under 192 B, mode 0%s), DMA %.1f copies %.1f KiB, 8 KiB+ not given to DMA (address at or above 0x81000000) %.1f, DMA errors %.1f\n", pictures,
                     g_wrCopyMode, g_wrCopyFb ? "neon64" : "plain", d[0] / p, d[1] / 1024.0 / p, d[2] / p, d[3] / 1024.0 / p, g_wrCopyMode == 3 && !g_wrCopyFb ? ", and mode 3's copies the DMA engine does not take (FB=0)" : "", d[4] / p, d[5] / 1024.0 / p, d[6] / p, d[7] / p);
    }
#endif
    if (!g_streamSceneFlush || !pictures) return;
    vglGlbStats s;
    vglGlbGetStats(&s);
    if (s.version < 5) return;
    static uint32_t prevFl = 0, prevScene = 0, prevBytes = 0, prevOver = 0;
    const double p = static_cast<double>(pictures);
    std::fprintf(stderr, "[streamc] per picture over %u pictures: write-back calls %.1f (at a scene submit %.1f), written back %.1f KiB, arena peak last picture %u KiB (overall %u KiB), spills into the uncached pool %u\n", pictures,
                 (s.stream_flushes - prevFl) / p, (s.stream_scene_flushes - prevScene) / p, (s.stream_flushed_bytes - prevBytes) / 1024.0 / p, s.stream_frame_peak >> 10, s.stream_peak >> 10, s.stream_overruns - prevOver);
    prevFl = s.stream_flushes; prevScene = s.stream_scene_flushes; prevBytes = s.stream_flushed_bytes; prevOver = s.stream_overruns;
}
extern "C" void vgx_vita_log_glb_stats(void) {
    vglGlbStats s;
    vglGlbGetStats(&s);
    {   // 1.3 (vram19): the patcher's bufferMem in use against what was asked for (the figure is 0 in Vita3K, which does not model it)
        const unsigned kb = s.patcher_buffer >> 10;
        ++g_patcherBufSamples;
        if (kb > g_patcherBufHigh) g_patcherBufHigh = kb;
        const unsigned want = ((g_patcherBufHigh * 2u + 511u) / 512u) * 512u;
        std::fprintf(stderr, "[vgx-vita] patcher buffer high-water %u KiB of %u KiB asked for (%u samples); suggested MSC_VGL_PATCHER_BUF_KB %u (2 x high-water, 512 KiB steps, at least 1024)\n",
                     g_patcherBufHigh, g_patcherBufKb, g_patcherBufSamples, want < 1024u ? 1024u : want);
        if (g_dispDepthNoMem || g_patcherBufKb) {
            uint32_t mode = 0, real = 0, made = 0, freed = 0, failed = 0;
            vglDisplayDepthStats(&mode, &real, &made, &freed, &failed);
            std::fprintf(stderr, "[vgx-vita] display depth: %s, memory now %s, made %u freed %u failed %u (dialog frames)\n", mode ? "nomem" : "real", real ? "yes" : "no", made, freed, failed);
        }
    }
    LogGxmStateCounters(s);
    if (s.version >= 4) {   // be26: the default uniform buffers' copies to the ring, per frame since the last line (MSC_VGX_VGL_UNIREUSE: 1 names the last copy again, 2 also any of the scene's last four)
        static uint32_t prev[6] = {}, prevFrame = 0;
        const uint32_t frames = s.frame > prevFrame ? s.frame - prevFrame : 1u;
        const uint32_t now[6] = {s.uni_copies, s.uni_copy_bytes, s.uni_last_hits, s.uni_last_bytes, s.uni_hist_hits, s.uni_hist_bytes};
        std::fprintf(stderr, "[vgx] vitaGL uniform buffers: per frame over %u frames, copied to the ring %.1f (%.1f KiB), last copy named again %.1f (%.1f KiB not copied), an older copy of the scene named %.1f (%.1f KiB not copied), reuses that differed from the buffer (verify only) %u\n", frames,
                     static_cast<double>(now[0] - prev[0]) / frames, static_cast<double>(now[1] - prev[1]) / 1024.0 / frames, static_cast<double>(now[2] - prev[2]) / frames, static_cast<double>(now[3] - prev[3]) / 1024.0 / frames,
                     static_cast<double>(now[4] - prev[4]) / frames, static_cast<double>(now[5] - prev[5]) / 1024.0 / frames, s.uni_bad);
        for (int i = 0; i < 6; ++i) prev[i] = now[i];
        prevFrame = s.frame;
    }
    if (g_texCopyMode && vglTextureCopyStats) {
        uint32_t t[6];
        vglTextureCopyStats(t);
        std::fprintf(stderr, "[texcopy] MSC_VITA_TEXCOPY=%d, since the start: %u texture copies by the DMA engine (%.2f MB in %.1f ms = %.0f MB/s), %u left to the CPU, %u differed (mode 2), %u refused\n", g_texCopyMode,
                     t[0], t[1] / 1048576.0, t[2] / 1000.0, t[2] ? static_cast<double>(t[1]) / t[2] : 0.0, t[3], t[4], t[5]);
    }
#ifdef VITAGL_LAZY_CLEAR
    if (s.version >= 3 && s.lazy_built) {
        static uint32_t prevClears = 0, prevRestores = 0, prevCont = 0, prevFrame = 0;
        const uint32_t frames = s.frame > prevFrame ? s.frame - prevFrame : 1u;
        std::fprintf(stderr, "[vgx] vitaGL lazy clear (%s): per frame over %u frames, glClear calls %.1f, state restores %.1f, clears that found the clear state still in place %.1f\n", s.lazy_on ? "on" : "off", frames,
                     static_cast<double>(s.lazy_clears - prevClears) / frames, static_cast<double>(s.lazy_restores - prevRestores) / frames, static_cast<double>(s.lazy_continued - prevCont) / frames);
        prevClears = s.lazy_clears; prevRestores = s.lazy_restores; prevCont = s.lazy_continued; prevFrame = s.frame;
    }
#endif
    std::fprintf(stderr,
                 "[vgx] vitaGL: pool/slice=%u KiB peak frame=%u KiB overall=%u KiB overruns=%u (%u KiB) odd-sized reservations=%u | stream arena(kind %u)/slice=%u KiB peak frame=%u KiB overall=%u KiB overruns=%u flushes=%u (%u KiB) | "
                 "ring %u MiB (cdram %u) peak frame=%u KiB overall=%u KiB wraps=%u | programs %u/%u shaders %u/%u | patcher host=%u KiB buffer=%u KiB vusse=%u KiB fusse=%u KiB | "
                 "skipped/calls vprog %u/%u fprog %u/%u stream %u/%u tex %u/%u\n",
                 s.pool_slice_bytes >> 10, s.pool_frame_peak >> 10, s.pool_peak >> 10, s.pool_overruns, s.pool_overrun_bytes >> 10, s.pool_odd, s.stream_mem, s.stream_slice_bytes >> 10,
                 s.stream_frame_peak >> 10, s.stream_peak >> 10, s.stream_overruns, s.stream_flushes, s.stream_flushed_bytes >> 10, s.ring_bytes >> 20, s.ring_cdram, s.ring_frame_peak >> 10,
                 s.ring_peak >> 10, s.ring_wraps, s.programs_live, s.programs_max, s.shaders_live, s.shaders_max, s.patcher_host >> 10, s.patcher_buffer >> 10,
                 s.patcher_vusse >> 10, s.patcher_fusse >> 10, s.skip_skipped[0], s.skip_calls[0], s.skip_skipped[1], s.skip_calls[1], s.skip_skipped[2], s.skip_calls[2],
                 s.skip_skipped[3], s.skip_calls[3]);
}
#endif

extern "C" {

#if !defined(VGX_NATIVE)
AuroraInfo aurora_initialize(int, char**, const AuroraConfig* config) {
    AuroraInfo info{};
    info.backend = BACKEND_OPENGLES;
    info.userPath = config ? config->userPath : nullptr;
    info.cachePath = config ? config->cachePath : nullptr;
    info.window = nullptr;
    DecideWidescreen(config ? config->windowWidth : 0u, config ? config->windowHeight : 0u);
    info.windowSize = Widescreen() ? AuroraWindowSize{960, 544, 960, 544, 960, 544, 1.0f} : AuroraWindowSize{640, 480, 640, 480, 640, 480, 1.0f};
    g_stretch.store(Widescreen());
    const bool ok = EnsureThread();
    if (!ok) {
        // Give the display back to the text console: the runtime reports this
        // as a fatal error there.
        VitaConsole::SetDisplayOwner(true);
        std::fprintf(stderr, "[vgx-vita] graphics start-up failed: %s\n", g_startError);
    }
    info.initializationStatus = ok ? AURORA_INITIALIZATION_SUCCESS : AURORA_INITIALIZATION_GRAPHICS_UNAVAILABLE;
    info.initializationError = ok ? nullptr : g_startError;
    rec::BackendFlags(false, false);   // no frame capture, no skipped draws
    return info;
}
void aurora_shutdown() {}
const AuroraEvent* aurora_update() {
    static AuroraEvent none{};
    none.type = AURORA_NONE;
    return &none;
}
bool aurora_begin_frame() { return true; }
void aurora_end_frame() {
    // Two-core split (workstream S): for a forked frame this runs on the split's worker
    // context, and the runtime has already done the game thread's part at the fork. What
    // belongs to the game thread is skipped there (vita/src/plat_split.h): the debug controls
    // read the pad (statics, and the game thread reads it at the same time), the frame
    // accounting drives the console, the harness's script and its exit. Presenting is right
    // on either thread: the recorder has one producer at a time, handed over at the fork and
    // back at the join under the runtime's mutex.
    const bool forked = PlatSplit::OnWorker();
    if (!forked) DebugControls();
    // Returns once the player has read everything this frame refers to in
    // guest memory; the render thread may still be drawing it.
    rec::Present(false);
    if (!forked) PlatFrameStats::OnFramePresented();
}
AuroraDisplayMode aurora_get_display_mode() { return AURORA_DISPLAY_MODE_WINDOWED; }
void aurora_set_frame_worker_wait_callback(AuroraFrameWorkerWaitCallback) {}
void aurora_wait_for_frame_worker() {}
void aurora_set_present_schedule(uint64_t, uint64_t) {}
void aurora_report_producer_paced(bool) {}
void aurora_set_preferred_refresh_rate(float) {}
void aurora_set_guest_write_hooks(AuroraGuestWriteGenerationCallback generation, AuroraGuestWriteNotifyCallback) {
    SetGuestWriteGenerationHook(generation);
    rec::SetGuestMemoryProbe(generation);
}
void AuroraSetViewportPolicy(AuroraViewportPolicy) {}
#endif
// Where the picture is on the screen, for the touch panel's coordinates (from Codex's touch work; unused
// unless the runtime's touch code asks). 0: the picture is not on screen (text console or fatal screen).
int MscVitaTouchViewport(float* x, float* y, float* width, float* height, int* flipY) {
    if (!x || !y || !width || !height || !flipY ||
        g_showConsole.load(std::memory_order_relaxed) || VitaConsole::OwnsDisplay()) return 0;
    const auto display = vgxvita::GetDisplayRect(g_stretch.load(std::memory_order_relaxed));
    *x = static_cast<float>(display.x);
    *y = static_cast<float>(display.y);
    *width = static_cast<float>(display.width);
    *height = static_cast<float>(display.height);
    *flipY = g_flipY.load(std::memory_order_relaxed) ? 1 : 0;
    return 1;
}
#if !defined(VGX_NATIVE)
void AuroraGetSurfaceSize(u32* width, u32* height) {
    // The screen's own shape in full-screen mode (the runtime fits its 3D projections to it),
    // 4:3 otherwise: see DecideWidescreen().
    if (width) *width = Widescreen() ? 960 : 640;
    if (height) *height = Widescreen() ? 544 : 480;
}

// One controller on port 0: the Vita's own controls (vita/src/vita_input.cpp).
u32 PADRead(PADStatus* status) {
    if (!status) return 0;
    for (int i = 0; i < PAD_CHANMAX; ++i) {
        std::memset(&status[i], 0, sizeof(PADStatus));
        status[i].err = PAD_ERR_NO_CONTROLLER;
    }
    PlatInput::Fill(status[0]);
    status[0].err = PAD_ERR_NONE;
    return 0x80000000u;
}
s32 PADGetIndexForPort(u32) { return -1; }

#endif
#if defined(VGX_NATIVE)
// ---- the native library's C interface (native/vgx_native.h): what the game's GX glue (np/platform/gx) calls
int vgxn_init(const vgxn_config* config) {
    // The shape the game draws for: wider than 4:3 = the game's widescreen setting is on = full screen.
    DecideWidescreen(config ? static_cast<unsigned>(config->surface_width) : 0u, config ? static_cast<unsigned>(config->surface_height) : 0u);
    g_stretch.store(Widescreen());
    const bool ok = EnsureThread();
    if (!ok) {
        // Give the display back to the text console: the platform layer reports this as a fatal error there.
        VitaConsole::SetDisplayOwner(true);
        std::fprintf(stderr, "[vgx-vita] graphics start-up failed: %s\n", g_startError);
    }
    vgx::rec::BackendFlags(false, false);   // no frame capture, no skipped draws
    return ok ? 1 : 0;
}
void vgxn_shutdown(void) {}

// (1.3) Why vgxn_init returned 0: VGXN_START_* (vgx_native_vita.h), 0 when it did not fail. A code, not the text: the platform's fatal screen names it in the
// player's language (np_startup_check.cpp); the text stays in the log.
extern "C" int vgxn_vita_start_failure(void) { return g_startCause; }

int vgxn_vita_wait_idle(void) {
    // The experimental wall-clock presenter can redraw independently of game
    // frames. Refuse takeover in that mode rather than race its scanout.
    if (vgx::present::Enabled() || !vgx::rec::WaitPresented(2000)) return 0;
    return sceGxmDisplayQueueFinish() >= 0;
}
// MSC_VITA_DRAW_EVERY=N (env.txt; emulator / test runs): only every Nth presented frame is rasterised, the others are recorded and replayed with every draw call
// skipped (textures, shaders, state, EFB copies and the game's own work are all unchanged). In Vita3K the draws are what a frame costs (about 200 ms of a 240 ms
// frame in a match, software GL), so a whole match plays in minutes instead of an hour; NPV3 (np/docs/wave3-NPV3.md). Default 1 = every frame, as always.
unsigned DrawEvery() {
    static const unsigned n = [] {
        const char* e = std::getenv("MSC_VITA_DRAW_EVERY");
        const int v = e && *e ? std::atoi(e) : 1;
        if (v > 1) std::fprintf(stderr, "[vgx-vita] MSC_VITA_DRAW_EVERY=%d: only every %dth frame is drawn (test runs; the rest is recorded and replayed without draw calls)\n", v, v);
        return v > 1 ? static_cast<unsigned>(v) : 1u;
    }();
    return n;
}
void vgxn_end_frame(void) {
    DebugControls();
    const uint64_t latTag = NpLatTakeSendTag ? NpLatTakeSendTag() : 0;   // W5 RT: the input-delay record of this frame (np/latency.h)
    // Returns once the player has read everything this frame refers to in memory; the render thread may still be drawing it.
    if(NpDiagEnabled&&NpDiagEnabled()){
        if(NpDiagActive&&NpDiagActive())NpDiagEvent(NPD_SEND,0,latTag,0,0,0,0,0);
        vgx::rec::Present(false,latTag);
    }else{
        // int28 (Carbon C1): always pass latTag into g_buf.latencyTag (stamped via NpLatDecoded in FrontPresent); keep LatMapPut as fallback with atomic CurrentFrame().
        vgx::rec::Present(false,latTag);
        if (latTag) {
            const uint64_t f = vgx::CurrentFrame() - 1;
            LatMapPut(f, latTag);
        }
    }
    ++g_nativeFrames;
    if (const unsigned n = DrawEvery(); n > 1) vgx::rec::BackendFlags(false, (g_nativeFrames + 1) % n != 0);   // the flags of the frame that follows
}
unsigned long long vgxn_frame_count(void) { return g_nativeFrames; }
int vgxn_request_dump(const char*) { return 0; }   // (MSC_VITA_SHOT in env.txt saves pictures on the console)
const unsigned char* vgxn_last_picture(int* width, int* height) {
    if (width) *width = 0;
    if (height) *height = 0;
    return nullptr;
}
void vgxn_surface_size(unsigned int* width, unsigned int* height) {
    if (width) *width = Widescreen() ? 960 : 640;
    if (height) *height = Widescreen() ? 544 : 480;
}
#else
#endif // VGX_NATIVE

// 1.3 MSC_VGX_ASYNC_SHADERS (vgx_backend_gl.cpp starts the shader compile worker; it calls this on itself first): cores 1-2 at the lowest user
// priority, so it only takes time the frame does not use. MSC_VITA_CORE3_SHADERS is refused since r16 (NpVitaCore3Mask answers 0, the worker never goes to
// the fourth core: Galaxy's console powered off with it there).
int NpVitaCore3Mask(void) __attribute__((weak));   // (native/platform/vita/src/vita_threads.cpp: the system core's mask once proven, else 0)
extern unsigned int _newlib_heap_size_user;        // (native/platform/vita/src/np_vita_process.cpp: the C heap's size)
// 1.3 (hardware audit): the worker compiles only while the C heap has this much free (see CompileToCache in vgx_backend_gl.cpp). The compiler's memory is not seen in the
// emulator (no real libshacccg there), the heap reached 84 of 96 MiB in it, and vglMalloc's fallbacks are not thread-safe. MSC_VGX_ASYNC_SHADERS_HEAP_MIB (default 8; 0 = no check).
int VgxHostAsyncHeapRoom(void) {
    static const long needMib = [] { const char* e = std::getenv("MSC_VGX_ASYNC_SHADERS_HEAP_MIB"); return (e && *e) ? std::atol(e) : 8L; }();
    if (needMib <= 0) return 1;
    const struct mallinfo mi = mallinfo();
    const unsigned long total = _newlib_heap_size_user, used = static_cast<unsigned long>(mi.uordblks);
    return total > used && (total - used) >= static_cast<unsigned long>(needMib) * 1048576ul ? 1 : 0;
}
// 1.3 wave 14 MSC_VGX_MIDFLUSH (vgx_midflush.h): sceGxmMidSceneFlush on vitaGL's context. The default uniform buffers are kept; what vitaGL's GXM state caches believe the context
// holds (programs, vertex streams, fragment textures, the GL state cache of the draw path) is forgotten, so the next draw gives it again. A texture upload's transfer is waited for first
// (the flushed part of the scene may sample a texture just uploaded). Layouts copied from vitaGL utils/gxm_cache.h (same SCE_GXM_ limits). A library without the symbols fails the call
// (-1) and the back end switches the feature off.
extern "C" SceGxmContext* gxm_context __attribute__((weak));
extern "C" {
struct VgxGxcMirror {
    const SceGxmVertexProgram* vprog;
    const SceGxmFragmentProgram* fprog;
    const void* vstream[SCE_GXM_MAX_VERTEX_STREAMS];
    const SceGxmVertexProgram* vstream_prog[SCE_GXM_MAX_VERTEX_STREAMS];
    uint32_t ftex[SCE_GXM_MAX_TEXTURE_UNITS][4];
    uint32_t ftex_valid;
};
extern VgxGxcMirror vgl_gxc __attribute__((weak));
extern uint32_t vgl_gxs_valid __attribute__((weak));   // (only a library built with GXM_STATE_CACHE)
extern void vglFlushStreamArena(void) __attribute__((weak));
int VgxHostMidSceneFlush(void) {
    if (&gxm_context == nullptr || gxm_context == nullptr || &vgl_gxc == nullptr) return -1;
    if (UploadSync()) sceGxmTransferFinish();
    if (&vglFlushStreamArena != nullptr) vglFlushStreamArena();   // 0060/int29: flush cached stream arena before kicking TA mid-scene
    const int rc = sceGxmMidSceneFlush(gxm_context, SCE_GXM_MIDSCENE_PRESERVE_DEFAULT_UNIFORM_BUFFERS, nullptr, nullptr);
    if (rc < 0) return rc;
    std::memset(&vgl_gxc, 0, sizeof vgl_gxc);
    if (&vgl_gxs_valid != nullptr) vgl_gxs_valid = 0;
    return 0;
}
}

// 1.3 wave 14 MSC_VGX_ASYNC_HEAPONLY=0|1 (Galaxy asyncsh31; Vita plan 1, code 0): the shader worker's vglMalloc / vglCalloc / vglMemalign / vglRealloc calls (the translator,
// vitaShaRK and libshacccg allocate through them) take newlib's heap only and never vitaGL's pools. vglMalloc tries newlib first and then the RAM / PHYCONT / budget / VRAM pools,
// which the render thread uses for textures, buffers and programs without a lock: a worker compile that reached them while the render thread allocated from the same pool
// corrupted it. MSC_VGX_ASYNC_SHADERS_HEAP_MIB's room check (above) is a snapshot and cannot close that. A block newlib cannot give is NULL: the compile fails, the job is marked
// failed and the draw takes the render thread's own compile, as it did before the worker existed. Every other thread gets the original call (the wrappers below; -Wl,--wrap in
// native/platform/vita/vita.mk). The worker's id is published here, by the worker itself, before its first job.
volatile int vgxvita_asyncWorkerTid = -1;
namespace vgx_wa {
vgx::workeralloc::Counts g_counts;
}
void VgxHostAsyncWorkerAllocStats(uint32_t* heapBlocks, uint32_t* refused, uint32_t* foreignRealloc) {
    if (heapBlocks) *heapBlocks = vgx_wa::g_counts.heapBlocks.load(std::memory_order_relaxed);
    if (refused) *refused = vgx_wa::g_counts.refused.load(std::memory_order_relaxed);
    if (foreignRealloc) *foreignRealloc = vgx_wa::g_counts.foreignRealloc.load(std::memory_order_relaxed);
}
int VgxHostAsyncWorkerPlace(int core3, char* desc, int size) {
    constexpr int kCores12 = 0x00060000, kLowestUser = 0xBF;
    int mask = kCores12;
    const char* why = "";
    {
        const char* e = std::getenv("MSC_VGX_ASYNC_HEAPONLY");
        if (e && *e && std::atoi(e) != 0) vgxvita_asyncWorkerTid = sceKernelGetThreadId();   // (the wrappers below read it; -1 = every thread on the originals)
    }
    if (core3) {
        const int m = NpVitaCore3Mask ? NpVitaCore3Mask() : 0;
        if (m) mask = m;
        else why = " (MSC_VITA_CORE3_SHADERS is refused: the worker stays on cores 1-2)";
    }
    const SceUID self = sceKernelGetThreadId();
    int rc = sceKernelChangeThreadCpuAffinityMask(self, mask);
    if (rc < 0 && mask != kCores12) {
        why = " (the fourth core was refused)";
        mask = kCores12;
        rc = sceKernelChangeThreadCpuAffinityMask(self, mask);
    }
    const int prc = sceKernelChangeThreadPriority(self, kLowestUser);
    if (desc && size > 0)
        std::snprintf(desc, static_cast<size_t>(size), "%s at the lowest priority (0x%X%s)%s%s", mask == kCores12 ? "on cores 1-2" : "on the fourth core", kLowestUser,
                      prc < 0 ? ", refused" : "", rc < 0 ? " (core mask refused)" : "", why);
    return rc < 0 ? rc : 0;
}
} // extern "C"

// ---- MSC_VGX_ASYNC_HEAPONLY: vitaGL's four allocation entry points, wrapped at the link (vita.mk: -Wl,--wrap=vglMalloc,vglCalloc,vglMemalign,vglRealloc). The wrap reaches every
// caller outside vgl.o: the translator, custom_shaders.o and vitaShaRK / libshacccg through gxm.o's shark_set_allocators(vglMalloc, vglFree); vgl.o's own vglRealloc -> vglMalloc
// fallback is not seen, which is why vglRealloc itself is wrapped. vgx_worker_alloc.h has the rule (tests/run_worker_alloc_test.sh proves it with fake allocators).
extern "C" {
void* __real_vglMalloc(uint32_t size);
void* __real_vglCalloc(uint32_t nmember, uint32_t size);
void* __real_vglMemalign(uint32_t alignment, uint32_t size);
void* __real_vglRealloc(void* ptr, uint32_t size);
int vgl_mem_get_type_by_addr(void* addr) __attribute__((weak));   // vitaGL utils/mem_utils.c: 4 = VGL_MEM_EXTERNAL (newlib's heap)
}
namespace vgx_wa {
bool IsHeap(void* p) { return !vgl_mem_get_type_by_addr || vgl_mem_get_type_by_addr(p) == 4; }
void* HeapMemalign(size_t a, size_t s) { return memalign(a, s); }
// (constant-initialised: no static constructor, so a vitaGL allocation made before this unit's initialisers ran finds it)
constexpr vgx::workeralloc::Ops kOps = {&malloc, &calloc, &HeapMemalign, &realloc, &__real_vglMalloc, &__real_vglCalloc, &__real_vglMemalign, &__real_vglRealloc, &IsHeap};
inline bool OnWorker() {   // one load and a compare while no worker is marked; sceKernelGetThreadId only then
    const int tid = vgxvita_asyncWorkerTid;
    return tid >= 0 && sceKernelGetThreadId() == tid;
}
}  // namespace vgx_wa
extern "C" void* __wrap_vglMalloc(uint32_t size) { return vgx::workeralloc::Malloc(vgx_wa::OnWorker(), size, vgx_wa::kOps, vgx_wa::g_counts); }
extern "C" void* __wrap_vglCalloc(uint32_t n, uint32_t size) { return vgx::workeralloc::Calloc(vgx_wa::OnWorker(), n, size, vgx_wa::kOps, vgx_wa::g_counts); }
extern "C" void* __wrap_vglMemalign(uint32_t alignment, uint32_t size) { return vgx::workeralloc::Memalign(vgx_wa::OnWorker(), alignment, size, vgx_wa::kOps, vgx_wa::g_counts); }
extern "C" void* __wrap_vglRealloc(void* ptr, uint32_t size) { return vgx::workeralloc::Realloc(vgx_wa::OnWorker(), ptr, size, vgx_wa::kOps, vgx_wa::g_counts); }

#endif // __vita__
