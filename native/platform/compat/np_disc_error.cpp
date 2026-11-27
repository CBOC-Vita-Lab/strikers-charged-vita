#include "np/disc_error.h"
#include <cstdio>

extern "C" int NpDiscGetReadError(char*, size_t) __attribute__((weak));
extern "C" void NpPlatformFatal(const char*, const char*) __attribute__((weak));
extern "C" void NpPlatformExit(int) __attribute__((weak));
extern "C" int np_in_irq(void) __attribute__((weak));
extern "C" void np_irq_release_for_wait(int*) __attribute__((weak));
extern "C" void np_irq_reacquire_after_wait(int) __attribute__((weak));

extern "C" int NpDiscReportReadError(void) {
    if (!NpDiscGetReadError || !NpPlatformFatal || !NpPlatformExit || !np_in_irq ||
        !np_irq_release_for_wait || !np_irq_reacquire_after_wait) return 0;
    if (np_in_irq()) return 0;
    char error[1200];
    if (!NpDiscGetReadError(error, sizeof error)) return 0;
    char details[1536];
    std::snprintf(details, sizeof details,
        "%s\n\nRe-copy the complete extracted game to ux0:data/mscharged/game "
        "using the installer, or prepare it again from your own Wii USA Rev 1 game file. "
        "Keep the sys and files folders together.", error);
    // Movie prefetch can reach the game-side fatal check with interrupts disabled.
    int savedInterrupts = 0;
    np_irq_release_for_wait(&savedInterrupts);
    NpPlatformFatal("Game files are missing or incomplete", details);
    NpPlatformExit(1);
    np_irq_reacquire_after_wait(savedInterrupts);
    return 1;
}
