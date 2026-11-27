// The start check for Picture and Language changes: see np_change_guard.h.
#include "np_change_guard.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>

namespace NpChangeGuard {
namespace {

const char* const kWatched[] = {"MSC_VITA_RENDER", "MSC_FPS", "MSC_LOCK60", "MSC_VGX_MSAA", "MSC_DOF", "MSC_VITA_UPSCALE", "NP_WIDESCREEN", "MSC_HDTEX", "MSC_SHADOW_LIVE", "MSC_LANGUAGE", "NP_LANG"};

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

// The active key=value pairs of a file as the loader reads them (np_vita_main.cpp LoadEnvFile: a leading # is a comment, the first = splits, the last line of a key wins).
std::map<std::string, std::string> Active(const std::string& text) {
    std::map<std::string, std::string> out;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        const std::string line = Trim(text.substr(pos, end - pos));
        pos = end + 1;
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        out[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return out;
}

bool ParseNumber(const char*& p, unsigned long long& v) {
    while (*p == ' ') ++p;
    if (*p < '0' || *p > '9') return false;
    v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + static_cast<unsigned>(*p - '0');
        if (v > 0xFFFFFFFFull) return false;
        ++p;
    }
    return true;
}

}  // namespace

bool IsWatched(const char* key) {
    if (!key) return false;
    for (const char* k : kWatched)
        if (std::strcmp(k, key) == 0) return true;
    return false;
}

std::vector<std::string> WatchedLines(const std::string& settingsText) {
    std::vector<std::string> out;
    for (const auto& kv : Active(settingsText))   // (a std::map: sorted by key)
        if (IsWatched(kv.first.c_str())) out.push_back(kv.first + "=" + kv.second);
    return out;
}

uint32_t Fingerprint(const std::string& settingsText) {
    const std::vector<std::string> lines = WatchedLines(settingsText);
    if (lines.empty()) return 0;
    uint32_t h = 2166136261u;
    for (const std::string& l : lines) {
        for (unsigned char c : l) { h ^= c; h *= 16777619u; }
        h ^= '\n';
        h *= 16777619u;
    }
    return h == 0 ? 1u : h;
}

bool SwitchedOffByText(const std::string& settingsText) {
    const auto active = Active(settingsText);
    for (const char* kit : {"MSC_BOOT_MATCH", "MSC_INPUT_SCRIPT"}) {
        const auto it = active.find(kit);
        if (it != active.end() && !it->second.empty()) return true;
    }
    const auto it = active.find("MSC_CHANGE_GUARD");
    return it != active.end() && it->second == "0";
}

bool Parse(const std::string& text, Record& out) {
    const char* p = text.c_str();
    unsigned long long a, b, c;
    if (!ParseNumber(p, a) || !ParseNumber(p, b) || !ParseNumber(p, c)) return false;
    while (*p == ' ' || *p == '\n' || *p == '\r') ++p;
    if (*p != '\0' || c > 1000) return false;
    out.good = static_cast<uint32_t>(a);
    out.tried = static_cast<uint32_t>(b);
    out.tries = static_cast<int>(c);
    return true;
}

std::string Format(const Record& r) {
    return std::to_string(r.good) + " " + std::to_string(r.tried) + " " + std::to_string(r.tries) + "\n";
}

bool Guard::Reserve(const Record& next) {
    if (!files_.WriteGuard(Format(next))) return false;
    record_ = next;
    return true;
}

Guard::State Guard::Begin(bool switchedOff) {
    // A second call in this process (the settings screen saved and the start went on): the first call's reservation is taken back, the file is read again.
    if (state_ == State::kReserved) {
        if (files_.WriteGuard(Format(before_))) record_ = before_;
    }
    state_ = State::kOff;
    lines_.clear();
    fingerprint_ = 0;
    if (switchedOff) return state_;
    std::string settings;
    if (files_.ReadSettings(settings) == 2) return state_;   // a card error: nothing this start
    if (SwitchedOffByText(settings)) return state_;
    fingerprint_ = NpChangeGuard::Fingerprint(settings);
    if (fingerprint_ == 0) return state_;   // no watched line: the standard picture and language cannot be blamed, nothing is read or written
    std::string text;
    Record rec;
    const int got = files_.ReadGuard(text);
    if (got == 2) return state_;
    if (got == 0 && !Parse(text, rec)) rec = Record();   // a damaged file counts as none
    record_ = rec;
    if (fingerprint_ == rec.good) return state_;           // an ordinary start: nothing is written
    const int tries = rec.tried == fingerprint_ ? rec.tries : 0;
    before_ = rec;
    for (const std::string& l : WatchedLines(settings)) {
        if (!lines_.empty()) lines_ += ' ';
        lines_ += l;
    }
    if (tries >= kFallbackAfter) {
        state_ = State::kFallback;   // the standard lines for this start; the file is not rewritten
        return state_;
    }
    Record next = rec;
    next.tried = fingerprint_;
    next.tries = tries + 1;
    if (!Reserve(next)) return state_;   // could not write: the guard does nothing this start
    state_ = State::kReserved;
    return state_;
}

bool Guard::Cancel() {
    if (state_ != State::kReserved) return false;
    if (files_.WriteGuard(Format(before_))) record_ = before_;
    state_ = State::kCancelled;
    return true;
}

void Guard::Restore() {
    if (state_ != State::kCancelled) return;
    Record next = before_;
    next.tried = fingerprint_;
    next.tries = (before_.tried == fingerprint_ ? before_.tries : 0) + 1;
    if (Reserve(next)) state_ = State::kReserved;
}

void Guard::Failed() { Restore(); }

void Guard::Stable() {
    if (state_ != State::kReserved && state_ != State::kCancelled) return;
    Record next;
    next.good = fingerprint_;
    next.tried = fingerprint_;
    next.tries = 0;
    Reserve(next);
    state_ = State::kStable;
}

void Guard::SettingsSaved() {
    std::string text;
    Record rec;
    if (files_.ReadGuard(text) != 0 || !Parse(text, rec)) return;   // no file, a card error or a damaged file: nothing to clear
    if (rec.tries == 0) return;                                      // no failed start stands: nothing is written
    Record next = rec;
    next.tried = 0;
    next.tries = 0;
    if (!files_.WriteGuard(Format(next))) return;
    record_ = next;
    if (state_ == State::kReserved || state_ == State::kCancelled) { before_.tried = 0; before_.tries = 0; }   // (a later Cancel / second Begin must not write the old count back)
}

}  // namespace NpChangeGuard
