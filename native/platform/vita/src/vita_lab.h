// Lab mode: a remote test loop inside the application itself (vita_lab.cpp has the whole
// description: the command server, the watchdog, how a run ends and the application comes
// back). Built only with LAB=1 (-DMSC_LAB=1); without it every function here is an empty
// inline and the executable contains none of it.
#pragma once

namespace VitaLab {

#if defined(__vita__) && defined(MSC_LAB)

// The log file this process starts with: the command server's own (lab.txt) in lab mode,
// log.txt otherwise. Decides lab mode on first call (LAB build and no ux0:data/mscharged/nolab).
const char* EarlyLogName();
// main(), before anything else of the platform start-up: serves commands until RUN (or
// START on the pad), then returns and the game boots in this process. `reloadEnv` re-reads
// env.txt (a client usually uploads a new one before RUN).
void ServeBeforeRun(void (*reloadEnv)());
// A lab run is in progress: nobody is at the console (fatal screens do not wait for START)
// and every way out of the process leads back into the command server.
bool RunActive();
// Why the run is about to end, for the status the server reports afterwards (printf style;
// the first reason given wins). The exit itself may then be any exit call.
void EndReason(const char* format, ...) __attribute__((format(printf, 1, 2)));
// Ends the run: last log line, status file, reload of app0:eboot.bin; plain exit if the
// reload is refused. Does not return. Outside a lab run: plain exit.
[[noreturn]] void EndRun(int code);
// From the crash handler, after the report is on the screen and in the log: hands the reload
// to the watchdog thread and, if that does not happen, tries it from here. Returns only
// when no lab run is active.
void CrashEnd();
// The real sceKernelExitProcess (lab builds wrap that symbol).
[[noreturn]] void RealExit(int code);
// Game thread, once per presented frame: the deliberate-failure switches (MSC_LAB_CRASH_AT).
void OnFramePresented();

#else

inline const char* EarlyLogName() { return "log.txt"; }
inline void ServeBeforeRun(void (*)()) {}
inline bool RunActive() { return false; }
inline void EndReason(const char*, ...) {}
inline void CrashEnd() {}
inline void OnFramePresented() {}

#endif

} // namespace VitaLab
