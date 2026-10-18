/*
 * np_hook.h - the load-time conversion hook the decomp's file layer calls (NP_NATIVE builds).
 *
 *     int tag = npa_hook_tag(path);                        // when a load is requested (cheap)
 *     ... the file is read, and inflated when it is an NLG .zlib container ...
 *     npa_hook_convert(tag, buffer, size, path);           // before any loader looks at the buffer
 *
 * tag is 0 for files that need no conversion. npa_hook_convert never fails the load: a file the
 * converter rejects is reported through the log callback (every time it is loaded) and left as it
 * is, exactly as a corrupt file would behave. Safe to call from several threads (async loads). Conversion is idempotent, so a buffer may pass through the hook twice
 * (nlLoadEntireFile followed by a loader that calls it again, or data pre-converted offline).
 *
 * Plain C linkage so both the C++ decomp files and C platform code can call it.
 */
#ifndef NP_HOOK_H
#define NP_HOOK_H

#ifdef __cplusplus
extern "C" {
#endif

/* 0 = nothing to convert, otherwise (kind) | (is_zlib << 8) | 0x10000 */
int npa_hook_tag(const char* path);
void npa_hook_convert(int tag, void* data, unsigned long size, const char* path);

/* Convert a buffer whose kind is known by the caller (NisPlayer's raw async read, BundleFile...). */
void npa_hook_convert_kind(int kind, void* data, unsigned long size, const char* what);

/* Diagnostics: called with a one-line message per rejected file; default writes to stderr. */
void npa_hook_set_log(void (*fn)(const char* line));

/* Switch the hook off (data set already converted by tools/assets/npa_convert_tree: the converters
 * would only run their cheap "already converted" test anyway). */
void npa_hook_enable(int on);

/* Statistics since start-up (for the lab overlay / performance work). */
void npa_hook_stats(unsigned long* files, unsigned long* bytes, unsigned long* failed, double* seconds);

#ifdef __cplusplus
}
#endif

#endif /* NP_HOOK_H */
