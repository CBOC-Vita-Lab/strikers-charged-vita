// The default audio sink and the registration of a platform's sink. The default keeps real time (it
// sleeps for the duration of what it is given) and drops the samples; NP_AUDIO_WAV=<file> also
// writes them to a 16-bit stereo WAV (rewritten header at exit), which is how audio is checked
// without a sound device.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "np_internal.h"
#include "audio_out.h"

namespace {

NpAudioSink g_user;
bool g_have_user = false;

// default sink
FILE* g_wav = 0;
uint32_t g_wav_frames = 0;
uint32_t g_rate = 32000;
uint64_t g_clock_ns = 0;   // when the data written so far is due to have played out

void put32(unsigned char* p, uint32_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24); }
void put16(unsigned char* p, uint16_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }

void wav_header(FILE* f, uint32_t rate, uint32_t frames) {
    unsigned char h[44];
    memcpy(h, "RIFF", 4); put32(h + 4, 36 + frames * 4); memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16); put16(h + 20, 1); put16(h + 22, 2); put32(h + 24, rate); put32(h + 28, rate * 4);
    put16(h + 32, 4); put16(h + 34, 16); memcpy(h + 36, "data", 4); put32(h + 40, frames * 4);
    fwrite(h, 1, 44, f);
}

void wav_fix() {
    if (!g_wav) return;
    fflush(g_wav);
    fseek(g_wav, 0, SEEK_SET);
    wav_header(g_wav, g_rate, g_wav_frames);
    fflush(g_wav);
    fseek(g_wav, 0, SEEK_END);
}

int def_open(uint32_t rate) {
    g_rate = rate;
    g_clock_ns = np_time_real_ns();
    const char* path = getenv("NP_AUDIO_WAV");
    if (path && *path) {
        g_wav = fopen(path, "wb");
        if (g_wav) {
            wav_header(g_wav, rate, 0);
            atexit(wav_fix);
            np_log("audio", "writing %s", path);
        }
    }
    return 0;
}

void def_write(const int16_t* lr, uint32_t frames) {
    if (g_wav) {
        fwrite(lr, 4, frames, g_wav);
        g_wav_frames += frames;
        if ((g_wav_frames & 0x3FFF) < frames) wav_fix();
    }
    if (np_vt_on) return;   // virtual time: the events pace the blocks, not the sink
    g_clock_ns += (uint64_t)frames * 1000000000ull / g_rate;
    uint64_t now = np_time_real_ns();
    if (g_clock_ns > now + 20000000ull) {
        np_sleep_ns(g_clock_ns - now - 20000000ull);   // stay at most ~20 ms ahead of real time
    } else if (now > g_clock_ns + 200000000ull) {
        g_clock_ns = now;   // far behind (stall): do not try to catch up
    }
}

void def_close() { wav_fix(); }

const NpAudioSink g_default = {def_open, def_write, def_close};

}  // namespace

extern "C" {
void NpSdk_SetAudioSink(const NpAudioSink* sink) {
    if (sink) { g_user = *sink; g_have_user = true; } else { g_have_user = false; }
}
}

const NpAudioSink* np_audio_sink(void) { return g_have_user ? &g_user : &g_default; }
