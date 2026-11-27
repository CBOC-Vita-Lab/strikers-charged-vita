// Stdio that is safe to use from several threads on the Vita, and the bounded retry of the opens a start-up or a match load cannot do without.
//
// Why (11 Nov 2026, from the sibling Galaxy port's console crash dumps; same vitasdk, same libc): newlib's fopen picks a free FILE slot of the process-wide table
// without a lock on this target. Two threads that open a file at the same moment can get the same slot: the stream's descriptor is -1, the reads return nothing, and
// whoever needed the file (the game aborted at launch: "game data not found") fails on a card that is fine. Strikers opens files from many threads (game, render:
// HD textures and the shader list, the loading screen, audio, the disc reader's helper, the shader compiler's cache, the log files).
//
// np_stdio_lock.cpp defines __wrap_fopen / __wrap_freopen / __wrap_fdopen / __wrap_tmpfile / __wrap_fclose (vita.mk adds -Wl,--wrap=...): one process-wide lock around the real
// call, so the slot search is serial. The lock never blocks for ever: after 3 s the call goes on without it (and is counted) rather than hang the game.
//
// np_boot_fopen / NpBootTry: for a file the start-up or the match load must have. A failed open (or a read that came back empty, which the caller reports with np_boot_try_again)
// is tried again every 50 ms for up to 3 s, each failure logged with its errno, the loading screen showing "retrying" meanwhile; the caller then fails soft.
//   MSC_BOOTIO_INJECT=<n>   (tests, any build) the first n attempts of np_boot_fopen fail with EIO: proves the retry path without a faulty card.
//   MSC_BOOTIO_MS=<ms>      the retry budget, default 3000 (tests use a small one).
#pragma once
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NpBootTry {
    unsigned attempts;
    unsigned long long startUs;
} NpBootTry;

void np_boot_try_begin(NpBootTry* t);
// One failed attempt of `what` (a short label) on `path`, with its errno: log it, show the loading-screen notice, wait 50 ms. Returns 1 when the caller should try
// again, 0 when the budget is spent (then the caller fails soft). Never sleeps after the last attempt.
int np_boot_try_again(NpBootTry* t, const char* what, const char* path, int err);
// fopen with the retry above. Null when the file is still not there after the budget (errno of the last failure). Adds the injected failures (MSC_BOOTIO_INJECT).
FILE* np_boot_fopen(const char* path, const char* mode, const char* what);
// One attempt (the injected failures and the descriptor check, no retry): for a caller that retries a whole load (open and read) itself with np_boot_try_again.
FILE* np_boot_fopen_once(const char* path, const char* mode);

// Counters of the stdio lock (tests and the log): opens through it, waits that had to sleep, waits that gave up after 3 s.
void np_stdio_lock_stats(unsigned* calls, unsigned* waits, unsigned* timeouts);

// The loading screen (vita_bootscreen.cpp) defines this; weak, so a host test links without it.
void NpBootScreenNotice(void) __attribute__((weak));

#ifdef __cplusplus
}
#endif
