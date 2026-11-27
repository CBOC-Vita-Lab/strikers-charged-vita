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
#include "thp_hwjpeg.h"
#include "thp_jpeg.h"
#include "thp_pacing.h"

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
// below). Return values as THPVideoDecode: 0 ok, 1 no input, 2 no output, 3.. stream errors (see np_thp::k*). `end` (may be null) is
// the first byte after the frame's video component: a frame that claims more than it holds ends in an error (3) instead of decoding
// the memory behind it.
static int NpThpDecodeNow(void* file, void* tileY, void* tileU, void* tileV, const uint8_t* end = nullptr) {
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
    int r = np_thp::DecodeFrame((const uint8_t*)file, (uint8_t*)tileY, (uint8_t*)tileU, (uint8_t*)tileV, tables, frame, supported, end);
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

// The movie being played (for the log lines that name it): the player tells us when it opens a file.
static char g_movieName[128];
extern "C" void NpThpNoteName(const char* name) {
    snprintf(g_movieName, sizeof g_movieName, "%s", name ? name : "?");
}
// The player found a frame size that cannot be right (0 / larger than the read buffers): the file is cut short or damaged. frame 0:
// the header itself, the movie is not opened; otherwise the movie ends after the frames before this one.
extern "C" void NpThpLogBadFrameSize(unsigned long size, unsigned long limit, long frame) {
    fprintf(stderr, "[thp] %s: frame %ld announces %lu bytes but the read buffers hold %lu: the file is truncated or damaged, %s\n", g_movieName, frame, size, limit,
            frame == 0 ? "the movie is not played" : "the movie ends here");
}
// The next frame lies (partly) behind the end of the file: the file is shorter than its header says. The disc layer would stop on such a
// read (a scanned file table) or zero-fill it, so the movie ends before the read is made.
extern "C" void NpThpLogPastEnd(unsigned long offset, unsigned long size, unsigned long fileSize, long frame) {
    fprintf(stderr, "[thp] %s: frame %ld lies at %lu + %lu bytes but the file has %lu: the file is truncated, %s\n", g_movieName, frame, offset, size, fileSize,
            frame == 0 ? "the movie is not played" : "the movie ends here");
}

// ---- decode-ahead lanes -----------------------------------------------------------------------
// The player (THPSimple.cpp, patched by patches/thp/THPSimple.cpp.sed) decodes one video frame per call of THPSimpleDecode on the game
// thread: 5.4 M instructions in the stadium-select preview, 13.5 M per decoded frame in the intro movie (np/docs/wave2-NPP.md F4,
// docs/wave3-NPP2.md). The compressed frames sit in the player's ring of ten read buffers long before they are due, so when the
// decode of frame k is finished the player tells the lanes about the next frames (NpThpPrefetch: the video component's address, size,
// the frame number): each of the kSlots lanes takes one frame (a private copy of the compressed component), decodes it into private
// planes, and when the player asks for that frame (NpThpVideoDecode) it takes the result: a copy of the planes into the texture memory
// instead of the decode. If no lane has the frame (it was not read in time, the lanes were busy) the frame is decoded here and now,
// exactly as before. Same function, same input, same output: the planes the game sees are bit-identical either way.
//
// A lane decodes with the console's JPEG engine first when MSC_THP_HWJPEG allows it (thp_jpeg.h: restuffed stream, sceJpeg, retiled
// planes; the result differs from the software decoder by rounding only) and with the software decoder when the engine refuses a
// frame, so one bad frame costs a software decode, not the picture.
//
// Two ways of dealing with a lane that is not done when its frame is asked for:
//  * MSC_THP_NOWAIT=0 (host default; what 1.0 and 1.1 did): the game thread waits for the lane, so every picture is the exact frame.
//  * MSC_THP_NOWAIT=1 (console default): the game thread never waits. The packet is consumed (its audio was issued before), and the
//    picture is the newest finished frame that is not newer than the asked one; if none finished the last picture stays, except
//    that a movie's first picture is waited for (one bounded wait for the lane that has the frame, else a decode here) and a frame
//    that could not be queued (too large, no memory) is decoded here: MSC_THP_FIRSTFRAME=0 restores the plain "picture stays". A late
//    frame is shown by the first call after it finished. Skipping or ending a movie does not wait either: the lanes work on private
//    copies, a result nobody asks for any more is dropped when its lane is done.
// A frame is consumed by the call that asks for it; a slot that holds a frame that is not the one asked for and not a later one is
// stale and is released. MSC_THP_LANE=0 switches the lanes off (the A/B of docs/wave3-NPP2.md); MSC_THP_DEPTH=1..4 sets how many
// frames are decoded ahead at once (default 2: the intro movie needs about 13.5 M instructions per frame in software, one core of the
// console runs 7.7 M per 33 ms; with the JPEG engine one lane is plenty).
//
// MSC_THP_DELAY=<calls> (0..4, default 0 = exactly the rules above; needs MSC_THP_NOWAIT=1): a delay line, ported from Galaxy's movie36.
// The call that asks for frame N shows frame N - delay, so a frame has `delay` more calls (33 ms each) to be decoded in, instead of
// one. The lanes become a small pool (MSC_THP_DEPTH workers; delay + 4 slots hold the frames waiting to be shown and the 4-frame look-ahead) whose workers
// take the frame that is due first and can still be finished in time (thp_pacing.h PickJob), drop frames nobody will show any more,
// and a frame that misses its call is passed over for the one due next: no picture is shown twice for one slow decode and the
// movie neither runs ahead of nor drifts from its sound. The picture is `delay` frames behind its sound (66 ms at 2: inside
// lip-sync tolerance; the sound is issued when the packet is consumed). The last `delay` pictures of a movie are not shown.
//
// MSC_THP_RECOVER=<calls> (default 8; 0 = off): the failure path of a frozen picture. When `calls` consecutive calls find a due
// frame that no lane has finished (every lane stuck, frames never queued, a codec call that never returns) the asked frame is
// decoded here in software instead of leaving the picture frozen while the sound plays on. A codec call that has not returned
// after MSC_THP_HWSTUCK_MS (default 700) switches the codec off for the run and gives the lane up (another takes its place); a
// lane waits at most 250 ms for the codec's lock; the first picture of a movie is waited for at most 1.5 s. Nothing changes while
// the lanes keep up. Counted in the movie's summary line.
namespace {
enum { kIdle = 0, kQueued, kRunning, kDone };
const int kMaxSlots = 8;
const size_t kCopyPad = 512;                // zero bytes behind a private copy (the bit reader looks a few bytes ahead)
const unsigned kMaxFrameBytes = 1u << 20;   // a video component larger than this is never queued
struct Slot {
    NpThread* thread = nullptr;
    NpSem wake, done;
    volatile int state = kIdle;      // game thread and lane: __atomic
    bool tokenTaken = true;          // the done token of the current job has been consumed by the game thread
    bool retired = false;            // game thread only: nobody will ask for this result any more
    uint32_t gen = 0;                // the movie it belongs to (g_lanes.gen when queued)
    const uint8_t* src = nullptr;    // identity of the frame: the video component's address in the player's ring
    uint8_t* copy = nullptr;         // private copy of the component, kCopyPad zero bytes behind it
    size_t copyAlloc = 0, copySize = 0;
    int32_t frame = 0;
    uint32_t w = 0, h = 0;
    uint8_t* plane[3] = {nullptr, nullptr, nullptr};
    size_t planeBytes[3] = {0, 0, 0};
    size_t allocBytes[3] = {0, 0, 0};
    int result = 0;
    bool viaHw = false;
    bool inited = false;             // the semaphores exist (delay pool: slots have no thread of their own)
    volatile uint32_t swSinceMs = 0; // tools15 (movie14 review L4): the lane is inside the SOFTWARE decoder since this time (ms, never 0); 0 = not. Counted by SwWatch, never acted on
    uint32_t swFlagged = 0;          // game thread: the swSinceMs already counted as stuck
};
const int kSkipped = -1000;          // a queued job dropped by a worker before it was decoded (nobody would show it): its planes are not a picture
struct Lanes {
    int enabled = -1;                // -1 not decided
    int nowait = -1;
    int depth = 2;
    uint32_t gen = 1;
    int32_t lastShown = -1;          // nowait: the newest frame number whose planes were copied into the textures
    int firstFix = -1;               // nowait: MSC_THP_FIRSTFRAME (default 1): a movie shows a picture at its first call; frames no lane can take are decoded here
    const uint8_t* rejSrc = nullptr; // the last frame NpThpPrefetch could not queue (no thread, no memory): decoded on the game thread
    int32_t rejFrame = -1;
    uint32_t rejGen = 0;
    int delay = 0;                   // MSC_THP_DELAY (effective only with nowait)
    int recover = 8;                 // MSC_THP_RECOVER: calls without a picture before the asked frame is decoded here (0 = off)
    int32_t base = -1;               // delay: the first frame asked in this movie / loop
    int32_t lastAsked = -1;
    int starved = 0;                 // consecutive calls on which a due frame was not there
    bool sticky = false;             // a recovery happened and no lane has delivered since: the threshold is 2 calls
    Slot slot[kMaxSlots];
    struct Stats {
        unsigned long long prefetched = 0, hits = 0, waits = 0, misses = 0, dropped = 0, stale = 0;
        unsigned long long lateShown = 0, notReady = 0, noSlot = 0, retiredFrames = 0, errors = 0;
        unsigned long long recovered = 0, firstTimeouts = 0, lanesLost = 0, jobsDropped = 0, swStuck = 0;
    } st, reported;
};
Lanes g_lanes;
// The slots in use: one per lane; with the delay line the frames waiting to be shown too (the player's look-ahead is 4 frames, so
// delay + 4 slots hold everything it offers; fewer is fine, the frames not queued yet are offered again at the next call).
int NSlots() {
    int n = g_lanes.depth;
    if (g_lanes.delay > 0) {
        n += g_lanes.delay + 1;
        if (n < g_lanes.delay + 4) n = g_lanes.delay + 4;
    }
    return n > kMaxSlots ? kMaxSlots : n;
}

// ---- pace statistics (Galaxy's movie33): how evenly the new pictures came, in wall time, one line per movie ----
// A picture is "late" when it follows the one before by more than 1.5 call periods (a slow decode, a preempted game thread, a slow
// card: the eye sees a held picture), "doubled" when it follows by less than half a period (two pictures within one turn: a burst
// after a hold). A call that brought no new picture although one was due is "repeated". The period is the smoothed time between calls.
struct Pace {
    unsigned long long calls = 0, fresh = 0, repeated = 0, onTime = 0, passed = 0, late = 0, doubled = 0, gaps = 0, periodN = 0, early = 0;
    double periodUs = 33333.0, periodSumMs = 0.0, maxGapMs = 0.0;
    uint64_t lastCallUs = 0, lastFreshUs = 0;
    bool haveFresh = false;
    int64_t lastFreshFrame = -1;
} g_pace, g_paceRep;
uint64_t NowUs();
void PaceCall() {
    const uint64_t now = NowUs();
    ++g_pace.calls;
    if (g_pace.lastCallUs) {
        const double since = (double)(now - g_pace.lastCallUs);
        if (since <= 250000.0) {
            g_pace.periodUs += (since - g_pace.periodUs) / 16.0;
            g_pace.periodSumMs += since / 1000.0;
            ++g_pace.periodN;
        } else {
            g_pace.haveFresh = false;
        }
    }
    g_pace.lastCallUs = now;
}
// a new picture reached the textures: `shown` is its frame number, `due` the frame that was due at this call
void PaceFresh(int64_t shown, int64_t due) {
    const uint64_t now = NowUs();
    if (g_pace.haveFresh) {
        const double gap = (double)(now - g_pace.lastFreshUs);
        ++g_pace.gaps;
        if (gap > g_pace.periodUs * 1.5) ++g_pace.late;
        if (gap < g_pace.periodUs * 0.5) ++g_pace.doubled;
        if (gap / 1000.0 > g_pace.maxGapMs) g_pace.maxGapMs = gap / 1000.0;
    }
    if (g_pace.lastFreshFrame >= 0 && shown > g_pace.lastFreshFrame + 1) g_pace.passed += (unsigned long long)(shown - g_pace.lastFreshFrame - 1);
    if (shown == due) ++g_pace.onTime;
    if (shown > due) ++g_pace.early;
    g_pace.lastFreshUs = now;
    g_pace.haveFresh = true;
    g_pace.lastFreshFrame = shown;
    ++g_pace.fresh;
}
void PaceRepeat() { ++g_pace.repeated; }
void PaceReset() {
    g_pace.lastCallUs = 0;
    g_pace.haveFresh = false;
    g_pace.lastFreshFrame = -1;
    g_pace.maxGapMs = 0;
}

bool LanesEnabled() {
    if (g_lanes.enabled < 0) {
        const char* e = getenv("MSC_THP_LANE");
        g_lanes.enabled = (e && *e == '0') ? 0 : 1;
        if (const char* d = getenv("MSC_THP_DEPTH")) {
            const int v = atoi(d);
            if (v >= 1 && v <= 4) g_lanes.depth = v;
        }
#if defined(NP_VITA)
        g_lanes.nowait = 1;
#else
        g_lanes.nowait = 0;
#endif
        if (const char* nw = getenv("MSC_THP_NOWAIT")) g_lanes.nowait = (*nw == '0') ? 0 : 1;
        const char* ff = getenv("MSC_THP_FIRSTFRAME");
        g_lanes.firstFix = (ff && *ff == '0') ? 0 : 1;
        if (g_lanes.enabled == 0) g_lanes.nowait = 0;   // no lanes: the game thread decodes
        if (const char* dl = getenv("MSC_THP_DELAY")) {
            const int v = atoi(dl);
            g_lanes.delay = v < 0 ? 0 : v > 5 ? 5 : v;
        }
        if (!g_lanes.nowait) g_lanes.delay = 0;          // the delay line needs the never-waiting rules
        if (const char* rc = getenv("MSC_THP_RECOVER")) {
            const int v = atoi(rc);
            g_lanes.recover = v <= 0 ? 0 : v < 2 ? 2 : v > 120 ? 120 : v;
        }
        if (g_lanes.delay > kMaxSlots - 4) g_lanes.delay = kMaxSlots - 4;   // (delay + 4 slots)
    }
    return g_lanes.enabled == 1;
}

// ---- the JPEG engine ---------------------------------------------------------------------------
// MSC_THP_HWJPEG=1|0 (console default 1, hosts 0); "fake" (hosts): a stand-in codec built from the software decoder, so that the
// whole path around the engine can be exercised and compared on a host (MSC_THP_HWJPEG_FAKE_FAIL=N: every Nth frame is refused;
// MSC_THP_HWJPEG_FAKE_BAD=1: the codec returns wrong pictures, which the first-frames check must catch).
// MSC_THP_HWJPEG_VERIFY=N (default 2): the first N frames of each movie are also decoded in software and compared; a picture that
// differs more than rounding (mean absolute difference above 3 levels in any plane) switches the engine off for the rest of the run
// and the software picture is used.
struct Hw {
    int mode = -1;                   // -1 undecided, 0 off, 1 console engine, 2 fake codec
    NpMutex mu;
    bool muInit = false;
    const NpJpegCodec* codec = nullptr;
    void* ctx = nullptr;
    uint32_t w = 0, h = 0;           // geometry the codec is open for
    uint32_t streamCap = 0;
    uint8_t* scratch = nullptr;      // the restuffed stream (built in cached memory, then handed to the codec)
    size_t scratchAlloc = 0;
    uint8_t* vplane[3] = {nullptr, nullptr, nullptr};   // software picture for the check
    size_t vplaneAlloc[3] = {0, 0, 0};
    bool dead = false;               // given up for the rest of the run
    int failStreak = 0;
    int verifyLeft = 0;
    volatile int releasePending = 0;
    volatile uint32_t busyStamp = 0;     // a lane is inside a codec call: (call number << 2) | 1 decode, | 3 open; 0 = none. A number, not a time: the
                                         // stuck time is measured by the game thread in slices it has seen (HwWatch), so a clock that jumps costs nothing
    uint32_t busySeq = 0;                // calls stamped so far (under the codec lock)
    uint32_t stuckHandled = 0;           // game thread: the busy stamp already given up
    uint32_t wStamp = 0, wAccMs = 0, wLastMs = 0;   // game thread: the stamp being watched, the stuck time seen, when it was last looked at
    bool watchdogDead = false;           // `dead` was set by the watchdog (not by HwGiveUp): it heals when the call returned
    volatile uint32_t lockTo = 0;        // lanes that left the codec's lock loop for the stuck call (atomic)
    struct Stats {
        unsigned long long frames = 0, fellBack = 0, bridgeRefused = 0, codecRefused = 0, opened = 0, stuck = 0, lockTimeouts = 0;
        unsigned long long usBridge = 0, usDecode = 0, usTile = 0, usVerify = 0;
    } st, reported;
};
Hw g_hw;

#if !defined(NP_VITA)
// ---- host stand-in codec ----
struct FakeCtx {
    uint32_t w, h, pitch;
    uint8_t* tiled[3];
    uint8_t* lin;
    uint8_t* unstuffed;
    size_t unstuffedCap;
    unsigned long long calls;
};
int FakeFailEvery() { const char* e = getenv("MSC_THP_HWJPEG_FAKE_FAIL"); return e ? atoi(e) : 0; }
int FakeOpen(void** ctx, const uint8_t* probe, uint32_t probeSize, uint32_t maxStream, uint32_t* width, uint32_t* height) {
    np_thp::JpegInfo info;
    // `probe` is a standard stream here: locate the header the same way, the scan marker ends it
    np_thp::Frame f;
    if (!np_thp::PeekFrame(probe, f)) return -1;
    (void)info; (void)probeSize; (void)maxStream;
    FakeCtx* c = (FakeCtx*)calloc(1, sizeof(FakeCtx));
    c->w = f.width;
    c->h = f.height;
    c->pitch = f.width + 32;   // a pitch wider than the picture, as a codec may produce
    const size_t y = (size_t)c->pitch * c->h, uv = (size_t)(c->pitch / 2) * (c->h / 2);
    c->lin = (uint8_t*)malloc(y + 2 * uv);
    for (int i = 0; i < 3; ++i) c->tiled[i] = (uint8_t*)malloc(i == 0 ? (size_t)f.width * f.height : (size_t)(f.width / 2) * (f.height / 2));
    c->unstuffedCap = (size_t)maxStream + 4096;
    c->unstuffed = (uint8_t*)malloc(c->unstuffedCap);
    *width = c->w;
    *height = c->h;
    *ctx = c;
    return 0;
}
void FakeDetile(const uint8_t* tiled, uint32_t w, uint32_t h, uint32_t pitch, uint8_t* dst) {
    const uint32_t tilesX = w / 8;
    for (uint32_t ty = 0; ty < h / 4; ++ty)
        for (uint32_t tx = 0; tx < tilesX; ++tx)
            for (uint32_t r = 0; r < 4; ++r) memcpy(dst + (size_t)(ty * 4 + r) * pitch + tx * 8, tiled + ((size_t)ty * tilesX + tx) * 32 + r * 8, 8);
}
int FakeDecode(void* ctx, const uint8_t* jpeg, uint32_t size, const uint8_t** planes, uint32_t* pw, uint32_t* ph) {
    FakeCtx* c = (FakeCtx*)ctx;
    ++c->calls;
    const int every = FakeFailEvery();
    if (every > 0 && c->calls % (unsigned)every == 0) return -7;
    static const int delayMs = getenv("MSC_THP_HWJPEG_FAKE_DELAY_MS") ? atoi(getenv("MSC_THP_HWJPEG_FAKE_DELAY_MS")) : 0;
    if (delayMs > 0) np_sleep_ns((uint64_t)delayMs * 1000000u);
    // MSC_THP_HWJPEG_FAKE_SLOW_EVERY=N, _SLOW_MS=ms: every Nth call takes that long on top (the occasional slow picture of a busy scene or a card read)
    static const int slowEvery = getenv("MSC_THP_HWJPEG_FAKE_SLOW_EVERY") ? atoi(getenv("MSC_THP_HWJPEG_FAKE_SLOW_EVERY")) : 0;
    if (slowEvery > 0 && c->calls % (unsigned)slowEvery == 0) np_sleep_ns((uint64_t)(getenv("MSC_THP_HWJPEG_FAKE_SLOW_MS") ? atoi(getenv("MSC_THP_HWJPEG_FAKE_SLOW_MS")) : 150) * 1000000u);
    // MSC_THP_HWJPEG_FAKE_HANG_AT=N (+ _FAKE_HANG_MS, default 3000): the Nth call of the codec does not return for that long (a codec
    // that hangs: the lanes must not freeze the picture, see MSC_THP_RECOVER)
    static const int hangAt = getenv("MSC_THP_HWJPEG_FAKE_HANG_AT") ? atoi(getenv("MSC_THP_HWJPEG_FAKE_HANG_AT")) : 0;
    if (hangAt > 0 && (int)c->calls == hangAt) {
        const int ms = getenv("MSC_THP_HWJPEG_FAKE_HANG_MS") ? atoi(getenv("MSC_THP_HWJPEG_FAKE_HANG_MS")) : 3000;
        np_sleep_ns((uint64_t)ms * 1000000u);
    }
    if (size > c->unstuffedCap - 4096) return -1;
    // standard stream -> THP stream (what the bridge undid), decoded by the software decoder
    np_thp::JpegInfo ji;
    // the header ends where the scan marker's segment ends
    size_t pos = 2;
    for (;;) {
        if (pos + 4 > size || jpeg[pos] != 0xFF) return -2;
        const uint8_t m = jpeg[pos + 1];
        const size_t len = ((size_t)jpeg[pos + 2] << 8) | jpeg[pos + 3];
        pos += 2 + len;
        if (m == 0xDA) break;
    }
    (void)ji;
    const size_t n = np_thp::UnstuffScan(jpeg, size - 2, pos, c->unstuffed);   // without the end-of-image marker
    memset(c->unstuffed + n, 0, 64);
    np_thp::Tables tables;
    static const uint8_t kNat[80] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                                     35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
                                     63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63};
    static const double kAan[8] = {1.0, 1.387039845, 1.306562965, 1.175875602, 1.0, 0.785694958, 0.541196100, 0.275899379};
    tables.naturalOrder = kNat;
    for (int i = 0; i < 8; ++i) tables.aan[i] = (double)(float)kAan[i];
    np_thp::Frame fr;
    bool supported = true;
    if (np_thp::DecodeFrame(c->unstuffed, c->tiled[0], c->tiled[1], c->tiled[2], tables, fr, supported) != 0 || !supported) return -3;
    memset(c->lin, 0xEE, (size_t)c->pitch * c->h * 3 / 2);   // the padding must never reach the pictures
    uint8_t* y = c->lin;
    uint8_t* cb = y + (size_t)c->pitch * c->h;
    uint8_t* cr = cb + (size_t)(c->pitch / 2) * (c->h / 2);
    FakeDetile(c->tiled[0], c->w, c->h, c->pitch, y);
    FakeDetile(c->tiled[1], c->w / 2, c->h / 2, c->pitch / 2, cb);
    FakeDetile(c->tiled[2], c->w / 2, c->h / 2, c->pitch / 2, cr);
    static const bool bad = getenv("MSC_THP_HWJPEG_FAKE_BAD") != nullptr;
    if (bad) memset(c->lin, 0x40, (size_t)c->pitch * c->h * 3 / 2);
    *planes = c->lin;
    *pw = c->pitch;
    *ph = c->h;
    return 0;
}
void FakeClose(void* ctx) {
    FakeCtx* c = (FakeCtx*)ctx;
    if (!c) return;
    for (int i = 0; i < 3; ++i) free(c->tiled[i]);
    free(c->lin);
    free(c->unstuffed);
    free(c);
}
const NpJpegCodec kFakeCodec = {"fake", FakeOpen, FakeDecode, FakeClose};
#endif

int64_t g_clockSkewUs = 0;   // test only: a clock that jumps (tests/test_thp_delay.cpp); 0 in the game
uint64_t NowUs() { return np_time_real_ns() / 1000u + (uint64_t)g_clockSkewUs; }

bool HwDecided() {
    if (g_hw.mode < 0) {
        np_mutex_init(&g_hw.mu);
        g_hw.muInit = true;
        int mode = 0;
#if defined(NP_VITA)
        mode = 1;
#endif
        if (const char* e = getenv("MSC_THP_HWJPEG")) {
            if (*e == '0') mode = 0;
            else if (!strcmp(e, "fake")) mode = 2;
            else mode = 1;
        }
        if (mode == 1) g_hw.codec = NpJpegCodecVita();
#if !defined(NP_VITA)
        if (mode == 2) g_hw.codec = &kFakeCodec;
#endif
        if (mode != 0 && !g_hw.codec) {
            fprintf(stderr, "[thp-hw] MSC_THP_HWJPEG is on but this build has no JPEG codec: software decoding\n");
            mode = 0;
        }
        if (g_lanes.enabled != 1) mode = 0;
        g_hw.verifyLeft = 2;
        if (const char* v = getenv("MSC_THP_HWJPEG_VERIFY")) g_hw.verifyLeft = atoi(v);
        g_hw.mode = mode;
        if (mode) fprintf(stderr, "[thp-hw] hardware JPEG decoding of movie frames: %s (verify the first %d frames of each movie)\n", g_hw.codec->name, g_hw.verifyLeft);
    }
    return g_hw.mode != 0;
}

void HwCloseLocked() {
    if (g_hw.ctx) {
        g_hw.codec->close(g_hw.ctx);
        g_hw.ctx = nullptr;
    }
    g_hw.w = g_hw.h = 0;
    g_hw.releasePending = 0;
}

bool EnsureBuf(uint8_t*& p, size_t& alloc, size_t need) {
    if (alloc >= need) return true;
    free(p);
    p = (uint8_t*)malloc(need);
    alloc = p ? need : 0;
    return p != nullptr;
}

void HwGiveUp(const char* why) {
    if (!g_hw.dead) fprintf(stderr, "[thp-hw] hardware JPEG decoding switched off for this run: %s\n", why);
    __atomic_store_n(&g_hw.dead, true, __ATOMIC_RELEASE);
    HwCloseLocked();
}
uint32_t BusyStamp(uint32_t kind) { return (++g_hw.busySeq << 2) | kind; }

// Decodes the slot's frame with the engine into the slot's planes. Returns true when the planes are the frame. Runs on a lane.
bool HwDecodeLocked(Slot& sl) {
    if (__atomic_load_n(&g_hw.dead, __ATOMIC_ACQUIRE)) return false;
    np_thp::JpegInfo info;
    const uint64_t t0 = NowUs();
    int r = np_thp::ScanFrame(sl.copy, sl.copySize, info);
    if (r != np_thp::kBridgeOk || info.width != sl.w || info.height != sl.h || (info.width & 15) || (info.height & 15)) {
        ++g_hw.st.bridgeRefused;
        static int logged = 0;
        if (logged++ < 4) fprintf(stderr, "[thp-hw] frame %d of %s not handed to the codec (bridge code %d, %ux%u)\n", sl.frame, g_movieName, r, info.width, info.height);
        return false;
    }
    const size_t need = np_thp::StuffedSize(info);
    if (!EnsureBuf(g_hw.scratch, g_hw.scratchAlloc, need + 64)) return false;
    size_t outSize = 0;
    r = np_thp::MakeStandardJpeg(sl.copy, sl.copySize, g_hw.scratch, g_hw.scratchAlloc, &outSize);
    if (r != np_thp::kBridgeOk) {
        ++g_hw.st.bridgeRefused;
        return false;
    }
    const uint64_t t1 = NowUs();
    if (g_hw.ctx && (g_hw.w != sl.w || g_hw.h != sl.h)) HwCloseLocked();   // another movie
    if (g_hw.ctx && outSize > g_hw.streamCap) {
        ++g_hw.st.bridgeRefused;   // bigger than anything the buffers were sized for: this frame is decoded in software
        return false;
    }
    if (!g_hw.ctx) {
        uint32_t cw = 0, ch = 0;
        uint32_t cap = (uint32_t)(outSize + outSize / 2);
        if (cap < 128u * 1024u) cap = 128u * 1024u;
        cap = (cap + 255u) & ~255u;
        __atomic_store_n(&g_hw.busyStamp, BusyStamp(3u), __ATOMIC_RELEASE);   // (the open has a longer limit, see HwWatch)
        const int o = g_hw.codec->open(&g_hw.ctx, g_hw.scratch, (uint32_t)outSize, cap, &cw, &ch);
        __atomic_store_n(&g_hw.busyStamp, 0u, __ATOMIC_RELEASE);
        if (o != 0 || cw != sl.w || ch != sl.h) {
            if (o == 0) {
                g_hw.codec->close(g_hw.ctx);
                g_hw.ctx = nullptr;
            }
            ++g_hw.st.codecRefused;
            if (++g_hw.failStreak >= 3 || o != 0) HwGiveUp(o != 0 ? "the codec could not be opened" : "the codec reports another picture size");
            return false;
        }
        g_hw.w = cw;
        g_hw.h = ch;
        g_hw.streamCap = cap;
        ++g_hw.st.opened;
    }
    const uint8_t* planes = nullptr;
    uint32_t pw = 0, ph = 0;
    __atomic_store_n(&g_hw.busyStamp, BusyStamp(1u), __ATOMIC_RELEASE);
    r = g_hw.codec->decode(g_hw.ctx, g_hw.scratch, (uint32_t)outSize, &planes, &pw, &ph);
    __atomic_store_n(&g_hw.busyStamp, 0u, __ATOMIC_RELEASE);
    const uint64_t t2 = NowUs();
    if (r != 0 || !planes || pw < sl.w || ph < sl.h || (pw & 15) != 0) {
        ++g_hw.st.codecRefused;
        static int logged = 0;
        if (logged++ < 8) fprintf(stderr, "[thp-hw] codec refused frame %d of %s: code 0x%08x (%u bytes, pitch %u x %u)\n", sl.frame, g_movieName, (unsigned)r, (unsigned)outSize, pw, ph);
        if (++g_hw.failStreak >= 8) HwGiveUp("the codec refused 8 frames in a row");
        return false;
    }
    g_hw.failStreak = 0;
    np_thp::TilePlane(planes, pw, sl.w, sl.h, sl.plane[0]);
    const uint8_t* cb = planes + (size_t)pw * ph;
    const uint8_t* cr = cb + (size_t)(pw / 2) * (ph / 2);
    np_thp::TilePlane(cb, pw / 2, sl.w / 2, sl.h / 2, sl.plane[1]);
    np_thp::TilePlane(cr, pw / 2, sl.w / 2, sl.h / 2, sl.plane[2]);
    const uint64_t t3 = NowUs();
    g_hw.st.usBridge += t1 - t0;
    g_hw.st.usDecode += t2 - t1;
    g_hw.st.usTile += t3 - t2;
    ++g_hw.st.frames;

    if (g_hw.verifyLeft > 0) {
        // The first frames of a movie also go through the software decoder: a different picture size, plane order or
        // pitch on the real engine would show as a large difference here instead of on the screen.
        --g_hw.verifyLeft;
        bool ok = true;
        bool alloc = true;
        for (int i = 0; i < 3; ++i) alloc = alloc && EnsureBuf(g_hw.vplane[i], g_hw.vplaneAlloc[i], sl.planeBytes[i]);
        if (alloc && NpThpDecodeNow(sl.copy, g_hw.vplane[0], g_hw.vplane[1], g_hw.vplane[2], sl.copy + sl.copySize) == 0) {
            double worst = 0;
            unsigned worstMax = 0;
            for (int i = 0; i < 3; ++i) {
                double mean;
                unsigned mx;
                np_thp::PlaneDiff(sl.plane[i], g_hw.vplane[i], sl.planeBytes[i], mean, mx);
                if (mean > worst) worst = mean;
                if (mx > worstMax) worstMax = mx;
            }
            fprintf(stderr, "[thp-hw] check of frame %d of %s against the software decoder: mean difference %.3f, largest %u\n", sl.frame, g_movieName, worst, worstMax);
            if (worst > 3.0) {
                ok = false;
                for (int i = 0; i < 3; ++i) memcpy(sl.plane[i], g_hw.vplane[i], sl.planeBytes[i]);   // this frame: the software picture
                HwGiveUp("the codec's picture differs from the software decoder's");
            }
        }
        g_hw.st.usVerify += NowUs() - t3;
        (void)ok;
    }
    return true;
}

bool HwTry(Slot& sl) {
    if (g_hw.mode <= 0) return false;   // (decided by the game thread before the first lane started)
    if (g_lanes.recover > 0) {
        // The codec is one context behind one lock. A lane that waits for it for longer than any decode takes (another lane is stuck in
        // a codec call) decodes this frame in software instead of waiting for ever; a codec given up is not asked again.
        if (__atomic_load_n(&g_hw.dead, __ATOMIC_ACQUIRE)) return false;
        // Leaves the loop when the holder has been inside one codec call for ~60 ms (a decode takes a few ms: it is the stuck call), or after
        // 250 ms in all (measured with the clock, not by counting sleeps).
        const uint64_t t0 = NowUs();
        uint32_t seenStamp = 0;
        uint64_t seenAt = 0;
        while (!np_mutex_trylock(&g_hw.mu)) {
            if (__atomic_load_n(&g_hw.dead, __ATOMIC_ACQUIRE)) return false;
            const uint64_t now = NowUs();
            const uint32_t stamp = __atomic_load_n(&g_hw.busyStamp, __ATOMIC_ACQUIRE);
            if (stamp != seenStamp) { seenStamp = stamp; seenAt = now; }
            if (now - t0 >= 250000u || (stamp && now - seenAt >= 60000u)) {
                __atomic_add_fetch(&g_hw.lockTo, 1u, __ATOMIC_RELAXED);
                return false;
            }
            np_sleep_ns(500000u);
        }
        const bool ok = HwDecodeLocked(sl);
        if (!ok) ++g_hw.st.fellBack;
        if (g_hw.releasePending) HwCloseLocked();
        np_mutex_unlock(&g_hw.mu);
        return ok;
    }
    np_mutex_lock(&g_hw.mu);
    const bool ok = HwDecodeLocked(sl);
    if (!ok) ++g_hw.st.fellBack;
    if (g_hw.releasePending) HwCloseLocked();
    np_mutex_unlock(&g_hw.mu);
    return ok;
}

// The game thread: a movie ended or another starts. Frees the codec's memory without waiting for a lane that is using it (that lane
// frees it when it is done).
void HwRelease() {
    if (g_hw.mode <= 0) return;
    int verify = 2;
    if (const char* v = getenv("MSC_THP_HWJPEG_VERIFY")) verify = atoi(v);
    __atomic_store_n(&g_hw.verifyLeft, verify, __ATOMIC_RELAXED);
    // A watchdog that fired on a call that then returned was a false alarm (a suspend, a lane kept off its core): the codec is used again.
    // A call that really hangs never returns, so its alarm stays.
    if (g_hw.watchdogDead && !__atomic_load_n(&g_hw.busyStamp, __ATOMIC_ACQUIRE)) {
        g_hw.watchdogDead = false;
        __atomic_store_n(&g_hw.dead, false, __ATOMIC_RELEASE);
        fprintf(stderr, "[thp-hw] the call the watchdog gave up has returned: hardware JPEG decoding is used again\n");
    }
    if (np_mutex_trylock(&g_hw.mu)) {
        HwCloseLocked();
        np_mutex_unlock(&g_hw.mu);
    } else {
        __atomic_store_n(&g_hw.releasePending, 1, __ATOMIC_RELEASE);
    }
}

// The game thread, every call (MSC_THP_RECOVER): a lane that has been inside the codec for longer than MSC_THP_HWSTUCK_MS (default 700; a
// frame takes a few ms) is stuck there for good as far as the picture is concerned. The codec is switched off for the run (the stuck lane
// holds its lock and its context, which are left alone), the other lanes decode in software from now on, and `*lost` says a lane was given
// up so the caller can put another in its place.
uint32_t HwStuckLimitMs() {
    static const uint32_t limit = [] { const char* e = getenv("MSC_THP_HWSTUCK_MS"); const int v = e ? atoi(e) : 700; return (uint32_t)(v < 20 ? 20 : v); }();
    return limit;
}
// tools15 (movie14 review L4: "a lane stuck outside the codec is not watched"): COUNT, never act. A lane inside the software decoder for MSC_THP_SWSTUCK_MS (default 1500;
// a software frame is tens of ms) is counted once per call and logged once; its slot stays running as before (no lane is replaced, nothing is decoded elsewhere).
void SwWatch() {
    static const uint32_t limit = [] { const char* e = getenv("MSC_THP_SWSTUCK_MS"); const int v = e ? atoi(e) : 1500; return (uint32_t)(v < 50 ? 50 : v); }();
    const uint32_t nowMs = (uint32_t)(NowUs() / 1000u) | 1u;
    for (int i = 0; i < kMaxSlots; ++i) {
        Slot& sl = g_lanes.slot[i];
        const uint32_t since = __atomic_load_n(&sl.swSinceMs, __ATOMIC_ACQUIRE);
        if (!since || since == sl.swFlagged || (uint32_t)(nowMs - since) < limit) continue;
        sl.swFlagged = since;
        ++g_lanes.st.swStuck;
        fprintf(stderr, "[thp-hw] a lane has been inside the software decoder of %s for %u ms (frame %d, slot %d): not watched, not replaced (counted only)\n", g_movieName, (unsigned)(nowMs - since), (int)sl.frame, i);
    }
}
bool HwWatch() {
    SwWatch();
    if (g_lanes.recover <= 0 || g_hw.mode <= 0) return false;
    const uint32_t stamp = __atomic_load_n(&g_hw.busyStamp, __ATOMIC_ACQUIRE);
    const uint32_t nowMs = (uint32_t)(NowUs() / 1000u);
    if (!stamp || stamp == g_hw.stuckHandled) { g_hw.wStamp = 0; return false; }
    if (stamp != g_hw.wStamp) {   // a new call: counted from now
        g_hw.wStamp = stamp;
        g_hw.wAccMs = 0;
        g_hw.wLastMs = nowMs;
        return false;
    }
    // Stuck time is what this thread has watched, at most 50 ms per look: a suspend, a clock that jumps or a game thread that was not
    // scheduled adds 50 ms, not the whole gap.
    uint32_t d = nowMs - g_hw.wLastMs;
    if (d > 50u) d = 50u;
    g_hw.wLastMs = nowMs;
    g_hw.wAccMs += d;
    const uint32_t limit = HwStuckLimitMs() * ((stamp & 3u) == 3u ? 4u : 1u);   // the codec's open may take longer than a frame
    if (g_hw.wAccMs < limit) return false;
    g_hw.stuckHandled = stamp;
    ++g_hw.st.stuck;
    ++g_lanes.st.lanesLost;
    g_hw.watchdogDead = true;
    __atomic_store_n(&g_hw.dead, true, __ATOMIC_RELEASE);
    fprintf(stderr, "[thp-hw] a codec call of %s has not returned for %u ms: hardware JPEG decoding switched off, that lane is given up and the frames are decoded in software\n",
            g_movieName, (unsigned)g_hw.wAccMs);
    return true;
}

void RunJob(Slot& sl) {
    sl.viaHw = HwTry(sl);
    if (sl.viaHw) { sl.result = 0; return; }
    __atomic_store_n(&sl.swSinceMs, (uint32_t)(NowUs() / 1000u) | 1u, __ATOMIC_RELEASE);
    sl.result = NpThpDecodeNow(sl.copy, sl.plane[0], sl.plane[1], sl.plane[2], sl.copy + sl.copySize);
    __atomic_store_n(&sl.swSinceMs, 0u, __ATOMIC_RELEASE);
}

void* LaneMain(void* arg) {
    Slot& sl = *(Slot*)arg;
    for (;;) {
        np_sem_wait(&sl.wake);
        __atomic_store_n(&sl.state, (int)kRunning, __ATOMIC_RELEASE);
        RunJob(sl);
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

// ---- the delay line's workers (MSC_THP_DELAY > 0) ----
// MSC_THP_DEPTH worker threads share the slots: a worker takes, of the queued frames, the one that is due first and can still be finished
// in time (np_thp_pace::PickJob), after dropping the queued frames nobody will show any more (JobStale: they finish as kSkipped without
// a decode). The game thread tells them where the movie is (PoolNote) at every call.
struct Pool {
    NpMutex mu;
    NpSem jobs;
    bool init = false;
    NpThread* thread[kMaxSlots] = {};
    int started = 0;
    int64_t nextFrame = 0, base = 0, lastShown = -1, delay = 0, periodUs = 33333;
    bool anyShown = false;
    uint64_t lastCallUs = 0;
    double needUs = 0.0;             // smoothed decode time of the jobs the workers ran
} g_pool;

void SlotInit(Slot& sl) {
    if (sl.inited) return;
    np_sem_init(&sl.wake, 0);
    np_sem_init(&sl.done, 0);
    sl.inited = true;
}

Slot* PoolPickLocked() {
    int64_t seqs[kMaxSlots];
    int idx[kMaxSlots], n = 0;
    for (int i = 0; i < kMaxSlots; ++i) {
        Slot& sl = g_lanes.slot[i];
        if (__atomic_load_n(&sl.state, __ATOMIC_ACQUIRE) != kQueued) continue;
        if (sl.gen != g_lanes.gen || sl.retired || np_thp_pace::JobStale(sl.frame, g_pool.base, g_pool.nextFrame, g_pool.delay, g_pool.anyShown, g_pool.lastShown)) {
            sl.result = kSkipped;
            sl.viaHw = false;
            ++g_lanes.st.jobsDropped;
            __atomic_store_n(&sl.state, (int)kDone, __ATOMIC_RELEASE);
            np_sem_post(&sl.done);
            continue;
        }
        seqs[n] = sl.frame;
        idx[n++] = i;
    }
    if (!n) return nullptr;
    const uint64_t now = NowUs();
    const int64_t since = g_pool.lastCallUs && now > g_pool.lastCallUs ? (int64_t)(now - g_pool.lastCallUs) : 0;
    const int pick = np_thp_pace::PickJob(seqs, n, g_pool.nextFrame, g_pool.delay, since, g_pool.periodUs, (int64_t)(g_pool.needUs * 1.25));
    Slot* sl = &g_lanes.slot[idx[pick]];
    __atomic_store_n(&sl->state, (int)kRunning, __ATOMIC_RELEASE);
    return sl;
}

void* PoolMain(void*) {
    for (;;) {
        np_sem_wait(&g_pool.jobs);
        np_mutex_lock(&g_pool.mu);
        Slot* sl = PoolPickLocked();
        np_mutex_unlock(&g_pool.mu);
        if (!sl) continue;
        const uint64_t t0 = NowUs();
        RunJob(*sl);
        const double took = (double)(NowUs() - t0);
        np_mutex_lock(&g_pool.mu);
        g_pool.needUs = g_pool.needUs == 0.0 ? took : g_pool.needUs + (took - g_pool.needUs) / 8.0;
        np_mutex_unlock(&g_pool.mu);
        __atomic_store_n(&sl->state, (int)kDone, __ATOMIC_RELEASE);
        np_sem_post(&sl->done);
    }
    return nullptr;
}

// Makes sure `want` workers exist (a lane given up is replaced by one more). False when no thread could be made at all.
bool PoolEnsure(int want) {
    if (!g_pool.init) {
        np_mutex_init(&g_pool.mu);
        np_sem_init(&g_pool.jobs, 0);
        g_pool.init = true;
    }
    if (want > kMaxSlots) want = kMaxSlots;
    while (g_pool.started < want) {
        NpThread* t = np_thread_create("thp-decode", NP_ROLE_HELPER, PoolMain, nullptr, 0, 262144);
        if (!t) break;
        np_thread_detach(t);
        g_pool.thread[g_pool.started++] = t;
    }
    return g_pool.started > 0;
}

// game thread: where the movie is, for the workers' choices
void PoolNote(int64_t nextFrame) {
    if (!g_pool.init) return;
    np_mutex_lock(&g_pool.mu);
    g_pool.nextFrame = nextFrame;
    g_pool.base = g_lanes.base < 0 ? 0 : g_lanes.base;
    g_pool.delay = g_lanes.delay;
    g_pool.anyShown = g_lanes.lastShown >= 0;
    g_pool.lastShown = g_lanes.lastShown;
    g_pool.lastCallUs = g_pace.lastCallUs;
    g_pool.periodUs = (int64_t)g_pace.periodUs;
    np_mutex_unlock(&g_pool.mu);
}

// A lane was given up (HwWatch): another takes its place (a slot more, a worker more).
void PoolReplaceLostLane() {
    if (g_lanes.depth < kMaxSlots) ++g_lanes.depth;
    if (g_lanes.delay > 0) PoolEnsure(g_lanes.depth);
}

bool LanesStarted() {
    if (g_pool.started > 0) return true;
    for (int i = 0; i < kMaxSlots; ++i)
        if (g_lanes.slot[i].thread) return true;
    return false;
}

// game thread, state != kIdle: returns with the job finished and its done token taken (the result stays valid until SlotRelease)
void SlotWait(Slot& sl) {
    if (!sl.tokenTaken) {
        np_sem_wait(&sl.done);   // (posted by the lane after it set kDone)
        sl.tokenTaken = true;
    }
}
// the same, for at most `ns` (MSC_THP_RECOVER): false when the lane is not done by then (its token stays; the slot is released when it is)
bool SlotWaitFor(Slot& sl, uint64_t ns) {
    if (sl.tokenTaken) return true;
    if (!np_sem_timedwait_ns(&sl.done, ns)) return false;
    sl.tokenTaken = true;
    return true;
}
void SlotRelease(Slot& sl) {
    sl.retired = false;
    __atomic_store_n(&sl.state, (int)kIdle, __ATOMIC_RELEASE);
}
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

bool SlotMatches(const Slot& sl, const void* file, int frame, unsigned width, unsigned height) {
    return sl.src == (const uint8_t*)file && sl.frame == frame && sl.w == width && sl.h == height && sl.gen == g_lanes.gen;
}

// nowait: releases every finished slot nobody will use: retired, of another movie, or far from the frame being asked for
// (a loop restarted the numbering).
void ReleaseUnwanted(int frame) {
    for (int i = 0; i < kMaxSlots; ++i) {
        Slot& sl = g_lanes.slot[i];
        if (SlotState(sl) != kDone) continue;
        const bool far = sl.frame > frame + 16 || sl.frame < frame - 64;
        if (sl.retired || sl.gen != g_lanes.gen || far) {
            SlotWait(sl);
            SlotRelease(sl);
            ++g_lanes.st.retiredFrames;
        }
    }
}
} // namespace

// A frame NpThpPrefetch can take at all: the same test is used by NpThpVideoDecode to tell "not queued yet" from "can never be queued".
static bool FrameEligible(const void* video, unsigned size, unsigned width, unsigned height) {
    return video && size != 0 && size <= kMaxFrameBytes && width != 0 && (width & 15u) == 0 && width <= 1024 && height <= 1024;
}
static void NoteRejected(const void* video, int frame) {
    g_lanes.rejSrc = (const uint8_t*)video;
    g_lanes.rejFrame = frame;
    g_lanes.rejGen = g_lanes.gen;
}
static void ClearRejected(const void* video, int frame) {
    if (g_lanes.rejSrc == video && g_lanes.rejFrame == frame && g_lanes.rejGen == g_lanes.gen) {
        g_lanes.rejSrc = nullptr;
        g_lanes.rejFrame = -1;
        g_lanes.rejGen = 0;
    }
}

// Player -> lanes: frame `frame` (the video component starts at `video`, `size` bytes) is due soon. A frame some lane already has is not queued again.
void NpThpPrefetch(const void* video, unsigned size, int frame, unsigned width, unsigned height) {
    if (!LanesEnabled() || !FrameEligible(video, size, width, height) || !g_thp_init) return;
    HwDecided();
    if (HwWatch()) PoolReplaceLostLane();
    if (g_lanes.nowait) ReleaseUnwanted(frame);
    Slot* freeSlot = nullptr;
    const int nslots = NSlots();
    for (int i = 0; i < nslots; ++i) {
        Slot& sl = g_lanes.slot[i];
        if (SlotState(sl) != kIdle) {
            if (!sl.retired && SlotMatches(sl, video, frame, width, height)) {
                ClearRejected(video, frame);
                return;   // already queued or done
            }
        } else if (!freeSlot) {
            freeSlot = &sl;
        }
    }
    if (!freeSlot) { ++g_lanes.st.dropped; return; }
    const bool pooled = g_lanes.delay > 0;
    if (pooled ? !PoolEnsure(g_lanes.depth) : !SlotStart(*freeSlot)) { NoteRejected(video, frame); return; }
    if (pooled) SlotInit(*freeSlot);
    if (!EnsurePlanes(*freeSlot, width, height)) { NoteRejected(video, frame); return; }
    if (freeSlot->copyAlloc < size + kCopyPad) {
        free(freeSlot->copy);
        freeSlot->copy = (uint8_t*)malloc(size + kCopyPad + 4096);
        freeSlot->copyAlloc = freeSlot->copy ? size + kCopyPad + 4096 : 0;
        if (!freeSlot->copy) { NoteRejected(video, frame); return; }
    }
    memcpy(freeSlot->copy, video, size);
    memset(freeSlot->copy + size, 0, kCopyPad);
    freeSlot->copySize = size;
    freeSlot->src = (const uint8_t*)video;
    freeSlot->frame = frame;
    freeSlot->w = width;
    freeSlot->h = height;
    freeSlot->gen = g_lanes.gen;
    freeSlot->retired = false;
    freeSlot->tokenTaken = false;
    ++g_lanes.st.prefetched;
    __atomic_store_n(&freeSlot->state, (int)kQueued, __ATOMIC_RELEASE);
    ClearRejected(video, frame);
    np_sem_post(pooled ? &g_pool.jobs : &freeSlot->wake);
}

static void NpThpLogError(int frame, int r) {
    ++g_lanes.st.errors;
    static int logged = 0;
    if (logged++ < 16) fprintf(stderr, "[thp] video frame %d of %s cannot be decoded (code %d: damaged or truncated data)\n", frame, g_movieName, r);
}

// The player's call (VideoDecode in THPSimple.cpp): the same contract as THPVideoDecode, plus the frame number, the size and the size
// of the video component.
int NpThpVideoDecode(void* file, void* tileY, void* tileU, void* tileV, void* work, int frame, unsigned width, unsigned height, unsigned size) {
    (void)work;
    if (!file) return 1;
    if (!tileY || !tileU || !tileV) return 2;
    if (!g_thp_init) return 7;
    if (!size) {
        NpThpLogError(frame, np_thp::kBadSyntax);
        return np_thp::kBadSyntax;
    }
    const uint8_t* end = (const uint8_t*)file + size;
    PaceCall();
    if (g_lanes.enabled == 1 && g_lanes.nowait) {
        if (HwWatch()) PoolReplaceLostLane();
        const bool dly = g_lanes.delay > 0;
        if (frame < g_lanes.lastShown || (dly && frame < g_lanes.lastAsked)) {   // a looping movie started over
            g_lanes.lastShown = -1;
            g_lanes.base = -1;
        }
        if (g_lanes.base < 0) g_lanes.base = frame;
        g_lanes.lastAsked = frame;
        const bool anyLane = LanesStarted();
        // The delay line: the frame due at this call (delay 0: the frame asked for). Without a lane the game thread decodes the frame asked for.
        const int64_t delay = dly && anyLane ? g_lanes.delay : 0;
        if (dly) {
            NpThpPrefetch(file, size, frame, width, height);   // a frame the player's look-ahead could not queue (all slots busy) is queued now: it is due `delay` calls from now
            PoolNote((int64_t)frame + 1);
        }
        const int64_t due = np_thp_pace::Due(delay ? g_lanes.base : 0, frame, delay);
        Slot* hit = nullptr;
        Slot* show = nullptr;
        {
            int64_t seqs[kMaxSlots];
            bool done[kMaxSlots];
            Slot* cand[kMaxSlots];
            int n = 0;
            const int nslots = NSlots();
            for (int i = 0; i < nslots; ++i) {
                Slot& sl = g_lanes.slot[i];
                if (SlotState(sl) == kIdle || sl.retired || sl.gen != g_lanes.gen || sl.w != width || sl.h != height) continue;
                if (SlotMatches(sl, file, frame, width, height)) hit = &sl;
                // the newest finished frame that is not newer than the one due and newer than the picture on screen (delay 0: the old rule)
                seqs[n] = sl.frame;
                done[n] = SlotState(sl) == kDone && !(delay && sl.result != 0);
                cand[n++] = &sl;
            }
            const int k = np_thp_pace::ChooseShow(seqs, done, n, delay ? g_lanes.base : 0, frame, delay, g_lanes.lastShown >= 0, g_lanes.lastShown);
            if (k >= 0) show = cand[k];
        }
        // the first picture of a movie is waited for; a frame that no lane can ever have is decoded here
        const bool fix = g_lanes.firstFix != 0;
        const bool first = fix && g_lanes.lastShown < 0;
        const bool rejected = fix && (!FrameEligible(file, size, width, height) || (g_lanes.rejSrc == (const uint8_t*)file && g_lanes.rejFrame == frame && g_lanes.rejGen == g_lanes.gen));
        // A rejected current frame must use the fallback, not a completed earlier picture.
        if (rejected) show = hit = nullptr;
        if (!show && hit && first) {
            // bounded: one frame of one lane. MSC_THP_RECOVER: waited for in 50 ms slices, the codec watchdog looks between them, and the wait ends
            // at the watchdog or at min(1.5 s, its limit + 100 ms): a lane stuck in the codec must not hold the movie's start (the frame is then decoded here)
            bool got = false;
            if (g_lanes.recover > 0) {
                uint32_t capMs = 1500u;
                if (g_hw.mode > 0 && HwStuckLimitMs() + 100u < capMs) capMs = HwStuckLimitMs() + 100u;
                for (uint32_t waited = 0; waited < capMs && !got; waited += 50u) {
                    got = SlotWaitFor(*hit, 50u * 1000000u);
                    if (!got && HwWatch()) { PoolReplaceLostLane(); break; }
                }
            } else {
                SlotWait(*hit);
                got = true;
            }
            if (got) {
                ++g_lanes.st.waits;
                show = hit;
            } else {
                ++g_lanes.st.firstTimeouts;
                static int logged = 0;
                if (logged++ < 4) fprintf(stderr, "[thp] %s: the lane did not finish the first picture in time: it is decoded here\n", g_movieName);
                hit = nullptr;
            }
        }
        const bool newDue = due > g_lanes.lastShown;   // a frame newer than the picture on the screen should be on the screen by now
        int ret = 0;
        bool fresh = false;
        int64_t shownFrame = -1;
        if (show) {
            SlotWait(*show);   // (finished: the token is there)
            ++(show == hit ? g_lanes.st.hits : g_lanes.st.lateShown);
            if (show->result == 0) {
                memcpy(tileY, show->plane[0], show->planeBytes[0]);
                memcpy(tileU, show->plane[1], show->planeBytes[1]);
                memcpy(tileV, show->plane[2], show->planeBytes[2]);
                g_lanes.lastShown = show->frame;
                g_lanes.sticky = false;   // a lane delivered
                fresh = true;
                shownFrame = show->frame;
            } else if (show == hit) {
                ret = show->result;
                NpThpLogError(frame, ret);
            }
        } else if (hit) {
            if (!delay || newDue) ++g_lanes.st.notReady;   // the lane is still at it: the picture on screen stays
        } else if (anyLane && !first && !rejected) {
            if (!delay || newDue) ++g_lanes.st.noSlot;     // never queued (all lanes were busy): the picture on screen stays
        } else {
            ++g_lanes.st.misses;     // no lane could be started, or none can take this frame, or nothing was shown yet: decode here
            ret = NpThpDecodeNow(file, tileY, tileU, tileV, end);
            if (ret) NpThpLogError(frame, ret);
            else {
                fresh = true;
                shownFrame = frame;
                if (frame > g_lanes.lastShown) g_lanes.lastShown = frame;
            }
        }
        if (delay && !show && ret == 0) {
            // the delay line: a frame that failed to decode (result != 0) is reported when its picture is due, as the frame asked for is with delay 0
            for (int i = 0; i < NSlots(); ++i) {
                Slot& sl = g_lanes.slot[i];
                if (SlotState(sl) == kDone && sl.gen == g_lanes.gen && sl.result > 0 && sl.frame <= due && sl.frame > g_lanes.lastShown && sl.w == width && sl.h == height) {
                    SlotWait(sl);
                    ret = sl.result;
                    NpThpLogError(sl.frame, ret);
                    break;
                }
            }
        }
        // MSC_THP_RECOVER: the picture does not move although a frame is due. After `recover` such calls in a row the frame asked for is
        // decoded here (software), however many lanes are stuck, lost, never given the frame or waiting for a codec that does not answer.
        g_lanes.starved = np_thp_pace::StarveNext(g_lanes.starved, newDue, fresh);
        if (g_lanes.recover > 0 && g_lanes.starved >= (g_lanes.sticky && g_lanes.recover > 2 ? 2 : g_lanes.recover) && ret == 0 && !fresh) {
            g_lanes.sticky = true;
            const int r = NpThpDecodeNow(file, tileY, tileU, tileV, end);
            ++g_lanes.st.recovered;
            static int logged = 0;
            if (logged++ < 6) fprintf(stderr, "[thp] %s: no new picture for %d calls (lanes busy or stuck): frame %d decoded on the game thread%s\n", g_movieName, g_lanes.starved, frame, r ? " (and it failed)" : "");
            g_lanes.starved = 0;
            if (r == 0) {
                fresh = true;
                shownFrame = frame;
                if (frame > g_lanes.lastShown) g_lanes.lastShown = frame;
            }
        }
        if (fresh) PaceFresh(shownFrame, due);
        else if (newDue && ret == 0) PaceRepeat();
        // everything finished that is not newer than this frame has been shown or is older than what is shown
        const int64_t upTo = delay ? (due > g_lanes.lastShown ? due : g_lanes.lastShown) : frame;
        for (int i = 0; i < kMaxSlots; ++i) {
            Slot& sl = g_lanes.slot[i];
            if (SlotState(sl) == kDone && sl.gen == g_lanes.gen && sl.frame <= upTo && (sl.w == width && sl.h == height)) {
                SlotWait(sl);
                SlotRelease(sl);
            }
        }
        ReleaseUnwanted(frame);
        if (dly) PoolNote((int64_t)frame + 1);
        return ret;
    }
    if (g_lanes.enabled == 1) {
        Slot* hit = nullptr;
        for (int i = 0; i < g_lanes.depth; ++i) {
            Slot& sl = g_lanes.slot[i];
            if (SlotState(sl) == kIdle) continue;
            if (SlotMatches(sl, file, frame, width, height)) hit = &sl;
            else if (sl.frame < frame) {   // an earlier frame that nobody asked for: of no use any more
                SlotWait(sl);
                SlotRelease(sl);
                ++g_lanes.st.stale;
            }
        }
        if (hit) {
            const bool ready = SlotState(*hit) == kDone;
            SlotWait(*hit);
            ++(ready ? g_lanes.st.hits : g_lanes.st.waits);
            const int r = hit->result;
            if (r == 0) {   // the planes, as the decoder would have written them
                memcpy(tileY, hit->plane[0], hit->planeBytes[0]);
                memcpy(tileU, hit->plane[1], hit->planeBytes[1]);
                memcpy(tileV, hit->plane[2], hit->planeBytes[2]);
                PaceFresh(frame, frame);
            } else {
                NpThpLogError(frame, r);
            }
            SlotRelease(*hit);
            return r;
        }
    }
    ++g_lanes.st.misses;
    const int r = NpThpDecodeNow(file, tileY, tileU, tileV, end);
    if (r) NpThpLogError(frame, r);
    else PaceFresh(frame, frame);
    return r;
}

// A skipped packet can already be referenced by a decode-ahead worker. Without nowait the worker is drained before THPSimple
// invalidates/refills its compressed read buffer (the worker reads the buffer in place no longer, but the wait keeps the old
// behaviour and the pictures deterministic). With nowait the result is simply dropped when its lane is done.
void NpThpDiscard(const void* video, int frame) {
    if (g_lanes.enabled != 1) return;
    const int nslots = NSlots();
    for (int i = 0; i < nslots; ++i) {
        Slot& sl = g_lanes.slot[i];
        if (SlotState(sl) != kIdle && sl.src == video && sl.frame == frame && sl.gen == g_lanes.gen) {
            if (g_lanes.nowait) {
                sl.retired = true;
            } else {
                SlotWait(sl);
                SlotRelease(sl);
                ++g_lanes.st.stale;
            }
        }
    }
    if (g_lanes.nowait) ReleaseUnwanted(frame);
}

// One line per movie (the console log): what the lanes and the codec did since the last line.
// Always on, one line per movie: the pace of the pictures (Galaxy's movie33 statistics) and what the failure paths did. `repeated` is the
// number of calls that brought no new picture although one was due, `late` the gaps between new pictures over 1.5 call periods,
// `doubled` the gaps under half a period, `rate` the decode calls a second (30 = every 2nd retrace of a 60 Hz display).
static void NpThpPaceSummary() {
    const Pace& a = g_pace;
    const Pace& b = g_paceRep;
    if (a.calls == b.calls) return;
    const unsigned long long periods = a.periodN - b.periodN;
    const double callMs = periods ? (a.periodSumMs - b.periodSumMs) / (double)periods : 0.0;
    const Lanes::Stats& l = g_lanes.st;
    static Lanes::Stats lr;   // what the previous pace line already counted (the lanes' own `reported` follows the other summary line)
    fprintf(stderr,
            "[thp-pace] %s: delay %d, calls %llu (every %.2f ms, %.2f a second), new pictures %llu (on time %llu, passed over %llu, early %llu), repeated %llu, gaps late %llu of %llu (over 1.5 calls), doubled %llu (under half a call), "
            "longest gap %.1f ms; recovered on the game thread %llu, first picture timeouts %llu, lanes given up %llu, jobs dropped as stale %llu, codec lock timeouts %llu\n",
            g_movieName, g_lanes.delay, a.calls - b.calls, callMs, callMs > 0 ? 1000.0 / callMs : 0.0, a.fresh - b.fresh, a.onTime - b.onTime, a.passed - b.passed, a.early - b.early,
            a.repeated - b.repeated, a.late - b.late, a.gaps - b.gaps, a.doubled - b.doubled, a.maxGapMs, l.recovered - lr.recovered, l.firstTimeouts - lr.firstTimeouts, l.lanesLost - lr.lanesLost,
            l.jobsDropped - lr.jobsDropped, (unsigned long long)g_hw.lockTo - g_hw.reported.lockTimeouts);
    g_paceRep = g_pace;
    g_hw.reported.lockTimeouts = g_hw.lockTo;
    lr.recovered = l.recovered;
    lr.firstTimeouts = l.firstTimeouts;
    lr.lanesLost = l.lanesLost;
    lr.jobsDropped = l.jobsDropped;
}

static void NpThpMovieSummary() {
    NpThpPaceSummary();
    const Lanes::Stats& a = g_lanes.st;
    const Lanes::Stats& b = g_lanes.reported;
    if (a.prefetched == b.prefetched) return;
    fprintf(stderr,
            "[thp] %s: frames queued %llu, shown ready %llu, shown late %llu, waited for %llu, picture kept (lane busy %llu, not queued %llu), decoded on the game thread %llu, dropped %llu, errors %llu\n",
            g_movieName, a.prefetched - b.prefetched, a.hits - b.hits, a.lateShown - b.lateShown, a.waits - b.waits, a.notReady - b.notReady, a.noSlot - b.noSlot,
            a.misses - b.misses, a.dropped - b.dropped, a.errors - b.errors);
    g_lanes.reported = a;
    if (g_hw.mode > 0 && g_hw.st.frames != g_hw.reported.frames) {
        const Hw::Stats& h = g_hw.st;
        const Hw::Stats& p = g_hw.reported;
        const double n = (double)(h.frames - p.frames);
        fprintf(stderr,
                "[thp-hw] %s: %llu frames by the codec (per frame: bridge %.2f ms, decode %.2f ms, tiling %.2f ms), refused by the codec %llu, not handed over %llu, software fallbacks %llu%s\n",
                g_movieName, h.frames - p.frames, (h.usBridge - p.usBridge) / 1000.0 / n, (h.usDecode - p.usDecode) / 1000.0 / n, (h.usTile - p.usTile) / 1000.0 / n,
                h.codecRefused - p.codecRefused, h.bridgeRefused - p.bridgeRefused, h.fellBack - p.fellBack, g_hw.dead ? " (switched off)" : "");
        g_hw.reported = h;
    }
}

// A movie is opened, closed, restarted or stopped. Without nowait no lane may still be in flight afterwards (the old contract); with
// nowait the results of the old movie are retired and dropped as their lanes finish, and nobody waits.
void NpThpReset(void) {
    if (g_lanes.enabled != 1) {
        NpThpPaceSummary();   // (no lanes: the calls of the game thread's own decodes are counted too)
        PaceReset();
        return;
    }
    NpThpMovieSummary();
    PaceReset();
    g_lanes.base = -1;
    g_lanes.lastAsked = -1;
    g_lanes.starved = 0;
    g_lanes.sticky = false;
    if (g_lanes.nowait) {
        ++g_lanes.gen;
        g_lanes.lastShown = -1;
        for (int i = 0; i < kMaxSlots; ++i) {
            Slot& sl = g_lanes.slot[i];
            if (SlotState(sl) == kDone) {
                SlotWait(sl);
                SlotRelease(sl);
            }
        }
        HwRelease();
        return;
    }
    for (int i = 0; i < kMaxSlots; ++i) {
        Slot& sl = g_lanes.slot[i];
        if (SlotState(sl) == kIdle) continue;
        SlotWait(sl);
        SlotRelease(sl);
    }
    g_lanes.lastShown = -1;
    HwRelease();
}

void NpThpLogStats(void) {
    if (g_lanes.enabled != 1) return;
    fprintf(stderr, "[thp] decode-ahead lanes (depth %d, %s): prefetched %llu, taken ready %llu, taken after a wait %llu, shown late %llu, picture kept %llu+%llu, decoded on the game thread %llu, stale released %llu, not queued (all busy) %llu\n",
            g_lanes.depth, g_lanes.nowait ? "never waiting" : "waiting", g_lanes.st.prefetched, g_lanes.st.hits, g_lanes.st.waits, g_lanes.st.lateShown, g_lanes.st.notReady, g_lanes.st.noSlot,
            g_lanes.st.misses, g_lanes.st.stale, g_lanes.st.dropped);
    fprintf(stderr, "[thp] recovery (MSC_THP_RECOVER=%d) and delay line (MSC_THP_DELAY=%d): frames decoded on the game thread after a frozen picture %llu, first-picture timeouts %llu, lanes given up %llu, queued jobs dropped as stale %llu, software decodes over the stuck limit (counted only) %llu\n",
            g_lanes.recover, g_lanes.delay, g_lanes.st.recovered, g_lanes.st.firstTimeouts, g_lanes.st.lanesLost, g_lanes.st.jobsDropped, g_lanes.st.swStuck);
    if (g_hw.mode > 0)
        fprintf(stderr, "[thp-hw] %s: %llu frames decoded by the codec, %llu opens, %llu refused by the codec, %llu not handed over, %llu software fallbacks, %llu codec calls stuck, %llu lock timeouts%s\n", g_hw.codec->name,
                g_hw.st.frames, g_hw.st.opened, g_hw.st.codecRefused, g_hw.st.bridgeRefused, g_hw.st.fellBack, g_hw.st.stuck, (unsigned long long)g_hw.lockTo, g_hw.dead ? ", switched off" : "");
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
