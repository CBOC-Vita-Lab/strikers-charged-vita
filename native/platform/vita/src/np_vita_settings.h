// The Settings screen of the player build (np_vita_settings.cpp). Only the player build (NP_PLAYER) has it.
#pragma once

namespace NpSettingsScreen {

enum class Result {
    kStart,   // "Save and start": the file is written; the game goes on (Run has already tried to start the application again)
    kExit     // "Save and exit" or Circle: quit to the LiveArea
};

// True when this start is for the Settings: the LiveArea button (parameter "-settings"), L + R held at the start, or the test marker file. False when the
// application was started again by "Save and start" (argument -play).
bool Requested(int argc, char** argv);
// Shows the screen until the player leaves it. Takes over the display; gives it back.
Result Run();

} // namespace NpSettingsScreen
