// Host test of the Vita output queue (vita_audio_queue.h): a producer that mixes 96-frame blocks the way the SDK's audio thread does (paced by
// PaceFrames, like np_vita_wire.cpp SinkWrite) and a consumer that takes 512-frame periods every 16 ms, on a simulated clock (microseconds).
// The signal is a sine, so a click is a sample-to-sample step far above a sine's own. Checks:
//   (1) steady producer: no underrun, no drop, the queue's level (the sound's latency) stays under the pace level;
//   (2) the mixer stalls for 40/100/250 ms (a busy core): the gap is soft (every step stays near the sine's own) and the sound is back after it
//       within the start level's time; no step at the fade edges;
//   (3) the policy kFixed (the 1.1 queue) has hard gap edges: the same metric must flag it (the deliberate break: it proves the test can fail);
//   (4) MSC_AUDIO_PACE_SLACK: latency and stall tolerance per slack value (printed: the table the owner's console A/B reads);
//   (5) latency never creeps up: after a stall the queue returns under the pace level.
#include "../src/vita_audio_queue.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace VitaAudioQueue;

static int g_failed = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_failed; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

struct Result {
    unsigned underruns = 0, dropped = 0, silentPeriods = 0, trims = 0;
    int maxStep = 0;            // largest sample-to-sample step in the output outside silence-to-silence
    int maxStepAtEdge = 0;      // ... right at a gap edge (a step from or to a zero run)
    double meanQueuedMs = 0;
    unsigned maxQueuedMs = 0;
    int firstSoundAfterStallMs = -1;
};

static const double kRate = 32000.0, kAmp = 12000.0, kHz = 440.0;
static const int kNaturalStep = (int)(2 * M_PI * kHz / kRate * kAmp) + 2;   // 1037: the sine's own steepest step

// stallStartMs/stallMs: the producer does nothing in that window (a stalled mixer). Runs `totalMs` of simulated time.
static Result Run(Policy pol, uint32_t slack, int stallStartMs, int stallMs, int totalMs) {
    static Queue q;   // (the ring is 64 KB)
    q.Reset(32000, pol);
    q.SetPaceSlack(slack);
    Result r;
    uint64_t t = 0, nextProd = 0, nextCons = 16000;
    double phase = 0;
    std::vector<Frame> out;
    out.reserve((size_t)totalMs * 32 + 1024);
    double sumQ = 0; unsigned nQ = 0;
    const uint64_t stallA = (uint64_t)stallStartMs * 1000, stallB = stallA + (uint64_t)stallMs * 1000;
    bool wasSilent = true;
    while (t < (uint64_t)totalMs * 1000) {
        t += 1000;   // 1 ms tick
        // producer: one 96-frame block per 3 ms of work, only while the queue is under the pace level (SinkWrite) and not stalled
        const bool stalled = t >= stallA && t < stallB;
        if (!stalled && t >= nextProd) {
            const int space = (pol == Policy::kElastic) ? (int)((q.PaceFrames() > q.Queued()) ? q.PaceFrames() - q.Queued() : 0) : 1 << 30;
            if (space >= 96) {
                Frame b[96];
                for (int i = 0; i < 96; ++i) {
                    const int16_t v = (int16_t)(kAmp * std::sin(phase));
                    phase += 2 * M_PI * kHz / kRate;
                    b[i].left = b[i].right = v;
                }
                q.Enqueue(b, 96);
                nextProd = t + 1000;   // the mixer takes about 1 ms per 3 ms block (3x real time), then waits for the sink's pace level
            }
        }
        if (t >= nextCons) {
            nextCons += 16000;
            Frame p[kPeriodFrames];
            q.Dequeue(p, kPeriodFrames);
            for (uint32_t i = 0; i < kPeriodFrames; ++i) out.push_back(p[i]);
            sumQ += q.Queued() * 1000.0 / 32000.0; ++nQ;
            if (q.Queued() * 1000u / 32000u > r.maxQueuedMs) r.maxQueuedMs = q.Queued() * 1000u / 32000u;
            bool loud = false;
            for (uint32_t i = 0; i < kPeriodFrames; ++i) if (std::abs(p[i].left) > 200) { loud = true; break; }
            if (loud && wasSilent && stalled == false && (int)(t / 1000) > stallStartMs + stallMs && r.firstSoundAfterStallMs < 0 && stallMs > 0)
                r.firstSoundAfterStallMs = (int)(t / 1000) - (stallStartMs + stallMs);
            wasSilent = !loud;
        }
    }
    const Stats s = q.GetStats();
    r.underruns = s.underruns; r.dropped = s.droppedBlocks; r.silentPeriods = s.silentPeriods; r.trims = s.trims;
    r.meanQueuedMs = nQ ? sumQ / nQ : 0;
    // steps: skip the opening (the start level fills in silence)
    size_t i0 = 0;
    while (i0 < out.size() && out[i0].left == 0) ++i0;
    for (size_t i = i0 + 1; i < out.size(); ++i) {
        const int d = std::abs((int)out[i].left - (int)out[i - 1].left);
        if (d > r.maxStep) r.maxStep = d;
        // a step into or out of a zero run longer than 8 frames is the edge of a gap
        const bool edge = (out[i].left == 0 && std::abs((int)out[i - 1].left) > 300) || (out[i - 1].left == 0 && std::abs((int)out[i].left) > 300);
        if (edge && d > r.maxStepAtEdge) r.maxStepAtEdge = d;
    }
    return r;
}

int main() {
    // (1) steady
    {
        const Result r = Run(Policy::kElastic, 3, 0, 0, 10000);
        std::printf("steady: underruns %u dropped %u trims %u, queue mean %.1f ms max %u ms, max step %d (sine's own %d)\n", r.underruns, r.dropped, r.trims,
                    r.meanQueuedMs, r.maxQueuedMs, r.maxStep, kNaturalStep);
        CHECK(r.underruns == 0 && r.dropped == 0 && r.trims == 0);
        CHECK(r.maxStep <= kNaturalStep + 20);
        CHECK(r.maxQueuedMs <= 24 + 48 + 16);   // start level + slack + one period
    }
    // (2) stalls
    for (int stall : {40, 100, 250}) {
        const Result r = Run(Policy::kElastic, 3, 3000, stall, 10000);
        std::printf("stall %3d ms: underruns %u silent periods %u, max step %d, step at gap edge %d, sound back %d ms after the stall, queue max %u ms\n", stall,
                    r.underruns, r.silentPeriods, r.maxStep, r.maxStepAtEdge, r.firstSoundAfterStallMs, r.maxQueuedMs);
        CHECK(r.dropped == 0);
        CHECK(r.maxStep <= kNaturalStep + 20);          // soft gap: no click
        if (stall >= 100) CHECK(r.underruns >= 1 && r.firstSoundAfterStallMs >= 0 && r.firstSoundAfterStallMs <= 260);
        CHECK(r.trims == 0);
    }
    // (3) the break: the 1.1 queue's gap edges are hard; the metric must catch that, or it proves nothing
    {
        const Result r = Run(Policy::kFixed, 3, 3000, 100, 10000);
        std::printf("fixed policy, stall 100 ms: max step %d (a click is anything over %d)\n", r.maxStep, kNaturalStep + 20);
        CHECK(r.maxStep > 2 * kNaturalStep);            // hard edge: the test can see a click
    }
    // (4) slack table
    std::printf("pace slack (periods): latency of the queue (mean ms) and the shortest stall that makes it run dry\n");
    for (uint32_t slack : {0u, 1u, 2u, 3u, 5u}) {
        const Result st = Run(Policy::kElastic, slack, 0, 0, 6000);
        int dry = -1;
        for (int stall = 8; stall <= 200; stall += 4) {
            if (Run(Policy::kElastic, slack, 3000, stall, 6000).underruns > 0) { dry = stall; break; }
        }
        std::printf("  slack %u: queue %.1f ms, runs dry from a stall of %d ms (steady underruns %u)\n", slack, st.meanQueuedMs, dry, st.underruns);
        if (slack == 3) CHECK(dry >= 40);
    }
    // (5) after a long stall the level returns under the pace level (no creeping latency, no forward skips)
    {
        const Result r = Run(Policy::kElastic, 3, 3000, 250, 20000);
        CHECK(r.trims == 0 && r.maxQueuedMs <= 208 + 48 + 16);
    }
    std::printf("audio queue: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
