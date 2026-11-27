// MSC_PACE_LOG: where one picture's time goes between the render thread and the screen, per window, for the console test. Pure code (no clock, no I/O, no GL) so a host test runs it; the Vita host
// (renderer/vita/vgx_host_vita.cpp) feeds it from the two threads that touch a picture and prints one line each per window:
//   render thread (SwapDisplay):  the swap call split in two (vglSwapBuffers = the display queue taking the buffer, which blocks while the queue is full; then the GPU-queue limiter = the wait for
//                                 the frame `limit` swaps back to finish), and the GPU's own time of the frame (vglRtLastGpuFrame, the existing GPU timer). Medians, p95 and maxima from 1 ms bins.
//   display thread (GateDisplay): how many vblanks the callback waited for its due vblank (the picture was ready early: the gate or MSC_PRESENT_SYNC waited), the vblank it was shown at against the
//                                 vblank it was first possible at (the flip), the vblanks each picture was held (1 / 2 / 3+), the frame ids shown (the tag of the frame the picture came from:
//                                 first, last, how many ids were never shown, how many were shown twice), and the pictures held 3 vblanks or more by (picture number % 600) in 60-wide bins, so a
//                                 periodic cause (the 600-frame sweep) shows as one bin. The first lines of such long holds name the picture and frame.
// A window is MSC_PACE_LOG seconds (default 0 = off; the Vita plan sets 5).
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>

#ifndef VGX_PACELOG_BREAK
#define VGX_PACELOG_BREAK 0   // host test mutants: 1 an id gap counts every id as missing, 2 the fold bins by picture % 599, 3 repeats are counted as gaps, 4 the 16-bit stamp's wrap is not undone
#endif

namespace vgx::pacelog {

struct Hist {
    uint32_t bin[102] = {};   // 0..100 ms in 1 ms bins (bin i holds [i, i + 1)), 101: 101 ms and over
    uint32_t n = 0;
    float max = 0.0f;
    double sum = 0.0;
    void Add(float ms) {
        if (ms < 0.0f) ms = 0.0f;
        const unsigned i = ms >= 101.0f ? 101u : static_cast<unsigned>(ms);
        ++bin[i];
        ++n;
        sum += ms;
        if (ms > max) max = ms;
    }
    // The lower edge of the bin the p-th fraction of the samples falls in (whole ms), 0 for no samples.
    float Percentile(double p) const {
        if (!n) return 0.0f;
        uint32_t want = static_cast<uint32_t>(p * n);
        if (want >= n) want = n - 1;
        uint32_t seen = 0;
        for (unsigned i = 0; i < 102; ++i) {
            seen += bin[i];
            if (seen > want) return i >= 101 ? max : static_cast<float>(i);
        }
        return max;
    }
    float Mean() const { return n ? static_cast<float>(sum / n) : 0.0f; }
    void Clear() { *this = Hist(); }
};

struct RenderWin {
    Hist queue, limiter, gpu;   // ms per picture: the display-queue call, the GPU-queue limiter, the GPU's busy time of a frame
    uint32_t swaps = 0, limiterWaits = 0;
    double startMs = -1.0;
    void Swap(double nowMs, float queueMs, float limiterMs, float gpuMs) {
        if (startMs < 0.0) startMs = nowMs;
        ++swaps;
        queue.Add(queueMs);
        limiter.Add(limiterMs);
        if (limiterMs > 0.05f) ++limiterWaits;
        if (gpuMs >= 0.0f) gpu.Add(gpuMs);
    }
    bool Due(double nowMs, double windowMs) const { return startMs >= 0.0 && nowMs - startMs >= windowMs; }
    size_t Print(char* out, size_t cap, double nowMs, uint64_t picture) {
        const double s = (nowMs - startMs) / 1000.0;
        const int n = std::snprintf(out, cap,
                                    "[pace] render picture %llu, %.1f s, %u swaps (%.1f a second): display-queue call median/p95/max %.0f/%.0f/%.0f ms; GPU-queue limiter waited %u times, median/p95/max %.0f/%.0f/%.0f ms; "
                                    "GPU busy mean/p95/max %.1f/%.0f/%.0f ms (%u timed)",
                                    static_cast<unsigned long long>(picture), s, swaps, s > 0 ? swaps / s : 0.0, queue.Percentile(0.5), queue.Percentile(0.95), queue.max, limiterWaits,
                                    limiter.Percentile(0.5), limiter.Percentile(0.95), limiter.max, gpu.Mean(), gpu.Percentile(0.95), gpu.max, gpu.n);
        queue.Clear(); limiter.Clear(); gpu.Clear();
        swaps = limiterWaits = 0;
        startMs = nowMs;
        return n > 0 ? static_cast<size_t>(n) : 0u;
    }
};

struct DisplayWin {
    static constexpr unsigned kFold = 10;   // 600 / 60
    uint32_t pictures = 0, waited[4] = {}, held[4] = {}, late = 0;   // waited: vblanks the callback waited for its due vblank 0/1/2/3+; held: vblanks shown 1/2/3+ (index 0: 0 = two in one vblank); late: shown after the first possible vblank
    uint32_t flipLate[4] = {};   // shown - first possible vblank: 0/1/2/3+
    uint32_t ids = 0, idGaps = 0, idRepeats = 0, idBackwards = 0;
    uint64_t firstId = 0, lastId = 0;
    bool haveId = false;
    uint32_t fold[kFold] = {}, longHolds = 0;
    uint32_t liveHeld[4] = {};
    Hist toShown;   // wave 10: ms from the vblank a picture's frame began at to the vblank it was shown at (frame start to screen)
    double startMs = -1.0;
    uint32_t linesLeft = 40;   // long-hold lines per run (the caller decrements through LongHoldLine)
    // One picture. earliest: the first vblank it could have been shown at; shown: the vblank it was shown at; lastShown: the previous picture's (0 for none); waitedVb: vblanks the callback waited;
    // id: the frame tag (0: a picture the platform drew itself); picture: the running picture number; live: match play.
    void Shown(double nowMs, uint32_t earliest, uint32_t shown, uint32_t lastShown, bool haveLast, uint32_t waitedVb, uint64_t id, uint64_t picture, bool live) {
        if (startMs < 0.0) startMs = nowMs;
        ++pictures;
        ++waited[waitedVb > 3 ? 3 : waitedVb];
        const uint32_t fl = static_cast<uint32_t>(static_cast<int32_t>(shown - earliest) > 0 ? shown - earliest : 0);
        ++flipLate[fl > 3 ? 3 : fl];
        if (fl) ++late;
        if (haveLast) {
            const uint32_t h = shown - lastShown;
            const unsigned b = h == 0 ? 0u : (h >= 3 ? 3u : h);
            ++held[b];
            if (live) ++liveHeld[b];
            if (h >= 3 && h < 1000) {
                ++longHolds;
#if VGX_PACELOG_BREAK == 2
                ++fold[(picture % 599) / 60 % kFold];
#else
                ++fold[(picture % 600) / 60];
#endif
            }
        }
        if (id) {
            if (haveId) {
                if (id == lastId) {
#if VGX_PACELOG_BREAK == 3
                    ++idGaps;
#else
                    ++idRepeats;
#endif
                } else if (id < lastId) ++idBackwards;
                else if (id > lastId + 1) {
#if VGX_PACELOG_BREAK == 1
                    idGaps += static_cast<uint32_t>(id - firstId);
#else
                    idGaps += static_cast<uint32_t>(id - lastId - 1);
#endif
                }
            } else firstId = id;
            haveId = true;
            lastId = id;
            ++ids;
        }
    }
    // wave 10: the picture's frame start (the vblank count the game stamped it with, 16 bits) to its shown vblank, in ms (a vblank is 16.683 ms)
    void Delay(uint32_t shown, uint32_t stampVblank16) {
#if VGX_PACELOG_BREAK == 4
        const uint32_t vb = shown - stampVblank16;
#else
        const uint32_t vb = (shown - stampVblank16) & 0xFFFFu;
#endif
        if (vb < 600) toShown.Add(static_cast<float>(vb) * 16.683f);   // (a stamp older than 10 s is not a frame start)
    }
    bool Due(double nowMs, double windowMs) const { return startMs >= 0.0 && nowMs - startMs >= windowMs; }
    bool LongHoldLine() { return linesLeft > 0 && linesLeft-- > 0; }
    size_t Print(char* out, size_t cap, double nowMs, uint64_t picture, int interval) {
        const double s = (nowMs - startMs) / 1000.0;
        size_t n = 0;
        auto add = [&](int w) { if (w > 0) n += static_cast<size_t>(w) < cap - n ? static_cast<size_t>(w) : cap - n - 1; };
        const uint32_t hn = held[1] + held[2] + held[3];
        add(std::snprintf(out, cap,
                          "[pace] display picture %llu, %.1f s, interval %d, %u pictures shown (%.1f a second): callback waited 0/1/2/3+ vblanks %u/%u/%u/%u; shown after the first possible vblank by 0/1/2/3+ %u/%u/%u/%u; "
                          "held 1/2/3+ vblanks %u/%u/%u (%.1f/%.1f/%.1f %%), live play 1/2/3+ %u/%u/%u; frame ids %u shown (%llu..%llu), %u never shown, %u shown twice, %u out of order; "
                          "frame start to shown median/p95/max %.0f/%.0f/%.0f ms (%u stamped), estimated press to picture mean %.0f ms (frame start to shown mean + half a picture interval, a press lands anywhere between two pad reads; the measured one is [lat]); "
                          "holds of 3+ by picture %% 600 in 60-wide bins:",
                          static_cast<unsigned long long>(picture), s, interval, pictures, s > 0 ? pictures / s : 0.0, waited[0], waited[1], waited[2], waited[3], flipLate[0], flipLate[1], flipLate[2], flipLate[3],
                          held[1], held[2], held[3], hn ? 100.0 * held[1] / hn : 0.0, hn ? 100.0 * held[2] / hn : 0.0, hn ? 100.0 * held[3] / hn : 0.0, liveHeld[1], liveHeld[2], liveHeld[3], ids,
                          static_cast<unsigned long long>(firstId), static_cast<unsigned long long>(lastId), idGaps, idRepeats, idBackwards,
                          toShown.Percentile(0.5), toShown.Percentile(0.95), toShown.max, toShown.n, toShown.n ? toShown.Mean() + (s > 0 && pictures ? 500.0 * s / pictures : 0.0) : 0.0));
        for (unsigned i = 0; i < kFold; ++i) add(std::snprintf(out + n, cap - n, " %u", fold[i]));
        const uint32_t keep = linesLeft;
        *this = DisplayWin();
        linesLeft = keep;
        startMs = nowMs;
        return n;
    }
};

} // namespace vgx::pacelog
