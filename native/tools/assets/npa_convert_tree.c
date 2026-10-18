/*
 * npa_convert_tree - offline conversion of a game file tree to the native (little-endian) form.
 *
 *   npa_convert_tree [-l] [-n] [-V] [-q] [-z level] <srcdir> <dstdir>
 *
 * Every file whose kind has a converter (npa_classify) is loaded, converted in place with the same
 * npa_convert() functions the load-time hook uses, and written to <dstdir>/<same relative path>.
 *   .zlib containers are inflated, converted and deflated again; the leading size word stays BIG-endian
 *   (the nlFile patch byte-swaps it when it reads the header, exactly as for the untouched disc files).
 *   -l  also symlink every other file of <srcdir> into <dstdir> (so <dstdir> is a complete overlay)
 *   -n  dry run: convert and count, write nothing
 *   -V  run the structural verifier (npa_verify) on every converted buffer before writing it
 *   -z  deflate level for .zlib outputs (default 6)
 * A converted tree is also valid input for the load-time hook: every converter recognises the
 * converted state and reports NPA_ALREADY, so hook and tree can be mixed freely.
 * Exit status is non-zero if any file failed to convert; failed files are not written.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifndef NPA_NO_ZLIB
#include <zlib.h>
#endif

#include "np_assets.h"
#include "npa_verify.h"

static int g_link, g_dry, g_verify, g_quiet, g_level = 6;
static const char *g_src, *g_dst;
static unsigned long g_conv, g_already, g_fail, g_linked, g_skipped;
static size_t g_in_bytes, g_out_bytes;
static double g_secs;

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int mkdirs(const char* path)
{
    char tmp[1024];
    size_t i, n = strlen(path);
    if (n >= sizeof tmp) return -1;
    memcpy(tmp, path, n + 1);
    for (i = 1; i < n; i++)
        if (tmp[i] == '/')
        {
            tmp[i] = 0;
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            tmp[i] = '/';
        }
    return (mkdir(tmp, 0755) != 0 && errno != EEXIST) ? -1 : 0;
}

static unsigned char* read_all(const char* path, size_t* size)
{
    FILE* f = fopen(path, "rb");
    struct stat st;
    unsigned char* raw;
    if (!f) return NULL;
    fstat(fileno(f), &st);
    raw = (unsigned char*)malloc((size_t)st.st_size + 1);
    if (!raw || fread(raw, 1, (size_t)st.st_size, f) != (size_t)st.st_size) { fclose(f); free(raw); return NULL; }
    fclose(f);
    *size = (size_t)st.st_size;
    return raw;
}

/* 32-byte aligned buffer (the loaders' chunk alignment is computed on the absolute address) */
static void* aligned_copy(const void* src, size_t n)
{
    void* out;
    if (posix_memalign(&out, 32, n + 32) != 0) return NULL;
    memcpy(out, src, n);
    return out;
}

static int write_file(const char* path, const void* data, size_t n)
{
    FILE* f;
    char dir[1024];
    char* slash;
    snprintf(dir, sizeof dir, "%s", path);
    slash = strrchr(dir, '/');
    if (slash) { *slash = 0; if (mkdirs(dir) != 0) return -1; }
    f = fopen(path, "wb");
    if (!f) return -1;
    if (fwrite(data, 1, n, f) != n) { fclose(f); return -1; }
    return fclose(f);
}

static void do_file(const char* rel)
{
    char sp[1024], dp[1024];
    int z = 0;
    NpaKind k = npa_classify(rel, &z);
    size_t rawsz = 0, size = 0;
    unsigned char *raw = NULL, *buf = NULL;
    NpaResult r;
    double t0;
    snprintf(sp, sizeof sp, "%s/%s", g_src, rel);
    snprintf(dp, sizeof dp, "%s/%s", g_dst, rel);
    if (k == NPA_K_NONE)
    {
        if (g_link && !g_dry)
        {
            char dir[1024];
            char* slash;
            snprintf(dir, sizeof dir, "%s", dp);
            slash = strrchr(dir, '/');
            if (slash) { *slash = 0; mkdirs(dir); }
            {
                char* abs = realpath(sp, NULL);
                if (abs && (symlink(abs, dp) == 0 || errno == EEXIST)) g_linked++;
                free(abs);
            }
        }
        else g_skipped++;
        return;
    }
#ifdef NPA_NO_ZLIB
    if (z) { g_skipped++; return; }
#endif
    raw = read_all(sp, &rawsz);
    if (!raw) { fprintf(stderr, "%s: read failed\n", sp); g_fail++; return; }
#ifndef NPA_NO_ZLIB
    if (z)
    {
        uLongf outlen;
        uint32_t want;
        if (rawsz < 5) { fprintf(stderr, "%s: too short for a zlib container\n", sp); g_fail++; free(raw); return; }
        want = npa_zlib_container_size(raw);
        outlen = want;
        if (posix_memalign((void**)&buf, 32, (size_t)want + 32) != 0) { g_fail++; free(raw); return; }
        if (uncompress(buf, &outlen, raw + 4, (uLong)rawsz - 4) != Z_OK || outlen != want)
        {
            fprintf(stderr, "%s: inflate failed\n", sp);
            g_fail++; free(raw); free(buf); return;
        }
        size = want;
    }
    else
#endif
    {
        buf = (unsigned char*)aligned_copy(raw, rawsz);
        size = rawsz;
        if (!buf) { g_fail++; free(raw); return; }
    }
    t0 = now();
    npa_convert(k, buf, size, &r);
    g_secs += now() - t0;
    if (r.status < 0)
    {
        if (r.status != NPA_ERR_UNSUPPORTED || !g_quiet)
            fprintf(stderr, "FAIL %-10s %s : %s\n", npa_kind_name(k), rel, r.msg);
        if (r.status == NPA_ERR_UNSUPPORTED) g_skipped++; else g_fail++;
        free(raw); free(buf);
        return;
    }
    if (g_verify)
    {
        char vmsg[256] = "";
        if (npav_verify(k, buf, size, vmsg, sizeof vmsg) != 0)
        {
            fprintf(stderr, "FAIL %-10s %s : verify: %s\n", npa_kind_name(k), rel, vmsg);
            g_fail++; free(raw); free(buf);
            return;
        }
    }
    if (r.status == NPA_ALREADY) g_already++;
    else g_conv++;
    g_in_bytes += rawsz;
    if (!g_dry)
    {
        const void* out = buf;
        size_t outsz = size;
        unsigned char* z_out = NULL;
#ifndef NPA_NO_ZLIB
        if (z)
        {
            uLongf cap = compressBound((uLong)size) + 4, got;
            z_out = (unsigned char*)malloc(cap);
            if (!z_out) { g_fail++; free(raw); free(buf); return; }
            z_out[0] = (unsigned char)(size >> 24); z_out[1] = (unsigned char)(size >> 16);
            z_out[2] = (unsigned char)(size >> 8);  z_out[3] = (unsigned char)size; /* BE size word, as on disc */
            got = cap - 4;
            if (compress2(z_out + 4, &got, buf, (uLong)size, g_level) != Z_OK) { g_fail++; free(raw); free(buf); free(z_out); return; }
            out = z_out;
            outsz = got + 4;
        }
#endif
        if (write_file(dp, out, outsz) != 0)
        {
            fprintf(stderr, "%s: write failed (%s)\n", dp, strerror(errno));
            g_fail++;
        }
        else
            g_out_bytes += outsz;
        free(z_out);
    }
    if (!g_quiet) printf("%-10s %s%s\n", npa_kind_name(k), rel, r.status == NPA_ALREADY ? " (already host order)" : "");
    free(raw);
    free(buf);
}

static void walk(const char* rel)
{
    char dir[1024], p[1024], r2[1024];
    DIR* d;
    struct dirent* e;
    struct stat st;
    snprintf(dir, sizeof dir, "%s/%s", g_src, rel);
    d = opendir(dir);
    if (!d) return;
    while ((e = readdir(d)))
    {
        if (e->d_name[0] == '.') continue;
        if (rel[0]) snprintf(r2, sizeof r2, "%s/%s", rel, e->d_name);
        else snprintf(r2, sizeof r2, "%s", e->d_name);
        snprintf(p, sizeof p, "%s/%s", g_src, r2);
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) walk(r2);
        else do_file(r2);
    }
    closedir(d);
}

int main(int argc, char** argv)
{
    int i, nargs = 0;
    const char* args[2] = { NULL, NULL };
    for (i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "-l")) g_link = 1;
        else if (!strcmp(argv[i], "-n")) g_dry = 1;
        else if (!strcmp(argv[i], "-V")) g_verify = 1;
        else if (!strcmp(argv[i], "-q")) g_quiet = 1;
        else if (!strcmp(argv[i], "-z") && i + 1 < argc) g_level = atoi(argv[++i]);
        else if (nargs < 2) args[nargs++] = argv[i];
        else nargs = 3;
    }
    if (nargs != 2)
    {
        fprintf(stderr, "usage: %s [-l] [-n] [-V] [-q] [-z level] <srcdir> <dstdir>\n", argv[0]);
        return 2;
    }
    g_src = args[0];
    g_dst = args[1];
    if (!g_dry && mkdirs(g_dst) != 0) { fprintf(stderr, "cannot create %s\n", g_dst); return 2; }
    walk("");
    printf("--- %lu converted, %lu already host order, %lu failed, %lu linked, %lu skipped; read %zu bytes, wrote %zu bytes; convert time %.3f ms\n",
           g_conv, g_already, g_fail, g_linked, g_skipped, g_in_bytes, g_out_bytes, g_secs * 1e3);
    return g_fail ? 1 : 0;
}
