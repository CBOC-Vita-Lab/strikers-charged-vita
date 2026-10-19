// PS Vita platform layer of the native port: the pieces shared by its source files.
//
// Ported from the recompiled build's layer (one/vita/src/vita_platform.h): the guest-memory,
// fiber and split-worker parts are gone (the game runs natively), the namespaces and file names
// are kept so that fixes can be carried both ways. Most of this layer is ordinary C++ over a thin
// system boundary and is also built for armhf Linux (platform/vita/Makefile, target `check`) so
// that qemu runs the logic the console will run: the audio queue, the text console, the pad
// mapping, the job pool. Only code inside `#if defined(__vita__)` is specific to the console.
#pragma once

#include <cstddef>
#include <cstdint>

#ifndef NP_LOGGING
#define NP_LOGGING 1
#endif

struct NpPadState;

namespace VitaPlatform {

// Everything the port reads and writes lives under ux0:data/mscharged (the same game files as the
// recompiled build). std::filesystem treats "ux0:data/..." as a relative path (absolute(),
// canonical() and relative() would prepend the working directory), so the directory is named
// in POSIX form and the working directory is set to it at startup: VitaSDK's newlib resolves a
// path with a leading '/' on the drive of the working directory.
inline constexpr const char* kDataDevicePath = "ux0:/data/mscharged";   // for sceIo* and chdir
inline constexpr const char* kDataDirectory = "/data/mscharged";        // for std::filesystem
inline constexpr const char* kPackageDirectory = "app0:";               // read-only .vpk contents
inline constexpr const char* kLogFileName = "log.txt";

// Boot progress line on the console and in the log ("[vita] stage: <text>  [+N ms]", N = ms since the process started).
void BootStage(const char* text);
unsigned long BootMs();

// Called once per presented frame (np_harness.h: NpOnFramePresented calls it through
// NpVitaOnFramePresented): frame/fps accounting, console refresh, periodic log flush.
void OnFramePresented();
uint64_t FrameCount();

// Shows a fatal error (on the Vita: full-screen, waits for a button, then
// returns so the caller can exit). Never throws.
void ShowFatal(const char* category, const char* details) noexcept;

// vita_crash.cpp (Vita only). Registers the kubridge exception handler that
// reports a crash on the console and in the log; returns how many of the
// three exception types were accepted.
int InstallCrashHandler();
// For the probe: while armed, a fault makes the faulting leaf function return
// kFaultMarker in r0 instead of ending the program.
inline constexpr uint32_t kFaultMarker = 0xFA017EDu;
void ArmFaultRecovery(bool on);
int FaultCount();
void LastFault(uint32_t* address, uint32_t* pc, uint32_t* type);

// np_vita_wire.cpp (Vita only): registers the audio sink, the input source, the quit hook and the thread hook
// of NPS's SDK layer and the services of NPR's renderer, whichever of them is linked (weak references).
// Returns how many of those hooks were available.
int WireSdkAndRenderer();

// Start-up checks of the platform's own logic (pad mapping table, log line splitter, job pool). Cheap; run on every boot and logged as one line.
bool SelfTest();

} // namespace VitaPlatform

// vita_threads.cpp: which thread runs on which core (env.txt: MSC_VITA_CORE_*), and the job
// pool's workers (np_jobs.cpp). The plan and the variables are described in vita_threads.cpp.
namespace VitaThreads {
enum Role { kGame = 0, kRender, kDisplayQueue, kAudioOut, kHelper, kCollector, kPlayer, kWorker, kFrameSend, kRoleCount };
// The role's core mask in sceKernel's form (0x10000 << core, or several bits).
int CoreMask(Role role);
const char* RoleName(Role role);
// Moves the calling thread to its role's core(s); returns the system call's result (0 off the Vita).
int PinCurrentThread(Role role, bool log = true);
// One line with every role's core, for the log.
void LogPlan();
// For the pthread_create wrap: replaces a helper thread's entry by one that first moves the
// thread to the helper role's core. False (nothing changed) when memory is short.
bool WrapHelperThread(void* (**entry)(void*), void** arg);
} // namespace VitaThreads

// vita_plan.cpp: the player build's core plan (C / A / S) and the guard file that steps down when a start does not get anywhere.
namespace VitaPlan {
int Apply(const char** names, int maxNames);   // after the settings file was read; returns how many variables it set (their names in `names`)
void Heartbeat(unsigned long long frames);     // from the stall-watch thread: clears the guard at 900 presented frames
void Failed(const char* why);                  // a stall or a crash: the next start takes the next plan down
int Level();                                   // 0 C, 1 A, 2 S, -1 none
} // namespace VitaPlan

// vita_probe.cpp: the real CPU clock (a calibrated dependent-add loop) and the fourth-core probe (PIPE, wave 5).
namespace VitaProbe {
double MeasureMHz(uint32_t tries = 5, uint32_t windowMs = 20);   // best of `tries` windows, in MHz
void LogClock(const char* when);                                  // one "[clock]" line
void StartClockMonitor();                                         // MSC_VITA_CLOCK_PERIOD=<s>: log it again every <s> seconds
void RunCore4Probe();                                             // MSC_VITA_PROBE_CORE4=1|only: see vita_probe.cpp
} // namespace VitaProbe

// vita_memreport.cpp (Vita game packages only; weak, so that the probe and the check build,
// which do not link it, need no stand-ins: test the function's address before calling).
// What the process uses in each of the console's memory budgets; see that file.
namespace VitaMem {
void SampleHeap() __attribute__((weak));                    // C heap in use -> its high-water mark
void Checkpoint(const char* when) __attribute__((weak));    // one "[mem]" line
void ReportNow(const char* why) __attribute__((weak));      // the full table
void RegisterThread(const char* name) __attribute__((weak)); // the calling thread's stack, for the table
void RegisterArena(const char* name, void* base, size_t size) __attribute__((weak)); // a game arena, for the table
} // namespace VitaMem

namespace VitaLog {
constexpr int kTailColumns = 80;
constexpr int kTailLines = 16;

// Appends the process's log to <devicePath>/<fileName> (rotated to
// <fileName>.old once it passes 2 MiB). Safe to call before the directory exists.
void Open(const char* devicePath, const char* fileName = VitaPlatform::kLogFileName);
// Routes stdout and stderr (C stdio and iostreams) into Write().
void InstallStdioHooks();
// Accepts log bytes from any thread.
void Write(const char* data, size_t size);
void Flush() noexcept;
void FlushIfDue();
// While set, every Write() goes to the file before it returns (start-up: the
// last line in log.txt then names what was being attempted). Cleared when
// the guest presents its first frame; the buffered mode holds back at most
// 8 KiB or a quarter of a second.
void SetSynchronous(bool on);
// Copies the newest lines, already wrapped to kTailColumns, oldest first.
// Returns how many; `generation` changes whenever the tail does.
int Tail(char lines[][kTailColumns + 1], int maxLines, uint32_t* generation);
// Vita only, used by lab builds (vita_lab.cpp; described in vita_log.cpp):
bool SwitchFile(const char* newPath, const char* keepOldAs);
const char* CurrentPath();
void Pause();
void Resume();
void EmergencyLine(const char* text) noexcept;
} // namespace VitaLog

namespace VitaConsole {
constexpr int kColumns = 80;
constexpr int kRows = 22;
constexpr int kStatusRows = 6;        // rows 0-5: title and status; the rest: log tail
constexpr int kWidth = 960;
constexpr int kHeight = 544;

bool Init();
void SetLine(int row, uint32_t rgb, const char* format, ...) __attribute__((format(printf, 3, 4)));
// Redraws rows whose text changed and pulls in the log tail if it changed.
void Refresh();
// Refresh, but at most every `minIntervalMs` and only on the thread that
// called Init() (log output can come from any thread).
void RefreshThrottled(unsigned minIntervalMs);
// Replaces the whole screen with an error report.
void ShowFatal(const char* category, const char* details);
// How long ShowFatal leaves the report up waiting for START (default two minutes). A lab run
// sets 0: nobody is there to press it, and the run has to end.
void SetFatalWaitMs(unsigned ms);
// The graphics build hands the display to the renderer: while suspended the
// console keeps its text up to date in memory but does not touch the display
// (ShowFatal takes the display back).
void SetDisplayOwner(bool consoleOwnsDisplay);
bool OwnsDisplay();
const uint32_t* Pixels();             // kWidth x kHeight, 0xAABBGGRR
// A full-screen picture of the caller's own (the Settings screen, np_vita_settings.cpp): TakeScreen returns the framebuffer (kWidth x kHeight, 0xAABBGGRR; the
// text rows and the log tail stop drawing), PresentScreen hands it to the display, ReleaseScreen gives the console its rows back.
uint32_t* TakeScreen();
void PresentScreen();
void ReleaseScreen();
bool DumpPpm(const char* path);       // check builds: write the screen as a picture
// The text (status rows and log tail) is drawn only once ShowText() was called (W5 BOOT: the player build's loading screen has the
// framebuffer until the game shows its first picture; vita_bootscreen.cpp). Any thread; the drawing happens at the next Refresh.
void ShowText();
} // namespace VitaConsole

// vita_bootscreen.cpp (W5 BOOT): the loading screen of the player build, from the process's first frame to the game's first picture.
namespace VitaBootScreen {
enum { kSeedCopy = 1, kWarmup = 2, kPresent = 3 };
void Begin();                                        // after the settings file was read: decides art or text (MSC_BOOT_SCREEN) and starts
bool Active();                                       // the loading screen has the screen
void Stage(const char* text);                        // a boot stage (VitaPlatform::BootStage)
void Progress(int kind, unsigned done, unsigned total);   // the renderer: seed copy, shader warm-up, presents
void Frame(uint64_t frames);                         // a game frame was presented
void Finish(const char* why);                        // the game shows its own first picture: run to the end and fade out
int Compose(void* framebuffer);                      // the renderer's display callback: 1 = the buffer now holds the loading screen
void Pause();                                        // the settings screen takes the console's framebuffer
void Resume();
void Stop();                                         // a fatal error: the console takes over at once
bool TestLoad(const char* path);                     // tests: the picture (or the plain screen), one frame
void TestRender(uint32_t* framebuffer, float shown, double now, float brightness);
} // namespace VitaBootScreen

namespace VitaInput {
// Raw controller state as sceCtrl reports it.
struct Raw {
    uint32_t buttons = 0;             // SCE_CTRL_* bits
    uint8_t lx = 128, ly = 128;       // 0..255, 128 = centre, y grows downwards
    uint8_t rx = 128, ry = 128;
};
// Bits of Raw::buttons (the values of the SCE_CTRL_* constants).
enum : uint32_t {
    kSelect = 0x0001, kStart = 0x0008, kUp = 0x0010, kRight = 0x0020, kDown = 0x0040, kLeft = 0x0080,
    kLTrigger = 0x0100, kRTrigger = 0x0200, kTriangle = 0x1000, kCircle = 0x2000, kCross = 0x4000, kSquare = 0x8000,
};
// Vita controls -> the GameCube pad (the nav scripts' and the SDK layer's PAD form).
void Map(const Raw& raw, NpPadState& status);
// Reads the console's controls. False when no sample is available.
bool Read(Raw& raw);
// Last state Map() produced, for the console status line.
void LastState(uint16_t* buttons, int* stickX, int* stickY);
} // namespace VitaInput

namespace VitaAudio {
struct Stats {
    uint32_t queuedFrames = 0;        // currently in the ring
    uint32_t droppedBlocks = 0;       // pushes refused because the ring was full
    uint32_t underruns = 0;           // times playback ran dry (one per gap, not one per silent period)
    uint32_t sampleRate = 0;
    bool open = false;                // the output port/thread is running
    // The queue's own account (vita_audio_queue.h):
    bool elastic = false;             // MSC_AUDIO_QUEUE: elastic (default) / fixed
    uint32_t silentPeriods = 0;       // 512-frame periods (16 ms) of silence handed to the device
    uint32_t trims = 0;               // forward skips of the elastic queue
    uint32_t trimmedFrames = 0;
    uint32_t startFrames = 0;         // the level playback (re)starts at, now
    uint32_t maxQueued = 0;           // highest fill after a push
};
Stats GetStats();
// One log line "[vita] audio at <when>: ..." with the queue's account and the runtime's tick
// counters (MscAudioPaceGet, when the runtime has them): every 600 frames and at a run's end.
void LogReport(const char* when);
} // namespace VitaAudio
