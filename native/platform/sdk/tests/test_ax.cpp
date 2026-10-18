// AX end to end: the game's start-up (AIInit, AXInit, MIXInit, AXRegisterCallback), one PCM16 voice
// playing a sine wave from sample memory, one ADPCM-free check of the output through a capturing sink.
#include <revolution/ai.h>
#include <revolution/ax.h>
#include <revolution/axfx.h>
#include <revolution/mix.h>
#include <revolution/os.h>

#include <math.h>
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

// capturing sink (paced like the device: 3 ms blocks take 3 ms)
static int16_t* g_cap = 0;
static uint32_t g_cap_frames = 0;
static const uint32_t kCapMax = 32000 * 4;
static int sink_open(uint32_t rate) { printf("  sink opened at %u Hz\n", rate); return 0; }
static void sink_write(const int16_t* lr, uint32_t frames) {
    if (g_cap_frames + frames <= kCapMax) {
        memcpy(g_cap + g_cap_frames * 2, lr, frames * 4);
        g_cap_frames += frames;
    }
    np_sleep_ns((uint64_t)frames * 1000000000ull / 32000 - 100000);
}
static void sink_close(void) {}



extern "C" void MscAxPrintStats(void);
static volatile int g_callbacks = 0;
static void frame_cb(void) {
    g_callbacks++;
    MIXUpdateSettings();
}

static uint16_t be16(int16_t v) { uint16_t u = (uint16_t)v; return (uint16_t)((u << 8) | (u >> 8)); }

int main() {
    g_cap = (int16_t*)calloc(kCapMax * 2, sizeof(int16_t));
    NpSdkConfig cfg;
    NpSdk_ConfigDefaults(&cfg);
    NpSdk_Init(&cfg);
    NpAudioSink sink = {sink_open, sink_write, sink_close};
    NpSdk_SetAudioSink(&sink);

    AIInit(0);
    AXInit();
    MIXInit();
    AXRegisterCallback(frame_cb);
    MIXSetSoundMode(MIX_SOUND_MODE_STEREO);
    AXSetMode(0);

    // sample memory from the MEM2 arena (high addresses)
    const int N = 3200;   // 0.1 s at 32 kHz; 1000 Hz sine = 32 samples per period
    int16_t* samples = (int16_t*)OSAllocFromMEM2ArenaLo(N * 2, 32);
    CHECK(((unsigned long)samples & 0x80000000ul) != 0);
    for (int i = 0; i < N; ++i) samples[i] = (int16_t)be16((int16_t)(12000.0 * sin(2.0 * 3.14159265358979 * 1000.0 * i / 32000.0)));

    AXVPB* v = AXAcquireVoice(15, 0, 0);
    CHECK(v != 0);
    if (!v) return 1;
    MIXInitChannel(v, 0, 0, -960, -960, -960, 64, 127, 0);
    unsigned long start = (unsigned long)samples / 2;           // PCM16: sample addresses
    unsigned long end = start + N - 1;
    AXPBADDR addr;
    memset(&addr, 0, sizeof addr);
    addr.loopFlag = 1;
    addr.format = AX_SAMPLE_FORMAT_PCM_S16;
    addr.loopAddressHi = start >> 16; addr.loopAddressLo = start & 0xFFFF;
    addr.endAddressHi = end >> 16;    addr.endAddressLo = end & 0xFFFF;
    addr.currentAddressHi = start >> 16; addr.currentAddressLo = start & 0xFFFF;
    AXPBADPCM adpcm;
    memset(&adpcm, 0, sizeof adpcm);
    adpcm.gain = 0x800;   // unity for PCM16 (the game's SP library writes this too)
    AXSetVoiceAdpcm(v, &adpcm);
    AXSetVoiceType(v, AX_VOICE_NORMAL);
    AXSetVoiceAddr(v, &addr);
    AXSetVoiceSrcType(v, AX_SRC_TYPE_NONE);
    AXSetVoiceSrcRatio(v, 1.0f);
    AXSetVoiceState(v, AX_VOICE_RUN);

    OSSleepTicks(OSMillisecondsToTicks(300));
    OSSleepTicks(OSMillisecondsToTicks(900));
    AXSetVoiceState(v, AX_VOICE_STOP);
    AXFreeVoice(v);

    if (getenv("MSC_AX_STATS")) MscAxPrintStats();
    NpAudioStats st;
    NpSdk_GetAudioStats(&st);
    printf("  %llu blocks, callbacks %d, mix+callback total %.1f ms, max %.2f ms\n", (unsigned long long)st.blocks,
           g_callbacks, st.mix_ns_total / 1e6, st.mix_ns_max / 1e6);
    CHECK(g_callbacks > 200);
    CHECK(g_cap_frames > 20000);
    // analyse: find a stretch with signal; count rising zero crossings of the left channel
    uint32_t best_start = 0; int peak = 0;
    for (uint32_t i = 0; i < g_cap_frames; ++i) { int a = abs(g_cap[i * 2]); if (a > peak) { peak = a; best_start = i; } }
    printf("  peak %d at frame %u\n", peak, best_start);
    if (getenv("MSC_AX_STATS")) {
        for (uint32_t b = 0; b + 4800 <= g_cap_frames; b += 4800) {
            int pk = 0; for (uint32_t i = b; i < b + 4800; ++i) { int a = abs(g_cap[i * 2]); if (a > pk) pk = a; int c = abs(g_cap[i * 2 + 1]); if (c > pk) pk = c; }
            printf("    frames %u..%u peak %d\n", b, b + 4800, pk);
        }
    }
    CHECK(peak > 1500);
    uint32_t a0 = best_start > 8000 ? best_start - 4000 : 2000, a1 = a0 + 16000 <= g_cap_frames ? a0 + 16000 : g_cap_frames;
    int rising = 0; double energy = 0, lr = 0;
    for (uint32_t i = a0 + 1; i < a1; ++i) {
        if (g_cap[(i - 1) * 2] < 0 && g_cap[i * 2] >= 0) rising++;
        energy += (double)g_cap[i * 2] * g_cap[i * 2];
        lr += (double)g_cap[i * 2] * g_cap[i * 2 + 1];
    }
    double secs = (double)(a1 - a0) / 32000.0;
    printf("  %d rising crossings in %.3f s = %.1f Hz\n", rising, secs, rising / secs);
    CHECK(rising / secs > 900 && rising / secs < 1100);
    CHECK(lr > 0);   // centred voice: left and right correlated

    if (g_fail) { printf("%d check(s) FAILED\n", g_fail); return 1; }
    printf("test_ax: ok\n");
    return 0;
}
