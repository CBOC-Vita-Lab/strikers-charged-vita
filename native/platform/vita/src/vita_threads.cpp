// Which thread runs on which core, in one place, settable from env.txt (the recompiled build's
// vita_threads.cpp without the split worker, plus the job pool's role).
//
// A Vita gives an application three cores (a fourth only through a kernel plug-in, CapUnlocker,
// which is the owner's decision: see np/docs/vita.md). The threads that matter:
//   game     the main thread: the Wii game's task loop (simulation, animation, frame building) and
//            the SDK layer called from it
//   render   the renderer's player and GL back end (vgx on vitaGL), and with it sceGxm's
//            display-queue thread ("gxmqueue")
//   audio    the audio output thread (vita_audio.cpp): blocks in sceAudioOutOutput
//   helper   threads the SDK layer creates through std::thread: the AX mixer, texture workers
//   gc       vitaGL's collector thread
//   player   the renderer's optional third thread (MSC_VGX_BACKEND_THREAD=1)
//   worker   the job pool's workers (np_jobs.cpp): fork-join jobs lent to the game thread
//   framesend the frame-send worker (np_framesend.cpp): the End Frame task's send of the finished frame (default: core 2, with audio and the mixer)
//
// DEFAULT PLAN (np/docs/vita.md "Core plan" has the reasoning and the numbers):
//   core 0   game
//   core 1   render, gxm display queue, worker 1
//   core 2   audio output, helpers (mixer), gc, player, worker 0
// Workers sit on the two cores that have idle time and run at a priority ABOVE the render
// thread's (np_jobs.cpp), so a job is never queued behind a 15 ms render burst.
//
// env.txt: MSC_VITA_CORE_GAME / _RENDER / _GXMQUEUE / _AUDIO / _HELPER / _GC / _PLAYER / _WORKER
// / _FRAMESEND = 0, 1 or 2; several digits for a set ("12" = core 1 or 2); "any" for all three.
// The same file is built for armhf Linux (target `check`): there the roles are only parsed and logged.
#include "np_vita_internal.h"
#include "../../../include/np/touch_diag.h"
#pragma weak NpDiagRegisterThread

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__vita__)
#include <psp2/kernel/threadmgr.h>
// (W5 FPS) np_fps_probe.cpp: every pinned thread is listed for the per-core busy time that MSC_FPS=auto reads. Weak: a build without it does not list them.
extern "C" void NpFpsProbeRegisterThread(int tid, int kind) __attribute__((weak));
#endif

namespace VitaThreads {
namespace {

constexpr int kUser0 = 0x00010000;   // SCE_KERNEL_CPU_MASK_USER_0; cores 1 and 2 are the next two bits
constexpr int kUserAll = 0x00070000;
#if defined(__vita__)
static_assert(kUser0 == SCE_KERNEL_CPU_MASK_USER_0 && kUserAll == SCE_KERNEL_CPU_MASK_USER_ALL, "core mask constants");
#endif

struct RoleInfo {
    const char* name;      // in log lines
    const char* variable;  // in env.txt
};
constexpr RoleInfo kRoles[kRoleCount] = {
    {"game", "MSC_VITA_CORE_GAME"},   {"render", "MSC_VITA_CORE_RENDER"},     {"gxm display queue", "MSC_VITA_CORE_GXMQUEUE"},
    {"audio output", "MSC_VITA_CORE_AUDIO"}, {"helper", "MSC_VITA_CORE_HELPER"}, {"vitaGL collector", "MSC_VITA_CORE_GC"},
    {"record player", "MSC_VITA_CORE_PLAYER"}, {"job worker", "MSC_VITA_CORE_WORKER"}, {"frame send", "MSC_VITA_CORE_FRAMESEND"},
};

// "0" / "1" / "2", several digits for a set, "any" / "all". 0 = not understood.
int ParseMask(const char* text) {
    if (!text || !*text) return 0;
    if (!std::strcmp(text, "any") || !std::strcmp(text, "all")) return kUserAll;
    int mask = 0;
    for (const char* c = text; *c; ++c) {
        if (*c < '0' || *c > '2') return 0;
        mask |= kUser0 << (*c - '0');
    }
    return mask;
}

int DefaultMask(Role role) {
    switch (role) {
    case kGame: return kUser0;
    case kRender: return kUser0 << 1;
    case kDisplayQueue: return CoreMask(kRender);
    case kWorker: return (kUser0 << 1) | (kUser0 << 2);
    default: return kUser0 << 2;   // audio, helper, gc, player
    }
}

void MaskText(int mask, char* out, size_t size) {
    if (mask == kUserAll) { std::snprintf(out, size, "any"); return; }
    size_t n = 0;
    for (int core = 0; core < 3 && n + 1 < size; ++core)
        if (mask & (kUser0 << core)) out[n++] = static_cast<char>('0' + core);
    out[n] = '\0';
}

} // namespace

int CoreMask(Role role) {
    static int cached[kRoleCount];   // 0 = not decided yet
    if (role < 0 || role >= kRoleCount) return kUserAll;
    if (cached[role]) return cached[role];
    int mask = 0;
    if (const char* text = std::getenv(kRoles[role].variable); text && *text) {
        mask = ParseMask(text);
        if (!mask) std::fprintf(stderr, "[vita] %s=%s not understood (0, 1, 2, a set like 12, or any): using the default\n", kRoles[role].variable, text);
    }
    if (!mask) mask = DefaultMask(role);
    cached[role] = mask;
    return mask;
}

const char* RoleName(Role role) { return role >= 0 && role < kRoleCount ? kRoles[role].name : "?"; }

int PinCurrentThread(Role role, bool log) {
    const int mask = CoreMask(role);
    int result = 0;
#if defined(__vita__)
    result = sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), mask);
    if(NpDiagRegisterThread)NpDiagRegisterThread((uint32_t)role,(uint32_t)sceKernelGetThreadId());
    // kind: 2 the game, 1 work that follows time (audio output, mixer, helpers), 0 per frame
    if (NpFpsProbeRegisterThread) NpFpsProbeRegisterThread(sceKernelGetThreadId(), role == kGame ? 2 : (role == kAudioOut || role == kHelper) ? 1 : 0);
#endif
    if (VitaMem::RegisterThread) VitaMem::RegisterThread(RoleName(role));
    if (log) {
        char cores[8];
        MaskText(mask, cores, sizeof(cores));
        std::fprintf(stderr, "[vita] thread \"%s\" on core %s (mask 0x%X): %s (0x%X)\n", RoleName(role), cores, static_cast<unsigned>(mask),
                     result >= 0 ? "ok" : "REFUSED", static_cast<unsigned>(result));
    }
    return result;
}

void LogPlan() {
    char line[320];
    size_t n = 0;
    for (int role = 0; role < kRoleCount; ++role) {
        char cores[8];
        MaskText(CoreMask(static_cast<Role>(role)), cores, sizeof(cores));
        const int wrote = std::snprintf(line + n, sizeof(line) - n, "%s%s %s", n ? ", " : "", kRoles[role].name, cores);
        if (wrote < 0 || static_cast<size_t>(wrote) >= sizeof(line) - n) break;
        n += static_cast<size_t>(wrote);
    }
    std::fprintf(stderr, "[vita] core plan: %s (MSC_VITA_CORE_* in env.txt changes it)\n", line);
}

// ---- helper threads: the SDK layer's std::thread objects (np_vita_process.cpp wraps pthread_create)
namespace {
struct HelperStart {
    void* (*entry)(void*);
    void* arg;
};
void* HelperTrampoline(void* raw) {
    const HelperStart start = *static_cast<HelperStart*>(raw);
    std::free(raw);
    PinCurrentThread(kHelper, false);   // (also lists the thread's stack for the memory report)
    return start.entry(start.arg);
}
} // namespace

bool WrapHelperThread(void* (**entry)(void*), void** arg) {
    HelperStart* start = static_cast<HelperStart*>(std::malloc(sizeof(HelperStart)));
    if (!start) return false;
    start->entry = *entry;
    start->arg = *arg;
    *entry = HelperTrampoline;
    *arg = start;
    return true;
}

} // namespace VitaThreads
