/*
 * np_convert.c - file classification and the single conversion entry point.
 */
#include <ctype.h>
#include <string.h>

#include "np_internal.h"

static const char* const kKindNames[NPA_K_COUNT] = {
    "none", "rlg", "rlt", "sanim", "shier", "cam", "nis", "cph", "retarget", "world.res", "world.tmp",
    "bun", "resbun", "fen", "bundlefile", "loc", "trg", "byte_code"
};

const char* npa_kind_name(NpaKind kind)
{
    return (unsigned)kind < NPA_K_COUNT ? kKindNames[kind] : "?";
}

/* case-insensitive "path ends with suffix" */
static int ends_with(const char* s, size_t n, const char* suf)
{
    size_t m = strlen(suf), i;
    if (m > n) return 0;
    for (i = 0; i < m; i++)
        if (tolower((unsigned char)s[n - m + i]) != suf[i]) return 0;
    return 1;
}

/* the whole path lower-cased into buf (backslashes become '/'), *base = start of the file name */
static void path_lower(const char* path, char* buf, size_t cap, const char** base)
{
    size_t i = 0;
    const char* b = buf;
    for (; path[i] && i + 1 < cap; i++)
    {
        char ch = (char)tolower((unsigned char)path[i]);
        if (ch == '\\') ch = '/';
        buf[i] = ch;
        if (ch == '/') b = buf + i + 1;
    }
    buf[i] = 0;
    *base = b;
}

NpaKind npa_classify(const char* path, int* is_zlib)
{
    char full[256];
    const char* b;
    size_t n;
    int z = 0;
    NpaKind k = NPA_K_NONE;
    if (is_zlib) *is_zlib = 0;
    if (path == NULL) return NPA_K_NONE;
    path_lower(path, full, sizeof full, &b);
    n = strlen(b);
    if (ends_with(b, n, ".zlib"))
    {
        z = 1;
        n -= 5;
        full[strlen(full) - 5] = 0; /* b points into full, so it now ends at the inner extension */
    }
    if (ends_with(b, n, ".rlg")) k = NPA_K_RLG;
    else if (ends_with(b, n, ".rlt")) k = NPA_K_RLT;
    else if (ends_with(b, n, ".sanim")) k = NPA_K_SANIM;
    else if (ends_with(b, n, ".shier")) k = NPA_K_SHIER;
    else if (ends_with(b, n, ".cam")) k = NPA_K_CAM;
    else if (ends_with(b, n, ".nis")) k = NPA_K_NIS;
    else if (ends_with(b, n, ".cph")) k = NPA_K_CPH;
    else if (ends_with(b, n, ".fen")) k = NPA_K_FEN;
    else if (ends_with(b, n, ".loc")) k = NPA_K_LOC;
    else if (ends_with(b, n, ".trg")) k = NPA_K_TRG;
    else if (ends_with(b, n, ".byte_code")) k = NPA_K_BYTECODE;
    else if (ends_with(b, n, ".resbun")) k = NPA_K_RESBUN;
    else if (ends_with(b, n, ".bun")) k = NPA_K_BUN;
    else if (ends_with(b, n, ".res") || ends_with(b, n, ".dmn"))
    {
        /* gameworld.res is the stadium world resource, every other .res / .dmn is a BundleFile */
        k = (strcmp(b, "gameworld.res") == 0) ? NPA_K_WORLD_RES : NPA_K_BUNDLEFILE;
    }
    else if (ends_with(b, n, ".tmp") && strcmp(b, "gameworld.tmp") == 0) k = NPA_K_WORLD_TMP;
    else if (ends_with(b, n, ".bin") && strstr(full, "/animretarget/") != NULL) k = NPA_K_RETARGET;
    if (is_zlib) *is_zlib = z;
    return k;
}

int npa_convert(NpaKind kind, void* data, size_t size, NpaResult* res)
{
    switch (kind)
    {
    case NPA_K_NONE: { NpaResult d; if (!res) res = &d; memset(res, 0, sizeof *res); res->status = NPA_ALREADY; return NPA_ALREADY; }
    case NPA_K_RLG: return npa_convert_rlg(data, size, res);
    case NPA_K_RLT: return npa_convert_rlt(data, size, res);
    case NPA_K_SANIM: return npa_convert_sanim(data, size, res);
    case NPA_K_SHIER: return npa_convert_shier(data, size, res);
    case NPA_K_CAM: return npa_convert_cam(data, size, res);
    case NPA_K_NIS: return npa_convert_nis(data, size, res);
    case NPA_K_CPH: return npa_convert_cph(data, size, res);
    case NPA_K_RETARGET: return npa_convert_retarget(data, size, res);
    case NPA_K_WORLD_RES: return npa_convert_world_res(data, size, res);
    case NPA_K_WORLD_TMP: return npa_convert_world_tmp(data, size, res);
    case NPA_K_FEN: return npa_convert_fen(data, size, res);
    case NPA_K_BUN: return npa_convert_bun(data, size, res);
    case NPA_K_RESBUN: return npa_convert_resbun(data, size, res);
    case NPA_K_LOC: return npa_convert_loc(data, size, res);
    case NPA_K_TRG: return npa_convert_trg(data, size, res);
    case NPA_K_BYTECODE: return npa_convert_bytecode(data, size, res);
    case NPA_K_BUNDLEFILE: return npa_convert_bundlefile(data, size, res);
    default:
    {
        NpaResult d;
        if (!res) res = &d;
        memset(res, 0, sizeof *res);
        res->status = NPA_ERR_UNSUPPORTED;
        snprintf(res->msg, sizeof res->msg, "no converter for kind %s yet", npa_kind_name(kind));
        return res->status;
    }
    }
}
