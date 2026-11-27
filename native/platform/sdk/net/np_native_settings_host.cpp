// Host proof backend of np_native_settings.h: the settings file is the one named by MSC_SETTINGS_FILE (a host test or a picture run; unset = no backend answer, the screen is
// not offered). Never compiled into the Vita build (the console backend is the "native18" block of platform/vita/src/np_vita_settings.cpp).
#if !defined(__vita__)
#include "platform/sdk/net/np_native_settings.h"
#include "platform/sdk/include/np/np_text.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <pthread.h>
#include <unistd.h>

namespace {
int g_restarts = 0, g_openAlls = 0;
}

extern "C" bool NpNativeSettingsBackendRead(char* out, int cap, int* length) {
    const char* path = std::getenv("MSC_SETTINGS_FILE");
    if (!path || !*path) return false;
    *length = 0;
    FILE* f = std::fopen(path, "rb");
    if (!f) return true;   // no file yet: empty text, every row at its default
    const size_t n = std::fread(out, 1, static_cast<size_t>(cap), f);
    std::fclose(f);
    *length = static_cast<int>(n);
    return true;
}

extern "C" bool NpNativeSettingsBackendWrite(const char* text, int length) {
    const char* path = std::getenv("MSC_SETTINGS_FILE");
    if (!path || !*path) return false;
    std::string tmp = std::string(path) + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(text, 1, static_cast<size_t>(length), f) == static_cast<size_t>(length);
    if (std::fclose(f) != 0 || !ok) return false;
    return std::rename(tmp.c_str(), path) == 0;
}

// ---- host test harness: MSC_LIVE_FLIP="<ms>:<NAME>=<VALUE>[,<NAME>=<VALUE>...][;<ms>:...]" flips live rows in a running host game through the native model, exactly as the
// player's save does (Open, Press until the row reads VALUE, Save): NAME is the row's English name (CROWD, LESS SHAKE, REAR TOUCH), VALUE its English value (HIDE, ON, OFF, ...).
// <ms> is the wall-clock time since the start. It needs MSC_SETTINGS_FILE (the proof backend's file). Used by w8/live19.md's proofs; unset = nothing runs.
namespace {
struct FlipStep {
    unsigned ms;
    std::string rows;
};
void* FlipThread(void* arg) {
    std::vector<FlipStep>* steps = static_cast<std::vector<FlipStep>*>(arg);
    unsigned elapsed = 0;
    for (const FlipStep& step : *steps) {
        if (step.ms > elapsed) usleep((step.ms - elapsed) * 1000u);
        elapsed = step.ms > elapsed ? step.ms : elapsed;
        if (!NpNativeSettings::Open(NpText::kEnglish)) {
            std::fprintf(stderr, "[live-flip] the model did not open\n");
            continue;
        }
        size_t at = 0;
        while (at < step.rows.size()) {
            size_t end = step.rows.find(',', at);
            if (end == std::string::npos) end = step.rows.size();
            const std::string pair = step.rows.substr(at, end - at);
            at = end + 1;
            const size_t eq = pair.find('=');
            if (eq == std::string::npos) continue;
            const std::string name = pair.substr(0, eq), value = pair.substr(eq + 1);
            bool done = false;
            for (int p = 0; p < NpNativeSettings::Pages() && !done; ++p)
                for (int r = 0; r < NpNativeSettings::Rows(p) && !done; ++r) {
                    char n[64], v[64];
                    NpNativeSettings::Name(p, r, n, sizeof n);
                    if (name != n) continue;
                    for (int k = 0; k < 6 && !done; ++k) {
                        NpNativeSettings::Value(p, r, v, sizeof v);
                        if (value == v) done = true;
                        else NpNativeSettings::Press(p, r, 1);
                    }
                }
            std::fprintf(stderr, "[live-flip] %s=%s %s\n", name.c_str(), value.c_str(), done ? "set" : "NOT FOUND");
        }
        const bool changed = NpNativeSettings::Changed();
        const bool saved = NpNativeSettings::Save();
        std::fprintf(stderr, "[live-flip] t=%u ms: changed=%d save=%d\n", step.ms, changed ? 1 : 0, saved ? 1 : 0);
        NpNativeSettings::Close();
    }
    delete steps;
    return nullptr;
}
struct FlipStarter {
    FlipStarter() {
        const char* spec = std::getenv("MSC_LIVE_FLIP");
        if (!spec || !*spec) return;
        std::vector<FlipStep>* steps = new std::vector<FlipStep>();
        std::string all = spec;
        size_t at = 0;
        while (at < all.size()) {
            size_t end = all.find(';', at);
            if (end == std::string::npos) end = all.size();
            const std::string one = all.substr(at, end - at);
            at = end + 1;
            const size_t colon = one.find(':');
            if (colon == std::string::npos) continue;
            steps->push_back({static_cast<unsigned>(std::atoi(one.substr(0, colon).c_str())), one.substr(colon + 1)});
        }
        pthread_t t;
        if (pthread_create(&t, nullptr, FlipThread, steps) == 0) pthread_detach(t);
    }
} g_flipStarter;
}  // namespace

extern "C" bool NpNativeSettingsBackendRestart(void) {
    ++g_restarts;
    std::fprintf(stderr, "[native-settings] host: restart requested (not performed)\n");
    return true;
}

extern "C" int NpNativeSettingsHostRestarts(void) { return g_restarts; }   // the unit tests count the restarts the model asked for

extern "C" bool NpNativeSettingsBackendOpenAll(void) {
    ++g_openAlls;
    std::fprintf(stderr, "[native-settings] host: ONLINE page requested (not opened)\n");
    return true;
}
#endif
