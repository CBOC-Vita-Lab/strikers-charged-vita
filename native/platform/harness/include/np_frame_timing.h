// np_frame_timing.h: the MSC_FRAME_TIMING windows (from Codex's frame-timing work, as kept in
// one/vita/src/vita_frame_timing.h). Only change: the Segment is filled by the harness from the
// game's own state (np_harness.cpp) instead of ReadSegment() over guest memory.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

namespace NpFrameTiming {
enum class Reason { Window, Identity, Reset, End };
struct Segment {
    int32_t scene = -1, gameState = -1;
    uint32_t sceneObject = 0, game = 0, task = 0;   // object pointers truncated to 32 bits
    bool paused = false;
};
struct Report {
    Segment segment;
    Reason reason = Reason::Window;
    uint32_t count = 0, over33333 = 0, over16667 = 0;
    uint64_t firstFrame = 0, lastFrame = 0, startUs = 0, endUs = 0, totalUs = 0;
    uint64_t minUs = 0, p50Us = 0, p95Us = 0, p99Us = 0, maxUs = 0;
};
struct Event {
    bool hasReport = false, boundary = false, reset = false;
    uint64_t gapUs = 0, fromFrame = 0, toFrame = 0;
    Segment from, to;
    Report report;
};
inline unsigned ParseWindow(const char* value) {
    if (!value) return 0;
    if (!std::strcmp(value, "1") || !std::strcmp(value, "120")) return 120;
    return !std::strcmp(value, "300") ? 300 : 0;
}
class Window {
public:
    explicit Window(unsigned size) : limit_(size == 300 ? 300 : 120) {}
    Event Add(uint64_t frame, uint64_t now, const Segment& segment) {
        Event event{};
        if (!anchored_) { Anchor(frame, now, segment); return event; }
        const bool reset = frame <= lastFrame_ || frame - lastFrame_ != 1 || now <= lastUs_;
        if (reset || !Same(segment, segment_)) {
            event = Flush(reset ? Reason::Reset : Reason::Identity);
            event.boundary = true;
            event.reset = reset;
            event.gapUs = now >= lastUs_ ? now - lastUs_ : 0;
            event.from = segment_; event.to = segment;
            event.fromFrame = lastFrame_; event.toFrame = frame;
            Anchor(frame, now, segment);
            return event;
        }
        const uint64_t interval = now - lastUs_;
        if (!report_.count) {
            report_.segment = segment;
            report_.firstFrame = lastFrame_;
            report_.startUs = lastUs_;
        }
        intervals_[report_.count++] = interval;
        report_.totalUs += interval;
        report_.over33333 += interval > 33333;
        report_.over16667 += interval > 16667;
        report_.lastFrame = frame;
        report_.endUs = now;
        Anchor(frame, now, segment);
        if (report_.count == limit_) return Flush(Reason::Window);
        return event;
    }
    Event Finish() {
        Event event = Flush(Reason::End);
        anchored_ = false;
        return event;
    }

private:
    static bool Same(const Segment& a, const Segment& b) {
        return a.scene == b.scene && a.sceneObject == b.sceneObject && a.game == b.game &&
               a.gameState == b.gameState && a.task == b.task && a.paused == b.paused;
    }
    void Anchor(uint64_t frame, uint64_t now, const Segment& segment) {
        anchored_ = true; lastFrame_ = frame; lastUs_ = now; segment_ = segment;
    }
    Event Flush(Reason reason) {
        Event event{};
        if (!report_.count) return event;
        auto sorted = intervals_;
        std::sort(sorted.begin(), sorted.begin() + report_.count);
        const auto percentile = [&](unsigned p) { return sorted[(report_.count * p + 99) / 100 - 1]; };
        report_.reason = reason;
        report_.minUs = sorted[0]; report_.maxUs = sorted[report_.count - 1];
        report_.p50Us = percentile(50); report_.p95Us = percentile(95); report_.p99Us = percentile(99);
        event.hasReport = true;
        event.report = report_;
        report_ = {};
        return event;
    }
    std::array<uint64_t, 300> intervals_{};
    Report report_{};
    Segment segment_{};
    uint64_t lastFrame_ = 0, lastUs_ = 0;
    unsigned limit_ = 120;
    bool anchored_ = false;
};
}
