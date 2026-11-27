// The three live cells (np/live_settings.h, platform/compat/np_live_settings.cpp) and what feeds them: the first read equals the old getenv rule, Set stores a value without
// setenv, an unknown key is ignored, a default choice gives the unset rule, and a save through the native model (NpNativeSettings::Save, ApplyLive) reaches the three readers
// (NpShakeScale, NpPerformanceMode, NpControls::RearEnabled). Run with mutants by run_live_settings_tests.py.
//   test_live_settings read shake|crowd|rear     prints the value of the cell after its first read (the runner sets the environment)
//   test_live_settings                           the in-process tests
#include "np/comfort.h"
#include "np/live_settings.h"
#include "np/perfmode.h"
#include "platform/sdk/input/np_controls.h"
#include "platform/sdk/include/np/np_text.h"
#include "platform/sdk/net/np_native_settings.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

static int g_checks = 0, g_failed = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failed; std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

namespace NN = NpNativeSettings;

static std::string ReadRaw(const std::string& path) {
    std::string out;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return out;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

int main(int argc, char** argv) {
    if (argc == 3 && !std::strcmp(argv[1], "read")) {
        const int id = !std::strcmp(argv[2], "shake") ? NP_LIVE_SHAKE : !std::strcmp(argv[2], "crowd") ? NP_LIVE_CROWD : NP_LIVE_REAR;
        std::printf("%d\n", id == NP_LIVE_SHAKE ? NpLiveShakeQuarter() : id == NP_LIVE_CROWD ? NpLiveCrowdHidden() : NpLiveRear());
        return 0;
    }
    if (argc == 2 && !std::strcmp(argv[1], "setfirst")) {   // a Set before the first read wins over the environment (the environment says the opposite)
        setenv("MSC_REDUCED_MOTION", "1", 1); setenv("MSC_PERFORMANCE_MODE", "1", 1); setenv("MSC_REAR_TOUCH", "1", 1);
        NpLiveSettingsSet("MSC_REDUCED_MOTION", "0"); NpLiveSettingsSet("MSC_PERFORMANCE_MODE", "0"); NpLiveSettingsSet("MSC_REAR_TOUCH", "0");
        const bool ok = NpLiveShakeQuarter() == 0 && NpLiveCrowdHidden() == 0 && NpLiveRear() == 0;
        if (!ok) std::fprintf(stderr, "FAIL setfirst\n");
        return ok ? 0 : 1;
    }
    if (argc == 2 && !std::strcmp(argv[1], "once")) {   // the first read is kept: a later setenv changes nothing
        setenv("MSC_REDUCED_MOTION", "1", 1); setenv("MSC_PERFORMANCE_MODE", "1", 1); setenv("MSC_REAR_TOUCH", "1", 1);
        const bool first = NpLiveShakeQuarter() == 1 && NpLiveCrowdHidden() == 1 && NpLiveRear() == 1;
        setenv("MSC_REDUCED_MOTION", "0", 1); setenv("MSC_PERFORMANCE_MODE", "0", 1); setenv("MSC_REAR_TOUCH", "0", 1);
        const bool ok = first && NpLiveShakeQuarter() == 1 && NpLiveCrowdHidden() == 1 && NpLiveRear() == 1;
        if (!ok) std::fprintf(stderr, "FAIL once\n");
        return ok ? 0 : 1;
    }
    // ---- the readers through their old names; nothing in the environment: the unset rule
    unsetenv("MSC_REDUCED_MOTION"); unsetenv("MSC_PERFORMANCE_MODE"); unsetenv("MSC_REAR_TOUCH"); unsetenv("MSC_CONTROLS");
    CHECK(NpShakeScale() == 1.0f && NpPerformanceMode() == 0);
    CHECK(NpControls::RearEnabled(NpControls::kModern) && !NpControls::RearEnabled(NpControls::kClassic));   // unset: on with MODERN, off with CLASSIC

    // ---- Set, then read; it never calls setenv
    CHECK(NpLiveSettingsSet("MSC_REDUCED_MOTION", "1") == 1 && NpShakeScale() == 0.25f);
    CHECK(NpLiveSettingsSet("MSC_PERFORMANCE_MODE", "1") == 1 && NpPerformanceMode() == 1);
    CHECK(NpLiveSettingsSet("MSC_REAR_TOUCH", "0") == 1 && !NpControls::RearEnabled(NpControls::kModern) && !NpControls::RearEnabled(NpControls::kClassic));
    CHECK(NpLiveSettingsSet("MSC_REAR_TOUCH", "1") == 1 && NpControls::RearEnabled(NpControls::kModern) && NpControls::RearEnabled(NpControls::kClassic));
    CHECK(!getenv("MSC_REDUCED_MOTION") && !getenv("MSC_PERFORMANCE_MODE") && !getenv("MSC_REAR_TOUCH"));
    // a value of 0 is off for the two switches; any other first character is on
    CHECK(NpLiveSettingsSet("MSC_REDUCED_MOTION", "0") == 1 && NpShakeScale() == 1.0f);
    CHECK(NpLiveSettingsSet("MSC_PERFORMANCE_MODE", "0") == 1 && NpPerformanceMode() == 0);
    CHECK(NpLiveSettingsSet("MSC_REDUCED_MOTION", "yes") == 1 && NpShakeScale() == 0.25f);
    // a default choice writes nothing: the unset rule (full shake, crowd shown, rear follows the layout)
    CHECK(NpLiveSettingsSet("MSC_REDUCED_MOTION", nullptr) == 1 && NpShakeScale() == 1.0f);
    CHECK(NpLiveSettingsSet("MSC_PERFORMANCE_MODE", nullptr) == 1 && NpPerformanceMode() == 0);
    CHECK(NpLiveSettingsSet("MSC_REAR_TOUCH", nullptr) == 1 && NpLiveRear() == -1 && NpControls::RearEnabled(NpControls::kModern) && !NpControls::RearEnabled(NpControls::kClassic));
    CHECK(NpLiveSettingsSet("MSC_REAR_TOUCH", "") == 1 && NpLiveRear() == -1);
    // an unknown key is ignored and moves no cell; so is a null key
    NpLiveSettingsSet("MSC_REDUCED_MOTION", "1"); NpLiveSettingsSet("MSC_PERFORMANCE_MODE", "1"); NpLiveSettingsSet("MSC_REAR_TOUCH", "0");
    CHECK(NpLiveSettingsSet("MSC_FPS", "30") == 0 && NpLiveSettingsSet("MSC_REDUCED_MOTIONS", "0") == 0 && NpLiveSettingsSet("", "0") == 0 && NpLiveSettingsSet(nullptr, "0") == 0);
    CHECK(NpShakeScale() == 0.25f && NpPerformanceMode() == 1 && NpLiveRear() == 0);
    // the environment is read once: a later setenv changes nothing
    setenv("MSC_REDUCED_MOTION", "0", 1);
    setenv("MSC_PERFORMANCE_MODE", "0", 1);
    setenv("MSC_REAR_TOUCH", "1", 1);
    CHECK(NpShakeScale() == 0.25f && NpPerformanceMode() == 1 && NpLiveRear() == 0);
    unsetenv("MSC_REDUCED_MOTION"); unsetenv("MSC_PERFORMANCE_MODE"); unsetenv("MSC_REAR_TOUCH");

    // ---- a save through the native model reaches the three readers (the running game's save)
    char dir[] = "/tmp/live-settings-XXXXXX";
    CHECK(mkdtemp(dir) != nullptr);
    const std::string path = std::string(dir) + "/np-player.txt";
    setenv("MSC_SETTINGS_FILE", path.c_str(), 1);
    NpLiveSettingsSet("MSC_REDUCED_MOTION", nullptr); NpLiveSettingsSet("MSC_PERFORMANCE_MODE", nullptr); NpLiveSettingsSet("MSC_REAR_TOUCH", nullptr);
    CHECK(NN::Open(NpText::kEnglish));
    CHECK(NN::Press(1, 0, 1) == NN::kChanged);   // CROWD: HIDE (set18 layout: PICTURE page)
    CHECK(NN::Press(3, 0, 1) == NN::kChanged);   // LESS SHAKE: ON (MATCH page)
    CHECK(NN::Press(2, 0, 1) == NN::kChanged);   // REAR TOUCH: ON (TOUCH page)
    CHECK(NpPerformanceMode() == 0 && NpShakeScale() == 1.0f && NpLiveRear() == -1);   // nothing applied before the save
    CHECK(NN::Save());
    CHECK(NpPerformanceMode() == 1 && NpShakeScale() == 0.25f && NpLiveRear() == 1);
    CHECK(!getenv("MSC_PERFORMANCE_MODE") && !getenv("MSC_REDUCED_MOTION") && !getenv("MSC_REAR_TOUCH"));
    CHECK(ReadRaw(path).find("MSC_PERFORMANCE_MODE=1") != std::string::npos);
    // a next-start row changes no cell; DON'T SAVE (Discard) applies nothing
    CHECK(NN::Press(0, 2, 1) == NN::kChanged && NN::NextStartChanged());   // FRAME RATE
    NN::Discard();
    CHECK(NpPerformanceMode() == 1 && NpShakeScale() == 0.25f && NpLiveRear() == 1 && !NN::Changed());
    NN::Press(1, 0, 1);   // CROWD back to SHOW, then not saved
    NN::Discard();
    CHECK(NpPerformanceMode() == 1);
    // the player sets them back: the unset rule
    NN::Press(1, 0, 1); NN::Press(3, 0, 1); NN::Press(2, 0, -1);   // SHOW, OFF, REAR: ON -> WITH MODERN
    CHECK(NN::Save());
    CHECK(NpPerformanceMode() == 0 && NpShakeScale() == 1.0f && NpLiveRear() == -1);
    // a save that changes only a next-start row applies no live row (they stay as they are)
    NpLiveSettingsSet("MSC_PERFORMANCE_MODE", "1");
    NN::Press(0, 2, 1);   // FRAME RATE
    CHECK(NN::Save() && NpPerformanceMode() == 1);
    // a failed write applies nothing
    NN::Press(1, 0, 1);   // CROWD
    unlink(path.c_str());
    char sub[300];
    std::snprintf(sub, sizeof sub, "%s/missing/np-player.txt", dir);
    setenv("MSC_SETTINGS_FILE", sub, 1);
    CHECK(!NN::Save() && NpPerformanceMode() == 1);
    NN::Close();
    std::remove((std::string(dir) + "/np-player.txt.tmp").c_str());
    rmdir(dir);
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
