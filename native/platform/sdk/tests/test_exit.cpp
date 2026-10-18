// Process exit while handler threads run: an AX frame callback and an alarm use a global object; its destructor
// (which runs during exit, after main) destroys that object. The SDK layer's exit handler must have stopped the
// handlers by then, or a callback would touch freed state (the test aborts if one does).
#include <revolution/ai.h>
#include <revolution/ax.h>
#include <revolution/mix.h>
#include <revolution/os.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "np/np_plat.h"
#include "np/np_sdk.h"

struct Game {
    volatile int alive;
    int* data;
    Game() : alive(1), data((int*)calloc(4096, sizeof(int))) {}
    ~Game() {
        alive = 0;
        free(data);
        data = 0;
        // give a straggling handler time to touch the object: with the exit handler in place none runs
        np_sleep_ns(30000000);
    }
};
static Game g_game;
static volatile int g_cb = 0;

static void touch() {
    if (!g_game.alive || !g_game.data) {
        fprintf(stderr, "FAIL: a handler ran after the game's destructor\n");
        abort();
    }
    for (int i = 0; i < 64; ++i) g_game.data[i]++;
    ++g_cb;
}
static int sink_open(uint32_t) { return 0; }
static void sink_write(const int16_t*, uint32_t frames) { np_sleep_ns((uint64_t)frames * 1000000000ull / 32000 - 100000); }
static void sink_close(void) {}
static void ax_cb(void) { touch(); MIXUpdateSettings(); }
static void alarm_cb(OSAlarm*, OSContext*) { touch(); }

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
    static OSAlarm al;
    OSCreateAlarm(&al);
    OSSetPeriodicAlarm(&al, OSGetTime() + OSMillisecondsToTicks(1), OSMillisecondsToTicks(1), alarm_cb);
    OSSleepTicks(OSMillisecondsToTicks(150));
    printf("  %d callbacks before exit\n", (int)g_cb);
    printf("test_exit: ok\n");
    fflush(stdout);
    return 0;   // exit(): handlers still firing every millisecond
}
