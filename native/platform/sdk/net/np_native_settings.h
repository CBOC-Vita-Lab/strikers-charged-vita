// The player settings as an engine-free list of rows for the game's own pause-style screen (SHVitaSettings, scene18): set18, the one place a player changes a setting. No game headers, no drawing:
// this is the model (which rows, what each shows, what a press does, what saving writes) over the same NpSettings::State and Serialize the technical screen
// (platform/vita/src/np_settings.cpp) uses, so a file saved from here is byte-for-byte the file the technical screen would write for the same steps.
//
// Shape (set18): 5 pages of at most 5 rows, every player setting, most used first. MAIN: ALL CHARACTERS, ALL STADIUMS, FRAME RATE, CONTROLS, LANGUAGE. PICTURE: CROWD,
// SHARPNESS, BLUR, HD CHARACTERS. TOUCH: REAR TOUCH, MEGA SAVE, SAVE SPEED, TOUCH STRIKE, TOUCH SHAKEDOWN. MATCH: LESS SHAKE, MEGA STRIKES, GAME FIXES, POINTER SPEED.
// ONLINE: CONNECTION, ONLINE ACCOUNT, RESET TO DEFAULTS (mp24: NEARBY PAIRING went). Every choice of a row is reachable.
// A row's text is "NAME: VALUE" (UTF-8, through NpText::In in the language given to Open), at most kMaxLabelChars characters in English, Spanish and French.
// The ONLINE ACCOUNT row (IsAllSettingsRow: the name is kept from the 1.3 ALL SETTINGS row it replaced) has no value and a press on it returns kAllSettings: the caller opens the
// pixel screen's ONLINE page (OpenAll: the online name, SEND A REPORT, SAVE A REPORT, DELETE MY ONLINE ACCOUNT) and calls Reload() when it returns. The RESET TO DEFAULTS row has no
// value either: a press puts every row at its default (kChanged) and the leave popup saves it.
// The file: read and written through weak backend functions (below). The Vita build's backend is the "native18" block of np_vita_settings.cpp; the host
// proof backend (np_native_settings_host.cpp) uses the file named by MSC_SETTINGS_FILE. No backend: Open() returns false and the screen is not offered.
//
// Stable: additions only.
#pragma once

#include <cstddef>

namespace NpNativeSettings {

constexpr int kPages = 5;
constexpr int kMaxRowsPerPage = 5;
constexpr int kMaxLabelChars = 24;     // every Label/Name/Value/button text fits this many characters in EN, ES and FR (tests/test_native_settings.cpp)
constexpr int kMaxLabelBytes = 64;     // a buffer that always holds a label (UTF-8, Latin-1 accents are two bytes)

// What Press did.
enum PressResult {
    kNothing = 0,       // bad page/row, or the model is not open
    kChanged,           // the value moved (Label changed); the caller plays the value-changed sound
    kNoEffect,          // a grey row: nothing moved; Why() says why (show it in an OK popup, play the hover sound only)
    kAllSettings,       // the ONLINE ACCOUNT row: call OpenAll(), then Reload() when the pixel screen returns
};

// What the leave popup offers when Changed(): three buttons in this order, button 0 lit first (SAVE FOR NEXT START). Circle = keep editing.
enum LeaveButton { kLeaveSaveNext = 0, kLeaveSaveRestart = 1, kLeaveDiscard = 2, kLeaveButtons = 3 };

// language: NpText::kEnglish / kSpanish / kFrench, or -1 = NpText::CurrentLanguage() (the game's language).
// Open: reads the file through the backend into a fresh state. False when the build has no backend (the screen must not be offered) or Open failed.
bool Open(int language = -1);
bool IsOpen();
void Close();                          // forget the state (nothing is written)
// Reload: read the file again and drop every unsaved step (after the technical screen returned: it may have saved). Keeps the language.
bool Reload();

// Layout.
int Pages();                           // 5
int Rows(int page);                    // rows on the page (5, 4, 5, 4 and 3); 0 for a bad page
bool IsAllSettingsRow(int page, int row);   // the ONLINE ACCOUNT row (set18; the 1.3 ALL SETTINGS row)
bool IsResetRow(int page, int row);         // set18: the RESET TO DEFAULTS row
int Option(int page, int row);              // set18: the row's option (NpSettings::OptionIndex), -1 for ONLINE ACCOUNT, RESET and a bad row
// The full text of the row: "NAME: VALUE", or the name alone for ONLINE ACCOUNT and RESET TO DEFAULTS. Returns the length in bytes (0 on a bad row). Always NUL-terminated when cap > 0.
int Label(int page, int row, char* out, int cap);
int Name(int page, int row, char* out, int cap);    // "FRAME RATE" (translated)
int Value(int page, int row, char* out, int cap);   // "ON" (translated, the choice label cut at " ("); "" for ONLINE ACCOUNT and RESET TO DEFAULTS
// set18: the row's one-line help, in the open language (an option's help line of np_settings.cpp; its own for ONLINE ACCOUNT and RESET).
int Help(int page, int row, char* out, int cap);
// set18 (owner, 23 Nov: the description "if you can make it look native"): the line the scene shows under the panel for the bar under the cursor. bar: the scene's bar index
// (0..kMaxRowsPerPage-1 the rows, others MORE / DONE), -1 none. shown: the panel is in place (not sliding in or out, no popup). "" (length 0) when nothing is to be shown:
// no bar under the cursor, MORE, DONE, an empty row, or the panel not in place.
int HelpLine(int page, int bar, bool shown, char* out, int cap);
// The "MORE (1/2)" bar: page is the page being shown. The DONE bar.
int MoreLabel(int page, char* out, int cap);
int DoneLabel(char* out, int cap);
int NextPage(int page);                // wraps: 0 -> 1 -> 0

// Grey rows. Dim: the row is shown grey (the option has no effect while the other rows stand as they are, NpSettings::WhyNoEffect). Why: the reason in the
// open language, or null when the row acts. A grey row can still be stepped (it changes nothing in the game while grey); Press returns kNoEffect and does NOT move it.
bool Dim(int page, int row);
const char* Why(int page, int row);

// A press: dir +1 next value (wraps), -1 previous, 0 = +1 (Cross / a finger). Dim rows do not change. RESET TO DEFAULTS: every row to its default (kChanged).
PressResult Press(int page, int row, int dir);

// Unsaved steps exist (NpSettings::Changed, or RESET TO DEFAULTS was pressed): leaving needs the popup.
bool Changed();
// Does the popup's SAVE AND RESTART make sense (a changed row that is not live): true when Changed() and NpSettings::RestartNeeded.
bool RestartNeeded();

// Popup texts in the open language (what NpMenuUiPrepare takes).
int LeaveMessage(char* out, int cap);                // "Changes apply the next time the game starts. SAVE AND RESTART ends this match now. Circle: keep editing."
int LeaveButtonLabel(int button, char* out, int cap);   // LeaveButton -> "SAVE FOR NEXT START" / "SAVE AND RESTART" / "DISCARD"
int SaveFailedMessage(char* out, int cap);           // "Could not save the settings."

// ---- 1.3 live rows (live19): additions. A running match applies CROWD, LESS SHAKE and REAR TOUCH the moment they are saved; no restart from a match. ----
// The leave popup by place: matchRunning = the popup of a running match (SAVE / DON'T SAVE / KEEP EDITING, nothing is restarted: the three rows above apply at once, the others
// at the next start); not running = the front-end entry (OPTIONS): when a next-start row changed SAVE AND START AGAIN / DON'T SAVE / KEEP EDITING, else the match list.
// Button 0 is lit first, Circle = keep editing. The old LeaveButton functions above keep their meaning.
enum LeaveActionId { kActSave = 0, kActDiscard = 1, kActKeep = 2, kActSaveRestart = 3 };
bool NextStartChanged();                                  // a changed row that only applies at the next start (FRAME RATE, SHADOWS, LANGUAGE, CONTROLS and the other rows that are not live)
int LeaveCount(bool matchRunning);                        // 3 (the popup is only asked for when Changed())
LeaveActionId LeaveAction(int index, bool matchRunning);
int LeaveLabel(int index, bool matchRunning, char* out, int cap);   // "SAVE" / "DON'T SAVE" / "KEEP EDITING" / "SAVE AND START AGAIN", at most kMaxLabelChars in EN, ES, FR
// The popup's sentence: "" when only live rows changed in a match (nothing waits for the next start); else "Some changes apply the next time the game starts." (match), or
// "The game starts again to apply the changes." (front-end entry, SAVE AND START AGAIN).
int LeaveNote(bool matchRunning, char* out, int cap);

// ---- ux13 (U2): the question after SAVE in a running match ----
// PauseRestartOffered: call BEFORE Apply(kActSave) (the save clears Changed). True when MSC_PAUSE_RESTART=1, a match runs and a row that waits for the next start changed:
// the screen then asks (RestartAskMessage, two buttons: 0 RestartAskLabel NOT NOW, 1 START AGAIN) instead of going straight back to the pause menu. START AGAIN calls Restart().
bool PauseRestartOffered(bool matchRunning);
int RestartAskMessage(char* out, int cap);
int RestartAskLabel(int button, char* out, int cap);

// ---- 1.3 front19: doing what a leave button says (the same for the pause panel and the OPTIONS entry; SHVitaSettings only draws and navigates) ----
// kStay: KEEP EDITING (nothing written, the screen stays). kLeft: the screen may be left (saved, discarded, or saved and the game asked to start again: on a console the process is
// replaced inside the call and nothing returns; a refused restart or the host harness returns, the file IS saved and the changes apply at the next start). kOutSaveFailed: nothing was
// written, the model stays open and unchanged, the screen shows SaveFailedMessage.
enum LeaveOutcome { kStay = 0, kLeft = 1, kOutSaveFailed = 2 };
LeaveOutcome Apply(LeaveActionId action);

// Leaving. Save: Serialize the state and write the whole file through the backend; true when written. On false the model stays open and unchanged (the screen shows
// SaveFailedMessage). After a true Save, Changed() is false. A successful Save also applies the live rows (NpLiveSettingsSet, np/live_settings.h) that differ from the loaded file: it is the running game's save.
bool Save();
// Save, then ask the platform to start the game again (the "-play" relaunch). False when the save failed (nothing restarted) or the platform refused (the file IS saved).
bool Restart();
// Discard: drop the steps (reload from the file). Nothing is written.
void Discard();
// ONLINE ACCOUNT: ask the platform to open the pixel screen's ONLINE page over the paused game (NpVitaSettingsRequestOpen). False when there is none.
bool OpenAll();

// ---- backend (weak): the platform provides these; the host proof backend is np_native_settings_host.cpp ----
// Read the settings file's text into out; false when there is no backend (an absent file is true with empty text).
extern "C" bool NpNativeSettingsBackendRead(char* out, int cap, int* length) __attribute__((weak));
// Write the whole file; true when written (atomically where the platform can).
extern "C" bool NpNativeSettingsBackendWrite(const char* text, int length) __attribute__((weak));
// Start the game again as a fresh process; false when refused (the caller keeps the match).
extern "C" bool NpNativeSettingsBackendRestart(void) __attribute__((weak));
// Open the pixel screen's ONLINE page; false when there is none.
extern "C" bool NpNativeSettingsBackendOpenAll(void) __attribute__((weak));

}  // namespace NpNativeSettings
