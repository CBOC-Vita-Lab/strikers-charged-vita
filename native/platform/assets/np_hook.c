/*
 * np_hook.c - glue between the decomp's file loads and the converters (see np_hook.h).
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "np_assets.h"
#include "np_hook.h"

static void default_log(const char* line) { fprintf(stderr, "%s\n", line); }

static void (*g_log)(const char*) = default_log;
static int g_enabled = 1;
/* async loads may complete on another thread: the counters are atomic, the time is kept in nanoseconds */
static unsigned long g_files, g_bytes, g_failed;
static unsigned long long g_nanos;

static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

void npa_hook_set_log(void (*fn)(const char*)) { g_log = fn ? fn : default_log; }
void npa_hook_enable(int on) { g_enabled = on; }

void npa_hook_stats(unsigned long* files, unsigned long* bytes, unsigned long* failed, double* seconds)
{
    if (files) *files = __atomic_load_n(&g_files, __ATOMIC_RELAXED);
    if (bytes) *bytes = __atomic_load_n(&g_bytes, __ATOMIC_RELAXED);
    if (failed) *failed = __atomic_load_n(&g_failed, __ATOMIC_RELAXED);
    if (seconds) *seconds = (double)__atomic_load_n(&g_nanos, __ATOMIC_RELAXED) * 1e-9;
}

int npa_hook_tag(const char* path)
{
    int z = 0;
    NpaKind k = npa_classify(path, &z);
    if (k == NPA_K_NONE) return 0;
    /* BundleFile archives are read piecewise through nlRead; their pieces go through the BundleFile and
     * texture hooks instead of a whole-file conversion */
    if (k == NPA_K_BUNDLEFILE) return 0;
    return 0x10000 | (int)k | (z << 8);
}

static void run(NpaKind kind, void* data, unsigned long size, const char* what)
{
    NpaResult r;
    double t0;
    char line[320];
    if (!g_enabled || data == NULL || size == 0) return;
    t0 = now_seconds();
    npa_convert(kind, data, size, &r);
    __atomic_fetch_add(&g_nanos, (unsigned long long)((now_seconds() - t0) * 1e9), __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_files, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_bytes, size, __ATOMIC_RELAXED);
    if (r.status < 0)
    {
        __atomic_fetch_add(&g_failed, 1, __ATOMIC_RELAXED);
        snprintf(line, sizeof line, "npa: %s (%s): %s", what ? what : "?", npa_kind_name(kind), r.msg);
        g_log(line);
    }
}

void npa_hook_convert(int tag, void* data, unsigned long size, const char* path)
{
    if (!(tag & 0x10000)) return;
    run((NpaKind)(tag & 0xFF), data, size, path);
}

void npa_hook_convert_kind(int kind, void* data, unsigned long size, const char* what)
{
    run((NpaKind)kind, data, size, what);
}
