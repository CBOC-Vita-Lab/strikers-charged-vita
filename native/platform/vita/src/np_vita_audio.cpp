// Audio output of the native port: a single-producer/single-consumer ring of 16-bit stereo frames,
// drained by a dedicated output thread into a sceAudioOut BGM port. Ported from the recompiled
// build's vita_audio.cpp (one/vita/src): the AudioBackend class of its runtime is replaced by the
// plain C functions of np_vita.h (NpAudioOpen / NpAudioPush...), everything below them (the queue
// and its two policies, the device thread, the delay line) is unchanged.
//
// The producer is the SDK layer's AX mixer (platform/sdk), which pushes one block per elapsed
// block period (3 ms of sound at 32 kHz), on one thread. sceAudioOutOutput blocks until the
// previous buffer has been taken, which is what paces the output thread.
//
// The queue and its two policies are in vita_audio_queue.h (workstream AUD, 5 Oct):
//   MSC_AUDIO_QUEUE=elastic (default)  start level that follows the producer's rhythm (32..160 ms),
//       an underrun is a faded gap, no dropped blocks: the mixer asks NpAudioSpaceFrames() before it
//       mixes a block and does not produce audio there is no room for;
//   MSC_AUDIO_QUEUE=fixed              the queue as it was: 120 ms limit, new blocks dropped when
//       it is full, hard gaps.
// MSC_AUDIO_DELAY_MS=<ms> (default 0): a fixed delay line between the queue and the device, for the
// frame presenter (the picture it shows is one built frame behind).
//
// The ring and the conversion are plain C++ and also run on armhf Linux and x86, where the "device"
// is a thread that discards a period of audio every period.
#include "np_vita.h"
#include "np_vita_internal.h"

#include "vita_audio_queue.h"

#include <atomic>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__vita__)
#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#else
#include <chrono>
#include <thread>
#endif

namespace {

using VitaAudioQueue::Frame;
using VitaAudioQueue::kPeriodFrames;

VitaAudioQueue::Queue g_queue;        // the ring and both policies: vita_audio_queue.h
VitaAudioQueue::Policy g_policy = VitaAudioQueue::Policy::kElastic;
std::atomic<uint32_t> g_sampleRate{0};
std::atomic<bool> g_run{false};
std::atomic<bool> g_open{false};
std::atomic<int> g_volume{32768};     // 0..32768 (0 dB), already includes mute
bool g_reportedDrop = false;

inline uint32_t Queued() { return g_queue.Queued(); }

// Producer side. Returns false when the block had to be dropped.
bool Enqueue(const Frame* frames, uint32_t count) {
    if (g_queue.Enqueue(frames, count)) return true;
    if (!g_reportedDrop) {
        g_reportedDrop = true;
        std::fprintf(stderr, "[audio] output queue full (%u/%u frames); dropping blocks to preserve continuity\n",
                     g_queue.Queued(), g_queue.LimitFrames());
    }
    return false;
}

// Consumer side: fills one output period.
void Dequeue(Frame* out, uint32_t count) { g_queue.Dequeue(out, count); }

#if defined(__vita__)
SceUID g_thread = -1;
int g_port = -1;

// MSC_AUDIO_DELAY_MS=<ms> (default 0): a fixed delay line in front of the device, for the frame presenter
// (MSC_VGX_PRESENT): the picture it shows is one built frame behind the game, so without this the sound runs that
// much ahead of the picture. Whole periods, at most 500 ms; silence while it fills at start. The queue and its
// policies do not see it (it sits between the queue and the device), so it adds latency and nothing else.
uint32_t g_delayPeriods = 0;

// The device reads a buffer after sceAudioOutOutput has returned (the call blocks until the PREVIOUS one is taken), so the
// thread must not write the buffer it just handed over: three buffers in turn (17 Oct; one buffer rewritten while it played
// could be heard as a buzz at the period rate, here 62.5 Hz).
constexpr int kOutBuffers = 3;

int OutputThread(SceSize, void*) {
    static Frame periods[kOutBuffers][kPeriodFrames];
    int cur = 0;
    Frame* ring = nullptr;   // g_delayPeriods periods: a slot is sent g_delayPeriods iterations after it was filled
    uint32_t slot = 0;
    if (g_delayPeriods) {
        ring = static_cast<Frame*>(std::calloc(static_cast<size_t>(g_delayPeriods) * kPeriodFrames, sizeof(Frame)));
        if (!ring) g_delayPeriods = 0;
    }
    int applied = -1;
    if (VitaMem::RegisterThread) VitaMem::RegisterThread("audio output");
    while (g_run.load(std::memory_order_relaxed)) {
        const int volume = g_volume.load(std::memory_order_relaxed);
        if (volume != applied) {
            int both[2] = {volume, volume};
            sceAudioOutSetVolume(g_port, static_cast<SceAudioOutChannelFlag>(SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH), both);
            applied = volume;
        }
        Frame* const period = periods[cur];
        cur = cur + 1 == kOutBuffers ? 0 : cur + 1;
        if (ring) {
            // Write the newest period into this slot after sending the one it held (g_delayPeriods periods old).
            Frame* const p = ring + static_cast<size_t>(slot) * kPeriodFrames;
            std::memcpy(period, p, sizeof(Frame) * kPeriodFrames);
            Dequeue(p, kPeriodFrames);
            slot = slot + 1 == g_delayPeriods ? 0 : slot + 1;
        } else {
            Dequeue(period, kPeriodFrames);
        }
        sceAudioOutOutput(g_port, period);   // blocks until the previous buffer is taken
    }
    std::free(ring);
    return sceKernelExitDeleteThread(0);
}

bool StartDevice(uint32_t sampleRate) {
    g_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, kPeriodFrames, static_cast<int>(sampleRate),
                                 SCE_AUDIO_OUT_MODE_STEREO);
    if (g_port < 0) {
        std::fprintf(stderr, "[audio] sceAudioOutOpenPort(BGM, %u Hz) failed: 0x%08X\n", sampleRate, static_cast<unsigned>(g_port));
        return false;
    }
    if (const char* d = std::getenv("MSC_AUDIO_DELAY_MS")) {
        unsigned ms = static_cast<unsigned>(std::strtoul(d, nullptr, 10));
        if (ms > 500) ms = 500;
        g_delayPeriods = static_cast<uint32_t>((static_cast<uint64_t>(ms) * sampleRate / 1000 + kPeriodFrames / 2) / kPeriodFrames);
        std::fprintf(stderr, "[audio] delay line: %u ms asked, %u periods of %u frames (%.1f ms)\n", ms, g_delayPeriods, kPeriodFrames,
                     1000.0 * g_delayPeriods * kPeriodFrames / sampleRate);
    }
    g_run.store(true);
    // Above the game thread's default priority (lower number = higher
    // priority) and on the third core, away from the game thread (core 0);
    // vita_threads.cpp has the plan (MSC_VITA_CORE_AUDIO in env.txt).
    g_thread = sceKernelCreateThread("msc_audio_out", OutputThread, 0x10000100 - 32, 0x10000, 0,
                                     VitaThreads::CoreMask(VitaThreads::kAudioOut), nullptr);
    if (g_thread < 0 || sceKernelStartThread(g_thread, 0, nullptr) < 0) {
        std::fprintf(stderr, "[audio] could not start the output thread: 0x%08X\n", static_cast<unsigned>(g_thread));
        g_run.store(false);
        sceAudioOutReleasePort(g_port);
        g_port = -1;
        return false;
    }
    return true;
}

void StopDevice() {
    g_run.store(false);
    if (g_thread >= 0) {
        sceKernelWaitThreadEnd(g_thread, nullptr, nullptr);
        g_thread = -1;
    }
    if (g_port >= 0) {
        sceAudioOutReleasePort(g_port);
        g_port = -1;
    }
}
#else
std::thread g_thread;

bool StartDevice(uint32_t sampleRate) {
    g_run.store(true);
    g_thread = std::thread([sampleRate] {
        static Frame period[kPeriodFrames];
        const auto periodTime = std::chrono::microseconds(1000000ull * kPeriodFrames / sampleRate);
        auto next = std::chrono::steady_clock::now();
        while (g_run.load(std::memory_order_relaxed)) {
            Dequeue(period, kPeriodFrames);
            next += periodTime;
            std::this_thread::sleep_until(next);
        }
    });
    return true;
}

void StopDevice() {
    g_run.store(false);
    if (g_thread.joinable()) g_thread.join();
}
#endif

} // namespace

// ---------------------------------------------------------------------------------------------
// np_vita.h
// ---------------------------------------------------------------------------------------------
namespace {
std::mutex g_audioMutex;
bool g_initialized = false;
uint32_t g_openRate = 0;
float g_masterVolume = 1.0f;
bool g_muted = false;
void ApplyVolume() { g_volume.store(g_muted ? 0 : static_cast<int>(g_masterVolume * 32768.0f)); }
} // namespace

extern "C" {

int NpAudioOpen(uint32_t sampleRate) {
    std::lock_guard<std::mutex> lock(g_audioMutex);
    if (g_initialized && g_openRate == sampleRate) return 1;
    if (sampleRate == 0) {
        std::fprintf(stderr, "[audio] unsupported output format: %u Hz\n", sampleRate);
        return 0;
    }
    if (g_initialized) {
        StopDevice();
        g_open.store(false);
        g_initialized = false;
    }
    {
        const char* policy = std::getenv("MSC_AUDIO_QUEUE");
        g_policy = (policy && std::strcmp(policy, "fixed") == 0) ? VitaAudioQueue::Policy::kFixed : VitaAudioQueue::Policy::kElastic;
    }
    g_queue.Reset(sampleRate, g_policy);
    g_sampleRate.store(sampleRate);
    if (!StartDevice(sampleRate)) return 0;
    g_openRate = sampleRate;
    g_initialized = true;
    g_open.store(true);
    ApplyVolume();
    std::fprintf(stderr, "[audio] output open: %u Hz stereo, %u-frame periods, queue %s (limit %u ms)\n", sampleRate, kPeriodFrames,
                 g_policy == VitaAudioQueue::Policy::kFixed ? "fixed" : "elastic", g_queue.LimitFrames() * 1000u / sampleRate);
    return 1;
}

// Wii AI DMA frames are big-endian and ordered right, left.
int NpAudioPushWiiAiBE16(const uint8_t* data, size_t bytes) {
    if (!data || bytes < 4 || !g_initialized) return 0;
    static Frame block[2048];
    size_t frameCount = bytes / 4;
    int pushed = 1;
    while (frameCount != 0) {
        const uint32_t n = static_cast<uint32_t>(frameCount < 2048 ? frameCount : 2048);
        for (uint32_t i = 0; i < n; ++i) {
            const uint8_t* p = data + static_cast<size_t>(i) * 4;
            block[i].right = static_cast<int16_t>(static_cast<uint16_t>(p[0] << 8 | p[1]));
            block[i].left = static_cast<int16_t>(static_cast<uint16_t>(p[2] << 8 | p[3]));
        }
        if (!Enqueue(block, n)) pushed = 0;
        data += static_cast<size_t>(n) * 4;
        frameCount -= n;
    }
    return pushed;
}

// Interleaved left, right, native endian.
int NpAudioPushLE16(const int16_t* samples, uint32_t frames) {
    if (!samples || frames == 0 || !g_initialized) return 0;
    static_assert(sizeof(Frame) == 2 * sizeof(int16_t), "Frame is two packed samples");
    const Frame* f = reinterpret_cast<const Frame*>(samples);
    int pushed = 1;
    while (frames != 0) {
        const uint32_t n = frames < 2048 ? frames : 2048;
        if (!Enqueue(f, n)) pushed = 0;
        f += n;
        frames -= n;
    }
    return pushed;
}

void NpAudioSetVolume(float volume) {
    std::lock_guard<std::mutex> lock(g_audioMutex);
    g_masterVolume = volume < 0.0f ? 0.0f : (volume > 1.0f ? 1.0f : volume);
    ApplyVolume();
}

void NpAudioSetMuted(int muted) {
    std::lock_guard<std::mutex> lock(g_audioMutex);
    g_muted = muted != 0;
    ApplyVolume();
}

// How many frames fit before the mixer produces a block: with the elastic queue no block is
// produced that would have to be dropped. A negative answer means "not known" (fixed policy).
int NpAudioSpaceFrames(void) {
    if (g_policy != VitaAudioQueue::Policy::kElastic || !g_open.load(std::memory_order_relaxed)) return -1;
    return static_cast<int>(g_queue.SpaceFrames());
}

// How many frames the producer may push now without passing the queue's pace level (vita_audio_queue.h PaceFrames):
// what a producer with no clock of its own waits for. MSC_AUDIO_PACE=limit restores the old pacing by SpaceFrames.
int NpAudioPaceFrames(void) {
    if (g_policy != VitaAudioQueue::Policy::kElastic || !g_open.load(std::memory_order_relaxed)) return -1;
    static const bool byLimit = [] { const char* e = std::getenv("MSC_AUDIO_PACE"); return e && std::strcmp(e, "limit") == 0; }();
    if (byLimit) return static_cast<int>(g_queue.SpaceFrames());
    const uint32_t pace = g_queue.PaceFrames(), queued = Queued();
    return queued < pace ? static_cast<int>(pace - queued) : 0;
}

} // extern "C"

namespace VitaAudio {
Stats GetStats() {
    Stats stats;
    const VitaAudioQueue::Stats queue = g_queue.GetStats();
    stats.queuedFrames = Queued();
    stats.droppedBlocks = queue.droppedBlocks;
    stats.underruns = queue.underruns;
    stats.sampleRate = g_sampleRate.load(std::memory_order_relaxed);
    stats.open = g_open.load(std::memory_order_relaxed);
    stats.elastic = g_policy == VitaAudioQueue::Policy::kElastic;
    stats.silentPeriods = queue.silentPeriods;
    stats.trims = queue.trims;
    stats.trimmedFrames = queue.trimmedFrames;
    stats.startFrames = queue.startFrames;
    stats.maxQueued = queue.maxQueued;
    return stats;
}

void LogReport(const char* when) {
    const Stats s = GetStats();
    if (!s.open || !s.sampleRate) return;
    const auto ms = [&](uint32_t frames) { return static_cast<unsigned>(static_cast<uint64_t>(frames) * 1000u / s.sampleRate); };
    std::fprintf(stderr, "[vita] audio at %s: queue %s, %u ms queued (start level %u ms, highest fill %u ms), ran dry %u times, "
                         "%u silent periods (%u ms), %u dropped blocks, %u forward skips (%u ms)\n",
                 when, s.elastic ? "elastic" : "fixed", ms(s.queuedFrames), ms(s.startFrames), ms(s.maxQueued), s.underruns,
                 s.silentPeriods, s.silentPeriods * ms(kPeriodFrames), s.droppedBlocks, s.trims, ms(s.trimmedFrames));
}
} // namespace VitaAudio
