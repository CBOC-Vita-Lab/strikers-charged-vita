// The Settings screen of the player build (the LiveArea's Settings button, like Mario Kart Wii Vita 1.3.0's): the model (which options exist, what each choice
// writes into np-player.txt, the three presets), the reading and writing of that file, and the drawing of the screen into a 960 x 544 picture. Portable
// code: the console glue (input, files, relaunch) is np_vita_settings.cpp, the host test is platform/vita/tests/test_settings.cpp.
//
// np-player.txt is NAME=VALUE lines (the format of the lab build's env.txt, read by LoadEnvFile in np_vita_main.cpp). The screen only ever adds, changes or
// removes the lines of the options it manages: every other line (comments, the tester's own variables) is kept. A choice that is the build's default
// writes nothing, so a later change of a default (the performance workstreams change some) reaches every player who never moved that option.
//
// Adding an option or a preset key: one row in kTable (np_settings.cpp); presets name the options they set by title, the others keep their value.
#pragma once

#include <cstdint>
#include <string>

namespace NpSettings {

constexpr int kMaxOptions = 24;   // (W5 CAM: 20 rows with BONUS CAPTAINS / STADIUMS; room for more)
constexpr int kMaxChoices = 6;
constexpr int kMaxAssign = 3;

// One line of np-player.txt: NAME=VALUE (value null: the line is not there).
struct Assign {
    const char* key;
    const char* value;
};
struct Choice {
    const char* label;               // as drawn, upper case, at most 22 characters
    Assign set[kMaxAssign];          // key == nullptr ends the list
};
struct Option {
    const char* title;               // at most 22 characters
    const char* help[2];             // two lines of at most 78 characters
    int defaultChoice;               // the choice that writes nothing (the build's own default)
    bool inPreset;                   // part of what a preset sets (a preset's mismatch makes the row PRESET read CUSTOM)
    Choice choices[kMaxChoices];     // label == nullptr ends the list
};
struct PresetItem {
    const char* option;              // an option's title; null ends the list
    int choice;
};
struct Preset {
    const char* name;
    PresetItem items[kMaxOptions];   // the options the preset sets; the others keep their value
};

int OptionCount();
const Option& GetOption(int index);
int ChoiceCount(int option);
int PresetCount();
const Preset& GetPreset(int index);

// The file's content as the screen sees it.
struct State {
    std::string text;                // np-player.txt as read (never modified in place; Serialize makes the new text)
    int choice[kMaxOptions];         // current choice per option; -1 = the file holds a value that is none of the choices ("CUSTOM")
    bool touched[kMaxOptions];       // changed on this screen
    char custom[kMaxOptions][40];    // for choice -1: the lines' values, as "KEY=VALUE KEY=VALUE", shown as the value
};

void Load(State& state, const char* fileText);              // fileText may be null (no file): everything is the default
void SetChoice(State& state, int option, int choice);       // marks the option touched
void Step(State& state, int option, int direction);         // next/previous choice, wrapping; from CUSTOM the first/last
void ResetAll(State& state);                                // every option to its default (the file's other lines are kept)
int MatchPreset(const State& state);                        // index of the preset the settings equal, or -1
void ApplyPreset(State& state, int preset);
// The new file text: the original with the managed lines replaced by a block at the end. False when it would not fit (limit bytes, the loader reads
// that much of the file).
bool Serialize(const State& state, std::string& out, size_t limit = 15000);

// ---- the screen ----
constexpr int kWidth = 960, kHeight = 544;
enum RowKind { kRowPreset, kRowOption, kRowReset, kRowSaveStart, kRowSaveExit, kRowKindCount };
struct Row {
    RowKind kind;
    int option;                      // for kRowOption
};
int RowCount();
Row GetRow(int index);
// Draws the whole screen into pixels (kWidth x kHeight, 0xAABBGGRR). selected = row index; message (may be null) is shown in the status line,
// error = in red.
void Draw(uint32_t* pixels, const State& state, int selected, const char* message, bool error);
// W5 CTL: the controls page (Triangle on the list): every button of the layout the CONTROLS row holds, with and without the ball,
// the rear touchpad, the minigames and the menus.
void DrawControls(uint32_t* pixels, const State& state);
int OptionIndex(const char* title);   // -1 when there is no such row
// The first row drawn when `selected` is selected (the list scrolls when it has more rows than fit).
int FirstVisibleRow(int selected);

} // namespace NpSettings
