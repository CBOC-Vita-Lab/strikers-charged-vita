// MSC_VGX_HUDNATIVE split present, split17: a counter pair that proves every present the host holds (PresentSplitBegin returned true) reaches exactly one swap
// (PresentSplitFinish). Header only, no switch: the hooks are only reached on the split path, which exists only with MSC_VGX_HUDNATIVE=1, so the counters and the line
// are invisible with it off. Render thread only (Begin, Finish and the buffer end all run on the thread that executes the command buffers).
//   Begin()           the host made the display copy and holds the swap
//   Finish()          the swap of a held present was made
//   BufferEnd(hudOn)  the back end left a buffer with the display still standing in for the EFB: a leak; the caller finishes it (one swap), so the picture is never stale
#pragma once
#include <cstdint>
#include <cstdio>
namespace vgx { namespace split17 {
struct State {
    uint64_t held = 0;       // Begin calls
    uint64_t finished = 0;   // Finish calls that closed a held present
    uint64_t leaked = 0;     // a Begin while one was open, or a buffer that ended with one open
    uint64_t stray = 0;      // a Finish with nothing held
    bool open = false;
};
inline State& S() { static State s; return s; }
inline void Line(const char* why) {
    const State& s = S();
    std::fprintf(stderr, "[split] held %llu finished %llu leaked %llu stray %llu%s%s\n", static_cast<unsigned long long>(s.held), static_cast<unsigned long long>(s.finished),
                 static_cast<unsigned long long>(s.leaked), static_cast<unsigned long long>(s.stray), why[0] ? " " : "", why);
}
inline void Begin() {
    State& s = S();
    if (s.open) { ++s.leaked; if (s.leaked <= 20) Line("(begin while a held present was open)"); }
    s.open = true;
    ++s.held;
}
inline void Finish() {
    State& s = S();
    if (!s.open) { ++s.stray; if (s.stray <= 20) Line("(finish with nothing held)"); return; }
    s.open = false;
    ++s.finished;
    if (s.finished == 1 || s.finished % 600 == 0) Line("");
}
// True: the buffer ended with the display still the target; the caller must finish the present.
inline bool BufferEnd(bool hudOn) {
    State& s = S();
    if (!hudOn && !s.open) return false;
    if (s.open) { ++s.leaked; if (s.leaked <= 20) Line("(buffer ended with a held present)"); }
    return hudOn;
}
inline bool Balanced() { const State& s = S(); return !s.open && s.held == s.finished && s.leaked == 0 && s.stray == 0; }
inline void Reset() { S() = State{}; }
} }   // namespace vgx::split17
