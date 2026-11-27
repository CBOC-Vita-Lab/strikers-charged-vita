// 1.3 (MSC_AUDIO_NEON): the game's reverb (AXFXReverbHiExpCallback, ref/RVL_SDK/axfx) in NEON, four samples at a time.
//
// Why this one: the AX voice and bus loops already have NEON kernels (src/ax/ax_mix_fast.h, MSC_AX_FAST=1). The reverb was the mixer's one large floating-point
// loop left in plain VFP code: three channels x 96 samples x about 13 delay-line stages every 3 ms, and it runs in the AI callback (dsp_hle.cpp), under the interrupt
// lock, not in the mix outside it. So its time also counts against a game thread that wants that lock.
// patches/axfx/AXFXReverbHiExp.c.sed calls this after the reference's own "active" test; a result of 0 means "not done here", and the reference's loop runs.
//
// Why four samples at a time is the same computation. Every stage is a delay line: read at pos, write at pos, pos + 1. Inside a run with no wrap of any position, the
// positions of a line are all different, so no sample of a run reads what the same run wrote, and the stages can take four samples at once; a run ends at the next wrap, and
// the 0-3 samples that do not fill a block go through OneSample, the reference's arithmetic. The damping filter is the one true recursion: it stays one sample
// at a time, in VFP. Operation order and operands are the reference's: VMLA on the Cortex-A9 is not fused, on VFP as on NEON, and VCVT truncates and saturates
// like the reference's (s32) cast.
//
// The one difference: NEON flushes denormal inputs and results to zero, VFP (FPSCR.FZ clear) keeps them. So a reverb tail dying away can differ in delay-line values near
// 2^-126. The difference cannot reach the output: it stays that small (coefficients are at most 1, the feedback sums are bounded), is rounded away by any sum with a value
// of ordinary size, and nothing that small truncates to a nonzero s32. Outputs are the same integers. tests/axfx_reverb_neon/ runs both paths as ARM code under an emulator
// and checks this, the denormal stretch included.
//
// Audio only: this reads and writes the reverb's own state and its bus buffers. Nothing here reaches the game's simulation; the voices' parameter blocks are not touched.
//
// Not done here: fx->busIn / busOut (the game's reverb has neither: AXFXReverbHi.c sets both NULL) and the surround mixes (AXFXReverbHiExpDpl2: the game runs in stereo).
#include <revolution/axfx.h>

// Set by the AX mixer from MSC_AUDIO_NEON and MSC_AX_FAST (ax_mix.cpp ReadAxMode).
int g_npAxfxReverbNeon = 1;

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>

// How many samples until pos wraps, at most `run`. A position at or past the end (not reachable through the game's settings) gets a single reference step.
static inline u32 Room(u32 pos, u32 length, u32 run) {
    if (pos >= length) return 1;
    const u32 left = length - pos;
    return left < run ? left : run;
}

static inline u32 Advance(u32 pos, u32 length, u32 n) {
    pos += n;
    return pos >= length ? 0 : pos;
}

// The reference's body for one sample (without the bus paths), positions included.
static void OneSample(AXFX_REVERBHI_EXP* fx, s32* input[3], u32 samp, f32 lpfCoef1, f32 lpfCoef2, f32 allpassCoef, f32 fusedGain, f32 crosstalkGain) {
    u32 ch, i;
    f32 fusedOut[3];
    for (ch = 0; ch < 3; ch++) {
        const f32 data = (f32)input[ch][samp];
        const f32* earlyLine = fx->earlyLine[ch];
        const f32 earlyOut = earlyLine[fx->earlyPos[0]] * fx->earlyCoef[0] + earlyLine[fx->earlyPos[1]] * fx->earlyCoef[1] + earlyLine[fx->earlyPos[2]] * fx->earlyCoef[2];
        f32 preDelayOut, filterOut = 0.0f, outTmp, allpassIn, lpfOut, out;
        f32* allpass;
        if (fx->preDelayLength != 0) {
            f32* preDelayLine = fx->preDelayLine[ch];
            preDelayOut = preDelayLine[fx->preDelayPos];
            preDelayLine[fx->preDelayPos] = data;
        } else {
            preDelayOut = data;
        }
        for (i = 0; i < 3; i++) {
            f32* combLine = fx->combLine[ch][i];
            const f32 combOutOne = combLine[fx->combPos[i]];
            combLine[fx->combPos[i]] = preDelayOut + (combOutOne * fx->combCoef[i]);
            filterOut += combOutOne;
        }
        for (i = 0; i < 2; i++) {
            allpass = fx->allpassLine[ch][i];
            outTmp = allpass[fx->allpassPos[i]];
            allpassIn = filterOut + outTmp * allpassCoef;
            allpass[fx->allpassPos[i]] = allpassIn;
            filterOut = outTmp - allpassIn * allpassCoef;
        }
        lpfOut = lpfCoef1 * filterOut + lpfCoef2 * fx->lastLpfOut[ch];
        fx->lastLpfOut[ch] = lpfOut;
        allpass = fx->lastAllpassLine[ch];
        outTmp = allpass[fx->lastAllpassPos[ch]];
        allpassIn = lpfOut + outTmp * allpassCoef;
        allpass[fx->lastAllpassPos[ch]] = allpassIn;
        out = outTmp - allpassIn * allpassCoef;
        if (++fx->lastAllpassPos[ch] >= fx->lastAllpassLength[ch]) fx->lastAllpassPos[ch] = 0;
        out *= fusedGain;
        out += earlyOut;
        fusedOut[ch] = out;
    }
    {
        const f32 crosstalkL = fusedOut[1] + fusedOut[2];
        const f32 crosstalkR = fusedOut[0] + fusedOut[2];
        const f32 crosstalkS = fusedOut[0] + fusedOut[1];
        input[0][samp] = (s32)((fusedOut[0] + crosstalkL * crosstalkGain) * fx->outGain);
        input[1][samp] = (s32)((fusedOut[1] + crosstalkR * crosstalkGain) * fx->outGain);
        input[2][samp] = (s32)((fusedOut[2] + crosstalkS * crosstalkGain) * fx->outGain);
    }
    for (i = 0; i < 3; i++) {
        if (++fx->earlyPos[i] >= fx->earlyLength) fx->earlyPos[i] = 0;
    }
    if (fx->preDelayLength != 0) {
        if (++fx->preDelayPos >= fx->preDelayLength) fx->preDelayPos = 0;
    }
    for (i = 0; i < 3; i++) {
        if (++fx->combPos[i] >= fx->combLength[i]) fx->combPos[i] = 0;
    }
    for (i = 0; i < 2; i++) {
        if (++fx->allpassPos[i] >= fx->allpassLength[i]) fx->allpassPos[i] = 0;
    }
}

// One delay stage of the all-pass shape, four samples.
static inline float32x4_t Allpass4(f32* line, float32x4_t in, float32x4_t coef) {
    const float32x4_t tmp = vld1q_f32(line);
    const float32x4_t written = vmlaq_f32(in, tmp, coef);
    vst1q_f32(line, written);
    return vmlsq_f32(tmp, written, coef);
}

int NpAxfxReverbNeon(AXFX_BUFFERUPDATE* update, AXFX_REVERBHI_EXP* fx) {
    if (!g_npAxfxReverbNeon || fx->busIn != NULL || fx->busOut != NULL) return 0;
    s32* input[3] = {update->left, update->right, update->surround};
    const f32 lpfCoef1 = 1.0f - fx->lpfCoef;
    const f32 lpfCoef2 = fx->lpfCoef;
    const f32 allpassCoef = fx->allpassCoef;
    const f32 fusedGain = fx->fusedGain * 0.6f;
    const f32 crosstalkGain = fx->crosstalk * 0.5f;
    const float32x4_t vLpf1 = vdupq_n_f32(lpfCoef1);
    const float32x4_t vAllpass = vdupq_n_f32(allpassCoef);
    const float32x4_t vFused = vdupq_n_f32(fusedGain);
    const float32x4_t vCross = vdupq_n_f32(crosstalkGain);
    const float32x4_t vOut = vdupq_n_f32(fx->outGain);
    const float32x4_t vComb[3] = {vdupq_n_f32(fx->combCoef[0]), vdupq_n_f32(fx->combCoef[1]), vdupq_n_f32(fx->combCoef[2])};
    const float32x4_t vEarly[3] = {vdupq_n_f32(fx->earlyCoef[0]), vdupq_n_f32(fx->earlyCoef[1]), vdupq_n_f32(fx->earlyCoef[2])};
    const u32 preDelayLength = fx->preDelayLength;
    u32 samp = 0, ch, i;
    while (samp < 96) {
        u32 run = 96 - samp;
        for (i = 0; i < 3; i++) run = Room(fx->earlyPos[i], fx->earlyLength, run);
        if (preDelayLength != 0) run = Room(fx->preDelayPos, preDelayLength, run);
        for (i = 0; i < 3; i++) run = Room(fx->combPos[i], fx->combLength[i], run);
        for (i = 0; i < 2; i++) run = Room(fx->allpassPos[i], fx->allpassLength[i], run);
        for (ch = 0; ch < 3; ch++) run = Room(fx->lastAllpassPos[ch], fx->lastAllpassLength[ch], run);
        if (run < 4) {
            OneSample(fx, input, samp, lpfCoef1, lpfCoef2, allpassCoef, fusedGain, crosstalkGain);
            ++samp;
            continue;
        }
        const u32 blocks = run >> 2;
        for (u32 block = 0; block < blocks; ++block) {
            const u32 at = block << 2;
            float32x4_t fused[3];
            for (ch = 0; ch < 3; ch++) {
                const float32x4_t data = vcvtq_f32_s32(vld1q_s32((const int32_t*)(input[ch] + samp + at)));
                const f32* earlyLine = fx->earlyLine[ch];
                const float32x4_t earlyOut = vmlaq_f32(vmlaq_f32(vmulq_f32(vld1q_f32(earlyLine + fx->earlyPos[0] + at), vEarly[0]), vld1q_f32(earlyLine + fx->earlyPos[1] + at), vEarly[1]),
                                                       vld1q_f32(earlyLine + fx->earlyPos[2] + at), vEarly[2]);
                float32x4_t pre = data;
                if (preDelayLength != 0) {
                    f32* preDelayLine = fx->preDelayLine[ch] + fx->preDelayPos + at;
                    pre = vld1q_f32(preDelayLine);
                    vst1q_f32(preDelayLine, data);
                }
                float32x4_t filter = vdupq_n_f32(0.0f);
                for (i = 0; i < 3; i++) {
                    f32* combLine = fx->combLine[ch][i] + fx->combPos[i] + at;
                    const float32x4_t combOut = vld1q_f32(combLine);
                    vst1q_f32(combLine, vmlaq_f32(pre, combOut, vComb[i]));
                    filter = vaddq_f32(filter, combOut);
                }
                filter = Allpass4(fx->allpassLine[ch][0] + fx->allpassPos[0] + at, filter, vAllpass);
                filter = Allpass4(fx->allpassLine[ch][1] + fx->allpassPos[1] + at, filter, vAllpass);
                // The damping filter feeds itself sample by sample. In lanes of the vector registers, in VFP.
                const float32x4_t scaled = vmulq_f32(filter, vLpf1);
                f32 last = fx->lastLpfOut[ch];
                f32 lpf[4];
                vst1q_f32(lpf, scaled);
                last = lpf[0] + lpfCoef2 * last;
                lpf[0] = last;
                last = lpf[1] + lpfCoef2 * last;
                lpf[1] = last;
                last = lpf[2] + lpfCoef2 * last;
                lpf[2] = last;
                last = lpf[3] + lpfCoef2 * last;
                lpf[3] = last;
                fx->lastLpfOut[ch] = last;
                const float32x4_t out = Allpass4(fx->lastAllpassLine[ch] + fx->lastAllpassPos[ch] + at, vld1q_f32(lpf), vAllpass);
                fused[ch] = vaddq_f32(vmulq_f32(out, vFused), earlyOut);
            }
            const float32x4_t crosstalkL = vaddq_f32(fused[1], fused[2]);
            const float32x4_t crosstalkR = vaddq_f32(fused[0], fused[2]);
            const float32x4_t crosstalkS = vaddq_f32(fused[0], fused[1]);
            vst1q_s32((int32_t*)(input[0] + samp + at), vcvtq_s32_f32(vmulq_f32(vmlaq_f32(fused[0], crosstalkL, vCross), vOut)));
            vst1q_s32((int32_t*)(input[1] + samp + at), vcvtq_s32_f32(vmulq_f32(vmlaq_f32(fused[1], crosstalkR, vCross), vOut)));
            vst1q_s32((int32_t*)(input[2] + samp + at), vcvtq_s32_f32(vmulq_f32(vmlaq_f32(fused[2], crosstalkS, vCross), vOut)));
        }
        const u32 done = blocks << 2;
        for (i = 0; i < 3; i++) fx->earlyPos[i] = Advance(fx->earlyPos[i], fx->earlyLength, done);
        if (preDelayLength != 0) fx->preDelayPos = Advance(fx->preDelayPos, preDelayLength, done);
        for (i = 0; i < 3; i++) fx->combPos[i] = Advance(fx->combPos[i], fx->combLength[i], done);
        for (i = 0; i < 2; i++) fx->allpassPos[i] = Advance(fx->allpassPos[i], fx->allpassLength[i], done);
        for (ch = 0; ch < 3; ch++) fx->lastAllpassPos[ch] = Advance(fx->lastAllpassPos[ch], fx->lastAllpassLength[ch], done);
        samp += done;
    }
    return 1;
}
#else
int NpAxfxReverbNeon(AXFX_BUFFERUPDATE* update, AXFX_REVERBHI_EXP* fx) {
    (void)update;
    (void)fx;
    return 0;
}
#endif
