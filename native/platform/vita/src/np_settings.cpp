// The model and the drawing of the Settings screen: see np_settings.h.
#include "np_settings.h"

#include "vita_font.h"

#include "../../sdk/input/np_control_table.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace NpSettings {
namespace {

// ---- the options --------------------------------------------------------------------------------------------------------------------------------------------
// A choice with an empty list writes nothing: it is the build's own behaviour. The "inPreset" options are what the three presets set; the picture and speed
// workstreams (wave 5: LOOK, PIPE, GLB, GXD) add their keys here and to the presets as they land (their NOTES files name them).
constexpr Option kTable[] = {
    {"RESOLUTION",
     {"The size the 3D picture is drawn at; the screen itself is 960x544.", "Bigger is sharper and costs GPU time. STANDARD is the game's own size."},
     0, true,
     {{"STANDARD", {}},
      {"SCREEN 960X544", {{"MSC_VITA_RENDER", "960x544"}}},
      {"SMALLER 640X362", {{"MSC_VITA_RENDER", "640x362"}}},
      {"SMALLEST 480X272", {{"MSC_VITA_RENDER", "480x272"}}}}},
    {"PERFORMANCE MODE",
     {"ON leaves out the crowd in the stands and the people around the pitch.", "The match plays the same; the picture is a little emptier and faster."},
     0, true,
     {{"OFF", {}}, {"ON", {{"MSC_PERFORMANCE_MODE", "1"}}}}},
    {"FRAME PACING",
     {"ON keeps animations at the original speed while the Vita draws at 30 fps.", "Leave it ON. OFF is for finding problems: animations run at half speed."},
     0, true,
     {{"ON", {}}, {"OFF", {{"MSC_FRAME_PACING", "0"}}}}},
    // W5 FPS (np/docs/wave5-FPS.md): the game's pictures are shown for exactly two refreshes (30 fps) unless AUTO is chosen: AUTO shows 60 while the scene holds it (measured
    // per frame, with a cool-down so it does not flip) and 30 otherwise. 30 is the build's default and writes nothing. MSC_FPS=60|unlocked exist for testing, set by hand
    // in np-player.txt (a file with them reads CUSTOM here). A build without W5FPS ignores the key.
    {"FRAME RATE",
     {"30 FPS: every picture is shown for exactly two refreshes. The safe choice.", "AUTO: 60 where a scene can hold it, 30 elsewhere. Game speed is the same."},
     0, true,
     {{"30 FPS", {}}, {"AUTO (30 OR 60)", {{"MSC_FPS", "auto"}}}}},
    // PIPE (wave 5, np/docs/wave5-PIPE.md): the player build starts in core plan C with a safety net (ux0:data/mscharged/np-plan.txt steps down to A and then S after a
    // failed start); MSC_PLAN=A|S picks one by hand and switches the net off. A build without W5PIPE ignores the key, so the row only takes effect in a build that has it.
    // (The earlier row wrote MSC_VGX_BACKEND_THREAD and MSC_VITA_CORE_FRAMESEND, which count as explicit settings and bypass PIPE's net.)
    {"CORE PLAN",
     {"Which CPU core does which job (advanced). AUTO: the tested plan, with a net.", "C NO OVERLAP, A and S use fewer threads: S the fewest. Next start."},
     0, true,
     {{"AUTO", {}}, {"C NO OVERLAP", {{"MSC_PLAN", "Cn"}}}, {"PLAN A", {{"MSC_PLAN", "A"}}}, {"PLAN S (SAFEST)", {{"MSC_PLAN", "S"}}}}},
    // The picture rows of wave 5 LOOK (docs: LOOK.NOTES.md "PICTURE SETTINGS FOR ONE"): in no preset. The keys are read by LOOK's renderer changes (vgx
    // 50e3a62 and later); a build without them ignores the lines, so these rows only take effect in a build that has merged W5LOOK.
    {"SHARPNESS",
     {"Sharpens the picture when it is scaled up to the screen.", "CRISP+ is stronger. Needs the shader compiler. Not needed at 960x544."},
     0, false,
     {{"OFF", {}}, {"CRISP", {{"MSC_VITA_UPSCALE", "sharp"}}}, {"CRISP+", {{"MSC_VITA_UPSCALE", "sharp+"}, {"MSC_VITA_SHARPEN", "0.35"}}}}},
    {"ANTI-ALIASING",
     {"Smooths jagged edges. Experimental: costs GPU time and memory.", "Not measured yet. Leave it OFF unless you want to try it."},
     0, false,
     {{"OFF", {}}, {"2X", {{"MSC_VITA_MSAA", "2"}}}, {"4X", {{"MSC_VITA_MSAA", "4"}}}}},
    {"PICTURE FORMAT",
     {"WIDESCREEN fills the screen. 4:3 is the original picture with bars.", "Changes what is shown at the sides of the pitch, not the match."},
     0, false,
     {{"WIDESCREEN", {}}, {"4:3 BOX", {{"NP_WIDESCREEN", "0"}}}}},
    // Wave 5 NET: if two Vitas on NEARBY do not find each other (both made a cell of their own), NEARBY HOST on one and NEARBY JOIN on the other
    // (the ad hoc dialog's create / join modes, MSC_LAN_DIALOG). The first two choices keep their place (saved files read the same).
    {"MULTIPLAYER LINK",
     {"NEARBY: Vita to Vita, no router. SAME WI-FI: through your access point.", "Not finding each other? NEARBY HOST on one Vita, NEARBY JOIN on the other."},
     0, false,
     {{"NEARBY (AD HOC)", {}}, {"SAME WI-FI", {{"MSC_LAN_MODE", "wifi"}}}, {"NEARBY HOST", {{"MSC_LAN_DIALOG", "create"}}},
      {"NEARBY JOIN", {{"MSC_LAN_DIALOG", "join"}}}}},
    // 17 Oct (W5 ITEM): DRAWING ONLY is the Vita default (items throw players into electric fences; as retail the electrocuted player is a
    // dark silhouette). VitaPlan sets MSC_GAME_FIXES=net,electro when the file does not name it; OFF writes "none".
    {"ORIGINAL GAME FIXES",
     {"Fixes retail bugs: resting goal nets, electrocution picture, Boo glitch.", "ALL changes the match itself: both Vitas must use the same setting."},
     1, false,
     {{"OFF (AS RETAIL)", {{"MSC_GAME_FIXES", "none"}}}, {"DRAWING ONLY", {}}, {"ALL (WITH BOO FIX)", {{"MSC_GAME_FIXES", "all"}}}}},
    // W5 CAM (np/docs/wave5-CAM.md part 2, np/unlocks.h): the content the original game locks behind the cups of Road to the Striker Cup, opened by
    // the game's own unlock predicates, never by the save's cup bits: cups, trophies and the save stay as earned. Captains: ON is the Vita build's
    // default (writes nothing); stadiums: OFF is the default (the maintainer has not decided). A build without W5CAM ignores both keys.
    {"BONUS CAPTAINS",
     {"ON: Bowser Jr., Diddy Kong and Petey Piranha without winning the cups.", "Cups, stadiums and trophies stay as earned. OFF: as the original game."},
     0, false,
     {{"ON", {}}, {"OFF", {{"MSC_UNLOCK_CAPTAINS", "0"}}}}},
    {"BONUS STADIUMS",
     {"ON: Lava Pit, Crystal Canyon and Stormship without winning the cups.", "Cups, captains and trophies stay as earned. OFF: as the original game."},
     0, false,
     {{"OFF", {}}, {"ON", {{"MSC_UNLOCK_STADIUMS", "1"}}}}},
    // W5 CTL (np/docs/wave5-CTL.md): the button layout, the rear touchpad, and each touch feature on its own. A build without W5CTL
    // ignores MSC_CONTROLS / MSC_REAR_TOUCH / MSC_TOUCH_SAVE|METER|SHAKE. (The earlier MEGA STRIKE TOUCH row wrote MSC_TOUCH_MINIGAMES=0,
    // which still switches all three minigames off when it is in the file.)
    {"CONTROLS",
     {"MODERN: Cross pass, Circle shoot, Square lob/big hit, Triangle item, R turbo", "CLASSIC: the GameCube Strikers buttons. TRIANGLE: see every button."},
     0, false,
     {{"MODERN", {}}, {"CLASSIC", {{"MSC_CONTROLS", "classic"}}}}},
    {"REAR TOUCH",
     {"Tap the back: right half switches player, left half cycles items (and", "shakes their Mega Strike meter). A resting finger never counts."},
     0, false,
     {{"ON WITH MODERN", {}}, {"ON", {{"MSC_REAR_TOUCH", "1"}}}, {"OFF", {{"MSC_REAR_TOUCH", "0"}}}}},
    {"MENU STICK",
     {"The left stick moves through the menus as well as the D-pad.", ""},
     0, false,
     {{"ON", {}}, {"OFF", {{"NP_MENU_NAV", "0"}}}}},
    {"TOUCH POINTER",
     {"A finger on the screen moves the pointer in menus that use one.", ""},
     0, false,
     {{"ON", {}}, {"OFF", {{"NP_TOUCH_POINTER", "0"}}}}},
    {"TOUCH MEGA SAVE",
     {"Mega Save: the finger aims and blocks where it touches.", "OFF: aim with either stick, block with Cross or R."},
     0, false,
     {{"ON", {}}, {"OFF", {{"MSC_TOUCH_SAVE", "0"}}}}},
    {"TOUCH MEGA STRIKE",
     {"Your Mega Strike: hold a finger on the screen to stop the meter.", "OFF: the shoot button does it."},
     0, false,
     {{"ON", {}}, {"OFF", {{"MSC_TOUCH_METER", "0"}}}}},
    {"TOUCH SHAKEDOWN",
     {"Their Mega Strike: swipe the screen to shake their meter.", "OFF: tap the back left, or mash Select, Square or Triangle."},
     0, false,
     {{"ON", {}}, {"OFF", {{"MSC_TOUCH_SHAKE", "0"}}}}},
    {"POINTER SPEED",
     {"How fast the pointer moves in menus when steered with the right stick.", ""},
     2, false,
     {{"SLOW 0.5", {{"NP_POINTER_SPEED", "0.5"}}},
      {"SLOWER 0.75", {{"NP_POINTER_SPEED", "0.75"}}},
      {"NORMAL", {}},
      {"FASTER 1.5", {{"NP_POINTER_SPEED", "1.5"}}},
      {"FAST 2", {{"NP_POINTER_SPEED", "2"}}}}},
};
constexpr int kOptionTotal = int(sizeof(kTable) / sizeof(kTable[0]));
static_assert(kOptionTotal <= kMaxOptions, "raise kMaxOptions");

// The presets (option title, choice index). Placeholders until the wave 5 performance work lands its keys (docs/wave5-ONE.md, "Presets").
constexpr Preset kPresets[] = {
    {"PERFORMANCE", {{"RESOLUTION", 2}, {"PERFORMANCE MODE", 1}, {"FRAME PACING", 0}, {"FRAME RATE", 1}, {"CORE PLAN", 0}}},   // smaller picture, no crowd, 60 where it holds
    {"DEFAULT", {{"RESOLUTION", 0}, {"PERFORMANCE MODE", 0}, {"FRAME PACING", 0}, {"FRAME RATE", 0}, {"CORE PLAN", 0}}},      // (30: until a console has measured AUTO)
    {"QUALITY", {{"RESOLUTION", 1}, {"PERFORMANCE MODE", 0}, {"FRAME PACING", 0}, {"FRAME RATE", 0}, {"CORE PLAN", 0}}},       // the screen's own size, a steady 30
};
constexpr int kPresetTotal = int(sizeof(kPresets) / sizeof(kPresets[0]));

const char* const kBlockStart = "# ---- Settings screen (LiveArea > Settings): the lines below are written by it; the rest of this file is kept ----";
const char* const kBlockEnd = "# ---- end of the Settings screen's lines ----";

struct Line {
    std::string key, value;
};

// NAME=VALUE lines of the file text, in order (comments, blanks and lines without '=' are not variables).
std::vector<Line> Variables(const std::string& text) {
    std::vector<Line> out;
    size_t at = 0;
    while (at <= text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;
        size_t b = 0;
        while (b < line.size() && (line[b] == ' ' || line[b] == '\t')) ++b;
        size_t e = line.size();
        while (e > b && (line[e - 1] == '\r' || line[e - 1] == ' ' || line[e - 1] == '\t')) --e;
        line = line.substr(b, e - b);
        const size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos || eq == 0) continue;
        Line v;
        v.key = line.substr(0, eq);
        v.value = line.substr(eq + 1);
        out.push_back(v);
    }
    return out;
}

// The value a key has in the file (the last line wins, as setenv does when LoadEnvFile reads them in order), or null.
const std::string* Lookup(const std::vector<Line>& vars, const char* key) {
    const std::string* found = nullptr;
    for (const Line& v : vars)
        if (v.key == key) found = &v.value;
    return found;
}

// The file holds this choice when every key the option owns has the value the choice gives it (a key the choice does not mention must be absent).
bool Matches(const std::vector<Line>& vars, const Option& option, const Choice& choice) {
    for (const Choice& any : option.choices) {
        if (!any.label) break;
        for (const Assign& a : any.set) {
            if (!a.key) break;
            const char* want = nullptr;
            for (const Assign& b : choice.set)
                if (b.key && std::strcmp(b.key, a.key) == 0) want = b.value;
            const std::string* have = Lookup(vars, a.key);
            if (want ? !(have && *have == want) : have != nullptr) return false;
        }
    }
    return true;
}

bool OptionOwnsKey(int option, const std::string& key) {
    for (const Choice& c : kTable[option].choices) {
        if (!c.label) break;
        for (const Assign& a : c.set)
            if (a.key && key == a.key) return true;
    }
    return false;
}

void Describe(State& s, const std::vector<Line>& vars, int option) {
    std::string t;
    for (const Choice& c : kTable[option].choices) {
        if (!c.label) break;
        for (const Assign& a : c.set) {
            if (!a.key) break;
            if (const std::string* v = Lookup(vars, a.key)) {
                const std::string item = std::string(a.key) + "=" + *v;
                if (t.find(item) == std::string::npos) t += (t.empty() ? "" : " ") + item;
            }
        }
    }
    std::snprintf(s.custom[option], sizeof s.custom[option], "%s", t.c_str());
}

} // namespace

int OptionCount() { return kOptionTotal; }
const Option& GetOption(int index) { return kTable[index]; }
int ChoiceCount(int option) {
    int n = 0;
    while (n < kMaxChoices && kTable[option].choices[n].label) ++n;
    return n;
}
int PresetCount() { return kPresetTotal; }
const Preset& GetPreset(int index) { return kPresets[index]; }

void Load(State& s, const char* fileText) {
    s.text = fileText ? fileText : "";
    const std::vector<Line> vars = Variables(s.text);
    for (int o = 0; o < kMaxOptions; ++o) {
        s.choice[o] = 0;
        s.touched[o] = false;
        s.custom[o][0] = '\0';
    }
    for (int o = 0; o < kOptionTotal; ++o) {
        s.choice[o] = -1;
        for (int c = 0; c < ChoiceCount(o); ++c)
            if (Matches(vars, kTable[o], kTable[o].choices[c])) {
                s.choice[o] = c;
                break;
            }
        if (s.choice[o] < 0) Describe(s, vars, o);
    }
}

void SetChoice(State& s, int option, int choice) {
    if (option < 0 || option >= kOptionTotal || choice < 0 || choice >= ChoiceCount(option)) return;
    s.choice[option] = choice;
    s.touched[option] = true;
}

void Step(State& s, int option, int direction) {
    const int n = ChoiceCount(option);
    if (n < 2 && s.choice[option] >= 0) return;
    int at = s.choice[option];
    if (at < 0) at = direction > 0 ? -1 : n;
    SetChoice(s, option, ((at + direction) % n + n) % n);
}

void ResetAll(State& s) {
    for (int o = 0; o < kOptionTotal; ++o) SetChoice(s, o, kTable[o].defaultChoice);
}

namespace {
int FindOption(const char* title) {
    for (int o = 0; o < kOptionTotal; ++o)
        if (std::strcmp(kTable[o].title, title) == 0) return o;
    return -1;
}
} // namespace

int MatchPreset(const State& s) {
    // equal on what the preset sets AND on every other inPreset option at its default (a preset is a whole picture/speed profile)
    for (int p = 0; p < kPresetTotal; ++p) {
        bool all = true;
        for (int o = 0; o < kOptionTotal && all; ++o) {
            if (!kTable[o].inPreset) continue;
            int want = kTable[o].defaultChoice;
            for (const PresetItem& item : kPresets[p].items)
                if (item.option && FindOption(item.option) == o) want = item.choice;
            if (s.choice[o] != want) all = false;
        }
        if (all) return p;
    }
    return -1;
}

void ApplyPreset(State& s, int preset) {
    if (preset < 0 || preset >= kPresetTotal) return;
    for (int o = 0; o < kOptionTotal; ++o) {
        if (!kTable[o].inPreset) continue;
        int want = kTable[o].defaultChoice;
        for (const PresetItem& item : kPresets[preset].items)
            if (item.option && FindOption(item.option) == o) want = item.choice;
        SetChoice(s, o, want);
    }
}

bool Serialize(const State& s, std::string& out, size_t limit) {
    // The original text without the lines this screen owns: those of options the player changed or that equal a choice (they are written again below
    // from the choice), and its own block comments. The lines of an option holding a value that is none of the choices stay where they are.
    std::string kept;
    size_t at = 0;
    while (at < s.text.size()) {
        size_t end = s.text.find('\n', at);
        if (end == std::string::npos) end = s.text.size();
        std::string line = s.text.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
        bool drop = line == kBlockStart || line == kBlockEnd;
        if (!drop) {
            const std::vector<Line> v = Variables(line);
            if (v.size() == 1) {
                for (int o = 0; o < kOptionTotal && !drop; ++o)
                    if (OptionOwnsKey(o, v[0].key) && (s.touched[o] || s.choice[o] >= 0)) drop = true;
            }
        }
        if (!drop) kept += line + "\n";
    }
    while (kept.size() >= 2 && kept[kept.size() - 1] == '\n' && kept[kept.size() - 2] == '\n') kept.erase(kept.size() - 1);
    std::string block;
    for (int o = 0; o < kOptionTotal; ++o) {
        if (s.choice[o] < 0) continue;
        for (const Assign& a : kTable[o].choices[s.choice[o]].set) {
            if (!a.key) break;
            if (a.value) block += std::string(a.key) + "=" + a.value + "\n";
        }
    }
    out = kept;
    if (!block.empty()) {
        if (!out.empty()) out += "\n";
        out += std::string(kBlockStart) + "\n" + block + kBlockEnd + "\n";
    }
    return out.size() <= limit;
}

// ---- the screen ---------------------------------------------------------------------------------------------------------------------------------------------
int RowCount() { return 1 + kOptionTotal + 3; }
Row GetRow(int index) {
    if (index == 0) return {kRowPreset, -1};
    if (index <= kOptionTotal) return {kRowOption, index - 1};
    return {RowKind(kRowReset + (index - kOptionTotal - 1)), -1};
}

namespace {

constexpr uint32_t kBack = 0xFF201810u, kBand = 0xFF8A4A1Eu, kBandLow = 0xFF5A2E10u, kSelect = 0xFF5A3A24u, kEdge = 0xFFE8A050u;
constexpr int kVisibleRows = 14;   // rows of the list that fit above the help text at 25 px
constexpr uint32_t kWhite = 0xFFF0F0F0u, kDim = 0xFFB8B8B8u, kYellow = 0xFF40E0FFu, kGreen = 0xFF70E070u, kRed = 0xFF6060FFu, kHint = 0xFF909090u;

void Rect(uint32_t* px, int x, int y, int w, int h, uint32_t color) {
    for (int j = y < 0 ? 0 : y; j < y + h && j < kHeight; ++j)
        for (int i = x < 0 ? 0 : x; i < x + w && i < kWidth; ++i) px[j * kWidth + i] = color;
}

int Text(uint32_t* px, int x, int y, uint32_t color, const char* s, int scale = 1) {
    for (; *s; ++s, x += kVitaFontWidth * scale) {
        unsigned char c = static_cast<unsigned char>(*s);
        if (c < 32 || c > 126) c = '?';
        for (int j = 0; j < kVitaFontHeight; ++j) {
            const uint16_t bits = kVitaFont[c - 32][j];
            if (!bits) continue;
            for (int i = 0; i < kVitaFontWidth; ++i)
                if (bits & (1u << (kVitaFontWidth - 1 - i))) Rect(px, x + i * scale, y + j * scale, scale, scale, color);
        }
    }
    return x;
}

} // namespace

void Draw(uint32_t* px, const State& s, int selected, const char* message, bool error, bool running) {
    Rect(px, 0, 0, kWidth, kHeight, kBack);
    // header band: the title, with a darker lower edge
    Rect(px, 0, 0, kWidth, 62, kBand);
    Rect(px, 0, 62, kWidth, 3, kBandLow);
    Text(px, 36, 6, kWhite, "MARIO STRIKERS CHARGED", 2);
    Text(px, kWidth - 36 - 8 * 24, 6, kYellow, "SETTINGS", 2);

    // 25 px per row (the font is 24 px high); W5 CTL: more rows than fit above the help text scroll (they used to squeeze the pitch)
    const int top = 74, labelX = 52, valueX = 404;
    const int pitch = 25;
    const int first = FirstVisibleRow(selected);
    const int last = first + kVisibleRows - 1 < RowCount() - 1 ? first + kVisibleRows - 1 : RowCount() - 1;
    if (first > 0) Text(px, kWidth - 44, top, kEdge, "^");
    if (last < RowCount() - 1) Text(px, kWidth - 44, top + (kVisibleRows - 1) * pitch, kEdge, "v");
    int y = top;
    for (int r = first; r <= last; ++r) {
        const Row row = GetRow(r);
        if (row.kind == kRowReset && r != first) y += 4;
        const bool on = r == selected;
        if (on) {
            Rect(px, 20, y - 1, kWidth - 40, pitch, kSelect);
            Rect(px, 20, y - 1, 4, pitch, kEdge);
        }
        const char* label = "";
        char value[64] = "";
        uint32_t valueColor = kYellow;
        bool changeable = false;
        switch (row.kind) {
        case kRowPreset: {
            label = "PRESET";
            const int p = MatchPreset(s);
            std::snprintf(value, sizeof value, "%s", p >= 0 ? kPresets[p].name : "CUSTOM");
            changeable = true;
            break;
        }
        case kRowOption: {
            const Option& o = kTable[row.option];
            label = o.title;
            const int c = s.choice[row.option];
            if (c >= 0) std::snprintf(value, sizeof value, "%s", o.choices[c].label);
            else std::snprintf(value, sizeof value, "CUSTOM: %.40s", s.custom[row.option]);
            changeable = true;
            break;
        }
        case kRowReset: label = "RESET TO DEFAULTS"; valueColor = kDim; break;
        case kRowSaveStart: label = running ? "SAVE AND RETURN" : "SAVE AND START"; valueColor = kGreen; break;
        case kRowSaveExit: label = running ? "SAVE AND RESTART" : "SAVE AND EXIT"; valueColor = kGreen; break;
        default: break;
        }
        const uint32_t labelColor = (row.kind == kRowSaveStart || row.kind == kRowSaveExit) ? kGreen : (on ? kWhite : kDim);
        Text(px, labelX, y, labelColor, label);
        if (changeable) {
            int x = valueX;
            if (on) x = Text(px, x - 24, y, kEdge, "<") + 12;
            x = Text(px, x, y, valueColor, value);
            if (on) Text(px, x + 12, y, kEdge, ">");
        }
        y += pitch;
    }

    // help for the selected row (or the status line instead of its second line), the buttons
    const Row sel = GetRow(selected);
    const char* help1 = "";
    const char* help2 = "";
    switch (sel.kind) {
    case kRowPreset:
        help1 = "A preset sets resolution, Performance Mode, pacing, frame rate, core plan.";
        help2 = "Change any of those and it reads CUSTOM. The rows below are not in a preset.";
        break;
    case kRowOption: help1 = kTable[sel.option].help[0]; help2 = kTable[sel.option].help[1]; break;
    case kRowReset: help1 = "Puts every option above back to the game's choice. Other lines stay."; break;
    case kRowSaveStart:
        help1 = running ? "Saves for the next launch. Your current game continues unchanged."
                        : "Writes ux0:data/mscharged/np-player.txt and starts the game.";
        break;
    case kRowSaveExit:
        help1 = running ? "Apply saved settings now by restarting. Unsaved match progress is lost."
                        : "Writes np-player.txt and returns to the LiveArea.";
        break;
    default: break;
    }
    Rect(px, 20, 458, kWidth - 40, 2, kBandLow);
    Text(px, 24, 462, kDim, help1);
    if (message && *message) Text(px, 24, 486, error ? kRed : kGreen, message);
    else Text(px, 24, 486, kDim, help2);
    Text(px, 24, 516, kHint, running ? "Up/Down: select  Left/Right, Cross: change  Triangle: controls  Circle: back"
                                   : "Up/Down: select  Left/Right, Cross: change  Triangle: controls  Circle: leave");
}

void DrawRestartConfirmation(uint32_t* px, bool restart) {
    Rect(px, 80, 160, 800, 220, kBack);
    Rect(px, 80, 160, 800, 3, kEdge);
    Text(px, 112, 180, kWhite, "RESTART THE GAME?", 2);
    Text(px, 112, 242, kDim, "Unsaved match progress will be lost.");
    Rect(px, restart ? 480 : 112, 300, 320, 38, kSelect);
    Text(px, 136, 308, restart ? kDim : kWhite, "CANCEL");
    Text(px, 504, 308, restart ? kWhite : kDim, "SAVE AND RESTART");
}

int FirstVisibleRow(int selected) {
    const int rows = RowCount();
    if (rows <= kVisibleRows) return 0;
    int first = selected - kVisibleRows / 2;
    if (first < 0) first = 0;
    if (first > rows - kVisibleRows) first = rows - kVisibleRows;
    return first;
}

int OptionIndex(const char* title) {
    for (int o = 0; o < kOptionTotal; ++o)
        if (std::strcmp(kTable[o].title, title) == 0) return o;
    return -1;
}

// ---- the controls page (W5 CTL) -----------------------------------------------------------------------------------------------------------------------------
// Every line comes from the controls table the game itself is checked against (np/platform/sdk/input/np_control_table.h: the pause menu's
// controller map and the game's rewritten prompts use the same lines; test_controls checks them against the mapping, a script in the game).
namespace {
NpControlTable::Config ControlsConfig(const State& s) {
    const int layoutRow = OptionIndex("CONTROLS");
    const int rearRow = OptionIndex("REAR TOUCH");
    NpControlTable::Config c;
    c.layout = (layoutRow >= 0 && s.choice[layoutRow] == 1) ? 0 : 1;   // CUSTOM reads as the build's default (MODERN)
    const int rearChoice = rearRow >= 0 ? s.choice[rearRow] : 0;
    c.rear = rearChoice == 1 || (rearChoice != 2 && c.layout == 1);
    auto on = [&](const char* title) {
        const int o = OptionIndex(title);
        return o < 0 || s.choice[o] != 1;
    };
    c.touchSave = on("TOUCH MEGA SAVE");
    c.touchMeter = on("TOUCH MEGA STRIKE");
    c.touchShake = on("TOUCH SHAKEDOWN");
    c.touchPointer = on("TOUCH POINTER");
    return c;
}
}  // namespace

void DrawControls(uint32_t* px, const State& s) {
    using NpControlTable::LineAt;
    using NpControlTable::LineCount;
    const NpControlTable::Config cfg = ControlsConfig(s);
    const bool modern = cfg.layout == 1;
    Rect(px, 0, 0, kWidth, kHeight, kBack);
    Rect(px, 0, 0, kWidth, 62, kBand);
    Rect(px, 0, 62, kWidth, 3, kBandLow);
    Text(px, 36, 6, kWhite, "CONTROLS", 2);
    Text(px, 36 + 9 * 24, 6, kYellow, modern ? "MODERN" : "CLASSIC", 2);
    Text(px, kWidth - 36 - 12 * 20, 32, kDim, modern ? "BATTLE LEAGUE / EA FC" : "GAMECUBE STRIKERS");

    const int col0 = 24, col1 = 236, col2 = 596, top = 72;
    Text(px, col0, top, kEdge, "BUTTON");
    Text(px, col1, top, kEdge, "WITH THE BALL");
    Text(px, col2, top, kEdge, "WITHOUT THE BALL");
    Rect(px, 20, top + 25, kWidth - 40, 2, kBandLow);
    int y = top + 30;
    bool rule = false;
    char line[160];
    for (int i = 0; i < LineCount(); ++i) {
        const NpControlTable::Line& l = LineAt(i);
        if (!NpControlTable::Shown(l, cfg)) continue;
        if (NpControlTable::IsSentence(l)) {
            if (!rule) {   // the minigames and the menus below a rule
                Rect(px, 20, y + 2, kWidth - 40, 2, kBandLow);
                y += 8;
                rule = true;
            }
            char text[128];
            std::snprintf(line, sizeof line, "%s: %s", l.input, NpControlTable::Sentence(l, cfg, text, sizeof text));
            Text(px, col0, y, kDim, line);
            y += 24;
            continue;
        }
        const bool on = NpControlTable::Active(l, cfg);
        Text(px, col0, y, on ? kYellow : kHint, l.input);
        Text(px, col1, y, on ? kWhite : kHint, on ? l.withBall : "(REAR TOUCH OFF)");
        if (on) Text(px, col2, y, kWhite, l.withoutBall);
        y += 24;
    }
    Text(px, 24, 516, kHint, "Left/Right: MODERN or CLASSIC   Circle, Cross or Triangle: back to the list");
}

} // namespace NpSettings
