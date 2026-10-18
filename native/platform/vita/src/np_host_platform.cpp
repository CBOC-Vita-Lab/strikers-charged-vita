// The np_vita.h functions on host32, armhf and x86-64 Linux, so that the SDK layer is written once:
// arenas (aligned heap blocks), the data and game paths (environment), a quiet platform. The audio
// queue (np_vita_audio.cpp, a discarding "device" thread), the job pool and the harness are the same
// files as on the Vita. Not compiled for the Vita.
#if !defined(__vita__)

#include "np_vita.h"
#include "np_vita_internal.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

extern "C" {

// The data directory: NP_DATA_DIR, else the current directory. Game files: MSC_GAME_ROOT, else
// <data>/game (the extracted disc: sys/ and files/).
const char* NpDataDevicePath(void) {
    static char path[256];
    if (!path[0]) {
        const char* e = std::getenv("NP_DATA_DIR");
        if (e && *e) std::snprintf(path, sizeof(path), "%s", e);
        else if (!getcwd(path, sizeof(path))) std::snprintf(path, sizeof(path), ".");
    }
    return path;
}
const char* NpGameRootPath(void) {
    static char path[300];
    if (!path[0]) {
        const char* e = std::getenv("MSC_GAME_ROOT");
        if (e && *e) std::snprintf(path, sizeof(path), "%s", e);
        else std::snprintf(path, sizeof(path), "%s/game", NpDataDevicePath());
    }
    return path;
}

void* NpAllocArena(size_t bytes, size_t alignment, const char* name) {
    if (alignment < 4096) alignment = 4096;
    const size_t request = (bytes + alignment - 1) & ~(alignment - 1);
    void* p = nullptr;
    if (posix_memalign(&p, alignment, request) != 0) {
        std::fprintf(stderr, "[np] arena \"%s\" (%u KiB): allocation failed\n", name ? name : "?", static_cast<unsigned>(bytes / 1024u));
        return nullptr;
    }
    std::fprintf(stderr, "[np] arena \"%s\": %u KiB at %p\n", name ? name : "?", static_cast<unsigned>(request / 1024u), p);
    NpHarnessRegisterRegion(name, p, static_cast<uint32_t>(request));   // MSC_STATE_HASH covers it (deterministic where the heap is: qemu-user, setarch -R)
    return p;
}

int NpVitaCpuMHz(void) { return 0; }
void NpVitaKeepAwake(void) {}
void NpPlatformExit(int code) {
    std::fflush(nullptr);
    _exit(code);
}
void NpPlatformFatal(const char* category, const char* details) {
    std::fprintf(stderr, "[np] FATAL: %s\n[np] %s\n", category ? category : "", details ? details : "");
    std::fflush(stderr);
}

} // extern "C"

#endif
