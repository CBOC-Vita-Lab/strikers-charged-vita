/* np/framepace.h: 30 fps frame pacing for the code that steps once per DRAWN frame (MSP, wave 4).
 *
 * The game was written for 59.94 drawn frames a second (one VI retrace each; 30 in a cutscene that locks its framerate). A few things
 * outside the fixed 50 Hz simulation advance a fixed step per drawn frame, or decide once per frame, instead of following elapsed time.
 * Presented at 30 fps (the Vita's target) they would run at half speed. Each hook keeps the original's pace instead:
 *
 *   (each call names its site, NP_PACE_*, so MSC_FRAME_PACING_SITES can leave a site on the original path for diagnosis)
 *   continuous step   scale it with NpPaceFrames()  = this frame's real length in ORIGINAL frames (1.0 at the original rate, 2.0 at 30 fps)
 *   discrete step    do it NpPaceSteps() times      = the original frames this drawn frame covers (1 at the original rate, 2 at 30 fps),
 *                                                     each with dt / NpPaceSteps(); a roll that stops at its first success (an emission)
 *                                                     stops there, as it would have in the original frame it succeeded in
 *   frame counter    NpPaceFrameNumber(drawnFrame)   = the count the original would have reached
 *
 * Frames at or below the original length are the original path exactly (1.0, 1, the counter unchanged). A frame of 4 original frames
 * (66 ms) or more is a loading stall, not the game's speed: it counts as 4. A frame within 0.1 of a whole number of original frames
 * counts as that number (VI-locked frames are whole retraces; the jitter must not add or drop an original frame now and then).
 *
 * Adapted from yannicksuter/mscharged-port (CC0): src/platform/game_tweaks.cpp (original-frame phase accumulator) and patches
 * tweaks/0013, 0014 (the list of sites). That port raises the rate ABOVE the original and so suppresses steps; this one is for below.
 *
 * Switch: MSC_FRAME_PACING=0 (environment, env.txt on the Vita) turns it off; default on. MSC_FRAME_PACING_SITES=<mask> (diagnosis, default 63) leaves
 * only some hook sites paced (1 camera ease, 2 letterbox, 4 emission rolls, 8 texture animations, 16 blinking, 32 frame counter): the others
 * run the original path, so a difference between two runs can be traced to a site. MSC_FRAME_PACING_LOG=1 prints one line per
 * 120 frames (virtual seconds, drawn and original frames, rolls, image changes), =2 also one line per event (letterbox, camera ease). BeginFrameTask calls NpPaceBeginFrame with the task manager's real (unbounded, undilated) frame length first thing;
 * SetupRenderInfo tells the swap mode (a swap mode 2 frame is two retraces).
 */
#ifndef NP_FRAMEPACE_H
#define NP_FRAMEPACE_H

#ifdef NP_NATIVE
#ifdef __cplusplus
extern "C" {
#endif
void NpPaceBeginFrame(float realSeconds);
void NpPaceSwapMode(int swapMode, int palTv);
int NpPaceEnabled(void);
extern float g_npPaceFrames;        /* read through the inline functions below */
extern int g_npPaceSteps;
extern unsigned long g_npPaceExtra;
extern unsigned g_npPaceSites;      /* MSC_FRAME_PACING_SITES */
/* Proof aids (MSC_FRAME_PACING_LOG): counters of the discrete steps taken, and one trace line per event with level 2. */
extern unsigned long g_npPaceRolls;     /* emission rolls */
extern unsigned long g_npPaceChanges;   /* texture-animation image changes and blinks */
void NpPaceEvent(const char* what, float value);
#ifdef __cplusplus
}
#endif
enum { NP_PACE_CAMERA = 1, NP_PACE_LETTERBOX = 2, NP_PACE_ROLLS = 4, NP_PACE_TEXANIM = 8, NP_PACE_BLINK = 16, NP_PACE_FRAMENO = 32 };
static inline float NpPaceFrames(unsigned site) { return (g_npPaceSites & site) ? g_npPaceFrames : 1.0f; }
static inline int NpPaceSteps(unsigned site) { return (g_npPaceSites & site) ? g_npPaceSteps : 1; }
static inline unsigned long NpPaceFrameNumber(unsigned site, unsigned long drawnFrame) { return drawnFrame + ((g_npPaceSites & site) ? g_npPaceExtra : 0); }
static inline void NpPaceCountRoll(void) { ++g_npPaceRolls; }
static inline void NpPaceCountChange(void) { ++g_npPaceChanges; }
#else
static inline void NpPaceBeginFrame(float) {}
static inline void NpPaceSwapMode(int, int) {}
enum { NP_PACE_CAMERA = 1, NP_PACE_LETTERBOX = 2, NP_PACE_ROLLS = 4, NP_PACE_TEXANIM = 8, NP_PACE_BLINK = 16, NP_PACE_FRAMENO = 32 };
static inline float NpPaceFrames(unsigned) { return 1.0f; }
static inline int NpPaceSteps(unsigned) { return 1; }
static inline unsigned long NpPaceFrameNumber(unsigned, unsigned long drawnFrame) { return drawnFrame; }
static inline void NpPaceCountRoll(void) {}
static inline void NpPaceCountChange(void) {}
static inline void NpPaceEvent(const char*, float) {}
#endif

#endif // NP_FRAMEPACE_H
