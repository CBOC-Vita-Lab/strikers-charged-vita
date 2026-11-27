// Host test of AudioStreamKeep (native/include/Game/Audio/AudioStreamKeep.h) on a simulated clock: a music stream of 250 ms blocks in a
// two-block ring, played by a voice, refilled by reads that the game thread issues and completes once per game frame (AudioBackend::
// ServiceReadQueue, nlServiceFileSystem), polled every 3 ms by the audio update exactly as AudioReadState::Update does.
// Stalls of the game thread (100, 250, 1000 ms) and a disc read that arrives 1 s late: the music must be playing again afterwards, from valid data.
// The deliberate break (MSC_AUDIO_STREAM_KEEP=0, the old stop) must fail the same check, or the test proves nothing.
#include "../../../include/Game/Audio/AudioStreamKeep.h"
#include <cstdio>
#include <deque>

static int g_failed = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_failed; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

struct Out { bool stopped, running; int staleMs; int pausedMs; bool resumedValid; };

// stallAt/stallMs: the game thread issues and completes nothing in that window. lateMs: extra disc latency for reads issued in [lateFrom, lateFrom+2000).
static Out Run(bool keep, int stallAt, int stallMs, int lateFrom, int lateMs, int totalMs = 20000) {
    const int block = 250, frame = 16, baseLatency = 20;
    AudioStreamKeep k;
    int play = 0, readHead = 2 * block, validUntil = 2 * block, pending = 0;
    struct Rd { int readyAt; int upTo; bool issued; };
    std::deque<Rd> q;   // in order
    bool running = true, stopped = false;
    Out o = {false, false, 0, 0, true};
    int nextFrame = 0;
    for (int t = 0; t < totalMs; ++t) {
        const bool stalled = t >= stallAt && t < stallAt + stallMs;
        if (t >= nextFrame) {   // a game frame: issue the queued reads, complete the ones that have arrived
            nextFrame = t + frame;
            if (!stalled) {
                for (Rd& r : q) if (!r.issued) { r.issued = true; r.readyAt = t + baseLatency + ((t >= lateFrom && t < lateFrom + 2000) ? lateMs : 0); }
                while (!q.empty() && q.front().issued && q.front().readyAt <= t) { validUntil = q.front().upTo; --pending; q.pop_front(); }
            }
        }
        if (t % 3 != 0 || stopped) continue;   // the audio update
        if (k.starved) {
            const AudioStreamKeep::Act a = k.Tick(pending);
            if (a == AudioStreamKeep::kResume) running = true;
            if (a == AudioStreamKeep::kGiveUp) { stopped = true; running = false; }
            if (!running) o.pausedMs += 3;
            continue;
        }
        if (!running) continue;
        play += 3;
        if (play > validUntil) o.staleMs += 3;
        if (readHead - play < block) {
            if (pending > 2) {
                const AudioStreamKeep::Act a = k.Starve(keep);
                if (a == AudioStreamKeep::kPause) running = false;
                else if (a == AudioStreamKeep::kGiveUp) { stopped = true; running = false; }
                continue;
            }
            readHead += block;
            q.push_back({0, readHead, false});
            ++pending;
        }
    }
    // the check: playing at the end, on valid data, still being refilled
    o.stopped = stopped; o.running = running && !k.starved;
    o.resumedValid = play <= validUntil;
    return o;
}

int main() {
    struct { const char* name; int stallAt, stallMs, lateFrom, lateMs; } cases[] = {
        {"stall 100 ms", 5000, 100, -1, 0}, {"stall 250 ms", 5000, 250, -1, 0}, {"stall 1000 ms", 5000, 1000, -1, 0},
        {"late read 1 s", -1, 0, 5000, 1000}, {"stall 250 ms + late read", 5000, 250, 5000, 600},
    };
    for (auto& c : cases) {
        const Out keep = Run(true, c.stallAt, c.stallMs, c.lateFrom, c.lateMs);
        const Out old = Run(false, c.stallAt, c.stallMs, c.lateFrom, c.lateMs);
        std::printf("%-26s keep: playing %d stopped %d paused %d ms, stale %d ms | old stop: playing %d stopped %d\n", c.name, keep.running, keep.stopped, keep.pausedMs, keep.staleMs, old.running, old.stopped);
        CHECK(keep.running && !keep.stopped && keep.resumedValid);
        if (c.stallMs >= 1000 || c.lateMs >= 1000) CHECK(old.stopped);   // the break: the old stop kills the music here, so the check can fail
    }
    // a read that never completes: the old stop applies after about 6 s, the stream is not left silent for ever
    {
        AudioStreamKeep k; CHECK(k.Starve(true) == AudioStreamKeep::kPause);
        AudioStreamKeep::Act a = AudioStreamKeep::kNone; unsigned n = 0;
        while (a == AudioStreamKeep::kNone && n < 100000) { a = k.Tick(2); ++n; }
        CHECK(a == AudioStreamKeep::kGiveUp && n == AudioStreamKeep::kGiveUpTicks);
        CHECK(k.Starve(false) == AudioStreamKeep::kGiveUp);
    }
    std::printf("stream keep: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
