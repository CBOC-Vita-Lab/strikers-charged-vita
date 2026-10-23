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
//   MSC_VITA_SHARPEN=0.5     the unsharp mask's amount (smooth 0.5, sharp+ 0.35; 0.2 to 0.8). Unset,
//                            smooth uses 0.7 / 0.8 in Mega Strike scenes (drawn smaller)
//   MSC_VITA_MSAA=0|2|4      multisampling of every render target (vglInit's last argument; the
//                            depth buffers grow 2x / 4x, and with STORE_DEPTH_STENCIL the tile
//                            load / store traffic with them): see StartGl
//   MSC_VITA_PATCHER_MIB=8   shader patcher pools: this many MiB of host memory and half of it for each of vertex
//                            and fragment code (default 8 / 4 / 4; see StartGl)
//   MSC_VGX_MAXPROGRAMS=900  (back end, src/vgx_backend_gl.cpp) linked programs above which a frame start deletes the
//                            ones longest without a draw (none drawn in the last 600 frames), a hundred at a time, and
//                            the warm-up stops linking new ones; vitaGL's table has 1,024 slots. 0 = no limit
#if defined(__vita__)

#define VGX_VITA_NO_GL_ROUTING
#include <GLES2/gl2.h>

#if defined(VGX_NATIVE)
// The native library (np branch, native/native-vita.mk): no recompilation runtime. The services the host takes from the
// platform layer (console, thread cores, debug pad, boot log) come from native/vita_shim, whose headers carry the
// runtime's names; vgxn_* below replaces the aurora_* entry points.
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
#include "vgx_rec.h"
#include "vgx_shadergen.h"
#include "vgx_state.h"
#include "vita_platform.h"
#include "vgx_display_rect.h"
#include "vgx_listmerge.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

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

// Multiplayer (platform layer, vita_multiplayer.cpp): the system NetCheck dialog of the ad hoc connection is
// drawn by vitaGL's swap, which must be told that a CommonDialog is on screen. Weak: absent in an older platform
// snapshot (then every swap is a plain one).
extern "C" {
void np_vita_radio_dialog_render(void) __attribute__((weak));
bool np_vita_radio_dialog_active(void) __attribute__((weak));
void np_vita_radio_set_dialog_ready(bool) __attribute__((weak));
int np_vita_radio_dialog_prepare(bool) __attribute__((weak));
// (W5 FPS, platform/compat/np_fps.cpp) how many vblanks a picture is shown for: 2 at MSC_FPS=30, 1 at 60, 0 = no gate (unlocked, the build's own pacing).
// Weak: absent in an older platform snapshot (no gate).
int NpFpsDisplayInterval(void) __attribute__((weak));
}

#ifdef VITAGL_GLB
// kubridge.skprx (taiHEN plug-in), optional: the port links its WEAK stubs, so the address is null without the plug-in.
extern "C" void kuKernelFlushCaches(const void* ptr, SceSize len) __attribute__((weak));
// W5 SEED: the read-through patch of vitaGL (np tools/seed/vitagl/0002-seed-readthrough.patch). A vitaGL without it does not have this function (weak: null).
extern "C" void vglSetShaderSeedPath(const char* path) __attribute__((weak));
#endif
// W5 BOOT: the in-memory shader pack (np platform/vita/vitagl/0004-boot-shader-pack.patch). Weak: null without the patch (any vitaGL build).
extern "C" void vglSetShaderPackLookup(const void* (*fn)(int fragment, uint64_t hash, uint32_t* size)) __attribute__((weak));
extern "C" unsigned vglStrikersShaderCacheCaps(void) __attribute__((weak));
// W5 RT: the input-delay record (np include/np/latency.h, platform/compat/np_latency.cpp) and the VI thread's last vblank (np np_plat_vita.cpp)
extern "C" uint64_t NpLatTakeSendTag(void) __attribute__((weak));
#include "../src/vgx_perf_diag.h"

extern "C" float VgxRenderScale(void); // vgx_backend_gl.cpp (DYNRES): the scene's render scale in force
extern "C" void NpLatDecoded(uint64_t tag) __attribute__((weak));
extern "C" void NpLatPresented(uint64_t tag) __attribute__((weak));
extern "C" void NpLatShown(uint64_t tag, uint64_t shownNs, int handedToo) __attribute__((weak));
extern "C" int np_display_last_vblank(uint32_t* vcount, uint64_t* ns) __attribute__((weak));

namespace {

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

// ---- present state. The flags are set by the game thread (debug controls).
std::atomic<bool> g_flipY{false}, g_stretch{false}, g_showConsole{false};
int g_upscaleMode = 4;       // vgx::backend::PictureMode; 0 = glBlitFramebuffer, 4 = PIC_SMOOTH (default)
bool g_sharpenSet = false;   // MSC_VITA_SHARPEN given: Mega Strike scenes keep it too
float g_sharpen = 0.5f;      // MSC_VITA_SHARPEN (default for smooth; sharp+ was tuned at 0.35)
uint64_t g_presented = 0;       // render thread
unsigned long long g_nativeFrames = 0;   // vgxn_end_frame calls (native build; unused otherwise)
uint64_t g_lastPresentUs = 0;
vgx::backend::Stats g_lastStats;

uint64_t NowUs() { return sceKernelGetProcessTimeWide(); }

bool Exists(const char* path) {
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
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
// The renderer's frame number -> input-delay tag (written by the thread that sends the frame, read by the back end at its display copy).
struct LatMap { uint64_t frame = 0, tag = 0; };
LatMap g_latMap[256];
uint64_t LatTagOf(uint64_t frame) {
    const LatMap& m = g_latMap[frame % 256];
    return m.frame == frame ? m.tag : 0;
}

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

void GateDisplay() {
    uint64_t diagTag=0;
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
    if (interval > 0 && g.enabled && g.have) {
        const uint32_t due = g.lastShown + static_cast<uint32_t>(interval);
        int guard = 0;
        if (static_cast<int32_t>(due - shown) > 0) ++g.waited;
        while (static_cast<int32_t>(due - shown) > 0 && guard++ < 8) {
            if (sceDisplayWaitVblankStart() < 0) break;
            shown = static_cast<uint32_t>(sceDisplayGetVcount()) + 1u;
        }
        if (static_cast<int32_t>(shown - due) > 0) ++g.late;
    }
    if (g.have) {
        const uint32_t held = shown - g.lastShown;
        const int bin = held == 0 ? 0 : held >= 4 ? 4 : static_cast<int>(held);
        if (held < 1000) {   // (a stall of seconds is not a pace)
            ++g.total; ++g.held[bin]; ++g.winTotal; ++g.winHeld[bin];
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

// ux0:data/mscharged/shadercache/<8 hex digits>: the folder vitaGL keeps compiled shaders in.
// A package whose key differs starts with an empty folder (the old one is left behind; it is
// some hundred small files and can be deleted by hand).
const char* ShaderCacheFolder() {
    static char path[160] = "";
    if (path[0]) return path;
    char id[512];
    std::snprintf(id, sizeof(id), "%s|%s|gen %u", MSC_VGL_ID, kHostShaderAbi, static_cast<unsigned>(vgx::kShaderGenVersion));
    uint32_t h = 2166136261u; // FNV-1a
    for (const char* c = id; *c; ++c) h = (h ^ static_cast<uint8_t>(*c)) * 16777619u;
    std::snprintf(path, sizeof(path), "%s/%08x", kShaderCacheDir, static_cast<unsigned>(h));
    std::fprintf(stderr, "[vgx-vita] compiled-shader cache: %s (key of: %s)\n", path, id);
    std::fprintf(stderr, "[vgx-vita] shader cache buckets: %s\n", CacheWriterOwnsBuckets()
        ? "library-managed (host bucket counters inactive)" : "legacy host workaround");
    return path;
}

// Every swap of the display goes through here: a pending dialog operation is processed before the swap, and the
// swap is told while the dialog is shown.
// tag: the input-delay record of the frame being swapped (0: a picture that is not a game frame: start-up, progress bar)
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
    vglSwapBuffers(np_vita_radio_dialog_active && np_vita_radio_dialog_active() ? GL_TRUE : GL_FALSE);
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
// The folder name vitaGL cdbba42 puts under the cache path ("v<SHADER_CACHE_MAGIC>", source/
// shared.h). Only used to make <cache>/v2/v/00 before vitaGL starts, which stops it creating
// all 512 bucket folders; with another vitaGL the only effect of a wrong value is that it
// creates them on the first start, as before round 6.
constexpr const char* kVglCacheVersionDir = "v2";

// What the first console run showed (hw/run2-gl): "123 files, 133 KiB, 0 failed, 55272 ms" for
// the seed copy, i.e. 55 s of black screen on the first start of a package. The copy made, on
// the memory card: 123 small files, their ~110 bucket folders, and then ALL 512 bucket folders
// (EnsureBuckets, because vitaGL only creates them when <cache>/v2/v/00 does not exist yet),
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
    if (mayExist && Exists(to)) { ++st.kept; return true; }
    static char buffer[64 * 1024];
    const SceUID in = sceIoOpen(from, SCE_O_RDONLY, 0);
    if (in < 0) return false;
    const SceUID out = sceIoOpen(to, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
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
    if (!ok) sceIoRemove(to); // never leave half a program behind: vitaGL would load it
    return ok;
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
        if (std::snprintf(path, sizeof(path), "%s/%s%s", folder, kVglCacheVersionDir, part) >= static_cast<int>(sizeof(path))) return;
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
const void* PackLookup(int fragment, uint64_t hash, uint32_t* size) {
    const PackEntry* e = PackFind(fragment, hash);
    if (!e) return nullptr;
    if (size) *size = e->size;
    g_packHits.fetch_add(1, std::memory_order_relaxed);
    return g_pack + e->offset;
}
bool LoadPack(const char* path, unsigned* ms) {
    const uint64_t t0 = NowUs();
    const SceUID f = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (f < 0) return false;
    const SceOff size = sceIoLseek(f, 0, SCE_SEEK_END);
    sceIoLseek(f, 0, SCE_SEEK_SET);
    if (size < 16 || size > 64 * 1024 * 1024) { sceIoClose(f); return false; }
    uint8_t* data = static_cast<uint8_t*>(std::malloc(static_cast<size_t>(size)));
    if (!data) { sceIoClose(f); return false; }
    size_t got = 0;
    while (got < static_cast<size_t>(size)) {
        const size_t want = static_cast<size_t>(size) - got > 512u * 1024u ? 512u * 1024u : static_cast<size_t>(size) - got;
        const int n = sceIoRead(f, data + got, static_cast<SceSize>(want));
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

// Returns the folder vitaGL is to use as its cache.
const char* SeedShaderCache(const char* folder) {
    const char* key = std::strrchr(folder, '/');
    static char seed[200];
    char marker[200];
    std::snprintf(seed, sizeof(seed), "%s%s", kSeedDir, key ? key : "/none");
    std::snprintf(marker, sizeof(marker), "%s/seeded", folder);
    SeedCopyStats st;
    const char* seedMode = std::getenv("MSC_VITA_SEED");
    if (!(seedMode && *seedMode && std::strcmp(seedMode, "pack") != 0)) {
        char packPath[200];
        std::snprintf(packPath, sizeof(packPath), "%s%s.pack", kSeedDir, key ? key : "/none");
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
                        std::snprintf(dir, sizeof(dir), "%s/%s/%s/%02X", folder, kVglCacheVersionDir, e.kind ? "f" : "v", static_cast<unsigned>(e.hash >> 56));
                        std::snprintf(path, sizeof(path), "%s/%llX.gxp", dir, static_cast<unsigned long long>(e.hash));
                        if (Exists(path)) continue;
                        sceIoMkdir(dir, 0777);
                        const SceUID f = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
                        const bool ok = f >= 0 && sceIoWrite(f, g_pack + e.offset, e.size) == static_cast<int>(e.size);
                        if (f >= 0) sceIoClose(f);
                        if (ok) ++written; else { ++failed; sceIoRemove(path); }
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
            std::snprintf(seedPath, sizeof(seedPath), "%s/%s", seed, kVglCacheVersionDir);
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
    const uint64_t t0 = NowUs();
    std::string pk;
    if (!ReadWhole(seed, pk)) { std::fprintf(stderr, "[vgx-vita] warm-up list from the package: cannot read %s\n", seed); return; }
    std::string out = pk;
    unsigned added = 0;
    const char* how = "copied";
    if (have) {
        std::string con;
        const vgx_listmerge::List P = vgx_listmerge::Parse(reinterpret_cast<const uint8_t*>(pk.data()), pk.size());
        const vgx_listmerge::List C = ReadWhole(kShaderListFile, con) ? vgx_listmerge::Parse(reinterpret_cast<const uint8_t*>(con.data()), con.size()) : vgx_listmerge::List();
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
    std::fprintf(stderr, "[vgx-vita] warm-up list from the package (%u bytes): %s%s, %u records of this console's list added, %u bytes now, %u ms\n",
                 static_cast<unsigned>(pk.size()), ok ? how : "WRITE FAILED", ok ? "" : "", added, static_cast<unsigned>(out.size()), static_cast<unsigned>((NowUs() - t0) / 1000u));
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
        if (std::strstr(info.name, "Garbage Collector")) target = VitaThreads::CoreMask(VitaThreads::kCollector);
        else if (!std::strcmp(info.name, "SceGxmDisplayQueue")) target = VitaThreads::CoreMask(VitaThreads::kDisplayQueue);
        if (target && info.currentCpuAffinityMask != static_cast<SceUInt32>(target)) {
            const int res = sceKernelChangeThreadCpuAffinityMask(uid, target);
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
    int64_t cdramReserveMib = 18;
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
    vglSetupRenderTargetScenesNum(1, static_cast<uint8_t>(fboScenes));
    std::fprintf(stderr, "[vgx-vita] render targets: %d scene(s) per frame for framebuffer objects (MSC_VITA_FBO_SCENES in env.txt changes it)\n", fboScenes);

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
    // default is 8 / 4 / 4 MiB (+8 MiB of main memory, which the 96 MiB vitaGL pool limit leaves
    // room for: about 85 MiB unassigned). The 27 + 48 "patching failed" lines of the Vita3K compile
    // run of the whole catalogue were NOT pool exhaustion (all SCE_GXM_ERROR_INVALID_POINTER on
    // shader 0x0: the 30 programs past vitaGL's 1,024-slot table); Vita3K does not model the pools.
    unsigned patcherMib = 8;
    if (const char* e = std::getenv("MSC_VITA_PATCHER_MIB")) patcherMib = static_cast<unsigned>(std::atoi(e));
    if (patcherMib < 1) patcherMib = 1;
    if (patcherMib > 16) patcherMib = 16;
    vglSetupShaderPatcher(patcherMib * 1024u * 1024u, (patcherMib + 1) / 2 * 1024u * 1024u, (patcherMib + 1) / 2 * 1024u * 1024u);
    vglSetupGarbageCollector(kGcPriority, VitaThreads::CoreMask(VitaThreads::kCollector));
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
    {
        int streamKib = 0;
        if (const char* e = std::getenv("MSC_VITA_VGL_STREAM_KIB"); e && *e) streamKib = std::atoi(e);
        if (streamKib < 0) streamKib = 0;
        if (streamKib > 16384) streamKib = 16384;
        int memKind = VGL_STREAM_CDRAM;
        const char* memName = "cdram";
        if (const char* e = std::getenv("MSC_VITA_VGL_STREAM_MEM"); e && *e) {
            if (std::strcmp(e, "cached") == 0) { memKind = VGL_STREAM_CACHED; memName = "cached"; }
            else if (std::strcmp(e, "uncached") == 0) { memKind = VGL_STREAM_UNCACHED; memName = "uncached"; }
        }
        const char* mode = kuKernelFlushCaches ? "kubridge" : "syncvm";
        if (const char* e = std::getenv("MSC_VITA_VGL_FLUSH"); e && *e) mode = e;
        vglCacheFlushFn fn = nullptr; // vitaGL's own: sceKernelSyncVMDomain on the arena
        if (std::strcmp(mode, "none") == 0) fn = [](void*, uint32_t) {};
        else if (std::strcmp(mode, "kubridge") == 0 && kuKernelFlushCaches) fn = [](void* a, uint32_t n) { kuKernelFlushCaches(a, n); };
        else mode = "syncvm";
        vglSetupStreamArena(static_cast<uint32_t>(streamKib) * 1024u, memKind, fn);
        int ringCdram = 0;
        if (const char* e = std::getenv("MSC_VITA_VGL_RING_MEM"); e && *e) ringCdram = std::strcmp(e, "cdram") == 0;
        vglSetUniformRingMem(ringCdram);
        int skip = 0;
        if (const char* e = std::getenv("MSC_VITA_VGL_SKIP"); e && *e) skip = std::atoi(e) != 0;
        vglSetSkipRedundantSets(skip ? GL_TRUE : GL_FALSE);
        std::fprintf(stderr, "[vgx-vita] vitaGL GLB: stream arena %d KiB per display buffer (MSC_VITA_VGL_STREAM_KIB; 0 = off), memory %s (MSC_VITA_VGL_STREAM_MEM), write-back %s "
                     "(MSC_VITA_VGL_FLUSH; cached only), uniform ring %s (MSC_VITA_VGL_RING_MEM), skip redundant sceGxm sets %s (MSC_VITA_VGL_SKIP)\n", streamKib, memName,
                     memKind == VGL_STREAM_CACHED ? mode : "-", ringCdram ? "cdram" : "pool memory", skip ? "on" : "off");
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
    if (const char* e = std::getenv("MSC_VITA_MSAA"); e && *e) {
        const int n = std::atoi(e);
        if (n == 2) msaaMode = SCE_GXM_MULTISAMPLE_2X;
        else if (n == 4) msaaMode = SCE_GXM_MULTISAMPLE_4X;
        else if (n != 0) std::fprintf(stderr, "[vgx-vita] MSC_VITA_MSAA=%s ignored (0, 2 or 4)\n", e);
        std::fprintf(stderr, "[vgx-vita] MSC_VITA_MSAA: %dx multisampling\n", n == 2 ? 2 : (n == 4 ? 4 : 0));
    }
    if (const char* e = std::getenv("MSC_VITA_UPSCALE"); e && *e) {
        const int mode = vgx::backend::PictureModeFromString(e);
        if (mode < 0) std::fprintf(stderr, "[vgx-vita] MSC_VITA_UPSCALE=%s ignored (off, linear, sharp, sharp+, smooth)\n", e);
        else g_upscaleMode = mode;
        if (mode == 3) g_sharpen = 0.35f;
    }
    if (const char* e = std::getenv("MSC_VITA_SHARPEN"); e && *e) { g_sharpen = static_cast<float>(std::atof(e)); g_sharpenSet = true; }
    if (g_sharpen < 0.0f) g_sharpen = 0.0f;
    if (g_sharpen > 1.5f) g_sharpen = 1.5f;
    std::fprintf(stderr, "[vgx-vita] picture to the screen: %s%s\n",
                 g_upscaleMode == 0 ? "glBlitFramebuffer (MSC_VITA_UPSCALE=off)" : (g_upscaleMode == 1 ? "linear, through the picture shader" : (g_upscaleMode == 2 ? "sharp bilinear" : (g_upscaleMode == 3 ? "sharp bilinear + unsharp mask" : "bilinear + unsharp mask (smooth)"))),
                 g_upscaleMode >= 3 ? (" amount " + std::to_string(g_sharpen)).c_str() : "");
    // MSC_VITA_DISPLAY_BUFFERS=2 (23 Oct, console A/B only): double instead of vitaGL's triple buffering. One frame fewer can
    // wait in the display queue, so a GPU-bound scene shows each frame about one frame time sooner (input feels quicker), at the
    // risk of a lower frame rate when frame times hover near the 33 ms budget. Unset or 3: vitaGL's default (3).
    if (const char* e = std::getenv("MSC_VITA_DISPLAY_BUFFERS"); e && (e[0] == '2' || e[0] == '3') && e[1] == '\0') {
        vglSetDisplayBufferCount(e[0] - '0');
        std::fprintf(stderr, "[vgx-vita] MSC_VITA_DISPLAY_BUFFERS: %c display buffers\n", e[0]);
    }
    const GLboolean fellBack = vglInitWithCustomSizes(0, kScreenW, kScreenH, static_cast<int>(ram), static_cast<int>(cdram),
                                                      static_cast<int>(phycont), 0, msaaMode);
    if (fellBack)
        std::fprintf(stderr, "[vgx-vita] vitaGL could not set %dx%d and fell back to the display's largest mode; the picture's placement will be off\n",
                     kScreenW, kScreenH);
    if (vglMemTotal(VGL_MEM_VRAM) == 0 && vglMemTotal(VGL_MEM_RAM) == 0) {
        std::snprintf(g_startError, sizeof(g_startError), "vitaGL started without any memory pool (see the [vitaGL] lines in log.txt).");
        return false;
    }
    LogMemory("after vitaGL");
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
            std::snprintf(g_startError, sizeof(g_startError),
                          "vitaGL could not allocate its video memory pool (%u KiB asked, %u KiB of video memory still free).\n"
                          "Set MSC_VITA_CDRAM_RESERVE_MIB in env.txt to a larger value than %d.",
                          static_cast<unsigned>(cdram / 1024), static_cast<unsigned>(after.size_cdram / 1024),
                          static_cast<int>(cdramReserveMib));
            return false;
        }
    }
    LogPools("after start-up");
    std::fprintf(stderr, "[vgx-vita] GL: %s / %s / %s\n", reinterpret_cast<const char*>(glGetString(GL_VENDOR)),
                 reinterpret_cast<const char*>(glGetString(GL_RENDERER)), reinterpret_cast<const char*>(glGetString(GL_VERSION)));

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
    if (!vgx::backend::Init(renderW, renderH)) {
        std::snprintf(g_startError, sizeof(g_startError), "the vgx GL back end failed to initialise");
        return false;
    }
#ifndef MSC_VGX_COMMIT
#define MSC_VGX_COMMIT "unknown"
#endif
    vgx::backend::SetPresentHook(PresentDisplayCopy);
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
bool ShotWanted(uint64_t present) {
    if (g_shotCount < 0) ShotInit();
    if (g_shotCount == 0 || g_shotsWritten >= kShotMaxFiles) return false;
    for (int i = 0; i < g_shotCount; ++i) {
        const ShotItem& it = g_shotItems[i];
        if (present >= it.first && present <= it.last && (present - it.first) % it.step == 0) return true;
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
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(h) * (w * 3 + 1));
    for (int y = 0; y < h; ++y) { raw.push_back(0); raw.insert(raw.end(), rgb + static_cast<size_t>(y) * w * 3, rgb + static_cast<size_t>(y + 1) * w * 3); }
    std::vector<uint8_t> z;
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
    static std::vector<uint8_t> px, rgb, png;
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
uint64_t g_rtPrevDraw[4] = {};
bool g_rtHavePrev = false;
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
    vglRtSetup(flags, static_cast<int>(EnvL("MSC_VITA_RT_PRIO", 0x10000100 - 16)), affinity);
    g_rtDisplayHint = hints > 0;
    if (hints > 0) vgx::backend::SetSceneHintHook([](unsigned fb, unsigned h) { vglRtSceneHint(fb, h); });
    vgx::backend::SetMicroClock([]() -> uint32_t { return sceKernelGetProcessTimeLow(); });
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
            *ms = static_cast<float>(f.busy_us) * 0.001f;
            return true;
        });
    }
}
void LogRt() {
    if (g_rtLogEvery <= 0 || g_presented % static_cast<unsigned long long>(g_rtLogEvery) != 0) return;
    vglRtGetStats(&g_rtNow);
    uint64_t dt[4];
    vgx::backend::GetDrawTimers(dt);
    if (!g_rtHavePrev) {
        g_rtPrev = g_rtNow;
        for (int i = 0; i < 4; ++i) g_rtPrevDraw[i] = dt[i];
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
    for (uint32_t i = (frames > 128 ? b.cpu_frame_head - 128 : a.cpu_frame_head); i != b.cpu_frame_head; ++i) sw.push_back(b.cpu_frames[i % 128].switch_us);
    const uint32_t gf = b.gpu_frame_head - a.gpu_frame_head;
    for (uint32_t i = (gf > 128 ? b.gpu_frame_head - 128 : a.gpu_frame_head); i != b.gpu_frame_head; ++i) {
        const vglRtGpuFrame& g = b.gpu_frames[i % 128];
        if (!g.timed) continue;
        busy.push_back(g.busy_us);
        span.push_back(g.span_us);
        tail.push_back(g.tail_us);
    }
    uint32_t sw50, sw95, b50, b95, s50, s95, t50, t95;
    Pct(sw, sw50, sw95);
    Pct(busy, b50, b95);
    Pct(span, s50, s95);
    Pct(tail, t50, t95);
    const double mb = 1.0 / (1024.0 * 1024.0);
    const uint64_t firstN = dt[0] - g_rtPrevDraw[0], firstUs = dt[1] - g_rtPrevDraw[1], sampN = dt[2] - g_rtPrevDraw[2], sampUs = dt[3] - g_rtPrevDraw[3];
    std::fprintf(stderr,
                 "[rt] %u frames: scenes %.1f/frame (display %.1f), depth not loaded %.1f (by the whole-clear rule %.1f), not stored %.1f, hints %.1f given %.1f used; "
                 "depth MB/frame loaded %.2f stored %.2f, not loaded %.2f, not stored %.2f, mask-clear quads saved %.1f | CPU scene change %.3f ms/frame (end %.3f begin %.3f set-up %.3f; frame p50 %.3f p95 %.3f, max scene %.3f ms) | "
                 "draw that begins a scene %.1f us (x%.1f/frame), other draws %.1f us | GPU (%lu of the last frames timed) busy p50 %.2f p95 %.2f ms, first submit to last done p50 %.2f p95 %.2f ms, "
                 "last submit to done p50 %.2f p95 %.2f ms; scenes timed %u untimed %u already done when looked at %u, pending %u\n",
                 frames, (b.scenes - a.scenes) / f, (b.scenes_display - a.scenes_display) / f, (b.scenes_noload - a.scenes_noload) / f,
                 (b.scenes_noload_auto - a.scenes_noload_auto) / f, (b.scenes_nostore - a.scenes_nostore) / f, (b.hints_given - a.hints_given) / f,
                 (b.hints_used - a.hints_used) / f, (b.ds_loaded - a.ds_loaded) * mb / f, (b.ds_stored - a.ds_stored) * mb / f,
                 (b.ds_load_skipped - a.ds_load_skipped) * mb / f, (b.ds_store_skipped - a.ds_store_skipped) * mb / f,
                 (b.mask_invalidations_skipped - a.mask_invalidations_skipped) / f,
                 ((b.cpu_end_us - a.cpu_end_us) + (b.cpu_begin_us - a.cpu_begin_us) + (b.cpu_setup_us - a.cpu_setup_us)) / 1000.0 / f,
                 (b.cpu_end_us - a.cpu_end_us) / 1000.0 / f, (b.cpu_begin_us - a.cpu_begin_us) / 1000.0 / f, (b.cpu_setup_us - a.cpu_setup_us) / 1000.0 / f, sw50 / 1000.0,
                 sw95 / 1000.0, b.cpu_switch_max_us / 1000.0, firstN ? static_cast<double>(firstUs) / static_cast<double>(firstN) : 0.0, firstN / f,
                 sampN ? static_cast<double>(sampUs) / static_cast<double>(sampN) : 0.0, static_cast<unsigned long>(busy.size()), b50 / 1000.0, b95 / 1000.0, s50 / 1000.0, s95 / 1000.0, t50 / 1000.0,
                 t95 / 1000.0, b.gpu_scenes_timed - a.gpu_scenes_timed, b.gpu_scenes_untimed - a.gpu_scenes_untimed, b.gpu_scenes_late - a.gpu_scenes_late, b.gpu_pending);
    g_rtPrev = g_rtNow;
    for (int i = 0; i < 4; ++i) g_rtPrevDraw[i] = dt[i];
}
#endif

void PresentDisplayCopy(unsigned efb, int x, int y, int w, int h) {
    ++g_presented;
    if (g_presented <= 1000) BootProgress(VGXN_BOOT_PRESENT, static_cast<unsigned>(g_presented), 0);
    if (g_presented <= 2)   // W5 BOOT: the start's timeline (present 1 is the first frame; present 2 follows its shader warm-up)
        std::fprintf(stderr, "[boot] renderer: present %u%s  [+%lu ms]\n", static_cast<unsigned>(g_presented), g_presented == 2 ? " (the warm-up of frame 1 is done)" : "", BootMs());
#ifdef VITAGL_RT
    LogRt();
#endif
    const uint64_t now = NowUs();
    const vgx::backend::Stats stats = vgx::backend::GetStats();
    if (g_presented == 1)
        std::fprintf(stderr, "[vgx-vita] first present: display copy %d,%d %dx%d from framebuffer object %u\n", x, y, w, h, efb);
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
            std::fprintf(stderr, "[vgx-vita] shader pack: %u of the shaders asked for came from the package's pack  [+%lu ms]\n", g_packHits.load(std::memory_order_relaxed), BootMs());
        {
            unsigned made = 0, ms = 0;
            vgxvita_cacheFolderStats(&made, &ms);
            if (made != 0) std::fprintf(stderr, "[vgx-vita] shader cache: %u bucket folders made as shaders needed them, %u ms in all\n", made, ms);
        }
        LogPools("now");
    }
    if (g_presented == 5 || g_presented == 30 || g_presented == 120 || g_presented % 600 == 0) PinLibraryThreads(false);
    g_lastPresentUs = now;
    g_lastStats = stats;

    // (With the text console selected the frame is still drawn and swapped,
    // so vitaGL's per-frame bookkeeping goes on; DisplayCallback then puts the
    // console's picture over it.)
    // A fatal-error screen has taken the display back: leave it alone.
    if (VitaConsole::OwnsDisplay()) return;
    if (efb == 0 || w <= 0 || h <= 0) return;
    if (ShotWanted(g_presented)) TakeShot(g_presented, x, y, w, h);
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
        if (g_upscaleMode == 4 && !g_sharpenSet) {
            const float s = VgxRenderScale();
            if (s < 0.99f) amount = std::min(0.8f, g_sharpen + (1.0f - s) * 0.6f);
            else if (s > 1.01f) amount = 0.4f;
        }
        if (!vgx::backend::DrawPicture(0, x, y, w, h, dx0, 0, dx1 - dx0, kScreenH, g_upscaleMode, amount, !g_flipY.load(std::memory_order_relaxed))) {
            std::fprintf(stderr, "[vgx-vita] picture shader unavailable: MSC_VITA_UPSCALE off\n");
            g_upscaleMode = 0;
            glBlitFramebuffer(x, sy0, x + w, sy1, dx0, 0, dx1, kScreenH, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        }
    } else {
        glBlitFramebuffer(x, sy0, x + w, sy1, dx0, 0, dx1, kScreenH, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }
    const uint64_t latTag = NpDiagEnabled&&NpDiagEnabled()?vgx::backend::ExecutingLatencyTag():LatTagOf(vgx::backend::ExecutingFrame());
    if (latTag && NpLatPresented) NpLatPresented(latTag);
    SwapDisplay(latTag);
    // Back to what the back end had: it clears the EFB next.
    glBindFramebuffer(GL_FRAMEBUFFER, efb);
    if (g_scissorOn) { UnparkScissor(); glEnable(GL_SCISSOR_TEST); }
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

void* RenderThread(void*) {
    // The core plan is the platform layer's (vita/src/vita_threads.cpp): core 1 while the game
    // thread is the only heavy thread beside this one, core 2 (with audio) when the two-core
    // split's worker has core 1 (MSC_SPLIT=thread); MSC_VITA_CORE_RENDER in env.txt overrides.
    VitaThreads::PinCurrentThread(VitaThreads::kRender);
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
        std::fprintf(stderr, "[vgx-vita] shader warm-up list: %s (%s); texture budget: %s\n",
                     std::getenv("MSC_VGX_SHADERLIST") ? std::getenv("MSC_VGX_SHADERLIST") : kShaderListFile,
                     Exists(std::getenv("MSC_VGX_SHADERLIST") ? std::getenv("MSC_VGX_SHADERLIST") : kShaderListFile)
                         ? "found: all known shaders are compiled during the first frame" : "not there yet: shaders are compiled as the game needs them",
                     std::getenv("MSC_VGX_TEXBUDGET") ? "MSC_VGX_TEXBUDGET (MiB) from env.txt" : "32 MiB");
        vgx::rec::InitMode();
        vgx::rec::SetPlayerThreadInit(PinPlayerThread);
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
// vitaGL stores a compiled shader as <cache>/v2/{v,f}/<HH>/<hash>.gxp, hash = XXH3_64bits of
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
            std::snprintf(path, sizeof(path), "%s/%s/%02X/%llX.gxp", g_seedLinkPath, kind ? "f" : "v", static_cast<unsigned>(hash >> 56), static_cast<unsigned long long>(hash));
            packaged = Exists(path);
        }
        if (!packaged) EnsureBucket(kind, static_cast<unsigned>(hash >> 56));
    }
    glShaderSource(shader, count, string, length);
}
extern "C" void vgxvita_cacheFolderStats(unsigned* made, unsigned* ms) {
    if (made) *made = g_bucketsMade;
    if (ms) *ms = static_cast<unsigned>(g_bucketUs / 1000u);
}

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
extern "C" void vgx_vita_log_glb_stats(void) {
    vglGlbStats s;
    vglGlbGetStats(&s);
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
        vgx::rec::Present(false);
        if(latTag){const uint64_t f=vgx::CurrentFrame()-1;g_latMap[f%256].tag=latTag;g_latMap[f%256].frame=f;if(NpLatDecoded)NpLatDecoded(latTag);}
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
} // extern "C"

#endif // __vita__
