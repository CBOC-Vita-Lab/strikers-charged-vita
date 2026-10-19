#pragma once
#include <atomic>
#include <cstdint>

// One writer (the game thread). A single atomic word holds either elapsed pause
// time or the frozen instant, so readers cannot combine two different pauses.
class NpModalClock {
    static constexpr uint64_t kPaused = uint64_t(1) << 63;
    std::atomic<uint64_t> state_{0};
public:
    template<class Clock> uint64_t Now(Clock realNow) const {
        for (;;) {
            const uint64_t before = state_.load(std::memory_order_acquire);
            if (before & kPaused) return before & ~kPaused;
            const uint64_t now = realNow();
            if (state_.load(std::memory_order_acquire) == before) return now - before;
        }
    }
    void Pause(uint64_t realNow) {
        const uint64_t before = state_.load(std::memory_order_relaxed);
        if (!(before & kPaused)) state_.store(kPaused | (realNow - before), std::memory_order_release);
    }
    void Resume(uint64_t realNow) {
        const uint64_t before = state_.load(std::memory_order_relaxed);
        if (before & kPaused) state_.store(realNow - (before & ~kPaused), std::memory_order_release);
    }
};
