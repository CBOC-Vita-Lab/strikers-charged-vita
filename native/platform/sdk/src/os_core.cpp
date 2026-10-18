// OS core: OSInit, the memory arenas, reporting, console identity, reset/shutdown requests, cache
// maintenance (no-ops on a coherent host), contexts, and the other small OS entry points the game
// and the SDK code call. Logic follows the recomp's HLE (one/port/runtime/src/hle/os/*) and the SDK
// sources in np/ref/RVL_SDK/os, without the guest-memory layer.
#include <revolution/os.h>
#include <revolution/base.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "np_internal.h"

static NpSdkConfig g_cfg;
static int g_cfg_set = 0;
static void* g_mem1 = 0;
static void* g_mem2 = 0;
static int g_quit_requested = 0;
static OSShutdownFunctionQueue g_shutdown_queue;
static void (*g_quit_hook)(int) = 0;

extern "C" {

// ---------------------------------------------------------------------------------------------
// Configuration

static const char* env_str(const char* name, const char* def) {
    const char* v = getenv(name);
    return (v && *v) ? v : def;
}
static int env_int(const char* name, int def) {
    const char* v = getenv(name);
    return (v && *v) ? atoi(v) : def;
}

// A relative disc or data directory is relative to where the process was started, not to where a platform main
// (the harness: NP_DATA_DIR becomes the working directory) has moved to by the time the first OS call happens.
#if !defined(__vita__)
static char g_start_cwd[1024];
__attribute__((constructor(102))) static void record_start_cwd(void) {
    if (!getcwd(g_start_cwd, sizeof(g_start_cwd))) g_start_cwd[0] = 0;
}
#endif
static const char* absolute_dir(const char* path, char* buf, size_t size) {
#if !defined(__vita__)
    if (path && path[0] != '/' && g_start_cwd[0]) {
        snprintf(buf, size, "%s/%s", g_start_cwd, path);
        return buf;
    }
#endif
    (void)buf; (void)size;
    return path;
}

void NpSdk_ConfigDefaults(NpSdkConfig* c) {
    static char disc_buf[1100], data_buf[1100];
    memset(c, 0, sizeof(*c));
#if defined(__vita__)
    c->disc_dir = env_str("NP_DISC_DIR", "ux0:data/mscharged/game");
    c->data_dir = env_str("NP_DATA_DIR", "ux0:data/mscharged");
#else
    c->disc_dir = absolute_dir(env_str("NP_DISC_DIR", "game"), disc_buf, sizeof(disc_buf));
    c->data_dir = absolute_dir(env_str("NP_DATA_DIR", "appdata"), data_buf, sizeof(data_buf));
#endif
    c->mem1_size = (uint32_t)env_int("NP_MEM1_MB", 24) * 1024u * 1024u;
    c->mem2_size = (uint32_t)env_int("NP_MEM2_MB", 64) * 1024u * 1024u;
    c->mem1_base = 0x80000000u;
    // NP_MEM1_BASE (hex): move the MEM1 arena up, out of the retail static data range 0x80004000-0x806E8000 (tools/dtlive: the retail
    // translation addresses the native objects by their own addresses and must be able to tell them from retail statics)
    if (const char* b = getenv("NP_MEM1_BASE")) c->mem1_base = (uint32_t)strtoul(b, nullptr, 16);
    c->mem2_base = 0x90000000u;
#if !defined(__vita__)
    // NP_MEM2_BASE (hex, like NP_MEM1_BASE above): map the arenas elsewhere (determinism tests: the simulation must not depend on where the heaps are)
    if (const char* b = getenv("NP_MEM2_BASE")) c->mem2_base = (uint32_t)strtoul(b, 0, 16);
#endif
    c->language = env_int("NP_LANG", 1);
    c->widescreen = env_int("NP_WIDESCREEN", 1);
    c->progressive = env_int("NP_PROGRESSIVE", 1);
    c->eurgb60 = env_int("NP_EURGB60", 1);
    c->tv_format = env_int("NP_TVFORMAT", 0);
}

const NpSdkConfig* NpSdk_GetConfig(void) {
    if (!g_cfg_set) {
        NpSdk_ConfigDefaults(&g_cfg);
        g_cfg_set = 1;
    }
    return &g_cfg;
}

// ---------------------------------------------------------------------------------------------
// Memory arenas: the game takes all of MEM1 and MEM2 for its own allocators.

static void* arena_map(uint32_t size, uintptr_t base, const char* what) {
    void* p = np_mem_map(base, size);
    if (!p) {
        np_log("os", "cannot map %u MiB for %s", size >> 20, what);
        abort();
    }
    if (base && (uintptr_t)p != base) {
        np_log("os", "%s mapped at %p, not at the requested 0x%lx (fine on the Vita; on the host "
                     "the audio mixer assumes bit 31 of sample addresses is set)",
               what, p, (unsigned long)base);
    }
    return p;
}

void np_os_arena_init(void) {
    const NpSdkConfig* c = NpSdk_GetConfig();
    if (g_mem1) return;
    g_mem1 = arena_map(c->mem1_size, c->mem1_base, "MEM1");
    g_mem2 = arena_map(c->mem2_size, c->mem2_base, "MEM2");
    OSSetMEM1ArenaLo(g_mem1);
    OSSetMEM1ArenaHi((char*)g_mem1 + c->mem1_size);
    OSSetMEM2ArenaLo(g_mem2);
    OSSetMEM2ArenaHi((char*)g_mem2 + c->mem2_size);
    OS_PHYSICAL_MEM1_SIZE = OS_SIMULATED_MEM1_SIZE = c->mem1_size;
    OS_PHYSICAL_MEM2_SIZE = OS_SIMULATED_MEM2_SIZE = c->mem2_size;
    OS_USABLE_MEM1_END = (char*)g_mem1 + c->mem1_size;
    np_log("os", "MEM1 %p..%p, MEM2 %p..%p", g_mem1, (char*)g_mem1 + c->mem1_size, g_mem2,
           (char*)g_mem2 + c->mem2_size);
    if (np_gx_register_ram) {
        np_gx_register_ram(g_mem1, c->mem1_size);
        np_gx_register_ram(g_mem2, c->mem2_size);
    }
}

// ---------------------------------------------------------------------------------------------
// Start-up

void np_os_lazy_init(void) {
    if (np_os_initialized) return;
    np_os_initialized = 1;   // first: the steps below call OSDisableInterrupts
    // Registered by the first OS call (the game's OSInit, or NpSdk_Init): runs at exit before the destructors of
    // everything constructed earlier, i.e. before the game's globals are torn down.
    atexit(np_exit_quiesce);
    OS_BUS_CLOCK_SPEED = 243000000ul;
    np_vt_init();   // virtual time (harness): installs the clock before anything reads it
    np_time_init();
    OS_TV_FORMAT = (u32)NpSdk_GetConfig()->tv_format;
    OS_HOLLYWOOD_REV = 0x11;
    OS_DVD_DEVICE_CODE = 0x8201;
    np_os_threads_init();
    np_os_arena_init();
    np_alarm_init();
}

void OSInit(void) { np_os_lazy_init(); }

int NpSdk_Init(const NpSdkConfig* cfg) {
    if (cfg) {
        g_cfg = *cfg;
        g_cfg_set = 1;
    }
    np_os_lazy_init();
    return 0;
}

void NpSdk_Shutdown(void) {}

void OSRegisterVersion(const char* version) { (void)version; }
u32 __OSGetHollywoodRev(void) { return 0x11; }
const char* OSGetAppGamename(void) { return "R4QE"; }
u8 OSGetAppType(void) { return OS_APP_TYPE_DVD; }

u32 OSGetConsoleType(void) { return 0x00000012; }
u32 OSGetConsoleSimulatedMem2Size(void) { return NpSdk_GetConfig()->mem2_size; }
u32 OSGetPhysicalMem1Size(void) { return NpSdk_GetConfig()->mem1_size; }
u32 OSGetPhysicalMem2Size(void) { return NpSdk_GetConfig()->mem2_size; }

void* OSGetStackPointer(void) { return __builtin_frame_address(0); }

// ---------------------------------------------------------------------------------------------
// Reporting and fatal errors

void OSVReport(const char* fmt, va_list ap) {
    char line[1024];
    vsnprintf(line, sizeof(line), fmt, ap);
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    np_log("game", "%s", line);
}
void OSReport(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    OSVReport(fmt, ap);
    va_end(ap);
}
void OSPanic(const char* file, int line, const char* fmt, ...) {
    va_list ap;
    np_log("os", "PANIC at %s:%d", file ? file : "?", line);
    va_start(ap, fmt);
    OSVReport(fmt, ap);
    va_end(ap);
    abort();
}
void OSFatal(GXColor fg, GXColor bg, const char* msg) {
    (void)fg;
    (void)bg;
    np_log("os", "OSFatal: %s", msg ? msg : "(null)");
    abort();
}

// ---------------------------------------------------------------------------------------------
// Reset, shutdown, power. The application decides what "leave the game" means (NpSdk_SetQuitHook,
// default: exit the process). The game polls OSGetResetButtonState and has a power callback.

static OSStateCallback g_power_cb = 0;

void NpSdk_SetQuitHook(void (*hook)(int reason)) { g_quit_hook = hook; }
int NpSdk_QuitRequested(void) { return g_quit_requested; }

// The platform's "power button": calls the game's callback (as the console's power button does).
void NpSdk_PressPower(void) {
    if (g_power_cb) g_power_cb();
}

static void do_quit(int reason) {
    g_quit_requested = reason ? reason : 1;
    OSShutdownFunctionInfo* f;
    for (f = g_shutdown_queue.head; f; f = f->next) {
        if (f->func) f->func(TRUE, (u32)reason);
    }
    if (g_quit_hook) {
        g_quit_hook(reason);
    } else {
        fflush(0);
        exit(0);
    }
}

void OSRegisterShutdownFunction(OSShutdownFunctionInfo* info) {
    info->prev = g_shutdown_queue.tail;
    info->next = 0;
    if (g_shutdown_queue.tail)
        g_shutdown_queue.tail->next = info;
    else
        g_shutdown_queue.head = info;
    g_shutdown_queue.tail = info;
}
BOOL __OSCallShutdownFunctions(BOOL final, u32 event) {
    OSShutdownFunctionInfo* f;
    for (f = g_shutdown_queue.head; f; f = f->next) {
        if (f->func) f->func(final, event);
    }
    return TRUE;
}
void OSRestart(u32 resetCode) { (void)resetCode; do_quit(OS_SHUTDOWN_RESTART); }
void OSRebootSystem(void) { do_quit(OS_SHUTDOWN_REBOOT); }
void OSShutdownSystem(void) { do_quit(OS_SHUTDOWN_SHUTDOWN); }
void OSReturnToMenu(void) { do_quit(OS_SHUTDOWN_RETURN_MENU); }
void __OSReturnToMenuForError(void) { do_quit(OS_SHUTDOWN_FATAL); }
void OSResetSystem(int reset, u32 resetCode, BOOL forceMenu) {
    (void)resetCode;
    (void)forceMenu;
    do_quit(reset ? reset : OS_SHUTDOWN_RESTART);
}
u32 OSGetResetCode(void) { return 0; }
BOOL OSGetResetButtonState(void) { return FALSE; }
OSStateCallback OSSetPowerCallback(OSStateCallback cb) {
    OSStateCallback old = g_power_cb;
    g_power_cb = cb;
    return old;
}

// ---------------------------------------------------------------------------------------------
// Caches: the host is coherent. DCZeroRange really zeroes (dcbz).

// The renderer's caches (decoded display lists, textures hashed per 4 KiB) are retired when the game says it
// wrote memory for the GPU (np_gx_notify_write; a no-op without the renderer).
void DCInvalidateRange(const void* a, unsigned long n) { np_notify_write(a, n); }
void DCFlushRange(const void* a, unsigned long n) { np_notify_write(a, n); __sync_synchronize(); }
void DCStoreRange(const void* a, unsigned long n) { np_notify_write(a, n); __sync_synchronize(); }
void DCFlushRangeNoSync(const void* a, unsigned long n) { np_notify_write(a, n); }
void DCStoreRangeNoSync(const void* a, unsigned long n) { np_notify_write(a, n); }
void DCZeroRange(const void* a, unsigned long n) {
    uintptr_t p = (uintptr_t)a & ~(uintptr_t)31;
    uintptr_t e = ((uintptr_t)a + n + 31) & ~(uintptr_t)31;
    memset((void*)p, 0, e - p);
}
void DCTouchRange(const void* a, unsigned long n) { (void)a; (void)n; }
void ICInvalidateRange(const void* a, unsigned long n) { (void)a; (void)n; }
void DCEnable(void) {}
void ICEnable(void) {}
void ICFlashInvalidate(void) {}
void LCEnable(void) {}
void LCDisable(void) {}
void L2Enable(void) {}
void L2Disable(void) {}
void L2GlobalInvalidate(void) {}
void __OSCacheInit(void) {}
// Locked cache: a plain copy.
void LCLoadBlocks(void* d, const void* s, u32 blocks) { memcpy(d, s, (size_t)blocks * 32); }
void LCStoreBlocks(void* d, const void* s, u32 blocks) { memcpy(d, s, (size_t)blocks * 32); }
u32 LCStoreData(void* d, const void* s, u32 len) { memcpy(d, s, len); return 0; }
u32 LCQueueLength(void) { return 0; }
void LCQueueWait(u32 count) { (void)count; }

__attribute__((weak)) void PPCSync(void) { __sync_synchronize(); }   // (NPB's np_compat.c has the same)

// ---------------------------------------------------------------------------------------------
// Contexts: the register state is not used (the scheduler hands the CPU over with semaphores).

void OSSetCurrentContext(OSContext* c) { OS_CURRENT_CONTEXT = c; }
OSContext* OSGetCurrentContext(void) { return OS_CURRENT_CONTEXT; }
void OSClearContext(OSContext* c) { c->mode = 0; c->state = 0; }
void OSInitContext(OSContext* c, void* srr0, void* stack) {
    (void)srr0;
    (void)stack;
    memset(c, 0, sizeof(*c));
}
void OSSaveFPUContext(OSContext* c) { (void)c; }
void OSDumpContext(const OSContext* c) { (void)c; }
void __OSContextInit(void) {}

}  // extern "C"
