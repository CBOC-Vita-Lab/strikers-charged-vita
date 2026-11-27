// np_fps_ctl.h: the decision logic of MSC_FPS=auto (W5 FPS). Pure: no clock, no I/O, no game types, so the game's np_fps.cpp and the unit
// test (tools/fps/fps_unit.cpp) run the same code. C++11 (the game's compat flags).
//
// What it decides: whether the presentation policy for the NEXT frame is "30" (a picture every two refreshes at most) or "60" (a picture
// per refresh, a late frame waits for the next one). It is fed once per drawn frame with what the previous frame cost on every core.
//
//   going up   (30 -> 60): the frames of at least UP_HOLD seconds (and UP_FRAMES frames) in a row ALL had a predicted 60 Hz cost of at most
//              UP_MARGIN of a refresh on every core; not before the cool-down of the last down is over; a new scene (task state) or a
//              loading stall starts the evidence again.
//   going down (60 -> 30): at once when a frame misses its refresh during the first PROBATION seconds after going up; later when MISS_LIMIT
//              of the last MISS_WINDOW frames missed, or the busiest core's cost stayed above DOWN_BUSY of a refresh for BUSY_FRAMES
//              frames; or when the frames of the last SLOW_FRAMES lasted more than SLOW_MEAN refreshes on average (the limit is then not on a
//              CPU core: the GPU, whose time the per-core counters do not see; a wait for a peer's input ends in 30 as well, which is harmless).
//              A miss is a frame of more than 1.5 refreshes while a core was busy (a wait for a peer's input is not a miss).
//   the GPU (23 Oct): when the renderer reports gpuMinMs (its predicted GPU time per frame at the smallest picture it would use in this scene,
//              vgx_backend_gl.cpp, measured by vitaGL's GPU timers), going up also needs that under the margin, and it goes down when it stayed
//              above DOWN_BUSY of a refresh for BUSY_FRAMES frames: a scene the GPU cannot draw 60 times a second is not tried, instead of being
//              found out by the sustained-slowness rule after a few slow seconds.
//   no flip-flop: the cool-down after a down is COOL_BASE seconds, doubled for every down in the last 60 s; a down inside the probation
//              (the prediction was too optimistic) also lowers the margin for going up (PENALTY_STEP per such down, up to 3, forgotten
//              after 120 s of stable 60). So a scene that does not hold 60 is tried a few times and then left at 30.
//
// 1.2 (asymmetric hysteresis, after the Halo CE Vita port; Config::V11() has the 1.1 numbers, MSC_FPS_HYST=1): down quickly, up slowly, and a try
// that failed makes the next one wait longer within the scene.
//   down  after DOWN_FRAMES (4) frames in a row over DOWN_BUSY (92 %) of a refresh on a CPU core or the GPU (1.1: 8 frames over 97 %), before a frame
//         has to miss its refresh
//   up    only after UP_HOLD (2 s) of frames that all (UP_OUTLIERS: 10 % of them may not) fit within UP_MARGIN (80 %; 1.1: 1 s, 85 %), after the cool-down
//   retry every down within FAIL_WINDOW (30 s) of the up is a failed try of the scene: the cool-down is COOL_BASE (6 s) doubled for every failed try in
//         this scene (12, 24, 48, 96, at most COOL_MAX 120 s), and the margin loses PENALTY_STEP per failed try; a new scene or 120 s of stable 60
//         forgives one step. (1.1: the cool-down doubled with the downs of the last 60 s.)
//   a picture-size change of the renderer (dynamic resolution) makes a few frames late by itself: FrameIn::dynGrace skips those frames at 60
//   a scene class that stays at 30 once a try failed (replays, celebrations, cut-scenes: FrameIn::sceneClass) is not tried again before the scene ends
//
// 1.2 (MSC_FPS_LADDERHOLD=1; the Vita player build turns it on, vita_plan.cpp): the GPU-limited miss waits for the dynamic resolution. A frame that misses its refresh while no CPU core is near the
//   budget is the GPU's, and the renderer may still have picture steps to give (FrameIn::ladderRoom: its ladder is above the floor it uses at 60, and no step below
//   was found useless). The frame-miss rules (a miss right after going up, too many misses) and the sustained-slowness rule are then held, for LADDER_HOLD_MAX seconds
//   at most, so that the ladder gets to use its rungs before 30 is chosen. A CPU-limited miss (a core over DOWN_BUSY of a refresh in the last 8 frames) goes to 30
//   at once, as does the busiest-core rule and the GPU-at-the-smallest-picture rule (the prediction already includes the rungs).
//
// The prediction of the 60 Hz cost (Predict): per core, the part of the frame's cost that follows the number of frames (frame building, the
// frame send, decoding, drawing) is the measured cost; the part that follows time (the audio mixer, helper threads) is scaled to one refresh;
// on the game core the simulation part (steps x cost per step) is replaced by ONE step: at 60 Hz a frame holds 0.834 steps on average, but five
// frames in six hold one, and that frame has to fit into the refresh (a frame with two steps, after a late one, is the exception).
// Settling: the first frames after a switch up carry the backlog of the 30 fps frames before them (two steps instead of one): they may miss and
// are not judged (SETTLE frames); a system whose one-step frame fits the refresh then settles into 60 by itself.
#ifndef NP_FPS_CTL_H
#define NP_FPS_CTL_H

#include <math.h>
#include <string.h>
#include "np_simpace18.h"

#ifndef NP_P98_BREAK
#define NP_P98_BREAK 0    // host test mutants (tools/fps/p98_unit.cpp; each must FAIL it): 1 a burst falls 60 at once (no sustain), 2 never falls, 3 the hitch rule is ignored, 4 the trial never keeps 60,
                          // 5 the backoff never grows, 6 no trial at a cut, 7 a GPU-bound relief is retried
#endif
namespace npfps {

enum Policy { kPolicyGame = 0, kPolicy30 = 1, kPolicy60 = 2, kPolicyUnlocked = 3 };

static const int kMaxCores = 4;

struct Config {
    double refresh;       // seconds: one VI refresh (16.683 ms NTSC)
    double upMargin;      // fraction of a refresh every core's predicted 60 Hz cost must stay under to go up
    double downBusy;      // fraction of a refresh the busiest core's measured cost may not stay above
    double upHold;        // seconds of good evidence before going up
    int upFrames;         // and at least this many frames
    double coolBase;      // seconds of cool-down after a down
    double coolMax;
    double probation;     // seconds after going up in which one miss sends it down
    int missWindow;       // frames
    int missLimit;        // misses in the window that send it down
    int busyFrames;       // consecutive frames above downBusy that send it down
    double missBusy;      // a long frame counts as a miss only when the busiest core used more than this fraction of a refresh
    double stall;         // refreshes: a frame this long (a loading stall) is neither evidence nor a miss
    double penaltyStep;   // margin lost per failed try
    int settle;           // frames after a switch that are not judged
    int slowFrames;       // window of the sustained-slowness rule
    double slowMean;      // refreshes: the mean frame length over the window that sends it down
    double stepSeconds;   // one simulation step
    // 1.2
    double upOutliers;    // fraction of the evidence window's frames that may be over the margin (below downBusy) without starting it again
    double failWindow;    // seconds after an up in which a down is a failed try of the scene (the retry back-off)
    bool backoffByScene;  // the cool-down doubles with the failed tries of the scene (false: with the downs of the last 60 s, as in 1.1)
    bool ladderHold;      // 1.2: a GPU-limited miss at 60 is held while the renderer's ladder has room (see above)
    double ladderHoldMax; // seconds the hold lasts at most
    bool classHold;       // a scene class (FrameIn::sceneClass 1) is not tried again after a failed try until the scene changes
    // holdgpu24 MSC_LOCK60_STICKY (s29 console: 4-8 switches between 60 and the held 30 every 30 s): once a gameplay scene has been given the full-size 30, 60 comes back only after a long proof at the
    // measured headroom (stickyProof seconds of every core's predicted 60 Hz cost under stickyMargin, with the GPU's smallest-picture prediction under it too), never sooner than stickyMinHold after the
    // relief, the wait doubles with each relief in the last stickyWindow seconds, and stickyLatchAfter reliefs in that window latch the 30 for stickyLatchSecs (a new scene does not force 60 meanwhile).
    bool sticky = false;
    double stickyMinHold = 25.0, stickyProof = 12.0, stickyMargin = 0.75, stickyWindow = 180.0, stickyLatchSecs = 240.0;
    int stickyLatchAfter = 3;
    // pace13 (22 Nov, host model native/tools/fps/tier_flap_unit.cpp; both OFF by default, MSC_TIER_DWELL / MSC_TIER_RELAPSE): Galaxy's governor keeps changes >= 8 s apart and backs off after a relapse.
    //   tierDwell  > 0: a fall from 60 by the SOFT rules (misses in the window, a slow mean) is not taken until this many seconds after the rise; the hard rules (a core or the GPU over budget for
    //              busyFrames pictures) still fall at once
    //   tierRelapse: a fall within failWindow seconds of a rise counts as two reliefs in the sticky history (hold 25 -> 50 s -> the 240 s latch one relapse sooner)
    double tierDwell = 0.0;
    bool tierRelapse = false;
    // hold60 (MSC_LOCK60_HOLD=1, 23 Nov; Config::Lock60Hold, host test native/tools/fps/hold60_unit.cpp): judge 60 by the pictures the player gets and climb on the work a picture costs.
    //   fall  only when the pictures delivered at 60 stay under holdSlowRate a second over holdSlowSecs (a steady 52-58 with repeated refreshes is kept: each one is shown on the vblank its
    //         game time stands for), or under holdFastRate over holdFastSecs (plainly worse than the 45 below); a core or the GPU over budget for busyFrames pictures counts only while the
    //         fast window is under holdSlowRate too. The miss count, the mean-length rule and the probation are not used. After a cut into a scene at 60 (kick-off, play after a goal) the
    //         slow window is not judged for holdGrace seconds: the console (23 Nov) had 20 pictures in the first 0.7 s and about 22 ms pictures for 3 s after every cut into play, then
    //         14 ms (the fast window still catches a scene that does not recover).
    //   climb from the relief when, for holdProof seconds and the last holdProofPics pictures, no more than holdUpOver of them had a per-picture cost over holdUpFit of a refresh, where
    //         the cost is the WORK at 60: the game thread's time less its waits with the simulation re-priced to one step (FrameIn::workMs), the front-end and back-end lanes' CPU
    //         (feMs / beMs) and the GPU at the 60 floor (gpuMinMs); not the frame length (which holds the relief's waits) and not the probe's per-core busy time (which holds
    //         time-following work and spins). Without workMs the old prediction (max60) is used.
    //         hold60b (24 Nov, console s49: the climb cost 16.5-19.8 ms against [gsplit-pic] 1-step work 13.3-14.2): the game term keeps the game thread's blocked wait for the helper lane's
    //         snapshot blend (MSC_BLEND_ASYNC's join, FrameIn::laneWaitMs: 3.0-5.6 ms a picture at 45, 1.7 at 60 in the Mega Strike, 0.6-0.9 at the paced 30; [gsplit-pic] leaves it out as a
    //         wait). It is on the game thread's path at 60 too (the console's 60 has it), so it stays in. The step is priced as the 60 runs it: a picture at 60 carries one 20 ms step
    //         R / 20 ms = 83 % of the time and none otherwise, so a picture counts as over by that share with the step and the rest without it (before: every picture with a full step).
    //   wait  after a fall from a gameplay scene: holdMin seconds, doubled for each failed try (a fall within failWindow of a climb) in stickyWindow, at most holdMax; no latch (60 stays
    //         reachable); a fall inside the cut-in's grace is not a failed try (it is the cut's transient); a new scene does not force 60 while the wait lasts.
    bool hold60 = false;
    bool held30 = false;   // held30 (MSC_LOCK60_HELD30, default 1 with the hold): under LOCKED 60 a scene outside live play (sceneClass 1: walk-out, replay, celebration, cut-scene, menu) starts at 30 and is held there until it ends
    double holdSlowSecs = 5.0, holdSlowRate = 50.0, holdFastSecs = 2.0, holdFastRate = 36.0, holdGrace = 6.0;
    double holdUpFit = 0.97, holdUpOver = 0.06, holdProof = 8.0;   // (hold60b: 8 s, was 6: a climb is never within 8 s of the 30 -> 45 before it)
    int holdProofPics = 240;
    double holdMin = 10.0, holdMax = 120.0;
    // wave 24 (pace24), both default off (MSC_LOCK60_FASTGRACE, MSC_HOLD60_WALLSIM):
    //   holdFastGrace: the 2 s fast window does not fall 60 inside the cut-in's grace unless the delivered rate is under holdPanicRate (a cut's 25-56 ms transient fills the 2 s window; the
    //                  slow window and the busy-core rules were already held back by the grace, this one was not).
    //   holdWallSim:   the climb prices the simulation in the same clock as the frame (wall ms inside the step loop, FrameIn::simWallMs) instead of the thread's CPU ms; a stall inside a step
    //                  (a preempted or blocked game thread) is then part of the step, not of the picture's base. Waits outside the steps (the lane's join, the frame-send join) stay in the base.
#ifndef NP_P24_BREAK
#define NP_P24_BREAK 0   // host test mutants (tools/fps/pace24_unit.cpp; each must FAIL it): 1 the fast grace never lets a collapse fall 60, 2 the fast grace does nothing, 3 the wall sim is priced with the CPU ms,
                         // 4 the 45's delivered-rate rule never speaks, 5 the delivered-rate rule is inverted
#endif
    bool holdFastGrace = false, holdWallSim = false;
    double holdPanicRate = 24.0;
    // wave 24 (p98, MSC_LOCK60_P98, default off; host test tools/fps/p98_unit.cpp): hold 60 on what the player SEES. Over p98Window seconds of live play 60 stays while BOTH hold: (a) repeated refreshes are
    // at most p98RepeatPct % of the refreshes, (b) no more than one visible hitch (a picture held 3+ refreshes, or 3+ repeats within 0.5 s) per p98HitchSecs. It falls only when (a) or (b) has been violated
    // for p98Sustain seconds AND a repeat is still happening (p98Recent seconds): one burst of 2 s does not take 60 away for the rest of the game. The old slow / busy-core rules are not used (the
    // catastrophic fast window, under holdFastRate over holdFastSecs outside the cut-in's grace, stays).
    // p98Trials (MSC_LOCK60_TRIAL): at the relief (45 / 30) in live play, after p98TrialAfter seconds and at every cut back into play, 60 is tried for p98TrialSecs and kept when the trial's repeats are within
    // p98RepeatPct and it had at most one visible hitch; a failed trial doubles the wait (after, 2 x, ... at most p98BackoffMax); the wait resets after p98KeepSecs of kept 60 or a new match; never while the GPU at its
    // smallest picture is over p98GpuFit of a refresh (The Dump).
    bool p98 = false, p98Trials = false;
    // wave 25 flip25. p98Defer (MSC_LOCK60_TRIAL_DEFER, default ON, a controller bug fix; tools/fps/flip25_unit.cpp): DeferUp's "while the 45 / 50 is itself on its relief, 60 is not tried (30 -> 45 -> 60)" is
    // honoured by the p98 trial by time as well (it only set the cool-down, which that trial ignores: the console had a 30 -> 60 trial 0.28 s after a 50 -> 30 fall, jobs 6122 / 6123).
    // p98Calm (MSC_LOCK60_TRIAL_CALM; the Config default stays false for the unit tests, np_fps.cpp's MakeAutoConfig() sets it from the switch, default ON since nxpace25): the trial of 60 BY TIME at the relief is the visible part of the 60 <-> 50 flips (console int9 Vice, 26 trials, 0 kept, 14 of 21 with a transition hitch):
    // it waits p98CalmSettle seconds after a scene change (not 0.2 s into the kick-off), is skipped while the predicted busiest-core cost at 60 is over p98CalmFit of a refresh, and stops after
    // p98CalmFails failed trials in a row in the same scene (the cut back into play still tries 60: a cut hides it).
    bool p98Defer = true, p98Calm = false;
    int p98CalmFails = 2;
    double p98CalmFit = 0.90, p98CalmSettle = 6.0;
    // p98Burst: the one worst burst of this many seconds in the window is forgiven (a 2 s spike is not a reason to leave 60): only what is over the limits WITHOUT it counts
    double p98RepeatPct = 1.5, p98Window = 10.0, p98Sustain = 3.0, p98HitchSecs = 5.0, p98Recent = 3.0, p98Burst = 2.0;
    double p98TrialAfter = 4.0, p98TrialSecs = 2.0, p98TrialSkip = 0.5, p98CutSkip = 3.0, p98BackoffMax = 20.0, p98KeepSecs = 30.0, p98GpuFit = 0.97;
    bool lock60;          // 1.3 MSC_LOCK60: every scene starts at 60 (not 30) and a scene that was given 30 gets 60 again when the scene changes (see Lock60())
    Config()
        : refresh(1001.0 / 60000.0), upMargin(0.80), downBusy(0.92), upHold(2.0), upFrames(40), coolBase(6.0), coolMax(120.0), probation(3.0),
          missWindow(60), missLimit(3), busyFrames(4), missBusy(0.75), stall(8.0), penaltyStep(0.06), settle(8), slowFrames(30), slowMean(1.3), stepSeconds(0.02),
          upOutliers(0.10), failWindow(30.0), backoffByScene(true), ladderHold(false), ladderHoldMax(8.0), classHold(true), lock60(false) {}
    // 1.3 MSC_LOCK60 (docs/TECHNICAL.md "LOCKED 60"): the controller of the LOCKED 60 picture mode. 60 is the rule and 30 the relief, the reverse of AUTO's start:
    //   - a session and every new scene start at 60 (settled for 8 frames, no probation: a load must not count as a failed try); the renderer's scene presets put the picture
    //     size where the budget table says it fits, so the controller is not left to discover it by missing frames
    //   - 30 is only the relief: the ladder has no step left (ladderRoom false, which the renderer reports once the floor of the scene is reached) and frames still miss
    //     (3 of 60, or the frames last over 1.25 refreshes on average over 30), or a core really is over its budget (98 % for 8 frames: AUTO's 92 % for 4 would call a
    //     core that still makes its refresh "over"). The hold for a ladder with room is 2 s, not 8: the ladder's rungs are made ahead and act at once
    //   - a relief in a gameplay scene is tried again after 4 s (doubled for each failed try of the scene, at most 32 s) on evidence at 85 % of a refresh for 1.5 s; in any
    //     other scene (replay, celebration, cut-scene, menus: FrameIn::sceneClass 1) it lasts until the scene ends: no flip-flop inside a short scene
    static Config Lock60() {
        Config c;
        c.lock60 = true;
        c.ladderHold = true; c.ladderHoldMax = 2.0;
        c.downBusy = 0.98; c.busyFrames = 8; c.missLimit = 3; c.slowMean = 1.25;
        c.upMargin = 0.85; c.upHold = 1.5; c.upFrames = 60; c.coolBase = 4.0; c.coolMax = 32.0; c.failWindow = 20.0; c.settle = 8;
        c.sticky = true;
        return c;
    }
    // 1.3 MSC_LOCK60_TIERS (Fable, owner 18 Nov: "it should defend 60 ... some allowance down to like a low 50 fps before we jump"): Smooth 60 with tiers, 60 -> the strict 45 cadence
    // (np_fps45.h, np_fps_tier.h) -> the full-size 30. The relief of this controller is then cheap (45, not a halving), but 60 is still defended: a short burst of repeated refreshes (a
    // goal, an effect burst, the 2-step picture after a miss) must not trip it, a steady 55 pictures a second or less must.
    //   - misses: 12 of the last 180 pictures (3 s; a steady 56 a second or less trips it in 2.4 to 3 s; an isolated burst of up to 11 repeated refreshes does not), where Lock60 has 3 of 60
    //   - mean length: over 1.25 refreshes across 60 pictures (a second at 48 a second or less: 45 is plainly better then), where Lock60 judges 30 pictures (a burst of 8 trips it)
    //   - a core over 98 % of a refresh for 45 pictures (0.75 s) in a row, where Lock60 has 8 (133 ms: any spike)
    //   - no probation (Lock60: one miss within 3 s of going up sends it down): the same windows judge a fresh 60
    // The way back up (sticky hold, 12 s proof at 75 %, latch) is Lock60's: the tiers add the 45 below, they do not make 60 easier to re-enter.
#ifndef NP_TMEM_BREAK
#define NP_TMEM_BREAK 0   // (tools/fps/tiermem_unit.cpp mutants; np_fps_tiermem.h defines the same macro)
#endif
#ifndef NP_FLAP_BREAK
#define NP_FLAP_BREAK 0   // host test mutants (tools/fps/tier_flap_unit.cpp): 1 the dwell is ignored, 2 the relapse backoff never counts
#endif
#ifndef NP_TIER_BREAK
#define NP_TIER_BREAK 0   // host test mutants (tools/fps/tier_unit.cpp; each must fail it): 1 falls on a spike (Lock60's own windows), 2 never falls, 3 flaps (no sticky hold),
                          // 4 skips 45 (np_fps_tier.h: the relief is the 30)
#endif
    static Config Lock60Tiered() {
        Config c = Lock60();
#if NP_TIER_BREAK != 1
        c.missWindow = 180; c.missLimit = 12; c.slowFrames = 60; c.slowMean = 1.25; c.busyFrames = 45; c.probation = 0.0;
#endif
#if NP_TIER_BREAK == 2
        c.missLimit = 1 << 30; c.slowMean = 1e9; c.busyFrames = 1 << 30;
#elif NP_TIER_BREAK == 3
        c.sticky = false;
#endif
        return c;
    }
    // hold60 MSC_LOCK60_HOLD=1 (owner 23 Nov: "getting to 60 smooth should be the goal"; "if a game is close, say 54fps, there's a smooth loss of a frame ... rather than drop all the way
    // back to 30"): the tiers with the fall judged on the delivered rate and the climb on measured work (see Config::hold60). The 45 below takes Fallback45Config::Hold().
#ifndef NP_HOLD_BREAK
#define NP_HOLD_BREAK 0   // host test mutants (tools/fps/hold60_unit.cpp; each must FAIL it): 1 never falls, 2 falls on a spike (0.3 s window at 58 a second, no grace), 3 flaps (no wait, a 0.5 s proof),
                          // 4 never climbs, 5 climbs on the frame length (the relief's waits counted as cost), 6 the 45 keeps the old fast fallback window (8 % of 90), 7 the 30 never climbs,
                          // 8 (hold60b) every picture priced with a full step (not the 60's 83 %), 9 (hold60b) the climb cost leaves out the blend lane's wait (prices the work as [gsplit-pic] does)
#endif
    static Config Lock60Hold() {
        Config c = Lock60Tiered();
        c.hold60 = true;
#if NP_HOLD_BREAK == 1
        c.holdSlowRate = 0.0; c.holdFastRate = 0.0;
#elif NP_HOLD_BREAK == 2
        c.holdSlowSecs = 0.3; c.holdSlowRate = 58.0; c.holdGrace = 0.0;
#elif NP_HOLD_BREAK == 3
        c.holdMin = 0.0; c.holdMax = 0.0; c.holdProof = 0.5; c.holdProofPics = 10; c.holdUpOver = 0.5;
#elif NP_HOLD_BREAK == 4
        c.holdUpOver = -1.0;
#endif
        return c;
    }
    // The 1.1 numbers (MSC_FPS_HYST=1; the tests of the 1.1 controller)
    static Config V11() {
        Config c;
        c.upMargin = 0.85; c.downBusy = 0.97; c.upHold = 1.0; c.upFrames = 20; c.coolBase = 4.0; c.coolMax = 32.0; c.busyFrames = 8;
        c.upOutliers = 0.0; c.failWindow = 10.0; c.backoffByScene = false; c.classHold = false; c.ladderHold = false;
        return c;
    }
};

struct FrameIn {
    double now;            // seconds on any monotonic clock
    double interval;       // seconds the previous frame lasted (BeginFrame to BeginFrame)
    int steps;             // simulation steps it ran
    double simMs;          // game-thread time inside the fixed-update task
    bool costsValid;       // cpuMs / timeMs are measurements
    int gameCore;          // the core of the game thread
    double cpuMs[kMaxCores];   // busy milliseconds per core in that frame (all kinds)
    double timeMs[kMaxCores];  // the part of cpuMs that follows time, not frames
    double gameBusyMs;     // without a probe: the game thread's busy time (interval minus waits), the only figure there is
    bool locked;           // the game forced its own 30 fps cut-scene lock in that frame
    unsigned scene;        // scene identity (task state): a change starts a new evidence window
    double gpuMinMs;       // the renderer's predicted GPU ms per frame at its smallest picture for this scene; <= 0: not known (the rule is off)
    int sceneClass;        // 1.2: 0 ordinary; 1 a scene that is not tried again at 60 once a try failed (replay, celebration, cut-scene)
    bool ladderRoom;       // 1.2: the renderer's dynamic resolution can still go down at 60 (its picture is above the floor it uses there, and no step below was found useless)
    bool dynGrace;         // 1.2: the renderer changed its picture size a few frames ago (the frames around a resize are late by themselves)
    // pace18 (np_simpace18.h): the simulation step on its own thread (MSC_SIMT). All 0 / -1 when it is not: the prediction is then exactly the old one.
    int simMode = 0;           // 0 the step is on the game thread, 1 np_sim and the game thread waits, 2 np_sim while the game thread draws the window
    bool simOnGameCore = true; // mode 1: np_sim shares the game thread's core (its CPU is in the game core's counter)
    int simCore = -1;          // mode 2: the core np_sim sits on (one core); -1 not known
    double windowMs = 0;       // mode 2: the game thread's window this picture (sum over its kicks), wall ms
    double joinMs = 0;         // modes 1, 2: the game thread's wait at the join this picture, wall ms
    double simWallMs = 0;      // modes 1, 2: np_sim's wall ms for the step this picture
    // hold60 (Config::hold60): the picture's measured work. <= 0: not known (the climb then uses the prediction max60)
    double workMs = 0;         // the game thread's time in the picture less its waits (frame length minus the counted waits), wall ms; holds the picture's simMs
    double feMs = 0, beMs = 0; // the renderer's front-end and back-end lanes, CPU ms a picture (np_late_cause.h LaneRing::Recent); 0: not known
    double stepWallMs = 0;     // pace24 (Config::holdWallSim): wall ms the game thread spent inside the fixed-update steps this picture (np_fps.cpp NpFpsSimBegin/End); 0: not known (simMs is used)
    bool p98 = false;          // p98: the picture is live single-player play (not LAN / online / recorded): the delivered-rate rule and the trials apply; false: the rules as before
    double laneWaitMs = 0;     // hold60b: the part of workMs the game thread spent blocked on the helper lane (the async snapshot blend's join, np_framesend.cpp NpLaneWait); 0: none / not known
};

#ifndef NP_FREECOST_BREAK
#define NP_FREECOST_BREAK 0   // host test mutants (tools/fps/free_cost_unit.cpp): 1 the cost at the frame rate in force (no re-pricing), 2 the per-step part left out, 3 steps a picture fixed at the measured count
#endif
// Wave 10 (pacing11): the busiest core's cost a picture would have at the FREE-RUN rate. The game core's frame holds perPicture + steps x step ms; at a held 30 a picture carries 1.67 steps, at a free
// run of about 50 pictures a second 1.0, so the cost measured at 30 overstates the free run's by 0.67 steps (3.3 ms on the console: 21.7 ms at 30 against 18.4 ms free, Vice, PERFORMANCE mode).
// Solves P = perPicture + step x (P / stepMs_fixed) for the picture period P (the simulation runs 50 steps a second whatever the picture rate), at least one step a picture.
inline double FreeRateCostMs(double perPictureMs, double stepMs, double fixedStepMs, double measuredMs, double measuredSteps) {
#if NP_FREECOST_BREAK == 1
    (void)perPictureMs; (void)stepMs; (void)fixedStepMs; (void)measuredSteps;
    return measuredMs;
#else
    const double k = stepMs / fixedStepMs;   // ms of step work per ms of picture period
    double P = k < 0.9 ? perPictureMs / (1.0 - k) : perPictureMs + stepMs;
#if NP_FREECOST_BREAK == 2
    P = perPictureMs;
#elif NP_FREECOST_BREAK == 3
    P = perPictureMs + stepMs * measuredSteps;
#endif
    const double floorMs = perPictureMs + stepMs;   // (one step a picture at least: a picture period under a step is the 60 Hz case Predict already prices)
    if (P < floorMs) P = floorMs;
    const double cap = perPictureMs + stepMs * 2.0;
    if (P > cap) P = cap;
    (void)measuredMs;
    return P;
#endif
}

struct Prediction {
    bool valid;
    double max60;          // the busiest core's predicted cost at 60 Hz, ms
    double maxFree;        // wave 10: ... at the free-run rate (FreeRateCostMs), ms; 0 when not known
    double maxNow;         // the busiest core's measured cost, ms
    int core;              // which core
};

struct Decision {
    Policy policy;
    bool switched;
    const char* reason;
};

class Auto {
public:
    explicit Auto(const Config& c = Config()) : cfg_(c) { Reset(); }

    void Reset() {
        cur_ = cfg_.lock60 ? kPolicy60 : kPolicy30;
        goodSince_ = -1;
        goodFrames_ = 0;
        scene_ = 0xFFFFFFFFu;
        cooldownUntil_ = 0;
        upAt_ = -1e9;
        penalty_ = 0;
        stableSince_ = -1;
        memset(miss_, 0, sizeof miss_);
        missPos_ = 0;
        missCount_ = 0;
        busyHigh_ = 0;
        gpuHigh_ = 0;
        holdSince_ = -1;
        holdLast_ = -1;
        cpuHighAge_ = 1000;
        nDowns_ = 0;
        stepCost_ = 0.0; stepWallCost_ = 0.0;
        switches_ = 0;
        settle_ = 0;
        slowPos_ = slowCount_ = 0;
        slowSum_ = 0.0;
        slowFails_ = 0;
        sceneFails_ = 0;
        classHeld_ = false;
        windowFrames_ = 0;
        outliers_ = 0;
        lastPred_.valid = false;
        lastPred_.max60 = lastPred_.maxNow = 0;
        lastPred_.core = 0;
        sp_.Reset();
        nRelief_ = 0;
        stickyUntil_ = 0;
        stickyOn_ = false;
        hRateN_ = hRatePos_ = 0;
        hEnter_ = hGraceUntil_ = 0;
        hProofN_ = hProofPos_ = hProofOver_ = 0;
        nHFail_ = 0;
        P98NewMatch();
        p98N_ = p98Pos_ = 0; p98Viol_ = 0.0; p98LastRep_ = -1e9; nPr_ = 0; nHit_ = 0;
        p98Trials_ = p98Ok_ = p98Fail_ = p98Falls_ = 0; p98Why_ = "";
    }
    // p98: a new match: the trial backoff starts again
    void P98NewMatch() { p98Backoff_ = 0.0; p98NextTrial_ = 0.0; p98TrialOn_ = false; p98KeptAt_ = -1e9; p98ConsecFail_ = 0; p98DeferUntil_ = -1e9; }
    unsigned P98CalmSkips() const { return p98CalmSkips_; }
    unsigned P98DeferSkips() const { return p98DeferSkips_; }
    struct P98Stats { double onSlot, repeatPct, backoff; int hitches, window; unsigned trials, ok, fail, falls; bool trialOn; };
    // p98: the delivered picture over the window (at `now`), for the [p98] line
    P98Stats P98Take(double now) const {
        P98Stats t; double span = 0; int hit = 0; const int rep = P98Window(now, &span, &hit);
        const double slots = span / cfg_.refresh;
        t.onSlot = slots > 1.0 ? 100.0 * (1.0 - rep / slots) : 100.0; t.repeatPct = 100.0 - t.onSlot; t.hitches = hit; t.window = (int)span;
        t.backoff = p98Backoff_ > 0 ? p98Backoff_ : cfg_.p98TrialAfter; t.trials = p98Trials_; t.ok = p98Ok_; t.fail = p98Fail_; t.falls = p98Falls_; t.trialOn = p98TrialOn_;
        return t;
    }
    const char* P98Why() const { return p98Why_; }
    unsigned P98GpuSkips() const { return p98GpuSkips_; }

    Policy Current() const { return cur_; }
    int Switches() const { return switches_; }
    int Penalty() const { return penalty_; }
    int SceneFails() const { return sceneFails_; }
    const Prediction& LastPrediction() const { return lastPred_; }
    double CooldownUntil() const { return cooldownUntil_; }
    // pace13 MSC_TIER_MEMORY (np_fps_tiermem.h): a match starts at the relief (the 45 tier) as if 60 had just failed once, without the failure's bookkeeping (no relief is recorded, so the latch is
    // not nearer): the sticky hold `secs` and the 12 s proof apply before 60 is tried again. Only with the sticky rules (Smooth 60).
    void SeedRelief(double now, double secs) {
        if (!cfg_.lock60 || !cfg_.sticky) return;
        if (cur_ != kPolicy30) Go(kPolicy30, now);
        stickyOn_ = true;
#if NP_TMEM_BREAK == 4
        stickyUntil_ = now + 1e9;   // (mutant: the way up is shut for good)
#else
        stickyUntil_ = now + secs;
#endif
        stickyWait_ = secs;
        if (cooldownUntil_ < now + secs) cooldownUntil_ = now + secs;
        stableSince_ = -1;
        ResetEvidence();
        ++seeds_;
    }
    unsigned Seeds() const { return seeds_; }
    unsigned DwellHeld() const { return dwellHeld_; }     // pictures whose soft fall verdict the dwell held back (counter)
    unsigned Relapses() const { return relapses_; }       // falls within failWindow of a rise that were counted twice
    // MSC_LOCK60_TIERS: while the tier below (45) is itself on its relief (the full-size 30), 60 is not tried: the way up is 30 -> 45 -> 60
    void DeferUp(double now, double secs) {
        if (cooldownUntil_ < now + secs) cooldownUntil_ = now + secs;
        if (p98DeferUntil_ < now + secs) p98DeferUntil_ = now + secs;
        if (cfg_.hold60) ResetEvidence();   // hold60: the climb's proof is made at the 45 itself, after it came back (no 30 -> 45 -> 60 within seconds)
    }
    // hold60: pictures a second delivered at 60 over the last `secs` (0 while not at 60), the last climb cost, the proof window's pictures and how many were over
    double HoldRate(double now, double secs) const {
        if (secs <= 0) return 0;
        int n = 0;
        for (int i = 0; i < hRateN_; ++i) {
            const double t = hRate_[(hRatePos_ - 1 - i + kRateRing) % kRateRing];
            if (now - t >= secs) break;
            ++n;
        }
        return n / secs;
    }
    double HoldUpCost() const { return lastUp_; }
    int HoldProofN() const { return hProofN_; }
    double HoldProofOver() const { return hProofOver_ / (double)kOverUnit; }   // (pictures, fractional: a picture over only with its step counts by the 60's step share)
    // hold60b: what the climb cost was made of (pictures judged for the climb since the last take), and what the 60 measured (pictures at 60 since the last take). Logging and tests.
    struct HoldParts {
        int n = 0;
        double game = 0, lane = 0, fe = 0, be = 0, gpu = 0, gpuMax = 0, over = 0;   // sums of the game term (with its step), its lane wait, the lanes, the GPU at the floor; over: pictures (fractional)
        int by[5] = {};   // pictures over with the step, by the input that set the cost: 0 game, 1 game only because of the lane wait, 2 front end, 3 back end, 4 GPU at the floor
    };
    struct At60 { int n = 0, late = 0, over = 0; double work = 0, lane = 0; };   // at 60: pictures, held 2+ refreshes, work over holdUpFit; sums of the work (frame less waits) and the lane wait
    HoldParts HoldTakeParts() { const HoldParts h = hParts_; hParts_ = HoldParts(); return h; }
    At60 HoldTakeAt60() { const At60 a = hAt60_; hAt60_ = At60(); return a; }
    double HoldWait() const { return stickyWait_; }
    unsigned HoldFails() const { return hFails_; }

    Prediction Predict(const FrameIn& f) {
        Prediction p;
        p.valid = false;
        p.max60 = 0;
        p.maxFree = 0;
        p.maxNow = 0;
        p.core = 0;
        const double R = cfg_.refresh;
        const double stepsPerRefresh = ceil(R / cfg_.stepSeconds - 1e-9);   // the frame with the most steps that a refresh normally holds
        if (f.steps > 0 && f.simMs > 0) {
            const double c = f.simMs / f.steps;
            stepCost_ = stepCost_ > 0 ? 0.8 * stepCost_ + 0.2 * c : c;   // ms per simulation step, smoothed (a frame without steps still has a price)
            if (f.stepWallMs >= f.simMs) { const double w = f.stepWallMs / f.steps; stepWallCost_ = stepWallCost_ > 0 ? 0.8 * stepWallCost_ + 0.2 * w : w; }   // pace24: the same step in wall ms
        }
        const bool simOn = f.simMode == npsimpace::kOverlap || (f.simMode == npsimpace::kBlock && !f.simOnGameCore);   // pace18: the step is not in the game core's counter
        if (f.simMode == npsimpace::kOverlap && f.windowMs + f.joinMs + f.simWallMs > 0) sp_.Observe(f.windowMs, f.joinMs, f.simWallMs);
        const double step60 = stepCost_ * stepsPerRefresh;
        if (f.costsValid) {
            p.valid = true;
            for (int c = 0; c < kMaxCores; ++c) {
                double now = f.cpuMs[c];
                double pred = now - f.timeMs[c] + f.timeMs[c] * (f.interval > 0 ? (R / f.interval) : 1.0);
                if (c == f.gameCore) {
                    pred = npsimpace::GameCore60(f.simMode, f.simOnGameCore, pred, f.simMs, step60, f.windowMs, sp_);
                } else if (f.simMode == npsimpace::kOverlap && c == f.simCore) {
                    pred = pred - f.simMs + step60;   // the core np_sim sits on carries the step (re-priced to the display rate like the game core's)
                    if (pred < 0) pred = 0;
                }
                {   // wave 10: the cost at the free-run rate
                    double perPic = now - f.timeMs[c] + f.timeMs[c] * (f.interval > 0 ? (R / f.interval) : 1.0);
                    double fr = perPic;
                    if (c == f.gameCore) {
                        if (f.simMode == npsimpace::kOverlap) {
                            double base = perPic - f.windowMs;
                            if (base < 0) base = 0;
                            fr = stepCost_ > 0 ? npsimpace::FreeRateOverlapMs(base, sp_.Have() ? sp_.WindowMs() : f.windowMs, stepCost_, cfg_.stepSeconds * 1000.0, sp_) : pred;
                        } else {
                            if (!simOn) perPic -= f.simMs;
                            if (perPic < 0) perPic = 0;
                            fr = stepCost_ > 0 ? FreeRateCostMs(perPic, stepCost_, cfg_.stepSeconds * 1000.0, now, f.steps) : pred;
                        }
                    } else if (f.simMode == npsimpace::kOverlap && c == f.simCore) {
                        perPic -= f.simMs;
                        if (perPic < 0) perPic = 0;
                        fr = stepCost_ > 0 ? FreeRateCostMs(perPic, stepCost_, cfg_.stepSeconds * 1000.0, now, f.steps) : pred;
                    }
                    if (fr > p.maxFree) p.maxFree = fr;
                }
                if (pred > p.max60) { p.max60 = pred; p.core = c; }
                if (now > p.maxNow) p.maxNow = now;
            }
        } else {
            // Only the game thread's busy time is known (a build without a probe): its unthrottled time with the simulation part re-priced.
            p.valid = f.gameBusyMs > 0;
            p.maxNow = f.gameBusyMs;
            double pred = npsimpace::GameBusy60(f.simMode, f.simOnGameCore, f.gameBusyMs, f.simMs, step60, f.windowMs, f.joinMs, sp_);
            p.max60 = pred < 0 ? 0 : pred;
            if (f.simMode == npsimpace::kOverlap) {
                double base = f.gameBusyMs - f.joinMs - f.windowMs;
                if (base < 0) base = 0;
                p.maxFree = stepCost_ > 0 ? npsimpace::FreeRateOverlapMs(base, sp_.Have() ? sp_.WindowMs() : f.windowMs, stepCost_, cfg_.stepSeconds * 1000.0, sp_) : p.max60;
            } else {
                double pp = f.simMode == npsimpace::kBlock && !f.simOnGameCore ? f.gameBusyMs - f.joinMs : f.gameBusyMs - f.simMs;
                if (pp < 0) pp = 0;
                p.maxFree = stepCost_ > 0 ? FreeRateCostMs(pp, stepCost_, cfg_.stepSeconds * 1000.0, f.gameBusyMs, f.steps) : p.max60;
            }
            p.core = f.gameCore;
        }
        return p;
    }

    Decision Frame(const FrameIn& f) {
        Decision d;
        d.policy = cur_;
        d.switched = false;
        d.reason = "";
        const double R = cfg_.refresh;
        const double Rms = R * 1000.0;
        if (p98TrialOn_ && (!f.p98 || cur_ != kPolicy60)) p98TrialOn_ = false;   // (a trial ends with live single-player play, and with the 60 it tests)
        if (f.locked) {
            // the game's own cut-scene lock: no evidence either way
            ResetEvidence();
            if (cfg_.hold60) HoldRestartRate(f.now);
            return d;
        }
        if (f.scene != scene_) {
            scene_ = f.scene;
            sceneAt_ = f.now;
            p98ConsecFail_ = 0;
            ResetEvidence();
            // a new scene is a new case: the GPU-bound back-off of the last one does not follow it (the ordinary cool-down does)
            if (slowFails_ > 0) {
                slowFails_ = 0;
                if (cooldownUntil_ > f.now + cfg_.coolBase) cooldownUntil_ = f.now + cfg_.coolBase;
            }
            if (cfg_.backoffByScene && sceneFails_ > 0) {
                // (1.2) the retry back-off belongs to the scene as well; the ordinary cool-down after the last down stays
                sceneFails_ = 0;
                if (cooldownUntil_ > f.now + cfg_.coolBase) cooldownUntil_ = f.now + cfg_.coolBase;
            }
            if (classHeld_) {
                classHeld_ = false;
                if (cooldownUntil_ > f.now + cfg_.coolBase) cooldownUntil_ = f.now + cfg_.coolBase;
            }
            if (cfg_.lock60) {
                // LOCKED 60: a new scene is 60 again, whatever the last one was given (sticky: not while the full-size 30 is held after a relief)
                bool stuck = cfg_.sticky && StickyBreak() != 4 && cur_ == kPolicy30 && f.now < stickyUntil_;
                // p98 trials: a cut back into live play from the relief is a trial of 60 (judged after the cut's transient), unless the GPU is the limit (The Dump): then the relief stays
                bool cutTrial = false;
                if (cfg_.hold60 && cfg_.p98 && cfg_.p98Trials && f.p98 && f.sceneClass == 0 && cur_ == kPolicy30 && NP_P98_BREAK != 6) {
                    if (P98GpuOk(f, Rms)) { cutTrial = true; stuck = false; }
                    else { stuck = true; ++p98GpuSkips_; }
                }
                if (!stuck) { cooldownUntil_ = 0; stickyOn_ = false; }
                sceneFails_ = 0;
                if (cfg_.held30 && f.sceneClass == 1) {
                    p98TrialOn_ = false;
                    // held30: the console (s49 5141, 23 Nov) had every walk-out, Mega Strike scene and menu start at 60 and fall after the 2 s fast window (97 % late,
                    // judder 1.38): the owner wants those at a steady 30, so they start there and stay (classHeld_: back to the 60 at the next scene)
                    if (cur_ != kPolicy30) {
                        Go(kPolicy30, f.now);
                        d.policy = cur_;
                        d.switched = true;
                        d.reason = "LOCKED 60: a scene outside live play starts at 30 and stays (MSC_LOCK60_HELD30)";
                    }
                    classHeld_ = true;
                    cooldownUntil_ = 1e9;
                    upAt_ = -1e9;
                    return d;
                }
                if (cur_ != kPolicy60 && !stuck) {
                    Go(kPolicy60, f.now);
                    d.policy = cur_;
                    d.switched = true;
                    d.reason = "LOCKED 60: a new scene starts at 60";
                    if (cutTrial) { p98TrialOn_ = true; p98TrialEnd_ = f.now + cfg_.p98CutSkip + cfg_.p98TrialSecs; p98TrialFrom_ = f.now + cfg_.p98CutSkip; p98TrRep_ = p98TrSlots_ = p98TrHit_ = 0; p98TrCut_ = true; ++p98Trials_; d.reason = "p98: a trial of 60 at the cut back into play"; }
                }
                upAt_ = -1e9;
                if (cfg_.hold60) {
                    HoldRestartRate(f.now);   // (a new scene: its own rate window)
                    if (cur_ == kPolicy60) hGraceUntil_ = f.now + cfg_.holdGrace;   // (a fall inside the cut-in's grace is the cut-in's transient, not a failed try: a short wait, no doubling)
                }
                settle_ = cfg_.settle;
                for (int i = 0; i < cfg_.missWindow && i < kMaxWindow; ++i) miss_[i] = 0;
                missCount_ = 0;
                slowPos_ = slowCount_ = 0;
                slowSum_ = 0.0;
                busyHigh_ = gpuHigh_ = 0;
                if (stableSince_ < 0) stableSince_ = f.now;
                if (d.switched) return d;
            }
        }
        const Prediction p = Predict(f);
        lastPred_ = p;
        const bool stall = f.interval > cfg_.stall * R;
        if (cur_ == kPolicy30 && cfg_.hold60) {
            // hold60: the climb on the measured work of the last pictures (sliding window: a lone heavy picture does not start the proof again)
            if (stall || !p.valid) { ResetEvidence(); return d; }
            if (cfg_.p98 && cfg_.p98Trials && f.p98 && f.sceneClass == 0 && f.now >= p98NextTrial_ && P98GpuOk(f, Rms) && P98TimeTrialOk(f, p, Rms)) {   // p98: the trial of 60 by time
                P98StartTrial(f, d, cfg_.p98TrialSkip, "p98: a trial of 60 (the wait after the last fall is over)");
                return d;
            }
            const UpParts u = UpCostParts(f, p);
            lastUp_ = u.with;
            const int need = cfg_.holdProofPics < 1 ? 1 : (cfg_.holdProofPics < kMaxWindow ? cfg_.holdProofPics : kMaxWindow);
            if (goodSince_ < 0) { goodSince_ = f.now; goodFrames_ = 0; hProofN_ = hProofPos_ = hProofOver_ = 0; }
            const double lim = cfg_.holdUpFit * Rms;
            const unsigned char over = OverUnits(u, lim, R);
            NoteParts(u, f, lim, over);
            if (hProofN_ == need) hProofOver_ -= hProof_[hProofPos_]; else ++hProofN_;
            hProof_[hProofPos_] = over;
            hProofOver_ += over;
            hProofPos_ = (hProofPos_ + 1) % need;
            ++goodFrames_;
            if (hProofN_ == need && f.now - goodSince_ >= cfg_.holdProof && (double)hProofOver_ <= cfg_.holdUpOver * need * kOverUnit && f.now >= cooldownUntil_ &&
                !(stickyOn_ && f.now < stickyUntil_)) {
                Go(kPolicy60, f.now);
                d.policy = cur_;
                d.switched = true;
                d.reason = "hold60: the work of the last pictures fits a refresh (game thread less waits, the blend lane's wait kept, with the 60's step share; front end, back end, GPU at the floor)";
                upAt_ = f.now;
                hGraceUntil_ = 0;   // (a climb has no grace: its own proof said it fits)
                settle_ = cfg_.settle;
                for (int i = 0; i < cfg_.missWindow && i < kMaxWindow; ++i) miss_[i] = 0;
                missCount_ = 0;
                slowPos_ = slowCount_ = 0;
                slowSum_ = 0.0;
                busyHigh_ = 0;
                gpuHigh_ = 0;
                if (stableSince_ < 0) stableSince_ = f.now;
            }
            return d;
        }
        if (cur_ == kPolicy30) {
            if (stall || !p.valid) { ResetEvidence(); return d; }
            const double margin = Margin();
            // a frame over the margin starts the evidence again; 1.2: a few of them (UP_OUTLIERS) are tolerated as long as none is near the
            // down threshold (a single spike must not postpone 60 for ever, a load that really hovers at the margin must)
            const bool overMargin = p.max60 > margin * Rms || (f.gpuMinMs > 0 && f.gpuMinMs > margin * Rms);   // (the GPU would not hold a refresh even at its smallest picture)
            if (overMargin) {
                const bool tolerated = cfg_.upOutliers > 0 && goodSince_ >= 0 && p.max60 <= cfg_.downBusy * Rms &&
                                       !(f.gpuMinMs > 0 && f.gpuMinMs > cfg_.downBusy * Rms) &&
                                       (double)(outliers_ + 1) <= cfg_.upOutliers * (double)(windowFrames_ + 1);
                if (!tolerated) { ResetEvidence(); return d; }
                ++outliers_;
                ++windowFrames_;
                return d;
            }
            if (goodSince_ < 0) { goodSince_ = f.now; goodFrames_ = 0; windowFrames_ = 0; outliers_ = 0; }
            ++goodFrames_;
            ++windowFrames_;
            const double proof = stickyOn_ && StickyBreak() != 2 ? cfg_.stickyProof : cfg_.upHold;
            const int proofFrames = stickyOn_ && StickyBreak() != 2 ? (int)(cfg_.stickyProof * 25.0) : cfg_.upFrames;   // (about 25 pictures a second at a held 30)
            if (f.now - goodSince_ >= proof && goodFrames_ >= proofFrames && f.now >= cooldownUntil_ && !(stickyOn_ && f.now < stickyUntil_)) {
                Go(kPolicy60, f.now);
                d.policy = cur_;
                d.switched = true;
                d.reason = "every core predicted under the budget for the whole hold time";
                upAt_ = f.now;
                settle_ = cfg_.settle;
                for (int i = 0; i < cfg_.missWindow && i < kMaxWindow; ++i) miss_[i] = 0;
                missCount_ = 0;
                slowPos_ = slowCount_ = 0;
                slowSum_ = 0.0;
                busyHigh_ = 0;
                gpuHigh_ = 0;
                if (stableSince_ < 0) stableSince_ = f.now;
            }
            return d;
        }
        if (cur_ == kPolicy60) {
            if (stall) { if (cfg_.hold60) HoldRestartRate(f.now); return d; }   // (hold60: a loading stall is not a slow second)
            if (cfg_.hold60) HoldPush(f.now);           // every picture at 60 counts for the delivered rate, the settling ones too
            if (cfg_.hold60 && cfg_.p98 && f.p98) { P98Push(f); if (p98TrialOn_) { const Decision td = P98TrialStep(f, d); if (td.switched) return td; } }
            if (cfg_.hold60 && f.workMs > 0) {          // hold60b: what a picture at 60 measured (the climb cost's check on the console)
                ++hAt60_.n;
                hAt60_.work += f.workMs;
                hAt60_.lane += f.laneWaitMs;
                if (f.interval > 1.5 * R) ++hAt60_.late;
                if (f.workMs > cfg_.holdUpFit * Rms) ++hAt60_.over;
            }
            if (settle_ > 0) { --settle_; return d; }   // the backlog of the 30 fps frames before: not judged
            if (f.dynGrace) return d;                   // 1.2: the frames around a picture-size change of the renderer: not judged (it steps down by itself)
            // forgive the penalty after a long stable stretch
            if ((penalty_ > 0 || slowFails_ > 0 || sceneFails_ > 0) && stableSince_ >= 0 && f.now - stableSince_ >= 120.0) {
                if (penalty_ > 0) --penalty_;
                if (slowFails_ > 0) --slowFails_;
                if (sceneFails_ > 0) --sceneFails_;
                stableSince_ = f.now;
            }
            const bool longFrame = f.interval > 1.5 * R;
            const bool busy = !f.costsValid ? true : (p.maxNow > cfg_.missBusy * Rms);
            const bool miss = longFrame && busy;
            const int missWin = cfg_.missWindow > 0 ? cfg_.missWindow : 1;   // (a window of 0 would divide by zero: 0 on the console, a crash on the host)
            const int idx = missPos_ % missWin;
            if (miss_[idx]) --missCount_;
            miss_[idx] = miss ? 1 : 0;
            if (miss) ++missCount_;
            missPos_ = (missPos_ + 1) % missWin;
            if (p.valid && p.maxNow > cfg_.downBusy * Rms) { ++busyHigh_; cpuHighAge_ = 0; } else { busyHigh_ = 0; if (cpuHighAge_ < 1000) ++cpuHighAge_; }
            if (f.gpuMinMs > 0 && f.gpuMinMs > cfg_.downBusy * Rms) ++gpuHigh_; else gpuHigh_ = 0;
            const char* why = 0;
            bool failedTry = false, slowTry = false;
            if (cfg_.hold60) {
                // hold60: the delivered rate decides; a core or the GPU over budget only while the pictures do not come either
                const double age = f.now - hEnter_;
                const double fast = HoldRate(f.now, cfg_.holdFastSecs), slow = HoldRate(f.now, cfg_.holdSlowSecs);
                const bool inGrace = f.now < hGraceUntil_;
                // pace24 (Config::holdFastGrace): inside the cut-in's grace the fast window speaks only for a collapse (under holdPanicRate), not for the cut's own transient
                const bool fastFull = age >= cfg_.holdFastSecs, slowFull = age >= cfg_.holdSlowSecs && !inGrace;
#if NP_P24_BREAK == 1
                const double fastLimit = cfg_.holdFastGrace && inGrace ? 0.0 : cfg_.holdFastRate;
#elif NP_P24_BREAK == 2
                const double fastLimit = cfg_.holdFastRate;
#else
                const double fastLimit = cfg_.holdFastGrace && inGrace ? cfg_.holdPanicRate : cfg_.holdFastRate;
#endif
                const bool useP98 = cfg_.p98 && f.p98;
                if (useP98 && p98TrialOn_) { /* the trial judges itself (P98TrialStep) */ }
                else if (useP98) {
                    const bool fastBad = fastFull && fast < (inGrace ? cfg_.holdPanicRate : cfg_.holdFastRate);
                    if (fastBad) why = "p98: fewer pictures a second than the 45 gives, over the fast window (a collapse)";
                    else if (!inGrace && P98Violated(f)) why = p98Why_;
                    if (why) failedTry = f.now - upAt_ < cfg_.failWindow && f.now >= hGraceUntil_;
                }
                else if (fastFull && fast < fastLimit) why = "hold60: fewer pictures a second than the 45 gives, over the fast window";
                else if (slowFull && slow < cfg_.holdSlowRate) why = "hold60: under the tolerated rate over the slow window";
                else if (!useP98 && fastFull && !(cfg_.holdFastGrace && inGrace) && fast < cfg_.holdSlowRate && busyHigh_ >= cfg_.busyFrames) why = "hold60: the busiest core stayed over the budget and the pictures do not come";
                else if (!useP98 && fastFull && !(cfg_.holdFastGrace && inGrace) && fast < cfg_.holdSlowRate && gpuHigh_ >= cfg_.busyFrames) why = "hold60: the GPU stayed over the budget even at the smallest picture and the pictures do not come";
                if (why && !useP98) failedTry = f.now - upAt_ < cfg_.failWindow && f.now >= hGraceUntil_;
            }
            else if (miss && f.now - upAt_ < cfg_.probation) { why = "a frame missed its refresh right after going up"; failedTry = true; }
            else if (missCount_ >= cfg_.missLimit) { why = "too many frames missed their refresh"; failedTry = cfg_.backoffByScene && f.now - upAt_ < cfg_.failWindow; }
            else if (busyHigh_ >= cfg_.busyFrames) { why = "the busiest core stayed over the budget"; failedTry = f.now - upAt_ < cfg_.failWindow; }
            else if (gpuHigh_ >= cfg_.busyFrames) { why = "the GPU stayed over the budget even at the smallest picture"; failedTry = f.now - upAt_ < cfg_.failWindow; }
            // the sustained-slowness rule: the window of the last frames
            {
                const int w = cfg_.slowFrames < 1 ? 1 : (cfg_.slowFrames < kMaxWindow ? cfg_.slowFrames : kMaxWindow);
                if (slowCount_ == w) slowSum_ -= slowRing_[slowPos_]; else ++slowCount_;
                slowRing_[slowPos_] = f.interval;
                slowSum_ += f.interval;
                slowPos_ = (slowPos_ + 1) % w;
                if (!why && !cfg_.hold60 && slowCount_ == w && slowSum_ / w > cfg_.slowMean * R)
                {
                    why = "frames last longer than a refresh on average (the limit is not on a CPU core: the GPU?)";
                    failedTry = f.now - upAt_ < cfg_.failWindow;
                    slowTry = true;
                }
            }
            // 1.2 ladder hold: a frame-miss or sustained-slowness verdict that is the GPU's (no core near the budget lately) waits while the renderer's ladder
            // has picture steps left, for LADDER_HOLD_MAX seconds at most (the rules keep speaking while they hold, so the verdict stands the moment the ladder
            // is out of steps or the time is up).
            if (why && cfg_.ladderHold && f.ladderRoom && cpuHighAge_ >= 8 && !(busyHigh_ >= cfg_.busyFrames) && !(gpuHigh_ >= cfg_.busyFrames)) {
                if (holdSince_ < 0 || f.now - holdLast_ > 3.0) holdSince_ = f.now;   // (a hold begins; one that was quiet for 3 s begins again)
                holdLast_ = f.now;
                if (f.now - holdSince_ < cfg_.ladderHoldMax) { why = 0; failedTry = slowTry = false; }
            } else if (!f.ladderRoom) {
                if (cfg_.ladderHold && holdSince_ >= 0 && f.now - holdLast_ <= 3.0) {
                    // the ladder has just given its last step: the misses of the frames before it say nothing about the picture it left, so the evidence starts
                    // again (a picture that still does not fit speaks again within a few frames; one that fits is kept)
                    memset(miss_, 0, sizeof miss_);
                    missCount_ = 0;
                    slowPos_ = slowCount_ = 0;
                    slowSum_ = 0.0;
                    busyHigh_ = gpuHigh_ = 0;
                    why = 0;
                    failedTry = slowTry = false;
                }
                holdSince_ = -1;
            }
            // pace13 MSC_TIER_DWELL: the soft rules wait out the dwell after a rise (the hard ones, a core / the GPU over budget, do not)
            if (why && cfg_.tierDwell > 0.0 && NP_FLAP_BREAK != 1 && f.now - upAt_ < cfg_.tierDwell && !(busyHigh_ >= cfg_.busyFrames) && !(gpuHigh_ >= cfg_.busyFrames)) { why = 0; failedTry = slowTry = false; ++dwellHeld_; }
            if (why && cfg_.hold60 && cfg_.p98 && f.p98) {
                ++p98Falls_; p98Viol_ = 0.0;
                if (p98Backoff_ <= 0.0) p98Backoff_ = cfg_.p98TrialAfter;
                p98NextTrial_ = f.now + p98Backoff_;
                p98TrialOn_ = false;
            }
            if (why) {
                if (cfg_.tierRelapse && failedTry && cfg_.sticky && f.sceneClass == 0 && NP_FLAP_BREAK != 2) {   // pace13 MSC_TIER_RELAPSE: a relapse counts twice in the relief history
                    if (nRelief_ < 8) relief_[nRelief_++] = f.now;
                    else { memmove(relief_, relief_ + 1, sizeof(double) * 7); relief_[7] = f.now; }
                    ++relapses_;
                }
                if (cfg_.hold60 && f.sceneClass == 0) {
                    // hold60: the wait before the climb's proof may pass: holdMin, doubled for each failed try lately; no latch
                    int prev = 0;
                    for (int i = 0; i < nHFail_; ++i) if (f.now - hFail_[i] < cfg_.stickyWindow) ++prev;
                    if (failedTry) {
                        if (nHFail_ < 8) hFail_[nHFail_++] = f.now;
                        else { memmove(hFail_, hFail_ + 1, sizeof(double) * 7); hFail_[7] = f.now; }
                        ++hFails_;
                    }
                    double wait = cfg_.holdMin;
                    for (int i = 0; i < prev && wait < cfg_.holdMax; ++i) wait *= 2.0;
                    if (wait > cfg_.holdMax) wait = cfg_.holdMax;
                    // (a fall inside the cut-in's grace is the cut's transient: holdMin, never doubled, and it does not count as a failed try)
                    stickyUntil_ = f.now + wait;
                    stickyOn_ = true;
                    stickyWait_ = wait;
                }
                else if (cfg_.sticky && f.sceneClass == 0) {   // the full-size 30 is held: long minimum, doubled for each relief lately, latched after a few
                    int recent = 0;
                    for (int i = 0; i < nRelief_; ++i) if (f.now - relief_[i] < cfg_.stickyWindow) ++recent;
                    ++recent;
                    if (nRelief_ < 8) relief_[nRelief_++] = f.now;
                    else { memmove(relief_, relief_ + 1, sizeof(double) * 7); relief_[7] = f.now; }
                    double wait = StickyBreak() == 1 ? 0.0 : cfg_.stickyMinHold;
                    for (int i = 1; i < recent && wait < 240.0; ++i) wait *= 2.0;
                    if (recent >= cfg_.stickyLatchAfter && StickyBreak() != 3) wait = cfg_.stickyLatchSecs;
                    stickyUntil_ = f.now + wait;
                    stickyOn_ = true;
                    stickyWait_ = wait;
                }
                if (failedTry && penalty_ < 3) ++penalty_;
                if (failedTry && cfg_.backoffByScene && sceneFails_ < 8) ++sceneFails_;
                double cool = cfg_.coolBase;
                if (cfg_.backoffByScene) {
                    // 1.2: every failed try of this scene doubles the wait for the next one
                    for (int i = 0; i < sceneFails_ && cool < cfg_.coolMax; ++i) cool *= 2.0;
                } else {
                    // 1.1: downs in the last 60 s double the cool-down
                    int recent = 0;
                    for (int i = 0; i < nDowns_; ++i) if (f.now - downs_[i] < 60.0) ++recent;
                    for (int i = 0; i < recent && cool < cfg_.coolMax; ++i) cool *= 2.0;
                }
                if (cool > cfg_.coolMax) cool = cfg_.coolMax;
                if (slowTry) {
                    // the margin cannot help here (no core is the limit): a scene that is slow with idle cores is tried again
                    // after 32, 64, 128, 256 s (a new scene or 120 s of stable 60 forgive one step)
                    if (slowFails_ < 4) ++slowFails_;
                    double slow = cfg_.coolMax;
                    for (int i = 1; i < slowFails_; ++i) slow *= 2.0;
                    if (slow > cool) cool = slow;
                }
                if (cfg_.classHold && f.sceneClass == 1) {
                    // a replay / celebration / cut-scene that did not hold 60 is shown at 30 until it ends (a short scene: no second try, no flip-flop)
                    cool = 1e9;
                    classHeld_ = true;
                }
                if (cfg_.sticky && f.sceneClass == 0 && f.now + cool < stickyUntil_) cool = stickyUntil_ - f.now;
                if (cfg_.hold60 && f.sceneClass == 0) cool = stickyUntil_ - f.now;   // (hold60: its own wait, not the scene back-off of Lock60)
                cooldownUntil_ = f.now + cool;
                if (nDowns_ < kMaxDowns) downs_[nDowns_++] = f.now;
                else { memmove(downs_, downs_ + 1, sizeof(double) * (kMaxDowns - 1)); downs_[kMaxDowns - 1] = f.now; }
                Go(kPolicy30, f.now);
                stableSince_ = -1;
                ResetEvidence();
                d.policy = cur_;
                d.switched = true;
                d.reason = why;
            }
        }
        return d;
    }

    // A scene that is to stay at 30 (23 Oct: Mega Strikes, drawn at full size rather than smaller at 60): 30 now, no evidence while held,
    // and the ordinary cool-down after it before 60 is tried again.
    Decision Hold30(double now, const char* reason = "a scene that stays at 30 (a Mega Strike: full-size picture)") {
        Decision d;
        d.policy = kPolicy30;
        d.switched = cur_ != kPolicy30;
        d.reason = reason;
        p98TrialOn_ = false;
        if (d.switched) Go(kPolicy30, now);
        stableSince_ = -1;
        ResetEvidence();
        if (cooldownUntil_ < now + cfg_.coolBase) cooldownUntil_ = now + cfg_.coolBase;
        return d;
    }

    // MSC_CADENCE: the cut back to a scene whose rate is known (np_fps_cadence.h): `p` at once, with no cool-down and no probation, as if the scene had always been in it. A 60 put back
    // settles for the usual frames (the backlog of the held 30's last frames is not judged); the failed-try counters are kept (a scene that keeps failing must still back off).
    Decision Resume(Policy p, double now, const char* reason = "MSC_CADENCE: the scene's own rate from its cut") {
        Decision d;
        d.policy = p;
        d.switched = cur_ != p;
        d.reason = reason;
        p98TrialOn_ = false;
        if (d.switched) Go(p, now);
        cooldownUntil_ = 0;
        upAt_ = -1e9;
        ResetEvidence();
        if (p == kPolicy60) {
            if (cfg_.hold60) hGraceUntil_ = now + cfg_.holdGrace;   // (hold60: a cut back into play)
            settle_ = cfg_.settle;
            for (int i = 0; i < cfg_.missWindow && i < kMaxWindow; ++i) miss_[i] = 0;
            missCount_ = 0;
            slowPos_ = slowCount_ = 0;
            slowSum_ = 0.0;
            busyHigh_ = gpuHigh_ = 0;
            classHeld_ = false;
            if (stableSince_ < 0) stableSince_ = now;
        }
        return d;
    }

    // The margin for going up now (the build's own UP_MARGIN, lowered by the failed tries).
    double Margin() const {
        double m = cfg_.upMargin - cfg_.penaltyStep * penalty_;
        if (stickyOn_ && cfg_.stickyMargin < m && StickyBreak() != 2) m = cfg_.stickyMargin;
        return m < 0.55 ? 0.55 : m;
    }

private:
    // ---- p98 (Config::p98): the delivered picture at 60 as the player sees it
    static const int kP98Ring = 1024;
    // repeats (refreshes beyond one a picture) over the last p98Window seconds, the span covered, and the visible hitches in it
    int P98Window(double now, double* span, int* hitches) const {
        int rep = 0; double oldest = now;
        for (int i = 0; i < p98N_; ++i) {
            const int k = (p98Pos_ - 1 - i + kP98Ring) % kP98Ring;
            if (now - p98T_[k] >= cfg_.p98Window) break;
            rep += p98R_[k]; oldest = p98T_[k];
        }
        double sp = now - oldest; if (p98N_ > 0 && sp < cfg_.p98Window && p98N_ < kP98Ring) sp += cfg_.refresh;
        *span = sp > cfg_.p98Window ? cfg_.p98Window : sp;
        int h = 0; for (int i = 0; i < nHit_; ++i) if (now - p98Hit_[i] < cfg_.p98Window) ++h;
        *hitches = h;
        return rep;
    }
    void P98Push(const FrameIn& f) {
        const double R = cfg_.refresh;
        int slots = (int)(f.interval / R + 0.5); if (slots < 1) slots = 1;
        const int rep = slots - 1 > 250 ? 250 : slots - 1;
        p98T_[p98Pos_] = f.now; p98R_[p98Pos_] = (unsigned char)rep; p98Pos_ = (p98Pos_ + 1) % kP98Ring; if (p98N_ < kP98Ring) ++p98N_;
        if (rep > 0) p98LastRep_ = f.now;
        // a visible hitch: a picture held 3+ refreshes (2 repeats in a row), or 3+ repeats within 0.5 s
        bool hitch = false;
        if (NP_P98_BREAK != 3) {
            if (rep >= 2) hitch = true;
            int inHalf = rep;
            if (rep > 0) { if (nPr_ < 8) p98Pr_[nPr_++] = f.now; else { for (int i = 1; i < 8; ++i) p98Pr_[i - 1] = p98Pr_[i]; p98Pr_[7] = f.now; } }
            int c = 0; for (int i = 0; i < nPr_; ++i) if (f.now - p98Pr_[i] < 0.5) ++c;
            if (c >= 3 || inHalf >= 3) hitch = true;
        }
        if (hitch) {
            if (nHit_ < 16) p98Hit_[nHit_++] = f.now; else { for (int i = 1; i < 16; ++i) p98Hit_[i - 1] = p98Hit_[i]; p98Hit_[15] = f.now; }
            nPr_ = 0;   // (the hitch took its repeats: they are not counted again)
            if (p98TrialOn_ && f.now >= p98TrialFrom_) ++p98TrHit_;
        }
        if (p98TrialOn_ && f.now >= p98TrialFrom_) { p98TrSlots_ += slots; p98TrRep_ += rep; }
        // the sustain timer: violated while (a) or (b) is violated over the window and a repeat is still going on (within p98Recent s); it decays at half speed when not
        double span = 0; int hit = 0; const int wr = P98Window(f.now, &span, &hit);
        const double dt = f.interval > 0.1 ? 0.1 : f.interval;
        bool viol = false;
        if (span >= 3.0 || span >= cfg_.p98Window) {
            const double allowRep = cfg_.p98RepeatPct * 0.01 * (span / R);
            int allowHit = (int)(span / cfg_.p98HitchSecs); if (allowHit < 1) allowHit = 1;
            bool a = wr > allowRep, b = NP_P98_BREAK != 3 && hit > allowHit;
            if (a || b) {   // forgive the one worst burst of p98Burst seconds
                int bestRep = 0, bestHit = 0;
                for (int i = 0, j = 0, sum = 0; i < p98N_; ++i) {   // i: the newest end of a sub-window, going back; j: its oldest
                    const int ki = (p98Pos_ - 1 - i + kP98Ring) % kP98Ring;
                    if (f.now - p98T_[ki] >= cfg_.p98Window) break;
                    sum += p98R_[ki];
                    while (p98T_[(p98Pos_ - 1 - j + kP98Ring) % kP98Ring] - p98T_[ki] >= cfg_.p98Burst) { sum -= p98R_[(p98Pos_ - 1 - j + kP98Ring) % kP98Ring]; ++j; }
                    if (sum > bestRep) bestRep = sum;
                }
                for (int i = 0; i < nHit_; ++i) { int c = 0; for (int k = 0; k < nHit_; ++k) if (p98Hit_[k] >= p98Hit_[i] && p98Hit_[k] - p98Hit_[i] < cfg_.p98Burst && f.now - p98Hit_[k] < cfg_.p98Window) ++c; if (c > bestHit) bestHit = c; }
                a = wr - bestRep > allowRep; b = NP_P98_BREAK != 3 && hit - bestHit > allowHit;
            }
            if ((a || b) && f.now - p98LastRep_ <= cfg_.p98Recent) { viol = true; p98Why_ = b && !a ? "p98: visible hitches more often than once per 5 s over the window, for the sustain span" : "p98: repeated refreshes over the tolerated share of the window, for the sustain span"; }
        }
        if (viol) p98Viol_ += dt; else { p98Viol_ -= dt; if (p98Viol_ < 0) p98Viol_ = 0; }
        // a kept trial's 60 for p98KeepSecs resets the backoff
        if (!p98TrialOn_ && p98KeptAt_ > 0 && f.now - p98KeptAt_ >= cfg_.p98KeepSecs && p98Backoff_ > 0.0) { p98Backoff_ = 0.0; p98KeptAt_ = -1e9; }
    }
    bool P98Violated(const FrameIn&) const {
#if NP_P98_BREAK == 1
        return p98Viol_ > 0.0;
#elif NP_P98_BREAK == 2
        return false;
#else
        return p98Viol_ >= cfg_.p98Sustain;
#endif
    }
    bool P98GpuOk(const FrameIn& f, double Rms) const {
#if NP_P98_BREAK == 7
        (void)f; (void)Rms; return true;
#else
        return !(f.gpuMinMs > cfg_.p98GpuFit * Rms);
#endif
    }
    // flip25: the two gates of the trial of 60 by time (see Config::p98Defer / p98Calm)
    bool P98TimeTrialOk(const FrameIn& f, const Prediction& p, double Rms) {
#ifndef NP_FLIP_BREAK
#define NP_FLIP_BREAK 0   // host test mutants (tools/fps/flip25_unit.cpp): 1 the defer is ignored, 2 no cap on failed trials, 3 no settle after a scene change, 4 no cost gate
#endif
        if (NP_FLIP_BREAK != 1 && cfg_.p98Defer && f.now < p98DeferUntil_) { ++p98DeferSkips_; return false; }
        if (!cfg_.p98Calm) return true;
        if ((NP_FLIP_BREAK != 3 && f.now - sceneAt_ < cfg_.p98CalmSettle) || (NP_FLIP_BREAK != 2 && p98ConsecFail_ >= cfg_.p98CalmFails) ||
            (NP_FLIP_BREAK != 4 && (!p.valid || p.max60 > cfg_.p98CalmFit * Rms))) { ++p98CalmSkips_; return false; }
        return true;
    }
    void P98StartTrial(const FrameIn& f, Decision& d, double skip, const char* why) {
        Go(kPolicy60, f.now);
        d.policy = cur_; d.switched = true; d.reason = why;
        upAt_ = f.now; hGraceUntil_ = 0; settle_ = cfg_.settle;
        for (int i = 0; i < cfg_.missWindow && i < kMaxWindow; ++i) miss_[i] = 0;
        missCount_ = 0; slowPos_ = slowCount_ = 0; slowSum_ = 0.0; busyHigh_ = gpuHigh_ = 0;
        p98TrialOn_ = true; p98TrCut_ = false; p98TrialFrom_ = f.now + skip; p98TrialEnd_ = p98TrialFrom_ + cfg_.p98TrialSecs; p98TrRep_ = p98TrSlots_ = p98TrHit_ = 0;
        p98Viol_ = 0.0; p98N_ = 0; nPr_ = 0; nHit_ = 0;
        ++p98Trials_;
    }
    // a trial at 60: kept when its repeats are within the share and it had at most one visible hitch; else back to the relief with the wait doubled
    Decision P98TrialStep(const FrameIn& f, Decision d) {
        d.switched = false;
        if (f.now < p98TrialEnd_) {
            // an early end: a collapse (the fast window of the catastrophic guard) fails the trial at once
            const double age = f.now - hEnter_;
            if (!(f.now >= hGraceUntil_ && age >= cfg_.holdFastSecs && HoldRate(f.now, cfg_.holdFastSecs) < cfg_.holdFastRate)) return d;
        }
        const double slots = p98TrSlots_ > 0 ? (double)p98TrSlots_ : 1.0;
        const bool ok = NP_P98_BREAK != 4 && f.now >= p98TrialEnd_ && p98TrRep_ <= cfg_.p98RepeatPct * 0.01 * slots + 0.5 && p98TrHit_ <= 1;
        p98TrialOn_ = false;
        if (ok) { p98ConsecFail_ = 0; ++p98Ok_; p98KeptAt_ = f.now; p98Viol_ = 0.0; d.reason = "p98: the trial kept 60"; p98Why_ = d.reason; return d; }
        ++p98Fail_;
        if (!p98TrCut_) ++p98ConsecFail_;
        const double was = p98Backoff_ > 0.0 ? p98Backoff_ : cfg_.p98TrialAfter;
        double nb = was * 2.0; if (NP_P98_BREAK == 5) nb = was; if (nb > cfg_.p98BackoffMax) nb = cfg_.p98BackoffMax;
        p98Backoff_ = nb;
        p98NextTrial_ = f.now + nb;
        Go(kPolicy30, f.now);
        stableSince_ = -1; ResetEvidence();
        d.policy = cur_; d.switched = true; d.reason = "p98: the trial of 60 failed (repeats or hitches), back to the relief";
        p98Why_ = d.reason;
        return d;
    }
    static const int kMaxWindow = 256;
    static const int kMaxDowns = 16;
    static int StickyBreak() {
#ifdef NP_STICKY_BREAK
        return NP_STICKY_BREAK;   // host test mutants (tools/fps/fps_sticky_unit.cpp): 1 no minimum hold, 2 the ordinary proof, 3 no latch, 4 a new scene forces 60 at once
#else
        return 0;
#endif
    }
    void ResetEvidence() { goodSince_ = -1; goodFrames_ = 0; windowFrames_ = 0; outliers_ = 0; holdSince_ = -1; hProofN_ = hProofPos_ = hProofOver_ = 0; }
    void Go(Policy p, double now) { if (p == kPolicy60) HoldRestartRate(now); cur_ = p; ++switches_; }
    // hold60: the delivered-rate window
    static const int kRateRing = 512;
    void HoldRestartRate(double now) { hEnter_ = now; hRateN_ = 0; }
    void HoldPush(double now) { hRate_[hRatePos_] = now; hRatePos_ = (hRatePos_ + 1) % kRateRing; if (hRateN_ < kRateRing) ++hRateN_; }
    // hold60: one picture's cost at 60 for the climb: the WORK (game thread less waits, the simulation re-priced to the 60's steps), the renderer's lanes, the GPU at the 60 floor.
    // Not the frame length (the relief's waits), not the probe's per-core busy time (time-following work and spins). hold60b: the game term keeps the blend lane's wait (FrameIn::laneWaitMs:
    // the game thread is blocked on it at 60 as well), and a picture is priced both with its step (`with`) and without (`without`): at 60 a picture carries a step R / 20 ms of the time.
    struct UpParts { double with, without, game, lane, fe, be, gpu; int by; };
    UpParts UpCostParts(const FrameIn& f, const Prediction& p) const {
        UpParts u;
        u.lane = f.laneWaitMs > 0 ? f.laneWaitMs : 0.0;
#if NP_HOLD_BREAK == 5
        double base = f.interval * 1000.0, step = 0.0;   // (mutant: the frame length, the relief's waits included)
        (void)p;
#else
        double base = p.max60, step = 0.0;
        if (f.workMs > 0) {
            base = f.workMs;
#if NP_HOLD_BREAK == 9
            base -= u.lane;   // (mutant: the lane's wait left out, as [gsplit-pic] prices the work)
#endif
            if (f.simMode == 0) {
                if (NP_P24_BREAK != 3 && cfg_.holdWallSim && f.stepWallMs >= f.simMs && f.stepWallMs > 0 && f.steps > 0 && stepWallCost_ > 0) { base -= f.stepWallMs; step = stepWallCost_; }   // pace24: one clock for the frame and its steps
                else { base -= f.simMs; step = stepCost_; }
                if (base < 0) base = 0;
            }
        }
#endif
        u.game = base + step;
        u.fe = f.feMs > 0 ? f.feMs : 0.0;
        u.be = f.beMs > 0 ? f.beMs : 0.0;
        u.gpu = f.gpuMinMs > 0 ? f.gpuMinMs : 0.0;
        const double lanes = u.fe > u.be ? u.fe : u.be, rest = lanes > u.gpu ? lanes : u.gpu;
        u.with = u.game > rest ? u.game : rest;
        u.without = base > rest ? base : rest;
        u.by = u.game >= rest ? 0 : (u.gpu >= lanes ? 4 : (u.fe >= u.be ? 2 : 3));
        return u;
    }
    // hold60b: how much of a picture the proof counts as over, in 1/kOverUnit pictures: the 60's step share with the step, the rest without it
    static const int kOverUnit = 64;
    unsigned char OverUnits(const UpParts& u, double lim, double R) const {
        const double share = cfg_.stepSeconds > 0 ? (R / cfg_.stepSeconds < 1.0 ? R / cfg_.stepSeconds : 1.0) : 1.0;
        const int w = (int)(share * kOverUnit + 0.5);
#if NP_HOLD_BREAK == 8
        (void)w;
        return u.with > lim ? (unsigned char)kOverUnit : 0;   // (mutant: every picture with a full step)
#else
        return (unsigned char)((u.with > lim ? w : 0) + (u.without > lim ? kOverUnit - w : 0));
#endif
    }
    void NoteParts(const UpParts& u, const FrameIn& f, double lim, unsigned char over) {
        ++hParts_.n;
        hParts_.game += u.game; hParts_.lane += u.lane; hParts_.fe += u.fe; hParts_.be += u.be; hParts_.gpu += u.gpu;
        if (u.gpu > hParts_.gpuMax) hParts_.gpuMax = u.gpu;
        hParts_.over += over / (double)kOverUnit;
        if (u.with > lim) {
            int by = u.by;
            const double lanes = u.fe > u.be ? u.fe : u.be, rest = lanes > u.gpu ? lanes : u.gpu;
            if (by == 0 && u.game - u.lane <= lim && rest <= lim) by = 1;   // (over only because of the lane's wait)
            ++hParts_.by[by];
        }
        (void)f;
    }
    Config cfg_;
    Policy cur_;
    double goodSince_;
    int goodFrames_;
    unsigned scene_;
    double cooldownUntil_;
    double upAt_;
    unsigned dwellHeld_ = 0, relapses_ = 0, seeds_ = 0;
    int penalty_;
    double stableSince_;
    unsigned char miss_[kMaxWindow];
    int missPos_, missCount_, busyHigh_, gpuHigh_;
    double holdSince_, holdLast_;   // 1.2 ladder hold: when it began, and the last frame a rule wanted 30 in it
    int cpuHighAge_;                // frames since the busiest core was over DOWN_BUSY of a refresh
    int settle_;
    double slowRing_[kMaxWindow];
    int slowPos_, slowCount_;
    double slowSum_;
    int slowFails_;
    int sceneFails_;
    int windowFrames_, outliers_;
    bool classHeld_ = false;
    double downs_[kMaxDowns];
    int nDowns_;
    double stepCost_;
    double stepWallCost_ = 0.0;   // pace24: ms of wall time per simulation step, smoothed (Config::holdWallSim)
    int switches_;
    double relief_[8];
    int nRelief_;
    double stickyUntil_, stickyWait_ = 0;
    bool stickyOn_;
    Prediction lastPred_;
    npsimpace::Model sp_;   // pace18: the overlap's smoothed window and join overhead
    // hold60
    double hRate_[kRateRing];
    int hRateN_ = 0, hRatePos_ = 0;
    double hEnter_ = 0, hGraceUntil_ = 0;
    unsigned char hProof_[kMaxWindow];
    int hProofN_ = 0, hProofPos_ = 0, hProofOver_ = 0;
    double hFail_[8];
    int nHFail_ = 0;
    unsigned hFails_ = 0;
    double lastUp_ = 0;
    HoldParts hParts_;
    At60 hAt60_;
    // p98
    double p98T_[kP98Ring]; unsigned char p98R_[kP98Ring];
    int p98N_ = 0, p98Pos_ = 0, nPr_ = 0, nHit_ = 0;
    double p98Pr_[8], p98Hit_[16];
    double p98DeferUntil_ = -1e9, sceneAt_ = -1e9;
    int p98ConsecFail_ = 0;
    unsigned p98CalmSkips_ = 0, p98DeferSkips_ = 0;
    double p98Viol_ = 0.0, p98LastRep_ = -1e9, p98Backoff_ = 0.0, p98NextTrial_ = 0.0, p98KeptAt_ = -1e9, p98TrialEnd_ = 0.0, p98TrialFrom_ = 0.0;
    bool p98TrialOn_ = false, p98TrCut_ = false;
    int p98TrRep_ = 0, p98TrSlots_ = 0, p98TrHit_ = 0;
    unsigned p98Trials_ = 0, p98Ok_ = 0, p98Fail_ = 0, p98Falls_ = 0, p98GpuSkips_ = 0;
    const char* p98Why_ = "";
};

} // namespace npfps

#endif // NP_FPS_CTL_H
