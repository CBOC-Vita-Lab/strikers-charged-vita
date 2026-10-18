/* np/fps.h: how fast pictures are presented (W5 FPS): MSC_FPS=30|auto|60|unlocked (default 30; "game" = the build as it was before this switch).
 *
 * Background (docs/wave5-FPS.md has the measurements). The picture is shown by the renderer at GXCopyDisp, the game paces itself against the
 * VI emulation (59.94 Hz): after the frame is built it waits in glxSwap.cpp swap_Post for a retrace, according to a "swap mode" it picks per
 * state: 1 in the menus and replays (wait for the next retrace: a frame is held 1, 2, 3 .. retraces), 3 in a match (wait only when the frame was
 * early: the Wii's free-running mode), 2 in a cut-scene that locks its frame rate (two retraces). That is what the build did before this switch
 * ("game"), and no part of it forced 30. The four values:
 *
 *   30        a picture at most every second refresh: at least two retraces between the hand-over of two frames, no extra wait for a frame that
 *             is already later (swap mode 4 below). Flat 30 where the machine can do it, the frames' own pace where it cannot.
 *   60        a picture per refresh: wait one retrace after every frame (swap mode 1 everywhere): 60 where a frame fits into a refresh, a frame
 *             that misses its refresh is held two (the double-buffered vsync of a console).
 *   unlocked  the Wii's free-running mode 3 everywhere: no wait for a late frame, a wait only when the frame was early. Every frame is shown at the
 *             next refresh, an uneven 16.7 / 33.3 ms mix between 30 and 60.
 *   auto      60 while the scene holds it, 30 otherwise. Decided from the measured cost of every core per frame (np_fps_ctl.h): up after one
 *             second of frames that all fit with margin, down at once after a miss, a cool-down that grows when it has to go down again.
 *
 * A cut-scene that locks its frame rate (BeginFrameTask::s_FramerateLocked, swap mode 2) keeps the game's own lock in every mode.
 * The game's per-drawn-frame code (np/framepace.h, MSP) follows the real frame length in every mode: 1 original frame at 60, 2 at 30, a mix
 * when unlocked.
 *
 * Swap modes returned by NpFpsSwapMode: 0..3 are the game's (glxSwap.cpp), 4 = the "30" cap.
 */
#ifndef NP_FPS_H
#define NP_FPS_H

#ifdef NP_NATIVE
#ifdef __cplusplus
extern "C" {
#endif
/* The game side (glxSwap.cpp, BeginFrameTask through NpPaceBeginFrame, FixedUpdateTask.cpp, movie.cpp). */
int NpFpsSwapMode(int gameMode);                       /* the swap mode to use for a frame whose game mode is gameMode */
void NpFpsSwapWaitBegin(void);                         /* around the retrace waits of one swap_Post (counted for the statistics and the controller) */
void NpFpsSwapWaitEnd(void);
void NpFpsBeginFrame(float realSeconds, unsigned taskState);   /* once per drawn frame (BeginFrameTask): the previous frame's length, the task manager's state */
void NpFpsSimBegin(void);                              /* FixedUpdateTask::Run: the simulation part of the frame (cost per step) */
void NpFpsSimStep(void);
void NpFpsSimEnd(void);
void NpFpsMovieEnd(unsigned decoded, unsigned retraces);   /* a movie stopped: decoded frames and retraces since its start (speed check) */
int NpFpsPolicy(void);                                 /* 0 game, 1 = 30, 2 = 60, 3 = unlocked: the policy in force right now */
int NpFpsMode(void);                                   /* 0 game, 1 = 30, 2 = 60, 3 = unlocked, 4 = auto: what MSC_FPS asked for */
int NpFpsDisplayInterval(void);                        /* vblanks a picture is shown for: 2 under policy 30, 1 under 60, 0 = no gate (the vgx display gate reads it) */
void NpFpsBootDone(int atTitle);                       /* W5 BOOT: the boot screens (0) / the title screen (1): the start-up's picture-per-refresh pacing (MSC_BOOT_FPS) ends */
/* W5 RT, the late wait (MSC_LATE_WAIT, input delay; np_fps.cpp has the reasoning). Under policy 30 (swap mode 4):
 *  1 = hand over first: swap_Post skips its wait (NpFpsLateWait(4, retrace at the previous hand-over + 2) returns 1) and the same wait is made
 *      right after the frame's send (glplatSendFrame / glplatSendFrameForked: NpFpsLateWaitAfterSend), two retraces after the previous one ended;
 *  2 = before the pad read (experiment): NpFpsLateWait arms, NpFpsLateWaitOwe(retrace at the hand-over + 2) at the hand-over, FixedUpdateTask::Run
 *      makes it first (NpFpsLateWaitRun(0)), swap_Post when it did not run (NpFpsLateWaitRun(1)). */
int NpFpsLateWait(int swapMode, unsigned long refRetrace);
void NpFpsLateWaitAfterSend(void);
void NpFpsLateWaitOwe(unsigned long refRetrace);
void NpFpsLateWaitRun(int where);

/* The platform side: what a core spent. cpuMs / timeMs are CUMULATIVE busy milliseconds per core since the start (monotone); timeMs is the part of
 * it that follows time (audio mixer, helpers), not frames. gameCore is the core of the game thread. Returns 0 when there is no measurement. */
typedef struct NpFpsProbeOut {
    double cpuMs[4];
    double timeMs[4];
    int gameCore;
} NpFpsProbeOut;
typedef int (*NpFpsProbeFn)(NpFpsProbeOut* out);
void NpFpsSetProbe(NpFpsProbeFn fn);
/* CPU time of the calling thread in milliseconds (the game thread); default: clock_gettime(CLOCK_THREAD_CPUTIME_ID) where there is one. */
typedef double (*NpFpsThreadClockFn)(void);
void NpFpsSetThreadClock(NpFpsThreadClockFn fn);

/* The harness side (virtual clock runs, MSC_FPS_MODEL): the modelled cost of the frame that just ended, in nanoseconds of virtual time to
 * charge (the busiest core: the steady-state period of the pipeline); also what the controller reads at the next BeginFrame instead of a probe.
 * Returns 0 when no model is on. */
long long NpFpsModelCharge(unsigned taskState, int sceneId, unsigned steps);
#ifdef __cplusplus
}
#endif
#else
static inline int NpFpsSwapMode(int gameMode) { return gameMode; }
static inline void NpFpsSwapWaitBegin(void) {}
static inline void NpFpsSwapWaitEnd(void) {}
static inline void NpFpsBeginFrame(float, unsigned) {}
static inline void NpFpsSimBegin(void) {}
static inline void NpFpsSimStep(void) {}
static inline void NpFpsSimEnd(void) {}
static inline void NpFpsMovieEnd(unsigned, unsigned) {}
#endif

#endif /* NP_FPS_H */
