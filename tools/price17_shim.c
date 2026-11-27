// price17_shim.c: LD_PRELOAD counter for host32 (gcc -m32 -shared -fPIC -O1 price17_shim.c -o price17_shim.so -ldl -lpthread).
// Counts per thread: getenv, clock reads, stdio/log writes, mutex locks, waits, sleeps, malloc, with the caller of each getenv / clock read.
// SHIM_QUIET=1 makes fprintf/printf to stdout/stderr return 0 without formatting (what NP_LOGGING=0 does on the console, vita_silent_stdio.cpp).
// Output at exit to $SHIM_OUT (text): one line per thread + one line per (thread, kind, caller) with its count.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

enum { K_GETENV, K_CLOCK, K_TIME, K_LOGCALL, K_LOGBYTES, K_MUTEX, K_TRYLOCK, K_CONDW, K_SEMW, K_SLEEP, K_YIELD, K_MALLOC, K_SYSCALL, K_WRITE, K_N };
static const char* kName[K_N] = {"getenv", "clock_gettime", "gettimeofday/time", "log_calls(stdout/stderr)", "log_bytes", "mutex_lock", "mutex_trylock", "cond_wait", "sem_wait", "sleep", "sched_yield", "malloc", "syscall", "write(2)"};
#define SITES 2048
typedef struct { unsigned kind; void* a; void* b; void* c; unsigned long n; char name[40]; } Site;
typedef struct { int tid; char name[20]; unsigned long c[K_N]; Site s[SITES]; int used; } Th;
#define MAXTH 64
static Th* g_th[MAXTH]; static int g_nth; static int g_quiet; static __thread Th* t_th; static __thread int t_busy;
static int g_ready;
static Th* me(void) {
    if (t_th) return t_th;
    Th* t = calloc(1, sizeof(Th)); t->tid = (int)syscall(SYS_gettid);
    int i = __sync_fetch_and_add(&g_nth, 1); if (i >= MAXTH) return 0; g_th[i] = t; t_th = t; return t;
}
static void note(unsigned kind, unsigned long add, int sites) {
    if (!g_ready || t_busy) return; t_busy = 1;
    Th* t = me(); if (!t) { t_busy = 0; return; }
    t->c[kind] += add;
    if (t->name[0] == 0 || (t->c[kind] & 0x3ff) == 1) { char nm[20] = {0}; prctl(PR_GET_NAME, nm); strncpy(t->name, nm, 19); }
    if (sites) {
        void* bt[9]; int n = backtrace(bt, 9); int o = 2; { Dl_info q = {0}; if (n > 2 && dladdr(bt[2], &q) && q.dli_sname && !strcmp(q.dli_sname, "clock_gettime")) o = 3; if (n > o && dladdr(bt[o], &q) && q.dli_sname && !strcmp(q.dli_sname, "np_time_ns")) o++; if (n > o && dladdr(bt[o], &q) && q.dli_sname && (!strcmp(q.dli_sname, "np_time_ns") || !strcmp(q.dli_sname, "NpMonotonicNs"))) o++; } void* a = n > o ? bt[o] : 0; void* b = n > o+1 ? bt[o+1] : 0; void* c = n > o+2 ? bt[o+2] : 0;
        unsigned h = ((unsigned)(size_t)a * 2654435761u ^ (unsigned)(size_t)b * 40503u ^ (unsigned)(size_t)c * 2246822519u ^ kind) % SITES;
        for (int k = 0; k < SITES; ++k) { Site* s = &t->s[(h + k) % SITES];
            if (s->n == 0) { s->kind = kind; s->a = a; s->b = b; s->c = c; s->n = 1; t->used++; break; }
            if (s->kind == kind && s->a == a && s->b == b && s->c == c) { s->n++; break; } }
    }
    t_busy = 0;
}
#define REAL(ret, nm, args) static ret (*real_##nm) args; if (!real_##nm) real_##nm = dlsym(RTLD_NEXT, #nm)
char* getenv(const char* n) { REAL(char*, getenv, (const char*)); note(K_GETENV, 1, 1); return real_getenv(n); }
char* secure_getenv(const char* n) { return getenv(n); }
int clock_gettime(clockid_t c, struct timespec* t) { REAL(int, clock_gettime, (clockid_t, struct timespec*)); note(K_CLOCK, 1, 1); return real_clock_gettime(c, t); }
int gettimeofday(struct timeval* tv, void* tz) { REAL(int, gettimeofday, (struct timeval*, void*)); note(K_TIME, 1, 1); return real_gettimeofday(tv, tz); }
time_t time(time_t* t) { REAL(time_t, time, (time_t*)); note(K_TIME, 1, 1); return real_time(t); }
static int islog(FILE* f) { return f == stdout || f == stderr; }
int fprintf(FILE* f, const char* fmt, ...) { REAL(int, vfprintf, (FILE*, const char*, va_list)); va_list ap; va_start(ap, fmt); int r = 0;
    if (islog(f)) { note(K_LOGCALL, 1, 1); if (!g_quiet) { r = real_vfprintf(f, fmt, ap); note(K_LOGBYTES, r > 0 ? r : 0, 0); } } else r = real_vfprintf(f, fmt, ap); va_end(ap); return r; }
int vfprintf(FILE* f, const char* fmt, va_list ap) { REAL(int, vfprintf, (FILE*, const char*, va_list)); int r = 0;
    if (islog(f)) { note(K_LOGCALL, 1, 1); if (!g_quiet) { r = real_vfprintf(f, fmt, ap); note(K_LOGBYTES, r > 0 ? r : 0, 0); } } else r = real_vfprintf(f, fmt, ap); return r; }
int printf(const char* fmt, ...) { REAL(int, vfprintf, (FILE*, const char*, va_list)); va_list ap; va_start(ap, fmt); int r = 0; note(K_LOGCALL, 1, 1);
    if (!g_quiet) { r = real_vfprintf(stdout, fmt, ap); note(K_LOGBYTES, r > 0 ? r : 0, 0); } va_end(ap); return r; }
int vprintf(const char* fmt, va_list ap) { return vfprintf(stdout, fmt, ap); }
int puts(const char* s) { REAL(int, puts, (const char*)); note(K_LOGCALL, 1, 1); return g_quiet ? 0 : real_puts(s); }
int fputs(const char* s, FILE* f) { REAL(int, fputs, (const char*, FILE*)); if (islog(f)) { note(K_LOGCALL, 1, 1); if (g_quiet) return 0; note(K_LOGBYTES, strlen(s), 0); } return real_fputs(s, f); }
size_t fwrite(const void* p, size_t a, size_t b, FILE* f) { REAL(size_t, fwrite, (const void*, size_t, size_t, FILE*)); if (islog(f)) { note(K_LOGCALL, 1, 1); if (g_quiet) return b; note(K_LOGBYTES, a * b, 0); } return real_fwrite(p, a, b, f); }
int fputc(int c, FILE* f) { REAL(int, fputc, (int, FILE*)); if (islog(f)) { note(K_LOGCALL, 1, 0); if (g_quiet) return c; } return real_fputc(c, f); }
ssize_t write(int fd, const void* p, size_t n) { REAL(ssize_t, write, (int, const void*, size_t)); note(K_WRITE, 1, fd > 2 ? 0 : 1); return real_write(fd, p, n); }
int pthread_mutex_lock(pthread_mutex_t* m) { REAL(int, pthread_mutex_lock, (pthread_mutex_t*)); note(K_MUTEX, 1, 1); return real_pthread_mutex_lock(m); }
int pthread_mutex_trylock(pthread_mutex_t* m) { REAL(int, pthread_mutex_trylock, (pthread_mutex_t*)); note(K_TRYLOCK, 1, 0); return real_pthread_mutex_trylock(m); }
int pthread_cond_wait(pthread_cond_t* c, pthread_mutex_t* m) { REAL(int, pthread_cond_wait, (pthread_cond_t*, pthread_mutex_t*)); note(K_CONDW, 1, 1); return real_pthread_cond_wait(c, m); }
int pthread_cond_timedwait(pthread_cond_t* c, pthread_mutex_t* m, const struct timespec* t) { REAL(int, pthread_cond_timedwait, (pthread_cond_t*, pthread_mutex_t*, const struct timespec*)); note(K_CONDW, 1, 1); return real_pthread_cond_timedwait(c, m, t); }
int sem_wait(sem_t* s) { REAL(int, sem_wait, (sem_t*)); note(K_SEMW, 1, 1); return real_sem_wait(s); }
int usleep(useconds_t u) { REAL(int, usleep, (useconds_t)); note(K_SLEEP, 1, 1); return real_usleep(u); }
int nanosleep(const struct timespec* a, struct timespec* b) { REAL(int, nanosleep, (const struct timespec*, struct timespec*)); note(K_SLEEP, 1, 1); return real_nanosleep(a, b); }
int sched_yield(void) { REAL(int, sched_yield, (void)); note(K_YIELD, 1, 0); return real_sched_yield(); }
void* malloc(size_t n) { REAL(void*, malloc, (size_t)); void* p; if (!real_malloc) { return NULL; } p = real_malloc(n); note(K_MALLOC, 1, 0); return p; }
static void fini(void);
static int g_inexit;
void _exit(int c) { REAL(void, _exit, (int)); if (!__sync_lock_test_and_set(&g_inexit, 1)) { fini(); alarm(15); exit(c); } real__exit(c); for (;;); }
void _Exit(int c) { _exit(c); }
__attribute__((destructor)) static void fini2(void) { fini(); }
__attribute__((constructor)) static void init(void) { g_quiet = getenv("SHIM_QUIET") && *getenv("SHIM_QUIET") == '1'; void* b[4]; backtrace(b, 4); g_ready = 1; }
static int g_dumped;
static void fini(void) {
    if (__sync_lock_test_and_set(&g_dumped, 1)) return;
    g_ready = 0; const char* o = getenv("SHIM_OUT"); FILE* f = o ? fopen(o, "w") : stderr; if (!f) return;
    Dl_info di; (void)di;
    for (int i = 0; i < g_nth; ++i) { Th* t = g_th[i]; fprintf(f, "T %d %s", t->tid, t->name); for (int k = 0; k < K_N; ++k) fprintf(f, " %s=%lu", kName[k], t->c[k]); fprintf(f, "\n"); }
    for (int i = 0; i < g_nth; ++i) { Th* t = g_th[i]; for (int k = 0; k < SITES; ++k) { Site* s = &t->s[k]; if (!s->n) continue;
        Dl_info a = {0}, b = {0}, c = {0}; dladdr(s->a, &a); dladdr(s->b, &b); dladdr(s->c, &c);
        fprintf(f, "S %d %s %s %lu %s+0x%lx %s+0x%lx %s+0x%lx\n", t->tid, t->name, kName[s->kind], s->n, a.dli_fname ? a.dli_fname : "?", a.dli_fbase ? (unsigned long)((char*)s->a - (char*)a.dli_fbase) : 0ul, b.dli_fname ? b.dli_fname : "?", b.dli_fbase ? (unsigned long)((char*)s->b - (char*)b.dli_fbase) : 0ul, c.dli_fname ? c.dli_fname : "?", c.dli_fbase ? (unsigned long)((char*)s->c - (char*)c.dli_fbase) : 0ul); } }
    if (o) fclose(f);
}
