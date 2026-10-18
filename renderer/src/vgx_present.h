// Frame presenter (workstream FI2): shows pictures at a fixed rate (30 or 60 per second) from a game thread that builds
// frames at a rate of its own, by interpolating between the two newest built frames (vgx_interp.h) on a display clock
// (vgx_present_sched.h has the algorithm and its comments). Runs on the render thread: the front end's finished frames
// come to Sink(); the player loop calls Service() between decoding slices.
//
//   MSC_VGX_PRESENT=<hz>        0 / unset: off (default; MSC_VGX_INTERP=1 then behaves as before: two pictures per built
//                               frame, shown back to back). 30, 60: display rate; 1 means 30.
//   MSC_VGX_PRESENT_CLOCK=wall|virtual   wall (default): ticks are driven by the clock, a late frame makes the last picture
//                               repeat and the display delay grow. virtual: no clock: when a frame arrives, the pictures of
//                               its interval are drawn at once (harness runs with its virtual time; the pictures are the
//                               ones the wall clock mode shows for an ideal steady game thread; deterministic).
//                               (virtual: frame n is at n x MSC_VGX_PRESENT_VINTERVAL ms [66.67], not read from any clock)
//   MSC_VGX_PRESENT_MARGIN=<ms> [20]   least buffer margin of the clock controller (the larger of this and 2 sigma)
//   MSC_VGX_PRESENT_GAIN=<f> [0.1]     largest speed change of the display clock; 0: no controller (the delay only grows)
//   MSC_VGX_PRESENT_BYPASS=<ms> [1.15 x tick]  smoothed built interval at or below which every frame is shown as itself
//   MSC_VGX_PRESENT_EXTRAP=<ms> [0]    how far past the newest frame's game time the display clock may run (extrapolating the
//                               matrices): lowers the delay by that much, at the price of prediction errors
//   MSC_VGX_PRESENT_LOG=1|2     summary every 300 ticks | one line per tick
//   MSC_VGX_PRESENT_CSV=<file>  one row per tick
// Test knobs (no effect unless set): MSC_VGX_THROTTLE=<ms>[:<jitter>] (game thread sleeps that long after every present, real
// time: a slow game thread for wall clock runs), MSC_VGX_PRESENT_DRAWMS=<ms> (render thread busy for that long per picture: the
// null back end then costs what a console picture does).
#pragma once
#include "vgx_cmd.h"

namespace vgx::present {

bool Enabled();
bool Wall();
// Frame sink (SetFrameSink): takes the finished frame by swapping buffers; `buf` comes back as a spare.
void Sink(CmdBuf& buf);
// Render thread, wall clock mode: runs the ticks that are due. Idle(): nothing to decode, wait up to a millisecond.
void Service();
void Idle();

// Render thread, at every R_FLAGS record: the two back-end flags of the frame that follows (the presenter keeps them with the frame:
// its pictures are made later, between the decoding of other frames).
void NoteFlags(bool capture, bool skipDraws);

// Smoothed display delay in milliseconds (0 when the presenter is off or shows frames as they come): the picture on the screen is this much
// older than the game's state, so the audio output has to be delayed by it to stay in step with the picture (not done anywhere yet).
double DelayMs();

// Time in milliseconds (the clock the presenter runs on) and the game thread's test knob.
double NowMs();
void Throttle();

// Display with a vertical blank counter (the console: sceDisplayGetVcount): when a source is set, a tick starts at the vblank the counter
// says is due (every 60/hz vblanks), not at a time of the thread's own. The picture made at a tick then appears two vblanks later every
// time (30 Hz), however long the thread's wait loop overshoots; without it the picture of a tick whose work ends just after a vblank
// would appear a vblank later than the others and the display would stutter 16 ms now and then.
using VblankCountFn = uint32_t (*)();
void SetVblankSource(VblankCountFn fn);

// The picture of the previous tick is to be shown again (set by the host that presents: a retained copy of the last picture).
using RepeatHook = void (*)();
void SetRepeatHook(RepeatHook hook);

} // namespace vgx::present
