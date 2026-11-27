// The Settings screen of the player build (np_vita_settings.cpp). Only the player build (NP_PLAYER) has it.
#pragma once

#include <string>

namespace NpSettingsScreen {

enum class Result {
    kStart,   // "Save and start": the file is written; the game goes on (Run has already tried to start the application again)
    kExit,    // startup only: quit to the LiveArea
    kReturn,  // running game: resume without applying saved changes
    kRestart, // running game: explicitly confirmed restart
    kNotShown // running game: the display could not be taken, nothing happened (the caller may try again)
};

// True when this start opens SAFE START (set18): L + R held at the start (the recovery path the fatal screen names), or the test marker file. The old LiveArea tile's "-settings"
// starts the game normally (logged). False when the application was started again by "Save and start" (argument -play).
bool Requested(int argc, char** argv);
// Shows the screen until the player leaves it. Takes over the display; gives it back.
Result Run(bool running = false);
// The game menu's ONLINE ACCOUNT row (np_native_settings.cpp OpenAll): the screen's ONLINE page opens at this frame's end (Pump), over the paused game. Safe from any thread.
void RequestOpen();
// Called at the game thread's frame boundary. Opens a pending request; an old LiveArea tile's press is logged and dropped.
void Pump();
// True while the screen (startup or pause menu) is open: the freeze button's thread does not count a hold then.
bool IsOpen();
// Recover an interrupted settings replacement before either startup or UI reads.
const char* SavedFilePath();
// Writes a settings text file (np-player.txt or its backup copy) so that a power cut leaves the old file or the new one (np_safe_file.h).
bool WriteSettingsText(const char* path, const std::string& text);

} // namespace NpSettingsScreen
