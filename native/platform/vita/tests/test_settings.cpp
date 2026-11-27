// Tests of the Settings screen's model (np_settings.cpp): choices <-> np-player.txt lines, round trips, the file's other lines kept, the retired keys of the
// 1.3 prune (rows that went from the screen: old files, kit files, 1.3 development files), and the drawing (screenshots as PPM when a directory is given: test_settings <outdir>).
//   clang++ -std=c++17 -Iplatform/vita/src platform/vita/tests/test_settings.cpp platform/vita/src/np_settings.cpp platform/vita/src/vita_plan.cpp -o /tmp/test_settings && /tmp/test_settings
#include "np_settings.h"
#include "np_vita_internal.h"
#include "np_build_identity.h"
#include "np_settings_guard.cpp"   // (the guard itself, so that the old-file test runs it before Load as the console does; no separate link line)
#include "np_picture_presets.h"
#include "vita_plan_defaults.h"
#include "../../sdk/include/np/np_text.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int g_checks = 0, g_failed = 0;
#define CHECK(cond)                                                                \
    do {                                                                           \
        ++g_checks;                                                                \
        if (!(cond)) {                                                             \
            ++g_failed;                                                            \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
        }                                                                          \
    } while (0)

using namespace NpSettings;

// set18: FRAME RATE's choices follow VitaPlanDefaults::kLock60 (AUTO / SMOOTH 60 / 30 LOCKED, or SMOOTH 60 / 30 LOCKED once SMOOTH 60 is the build's mode)
static const int kSmooth = VitaPlanDefaults::kLock60 ? 0 : 1;
static const int kLocked = kSmooth + 1;

static int Find(const char* title) {
    for (int o = 0; o < OptionCount(); ++o)
        if (!std::strcmp(GetOption(o).title, title)) return o;
    return -1;
}
static bool Has(const std::string& text, const char* line) {
    size_t at = 0;
    while (at <= text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        if (text.compare(at, end - at, line) == 0 && std::strlen(line) == end - at) return true;
        at = end + 1;
    }
    return false;
}
static int CountLines(const std::string& text, const char* prefix) {
    int n = 0;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        if (text.compare(at, std::strlen(prefix), prefix) == 0) ++n;
        at = end + 1;
    }
    return n;
}
static void ForgetDefaultsForTest() { VitaPlan::ForgetDefaults(); }
static int ApplySavedPlan(const std::string& text) {
    const char* keys[] = {"MSC_PLAN", "MSC_FRAMESEND", "MSC_VGX_BACKEND_THREAD", "MSC_VITA_CORE_FRAMESEND", "MSC_FS_PIPE", "NP_VITA_CORE_THP"};
    for (const char* key : keys) unsetenv(key);
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(at, end - at);
        const size_t eq = line.find('=');
        if (eq != std::string::npos)
            for (const char* key : keys)
                if (line.substr(0, eq) == key) setenv(key, line.substr(eq + 1).c_str(), 1);
        at = end + 1;
    }
    const char* applied[8];
    return VitaPlan::Apply(applied, 8);
}
// The screen with the row of this title selected, on its page.
static void DrawAt(uint32_t* px, const State& s, const char* title, const char* message = "", bool running = false) {
    int page = 0, row = 0;
    if (!FindRow(Find(title), &page, &row)) { page = 0; row = 0; }   // 1.3: a hidden row is on no page; the first page is drawn
    Draw(px, s, page, row, message, false, running);
}
static bool Ppm(const char* path, const uint32_t* px) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%d %d\n255\n", kWidth, kHeight);
    for (int i = 0; i < kWidth * kHeight; ++i) {
        const unsigned char rgb[3] = {static_cast<unsigned char>(px[i]), static_cast<unsigned char>(px[i] >> 8), static_cast<unsigned char>(px[i] >> 16)};
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
    return true;
}

// 1.3 (SC06): the console's hooks of the language default, as strong definitions of the weak ones in vita_plan.cpp
static const char* g_systemLanguage = nullptr;
static std::string g_discRoot;
extern "C" const char* NpVitaSystemLanguageCode(void) { return g_systemLanguage; }
extern "C" const char* NpGameRootPath(void) { return g_discRoot.c_str(); }

int main(int argc, char** argv) {
    // The game's language: the Vita's own by default, the player's choice over it, and English where the disc lacks the tables
    {
        char dir[] = "/tmp/lang-disc-XXXXXX";
        CHECK(mkdtemp(dir) != nullptr);
        g_discRoot = dir;
        const std::string art = g_discRoot + "/files/Art/fe";
        CHECK(std::system(("mkdir -p " + art + " && touch " + art + "/naspanish.loc " + art + "/nafrench.loc").c_str()) == 0);
        auto start = [&](const char* sys, const char* msc, const char* np) {
            VitaPlan::ForgetDefaults();
            unsetenv("MSC_LANGUAGE");
            unsetenv("NP_LANG");
            if (msc) setenv("MSC_LANGUAGE", msc, 1);
            if (np) setenv("NP_LANG", np, 1);
            g_systemLanguage = sys;
            const char* names[16];
            VitaPlan::Apply(names, 16);
        };
        auto env = [](const char* k) { const char* v = std::getenv(k); return std::string(v ? v : "<unset>"); };
        start("es", nullptr, nullptr);
        CHECK(env("MSC_LANGUAGE") == "es" && NpText::LanguageFromEnv() == NpText::kSpanish && !VitaPlan::LanguageNotice());   // the game's and the port's text from one setting
        start("fr", nullptr, nullptr);
        CHECK(env("MSC_LANGUAGE") == "fr" && NpText::LanguageFromEnv() == NpText::kFrench);
        start("en", nullptr, nullptr);
        CHECK(env("MSC_LANGUAGE") == "<unset>" && NpText::LanguageFromEnv() == NpText::kEnglish);
        start(nullptr, nullptr, nullptr);   // no answer from the system
        CHECK(env("MSC_LANGUAGE") == "<unset>");
        start("es", "fr", nullptr);         // the player's row wins over the system
        CHECK(env("MSC_LANGUAGE") == "fr");
        start("es", "en", nullptr);         // ENGLISH chosen on a Spanish Vita stays English
        CHECK(env("MSC_LANGUAGE") == "en" && NpText::LanguageFromEnv() == NpText::kEnglish);
        start("fr", nullptr, "4");          // NP_LANG=4 by hand (SC06's workaround) wins over the system as well, for the game and for the port
        CHECK(env("MSC_LANGUAGE") == "<unset>" && env("NP_LANG") == "4" && NpText::LanguageFromEnv() == NpText::kSpanish);
        // a disc without the tables
        CHECK(std::system(("rm " + art + "/naspanish.loc").c_str()) == 0);
        start("es", nullptr, nullptr);
        CHECK(env("MSC_LANGUAGE") == "en" && env("NP_LANG") == "1" && VitaPlan::LanguageNotice() && NpText::LanguageFromEnv() == NpText::kEnglish);
        start("en", "es", nullptr);
        CHECK(env("MSC_LANGUAGE") == "en" && VitaPlan::LanguageNotice());
        start("en", "fr", nullptr);         // French is there
        CHECK(env("MSC_LANGUAGE") == "fr" && !VitaPlan::LanguageNotice());
        start("en", nullptr, "4");
        CHECK(env("MSC_LANGUAGE") == "en" && env("NP_LANG") == "1");   // NP_LANG=4 and no Spanish tables
        VitaPlan::ForgetDefaults();
        unsetenv("MSC_LANGUAGE");
        unsetenv("NP_LANG");
        g_systemLanguage = nullptr;
        g_discRoot.clear();
        std::system((std::string("rm -rf ") + dir).c_str());
    }

    {
        const int aim = Find("MEGA SAVE AIM"), speed = Find("MEGA SAVE SPEED");
        CHECK(aim >= 0 && speed >= 0);
        if (aim >= 0 && speed >= 0) {
            State s;
            Load(s, "# retained\nMSC_WATCH=1\nMSC_REAR_TOUCH=0\n");
            CHECK(s.choice[aim] == 0 && s.choice[speed] == 0);
            SetChoice(s, aim, 1); SetChoice(s, speed, 1);
            std::string out;
            CHECK(Serialize(s, out) && Has(out, "MSC_MEGA_AIM=rear") && Has(out, "MSC_MEGA_POINTER_SPEED=4"));
            CHECK(Has(out, "MSC_REAR_TOUCH=0") && Has(out, "# retained") && Has(out, "MSC_WATCH=1"));
            State t; Load(t, out.c_str());
            CHECK(t.choice[aim] == 1 && t.choice[speed] == 1);
            Load(t, "MSC_MEGA_AIM=custom\nMSC_MEGA_POINTER_SPEED=3junk\n");
            CHECK(t.choice[aim] == -1 && t.choice[speed] == -1);
            CHECK(Serialize(t, out) && Has(out, "MSC_MEGA_AIM=custom") && Has(out, "MSC_MEGA_POINTER_SPEED=3junk"));
            SetChoice(t, aim, 0); SetChoice(t, speed, 0);
            CHECK(Serialize(t, out) && CountLines(out, "MSC_MEGA_AIM=") == 0 && CountLines(out, "MSC_MEGA_POINTER_SPEED=") == 0);
        }
    }
    // no file: everything is the build's default; nothing is written
    {
        State s;
        Load(s, nullptr);
        for (int o = 0; o < OptionCount(); ++o) CHECK(s.choice[o] == GetOption(o).defaultChoice);
        std::string out;
        CHECK(Serialize(s, out) && out.empty());
    }
    // rows write their lines, read back as the same choice, saving again gives the same text, and the default removes the line again (1.3 prune: no picture modes: every row stands alone)
    {
        State s;
        Load(s, "# my file\nMSC_WATCH=1\n");
        SetChoice(s, Find("FRAME RATE"), kLocked);
        SetChoice(s, Find("STADIUM CROWD"), 1);
        std::string out;
        CHECK(Serialize(s, out));
        CHECK(Has(out, "# my file") && Has(out, "MSC_WATCH=1") && Has(out, "MSC_FPS=30") && Has(out, "MSC_PERFORMANCE_MODE=1") && CountLines(out, "MSC_LOCK60") == 0);
        CHECK(CountLines(out, "MSC_VITA_RENDER") == 0 && CountLines(out, "MSC_DYNRES") == 0 && CountLines(out, "MSC_PICTURE_HOLD") == 0 && CountLines(out, "MSC_VGX_MSAA") == 0);   // (nothing else is written)
        State t;
        Load(t, out.c_str());
        CHECK(t.choice[Find("FRAME RATE")] == kLocked && t.choice[Find("STADIUM CROWD")] == 1);
        std::string again;
        CHECK(Serialize(t, again) && again == out);   // idempotent
        SetChoice(t, Find("FRAME RATE"), 0);
        SetChoice(t, Find("STADIUM CROWD"), 0);
        CHECK(Serialize(t, out) && !Has(out, "MSC_FPS=30") && CountLines(out, "MSC_PERFORMANCE_MODE") == 0 && CountLines(out, "MSC_LOCK60") == 0);
        CHECK(Has(out, "MSC_WATCH=1") && CountLines(out, "# ---- ") == 0);
        // SMOOTH 60 is a choice of FRAME RATE (set18): it writes MSC_LOCK60=1 alone (or nothing, once it is the build's mode) and moves no other row
        State v;
        Load(v, "MSC_HDTEX=0\n");
        int before[kMaxOptions];
        for (int o = 0; o < OptionCount(); ++o) before[o] = v.choice[o];
        SetChoice(v, Find("FRAME RATE"), kSmooth);
        for (int o = 0; o < OptionCount(); ++o) CHECK(o == Find("FRAME RATE") ? v.choice[o] == kSmooth : v.choice[o] == before[o]);
        CHECK(Serialize(v, out) && CountLines(out, "MSC_LOCK60=1") == (VitaPlanDefaults::kLock60 ? 0 : 1) && CountLines(out, "MSC_FPS") == 0);
        SetChoice(v, Find("FRAME RATE"), 0);
        for (int o = 0; o < OptionCount(); ++o) CHECK(v.choice[o] == before[o]);
        CHECK(v.choice[Find("HD CHARACTERS")] == 1);
    }
    // Legacy picture selectors must describe the effective runtime values. 1.3 prune: a selector whose settings the rows can hold is written out as the rows' lines; one that has a setting no
    // row holds ("full": the keys of removed rows) stays as the selector line, which the game expands as ever, so nothing is lost. set18: the fixed sizes of "performance" and "quality"
    // are values no row offers because they were only worse (kObsolete): the rewrite leaves them out (the start-up expands a hand-written selector as ever until the screen saves).
    {
        const int perf = Find("STADIUM CROWD"), fr = Find("FRAME RATE");
        for (const char* alias : {"quality", "high"}) {
            State s;
            const std::string file = std::string("# retained\nMSC_PICTURE=") + alias + "\nMSC_WATCH=1\n";
            Load(s, file.c_str());
            for (int o = 0; o < OptionCount(); ++o) CHECK(s.choice[o] == GetOption(o).defaultChoice);   // (960x544 is no row's: every row reads its default)
            std::string out;
            CHECK(Serialize(s, out) && CountLines(out, "MSC_PICTURE=") == 0 && CountLines(out, "MSC_VITA_RENDER=") == 0);
            CHECK(CountLines(out, "MSC_FPS=") == 0 && Has(out, "# retained") && Has(out, "MSC_WATCH=1"));
            State again;
            Load(again, out.c_str());
            std::string stable;
            CHECK(Serialize(again, stable) && stable == out);
        }
        for (const char* alias : {"performance", "perf", "fast"}) {
            State s;
            const std::string file = std::string("MSC_PICTURE=") + alias + "\n";
            Load(s, file.c_str());
            CHECK(s.choice[perf] == 1 && s.choice[fr] == 0);
            std::string out;
            CHECK(Serialize(s, out) && CountLines(out, "MSC_VITA_RENDER=") == 0 && Has(out, "MSC_PERFORMANCE_MODE=1"));
            CHECK(CountLines(out, "MSC_PICTURE=") == 0);
            ResetAll(s);
            CHECK(Serialize(s, out) && CountLines(out, "MSC_PICTURE=") == 0 && CountLines(out, "MSC_VITA_RENDER=") == 0 && CountLines(out, "MSC_PERFORMANCE_MODE=") == 0);
        }
        for (const char* alias : {"lock60", "locked60", "60"}) {
            State s;
            const std::string file = std::string("MSC_PICTURE=") + alias + "\n";
            Load(s, file.c_str());
            std::string out;
            CHECK(s.choice[fr] == kSmooth && Serialize(s, out) && CountLines(out, "MSC_LOCK60=1") == (VitaPlanDefaults::kLock60 ? 0 : 1) && CountLines(out, "MSC_PICTURE=") == 0);
        }
        for (const char* alias : {"full", "hold"}) {   // MSC_PICTURE_HOLD and MSC_FX_GOV: keys of removed rows; the rows stay at their defaults and the selector is kept
            State s;
            const std::string file = std::string("MSC_PICTURE=") + alias + "\nMSC_WATCH=1\n";
            Load(s, file.c_str());
            for (int o = 0; o < OptionCount(); ++o) CHECK(s.choice[o] == GetOption(o).defaultChoice);
            std::string out;
            CHECK(Serialize(s, out) && Has(out, (std::string("MSC_PICTURE=") + alias).c_str()) && Has(out, "MSC_WATCH=1"));
            SetChoice(s, perf, 1);
            CHECK(Serialize(s, out) && Has(out, (std::string("MSC_PICTURE=") + alias).c_str()) && Has(out, "MSC_PERFORMANCE_MODE=1"));
        }
        State overridden;
        Load(overridden, "MSC_PICTURE=quality\nMSC_VITA_RENDER=480x272\nMSC_FPS=30\nMSC_WATCH=1\n");
        CHECK(overridden.choice[fr] == kLocked);   // (the hand-written size is no row's: it stays as written, the plan raises it to 864x488)
        std::string out;
        CHECK(Serialize(overridden, out) && Has(out, "MSC_VITA_RENDER=480x272") && Has(out, "MSC_FPS=30") && Has(out, "MSC_WATCH=1"));
        CHECK(CountLines(out, "MSC_PICTURE=") == 0);
        for (const char* alias : {"default", "normal"}) {
            State defaults;
            const std::string file = std::string("MSC_PICTURE=") + alias + "\n";
            Load(defaults, file.c_str());
            for (int o = 0; o < OptionCount(); ++o) CHECK(defaults.choice[o] == GetOption(o).defaultChoice);
            CHECK(Serialize(defaults, out) && out.empty());
        }
        State duplicate;
        Load(duplicate, "MSC_PICTURE=performance\nMSC_PICTURE=quality\n");   // (the last one decides)
        CHECK(duplicate.choice[perf] == 0);
        CHECK(Serialize(duplicate, out) && CountLines(out, "MSC_PICTURE=") == 0 && CountLines(out, "MSC_VITA_RENDER=") == 0);
        State custom;
        Load(custom, "MSC_PICTURE=high\nMSC_VITA_RENDER=320x224\n");
        CHECK(Serialize(custom, out) && Has(out, "MSC_VITA_RENDER=320x224") && CountLines(out, "MSC_PICTURE=") == 0);   // (a hand-written line outside the block stays)
        ResetAll(custom);
        CHECK(Serialize(custom, out) && CountLines(out, "MSC_VITA_RENDER=") == 0 && CountLines(out, "MSC_PICTURE=") == 0);   // (RESET drops a retired key wherever it stands)
        State unknown;
        Load(unknown, "MSC_PICTURE=experimental-owner-profile\nMSC_DYNRES_MIN=0.93\n");
        CHECK(Serialize(unknown, out) && Has(out, "MSC_PICTURE=experimental-owner-profile") && Has(out, "MSC_DYNRES_MIN=0.93"));
    }
    // LoadEnvFile accepts 96 assignments (duplicates and empty values consume slots too). Expanding a selector must not overflow it.
    {
        std::string file = "MSC_PICTURE=performance\n";
        for (int i = 0; i < 94; ++i) file += "OWNER_" + std::to_string(i) + "=kept\n";
        State fits;
        Load(fits, file.c_str());
        std::string out;
        CHECK(Serialize(fits, out) && Has(out, "MSC_PERFORMANCE_MODE=1"));
        CHECK(CountLines(out, "OWNER_") == 94 && CountLines(out, "MSC_PICTURE=") == 0);
        const std::string original = file + "OWNER_94=kept\nOWNER_95=kept\n";   // (set18: the selector's fixed size is no longer written out: one line fewer)
        State full;
        Load(full, original.c_str());
        CHECK(!Serialize(full, out));
        CHECK(full.text == original);
        for (const char* assignment : {"OWNER=kept\n", "OWNER=\n"}) {
            std::string repeated = "MSC_PICTURE=performance\n";
            for (int i = 0; i < 96; ++i) repeated += assignment;
            State duplicates;
            Load(duplicates, repeated.c_str());
            CHECK(!Serialize(duplicates, out));
        }
    }
    // W5 FPS: FRAME RATE is AUTO (UP TO 60: the build's default, writes nothing) or 30 FPS (MSC_FPS=30); the MSC_FPS=auto line of earlier builds reads AUTO and goes at the next
    // save; 60 / unlocked by hand read CUSTOM. 1.3 prune: the UP TO 60 choice is gone (MSC_FPS_BETWEEN=pace is what AUTO does)
    {
        const int fr = Find("FRAME RATE");
        // set18: one row for the frame rate (FRAME RATE and SMOOTH 60 MODE were two). SMOOTH 60 (DEFAULT) / 30 LOCKED since the 23 Nov console A/B (kLock60), AUTO / SMOOTH 60 / 30 LOCKED before; never a 45 mode
        CHECK(fr >= 0 && ChoiceCount(fr) == kLocked + 1 && GetOption(fr).defaultChoice == 0 && Find("SMOOTH 60 MODE") < 0);
        CHECK(!std::strncmp(GetOption(fr).choices[kSmooth].label, "SMOOTH 60", 9) && !std::strcmp(GetOption(fr).choices[kLocked].label, "30 LOCKED"));
        if (!VitaPlanDefaults::kLock60) CHECK(!std::strcmp(GetOption(fr).choices[0].label, "AUTO") && !std::strcmp(GetOption(fr).choices[kSmooth].set[0].key, "MSC_LOCK60"));
        for (int c = 0; c < ChoiceCount(fr); ++c) CHECK(!std::strstr(GetOption(fr).choices[c].label, "45"));
        State s;
        Load(s, nullptr);
        CHECK(s.choice[fr] == 0);
        std::string out;
        CHECK(Serialize(s, out) && CountLines(out, "MSC_FPS") == 0);
        Step(s, fr, -1);
        CHECK(s.choice[fr] == kLocked);
        CHECK(Serialize(s, out) && Has(out, "MSC_FPS=30") && CountLines(out, "MSC_FPS=") == 1 && CountLines(out, "MSC_FPS_BETWEEN") == 0 && CountLines(out, "MSC_LOCK60") == 0);
        State t;
        Load(t, out.c_str());
        CHECK(t.choice[fr] == kLocked);
        SetChoice(t, fr, 0);                   // back to the default: the line goes
        CHECK(t.choice[fr] == 0 && Serialize(t, out) && CountLines(out, "MSC_FPS") == 0);
        State old;
        Load(old, "MSC_FPS=auto\nMSC_WATCH=1\n");   // an earlier build's AUTO
        CHECK(old.choice[fr] == 0);
        SetChoice(old, fr, 0);
        CHECK(Serialize(old, out) && CountLines(out, "MSC_FPS=") == 0 && Has(out, "MSC_WATCH=1"));
        State c;
        Load(c, "MSC_FPS=60\n");              // a tester's own value: CUSTOM, kept until the row is changed
        CHECK(c.choice[fr] == -1 && std::strstr(c.custom[fr], "MSC_FPS=60") != nullptr);
        CHECK(Serialize(c, out) && Has(out, "MSC_FPS=60"));   // saving without touching the row keeps the line
        State d;
        Load(d, "MSC_FPS=unlocked\nMSC_WATCH=1\n");
        SetChoice(d, fr, 0);
        CHECK(Serialize(d, out) && CountLines(out, "MSC_FPS") == 0 && Has(out, "MSC_WATCH=1"));
        // the retired UP TO 60 line (1.3 development files) reads as AUTO and is no CUSTOM
        State up;
        Load(up, "MSC_FPS_BETWEEN=pace\nMSC_WATCH=1\n");
        CHECK(up.choice[fr] == 0 && !Changed(up));
        // the 1.3 SMOOTH 60 MODE row's line reads as SMOOTH 60; with 30 FPS beside it (the old pair that did nothing) it is CUSTOM and kept until the row is stepped
        State l;
        Load(l, "MSC_LOCK60=1\n");
        CHECK(l.choice[fr] == kSmooth);
        State both;
        Load(both, "MSC_LOCK60=1\nMSC_FPS=30\n");
        if (VitaPlanDefaults::kLock60) {   // MSC_LOCK60=1 is the build's own value: the pair reads 30 LOCKED (MSC_FPS=30 wins in np_fps.cpp) and the save drops the LOCK60 line
            CHECK(both.choice[fr] == kLocked && Serialize(both, out) && !Has(out, "MSC_LOCK60=1") && Has(out, "MSC_FPS=30"));
            State off;
            Load(off, "MSC_LOCK60=0\n");   // a tester's SMOOTH 60 off: CUSTOM, kept until the row is stepped
            CHECK(off.choice[fr] == -1 && Serialize(off, out) && Has(out, "MSC_LOCK60=0"));
            SetChoice(off, fr, kSmooth);
            CHECK(Serialize(off, out) && CountLines(out, "MSC_LOCK60") == 0);
        } else {
            CHECK(both.choice[fr] == -1 && Serialize(both, out) && Has(out, "MSC_LOCK60=1") && Has(out, "MSC_FPS=30"));
            SetChoice(both, fr, kLocked);
            CHECK(Serialize(both, out) && !Has(out, "MSC_LOCK60=1") && Has(out, "MSC_FPS=30"));
        }
        // set18: an earlier screen's AUTO / UP TO 60 lines inside its own block are skipped at start-up and read as the default (SMOOTH 60 is the mode)
        const std::string blk = "# ---- Settings screen (LiveArea > Settings): the lines below are written by it; the rest of this file is kept ----\nMSC_FPS=auto\nMSC_FPS_BETWEEN=pace\n# ---- end of the Settings screen's lines ----\n";
        std::vector<std::string> notes;
        CHECK(WithoutObsolete(blk, &notes).find("MSC_FPS") == std::string::npos && notes.size() == 2);
    }
    // Explicit old thread settings must not hide behind AUTO or defeat a newly selected core plan.
    {
        const int core = Find("CORE PLAN"), pointer = Find("POINTER SPEED");
        const char* file = "MSC_VGX_BACKEND_THREAD=1\nMSC_VITA_CORE_FRAMESEND=1\nMSC_WATCH=1\n";
        State s;
        Load(s, file);
        CHECK(s.choice[core] == -1 && std::strstr(s.custom[core], "MSC_VGX_BACKEND_THREAD=1"));
        SetChoice(s, pointer, 3);
        std::string out;
        CHECK(Serialize(s, out) && Has(out, "MSC_VGX_BACKEND_THREAD=1") && Has(out, "MSC_VITA_CORE_FRAMESEND=1"));
        CHECK(ApplySavedPlan(out) == 0);   // an unrelated save preserves the owner's explicit threading
        SetChoice(s, core, 3);
        CHECK(Serialize(s, out) && Has(out, "MSC_PLAN=S") && Has(out, "MSC_WATCH=1"));
        CHECK(CountLines(out, "MSC_VGX_BACKEND_THREAD=") == 0 && CountLines(out, "MSC_VITA_CORE_FRAMESEND=") == 0);
        CHECK(ApplySavedPlan(out) == 1 && std::getenv("MSC_FRAMESEND") && !std::strcmp(std::getenv("MSC_FRAMESEND"), "off"));
        for (const char* key : {"MSC_FRAMESEND", "MSC_VGX_BACKEND_THREAD", "MSC_VITA_CORE_FRAMESEND"}) {
            State manual;
            const std::string lines = std::string("MSC_PLAN=S\n") + key + "=off\n";
            Load(manual, lines.c_str());
            CHECK(manual.choice[core] == -1);
            CHECK(Serialize(manual, out) && Has(out, (std::string(key) + "=off").c_str()) && Has(out, "MSC_PLAN=S"));
            CHECK(Serialize(manual, out) && Has(out, (std::string(key) + "=off").c_str()) && Has(out, "MSC_PLAN=S"));
            ResetAll(manual);                                // RESET and the row itself are the ways back
            CHECK(Serialize(manual, out) && CountLines(out, key) == 0 && CountLines(out, "MSC_PLAN=") == 0);
            CHECK(ApplySavedPlan(out) == 5 && std::getenv("MSC_FS_PIPE") && !std::strcmp(std::getenv("MSC_FS_PIPE"), "1"));
        }
        State piped;
        Load(piped, "MSC_FS_PIPE=1\n");
        CHECK(piped.choice[core] == -1 && Serialize(piped, out) && Has(out, "MSC_FS_PIPE=1"));
        SetChoice(piped, core, 1);   // C NO OVERLAP must remove a manual pipeline override too
        CHECK(Serialize(piped, out) && Has(out, "MSC_PLAN=Cn") && CountLines(out, "MSC_FS_PIPE=") == 0);
        CHECK(ApplySavedPlan(out) == 4 && !std::getenv("MSC_FS_PIPE"));
        State reset;
        Load(reset, file);
        ResetAll(reset);
        CHECK(Serialize(reset, out) && CountLines(out, "MSC_VGX_BACKEND_THREAD=") == 0 && Has(out, "MSC_WATCH=1"));
        CHECK(ApplySavedPlan(out) == 5);
    }
    // set18: kObsolete. Values an earlier 1.3 screen wrote into its own block that were only worse are skipped (start-up and screen) and dropped by the next save; the same lines outside
    // the block (the kit's, a player's own) are applied and shown as they are
    {
        const std::string kS = "# ---- Settings screen (LiveArea > Settings): the lines below are written by it; the rest of this file is kept ----";
        const std::string kE = "# ---- end of the Settings screen's lines ----";
        const std::string inside = "# mine\nMSC_WATCH=1\n" + kS + "\nMSC_VITA_RENDER=864x488\nMSC_VGX_MSAA=0\nMSC_GAME_FIXES=none\nMSC_MEGA_POINTER_SPEED=1\nMSC_LANGUAGE=es\n" + kE + "\n";
        std::vector<std::string> notes;
        const std::string kept = WithoutObsolete(inside, &notes);
        CHECK(notes.size() == 4);
        for (const char* gone : {"MSC_VITA_RENDER", "MSC_VGX_MSAA", "MSC_GAME_FIXES", "MSC_MEGA_POINTER_SPEED"}) CHECK(CountLines(kept, gone) == 0);
        for (const char* stays : {"# mine", "MSC_WATCH=1", "MSC_LANGUAGE=es"}) CHECK(Has(kept, stays));
        for (const std::string& n : notes) CHECK(n.find("ignored") != std::string::npos && n.size() < 200);
        State s;
        Load(s, inside.c_str());
        CHECK(s.choice[Find("ORIGINAL GAME FIXES")] == 0 && s.choice[Find("MEGA SAVE SPEED")] == 0 && s.choice[Find("LANGUAGE")] == 2 && !Changed(s));
        std::string out;
        CHECK(Serialize(s, out));
        for (const char* gone : {"MSC_VITA_RENDER", "MSC_VGX_MSAA", "MSC_GAME_FIXES", "MSC_MEGA_POINTER_SPEED"}) CHECK(CountLines(out, gone) == 0);
        CHECK(Has(out, "MSC_LANGUAGE=es") && Has(out, "MSC_WATCH=1"));
        // outside the block: applied as written, and a row that owns the key reads CUSTOM
        const std::string outside = "MSC_VITA_RENDER=864x488\nMSC_VGX_MSAA=0\nMSC_GAME_FIXES=none\nMSC_MEGA_POINTER_SPEED=1\n";
        notes.clear();
        CHECK(WithoutObsolete(outside, &notes) == outside && notes.empty());
        Load(s, outside.c_str());
        CHECK(s.choice[Find("ORIGINAL GAME FIXES")] == -1 && s.choice[Find("MEGA SAVE SPEED")] == -1);
        CHECK(Serialize(s, out) && out == outside);
        // a value that is not obsolete stays inside the block too (a choice of today's rows, a kept key)
        const std::string fine = kS + "\nMSC_GAME_FIXES=all\nMSC_VGX_MSAA=2\n" + kE + "\n";
        CHECK(WithoutObsolete(fine) == fine);
    }
    // stepping wraps in both directions
    {
        State s;
        Load(s, nullptr);
        const int perf = Find("STADIUM CROWD"), shadows = Find("REAR TOUCH");   // (set18: LIVE SHADOWS went; REAR TOUCH has three choices)
        Step(s, perf, +1);
        CHECK(s.choice[perf] == 1);
        Step(s, perf, +1);
        CHECK(s.choice[perf] == 0);
        Step(s, shadows, -1);
        CHECK(s.choice[shadows] == 2);
        std::string out;
        CHECK(Serialize(s, out) && Has(out, "MSC_REAR_TOUCH=0"));
        const int speed = Find("POINTER SPEED");
        CHECK(s.choice[speed] == 4);   // 5 (NORMAL) of the 1-10 scale
        for (int i = 0; i < 5; ++i) Step(s, speed, -1);
        CHECK(s.choice[speed] == 9);   // wrapped to the last: 10 (FASTEST)
    }
    // values that are none of the choices: shown as CUSTOM, left alone until the option is changed; lines of removed rows (set18: MSC_VITA_RENDER, MSC_LAN_MODE) are kept as written
    // outside the screen's block until RESET TO DEFAULTS
    {
        const char* file = "MSC_VITA_RENDER=800x450\nNP_POINTER_SPEED=3\nMSC_LAN_MODE=wifi\nMSC_GAME_FIXES=1\nNP_DATA_DIR=ux0:data/mscharged\n";
        State s;
        Load(s, file);
        CHECK(s.choice[Find("POINTER SPEED")] == -1);
        CHECK(s.choice[Find("ORIGINAL GAME FIXES")] == -1);   // "1" is a bit mask: not one of the named choices
        CHECK(!Changed(s));
        std::string out;
        CHECK(Serialize(s, out));
        CHECK(Has(out, "MSC_VITA_RENDER=800x450") && Has(out, "NP_POINTER_SPEED=3") && Has(out, "MSC_GAME_FIXES=1") && Has(out, "NP_DATA_DIR=ux0:data/mscharged"));
        CHECK(Has(out, "MSC_LAN_MODE=wifi"));
        Step(s, Find("POINTER SPEED"), +1);        // from CUSTOM: the first choice
        CHECK(s.choice[Find("POINTER SPEED")] == 0);
        CHECK(Serialize(s, out) && !Has(out, "NP_POINTER_SPEED=3") && Has(out, "NP_POINTER_SPEED=0.5") && Has(out, "MSC_VITA_RENDER=800x450"));
        ResetAll(s);
        CHECK(Serialize(s, out));
        CHECK(!Has(out, "NP_POINTER_SPEED=0.5") && !Has(out, "MSC_LAN_MODE=wifi") && !Has(out, "MSC_VITA_RENDER=800x450") && Has(out, "NP_DATA_DIR=ux0:data/mscharged"));
    }
    // NEARBY PAIRING (mp24): no row; a role line is retired (read by the game, ignored by the screen, dropped from its block at the next save, kept outside it)
    {
        CHECK(Find("NEARBY PAIRING") < 0 && Find("MULTIPLAYER LINK") < 0);
        State old;
        Load(old, "MSC_LAN_MODE=online\nMSC_LAN_DIALOG=join\n");
        std::string out;
        CHECK(Serialize(old, out) && Has(out, "MSC_LAN_DIALOG=join"));   // outside the screen's block: byte for byte
    }
    // CONNECTION writes its key and reads back; the default writes nothing (INPUT DELAY went in set18: the game's own delay adapts)
    {
        const int conn = Find("CONNECTION");
        CHECK(conn >= 0 && Find("INPUT DELAY") < 0);
        State s;
        Load(s, nullptr);
        std::string out;
        CHECK(Serialize(s, out) && !Has(out, "MSC_NET_RELAY_ONLY"));
        SetChoice(s, conn, 1);
        CHECK(Serialize(s, out) && Has(out, "MSC_NET_RELAY_ONLY=1"));
        State r;
        Load(r, out.c_str());
        CHECK(r.choice[conn] == 1);
        SetChoice(r, conn, 0);
        CHECK(Serialize(r, out) && CountLines(out, "MSC_NET_RELAY_ONLY") == 0);
    }
    // two lines for one key: the last one decides what the screen shows; saving removes both when the option is set
    {
        State s;
        Load(s, "MSC_PERFORMANCE_MODE=0\nMSC_PERFORMANCE_MODE=1\n");
        CHECK(s.choice[Find("STADIUM CROWD")] == 1);
        SetChoice(s, Find("STADIUM CROWD"), 0);
        std::string out;
        CHECK(Serialize(s, out) && CountLines(out, "MSC_PERFORMANCE_MODE") == 0);
    }
    // SHARPNESS (three choices on one key), the removed variants (CRISP+ and its second key), CRLF files, and the size limit
    {
        const int sharp = Find("SHARPNESS");
        CHECK(sharp >= 0 && ChoiceCount(sharp) == 3 && !std::strcmp(GetOption(sharp).choices[0].label, "SMOOTH") && !std::strcmp(GetOption(sharp).choices[2].label, "CRISP"));
        State s;
        Load(s, "MSC_VITA_UPSCALE=sharp+\r\nMSC_VITA_SHARPEN=0.35\r\n# end\r\n");   // the removed CRISP+: CUSTOM, the pair stays while the row is untouched
        CHECK(s.choice[sharp] == -1 && std::strstr(s.custom[sharp], "MSC_VITA_UPSCALE=sharp+"));
        std::string out;
        CHECK(Serialize(s, out) && Has(out, "# end") && Has(out, "MSC_VITA_UPSCALE=sharp+") && Has(out, "MSC_VITA_SHARPEN=0.35"));
        Step(s, sharp, +1);   // from CUSTOM: the first choice, and the whole variant goes
        CHECK(s.choice[sharp] == 0 && Serialize(s, out) && CountLines(out, "MSC_VITA_UPSCALE") == 0);
        State t;
        Load(t, "MSC_VITA_UPSCALE=sharp\n");
        CHECK(t.choice[sharp] == 2);
        for (const char* gone : {"smooth", "fast", "sharp+"}) {   // the removed SMOOTH FULL, FAST and CRISP+ values
            State g;
            const std::string file = std::string("MSC_VITA_UPSCALE=") + gone + "\n";
            Load(g, file.c_str());
            CHECK(g.choice[sharp] == -1);
        }
        State big;
        Load(big, std::string(20000, '#').c_str());
        CHECK(!Serialize(big, out, 15000));
    }
    // W5 CTL: the controls rows: the defaults write nothing, CLASSIC / the rear touch / each touch feature write their key and read back
    {
        State s;
        Load(s, nullptr);
        const int controls = Find("CONTROLS"), rear = Find("REAR TOUCH");
        CHECK(controls >= 0 && rear >= 0 && OptionIndex("CONTROLS") == controls && OptionIndex("NO SUCH ROW") == -1);
        CHECK(s.choice[controls] == 0 && s.choice[rear] == 0);
        CHECK(!std::strcmp(GetOption(controls).choices[0].label, "MODERN") && !std::strcmp(GetOption(controls).choices[1].label, "CLASSIC"));
        SetChoice(s, controls, 1);
        SetChoice(s, rear, 2);
        const char* touch[3] = {"MEGA SAVE AIM", "TOUCH MEGA STRIKE", "TOUCH SHAKEDOWN"};
        const int touchChoice[3] = {2, 1, 1};   // (set18: STICK ONLY is the old TOUCH MEGA SAVE OFF)
        const char* keys[3] = {"MSC_TOUCH_SAVE=0", "MSC_TOUCH_METER=0", "MSC_TOUCH_SHAKE=0"};
        CHECK(Find("TOUCH MEGA SAVE") < 0 && Find("TOUCH POINTER") < 0);
        for (int i = 0; i < 3; ++i) {
            CHECK(Find(touch[i]) >= 0);
            SetChoice(s, Find(touch[i]), touchChoice[i]);
        }
        std::string out;
        CHECK(Serialize(s, out));
        CHECK(Has(out, "MSC_CONTROLS=classic") && Has(out, "MSC_REAR_TOUCH=0"));
        for (int i = 0; i < 3; ++i) CHECK(Has(out, keys[i]));
        State t;
        Load(t, out.c_str());
        CHECK(t.choice[controls] == 1 && t.choice[rear] == 2);
        for (int i = 0; i < 3; ++i) CHECK(t.choice[Find(touch[i])] == touchChoice[i]);
        SetChoice(t, rear, 1);
        CHECK(Serialize(t, out) && Has(out, "MSC_REAR_TOUCH=1") && CountLines(out, "MSC_REAR_TOUCH=") == 1);
        ResetAll(t);
        CHECK(Serialize(t, out) && out.empty());
        // the list scrolls: the selected row is always drawn
        for (int p = 0; p < PageCount(); ++p) {
            for (int r = 0; r < RowCount(p); ++r) {
                const int first = FirstVisibleRow(p, r);
                CHECK(first >= 0 && r >= first && r < first + 14 && first + 14 <= (RowCount(p) > 14 ? RowCount(p) : 14));
            }
            CHECK(FirstVisibleRow(p, 0) == 0);
        }
    }
    // W5 CAM / int34: UNLOCK ALL CHARACTERS and UNLOCK ALL STADIUMS both default OFF (write nothing; ON writes MSC_UNLOCK_CAPTAINS=1 /
    // MSC_UNLOCK_STADIUMS=1); older files naming MSC_UNLOCK_CAPTAINS=0 read cleanly as OFF (IsBuildDefault) and drop the redundant line on save.
    {
        State s;
        Load(s, "# mine\nMSC_WATCH=1\n");
        const int cap = Find("UNLOCK ALL CHARACTERS"), stad = Find("UNLOCK ALL STADIUMS");
        CHECK(cap >= 0 && stad >= 0 && OptionIndex("UNLOCK ALL CHARACTERS") == cap);
        CHECK(s.choice[cap] == 0 && s.choice[stad] == 0);
        CHECK(!std::strcmp(GetOption(cap).choices[0].label, "OFF") && !std::strcmp(GetOption(stad).choices[0].label, "OFF"));
        std::string out;
        CHECK(Serialize(s, out) && !Has(out, "MSC_UNLOCK_CAPTAINS=") && CountLines(out, "MSC_UNLOCK_") == 0);   // defaults write nothing
        SetChoice(s, cap, 1);
        SetChoice(s, stad, 1);
        CHECK(Serialize(s, out) && Has(out, "MSC_UNLOCK_CAPTAINS=1") && Has(out, "MSC_UNLOCK_STADIUMS=1") && Has(out, "MSC_WATCH=1") && Has(out, "# mine"));
        CHECK(CountLines(out, "MSC_UNLOCK_CAPTAINS=") == 1 && CountLines(out, "MSC_UNLOCK_STADIUMS=") == 1);
        State t;
        Load(t, out.c_str());
        CHECK(t.choice[cap] == 1 && t.choice[stad] == 1);
        Step(t, cap, +1);                                  // ON -> OFF (wraps): the line goes away
        CHECK(t.choice[cap] == 0 && Serialize(t, out) && CountLines(out, "MSC_UNLOCK_CAPTAINS=") == 0 && Has(out, "MSC_UNLOCK_STADIUMS=1"));
        State leg;
        Load(leg, "MSC_UNLOCK_CAPTAINS=0\n");              // older build's OFF line reads as default OFF and drops on save
        CHECK(leg.choice[cap] == 0 && Serialize(leg, out) && CountLines(out, "MSC_UNLOCK_CAPTAINS=") == 0);
        State u;
        Load(u, "MSC_UNLOCK_CAPTAINS=2\nMSC_UNLOCK_STADIUMS=0\n");   // hand-written values outside choices: shown as CUSTOM, kept
        CHECK(u.choice[cap] == -1 && u.choice[stad] == -1);
        CHECK(Serialize(u, out) && Has(out, "MSC_UNLOCK_CAPTAINS=2") && Has(out, "MSC_UNLOCK_STADIUMS=0"));
        ResetAll(t);
        CHECK(Serialize(t, out) && CountLines(out, "MSC_UNLOCK_") == 0);
    }
    // each kept row maps to one switch (or two): the first choice is the default and writes nothing, every other choice writes its line, reads back as the same choice, and the
    // default removes it again
    {
        struct Case { const char* row; int choices; const char* line[4]; };
        const Case cases[] = {
            VitaPlanDefaults::kLock60 ? Case{"FRAME RATE", 2, {nullptr, "MSC_FPS=30", nullptr, nullptr}} : Case{"FRAME RATE", 3, {nullptr, "MSC_LOCK60=1", "MSC_FPS=30", nullptr}},
            {"HD CHARACTERS", 2, {nullptr, "MSC_HDTEX=0", nullptr, nullptr}},
            {"DEPTH OF FIELD", 2, {nullptr, "MSC_DOF=lazy", nullptr, nullptr}},
            {"SHARPNESS", 3, {nullptr, "MSC_VITA_UPSCALE=off", "MSC_VITA_UPSCALE=sharp", nullptr}},
            {"MEGA STRIKES", 2, {nullptr, "MSC_RULE_NOMEGA=1", nullptr, nullptr}},
            {"LANGUAGE", 4, {nullptr, "MSC_LANGUAGE=en", "MSC_LANGUAGE=es", "MSC_LANGUAGE=fr"}},   // (1.3: AUTOMATIC first, then ENGLISH, SPANISH, FRENCH)
            {"REDUCED CAMERA SHAKE", 2, {nullptr, "MSC_REDUCED_MOTION=1", nullptr, nullptr}},
            {"REAR TOUCH", 3, {nullptr, "MSC_REAR_TOUCH=1", "MSC_REAR_TOUCH=0", nullptr}},
            {"MEGA SAVE AIM", 3, {nullptr, "MSC_MEGA_AIM=rear", "MSC_TOUCH_SAVE=0", nullptr}},   // (set18: STICK ONLY is the old TOUCH MEGA SAVE OFF)
            {"MEGA SAVE SPEED", 2, {nullptr, "MSC_MEGA_POINTER_SPEED=4", nullptr, nullptr}},   // (set18: ORIGINAL and 2x went)
            {"ORIGINAL GAME FIXES", 2, {nullptr, "MSC_GAME_FIXES=all", nullptr, nullptr}},   // (set18: OFF (AS RETAIL) went)
            {"CONNECTION", 2, {nullptr, "MSC_NET_RELAY_ONLY=1", nullptr, nullptr}},
        };
        for (const Case& c : cases) {
            const int o = Find(c.row);
            CHECK(o >= 0 && ChoiceCount(o) == c.choices && GetOption(o).defaultChoice == 0);
            if (o < 0) continue;
            State none;
            Load(none, "MSC_WATCH=1\n");
            CHECK(none.choice[o] == 0);   // a file without the row's keys: the default
            std::string out;
            CHECK(Serialize(none, out) && CountLines(out, "MSC_FX_") == 0 && CountLines(out, "MSC_DYNRES") == 0 && CountLines(out, "MSC_VGX_MSAA") == 0);
            for (int k = 1; k < c.choices; ++k) {
                State s;
                Load(s, "# mine\nMSC_WATCH=1\n");
                SetChoice(s, o, k);
                CHECK(Serialize(s, out) && Has(out, c.line[k]) && Has(out, "# mine") && Has(out, "MSC_WATCH=1"));
                State t;
                Load(t, out.c_str());
                CHECK(t.choice[o] == k);   // reads back as the same choice
                std::string again;
                CHECK(Serialize(t, again) && again == out);
                SetChoice(t, o, 0);
                CHECK(Serialize(t, out) && !Has(out, c.line[k]) && Has(out, "MSC_WATCH=1"));
            }
        }
        std::string out;
        // hand-written values that are none of the choices read CUSTOM (exactly one row) and are kept: an HD dry run, a multisample count, the removed "AS GAME" blur, the removed fixed
        // 960x544 picture, a tester's frame rate
        for (const char* file : {"MSC_HDTEX=2\n", "MSC_DOF=1\n", "MSC_FPS=60\n", "MSC_MEGA_AIM=both\n", "MSC_GAME_FIXES=none\n"}) {
            State s;
            Load(s, file);
            int custom = 0;
            for (int o = 0; o < OptionCount(); ++o)
                if (s.choice[o] == -1) ++custom;
            CHECK(custom == 1);
            CHECK(Serialize(s, out) && Has(out, std::string(file).substr(0, std::strcspn(file, "\n")).c_str()));
        }
        // set18: the keys of RESOLUTION and ANTI-ALIASING (and 1.0 / 1.1's MSC_VITA_MSAA) belong to no row: every row reads its default, and a hand-written line stays as written
        for (const char* file : {"MSC_VITA_MSAA=4\nMSC_WATCH=1\n", "MSC_VGX_MSAA=8\nMSC_WATCH=1\n", "MSC_VITA_RENDER=960x544\nMSC_WATCH=1\n"}) {
            State s;
            Load(s, file);
            for (int o = 0; o < OptionCount(); ++o) CHECK(s.choice[o] == GetOption(o).defaultChoice);
            CHECK(Serialize(s, out) && Has(out, std::string(file).substr(0, std::strcspn(file, "\n")).c_str()) && Has(out, "MSC_WATCH=1"));
        }
        // the rows keep their indices for 1.0 / 1.1 files: each of these reads as before
        {
            State s;
            Load(s, "MSC_VITA_RENDER=640x362\nMSC_PERFORMANCE_MODE=1\nMSC_FPS=30\nMSC_VITA_UPSCALE=sharp\nNP_WIDESCREEN=0\nMSC_GAME_FIXES=all\n");
            CHECK(s.choice[Find("STADIUM CROWD")] == 1 && s.choice[Find("FRAME RATE")] == kLocked);   // (the 1.1 file's 640x362 has no row; the guard turns it off at start-up)
            CHECK(s.choice[Find("SHARPNESS")] == 2 && s.choice[Find("ORIGINAL GAME FIXES")] == 1);   // (NP_WIDESCREEN=0: no row since set18, kept as written)
            for (const char* row : {"HD CHARACTERS", "DEPTH OF FIELD"}) CHECK(s.choice[Find(row)] == 0);
        }
        // the worst case still fits the loader's 96 assignments: every option at its last choice
        State all;
        Load(all, nullptr);
        for (int o = 0; o < OptionCount(); ++o) SetChoice(all, o, ChoiceCount(o) - 1);
        CHECK(Serialize(all, out));
        size_t lines = 0;
        for (size_t at = 0; at < out.size(); ++at) lines += out[at] == '\n';
        CHECK(lines < 96 && out.size() < 15000);
    }
    // the crowd row (called PERFORMANCE MODE before 1.3): SHOW is the default, HIDE writes the same key as ever, old files read as before
    {
        const int crowd = Find("STADIUM CROWD");
        CHECK(crowd >= 0 && Find("PERFORMANCE MODE") == -1 && ChoiceCount(crowd) == 2 && GetOption(crowd).defaultChoice == 0);
        CHECK(!std::strcmp(GetOption(crowd).choices[0].label, "SHOW") && !std::strcmp(GetOption(crowd).choices[1].label, "HIDE"));
        State d;
        Load(d, "MSC_WATCH=1\nMSC_PERFORMANCE_MODE=1\n");   // a file of 1.1 or an earlier 1.2 build
        CHECK(d.choice[crowd] == 1);
        std::string out;
        CHECK(Serialize(d, out) && Has(out, "MSC_PERFORMANCE_MODE=1") && Has(out, "MSC_WATCH=1"));
        Step(d, crowd, +1);
        CHECK(d.choice[crowd] == 0 && Serialize(d, out) && CountLines(out, "MSC_PERFORMANCE_MODE") == 0);
        Load(d, "MSC_PERFORMANCE_MODE=0\n");   // (written by hand: the key's own off value, no row choice: CUSTOM as before)
        CHECK(d.choice[crowd] == -1);
    }
    // files written by 1.0 / 1.1 read as before: each row shows its old choice, nothing is lost on a save without a change, and a row changed on this
    // screen replaces only its own lines
    {
        const char* files[] = {
            "# written by 1.1\nMSC_VITA_RENDER=960x544\nMSC_FPS=30\nMSC_WATCH=1\n",                                   // the 1.1 QUALITY preset
            "MSC_VITA_RENDER=640x362\nMSC_PERFORMANCE_MODE=1\n",                                                        // the 1.1 PERFORMANCE preset
            "MSC_FRAME_PACING=0\nMSC_PLAN=A\nMSC_VITA_UPSCALE=sharp+\nMSC_VITA_SHARPEN=0.35\nMSC_VITA_ANISO=3\n",
            "NP_WIDESCREEN=0\nMSC_GAME_FIXES=none\nMSC_UNLOCK_CAPTAINS=1\nMSC_UNLOCK_STADIUMS=1\nMSC_LAN_MODE=wifi\n",
            "MSC_CONTROLS=classic\nMSC_REAR_TOUCH=1\nNP_MENU_NAV=0\nNP_TOUCH_POINTER=0\nMSC_TOUCH_SAVE=0\nMSC_MEGA_AIM=rear\nMSC_MEGA_POINTER_SPEED=4\nNP_POINTER_SPEED=1.5\n",
            "MSC_VITA_MSAA=4\nMSC_DYNRES_MIN=0.93\nMSC_MEGA_RENDER=off\n",                                               // a 1.1 anti-aliasing line and owner lines
        };
        for (const char* file : files) {
            State s;
            Load(s, file);
            std::string out;
            CHECK(Serialize(s, out));
            // the same lines (the screen's choices are written again inside its block; order is not part of the contract)
            auto lines = [](const std::string& t) {
                std::vector<std::string> v;
                size_t at = 0;
                while (at < t.size()) {
                    size_t end = t.find('\n', at);
                    if (end == std::string::npos) end = t.size();
                    const std::string l = t.substr(at, end - at);
                    if (!l.empty() && l[0] != '#') v.push_back(l);
                    at = end + 1;
                }
                std::sort(v.begin(), v.end());
                return v;
            };
            // (1.3 prune: the keys of the removed rows, MSC_FRAME_PACING and MSC_VITA_ANISO among them, are lines no row owns: they pass through unchanged)
            CHECK(lines(out) == lines(file));
            // the 1.2 rows read their defaults from a file that does not mention them
            for (const char* row : {"HD CHARACTERS", "DEPTH OF FIELD"}) CHECK(s.choice[Find(row)] == 0);
        }
        State a;
        Load(a, files[0]);
        CHECK(a.choice[Find("FRAME RATE")] == kLocked);
        State b;
        Load(b, files[5]);
        CHECK(b.choice[Find("STADIUM CROWD")] == 0 && !Changed(b));
    }
    // 1.2 defaults: the build applies the anti-aliasing rungs and the lazy depth of field itself (vita_plan.cpp). A file that names the build's own value reads as the row's first
    // choice and the save drops the line; the OFF choices write the value that turns the item off; other values are CUSTOM.
    {
        const int dof = Find("DEPTH OF FIELD");
        State s;
        Load(s, "MSC_DOF=0\nMSC_WATCH=1\n");   // (ship26: the build's DOF is 0, the first choice; ON writes lazy)
        CHECK(s.choice[dof] == 0);
        std::string out;
        CHECK(Serialize(s, out) && CountLines(out, "MSC_DOF") == 0 && Has(out, "MSC_WATCH=1"));
        Load(s, "MSC_DOF=lazy\n");
        CHECK(s.choice[dof] == 1);
        CHECK(Serialize(s, out) && Has(out, "MSC_DOF=lazy"));
        Load(s, "MSC_DOF=1\n");   // the removed "ON (AS GAME)": CUSTOM, kept until the row is stepped
        CHECK(s.choice[dof] == -1 && Serialize(s, out) && Has(out, "MSC_DOF=1"));
        for (const char* odd : {"MSC_DOF=lazyish\n"}) {
            Load(s, odd);
            int custom = 0;
            for (int o = 0; o < OptionCount(); ++o) custom += s.choice[o] == -1;
            CHECK(custom == 1);
        }
        // all first choices write nothing
        State d;
        Load(d, nullptr);
        for (int o = 0; o < OptionCount(); ++o) SetChoice(d, o, ChoiceCount(o) - 1);
        ResetAll(d);
        CHECK(Serialize(d, out) && out.empty());
    }
    // 1.3: leaving with changes: Changed sees a real change, not a save-time clean-up, and a change undone by hand is no change
    {
        const int crowd = Find("STADIUM CROWD");
        State s;
        Load(s, "MSC_VGX_MSAA=4\nMSC_FPS=auto\nMSC_WATCH=1\n");   // (lines the next save tidies are not a change of the player's)
        CHECK(!Changed(s));
        SetChoice(s, crowd, 1);
        CHECK(Changed(s));
        SetChoice(s, crowd, 0);
        CHECK(!Changed(s));
        SetChoice(s, Find("FRAME RATE"), 1);
        CHECK(Changed(s));
        Load(s, "MSC_PERFORMANCE_MODE=1\n");
        ResetAll(s);
        CHECK(Changed(s));
        State fresh;
        Load(fresh, nullptr);
        CHECK(!Changed(fresh));
        ResetAll(fresh);
        CHECK(!Changed(fresh));
    }
    // 1.3: CORE PLAN is the way out of a failed start: it is on SAFE START, and no other row moves it
    {
        const int core = Find("CORE PLAN");
        State s;
        Load(s, "MSC_PLAN=S\n");
        CHECK(s.choice[core] == 3);
        SetChoice(s, Find("FRAME RATE"), kLocked);
        CHECK(s.choice[core] == 3);
        std::string out;
        CHECK(Serialize(s, out) && Has(out, "MSC_PLAN=S"));
        int page = -1, row = -1;
        CHECK(FindRow(core, &page, &row) && !std::strcmp(PageName(page), "SAFE START") && row == 0);   // (set18: the recovery page L + R opens)
    }
    // 1.3: the LiveArea's parameter. A press is the parameter appearing: one that stays readable after the read (the Galaxy port's console) opens the screen once and
    // not every 250 ms; one that is consumed by the read opens it at every press. Three failed tries drop the press; a retry that works ends it.
    {
        OpenRequest sticky;
        CHECK(sticky.Observe(true) && sticky.Pending());
        sticky.Opened();
        for (int i = 0; i < 40; ++i) {
            CHECK(!sticky.Observe(true));
            CHECK(!sticky.Pending());
        }
        OpenRequest consumed;
        for (int press = 0; press < 3; ++press) {
            CHECK(consumed.Observe(true) && consumed.Pending());
            consumed.Opened();
            CHECK(!consumed.Observe(false) && !consumed.Pending());
        }
        OpenRequest failing;
        CHECK(failing.Observe(true));
        CHECK(!failing.Failed() && failing.Pending());
        CHECK(!failing.Observe(true) && failing.Pending());   // the next poll tries again
        CHECK(!failing.Failed() && failing.Pending());
        CHECK(failing.Failed() && !failing.Pending());        // the third try was the last
        CHECK(!failing.Observe(true) && !failing.Pending() && !failing.Failed());   // stopped, even where the parameter stays
        CHECK(!failing.Observe(false) && failing.Observe(true) && failing.Pending());   // a later press is a new one, with three tries of its own
        CHECK(!failing.Failed());
        failing.Opened();
        CHECK(!failing.Pending());
        OpenRequest started;
        started.Seed(true);   // the start-up read saw it (the screen of that start, or an old one on a restart)
        CHECK(!started.Observe(true) && !started.Pending());
        CHECK(!started.Observe(false) && started.Observe(true));
    }
    // 1.3: older files and new defaults. A file only names what its player moved, so nothing an older build's screen wrote can touch a switch outside its rows: the
    // defaults of vita_plan.cpp are given in full over such a file, and a save writes no line the file and the choices do not ask for. The values kBuildDefaults reads as a
    // row's default are the values vita_plan.cpp gives: a default changed there must keep its old value listed, or a line naming it would pin the old default.
    {
        const char* const older[] = {
            "MSC_PERFORMANCE_MODE=1\nMSC_FPS=30\nMSC_VITA_RENDER=640x362\nMSC_VITA_MSAA=2\nMSC_VITA_ANISO=3\n",                                    // the 1.1 rows
            "MSC_DYNRES_MAX=1.0\nMSC_VGX_MSAA=0\nMSC_FPS_REPLAY=30\nMSC_FX_LOWRES_SCENES=nis\nMSC_FX_LOWRES_MINPX=480000\nMSC_VITA_ANISO=0\n",   // 1.2's PERFORMANCE (60)
        };
        const char* const untouched[] = {"MSC_VGX_SOFTXFORM", "MSC_VGX_SHQUAD", "MSC_VITA_QUEUE_FIXED30", "MSC_PACE_VBLANK", "MSC_SLIP", "MSC_DYNRES_UNDO", "MSC_FPS_LADDERHOLD",
                                         "MSC_VITA_VGL_SKIP", "MSC_VGX_UNIDUP", "MSC_LATE_WAIT", "MSC_VGX_RT", "MSC_GAME_FIXES"};
        for (const char* file : older) {
            State s;
            Load(s, file);
            std::string out;
            CHECK(Serialize(s, out));
            for (const char* key : untouched) CHECK(CountLines(out, key) == 0);
            for (const char* key : untouched) unsetenv(key);
            CHECK(ApplySavedPlan(out) >= 0);
            for (const char* key : untouched) CHECK(std::getenv(key) != nullptr);
        }
        const char* const named[][2] = {{"DEPTH OF FIELD", "MSC_DOF"}};   // (the other build values of the removed rows are no row's lines any more; MSC_VGX_MSAA=4 is the ladder's, tested above)
        ForgetDefaultsForTest();
        for (const auto& row : named) unsetenv(row[1]);
        unsetenv("MSC_DYNRES");
        unsetenv("MSC_VITA_RENDER");
        unsetenv("MSC_VITA_MSAA");
        ApplySavedPlan("");
        for (const auto& row : named) {
            const char* value = std::getenv(row[1]);
            CHECK(value != nullptr);
            if (!value) continue;
            const std::string file = std::string(row[1]) + "=" + value + "\n";
            State s;
            Load(s, file.c_str());
            CHECK(s.choice[Find(row[0])] == GetOption(Find(row[0])).defaultChoice);   // the build's own value, named in a file, is the default choice
            std::string out;
            CHECK(Serialize(s, out) && out.empty());                                  // and the next save drops the line
        }
    }
    // 1.3 prune: the keys of the removed rows and choices (kRetiredKeys): no row owns one; every one still reaches the game as an ordinary line; the old files, the kit's files and the
    // development files of 1.3 are handled as the notes say (docs/SETTINGS.md "Rows that went"). `value` is a non-default value the removed row could have written.
    {
        struct Retired { const char* key; const char* value; };
        static const Retired kRetired[] = {
            {"MSC_FPS_BETWEEN", "pace"}, {"MSC_FPS_REPLAY", "30"}, {"MSC_PICTURE_HOLD", "0"}, {"MSC_PICTURE_HOLD_MSAA", "4"}, {"MSC_DYNRES", "off"}, {"MSC_DYNRES_MAX", "1.0"},
            {"MSC_DYNRES_FG", "0"}, {"MSC_DYNRES_MIN60", "0.70"}, {"MSC_FX_LOWRES", "0"}, {"MSC_FX_LOWRES_SCENES", "nis"}, {"MSC_FX_LOWRES_MINPX", "480000"}, {"MSC_VITA_ANISO", "0"},
            {"MSC_FRAME_PACING", "0"}, {"MSC_FX_GLOW", "half"}, {"MSC_SHADOW_EVERY", "2"}, {"MSC_REPLAY_FARLOD", "12"}, {"MSC_VGX_GXM_DIRECT", "0"}, {"MSC_VITA_CORE3", "1"},
            {"MSC_VITA_CORE3_SEND", "2"}, {"MSC_VGX_LODBIAS", "-2"}, {"MSC_VITA_SHAPE", "1"}, {"MSC_VGX_HUDNATIVE", "1"}, {"MSC_MENU_STILL", "1"}, {"MSC_PAUSE_STILL", "1"},
            {"MSC_FX_GOV", "1"}, {"MSC_FX_GOV_MEGA", "1"}, {"MSC_SHADOW_PLAY", "3"}, {"MSC_VGX_LA88", "2"}, {"MSC_VGX_CMPRA", "2"}, {"MSC_VGX_TEX16", "1"}, {"NP_MENU_NAV", "0"},
            {"MSC_VITA_SHARPEN", "0.35"}, {"MSC_VITA_EDGE_AA", "0"},
            // set18
            {"MSC_VITA_RENDER", "864x488"}, {"MSC_VGX_MSAA", "0"}, {"MSC_LAN_INPUT_DELAY", "6"}, {"NP_TOUCH_POINTER", "0"}, {"MSC_LAN_MODE", "wifi"},
            {"MSC_SHADOW_LIVE", "full"}, {"NP_WIDESCREEN", "0"}, {"MSC_LAN_DIALOG", "join"},
        };
        const int kRetiredCount = int(sizeof(kRetired) / sizeof(kRetired[0]));
        CHECK(kRetiredCount == 41);
        const char* const kStart = "# ---- Settings screen (LiveArea > Settings): the lines below are written by it; the rest of this file is kept ----";
        const char* const kEnd = "# ---- end of the Settings screen's lines ----";
        const auto allRetired = [&]() {
            std::string t;
            for (const Retired& r : kRetired) t += std::string(r.key) + "=" + r.value + "\n";
            return t;
        };
        const auto onlyDefaults = [](const State& st) {
            for (int o = 0; o < OptionCount(); ++o)
                if (st.choice[o] != GetOption(o).defaultChoice) return false;
            return true;
        };
        // no row owns a retired key: not in any choice, not as an old key; a file naming one alone changes no row and is kept byte for byte
        for (const Retired& r : kRetired) {
            for (int o = 0; o < OptionCount(); ++o)
                for (int c = 0; c < ChoiceCount(o); ++c)
                    for (const Assign& a : GetOption(o).choices[c].set)
                        if (a.key) CHECK(std::strcmp(a.key, r.key) != 0);
            const std::string file = std::string("# hand file\nMSC_WATCH=1\n") + r.key + "=" + r.value + "\n";
            State s;
            Load(s, file.c_str());
            std::string out;
            CHECK(onlyDefaults(s) && !Changed(s) && Serialize(s, out) && out == file);   // outside the block, without a reset: byte for byte
        }
        // the whole set in one unstamped 1.1 / 1.2 file with the removed choices of the kept rows: the guard cleans the lines of its rules once and names the backup; the screen then reads
        // a real choice everywhere but SHARPNESS (CRISP+) and RESOLUTION (960x544), crowd HIDE and MSC_FPS=30 survive, and the 1.3-only keys no public file names pass through
        {
            const std::string file = std::string("# my 1.1 file\nMSC_PERFORMANCE_MODE=1\nMSC_FPS=30\nMSC_VITA_RENDER=960x544\nMSC_DOF=1\nMSC_VITA_UPSCALE=sharp+\nMSC_WATCH=1\n") + allRetired();
            NpSettingsGuard::Report report;
            const std::string cleaned = NpSettingsGuard::Migrate(file, "1.3.0", "0123456789abcdef", report);
            CHECK(!report.current && report.changed && report.backupName == "np-player.before-1.3.txt" && report.turnedOff >= 12 && NpSettingsGuard::LoadingLine(report));
            State s;
            Load(s, cleaned.c_str());
            int custom = 0;
            for (int o = 0; o < OptionCount(); ++o) {
                CHECK(s.choice[o] >= -1 && s.choice[o] < ChoiceCount(o));
                if (s.choice[o] == -1) {
                    ++custom;
                    CHECK(o == Find("SHARPNESS"));
                }
            }
            CHECK(custom == 1);   // (RESOLUTION: the guard turned MSC_VITA_RENDER off; only SHARPNESS reads CUSTOM)
            CHECK(s.choice[Find("STADIUM CROWD")] == 1 && s.choice[Find("FRAME RATE")] == kLocked && s.choice[Find("DEPTH OF FIELD")] == 0);
            CHECK(s.choice[Find("SHARPNESS")] == -1 && !Changed(s));
            std::string out;
            CHECK(Serialize(s, out) && out.size() < 15000);
            size_t assignments = 0;
            for (size_t at = 0; at < out.size();) {
                size_t end = out.find('\n', at);
                if (end == std::string::npos) end = out.size();
                if (out[at] != '#' && out.find('=', at) < end) ++assignments;
                at = end + 1;
            }
            CHECK(assignments <= 96);
            for (const char* gone : {"MSC_VITA_RENDER=960x544", "MSC_DOF=1", "MSC_FPS_REPLAY=30", "MSC_DYNRES=off", "MSC_FRAME_PACING=0", "MSC_VITA_ANISO=0", "MSC_FX_LOWRES=0", "NP_MENU_NAV=0"})
                CHECK(!Has(out, gone));   // (turned off by the guard: only as a "# 1.3 turned off:" comment)
            for (const char* kept : {"MSC_PERFORMANCE_MODE=1", "MSC_FPS=30", "MSC_WATCH=1", "MSC_VITA_UPSCALE=sharp+", "MSC_VITA_SHARPEN=0.35", "MSC_FX_GLOW=half", "MSC_FX_GOV=1", "MSC_PICTURE_HOLD=0"})
                CHECK(Has(out, kept));    // (a row's own lines, the CUSTOM pair, and 1.3-only keys the guard has no rule for)
            // a 1.2 PERFORMANCE file (nothing but what its screen wrote): everything is the default but the crowd
            const char* perf12 = "MSC_PERFORMANCE_MODE=1\nMSC_FPS_REPLAY=30\nMSC_DYNRES_MAX=1.0\nMSC_VGX_MSAA=0\nMSC_FX_LOWRES_SCENES=nis\nMSC_FX_LOWRES_MINPX=480000\nMSC_VITA_ANISO=0\n";
            NpSettingsGuard::Report r2;
            const std::string c2 = NpSettingsGuard::Migrate(perf12, "1.3.0", "0123456789abcdef", r2);
            State p;
            Load(p, c2.c_str());
            for (int o = 0; o < OptionCount(); ++o) CHECK(p.choice[o] == (o == Find("STADIUM CROWD") ? 1 : GetOption(o).defaultChoice));
            CHECK(r2.turnedOff == 6 && !Changed(p));
            // 1.0 / 1.1 menu stick off and 1.1 distant textures level 3
            NpSettingsGuard::Report r3;
            const std::string c3 = NpSettingsGuard::Migrate("NP_MENU_NAV=0\nMSC_VITA_ANISO=3\nMSC_DOF=1\nMSC_FPS=30\n", "1.3.0", "x", r3);
            CHECK(r3.turnedOff == 3 && NpSettingsGuard::Migrate(c3, "1.3.0", "x", r3) == c3);   // (cleaned once: a second pass finds it stamped)
            State q;
            Load(q, c3.c_str());
            CHECK(q.choice[Find("DEPTH OF FIELD")] == 0 && q.choice[Find("FRAME RATE")] == 1);
        }
        // a kit-style file (stamped 1.3.0, no block, every retired key set with the kit's own values): the guard returns it unchanged; the screen shows the defaults and an unchanged save
        // gives back the same bytes
        {
            const std::string file = std::string("# np-player.txt written by 1.3.0 build console-kit (the kit's files are not old settings files)\n# run perf13\nMSC_INPUT_SCRIPT=ux0:data/x.txt\n") + allRetired() + "MSC_WATCH=1\n";
            NpSettingsGuard::Report report;
            CHECK(NpSettingsGuard::Migrate(file, "1.3.0", "0123456789abcdef", report) == file && report.current && !report.changed);
            State s;
            Load(s, file.c_str());
            std::string out;
            CHECK(onlyDefaults(s) && !Changed(s) && Serialize(s, out) && out == file);
            // a kit file that also holds a key a row owns: the row's line moves into the block, every retired line stays where it was
            const std::string mixed = file + "MSC_PERFORMANCE_MODE=1\n";
            State m;
            Load(m, mixed.c_str());
            CHECK(m.choice[Find("STADIUM CROWD")] == 1 && Serialize(m, out));
            for (const Retired& r : kRetired) CHECK(Has(out, (std::string(r.key) + "=" + r.value).c_str()));
        }
        // a 1.3 development file: the screen wrote the retired keys into its block. Load shows the defaults; an unchanged save drops exactly those lines and nothing else
        {
            const std::string head = "# np-player.txt written by 1.3.0 build dev\n# mine\nMSC_WATCH=1\nOTHER_KEY=kept\n";
            const std::string file = head + kStart + "\nMSC_FPS=30\n" + allRetired() + kEnd + "\nTAIL=1\n";
            State s;
            Load(s, file.c_str());
            for (int o = 0; o < OptionCount(); ++o)
                if (o != Find("SHARPNESS")) CHECK(s.choice[o] == (o == Find("FRAME RATE") ? 1 : GetOption(o).defaultChoice));
            std::string out;
            CHECK(Serialize(s, out));
            for (const Retired& r : kRetired) CHECK(CountLines(out, (std::string(r.key) + "=").c_str()) == 0);
            for (const char* kept : {"# np-player.txt written by 1.3.0 build dev", "# mine", "MSC_WATCH=1", "OTHER_KEY=kept", "TAIL=1", "MSC_FPS=30"}) CHECK(Has(out, kept));
            CHECK(CountLines(out, "MSC_FPS=") == 1);
            // (SHARPNESS reads the default here: MSC_VITA_UPSCALE is not in the file, so its companions go too)
            CHECK(s.choice[Find("SHARPNESS")] == 0);
            State again;
            Load(again, out.c_str());
            std::string second;
            CHECK(Serialize(again, second) && second == out);   // and a second save changes nothing
            // a hand line of a retired key OUTSIDE the block stays through a save, and also goes after RESET TO DEFAULTS
            const std::string outside = head + "MSC_FX_GOV=1\n" + kStart + "\nMSC_FPS=30\nMSC_DYNRES_FG=0\n" + kEnd + "\nMSC_SHADOW_PLAY=3\n";
            State t;
            Load(t, outside.c_str());
            CHECK(Serialize(t, out) && Has(out, "MSC_FX_GOV=1") && Has(out, "MSC_SHADOW_PLAY=3") && CountLines(out, "MSC_DYNRES_FG") == 0);
            ResetAll(t);
            CHECK(Serialize(t, out) && CountLines(out, "MSC_FX_GOV") == 0 && CountLines(out, "MSC_SHADOW_PLAY") == 0 && CountLines(out, "MSC_DYNRES_FG") == 0 && CountLines(out, "MSC_FPS=") == 0);
            for (const char* kept : {"# mine", "MSC_WATCH=1", "OTHER_KEY=kept"}) CHECK(Has(out, kept));
            // the whole set outside a block, then RESET TO DEFAULTS: all of them go, the other lines stay
            const std::string loose = head + allRetired();
            State u;
            Load(u, loose.c_str());
            CHECK(Serialize(u, out) && out == loose);
            ResetAll(u);
            CHECK(Serialize(u, out));
            for (const Retired& r : kRetired) CHECK(CountLines(out, (std::string(r.key) + "=").c_str()) == 0);
            for (const char* kept : {"# mine", "MSC_WATCH=1", "OTHER_KEY=kept"}) CHECK(Has(out, kept));
        }
        // a development file whose block holds a removed CHOICE of a kept row: the row reads CUSTOM and the lines stay with it (the pair of CRISP+ is not torn apart); stepping the row ends it
        {
            const std::string file = std::string(kStart) + "\nMSC_VITA_RENDER=960x544\nMSC_DOF=1\nMSC_VITA_UPSCALE=sharp+\nMSC_VITA_SHARPEN=0.35\nMSC_VITA_EDGE_AA=0\nMSC_FX_GOV=1\n" + kEnd + "\n";
            State s;
            Load(s, file.c_str());
            CHECK(s.choice[Find("DEPTH OF FIELD")] == -1 && s.choice[Find("SHARPNESS")] == -1);
            std::string out;
            CHECK(Serialize(s, out) && !Has(out, "MSC_VITA_RENDER=960x544") && Has(out, "MSC_DOF=1") && Has(out, "MSC_VITA_UPSCALE=sharp+") && Has(out, "MSC_VITA_SHARPEN=0.35") &&
                  Has(out, "MSC_VITA_EDGE_AA=0") && CountLines(out, "MSC_FX_GOV") == 0);
            Step(s, Find("SHARPNESS"), +1);
            CHECK(Serialize(s, out) && CountLines(out, "MSC_VITA_UPSCALE") == 0 && CountLines(out, "MSC_VITA_SHARPEN") == 0 && CountLines(out, "MSC_VITA_EDGE_AA") == 0 && Has(out, "MSC_DOF=1"));
        }
    }
    // every option and choice is drawn within the buffer, every label fits its column, every row reachable
    {
        std::vector<uint32_t> buffer(kWidth * kHeight + 4096, 0xDEADBEEFu);
        State s;
        Load(s, nullptr);
        for (int o = 0; o < OptionCount(); ++o) {
            CHECK(std::strlen(GetOption(o).title) <= 22);
            CHECK(std::strlen(GetOption(o).help[0]) <= 76 && std::strlen(GetOption(o).help[1]) <= 76);
            for (int c = 0; c < ChoiceCount(o); ++c) CHECK(std::strlen(GetOption(o).choices[c].label) <= 22);
        }
        for (int p = 0; p < PageCount(); ++p)
            for (int r = 0; r < RowCount(p); ++r) Draw(buffer.data(), s, p, r, r == 3 ? "Could not write the file" : "", r == 3);
        for (int choice = 0; choice < kLeaveChoices; ++choice) DrawLeaveConfirmation(buffer.data(), choice);
        {
            State c;
            Load(c, "MSC_CONTROLS=classic\nMSC_REAR_TOUCH=0\nMSC_TOUCH_SHAKE=0\n");
            DrawControls(buffer.data(), c);
            DrawControls(buffer.data(), s);
        }
        for (size_t i = kWidth * kHeight; i < buffer.size(); ++i) CHECK(buffer[i] == 0xDEADBEEFu);
        // set18: two pages. SAFE START (L + R at start-up: CORE PLAN and the facts) and ONLINE (the game menu's ONLINE ACCOUNT row: the online name and the three actions). Every other row
        // is the game menu's (np_native_settings.cpp) and on no page here; every page ends with RESET and the two SAVE rows and none needs scrolling
        {
            std::vector<int> seen(OptionCount(), 0);
            for (int p = 0; p < PageCount(); ++p) {
                const int rows = RowCount(p);
                CHECK(rows <= 14);
                CHECK(GetRow(p, rows - 3).kind == kRowReset && GetRow(p, rows - 2).kind == kRowSaveStart && GetRow(p, rows - 1).kind == kRowSaveExit);
                for (int r = 0; r < rows - 3; ++r) {
                    const Row row = GetRow(p, r);
                    CHECK(row.kind == kRowOption || row.kind == kRowInfo || row.kind == kRowAction);
                    if (row.kind == kRowInfo) {
                        CHECK(row.option >= 0 && row.option < InfoCount() && InfoTitle(row.option)[0]);
                        continue;
                    }
                    if (row.kind == kRowAction) {
                        CHECK(row.option >= 0 && row.option < ActionCount() && ActionTitle(row.option)[0]);
                        continue;
                    }
                    ++seen[row.option];
                    int atPage = -1, atRow = -1;
                    CHECK(FindRow(row.option, &atPage, &atRow) && atPage == p && atRow == r);
                }
            }
            for (int o = 0; o < OptionCount(); ++o) CHECK(seen[o] == (o == Find("CORE PLAN") ? 1 : 0));
            int nowhere = -1, nowhereRow = -1;
            CHECK(!FindRow(Find("FRAME RATE"), &nowhere, &nowhereRow));
            struct Layout { const char* page; int rows; const char* row[10]; };
            const Layout layout[] = {
                {"SAFE START", 5, {"CORE PLAN", "(info) VERSION", "(info) BUILD", "(info) SETTINGS FILE", "(info) GAME FILES"}},
                {"ONLINE", 4, {"(info) ONLINE NAME", "(action) SEND A REPORT", "(action) SAVE A REPORT", "(action) DELETE MY ONLINE ACCOUNT"}},
            };
            CHECK(PageCount() == 2 && OptionCount() == 20);
            for (int p = 0; p < 2 && p < PageCount(); ++p) {
                CHECK(!std::strcmp(PageName(p), layout[p].page) && RowCount(p) == layout[p].rows + 3);
                for (int r = 0; r < layout[p].rows && r < RowCount(p) - 3; ++r) {
                    const Row row = GetRow(p, r);
                    const std::string name = row.kind == kRowInfo ? std::string("(info) ") + InfoTitle(row.option) : row.kind == kRowAction ? std::string("(action) ") + ActionTitle(row.option) : std::string(row.kind == kRowOption ? GetOption(row.option).title : "?");
                    CHECK(name == layout[p].row[r]);
                }
            }
            // the removed rows are in no table (1.3 prune and set18)
            for (const char* gone : {"PICTURE MODE", "REPLAYS & CUT-SCENES", "FULL PICTURE", "DYNAMIC RESOLUTION", "SMALLEST PICTURE AT 60", "PRIORITIZE PLAYERS", "EFFECTS QUALITY", "DISTANT TEXTURES",
                                     "MENU STICK", "FRAME PACING", "SHARPER WHEN POSSIBLE", "GLOW", "REPLAY SHADOWS", "REPLAY FAR PLAYERS", "GXM DIRECT", "FOURTH CORE", "TEXTURE DETAIL",
                                     "PICTURE SHAPE", "NATIVE HUD", "STILL BACKDROPS", "EFFECTS UNDER LOAD", "MATCH SHADOWS", "TEXTURE MEMORY",
                                     "RESOLUTION", "ANTI-ALIASING", "SMOOTH 60 MODE", "INPUT DELAY", "TOUCH POINTER", "TOUCH MEGA SAVE", "MULTIPLAYER LINK", "LIVE SHADOWS", "PICTURE FORMAT", "NEARBY PAIRING"})
                CHECK(Find(gone) == -1 && OptionIndex(gone) == -1);
            for (int i = 0; i < InfoCount(); ++i) CHECK(std::strcmp(InfoTitle(i), "PICTURE (AUTO)") != 0 && std::strcmp(InfoTitle(i), "SETTINGS AT START") != 0);
            // the choices that went from kept rows
            CHECK(ChoiceCount(Find("FRAME RATE")) == kLocked + 1 && ChoiceCount(Find("DEPTH OF FIELD")) == 2 && ChoiceCount(Find("SHARPNESS")) == 3);
            CHECK(ChoiceCount(Find("ORIGINAL GAME FIXES")) == 2 && ChoiceCount(Find("MEGA SAVE SPEED")) == 2 && ChoiceCount(Find("MEGA SAVE AIM")) == 3);
            for (int o = 0; o < OptionCount(); ++o)
                for (int c = 0; c < ChoiceCount(o); ++c) {
                    const char* label = GetOption(o).choices[c].label;
                    for (const char* g : {"UP TO 60", "4X", "CRISP+", "SMOOTH FULL", "SMOOTH NO EDGE AA", "ON (AS GAME)", "SCREEN 960X544", "OFF (AS RETAIL)", "ORIGINAL", "45 FPS", "AUTO (UP TO 60)"})
                        CHECK(std::strcmp(label, g) != 0);
                }
            for (int o = 0; o < OptionCount(); ++o) CHECK(GetOption(o).help[0][0] && !GetOption(o).help[1][0]);   // one line of help per row
            // the version and the build id are on SAFE START, and are this build's
            bool version = false, build = false;
            for (int r = 0; r < RowCount(0) - 3; ++r) {
                const Row row = GetRow(0, r);
                if (row.kind != kRowInfo) continue;
                version |= !std::strcmp(InfoTitle(row.option), "VERSION") && !std::strcmp(InfoValue(row.option), NpBuildIdentity::Version());
                build |= !std::strcmp(InfoTitle(row.option), "BUILD") && InfoValue(row.option)[0];
            }
            CHECK(version && build);
            // the three actions stand on the ONLINE page, once each
            int found = 0, onPage = -1;
            for (int p = 0; p < PageCount(); ++p)
                for (int r = 0; r < RowCount(p); ++r) {
                    const Row row = GetRow(p, r);
                    if (row.kind == kRowAction) { ++found; onPage = p; }
                }
            CHECK(found == 3 && ActionCount() == 3 && onPage >= 0 && !std::strcmp(PageName(onPage), "ONLINE"));
        }
        if (argc > 1) {
            char path[512];
            State q;
            Load(q, nullptr);
            Draw(buffer.data(), q, 0, 0, "", false);
            std::snprintf(path, sizeof path, "%s/settings-default.ppm", argv[1]);
            CHECK(Ppm(path, buffer.data()));
            // W5 CTL: the list scrolled to its controls rows, and the controls page of both layouts
            DrawAt(buffer.data(), q, "CONTROLS");
            std::snprintf(path, sizeof path, "%s/settings-controls-rows.ppm", argv[1]);
            CHECK(Ppm(path, buffer.data()));
            // W5 CAM: the list scrolled to the two bonus rows (UNLOCK ALL CHARACTERS selected), defaults, and with both changed
            DrawAt(buffer.data(), q, "UNLOCK ALL CHARACTERS");
            std::snprintf(path, sizeof path, "%s/settings-bonus-rows.ppm", argv[1]);
            CHECK(Ppm(path, buffer.data()));
            {
                State b;
                Load(b, "MSC_UNLOCK_CAPTAINS=0\nMSC_UNLOCK_STADIUMS=1\n");
                DrawAt(buffer.data(), b, "UNLOCK ALL STADIUMS");
                std::snprintf(path, sizeof path, "%s/settings-bonus-rows-changed.ppm", argv[1]);
                CHECK(Ppm(path, buffer.data()));
            }
            DrawControls(buffer.data(), q);
            std::snprintf(path, sizeof path, "%s/controls-modern.ppm", argv[1]);
            CHECK(Ppm(path, buffer.data()));
            {
                State c;
                Load(c, "MSC_CONTROLS=classic\n");
                DrawControls(buffer.data(), c);
                std::snprintf(path, sizeof path, "%s/controls-classic.ppm", argv[1]);
                CHECK(Ppm(path, buffer.data()));
            }
            {
                State rear;
                Load(rear, "MSC_MEGA_AIM=rear\nMSC_MEGA_POINTER_SPEED=4\n");
                DrawAt(buffer.data(), rear, "MEGA SAVE AIM", "", true);
                std::snprintf(path, sizeof path, "%s/settings-mega-aim.ppm", argv[1]);
                CHECK(Ppm(path, buffer.data()));
                DrawAt(buffer.data(), rear, "MEGA SAVE SPEED", "", true);
                std::snprintf(path, sizeof path, "%s/settings-mega-speed.ppm", argv[1]);
                CHECK(Ppm(path, buffer.data()));
                DrawControls(buffer.data(), rear);
                std::snprintf(path, sizeof path, "%s/controls-rear.ppm", argv[1]);
                CHECK(Ppm(path, buffer.data()));
            }
            // the picture rows (defaults, and a 1.1 file whose ANTI-ALIASING line reads CUSTOM)
            DrawAt(buffer.data(), q, "FRAME RATE");
            std::snprintf(path, sizeof path, "%s/settings-picture-rows.ppm", argv[1]);
            CHECK(Ppm(path, buffer.data()));
            {
                State legacy;
                Load(legacy, "MSC_VITA_MSAA=2\n");
                DrawAt(buffer.data(), legacy, "ANTI-ALIASING");
                std::snprintf(path, sizeof path, "%s/settings-antialiasing-legacy.ppm", argv[1]);
                CHECK(Ppm(path, buffer.data()));
            }
            SetChoice(q, Find("FRAME RATE"), 1);
            SetChoice(q, Find("MULTIPLAYER LINK"), 1);
            SetChoice(q, Find("ORIGINAL GAME FIXES"), 2);
            DrawAt(buffer.data(), q, "ORIGINAL GAME FIXES");
            std::snprintf(path, sizeof path, "%s/settings-quality.ppm", argv[1]);
            CHECK(Ppm(path, buffer.data()));
            State c;
            Load(c, "MSC_VITA_RENDER=800x450\n");
            SetChoice(c, Find("STADIUM CROWD"), 1);
            Draw(buffer.data(), c, 0, RowCount(0) - 2, "Saved to ux0:data/mscharged/np-player.txt", false);
            std::snprintf(path, sizeof path, "%s/settings-custom.ppm", argv[1]);
            CHECK(Ppm(path, buffer.data()));
            // 1.3: the pages, a row with no effect, the leave dialog
            for (int p = 0; p < PageCount(); ++p) {
                Draw(buffer.data(), s, p, 0, "", false);
                std::snprintf(path, sizeof path, "%s/settings-page-%d.ppm", argv[1], p);
                CHECK(Ppm(path, buffer.data()));
            }
            {   // wave 11 mpdiag18: the MULTIPLAYER page with SAVE A REPORT selected, and after Cross (the glue's message), and when the card refuses
                int page = 0;
                for (int p = 0; p < PageCount(); ++p) if (!std::strcmp(PageName(p), "MULTIPLAYER")) page = p;
                Draw(buffer.data(), s, page, 3, "", false);
                std::snprintf(path, sizeof path, "%s/settings-report-row.ppm", argv[1]);
                CHECK(Ppm(path, buffer.data()));
                Draw(buffer.data(), s, page, 3, "Report saved: ux0:data/mscharged/mp-report.txt", false);
                std::snprintf(path, sizeof path, "%s/settings-report-saved.ppm", argv[1]);
                CHECK(Ppm(path, buffer.data()));
                Draw(buffer.data(), s, page, 3, "Could not write the report. Is the memory card full?", true);
                std::snprintf(path, sizeof path, "%s/settings-report-failed.ppm", argv[1]);
                CHECK(Ppm(path, buffer.data()));
                // W22: the consent screen, the delete confirmation and the waiting screens, drawn over the page
                static const struct { ReportDialog kind; const char* name; } kDialogs[] = {{kReportSend, "send"}, {kReportDelete, "delete"}, {kReportWorking, "working"}, {kReportDeleting, "deleting"}};
                for (const auto& d : kDialogs) {
                    Draw(buffer.data(), s, page, 5, "", false);
                    DrawReportDialog(buffer.data(), d.kind, 0, false);
                    std::snprintf(path, sizeof path, "%s/settings-report-dialog-%s.ppm", argv[1], d.name);
                    CHECK(Ppm(path, buffer.data()));
                }
            }
            DrawAt(buffer.data(), c, "ANTI-ALIASING");
            std::snprintf(path, sizeof path, "%s/settings-no-effect.ppm", argv[1]);
            CHECK(Ppm(path, buffer.data()));
            DrawLeaveConfirmation(buffer.data(), kLeaveKeepEditing);
            std::snprintf(path, sizeof path, "%s/settings-leave.ppm", argv[1]);
            CHECK(Ppm(path, buffer.data()));
        }
    }
    // 1.3 in-game settings: the leave dialog's logic. Nothing changed: no dialog (leave silently). Start-up: KEEP EDITING, DISCARD, SAVE. Running game with a change (every
    // change needs a restart; 1.3: STADIUM CROWD, REDUCED CAMERA SHAKE and REAR TOUCH are live): SAVE, DON'T SAVE, KEEP EDITING (never a restart from a match). SAVE is lit first. Circle (the caller) = keep editing.
    {
        int ids[kMaxLeave];
        State s;
        Load(s, nullptr);
        CHECK(!Changed(s) && !RestartNeeded(s));
        CHECK(LeaveDialog(true, Changed(s), RestartNeeded(s), ids) == 0);    // unchanged: silent, running or not
        CHECK(LeaveDialog(false, Changed(s), RestartNeeded(s), ids) == 0);
        const int fps = Find("FRAME RATE");
        SetChoice(s, fps, 1);
        CHECK(Changed(s) && RestartNeeded(s));
        int n = LeaveDialog(false, Changed(s), RestartNeeded(s), ids);
        CHECK(n == 3 && ids[0] == kLeaveKeepEditing && ids[1] == kLeaveDiscard && ids[2] == kLeaveSave);
        CHECK(LeaveDefault(ids, n) == 2 && ids[LeaveDefault(ids, n)] == kLeaveSave);
        n = LeaveDialog(true, Changed(s), RestartNeeded(s), ids);
        CHECK(n == 3 && ids[0] == kLeaveSave && ids[1] == kLeaveDiscard && ids[2] == kLeaveKeepEditing);   // (SAVE first, and lit first; no restart)
        CHECK(ids[LeaveDefault(ids, n)] == kLeaveSave && LeaveDefault(ids, n) == 0);
        bool keep = false, discard = false, restart = false, save = false;
        for (int i = 0; i < n; ++i) { keep |= ids[i] == kLeaveKeepEditing; discard |= ids[i] == kLeaveDiscard; restart |= ids[i] == kLeaveSaveRestart; save |= ids[i] == kLeaveSave; }
        CHECK(keep && discard && !restart && save);   // Keep editing is always there: edits are never lost silently; a running game never restarts
        CHECK(!std::strcmp(LeaveLabel(kLeaveSave, true), "SAVE") && !std::strcmp(LeaveLabel(kLeaveSave, false), "SAVE") && !std::strcmp(LeaveLabel(kLeaveDiscard, true), "DON'T SAVE") && !std::strcmp(LeaveLabel(kLeaveDiscard, false), "DISCARD"));
        CHECK(!std::strcmp(LeaveLabel(kLeaveSaveRestart, false), "SAVE AND START AGAIN") && !std::strcmp(LeaveLabel(kLeaveKeepEditing, true), "KEEP EDITING"));
        // moving wraps both ways and stays in range
        CHECK(LeaveMove(0, 3, -1) == 2 && LeaveMove(2, 3, 1) == 0 && LeaveMove(1, 3, 1) == 2 && LeaveMove(5, 0, 1) == 0);
        // 1.3 live rows: which rows are live, what ApplyLive hands over, and that a next-start row is never applied
        CHECK(IsLive(Find("STADIUM CROWD")) && IsLive(Find("REDUCED CAMERA SHAKE")) && IsLive(Find("REAR TOUCH")));
        int liveCount = 0;
        for (int o = 0; o < OptionCount(); ++o) liveCount += IsLive(o) ? 1 : 0;
        CHECK(liveCount == 3 && !IsLive(fps) && !IsLive(Find("LANGUAGE")) && !IsLive(Find("CONTROLS")) && Find("LIVE SHADOWS") < 0 && Find("SMOOTH 60 MODE") < 0 && Find("ANTI-ALIASING") < 0);
        {
            static std::string g_got;
            g_got.clear();
            auto record = [](const char* key, const char* value) { g_got += std::string(key) + "=" + (value ? value : "(unset)") + ";"; };
            CHECK(ApplyLive(s, record) == 0 && g_got.empty());     // only FRAME RATE changed (next start): nothing applied live
            CHECK(RestartNeeded(s) && NextStartChanged(s));
            State l;
            Load(l, nullptr);
            SetChoice(l, Find("STADIUM CROWD"), 1);
            SetChoice(l, Find("REDUCED CAMERA SHAKE"), 1);
            SetChoice(l, Find("REAR TOUCH"), 2);
            CHECK(!RestartNeeded(l) && !NextStartChanged(l) && Changed(l));   // live rows only: nothing waits for the next start
            CHECK(ApplyLive(l, record) == 3);
            CHECK(g_got == "MSC_PERFORMANCE_MODE=1;MSC_REDUCED_MOTION=1;MSC_REAR_TOUCH=0;" || (g_got.find("MSC_PERFORMANCE_MODE=1;") != std::string::npos && g_got.find("MSC_REDUCED_MOTION=1;") != std::string::npos && g_got.find("MSC_REAR_TOUCH=0;") != std::string::npos));
            g_got.clear();
            Load(l, "MSC_PERFORMANCE_MODE=1\nMSC_REDUCED_MOTION=1\nMSC_REAR_TOUCH=1\n");   // the file holds them; the player sets the defaults back: "unset" values
            SetChoice(l, Find("STADIUM CROWD"), 0);
            SetChoice(l, Find("REDUCED CAMERA SHAKE"), 0);
            SetChoice(l, Find("REAR TOUCH"), 0);
            CHECK(ApplyLive(l, record) == 3 && g_got.find("MSC_PERFORMANCE_MODE=(unset);") != std::string::npos && g_got.find("MSC_REDUCED_MOTION=(unset);") != std::string::npos && g_got.find("MSC_REAR_TOUCH=(unset);") != std::string::npos);
            CHECK(ApplyLive(l, nullptr) == 0);
            Load(l, nullptr);
            CHECK(ApplyLive(l, record) == 0);                       // nothing changed: nothing applied
        }
        // a change that is put back is no change: no dialog
        SetChoice(s, fps, 0);
        CHECK(!Changed(s) && LeaveDialog(true, Changed(s), RestartNeeded(s), ids) == 0);
        // set18: SMOOTH 60 is a choice of FRAME RATE (the game menu's row); its line is MSC_LOCK60=1, or nothing once it is the build's mode
        const int l60 = Find("FRAME RATE");
        CHECK(l60 >= 0 && GetOption(l60).defaultChoice == 0 && !std::strncmp(GetOption(l60).choices[kSmooth].label, "SMOOTH 60", 9));
        State f;
        Load(f, nullptr);
        SetChoice(f, l60, kSmooth);
        std::string out;
        CHECK(Serialize(f, out, 4096) && CountLines(out, "MSC_LOCK60=1") == (VitaPlanDefaults::kLock60 ? 0 : 1));
        // the dialog draws inside the picture
        std::vector<uint32_t> buffer(kWidth * kHeight + 4096, 0xDEADBEEFu);
        const int four[4] = {kLeaveSave, kLeaveSaveRestart, kLeaveDiscard, kLeaveKeepEditing};
        for (int i = 0; i < 4; ++i) DrawLeaveDialog(buffer.data(), four, 4, i, true);
        for (size_t i = kWidth * kHeight; i < buffer.size(); ++i) CHECK(buffer[i] == 0xDEADBEEFu);
    }

    // galaxyport18 (Galaxy c171/c172): no picture-mode column may set a ladder / child key whose gate key is off (np_picture_presets.h kGates). The columns are every preset of the screen (the
    // modes of the PICTURE MODE row), the MSC_PICTURE lists of np_picture_presets.h, and every choice of every row on its own (a row that needs another row says so in WhyNoEffect: it
    // must still not write its child with the gate off by default). A fixture proves the checker itself (a clean column passes, a broken one is named).
    {
        using np_picture::FirstGateBroken;
        auto columnGet = [](const std::vector<std::pair<std::string, std::string>>& kv) {
            return [&kv](const char* name) -> const char* {
                for (const auto& e : kv) if (e.first == name) return e.second.c_str();
                return nullptr;
            };
        };
        auto parse = [](const std::string& text) {
            std::vector<std::pair<std::string, std::string>> kv;
            size_t at = 0;
            while (at < text.size()) {
                size_t end = text.find('\n', at);
                if (end == std::string::npos) end = text.size();
                const std::string line = text.substr(at, end - at);
                at = end + 1;
                const size_t eq = line.find('=');
                if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
                kv.emplace_back(line.substr(0, eq), line.substr(eq + 1));
            }
            return kv;
        };
        // the checker itself
        {
            std::vector<std::pair<std::string, std::string>> clean = {{"MSC_PICTURE_HOLD", "0"}, {"MSC_DYNRES_MAX", "1.0"}, {"MSC_VGX_MSAA", "0"}};
            CHECK(!FirstGateBroken(columnGet(clean)));
            // (plan24 FIX-01: SMOOTH 60 is the default mode and has no held picture, so a limit with no mode named is fine; the hold is on only when the file says LOCKED 60 is off)
            std::vector<std::pair<std::string, std::string>> held = {{"MSC_LOCK60", "0"}, {"MSC_DYNRES_MAX", "1.0"}};   // the full-picture hold is on: no ladder, the limit is dead
            const np_picture::Gate* hg = FirstGateBroken(columnGet(held));
            CHECK(hg && !std::strcmp(hg->gate, "MSC_PICTURE_HOLD"));
            std::vector<std::pair<std::string, std::string>> smooth = {{"MSC_DYNRES_MAX", "1.0"}};
            CHECK(!FirstGateBroken(columnGet(smooth)));
            std::vector<std::pair<std::string, std::string>> thirty = {{"MSC_FPS", "30"}, {"MSC_DYNRES_MAX", "1.0"}};   // a 30 FPS rate: the plan does not turn the hold on
            CHECK(!FirstGateBroken(columnGet(thirty)));
            std::vector<std::pair<std::string, std::string>> off = {{"MSC_DYNRES", "off"}, {"MSC_DYNRES_MAX", "1.0"}};
            const np_picture::Gate* g = FirstGateBroken(columnGet(off));
            CHECK(g && !std::strcmp(g->child, "MSC_DYNRES_MAX") && !std::strcmp(g->gate, "MSC_DYNRES"));
            std::vector<std::pair<std::string, std::string>> fixed = {{"MSC_LOCK60", "1"}, {"MSC_VITA_RENDER", "640x362"}};
            CHECK(FirstGateBroken(columnGet(fixed)));
            std::vector<std::pair<std::string, std::string>> fps30 = {{"MSC_LOCK60", "1"}, {"MSC_FPS", "30"}};
            CHECK(FirstGateBroken(columnGet(fps30)));
            std::vector<std::pair<std::string, std::string>> fpsAuto = {{"MSC_LOCK60", "1"}, {"MSC_FPS", "auto"}};
            CHECK(!FirstGateBroken(columnGet(fpsAuto)));
            std::vector<std::pair<std::string, std::string>> pair = {{"MSC_PAIR_FAST_LEAVE", "3"}};   // MSC_PAIR_RATE is off by default
            CHECK(FirstGateBroken(columnGet(pair)));
            pair.emplace_back("MSC_PAIR_RATE", "1");
            CHECK(!FirstGateBroken(columnGet(pair)));
            std::vector<std::pair<std::string, std::string>> zero = {{"MSC_PAIR_FAST_LEAVE", "0"}};   // a child at 0 is not "set"
            CHECK(!FirstGateBroken(columnGet(zero)));
            std::vector<std::pair<std::string, std::string>> bg = {{"MSC_BG30_SCREENS", "1"}, {"MSC_BG30", "1"}};   // MSC_BG30 acts as 2 only
            CHECK(FirstGateBroken(columnGet(bg)));
        }
        auto report = [&](const std::string& name, const std::vector<std::pair<std::string, std::string>>& kv) {
            const np_picture::Gate* g = FirstGateBroken(columnGet(kv));
            if (g) std::fprintf(stderr, "preset gate: %s sets %s but its gate %s is %s\n", name.c_str(), g->child, g->gate, g->rule == np_picture::kNeedsOn ? "off" : "on");
            CHECK(!g);
        };
        // (the screen's picture modes were removed by prune18: the MSC_PICTURE lists and every row choice below are the columns now)
        const std::pair<const char*, const np_picture::Setting*> lists[] = {{"MSC_PICTURE=performance", np_picture::Find("performance")}, {"MSC_PICTURE=default", np_picture::Find("default")},
                                                                             {"MSC_PICTURE=quality", np_picture::Find("quality")}, {"MSC_PICTURE=full", np_picture::Find("full")},
                                                                             {"MSC_PICTURE=lock60", np_picture::Find("lock60")}};
        for (const auto& l : lists) {
            std::vector<std::pair<std::string, std::string>> kv;
            for (const np_picture::Setting* e = l.second; e && e->name; ++e) kv.emplace_back(e->name, e->value);
            report(l.first, kv);
        }
        for (int o = 0; o < OptionCount(); ++o)
            for (int c = 0; c < ChoiceCount(o); ++c) {
                State st;
                Load(st, nullptr);
                SetChoice(st, o, c);
                std::string out;
                CHECK(Serialize(st, out));
                if (WhyNoEffect(st, o)) continue;   // (the screen dims such a row and says why: it is not a column that silently does nothing)
                report(std::string("row ") + GetOption(o).title + " choice " + GetOption(o).choices[c].label, parse(out));
            }
    }
    std::printf("test_settings: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
