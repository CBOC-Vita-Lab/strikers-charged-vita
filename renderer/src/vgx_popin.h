// 1.3 wave 11 (popin16) MSC_POPIN_BLOCK: no material pops in at the stadium walk-out. Pure policy, no GL (test: renderer/tests/popin.cpp).
//
// The mechanism (found in the code, the r13 console log and warm10's notes): a draw whose program is not linked yet is, in the main scene, LEFT OUT of the picture
// (asyncsh::Skippable) and its program is linked one a picture; a shader still compiling in the background is waited for at most a few ms and the draw is left out too.
// At the walk-out the first pictures meet many programs for the first time (r13 console: 31 programs resident at the start of the match, "+2 programs" a picture for the
// first pictures), so a stadium's, a player's or an effect's material is missing for some pictures and then appears: pop-in. The textures are not the cause: the
// pretouch has uploaded them under the loading picture (r13: 497 textures, 0 left, 6 first uses in the first picture).
// warm10 links the match's own 300 programs behind the loading picture (MSC_WARM_MATCH); what it leaves out (programs beyond the cap, a variant the list never saw) is
// still left out for a picture or more. This window closes that: from the match's first picture to POPIN_TAIL pictures after kick-off, every colour draw is made
// whole, its shader and program built at once. The cost is a longer picture in the walk-out (a link is 100-200 ms on the console, estimate) instead of a hole; the
// loading picture is not touched. Simulation: none (render only; the draw lists are unchanged).
//   MSC_POPIN_BLOCK=0|1   (code default 0)      MSC_POPIN_TAIL=<pictures> after kick-off (default 90)
#pragma once
#include <cstdint>

namespace vgx::popin {

struct Window {
    bool inMatch = false;       // the match's ledger is open (first picture after the loading scene)
    bool kicked = false;        // live play has begun
    uint64_t serial = 0;        // this picture
    uint64_t kickSerial = 0;    // the picture of the kick-off
};
// Pure: is the walk-out window open? Opens at the match's first picture, closes `tail` pictures after kick-off.
inline bool Active(const Window& w, uint32_t tail) {
#if defined(VGX_POPIN_BREAK) && VGX_POPIN_BREAK == 1
    (void)tail; return w.inMatch && !w.kicked;   // deliberate break: closes at kick-off, the tail is dropped
#elif defined(VGX_POPIN_BREAK) && VGX_POPIN_BREAK == 2
    (void)tail; return w.inMatch;                 // deliberate break: never closes (live play pays the blocking links)
#else
    if (!w.inMatch) return false;
    if (!w.kicked) return true;
    return w.serial < w.kickSerial + tail;
#endif
}
// Pure: a draw that could be left out is made whole when the window is open. (The caller still makes non-skippable draws whole under MSC_WARM_BLOCK.)
inline bool ForceWhole(bool skippable, bool windowOpen) { return skippable && windowOpen; }

struct Counters {
    uint64_t draws = 0;        // skippable draws seen in the window
    uint64_t forced = 0;       // of them, ones that made a program or shader link now (would have been left out)
    uint64_t stillOut = 0;     // made whole yet the program is not ok (dead: four failed links)
    uint64_t picturesOpen = 0; // pictures with the window open
    double linkMs = 0, worstMs = 0;   // time of the forced builds
    bool wasOpen = false;
    uint64_t wouldSkip = 0;    // draws that the shader worker's wait would have left out (async shaders, console only)
    uint32_t firstTex = 0, firstTexKib = 0, firstLinks = 0;   // the window's first picture (the hand-over from the loading scene)
    uint64_t lateTex = 0, lateTexKib = 0, latePicsTex = 0, lateLinks = 0, latePicsLink = 0;   // after it
};

}  // namespace vgx::popin
