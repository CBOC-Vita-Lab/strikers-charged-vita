// VitaSDK implementation of np_plat.h.
//
// Kernel objects instead of pthreads: a thread switch of the SDK's OS scheduler is one semaphore
// post plus one wait, the "interrupt" lock is taken by the game thousands of times a frame (a
// light-weight mutex is a user-space operation when uncontended), and the thread plan (cores,
// priorities) needs the sce calls anyway.
//
// Core plan (nothing of this has run on a console yet, so each role can be overridden in env.txt
// or through np_plat_set_thread_hook()): the OS threads run one at a time (baton), so all of them
// sit on core 0 with the game; helper threads that wait for I/O or a clock (audio, disc/NAND
// workers, alarm and retrace timers, movie decoding) sit on core 2, which the renderer of the
// native port leaves to them. Variables, as in the recompilation's Vita layer:
//   NP_VITA_CORE_<GAME|OSTHREAD|AUDIO|IO|TIMER|HELPER> = 0, 1, 2, a set like "12", or "any"
//   NP_VITA_PRIO_<ROLE>   = a priority as sceKernelChangeThreadPriority takes it (lower = more urgent)
//   NP_VITA_STACK_KIB     = minimum stack of the threads made here (default 128)
#include "np/np_plat.h"
#include "np/modal_clock.h"

#include <atomic>
#include <errno.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <psp2/display.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/error.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

// ---------------------------------------------------------------------------------------------
// Time
static NpTimeSource g_time_source = 0;
static NpModalClock g_modalClock;

uint64_t np_time_real_ns(void) { return (uint64_t)sceKernelGetSystemTimeWide() * 1000ull; }
uint64_t np_time_ns(void) {
    NpTimeSource f = g_time_source;
    return f ? f() : g_modalClock.Now(np_time_real_ns);
}
void np_time_set_source(NpTimeSource fn) { g_time_source = fn; }
extern "C" void np_time_pause(int paused) {
    if (paused) g_modalClock.Pause(np_time_real_ns());
    else g_modalClock.Resume(np_time_real_ns());
}

void np_sleep_ns(uint64_t ns) {
    uint64_t us = (ns + 999u) / 1000u;
    if (us == 0) us = 1;
    while (us > 0x7fffffffull) {   // the delay is an unsigned 32 bit number of microseconds
        sceKernelDelayThread(0x7fffffffu);
        us -= 0x7fffffffull;
    }
    sceKernelDelayThread((SceUInt)us);
}
void np_yield(void) { sceKernelDelayThread(1); }
// W5 RT: when the last vblank the VI thread waited for began (np_display_last_vblank: the input-delay record dates a picture's refresh from it)
// STR-102 (1.2): the pair (count, time) is published through a sequence lock: the VI thread (the only writer) makes the sequence odd, writes both, makes it
// even; a reader takes the pair only when the sequence was even and unchanged across both reads. (The old pair was two plain volatiles: a reader could take
// the old count with the new time, and the check "the count did not move" passed because the writer stored the count last.)
static std::atomic<uint32_t> g_vblankSeq{0};
static std::atomic<uint32_t> g_lastVblankCount{0};
static std::atomic<uint64_t> g_lastVblankNs{0};
int64_t np_display_vblank_wait(void) {
    if (sceDisplayWaitVblankStart() < 0) return -1;
    const uint32_t v = (uint32_t)sceDisplayGetVcount();
    const uint64_t t = np_time_real_ns();
    const uint32_t s = g_vblankSeq.load(std::memory_order_relaxed);
    g_vblankSeq.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    g_lastVblankNs.store(t, std::memory_order_relaxed);
    g_lastVblankCount.store(v, std::memory_order_relaxed);
    g_vblankSeq.store(s + 2, std::memory_order_release);
    return (int64_t)v;
}
extern "C" int np_display_last_vblank(uint32_t* vcount, uint64_t* ns) {
    for (int i = 0; i < 8; ++i) {
        const uint32_t s1 = g_vblankSeq.load(std::memory_order_acquire);
        if (s1 & 1u) continue;   // (being written)
        const uint32_t v = g_lastVblankCount.load(std::memory_order_relaxed);
        const uint64_t t = g_lastVblankNs.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (s1 == g_vblankSeq.load(std::memory_order_relaxed) && t) { *vcount = v; *ns = t; return 1; }
    }
    return 0;
}
int64_t np_display_vcount(void) { return (int64_t)sceDisplayGetVcount(); }

// ---------------------------------------------------------------------------------------------
// Semaphore and mutex
int np_sem_init(NpSem* s, int initial) {
    SceUID id = sceKernelCreateSema("np_sem", 0, initial, 0x7fffffff, NULL);
    if (id < 0) return -1;
    s->p = (void*)(intptr_t)id;
    return 0;
}
void np_sem_destroy(NpSem* s) {
    if (!s->p) return;
    sceKernelDeleteSema((SceUID)(intptr_t)s->p);
    s->p = 0;
}
void np_sem_post(NpSem* s) { sceKernelSignalSema((SceUID)(intptr_t)s->p, 1); }
void np_sem_wait(NpSem* s) { sceKernelWaitSema((SceUID)(intptr_t)s->p, 1, NULL); }
int np_sem_timedwait_ns(NpSem* s, uint64_t timeout_ns) {
    uint64_t us = (timeout_ns + 999u) / 1000u;
    if (us == 0) us = 1;
    if (us > 0xffffffffull) us = 0xffffffffull;
    SceUInt t = (SceUInt)us;
    return sceKernelWaitSema((SceUID)(intptr_t)s->p, 1, &t) >= 0 ? 1 : 0;
}

// A light-weight mutex: the work area is allocated here (8-byte aligned by malloc).
int np_mutex_init(NpMutex* m) {
    SceKernelLwMutexWork* w = (SceKernelLwMutexWork*)memalign(8, sizeof(SceKernelLwMutexWork));
    if (!w) return -1;
    memset(w, 0, sizeof(*w));
    if (sceKernelCreateLwMutex(w, "np_mutex", 0, 0, NULL) < 0) {
        free(w);
        return -1;
    }
    m->p = w;
    return 0;
}
void np_mutex_destroy(NpMutex* m) {
    if (!m->p) return;
    sceKernelDeleteLwMutex((SceKernelLwMutexWork*)m->p);
    free(m->p);
    m->p = 0;
}
void np_mutex_lock(NpMutex* m) { sceKernelLockLwMutex((SceKernelLwMutexWork*)m->p, 1, NULL); }
int np_mutex_trylock(NpMutex* m) { return sceKernelTryLockLwMutex((SceKernelLwMutexWork*)m->p, 1) >= 0; }
void np_mutex_unlock(NpMutex* m) { sceKernelUnlockLwMutex((SceKernelLwMutexWork*)m->p, 1); }

// ---------------------------------------------------------------------------------------------
// Thread plan

// (W5 FPS) np_fps_probe.cpp lists the threads for the per-core busy time MSC_FPS=auto reads. Weak: absent in a build without it.
extern "C" void NpFpsProbeRegisterThread(int tid, int kind) __attribute__((weak));
// (T8) The player build's rules for the system core (vita/src/np_vita_core3.h, vita_threads.cpp). Weak: a build without them places threads as before.
extern "C" int NpVitaCore3Filter(const char* name, int role, int mask) __attribute__((weak));
extern "C" int NpVitaCore3Priority(int mask, int prio) __attribute__((weak));
extern "C" void NpVitaCore3Placed(void) __attribute__((weak));

namespace {

const char* const kRoleName[NP_ROLE_COUNT] = {"GAME", "OSTHREAD", "AUDIO", "IO", "TIMER", "HELPER"};

const int kUser0 = SCE_KERNEL_CPU_MASK_USER_0;
const int kUserAll = SCE_KERNEL_CPU_MASK_USER_ALL;
const int kSystemCore = 0x00080000;   // SCE_KERNEL_CPU_MASK_SYSTEM: "3" below, which vita_plan.cpp writes only after the core was proven (np_vita_core3.h)

// "0" / "1" / "2" / "3", several digits for a set, "any". 0 = not understood.
int parse_mask(const char* text) {
    if (!text || !*text) return 0;
    if (!strcmp(text, "any") || !strcmp(text, "all")) return kUserAll;
    int mask = 0;
    for (const char* c = text; *c; ++c) {
        if (*c == '3') { mask |= kSystemCore; continue; }
        if (*c < '0' || *c > '2') return 0;
        mask |= kUser0 << (*c - '0');
    }
    return mask;
}

int default_mask(NpRole role) {
    switch (role) {
    case NP_ROLE_GAME:
    case NP_ROLE_OSTHREAD: return kUser0;
    default: return kUser0 << 2;
    }
}

int role_mask(NpRole role) {
    static int cached[NP_ROLE_COUNT];   // 0 = not decided yet
    if ((unsigned)role >= NP_ROLE_COUNT) return kUserAll;
    if (cached[role]) return cached[role];
    char var[48];
    snprintf(var, sizeof(var), "NP_VITA_CORE_%s", kRoleName[role]);
    int mask = parse_mask(getenv(var));
    if (!mask) mask = default_mask(role);
    cached[role] = mask;
    return mask;
}

// 0: leave the priority alone (the creator's, i.e. the thread that made it).
int role_priority(NpRole role) {
    char var[48];
    snprintf(var, sizeof(var), "NP_VITA_PRIO_%s", kRoleName[role]);
    const char* text = getenv(var);
    if (text && *text) return (int)strtoul(text, 0, 0);
    switch (role) {
    case NP_ROLE_AUDIO:
    case NP_ROLE_TIMER: return 0x10000100 - 0x20;   // woken by a clock: do not wait behind the game
    default: return 0;
    }
}

NpThreadHook g_thread_hook = 0;

// The mask a thread of this name and role gets (the thp-decode lanes have a line of their own), after the core rules.
int thread_mask(const char* name, NpRole role) {
    if ((unsigned)role >= NP_ROLE_COUNT) role = NP_ROLE_HELPER;
    int mask = role_mask(role);
    if (name && !strcmp(name, "thp-decode")) {
        const int thp = parse_mask(getenv("NP_VITA_CORE_THP"));
        if (thp) mask = thp;
    }
    if ((mask & kSystemCore) && NpVitaCore3Filter) mask = NpVitaCore3Filter(name ? name : "", (int)role, mask);
    return mask;
}

void apply_role(const char* name, NpRole role) {
    if ((unsigned)role >= NP_ROLE_COUNT) role = NP_ROLE_HELPER;
    SceUID me = sceKernelGetThreadId();
    // (W5 PIPE) The movie decode lanes (thp_native.cpp, "thp-decode"): NP_VITA_CORE_THP = 0, 1, 2, a set like 12, or any places them apart from the other helpers; not set: the helper role's core.
    const int mask = thread_mask(name, role);   // (T8: the core rules are applied here)
    const int placed = sceKernelChangeThreadCpuAffinityMask(me, mask);
    if (placed >= 0 && (mask & kSystemCore) && NpVitaCore3Placed) NpVitaCore3Placed();
    if (placed < 0 && (mask & kSystemCore)) {   // a line that names the fourth core, or a core that stopped being there: the role's plain core, never "any"
        fprintf(stderr, "[vita] sdk thread '%s': the fourth core was refused (0x%X), back to the plan's core\n", name ? name : "", (unsigned)placed);
        sceKernelChangeThreadCpuAffinityMask(me, default_mask(role));
    }
    int prio = role_priority(role);
    if ((mask & kSystemCore) && NpVitaCore3Priority) prio = NpVitaCore3Priority(mask, prio);   // (T8) no more urgent than MSC_VITA_CORE3_AUDIO_PRIO on the system core
    // 1.2 (the Galaxy port's VBLANK_PRIO): MSC_VITA_PRIO_VBLANK = a priority in sceKernel's form for the thread that follows the display's vblank
    // ("vi-retrace"), which the game's retrace waits, the movies and the 30 fps cadence all hang on. By default it sits at the timer role's 128, level with the
    // mixer and the audio output on core 2, so a mix burst can hold a retrace back by a time slice; 0x10000100 - 48 (112) puts it ahead of them.
    // Unset, 0 or off: unchanged (the timer role's priority).
    if (name && !strcmp(name, "vi-retrace")) {
        const char* text = getenv("MSC_VITA_PRIO_VBLANK");
        if (text && *text && strcmp(text, "0") != 0 && strcmp(text, "off") != 0) {   // (0 / off: the timer role's priority, as when it is not set; the player build sets 0x100000D0 by default)
            prio = (int)strtoul(text, 0, 0);
            fprintf(stderr, "[vita] vi-retrace thread priority 0x%X (MSC_VITA_PRIO_VBLANK)\n", (unsigned)prio);
        }
    }
    if (prio) sceKernelChangeThreadPriority(me, prio);
    // kind: 2 the game's OS threads (they run one at a time on the game's core), 1 work that follows time (audio, timers, disc workers, helpers)
    if (NpFpsProbeRegisterThread) NpFpsProbeRegisterThread((int)me, (role == NP_ROLE_GAME || role == NP_ROLE_OSTHREAD) ? 2 : 1);
    if (g_thread_hook) g_thread_hook(name, role);
}

}   // namespace

void np_plat_set_thread_hook(NpThreadHook hook) { g_thread_hook = hook; }
void np_thread_apply_role(NpRole role) { apply_role("", role); }

// ---------------------------------------------------------------------------------------------
// Threads
struct NpThread {
    SceUID id;
    char name[24];
    NpRole role;
    NpThreadFn fn;
    void* arg;
    volatile int state;   // 0 running, 1 finished and waiting for join/detach, 2 detached
};

static __thread NpThread* tl_self = 0;

// The end of every thread made here: joinable ones stay (for sceKernelWaitThreadEnd) until
// np_thread_join deletes them, detached ones delete themselves.
static void thread_finish(NpThread* t) {
    int old = __atomic_exchange_n(&t->state, 1, __ATOMIC_ACQ_REL);
    if (old == 2) {
        free(t);
        sceKernelExitDeleteThread(0);
    }
    sceKernelExitThread(0);
    for (;;) {
    }
}

static int thread_entry(SceSize, void* argp) {
    NpThread* t = *(NpThread**)argp;
    tl_self = t;
    apply_role(t->name, t->role);
    t->fn(t->arg);
    thread_finish(t);
    return 0;
}

NpThread* np_thread_create(const char* name, NpRole role, NpThreadFn fn, void* arg, void* /*stack*/,
                           size_t stack_size) {
    NpThread* t = (NpThread*)calloc(1, sizeof(NpThread));
    if (!t) return 0;
    snprintf(t->name, sizeof(t->name), "%s", name ? name : "np");
    t->role = role;
    t->fn = fn;
    t->arg = arg;
    // Native frames are larger than the PowerPC's and the SDK layer sits below them: the game's
    // stack size is a floor, not the size.
    size_t minimum = 128u * 1024u;
    if (const char* text = getenv("NP_VITA_STACK_KIB")) {
        if (*text) minimum = (size_t)strtoul(text, 0, 0) * 1024u;
    }
    if (stack_size < minimum) stack_size = minimum;
    stack_size = (stack_size + 0xfffu) & ~(size_t)0xfffu;
    // Same priority as the creator, same cores until apply_role() decides otherwise.
    SceKernelThreadInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    int prio = 0x10000100;
    if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) >= 0) prio = info.currentPriority;
    // (T8) A thread that belongs on the system core is created there (the mask in sceKernelCreateThread), not moved after it started; the system refusing it is the plain create, and apply_role
    // then moves it as before. Every other thread is created as it always was (mask 0).
    const int wanted = thread_mask(t->name, role);
    t->id = wanted & kSystemCore ? sceKernelCreateThread(t->name, thread_entry, prio, (SceSize)stack_size, 0, wanted, NULL) : -1;
    if (t->id < 0) t->id = sceKernelCreateThread(t->name, thread_entry, prio, (SceSize)stack_size, 0, 0, NULL);
    if (t->id < 0) {
        free(t);
        return 0;
    }
    if (sceKernelStartThread(t->id, sizeof(t), &t) < 0) {
        sceKernelDeleteThread(t->id);
        free(t);
        return 0;
    }
    return t;
}

void np_thread_join(NpThread* t) {
    if (!t) return;
    sceKernelWaitThreadEnd(t->id, NULL, NULL);
    sceKernelDeleteThread(t->id);
    free(t);
}

void np_thread_detach(NpThread* t) {
    if (!t) return;
    int old = __atomic_exchange_n(&t->state, 2, __ATOMIC_ACQ_REL);
    if (old == 1) {   // it has ended already
        sceKernelDeleteThread(t->id);
        free(t);
    }
}

void np_thread_exit(void) {
    NpThread* t = tl_self;
    if (t) thread_finish(t);
    sceKernelExitDeleteThread(0);
    for (;;) {
    }
}

void np_thread_stack_bounds(void** low, void** high) {
    *low = 0;
    *high = 0;
    SceKernelThreadInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) >= 0 && info.stack) {
        *low = info.stack;
        *high = (char*)info.stack + info.stackSize;
    }
}

// ---------------------------------------------------------------------------------------------
// Memory
//
// The system picks the address of a user memory block. A build that can place blocks (kubridge
// in the application's plugin set, see the recompilation's vita_fixedmem.cpp) provides
// np_vita_alloc_fixed(); the SDK layer asks for MEM1/MEM2 at 0x80000000 and 0x90000000 and falls
// back to any address (which is fine for the game as long as AX data stays in the window the
// mixer can address, see docs/sdk.md "AX address rule").
extern "C" void* np_vita_alloc_fixed(uintptr_t base, size_t size) __attribute__((weak));

void* np_mem_map(uintptr_t want_base, size_t size) {
    if (want_base && np_vita_alloc_fixed) {
        void* p = np_vita_alloc_fixed(want_base, size);
        if (p) return p;
    }
    size_t rounded = (size + 0xfffu) & ~(size_t)0xfffu;
    SceUID blk = sceKernelAllocMemBlock("np_mem", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, (SceSize)rounded, NULL);
    if (blk < 0) return 0;
    void* base = 0;
    if (sceKernelGetMemBlockBase(blk, &base) < 0 || !base) {
        sceKernelFreeMemBlock(blk);
        return 0;
    }
    memset(base, 0, size);   // blocks are not promised to be zeroed
    return base;
}

// ---------------------------------------------------------------------------------------------
// Files (newlib over sceIo)
int np_file_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}
int np_dir_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}
int np_mkdir_p(const char* path) {
    char buf[1024];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof(buf)) return -1;
    memcpy(buf, path, n + 1);
    // "ux0:data/x": the device prefix ends at the first ':'; nothing before it is a directory
    size_t start = 1;
    for (size_t i = 0; i < n; ++i) {
        if (buf[i] == ':') {
            start = i + 2;
            break;
        }
        if (buf[i] == '/') break;
    }
    for (size_t i = start; i < n; ++i) {
        if (buf[i] == '/') {
            buf[i] = 0;
            if (!np_dir_exists(buf)) mkdir(buf, 0777);
            buf[i] = '/';
        }
    }
    if (!np_dir_exists(buf)) mkdir(buf, 0777);
    return np_dir_exists(buf) ? 0 : -1;
}

// ---------------------------------------------------------------------------------------------
// Log
void np_log(const char* tag, const char* fmt, ...) {
    char line[512];
    int n = snprintf(line, sizeof(line), "[%s] ", tag ? tag : "np");
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
    va_end(ap);
    fprintf(stderr, "%s\n", line);
}
