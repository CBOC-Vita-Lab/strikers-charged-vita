// PS Vita platform layer of the native port: the pieces shared by its source files.
//
// Ported from the recompiled build's layer (one/vita/src/vita_platform.h): the guest-memory,
// fiber and split-worker parts are gone (the game runs natively), the namespaces and file names
// are kept so that fixes can be carried both ways. Most of this layer is ordinary C++ over a thin
// system boundary and is also built for armhf Linux (platform/vita/Makefile, target `check`) so
// that qemu runs the logic the console will run: the audio queue, the text console, the pad
// mapping, the job pool. Only code inside `#if defined(__vita__)` is specific to the console.
#pragma once

#include "np_vita_core3.h"
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
// 1.2: one line of figures about memory, for the player's log (the C heap: in use, free and in how many pieces; the system's free main memory and video
// memory; the renderer's frame buffers). Returns its length. Heap-free, safe from any thread and from a failing allocation.
size_t MemoryLine(char* out, size_t cap);

// Shows a fatal error (on the Vita: full-screen, waits for a button, then
// returns so the caller can exit). Never throws.
void ShowFatal(const char* category, const char* details) noexcept;

// vita_crash.cpp (Vita only). Registers the kubridge exception handler that
// reports a crash on the console and in the log; returns how many of the
// three exception types were accepted.
int InstallCrashHandler();
// galaxyport18 (freeze button): ends the process with a deliberate trap that the system turns into a core dump: our kubridge handler is released first, so the fault goes to the
// system's own handling (psp2core in ux0:data). If it cannot be released the handler itself exits cleanly (no screen, no plan demotion). Does not return.
[[noreturn]] void FreezeTrap();
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
// Modal settings: drain presentation, park game callbacks/audio, and preserve game time.
bool BeginSettings();
void EndSettings();
bool SettingsActive();

// np_vita_process.cpp (Vita only): std::terminate names its exception in the basic log; says so if libstdc++ believes the program is single-threaded.
void InstallFatalHooks();

// Start-up checks of the platform's own logic (pad mapping table, log line splitter, job pool). Cheap; run on every boot and logged as one line.
bool SelfTest();

} // namespace VitaPlatform

// vita_threads.cpp: which thread runs on which core (env.txt: MSC_VITA_CORE_*), and the job
// pool's workers (np_jobs.cpp). The plan and the variables are described in vita_threads.cpp.
namespace VitaThreads {
enum Role { kGame = 0, kRender, kDisplayQueue, kAudioOut, kHelper, kCollector, kPlayer, kWorker, kFrameSend, kSim, kRoleCount };   // kSim (pace18): np_sim, the simulation step thread of MSC_SIMT=1|2 (src/Game/NpSimRun17.cpp); appended: the other roles keep their numbers
// The role's core mask in sceKernel's form (0x10000 << core, or several bits).
int CoreMask(Role role);
// The plan's mask for the role without the fourth core: where to go when the system refuses CoreMask's.
int PlainMask(Role role);
// MSC_VITA_CORE3=1 and a probe thread proved the system core (np_vita_core3.h). Probes once; false without the key. Weak: the settings tests link vita_plan.cpp without this file.
bool Core3Active() __attribute__((weak));
// (1.3) The probe proved the system core and MSC_VITA_CORE3=1 or MSC_VITA_CORE3_SEND=1|2 asked for it (the latter moves only the frame-send worker, and with 2 the
// vitaGL collector). Probes once.
bool Core3Proven() __attribute__((weak));
// (1.3, refused since r16) MSC_VITA_CORE3_SHADERS: always false; the shader compile worker never asks for the fourth core (vita_threads.cpp).
bool EnvOneShaders();
// (1.3) CoreMask's answer before the fourth core was considered (the settings' and plan's core): where a thread goes when the system refuses the fourth.
int PlanMask(Role role);
const char* RoleName(Role role);
// (pace18) np_sim's pin: moves the calling thread to role kSim's core (MSC_VITA_CORE_SIM, alias MSC_SIMT_CORE; never the fourth core) and sets its priority (MSC_VITA_PRIO_SIM, alias MSC_SIMT_PRIO;
// default by `level` = MSC_SIMT in force: 1 the game thread's own, 2 one more urgent than the job workers). *coreMask gets one bit a core. Returns the affinity call's result.
extern "C" int NpVitaPinSimThread(int level, unsigned* coreMask);
// Moves the calling thread to its role's core(s); returns the system call's result (0 off the Vita).
int PinCurrentThread(Role role, bool log = true);
// MSC_VITA_CORE0_ALONE=1|log (1.2, on in the player build; 0 or off turns it off): other threads of the process that may run on core 0 and elsewhere go to cores 1-2 (log: only listed).
// Returns how many were moved; `log` prints the listing in log mode.
int KeepCoreZeroForGame(bool log);
bool SweepInBackground();   // 1.3 MSC_VITA_SWEEP_BG: the 600-frame thread sweeps run on the watch thread
// (T8, np_vita_core3.h rules) The threads on the system core are listed (Core3Register: id and name) for the online guard, which moves them off the core while an online or LAN session is up and back
// afterwards (Core3OnlineStep(NpLanActive() || NpOnlineFlowActive()), every few frames from the presented-frame hook), and for the stray sweep. Only with MSC_VITA_CORE3=1 (rules on) or a job lane on core 3.
void Core3Register(int uid, int retreatPrio = 0);
void Core3Unregister(int uid);   // (core3x15)
void Core3OnlineStep(bool online);
bool g_core3LostNow();
bool g_core3HeldNow();
NpVitaCore3::Rules CurrentRulesNow();
// One line with every role's core, for the log.
void LogPlan();
// One line per live thread (priority, cores, stack, CPU time) for the log; Vita only.
void LogThreadTable(const char* when);
// MSC_VITA_THREAD_CPU=1 (1.3, Galaxy r99 GXV_THREAD_CPU; default 0, logging builds): CPU time of every live thread since the last call, summed by its core mask
// (core 0 only, 1, 2, 1-2, 3, other), with the game thread's part of core 0 and every other thread that may run on core 0. Called with the 600-frame log line.
bool ThreadCpuOn();
void LogThreadCpu(const char* when);
// For the pthread_create wrap: replaces a helper thread's entry by one that first moves the
// thread to the helper role's core. False (nothing changed) when memory is short.
bool WrapHelperThread(void* (**entry)(void*), void** arg);
} // namespace VitaThreads

// vita_plan.cpp: the player build's core plan (C / Cn / A / S) and the guard file that steps down when a start does not get anywhere.
namespace VitaPlan {
int Apply(const char** names, int maxNames);   // after the settings file was read; returns how many variables it set (their names in `names`)
void ForgetDefaults();                         // before a reread of the settings file: takes back the defaults Apply gave (they are decided anew by the next Apply)
bool CancelPendingStart();                    // intentional settings exit/restart: restore the pre-start count if still unresolved
void RestorePendingStart(bool cancelled);     // a failed restart: re-arm only that cancelled, still-unresolved start
void Heartbeat(unsigned long long frames);     // from the stall-watch thread: clears the guard at 300 presented frames
void Failed(const char* why);                  // a stall or a crash: the next start takes the next plan down
void LaunchClean(const char* why);              // stab13 MSC_LAUNCH_GUARD: a clean leave sets the launch marker back to 0 (np_launch_guard.h)
int Level();                                   // 0 C, 1 Cn, 2 A, 3 S, -1 none
const char* LanguageNotice();                  // 1.3: the loading screen's line when the chosen language is not on the disc (English is used), else null
bool IsPsTv();                                 // 1.2: a PS TV (no touch screen, no rear touchpad); MSC_PSTV=1|0 forces the answer
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
// Routes stdout and stderr (C stdio and iostreams) into Write(). crash26b: the wrapped printf family bypasses their FILE (np_log_stdio.h).
void InstallStdioHooks();
// After the plan (env.txt + defaults): MSC_STDIO_LOCK=0 sends the printf family through newlib's FILE again (1.2). Says which in a [stdio] line.
void ApplyStdioMode();
// A [stdio] line (the end of a run, and "alive" every 600 frames): the switch, the wrapped calls and bytes, the format lock's waits / timeouts (the action counter).
void StdioReport(const char* when);
bool StdioLockOn();
// Accepts log bytes from any thread.
void Write(const char* data, size_t size);
void Flush() noexcept;
// Flush() for the end of a process that leaves without exit handlers: waits (up to 2 s) for the locks instead of giving up when the writer thread is busy.
// True when it wrote something. (extern "C" NpVitaLogFlush() = fflush(nullptr) and this, for the harness.)
bool FlushBlocking() noexcept;
// nxr6b: a mid-run flush that never waits for the card. On the Vita it wakes the writer thread (the buffer goes to the file within a scheduling slice, NOT on the calling
// thread); without a writer (host) it is Flush() (try-lock: skips rather than waits). The harness's [script] lines use it (core 0 used to wait behind the writer's sceIoWrite
// three times at the first kick-off). FlushBlocking() stays for the end of a run and the fatal paths. True when a wake or a write was made.
bool FlushAsync() noexcept;
void FlushIfDue();
// While set, every Write() goes to the file before it returns (start-up: the
// last line in log.txt then names what was being attempted). Cleared when
// the guest presents its first frame; the buffered mode holds back at most
// 8 KiB or a quarter of a second.
void SetSynchronous(bool on);
// 1.2: the player's basic record. One line from a fatal path (nlBreak, OSPanic, abort, the fatal screen, a crash) that reaches
// ux0:data/mscharged/log.txt in EVERY build, quiet ones included. Written straight to the card from the calling thread and only
// when such a path is taken (never during normal play); at most 64 lines per run, log.txt is rotated to log.txt.old at 96 KiB
// when the process writes its first line. Safe from a crash handler (no heap, no log lock, bounded).
void Event(const char* text) noexcept;
// galaxyport18: like Event but not counted against the 64 lines (the freeze button's report of every thread: a handful of lines once, written by the user's own act).
void EventUncapped(const char* text) noexcept;
void Note(const char* text) noexcept;   // 1.2: a survived failure, labelled "note" (at most 24 lines per run, separate from Event's 64)
void EventF(const char* format, ...) noexcept __attribute__((format(printf, 1, 2)));
// 1.2: the memory line every 30 s ("mem", at most 240 lines per run = two hours; MSC_VITA_MEMLOG=0 turns the line off): the C heap's use, so that a slow growth
// shows in the player's log before the heap runs out, and the same figures in the FATAL line of a request it could not meet.
void Memory(const char* text) noexcept;
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
bool FatalShown();                      // ShowFatal's report is on the screen (nothing else may be drawn over it)
const uint32_t* Pixels();             // kWidth x kHeight, 0xAABBGGRR
// Settings draw only into the inactive framebuffer. PresentScreen publishes it, waits for scanout retirement, and returns the next writable buffer.
// A null return means drawing must stop. ReleaseScreen gives the console its rows back; Pixels always identifies the front buffer.
uint32_t* TakeScreen();
uint32_t* PresentScreen();
void ReleaseScreen(bool preservePixels = false);
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
void Notice(const char* text);                       // 1.3: one line under the bar for the first seconds (the settings guard's); null: none
void Retrying();                                     // a start-up file is being tried again (np_boot_io.h): the line under the bar says so
void Progress(int kind, unsigned done, unsigned total);   // the renderer: seed copy, shader warm-up, presents
void Frame(uint64_t frames);                         // a game frame was presented
void Finish(const char* why);                        // the game shows its own first picture: run to the end and fade out
int Compose(void* framebuffer);                      // the renderer's display callback: 1 = the buffer now holds the loading screen
void Pause();                                        // the settings screen takes the console's framebuffer
void Resume();
void Stop();                                         // a fatal error: the console takes over at once
bool TestLoad(const char* path);                     // tests: the picture (or the plain screen), one frame
void TestRender(uint32_t* framebuffer, float shown, double now, float brightness);
void TestFade(const unsigned char* picture, double seconds, uint32_t* framebuffer, bool finish);   // tests: the made picture (a loading.bin image) fading in over the plain background; finish: it becomes the background
} // namespace VitaBootScreen

namespace VitaInput {
// Raw controller state as sceCtrl reports it.
struct Raw {
    uint32_t buttons = 0;             // SCE_CTRL_* bits
    uint8_t lx = 128, ly = 128;       // 0..255, 128 = centre, y grows downwards
    uint8_t rx = 128, ry = 128;
    uint32_t newestButtons = 0;       // wave 9: the newest sample's buttons alone (buttons also holds the latched taps)
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
bool ReadDebug(Raw& raw);   // passive observer (renderer debug chords): does not consume the game's tap history
void InvalidateTapHistory();
void TapCarryHold(bool on);   // wave 9 MSC_TAP_CARRY
void TapCarryClear();
// Last state Map() produced, for the console status line.
void LastState(uint16_t* buttons, int* stickX, int* stickY);
} // namespace VitaInput

namespace VitaAudio {
// Freeze queue consumption, not just volume. The caller parks the SDK producer.
bool Pause(bool paused);
// The console woke up after a suspend (the Home menu, standby): the output port is closed and opened again by the output thread. Any thread; no waiting.
void NotifyResume();
uint32_t Reopens();               // how many times the output port was opened again (a wake-up, or the port stopped taking sound)
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
