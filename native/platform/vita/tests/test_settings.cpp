// Tests of the Settings screen's model (np_settings.cpp): choices <-> np-player.txt lines, presets, round trips, the file's other lines kept, and the
// drawing (screenshots as PPM when a directory is given: test_settings <outdir>).
//   make -C platform/vita setcheck
#include "np_settings.h"

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

static int Find(const char* title) {
    for (int o = 0; o < OptionCount(); ++o)
        if (!std::strcmp(GetOption(o).title, title)) return o;
    return -1;
}
static int PresetIndex(const char* name) {
    for (int p = 0; p < PresetCount(); ++p)
        if (!std::strcmp(GetPreset(p).name, name)) return p;
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

int main(int argc, char** argv) {
    // no file: everything is the build's default; that is the DEFAULT preset; nothing is written
    {
        State s;
        Load(s, nullptr);
        for (int o = 0; o < OptionCount(); ++o) CHECK(s.choice[o] == GetOption(o).defaultChoice);
        CHECK(MatchPreset(s) == PresetIndex("DEFAULT"));
        std::string out;
        CHECK(Serialize(s, out) && out.empty());
    }
    // presets write their lines, read back as the same preset, and the default preset removes them again
    {
        State s;
        Load(s, "# my file\nMSC_WATCH=1\n");
        ApplyPreset(s, PresetIndex("QUALITY"));
        std::string out;
        CHECK(Serialize(s, out));
        CHECK(Has(out, "# my file") && Has(out, "MSC_WATCH=1") && Has(out, "MSC_VITA_RENDER=960x544"));
        State t;
        Load(t, out.c_str());
        CHECK(MatchPreset(t) == PresetIndex("QUALITY") && t.choice[Find("RESOLUTION")] == 1);
        ApplyPreset(t, PresetIndex("PERFORMANCE"));
        CHECK(Serialize(t, out));
        CHECK(Has(out, "MSC_VITA_RENDER=640x362") && Has(out, "MSC_PERFORMANCE_MODE=1") && !Has(out, "MSC_VITA_RENDER=960x544"));
        CHECK(CountLines(out, "MSC_VITA_RENDER=") == 1 && Has(out, "MSC_WATCH=1"));
        State u;
        Load(u, out.c_str());
        CHECK(MatchPreset(u) == PresetIndex("PERFORMANCE"));
        // saving again without a change gives the same text (idempotent)
        std::string again;
        CHECK(Serialize(u, again) && again == out);
        ApplyPreset(u, PresetIndex("DEFAULT"));
        CHECK(Serialize(u, out));
        CHECK(!Has(out, "MSC_PERFORMANCE_MODE=1") && CountLines(out, "MSC_VITA_RENDER") == 0 && Has(out, "MSC_WATCH=1") && CountLines(out, "# ---- ") == 0);
    }
    // W5 FPS: FRAME RATE is 30 (the build's default, writes nothing) or AUTO (MSC_FPS=auto); PERFORMANCE sets AUTO, DEFAULT and QUALITY keep 30; 60 / unlocked by hand read CUSTOM
    {
        const int fr = Find("FRAME RATE");
        CHECK(fr >= 0 && ChoiceCount(fr) == 2 && GetOption(fr).defaultChoice == 0 && GetOption(fr).inPreset);
        State s;
        Load(s, nullptr);
        CHECK(s.choice[fr] == 0);
        std::string out;
        CHECK(Serialize(s, out) && !Has(out, "MSC_FPS=auto") && CountLines(out, "MSC_FPS") == 0);
        Step(s, fr, +1);
        CHECK(s.choice[fr] == 1 && MatchPreset(s) == -1);
        CHECK(Serialize(s, out) && Has(out, "MSC_FPS=auto") && CountLines(out, "MSC_FPS=") == 1);
        State t;
        Load(t, out.c_str());
        CHECK(t.choice[fr] == 1);
        Step(t, fr, +1);                       // wraps back to 30: the line goes
        CHECK(t.choice[fr] == 0 && Serialize(t, out) && CountLines(out, "MSC_FPS") == 0);
        State p;
        Load(p, nullptr);
        ApplyPreset(p, PresetIndex("PERFORMANCE"));
        CHECK(p.choice[fr] == 1 && Serialize(p, out) && Has(out, "MSC_FPS=auto"));
        State q;
        Load(q, out.c_str());
        CHECK(MatchPreset(q) == PresetIndex("PERFORMANCE"));
        ApplyPreset(q, PresetIndex("QUALITY"));
        CHECK(q.choice[fr] == 0 && Serialize(q, out) && CountLines(out, "MSC_FPS") == 0);
        State c;
        Load(c, "MSC_FPS=60\n");              // a tester's own value: CUSTOM, kept until the row is changed
        CHECK(c.choice[fr] == -1 && std::strstr(c.custom[fr], "MSC_FPS=60") != nullptr);
        CHECK(Serialize(c, out) && Has(out, "MSC_FPS=60"));   // saving without touching the row keeps the line
        State d;
        Load(d, "MSC_FPS=unlocked\nMSC_WATCH=1\n");
        SetChoice(d, fr, 0);
        CHECK(Serialize(d, out) && CountLines(out, "MSC_FPS") == 0 && Has(out, "MSC_WATCH=1"));
    }
    // a change of one preset option makes the preset read CUSTOM; stepping wraps in both directions
    {
        State s;
        Load(s, nullptr);
        const int perf = Find("PERFORMANCE MODE"), pace = Find("FRAME PACING");
        Step(s, perf, +1);
        CHECK(s.choice[perf] == 1 && MatchPreset(s) == -1);
        Step(s, perf, +1);
        CHECK(s.choice[perf] == 0 && MatchPreset(s) == PresetIndex("DEFAULT"));
        Step(s, pace, -1);
        CHECK(s.choice[pace] == 1);
        std::string out;
        CHECK(Serialize(s, out) && Has(out, "MSC_FRAME_PACING=0"));
        const int speed = Find("POINTER SPEED");
        CHECK(s.choice[speed] == 2);
        Step(s, speed, -1);
        Step(s, speed, -1);
        Step(s, speed, -1);
        CHECK(s.choice[speed] == 4);   // wrapped to the last
    }
    // values that are none of the choices: shown as CUSTOM, left alone until the option is changed
    {
        const char* file = "MSC_VITA_RENDER=800x450\nNP_POINTER_SPEED=3\nMSC_LAN_MODE=wifi\nMSC_GAME_FIXES=1\nNP_DATA_DIR=ux0:data/mscharged\n";
        State s;
        Load(s, file);
        CHECK(s.choice[Find("RESOLUTION")] == -1 && std::strstr(s.custom[Find("RESOLUTION")], "MSC_VITA_RENDER=800x450"));
        CHECK(s.choice[Find("POINTER SPEED")] == -1);
        CHECK(s.choice[Find("MULTIPLAYER LINK")] == 1);
        CHECK(s.choice[Find("ORIGINAL GAME FIXES")] == -1);   // "1" is a bit mask: not one of the named choices
        CHECK(MatchPreset(s) == -1);
        std::string out;
        CHECK(Serialize(s, out));
        CHECK(Has(out, "MSC_VITA_RENDER=800x450") && Has(out, "NP_POINTER_SPEED=3") && Has(out, "MSC_GAME_FIXES=1") && Has(out, "NP_DATA_DIR=ux0:data/mscharged"));
        CHECK(Has(out, "MSC_LAN_MODE=wifi"));   // the choice, written again in the block
        Step(s, Find("RESOLUTION"), +1);        // from CUSTOM: the first choice
        CHECK(s.choice[Find("RESOLUTION")] == 0);
        CHECK(Serialize(s, out) && !Has(out, "MSC_VITA_RENDER=800x450") && Has(out, "NP_POINTER_SPEED=3"));
        ResetAll(s);
        CHECK(Serialize(s, out));
        CHECK(!Has(out, "NP_POINTER_SPEED=3") && !Has(out, "MSC_LAN_MODE=wifi") && Has(out, "NP_DATA_DIR=ux0:data/mscharged"));
    }
    // wave 5 NET: the ad hoc roles of MULTIPLAYER LINK write MSC_LAN_DIALOG, and read back as the same choice
    {
        const int link = Find("MULTIPLAYER LINK");
        State s;
        Load(s, nullptr);
        SetChoice(s, link, 2);
        std::string out;
        CHECK(Serialize(s, out) && Has(out, "MSC_LAN_DIALOG=create") && CountLines(out, "MSC_LAN_MODE") == 0);
        State r;
        Load(r, out.c_str());
        CHECK(r.choice[link] == 2);
        SetChoice(r, link, 3);
        CHECK(Serialize(r, out) && Has(out, "MSC_LAN_DIALOG=join") && !Has(out, "MSC_LAN_DIALOG=create"));
        State j;
        Load(j, out.c_str());
        CHECK(j.choice[link] == 3);
        SetChoice(j, link, 0);
        CHECK(Serialize(j, out) && CountLines(out, "MSC_LAN_DIALOG") == 0);
    }
    // two lines for one key: the last one decides what the screen shows; saving removes both when the option is set
    {
        State s;
        Load(s, "MSC_PERFORMANCE_MODE=0\nMSC_PERFORMANCE_MODE=1\n");
        CHECK(s.choice[Find("PERFORMANCE MODE")] == 1);
        SetChoice(s, Find("PERFORMANCE MODE"), 0);
        std::string out;
        CHECK(Serialize(s, out) && CountLines(out, "MSC_PERFORMANCE_MODE") == 0);
    }
    // the two-key choice (sharpness), CRLF files, and the size limit
    {
        State s;
        Load(s, "MSC_VITA_UPSCALE=sharp+\r\nMSC_VITA_SHARPEN=0.35\r\n# end\r\n");
        CHECK(s.choice[Find("SHARPNESS")] == 2);
        std::string out;
        CHECK(Serialize(s, out) && Has(out, "# end") && Has(out, "MSC_VITA_UPSCALE=sharp+") && Has(out, "MSC_VITA_SHARPEN=0.35"));
        State t;
        Load(t, "MSC_VITA_UPSCALE=sharp+\n");   // only one of the two keys: custom
        CHECK(t.choice[Find("SHARPNESS")] == -1);
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
        const char* touch[3] = {"TOUCH MEGA SAVE", "TOUCH MEGA STRIKE", "TOUCH SHAKEDOWN"};
        const char* keys[3] = {"MSC_TOUCH_SAVE=0", "MSC_TOUCH_METER=0", "MSC_TOUCH_SHAKE=0"};
        for (int i = 0; i < 3; ++i) {
            CHECK(Find(touch[i]) >= 0);
            SetChoice(s, Find(touch[i]), 1);
        }
        std::string out;
        CHECK(Serialize(s, out));
        CHECK(Has(out, "MSC_CONTROLS=classic") && Has(out, "MSC_REAR_TOUCH=0"));
        for (int i = 0; i < 3; ++i) CHECK(Has(out, keys[i]));
        State t;
        Load(t, out.c_str());
        CHECK(t.choice[controls] == 1 && t.choice[rear] == 2);
        for (int i = 0; i < 3; ++i) CHECK(t.choice[Find(touch[i])] == 1);
        SetChoice(t, rear, 1);
        CHECK(Serialize(t, out) && Has(out, "MSC_REAR_TOUCH=1") && CountLines(out, "MSC_REAR_TOUCH=") == 1);
        ResetAll(t);
        CHECK(Serialize(t, out) && out.empty());
        // the list scrolls: the selected row is always drawn
        for (int r = 0; r < RowCount(); ++r) {
            const int first = FirstVisibleRow(r);
            CHECK(first >= 0 && r >= first && r < first + 14 && first + 14 <= (RowCount() > 14 ? RowCount() : 14));
        }
        CHECK(FirstVisibleRow(0) == 0);
    }
    // W5 CAM: BONUS CAPTAINS (ON = the Vita default, writes nothing; OFF writes MSC_UNLOCK_CAPTAINS=0) and BONUS STADIUMS (OFF default; ON writes
    // MSC_UNLOCK_STADIUMS=1); both read back, a file's other lines are kept, reset removes them
    {
        State s;
        Load(s, "# mine\nMSC_WATCH=1\n");
        const int cap = Find("BONUS CAPTAINS"), stad = Find("BONUS STADIUMS");
        CHECK(cap >= 0 && stad >= 0 && OptionIndex("BONUS CAPTAINS") == cap);
        CHECK(s.choice[cap] == 0 && s.choice[stad] == 0);
        CHECK(!std::strcmp(GetOption(cap).choices[0].label, "ON") && !std::strcmp(GetOption(stad).choices[0].label, "OFF"));
        std::string out;
        CHECK(Serialize(s, out) && !Has(out, "MSC_UNLOCK_CAPTAINS=0") && CountLines(out, "MSC_UNLOCK_") == 0);   // defaults write nothing
        SetChoice(s, cap, 1);
        SetChoice(s, stad, 1);
        CHECK(Serialize(s, out) && Has(out, "MSC_UNLOCK_CAPTAINS=0") && Has(out, "MSC_UNLOCK_STADIUMS=1") && Has(out, "MSC_WATCH=1") && Has(out, "# mine"));
        CHECK(CountLines(out, "MSC_UNLOCK_CAPTAINS=") == 1 && CountLines(out, "MSC_UNLOCK_STADIUMS=") == 1);
        State t;
        Load(t, out.c_str());
        CHECK(t.choice[cap] == 1 && t.choice[stad] == 1);
        Step(t, cap, +1);                                  // OFF -> ON (wraps): the line goes away
        CHECK(t.choice[cap] == 0 && Serialize(t, out) && CountLines(out, "MSC_UNLOCK_CAPTAINS=") == 0 && Has(out, "MSC_UNLOCK_STADIUMS=1"));
        State u;
        Load(u, "MSC_UNLOCK_CAPTAINS=1\nMSC_UNLOCK_STADIUMS=0\n");   // hand-written values that are the defaults' meaning: shown as CUSTOM, kept
        CHECK(u.choice[cap] == -1 && u.choice[stad] == -1);
        CHECK(Serialize(u, out) && Has(out, "MSC_UNLOCK_CAPTAINS=1") && Has(out, "MSC_UNLOCK_STADIUMS=0"));
        ResetAll(t);
        CHECK(Serialize(t, out) && CountLines(out, "MSC_UNLOCK_") == 0);
        CHECK(MatchPreset(s) >= -1);   // the rows are in no preset
        for (int p = 0; p < PresetCount(); ++p)
            for (int i = 0; i < kMaxOptions && GetPreset(p).items[i].option; ++i)
                CHECK(std::strcmp(GetPreset(p).items[i].option, "BONUS CAPTAINS") && std::strcmp(GetPreset(p).items[i].option, "BONUS STADIUMS"));
    }
    // every option and choice is drawn within the buffer, every label fits its column, every row reachable
    {
        std::vector<uint32_t> buffer(kWidth * kHeight + 4096, 0xDEADBEEFu);
        State s;
        Load(s, nullptr);
        for (int o = 0; o < OptionCount(); ++o) {
            CHECK(std::strlen(GetOption(o).title) <= 22);
            CHECK(std::strlen(GetOption(o).help[0]) <= 76 && std::strlen(GetOption(o).help[1]) <= 76);
            for (int c = 0; c < ChoiceCount(o); ++c) CHECK(std::strlen(GetOption(o).choices[c].label) <= 24);
        }
        for (int r = 0; r < RowCount(); ++r) Draw(buffer.data(), s, r, r == 3 ? "Could not write the file" : "", r == 3);
        {
            State c;
            Load(c, "MSC_CONTROLS=classic\nMSC_REAR_TOUCH=0\nMSC_TOUCH_SHAKE=0\n");
            DrawControls(buffer.data(), c);
            DrawControls(buffer.data(), s);
        }
        for (size_t i = kWidth * kHeight; i < buffer.size(); ++i) CHECK(buffer[i] == 0xDEADBEEFu);
        CHECK(RowCount() == OptionCount() + 4 && GetRow(0).kind == kRowPreset && GetRow(RowCount() - 1).kind == kRowSaveExit);
        if (argc > 1) {
            char path[512];
            State q;
            Load(q, nullptr);
            Draw(buffer.data(), q, 0, "", false);
            std::snprintf(path, sizeof path, "%s/settings-default.ppm", argv[1]);
            CHECK(Ppm(path, buffer.data()));
            // W5 CTL: the list scrolled to its controls rows, and the controls page of both layouts
            Draw(buffer.data(), q, Find("CONTROLS") + 1, "", false);
            std::snprintf(path, sizeof path, "%s/settings-controls-rows.ppm", argv[1]);
            CHECK(Ppm(path, buffer.data()));
            // W5 CAM: the list scrolled to the two bonus rows (BONUS CAPTAINS selected), defaults, and with both changed
            Draw(buffer.data(), q, Find("BONUS CAPTAINS") + 1, "", false);
            std::snprintf(path, sizeof path, "%s/settings-bonus-rows.ppm", argv[1]);
            CHECK(Ppm(path, buffer.data()));
            {
                State b;
                Load(b, "MSC_UNLOCK_CAPTAINS=0\nMSC_UNLOCK_STADIUMS=1\n");
                Draw(buffer.data(), b, Find("BONUS STADIUMS") + 1, "", false);
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
            ApplyPreset(q, PresetIndex("QUALITY"));
            SetChoice(q, Find("MULTIPLAYER LINK"), 1);
            SetChoice(q, Find("ORIGINAL GAME FIXES"), 2);
            Draw(buffer.data(), q, Find("ORIGINAL GAME FIXES") + 1, "", false);
            std::snprintf(path, sizeof path, "%s/settings-quality.ppm", argv[1]);
            CHECK(Ppm(path, buffer.data()));
            State c;
            Load(c, "MSC_VITA_RENDER=800x450\n");
            SetChoice(c, Find("PERFORMANCE MODE"), 1);
            Draw(buffer.data(), c, RowCount() - 2, "Saved to ux0:data/mscharged/np-player.txt", false);
            std::snprintf(path, sizeof path, "%s/settings-custom.ppm", argv[1]);
            CHECK(Ppm(path, buffer.data()));
        }
    }
    std::printf("test_settings: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
