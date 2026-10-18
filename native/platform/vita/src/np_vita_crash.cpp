// Crash reporting through kubridge's exception hook: data abort, prefetch abort and undefined
// instruction are shown on the text console and written to the log with the register state, then
// the program exits. Ported from the recompiled build's vita_crash.cpp (one/vita/src) without its
// guest-memory parts.
//
// kubridge is OPTIONAL here (the native port needs no fixed addresses): the platform links the
// WEAK import stubs (libkubridge_stub_weak.a, tools/gen_kubridge_stubs.sh), and without the
// plug-in the handler is simply not installed: a crash then ends in the system's own dump
// (ux0:data/psp2core-*.psp2dmp; `vita-parse-core` with the build's .elf names the function).
//
// Limits (from kubridge's source, src/exceptions.c):
//  - the handler is only entered when the faulting stack pointer lies inside the faulting thread's
//    own stack (a fault on a job worker or the main thread is covered; a stack overflow is not);
//  - Vita3K stores the handler but only calls it for faults on ranges that were protected through
//    kubridge, which this program never does.
#if defined(__vita__)

#include "np_vita.h"
#include "np_vita_internal.h"
#include "vita_lab.h"

#include <kubridge.h>
#include <psp2/kernel/processmgr.h>

#include <cstdio>

// Weak: absent when kubridge.skprx is not loaded.
extern "C" int kuKernelRegisterExceptionHandler(SceUInt32, KuKernelExceptionHandler, KuKernelExceptionHandler*, KuKernelExceptionHandlerOpt*) __attribute__((weak));

namespace {

bool g_recoveryArmed = false;
int g_faults = 0;
uint32_t g_lastFaultAddress = 0, g_lastFaultPc = 0, g_lastFaultType = 0;

void CrashHandler(KuKernelExceptionContext* ctx) {
    // Probe support: return from the faulting leaf function with a marker in r0 (pc <- lr,
    // Thumb state from lr's low bit).
    if (g_recoveryArmed) {
        ++g_faults;
        g_lastFaultAddress = ctx->FAR;
        g_lastFaultPc = ctx->pc;
        g_lastFaultType = ctx->exceptionType;
        ctx->r0 = VitaPlatform::kFaultMarker;
        ctx->SPSR = (ctx->SPSR & ~0x20u) | ((ctx->lr & 1u) << 5);
        ctx->pc = ctx->lr & ~1u;
        return;
    }
    static int entered = 0;
#if defined(MSC_LAB)
    if (entered != 0) VitaLab::RealExit(1);   // a fault inside this handler: no second attempt at anything
#else
    if (entered != 0) sceKernelExitProcess(1);
#endif
    entered = 1;
    VitaPlan::Failed("crash");   // (the core plan's guard: the next start takes the next plan down)
    static const char* const kinds[] = {"data abort", "prefetch abort", "undefined instruction"};
    const char* kind = ctx->exceptionType < 3 ? kinds[ctx->exceptionType] : "exception";
    static char details[900];
    std::snprintf(details, sizeof(details),
                  "%s at pc=0x%08X lr=0x%08X sp=0x%08X\n"
                  "fault address 0x%08X  FSR=0x%08X\n"
                  "r0=%08X r1=%08X r2=%08X r3=%08X\n"
                  "r4=%08X r5=%08X r6=%08X r7=%08X\n"
                  "r8=%08X r9=%08X r10=%08X r11=%08X r12=%08X\n"
                  "The executable is loaded at 0x81000000 (no ASLR); the .map file in the build\n"
                  "directory names the function containing pc.",
                  kind, static_cast<unsigned>(ctx->pc), static_cast<unsigned>(ctx->lr), static_cast<unsigned>(ctx->sp),
                  static_cast<unsigned>(ctx->FAR), static_cast<unsigned>(ctx->FSR),
                  static_cast<unsigned>(ctx->r0), static_cast<unsigned>(ctx->r1), static_cast<unsigned>(ctx->r2), static_cast<unsigned>(ctx->r3),
                  static_cast<unsigned>(ctx->r4), static_cast<unsigned>(ctx->r5), static_cast<unsigned>(ctx->r6), static_cast<unsigned>(ctx->r7),
                  static_cast<unsigned>(ctx->r8), static_cast<unsigned>(ctx->r9), static_cast<unsigned>(ctx->r10), static_cast<unsigned>(ctx->r11),
                  static_cast<unsigned>(ctx->r12));
    VitaLab::EndReason("CRASH: %s at pc=0x%08X lr=0x%08X, fault address 0x%08X", kind, static_cast<unsigned>(ctx->pc), static_cast<unsigned>(ctx->lr),
                       static_cast<unsigned>(ctx->FAR));
    VitaPlatform::ShowFatal("the program crashed", details);
    VitaLog::Flush();
    // A lab run: nobody waits for START (ShowFatal returned at once); the application reloads
    // itself, from the watchdog thread if possible. Returns only outside a lab run.
    VitaLab::CrashEnd();
    sceKernelExitProcess(1);
}

} // namespace

namespace VitaPlatform {

int InstallCrashHandler() {
    if (!kuKernelRegisterExceptionHandler) {
        std::fprintf(stderr, "[vita] kubridge is not loaded: no on-screen crash report (a crash leaves the system's psp2core dump in ux0:data)\n");
        return 0;
    }
    int installed = 0;
    for (SceUInt32 type = 0; type <= KU_KERNEL_EXCEPTION_TYPE_UNDEFINED_INSTRUCTION; ++type) {
        const int res = kuKernelRegisterExceptionHandler(type, CrashHandler, nullptr, nullptr);
        if (res < 0)
            std::fprintf(stderr, "[vita] crash handler %u not registered: 0x%08X\n", static_cast<unsigned>(type), static_cast<unsigned>(res));
        else
            ++installed;
    }
    std::fprintf(stderr, "[vita] crash handler registered for %d of 3 exception types\n", installed);
    return installed;
}

void ArmFaultRecovery(bool on) { g_recoveryArmed = on; }
int FaultCount() { return g_faults; }
void LastFault(uint32_t* address, uint32_t* pc, uint32_t* type) {
    if (address) *address = g_lastFaultAddress;
    if (pc) *pc = g_lastFaultPc;
    if (type) *type = g_lastFaultType;
}

} // namespace VitaPlatform

#endif // __vita__
