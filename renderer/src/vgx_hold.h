// MSC_PICTURE_HOLD: the FULL picture policy for live play (header-only so a host test and the trace model run the shipped code).
//
// Live play keeps ONE picture size (the screen's 960x544, or the size named in MSC_PICTURE_HOLD) with the edge smoothing on. A frame that runs late is made cheaper by giving up the
// least noticeable things first, in this fixed order, and the size is touched only after all of them, in small steps down a ladder whose targets were made before the match:
//   1  kFxSize     the effects pass's accumulation target one size smaller (vgx_fxcomp.h Div: soft layers only, the detailed textures keep their size). Skipped while no effects pass runs.
//   2  kShadowFar  shadow pictures of players far from the camera redrawn less often (only the shadow's SHAPE lags; its place follows the player every frame)
//   3  kShadowAll  every player's shadow picture redrawn less often
//   4  kCrowd      the crowd's impostor atlas refreshed half as often (staggered by slot in ImpostorManager: a part of the crowd every picture; the sprites still move with the stands)
//   5  kAa1        MSC_PICTURE_HOLD_MSAA=4 only: the held picture's samples 4 -> 2 (the target of the other sample count is made ahead and kept, like a size's)
//   6  kAa2        the samples 2 -> 1 (anti-aliasing off at the held size; with MSC_PICTURE_HOLD_MSAA=2 this is the only AA step, with 0 neither exists)
//   7  kSize1      the picture 0.90 per side (864x488 of 960x544): a target made ahead, taken from the kept ones, never allocated in play (always single-sampled: the samples are gone by then)
//   8  kSize2      0.81 per side (776x440)
//   9  kSize3      0.73 per side (704x400): the floor, about the pixel count of the game's own 640x448
// Nothing here is a switch of the picture on and off: steps 1-4 change how often or how coarsely something is redrawn, the size steps are the one thing a player may see (the present eases
// the sharpening over 20 pictures at a size step: MSC_SHARPEN_EASE of the polish work).
//
// Down: the GPU time's mean over downAt of the budget for `hotFrames` frames (a late frame counts double) takes the next step; how many steps one decision takes (1..maxJump) follows how far
// over the mean is, by the table of estimated savings, so a stadium that needs four of them does not take six seconds to get there. Then nothing for `gap` frames. A size step needs its
// target (readyMask) or the decision stops there and reports noRoom.
// Up (one step at a time, last taken first): only on proof. The frame time the step's saving would add back must keep the mean under upAt of the budget for `calmFrames` frames
// (`sizeCalmFrames` for a size step). A step given back that has to be taken again within `probeFrames` doubles the wait for that step (up to blockMax); the doubling is forgiven one
// notch per `forgiveFrames` without a move down. So a load between two states settles at the lower one and tries the higher one now and then, never twice in a row.
// Not in live play (menus, replays, cut-scenes, goal celebrations and Mega Strikes: all held at a steady 30 with the full look): Freeze() - nothing is measured, nothing moves, the state is kept; Thaw() at the cut back resumes it with the measurement
// started afresh, so a replay's timing is never inherited.
// Render only: nothing here reaches the simulation, a random number or an input record.
#pragma once

#include <algorithm>
#include "vgx_floors.h"

namespace vgx {
namespace hold {

#ifndef VGX_GLOWHALF_BREAK
#define VGX_GLOWHALF_BREAK 0   // host test mutants (renderer/tests/picture_hold.cpp --break=50 / 51): 1 a tiny gain keeps the glow step, 2 no gain at all keeps it
#endif
enum Step { kNone = 0, kFxSize = 1, kShadowFar = 2, kShadowAll = 3, kCrowd = 4, kAa1 = 5, kAa2 = 6, kSize1 = 7, kSize2 = 8, kSize3 = 9, kSteps = 10 };   // (10: the new order's ladder has five sizes; the old order stops at 9)
constexpr int kFirstSize = kSize1;
constexpr int kSizeSteps = 5;   // size levels the new order can have (the old order has kOldSizes)
constexpr int kOldSizes = 3, kOldLast = 9;
constexpr int kShedSteps = kAa2;   // the steps that are not a size (the two anti-aliasing steps last among them: samples go before any size)

// The size level of a step: 0 the held size, 1..3 the ladder's.
inline int SizeLevel(int step) { return step >= kFirstSize ? step - kFirstSize + 1 : 0; }

inline const char* StepName(int s, bool aaLast = false) {
    static const char* const n[kSteps + 2] = {"none", "effects target smaller", "far shadows", "all shadows", "crowd refresh", "samples 4 to 2", "samples to 1", "size 0.90", "size 0.81", "size 0.73", "-", "-"};
    static const char* const nn[kSteps + 2] = {"none", "effects target smaller", "far shadows", "all shadows", "crowd refresh", "size 0.95 2x", "size 0.90 2x", "size 0.83 2x", "samples to 1", "size 0.78 1x", "size 0.73 1x", "-"};
    return s >= 0 && s <= kSteps ? (aaLast ? nn[s] : n[s]) : "?";
}

struct Policy {
    // configuration
    float downAt = 0.92f;       // mean GPU time over this share of the budget is "hot"
    float lateAt = 1.00f;       // one frame over this counts double
    float upAt = 0.80f;         // a step is given back only if the mean plus its saving stays under this share
    float fit = 0.90f;          // a move down aims at this share of the budget (by the table of savings)
    int hotFrames = 48;         // a window: a single late frame (worth 2) can never take a step
    int gap = 180;              // frames between two moves (3 s at 60); a move up waits for its proof, which is longer
    int upGap = 180;            // frames between a move up and the next move (MSC_PICTURE_HOLD_PACE: 45, so a scene climbs back in a few seconds)
    int maxJump = 3;            // most steps one decision takes
    int calmFrames = 300;       // proof for a shed step given back (5 s)
    int sizeCalmFrames = 900;   // proof for a size step given back (15 s)
    int probeFrames = 1800;     // a shed step taken again within this many frames of its return failed (30 s: longer than gap + the calm proof + the hot window, or the test could never be true)
    int sizeProbeFrames = 2400; // ... a size step, within this many (gap + sizeCalmFrames + hot window is 1128)
    int blockMax = 7200;        // the longest a failed step is kept from returning (2 min)
    int forgiveFrames = 3600;   // a minute without a move down forgives one doubling
    int maxStep = kOldLast;
    float pixShare = 0.45f;     // share of GPU time that follows the pixels at the held size (the size steps' saving)
    float scale[kSizeSteps + 1] = {1.0f, 0.90f, 0.81f, 0.73f, 0.73f, 0.73f};   // per side, of the held size, by size level
    float save[kCrowd + 1] = {0.0f, 0.50f, 0.20f, 0.35f, 0.40f};   // ms a shed step is estimated to buy (index = Step); the two sample steps are modelled from the mean (Save)
    // MSC_PICTURE_HOLD_MSAA: cost of a picture with 1 / 2 / 4 samples against one, for the pixel part (an ESTIMATE, not measured on a console; MSC_VGX_MSAA_COST's 1.3 / 1.9)
    float aaCost[3] = {1.0f, 1.3f, 1.9f};
    // state
    // The order of the last steps. Old (aaLast false): the samples go first (steps 5 and 6), then the sizes 7 to 9, all at one sample. New (MSC_PICTURE_HOLD_AALAST, owner's decision 13 Nov): the picture SIZE goes first
    // in small steps, every one with the samples the picture has (2x), and the samples go last, at the bottom of the ladder: steps 5 to 4+nSizes are sizes, then aa1 (4 to 2, only without sizes at 2x) and aa2 (2 to 1).
    bool aaLast = false;
    int maxSizes = kSizeSteps;  // the most size levels the floor (vgx_floors.h) lets the ladder have: the backend sets it from the sizes; SetOrder never goes past it
    int nSizes = 3;             // size levels in the order (the ladder a match has: a prefix of the sizes whose targets are kept)
    int n2x = 3;                // of them the first n2x keep the samples; the one-sample step follows the n2x-th, the other levels have one sample (new order)
    int excuse = 0;             // frames left in which nothing is learned and no step is given back: the first seconds of a match and of a scene (walk-out camera, first-use uploads)
    int firstSize = kFirstSize; // step number of the first size step
    int aa1 = kAa1, aa2 = kAa2; // step numbers of the two sample steps
    int Lvl(int s) const {      // the size level in force at step s (the sample steps of the new order stay at the bottom size)
        if (!aaLast) return SizeLevel(s);
        if (s < firstSize) return 0;
        return s <= aa2 ? std::min(s - firstSize + 1, n2x) : std::min(s - firstSize, nSizes);   // (never a level the ladder does not have: floors)
    }
    bool IsSize(int s) const { return aaLast ? ((s >= firstSize && s < aa2) || (s > aa2 && s <= firstSize + nSizes)) : s >= kFirstSize; }
    bool IsAa(int s) const { return s == aa1 || s == aa2; }
    int maxCfg = kSteps;        // the configured last step (MSC_PICTURE_HOLD_MAXSTEP)
    // The last step that does something in the new order: the one-sample step when there are samples to give up, else the last size.
    int Last() const { return firstSize + nSizes; }   // (the one-sample step takes one place whether or not it is used: unused it is passed over at no cost)
    // The order of the last steps and how many sizes the match's ladder has (see aaLast above).
    void SetOrder(bool last, int sizes = 3, int keep2x = 3) {
        aaLast = last;
        nSizes = std::clamp(sizes, 0, std::min(static_cast<int>(kSizeSteps), maxSizes));
        n2x = std::clamp(keep2x, 0, nSizes);
        if (last) { firstSize = kAa1; aa2 = firstSize + n2x; aa1 = kSteps + 1; }   // (no 4-to-2 step: aa1 is a place nobody reaches) else { nSizes = kOldSizes; n2x = kOldSizes; firstSize = kFirstSize; aa1 = kAa1; aa2 = kAa2; }
        for (int i = kAa1; i <= kSteps; ++i) off[i] = false;
        if (!last) { off[kAa1] = off[kAa2] = true; maxStep = std::min(maxCfg, std::min(kOldLast, kShedSteps + std::min(kOldSizes, maxSizes))); }
        SetAa(aaTop, aaHave2);
        if (last) { maxStep = std::min(maxCfg, Last()); step = std::min(step, maxStep); }
    }    int step = 0;
    float ema = -1.0f;
    unsigned readyMask = 1;     // bit L: the render target of size level L can be entered without making one (bit 0, the held size, is always there)
    int aaTop = 0;              // the samples of the held picture when nothing is shed: 0 (none), 2 or 4. SetAa() keeps off[aa1], off[aa2] in step with it
    bool aaHave2 = true;        // with 4 samples: the 2-sample target exists (else the step goes 4 -> 1 at once)
    unsigned aaReady = 7;       // bit 0 one sample, bit 1 two, bit 2 four: a target of that sample count at the held size can be entered without making one
    bool off[kSteps + 2] = {false, false, false, false, false, true, true, false, false, false, false, false};  // a step that is not in the order (a step whose engagement is not hidden is opt-in: effects target, crowd refresh); passed over at no cost
    bool fxLive = true;         // an effects pass has run lately (without one step 1 buys nothing and is passed over at no cost)
    bool fxOk = true;           // the effects pass has a smaller target to go to (MSC_FX_COMPOSITE on)
    bool frozen = false;
    int sizeFail = 0;           // frames left in which a size step is not tried again (a target could not be made)
    int hot = 0, calm = 0, wait = 0, sinceUp = 100000, sinceDown = 0, fails[kSteps + 1] = {}, block[kSteps + 1] = {};
    int downs = 0, ups = 0, flaps = 0, jumps = 0;
    bool noRoom = false;        // the last decision wanted a size step and none was ready, or every step is in force
    int lastUp = 0;             // the step given back last (the one a quick return counts against)
    // What the policy measured of a step's real cost, as a share of the mean with the step in force (the mean rose by that share when the step was given back, i.e. fell by it when taken, so a
    // lighter load has a smaller cost): beats the model's guess when it is larger.
    // outMs: the mean just before the step was taken; inMs: the mean 90 frames into the step; learnAdd = outMs - inMs. 0: not measured. Kept per stadium (index below 32) across matches.
    float learnAdd[kSteps + 1] = {};
    float mem[32][kSteps + 1] = {};
    int stad = -1;
    float outMs = 0.0f;
    int settleStep = 0, settleLeft = 0;   // settleStep: the one step this move's cost belongs to (0: a span of steps, see spanLo/spanHi)
    int spanLo = 0, spanHi = 0;           // the steps a move down took (inclusive), 0 when none is being measured
    float spanOther = 0.0f;               // what the model says the span's steps that are not sample steps bought (ms): the rest of the measured drop is the samples'
    bool settleFlap = false;
    int settleFrames = 90;      // frames after a move down before the mean stands for the new state
    float learnMargin = 1.15f;  // a step is given back only if the measured cost, times this, still fits
    int learned = 0;            // learnings stored (a counter: the switch's action)
    int vetoes = 0;             // returns refused because the measured cost does not fit (a counter)

    // The timing of a held scene's policy (a cut-scene lasts 4 s: it cannot wait 30 pictures to shed or half a minute to climb): shed within about four pictures over the budget, climb on a short proof.
    void SceneTiming() {
        hotFrames = 4; gap = 3; upGap = 20; calmFrames = 45; sizeCalmFrames = 60; probeFrames = 240; sizeProbeFrames = 240; blockMax = 600; forgiveFrames = 900;
        downAt = 0.95f; upAt = 0.75f; maxJump = 3; settleFrames = 20;
    }
    // The anti-aliasing the policy offers: top = 0, 2 or 4; have2: the 2-sample target for the middle stop of 4 exists.
    void SetAa(int top, bool have2 = true) {
        aaTop = top >= 4 ? 4 : (top >= 2 ? 2 : 0);
        aaHave2 = have2;
        off[aa1] = aaLast ? true : !(aaTop == 4 && aaHave2);   // (the new order: 4 samples become 2 at the first size step, the target there has two)
        off[aa2] = aaTop == 0;
        if (aaLast) maxStep = std::min(maxCfg, Last());
    }
    // The samples of the held picture at `step` (a size step is always 1).
    int Samples(int step) const {
        if (aaTop < 2) return 1;
        int s = aaTop;
        if (aaLast && step > aa2) return 1;   // (the sizes below the sample step have one)
        if (aaLast && s > 2 && step >= firstSize && nSizes > 0) s = 2;   // (sizes carry two samples at most)
        if (!off[aa1] && step >= aa1) s = 2;
        if (!off[aa2] && step >= aa2) s = 1;
        return s;
    }
    bool AaReady(int samples) const { return (aaReady & (samples >= 4 ? 4u : (samples >= 2 ? 2u : 1u))) != 0; }
    float AaFactor(int samples) const {   // relative GPU time of the picture with that many samples
        const float c = samples >= 4 ? aaCost[2] : (samples >= 2 ? aaCost[1] : aaCost[0]);
        return (1.0f - pixShare) + pixShare * c;
    }
    bool AtFloor() const { return step >= maxStep; }
    int Level() const { return Lvl(step); }
    // pace13 MSC_GLOW_HALF_PLAY=1: step 1 (kFxSize, opt-in and off otherwise) is the glow chain at half rate in live play, the first step of the ladder, before any size; it is KEPT ONLY IF IT BOUGHT GPU TIME:
    // the move that took it (alone) is judged settleFrames later, and a drop under glowMinGain of the mean (or none at all) makes it an empty step for the rest of the match (empty steps ride along at no cost).
    bool glowStep = false, glowNoGain = false;
    float glowMinGain = 0.03f;
    bool Empty(int s) const { return off[s] || (s == kFxSize && (glowStep ? glowNoGain : (!fxOk || !fxLive))); }   // a step that buys nothing now

    // The estimated saving (ms) of taking step `s` from the state `from` (= s - 1) at the mean `m`, and what giving it back adds.
    float Save(int s, float m) const {
        if (s == aa1 || s == aa2) {   // the samples of the state left (s - 1) down to the samples at s
            if (Empty(s)) return 0.0f;
            const float a = AaFactor(Samples(s - 1)), b = AaFactor(Samples(s));
            return m > 0.0f ? m * (a - b) / a : 0.0f;
        }
        if (!IsSize(s)) return Empty(s) ? 0.0f : (s < kFirstSize ? save[s] : 0.0f);
        const int l = Lvl(s);
        const float a = scale[l - 1] * scale[l - 1], b = scale[l] * scale[l];
        const float c = (1.0f - pixShare) + pixShare * a;   // relative cost at the level left
        return m > 0.0f ? m * pixShare * (a - b) / c : 0.0f;
    }
    // What taking step `s` back adds to the mean `m` measured with it in force (m is at the state s, so the sizes' ratio runs the other way).
    float Add(int s, float m) const {
        if (s == aa1 || s == aa2) {
            if (Empty(s)) return 0.0f;
            const float a = AaFactor(Samples(s - 1)), b = AaFactor(Samples(s));
            return m > 0.0f ? m * (a - b) / b : 0.0f;
        }
        if (!IsSize(s)) return Empty(s) ? 0.0f : (s < kFirstSize ? save[s] : 0.0f);
        const int l = Lvl(s);
        const float a = scale[l - 1] * scale[l - 1], b = scale[l] * scale[l];
        const float c = (1.0f - pixShare) + pixShare * b;   // relative cost at the level now
        return m > 0.0f ? m * pixShare * (a - b) / c : 0.0f;
    }
    void SaveLearned() { if (stad >= 0) for (int s = 0; s <= kSteps; ++s) mem[stad][s] = learnAdd[s]; }
    // What giving step `s` back is expected to add to the mean `m`: the larger of the model's figure and the measured one (with its margin)
    float AddReal(int s, float m) const {
        float a = Add(s, m);
        if (s >= 1 && s <= kSteps && learnAdd[s] > 0.0f && m > 0.0f) a = std::max(a, learnAdd[s] * m * learnMargin);   // (learnAdd is a share of the mean: the cost of a step follows the load)
        return a;
    }
    // A size step was taken but the renderer could not make the target: back to the step before, no new try for a long time.
    void SizeFailed() {
        if (IsSize(step) || (aaLast && step >= firstSize)) step = firstSize - 1;
        sizeFail = blockMax;
        wait = gap;
        ema = -1.0f;
        hot = calm = 0;
        noRoom = true;
    }
    // Not live play: nothing is measured and nothing moves. Thaw(): live play again; the measurement starts afresh and the first decision waits half a gap.
    void Freeze() { frozen = true; hot = calm = 0; ema = -1.0f; settleStep = settleLeft = spanLo = spanHi = 0; }
    void Thaw() {
        if (!frozen) return;
        frozen = false;
        hot = calm = 0;
        ema = -1.0f;
        wait = std::max(wait, gap / 2);
    }
    // A new match starts at `to` (a stadium that needed steps last time, or a heavy one): the counters start again, the doubling of a failure stays forgotten.
    void Start(int to, int stadium = -1, int excuseFrames = 0) {
        excuse = excuseFrames;
        SaveLearned();
        stad = stadium >= 0 && stadium < 32 ? stadium : -1;
        for (int s = 0; s <= kSteps; ++s) learnAdd[s] = stad >= 0 ? mem[stad][s] : 0.0f;
        settleStep = settleLeft = spanLo = spanHi = 0;
        glowNoGain = false;
        step = std::clamp(to, 0, maxStep);
        hot = calm = 0;
        wait = gap;
        ema = -1.0f;
        sinceUp = 100000;
        frozen = false;
        sizeFail = 0;
        for (int s = 0; s <= kSteps; ++s) fails[s] = block[s] = 0;
        noRoom = false;
    }
    // One measured frame of live play. ms: its GPU time; budget: the frame budget; holdUp: something else is still working on this frame (the effects governor).
    // Returns the new step or -1.
    // gpuLimit (MSC_PICTURE_HOLD_GPUBUSY): the GPU's busy time is what limits the pictures (Pacing::GpuLimit). False: a frame over the budget is not counted hot (the pictures are late for another stage's sake: a smaller picture
    // buys nothing). Default true: every caller that does not know the stage behaves as before.
    int Frame(float ms, float budget, bool holdUp, bool gpuLimit = true) {
        if (frozen) return -1;
        ema = ema < 0.0f ? ms : 0.75f * ema + 0.25f * ms;
        if (spanHi > 0 && settleLeft > 0 && --settleLeft == 0) {
            if (excuse == 0 && step >= spanHi && outMs > ema) {
                const float v = outMs - ema;
                if (settleStep > 0) {
                    const float r = v / std::max(ema, 0.1f);
#if VGX_GLOWHALF_BREAK != 1
                    if (glowStep && settleStep == kFxSize && r < glowMinGain) glowNoGain = true;
#endif
                    learnAdd[settleStep] = settleFlap ? std::max(r, learnAdd[settleStep]) : r;
                    ++learned;
                } else {   // several steps in one move: the samples get what the model does not give the others (never more than all of it), the others are proven by their own return
                    int aaN = 0;
                    for (int t = spanLo; t <= spanHi; ++t) if ((t == aa1 || t == aa2) && !Empty(t)) ++aaN;
                    if (aaN > 0) {
                        const float share = std::max(0.0f, v - spanOther) / static_cast<float>(aaN) / std::max(ema, 0.1f);
                        for (int t = spanLo; t <= spanHi; ++t) if ((t == aa1 || t == aa2) && !Empty(t)) { learnAdd[t] = settleFlap ? std::max(share, learnAdd[t]) : share; ++learned; }
                    }
                }
                SaveLearned();
            }
#if VGX_GLOWHALF_BREAK != 2
            else if (glowStep && settleStep == kFxSize && excuse == 0 && step >= spanHi) glowNoGain = true;   // (the mean did not fall at all)
#endif
            settleStep = spanLo = spanHi = 0;
        }
        if (wait > 0) --wait;
        if (excuse > 0) --excuse;
        if (sinceUp < 1000000) ++sinceUp;
        ++sinceDown;
        for (int s = 1; s <= kSteps; ++s) if (block[s] > 0) --block[s];
        if (sizeFail > 0) --sizeFail;
        const bool over = ema > downAt * budget;
        const bool late = ms > lateAt * budget;
        if ((over || late) && gpuLimit) hot = std::min(hot + (late ? 2 : 1), hotFrames * 2);
        else hot = std::max(hot - 1, 0);
        noRoom = false;
        // down
        if (hot >= hotFrames && wait == 0 && !holdUp) {
            if (step >= maxStep) {
                noRoom = true;
            } else {
                // how many steps: enough, by the table, to bring the mean under `fit` of the budget; the first step is always taken. Empty steps cost nothing and ride along.
                int n = step;
                float m = ema;
                const float want = fit * budget;
                int taken = 0;
                while (n < maxStep && taken < maxJump) {
                    const int nx = n + 1;
                    if (IsSize(nx) && (!(readyMask & (1u << Lvl(nx))) || sizeFail > 0)) { noRoom = true; break; }
                    if ((nx == aa1 || nx == aa2) && !Empty(nx) && !AaReady(Samples(nx))) { noRoom = true; break; }   // (a sample count without its target: not entered)
                    const float sv = Save(nx, m);
                    n = nx;
                    if (!Empty(nx)) ++taken;
                    m -= sv;
                    if (m <= want && taken > 0) break;
                    if (glowStep && nx == kFxSize && !Empty(nx)) break;   // (MSC_GLOW_HALF_PLAY: the glow step is taken alone, so that what it bought can be judged)
                }
                if (n > step) {
                    // a step given back that is needed again soon: it failed; its next return waits longer
                    for (int t = step + 1; t <= n; ++t) {
                        if (t != lastUp || Empty(t)) continue;
                        if (sinceUp < (IsSize(t) ? sizeProbeFrames : probeFrames)) {
                            ++flaps;
                            if (fails[t] < 6) ++fails[t];
                            block[t] = std::min((IsSize(t) ? sizeCalmFrames : calmFrames) << std::min(fails[t], 5), blockMax);
                        }
                    }
                    if (n - step > 1) ++jumps;
                    // what this move cost to learn: one real step only, with the mean that made it hot
                    int only = 0, cnt = 0;
                    bool flapped = false;
                    for (int t = step + 1; t <= n; ++t) if (!Empty(t)) { only = t; ++cnt; if (excuse == 0 && t == lastUp && sinceUp < (IsSize(t) ? sizeProbeFrames : probeFrames)) flapped = true; }
                    settleStep = cnt == 1 ? only : 0;
                    spanLo = step + 1; spanHi = n;
                    spanOther = 0.0f;
                    for (int t = step + 1; t <= n; ++t) if (!Empty(t) && t != aa1 && t != aa2) spanOther += Save(t, ema);
                    settleLeft = settleFrames;
                    settleFlap = flapped;
                    outMs = ema;
                    step = n;
                    ++downs;
                    sinceDown = 0;
                    hot = calm = 0;
                    wait = gap;
                    ema = -1.0f;   // the mean at the old state says nothing about the new one (the size steps in particular)
                    return step;
                }
            }
        }
        // forgive a failure after a minute without a move down
        if (sinceDown >= forgiveFrames) {
            sinceDown = 0;
            for (int s = 1; s <= kSteps; ++s) { if (fails[s] > 0) --fails[s]; learnAdd[s] *= 0.85f; }
        }
        // up, on proof
        if (excuse > 0) calm = 0;
        if (step > 0 && wait == 0 && !holdUp && !late && excuse == 0) {
            if (Empty(step)) {   // a step that buys nothing (off, or no effects pass) goes at no cost, with any empty one under it
                while (step > 0 && Empty(step)) --step;
                ++ups;
                return step;
            }
            const bool targetReady = (readyMask & (1u << Lvl(step - 1))) != 0 && AaReady(Samples(step - 1));   // (the size and the samples it returns to must exist: no target is made in play)
            const float proj = ema + AddReal(step, ema);
            if (targetReady && block[step] == 0 && proj >= upAt * budget && ema + Add(step, ema) < upAt * budget) ++vetoes;   // (the model would have returned it; the measured cost did not)
            if (proj < upAt * budget && block[step] == 0 && targetReady) {
                const int need = Lvl(step) > 0 ? sizeCalmFrames : calmFrames;
                if (++calm >= need) {
                    calm = 0;
                    lastUp = step;
                    settleStep = spanLo = spanHi = 0;
                    --step;
                    while (step > 0 && Empty(step)) --step;
                    ++ups;
                    sinceUp = 0;
                    wait = upGap;
                    ema = -1.0f;
                    return step;
                }
            } else {
                calm = 0;
            }
        } else if (step == 0 || late) {
            calm = 0;
        }
        return -1;
    }
};

// Warm-up bookkeeping (pure, shared with the backend's HoldTick; the host test checks it).
// A target is made ahead only when the video memory left afterwards stays at or above the floor under which TrimPool lets pinned targets go (MegaWarmFloorKib, 16384 by default):
// a lower gate made targets that were dropped within a second and then marked unusable for a whole match.
constexpr int kTrimFloorKib = 16384;
inline bool WarmRoomOk(int freeKib, long kib, long floorKib = kTrimFloorKib) { return freeKib < 0 || freeKib >= floorKib + kib; }   // (-1: the free memory is unknown: no objection)
// State of one target: 0 to make, 1 kept, 2 skipped (not made, or let go), 3 kept and ready. At a stadium change every skipped one is tried again and every one that has gone since is re-armed
// (before, a target already gone stayed marked unused until the stadium after next).
inline void RearmAtStadium(int* state, const bool* kept, int n) {
    for (int i = 0; i < n; ++i) if (state[i] == 2 || ((state[i] == 1 || state[i] == 3) && !kept[i])) state[i] = 0;
}
// The held size's own one-sample target must exist whenever the picture is at a smaller size: a size step is given back only to a target that is there (readyMask bit 0).
inline bool HeldTargetMissing(bool pictureAtHeldSize, bool keptHeldOneSample) { return !pictureAtHeldSize && !keptHeldOneSample; }
// heldstep23: the size levels a held scene (replay, cut-scene) may enter. Where the stadium's held scenes are ONE sample the scene is drawn into the level's one-sample target, so a level counts only
// if THAT target is kept (oneKept, bit L); the live ladder's own (2x) targets (live) say nothing about it, and taking such a level made a target in the cut (r17 goal 2: 912x520 1x, +11 targets, 135 ms).
// Level 0 (the held size, made ahead at one sample) stays the fall-back.
inline unsigned HeldSceneMask(bool heldOneSample, unsigned live, unsigned oneKept) { return heldOneSample ? (oneKept & ~1u) | (live & 1u) : live; }
// Walks a wanted step down until its target is ready (a sample step: the one-sample held target), else 0.
inline int HeldReadyStep(const Policy& P, int st, unsigned ready, unsigned aaReady) {
    while (st >= P.firstSize) {
        const int lv = P.Lvl(st);
        if (P.IsAa(st) ? (aaReady & 1u) != 0 : ((ready >> lv) & 1u) != 0) break;
        --st;
    }
    return st >= P.firstSize ? st : 0;
}

// MSC_PICTURE_HOLD_PACE: the budget the policy reads GPU time against follows what the pacing can use. 16.7 ms is the budget at 60; when pictures leave less often than that
// because another stage (the game thread, the 30 fps hold) sets the pace, GPU time up to that period costs no picture, so the budget is the measured period, unless the GPU
// itself is the stage that delays the pictures (its busy time is within 5 % of the period; back to "not the limit" under 90 %: a hysteresis, or the flip would flap).
struct Pacing {
    float base = 1001.0f / 60.0f, per = 0.0f, gpu = 0.0f;
    float hi = 0.95f, lo = 0.90f;     // GPU busy / period: at or over hi the GPU is the limit; under lo it is not
    float margin = 1.0f, heldAt = 29.0f;   // the share of a held period the budget is (MSC_PICTURE_HOLD_BUDGET_MARGIN, Vita plan 0.80), and the period from which it is a held rate
    int minN = 60, n = 0;             // pictures measured before the period is believed
    bool limited = false;             // the GPU is the stage that delays pictures
    // MSC_PICTURE_HOLD_GPUBUSY: is the GPU's BUSY time (the corrected figure of vgx_gpubusy.h: no display-buffer wait, no backlog wait) the limit of the pictures? Two slow figures, the same pictures as Add:
    // slow: the GPU busy mean over about 90 pictures (3 s at 30); lateShare: the share of the last 128 pictures that were held beyond the cadence (1.25 periods). The GPU is the limit when its mean takes
    // most of the period (>= 85 %), or when pictures are late a lot (>= 8 %) and it takes most of it anyway (>= 70 %). A GPU that takes 65 % of a period and a few late pictures is a CPU-bound scene.
    float slow = 0.0f, lateShare = 0.0f;
    int slowN = 0;
    float gpuAt = 0.85f, gpuLateAt = 0.70f, lateAt = 0.08f;
    float Cadence() const { return per >= heldAt ? 2.0f * base : base; }   // (the picture period the pictures are paced to: a held 30 or 60)
    bool GpuLimit() const {
        if (slowN < minN) return true;   // (nothing known yet: the old behaviour)
        const float c = Cadence();
        return slow >= gpuAt * c || (lateShare >= lateAt && slow >= gpuLateAt * c);
    }
    // The whole gate (MSC_PICTURE_HOLD_GPUBUSY) for one picture: may a late picture count towards a step down? Not before the pacing is known (the first minN pictures of a match: 2 s at 30, the
    // budget is still the 60 one and a CPU-bound start would be read as the GPU's); always when the budget is the 60 one (a GPU over 16.7 ms IS the limit at 60) or the GPU delays the pictures
    // (`limited`); at a held pace, only when the GPU's busy time is the limit (GpuLimit).
    bool GpuGate(float budget, float baseMs) const {
        if (n < minN) return false;
        if (budget <= baseMs + 0.5f || limited) return true;
        return GpuLimit();
    }
    void Reset() { per = gpu = 0.0f; n = 0; limited = false; slow = lateShare = 0.0f; slowN = 0; }
    // One measured picture: the wall time since the previous one (ms, over as many pictures as it covers) and its GPU busy time.
    void Add(float periodMs, float gpuMs) {
        if (periodMs > 100.0f) periodMs = 100.0f;   // (a hitch is not the pace)
        const float k = n < 32 ? 1.0f / static_cast<float>(n + 1) : 1.0f / 32.0f;
        per += (periodMs - per) * k;
        gpu += (gpuMs - gpu) * k;
        { const float ks = slowN < 90 ? 1.0f / static_cast<float>(slowN + 1) : 1.0f / 90.0f;
          slow += (gpuMs - slow) * ks;
          lateShare += ((periodMs > 1.25f * Cadence() ? 1.0f : 0.0f) - lateShare) * (slowN < 128 ? 1.0f / static_cast<float>(slowN + 1) : 1.0f / 128.0f);
          if (slowN < 1000000) ++slowN; }
        if (n < 1000000) ++n;
    }
    float Budget() {
        if (n < minN || per <= 0.0f) return base;
        const float r = gpu / per;
        limited = limited ? r >= lo : r >= hi;
        if (limited || per <= base) return base;
        return per >= heldAt ? per * margin : per;   // (a held 30: the pictures must make their flip with room to spare, so the budget is a share of the period: its p95, not its mean)
    }
};
// The ladder a match can use, from the sizes that have a target kept at kick-off: a prefix (a size step is never entered into a target that is not there, and the steps are taken in order).
// ready bit L (1..3) = the target of size level L is kept. Returns the last step of the order: the shed steps, then one per leading size that is kept.
inline int LadderMaxStep(unsigned ready, int maxStep, int maxSizes = kOldSizes) {   // (maxSizes: the floor's levels, vgx_floors.h)
    int m = kShedSteps;
    for (int l = 1; l <= kOldSizes && l <= maxSizes; ++l) { if (!(ready & (1u << l))) break; m = kShedSteps + l; }
    return m < maxStep ? m : maxStep;
}
// The new order (MSC_PICTURE_HOLD_AALAST): how many sizes the match's ladder has, the leading ones kept (the samples' steps follow the last of them: Policy::SetOrder).
inline int LadderSizes(unsigned ready, int maxSizes = kSizeSteps) {
    int n = 0;
    for (int l = 1; l <= kSizeSteps && l <= maxSizes; ++l) { if (!(ready & (1u << l))) break; n = l; }
    return n;
}
// What to give up before kick-off so that `needKib` of video memory stays free in play: the targets are let go in this order (4x, the smallest ladder size, the next, ... then 2x last), none
// in the middle of a match. have: bit 0..2 the ladder sizes 1..3 kept, bit 3 2x, bit 4 4x; kib[] the same order. Returns the bits to drop.
// New order (aaLast): bit 5 is the bottom size's one-sample target; the order is 4x, that one-sample target, the sizes from the smallest, the held size's 2x last.
inline unsigned DropForRoom(int freeKib, long needKib, unsigned have, const long* kib, bool aaLast = false) {
    unsigned drop = 0;
    long f = freeKib;
    if (freeKib < 0) return 0;
    const int orderOld[5] = {4, 2, 1, 0, 3};
    const int orderNew[8] = {7, 6, 4, 5, 2, 1, 0, 3};   // (bits 6 and 7: the two one-sample sizes below the sample step, the first to go)
    const int* order = aaLast ? orderNew : orderOld;
    const int nOrder = aaLast ? 8 : 5;
    for (int oi = 0; oi < nOrder; ++oi) {
        const int o = order[oi];
        if (f >= needKib) break;
        if (!(have & (1u << o))) continue;
        drop |= 1u << o;
        f += kib[o];
    }
    return drop;
}

// The video memory the hold reserves (KiB, colour + depth: 4 bytes a pixel each, a sample each; an ESTIMATE) and what the warm-up gate would make of it with `freeKib` free (-1: unknown) after the held
// size's own target. The order is the order of making: the ladder's targets, then 2x, then 4x; the order of letting go is the other way round (4x, then 2x, then the ladder).
struct MemPlan {
    long held = 0, ladder = 0, aa2 = 0, aa4 = 0;   // sizes
    int ladderKept = 0, ladderN = 0;               // ladder targets that pass the gate, of how many
    bool aa2ok = false, aa4ok = false;
    long total = 0;                                // what is reserved by what passes
};
inline long TargetKib(int w, int h, int samples) { return static_cast<long>(w) * h * 4 * (1 + samples) / 1024; }
inline MemPlan PlanMemory(int w, int h, const int* lw, const int* lh, int ladderN, int top, int freeKib, long floorKib = kTrimFloorKib) {
    MemPlan m;
    m.held = TargetKib(w, h, 1);
    m.ladderN = ladderN;
    long rem = freeKib;
    auto take = [&](long kib) { const bool ok = WarmRoomOk(rem < 0 ? -1 : static_cast<int>(rem), kib, floorKib); if (ok && rem >= 0) rem -= kib; return ok; };
    for (int i = 0; i < ladderN; ++i) { const long k = TargetKib(lw[i], lh[i], 1); m.ladder += k; if (take(k)) { ++m.ladderKept; m.total += k; } }
    m.aa2 = TargetKib(w, h, 2);
    m.aa4 = TargetKib(w, h, 4);
    if (top >= 2 && take(m.aa2)) { m.aa2ok = true; m.total += m.aa2; }
    if (top >= 4 && take(m.aa4)) { m.aa4ok = true; m.total += m.aa4; }
    m.total += m.held;
    return m;
}

// The size levels of the ladder the floor (vgx_floors.h, Quality: 864x488 at least) lets a match have: the leading levels (scale[1..n]) whose size, StepDim of the held size, is allowed.
inline int FloorLevels(int mode, int w, int h, const float* scale, int n);
// The size of a step: the held size times `scale` per side, rounded to a multiple of 8 and never below 16.
inline int StepDim(int v, float scale) {
    int s = static_cast<int>(static_cast<float>(v) * scale + 0.5f);
    s = (s + 4) / 8 * 8;
    return std::max(16, std::min(s, v));
}

inline int FloorLevels(int mode, int w, int h, const float* scale, int n) {
    int k = 0;
    while (k < n && floors::SizeAllowed(mode, StepDim(w, scale[k + 1]), StepDim(h, scale[k + 1]))) ++k;
    return k;
}

// What a match learned about its stadium: the step it spent most live-play frames in (ties go to the higher step), when there were at least `minFrames` of them. -1: not enough to say.
inline int Learned(const unsigned* frames, int minFrames = 1800) {
    unsigned total = 0;
    int best = 0;
    for (int s = 0; s <= kSteps; ++s) {
        total += frames[s];
        if (frames[s] >= frames[best]) best = s;
    }
    return static_cast<int>(total) >= minFrames ? best : -1;
}

// Where a match starts: the shed steps a heavy stadium is known to need (two: effects target, far shadows) or what the last match here learned (at most the shed steps: a size step is found
// by proof, in seconds, and never assumed), whichever is higher.
inline int StartStep(int learned, bool heavy, int cap = kShedSteps) {
    int s = heavy ? kShadowFar : 0;
    if (learned > s) s = learned;
    return std::min(s, cap);
}
// The safe start (MSC_PICTURE_HOLD_START, new order): a match starts at a level that holds the rate, in place before the screen opens, and climbs when room is proven. What the last match here
// learned if there is one (it is the step the match spent most time in); else a table from the console's numbers: the stadiums whose GPU time at 960x544 with 2x is far over 33 ms start on the
// lowest size (class 2), the middle ones on the 2x size just above the sample step (class 1), the rest at full size (class 0). Returns the level class; StartLevel maps it to a step.
inline int StadiumClass(int stadium) {
    switch (stadium) {
        case 1: case 4: case 7: case 9: case 14: case 15: return 2;   // The Dump, Lava Pit, Sand Tomb, Bowser, Storm Ship, Wastelands (r10: GPU 39 to 49 ms at 960x544)
        default: return 0;                                             // Vice class and the rest: full size (Pipeline included: it holds 60 and 30 at full size)
    }
}
inline int StartLevel(int learned, int cls, int last, int sizeFirst, int aaStep, int nSizes) {
    if (learned >= 0) return std::min(learned, last);
    if (cls <= 0) return 0;
    // class 2: the first size below the sample step (one sample, 752x424), or the sample step itself when the ladder has no such size
    return std::min(nSizes > aaStep - sizeFirst ? aaStep + 1 : aaStep, last);
}

// Where a held scene (cut-scene, replay) starts when nothing was learned: a level known to fit from the console's numbers: a Vice goal cut-scene cost 37-71 ms at 960x544 with 2 samples, so it
// starts on one sample and a smaller size: 752x424 (the first size below the sample step), the heavy stadiums on 704x400.
inline int SceneStartStep(int cls, int aaStep, int nSizes, int sizeFirst) {
    const int below = nSizes - (aaStep - sizeFirst);   // sizes below the sample step
    if (below <= 0) return aaStep;
    return cls >= 2 && below >= 2 ? aaStep + 2 : aaStep + 1;
}

// Per-stadium memory for one session (a stadium index below 32; others are not remembered).
struct StadiumMemory {
    signed char learned[32];
    StadiumMemory() { for (signed char& v : learned) v = -1; }
    int Get(int stadium) const { return stadium >= 0 && stadium < 32 ? learned[stadium] : -1; }
    void Put(int stadium, int step) { if (stadium >= 0 && stadium < 32 && step >= 0) learned[stadium] = static_cast<signed char>(step); }
};

// The held size named by MSC_PICTURE_HOLD: 1 or "native" the game's own, "full" the screen's, WxH any. false: not a size (0, off, empty, garbage).
inline bool ParseSize(const char* v, int* w, int* h) {
    if (!v || !*v) return false;
    auto eq = [&](const char* a) { const char* p = v; while (*a && *p == *a) { ++p; ++a; } return !*a && !*p; };
    if (eq("0") || eq("off") || eq("no")) return false;
    if (eq("1") || eq("native") || eq("on")) { *w = 640; *h = 448; return true; }
    if (eq("full") || eq("screen")) { *w = 960; *h = 544; return true; }
    int a = 0, b = 0;
    const char* p = v;
    while (*p >= '0' && *p <= '9') a = a * 10 + (*p++ - '0');
    if (*p != 'x' && *p != 'X') return false;
    ++p;
    const char* q = p;
    while (*p >= '0' && *p <= '9') b = b * 10 + (*p++ - '0');
    if (p == q || *p || a < 160 || b < 120 || a > 1920 || b > 1088) return false;
    *w = a;
    *h = b;
    return true;
}

} // namespace hold
} // namespace vgx
