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

uint64_t np_time_real_ns(void) { return (uint64_t)sceKernelGetSystemTimeWide() * 1000ull; }
uint64_t np_time_ns(void) {
    NpTimeSource f = g_time_source;
    return f ? f() : np_time_real_ns();
}
void np_time_set_source(NpTimeSource fn) { g_time_source = fn; }

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
static volatile uint32_t g_lastVblankCount = 0;
static volatile uint64_t g_lastVblankNs = 0;
int64_t np_display_vblank_wait(void) {
    if (sceDisplayWaitVblankStart() < 0) return -1;
    const uint32_t v = (uint32_t)sceDisplayGetVcount();
    g_lastVblankNs = np_time_real_ns();
    g_lastVblankCount = v;
    return (int64_t)v;
}
extern "C" int np_display_last_vblank(uint32_t* vcount, uint64_t* ns) {
    // (two reads of a pair written by one thread: re-read when the count moved in between)
    for (int i = 0; i < 3; ++i) {
        const uint32_t v = g_lastVblankCount;
        const uint64_t t = g_lastVblankNs;
        if (v == g_lastVblankCount && t) { *vcount = v; *ns = t; return 1; }
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

namespace {

const char* const kRoleName[NP_ROLE_COUNT] = {"GAME", "OSTHREAD", "AUDIO", "IO", "TIMER", "HELPER"};

const int kUser0 = SCE_KERNEL_CPU_MASK_USER_0;
const int kUserAll = SCE_KERNEL_CPU_MASK_USER_ALL;

// "0" / "1" / "2", several digits for a set, "any". 0 = not understood.
int parse_mask(const char* text) {
    if (!text || !*text) return 0;
    if (!strcmp(text, "any") || !strcmp(text, "all")) return kUserAll;
    int mask = 0;
    for (const char* c = text; *c; ++c) {
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

void apply_role(const char* name, NpRole role) {
    if ((unsigned)role >= NP_ROLE_COUNT) role = NP_ROLE_HELPER;
    SceUID me = sceKernelGetThreadId();
    int mask = role_mask(role);
    // (W5 PIPE) The movie decode lanes (thp_native.cpp, "thp-decode"): NP_VITA_CORE_THP = 0, 1, 2, a set like 12, or any places them apart from the other helpers; not set: the helper role's core.
    if (name && !strcmp(name, "thp-decode")) {
        const int thp = parse_mask(getenv("NP_VITA_CORE_THP"));
        if (thp) mask = thp;
    }
    sceKernelChangeThreadCpuAffinityMask(me, mask);
    int prio = role_priority(role);
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
    t->id = sceKernelCreateThread(t->name, thread_entry, prio, (SceSize)stack_size, 0, 0, NULL);
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
