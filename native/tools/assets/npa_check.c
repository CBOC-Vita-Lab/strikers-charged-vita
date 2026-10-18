/*
 * npa_check - run the NPA converters over the real game files and verify the result.
 *
 *   npa_check [-v] [-H] [-k kind] <game files dir | file...>
 *   -H  also run each file through the load hook (npa_hook_tag/npa_hook_convert, twice) and compare
 *
 * For every file whose kind has a converter: load it (inflate .zlib containers), convert in place,
 * convert again (must report NPA_ALREADY), run the structural verifier for the kind.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifndef NPA_NO_ZLIB
#include <zlib.h>
#endif

#include "np_assets.h"
#include "np_hook.h"
#include "npa_verify.h"

static int g_verbose, g_hook;
static unsigned long g_hook_files, g_hook_mismatch;
static const char* g_only;
static unsigned long g_files, g_ok, g_already, g_fail, g_warn, g_skipped;
static unsigned long g_kind_files[NPA_K_COUNT], g_kind_fail[NPA_K_COUNT], g_kind_warn[NPA_K_COUNT];
static size_t g_kind_bytes[NPA_K_COUNT], g_kind_swapped[NPA_K_COUNT];
static unsigned long g_kind_chunks[NPA_K_COUNT];
static double g_kind_secs[NPA_K_COUNT];

static void* aligned_load(const char* path, size_t* size, int is_zlib)
{
    FILE* f = fopen(path, "rb");
    struct stat st;
    unsigned char* raw;
    void* out;
    if (!f) return NULL;
    fstat(fileno(f), &st);
    raw = (unsigned char*)malloc((size_t)st.st_size + 1);
    if (fread(raw, 1, (size_t)st.st_size, f) != (size_t)st.st_size) { fclose(f); free(raw); return NULL; }
    fclose(f);
#ifdef NPA_NO_ZLIB
    if (is_zlib) { free(raw); return NULL; }
#else
    if (is_zlib)
    {
        uLongf outlen;
        uint32_t want;
        if (st.st_size < 5) { free(raw); return NULL; }
        want = npa_zlib_container_size(raw);
        outlen = want;
        if (posix_memalign(&out, 32, (size_t)want + 32) != 0) { free(raw); return NULL; }
        if (uncompress((Bytef*)out, &outlen, raw + 4, (uLong)st.st_size - 4) != Z_OK || outlen != want)
        {
            fprintf(stderr, "%s: inflate failed\n", path);
            free(raw); free(out); return NULL;
        }
        *size = want;
        free(raw);
        return out;
    }
#endif
    if (posix_memalign(&out, 32, (size_t)st.st_size + 32) != 0) { free(raw); return NULL; }
    memcpy(out, raw, (size_t)st.st_size);
    *size = (size_t)st.st_size;
    free(raw);
    return out;
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void do_file(const char* path)
{
    int z = 0;
    NpaKind k = npa_classify(path, &z);
    size_t size = 0;
    void* buf;
    NpaResult r, r2;
    double t0, t1;
    char vmsg[256] = "";
    int vr;
    void* h = NULL; /* copy converted by the load hook (-H) */
    if (k == NPA_K_NONE) { g_skipped++; return; }
    if (g_only && strcmp(g_only, npa_kind_name(k)) != 0) return;
#ifdef NPA_NO_ZLIB
    if (z) { g_skipped++; return; }
#endif
    buf = aligned_load(path, &size, z);
    if (!buf) { fprintf(stderr, "%s: load failed\n", path); g_fail++; return; }
    g_files++;
    g_kind_files[k]++;
    g_kind_bytes[k] += size;
    if (g_hook)
    {
        /* the path the nlFile patch takes: tag at request time, hook on the (inflated) buffer, twice */
        int tag = npa_hook_tag(path);
        if (tag)
        {
            if (posix_memalign(&h, 32, size + 32) == 0)
            {
                memcpy(h, buf, size);
                npa_hook_convert(tag, h, size, path);
            }
        }
    }
    t0 = now();
    npa_convert(k, buf, size, &r);
    t1 = now();
    g_kind_secs[k] += t1 - t0;
    if (r.status == NPA_ERR_UNSUPPORTED) { g_files--; g_kind_files[k]--; g_kind_bytes[k] -= size; g_skipped++; free(buf); free(h); return; }
    if (r.status < 0)
    {
        g_fail++; g_kind_fail[k]++;
        printf("FAIL %-10s %s : %s\n", npa_kind_name(k), path, r.msg);
        free(buf); free(h);
        return;
    }
    if (r.status == NPA_ALREADY) g_already++;
    else g_ok++;
    g_kind_swapped[k] += r.bytes_swapped;
    g_kind_chunks[k] += r.chunks;
    if (r.warnings) { g_warn++; g_kind_warn[k]++; if (g_verbose || 1) printf("WARN %-10s %s : %u warnings, first: %s\n", npa_kind_name(k), path, r.warnings, r.msg); }
    npa_convert(k, buf, size, &r2);
    if (r2.status != NPA_ALREADY)
    {
        g_fail++; g_kind_fail[k]++;
        printf("FAIL %-10s %s : second conversion status %d (%s)\n", npa_kind_name(k), path, r2.status, r2.msg);
        free(buf); free(h);
        return;
    }
    vr = npav_verify(k, buf, size, vmsg, sizeof vmsg);
    if (vr != 0)
    {
        g_fail++; g_kind_fail[k]++;
        printf("FAIL %-10s %s : verify: %s\n", npa_kind_name(k), path, vmsg);
    }
    else if (g_verbose)
        printf("ok   %-10s %s (%zu bytes, %u chunks, %zu swapped)\n", npa_kind_name(k), path, size, r.chunks, r.bytes_swapped);
    if (h)
    {
        g_hook_files++;
        if (memcmp(h, buf, size) != 0)
        {
            g_hook_mismatch++; g_fail++; g_kind_fail[k]++;
            printf("FAIL %-10s %s : hook output differs from npa_convert\n", npa_kind_name(k), path);
        }
        else
        {
            npa_hook_convert(npa_hook_tag(path), h, size, path); /* second pass must not change anything */
            if (memcmp(h, buf, size) != 0)
            {
                g_hook_mismatch++; g_fail++; g_kind_fail[k]++;
                printf("FAIL %-10s %s : second hook pass changed the buffer\n", npa_kind_name(k), path);
            }
        }
    }
    free(buf); free(h);
}

static void walk(const char* dir)
{
    DIR* d = opendir(dir);
    struct dirent* e;
    char p[1024];
    struct stat st;
    if (!d) { do_file(dir); return; }
    while ((e = readdir(d)))
    {
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) walk(p);
        else do_file(p);
    }
    closedir(d);
}

int main(int argc, char** argv)
{
    int i;
    int k;
    for (i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "-v")) g_verbose = 1;
        else if (!strcmp(argv[i], "-H")) g_hook = 1;
        else if (!strcmp(argv[i], "-k") && i + 1 < argc) g_only = argv[++i];
        else walk(argv[i]);
    }
    printf("--- %lu files converted (%lu fresh, %lu already host), %lu failed, %lu with warnings, %lu skipped (no converter/unsupported)\n",
           g_files, g_ok, g_already, g_fail, g_warn, g_skipped);
    if (g_hook)
    {
        unsigned long hf, hb, hx;
        double hs;
        npa_hook_stats(&hf, &hb, &hx, &hs);
        printf("--- hook (-H): %lu files compared with npa_convert, %lu mismatches; hook stats: %lu calls, %lu bytes, %lu failed, %.3f ms\n",
               g_hook_files, g_hook_mismatch, hf, hb, hx, hs * 1e3);
    }
    for (k = 1; k < NPA_K_COUNT; k++)
        if (g_kind_files[k])
            printf("    %-10s %5lu files %10zu bytes, %9zu swapped (%4.1f%%), %7lu chunks/objects, %7.3f ms  fail %lu warn %lu\n", npa_kind_name((NpaKind)k),
                   g_kind_files[k], g_kind_bytes[k], g_kind_swapped[k], 100.0 * g_kind_swapped[k] / (g_kind_bytes[k] ? g_kind_bytes[k] : 1),
                   g_kind_chunks[k], g_kind_secs[k] * 1e3, g_kind_fail[k], g_kind_warn[k]);
    return g_fail ? 1 : 0;
}
