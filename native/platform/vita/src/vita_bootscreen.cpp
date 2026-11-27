// The player build's loading screen (W5 BOOT, 17 Oct 2026): a full-screen picture from the first frame the process can draw until
// the game's own first picture, instead of the text console's boot lines.
//
//   - the picture: app0:boot/loading.bin (960 x 544, RGB565, made at build time from NP_ART_DIR by make_vita_art.py: the
//     owner's art with the logo; never in the repository). Without it (a package built without NP_ART_DIR) a plain styled
//     screen is drawn here;
//   - the animated element: a progress bar that follows the boot (the stages of np_vita_main.cpp, the renderer's shader
//     warm-up, the game's first frames) with a glint running along it, and a golden Strikers ball spinning at its head;
//   - two phases, one picture:
//       A  before vitaGL: the text console's framebuffer is the display; a thread of this file (core 2, low priority) redraws
//          the bar region after every second vblank, so the bar moves even while the boot thread waits for the memory card;
//       B  vitaGL owns the display: vgx's display callback (vita/vgx_host_vita.cpp DisplayCallback, through the renderer
//          services) asks Compose() to draw the whole screen into every buffer about to be shown (the three start-up clears,
//          the warm-up progress swaps, the game's first black frames), and the thread keeps animating the buffer last shown;
//   - Finish() (the game created its boot screens: NpBootMark from BootLoadingScene::SceneCreated) runs the bar to the end and
//     fades to black over 0.3 s; then the game's pictures show.
//   - 1.3: a line under the bar names the milestone the boot has reached, and after MSC_BOOT_HINT seconds it says that a slow start is expected (the Galaxy port's fix for a
//     screen that looked hung on a first start: the shader work there can take minutes). Each milestone is a "[boot] milestone" log line; a slow start also leaves a
//     note in the player's basic record (log.txt).
//   - art25: a package without boot/loading.bin (the public one) still shows the game's own art. The picture is made on the console from the player's own
//     files (vita_bootart.cpp: files/Art/fe/mainui.dmn and files/opening.bnr, the port of tools/brand-from-disc.py's loading picture) and kept in
//     ux0:data/mscharged/cache/loading.bin with a stamp of the two files' sizes. Order: the package's picture, else the cache when its stamp is current, else the
//     plain screen at once and a low-priority thread (core 2, behind this one) builds the picture (about a second) and writes the cache (tmp + rename); the screen
//     fades to it over half a second when it is ready, and never waits for it. A missing or damaged file means the plain screen, with one [boot] line saying why.
//     Nothing of the game's art is in the package or the repository.
//   MSC_BOOT_ART=0             no picture from the game files (neither the cache nor the build): the plain screen as before art25. Default on (it changes this screen only).
//   MSC_BOOT_SCREEN=art|text   art is the player build's default, text (the debug console) the lab build's.
//   MSC_BOOT_HINT=<s>          seconds before the slow-start line (default 20); 0: no milestone line and no hint, the screen as 1.2 drew it.
//   MSC_BOOT_SHOT=1|<ms>       (tests) PPM pictures of the screen in ux0:data/mscharged/shots/boot_<ms>.ppm, every 1.5 s (or <ms>), at most 20, and 4 of the fade.
// The drawing is plain C++ on 0xAABBGGRR pixels (the console's and vitaGL's display format), also built on the host for
// tests/test_bootscreen.cpp, which writes pictures of it.
#include "np_vita_internal.h"
#include "vita_font.h"
#include "vita_bootstate.h"
#include "vita_bootart.h"
#include "vita_livearea_art.h"
#include "../../sdk/include/np/np_text.h"

#include <atomic>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#if defined(__vita__)
#include <psp2/display.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#else
#include "plat_realclock.h"
#endif

namespace VitaBootScreen {
namespace {

constexpr int W = VitaConsole::kWidth, H = VitaConsole::kHeight;
// The bar: a capsule track with the fill inset, centred low on the screen; the ball rides at the head of the fill.
constexpr int kTrackX = 230, kTrackW = 500, kTrackY = 497, kTrackH = 12;
constexpr int kFillInset = 2;
constexpr float kBallR = 17.0f;
// The region the animation redraws (bar, glow, ball and its aura), restored from the background each time.
constexpr int kRegX0 = kTrackX - 40, kRegX1 = kTrackX + kTrackW + 40, kRegY0 = kTrackY - 34, kRegY1 = kTrackY + kTrackH + 34;

uint32_t* g_bg = nullptr;                 // the background, W x H, 0xAABBGGRR, in cached memory
std::atomic<bool> g_bgReady{false};
std::atomic<int> g_mode{0};               // 0 undecided, 1 art (active), 2 text, 3 finished
std::atomic<bool> g_paused{false};        // the settings screen has the console's framebuffer
std::atomic<bool> g_stopThread{false};
std::mutex g_draw;                        // one drawer at a time (the thread, Compose on the display queue thread)
BootState::Machine g_m;                   // the life cycle (vita_bootstate.h), under g_draw
std::atomic<bool> g_finishPosted{false};  // Finish() was called (read by every game present without the lock)
void* g_front = nullptr;                  // phase B: the buffer vitaGL showed last (Compose's argument)
uint32_t* g_scratch = nullptr;            // W x H in cached memory: every picture is composed here, then copied (see Thread); under g_draw
bool g_haveArt = false;
// art25: the picture made from the game files, as the 1 MiB file image (header, RGB565 pixels, stamp). The builder thread hands it over in g_artPending; the screen's
// thread takes it into g_artImg (TickArt), blends it in over kArtFadeS, then expands it into g_bg and frees it. All under g_draw.
uint8_t* g_artPending = nullptr;
uint8_t* g_artImg = nullptr;
double g_fadeStartS = -1.0;
constexpr double kArtFadeS = 0.5;

// ---- progress: events set an anchor (where the bar is) and a cap (where this stage ends); between events the target creeps
// towards the cap with the stage's time constant, so the bar never stands still and never passes a stage it has not reached.
std::mutex g_prog;
float g_base = 0.0f, g_cap = 0.03f, g_tau = 0.5f;
double g_anchorS = 0.0;
float g_shown = 0.0f;                     // what the bar shows (eased, never decreasing); drawing thread(s) only, under g_draw
double g_finishS = -1.0;                  // when Finish() was called
std::atomic<bool> g_gameStage{false};
uint64_t g_frameBase = 0;

// ---- 1.3: the line under the bar
constexpr int kLabelY = 518;              // inside the animated region, under the ball's edge
constexpr const char* kSlowHint = "STILL LOADING. A FIRST START TAKES LONGER.";
static_assert(46 * kVitaFontWidth <= kRegX1 - kRegX0, "the slow-start line and the guard notice (46 characters) must fit the region that is redrawn");
std::atomic<const char*> g_milestone{nullptr};
std::atomic<const char*> g_notice{nullptr};   // Notice(): an English key of np_text_strings.tsv, at most 46 characters in every language
constexpr double kNoticeS = 7.0;              // process seconds it stays up (a start takes 10 s or more: the stage lines follow)
std::atomic<bool> g_slowNoted{false};
double g_hintAfterS = 20.0;               // 0: no line at all (MSC_BOOT_HINT)

// One whole line per call: these lines come from three threads (the screen's, the display queue's, the boot thread) and stderr is unbuffered, so a
// printf in parts is cut by the other threads' lines.
void Log(const char* format, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* format, ...) {
    char line[320];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof line, format, args);
    va_end(args);
    std::fputs(line, stderr);
}

double NowS() {
#if defined(__vita__)
    return static_cast<double>(sceKernelGetProcessTimeWide()) / 1e6;
#else
    static const uint64_t t0 = PlatRealMonotonicUs();
    return static_cast<double>(PlatRealMonotonicUs() - t0) / 1e6;
#endif
}

void SetAnchor(float base, float cap, float tau) {
    std::lock_guard<std::mutex> lock(g_prog);
    // never backwards: an anchor below what the creep already reached keeps the reached value
    const double now = NowS();
    const float creep = g_base + (g_cap - g_base) * static_cast<float>(1.0 - std::exp(-(now - g_anchorS) / (g_tau > 0.01f ? g_tau : 0.01f)));
    g_base = base > creep ? base : (creep < cap ? creep : base);
    g_cap = cap > g_base ? cap : g_base;
    g_tau = tau;
    g_anchorS = now;
}

float Target(double now) {
    std::lock_guard<std::mutex> lock(g_prog);
    if (g_finishS >= 0.0) return 1.0f;
    const float t = g_base + (g_cap - g_base) * static_cast<float>(1.0 - std::exp(-(now - g_anchorS) / (g_tau > 0.01f ? g_tau : 0.01f)));
    return t;
}

// ---- pixels
inline uint32_t Pack(int r, int g, int b) {
    return 0xFF000000u | (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(r);
}
inline int Clamp255(float v) { return v <= 0.0f ? 0 : (v >= 255.0f ? 255 : static_cast<int>(v + 0.5f)); }
inline float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
// src over dst with coverage a (0..1); colour in 0..255 floats
inline void Blend(uint32_t& px, float r, float g, float b, float a) {
    if (a <= 0.0f) return;
    if (a > 1.0f) a = 1.0f;
    const float dr = static_cast<float>(px & 0xFF), dg = static_cast<float>((px >> 8) & 0xFF), db = static_cast<float>((px >> 16) & 0xFF);
    px = Pack(Clamp255(dr + (r - dr) * a), Clamp255(dg + (g - dg) * a), Clamp255(db + (b - db) * a));
}
inline void Add(uint32_t& px, float r, float g, float b, float a) {
    if (a <= 0.0f) return;
    const float dr = static_cast<float>(px & 0xFF), dg = static_cast<float>((px >> 8) & 0xFF), db = static_cast<float>((px >> 16) & 0xFF);
    px = Pack(Clamp255(dr + r * a), Clamp255(dg + g * a), Clamp255(db + b * a));
}

// The plain screen (no art in the package): a deep blue field with a soft light behind the bar, a faint centre circle and
// diagonal stripes, in the colours of the bar.
void MakePlainBackground(uint32_t* out) {
    for (int y = 0; y < H; ++y) {
        const float fy = static_cast<float>(y) / H;
        for (int x = 0; x < W; ++x) {
            const float fx = static_cast<float>(x) / W;
            float r = 10 + 8 * (1 - fy), g = 18 + 14 * (1 - fy), b = 44 + 26 * (1 - fy);
            const float dx = (fx - 0.5f) * 1.75f, dy = (fy - 0.42f);
            const float glow = std::exp(-(dx * dx + dy * dy) * 6.0f);
            r += 40 * glow; g += 46 * glow; b += 70 * glow;
            const float rr = std::sqrt(dx * dx + dy * dy * 3.06f);
            const float ring = std::exp(-((rr - 0.30f) * (rr - 0.30f)) * 9000.0f) * 0.10f;
            r += 120 * ring; g += 170 * ring; b += 230 * ring;
            const float s = std::fmod(static_cast<float>(x + y) * 0.02f, 2.0f) < 1.0f ? 1.0f : 0.0f;
            r += 3 * s; g += 4 * s; b += 7 * s;
            out[y * W + x] = Pack(Clamp255(r), Clamp255(g), Clamp255(b));
        }
    }
}

// app0:boot/loading.bin: "NPBS", u16 width, u16 height, u32 format (1 = RGB565 little-endian), then the pixels, rows top first.
bool LoadArt(const char* path, uint32_t* out) {
#if defined(__vita__)
    const SceUID f = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (f < 0) return false;
    unsigned char head[12];
    if (sceIoRead(f, head, sizeof head) != static_cast<int>(sizeof head)) { sceIoClose(f); return false; }
#else
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    unsigned char head[12];
    if (std::fread(head, 1, sizeof head, f) != sizeof head) { std::fclose(f); return false; }
#endif
    const int w = head[4] | (head[5] << 8), h = head[6] | (head[7] << 8);
    const unsigned format = head[8] | (head[9] << 8) | (head[10] << 16) | (static_cast<unsigned>(head[11]) << 24);
    bool ok = std::memcmp(head, "NPBS", 4) == 0 && w == W && h == H && format == 1;
    // read straight into the second half of the output (2 bytes per pixel there, 4 per pixel when expanded front to back)
    uint16_t* src = reinterpret_cast<uint16_t*>(out + W * H / 2);
    const size_t bytes = static_cast<size_t>(W) * H * 2;
    if (ok) {
#if defined(__vita__)
        size_t got = 0;
        while (got < bytes) {
            const int n = sceIoRead(f, reinterpret_cast<char*>(src) + got, static_cast<SceSize>(bytes - got > 262144 ? 262144 : bytes - got));
            if (n <= 0) break;
            got += static_cast<size_t>(n);
        }
        ok = got == bytes;
#else
        ok = std::fread(src, 1, bytes, f) == bytes;
#endif
    }
#if defined(__vita__)
    sceIoClose(f);
#else
    std::fclose(f);
#endif
    if (!ok) return false;
    for (int i = 0; i < W * H; ++i) {
        const uint16_t p = src[i];   // (i * 4 bytes of output never pass i * 2 bytes of input + the half-buffer offset)
        const int r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
        out[i] = Pack((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2));
    }
    return true;
}

// Capsule coverage: distance of (x, y) from the segment (x0..x1, yc), radius rad.
inline float Capsule(float x, float y, float x0, float x1, float yc, float rad) {
    const float cx = x < x0 ? x0 : (x > x1 ? x1 : x);
    const float dx = x - cx, dy = y - yc;
    return Clamp01(rad - std::sqrt(dx * dx + dy * dy) + 0.5f);
}

// The golden ball: a metal sphere with three seam rings, lit from the upper left, an electric rim and aura that pulse.
void DrawBall(uint32_t* fb, float cx, float cy, float angle, float pulse) {
    const int x0 = static_cast<int>(cx - kBallR - 10), x1 = static_cast<int>(cx + kBallR + 11);
    const int y0 = static_cast<int>(cy - kBallR - 10), y1 = static_cast<int>(cy + kBallR + 11);
    // rotation: about a tilted axis, so the rings tumble
    const float ax = 0.35f, ay = 0.88f, az = 0.32f;   // (normalised below)
    const float al = std::sqrt(ax * ax + ay * ay + az * az);
    const float ux = ax / al, uy = ay / al, uz = az / al;
    const float c = std::cos(angle), s = std::sin(angle), t = 1.0f - c;
    const float m00 = t * ux * ux + c, m01 = t * ux * uy - s * uz, m02 = t * ux * uz + s * uy;
    const float m10 = t * ux * uy + s * uz, m11 = t * uy * uy + c, m12 = t * uy * uz - s * ux;
    const float m20 = t * ux * uz - s * uy, m21 = t * uy * uz + s * ux, m22 = t * uz * uz + c;
    const float lx = -0.45f, ly = -0.62f, lz = 0.64f;      // light (y down)
    const float hl = std::sqrt(lx * lx + ly * ly + (lz + 1) * (lz + 1));
    const float hx = lx / hl, hy = ly / hl, hz = (lz + 1) / hl;
    for (int y = y0; y < y1; ++y) {
        if (y < 0 || y >= H) continue;
        for (int x = x0; x < x1; ++x) {
            if (x < 0 || x >= W) continue;
            const float dx = (x + 0.5f - cx) / kBallR, dy = (y + 0.5f - cy) / kBallR;
            const float r2 = dx * dx + dy * dy, r = std::sqrt(r2);
            uint32_t& px = fb[y * W + x];
            // aura: a cyan halo outside the ball
            if (r > 0.9f) {
                const float d = (r - 1.0f) * kBallR;             // pixels outside the edge
                const float a = d < 0.0f ? 1.0f : std::exp(-d * 0.32f);
                Add(px, 90, 200, 255, a * (0.30f + 0.35f * pulse));
            }
            const float cover = Clamp01((1.0f - r) * kBallR + 0.5f);
            if (cover <= 0.0f) continue;
            const float nz = r2 < 1.0f ? std::sqrt(1.0f - r2) : 0.0f;
            const float nx = dx, ny = dy;
            // the surface point in the ball's own frame
            const float px_ = m00 * nx + m01 * ny + m02 * nz, py_ = m10 * nx + m11 * ny + m12 * nz, pz_ = m20 * nx + m21 * ny + m22 * nz;
            const float e = std::fmin(std::fabs(px_), std::fmin(std::fabs(py_), std::fabs(pz_)));
            const float seam = Clamp01((0.075f - e) * 40.0f);    // 1 on a seam ring
            const float diff = std::fmax(0.0f, nx * lx + ny * ly + nz * lz);
            const float nh = std::fmax(0.0f, nx * hx + ny * hy + nz * hz);
            const float n2 = nh * nh, n4 = n2 * n2, n8 = n4 * n4, n16 = n8 * n8;
            const float spec = n16 * n8 * n4;                    // nh^28
            const float om = 1.0f - nz;
            const float rim = om * om * std::sqrt(om) * (0.55f + 0.45f * pulse);   // (1 - nz)^2.5
            float cr = 255 * (0.22f + 0.95f * diff), cg = 186 * (0.22f + 0.95f * diff), cb = 38 * (0.22f + 0.95f * diff);
            const float k = 1.0f - 0.62f * seam;
            cr = cr * k + 255 * spec * 0.95f + 90 * rim;
            cg = cg * k + 250 * spec * 0.95f + 205 * rim;
            cb = cb * k + 225 * spec * 0.95f + 255 * rim;
            Blend(px, cr, cg, cb, cover);
        }
    }
}

// Centred, with a one-pixel shadow: the picture behind it is the owner's art, of any brightness.
void DrawLabel(uint32_t* fb, const char* text) {
    text = NpText::Tr(text);   // (1.3) in the game's language (np_text.h); UTF-8, drawn in characters
    const int count = VitaUtf8Length(text);
    const int x0 = (W - count * kVitaFontWidth) / 2;
    for (int pass = 0; pass < 2; ++pass) {
        const int shift = pass == 0 ? 1 : 0;
        const uint32_t color = pass == 0 ? 0xFF000000u : 0xFFF0F0F0u;
        const char* t = text;
        for (int c = 0; c < count; ++c) {
            const uint16_t* glyph = VitaFontGlyph(VitaUtf8Next(t));
            for (int j = 0; j < kVitaFontHeight; ++j) {
                const int y = kLabelY + j + shift;
                if (!glyph[j] || y >= kRegY1) continue;
                for (int i = 0; i < kVitaFontWidth; ++i)
                    if (glyph[j] & (1u << (kVitaFontWidth - 1 - i))) fb[y * W + x0 + c * kVitaFontWidth + i + shift] = color;
            }
        }
    }
}

bool SlowStart(double now) { return g_hintAfterS > 0.0 && now >= g_hintAfterS; }

// The bar region on top of the background that is already in `fb` there.
void DrawElements(uint32_t* fb, float shown, double now) {
    const float yc = kTrackY + kTrackH * 0.5f;
    const float tx0 = kTrackX + kTrackH * 0.5f, tx1 = kTrackX + kTrackW - kTrackH * 0.5f;
    const float fillH = kTrackH - 2.0f * kFillInset;
    const float fx0 = kTrackX + kFillInset + fillH * 0.5f;
    const float fullX1 = kTrackX + kTrackW - kFillInset - fillH * 0.5f;
    const float head = fx0 + (fullX1 - fx0) * Clamp01(shown);
    const bool fill = shown > 0.002f;
    const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(now) * 5.2f);
    const float glowK = 0.20f + 0.10f * pulse;
    // the glint: runs from the start of the fill to its head every 1.4 s
    const float phase = static_cast<float>(std::fmod(now, 1.4) / 1.4);
    const float glint = fx0 - 40.0f + (head - fx0 + 80.0f) * phase;
    constexpr float kGlowR = 26.0f;
    const int xa = static_cast<int>(std::fmin(static_cast<float>(kTrackX), fx0 - kGlowR)) - 1;
    const int xb = static_cast<int>(std::fmax(static_cast<float>(kTrackX + kTrackW), head + kGlowR)) + 2;
    // (the costly terms depend on the row alone between the caps of the capsules: computed once per row; square roots only near the ends)
    for (int y = static_cast<int>(yc - kGlowR) - 1; y <= static_cast<int>(yc + kGlowR) + 1; ++y) {
        if (y < kRegY0 || y >= kRegY1) continue;
        uint32_t* row = fb + y * W;
        const float py = y + 0.5f, dy = py - yc, ady = std::fabs(dy);
        const float glowRow = (fill && ady < kGlowR) ? std::exp(-ady * 0.16f) * glowK : 0.0f;
        const float outerRow = Clamp01(kTrackH * 0.5f - ady + 0.5f), innerRow = Clamp01(kTrackH * 0.5f - 1.0f - ady + 0.5f);
        const float fillRow = fill ? Clamp01(fillH * 0.5f - ady + 0.5f) : 0.0f;
        const float v = (py - (yc - fillH * 0.5f)) / fillH;     // 0 top .. 1 bottom of the fill
        const float hi = v < 0.45f ? (0.45f - v) * 1.4f : 0.0f;
        const float shade = 1.0f - 0.35f * Clamp01(v - 0.5f);
        for (int x = xa; x < xb; ++x) {
            if (x < kRegX0 || x >= kRegX1) continue;
            const float pxf = x + 0.5f;
            uint32_t& p = row[x];
            // the glow under the fill (orange, pulsing)
            if (fill) {
                float g = 0.0f;
                if (pxf >= fx0 && pxf <= head) g = glowRow;
                else {
                    const float ex = pxf < fx0 ? pxf - fx0 : pxf - head;
                    const float d = std::sqrt(ex * ex + dy * dy);
                    if (d < kGlowR) g = std::exp(-d * 0.16f) * glowK;
                }
                if (g > 0.0f) Add(p, 255, 120, 20, g);
            }
            // the track: dark glass with a thin light edge
            float outer, inner;
            if (pxf >= tx0 && pxf <= tx1) { outer = outerRow; inner = innerRow; }
            else { outer = Capsule(pxf, py, tx0, tx1, yc, kTrackH * 0.5f); inner = outer > 0.0f ? Capsule(pxf, py, tx0, tx1, yc, kTrackH * 0.5f - 1.0f) : 0.0f; }
            if (outer > 0.0f) {
                Blend(p, 4, 6, 14, outer * 0.62f);
                Blend(p, 210, 225, 255, (outer - inner) * 0.30f);
            }
            // the fill: orange to gold along the bar, a highlight on its upper half, the glint
            if (fillRow > 0.0f || (fill && ady < fillH)) {
                const float a = (pxf >= fx0 && pxf <= head) ? fillRow : Capsule(pxf, py, fx0, head, yc, fillH * 0.5f);
                if (a > 0.0f) {
                    const float u = (pxf - fx0) / (fullX1 - fx0);
                    float r = 255 + 80 * hi, g = 92 + 128 * u + 120 * hi, b = 30 * u + 160 * hi;
                    const float dg = pxf - glint;
                    const float gl = (dg > -40.0f && dg < 40.0f) ? std::exp(-(dg * dg) / 220.0f) * 0.75f : 0.0f;
                    r = r * shade + 255 * gl; g = g * shade + 255 * gl; b = b * shade + 240 * gl;
                    Blend(p, r, g, b, a);
                }
            }
        }
    }
    // the ball rolls with the head (one turn per 2 pi radii of travel) and spins by itself
    const float angle = (head - fx0) / kBallR + static_cast<float>(now) * 3.4f;
    DrawBall(fb, head, yc, angle, pulse);
    // 1.3: the settings guard's line (an older np-player.txt was updated) replaces the stage line for the first kNoticeS seconds; then the stages go on as before.
    const char* notice = g_notice.load(std::memory_order_relaxed);
    if (notice && now < kNoticeS) {
        DrawLabel(fb, notice);
    } else if (g_hintAfterS > 0.0) {
        const char* label = SlowStart(now) ? kSlowHint : g_milestone.load(std::memory_order_relaxed);
        if (label) DrawLabel(fb, label);
    }
}

void RestoreRegion(uint32_t* fb, float brightness) {
    for (int y = kRegY0; y < kRegY1; ++y) {
        if (brightness >= 0.999f) {
            std::memcpy(fb + y * W + kRegX0, g_bg + y * W + kRegX0, static_cast<size_t>(kRegX1 - kRegX0) * 4u);
        } else {
            for (int x = kRegX0; x < kRegX1; ++x) {
                const uint32_t s = g_bg[y * W + x];
                fb[y * W + x] = Pack(Clamp255((s & 0xFF) * brightness), Clamp255(((s >> 8) & 0xFF) * brightness), Clamp255(((s >> 16) & 0xFF) * brightness));
            }
        }
    }
}

void Darken(uint32_t* fb, int y0, int y1, float k) {
    if (k >= 0.999f) return;
    const uint32_t m = static_cast<uint32_t>(k * 256.0f);
    for (int i = y0 * W; i < y1 * W; ++i) {
        const uint32_t p = fb[i];
        fb[i] = 0xFF000000u | ((((p & 0xFF00FFu) * m) >> 8) & 0xFF00FFu) | ((((p & 0x00FF00u) * m) >> 8) & 0x00FF00u);
    }
}

// The bar value for this moment (eases towards the target, never back), and the fade at the end: 1 = full picture, 0 = black.
float Advance(double now, float* brightness) {
    const float target = Target(now);
    if (target > g_shown) g_shown += (target - g_shown) * (g_finishS >= 0.0 ? 0.45f : 0.22f) + 0.0004f;
    if (g_shown > 1.0f) g_shown = 1.0f;
    float b = 1.0f;
    if (g_finishS >= 0.0) {
        const double t = now - g_finishS - 0.12;   // a moment at 100 %, then the fade
        b = t <= 0.0 ? 1.0f : static_cast<float>(1.0 - t / 0.30);
        if (b < 0.0f) b = 0.0f;
    }
    *brightness = b;
    return g_shown;
}

// The picture behind the bar: g_bg, or while the made picture fades in (g_artImg) a blend of the plain background and it. Under g_draw. Small locals only:
// the display callback (a 4 KiB stack) runs this too.
void BackgroundInto(uint32_t* fb, double now) {
    if (!g_artImg) {
        std::memcpy(fb, g_bg, static_cast<size_t>(W) * H * 4u);
        return;
    }
    const double t = g_fadeStartS < 0.0 ? 0.0 : (now - g_fadeStartS) / kArtFadeS;
    const uint32_t k = t <= 0.0 ? 0u : (t >= 1.0 ? 256u : static_cast<uint32_t>(t * 256.0));
    const uint16_t* art = reinterpret_cast<const uint16_t*>(g_artImg + VitaBootArt::kHeaderBytes);
    for (int i = 0; i < W * H; ++i) {
        const uint32_t p = art[i], s = g_bg[i];
        const uint32_t r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
        fb[i] = Pack(static_cast<int>(((s & 0xFF) * (256u - k) + ((r << 3) | (r >> 2)) * k) >> 8),
                     static_cast<int>((((s >> 8) & 0xFF) * (256u - k) + ((g << 2) | (g >> 4)) * k) >> 8),
                     static_cast<int>((((s >> 16) & 0xFF) * (256u - k) + ((b << 3) | (b >> 2)) * k) >> 8));
    }
}

// The finished picture becomes the background for good (g_draw held; the screen's thread only: it frees memory).
void FinalizeArt() {
    if (!g_artImg) return;
    const uint16_t* art = reinterpret_cast<const uint16_t*>(g_artImg + VitaBootArt::kHeaderBytes);
    for (int i = 0; i < W * H; ++i) {
        const uint32_t p = art[i], r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
        g_bg[i] = Pack(static_cast<int>((r << 3) | (r >> 2)), static_cast<int>((g << 2) | (g >> 4)), static_cast<int>((b << 3) | (b >> 2)));
    }
    std::free(g_artImg);
    g_artImg = nullptr;
    g_fadeStartS = -1.0;
}

// Each turn of the screen's thread (g_draw held): take a handed-over picture, and end the fade (at once when the display is vitaGL's, where only
// whole pictures are composed, or when the end of the loading screen has begun).
[[maybe_unused]] void TickArt(double now, bool console) {
    if (g_artPending && !g_artImg) {
        g_artImg = g_artPending;
        g_artPending = nullptr;
        g_fadeStartS = now;
    }
    if (g_artImg && (!console || g_finishS >= 0.0 || now - g_fadeStartS >= kArtFadeS)) FinalizeArt();
}

void ComposeFull(uint32_t* fb, double now) {
    float brightness = 1.0f;
    const float shown = Advance(now, &brightness);
    if (!g_bgReady.load(std::memory_order_acquire)) {
        for (int i = 0; i < W * H; ++i) fb[i] = 0xFF000000u;
        return;
    }
    BackgroundInto(fb, now);
    DrawElements(fb, shown, now);
    if (brightness < 0.999f) Darken(fb, 0, H, brightness);   // (the fade: everything once)
}

void ComposeRegion(uint32_t* fb, double now) {
    float brightness = 1.0f;
    const float shown = Advance(now, &brightness);
    if (!g_bgReady.load(std::memory_order_acquire)) return;
    RestoreRegion(fb, 1.0f);
    DrawElements(fb, shown, now);
    if (brightness < 0.999f) Darken(fb, kRegY0, kRegY1, brightness);
}

// The end, for good (g_draw held; the state machine has said so): mode 3, the thread told to stop, the console gets its text back (in memory: the renderer has the display).
void EndLocked(double now) {
    g_mode.store(3);
    g_stopThread.store(true);
    g_front = nullptr;
    Log("[boot] loading screen done, the game's pictures show: %s  [+%lu ms]\n", g_m.endedBy ? g_m.endedBy : "", static_cast<unsigned long>(now * 1000.0));
    VitaConsole::ShowText();
}

#if defined(__vita__)
SceUID g_thread = -1;
int g_shots = 0;
double g_lastShot = -1.0;
int g_fadeShots = 0;
void MaybeShot(const uint32_t* fb, double now) {
    // MSC_BOOT_SHOT=1 (a picture every 1.5 s) or =<ms>; at most 20, and up to 4 more while it fades out
    static const int everyMs = [] { const char* e = std::getenv("MSC_BOOT_SHOT"); const int v = e ? std::atoi(e) : 0; return v <= 0 ? 0 : (v < 100 ? 1500 : v); }();
    if (everyMs == 0) return;
    const bool fading = g_finishS >= 0.0;
    if (fading) {
        if (g_fadeShots >= 4 || (g_lastShot >= 0.0 && now - g_lastShot < 0.09)) return;
        ++g_fadeShots;
    } else if (g_shots >= 20 || (g_lastShot >= 0.0 && now - g_lastShot < everyMs / 1000.0)) {
        return;
    }
    g_lastShot = now;
    sceIoMkdir("ux0:/data/mscharged/shots", 0777);
    char path[96];
    std::snprintf(path, sizeof path, "ux0:/data/mscharged/shots/boot_%06u.ppm", static_cast<unsigned>(now * 1000.0));
    const SceUID f = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (f < 0) return;
    char head[32];
    const int n = std::snprintf(head, sizeof head, "P6\n%d %d\n255\n", W, H);
    sceIoWrite(f, head, static_cast<SceSize>(n));
    static unsigned char line[W * 3];
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const uint32_t p = fb[y * W + x];
            line[x * 3] = static_cast<unsigned char>(p);
            line[x * 3 + 1] = static_cast<unsigned char>(p >> 8);
            line[x * 3 + 2] = static_cast<unsigned char>(p >> 16);
        }
        sceIoWrite(f, line, sizeof line);
    }
    sceIoClose(f);
    ++g_shots;
}

void ScanoutConsole() { VitaConsole::PresentScreen(); }

// The buffer the screen is shown from is the one being displayed (phase A: the console's; phase B: the one vitaGL showed last),
// and it is in CDRAM, which the CPU reads very slowly. Drawing the bar and the ball there (restore the region from the
// background, then blend the ball over it) let the display scan the region half drawn: on the maintainer console the ball looked
// transparent and flickered (17 Oct). So every picture is composed in g_scratch (cached memory) and only copied, finished,
// to the shown buffer right after a vertical blank, well before the beam reaches the bar at the bottom of the screen.
// (g_scratch itself is declared with the other state at the top.)
void CopyRows(uint32_t* dst, const uint32_t* src, int y0, int y1, int x0, int x1) {
    for (int y = y0; y < y1; ++y)
        std::memcpy(dst + y * W + x0, src + y * W + x0, static_cast<size_t>(x1 - x0) * 4u);
}

extern "C" const char* NpGameRootPath(void);

// ---- art25: the picture from the player's game files (vita_bootart.cpp), the cache of it, and the thread that makes it
struct GameFile {
    SceUID fd = -1;
    uint64_t size = 0;
    GameFile() = default;
    GameFile(const GameFile&) = delete;
    GameFile& operator=(const GameFile&) = delete;
    ~GameFile() { if (fd >= 0) sceIoClose(fd); }
    bool Open(const char* path) {
        fd = sceIoOpen(path, SCE_O_RDONLY, 0);
        if (fd < 0) { fd = -1; return false; }
        const SceOff end = sceIoLseek(fd, 0, SCE_SEEK_END);
        if (end <= 0) return false;
        size = static_cast<uint64_t>(end);
        return true;
    }
};
bool ReadGameFile(void* context, uint64_t offset, void* destination, size_t length) {
    GameFile* f = static_cast<GameFile*>(context);
    char* out = static_cast<char*>(destination);
    while (length > 0) {
        const int n = sceIoPread(f->fd, out, static_cast<SceSize>(length > 262144 ? 262144 : length), static_cast<SceOff>(offset));
        if (n <= 0) return false;
        out += n;
        offset += static_cast<uint64_t>(n);
        length -= static_cast<size_t>(n);
    }
    return true;
}

struct ArtPaths {
    char dmn[200], bnr[200], dir[160], cache[180], temp[190];
    ArtPaths() {
        const char* root = NpGameRootPath();
        std::snprintf(dmn, sizeof dmn, "%s/files/Art/fe/mainui.dmn", root);
        std::snprintf(bnr, sizeof bnr, "%s/files/opening.bnr", root);
        std::snprintf(dir, sizeof dir, "%s/cache", VitaPlatform::kDataDevicePath);
        std::snprintf(cache, sizeof cache, "%s/loading.bin", dir);
        std::snprintf(temp, sizeof temp, "%s/loading.bin.tmp", dir);
    }
};

bool BootArtOn() {
    const char* e = std::getenv("MSC_BOOT_ART");
    return !(e && e[0] == '0');
}

// The cache is current when its length is the picture's plus the stamp and the stamp names this format version and these two files' sizes.
bool CacheCurrent(const ArtPaths& paths, uint64_t dmnSize, uint64_t bnrSize) {
    GameFile cache;
    if (!cache.Open(paths.cache) || cache.size != VitaBootArt::kCacheBytes) return false;
    uint8_t trailer[VitaBootArt::kStampBytes];
    if (!ReadGameFile(&cache, VitaBootArt::kPictureBytes, trailer, sizeof trailer)) return false;
    return VitaBootArt::StampMatches(trailer, dmnSize, bnrSize);
}

// tmp + rename: a cut-off write (power, a kill) leaves the old file or none, never half of a new one.
bool WriteCache(const ArtPaths& paths, const uint8_t* image) {
    sceIoMkdir("ux0:/data", 0777);
    sceIoMkdir(VitaPlatform::kDataDevicePath, 0777);
    sceIoMkdir(paths.dir, 0777);
    const SceUID out = sceIoOpen(paths.temp, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (out < 0) return false;
    bool ok = true;
    for (size_t at = 0; at < VitaBootArt::kCacheBytes && ok;) {
        const size_t n = VitaBootArt::kCacheBytes - at > 65536 ? 65536 : VitaBootArt::kCacheBytes - at;
        ok = sceIoWrite(out, image + at, static_cast<SceSize>(n)) == static_cast<int>(n);
        at += n;
    }
    if (sceIoClose(out) < 0) ok = false;
    if (ok && sceIoRename(paths.temp, paths.cache) < 0) {
        sceIoRemove(paths.cache);   // (a rename that will not replace)
        ok = sceIoRename(paths.temp, paths.cache) >= 0;
    }
    if (!ok) sceIoRemove(paths.temp);
    return ok;
}

volatile bool g_artCancel = false;   // a fatal report has the screen: stop making the picture
SceUID g_artThread = -1;

// The builder: reads the two game files, makes the loading picture, writes the cache, hands it to the screen,
// and then (on first boot with generic artwork) builds the LiveArea & Home-screen bubble PNGs from the player's own disc.
// Low priority on core 2: never blocks boot; peak memory stays bounded (~5.5 MiB) because the two phases run sequentially.
int ArtThread(SceSize argsLen, void* argp) {
    const uint32_t flags = (argsLen >= sizeof(uint32_t) && argp) ? *static_cast<const uint32_t*>(argp) : 1u;
    if (flags & 1u) {
        const double t0 = NowS();
        const ArtPaths paths;
        char why[120] = "";
        uint8_t* image = static_cast<uint8_t*>(std::malloc(VitaBootArt::kCacheBytes));
        bool built = false, cached = false;
        GameFile dmn, bnr;
        if (!image) std::snprintf(why, sizeof why, "no memory");
        else if (!dmn.Open(paths.dmn)) std::snprintf(why, sizeof why, "Art/fe/mainui.dmn is missing or empty");
        else if (!bnr.Open(paths.bnr)) std::snprintf(why, sizeof why, "opening.bnr is missing or empty");
        else {
            VitaBootArt::Source a, b;
            a.read = b.read = ReadGameFile;
            a.context = &dmn; a.size = dmn.size;
            b.context = &bnr; b.size = bnr.size;
            VitaBootArt::Stats stats;
            built = VitaBootArt::Build(a, b, image, VitaBootArt::kCacheBytes, why, sizeof why, &stats, &g_artCancel);
            if (built) {
                VitaBootArt::WriteStamp(image + VitaBootArt::kPictureBytes, dmn.size, bnr.size);
                cached = !g_artCancel && WriteCache(paths, image);
            }
        }
        bool shown = false;
        if (built) {
            std::lock_guard<std::mutex> lock(g_draw);
            if (g_mode.load() == 1 && !g_stopThread.load() && g_finishS < 0.0 && !g_artPending && !g_artImg && g_bgReady.load()) {
                g_artPending = image;
                image = nullptr;
                shown = true;
            }
        }
        std::free(image);
        if (built) Log("[boot] picture: made from the game files in %u ms, %s, %s  [+%lu ms]\n", static_cast<unsigned>((NowS() - t0) * 1000.0),
                       cached ? "cache written" : "cache NOT written (the next start makes it again)", shown ? "fading in" : "too late for this start",
                       static_cast<unsigned long>(NowS() * 1000.0));
        else Log("[boot] picture: plain, %s  [+%lu ms]\n", why[0] ? why : "not made", static_cast<unsigned long>(NowS() * 1000.0));
    }
    if ((flags & 2u) && !g_artCancel) {
        VitaLiveAreaArt::ApplyIfNeeded(&g_artCancel);
    }
    g_artThread = -1;
    sceKernelExitDeleteThread(0);
    return 0;
}

int Thread(SceSize, void*) {
    // int33: keep VitaOS from auto-suspending (~15 s unattended) during early first-run seed copy / art build before PowerWatch starts.
    sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
    // the picture first (about 1 MiB from the package), then the animation
    uint32_t* bg = static_cast<uint32_t*>(std::malloc(static_cast<size_t>(W) * H * 4u));
    const double t0 = NowS();
    // art25: the package's picture wins; else the cache of the picture made from the game files when its stamp is current; else the plain screen now and the
    // picture made in the background (below, once the plain screen is up)
    bool fromCache = false, build = false;
    const char* plainWhy = "no picture in the package";
    if (bg) {
        g_haveArt = LoadArt("app0:boot/loading.bin", bg);
        if (!g_haveArt) {
            if (!BootArtOn()) {
                plainWhy = "no picture in the package; MSC_BOOT_ART=0";
            } else {
                const ArtPaths paths;
                GameFile dmn, bnr;
                if (!dmn.Open(paths.dmn) || !bnr.Open(paths.bnr)) plainWhy = "no picture in the package, and no game files to make one from";
                else if (CacheCurrent(paths, dmn.size, bnr.size) && LoadArt(paths.cache, bg)) { g_haveArt = true; fromCache = true; }
                else build = true;
            }
        }
        if (!g_haveArt) MakePlainBackground(bg);
        g_bg = bg;
        g_bgReady.store(true, std::memory_order_release);
    }
    uint32_t* scratch = bg ? static_cast<uint32_t*>(std::malloc(static_cast<size_t>(W) * H * 4u)) : nullptr;
    char plainText[96];
    std::snprintf(plainText, sizeof plainText, build ? "plain at first, the picture is made from the game files" : "plain (%s)", plainWhy);
    Log("[boot] loading screen: %s, ready in %u ms%s  [+%lu ms]\n", g_haveArt ? (fromCache ? "the picture made from the game files (cache)" : "the package's picture (boot/loading.bin)") : (bg ? plainText : "NO MEMORY"),
                 static_cast<unsigned>((NowS() - t0) * 1000.0), scratch ? "" : " (no memory for the off-screen buffer: drawn in place)",
                 static_cast<unsigned long>(NowS() * 1000.0));
    if (!bg) { g_mode.store(2); return 0; }
    {
        std::lock_guard<std::mutex> lock(g_draw);
        g_scratch = scratch;
        if (!g_paused.load() && VitaConsole::OwnsDisplay()) {
            uint32_t* fb = const_cast<uint32_t*>(VitaConsole::Pixels());
            if (fb) {
                if (g_scratch) { ComposeFull(g_scratch, NowS()); std::memcpy(fb, g_scratch, static_cast<size_t>(W) * H * 4u); }
                else ComposeFull(fb, NowS());
                ScanoutConsole();
            }
        }
    }
    const uint32_t artFlags = (build ? 1u : 0u) | (VitaLiveAreaArt::NeedsApply() ? 2u : 0u);
    if (artFlags) {
        // (behind this thread and the game's: core 2, one step above the lowest user priority; stack 96 KiB, the builder's own buffers are on the heap)
        g_artThread = sceKernelCreateThread("np_bootart", ArtThread, 0x10000100 + 30, 96 * 1024, 0, 0x40000, nullptr);
        if (g_artThread < 0 || sceKernelStartThread(g_artThread, sizeof artFlags, const_cast<uint32_t*>(&artFlags)) < 0) Log("[boot] picture: plain, the builder thread did not start (0x%X)\n", static_cast<unsigned>(g_artThread));
    }
    unsigned powerTickDiv = 0;
    while (!g_stopThread.load(std::memory_order_relaxed)) {
        if (++powerTickDiv >= 30) {   // ~1 s at 30 Hz (2 vblanks per iteration)
            powerTickDiv = 0;
            if (g_mode.load(std::memory_order_relaxed) == 1 && !g_paused.load(std::memory_order_relaxed))
                sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
        }
        // compose the next picture off screen, then show it at the second vertical blank from now (30 updates a second)
        bool full = false;
        {
            std::lock_guard<std::mutex> lock(g_draw);
            if (g_mode.load() != 1) break;
            const double now = NowS();
            TickArt(now, VitaConsole::OwnsDisplay());
            if (BootState::OnThreadTick(g_m, now)) { EndLocked(now); break; }   // (the safety net: neither the display callback nor a game present ended it)
            if (SlowStart(now) && !g_slowNoted.exchange(true)) {
                char note[96];
                std::snprintf(note, sizeof note, "slow start: still loading after %.0f s at \"%s\"", g_hintAfterS, g_milestone.load() ? g_milestone.load() : "the start");
                VitaLog::Note(note);
            }
            const bool console = VitaConsole::OwnsDisplay();
            if (!g_scratch) {
                // (no off-screen buffer: the old way, in the shown buffer)
                if (console && !g_paused.load()) {
                    if (uint32_t* fb = const_cast<uint32_t*>(VitaConsole::Pixels())) {
                        if (g_finishS >= 0.0 || g_artImg) ComposeFull(fb, now); else ComposeRegion(fb, now);
                    }
                } else if (!console && g_front) {
                    ComposeRegion(static_cast<uint32_t*>(g_front), now);
                }
            } else {
                full = console && (g_finishS >= 0.0 || g_artImg);   // (phase A's fade darkens the whole screen; so does the made picture's fade in: the whole background changes)
                if (full) ComposeFull(g_scratch, now); else ComposeRegion(g_scratch, now);
                MaybeShot(g_scratch, now);
            }
        }
        sceDisplayWaitVblankStart();
        sceDisplayWaitVblankStart();
        std::lock_guard<std::mutex> lock(g_draw);
        if (g_mode.load() != 1) break;
        if (!g_scratch) continue;
        if (VitaConsole::OwnsDisplay()) {
            if (g_paused.load()) continue;
            uint32_t* fb = const_cast<uint32_t*>(VitaConsole::Pixels());
            if (!fb) continue;
            if (full) std::memcpy(fb, g_scratch, static_cast<size_t>(W) * H * 4u);
            else CopyRows(fb, g_scratch, kRegY0, kRegY1, kRegX0, kRegX1);
        } else if (g_front) {
            CopyRows(static_cast<uint32_t*>(g_front), g_scratch, kRegY0, kRegY1, kRegX0, kRegX1);
        }
    }
    {
        // done: the 2 MiB off-screen buffer goes back to the heap (every user checks g_scratch under g_draw)
        std::lock_guard<std::mutex> lock(g_draw);
        std::free(g_scratch);
        g_scratch = nullptr;
        std::free(g_artImg);       // (a made picture the screen did not get to show; the builder frees one that comes later: it checks the mode under g_draw)
        std::free(g_artPending);
        g_artImg = g_artPending = nullptr;
        // (a screen that ended for good gives its picture back too: every user checks the mode under g_draw first)
        if (g_mode.load() == 3) {
            g_bgReady.store(false, std::memory_order_release);
            std::free(g_bg);
            g_bg = nullptr;
        }
    }
    Log("[boot] loading screen thread ends, its buffers are freed  [+%lu ms]\n", static_cast<unsigned long>(NowS() * 1000.0));
    return 0;
}
#endif

const char* ModeName(int m) { return m == 1 ? "art" : (m == 2 ? "text" : (m == 3 ? "finished" : "undecided")); }

} // namespace

bool Active() { return g_mode.load(std::memory_order_relaxed) == 1; }

void Begin() {
    if (g_mode.load() != 0) return;
    if (const char* e = std::getenv("MSC_BOOT_HINT"); e && *e) g_hintAfterS = std::atof(e);
#if defined(NP_PLAYER)
    bool art = true;
#else
    bool art = false;
#endif
    if (const char* e = std::getenv("MSC_BOOT_SCREEN"); e && *e) art = std::strcmp(e, "text") != 0;
#if defined(__vita__)
    if (art) {
        g_m.mode = BootState::kArt;
        g_mode.store(1);
        g_thread = sceKernelCreateThread("np_bootscreen", Thread, 0x10000100 + 20, 64 * 1024, 0, 0x40000, nullptr);   // core 2, below the game's threads
        if (g_thread < 0 || sceKernelStartThread(g_thread, 0, nullptr) < 0) {
            Log("[boot] loading screen thread not started (0x%X): the text console instead\n", static_cast<unsigned>(g_thread));
            g_m.mode = BootState::kText;
            g_mode.store(2);
            art = false;
        }
    }
#else
    art = false;
#endif
    if (!art) {
        g_mode.store(2);
        VitaConsole::ShowText();
#if defined(__vita__)
        if (VitaLiveAreaArt::NeedsApply()) {
            const uint32_t artFlags = 2u;
            g_artThread = sceKernelCreateThread("np_bootart", ArtThread, 0x10000100 + 30, 96 * 1024, 0, 0x40000, nullptr);
            if (g_artThread >= 0) sceKernelStartThread(g_artThread, sizeof artFlags, const_cast<uint32_t*>(&artFlags));
        }
#endif
    }
    Log("[boot] start-up screen: %s (MSC_BOOT_SCREEN=art|text)\n", ModeName(g_mode.load()));
}

// A new milestone: the line under the bar and one log line (the same milestone again, from a repeating progress report, is nothing).
static void Milestone(const char* text) {
    if (g_mode.load(std::memory_order_relaxed) == 3) {
        std::lock_guard<std::mutex> lock(g_draw);
        if (BootState::Guard(g_m)) Log("[boot] guard: a milestone (%s) was sent to the loading screen after it ended for good: nothing is drawn\n", text);
        return;
    }
    if (g_milestone.exchange(text, std::memory_order_relaxed) == text) return;
    Log("[boot] milestone: %s  [+%lu ms]\n", text, static_cast<unsigned long>(NowS() * 1000.0));
}

// np_boot_io.h: a file the start-up needs did not open or read and is being tried again (for up to 3 s): the line under the bar says so (the Tr table keeps the
// English text when a language has no entry), instead of a bar that seems to hang.
void Retrying() { Milestone("RETRYING THE GAME FILES"); }

void Notice(const char* text) {
    if (!text || !*text) return;
    if (g_mode.load(std::memory_order_relaxed) == 3) {
        std::lock_guard<std::mutex> lock(g_draw);
        if (BootState::Guard(g_m)) Log("[boot] guard: a notice was sent to the loading screen after it ended for good (%s): nothing is drawn\n", text);
        return;
    }
    g_notice.store(text, std::memory_order_relaxed);
    Log("[boot] notice on the loading screen: %s\n", text);
}

void Stage(const char* text) {
    if (!text) return;
    struct Anchor { const char* prefix; float base, cap, tau; const char* milestone; };
    static const Anchor kAnchors[] = {
        {"C++ static", 0.02f, 0.05f, 0.4f, "STARTING"},
        {"platform start-up", 0.05f, 0.09f, 0.6f, "STARTING"},
        {"checking the data", 0.09f, 0.10f, 0.3f, "CHECKING THE GAME FILES"},
        {"first-run", 0.10f, 0.11f, 0.3f, "CHECKING THE GAME FILES"},
        {"the native save", 0.11f, 0.12f, 0.3f, "CHECKING THE SAVE"},
        {"installing the crash", 0.12f, 0.13f, 0.2f, "STARTING"},
        {"starting the helper", 0.13f, 0.14f, 0.2f, "STARTING"},
        {"wiring", 0.14f, 0.20f, 1.2f, "STARTING"},
        {"vitaGL start-up", 0.20f, 0.26f, 0.8f, "STARTING THE GRAPHICS"},
        {"the game's start-up", 0.26f, 0.32f, 1.0f, "LOADING THE GAME"},
    };
    for (const Anchor& a : kAnchors)
        if (std::strncmp(text, a.prefix, std::strlen(a.prefix)) == 0) {
            Milestone(a.milestone);
            if (g_mode.load(std::memory_order_relaxed) == 1) SetAnchor(a.base, a.cap, a.tau);
            return;
        }
}

void Progress(int kind, unsigned done, unsigned total) {
    if (g_mode.load(std::memory_order_relaxed) != 1) return;
    if (kind == kSeedCopy) {
        Milestone("PREPARING THE SHADERS");
        SetAnchor(0.15f, 0.20f, 15.0f);   // (the copy is the fallback path: its length is not known in advance)
    } else if (kind == kWarmup && total != 0 && !g_gameStage) {
        Milestone("PREPARING THE SHADERS");
        const float f = static_cast<float>(done > total ? total : done) / static_cast<float>(total);
        SetAnchor(0.32f + 0.30f * f, 0.32f + 0.30f * f + 0.02f, 0.5f);
    } else if (kind == kPresent && done >= 2 && !g_gameStage) {
        // the renderer's second picture: the warm-up of frame 1 is over; the game's own start follows, about 80 frames to its
        // first picture (console log of 17 Oct)
        g_gameStage = true;
        Milestone("LOADING THE GAME");
        SetAnchor(0.62f, 0.97f, 2.4f);
    }
}

void Frame(uint64_t frames) {
    if (g_mode.load(std::memory_order_relaxed) != 1) return;
    if (g_finishPosted.load(std::memory_order_acquire)) {
        // a game picture after Finish(), whichever path showed it (the display callback's Compose is not always called: the ladder, a long present, a stall): once the fade time has passed
        // the loading screen ends here for good
        bool ended = false;
        {
            std::lock_guard<std::mutex> lock(g_draw);
            const double now = NowS();
            ended = BootState::OnGamePresent(g_m, now);
            if (ended) EndLocked(now);
        }
#if defined(__vita__)
        if (ended && g_thread >= 0) {
            SceUInt timeout = 300000;   // the thread leaves its loop within two vblanks
            sceKernelWaitThreadEnd(g_thread, nullptr, &timeout);
            sceKernelDeleteThread(g_thread);
            g_thread = -1;
        }
#endif
        if (ended) return;
    }
    if (g_frameBase == 0) g_frameBase = frames;
    // A game that never shows its boot screens (a test scenario) must not keep the loading screen forever.
    if (frames - g_frameBase == 900) Finish("900 frames without the game's boot screens");
}

void Finish(const char* why) {
    if (g_mode.load() != 1) return;
    {
        std::lock_guard<std::mutex> lock(g_prog);
        if (g_finishS >= 0.0) return;
        g_finishS = NowS();
    }
    {
        std::lock_guard<std::mutex> lock(g_draw);
        BootState::Finish(g_m, NowS());
    }
    g_finishPosted.store(true, std::memory_order_release);
    Log("[boot] loading screen ends: %s  [+%lu ms]\n", why ? why : "", static_cast<unsigned long>(NowS() * 1000.0));
}

// The renderer's display callback (phase B): the whole loading screen into the buffer about to be shown.
static int ComposeLocked(void* framebuffer);

int Compose(void* framebuffer) {
    if (!framebuffer || g_mode.load(std::memory_order_relaxed) != 1) return 0;
    if (!ComposeLocked(framebuffer)) return 0;
#if defined(__vita__)
    // (17 Oct) Show this buffer now and wait for the vblank it appears at, before vitaGL's display queue goes on. vgx runs vitaGL without vsync, so the
    // queue released the buffer shown before as soon as this callback and vitaGL's own sceDisplaySetFrameBuf(NEXTFRAME) returned, while that buffer
    // stayed on screen until the next vblank; the game's frames under the loading screen are black and quick, so the GPU drew the next one into the
    // buffer being shown: black flicker on the maintainer console once the faster picture copy (above) let the queue run at full speed. (vitaGL then sets the
    // same buffer again: no change.)
    SceDisplayFrameBuf fb;
    std::memset(&fb, 0, sizeof fb);
    fb.size = sizeof fb;
    fb.base = framebuffer;
    fb.pitch = W;
    fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    fb.width = W;
    fb.height = H;
    sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
    sceDisplayWaitVblankStart();
#endif
    return 1;
}

// Compose's work under the drawing lock: 1 = the loading screen is in `framebuffer`.
static int ComposeLocked(void* framebuffer) {
    std::lock_guard<std::mutex> lock(g_draw);
    if (g_mode.load() != 1) return 0;
    const double now = NowS();
    BootState::OnCompose(g_m, now);   // (from here on g_m.mode == kDone when the fade is over: this picture is the last)
    if (g_scratch) {
        // composed in cached memory, then one write-only copy (blending in the CDRAM buffer read it back pixel by pixel)
        ComposeFull(g_scratch, now);
        std::memcpy(framebuffer, g_scratch, static_cast<size_t>(W) * H * 4u);
    } else {
        ComposeFull(static_cast<uint32_t*>(framebuffer), now);
    }
    g_front = framebuffer;
#if defined(__vita__)
    MaybeShot(g_scratch ? g_scratch : static_cast<uint32_t*>(framebuffer), now);
#endif
    if (g_m.mode == BootState::kDone) EndLocked(now);   // faded out: from the next picture on the game shows
    return 1;
}

void Pause() {
    std::lock_guard<std::mutex> lock(g_draw);
    g_paused.store(true);
}
void Resume() {
    std::lock_guard<std::mutex> lock(g_draw);
    g_paused.store(false);
#if defined(__vita__)
    if (g_mode.load() == 1 && VitaConsole::OwnsDisplay()) {
        // (the screen is only redrawn before Finish(): after it the console side taking the display back ends the loading screen instead of drawing it again)
        if (!BootState::OnResume(g_m, NowS())) {
            if (g_m.mode == BootState::kDone) EndLocked(NowS());
            return;
        }
        uint32_t* fb = const_cast<uint32_t*>(VitaConsole::Pixels());
        if (fb) {
            if (g_scratch) { ComposeFull(g_scratch, NowS()); std::memcpy(fb, g_scratch, static_cast<size_t>(W) * H * 4u); }
            else ComposeFull(fb, NowS());
            ScanoutConsole();
        }
    }
#endif
}
void Stop() {
    if (g_mode.load() != 1) return;
#if defined(__vita__)
    g_artCancel = true;
#endif
    g_stopThread.store(true);
    std::lock_guard<std::mutex> lock(g_draw);
    g_m.mode = BootState::kDone;
    g_m.endedBy = "a fatal report has the screen";
    g_mode.store(3);
    g_front = nullptr;
}

// Tests (host): the background and one frame of the screen at a given bar value and time.
bool TestLoad(const char* path) {
    static uint32_t* bg = static_cast<uint32_t*>(std::malloc(static_cast<size_t>(W) * H * 4u));
    if (!bg) return false;
    g_haveArt = path && LoadArt(path, bg);
    if (!g_haveArt) MakePlainBackground(bg);
    g_bg = bg;
    g_bgReady.store(true);
    return g_haveArt;
}
void TestRender(uint32_t* fb, float shown, double now, float brightness) {
    std::memcpy(fb, g_bg, static_cast<size_t>(W) * H * 4u);
    DrawElements(fb, shown, now);
    if (brightness < 0.999f) Darken(fb, 0, H, brightness);
}

// Tests (host): the made picture's fade. `picture` is a loading.bin image (the header and the RGB565 pixels); seconds is the time since the fade began.
void TestFade(const unsigned char* picture, double seconds, uint32_t* fb, bool finish) {
    if (!g_artImg) {
        g_artImg = static_cast<uint8_t*>(std::malloc(VitaBootArt::kCacheBytes));
        if (!g_artImg) return;
        std::memcpy(g_artImg, picture, VitaBootArt::kPictureBytes);
        g_fadeStartS = 0.0;
    }
    BackgroundInto(fb, seconds);
    if (finish) FinalizeArt();
}

} // namespace VitaBootScreen

// The stdio lock's start-up retry (np_stdio_lock.cpp) calls this while it waits to try a file again; from any thread.
extern "C" void NpBootScreenNotice(void) { VitaBootScreen::Retrying(); }
