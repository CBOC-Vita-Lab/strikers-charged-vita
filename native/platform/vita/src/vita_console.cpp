// Text console on the display framebuffer: the null-graphics build's sign of
// life. 80x22 characters of a 12x24 font on the Vita's 960x544 screen. Rows
// 0-5 hold the title and status lines the platform layer sets; the rest shows
// the tail of the log.
//
// The framebuffer is a single 960x544 A8B8G8R8 surface handed to the display
// with sceDisplaySetFrameBuf (no GXM). Only rows whose text changed are
// redrawn, and redraws are rate-limited by the callers, because all of this
// runs on the game thread.
#include "plat_realclock.h"
#include "np_vita_internal.h"
#include "vita_font.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#if defined(__vita__)
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#else
#include <chrono>
#include <pthread.h>
#include <vector>
#endif

namespace VitaConsole {
namespace {

static_assert(kColumns * kVitaFontWidth == kWidth, "80 columns of 12 pixels fill the screen");
static_assert(kRows * kVitaFontHeight <= kHeight, "rows must fit the screen");

constexpr uint32_t kBackground = 0xFF201810u;   // 0xAABBGGRR: dark blue-grey
constexpr uint32_t kLogColor = 0xFFC8C8C8u;
constexpr int kTopMargin = (kHeight - kRows * kVitaFontHeight) / 2;

struct Row {
    char text[kColumns + 1];
    uint32_t color;
    bool dirty;
};

Row g_rows[kRows];
uint32_t* g_pixels = nullptr;
bool g_ready = false;
bool g_fatalShown = false;
bool g_screenTaken = false;               // a full-screen picture of the caller's own (TakeScreen)
std::atomic<bool> g_ownsDisplay{true};   // set by the renderer thread, read by the game thread (From Codex: it was a plain bool)
std::atomic<bool> g_text{false};          // W5 BOOT: the text is drawn only after ShowText() (the loading screen may have the framebuffer)
std::atomic<bool> g_needClear{false};
uint32_t g_tailGeneration = 0xFFFFFFFFu;
uint64_t g_lastRefreshMs = 0;
unsigned g_fatalWaitMs = 120 * 1000;

#if defined(__vita__)
SceUID g_block = -1;
SceUID g_ownerThread = -1;
uint64_t NowMs() { return sceKernelGetProcessTimeWide() / 1000u; }
bool OnOwnerThread() { return sceKernelGetThreadId() == g_ownerThread; }
void Scanout() {
    SceDisplayFrameBuf fb;
    std::memset(&fb, 0, sizeof(fb));
    fb.size = sizeof(fb);
    fb.base = g_pixels;
    fb.pitch = kWidth;
    fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    fb.width = kWidth;
    fb.height = kHeight;
    sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
}
#else
std::vector<uint32_t> g_storage;
pthread_t g_ownerThread;
uint64_t NowMs() {
    return PlatRealMonotonicUs() / 1000u;   // not the harness's virtual clock: plat_realclock.h
}
bool OnOwnerThread() { return pthread_equal(pthread_self(), g_ownerThread) != 0; }
void Scanout() {}
#endif

void DrawRow(int row) {
    const Row& r = g_rows[row];
    uint32_t* top = g_pixels + (kTopMargin + row * kVitaFontHeight) * kWidth;
    const int length = static_cast<int>(std::strlen(r.text));
    for (int y = 0; y < kVitaFontHeight; ++y) {
        uint32_t* out = top + y * kWidth;
        for (int column = 0; column < kColumns; ++column) {
            unsigned char c = column < length ? static_cast<unsigned char>(r.text[column]) : ' ';
            if (c < 32 || c > 126) c = '?';
            const uint16_t bits = kVitaFont[c - 32][y];
            for (int x = 0; x < kVitaFontWidth; ++x)
                *out++ = (bits & (1u << (kVitaFontWidth - 1 - x))) ? r.color : kBackground;
        }
    }
}

void SetText(int row, uint32_t rgb, const char* text) {
    if (row < 0 || row >= kRows) return;
    Row& r = g_rows[row];
    const uint32_t color = 0xFF000000u | ((rgb & 0xFF) << 16) | (rgb & 0xFF00) | ((rgb >> 16) & 0xFF);
    char clipped[kColumns + 1];
    std::strncpy(clipped, text, kColumns);
    clipped[kColumns] = '\0';
    if (r.color == color && std::strcmp(r.text, clipped) == 0) return;
    std::strcpy(r.text, clipped);
    r.color = color;
    r.dirty = true;
}

} // namespace

bool Init() {
    if (g_ready) return true;
#if defined(__vita__)
    // The display takes a framebuffer from the CDRAM partition; such blocks
    // are sized in multiples of 256 KiB (960*544*4 bytes rounds up to 2 MiB).
    const SceSize size = (kWidth * kHeight * 4 + 0x3FFFF) & ~0x3FFFFu;
    g_block = sceKernelAllocMemBlock("msc_console_fb", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, size, nullptr);
    void* base = nullptr;
    if (g_block < 0 || sceKernelGetMemBlockBase(g_block, &base) < 0) return false;
    g_pixels = static_cast<uint32_t*>(base);
    g_ownerThread = sceKernelGetThreadId();
#else
    g_storage.assign(static_cast<size_t>(kWidth) * kHeight, 0);
    g_pixels = g_storage.data();
    g_ownerThread = pthread_self();
#endif
    // Black and not yet shown: the start-up screen (VitaBootScreen::Begin) decides between the loading screen and the text.
    for (int i = 0; i < kWidth * kHeight; ++i) g_pixels[i] = 0xFF000000u;
    for (auto& row : g_rows) {
        row.text[0] = '\0';
        row.color = kLogColor;
        row.dirty = false;
    }
    g_ready = true;
    return true;
}

void ShowText() {
    g_needClear.store(true);
    g_text.store(true);
}

void SetLine(int row, uint32_t rgb, const char* format, ...) {
    char text[256];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    SetText(row, rgb, text);
}

void Refresh() {
    if (!g_ready || g_fatalShown || g_screenTaken || !g_text.load(std::memory_order_relaxed)) return;
    if (g_needClear.exchange(false)) {
        for (int i = 0; i < kWidth * kHeight; ++i) g_pixels[i] = kBackground;
        for (int row = 0; row < kRows; ++row) g_rows[row].dirty = true;
        g_tailGeneration = 0xFFFFFFFFu;
    }
    uint32_t generation = 0;
    static char tail[VitaLog::kTailLines][VitaLog::kTailColumns + 1];
    const int logRows = kRows - kStatusRows;
    const int count = VitaLog::Tail(tail, logRows < VitaLog::kTailLines ? logRows : VitaLog::kTailLines, &generation);
    if (generation != g_tailGeneration) {
        g_tailGeneration = generation;
        for (int i = 0; i < logRows; ++i)
            SetText(kStatusRows + i, 0xC8C8C8, i < count ? tail[i] : "");
    }
    bool any = false;
    for (int row = 0; row < kRows; ++row) {
        if (!g_rows[row].dirty) continue;
        DrawRow(row);
        g_rows[row].dirty = false;
        any = true;
    }
    if (any && g_ownsDisplay.load(std::memory_order_relaxed)) Scanout();
    g_lastRefreshMs = NowMs();
}

void RefreshThrottled(unsigned minIntervalMs) {
    if (!g_ready || g_fatalShown || g_screenTaken || !g_text.load(std::memory_order_relaxed) || !OnOwnerThread()) return;
    if (NowMs() - g_lastRefreshMs < minIntervalMs) return;
    Refresh();
}

void SetDisplayOwner(bool consoleOwnsDisplay) {
    g_ownsDisplay.store(consoleOwnsDisplay, std::memory_order_relaxed);
    if (consoleOwnsDisplay && g_ready && !g_fatalShown) Scanout();
}
bool OwnsDisplay() { return g_ownsDisplay.load(std::memory_order_relaxed); }

void ShowFatal(const char* category, const char* details) {
    if (!g_ready && !Init()) return;
    VitaBootScreen::Stop();   // (its thread no longer draws: the report has the screen)
    g_text.store(true);
    g_needClear.store(false);
    g_fatalShown = false;
    g_ownsDisplay.store(true, std::memory_order_relaxed);
    for (int row = 0; row < kRows; ++row) SetText(row, 0xC8C8C8, "");
    SetText(0, 0xFF6060, "Mario Strikers Charged stopped: fatal error");
    SetLine(1, 0xFFFFFF, "%s", category ? category : "");
    // Word-agnostic wrap of the details over the remaining rows.
    int row = 3;
    const char* p = details ? details : "";
    while (*p && row < kRows - 2) {
        char line[kColumns + 1];
        int n = 0;
        while (*p && *p != '\n' && n < kColumns) line[n++] = *p++;
        line[n] = '\0';
        if (*p == '\n') ++p;
        SetText(row++, 0xE0E0E0, line);
    }
    SetText(kRows - 1, 0xFFFF80, "The full log is ux0:data/mscharged/log.txt.  Press START to exit.");
    for (int r = 0; r < kRows; ++r) {
        DrawRow(r);
        g_rows[r].dirty = false;
    }
    Scanout();
    g_fatalShown = true;
#if defined(__vita__)
    // Leave the message up until START is pressed (at most two minutes, so an
    // unattended run still terminates).
    SceCtrlData pad;
    for (unsigned i = 0; i < g_fatalWaitMs / 50u; ++i) {
        std::memset(&pad, 0, sizeof(pad));
        if (sceCtrlPeekBufferPositive(0, &pad, 1) > 0 && (pad.buttons & SCE_CTRL_START)) break;
        Scanout();
        sceKernelDelayThread(50 * 1000);
    }
#endif
}

void SetFatalWaitMs(unsigned ms) { g_fatalWaitMs = ms; }

uint32_t* TakeScreen() {
    if (!g_ready && !Init()) return nullptr;
    VitaBootScreen::Pause();
    g_screenTaken = true;
    return g_pixels;
}
void PresentScreen() {
    if (g_ready) Scanout();
}
void ReleaseScreen() {
    if (!g_screenTaken) return;
    g_screenTaken = false;
    for (int row = 0; row < kRows; ++row) g_rows[row].dirty = true;
    g_tailGeneration = 0xFFFFFFFFu;
    if (VitaBootScreen::Active()) { VitaBootScreen::Resume(); return; }   // the loading screen again
    for (int i = 0; i < kWidth * kHeight; ++i) g_pixels[i] = kBackground;
}

const uint32_t* Pixels() { return g_pixels; }

bool DumpPpm(const char* path) {
    if (!g_ready) return false;
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%d %d\n255\n", kWidth, kHeight);
    for (int i = 0; i < kWidth * kHeight; ++i) {
        const uint32_t p = g_pixels[i];
        const unsigned char rgb[3] = {static_cast<unsigned char>(p), static_cast<unsigned char>(p >> 8),
                                      static_cast<unsigned char>(p >> 16)};
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
    return true;
}

} // namespace VitaConsole
