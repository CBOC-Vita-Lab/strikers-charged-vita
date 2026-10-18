// The host program entry (host32, armhf, x86-64 Linux): the strong `main` that overrides NPB's weak
// one (platform/compat/np_main.cpp). What it adds, all optional through the environment:
//   - NP_DATA_DIR      the working directory of the run (the SDK layer's saves, logs, settings)
//   - a crash handler  SIGSEGV / SIGBUS / SIGFPE / SIGILL / SIGABRT print "[crash] ..." and a backtrace
//                      (glibc backtrace_symbols_fd; run with -rdynamic for names), on its own stack so that
//                      a stack overflow is reported too; the process then ends with status 139.
//                      MSC_NO_CRASH_HANDLER=1 leaves the signals to the debugger / qemu.
//   - the job pool     NpJobsInit(0) when platform/vita/src/np_jobs.cpp is linked (weak reference)
//   - stderr line-buffered, so that a killed run keeps its last lines
// then calls the game's main (renamed np_game_main by -Dmain=np_game_main).
#if !defined(__vita__)

#include <csignal>
#include <initializer_list>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <ucontext.h>
#if defined(__linux__) && defined(__GLIBC__)
#include <execinfo.h>
#endif

int np_game_main();
extern "C" unsigned NpJobsInit(unsigned lanes) __attribute__((weak));

namespace {
void CrashHandler(int sig, siginfo_t* info, void* ctx) {
    static volatile sig_atomic_t entered = 0;
    if (entered++) _exit(139);
    char line[160];
    const int n = std::snprintf(line, sizeof(line), "[crash] signal %d at address %p\n", sig, info ? info->si_addr : nullptr);
    if (n > 0) { ssize_t r = write(2, line, static_cast<size_t>(n)); (void)r; }
#if defined(__linux__) && defined(__GLIBC__) && defined(__arm__)
    // (NPP2) where it happened: pc, lr, sp and r0..r12 (the backtrace below is empty without frame pointers)
    if (ctx) {
        const mcontext_t& m = static_cast<const ucontext_t*>(ctx)->uc_mcontext;
        char regs[256];
        const int k = std::snprintf(regs, sizeof(regs), "[crash] pc=%08lx lr=%08lx sp=%08lx r0-r3=%08lx %08lx %08lx %08lx r4-r7=%08lx %08lx %08lx %08lx r8-r12=%08lx %08lx %08lx %08lx %08lx\n",
            m.arm_pc, m.arm_lr, m.arm_sp, m.arm_r0, m.arm_r1, m.arm_r2, m.arm_r3, m.arm_r4, m.arm_r5, m.arm_r6, m.arm_r7, m.arm_r8, m.arm_r9, m.arm_r10, m.arm_fp, m.arm_ip);
        if (k > 0) { ssize_t r = write(2, regs, static_cast<size_t>(k)); (void)r; }
    }
#endif
#if defined(__linux__) && defined(__GLIBC__)
    void* frames[40];
    const int count = backtrace(frames, 40);
    backtrace_symbols_fd(frames, count, 2);
#endif
    _exit(139);
}
void InstallCrashHandler() {
    const char* off = std::getenv("MSC_NO_CRASH_HANDLER");
    if (off && *off && *off != '0') return;
    static char altStack[64 * 1024];
    stack_t ss;
    ss.ss_sp = altStack; ss.ss_size = sizeof(altStack); ss.ss_flags = 0;
    sigaltstack(&ss, nullptr);
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = CrashHandler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT}) sigaction(sig, &sa, nullptr);
}
} // namespace

extern "C" int main(int, char**) {
    setvbuf(stderr, nullptr, _IOLBF, 0);
    if (const char* dir = std::getenv("NP_DATA_DIR")) {
        if (*dir && chdir(dir) != 0) std::fprintf(stderr, "[np] NP_DATA_DIR=%s: cannot enter it\n", dir);
    }
    InstallCrashHandler();
    if (NpJobsInit) NpJobsInit(0);
    return np_game_main();
}

#endif
