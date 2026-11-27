// The stale-settings guard (1.3). Players keep ux0:data/mscharged/np-player.txt across versions, and an old line can silently switch off what a newer build gained: the
// community GameCube port had exactly that (6.4 fps until a stale file was fixed). Here the 1.0 and 1.1 Settings screens wrote `MSC_VITA_RENDER=640x362` for PERFORMANCE and
// `MSC_VITA_RENDER=960x544` for QUALITY, and in 1.3 any fixed MSC_VITA_RENDER turns the dynamic-resolution ladder off (and with it the anti-aliasing rungs and the 60 fps governor).
//
// The file carries a stamp, a comment line (the loader skips it, the Settings screen keeps it):
//     # np-player.txt written by 1.3.0 build 0123456789abcdef
// 1.0 and 1.1 wrote no stamp, so a file without one is an older file. At start-up, when the stamp is older than this build (or missing), Migrate returns the new text:
//   - every line that a rule below names is commented out with the reason ("# 1.3 turned off: <line>   (<why>)"), nothing else changes: controls, language, picture
//     look, MSC_FPS=30, MSC_PERFORMANCE_MODE and every unknown line stay as the player had them;
//   - the stamp is put on the first line;
//   - the old file is kept as np-player.before-1.3.txt by the caller (never overwritten once it exists);
//   - the report says what was done, for log.txt and the loading screen's one line.
// A stamp equal to or newer than this build is never touched (a newer file under an older build is left for its own build), and neither is any stamp of 1.3.0 or later: the 1.3 Settings
// screen offers the very lines the rules name as choices, so a later build must not undo them. Unknown keys are only counted and logged.
// Portable code (no console calls): the host test is tests/test_settings_guard.cpp; the console glue is np_settings_guard_vita.cpp.
#pragma once

#include <string>
#include <vector>

namespace NpSettingsGuard {

struct Version {
    int major = 0, minor = 0, patch = 0;
    bool valid = false;
};
Version ParseVersion(const char* text);            // "1.3.0", "1.30" is not a version here; valid = false when it is not dotted numbers
int Compare(const Version& a, const Version& b);   // -1, 0, 1

// The stamp line of this build, without the newline: "# np-player.txt written by 1.3.0 build 0123456789abcdef" (the build part only when buildId is not empty).
std::string StampLine(const char* version, const char* buildId);
// The version a stamp line (first lines of `text`) names; valid = false when the file has none.
Version ReadStamp(const std::string& text);

struct Report {
    bool current = false;           // the file was already stamped by this build or a newer one: nothing done
    bool changed = false;           // the returned text differs from the input
    int turnedOff = 0;              // lines commented out
    int added = 0;                  // lines the migration wrote (for example MSC_PERFORMANCE_MODE=1 for an old MSC_PICTURE=performance)
    int unknown = 0;                // keys that start with MSC_ or NP_ and that the Settings screen and the rules do not know (informational; hand-written test keys are normal)
    std::vector<std::string> log;   // one line each, for log.txt
    std::string backupName;         // "np-player.before-1.3.txt"
};

// `version`: this build's, "1.3.0". Does not touch the file system.
std::string Migrate(const std::string& text, const char* version, const char* buildId, Report& report);
// The text with this build's stamp as its first line (an existing stamp line is replaced). The Settings screen's save calls it.
std::string Stamped(const std::string& text, const char* version, const char* buildId);

// "np-player.before-1.3.txt" for "1.3.0".
std::string BackupName(const char* version);
// The one line for the loading screen (an English key of np_text_strings.tsv), or null when nothing was done.
const char* LoadingLine(const Report& report);

}  // namespace NpSettingsGuard
