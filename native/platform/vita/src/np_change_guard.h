// The start check for Picture and Language changes (startcheck19). A player who changes a Picture row (render size, frame rate, anti-aliasing, depth of field, widescreen,
// textures, shadows) or the Language row can make the game fail to start; the core plan guard (vita_plan.cpp, np-plan.txt) steps the threads down for that, but it cannot
// know the cause was a setting. This guard has its own small file, ux0:data/mscharged/np-change.txt, and one rule (the sister port's):
//   a change of those lines is "tried"; if the game does not reach a stable point on two starts in a row, the next start uses the standard lines for those rows (the watched
//   lines of np-player.txt are not read, the file itself is not rewritten) and the loading screen says so. Nothing else changes.
// The file holds three decimal numbers on one line: good tried tries
//   good    fingerprint of the watched lines that last got through a start (0 = no watched line: the standard picture and language)
//   tried   fingerprint of the watched lines being tried
//   tries   starts of `tried` that have not got through
// It never touches np-player.txt, writes nothing on a start whose watched lines equal `good` (an ordinary start), and is off for the console test kit (MSC_BOOT_MATCH or
// MSC_INPUT_SCRIPT set, in the file or in the environment) and with MSC_CHANGE_GUARD=0. A card error reading or writing the file means the guard does nothing this start.
// Portable code (no console calls; the file system is a small interface): the host test is tests/test_change_guard.cpp; the console glue is np_change_guard_vita.cpp.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace NpChangeGuard {

constexpr int kFallbackAfter = 2;   // earlier starts of the same lines that did not get through; the next start uses the standard ones

// The keys of the Picture class and the Language row (np_settings.cpp kTable). Not MSC_PLAN (the way out), not the live rows, not controls.
bool IsWatched(const char* key);

// One active (not commented) watched line per key, the last one of a key winning as the loader does (setenv with overwrite), sorted by key: "MSC_FPS=30".
std::vector<std::string> WatchedLines(const std::string& settingsText);
// 32-bit FNV-1a of the watched lines; 0 when there is none.
uint32_t Fingerprint(const std::string& settingsText);
// Kit runs and the switch: the active value of MSC_BOOT_MATCH / MSC_INPUT_SCRIPT is non-empty, or MSC_CHANGE_GUARD is 0, in the text.
bool SwitchedOffByText(const std::string& settingsText);

struct Record {
    uint32_t good = 0;
    uint32_t tried = 0;
    int tries = 0;
};
bool Parse(const std::string& text, Record& out);   // false: not three numbers
std::string Format(const Record& r);                // "good tried tries\n"

// The card. Read returns 0 = read, 1 = no such file, 2 = error. Write returns true when the new content is in place (the caller's writer is atomic).
class Files {
public:
    virtual ~Files() {}
    virtual int ReadSettings(std::string& out) = 0;   // np-player.txt
    virtual int ReadGuard(std::string& out) = 0;      // np-change.txt
    virtual bool WriteGuard(const std::string& text) = 0;
};

class Guard {
public:
    enum class State { kOff, kReserved, kCancelled, kFallback, kStable };
    explicit Guard(Files& files) : files_(files) {}

    // At start, after the older-version guard and before the file is read. `switchedOff`: the caller's own reasons (the environment). Safe to call again (a settings
    // screen that saved and went on in this process): an unresolved reservation of the first call is taken back first.
    State Begin(bool switchedOff);
    State state() const { return state_; }
    bool Fallback() const { return state_ == State::kFallback; }
    // The loader must not apply this key (the standard one is used instead).
    bool Skips(const char* key) const { return state_ == State::kFallback && IsWatched(key); }
    // The lines that were not applied, for the log ("MSC_VITA_RENDER=480x272 ...", at most `max` bytes).
    const std::string& Lines() const { return lines_; }
    uint32_t Fingerprint() const { return fingerprint_; }
    const Record& record() const { return record_; }

    // The plan guard's three: an intentional leave of the start (settings screen, restart) is not a failed start; the restart that failed arms it again; a crash or stall
    // before the stable point keeps the reservation (re-arms it when it was cancelled).
    bool Cancel();
    void Restore();
    void Failed();
    // 300 frames presented, or a clean quit after 120: the lines got through.
    void Stable();
    // r16 (fable-da21 F2): a settings screen saved np-player.txt (or RESET TO DEFAULTS was chosen): the failed-start count is cleared, so a choice saved again is tried
    // again (two failed starts no longer shut the same Picture and Language lines out for ever). Nothing is written when no count stands.
    void SettingsSaved();

private:
    bool Reserve(const Record& next);
    Files& files_;
    State state_ = State::kOff;
    Record record_;      // what the card holds (as far as this guard knows)
    Record before_;      // what it held before this start's reservation
    uint32_t fingerprint_ = 0;
    std::string lines_;
};

}  // namespace NpChangeGuard
