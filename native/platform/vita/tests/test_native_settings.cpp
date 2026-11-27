// Tests of the engine-free model of the native settings screen (platform/sdk/net/np_native_settings.*): rows, labels in three languages, presses, dim rows, saving
// byte for byte as the technical screen does, the file's other lines, leaving. Run with mutants by run_native_settings_tests.py.
#include "platform/sdk/net/np_native_settings.h"

#include "platform/sdk/include/np/np_text.h"
#include "platform/vita/src/np_build_identity.h"
#include "platform/vita/src/np_settings.h"
#include "platform/vita/src/np_settings_guard.h"
#include "platform/vita/src/vita_plan_defaults.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

static int g_checks = 0, g_failed = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failed; std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

namespace NN = NpNativeSettings;

static std::string g_path;

static void WriteRaw(const std::string& text) {
    FILE* f = std::fopen(g_path.c_str(), "wb");
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
}
static std::string ReadRaw() {
    std::string out;
    FILE* f = std::fopen(g_path.c_str(), "rb");
    if (!f) return out;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}
static int CodePoints(const char* s) {
    int n = 0;
    for (; *s; ++s)
        if ((static_cast<unsigned char>(*s) & 0xC0) != 0x80) ++n;
    return n;
}

struct Step {
    int page, row, dir;
    int raw = 1;   // raw steps of the pixel screen this one press stands for (set18: always 1, every choice is shown)
};

// set18: the rows of the game's menu, written out here (not read from the model) so that a model that moves or loses a row fails. nullptr: ONLINE ACCOUNT, RESET, no row.
static const char* const kOptions[5][5] = {{"UNLOCK ALL CHARACTERS", "UNLOCK ALL STADIUMS", "FRAME RATE", "CONTROLS", "LANGUAGE"},
                                           {"STADIUM CROWD", "SHARPNESS", "DEPTH OF FIELD", "HD CHARACTERS", nullptr},
                                           {"REAR TOUCH", "MEGA SAVE AIM", "MEGA SAVE SPEED", "TOUCH MEGA STRIKE", "TOUCH SHAKEDOWN"},
                                           {"REDUCED CAMERA SHAKE", "MEGA STRIKES", "ORIGINAL GAME FIXES", "POINTER SPEED", nullptr},
                                           {"CONNECTION", nullptr, nullptr, nullptr, nullptr}};

// What the pixel screen does for the same steps: Step on the row's option, then its save lambda (Serialize, stamp).
static std::string Reference(const std::string& file, const Step* steps, int count) {
    NpSettings::State s;
    NpSettings::Load(s, file.c_str());
    for (int i = 0; i < count; ++i)
        CHECK(kOptions[steps[i].page][steps[i].row] != nullptr);
    for (int i = 0; i < count; ++i)
        for (int k = 0; k < steps[i].raw; ++k) NpSettings::Step(s, NpSettings::OptionIndex(kOptions[steps[i].page][steps[i].row]), steps[i].dir);
    std::string out;
    NpSettings::Serialize(s, out);
    return NpSettingsGuard::Stamped(out, NpBuildIdentity::Version(), NpBuildIdentity::BuildId());
}

static std::string Model(const std::string& file, const Step* steps, int count) {
    WriteRaw(file);
    if (!NN::Open(NpText::kEnglish)) return "OPEN FAILED";
    for (int i = 0; i < count; ++i) NN::Press(steps[i].page, steps[i].row, steps[i].dir);
    if (!NN::Save()) return "SAVE FAILED";
    return ReadRaw();
}

extern "C" int NpNativeSettingsHostRestarts(void);

int main() {
    char dir[] = "/tmp/native-settings-XXXXXX";
    if (!mkdtemp(dir)) return 2;
    g_path = std::string(dir) + "/np-player.txt";
    setenv("MSC_SETTINGS_FILE", g_path.c_str(), 1);
    const bool smoothDefault = VitaPlanDefaults::kLock60;   // the FRAME RATE row follows the plan's switch (np_settings.cpp kFrameRateAuto / kFrameRateSmooth)
    const char* const frameDefault = smoothDefault ? "FRAME RATE: SMOOTH 60" : "FRAME RATE: AUTO";

    // ---- layout (set18): 5 pages (5, 4, 5, 4, 3 rows); every row of kOptions is where it says; ONLINE ACCOUNT and RESET are the last two rows of the ONLINE page
    WriteRaw("");
    CHECK(NN::Open(NpText::kEnglish) && NN::IsOpen());
    CHECK(NN::Pages() == 5 && NN::Rows(0) == 5 && NN::Rows(1) == 4 && NN::Rows(2) == 5 && NN::Rows(3) == 4 && NN::Rows(4) == 3 && NN::Rows(5) == 0 && NN::Rows(-1) == 0);
    for (int p = 0; p < NN::Pages(); ++p)
        for (int r = 0; r < NN::Rows(p); ++r) {
            const bool account = p == 4 && r == 1, reset = p == 4 && r == 2;
            CHECK(NN::IsAllSettingsRow(p, r) == account);
            CHECK(NN::IsResetRow(p, r) == reset);
            CHECK(NN::Option(p, r) == (kOptions[p][r] ? NpSettings::OptionIndex(kOptions[p][r]) : -1));
            if (kOptions[p][r]) CHECK(NN::Option(p, r) >= 0);
            char name[64], value[64], label[64], help[160];
            CHECK(NN::Name(p, r, name, sizeof name) > 0);
            NN::Value(p, r, value, sizeof value);
            CHECK(NN::Label(p, r, label, sizeof label) > 0);
            CHECK((account || reset) == (value[0] == 0));
            if (!account && !reset) CHECK(std::strcmp(label, (std::string(name) + ": " + value).c_str()) == 0);
            CHECK(NN::Help(p, r, help, sizeof help) > 10 && CodePoints(help) <= 76);   // one line of help per row
        }
    // every player setting is in the game's menu, once; CORE PLAN (a way out of a failed start) is only on the SAFE START page; the removed rows are gone from the table
    for (int o = 0; o < NpSettings::OptionCount(); ++o) {
        int seen = 0;
        for (int p = 0; p < NN::Pages(); ++p)
            for (int r = 0; r < NN::Rows(p); ++r) seen += NN::Option(p, r) == o;
        CHECK(seen == (std::strcmp(NpSettings::GetOption(o).title, "CORE PLAN") == 0 ? 0 : 1));
    }
    for (const char* gone : {"RESOLUTION", "SMOOTH 60 MODE", "ANTI-ALIASING", "INPUT DELAY", "TOUCH POINTER", "TOUCH MEGA SAVE", "MULTIPLAYER LINK", "LIVE SHADOWS", "PICTURE FORMAT", "NEARBY PAIRING"}) CHECK(NpSettings::OptionIndex(gone) < 0);
    CHECK(NpSettings::OptionCount() == 20);   // (19 in the game menu, CORE PLAN on SAFE START)
    CHECK(!NN::IsAllSettingsRow(0, 2) && !NN::IsAllSettingsRow(5, 5) && !NN::IsAllSettingsRow(-1, 0) && !NN::IsResetRow(4, 1) && NN::Option(9, 0) == -1);
    char buf[64];
    CHECK(NN::Label(0, 2, buf, sizeof buf) > 0 && std::strcmp(buf, frameDefault) == 0);
    CHECK(NN::Label(1, 0, buf, sizeof buf) > 0 && std::strcmp(buf, "CROWD: SHOW") == 0);
    CHECK(NN::Label(1, 2, buf, sizeof buf) > 0 && std::strcmp(buf, "BLUR: OFF") == 0);   // "OFF (DEFAULT)" cut at " (" (ship26)
    CHECK(NN::Label(2, 1, buf, sizeof buf) > 0 && std::strcmp(buf, "MEGA SAVE: FRONT TOUCH") == 0);
    CHECK(NN::Label(3, 2, buf, sizeof buf) > 0 && std::strcmp(buf, "GAME FIXES: DRAWING") == 0);   // the short value
    CHECK(NN::Label(4, 0, buf, sizeof buf) > 0 && std::strcmp(buf, "CONNECTION: AUTO") == 0);
    CHECK(NN::Label(4, 1, buf, sizeof buf) > 0 && std::strcmp(buf, "ONLINE ACCOUNT") == 0);
    CHECK(NN::Label(4, 2, buf, sizeof buf) > 0 && std::strcmp(buf, "RESET TO DEFAULTS") == 0);
    CHECK(NN::Label(9, 0, buf, sizeof buf) == 0 && buf[0] == 0);
    CHECK(NN::MoreLabel(0, buf, sizeof buf) > 0 && std::strcmp(buf, "MORE (1/5)") == 0);
    CHECK(NN::MoreLabel(4, buf, sizeof buf) > 0 && std::strcmp(buf, "MORE (5/5)") == 0);
    CHECK(NN::NextPage(0) == 1 && NN::NextPage(3) == 4 && NN::NextPage(4) == 0);
    // 1.3 / int34: the first page opens with the two unlock rows, both OFF by default (labels fit 24 characters in all three languages: the label loop below)
    CHECK(NN::Label(0, 0, buf, sizeof buf) > 0 && std::strcmp(buf, "ALL CHARACTERS: OFF") == 0);
    CHECK(NN::Label(0, 1, buf, sizeof buf) > 0 && std::strcmp(buf, "ALL STADIUMS: OFF") == 0);
    CHECK(NN::Label(0, 3, buf, sizeof buf) > 0 && std::strcmp(buf, "CONTROLS: MODERN") == 0);
    CHECK(NN::Label(0, 4, buf, sizeof buf) > 0 && std::strcmp(buf, "LANGUAGE: AUTOMATIC") == 0);
    CHECK(NN::DoneLabel(buf, sizeof buf) > 0 && std::strcmp(buf, "DONE") == 0);
    CHECK(NN::Label(0, 2, buf, 8) == 7 && std::strlen(buf) == 7);   // a small buffer is cut and terminated
    CHECK(NN::Help(0, 2, buf, sizeof buf) > 0 && std::strncmp(buf, "SMOOTH 60 aims for 60", 21) == 0);

    // ---- set18: the help line under the panel (np_settings_help.h): the row under the cursor's help, nothing over MORE / DONE / an empty bar / none, nothing while the
    // panel is not in place; the same text as Help, in the open language
    {
        char line[200], help[200];
        for (int p = 0; p < NN::Pages(); ++p)
            for (int bar = -1; bar < 7; ++bar) {
                const int n = NN::HelpLine(p, bar, true, line, sizeof line);
                if (bar >= 0 && bar < NN::Rows(p)) {
                    NN::Help(p, bar, help, sizeof help);
                    CHECK(n > 10 && std::strcmp(line, help) == 0);
                } else {
                    CHECK(n == 0 && line[0] == 0);
                }
                CHECK(NN::HelpLine(p, bar, false, line, sizeof line) == 0 && line[0] == 0);
            }
        CHECK(NN::HelpLine(1, 4, true, line, sizeof line) == 0);   // (PICTURE has four rows: bar 4 is empty)
        NN::Open(NpText::kFrench);
        NN::HelpLine(0, 2, true, line, sizeof line);
        NN::Open(NpText::kEnglish);
        NN::HelpLine(0, 2, true, help, sizeof help);
        CHECK(std::strcmp(line, help) != 0 && std::strstr(line, "FLUIDE 60"));
        NN::Close();
        CHECK(NN::HelpLine(0, 2, true, line, sizeof line) == 0);   // (no model open: nothing)
        NN::Open(NpText::kEnglish);
    }

    // ---- labels: at most 24 characters in all three languages, for every choice of every row (set directly on a state, as the rows would reach them)
    for (int lang = NpText::kEnglish; lang <= NpText::kFrench; ++lang) {
        WriteRaw("");
        CHECK(NN::Open(lang));
        for (int p = 0; p < NN::Pages(); ++p)
            for (int r = 0; r < NN::Rows(p); ++r) {
                char name[64], label[64], help[200];
                NN::Name(p, r, name, sizeof name);
                CHECK(CodePoints(name) <= NN::kMaxLabelChars);
                NN::Label(p, r, label, sizeof label);
                { if (CodePoints(label) > NN::kMaxLabelChars) std::fprintf(stderr, "LONG lang %d [%s]\n", lang, label); CHECK(CodePoints(label) <= NN::kMaxLabelChars); }
                CHECK(NN::Help(p, r, help, sizeof help) > 10 && CodePoints(help) <= 76);
                if (lang != NpText::kEnglish && NN::Option(p, r) >= 0) CHECK(std::strcmp(help, NpSettings::GetOption(NN::Option(p, r)).help[0]) != 0);   // the help is translated
            }
        // every choice of every row
        for (int p = 0; p < 5; ++p)
            for (int row = 0; row < 5; ++row) {
                if (!kOptions[p][row]) continue;
                const int opt = NpSettings::OptionIndex(kOptions[p][row]);
                CHECK(opt >= 0);
                if (opt < 0) continue;
                char label[64];
                for (int c = 0; c < NpSettings::ChoiceCount(opt) + 1; ++c) {   // the extra pass is CUSTOM, reached by a hand-written value
                    WriteRaw("");
                    if (c < NpSettings::ChoiceCount(opt)) {
                        NpSettings::State s;
                        NpSettings::Load(s, "");
                        NpSettings::SetChoice(s, opt, c);
                        std::string text;
                        NpSettings::Serialize(s, text);
                        WriteRaw(text);
                    } else {
                        // a key of the row with a value that is no choice
                        const NpSettings::Option& op = NpSettings::GetOption(opt);
                        const char* key = nullptr;
                        for (int k = 0; k < NpSettings::kMaxAssign; ++k)
                            if (op.choices[1].set[k].key) { key = op.choices[1].set[k].key; break; }
                        WriteRaw(std::string(key ? key : "MSC_X") + "=zzz\n");
                    }
                    CHECK(NN::Open(lang));
                    NN::Label(p, row, label, sizeof label);
                    { if (CodePoints(label) > NN::kMaxLabelChars) std::fprintf(stderr, "LONG lang %d [%s]\n", lang, label); CHECK(CodePoints(label) <= NN::kMaxLabelChars); }
                    CHECK(std::strlen(label) < static_cast<size_t>(NN::kMaxLabelBytes));
                    if (c < NpSettings::ChoiceCount(opt) && lang == NpText::kEnglish) CHECK(std::strstr(label, "CUSTOM") == nullptr);   // every choice is one the row names
                }
            }
        for (int b = 0; b < NN::kLeaveButtons; ++b) {
            NN::LeaveButtonLabel(b, buf, sizeof buf);
            CHECK(buf[0] != 0 && CodePoints(buf) <= 40);
        }
        CHECK(NN::MoreLabel(0, buf, sizeof buf) > 0 && CodePoints(buf) <= NN::kMaxLabelChars);
        CHECK(NN::DoneLabel(buf, sizeof buf) > 0 && CodePoints(buf) <= NN::kMaxLabelChars);
        char msg[512];
        CHECK(NN::LeaveMessage(msg, sizeof msg) > 20 && CodePoints(msg) < 480);
        CHECK(NN::SaveFailedMessage(msg, sizeof msg) > 10);
        if (lang != NpText::kEnglish) {   // a translation is not the English text
            char en[64], tr[64];
            for (int r = 0; r < NN::Rows(4); ++r) {
                NN::Open(NpText::kEnglish);
                NN::Name(4, r, en, sizeof en);
                NN::Open(lang);
                NN::Name(4, r, tr, sizeof tr);
                CHECK(std::strcmp(en, tr) != 0);
            }
        }
    }
    // Spanish and French values come through NpText (the choice labels of the table)
    WriteRaw("MSC_PERFORMANCE_MODE=1\n");
    NN::Open(NpText::kSpanish);
    NN::Label(1, 0, buf, sizeof buf);
    CHECK(std::strcmp(buf, "PÚBLICO: OCULTAR") == 0);
    NN::Open(NpText::kFrench);
    NN::Label(1, 0, buf, sizeof buf);
    CHECK(std::strcmp(buf, "PUBLIC: MASQUER") == 0);
    WriteRaw("MSC_FPS=30\n");
    NN::Open(NpText::kFrench);
    NN::Label(0, 2, buf, sizeof buf);
    CHECK(std::strcmp(buf, "IMAGES/S: 30 FIXE") == 0);

    WriteRaw("MSC_PERFORMANCE_MODE=zzz\n");
    NN::Open(NpText::kEnglish);
    NN::Label(1, 0, buf, sizeof buf);
    CHECK(std::strcmp(buf, "CROWD: CUSTOM") == 0);

    // ---- a press moves the value and Save writes what the pixel screen writes for the same steps
    const struct {
        const char* file;
        std::vector<Step> steps;
    } kCases[] = {
        {"", {{1, 0, 1}}},                                              // CROWD HIDE
        {"", {{0, 2, 1}}},                                              // FRAME RATE: the next mode
        {"", {{0, 2, 1}, {0, 2, -1}}},                                  // ... and back
        {"", {{0, 2, -1}}},                                             // FRAME RATE backwards: 30 LOCKED
        {"", {{0, 3, 1}, {0, 4, 1}, {0, 4, 1}}},                        // CONTROLS CLASSIC, LANGUAGE ENGLISH, SPANISH
        {"", {{2, 0, 1}, {3, 0, 1}, {1, 1, 1}}},                        // REAR TOUCH, LESS SHAKE, SHARPNESS
        {"", {{2, 1, 1}, {2, 1, 1}}},                                   // MEGA SAVE: REAR + CROSS, then STICK ONLY
        {"", {{3, 2, 1}, {2, 2, -1}, {4, 0, 1}}},                        // GAME FIXES ALL, SAVE SPEED 4x, RELAY ONLY
        {"MSC_FPS_BETWEEN=pace\n", {{0, 2, 1}}},                        // a retired UP TO 60 line (1.3 development file) reads as the default
        {"# my note\nFOO_BAR=1\nMSC_FPS=30\n", {{1, 0, 1}}},            // other lines kept
        {"MSC_LANGUAGE=fr\n", {{0, 4, 1}, {0, 4, -1}}},                 // a row stepped away and back
        {"MSC_LOCK60=1\nMSC_PICTURE_HOLD=0\n", {{0, 2, 1}}},            // SMOOTH 60 -> the next mode (the other line passes through)
        {"MSC_TOUCH_SAVE=0\n", {{2, 1, 1}}},                            // the old TOUCH MEGA SAVE OFF reads STICK ONLY: the next one is FRONT TOUCH
    };
    for (const auto& c : kCases) {
        const std::string want = Reference(c.file, c.steps.data(), static_cast<int>(c.steps.size()));
        const std::string got = Model(c.file, c.steps.data(), static_cast<int>(c.steps.size()));
        CHECK(got == want);
        CHECK(!want.empty());
        if (got != want) std::fprintf(stderr, "--- want\n%s--- got\n%s---\n", want.c_str(), got.c_str());
    }
    {   // the named effects
        const Step on[] = {{0, 2, 1}};
        const std::string t = Model("", on, 1);
        CHECK(t.find(smoothDefault ? "MSC_FPS=30" : "MSC_LOCK60=1") != std::string::npos);   // AUTO -> SMOOTH 60 (or, with SMOOTH 60 the default, SMOOTH 60 -> 30 LOCKED)
        const Step back[] = {{0, 2, -1}};
        CHECK(Model("", back, 1).find("MSC_FPS=30") != std::string::npos);                     // the last choice is 30 LOCKED either way
        const Step crowd[] = {{1, 0, 1}};
        const std::string c = Model("# my note\nFOO_BAR=1\n", crowd, 1);
        CHECK(c.find("FOO_BAR=1") != std::string::npos && c.find("# my note") != std::string::npos && c.find("MSC_PERFORMANCE_MODE=1") != std::string::npos);
        CHECK(c.compare(0, 1, "#") == 0);
        CHECK(c.find(NpBuildIdentity::Version()) != std::string::npos);   // the stamp
        const Step stick[] = {{2, 1, -1}};
        CHECK(Model("", stick, 1).find("MSC_TOUCH_SAVE=0") != std::string::npos);              // MEGA SAVE: STICK ONLY is the old TOUCH MEGA SAVE OFF
    }

    // ---- every choice of a row is reachable (set18: no hidden choices): FRAME RATE walks all its modes and comes back
    WriteRaw("");
    NN::Open(NpText::kEnglish);
    NN::Label(2, 0, buf, sizeof buf);
    CHECK(std::strcmp(buf, "REAR TOUCH: WITH MODERN") == 0);
    CHECK(NN::Press(2, 0, 1) == NN::kChanged);
    NN::Label(2, 0, buf, sizeof buf);
    CHECK(std::strcmp(buf, "REAR TOUCH: ON") == 0);
    {
        std::vector<std::string> seen;
        for (int i = 0; i < NpSettings::ChoiceCount(NpSettings::OptionIndex("FRAME RATE")); ++i) {
            NN::Label(0, 2, buf, sizeof buf);
            seen.push_back(buf);
            NN::Press(0, 2, 1);
        }
        NN::Label(0, 2, buf, sizeof buf);
        CHECK(std::strcmp(buf, frameDefault) == 0);
        CHECK(seen.size() == (smoothDefault ? 2u : 3u));
        bool smooth = false, locked = false;
        for (const std::string& v : seen) {
            smooth |= v == "FRAME RATE: SMOOTH 60";
            locked |= v == "FRAME RATE: 30 LOCKED";
            CHECK(v.find("45") == std::string::npos);   // (the 45 cadence as a mode of its own was rejected on 18 Nov)
        }
        CHECK(smooth && locked);
    }
    NN::Discard();

    // ---- set18: old np-player.txt files. Lines of removed rows are read without harm; a purely worse value that an earlier 1.3 screen wrote into its own block reads as the default,
    // and the next save drops it; the same value outside the block (the kit's, a player's own line) is kept and shows as CUSTOM where a row owns the key.
    {
        const std::string stamp = NpSettingsGuard::StampLine("1.3.0", "0123456789abcdef");
        const std::string block = std::string("# ---- Settings screen (LiveArea > Settings): the lines below are written by it; the rest of this file is kept ----\n");
        const std::string blockEnd = "# ---- end of the Settings screen's lines ----\n";
        const std::string old = stamp + "\n# kept\nMSC_VITA_RENDER=640x362\n" + block + "MSC_VITA_RENDER=864x488\nMSC_VGX_MSAA=0\nMSC_GAME_FIXES=none\nMSC_MEGA_POINTER_SPEED=1\n" +
                                "MSC_LAN_INPUT_DELAY=6\nMSC_LAN_MODE=wifi\nNP_TOUCH_POINTER=0\nMSC_TOUCH_SAVE=0\nMSC_LANGUAGE=fr\n" + blockEnd;
        WriteRaw(old);
        CHECK(NN::Open(NpText::kEnglish) && !NN::Changed());
        NN::Label(3, 2, buf, sizeof buf);
        CHECK(std::strcmp(buf, "GAME FIXES: DRAWING") == 0);   // "none" was only worse: read as the default
        NN::Label(2, 2, buf, sizeof buf);
        CHECK(std::strcmp(buf, "SAVE SPEED: 3x") == 0);        // ORIGINAL likewise
        NN::Label(2, 1, buf, sizeof buf);
        CHECK(std::strcmp(buf, "MEGA SAVE: STICK ONLY") == 0);   // the merged row reads the old TOUCH MEGA SAVE OFF
        NN::Label(0, 4, buf, sizeof buf);
        CHECK(std::strcmp(buf, "LANGUAGE: FRENCH") == 0);
        CHECK(NN::Press(1, 0, 1) == NN::kChanged && NN::Save());
        const std::string saved = ReadRaw();
        for (const char* gone : {"MSC_VITA_RENDER=864x488", "MSC_VGX_MSAA=0", "MSC_GAME_FIXES=none", "MSC_MEGA_POINTER_SPEED=1", "MSC_LAN_INPUT_DELAY=6", "MSC_LAN_MODE=wifi", "NP_TOUCH_POINTER=0"})
            CHECK(saved.find(gone) == std::string::npos);
        for (const char* kept : {"MSC_VITA_RENDER=640x362", "# kept", "MSC_TOUCH_SAVE=0", "MSC_LANGUAGE=fr", "MSC_PERFORMANCE_MODE=1"}) CHECK(saved.find(kept) != std::string::npos);
        // the same lines outside the block: kept as written (a row that owns the key shows CUSTOM)
        WriteRaw("MSC_GAME_FIXES=none\nMSC_LAN_INPUT_DELAY=6\n");
        NN::Open(NpText::kEnglish);
        NN::Label(3, 2, buf, sizeof buf);
        CHECK(std::strcmp(buf, "GAME FIXES: CUSTOM") == 0);
        NN::Press(1, 0, 1);
        CHECK(NN::Save() && ReadRaw().find("MSC_GAME_FIXES=none") != std::string::npos && ReadRaw().find("MSC_LAN_INPUT_DELAY=6") != std::string::npos);   // (a CUSTOM row's line and a retired line outside the block stay)
    }

    // ---- set18: RESET TO DEFAULTS. Every row back to its default, a change even when nothing moved (the save drops retired lines wherever they stand), Discard undoes it
    {
        WriteRaw("MSC_LAN_INPUT_DELAY=6\nMSC_PERFORMANCE_MODE=1\nFOO_BAR=1\n");
        NN::Open(NpText::kEnglish);
        CHECK(!NN::Changed());
        CHECK(NN::Press(4, 2, 1) == NN::kChanged && NN::Changed());
        NN::Label(1, 0, buf, sizeof buf);
        CHECK(std::strcmp(buf, "CROWD: SHOW") == 0);
        NN::Discard();
        CHECK(!NN::Changed());
        NN::Label(1, 0, buf, sizeof buf);
        CHECK(std::strcmp(buf, "CROWD: HIDE") == 0);
        WriteRaw("MSC_LAN_INPUT_DELAY=6\nFOO_BAR=1\n");
        NN::Open(NpText::kEnglish);
        CHECK(NN::Press(4, 2, 0) == NN::kChanged && NN::Changed() && !NN::NextStartChanged());   // every row already at its default: still a change to save
        CHECK(NN::Apply(NN::kActSave) == NN::kLeft && !NN::Changed());
        CHECK(ReadRaw().find("MSC_LAN_INPUT_DELAY") == std::string::npos && ReadRaw().find("FOO_BAR=1") != std::string::npos);
    }

    // ---- state after a press / save
    WriteRaw("");
    NN::Open(NpText::kEnglish);
    CHECK(!NN::Changed() && !NN::RestartNeeded());
    CHECK(NN::Press(1, 0, 1) == NN::kChanged);
    CHECK(NN::Changed() && !NN::RestartNeeded() && !NN::NextStartChanged());   // CROWD is a live row (1.3): saved, it applies now; nothing waits for the next start
    NN::Label(1, 0, buf, sizeof buf);
    CHECK(std::strcmp(buf, "CROWD: HIDE") == 0);
    CHECK(NN::Press(1, 0, -1) == NN::kChanged && !NN::Changed());   // back to what the file held
    CHECK(NN::Press(1, 0, 0) == NN::kChanged && NN::Changed());     // 0 = next
    CHECK(NN::Press(1, 0, 1) == NN::kChanged && !NN::Changed());    // wraps (two choices)
    CHECK(NN::Press(1, 0, 1) == NN::kChanged);
    CHECK(ReadRaw().empty());                                       // nothing is written before Save
    CHECK(NN::Save() && !NN::Changed());
    CHECK(ReadRaw().find("MSC_PERFORMANCE_MODE=1") != std::string::npos);
    CHECK(NN::Press(4, 1, 1) == NN::kAllSettings && !NN::Changed());
    CHECK(NN::Press(7, 0, 1) == NN::kNothing && NN::Press(0, 9, 1) == NN::kNothing && NN::Press(-1, 0, 1) == NN::kNothing && NN::Press(4, 3, 1) == NN::kNothing);
    // Discard drops steps and writes nothing
    NN::Press(1, 1, 1);
    CHECK(NN::Changed());
    const std::string before = ReadRaw();
    NN::Discard();
    CHECK(!NN::Changed() && ReadRaw() == before);
    NN::Label(1, 1, buf, sizeof buf);
    CHECK(std::strcmp(buf, "SHARPNESS: SMOOTH") == 0);
    // Reload sees a file changed meanwhile (the ONLINE page saved) and drops steps
    NN::Press(1, 1, 1);
    WriteRaw("MSC_LANGUAGE=es\n");
    CHECK(NN::Reload() && !NN::Changed());
    NN::Label(0, 4, buf, sizeof buf);
    CHECK(std::strcmp(buf, "LANGUAGE: SPANISH") == 0);
    NN::Label(1, 1, buf, sizeof buf);
    CHECK(std::strcmp(buf, "SHARPNESS: SMOOTH") == 0);

    // ---- grey rows: set18, no row depends on another any more (SMOOTH 60 and 30 FPS are one row, the Mega Save aim is one row): none is grey, whatever the file holds
    for (const char* file : {"", "MSC_FPS=30\n", "MSC_LOCK60=1\nMSC_FPS=30\n", "MSC_TOUCH_SAVE=0\nMSC_MEGA_AIM=rear\n", "MSC_VITA_RENDER=640x362\n"}) {
        WriteRaw(file);
        NN::Open(NpText::kEnglish);
        for (int p = 0; p < NN::Pages(); ++p)
            for (int r = 0; r < NN::Rows(p); ++r) CHECK(!NN::Dim(p, r) && NN::Why(p, r) == nullptr);
    }

    // ---- a failed save leaves the screen as it was; Restart saves first and asks the platform
    WriteRaw("");
    NN::Open(NpText::kEnglish);
    NN::Press(1, 0, 1);
    setenv("MSC_SETTINGS_FILE", "/nonexistent-dir/np-player.txt", 1);
    CHECK(!NN::Save() && NN::Changed());
    CHECK(!NN::Restart() && NN::Changed());
    setenv("MSC_SETTINGS_FILE", g_path.c_str(), 1);
    CHECK(NN::Restart() && !NN::Changed());
    CHECK(ReadRaw().find("MSC_PERFORMANCE_MODE=1") != std::string::npos);
    CHECK(NN::OpenAll());
    // no backend answer (no file named): not offered
    unsetenv("MSC_SETTINGS_FILE");
    CHECK(!NN::Open(NpText::kEnglish) && !NN::IsOpen());
    CHECK(NN::Press(1, 0, 1) == NN::kNothing && !NN::Changed() && !NN::Save());
    setenv("MSC_SETTINGS_FILE", g_path.c_str(), 1);
    NN::Open(NpText::kEnglish);
    NN::Close();
    CHECK(!NN::IsOpen() && NN::Press(1, 0, 1) == NN::kNothing);

    // ---- 1.3 live rows: the popups by place, the note, and Save applying the live rows
    {
        WriteRaw("");
        NN::Open(NpText::kEnglish);
        CHECK(NN::Press(1, 0, 1) == NN::kChanged);                 // CROWD: HIDE (live)
        char note[200], l0[64], l1[64], l2[64];
        CHECK(NN::LeaveNote(true, note, sizeof note) == 0 && !note[0]);   // only live rows changed: no note
        CHECK(NN::LeaveCount(true) == 3 && NN::LeaveCount(false) == 3);
        NN::LeaveLabel(0, true, l0, sizeof l0); NN::LeaveLabel(1, true, l1, sizeof l1); NN::LeaveLabel(2, true, l2, sizeof l2);
        CHECK(!std::strcmp(l0, "SAVE") && !std::strcmp(l1, "DON'T SAVE") && !std::strcmp(l2, "KEEP EDITING"));
        CHECK(NN::LeaveAction(0, true) == NN::kActSave && NN::LeaveAction(1, true) == NN::kActDiscard && NN::LeaveAction(2, true) == NN::kActKeep);
        NN::LeaveLabel(0, false, l0, sizeof l0);                  // front-end entry, only a live row changed: the match list
        CHECK(!std::strcmp(l0, "SAVE") && NN::LeaveAction(0, false) == NN::kActSave && NN::LeaveNote(false, note, sizeof note) == 0);
        CHECK(NN::Press(0, 2, -1) == NN::kChanged);                 // FRAME RATE: 30 LOCKED (next start)
        CHECK(NN::NextStartChanged() && NN::RestartNeeded());
        CHECK(NN::LeaveNote(true, note, sizeof note) > 0 && std::strstr(note, "next time the game starts") && !std::strstr(note, "RESTART"));
        for (int i = 0; i < 3; ++i) CHECK(NN::LeaveAction(i, true) != NN::kActSaveRestart);   // never a restart in a match
        CHECK(NN::LeaveAction(0, false) == NN::kActSaveRestart && NN::LeaveAction(1, false) == NN::kActDiscard && NN::LeaveAction(2, false) == NN::kActKeep);
        NN::LeaveLabel(0, false, l0, sizeof l0);
        CHECK(!std::strcmp(l0, "SAVE AND START AGAIN") && NN::LeaveNote(false, note, sizeof note) > 0);
        for (int lang = 0; lang < 3; ++lang) {                     // EN, ES, FR: every label and note within the row's width, and different from English where translated
            NN::Open(lang);
            NN::Press(1, 0, 1);
            NN::Press(0, 2, -1);
            for (int m = 0; m < 2; ++m)
                for (int i = 0; i < 3; ++i) {
                    NN::LeaveLabel(i, m != 0, l0, sizeof l0);
                    CHECK(l0[0] && CodePoints(l0) <= NN::kMaxLabelChars);
                }
            NN::LeaveLabel(1, true, l0, sizeof l0);
            CHECK(std::strcmp(l0, lang == NpText::kSpanish ? "NO GUARDAR" : lang == NpText::kFrench ? "NE PAS ENREGISTRER" : "DON'T SAVE") == 0);
            NN::LeaveNote(true, note, sizeof note);
            CHECK(note[0] && CodePoints(note) <= 61);
            NN::LeaveNote(false, note, sizeof note);
            CHECK(note[0] && CodePoints(note) <= 61);
        }
        // ---- ux13 (U2): the restart question after SAVE in a match (MSC_PAUSE_RESTART, default off)
        NN::Open(NpText::kEnglish);
        NN::Press(0, 2, -1);                                        // FRAME RATE (next start)
        unsetenv("MSC_PAUSE_RESTART");
        CHECK(!NN::PauseRestartOffered(true) && !NN::PauseRestartOffered(false));   // off by default
        setenv("MSC_PAUSE_RESTART", "0", 1);
        CHECK(!NN::PauseRestartOffered(true));
        setenv("MSC_PAUSE_RESTART", "1", 1);
        CHECK(NN::PauseRestartOffered(true) && !NN::PauseRestartOffered(false));    // only in a running match
        NN::Discard();
        NN::Press(1, 0, 1);                                        // only a live row (CROWD)
        CHECK(!NN::PauseRestartOffered(true));                     // nothing waits for the next start: no question
        NN::Press(0, 2, -1);
        CHECK(NN::PauseRestartOffered(true));
        CHECK(NN::Apply(NN::LeaveAction(0, true)) == NN::kLeft && !NN::Changed());  // SAVE as before; the question is the screen's
        CHECK(!NN::PauseRestartOffered(true));                     // saved: nothing left to ask about
        for (int lang = 0; lang < 3; ++lang) {
            NN::Open(lang);
            char a[400], b0[64], b1[64];
            CHECK(NN::RestartAskMessage(a, sizeof a) > 40 && CodePoints(a) < 300);
            CHECK(NN::RestartAskLabel(0, b0, sizeof b0) > 0 && NN::RestartAskLabel(1, b1, sizeof b1) > 0);
            CHECK(CodePoints(b0) <= NN::kMaxLabelChars && CodePoints(b1) <= NN::kMaxLabelChars && std::strcmp(b0, b1) != 0);
            if (lang == NpText::kEnglish) CHECK(std::strstr(a, "progress since your last save is lost") && !std::strcmp(b1, "START AGAIN"));
            else CHECK(!std::strstr(a, "Start the game again"));                // a translation, not the English text
        }
        unsetenv("MSC_PAUSE_RESTART");
        NN::Open(NpText::kEnglish);
        NN::Discard();
        CHECK(!NN::Changed());
    }

    // ---- 1.3 front19: doing what a leave button says (Apply), for a match and for the OPTIONS entry
    {
        std::remove(g_path.c_str());
        WriteRaw("");
        const int r0 = NpNativeSettingsHostRestarts();
        // OPTIONS, a next-start row changed (FRAME RATE 30): SAVE AND START AGAIN writes the file, asks for ONE restart, and the screen may be left
        NN::Open(NpText::kEnglish);
        CHECK(NN::Press(0, 2, -1) == NN::kChanged && NN::NextStartChanged());
        CHECK(NN::LeaveAction(0, false) == NN::kActSaveRestart);
        CHECK(NN::Apply(NN::kActKeep) == NN::kStay && NN::Changed() && NpNativeSettingsHostRestarts() == r0);   // KEEP EDITING: nothing written, nothing asked
        CHECK(ReadRaw().find("MSC_FPS=30") == std::string::npos);
        CHECK(NN::Apply(NN::kActDiscard) == NN::kLeft && !NN::Changed() && NpNativeSettingsHostRestarts() == r0);   // DON'T SAVE: nothing written, nothing asked
        CHECK(ReadRaw().find("MSC_FPS=30") == std::string::npos);
        NN::Press(0, 2, -1);
        CHECK(NN::Apply(NN::kActSaveRestart) == NN::kLeft && !NN::Changed() && NpNativeSettingsHostRestarts() == r0 + 1);
        CHECK(ReadRaw().find("MSC_FPS=30") != std::string::npos);
        // a match (or OPTIONS with only a live row changed): SAVE writes and asks for no restart
        WriteRaw("");
        NN::Open(NpText::kEnglish);
        NN::Press(1, 0, 1);                                        // CROWD (live)
        CHECK(NN::LeaveAction(0, false) == NN::kActSave);
        CHECK(NN::Apply(NN::kActSave) == NN::kLeft && !NN::Changed() && NpNativeSettingsHostRestarts() == r0 + 1);
        CHECK(ReadRaw().find("MSC_PERFORMANCE_MODE=1") != std::string::npos);
        // a failed write: SAVE and SAVE AND START AGAIN both report it, keep the steps, ask for no restart
        NN::Open(NpText::kEnglish);
        NN::Press(0, 2, -1);
        setenv("MSC_SETTINGS_FILE", "/nonexistent-dir/np-player.txt", 1);
        CHECK(NN::Apply(NN::kActSave) == NN::kOutSaveFailed && NN::Changed());
        CHECK(NN::Apply(NN::kActSaveRestart) == NN::kOutSaveFailed && NN::Changed() && NpNativeSettingsHostRestarts() == r0 + 1);
        setenv("MSC_SETTINGS_FILE", g_path.c_str(), 1);
        NN::Open(NpText::kEnglish);
    }

    // ---- popup texts
    NN::Open(NpText::kEnglish);
    char msg[512];
    NN::LeaveMessage(msg, sizeof msg);
    CHECK(std::strstr(msg, "next time the game starts") && std::strstr(msg, "SAVE AND RESTART"));
    NN::LeaveButtonLabel(NN::kLeaveSaveNext, buf, sizeof buf);
    CHECK(std::strcmp(buf, "SAVE FOR NEXT START") == 0);
    NN::LeaveButtonLabel(NN::kLeaveSaveRestart, buf, sizeof buf);
    CHECK(std::strcmp(buf, "SAVE AND RESTART") == 0);
    NN::LeaveButtonLabel(NN::kLeaveDiscard, buf, sizeof buf);
    CHECK(std::strcmp(buf, "DISCARD") == 0);

    std::remove(g_path.c_str());
    rmdir(dir);
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
