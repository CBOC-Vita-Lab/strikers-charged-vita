// THP movie support below the player: the video frame decoder (THPVideoDecode), the audio frame
// decoder (THPAudioDecode) and the conversion of the file's big-endian structure words.
//
// The player itself (THPSimple.cpp of the decomp) is the game's own code: it reads the file through
// the game's file layer and interprets headers and per-frame size tables as structs. It is compiled
// by the main build with patches/thp/THPSimple.cpp.sed, which calls NpThpFixHeader / NpThpFixFrame
// below right after each read. THPDec.c and THPAudio.c of the decomp are not used: the decoder here
// is the native one of the recompilation (thp_decode.h) and a big-endian aware audio decoder.
//
// Frame layout of a THP file (all words big-endian): per frame {u32 size of the next frame, u32 size of
// the previous frame, u32 component size for each component, then the components}. Video is a
// baseline JPEG; audio is a 4-bit ADPCM with a header {u32 offsetNextChannel, u32 sampleSize,
// s16 lCoef[8][2], s16 rCoef[8][2], s16 lYn1, lYn2, rYn1, rYn2} followed by the nibbles of each channel.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "np/np_plat.h"
#include "thp_decode.h"

static inline uint32_t bswap32(uint32_t v) { return __builtin_bswap32(v); }
static inline uint16_t bswap16(uint16_t v) { return __builtin_bswap16(v); }

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define NP_THP_LE 1
#else
#define NP_THP_LE 0
#endif

static inline uint32_t rd32be(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline int16_t rd16be(const uint8_t* p) { return (int16_t)(((uint32_t)p[0] << 8) | p[1]); }

extern "C" {

static int g_thp_init = 0;

int THPInit(void) {
    g_thp_init = 1;
    return 1;
}

// ---- video -----------------------------------------------------------------------------------
// The decoder proper: a pure function of the compressed frame, the same wherever it runs (the game thread, or the decode-ahead lane
// below). Return values as THPVideoDecode: 0 ok, 1 no input, 2 no output, 3.. stream errors (see np_thp::k*).
static int NpThpDecodeNow(void* file, void* tileY, void* tileU, void* tileV) {
    np_thp::Tables tables;
    static const uint8_t kNatural[80] = {
        0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48, 41, 34,
        27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
        58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63, 63, 63, 63, 63, 63, 63, 63, 63,
        63, 63, 63, 63, 63, 63, 63, 63};
    static const double kAan[8] = {1.0, 1.387039845, 1.306562965, 1.175875602, 1.0, 0.785694958, 0.541196100, 0.275899379};
    // the SDK's table holds these as float constants widened to double
    tables.naturalOrder = kNatural;
    for (int i = 0; i < 8; ++i) tables.aan[i] = (double)(float)kAan[i];
    np_thp::Frame frame;
    bool supported = true;
    int r = np_thp::DecodeFrame((const uint8_t*)file, (uint8_t*)tileY, (uint8_t*)tileU, (uint8_t*)tileV, tables, frame, supported);
    if (!supported) return 19;   // a stream shape the native decoder does not handle (reported like a sampling error)
    return r;
}

int THPVideoDecode(void* file, void* tileY, void* tileU, void* tileV, void* work) {
    (void)work;   // the original's Huffman tables live there; the native decoder keeps its own on the stack
    if (!file) return 1;
    if (!tileY || !tileU || !tileV) return 2;
    if (!g_thp_init) return 7;
    return NpThpDecodeNow(file, tileY, tileU, tileV);
}

// ---- decode-ahead lanes -----------------------------------------------------------------------
// The player (THPSimple.cpp, patched by patches/thp/THPSimple.cpp.sed) decodes one video frame per call of THPSimpleDecode on the game
// thread: 5.4 M instructions in the stadium-select preview, 13.5 M per decoded frame in the intro movie (np/docs/wave2-NPP.md F4,
// docs/wave3-NPP2.md). The compressed frames sit in the player's ring of ten read buffers long before they are due, so when the
// decode of frame k is finished the player tells the lanes about the next frames (NpThpPrefetch: the video component's address, the
// frame number, the size): each of the kSlots lanes takes one frame, decodes it into private planes, and when the player asks for
// that frame (NpThpVideoDecode) it takes the result: a copy of the planes into the texture memory instead of the decode. If the lane
// is not done the player waits for it; if no lane has the frame (it was not read in time, the lanes were busy) the frame is decoded
// here and now, exactly as before. Same function, same input, same output: the planes the game sees are bit-identical either way.
// A frame is consumed by the call that asks for it; a slot that holds a frame that is not the one asked for and not a later one is
// stale and is released. Anything that ends or restarts a movie (Open, Close, PreLoad, LoadStop) drains every lane first
// (NpThpReset), so no lane reads a buffer that was freed or refilled.
// MSC_THP_LANE=0 switches the lanes off (the A/B of docs/wave3-NPP2.md); MSC_THP_DEPTH=1..4 sets how many frames are decoded ahead
// at once (default 2: the intro movie needs about 13.5 M instructions per frame, one core of the console runs 7.7 M per 33 ms).
namespace {
enum { kIdle = 0, kQueued, kRunning, kDone };
const int kMaxSlots = 4;
struct Slot {
    NpThread* thread = nullptr;
    NpSem wake, done;
    volatile int state = kIdle;      // game thread and lane: __atomic
    bool tokenTaken = true;          // the done token of the current job has been consumed by the game thread
    const uint8_t* src = nullptr;
    int32_t frame = 0;
    uint32_t w = 0, h = 0;
    uint8_t* plane[3] = {nullptr, nullptr, nullptr};
    size_t planeBytes[3] = {0, 0, 0};
    size_t allocBytes[3] = {0, 0, 0};
    int result = 0;
};
struct Lanes {
    int enabled = -1;                // -1 not decided
    int depth = 2;
    Slot slot[kMaxSlots];
    unsigned long long prefetched = 0, hits = 0, waits = 0, misses = 0, dropped = 0, stale = 0;
};
Lanes g_lanes;

bool LanesEnabled() {
    if (g_lanes.enabled < 0) {
        const char* e = getenv("MSC_THP_LANE");
        g_lanes.enabled = (e && *e == '0') ? 0 : 1;
        if (const char* d = getenv("MSC_THP_DEPTH")) {
            const int v = atoi(d);
            if (v >= 1 && v <= kMaxSlots) g_lanes.depth = v;
        }
    }
    return g_lanes.enabled == 1;
}

void* LaneMain(void* arg) {
    Slot& sl = *(Slot*)arg;
    for (;;) {
        np_sem_wait(&sl.wake);
        __atomic_store_n(&sl.state, (int)kRunning, __ATOMIC_RELEASE);
        sl.result = NpThpDecodeNow((void*)sl.src, sl.plane[0], sl.plane[1], sl.plane[2]);
        __atomic_store_n(&sl.state, (int)kDone, __ATOMIC_RELEASE);
        np_sem_post(&sl.done);
    }
    return nullptr;
}

bool SlotStart(Slot& sl) {
    if (sl.thread) return true;
    np_sem_init(&sl.wake, 0);
    np_sem_init(&sl.done, 0);
    sl.thread = np_thread_create("thp-decode", NP_ROLE_HELPER, LaneMain, &sl, 0, 262144);
    if (!sl.thread) return false;
    np_thread_detach(sl.thread);
    return true;
}

// game thread, state != kIdle: returns with the job finished and its done token taken (the result stays valid until SlotRelease)
void SlotWait(Slot& sl) {
    if (!sl.tokenTaken) {
        np_sem_wait(&sl.done);   // (posted by the lane after it set kDone)
        sl.tokenTaken = true;
    }
}
void SlotRelease(Slot& sl) { __atomic_store_n(&sl.state, (int)kIdle, __ATOMIC_RELEASE); }
int SlotState(const Slot& sl) { return __atomic_load_n(&sl.state, __ATOMIC_ACQUIRE); }

bool EnsurePlanes(Slot& sl, uint32_t w, uint32_t h) {
    const uint32_t h16 = (h + 15u) & ~15u;
    const size_t y = (size_t)w * h16, uv = (size_t)(w / 2u) * (h16 / 2u);
    const size_t need[3] = {y, uv, uv};
    for (int i = 0; i < 3; ++i) {
        if (sl.allocBytes[i] < need[i]) {
            free(sl.plane[i]);
            sl.plane[i] = (uint8_t*)malloc(need[i]);
            sl.allocBytes[i] = sl.plane[i] ? need[i] : 0;
            if (!sl.plane[i]) return false;
        }
        sl.planeBytes[i] = need[i];
    }
    return true;
}
} // namespace

// Player -> lanes: frame `frame` (the video component starts at `video`) is due soon. A frame some lane already has is not queued again.
void NpThpPrefetch(const void* video, int frame, unsigned width, unsigned height) {
    if (!LanesEnabled() || !video || width == 0 || (width & 15u) != 0 || width > 1024 || height > 1024 || !g_thp_init) return;
    Slot* freeSlot = nullptr;
    for (int i = 0; i < g_lanes.depth; ++i) {
        Slot& sl = g_lanes.slot[i];
        if (SlotState(sl) != kIdle) {
            if (sl.src == (const uint8_t*)video && sl.frame == frame && sl.w == width && sl.h == height) return;   // already queued or done
        } else if (!freeSlot) {
            freeSlot = &sl;
        }
    }
    if (!freeSlot) { ++g_lanes.dropped; return; }
    if (!SlotStart(*freeSlot) || !EnsurePlanes(*freeSlot, width, height)) return;
    freeSlot->src = (const uint8_t*)video;
    freeSlot->frame = frame;
    freeSlot->w = width;
    freeSlot->h = height;
    freeSlot->tokenTaken = false;
    ++g_lanes.prefetched;
    __atomic_store_n(&freeSlot->state, (int)kQueued, __ATOMIC_RELEASE);
    np_sem_post(&freeSlot->wake);
}

// The player's call (VideoDecode in THPSimple.cpp): the same contract as THPVideoDecode, plus the frame number and the size.
int NpThpVideoDecode(void* file, void* tileY, void* tileU, void* tileV, void* work, int frame, unsigned width, unsigned height) {
    (void)work;
    if (!file) return 1;
    if (!tileY || !tileU || !tileV) return 2;
    if (!g_thp_init) return 7;
    if (g_lanes.enabled == 1) {
        Slot* hit = nullptr;
        for (int i = 0; i < g_lanes.depth; ++i) {
            Slot& sl = g_lanes.slot[i];
            if (SlotState(sl) == kIdle) continue;
            if (sl.src == (const uint8_t*)file && sl.frame == frame && sl.w == width && sl.h == height) hit = &sl;
            else if (sl.frame < frame) {   // an earlier frame that nobody asked for: of no use any more
                SlotWait(sl);
                SlotRelease(sl);
                ++g_lanes.stale;
            }
        }
        if (hit) {
            const bool ready = SlotState(*hit) == kDone;
            SlotWait(*hit);
            ++(ready ? g_lanes.hits : g_lanes.waits);
            const int r = hit->result;
            if (r == 0) {   // the planes, as the decoder would have written them
                memcpy(tileY, hit->plane[0], hit->planeBytes[0]);
                memcpy(tileU, hit->plane[1], hit->planeBytes[1]);
                memcpy(tileV, hit->plane[2], hit->planeBytes[2]);
            }
            SlotRelease(*hit);
            return r;
        }
    }
    ++g_lanes.misses;
    return NpThpDecodeNow(file, tileY, tileU, tileV);
}

// A movie is opened, closed, restarted or stopped: nothing of the lanes may be in flight or kept.
void NpThpReset(void) {
    if (g_lanes.enabled != 1) return;
    for (int i = 0; i < kMaxSlots; ++i) {
        Slot& sl = g_lanes.slot[i];
        if (SlotState(sl) == kIdle) continue;
        SlotWait(sl);
        SlotRelease(sl);
    }
}

void NpThpLogStats(void) {
    if (g_lanes.enabled != 1) return;
    fprintf(stderr, "[thp] decode-ahead lanes (depth %d): prefetched %llu, taken ready %llu, taken after a wait %llu, decoded on the game thread %llu, stale released %llu, not queued (all busy) %llu\n",
            g_lanes.depth, g_lanes.prefetched, g_lanes.hits, g_lanes.waits, g_lanes.misses, g_lanes.stale, g_lanes.dropped);
}

// ---- audio -----------------------------------------------------------------------------------
// ADPCM nibble reader as THPAudio.c's __THPAudioGetNewSample.
struct NpThpNibbles {
    const uint8_t* data;
    int offsetNibbles;
    int predictor;
    int scale;
};
static void nib_init(NpThpNibbles* n, const uint8_t* p) {
    n->data = p;
    n->offsetNibbles = 2;
    n->predictor = (*n->data & 0x70) >> 4;
    n->scale = *n->data & 0xF;
    n->data++;
}
static int32_t nib_next(NpThpNibbles* n) {
    int32_t sample;
    if (!(n->offsetNibbles & 0x0f)) {
        n->predictor = (*n->data & 0x70) >> 4;
        n->scale = *n->data & 0xF;
        n->data++;
        n->offsetNibbles += 2;
    }
    if (n->offsetNibbles & 0x1) {
        sample = (int32_t)((uint32_t)(*n->data & 0xF) << 28) >> 28;
        n->data++;
    } else {
        sample = (int32_t)((uint32_t)(*n->data & 0xF0) << 24) >> 28;
    }
    n->offsetNibbles++;
    return sample;
}

// Decodes one audio frame into `audioBuffer` (interleaved L,R unless flag == 1: all L then all R) and
// returns the number of samples per channel. The output is in host byte order.
uint32_t THPAudioDecode(int16_t* audioBuffer, uint8_t* audioFrame, int32_t flag) {
    if (!audioBuffer || !audioFrame) return 0;
    const uint8_t* h = audioFrame;
    const uint32_t offsetNextChannel = rd32be(h + 0);
    const uint32_t sampleSize = rd32be(h + 4);
    int16_t lCoef[8][2], rCoef[8][2];
    for (int i = 0; i < 8; ++i) {
        lCoef[i][0] = rd16be(h + 8 + i * 4);
        lCoef[i][1] = rd16be(h + 10 + i * 4);
        rCoef[i][0] = rd16be(h + 40 + i * 4);
        rCoef[i][1] = rd16be(h + 42 + i * 4);
    }
    const int16_t lYn1 = rd16be(h + 72), lYn2 = rd16be(h + 74), rYn1 = rd16be(h + 76), rYn2 = rd16be(h + 78);
    const uint8_t* left = audioFrame + 80;
    const uint8_t* right = left + offsetNextChannel;
    int16_t* decRight;
    int16_t* decLeft;
    int step;
    if (flag == 1) {
        decRight = audioBuffer;
        decLeft = audioBuffer + sampleSize;
        step = 1;
    } else {
        decRight = audioBuffer;
        decLeft = audioBuffer + 1;
        step = 2;
    }
    NpThpNibbles n;
    int16_t yn1, yn2;
    // one channel: the ADPCM recurrence of THPAudio.c
    struct Chan {
        static void run(NpThpNibbles* n, const int16_t (*coef)[2], int16_t y1, int16_t y2, uint32_t count, int16_t* out1,
                        int16_t* out2, int step) {
            for (uint32_t i = 0; i < count; ++i) {
                int32_t sample = nib_next(n);
                int64_t yn = (int64_t)coef[n->predictor][1] * y2;
                yn += (int64_t)coef[n->predictor][0] * y1;
                yn += (int64_t)((int32_t)((uint32_t)sample << n->scale)) << 11;
                yn <<= 5;
                yn += 0x8000;
                if (yn > 2147483647LL) yn = 2147483647LL;
                if (yn < -2147483648LL) yn = -2147483648LL;
                int16_t v = (int16_t)(yn >> 16);
                *out1 = v;
                out1 += step;
                if (out2) {
                    *out2 = v;
                    out2 += step;
                }
                y2 = y1;
                y1 = v;
            }
        }
    };
    if (offsetNextChannel == 0) {   // mono: both channels get the same samples
        nib_init(&n, left);
        yn1 = lYn1;
        yn2 = lYn2;
        Chan::run(&n, lCoef, yn1, yn2, sampleSize, decLeft, decRight, step);
    } else {
        nib_init(&n, left);
        yn1 = lYn1;
        yn2 = lYn2;
        Chan::run(&n, lCoef, yn1, yn2, sampleSize, decLeft, 0, step);
        nib_init(&n, right);
        yn1 = rYn1;
        yn2 = rYn2;
        Chan::run(&n, rCoef, yn1, yn2, sampleSize, decRight, 0, step);
    }
    return sampleSize;
}

// ---- big-endian structure words ---------------------------------------------------------------
// Called by the patched THPSimple.cpp. All are idempotent only in the sense that each is called
// once per read.

// The 48-byte file header (THPHeader) after it was read into `buf`: everything after the magic.
void NpThpFixHeader(void* buf) {
#if NP_THP_LE
    uint32_t* w = (uint32_t*)buf;
    for (int i = 1; i < 12; ++i) w[i] = bswap32(w[i]);   // version .. finalFrameDataOffsets (frameRate is a float word)
#else
    (void)buf;
#endif
}
// THPFrameCompInfo: {u32 numComponents, u8 frameComp[16]}
void NpThpFixCompInfo(void* buf) {
#if NP_THP_LE
    uint32_t* w = (uint32_t*)buf;
    w[0] = bswap32(w[0]);
#else
    (void)buf;
#endif
}
// words: 3 for THPVideoInfo, 4 for THPAudioInfo
void NpThpFixInfo(void* buf, int words) {
#if NP_THP_LE
    uint32_t* w = (uint32_t*)buf;
    for (int i = 0; i < words; ++i) w[i] = bswap32(w[i]);
#else
    (void)buf; (void)words;
#endif
}
// The head of a frame: {next size, previous size, component sizes...}
void NpThpFixFrame(void* frame, uint32_t numComponents) {
#if NP_THP_LE
    uint32_t* w = (uint32_t*)frame;
    for (uint32_t i = 0; i < 2 + numComponents; ++i) w[i] = bswap32(w[i]);
#else
    (void)frame; (void)numComponents;
#endif
}

}  // extern "C"
