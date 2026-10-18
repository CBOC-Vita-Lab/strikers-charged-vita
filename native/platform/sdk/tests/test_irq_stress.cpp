// The "interrupt" lock under a worst-case game thread: a loop of OSDisableInterrupts / OSRestoreInterrupts
// without a pause must not starve the handlers (the AX frame callback every 3 ms, a 1 ms periodic alarm): on the console
// an enabled interrupt is taken at once, here the handler threads need the lock the game thread keeps taking.
#include <revolution/ai.h>
#include <revolution/ax.h>
#include <revolution/mix.h>
#include <revolution/os.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "np/np_plat.h"
#include "np/np_sdk.h"

static int g_fail = 0;
#define CHECK(c)                                                                    \
    do {                                                                            \
        if (!(c)) {                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                     \
            g_fail++;                                                               \
        }                                                                           \
    } while (0)

static volatile int g_ax_cb = 0, g_alarm_cb = 0;
static int sink_open(uint32_t) { return 0; }
static void sink_write(const int16_t*, uint32_t frames) { np_sleep_ns((uint64_t)frames * 1000000000ull / 32000 - 100000); }
static void sink_close(void) {}
static void ax_cb(void) { ++g_ax_cb; MIXUpdateSettings(); }
static void alarm_cb(OSAlarm*, OSContext*) { ++g_alarm_cb; }

int main() {
    NpSdkConfig cfg;
    NpSdk_ConfigDefaults(&cfg);
    NpSdk_Init(&cfg);
    NpAudioSink sink = {sink_open, sink_write, sink_close};
    NpSdk_SetAudioSink(&sink);
    OSInit();
    AIInit(0);
    AXInit();
    MIXInit();
    AXRegisterCallback(ax_cb);
    OSAlarm al;
    OSCreateAlarm(&al);
    OSSetPeriodicAlarm(&al, OSGetTime() + OSMillisecondsToTicks(1), OSMillisecondsToTicks(1), alarm_cb);

    uint64_t t0 = np_time_real_ns();
    unsigned long loops = 0;
    volatile int sink_var = 0;
    while (np_time_real_ns() - t0 < 1000000000ull) {
        for (int i = 0; i < 1000; ++i) {
            BOOL e = OSDisableInterrupts();
            sink_var = sink_var + 1;
            OSRestoreInterrupts(e);
        }
        loops += 1000;
    }
    double secs = (np_time_real_ns() - t0) / 1e9;
    OSCancelAlarm(&al);
    printf("  %.2f s: %lu disable/restore pairs, %d AX callbacks (%.0f/s), %d alarm callbacks (%.0f/s)\n", secs, loops, (int)g_ax_cb,
           g_ax_cb / secs, (int)g_alarm_cb, g_alarm_cb / secs);
    CHECK(g_ax_cb > 250);       // 333 per second on the console
    CHECK(g_alarm_cb > 700);    // 1000 per second
    if (g_fail) {
        printf("%d check(s) FAILED\n", g_fail);
        return 1;
    }
    printf("test_irq_stress: ok\n");
    return 0;
}
