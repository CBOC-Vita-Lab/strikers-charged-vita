// Video interface: the retrace clock, frame buffer latching and the game's VI calls. Behaviour
// follows the SDK (vi.c: VIConfigure/VISetNextFrameBuffer/VISetBlack take effect at the next
// retrace after VIFlush; VIWaitForRetrace sleeps on a thread queue the retrace handler wakes) and
// the recomp's HLE (hle/vi.cpp: the retrace interval, callback order pre -> latch -> post).
#include <revolution/os.h>
#include <revolution/vi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "np_internal.h"

namespace {

struct ViState {
    int inited;
    uint32_t retrace_count;
    void* cur_fb;
    int cur_black;
    GXRenderModeObj cur_mode;
    // pending (VIConfigure / VISetNextFrameBuffer / VISetBlack), latched by the first retrace after VIFlush
    void* pend_fb;
    int pend_black;
    GXRenderModeObj pend_mode;
    int flush_armed;
    VIRetraceCallback pre_cb;
    VIRetraceCallback post_cb;
    OSThreadQueue wait_queue;
    int external;
    NpViRetraceHook hook;
    double hz;
    uint64_t next_ns;
    VITimeToDIM dim;
};
ViState g;

double hz_for_mode(const GXRenderModeObj* m) {
    uint32_t fmt = (m->tvInfo >> 2) & 7;
    return fmt == 1 ? 50.0 : 59.94;   // PAL 50, NTSC / MPAL / EURGB60 59.94
}

void fill_frame(NpViFrame* f) {
    f->xfb = g.cur_fb;
    f->black = g.cur_black;
    f->retrace_count = g.retrace_count;
    f->tv_info = g.cur_mode.tvInfo;
    f->fb_width = g.cur_mode.fbWidth;
    f->xfb_height = g.cur_mode.xfbHeight;
    f->vi_width = g.cur_mode.viWidth;
    f->vi_height = g.cur_mode.viHeight;
    f->vi_x_origin = g.cur_mode.viXOrigin;
    f->vi_y_origin = g.cur_mode.viYOrigin;
    f->progressive = (g.cur_mode.tvInfo & 3) == 2;
    f->refresh_hz = g.hz;
}

void do_retrace() {
    NpViFrame frame;
    NpViRetraceHook hook;
    np_irq_enter();
    uint32_t count = g.retrace_count + 1;
    if (g.pre_cb) g.pre_cb(count);
    if (g.flush_armed) {
        g.cur_fb = g.pend_fb;
        g.cur_black = g.pend_black;
        if (g.pend_mode.tvInfo != g.cur_mode.tvInfo) g.hz = hz_for_mode(&g.pend_mode);
        g.cur_mode = g.pend_mode;
        g.flush_armed = 0;
    }
    g.retrace_count = count;
    if (g.post_cb) g.post_cb(count);
    OSWakeupThread(&g.wait_queue);
    fill_frame(&frame);
    hook = g.hook;
    np_irq_leave();
    if (hook) hook(&frame);
}

// Virtual time: the retrace is an event source on the grid t0 + k * interval (a run that fell more than four intervals behind
// skips ahead instead of delivering a burst, like the real-time thread).
int g_vic_id = -1;
void vic_retrace(void*, uint64_t due) {
    do_retrace();
    const uint64_t interval = (uint64_t)(1e9 / g.hz + 0.5);
    g.next_ns = due + interval;
    const uint64_t now = np_vt_now();
    if (now > g.next_ns + 4 * interval) g.next_ns = now;
    np_vic_arm(g_vic_id, g.next_ns);
}

// The retrace follows the display (W5 FPS): where the platform has a vblank clock (the console) the thread waits for the real vblank and delivers
// one retrace per vblank, so the game's retrace waits, the movies' retrace counts and the picture's refreshes are the same beat and a frame
// held for two retraces is shown for two refreshes. A thread that woke late delivers the vblanks it missed one after the other (at most
// four, as the timer does), a longer stall skips ahead. NP_VI_VBLANK=0 keeps the 59.94 Hz timer; a PAL mode (50 Hz) also keeps it, and so does a
// wait that fails (screen off): the timer runs for that retrace. [vi] lines: the measured rate of the display, so the console shows it.
struct VblankStats {
    int64_t last;               // vcount of the last delivered retrace, -1 before the first
    uint64_t n, late, skipped;  // retraces delivered, bursts of catch-up retraces, stalls that skipped ahead
    uint64_t logN, logT, every; // the window of the next [vi] line
};
VblankStats vbs = {-1, 0, 0, 0, 0, 0, 600};

// One vblank-driven step. False: no vblank clock this time (the caller runs its timer).
bool vblank_step() {
    const int64_t v = np_display_vblank_wait();
    if (v < 0) { vbs.last = -1; return false; }
    const uint64_t now = np_time_real_ns();
    uint32_t behind = 1;
    if (vbs.last >= 0) behind = (uint32_t)v - (uint32_t)vbs.last;
    vbs.last = v;
    if (behind == 0) behind = 1;   // (a counter that did not move: the wait returned at once)
    if (behind > 4) { ++vbs.skipped; behind = 1; }
    else if (behind > 1) ++vbs.late;
    g.next_ns = now;               // the timer, if it has to take over, starts from here
    for (uint32_t i = 0; i < behind; ++i) {
        if (vbs.n == 0) { vbs.logN = 0; vbs.logT = now; }
        do_retrace();
        ++vbs.n;
    }
    if (vbs.n - vbs.logN >= vbs.every) {
        const double secs = (double)(now - vbs.logT) * 1e-9;
        np_log("vi", "retrace follows the display's vblank: %llu retraces in %.2f s = %.3f Hz (%llu bursts of late retraces, %llu stalls skipped, all so far)",
               (unsigned long long)(vbs.n - vbs.logN), secs, secs > 0 ? (double)(vbs.n - vbs.logN) / secs : 0.0, (unsigned long long)vbs.late,
               (unsigned long long)vbs.skipped);
        vbs.logN = vbs.n;
        vbs.logT = now;
        vbs.every = 6000;
    }
    return true;
}

void* retrace_thread(void*) {
    g.next_ns = np_time_real_ns();
    const char* env = getenv("NP_VI_VBLANK");
    const bool want_vblank = !(env && env[0] == '0');
    for (;;) {
        if (g.external) {
            np_sleep_ns(2000000);
            g.next_ns = np_time_real_ns();
            continue;
        }
        if (want_vblank && g.hz > 55.0 && vblank_step()) continue;
        uint64_t interval = (uint64_t)(1e9 / g.hz + 0.5);
        g.next_ns += interval;
        uint64_t now = np_time_real_ns();
        if (now > g.next_ns + 4 * interval) {   // the machine stalled: do not deliver a burst
            g.next_ns = now;
        } else if (g.next_ns > now) {
            uint64_t d = g.next_ns - now;
            if (d > 1500000) np_sleep_ns(d - 1000000);   // leave 1 ms to a short spin
            while (np_time_real_ns() < g.next_ns) np_yield();
        }
        do_retrace();
    }
    return 0;
}

}  // namespace

extern "C" {

void NpSdk_SetViRetraceHook(NpViRetraceHook hook) { g.hook = hook; }
void NpSdk_SetViExternalRetrace(int external) { g.external = external; }
void NpVI_Retrace(void) { if (g.inited) do_retrace(); }
void NpVI_GetFrame(NpViFrame* out) {
    BOOL en = OSDisableInterrupts();
    fill_frame(out);
    OSRestoreInterrupts(en);
}

void VIInit(void) {
    if (g.inited) return;
    np_os_lazy_init();
    g.inited = 1;
    OSInitThreadQueue(&g.wait_queue);
    memset(&g.cur_mode, 0, sizeof(g.cur_mode));
    g.cur_mode.tvInfo = (u32)((NpSdk_GetConfig()->tv_format << 2) | 1);
    g.cur_mode.fbWidth = 640;
    g.cur_mode.efbHeight = 480;
    g.cur_mode.xfbHeight = 480;
    g.cur_mode.viWidth = 640;
    g.cur_mode.viHeight = 480;
    g.pend_mode = g.cur_mode;
    g.hz = hz_for_mode(&g.cur_mode);
    g.cur_black = g.pend_black = 1;   // the console starts with a black screen
    g.dim = VI_DM_DEFAULT;
    if (np_vt_on) {
        g_vic_id = np_vic_add("retrace", vic_retrace, 0);
        g.next_ns = np_vt_now() + (uint64_t)(1e9 / g.hz + 0.5);
        np_vic_arm(g_vic_id, g.next_ns);
        return;
    }
    NpThread* t = np_thread_create("vi-retrace", NP_ROLE_TIMER, retrace_thread, 0, 0, 262144);
    if (t) np_thread_detach(t);
}

VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback cb) {
    BOOL en = OSDisableInterrupts();
    VIRetraceCallback old = g.pre_cb;
    g.pre_cb = cb;
    OSRestoreInterrupts(en);
    return old;
}
VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback cb) {
    BOOL en = OSDisableInterrupts();
    VIRetraceCallback old = g.post_cb;
    g.post_cb = cb;
    OSRestoreInterrupts(en);
    return old;
}

void VIWaitForRetrace(void) {
    if (!g.inited) VIInit();
    BOOL en = OSDisableInterrupts();
    uint32_t start = g.retrace_count;
    do {
        OSSleepThread(&g.wait_queue);
    } while (start == g.retrace_count);
    OSRestoreInterrupts(en);
}

void VIConfigure(const GXRenderModeObj* rmo) {
    if (!g.inited) VIInit();
    BOOL en = OSDisableInterrupts();
    g.pend_mode = *rmo;
    OSRestoreInterrupts(en);
    if (np_gx_set_efb_size) np_gx_set_efb_size(rmo->fbWidth, rmo->efbHeight);   // the renderer's logical EFB size
}
void VIConfigurePan(unsigned short x, unsigned short y, unsigned short w, unsigned short h) {
    (void)x; (void)y; (void)w; (void)h;
}
void VIFlush(void) {
    if (!g.inited) VIInit();
    BOOL en = OSDisableInterrupts();
    g.flush_armed = 1;
    OSRestoreInterrupts(en);
}
void VISetNextFrameBuffer(void* fb) {
    if (!g.inited) VIInit();
    BOOL en = OSDisableInterrupts();
    g.pend_fb = fb;
    OSRestoreInterrupts(en);
}
void* VIGetCurrentFrameBuffer(void) { return g.cur_fb; }
void VISetBlack(int black) {
    if (!g.inited) VIInit();
    BOOL en = OSDisableInterrupts();
    g.pend_black = black ? 1 : 0;
    OSRestoreInterrupts(en);
}
// The count without the virtual-time side effects (the input layer keys its samples on it).
uint32_t NpVI_GetRetraceCount(void) { return g.retrace_count; }
unsigned long VIGetRetraceCount(void) {
    np_vt_poll_api();   // (virtual time) a game that spins on the retrace count sees time pass
    return g.retrace_count;
}
unsigned long VIGetCurrentLine(void) { return 0; }
VITvFormat VIGetTvFormat(void) {
    if (!g.inited) return (VITvFormat)NpSdk_GetConfig()->tv_format;
    return (VITvFormat)((g.cur_mode.tvInfo >> 2) & 7);
}
unsigned long VIGetDTVStatus(void) { return 1; }   // component cable: progressive allowed
VITimeToDIM VISetTimeToDimming(VITimeToDIM time) {
    VITimeToDIM old = g.dim;
    g.dim = time;
    return old;
}
void __VIInit(VITVMode mode) { (void)mode; VIInit(); }
void __VIDisplayPositionToXY(unsigned long hct, unsigned long vct, short* x, short* y) {
    *x = (short)hct;
    *y = (short)vct;
}
int __VIResetRFIdle(void) { return 1; }
int __VIResetSIIdle(void) { return 1; }

}  // extern "C"
