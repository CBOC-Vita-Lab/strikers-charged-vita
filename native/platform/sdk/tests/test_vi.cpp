// Unit tests of VI: retrace rate, callback order, latching of frame buffers at the first retrace
// after VIFlush, VIWaitForRetrace.
#include <revolution/os.h>
#include <revolution/vi.h>

#include <stdio.h>
#include <string.h>

#include "np/np_sdk.h"

static int g_fail = 0;
#define CHECK(c)                                                                    \
    do {                                                                            \
        if (!(c)) {                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                     \
            g_fail++;                                                               \
        }                                                                           \
    } while (0)

static volatile int order_pre = 0, order_post = 0;
static volatile unsigned long last_pre = 0, last_post = 0;
static void pre(unsigned long n) { last_pre = n; order_pre++; CHECK(order_pre == order_post + 1); }
static void post(unsigned long n) { last_post = n; order_post++; CHECK(order_pre == order_post); }
static void* hook_fb = (void*)1;
static volatile int hook_black = -1;
static void on_hook(const NpViFrame* f) { hook_fb = f->xfb; hook_black = f->black; }

int main() {
    NpSdkConfig cfg;
    NpSdk_ConfigDefaults(&cfg);
    NpSdk_Init(&cfg);
    VIInit();
    VISetPreRetraceCallback(pre);
    VISetPostRetraceCallback(post);
    NpSdk_SetViRetraceHook(on_hook);
    CHECK(VIGetTvFormat() == 0);
    CHECK(VIGetDTVStatus() == 1);

    unsigned long c0 = VIGetRetraceCount();
    OSTime t0 = OSGetTime();
    for (int i = 0; i < 30; ++i) VIWaitForRetrace();
    OSTime t1 = OSGetTime();
    double ms = (double)OSTicksToMicroseconds(t1 - t0) / 1000.0;
    unsigned long n = VIGetRetraceCount() - c0;
    printf("  %lu retraces in %.1f ms (%.2f Hz)\n", n, ms, n * 1000.0 / ms);
    CHECK(n >= 30);
    CHECK(n * 1000.0 / ms > 50.0 && n * 1000.0 / ms < 66.0);   // loose: the machine may be loaded
    CHECK(last_post == last_pre || last_post + 1 == last_pre);   // a retrace may be in flight

    // the black start-up state; a frame buffer only shows after VIFlush + a retrace
    static char fb[2][64];
    VISetNextFrameBuffer(fb[0]);
    VIWaitForRetrace();
    VIWaitForRetrace();
    CHECK(VIGetCurrentFrameBuffer() != fb[0]);   // not flushed yet
    VISetBlack(0);
    VIFlush();
    VIWaitForRetrace();
    CHECK(VIGetCurrentFrameBuffer() == fb[0]);
    for (int i = 0; i < 50 && !(hook_fb == fb[0] && hook_black == 0); ++i) OSSleepTicks(OSMillisecondsToTicks(2));
    CHECK(hook_fb == fb[0] && hook_black == 0);
    VISetNextFrameBuffer(fb[1]);
    VISetBlack(1);
    VIWaitForRetrace();
    CHECK(VIGetCurrentFrameBuffer() == fb[0]);
    VIFlush();
    VIWaitForRetrace();
    CHECK(VIGetCurrentFrameBuffer() == fb[1]);
    for (int i = 0; i < 50 && hook_black != 1; ++i) OSSleepTicks(OSMillisecondsToTicks(2));
    CHECK(hook_black == 1);

    // PAL timing from the render mode
    GXRenderModeObj pal;
    memset(&pal, 0, sizeof pal);
    pal.tvInfo = (1u << 2) | 1;
    pal.fbWidth = 640; pal.xfbHeight = 480; pal.viWidth = 640; pal.viHeight = 480;
    VIConfigure(&pal);
    VIFlush();
    VIWaitForRetrace(); VIWaitForRetrace();
    c0 = VIGetRetraceCount();
    t0 = OSGetTime();
    for (int i = 0; i < 25; ++i) VIWaitForRetrace();
    t1 = OSGetTime();
    ms = (double)OSTicksToMicroseconds(t1 - t0) / 1000.0;
    n = VIGetRetraceCount() - c0;
    printf("  PAL: %lu retraces in %.1f ms (%.2f Hz)\n", n, ms, n * 1000.0 / ms);
    CHECK(n * 1000.0 / ms > 42.0 && n * 1000.0 / ms < 56.0);
    CHECK(VIGetTvFormat() == 1);

    if (g_fail) { printf("%d check(s) FAILED\n", g_fail); return 1; }
    printf("test_vi: ok\n");
    return 0;
}
