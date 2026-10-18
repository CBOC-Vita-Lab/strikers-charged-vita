// The game's sound path on real data: a sound bank of the disc dump (CHAR_BOO_Sfx: the SP sound table inside
// the .resbun, the ADPCM samples in the .nlxwb) goes through SPInitSoundTable (big-endian table swapped in place),
// SPPrepareSound and the AX voice calls exactly as AudioSource.cpp makes them; the mixer's output is compared
// with an independent ADPCM decode of the same samples (normalised cross-correlation). Skipped when the disc
// dump is not present.
#include <revolution/ai.h>
#include <revolution/ax.h>
#include <revolution/mix.h>
#include <revolution/os.h>
#include <revolution/sp.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include "np/np_plat.h"
#include "np/np_sdk.h"

extern "C" void MscAxPrintStats(void);
extern "C" void NpSpFixEndian(void*);
static int g_fail = 0;
#define CHECK(c)                                                                    \
    do {                                                                            \
        if (!(c)) {                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                     \
            g_fail++;                                                               \
        }                                                                           \
    } while (0)

static int16_t* g_cap = 0;
static uint32_t g_cap_frames = 0;
static const uint32_t kCapMax = 32000 * 2;
static int sink_open(uint32_t) { return 0; }
static void sink_write(const int16_t* lr, uint32_t frames) {
    if (g_cap_frames + frames <= kCapMax) {
        memcpy(g_cap + g_cap_frames * 2, lr, frames * 4);
        g_cap_frames += frames;
    }
    np_sleep_ns((uint64_t)frames * 1000000000ull / 32000 - 100000);
}
static void sink_close(void) {}
static void ax_frame_cb(void) { MIXUpdateSettings(); }

static uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static int16_t be16s(const uint8_t* p) { return (int16_t)(((uint32_t)p[0] << 8) | p[1]); }

static std::vector<uint8_t> slurp(const char* path) {
    std::vector<uint8_t> v;
    FILE* f = fopen(path, "rb");
    if (!f) return v;
    fseek(f, 0, SEEK_END);
    v.resize((size_t)ftell(f));
    fseek(f, 0, SEEK_SET);
    size_t n = fread(v.data(), 1, v.size(), f);
    (void)n;
    fclose(f);
    return v;
}

// independent GameCube/Wii DSP-ADPCM decoder (14 samples per 8-byte frame), nibble addressed
static std::vector<int16_t> decode_adpcm(const uint8_t* data, uint32_t startNibble, uint32_t endNibble, const int16_t coef[8][2],
                                         int16_t hist1, int16_t hist2) {
    std::vector<int16_t> out;
    int32_t h1 = hist1, h2 = hist2;
    for (uint32_t n = startNibble; n <= endNibble; ++n) {
        uint32_t frameStart = n & ~15u;
        if ((n & 15) < 2) continue;   // header nibbles
        uint8_t hdr = data[frameStart / 2];
        int pi = (hdr >> 4) & 7, scale = hdr & 15;
        uint8_t b = data[n / 2];
        int nib = (n & 1) ? (b & 15) : (b >> 4);
        if (nib >= 8) nib -= 16;
        int32_t v = ((nib << scale) << 11) + 1024 + coef[pi][0] * h1 + coef[pi][1] * h2;
        v >>= 11;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        out.push_back((int16_t)v);
        h2 = h1;
        h1 = v;
    }
    return out;
}

int main() {
    const char* dir = getenv("MSC_DISC_FILES");
    if (!dir) dir = "/home/claude/msc/game/files";
    char p1[512], p2[512];
    snprintf(p1, sizeof(p1), "%s/audio/CHAR_BOO_Sfx.resbun", dir);
    snprintf(p2, sizeof(p2), "%s/audio/CHAR_BOO_Sfx.nlxwb", dir);
    std::vector<uint8_t> bun = slurp(p1), wb = slurp(p2);
    if (bun.empty() || wb.empty()) {
        printf("  (bank not present: skipped)\ntest_sp_bank: ok (skipped)\n");
        return 0;
    }
    // the SP table chunk (id 0x23703 under the alignment bits), as nlChunk::GetID()/GetData() find it
    size_t tableOff = 0, tableSize = 0;
    for (size_t i = 0; i + 8 <= bun.size(); i += 4) {
        if ((be32(&bun[i]) & 0x80FFFFFFu) == 0x23703u) {
            uint32_t al = (be32(&bun[i]) >> 24) & 0xF;
            size_t off = i + 8;
            if (al) off = (off + ((size_t)1 << al) - 1) & ~(((size_t)1 << al) - 1);
            tableOff = off;
            tableSize = be32(&bun[i + 4]) - (off - (i + 8));
            break;
        }
    }
    CHECK(tableOff != 0);
    uint32_t entries = be32(&bun[tableOff]);
    printf("  table: %u entries, %zu bytes\n", entries, tableSize);
    CHECK(entries > 0 && entries < 1000);
    // raw (big-endian) view of entry 0 and its ADPCM record, taken before the SDK converts the table
    const uint8_t* e0 = &bun[tableOff + 4];
    uint32_t t0 = be32(e0), rate0 = be32(e0 + 4), end0 = be32(e0 + 16), cur0 = be32(e0 + 20);
    CHECK(t0 == 0);   // ADPCM, one shot
    const uint8_t* ad = &bun[tableOff + 4 + 28 * entries];   // the first SPADPCM record belongs to entry 0
    int16_t coef[8][2];
    for (int i = 0; i < 8; ++i) {
        coef[i][0] = be16s(ad + 4 * i);
        coef[i][1] = be16s(ad + 4 * i + 2);
    }
    uint16_t gain = (uint16_t)be16s(ad + 32);
    int16_t yn1 = be16s(ad + 36), yn2 = be16s(ad + 38);
    printf("  entry 0: rate %u Hz, nibbles %u..%u, gain 0x%x\n", rate0, cur0, end0, gain);

    g_cap = (int16_t*)calloc(kCapMax * 2, sizeof(int16_t));
    NpSdkConfig cfg;
    NpSdk_ConfigDefaults(&cfg);
    NpSdk_Init(&cfg);
    NpAudioSink sink = {sink_open, sink_write, sink_close};
    NpSdk_SetAudioSink(&sink);
    AIInit(0);
    AXInit();
    MIXInit();
    AXRegisterCallback(ax_frame_cb);
    AXSetMode(0);

    // the bank's sample data and the table live in the arena (above 0x80000000), like in the game
    uint8_t* data = (uint8_t*)OSAllocFromMEM2ArenaLo((u32)((wb.size() + 63) & ~63u), 32);
    memcpy(data, wb.data(), wb.size());
    uint8_t* silence = (uint8_t*)OSAllocFromMEM2ArenaLo(64, 32);
    memset(silence, 0, 64);
    SPSoundTable* table = (SPSoundTable*)OSAllocFromMEM2ArenaLo((u32)((tableSize + 31) & ~31u), 32);
    memcpy(table, &bun[tableOff], tableSize);
    SPInitSoundTable(table, (u32)(uintptr_t)data, (u32)(uintptr_t)silence);

    CHECK(table->entries == entries);
    // idempotence: a table the asset loader already converted (np_audio.c) must come out the same
    {
        SPSoundTable* t2 = (SPSoundTable*)malloc(tableSize + 64);
        memcpy(t2, &bun[tableOff], tableSize);
        NpSpFixEndian(t2);                       // what the loader does
        SPInitSoundTable(t2, (u32)(uintptr_t)data, (u32)(uintptr_t)silence);   // must not swap again
        CHECK(t2->entries == entries);
        for (uint32_t i = 0; i < entries; ++i) {
            CHECK(t2->sound[i].type == table->sound[i].type && t2->sound[i].currentAddr == table->sound[i].currentAddr &&
                  t2->sound[i].endAddr == table->sound[i].endAddr && t2->sound[i].sampleRate == table->sound[i].sampleRate);
            if (t2->sound[i].type <= 1) CHECK(t2->sound[i].adpcm->adpcm.a[3][1] == table->sound[i].adpcm->adpcm.a[3][1]);
        }
        free(t2);
    }
    SPSoundEntry* s0 = SPGetSoundEntry(table, 0);
    CHECK(s0 && s0->type == 0 && s0->sampleRate == rate0);
    uint32_t base4 = ((uint32_t)(uintptr_t)data - 0x80000000u) << 1;
    CHECK(s0->currentAddr == base4 + cur0 && s0->endAddr == base4 + end0);
    CHECK(s0->adpcm && s0->adpcm->adpcm.a[3][1] == (uint16_t)coef[3][1] && s0->adpcm->adpcm.gain == gain);
    CHECK(SPGetSoundEntry(table, entries) == 0);

    // the game's voice set-up (AudioSampleSource::Initialize / Update), one shot
    AXVPB* v = AXAcquireVoice(15, 0, 0);
    CHECK(v != 0);
    if (!v) return 1;
    SPPrepareSound(s0, v, s0->sampleRate);
    MIXInitChannel(v, 0, 0, -960, -960, -960, 64, 127, 0);
    // The game plays a few frames later (AudioSampleSource::Update, state 5): the AX frame in between copies the
    // whole address block (AX_PBSYNC_ADDR); a loop flag set in the same frame would take the partial-copy branch.
    OSSleepTicks(OSMillisecondsToTicks(20));
    AXSetVoiceLoop(v, false);
    AXSetVoiceLoopAddr(v, (unsigned long)silence * 2);   // what the game does once the first pass is done
    AXSetVoiceState(v, AX_VOICE_RUN);
    OSSleepTicks(OSMillisecondsToTicks(1500));
    AXSetVoiceState(v, AX_VOICE_STOP);
    AXFreeVoice(v);
    if (getenv("MSC_AX_STATS")) MscAxPrintStats();

    // reference: independent decode + linear resampling to 32 kHz
    std::vector<int16_t> ref = decode_adpcm(data, cur0, end0, coef, yn1, yn2);
    double ratio = (double)rate0 / 32000.0;
    size_t nref = (size_t)(ref.size() / ratio);
    std::vector<double> r(nref);
    for (size_t i = 0; i < nref; ++i) {
        double pos = i * ratio;
        size_t k = (size_t)pos;
        double fr = pos - k;
        double a = ref[k], b = k + 1 < ref.size() ? ref[k + 1] : a;
        r[i] = a + (b - a) * fr;
    }
    // mixer output, left channel
    std::vector<double> m(g_cap_frames);
    for (uint32_t i = 0; i < g_cap_frames; ++i) m[i] = g_cap[i * 2];
    // onset: first block with signal
    size_t on = 0;
    while (on < m.size() && fabs(m[on]) < 4) ++on;
    printf("  decoded %zu samples (%.2f s at %u Hz); mixer output onset at frame %zu of %u\n", ref.size(), ref.size() / (double)rate0, rate0, on, g_cap_frames);
    CHECK(on < m.size());
    // best normalised cross-correlation over lags 0..400 frames around the onset
    size_t n = nref < 8000 ? nref : 8000;
    if (on + 400 + n > m.size()) n = m.size() > on + 400 ? m.size() - on - 400 : 0;
    double best = -1, bestLag = 0, bestGain = 0;
    for (int lag = -200; lag <= 400; ++lag) {
        double sxy = 0, sxx = 0, syy = 0;
        for (size_t i = 0; i < n; ++i) {
            long j = (long)on + lag + (long)i;
            if (j < 0 || j >= (long)m.size()) continue;
            sxy += r[i] * m[j];
            sxx += r[i] * r[i];
            syy += m[j] * m[j];
        }
        double c = (sxx > 0 && syy > 0) ? sxy / sqrt(sxx * syy) : 0;
        if (c > best) { best = c; bestLag = lag; bestGain = sxx > 0 ? sxy / sxx : 0; }
    }
    printf("  correlation with the independent decode: %.4f at lag %.0f frames, gain %.3f\n", best, bestLag, bestGain);
    CHECK(best > 0.95);
    CHECK(bestGain > 0.05 && bestGain < 4.0);

    if (g_fail) {
        printf("%d check(s) FAILED\n", g_fail);
        return 1;
    }
    printf("test_sp_bank: ok\n");
    return 0;
}
