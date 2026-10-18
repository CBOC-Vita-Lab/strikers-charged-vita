/* np_vita.h: what the platform layer (platform/vita) offers to the other workstreams (NPB, NPS, NPR).
 *
 * Everything here exists on the Vita and, where it makes sense, on host32/armhf (so that the SDK
 * layer is written once). `extern "C"`, no game headers. See np/docs/vita.md for the plan behind it.
 */
#ifndef NP_VITA_H
#define NP_VITA_H

#include <stddef.h>
#include <stdint.h>
#include "np_harness.h"   /* NpPadState */
#include "np_jobs.h"
#include "np_time.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the game's entry --------------------------------------------------------------------
 * The platform layer owns main(): NPB's platform/compat/np_main.cpp defines a WEAK main that just calls
 * the game; the Vita layer (np_vita_main.cpp) and the host layer (np_host_main.cpp) define strong ones
 * that set up the platform first (log, console, env.txt, clocks, core plan, data directory check,
 * crash handler, job pool) and then call
 *     int np_game_main();          // C++ linkage: the decomp's `int main()` of src/Game/main.cpp,
 *                                  // renamed by compiling that file with -Dmain=np_game_main (NPB's Makefile).
 * The Wii game's main never returns (it runs the task loop); the process leaves through
 * NpPlatformExit or the harness. */

/* ---- paths ---------------------------------------------------------------------------------
 * All files live under ux0:data/mscharged (device path for sceIo, POSIX form for stdio after the
 * working directory was set to it at start-up). The extracted disc is "game/" there. */
const char* NpDataDevicePath(void);   /* "ux0:/data/mscharged" */
const char* NpGameRootPath(void);     /* the disc root: <data>/game (sys/ and files/) */

/* ---- memory arenas -----------------------------------------------------------------------
 * The game's Wii arenas (MEM1 24 MiB, MEM2 64 MiB, ...) are plain host memory in the native port:
 * no fixed addresses (so no kubridge), only a block of the requested size. On the Vita a
 * sceKernelAllocMemBlock USER_RW block (not the C heap: the heap stays small and the arena is
 * counted in the memory report); on host builds aligned_alloc. Never freed before exit. */
void* NpAllocArena(size_t bytes, size_t alignment, const char* name);

/* ---- audio output --------------------------------------------------------------------------
 * 16-bit stereo, one period = 512 frames, a ring behind it (policy MSC_AUDIO_QUEUE=elastic|fixed,
 * platform/vita/src/vita_audio_queue.h) drained by a thread on the audio core into a sceAudioOut
 * BGM port. Push from the SDK layer's mixer, on any one thread. */
int NpAudioOpen(uint32_t sampleRate);                       /* 1 = open (also when it already was) */
int NpAudioPushLE16(const int16_t* leftRight, uint32_t frames);   /* interleaved L,R; 1 pushed, 0 dropped */
int NpAudioPushWiiAiBE16(const uint8_t* data, size_t bytes);      /* Wii AI DMA block: big-endian, right then left */
int NpAudioSpaceFrames(void);                               /* frames that fit now; -1 = not known (fixed policy / not open) */
int NpAudioPaceFrames(void);                                /* frames a clockless producer may push now (start level + 48 ms); -1 = not known */
void NpAudioSetVolume(float volume);                        /* 0..1 */
void NpAudioSetMuted(int muted);

/* ---- input --------------------------------------------------------------------------------- */
typedef struct NpVitaTouchPoint { int id; float x, y; } NpVitaTouchPoint;      /* display pixels, 960 x 544 */
typedef struct NpVitaInput {
    uint32_t buttons;                    /* SCE_CTRL_* bits */
    uint8_t lx, ly, rx, ry;              /* 0..255, 128 = centre, y grows downwards */
    int touchCount;                      /* front panel, 0 when the panel is off or disabled */
    NpVitaTouchPoint touch[8];
    int rearCount;                       /* W5 CTL: rear panel (scaled to 960 x 544 like the front), 0 when off (MSC_REAR_TOUCH) */
    NpVitaTouchPoint rear[8];
} NpVitaInput;
/* Peeks the controls (never blocks). 0 = no sample (host builds, or the display is owned by a script run). */
int NpVitaReadInput(NpVitaInput* out);
/* The GameCube-pad form of the Vita controls (Cross=A, Square=B, Circle=X, Triangle=Y, L, R,
 * Select=Z, Start, D-pad, left stick = main stick, right stick = C stick; dead zone 16, 80 % of
 * travel = +-72). Fills `pad` completely. */
void NpVitaMapGcPad(const NpVitaInput* in, NpPadState* pad);
/* Touch for the minigames is only offered while no input script runs (the harness owns the pad). */
int NpVitaTouchAllowed(void);

/* ---- clocks and power ---------------------------------------------------------------------- */
int NpVitaCpuMHz(void);              /* the frequency the system reports now */
void NpVitaKeepAwake(void);          /* sceKernelPowerTick: call about once a second during unattended runs */

/* ---- process ------------------------------------------------------------------------------- */
/* Flush the log, end the lab run if one is active, leave the process. Not returning. */
void NpPlatformExit(int code) __attribute__((noreturn));
/* Fatal error: red screen with `details`, log line, waits for START (not in a lab run), then returns. */
void NpPlatformFatal(const char* category, const char* details);

#ifdef __cplusplus
}
#endif
#endif
