// POSIX implementation of np_plat.h (host32 and armhf Linux).
#define _GNU_SOURCE 1
#include "np/np_plat.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// ---------------------------------------------------------------------------------------------
// Time
static NpTimeSource g_time_source = 0;

uint64_t np_time_real_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
uint64_t np_time_ns(void) {
    NpTimeSource f = g_time_source;
    return f ? f() : np_time_real_ns();
}
void np_time_set_source(NpTimeSource fn) { g_time_source = fn; }

void np_sleep_ns(uint64_t ns) {
    struct timespec ts;
    ts.tv_sec = (time_t)(ns / 1000000000ull);
    ts.tv_nsec = (long)(ns % 1000000000ull);
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}
void np_yield(void) { sched_yield(); }
int64_t np_display_vblank_wait(void) { return -1; }   // no display clock on the host
int64_t np_display_vcount(void) { return -1; }

// ---------------------------------------------------------------------------------------------
// Semaphore and mutex
int np_sem_init(NpSem* s, int initial) {
    sem_t* sem = (sem_t*)malloc(sizeof(sem_t));
    if (!sem) return -1;
    if (sem_init(sem, 0, (unsigned)initial) != 0) {
        free(sem);
        return -1;
    }
    s->p = sem;
    return 0;
}
void np_sem_destroy(NpSem* s) {
    if (!s->p) return;
    sem_destroy((sem_t*)s->p);
    free(s->p);
    s->p = 0;
}
void np_sem_post(NpSem* s) { sem_post((sem_t*)s->p); }
void np_sem_wait(NpSem* s) {
    while (sem_wait((sem_t*)s->p) != 0 && errno == EINTR) {
    }
}
int np_sem_timedwait_ns(NpSem* s, uint64_t timeout_ns) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t ns = (uint64_t)ts.tv_nsec + timeout_ns % 1000000000ull;
    ts.tv_sec += (time_t)(timeout_ns / 1000000000ull + ns / 1000000000ull);
    ts.tv_nsec = (long)(ns % 1000000000ull);
    for (;;) {
        if (sem_timedwait((sem_t*)s->p, &ts) == 0) return 1;
        if (errno == ETIMEDOUT) return 0;
        if (errno != EINTR) return 0;
    }
}

int np_mutex_init(NpMutex* m) {
    pthread_mutex_t* mu = (pthread_mutex_t*)malloc(sizeof(pthread_mutex_t));
    if (!mu) return -1;
    pthread_mutex_init(mu, 0);
    m->p = mu;
    return 0;
}
void np_mutex_destroy(NpMutex* m) {
    if (!m->p) return;
    pthread_mutex_destroy((pthread_mutex_t*)m->p);
    free(m->p);
    m->p = 0;
}
void np_mutex_lock(NpMutex* m) { pthread_mutex_lock((pthread_mutex_t*)m->p); }
int np_mutex_trylock(NpMutex* m) { return pthread_mutex_trylock((pthread_mutex_t*)m->p) == 0; }
void np_mutex_unlock(NpMutex* m) { pthread_mutex_unlock((pthread_mutex_t*)m->p); }

// ---------------------------------------------------------------------------------------------
// Threads
struct NpThread {
    pthread_t th;
    char name[24];
    NpRole role;
    NpThreadFn fn;
    void* arg;
};

static NpThreadHook g_thread_hook = 0;
void np_plat_set_thread_hook(NpThreadHook hook) { g_thread_hook = hook; }

void np_thread_apply_role(NpRole role) {
    if (g_thread_hook) g_thread_hook("", role);
}

static void* thread_main(void* p) {
    NpThread* t = (NpThread*)p;
#if defined(__linux__)
    pthread_setname_np(pthread_self(), t->name);
#endif
    if (g_thread_hook) g_thread_hook(t->name, t->role);
    return t->fn(t->arg);
}

NpThread* np_thread_create(const char* name, NpRole role, NpThreadFn fn, void* arg, void* stack,
                           size_t stack_size) {
    NpThread* t = (NpThread*)calloc(1, sizeof(NpThread));
    if (!t) return 0;
    snprintf(t->name, sizeof(t->name), "%s", name ? name : "np");
    t->role = role;
    t->fn = fn;
    t->arg = arg;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (stack && stack_size >= 16384) {
        if (pthread_attr_setstack(&attr, stack, stack_size) != 0) {
            pthread_attr_setstacksize(&attr, stack_size < 65536 ? 65536 : stack_size);
        }
    } else if (stack_size) {
        pthread_attr_setstacksize(&attr, stack_size < 16384 ? 16384 : stack_size);
    }
    int rc = pthread_create(&t->th, &attr, thread_main, t);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        free(t);
        return 0;
    }
    return t;
}

void np_thread_join(NpThread* t) {
    if (!t) return;
    pthread_join(t->th, 0);
    free(t);
}
void np_thread_detach(NpThread* t) {
    if (!t) return;
    pthread_detach(t->th);
    // The handle is leaked on purpose: the thread's trampoline still reads it.
}

void np_thread_exit(void) { pthread_exit(0); }

void np_thread_stack_bounds(void** low, void** high) {
    *low = 0;
    *high = 0;
#if defined(__linux__)
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) == 0) {
        void* addr = 0;
        size_t size = 0;
        if (pthread_attr_getstack(&attr, &addr, &size) == 0) {
            *low = addr;
            *high = (char*)addr + size;
        }
        pthread_attr_destroy(&attr);
    }
#endif
}

// ---------------------------------------------------------------------------------------------
// Memory
#include <sys/mman.h>
void* np_mem_map(uintptr_t want_base, size_t size) {
    void* p = MAP_FAILED;
#ifdef MAP_FIXED_NOREPLACE
    if (want_base) {
        p = mmap((void*)want_base, size, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p != MAP_FAILED && (uintptr_t)p != want_base) {   // old kernels ignore the flag
            munmap(p, size);
            p = MAP_FAILED;
        }
    }
#endif
    if (p == MAP_FAILED) {
        p = mmap(0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    }
    return p == MAP_FAILED ? 0 : p;
}

// ---------------------------------------------------------------------------------------------
// Files
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
    for (size_t i = 1; i < n; ++i) {
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
