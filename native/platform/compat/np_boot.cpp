// The game's start-up seen from the port (W5 BOOT): see include/np/boot.h.
#include "np/boot.h"
#include "np/fps.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

// The Vita platform's log line with the time since the process started (platform/vita/src/np_vita_platform.cpp). Weak: the host builds have none.
extern "C" void NpPlatformBootMark(const char* what) __attribute__((weak));

extern "C" void NpBootMark(const char* what) {
    if (what && std::strncmp(what, "boot screens", 12) == 0) NpFpsBootDone(0);
    if (what && std::strncmp(what, "title screen", 12) == 0) NpFpsBootDone(1);
    if (NpPlatformBootMark) {
        NpPlatformBootMark(what);
        return;
    }
    std::fprintf(stderr, "[boot] %s\n", what ? what : "");
}

extern "C" int NpSkipStrapScreen(void) {
    static int decided = -1;
    if (decided < 0) {
        const char* e = std::getenv("MSC_SKIP_STRAP");
#if defined(__vita__)
        decided = (e && *e && std::atoi(e) == 0) ? 0 : 1;
#else
        decided = (e && *e && std::atoi(e) != 0) ? 1 : 0;
#endif
        std::fprintf(stderr, "[boot] Wii Remote strap and Nunchuk screens: %s (MSC_SKIP_STRAP=%d)\n", decided ? "skipped" : "shown", decided);
    }
    return decided;
}
