// The loading screen's life cycle as a pure state machine (1.3, "no loading in play"). vita_bootscreen.cpp keeps the drawing; WHEN the screen may be drawn and WHEN it ends for
// good is decided here, so a host test can drive every path (tests/test_bootstate.cpp).
//
//   Undecided -> Art (Begin) -> [Finish(): the game created its boot screens: the bar runs out and the picture fades, kFadeS] -> Done (final)
//   Done is final. Nothing re-arms it: the loading screen belongs to the process start; the port does not use it for a match load (the game has its own loading scenes).
//
// What ends it, whichever path the game's pictures take:
//   - the display callback's Compose (hold path), kFadeS after Finish: the fade has run;
//   - the first game picture presented kFadeS after Finish (OnGamePresent), if the display callback never composed (the ladder path, a long present, a stall);
//   - the screen's own thread, kGraceS after Finish (OnThreadTick), if neither of the above happened;
//   - a resume from the settings screen or from sleep (OnResume) after Finish: the screen is not redrawn, it ends.
// Once Done, Compose draws nothing, the thread is stopped, and an attempt from any other code is logged once by the caller (Guard()).
#pragma once

namespace BootState {

enum Mode { kUndecided = 0, kArt = 1, kText = 2, kDone = 3 };
constexpr double kFadeS = 0.45;    // Finish() to the end of the fade (VitaBootScreen::ComposeLocked)
constexpr double kGraceS = 1.5;    // the screen's own thread ends it this long after Finish at the latest

struct Machine {
    int mode = kUndecided;
    double finishS = -1.0;         // when Finish() was called, process seconds; < 0 not yet
    const char* endedBy = nullptr; // why it ended (for the log line)
    bool guardLogged = false;
};

#ifndef BOOTSTATE_BREAK
#define BOOTSTATE_BREAK 0
#endif

inline bool Active(const Machine& m) { return m.mode == kArt; }

inline bool Finish(Machine& m, double now) {
    if (m.mode != kArt || m.finishS >= 0.0) return false;
    m.finishS = now;
    return true;
}

inline void End(Machine& m, const char* why) {
    m.mode = kDone;
    m.endedBy = why;
}

// The display callback has a buffer to show: true = draw the loading screen into it. The fade over, the call ends the screen (the buffer still gets the last picture of the fade).
inline bool OnCompose(Machine& m, double now) {
    if (m.mode != kArt) return BOOTSTATE_BREAK == 4 && m.mode == kDone;   // (mutant 4: draws after the end)
    if (m.finishS >= 0.0 && now - m.finishS > kFadeS) End(m, "the fade ran out in the display callback");
    return true;
}

// A game picture has been presented (the game thread, whichever path showed it). True = the screen has just ended and its thread must be stopped.
inline bool OnGamePresent(Machine& m, double now) {
    if (BOOTSTATE_BREAK == 1) return false;   // (mutant 1: only the display callback ends it)
    if (m.mode != kArt || m.finishS < 0.0) return false;
    if (now - m.finishS <= kFadeS) return false;
    End(m, "a game picture after the fade time");
    return true;
}

// The screen's own thread, once per redraw. True = ended just now.
inline bool OnThreadTick(Machine& m, double now) {
    if (BOOTSTATE_BREAK == 3) return false;   // (mutant 3: the thread never ends it)
    if (m.mode != kArt || m.finishS < 0.0) return false;
    if (now - m.finishS <= kGraceS) return false;
    End(m, "the screen's own timer");
    return true;
}

// The console side took the display back (settings closed, a wake from sleep, a stall notice): may the loading screen be drawn on it? Only before Finish.
inline bool OnResume(Machine& m, double now) {
    (void)now;
    if (BOOTSTATE_BREAK == 2) return m.mode == kArt;   // (mutant 2: redraws after Finish)
    if (m.mode != kArt) return false;
    if (m.finishS >= 0.0) {
        End(m, "a resume after Finish");
        return false;
    }
    return true;
}

// Another piece of code asks for the loading screen (a notice, progress, a new milestone) after the end: nothing is drawn; true once, for the caller to log one line.
inline bool Guard(Machine& m) {
    if (m.mode != kDone || m.guardLogged) return false;
    m.guardLogged = true;
    return true;
}

} // namespace BootState
