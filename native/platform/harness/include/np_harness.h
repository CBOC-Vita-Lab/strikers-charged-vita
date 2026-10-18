/* np_harness.h: scripted runs of the native port (platform/harness).
 *
 * The same environment interface as the recompiled build's harness
 * (one/vita/src/plat_headless.cpp, tools/README-nav.md): MSC_INPUT_SCRIPT nav scripts with
 * state conditions, MSC_MAX_FRAMES, MSC_FRAME_LOG, MSC_WATCH ([nav] / [watch] lines),
 * MSC_WATCH_BUTTONS, MSC_FRAME_TRACE, MSC_QEMU_MARK, MSC_STATE_HASH, MSC_VIRTUAL_TIME and
 * friends. The state variables come from the game's own globals (symbols of the decomp), not
 * from guest addresses. Full description: np/docs/vita.md, section "Harness", and the header
 * comment of src/np_harness.cpp.
 *
 * Who calls what:
 *   NpOnFramePresented()   once per presented frame, on the game thread, right after the game
 *                          issued the frame's GXCopyDisp (the recompiled runtime's equivalent:
 *                          PlatFrameStats::OnFramePresented). The build wires it with
 *                          -Wl,--wrap=glxSwapPre (glxSwapPre is called once per frame from
 *                          glPlat.cpp); platform/harness/src/np_wrap_swap.cpp provides the
 *                          __wrap_ function. A frame limit or a script's `exit` does not return.
 *   NpHarnessFillPad()     from the SDK layer's PADRead (channel 0) after the real pad was read:
 *                          the script's pad replaces the real one while it presses something.
 *   NpHarnessFinish()      the end of a run asked for from outside (lab watchdog, fatal paths).
 *   NpHarnessRegisterRegion()  MSC_STATE_HASH also hashes registered memory regions (the SDK
 *                          layer registers its MEM1/MEM2 arenas); deterministic only where the
 *                          arenas sit at the same addresses (qemu-user, setarch -R).
 */
#ifndef NP_HARNESS_H
#define NP_HARNESS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The pad as the nav scripts see it: GameCube pad bits (LEFT 1, RIGHT 2, DOWN 4, UP 8, Z 0x10,
 * R 0x20, L 0x40, A 0x100, B 0x200, X 0x400, Y 0x800, START 0x1000) and signed sticks. */
typedef struct NpPadState {
    uint16_t buttons;
    int8_t stickX, stickY, substickX, substickY;
    uint8_t triggerL, triggerR;
} NpPadState;

/* W5 CTL: a script command in the Vita's own terms (vhold, vpress, vmash, touch, swipe, rtouch, rswipe, rmash, swmash): buttons as
 * SCE_CTRL bits, sticks in +-72 units (as the GameCube commands), one finger on the front or the rear panel (960 x 544).
 * NpHarnessFillRaw returns 1 while such a command runs (the SDK layer then uses it instead of the player's controls, and the
 * player's layout applies to it, unlike a GameCube-pad command). */
typedef struct NpHarnessRaw {
    uint32_t buttons;
    int8_t stickX, stickY, substickX, substickY;
    int touchCount;
    struct { int id; float x, y; } touch[4];
    int rearCount;
    struct { int id; float x, y; } rear[4];
} NpHarnessRaw;
int NpHarnessFillRaw(NpHarnessRaw* raw);
/* Script variables the harness does not know are asked from the game layer (weak; platform/sdk/input: the controls' counters). */
int NpHarnessGameVar(const char* name, double* out) __attribute__((weak));

void NpOnFramePresented(void);
/* Returns 1 when the harness changed `pad`. */
int NpHarnessFillPad(NpPadState* pad);
/* Ends the run through the harness's own exit path (end-of-run reports, exit hooks). Not returning. */
void NpHarnessFinish(int code, const char* why);
/* 1 when any harness service is switched on by the environment. */
int NpHarnessActive(void);
int NpHarnessOwnsPad(void);   /* an input script is loaded: the controller is the script's (logging alone leaves the real pad) */
/* Presented frames counted by the harness so far. */
uint64_t NpHarnessFrameCount(void);
void NpHarnessRegisterRegion(const char* name, const void* base, uint32_t size);
/* Run `fn` once, before the harness leaves the process (end-of-run reports of other layers). */
void NpHarnessAtExit(void (*fn)(void));

/* Task marks (MSC_TASK_MARKS=1, qemu instruction counts per nlTask): called by nlTaskManager::RunAllTasks
 * around each task's Run (weak references in the game; np/docs/vita.md "Task budgets"). */
void NpTaskBegin(const char* name, unsigned index);
void NpTaskEnd(const char* name, unsigned index);

/* Optional hooks implemented by the platform layer (weak references: absent = skipped). */
void NpVitaOnFramePresented(void) __attribute__((weak));   /* console/log accounting before the harness */
void NpVitaExitReport(void) __attribute__((weak));         /* memory high-water marks etc. */
void NpVitaEndRun(int code, const char* why) __attribute__((weak)); /* lab: reason for the status file */

#ifdef __cplusplus
}
#endif
#endif
