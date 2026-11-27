// The console glue of the start check for Picture and Language changes (np_change_guard.h): the card, the log and the one notice.
#if defined(__vita__)

#include "np_change_guard_vita.h"

#include "np_change_guard.h"
#include "np_vita_internal.h"
#include "np_vita_settings.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <pthread.h>
#include <string>

namespace NpChangeGuardVita {
namespace {

constexpr const char* kFile = "ux0:data/mscharged/np-change.txt";
constexpr const char* kNotice = "2 FAILED STARTS: STANDARD PICTURE AND LANGUAGE";   // np_text_strings.tsv, 46 characters at most in every language

pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;   // (static initialiser: Begin runs before the C++ constructors of the SDK)
bool g_loadingLine = false;

bool Exists(const char* path) {
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}

int ReadFile(const char* path, std::string& out) {
    if (!Exists(path)) return 1;
    const SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return 2;
    char buffer[2048];
    for (;;) {
        const int n = sceIoRead(fd, buffer, sizeof(buffer));
        if (n < 0) { sceIoClose(fd); return 2; }
        if (n == 0) break;
        out.append(buffer, static_cast<size_t>(n));
        if (out.size() > 64 * 1024) break;
    }
    sceIoClose(fd);
    return 0;
}

class CardFiles : public NpChangeGuard::Files {
public:
    int ReadSettings(std::string& out) override { return ReadFile(NpSettingsScreen::SavedFilePath(), out); }
    int ReadGuard(std::string& out) override { return ReadFile(kFile, out); }
    bool WriteGuard(const std::string& text) override { return NpSettingsScreen::WriteSettingsText(kFile, text); }   // tmp, fsync, rename (np_safe_file.h)
};

// The guard lives in raw storage built on first use, under the mutex: EarlyBoot (init_priority 101) calls Begin before this file's own static constructors have run, so a plain
// global object here would still be zero (found in the emulator: a null call at start). Nothing below needs a constructor to have run but the mutex, which is constant-initialised.
alignas(CardFiles) unsigned char g_cardStorage[sizeof(CardFiles)];
alignas(NpChangeGuard::Guard) unsigned char g_guardStorage[sizeof(NpChangeGuard::Guard)];
bool g_built = false;
NpChangeGuard::Guard& G() {   // call with the mutex held
    if (!g_built) {
        CardFiles* card = new (g_cardStorage) CardFiles();
        new (g_guardStorage) NpChangeGuard::Guard(*card);
        g_built = true;
    }
    return *reinterpret_cast<NpChangeGuard::Guard*>(g_guardStorage);
}

class Lock {
public:
    Lock() { pthread_mutex_lock(&g_mutex); }
    ~Lock() { pthread_mutex_unlock(&g_mutex); }
};

bool EnvOff() {
    for (const char* kit : {"MSC_BOOT_MATCH", "MSC_INPUT_SCRIPT"}) {
        const char* v = std::getenv(kit);
        if (v && *v) return true;
    }
    const char* g = std::getenv("MSC_CHANGE_GUARD");
    return g && std::strcmp(g, "0") == 0;
}

}  // namespace

bool Begin() {
    Lock lock;
    const NpChangeGuard::Guard::State state = G().Begin(EnvOff());
    g_loadingLine = state == NpChangeGuard::Guard::State::kFallback;
    switch (state) {
    case NpChangeGuard::Guard::State::kFallback: {
        std::fprintf(stderr, "[change] the Picture and Language lines of np-player.txt did not get through two starts in a row: the standard ones are used this start (np-player.txt is not changed): %s\n",
                     G().Lines().c_str());
        char note[200];
        std::snprintf(note, sizeof(note), "settings: two failed starts after a change: standard picture and language used, np-player.txt kept: %.110s", G().Lines().c_str());
        VitaLog::Note(note);
        break;
    }
    case NpChangeGuard::Guard::State::kReserved:
        std::fprintf(stderr, "[change] Picture or Language lines changed (%s): start %d of this change is counted in %s until the start is stable\n", G().Lines().c_str(),
                     G().record().tries, kFile);
        break;
    default:
        break;
    }
    return g_loadingLine;
}

bool Skips(const char* key) {
    Lock lock;
    return G().Skips(key);
}

const char* LoadingLine() { return g_loadingLine ? kNotice : nullptr; }

bool Cancel() {
    Lock lock;
    return G().Cancel();
}

void Restore() {
    Lock lock;
    G().Restore();
}

void Failed() {
    // From the crash handler: never wait for a holder that the fault may have interrupted.
    if (pthread_mutex_trylock(&g_mutex) != 0) return;
    G().Failed();
    pthread_mutex_unlock(&g_mutex);
}

void Stable() {
    Lock lock;
    const bool was = G().state() == NpChangeGuard::Guard::State::kReserved || G().state() == NpChangeGuard::Guard::State::kCancelled;
    G().Stable();
    if (was) std::fprintf(stderr, "[change] the start is stable: the Picture and Language lines (%s) are now the good ones\n", G().Lines().c_str());
}

void SettingsSaved() {
    Lock lock;
    G().SettingsSaved();
}

}  // namespace NpChangeGuardVita

#endif
