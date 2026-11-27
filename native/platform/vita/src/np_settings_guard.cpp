// The stale-settings guard: see np_settings_guard.h.
#include "np_settings_guard.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace NpSettingsGuard {
namespace {

const char* const kStampPrefix = "# np-player.txt written by ";

// What a rule does with a line: the key and the value test, and why. A value of "*" is any value.
struct Rule {
    const char* key;
    const char* values;   // '|'-separated, or "*"
    const char* why;
};

// Lines of an older build that would pin a behaviour 1.3 has replaced or switched on by itself. Ordered by key; the reasons are for the log (English).
// RENDER / MSC_VITA_MSAA: 1.0 and 1.1 PERFORMANCE and QUALITY wrote them; a fixed picture size bypasses the 1.3 dynamic resolution and its anti-aliasing and 60 fps rungs.
// DYNRES / DYNRES_MAX / DYNRES_FG / VGX_MSAA=0: tester lines that switch the ladder, its larger sizes, Prioritize Players or its multisampling off.
// FRAME_PACING=0: animations at half speed, "for finding problems"; the 1.3 screen hides the row.
// FPS=auto|unlocked: auto is the default now; MSC_FPS=30 is a real choice and is kept. (MSC_FPS=45, the FRAME RATE choice of the s29-f45 test builds, is gone: the plan reads it as 30; Smooth 60 uses the 45 cadence itself.)
// PICTURE=performance|quality: the hand-written selector expands to a fixed MSC_VITA_RENDER.
// PLAN=A: pins the old core plan and switches the 1.3 pipeline and its safety net off (Cn and S are ways out of a failed start and stay).
// 1.3 prune (the rows that went from the screen; a file that names one would hold the player in a choice no row shows): MSC_DOF=1 (the old way of capturing depth of field, the same picture,
// slower), MSC_FPS_REPLAY (1.3 holds replays and cut-scenes at 30 by itself), MSC_FX_LOWRES* (FULL SIZE and FASTER; the effects pass is automatic), MSC_VITA_ANISO (DISTANT TEXTURES is
// on at level 2 in 1.3, and 1.1 wrote level 3), NP_MENU_NAV=0 (the 1.3 screens need the D-pad and stick moves). MSC_FPS=30 and MSC_PERFORMANCE_MODE=1 stay: FRAME RATE 30 FPS and STADIUM CROWD HIDE are rows.
const Rule kRules[] = {
    {"MSC_DOF", "1", "the old way of capturing depth of field: the same picture, slower; 1.3 does it the cheap way"},
    {"MSC_DYNRES", "off|0", "dynamic resolution off holds 1.3's picture sizes and GPU timers back"},
    {"MSC_DYNRES_FG", "0", "turns off Prioritize Players, which is on in 1.3"},
    {"MSC_DYNRES_MAX", "*", "caps the larger sizes that 1.3's ladder uses when there is room"},
    {"MSC_FPS", "auto|unlocked", "1.3's default is AUTO; this value means something else now"},
    {"MSC_FPS_REPLAY", "*", "1.3 holds replays and cut-scenes at an even 30 by itself"},
    {"MSC_FRAME_PACING", "0", "animations at half speed; a 1.0/1.1 switch for finding problems"},
    {"MSC_FX_LOWRES", "0|off|4", "glow and haze at a fixed size; 1.3 chooses it by the load"},
    {"MSC_FX_LOWRES_MINPX", "*", "glow and haze at a fixed size; 1.3 chooses it by the load"},
    {"MSC_FX_LOWRES_SCENES", "*", "glow and haze at a fixed size; 1.3 chooses it by the load"},
    {"MSC_PICTURE", "performance|perf|fast|quality|high", "expands to a fixed MSC_VITA_RENDER, which bypasses 1.3's dynamic resolution"},
    {"MSC_PLAN", "A", "pins the old core plan; 1.3 starts in its own plan with a safety net"},
    {"MSC_VGX_MSAA", "0", "turns off 1.3's anti-aliasing rungs"},
    {"MSC_VITA_ANISO", "*", "distant textures are on in 1.3 (level 2); an older file pinned level 3 or off"},
    {"MSC_VITA_MSAA", "*", "1.0/1.1 anti-aliasing of every render target; 1.3 does it by the ladder and this switches that off"},
    {"MSC_VITA_RENDER", "*", "a fixed picture size written by 1.0/1.1 PERFORMANCE or QUALITY; bypasses 1.3's dynamic resolution"},
    {"NP_MENU_NAV", "0", "menus on the pointer only; 1.3's screens need the D-pad and stick moves"},
};

// Keys the Settings screen or the shipped example file name, and the lines the tools write: an unknown key is only counted. The list is the rows' keys and a few
// player-level ones; everything else that starts with MSC_ or NP_ is "unknown" here, which for a tester's file is normal.
const char* const kKnown[] = {
    "NP_DATA_DIR", "NP_WIDESCREEN", "NP_LANG", "NP_MENU_NAV", "NP_TOUCH_POINTER", "NP_POINTER_SPEED", "MSC_CONTROLS", "MSC_REAR_TOUCH", "MSC_TOUCH_SAVE", "MSC_TOUCH_METER",
    "MSC_TOUCH_SHAKE", "MSC_TOUCH_MINIGAMES", "MSC_MEGA_AIM", "MSC_MEGA_POINTER_SPEED", "MSC_LANGUAGE", "MSC_PERFORMANCE_MODE", "MSC_FPS", "MSC_LOCK60", "MSC_VITA_UPSCALE",
    "MSC_VITA_SHARPEN", "MSC_VITA_EDGE_AA", "MSC_VITA_ANISO", "MSC_UNLOCK_CAPTAINS", "MSC_UNLOCK_STADIUMS", "MSC_GAME_FIXES", "MSC_LAN_MODE", "MSC_LAN_DIALOG",
    "MSC_LAN_INPUT_DELAY", "MSC_NET_RELAY_ONLY", "MSC_RULE_NOMEGA", "MSC_REDUCED_MOTION", "MSC_HDTEX", "MSC_PLAN", "MSC_FPS_BETWEEN", "MSC_FPS_REPLAY", "MSC_ONLINE_NAME",
    "MSC_CADENCE45",   // (1.3: the developer switch for the 45 cadence alone; no Settings row)
};

bool Known(const std::string& key) {
    for (const char* k : kKnown)
        if (key == k) return true;
    return false;
}

bool ValueMatches(const char* values, const std::string& value) {
    if (std::strcmp(values, "*") == 0) return true;
    const char* p = values;
    while (*p) {
        const char* end = std::strchr(p, '|');
        const size_t n = end ? static_cast<size_t>(end - p) : std::strlen(p);
        if (value.size() == n && value.compare(0, n, p, n) == 0) return true;
        if (!end) break;
        p = end + 1;
    }
    return false;
}

const Rule* FindRule(const std::string& key, const std::string& value) {
    for (const Rule& r : kRules)
        if (key == r.key && ValueMatches(r.values, value)) return &r;
    return nullptr;
}

void Trim(std::string& s) {
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t')) ++b;
    size_t e = s.size();
    while (e > b && (s[e - 1] == '\r' || s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    s = s.substr(b, e - b);
}

// A NAME=VALUE line (after trimming; not a comment); false otherwise.
bool Assignment(const std::string& line, std::string* key, std::string* value) {
    std::string t = line;
    Trim(t);
    const size_t eq = t.find('=');
    if (t.empty() || t[0] == '#' || eq == std::string::npos || eq == 0) return false;
    *key = t.substr(0, eq);
    *value = t.substr(eq + 1);
    Trim(*key);
    Trim(*value);
    return !key->empty();
}

bool IsStampLine(const std::string& line) { return line.compare(0, std::strlen(kStampPrefix), kStampPrefix) == 0; }

std::vector<std::string> Lines(const std::string& text) {
    std::vector<std::string> out;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
        out.push_back(line);
        at = end + 1;
    }
    return out;
}

bool Has(const std::vector<std::string>& lines, const char* key) {
    for (const std::string& l : lines) {
        std::string k, v;
        if (Assignment(l, &k, &v) && k == key) return true;
    }
    return false;
}

}  // namespace

Version ParseVersion(const char* text) {
    Version v;
    if (!text || !*text) return v;
    int parts[3] = {0, 0, 0};
    int n = 0;
    const char* p = text;
    while (*p) {
        if (*p < '0' || *p > '9' || n >= 3) return Version();
        int value = 0, digits = 0;
        while (*p >= '0' && *p <= '9') {
            value = value * 10 + (*p - '0');
            if (++digits > 6) return Version();
            ++p;
        }
        parts[n++] = value;
        if (*p == '.') {
            ++p;
            if (!*p) return Version();
        } else if (*p && *p != ' ') {
            return Version();
        } else break;
    }
    v.major = parts[0];
    v.minor = parts[1];
    v.patch = parts[2];
    v.valid = n >= 1;
    return v;
}

int Compare(const Version& a, const Version& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return 0;
}

std::string StampLine(const char* version, const char* buildId) {
    std::string s = std::string(kStampPrefix) + (version ? version : "?");
    if (buildId && *buildId) s += std::string(" build ") + buildId;
    return s;
}

Version ReadStamp(const std::string& text) {
    // the stamp is a comment among the first lines (a hand-edited file may have put a line above it)
    const std::vector<std::string> lines = Lines(text);
    for (size_t i = 0; i < lines.size() && i < 8; ++i)
        if (IsStampLine(lines[i])) return ParseVersion(lines[i].c_str() + std::strlen(kStampPrefix));
    return Version();
}

std::string BackupName(const char* version) {
    const Version v = ParseVersion(version);
    char name[64];
    std::snprintf(name, sizeof name, "np-player.before-%d.%d.txt", v.major, v.minor);
    return name;
}

std::string Stamped(const std::string& text, const char* version, const char* buildId) {
    std::string out = StampLine(version, buildId) + "\n";
    for (const std::string& line : Lines(text)) {
        if (IsStampLine(line)) continue;
        out += line + "\n";
    }
    return out;
}

std::string Migrate(const std::string& text, const char* version, const char* buildId, Report& report) {
    report = Report();
    const Version mine = ParseVersion(version);
    report.backupName = BackupName(version);
    const Version stamp = ReadStamp(text);
    // The rules name lines that 1.0 and 1.1 wrote and that 1.3 replaced. A file stamped by 1.3.0 or later was written by a screen that offers those very lines as choices
    // (RESOLUTION SCREEN / SMALLER / SMALLEST = MSC_VITA_RENDER, DYNRES OFF, ANTI-ALIASING OFF, PLAN A...), so a later build (1.3.1, 1.4) must not take them away again.
    const Version rulesSince = ParseVersion("1.3.0");
    if (mine.valid && stamp.valid && (Compare(stamp, mine) >= 0 || Compare(stamp, rulesSince) >= 0)) {
        report.current = true;
        // (informational only: a count of keys nobody knows; never changes the file)
        return text;
    }
    const std::vector<std::string> lines = Lines(text);
    std::string out = StampLine(version, buildId) + "\n";
    std::string appended;
    char note[256];
    for (const std::string& line : lines) {
        if (IsStampLine(line)) continue;
        std::string key, value;
        if (!Assignment(line, &key, &value)) {
            out += line + "\n";
            continue;
        }
        const Rule* rule = FindRule(key, value);
        if (!rule) {
            if ((key.compare(0, 4, "MSC_") == 0 || key.compare(0, 3, "NP_") == 0) && !Known(key)) {
                ++report.unknown;
                if (report.unknown <= 6) {
                    std::snprintf(note, sizeof note, "settings: %s is not a Settings-screen key (kept; a tester's key is fine, a typo does nothing)", key.c_str());
                    report.log.push_back(note);
                }
            }
            out += line + "\n";
            continue;
        }
        out += "# 1.3 turned off: " + key + "=" + value + "   (" + rule->why + ")\n";
        ++report.turnedOff;
        std::snprintf(note, sizeof note, "settings: turned off %s=%s (%s)", key.c_str(), value.c_str(), rule->why);
        report.log.push_back(note);
        // A hand-written performance preset also meant "no crowd": keep that part, the one that still means the same.
        if (key == "MSC_PICTURE" && (value == "performance" || value == "perf" || value == "fast") && !Has(lines, "MSC_PERFORMANCE_MODE")) {
            appended += "MSC_PERFORMANCE_MODE=1\n";
            ++report.added;
            report.log.push_back("settings: kept the crowd setting of that preset: MSC_PERFORMANCE_MODE=1");
        }
    }
    out += appended;
    report.changed = out != text;
    std::snprintf(note, sizeof note, "settings: the file had %s: %d line(s) turned off, %d added, %d unknown key(s); the old file is np-player.before-%d.%d.txt",
                  stamp.valid ? "an older stamp" : "no stamp (1.0 or 1.1)", report.turnedOff, report.added, report.unknown, mine.major, mine.minor);
    report.log.insert(report.log.begin(), note);
    return out;
}

const char* LoadingLine(const Report& report) {
    if (report.current || !report.changed) return nullptr;
    return report.turnedOff > 0 ? "OLD SETTINGS UPDATED FOR 1.3. BACKUP KEPT." : nullptr;
}

}  // namespace NpSettingsGuard
