// np_plat.h: the small platform boundary under the SDK layer (platform/sdk).
//
// Everything in src/ is written against this header and the C library; the two implementations
// are host/np_plat_host.cpp (POSIX: host32 and armhf Linux) and vita/np_plat_vita.cpp (VitaSDK:
// sce* kernel objects, because pthread condition variables are polled on the console and a thread
// switch of the OS scheduler has to be a plain semaphore hand-off). No Wii SDK header is included
// here, so this file can be used from translation units that must not see them.
#ifndef NP_PLAT_H
#define NP_PLAT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ----------------------------------------------------------------------------------------------
// Time. np_time_ns() is a monotonic clock in nanoseconds (arbitrary origin). The harness may
// replace it with a virtual clock through np_time_set_source() (deterministic runs); every timer
// of the SDK layer (OSGetTime, alarms, retrace, audio frames) reads it through here.
typedef uint64_t (*NpTimeSource)(void);
uint64_t np_time_ns(void);
uint64_t np_time_real_ns(void);           // the real clock, never virtual
void np_time_set_source(NpTimeSource fn); // NULL restores the real clock
void np_sleep_ns(uint64_t ns);            // real sleep
void np_yield(void);                      // let another thread run
// The display's vertical blank (the console; W5 FPS). np_display_vblank_wait() blocks until the next vblank starts and returns the display's vblank
// counter (32 bit, wraps); np_display_vcount() returns the counter now. Both return -1 where the platform has no display clock (host builds) or the
// wait failed (screen off): the caller then keeps a timer of its own.
int64_t np_display_vblank_wait(void);
int64_t np_display_vcount(void);

// ----------------------------------------------------------------------------------------------
// Counting semaphore (hand-off between OS threads, wake-ups of the helper threads).
typedef struct NpSem { void* p; } NpSem;
int np_sem_init(NpSem* s, int initial);
void np_sem_destroy(NpSem* s);
void np_sem_post(NpSem* s);
void np_sem_wait(NpSem* s);
// Waits at most `timeout_ns` of real time; returns 1 if the semaphore was taken, 0 on timeout.
int np_sem_timedwait_ns(NpSem* s, uint64_t timeout_ns);

// Non-recursive mutex.
typedef struct NpMutex { void* p; } NpMutex;
int np_mutex_init(NpMutex* m);
void np_mutex_destroy(NpMutex* m);
void np_mutex_lock(NpMutex* m);
int np_mutex_trylock(NpMutex* m);
void np_mutex_unlock(NpMutex* m);

// ----------------------------------------------------------------------------------------------
// Threads. A role tells the platform which core / priority plan to apply (the Vita's plan lives in
// vita/np_plat_vita.cpp and can be overridden by the application with np_plat_set_thread_hook()).
typedef enum NpRole {
    NP_ROLE_GAME = 0,   // the thread that runs the game (the first OS thread)
    NP_ROLE_OSTHREAD,   // threads the game creates with OSCreateThread
    NP_ROLE_AUDIO,      // AX frame / audio output thread
    NP_ROLE_IO,         // disc and NAND worker threads
    NP_ROLE_TIMER,      // alarms, VI retrace
    NP_ROLE_HELPER,     // movie decoding and the like
    NP_ROLE_COUNT
} NpRole;

typedef struct NpThread NpThread;
typedef void* (*NpThreadFn)(void*);

// stack_size 0: the platform default. `stack` (may be NULL) is a caller supplied stack of
// `stack_size` bytes where the platform can use one (host: pthread_attr_setstack).
NpThread* np_thread_create(const char* name, NpRole role, NpThreadFn fn, void* arg, void* stack,
                           size_t stack_size);
void np_thread_join(NpThread* t);   // waits and frees the handle
void np_thread_detach(NpThread* t); // frees the handle when the thread ends
void np_thread_exit(void);          // ends the calling thread (one made by np_thread_create); never returns
// Bounds of the calling thread's stack (best effort; 0 when unknown).
void np_thread_stack_bounds(void** low, void** high);
// Applies the platform's plan for `role` to the calling thread.
void np_thread_apply_role(NpRole role);

// The application (NPV's Vita main, the harness) can take over core placement.
typedef void (*NpThreadHook)(const char* name, NpRole role);
void np_plat_set_thread_hook(NpThreadHook hook);

// ----------------------------------------------------------------------------------------------
// Memory. np_mem_map returns `size` bytes of zeroed read/write memory, at `want_base` when that
// is non-zero and free (host: mmap with a placement hint; Vita: a user memory block, the address
// is the system's choice), otherwise anywhere. NULL on failure. Never freed before exit.
void* np_mem_map(uintptr_t want_base, size_t size);

// ----------------------------------------------------------------------------------------------
// Files and directories (host: POSIX; Vita: sceIo through newlib's POSIX layer).
// Paths use '/' and are given as the platform wants them ("ux0:data/mscharged" on the console).
int np_file_exists(const char* path);
int np_dir_exists(const char* path);
int np_mkdir_p(const char* path);

// ----------------------------------------------------------------------------------------------
// Log: one line per call, newline added. Goes to stderr (the Vita layer redirects stderr into its
// log file), prefixed with the module tag.
void np_log(const char* tag, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

#ifdef __cplusplus
}
#endif
#endif
