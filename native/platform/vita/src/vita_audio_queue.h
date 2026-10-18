// The output queue of the Vita audio back end: a single-producer / single-consumer ring of
// 16-bit stereo frames between the runtime's Audio_HLE_Tick (game thread, whole AI DMA blocks of
// 96 frames) and the output thread (one period of 512 frames per sceAudioOutOutput call).
//
// Workstream AUD wrote this file for vita/src/ (it is the body of the patch in aud/for-lab/) and
// uses it unchanged in its queue model (aud/wt/scripts/aud/plat/plat_audio_tap.cpp), so the code
// that was measured is the code of the patch. No platform call in here: time is counted in
// output periods, which is the only clock the output thread has anyway.
//
// Two policies:
//   kFixed    what vita_audio.cpp did until now (kept for comparison and as the fall-back):
//             the producer drops a block that would take the queue over 120 ms; the consumer
//             plays silence whenever less than one period is queued and resumes at two periods.
//   kElastic  (1) the consumer's start level follows the producer: it measures, in periods, the
//                 longest pause between two arrivals of the last few seconds and waits for that
//                 much plus one and a half periods before it (re)starts, between 2 and 13 periods
//                 (24..200 ms); a level that proved too small (an underrun while playing) is
//                 raised at once, and the level sinks again when arrivals become regular;
//             (2) an underrun is a SOFT gap: the frames that are left are played with a fade-out
//                 (the consumer keeps a reserve of 4 ms so that there is always something to
//                 fade), silence follows, and playback resumes with a fade-in. No block is cut
//                 off at full level, so there is no click at either edge;
//             (3) the producer is never made to drop a block in normal operation: the limit is
//                 the ring (kHardLimitMs), and the runtime asks SpaceFrames() before it mixes a
//                 block (runtime/src/hle/audio/audio.cpp, MscAudioQueueSpaceFrames): audio that
//                 would not fit is not produced, the game's sound simply runs on after the gap;
//             (4) if the queue nevertheless stays more than kTrimSlackPeriods above the start
//                 level for kTrimAfterPeriods periods in a row (latency crept up after a
//                 catch-up), the consumer skips forward to the start level through a cross-fade:
//                 one soft discontinuity instead of a stream of dropped blocks.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

namespace VitaAudioQueue {

struct Frame {
    int16_t left, right;
};

constexpr uint32_t kRingFrames = 16384; // power of two; 512 ms at 32 kHz
constexpr uint32_t kRingMask = kRingFrames - 1;
constexpr uint32_t kPeriodFrames = 512; // 16 ms at 32 kHz; BGM ports take multiples of 64

enum class Policy { kFixed, kElastic };

// kFixed
constexpr uint32_t kFixedLimitMs = 120;
// kElastic
constexpr uint32_t kHardLimitMs = 320;      // the producer's limit (the ring holds 512 ms)
constexpr uint32_t kMinStartPeriods = 2;    // 32 ms
constexpr uint32_t kMaxStartPeriods = 13;   // 208 ms
constexpr uint32_t kFadeFrames = 256;       // 8 ms
constexpr uint32_t kReserveFrames = 128;    // a period is played normally only if this much stays queued behind it
constexpr uint32_t kTrimSlackPeriods = 6;   // 96 ms above the start level ...
constexpr uint32_t kTrimAfterPeriods = 125; // ... for 2 s without a break: skip forward
constexpr uint32_t kGapMemoryPeriods = 250; // a longest pause is remembered for 4 s

struct Stats {
    uint32_t droppedBlocks = 0;  // producer: blocks refused (queue at its limit)
    uint32_t underruns = 0;      // consumer: times playback ran dry
    uint32_t silentPeriods = 0;  // consumer: periods (or parts) of silence handed to the device
    uint32_t trims = 0;          // consumer: forward skips (kElastic)
    uint32_t trimmedFrames = 0;
    uint32_t startFrames = 0;    // the current start level
    uint32_t maxQueued = 0;      // highest fill seen by the producer after a push
};

class Queue {
public:
    void Reset(uint32_t sampleRate, Policy policy) {
        m_head.store(0);
        m_tail.store(0);
        m_rate = sampleRate;
        m_policy = policy;
        m_starved = true;
        m_startPeriods = kMinStartPeriods;
        m_periodsSinceArrival = 0;
        m_longestPause = 0;
        m_longestPauseAge = 0;
        m_overPeriods = 0;
        m_seenHead = 0;
        m_fadeIn = false;
    }

    uint32_t Queued() const {
        return m_head.load(std::memory_order_acquire) - m_tail.load(std::memory_order_acquire);
    }

    uint32_t LimitFrames() const {
        const uint32_t ms = m_policy == Policy::kFixed ? kFixedLimitMs : kHardLimitMs;
        const uint32_t limit = static_cast<uint32_t>(static_cast<uint64_t>(m_rate) * ms / 1000u);
        return limit < kRingFrames ? limit : kRingFrames;
    }

    // Producer: frames that can still be pushed without a drop.
    uint32_t SpaceFrames() const {
        const uint32_t limit = LimitFrames();
        const uint32_t queued = Queued();
        return queued < limit ? limit - queued : 0;
    }

    // Producer pacing (17 Oct): the fill a producer with no clock of its own should wait for. The native port's SDK audio
    // thread mixes the next block as soon as the sink returns, so a sink that waited for SpaceFrames() alone kept the queue
    // at the 320 ms limit: 320 ms of sound lag, and rule (4) above skipped ~255 ms of sound every 2 s (the maintainer console,
    // 17 Oct: 208 skips, 54 s of 6 minutes of play). Pacing to the start level plus half the trim slack (48 ms) keeps the
    // queue under the trim and follows the start level when an underrun raises it.
    uint32_t PaceFrames() const {
        uint32_t want = m_startFramesNow.load(std::memory_order_relaxed);
        if (want < kMinStartPeriods * kPeriodFrames) want = kMinStartPeriods * kPeriodFrames;
        want += (kTrimSlackPeriods / 2) * kPeriodFrames;
        const uint32_t limit = LimitFrames();
        return want < limit ? want : limit;
    }

    // Producer. Returns false when the block had to be dropped.
    bool Enqueue(const Frame* frames, uint32_t count) {
        const uint32_t queued = Queued();
        if (queued + count > LimitFrames()) {
            m_dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        const uint32_t head = m_head.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < count; ++i) {
            m_ring[(head + i) & kRingMask] = frames[i];
        }
        m_head.store(head + count, std::memory_order_release);
        if (queued + count > m_maxQueued.load(std::memory_order_relaxed)) {
            m_maxQueued.store(queued + count, std::memory_order_relaxed);
        }
        return true;
    }

    // Consumer: fills one output period.
    void Dequeue(Frame* out, uint32_t count) {
        if (m_policy == Policy::kFixed) {
            DequeueFixed(out, count);
        } else {
            DequeueElastic(out, count);
        }
    }

    Stats GetStats() const {
        Stats s;
        s.droppedBlocks = m_dropped.load(std::memory_order_relaxed);
        s.underruns = m_underruns.load(std::memory_order_relaxed);
        s.silentPeriods = m_silentPeriods.load(std::memory_order_relaxed);
        s.trims = m_trims.load(std::memory_order_relaxed);
        s.trimmedFrames = m_trimmedFrames.load(std::memory_order_relaxed);
        s.startFrames = m_startFramesNow.load(std::memory_order_relaxed);
        s.maxQueued = m_maxQueued.load(std::memory_order_relaxed);
        return s;
    }

private:
    // The policy of vita_audio.cpp before this file, unchanged.
    void DequeueFixed(Frame* out, uint32_t count) {
        const uint32_t queued = Queued();
        if (queued < count || (m_starved && queued < 2 * count)) {
            if (!m_starved) m_underruns.fetch_add(1, std::memory_order_relaxed);
            m_starved = true;
            m_silentPeriods.fetch_add(1, std::memory_order_relaxed);
            std::memset(out, 0, count * sizeof(Frame));
            return;
        }
        m_starved = false;
        Copy(out, m_tail.load(std::memory_order_relaxed), count);
        m_tail.store(m_tail.load(std::memory_order_relaxed) + count, std::memory_order_release);
        m_startFramesNow.store(2 * count, std::memory_order_relaxed);
    }

    void DequeueElastic(Frame* out, uint32_t count) {
        const uint32_t head = m_head.load(std::memory_order_acquire);
        uint32_t tail = m_tail.load(std::memory_order_relaxed);
        uint32_t queued = head - tail;

        // The producer's rhythm, in periods: how long was the longest pause between arrivals?
        if (head != m_seenHead) {
            // (Remembered up to the largest level only: a stall of seconds must not take minutes to forget.)
            const uint32_t pause = m_periodsSinceArrival < kMaxStartPeriods ? m_periodsSinceArrival : kMaxStartPeriods;
            if (pause >= m_longestPause) {
                m_longestPause = pause;
                m_longestPauseAge = 0;
            }
            m_periodsSinceArrival = 0;
            m_seenHead = head;
        }
        ++m_periodsSinceArrival;
        if (++m_longestPauseAge > kGapMemoryPeriods) {
            // Forget an old maximum step by step (a single long stall must not cost latency for ever).
            if (m_longestPause > 0) --m_longestPause;
            m_longestPauseAge = kGapMemoryPeriods / 2;
        }
        // Start level: the longest pause plus one and a half periods, at least what an underrun taught.
        uint32_t want = m_longestPause + 2;
        if (want < m_learnedPeriods) want = m_learnedPeriods;
        if (want < kMinStartPeriods) want = kMinStartPeriods;
        if (want > kMaxStartPeriods) want = kMaxStartPeriods;
        m_startPeriods = want;
        const uint32_t startFrames = m_startPeriods * count - count / 2;
        m_startFramesNow.store(startFrames, std::memory_order_relaxed);
        if (m_learnedAge++ > kGapMemoryPeriods && m_learnedPeriods > kMinStartPeriods) {
            --m_learnedPeriods;
            m_learnedAge = 0;
        }

        if (m_starved) {
            if (queued < startFrames) {
                m_silentPeriods.fetch_add(1, std::memory_order_relaxed);
                std::memset(out, 0, count * sizeof(Frame));
                return;
            }
            m_starved = false;
            m_fadeIn = true;
            m_overPeriods = 0;
        }

        // Latency crept up and stayed up: skip forward to the start level, cross-faded.
        if (queued > startFrames + kTrimSlackPeriods * count) {
            if (++m_overPeriods >= kTrimAfterPeriods && queued >= startFrames + count + kFadeFrames) {
                const uint32_t skip = queued - (startFrames + count);
                Copy(out, tail + skip, count);
                Frame old[kFadeFrames];
                Copy(old, tail, kFadeFrames);
                for (uint32_t i = 0; i < kFadeFrames; ++i) {
                    const int32_t in = static_cast<int32_t>(i);
                    const int32_t fade = static_cast<int32_t>(kFadeFrames) - in;
                    out[i].left = static_cast<int16_t>((out[i].left * in + old[i].left * fade) / static_cast<int32_t>(kFadeFrames));
                    out[i].right = static_cast<int16_t>((out[i].right * in + old[i].right * fade) / static_cast<int32_t>(kFadeFrames));
                }
                m_tail.store(tail + skip + count, std::memory_order_release);
                m_trims.fetch_add(1, std::memory_order_relaxed);
                m_trimmedFrames.fetch_add(skip, std::memory_order_relaxed);
                m_overPeriods = 0;
                m_fadeIn = false;
                return;
            }
        } else {
            m_overPeriods = 0;
        }

        if (queued >= count + kReserveFrames) {
            Copy(out, tail, count);
            m_tail.store(tail + count, std::memory_order_release);
            if (m_fadeIn) {
                FadeIn(out, count < kFadeFrames ? count : kFadeFrames);
                m_fadeIn = false;
            }
            return;
        }

        // Running dry: play what is left (at most this period) with a fade-out, then silence: a
        // soft gap. The reserve guarantees that a playing queue still holds at least
        // kReserveFrames here, so the fade is never shorter than that.
        const uint32_t play = queued < count ? queued : count;
        Copy(out, tail, play);
        if (m_fadeIn) {
            FadeIn(out, play < kFadeFrames ? play : kFadeFrames);
            m_fadeIn = false;
        }
        const uint32_t fade = play < kFadeFrames ? play : kFadeFrames;
        for (uint32_t i = 0; i < fade; ++i) {
            Frame& f = out[play - fade + i];
            const int32_t gain = static_cast<int32_t>(fade - 1 - i);
            f.left = static_cast<int16_t>(f.left * gain / static_cast<int32_t>(fade));
            f.right = static_cast<int16_t>(f.right * gain / static_cast<int32_t>(fade));
        }
        std::memset(out + play, 0, (count - play) * sizeof(Frame));
        m_tail.store(tail + play, std::memory_order_release);
        m_starved = true;
        m_underruns.fetch_add(1, std::memory_order_relaxed);
        m_silentPeriods.fetch_add(1, std::memory_order_relaxed);
        // The level was too small for this producer: one more period next time, remembered a while.
        if (m_learnedPeriods < m_startPeriods + 1) m_learnedPeriods = m_startPeriods + 1;
        if (m_learnedPeriods > kMaxStartPeriods) m_learnedPeriods = kMaxStartPeriods;
        m_learnedAge = 0;
    }

    void Copy(Frame* out, uint32_t from, uint32_t count) const {
        for (uint32_t i = 0; i < count; ++i) {
            out[i] = m_ring[(from + i) & kRingMask];
        }
    }

    static void FadeIn(Frame* out, uint32_t frames) {
        for (uint32_t i = 0; i < frames; ++i) {
            out[i].left = static_cast<int16_t>(out[i].left * static_cast<int32_t>(i) / static_cast<int32_t>(frames));
            out[i].right = static_cast<int16_t>(out[i].right * static_cast<int32_t>(i) / static_cast<int32_t>(frames));
        }
    }

    Frame m_ring[kRingFrames];
    std::atomic<uint32_t> m_head{0}; // total frames written (producer)
    std::atomic<uint32_t> m_tail{0}; // total frames read (consumer)
    uint32_t m_rate = 32000;
    Policy m_policy = Policy::kFixed;
    // consumer state
    bool m_starved = true;
    bool m_fadeIn = false;
    uint32_t m_startPeriods = kMinStartPeriods;
    uint32_t m_learnedPeriods = kMinStartPeriods;
    uint32_t m_learnedAge = 0;
    uint32_t m_periodsSinceArrival = 0;
    uint32_t m_longestPause = 0;
    uint32_t m_longestPauseAge = 0;
    uint32_t m_overPeriods = 0;
    uint32_t m_seenHead = 0;
    // statistics
    std::atomic<uint32_t> m_dropped{0};
    std::atomic<uint32_t> m_underruns{0};
    std::atomic<uint32_t> m_silentPeriods{0};
    std::atomic<uint32_t> m_trims{0};
    std::atomic<uint32_t> m_trimmedFrames{0};
    std::atomic<uint32_t> m_startFramesNow{0};
    std::atomic<uint32_t> m_maxQueued{0};
};

} // namespace VitaAudioQueue
