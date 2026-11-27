// The reverb's NEON form (src/axfx_reverb_neon.c) against the reference's loop, as ARM code. Built with the Vita compiler and run by run_test.py under an emulator: no C library,
// no system. Both paths are the production objects: the sed-patched AXFXReverbHiExp.c with g_npAxfxReverbNeon 0 and 1, same inputs, frame by frame.
//   Must hold:  every output integer equal; positions equal; a delay-line or filter word may differ only where both values are below 2^-96 (NEON flushes denormals);
//               no write outside the lines (a canary around every allocation).
#include <revolution/axfx.h>

extern int g_npAxfxReverbNeon;

typedef void* (*AllocFn)(size_t);
typedef void (*FreeFn)(void*);

#define kCanary 0x5AA5C33Cu
#define kPad 16   // words on each side

static u32 g_arena[3 * 1024 * 1024 / 4];
static u32 g_top;
static u32* g_blocks[64];
static u32 g_blockWords[64];
static int g_blockCount;

static void* Alloc(size_t bytes) {
    const u32 words = (u32)((bytes + 3) / 4);
    u32* block = g_arena + g_top;
    if (g_top + words + 2 * kPad > sizeof(g_arena) / 4 || g_blockCount >= 64) return 0;
    g_top += words + 2 * kPad;
    for (u32 i = 0; i < kPad; ++i) block[i] = block[kPad + words + i] = kCanary;
    g_blocks[g_blockCount] = block;
    g_blockWords[g_blockCount++] = words;
    return block + kPad;
}
static void Free(void* p) { (void)p; }
void* (*__AXFXAlloc)(size_t) = Alloc;
void (*__AXFXFree)(void*) = Free;

int OSDisableInterrupts(void) { return 0; }
int OSRestoreInterrupts(int v) { return v; }
void* memset(void* d, int c, size_t n) { unsigned char* p = d; while (n--) *p++ = (unsigned char)c; return d; }
void* memcpy(void* d, const void* s, size_t n) { unsigned char* p = d; const unsigned char* q = s; while (n--) *p++ = *q++; return d; }
// pow: only the comb gains in __InitParams use it; their values need to be sane, not exact. 10^x for x in [-3, 0): a few series terms.
double pow(double b, double e) {
    (void)b;
    double t = e * 2.302585092994046, r = 1.0, term = 1.0;
    int k;
    for (k = 1; k < 24; ++k) { term *= t / k; r += term; }
    return r;
}

volatile u32 g_seed = 20261104;
static u32 Rand(void) { u32 s = g_seed; s ^= s << 13; s ^= s >> 17; s ^= s << 5; g_seed = s; return s; }

// Results, read by run_test.py at known symbols.
volatile u32 g_outputMismatch, g_positionMismatch, g_stateNormalMismatch, g_stateDenormalDiffs, g_canaryBad, g_frames, g_cases, g_initFailed, g_neonDone, g_refDone;
volatile s32 g_firstBad[4];

static void Init(AXFX_REVERBHI_EXP* fx, u32 earlyMode, u32 fusedMode, f32 preDelay, f32 time, f32 color, f32 damp, f32 cross, f32 earlyGain, f32 outGain) {
    fx->preDelayTimeMax = 0.05f;
    fx->earlyMode = earlyMode;
    fx->preDelayTime = preDelay;
    fx->fusedMode = fusedMode;
    fx->fusedTime = time;
    fx->coloration = color;
    fx->damping = damp;
    fx->crosstalk = cross;
    fx->earlyGain = earlyGain;
    fx->fusedGain = 1.0f;
    fx->busIn = 0;
    fx->busOut = 0;
    fx->outGain = outGain;
    fx->sendGain = 0.0f;
    if (!AXFXReverbHiExpInit(fx)) ++g_initFailed;
}

static void FillInput(s32* a, u32 kind) {
    for (u32 i = 0; i < 96 * 3; ++i) {
        const u32 r = Rand();
        switch (kind & 7) {
        case 0: a[i] = (s32)(r % 65536u) - 32768; break;            // speech-sized bus values
        case 1: a[i] = (s32)r >> 8; break;                          // loud 24-bit buses
        case 2: a[i] = 0; break;                                    // silence: the tail dies away into the denormal range
        case 3: a[i] = 0; break;
        case 4: a[i] = 0; break;
        case 5: a[i] = (i & 1) ? 0x7fffffff : (s32)0x80000000; break; // the extremes, for the saturating conversion
        case 6: a[i] = (r & 0xff) == 0 ? (s32)r >> 16 : 0; break;   // sparse clicks
        default: a[i] = (s32)(r % 7u) - 3; break;                   // near silence: small floats, fractional outputs
        }
    }
}

static int Differs(const f32* a, const f32* b) {
    const u32 x = *(const u32*)a, y = *(const u32*)b;
    if (x == y) return 0;
    // Both below 2^-96: the flushed denormals, and what a sum of them leaves. Their sums can reach the bottom of the normal range, so the test is not "both denormal".
    if (((x & 0x7f800000u) >> 23) < 31 && ((y & 0x7f800000u) >> 23) < 31) { ++g_stateDenormalDiffs; return 0; }
    return 1;
}

static void CompareLine(const f32* a, const f32* b, u32 n) {
    for (u32 i = 0; i < n; ++i) if (Differs(a + i, b + i)) ++g_stateNormalMismatch;
}

static AXFX_REVERBHI_EXP g_a, g_b;
static s32 g_in[96 * 3], g_outA[96 * 3], g_outB[96 * 3];

// Instruction counts, one path at a time: run_test.py enters here with a pending count of frames.
volatile u32 g_countNeon;
void CountSetup(void) {
    g_top = 0;
    g_blockCount = 0;
    Init(&g_a, 5, 0, 0.02f, 5.0f, 0.5f, 0.5f, 0.3f, 0.0f, 1.0f);
    FillInput(g_in, 0);
}
void CountFrames(void);

static void CompareState(const AXFX_REVERBHI_EXP* a, const AXFX_REVERBHI_EXP* b) {
    u32 ch, i;
    for (i = 0; i < 3; ++i) if (a->earlyPos[i] != b->earlyPos[i] || a->combPos[i] != b->combPos[i] || a->lastAllpassPos[i] != b->lastAllpassPos[i]) ++g_positionMismatch;
    for (i = 0; i < 2; ++i) if (a->allpassPos[i] != b->allpassPos[i]) ++g_positionMismatch;
    if (a->preDelayPos != b->preDelayPos) ++g_positionMismatch;
    for (ch = 0; ch < 3; ++ch) {
        if (Differs(&a->lastLpfOut[ch], &b->lastLpfOut[ch])) ++g_stateNormalMismatch;
        CompareLine(a->earlyLine[ch], b->earlyLine[ch], a->earlyMaxLength);
        if (a->preDelayLine[ch]) CompareLine(a->preDelayLine[ch], b->preDelayLine[ch], a->preDelayMaxLength);
        for (i = 0; i < 3; ++i) CompareLine(a->combLine[ch][i], b->combLine[ch][i], a->combMaxLength[i]);
        for (i = 0; i < 2; ++i) CompareLine(a->allpassLine[ch][i], b->allpassLine[ch][i], a->allpassMaxLength[i]);
        CompareLine(a->lastAllpassLine[ch], b->lastAllpassLine[ch], a->lastAllpassMaxLength[ch]);
    }
}

static void Process(AXFX_REVERBHI_EXP* fx, s32* buffer, int neon) {
    AXFX_BUFFERUPDATE u = {buffer, buffer + 96, buffer + 192};
    g_npAxfxReverbNeon = neon;
    AXFXReverbHiExpCallback(&u, fx);
}

static void OneCase(u32 earlyMode, u32 fusedMode, f32 preDelay, f32 time, f32 color, f32 damp, f32 cross, f32 earlyGain, f32 outGain, u32 frames, u32 silenceFrom) {
    u32 f, i, ch;
    g_top = 0;
    g_blockCount = 0;
    Init(&g_a, earlyMode, fusedMode, preDelay, time, color, damp, cross, earlyGain, outGain);
    Init(&g_b, earlyMode, fusedMode, preDelay, time, color, damp, cross, earlyGain, outGain);
    // The reference never writes the early lines, so its taps read what is there: give them something when the case has an early gain.
    if (earlyGain != 0.0f) {
        for (ch = 0; ch < 3; ++ch) for (i = 0; i < g_a.earlyMaxLength; ++i) g_a.earlyLine[ch][i] = g_b.earlyLine[ch][i] = (f32)((s32)(Rand() % 2001u) - 1000);
    }
    for (f = 0; f < frames; ++f) {
        FillInput(g_in, f >= silenceFrom ? 2 : (f < 40 ? 0 : (f < 80 ? 1 : f / 37)));
        for (i = 0; i < 96 * 3; ++i) g_outA[i] = g_outB[i] = g_in[i];
        if (g_a.active != 0 || g_b.active != 0) ++g_initFailed;   // the callback would return at once and the comparison would prove nothing
        Process(&g_a, g_outA, 0);
        Process(&g_b, g_outB, 1);
        for (i = 0; i < 96 * 3; ++i) {
            if (g_outA[i] != g_outB[i]) {
                if (!g_outputMismatch) { g_firstBad[0] = (s32)f; g_firstBad[1] = (s32)i; g_firstBad[2] = g_outA[i]; g_firstBad[3] = g_outB[i]; }
                ++g_outputMismatch;
            }
        }
        ++g_frames;
        if (f >= silenceFrom && (f & 7) == 0) CompareState(&g_a, &g_b);   // the denormal stretch is short: look during it, not only at the end
    }
    CompareState(&g_a, &g_b);
    for (int b = 0; b < g_blockCount; ++b)
        for (u32 k = 0; k < kPad; ++k) if (g_blocks[b][k] != kCanary || g_blocks[b][kPad + g_blockWords[b] + k] != kCanary) ++g_canaryBad;
    ++g_cases;
}

void CountFrames(void) {
    for (u32 f = 0; f < 32; ++f) {
        for (u32 i = 0; i < 96 * 3; ++i) g_outA[i] = g_in[i];
        Process(&g_a, g_outA, (int)g_countNeon);
    }
}

// Runs every case. run_test.py enters here, and stops at g_stop.
void TestMain(void) {
    //      early fused preDelay   time  color damp  cross early out
    OneCase(5, 0, 0.02f,     5.0f, 0.5f, 0.5f, 0.3f, 0.0f, 1.0f, 900, ~0u);   // near the game's defaults (AXFXReverbHi.c: earlyMode 5, fusedMode 0, no early gain)
    OneCase(5, 0, 0.0f,      5.0f, 0.5f, 0.5f, 0.3f, 0.0f, 1.0f, 300, ~0u);   // no pre-delay line
    OneCase(0, 1, 0.00003f,  1.0f, 0.1f, 0.0f, 1.0f, 0.7f, 0.5f, 300, ~0u);   // a pre-delay of one sample: every run is shorter than a block
    OneCase(2, 2, 0.0001f,   0.5f, 0.9f, 1.0f, 0.0f, 1.0f, 1.0f, 300, ~0u);   // three samples of pre-delay
    OneCase(7, 3, 0.05f,    20.0f, 0.0f, 0.05f, 0.5f, 0.2f, 0.25f, 300, ~0u);
    OneCase(3, 5, 0.00625f,  0.1f, 1.0f, 0.3f, 0.8f, 0.5f, 1.0f, 2000, ~0u);
    OneCase(5, 0, 0.02f,     0.1f, 0.5f, 0.5f, 0.3f, 0.0f, 1.0f, 3000, 100);   // a tail dying away: must pass through the denormal range (g_stateDenormalDiffs counts it)
    OneCase(5, 4, 0.0f,      0.05f, 0.9f, 0.1f, 0.5f, 0.3f, 1.0f, 2500, 60);
}
