/* NPP: LD_PRELOAD spy for libc calls that dominate a profile: counts calls and bytes per CALLER (return address) of strlen/memset/memcpy/memcmp
 * and prints the top callers (dladdr names: the executable is linked -rdynamic) at exit and every 2^k calls.
 *   arm-linux-gnueabihf-gcc -shared -fPIC -O2 -o libspy.so strlen_spy.c -ldl
 *   qemu-arm ... -E LD_PRELOAD=libspy.so np-armhf-full */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#define N 8192
static void report(const char* why);
typedef struct { void* ret; uint64_t calls, bytes; int fn; } Ent;
static Ent tab[N];
static uint64_t total_calls[4], total_bytes[4];
static const char* fnames[4] = { "strlen", "memset", "memcpy", "memcmp" };
static void note(int fn, void* ret, size_t n) {
    total_calls[fn]++; total_bytes[fn] += n;
    if (((total_calls[0] + total_calls[1]) & 0x3ff) == 0) report("periodic");
    uintptr_t h = (((uintptr_t)ret >> 1) * 2654435761u + fn * 40503u) % N;
    for (int i = 0; i < N; i++) {
        Ent* e = &tab[(h + i) % N];
        if (!e->ret) { e->ret = ret; e->fn = fn; }
        if (e->ret == ret && e->fn == fn) { e->calls++; e->bytes += n; return; }
    }
}
static void report(const char* why);
static int cmp(const void* a, const void* b) { const Ent* x = a; const Ent* y = b; return y->bytes > x->bytes ? 1 : y->bytes < x->bytes ? -1 : 0; }
static void report(const char* why) {
    static Ent cp[N]; memcpy(cp, tab, sizeof tab); qsort(cp, N, sizeof(Ent), cmp);
    fprintf(stderr, "[spy] %s: ", why);
    for (int k = 0; k < 4; k++) fprintf(stderr, "%s calls=%llu bytes=%llu  ", fnames[k], (unsigned long long)total_calls[k], (unsigned long long)total_bytes[k]);
    fprintf(stderr, "\n");
    for (int i = 0; i < 25 && cp[i].ret; i++) {
        Dl_info di; const char* nm = "?";
        if (dladdr(cp[i].ret, &di) && di.dli_sname) nm = di.dli_sname;
        fprintf(stderr, "[spy]   %-7s bytes=%-12llu calls=%-9llu avg=%-7llu ret=%p (base %p) %s\n", fnames[cp[i].fn], (unsigned long long)cp[i].bytes,
                (unsigned long long)cp[i].calls, (unsigned long long)(cp[i].bytes / (cp[i].calls ? cp[i].calls : 1)), cp[i].ret, di.dli_fbase, nm);
    }
}
__attribute__((constructor)) static void init(void) { fprintf(stderr, "[spy] loaded\n"); }
__attribute__((destructor)) static void fini(void) { report("exit"); }
size_t strlen(const char* s) {
    static size_t (*real)(const char*);
    if (!real) real = (size_t (*)(const char*))dlsym(RTLD_NEXT, "strlen");
    size_t n = real(s);
    note(0, __builtin_return_address(0), n);
    return n;
}
void* memset(void* d, int c, size_t n) {
    static void* (*real)(void*, int, size_t);
    if (!real) real = (void* (*)(void*, int, size_t))dlsym(RTLD_NEXT, "memset");
    note(1, __builtin_return_address(0), n);
    return real(d, c, n);
}
