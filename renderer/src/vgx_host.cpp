// Host (Linux) platform layer, common to every back end: the render thread
// that owns the back end (and its GL context, see host::CreateContext), PNG
// frame dumps, and the remaining pieces of the desktop graphics-library API
// the runtime links against (aurora_*, PAD).
//
//   MSC_DUMP_FRAMES=n     write every n-th presented frame as a PNG
//   MSC_DUMP_LIST=a,b-c   additionally (or instead) dump these frame numbers
//   MSC_DUMP_DIR=dir      where (default ./vgx_frames)
//   MSC_VGX_LEAD=k        fast-forward: only rasterise frames that are dumped and the k
//                         frames before each of them (default: rasterise every frame)
//   MSC_RENDER_SIZE=WxH   render-target size (default: the game's EFB size)
//   MSC_VGX_SURFACE=WxH   the surface size reported to the runtime (default 640x480). With the
//                         game's widescreen setting on, the runtime fits its 3D projections to
//                         this shape and keeps 2D on a 16:9 canvas (dynamic_aspect.cpp), so
//                         960x544 here + MSC_RENDER_SIZE=960x544 gives the picture the Vita
//                         host shows in full-screen mode, pixel for pixel in layout.
//   MSC_VGX_LOG=1         verbose renderer log
//   MSC_VGX_STATS=file    feature-usage report (default <dump dir>/vgx_stats.txt)
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/gfx.h>
#include <dolphin/gx.h>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/pad.h>

#include "plat_frame_stats.h"
#include "vgx_rec.h"
#include "vgx_interp.h"
#include "vgx_present.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBIW_ASSERT(x)
#include <stb_image_write.h>

#include <condition_variable>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

namespace vgx {

bool LogEnabled() {
    static const bool on = EnvInt("MSC_VGX_LOG", 0) != 0;
    return on;
}

namespace {

struct Surface { int w, h; };
const Surface& SurfaceSize() {
    static const Surface s = [] {
        Surface v{640, 480};
        int w = 0, h = 0;
        if (const char* e = std::getenv("MSC_VGX_SURFACE"); e && std::sscanf(e, "%dx%d", &w, &h) == 2 && w >= 64 && h >= 64 && w <= 4096 && h <= 4096) v = Surface{w, h};
        return v;
    }();
    return s;
}

// ---- render thread: owns the GL context, executes one CmdBuf at a time.
std::mutex g_mtx;
std::condition_variable g_cv;
CmdBuf* g_job = nullptr;
bool g_jobDone = true, g_started = false, g_startOk = false;
std::thread g_thread;

void RenderThread() {
    bool ok = host::CreateContext();
    if (ok) {
        int w = 0, h = 0;
        if (const char* rs = std::getenv("MSC_RENDER_SIZE")) std::sscanf(rs, "%dx%d", &w, &h);
        ok = backend::Init(w, h);
    }
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_started = true;
        g_startOk = ok;
    }
    g_cv.notify_all();
    if (!ok) return;
    // Streaming mode: this thread is the player (front end + back end).
    if (rec::g_mode == rec::MODE_THREAD) rec::PlayerThreadMain();
    for (;;) {
        std::unique_lock<std::mutex> lk(g_mtx);
        g_cv.wait(lk, [] { return g_job != nullptr; });
        CmdBuf* job = g_job;
        lk.unlock();
        interp::Submit(*job); // (backend::Execute unless MSC_VGX_INTERP is set)
        lk.lock();
        g_job = nullptr;
        g_jobDone = true;
        lk.unlock();
        g_cv.notify_all();
    }
}

bool EnsureThread() {
    rec::InitMode();
    if (!g_thread.joinable()) {
        g_thread = std::thread(RenderThread);
        std::unique_lock<std::mutex> lk(g_mtx);
        g_cv.wait(lk, [] { return g_started; });
    }
    return g_startOk;
}

// ---- frame dumps
struct DumpCfg {
    bool init = false;
    uint64_t every = 0;
    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    std::string dir = "vgx_frames";
    std::string statsPath;
    uint64_t maxFrames = 0;
    long lead = -1;
} g_dump;

// MSC_VGX_TEXDUMP=<dir> (diagnostic): every uploaded texture as PNGs, one per mip level.
void DumpTexture(const TexUpload& up, uint32_t fmt, uint64_t frame) {
    static const char* const dir = std::getenv("MSC_VGX_TEXDUMP");
    if (up.format != TF_RGBA8) return; // use MSC_VGX_DXT1=0 MSC_VGX_LA88=0 to dump every decoded format
    uint32_t lw = up.width, lh = up.height;
    for (size_t l = 0; l < up.levels.size(); ++l) {
        char path[512];
        std::snprintf(path, sizeof path, "%s/tex%04u_f%06llu_%ux%u_fmt%X_L%u.png", dir, up.texId, static_cast<unsigned long long>(frame), up.width,
                      up.height, fmt, static_cast<unsigned>(l));
        stbi_write_png(path, static_cast<int>(lw), static_cast<int>(lh), 4, up.levels[l].data(), static_cast<int>(lw * 4));
        lw = lw > 1 ? lw / 2 : 1;
        lh = lh > 1 ? lh / 2 : 1;
    }
}

void InitDump() {
    if (g_dump.init) return;
    g_dump.init = true;
    if (const char* d = std::getenv("MSC_VGX_TEXDUMP")) {
        if (*d) { mkdir(d, 0755); SetTextureDumpHook(&DumpTexture); }
    }
    g_dump.every = static_cast<uint64_t>(EnvInt("MSC_DUMP_FRAMES", 0));
    if (const char* d = std::getenv("MSC_DUMP_DIR")) g_dump.dir = d;
    if (const char* l = std::getenv("MSC_DUMP_LIST")) {
        const char* p = l;
        while (*p) {
            char* e = nullptr;
            const uint64_t a = std::strtoull(p, &e, 10);
            uint64_t b = a;
            if (e == p) break;
            if (*e == '-') { p = e + 1; b = std::strtoull(p, &e, 10); }
            g_dump.ranges.emplace_back(a, b);
            p = (*e == ',') ? e + 1 : e;
            if (*e != ',' ) break;
        }
    }
    g_dump.maxFrames = static_cast<uint64_t>(EnvInt("MSC_MAX_FRAMES", 0));
    g_dump.lead = EnvInt("MSC_VGX_LEAD", -1);
    if (g_dump.every || !g_dump.ranges.empty()) mkdir(g_dump.dir.c_str(), 0755);
    if (!std::getenv("MSC_VGX_FRAMESTATS") && std::getenv("MSC_DUMP_DIR")) {
        mkdir(g_dump.dir.c_str(), 0755);
        setenv("MSC_VGX_FRAMESTATS", (g_dump.dir + "/vgx_frames.csv").c_str(), 1);
    }
    if (const char* s = std::getenv("MSC_VGX_STATS")) g_dump.statsPath = s;
    else if (std::getenv("MSC_DUMP_DIR")) g_dump.statsPath = g_dump.dir + "/vgx_stats.txt";
    stats::SetDetailed(!g_dump.statsPath.empty() || std::getenv("MSC_VGX_FRAMESTATS"));
}
bool WantDump(uint64_t frame) {
    if (g_dump.every && frame % g_dump.every == 0) return true;
    for (const auto& r : g_dump.ranges)
        if (frame >= r.first && frame <= r.second) return true;
    return false;
}

// Does `frame` have to be rasterised (see MSC_VGX_LEAD)?
bool WantRender(uint64_t frame) {
    if (g_dump.lead < 0) return true;
    for (uint64_t f = frame; f <= frame + static_cast<uint64_t>(g_dump.lead); ++f)
        if (WantDump(f)) return true;
    return false;
}

// Frame interpolation (MSC_VGX_INTERP): the in-between pictures of a frame that is dumped are
// dumped too, as frame_<n>_i<slot>.png (the frame itself stays frame_<n>.png, written by
// aurora_end_frame). Runs on the render thread right after the picture was drawn.
void InterpPicture(uint64_t frame, int slot, bool drawn, float t) {
    // (A frame shown as itself is dumped by aurora_end_frame, except with the presenter on a wall clock: there nobody waits for
    // the picture, so this hook writes every picture, as frame_<n>.png when it is the frame itself.)
    const bool selfHere = present::Wall();
    if (!drawn || (t >= 1.0f && !selfHere) || !g_dump.init || !WantDump(frame)) return;
    int w = 0, h = 0;
    const std::vector<uint8_t>& px = backend::LastDisplayCopy(w, h);
    if (w <= 0 || h <= 0) return;
    char path[512];
    if (t >= 1.0f) std::snprintf(path, sizeof path, "%s/frame_%06llu.png", g_dump.dir.c_str(), static_cast<unsigned long long>(frame));
    else std::snprintf(path, sizeof path, "%s/frame_%06llu_i%d.png", g_dump.dir.c_str(), static_cast<unsigned long long>(frame), slot);
    std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3);
    for (size_t i = 0, n = static_cast<size_t>(w) * h; i < n; ++i) {
        rgb[i * 3] = px[i * 4]; rgb[i * 3 + 1] = px[i * 4 + 1]; rgb[i * 3 + 2] = px[i * 4 + 2];
    }
    stbi_write_png(path, w, h, 3, rgb.data(), w * 3);
}

} // namespace

void SubmitToBackend(CmdBuf& buf) {
    if (!EnsureThread()) return;
    std::unique_lock<std::mutex> lk(g_mtx);
    g_job = &buf;
    g_jobDone = false;
    g_cv.notify_all();
    g_cv.wait(lk, [] { return g_jobDone; });
}

} // namespace vgx

using namespace vgx;

extern "C" {

AuroraInfo aurora_initialize(int, char**, const AuroraConfig* config) {
    AuroraInfo info{};
    info.backend = BACKEND_OPENGLES;
    info.userPath = config ? config->userPath : nullptr;
    info.cachePath = config ? config->cachePath : nullptr;
    info.window = nullptr;
    {
        const uint32_t w = static_cast<uint32_t>(SurfaceSize().w), h = static_cast<uint32_t>(SurfaceSize().h);
        info.windowSize = AuroraWindowSize{w, h, w, h, w, h, 1.0f};
    }
    const bool ok = EnsureThread();
    info.initializationStatus = ok ? AURORA_INITIALIZATION_SUCCESS : AURORA_INITIALIZATION_GRAPHICS_UNAVAILABLE;
    info.initializationError = ok ? nullptr : "EGL/GLES2 context creation failed";
    InitDump();
    if (present::Enabled() || interp::Enabled()) interp::SetPictureHook(&InterpPicture);
    rec::BackendFlags(WantDump(PlatFrameStats::FrameCount() + 1), !WantRender(PlatFrameStats::FrameCount() + 1));
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
    InitDump();
    const uint64_t frame = PlatFrameStats::FrameCount() + 1;
    const bool wallPresent = present::Wall(); // (MSC_VGX_PRESENT on a wall clock: pictures are made at display ticks, dumped by InterpPicture)
    const bool dump = WantDump(frame) && !wallPresent;
    // A frame that is dumped has to be drawn before this returns; any other
    // frame only has to be consumed by the player.
    rec::Present(dump);
    present::Throttle(); // (test knob MSC_VGX_THROTTLE: a slow game thread; the game's next frame begins after it)
    if (dump) {
        const backend::Stats st = backend::GetStats();
        int w = 0, h = 0;
        const std::vector<uint8_t>& px = backend::LastDisplayCopy(w, h);
        if (w > 0 && h > 0) {
            char path[512];
            std::snprintf(path, sizeof path, "%s/frame_%06llu.png", g_dump.dir.c_str(), static_cast<unsigned long long>(frame));
            // Alpha is forced opaque in the file: the EFB alpha channel is not part of the picture.
            std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3);
            for (size_t i = 0, n = static_cast<size_t>(w) * h; i < n; ++i) {
                rgb[i * 3] = px[i * 4]; rgb[i * 3 + 1] = px[i * 4 + 1]; rgb[i * 3 + 2] = px[i * 4 + 2];
            }
            stbi_write_png(path, w, h, 3, rgb.data(), w * 3);
            std::fprintf(stderr, "[vgx] frame %llu -> %s (%dx%d, %u draw calls, %u programs used, %u compiled so far)\n",
                         static_cast<unsigned long long>(frame), path, w, h, st.drawCallsThisFrame, st.programsUsedThisFrame, st.programs);
        } else {
            std::fprintf(stderr, "[vgx] frame %llu: no display copy to dump\n", static_cast<unsigned long long>(frame));
        }
        if (!g_dump.statsPath.empty()) stats::Dump(g_dump.statsPath.c_str());
    }
    // The harness leaves with _Exit from inside OnFramePresented (no atexit handlers run), so the
    // feature report is rewritten now and then; the player is idle or in the back end at this point.
    if (!g_dump.statsPath.empty() && !dump && frame % 200 == 0) stats::Dump(g_dump.statsPath.c_str());
    rec::BackendFlags(WantDump(frame + 1), !WantRender(frame + 1));
    PlatFrameStats::OnFramePresented();
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
void AuroraGetSurfaceSize(u32* width, u32* height) {
    // A 4:3 surface unless MSC_VGX_SURFACE says otherwise (the regression references and every
    // picture before round 6 were made with 640x480).
    if (width) *width = static_cast<u32>(SurfaceSize().w);
    if (height) *height = static_cast<u32>(SurfaceSize().h);
}

// One controller on port 0, fed by the harness (scripted input).
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

} // extern "C"

// The navigation harness (plat_headless.cpp) ends every virtual sleep of the
// game thread with sched_yield(). On this shared two-core machine each yield
// hands the core to another worker's job for a whole time slice, which made
// fast-forward runs several times slower than the game thread's own CPU time.
// The game thread never depends on another thread making progress during a
// yield here (the render thread is handed work synchronously), so the harness
// build turns the yield into a no-op unless MSC_VGX_YIELD=1.
extern "C" int sched_yield(void) noexcept {
    static const bool real = vgx::EnvInt("MSC_VGX_YIELD", 0) != 0;
    if (real) std::this_thread::sleep_for(std::chrono::microseconds(50));
    return 0;
}
