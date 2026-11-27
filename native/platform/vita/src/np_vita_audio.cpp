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
//
// 1.2, music after the system suspends the game (the Home menu, standby): Galaxy had to fix this one and so does any BGM port. Two ways in to the same repair, both
// ending in "close the port, open it again, set the volume again" (the ring, the mixer and the AX voices above it are plain memory and keep going, so nothing
// else needs to be started):
//   * the system says so: the platform's power callback (np_vita_wire.cpp) calls VitaAudio::NotifyResume() when the console wakes up;
//   * the port says so: sceAudioOutOutput returns an error (the port was taken away or closed while the game slept). Until it is open again the thread
//     drops the period it would have played and waits one period's time, so the mixer above keeps its real-time pace instead of racing ahead with nobody listening.
// MSC_AUDIO_RESUME=0 turns both off (the 1.1 behaviour: the output thread ignores the result of sceAudioOutOutput).
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
std::atomic<bool> g_paused{false}, g_pauseAcknowledged{false};
std::atomic<int> g_volume{32768};     // 0..32768 (0 dB), already includes mute
std::atomic<uint32_t> g_resumeSeq{0}; // VitaAudio::NotifyResume() counts here; the output thread compares with what it last acted on
std::atomic<uint32_t> g_reopens{0};   // times the output port was closed and opened again
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

// Close the output port and open it again (the thread's own call). Returns whether the new port is open (g_port >= 0).
bool ReopenPort(const char* why, uint32_t sampleRate) {
    const int old = g_port;
    if (old >= 0) sceAudioOutReleasePort(old);
    g_port = -1;
    sceKernelDelayThread(20 * 1000);   // (the system needs a moment after a wake-up before it hands a port out)
    g_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, kPeriodFrames, static_cast<int>(sampleRate), SCE_AUDIO_OUT_MODE_STEREO);
    const uint32_t n = g_reopens.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 8)
        std::fprintf(stderr, "[audio] output port reopened (%s): old port %d, new port 0x%08X%s\n", why, old, static_cast<unsigned>(g_port), g_port >= 0 ? "" : " (failed, will try again)");
    return g_port >= 0;
}

int OutputThread(SceSize, void*) {
    if (VitaThreads::CoreMask(VitaThreads::kAudioOut) & 0x00080000) VitaThreads::Core3Register(sceKernelGetThreadId());   // (T8) listed for the online guard and the stray sweep
    static Frame periods[kOutBuffers][kPeriodFrames];
    int cur = 0;
    Frame* ring = nullptr;   // g_delayPeriods periods: a slot is sent g_delayPeriods iterations after it was filled
    uint32_t slot = 0;
    if (g_delayPeriods) {
        ring = static_cast<Frame*>(std::calloc(static_cast<size_t>(g_delayPeriods) * kPeriodFrames, sizeof(Frame)));
        if (!ring) g_delayPeriods = 0;
    }
    int applied = -1;
    static const bool recover = [] { const char* e = std::getenv("MSC_AUDIO_RESUME"); return !(e && e[0] == '0'); }();
    const uint32_t rate = g_sampleRate.load(std::memory_order_relaxed);
    const SceUInt periodUs = rate ? static_cast<SceUInt>(1000000ull * kPeriodFrames / rate) : 16000u;
    uint32_t seenResume = g_resumeSeq.load(std::memory_order_acquire);
    unsigned failStreak = 0, cooldown = 0;
    if (VitaMem::RegisterThread) VitaMem::RegisterThread("audio output");
    while (g_run.load(std::memory_order_relaxed)) {
        if (recover) {
            const uint32_t resume = g_resumeSeq.load(std::memory_order_acquire);
            if (resume != seenResume) {
                seenResume = resume;
                if (cooldown == 0) {   // (the system may say "waking up" in more than one notification: one repair is enough)
                    ReopenPort("the console woke up", rate);
                    applied = -1;
                    failStreak = 0;
                    cooldown = 60;     // about a second
                }
            }
            if (cooldown != 0) --cooldown;
        }
        const int volume = g_volume.load(std::memory_order_relaxed);
        if (volume != applied && g_port >= 0) {
            int both[2] = {volume, volume};
            sceAudioOutSetVolume(g_port, static_cast<SceAudioOutChannelFlag>(SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH), both);
            applied = volume;
        }
        Frame* const period = periods[cur];
        cur = cur + 1 == kOutBuffers ? 0 : cur + 1;
        const bool paused = g_paused.load(std::memory_order_acquire);
        g_pauseAcknowledged.store(paused, std::memory_order_release);
        if (paused) {
            std::memset(period, 0, sizeof(Frame) * kPeriodFrames);
        } else if (ring) {
            // Write the newest period into this slot after sending the one it held (g_delayPeriods periods old).
            Frame* const p = ring + static_cast<size_t>(slot) * kPeriodFrames;
            std::memcpy(period, p, sizeof(Frame) * kPeriodFrames);
            Dequeue(p, kPeriodFrames);
            slot = slot + 1 == g_delayPeriods ? 0 : slot + 1;
        } else {
            Dequeue(period, kPeriodFrames);
        }
        if (!recover) {
            sceAudioOutOutput(g_port, period);   // blocks until the previous buffer is taken
            continue;
        }
        const int rc = g_port >= 0 ? sceAudioOutOutput(g_port, period) : -1;   // blocks until the previous buffer is taken
        if (rc >= 0) {
            if (failStreak != 0) {
                std::fprintf(stderr, "[audio] output is back after %u failed period(s)\n", failStreak);
                failStreak = 0;
            }
            continue;
        }
        // The port does not take sound (closed or taken away while the game slept, or not open yet). The period was already dequeued, so the mixer is
        // kept at its real-time pace by waiting one period here; the port is opened again after three bad periods and then every 30 (about half a second).
        if (failStreak == 0 && g_port >= 0)
            std::fprintf(stderr, "[audio] sceAudioOutOutput failed: 0x%08X\n", static_cast<unsigned>(rc));
        ++failStreak;
        sceKernelDelayThread(periodUs);
        if (failStreak == 3 || (failStreak > 3 && (failStreak - 3) % 30 == 0)) {
            ReopenPort("the output failed", rate);
            applied = -1;
        }
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
    // MSC_AUDIO_PRIO=<n> (1.2; np-player.txt / env.txt): the output thread's priority in sceKernel's form (default 0x10000100 - 32 = 128, the
    // same as the SDK's mixer and timer threads on this core; 0x10000100 - 48 = 112 puts the device feed ahead of the mixer, the timers and the
    // job and frame-send workers (144), so that a mix burst or a worker chunk can never delay the hand-over of a period).
    // A value the kernel cannot take (neither 64..191 absolute nor 0x10000100 +- an offset that lands in that range) or a thread that
    // cannot be created with it must not cost the game its sound: the default is tried once instead (and logged).
    const int kDefaultPriority = 0x10000100 - 32;
    int outputPriority = kDefaultPriority;
    if (const char* p = std::getenv("MSC_AUDIO_PRIO"); p && *p) {
        const long long v = std::strtoll(p, nullptr, 0);
        const long long absolute = (v >= 0x10000000LL && v <= 0x1000FFFFLL) ? 160 + (v - 0x10000100LL) : v;
        if (absolute >= 64 && absolute <= 191 && v >= 0 && v <= 0x7FFFFFFFLL) {
            outputPriority = static_cast<int>(v);
        } else {
            std::fprintf(stderr, "[audio] MSC_AUDIO_PRIO=%s is out of range (64..191, or 0x10000100 +- offset): using the default 0x%X\n", p,
                         static_cast<unsigned>(kDefaultPriority));
        }
    }
    // (T8, Galaxy's CORE3.md: its audio thread at the top user priority, 64, is the only thing it runs that can delay system threads on core 3) With the core rules on, the output thread is no more
    // urgent than MSC_VITA_CORE3_AUDIO_PRIO (default 128) while it is on the system core. The default priority is 128 already: this only bites when a line asked for more.
    {
        const int onSystem = VitaThreads::CoreMask(VitaThreads::kAudioOut) & 0x00080000;
        const int capped = NpVitaCore3::PriorityOnCore3(VitaThreads::CurrentRulesNow(), onSystem != 0, outputPriority, NpVitaCore3::Core3PrioFloor(std::getenv("MSC_VITA_CORE3_AUDIO_PRIO")));
        if (capped != outputPriority) {
            std::fprintf(stderr, "[audio] core 3 rules: output priority 0x%X is more urgent than MSC_VITA_CORE3_AUDIO_PRIO allows on the system core: 0x%X\n", static_cast<unsigned>(outputPriority), static_cast<unsigned>(capped));
            outputPriority = capped;
        }
    }
    g_thread = sceKernelCreateThread("msc_audio_out", OutputThread, outputPriority, 0x10000, 0,
                                     VitaThreads::CoreMask(VitaThreads::kAudioOut), nullptr);
    if (g_thread < 0 && outputPriority != kDefaultPriority) {
        std::fprintf(stderr, "[audio] output thread with priority 0x%X failed (0x%08X): retrying with the default 0x%X\n",
                     static_cast<unsigned>(outputPriority), static_cast<unsigned>(g_thread), static_cast<unsigned>(kDefaultPriority));
        g_thread = sceKernelCreateThread("msc_audio_out", OutputThread, kDefaultPriority, 0x10000, 0,
                                         VitaThreads::CoreMask(VitaThreads::kAudioOut), nullptr);
    }
    if (g_thread < 0 && VitaThreads::CoreMask(VitaThreads::kAudioOut) != VitaThreads::PlainMask(VitaThreads::kAudioOut)) {   // (1.3) the fourth core was refused after all: never lose the sound over it
        std::fprintf(stderr, "[audio] output thread on mask 0x%X failed (0x%08X): retrying on the plan's core\n", static_cast<unsigned>(VitaThreads::CoreMask(VitaThreads::kAudioOut)), static_cast<unsigned>(g_thread));
        g_thread = sceKernelCreateThread("msc_audio_out", OutputThread, kDefaultPriority, 0x10000, 0, VitaThreads::PlainMask(VitaThreads::kAudioOut), nullptr);
    }
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
            const bool paused = g_paused.load(std::memory_order_acquire);
            g_pauseAcknowledged.store(paused, std::memory_order_release);
            if (!paused) Dequeue(period, kPeriodFrames);
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
    if (const char* e = std::getenv("MSC_AUDIO_PACE_SLACK"); e && *e) g_queue.SetPaceSlack(static_cast<uint32_t>(std::strtoul(e, nullptr, 10)));   // (1.3) see vita_audio_queue.h
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

int NpAudioIsOpen(void) { return g_open.load(std::memory_order_relaxed) ? 1 : 0; }

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
void NotifyResume() { g_resumeSeq.fetch_add(1, std::memory_order_release); }
uint32_t Reopens() { return g_reopens.load(std::memory_order_relaxed); }
bool Pause(bool paused) {
    g_paused.store(paused, std::memory_order_release);
    for (int i = 0; i < 100; ++i) {
        if (!g_run.load(std::memory_order_acquire) || g_pauseAcknowledged.load(std::memory_order_acquire) == paused) return true;
#if defined(__vita__)
        sceKernelDelayThread(1000);
#else
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
#endif
    }
    if (paused) g_paused.store(false, std::memory_order_release);
    return false;
}
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
