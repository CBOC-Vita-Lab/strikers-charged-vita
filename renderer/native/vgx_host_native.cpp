// Host layer of the native library on Linux (host32 / host64; GL back end through EGL on Mesa, or the null
// back end): the counterpart of src/vgx_host.cpp without anything of the recompilation runtime (no aurora_*
// entry points, no PAD, no frame statistics of the harness, no sched_yield override).
//
//   - a render thread that owns the back end (and its GL context, host::CreateContext) and runs the player;
//   - the C interface of vgx_native.h: vgxn_init / vgxn_end_frame / vgxn_request_dump / ...;
//   - PNG dumps of presented frames, the same switches as the desktop harness:
//       MSC_DUMP_FRAMES=n     write every n-th presented frame as a PNG
//       MSC_DUMP_LIST=a,b-c   additionally (or instead) dump these frame numbers (the first presented frame is 1)
//       MSC_DUMP_DIR=dir      where (default ./vgx_frames, or vgxn_config::dump_dir)
//       MSC_VGX_LEAD=k        fast-forward: only rasterise dumped frames and the k frames before them
//       MSC_RENDER_SIZE=WxH   render-target size (default: follow the EFB size)
//       MSC_VGX_SURFACE=WxH   what vgxn_surface_size reports (default 640x480)
//       MSC_VGX_SCREEN=WxH    dumps are the picture scaled to this screen size by the present shader (with
//                             MSC_VITA_UPSCALE=off|linear|sharp|sharp+, MSC_VITA_SHARPEN=amount;
//                             MSC_VITA_UPSCALE_EXTRA=sharp,sharp+ also writes frame_N_<mode>.png)
//       MSC_VGX_LOG=1         verbose renderer log
//       MSC_VGX_STATS=file    feature-usage report
#include "vgx_hdtex.h"
#include "vgx_native.h"

#include "vgx_interp.h"
#include "vgx_present.h"
#include "vgx_rec.h"

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
Surface g_surface{640, 480};
int g_renderW = 0, g_renderH = 0;
std::vector<std::string> g_variantNames; // MSC_VITA_UPSCALE_EXTRA names, in order (the file suffixes)

// ---- render thread: owns the context, executes one CmdBuf at a time (or runs the player).
// (Heap objects that are never destroyed: the render thread may be waiting on them when the process exits, and destroying a
// condition variable that has a waiter blocks for ever in exit().)
std::mutex& g_mtx = *new std::mutex;
std::condition_variable& g_cv = *new std::condition_variable;
CmdBuf* g_job = nullptr;
bool g_jobDone = true, g_started = false, g_startOk = false;
std::thread g_thread;

void RenderThread() {
    bool ok = host::CreateContext();
    if (ok) {
        int w = g_renderW, h = g_renderH;
        if (const char* rs = std::getenv("MSC_RENDER_SIZE")) std::sscanf(rs, "%dx%d", &w, &h);
        ok = backend::Init(w, h);
        // MSC_VGX_SCREEN=WxH: dumps and captures are the picture scaled to the screen by the back end's present
        // shader (DrawPicture), as the console shows it; MSC_VITA_UPSCALE=off|linear|sharp|sharp+ and
        // MSC_VITA_SHARPEN=<amount> choose how (off = linear).
        if (ok) {
            int sw = 0, sh = 0;
            if (const char* e = std::getenv("MSC_VGX_SCREEN"); e && std::sscanf(e, "%dx%d", &sw, &sh) == 2 && sw >= 64 && sh >= 64 && sw <= 4096 && sh <= 4096) {
                int mode = backend::PictureModeFromString(std::getenv("MSC_VITA_UPSCALE"));
                if (mode < 0) mode = backend::PIC_LINEAR;
                float sharpen = 0.35f;
                if (const char* sp = std::getenv("MSC_VITA_SHARPEN")) sharpen = static_cast<float>(std::atof(sp));
                backend::SetScreenCapture(sw, sh, mode, sharpen);
                if (const char* ex = std::getenv("MSC_VITA_UPSCALE_EXTRA")) {
                    int modes[4], n = 0;
                    std::string list = ex;
                    for (size_t a = 0; a < list.size() && n < 4;) {
                        size_t b = list.find(',', a);
                        if (b == std::string::npos) b = list.size();
                        const std::string one = list.substr(a, b - a);
                        const int m = backend::PictureModeFromString(one.c_str());
                        if (m >= 0) { modes[n++] = m == backend::PIC_BLIT ? backend::PIC_LINEAR : m; g_variantNames.push_back(one); }
                        a = b + 1;
                    }
                    backend::SetScreenCaptureExtra(modes, n);
                }
            }
        }
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
    if (!g_thread.joinable() && !g_started) {
        g_thread = std::thread(RenderThread);
        // The render thread never returns (it is the player, or waits for jobs for ever): let the process
        // end without joining it.
        g_thread.detach();
    }
    std::unique_lock<std::mutex> lk(g_mtx);
    g_cv.wait(lk, [] { return g_started; });
    return g_startOk;
}

// ---- frame dumps
struct DumpCfg {
    bool init = false;
    uint64_t every = 0;
    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    std::string dir = "vgx_frames";
    std::string statsPath;
    long lead = -1;
} g_dump;

uint64_t g_frame = 0;            // frames presented so far
std::string g_requestPath;       // vgxn_request_dump: the next presented frame goes here
bool g_requested = false;

// MSC_VGX_TEXDUMP=<dir> (diagnostic): every uploaded texture as PNGs, one per mip level.
void DumpTexture(const TexUpload& up, uint32_t fmt, uint64_t frame) {
    static const char* const dir = std::getenv("MSC_VGX_TEXDUMP");
    if (up.format != TF_RGBA8) return;
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

void InitDump(const char* dirFromConfig) {
    if (g_dump.init) return;
    g_dump.init = true;
    if (dirFromConfig && *dirFromConfig) g_dump.dir = dirFromConfig;
    if (const char* d = std::getenv("MSC_VGX_TEXDUMP")) {
        if (*d) { mkdir(d, 0755); SetTextureDumpHook(&DumpTexture); }
    }
    vgx::hdtex::Install(nullptr);   // 1.1: HD character textures (MSC_HDTEX_PATH, default ./hdtex.pack)
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
            if (*e != ',') break;
        }
    }
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
bool WantRender(uint64_t frame) {
    if (g_dump.lead < 0) return true;
    for (uint64_t f = frame; f <= frame + static_cast<uint64_t>(g_dump.lead); ++f)
        if (WantDump(f)) return true;
    return false;
}

bool WritePng(const char* path, const std::vector<uint8_t>& px, int w, int h) {
    if (w <= 0 || h <= 0 || px.size() < static_cast<size_t>(w) * h * 4) return false;
    // Alpha is forced opaque in the file: the EFB alpha channel is not part of the picture.
    std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3);
    for (size_t i = 0, n = static_cast<size_t>(w) * h; i < n; ++i) {
        rgb[i * 3] = px[i * 4]; rgb[i * 3 + 1] = px[i * 4 + 1]; rgb[i * 3 + 2] = px[i * 4 + 2];
    }
    return stbi_write_png(path, w, h, 3, rgb.data(), w * 3) != 0;
}
bool WritePicture(const char* path) {
    int w = 0, h = 0;
    const std::vector<uint8_t>& px = backend::LastDisplayCopy(w, h);
    if (w <= 0 || h <= 0) return false;
    const bool ok = WritePng(path, px, w, h);
    // MSC_VITA_UPSCALE_EXTRA=sharp,sharp+: the same picture made with other present modes, as <name>_<mode>.png
    for (size_t i = 0; i < g_variantNames.size(); ++i) {
        int vw = 0, vh = 0;
        const std::vector<uint8_t>& vp = backend::LastScreenVariant(static_cast<int>(i), vw, vh);
        std::string name = path;
        const size_t dot = name.rfind(".png");
        if (dot != std::string::npos) name.erase(dot);
        std::string tag = g_variantNames[i];
        for (char& c : tag) if (c == '+') c = 'P';
        WritePng((name + "_" + tag + ".png").c_str(), vp, vw, vh);
    }
    return ok;
}

// Frame interpolation (MSC_VGX_INTERP / MSC_VGX_PRESENT): the in-between pictures of a dumped frame are dumped
// too, as frame_<n>_i<slot>.png. Runs on the render thread right after the picture was drawn.
void InterpPicture(uint64_t frame, int slot, bool drawn, float t) {
    const bool selfHere = present::Wall();
    if (!drawn || (t >= 1.0f && !selfHere) || !g_dump.init || !WantDump(frame)) return;
    char path[512];
    if (t >= 1.0f) std::snprintf(path, sizeof path, "%s/frame_%06llu.png", g_dump.dir.c_str(), static_cast<unsigned long long>(frame));
    else std::snprintf(path, sizeof path, "%s/frame_%06llu_i%d.png", g_dump.dir.c_str(), static_cast<unsigned long long>(frame), slot);
    WritePicture(path);
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

int vgxn_init(const vgxn_config* config) {
    if (config && config->surface_width >= 64 && config->surface_height >= 64) g_surface = Surface{config->surface_width, config->surface_height};
    int w = 0, h = 0;
    if (const char* e = std::getenv("MSC_VGX_SURFACE"); e && std::sscanf(e, "%dx%d", &w, &h) == 2 && w >= 64 && h >= 64 && w <= 4096 && h <= 4096)
        g_surface = Surface{w, h};
    if (config) { g_renderW = config->render_width; g_renderH = config->render_height; }
    const bool ok = EnsureThread();
    if (!ok)
        std::fprintf(stderr, "[vgx] the renderer did not start (no GL context). On the host32 build the 32-bit GL libraries come from the private Mesa:\n"
                             "[vgx]   run the program through vgx/native/test/run32.sh (LD_LIBRARY_PATH, EGL_PLATFORM=surfaceless, ...)\n");
    InitDump(config ? config->dump_dir : nullptr);
    if (present::Enabled() || interp::Enabled()) interp::SetPictureHook(&InterpPicture);
    rec::BackendFlags(WantDump(g_frame + 1), !WantRender(g_frame + 1));
    return ok ? 1 : 0;
}

void vgxn_shutdown(void) {}

// W5 RT: the input-delay record of the native port (np include/np/latency.h)
extern "C" uint64_t NpLatTakeSendTag(void) __attribute__((weak));
extern "C" void NpLatDecoded(uint64_t tag) __attribute__((weak));
extern "C" void NpLatShown(uint64_t tag, uint64_t shownNs, int handedToo) __attribute__((weak));
void vgxn_end_frame(void) {
    InitDump(nullptr);
    const uint64_t frame = g_frame + 1;
    const bool wallPresent = present::Wall(); // (MSC_VGX_PRESENT on a wall clock: pictures are made at display ticks)
    const bool env = WantDump(frame) && !wallPresent;
    const bool dump = env || g_requested;
    const uint64_t latTag = NpLatTakeSendTag ? NpLatTakeSendTag() : 0;   // W5 RT: the input-delay record (np/latency.h)
    // A frame that is dumped has to be drawn before this returns; any other frame only has to be consumed
    // by the player.
    rec::Present(dump);
    if (latTag) {
        // (host: no display; the record ends at the decode)
        if (NpLatDecoded) NpLatDecoded(latTag);
        if (NpLatShown) NpLatShown(latTag, 0, 0);
    }
    present::Throttle();
    ++g_frame;
    if (dump) {
        const backend::Stats st = backend::GetStats();
        char path[512];
        const char* target = path;
        if (g_requested) target = g_requestPath.c_str();
        else std::snprintf(path, sizeof path, "%s/frame_%06llu.png", g_dump.dir.c_str(), static_cast<unsigned long long>(frame));
        if (WritePicture(target))
            std::fprintf(stderr, "[vgx] frame %llu -> %s (%u draw calls, %u programs used, %u compiled so far)\n", static_cast<unsigned long long>(frame), target,
                         st.drawCallsThisFrame, st.programsUsedThisFrame, st.programs);
        else std::fprintf(stderr, "[vgx] frame %llu: no display copy to dump\n", static_cast<unsigned long long>(frame));
        if (!g_dump.statsPath.empty()) stats::Dump(g_dump.statsPath.c_str());
        g_requested = false;
    }
    if (!g_dump.statsPath.empty() && !dump && frame % 200 == 0) stats::Dump(g_dump.statsPath.c_str());
    rec::BackendFlags(WantDump(frame + 1), !WantRender(frame + 1));
}

unsigned long long vgxn_frame_count(void) { return g_frame; }

int vgxn_request_dump(const char* png_path) {
    if (!png_path || !*png_path) return 0;
    g_requestPath = png_path;
    g_requested = true;
    rec::BackendFlags(true, false);
    return 1;
}

const unsigned char* vgxn_last_picture(int* width, int* height) {
    int w = 0, h = 0;
    const std::vector<uint8_t>& px = backend::LastDisplayCopy(w, h);
    if (width) *width = w;
    if (height) *height = h;
    return w > 0 && h > 0 ? px.data() : nullptr;
}

void vgxn_surface_size(unsigned int* width, unsigned int* height) {
    if (width) *width = static_cast<uint32_t>(g_surface.w);
    if (height) *height = static_cast<uint32_t>(g_surface.h);
}

} // extern "C"
