// The player settings (the rows, what each choice writes into np-player.txt), the reading and writing of that file, and the pixel screen drawn into a 960 x 544 picture.
// The game shows the rows in its own menu (platform/sdk/net/np_native_settings.cpp: OPTIONS > VITA SETTINGS and the pause menu's VITA SETTINGS). The pixel screen (set18) has two
// pages only: SAFE START (hold L + R at start-up: the way out when a setting stops the game from starting) and ONLINE (the account and the reports, opened from the game's menu).
// The LiveArea's Settings tile is gone (1.3 template.xml); an old tile's "-settings" start opens the game normally. Portable code: the console glue (input, files, relaunch) is
// np_vita_settings.cpp, the host test is platform/vita/tests/test_settings.cpp.
//
// np-player.txt is NAME=VALUE lines (the format of the lab build's env.txt, read by LoadEnvFile in np_vita_main.cpp). The screen only ever adds, changes or
// removes the lines of the options it manages: every other line (comments, the tester's own variables) is kept. A choice that is the build's default
// writes nothing, so a later change of a default (the performance workstreams change some) reaches every player who never moved that option.
//
// Why there is no version line in the file: a key is only in the file when the player moved its row (or wrote it by hand), so a default changed by a later build, or a
// switch added by one, reaches every older file by itself. The one way a line can pin an old default is a file that names the old default's value: kBuildDefaults
// (np_settings.cpp) reads such a line as the row's default, test_settings holds that table equal to vita_plan.cpp, and a changed default must keep its old value listed there.
//
// Adding an option: one row at the end of kTable (np_settings.cpp, "NEW ROWS GO HERE") and its place in np_native_settings.cpp kRows. Taking one away (1.3 prune, set18): the row
// goes, its keys go to kRetiredKeys (the screen then stops writing them and drops them from a file it wrote), and its reader stays; a value that is only worse for the player goes
// to kObsolete as well (skipped at start-up when the screen's own block holds it).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace NpSettings {

constexpr int kMaxOptions = 56;   // (set18 and mp24: 20 rows; the 1.3 prune 29; 1.2 had 35, the 1.3 development 51; room for more)
constexpr int kMaxChoices = 10;   // (1.3: the 1-10 scales)
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
    Choice choices[kMaxChoices];     // label == nullptr ends the list
};
int OptionCount();
const Option& GetOption(int index);
int ChoiceCount(int option);

// The file's content as the screen sees it.
struct State {
    std::string text;                // np-player.txt as read (never modified in place; Serialize makes the new text)
    int choice[kMaxOptions];         // current choice per option; -1 = the file holds a value that is none of the choices ("CUSTOM")
    int loaded[kMaxOptions];         // the choices as the file held them (Changed)
    bool touched[kMaxOptions];       // changed on this screen
    char custom[kMaxOptions][40];    // for choice -1: the lines' values, as "KEY=VALUE KEY=VALUE", shown as the value
    bool reset = false;              // RESET TO DEFAULTS was used on this screen: Serialize then drops the lines of every retired key, inside the block or not
};

void Load(State& state, const char* fileText);              // fileText may be null (no file): everything is the default
// set18: the text without the lines of kObsolete (np_settings.cpp) that stand inside the screen's own block: values an earlier 1.3 screen wrote that no row offers any more because
// they were only worse (a fixed RESOLUTION, ANTI-ALIASING OFF, ORIGINAL GAME FIXES OFF, MEGA SAVE SPEED ORIGINAL). Lines outside the block (the kit's, a player's own) stay. notes
// (may be null) gets one log line per line left out. The player build's loader (np_vita_main.cpp LoadEnvFile) applies the result; Load reads the rows from it.
std::string WithoutObsolete(const std::string& fileText, std::vector<std::string>* notes = nullptr);
void SetChoice(State& state, int option, int choice);       // marks the option touched
void Step(State& state, int option, int direction);         // next/previous choice, wrapping; from CUSTOM the first/last
void ResetAll(State& state);                                // every option to its default (the file's other lines are kept)
bool Changed(const State& state);                           // a choice differs from what the file held (leaving would lose it)
// The new file text: the original with the managed lines replaced by a block at the end. False when it would not fit (limit bytes, the loader reads
// that much of the file).
bool Serialize(const State& state, std::string& out, size_t limit = 15000);

// A request to open the screen while the game runs: the game menu's ONLINE ACCOUNT row (Request), and the "-settings" parameter of an old LiveArea tile (set18: observed so that
// the press is logged and dropped, never opened). The console glue polls sceAppMgrGetAppParam every 250 ms and tells this what it saw. The parameter may stay readable
// after the first read (the Galaxy port's screen reopened itself every 250 ms that way), so a press is the parameter appearing, never the parameter being there. Where it
// does stay, a second press in one run cannot be told from the first and does nothing; where the read consumes it, every press counts. A press that cannot open the screen
// is tried again at the next polls and given up after kMaxOpenFailures tries.
constexpr int kMaxOpenFailures = 3;
class OpenRequest {
public:
    void Seed(bool present) { seen_ = present; }   // the read at start-up: whatever that start did with it, it is not a new press
    bool Observe(bool present);                    // one poll; true when this is a new press
    void Request() { pending_ = true; }            // 1.3: the game's own pause-menu item (not the LiveArea): a press that needs no parameter
    bool Pending() const { return pending_; }
    void Opened() { pending_ = false; failures_ = 0; }
    bool Failed();                                 // one try failed; true when that was the last one
private:
    bool seen_ = false, pending_ = false;
    int failures_ = 0;
};

// ---- the screen ----
constexpr int kWidth = 960, kHeight = 544;
// kRowAction (wave 11): a row that runs something when Cross is pressed (SAVE A REPORT); Row::option is its index (ActionTitle).
// kRowInfo (1.3): a line that shows a fact and cannot be changed (the online name, the version, the build). Row::option is its index (InfoTitle). It is last so that
// the kRowReset, kRowSaveStart, kRowSaveExit run stays contiguous.
enum RowKind { kRowOption, kRowReset, kRowSaveStart, kRowSaveExit, kRowInfo, kRowAction, kRowKindCount };
struct Row {
    RowKind kind;
    int option;                      // for kRowOption
};
// 1.3: the rows are grouped in pages, each with its own list and with RESET and the two SAVE rows at its end. Saved files do not know about pages.
// Why a row does nothing while the other rows stand as they are (the dimmed rows' reason), or null when it acts (1.3: for the tests).
const char* WhyNoEffect(const State& s, int option);
int PageCount();
const char* PageName(int page);
// The information lines (kRowInfo): shown on the page that names them. The console glue sets the values that only it knows (the stored online name); the version and the build id
// are this build's own (np_build_identity.h). A value never set reads "" and is drawn as "-".
int InfoCount();
const char* InfoTitle(int info);
const char* InfoHelp(int info, int line);
void SetInfo(int info, const char* value);
const char* InfoValue(int info);
// The action rows (kRowAction): shown on the page that names them; the console glue runs one by its title.
int ActionCount();
const char* ActionTitle(int action);
int RowCount(int page);
Row GetRow(int page, int index);
bool FindRow(int option, int* page, int* row);   // where an option is on the screen
// Optional art of the player's own game (np_brand_art.h): a picture behind the screen and the game's lettering for the headings. The console glue
// loads it from the disc when the screen opens and clears it when the screen closes; without it (null, or a null backdrop) the screen keeps its
// plain look, pixel for pixel. The Art must outlive the drawing.
struct Art {
    const uint32_t* backdrop = nullptr;   // kWidth x kHeight, 0xAABBGGRR, already dimmed
    // Draws `text` at (x, y) with a capital line `height` px high (null pixels or draw = false: only measures); returns the width.
    std::function<int(uint32_t* pixels, int x, int y, int height, const char* text, bool draw)> heading;
};
void SetArt(const Art* art);
// Draws the whole screen into pixels (kWidth x kHeight, 0xAABBGGRR). selected = row index on `page`; message (may be null) is shown in the status line,
// error = in red.
void Draw(uint32_t* pixels, const State& state, int page, int selected, const char* message, bool error, bool running = false);
void DrawRestartConfirmation(uint32_t* pixels, bool restart, bool classic = false);   // classic: CONTROLS CLASSIC (ClassicLayout): the hint names Square for back
// 1.3: Circle with unsaved changes. Choices, in the order drawn:
enum LeaveChoice { kLeaveKeepEditing, kLeaveDiscard, kLeaveSave, kLeaveChoices, kLeaveSaveRestart = kLeaveChoices };
constexpr int kMaxLeave = 4;
void DrawLeaveConfirmation(uint32_t* pixels, int choice, bool classic = false);
// 1.3 in-game settings (the dialog's logic, host-tested): which choices the leave dialog offers and in which order. 0 = nothing changed: leave without a dialog.
// IsLive/RestartNeeded: whether a changed row applies to the running game when it is saved (1.3: STADIUM CROWD, REDUCED CAMERA SHAKE, REAR TOUCH; every other switch is read at process
// start). RestartNeeded = NextStartChanged: a changed row that only applies at the next start. ApplyLive: after a successful save while a game runs, hands each changed live row to `set`
// (np_live_settings.cpp NpLiveSettingsSet: key and value of the choice, null value = the unset rule); returns how many rows it applied. It never reads or writes the environment.
bool IsLive(int option);
bool RestartNeeded(const State& state);
bool NextStartChanged(const State& state);
int ApplyLive(const State& state, void (*set)(const char* key, const char* valueOrNull));
int LeaveDialog(bool running, bool changed, bool restartNeeded, int ids[kMaxLeave]);
int LeaveDefault(const int* ids, int count);              // the index lit first: SAVE (never the restart; it is also first in the list)
int LeaveMove(int index, int count, int direction);       // wraps
const char* LeaveLabel(int id, bool running);
// The running game's dialog, a column of the choices LeaveDialog gave; index is the lit one.
// note: a next-start row changed (the dialog says so; with only live rows changed it says nothing).
// W22: the consent / confirmation / waiting screens of SEND A REPORT and DELETE MY ONLINE ACCOUNT (the settings screen cannot use the game's popups)
enum ReportDialog { kReportSend, kReportDelete, kReportWorking, kReportDeleting };
void DrawReportDialog(uint32_t* pixels, ReportDialog kind, int choice, bool classic = false);
void DrawLeaveDialog(uint32_t* pixels, const int* ids, int count, int index, bool running, bool classic = false, bool note = false);
bool ClassicLayout(const State& state);   // CONTROLS CLASSIC: the game's menus go back with Square; the screen reads Square as Circle in every layout
// W5 CTL: the controls page (Triangle on the list): every button of the layout the CONTROLS row holds, with and without the ball,
// the rear touchpad, the minigames and the menus.
void DrawControls(uint32_t* pixels, const State& state);
int OptionIndex(const char* title);   // -1 when there is no such row
// The first row drawn when `selected` is selected (the list scrolls when it has more rows than fit).
int FirstVisibleRow(int page, int selected);

} // namespace NpSettings
