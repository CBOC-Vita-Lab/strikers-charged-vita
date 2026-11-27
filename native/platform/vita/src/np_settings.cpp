// The model and the drawing of the Settings screen: see np_settings.h.
#include "np_settings.h"
#include "np_picture_presets.h"
#include "np_build_identity.h"
#include "vita_plan_defaults.h"

#include "vita_font.h"

#include "../../sdk/input/np_control_table.h"
#include "../../sdk/include/np/np_text.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace NpSettings {
namespace {

// ---- the options --------------------------------------------------------------------------------------------------------------------------------------------
// A choice with an empty list writes nothing: it is the build's own behaviour. These rows are the player's settings; the game shows them in its own menu (OPTIONS > VITA SETTINGS
// and the pause menu's VITA SETTINGS, platform/sdk/net/np_native_settings.cpp, which names the rows it shows and in what order). The pixel screen drawn below shows only the
// SAFE START page (hold L + R at start-up: the way out when a setting stops the game from starting) and the ONLINE page (the account and the reports, from the game's menu).
// 1.3 prune (owner, 15 Nov: "remove any settings that don't make sense anymore or aren't needed") took the table to 29 rows; set18 (owner, 23 Nov: "Remove any settings that aren't
// needed or are just pure negative to the experience. Simplify where able"; 20:36 "Sure, if they don't help anything") to 20: RESOLUTION, ANTI-ALIASING, INPUT DELAY, TOUCH POINTER, LIVE SHADOWS and PICTURE FORMAT went, SMOOTH 60 MODE became a choice of
// FRAME RATE, TOUCH MEGA SAVE a choice of MEGA SAVE AIM, MULTIPLAYER LINK (the game itself asks ONLINE / LOCAL and NEARBY / SAME WI-FI every time; its NEARBY HOST / JOIN choice, NEARBY PAIRING, went in mp24), and the
// purely worse choices left their rows (ORIGINAL GAME FIXES OFF, MEGA SAVE SPEED ORIGINAL and 2x). A removed row's keys keep their readers (kRetiredKeys below); a purely worse value that an
// earlier 1.3 screen wrote into its own block is ignored at start-up with a log line (kObsolete below), so an updated player is not stuck with it.
// Help: one line per row (help[1] stays empty: the pixel screen's second line is the reason a row does nothing, or the status line).

// FRAME RATE (set18: one row for what FRAME RATE and SMOOTH 60 MODE did between them). SMOOTH 60 is MSC_LOCK60=1 (docs/TECHNICAL.md "LOCKED 60"): 60 first, the picture size and
// effects give way, 30 only as a relief (with MSC_LOCK60_HOLD, the plan's default, the 60 is judged on the delivered rate). 30 LOCKED is MSC_FPS=30: every picture shown for exactly
// two refreshes. AUTO (MSC_FPS and MSC_LOCK60 unset) is the 1.2 governor: up to 60 by scene. The first choice writes nothing and must be the build's own mode, so the row follows the
// one switch VitaPlanDefaults::kLock60 (vita_plan_defaults.h): while it is false AUTO is that choice and SMOOTH 60 writes MSC_LOCK60=1; once it is true the row is SMOOTH 60 / 30 LOCKED
// (AUTO goes: the owner rejected modes that are only worse) and a file's MSC_LOCK60=1 reads as the default (IsBuildDefault). The 45 cadence as a choice of its own was rejected on
// 18 Nov; MSC_FPS=45 is read as 30 by the plan. MSC_FPS=60|unlocked exist for testing by hand (a file with them reads CUSTOM).
constexpr Option kFrameRateAuto = {"FRAME RATE",
                                   {"SMOOTH 60 aims for 60 and gives way in heavy scenes. 30 LOCKED: always 30.", ""},
                                   0,
                                   {{"AUTO", {}}, {"SMOOTH 60", {{"MSC_LOCK60", "1"}}}, {"30 LOCKED", {{"MSC_FPS", "30"}}}}};
constexpr Option kFrameRateSmooth = {"FRAME RATE",
                                     {"SMOOTH 60 aims for 60 and gives way in heavy scenes. 30 LOCKED: always 30.", ""},
                                     0,
                                     {{"SMOOTH 60 (DEFAULT)", {}}, {"30 LOCKED", {{"MSC_FPS", "30"}}}}};

constexpr Option kTable[] = {
    // (called PERFORMANCE MODE in 1.0-1.2; the key, its readers and the files are unchanged)
    {"STADIUM CROWD",
     {"SHOW draws the crowd in the stands and the people around the pitch.", ""},
     0,
     {{"SHOW", {}}, {"HIDE", {{"MSC_PERFORMANCE_MODE", "1"}}}}},
    VitaPlanDefaults::kLock60 ? kFrameRateSmooth : kFrameRateAuto,
    // MSC_HDTEX=0 ignores ux0:data/mscharged/hdtex.pack; without the pack the row does nothing.
    {"HD CHARACTERS",
     {"Sharper player and ball textures from hdtex.pack, if that file is there.", ""},
     0,
     {{"ON", {}}, {"OFF", {{"MSC_HDTEX", "0"}}}}},
    // MSC_DOF (depth of field): ship26 (26 Nov, the owner's 22:36 rule): the build sets 0 (vita_plan.cpp): no blur in cut-scenes, the cheapest. ON writes lazy: the soft blur, the
    // half-size capture only on the frames that draw it. A look choice; the simulation does not see it. ("ON (AS GAME)", MSC_DOF=1, went in the 1.3 prune: a 1.0-1.2 file naming it
    // is CUSTOM; before ship26 the first choice was ON and OFF wrote 0: a file naming MSC_DOF=lazy is now the ON choice, a file naming 0 the default.)
    {"DEPTH OF FIELD",
     {"ON: the soft background blur in cut-scenes, drawn the cheap way.", ""},
     0,
     {{"OFF (DEFAULT)", {}}, {"ON", {{"MSC_DOF", "lazy"}}}}},
    // PIPE (wave 5): the player build starts in core plan C with a safety net (np-plan.txt steps down to A and then S after a failed start); MSC_PLAN=A|S picks one by hand and switches
    // the net off. A way out of a failed start, so it stands on the SAFE START page (hold L + R at start-up) and not in the game's menu.
    {"CORE PLAN",
     {"Change it only if the game will not start: S is the safest. Next start.", ""},
     0,
     {{"AUTO", {}}, {"C NO OVERLAP", {{"MSC_PLAN", "Cn"}}}, {"PLAN A", {{"MSC_PLAN", "A"}}}, {"PLAN S (SAFEST)", {{"MSC_PLAN", "S"}}}}},
    // The upscale filter (wave 5 LOOK): SMOOTH is bilinear with a sharpening that follows the stretch, CRISP sharp bilinear, OFF the plain blit. (1.3 prune: the lab variants CRISP+,
    // SMOOTH NO EDGE AA, FAST and SMOOTH FULL went; MSC_VITA_SHARPEN and MSC_VITA_EDGE_AA are retired keys that stay with a CUSTOM row.)
    {"SHARPNESS",
     {"Sharpens the picture when it is scaled up to the screen.", ""},
     0,
     {{"SMOOTH", {}}, {"OFF", {{"MSC_VITA_UPSCALE", "off"}}}, {"CRISP", {{"MSC_VITA_UPSCALE", "sharp"}}}}},
    // MSC_NET_RELAY_ONLY=1 (vlink_online.cpp BuildConfig): online play goes through the relay only, no direct path, and no address is shown to the partner. A privacy choice.
    {"CONNECTION",
     {"RELAY ONLY hides your address from the other player but adds delay.", ""},
     0,
     {{"AUTO", {}}, {"RELAY ONLY", {{"MSC_NET_RELAY_ONLY", "1"}}}}},
    // STR-008: MSC_RULE_NOMEGA=1 removes Mega Strikes. It changes the match: every console of a multiplayer match must use the same value (so_net.cpp compares it), ranked online
    // ignores it.
    {"MEGA STRIKES",
     {"OFF removes Mega Strikes: a charged captain shot is a normal shot.", ""},
     0,
     {{"ON (STANDARD)", {}}, {"OFF (BOTH MUST MATCH)", {{"MSC_RULE_NOMEGA", "1"}}}}},
    // STR-004 / SC06: AUTOMATIC writes nothing and the start follows the Vita's own language (vita_plan.cpp: Spanish or French; anything else English); ENGLISH writes
    // MSC_LANGUAGE=en for a player who wants English on such a Vita. Read once at start-up.
    {"LANGUAGE",
     {"The menu and game texts. AUTOMATIC follows the Vita's language.", ""},
     0,
     {{"AUTOMATIC", {}}, {"ENGLISH", {{"MSC_LANGUAGE", "en"}}}, {"SPANISH", {{"MSC_LANGUAGE", "es"}}}, {"FRENCH", {{"MSC_LANGUAGE", "fr"}}}}},
    // MSC_REDUCED_MOTION=1 (np/comfort.h NpShakeScale): the camera shake on hits and Mega Strikes moves a quarter as far. A comfort choice; applies at once.
    {"REDUCED CAMERA SHAKE",
     {"Camera shake on hits and Mega Strikes is a quarter as strong.", ""},
     0,
     {{"OFF", {}}, {"ON (QUARTER SHAKE)", {{"MSC_REDUCED_MOTION", "1"}}}}},
    // 17 Oct (W5 ITEM): DRAWING ONLY is the Vita default (VitaPlan sets MSC_GAME_FIXES=net,electro when the file does not name it). ALL adds the Boo fix, which changes the match:
    // both Vitas must use the same setting. set18: OFF (AS RETAIL, "none": the resting goal nets and the dark electrocution picture back) went as purely worse (kObsolete).
    {"ORIGINAL GAME FIXES",
     {"Fixes retail bugs: resting goal nets, electrocution picture, Boo glitch.", ""},
     0,
     {{"DRAWING ONLY", {}}, {"ALL (WITH BOO FIX)", {{"MSC_GAME_FIXES", "all"}}}}},
    // W5 CAM (np/unlocks.h): the content the game locks behind the cups and awards, opened by the game's own unlock predicates, never by the save's cup bits. Both default OFF.
    {"UNLOCK ALL CHARACTERS",
     {"ON: Bowser Jr., Diddy Kong and Petey Piranha without winning the cups.", ""},
     0,
     {{"OFF", {}}, {"ON", {{"MSC_UNLOCK_CAPTAINS", "1"}}}}},
    {"UNLOCK ALL STADIUMS",
     {"ON: all 6 locked stadiums without winning the cups and awards.", ""},
     0,
     {{"OFF", {}}, {"ON", {{"MSC_UNLOCK_STADIUMS", "1"}}}}},
    // W5 CTL: the button layout, the rear touchpad and each touch feature. (The old MEGA STRIKE TOUCH row wrote MSC_TOUCH_MINIGAMES=0, which still switches all three minigames
    // off when it is in the file.)
    {"CONTROLS",
     {"MODERN: Cross pass, Circle shoot, Square lob/big hit, Triangle item, R turbo", ""},
     0,
     {{"MODERN", {}}, {"CLASSIC", {{"MSC_CONTROLS", "classic"}}}}},
    // WITH MODERN writes nothing (unset: on with MODERN, off with CLASSIC (np_controls.h)). Applies at once.
    {"REAR TOUCH",
     {"Tap the back: right half switches player, left half cycles items.", ""},
     0,
     {{"WITH MODERN", {}}, {"ON", {{"MSC_REAR_TOUCH", "1"}}}, {"OFF", {{"MSC_REAR_TOUCH", "0"}}}}},
    // set18: one row for how a Mega Save is aimed (TOUCH MEGA SAVE and MEGA SAVE AIM were two rows for it, the second doing nothing while the first was OFF). FRONT TOUCH: touch to
    // aim and block; REAR + CROSS: one finger on the back aims, Cross or R blocks (MSC_MEGA_AIM=rear); STICK ONLY: no touch, either stick aims (MSC_TOUCH_SAVE=0, the old TOUCH MEGA
    // SAVE OFF: a file holding it reads STICK ONLY).
    {"MEGA SAVE AIM",
     {"How you aim Mega Saves: front touch, rear touch + Cross, or the stick only.", ""},
     0,
     {{"FRONT TOUCH", {}}, {"REAR + CROSS", {{"MSC_MEGA_AIM", "rear"}}}, {"STICK ONLY", {{"MSC_TOUCH_SAVE", "0"}}}}},
    // set18: ORIGINAL (MSC_MEGA_POINTER_SPEED=1, the Wii pointer's speed) and 2x went as only worse: the glove did not reach the far side in time (kObsolete); 4x stays for a
    // player who wants it to reach sooner.
    {"MEGA SAVE SPEED",
     {"How fast the stick moves the glove in Mega Saves; 3x reaches the far side.", ""},
     0,
     {{"3x", {}}, {"4x", {{"MSC_MEGA_POINTER_SPEED", "4"}}}}},
    {"TOUCH MEGA STRIKE",
     {"Your Mega Strike: hold a finger on the screen to stop the meter.", ""},
     0,
     {{"ON", {}}, {"OFF", {{"MSC_TOUCH_METER", "0"}}}}},
    {"TOUCH SHAKEDOWN",
     {"Their Mega Strike: swipe the screen to shake their meter.", ""},
     0,
     {{"ON", {}}, {"OFF", {{"MSC_TOUCH_SHAKE", "0"}}}}},
    // 1.3: a 1-10 scale. 5 is the game's own speed and writes nothing; 1, 3, 8 and 10 are the old SLOW 0.5, SLOWER 0.75, FASTER 1.5 and FAST 2, so a 1.0-1.2 file reads as the same step.
    {"POINTER SPEED",
     {"How fast the pointer moves in menus when steered with the right stick.", ""},
     4,
     {{"1 (SLOWEST)", {{"NP_POINTER_SPEED", "0.5"}}}, {"2", {{"NP_POINTER_SPEED", "0.6"}}}, {"3", {{"NP_POINTER_SPEED", "0.75"}}}, {"4", {{"NP_POINTER_SPEED", "0.85"}}},
      {"5 (NORMAL)", {}}, {"6", {{"NP_POINTER_SPEED", "1.15"}}}, {"7", {{"NP_POINTER_SPEED", "1.3"}}}, {"8", {{"NP_POINTER_SPEED", "1.5"}}},
      {"9", {{"NP_POINTER_SPEED", "1.75"}}}, {"10 (FASTEST)", {{"NP_POINTER_SPEED", "2"}}}}},

    // ---- NEW ROWS GO HERE ----
    // Append, never insert: this order is the order of the lines in the file. The first choice writes nothing and is the build's own default (see np_settings.h), so a switch whose
    // default changes later reaches every file that never moved it. A new row is on no page of the pixel screen: name it in np_native_settings.cpp kRows to show it in the game.
};
constexpr int kOptionTotal = int(sizeof(kTable) / sizeof(kTable[0]));
static_assert(kOptionTotal <= kMaxOptions, "raise kMaxOptions");

// set18: the pixel screen has two pages. SAFE START is what holding L + R at start-up opens (the fatal screen names it): the core plan, RESET TO DEFAULTS and the facts a problem report
// needs. ONLINE is what the game's menu opens (VITA SETTINGS > ONLINE ACCOUNT): the online name and the three report / account actions, which need this screen's own dialogs. The last
// rows of every page are RESET and the two SAVE rows. Option rows not named here are on no page (they are the game menu's). kMaxPageRows is the longest list a page may name.
constexpr int kMaxPageRows = 24;
struct PageTable {
    const char* name;
    const char* rows[kMaxPageRows];   // a title of kTable or of an information line (kInfo) or of an action (kActions)
};
constexpr PageTable kPages[] = {
    {"SAFE START", {"CORE PLAN", "VERSION", "BUILD", "SETTINGS FILE", "GAME FILES"}},
    {"ONLINE", {"ONLINE NAME", "SEND A REPORT", "SAVE A REPORT", "DELETE MY ONLINE ACCOUNT"}},
};

// The information lines: what the row says and two help lines (76 characters at most), for kRowInfo.
struct Info {
    const char* title;
    const char* help[2];
};
constexpr Info kInfo[] = {
    {"ONLINE NAME", {"Name on the online rankings. The Vita keyboard asks for it the first time", "you pick Online. To change it, delete np-data/online_name.txt."}},
    {"VERSION", {"The version of this game, as in the log and in the LiveArea.", ""}},
    {"BUILD", {"The build's identity: the same on two Vitas that play each other.", "Quote it when you report a problem; log.txt starts with it."}},
    {"SETTINGS FILE", {"ux0:data/mscharged/np-player.txt. An older file is kept as", "np-player.before-1.3.txt when 1.3 first updates it."}},
    {"GAME FILES", {"The extracted disc (USA, R4QE01) goes in ux0:data/mscharged/game/sys and", "game/files. Compiler: libshacccg.suprx in ur0:data or data/external."}},
};
constexpr int kInfoTotal = int(sizeof(kInfo) / sizeof(kInfo[0]));
// wave 11 mpdiag18: a row that DOES something when Cross is pressed (kRowAction) instead of changing a choice. The console glue (np_vita_settings.cpp) runs it by title; the table only
// names it and says what it does. One row, one function: the page it stands on can move (kPages) without touching the action.
constexpr Info kActions[] = {
    {"SAVE A REPORT", {"Writes ux0:data/mscharged/mp-report.txt about this game's multiplayer.", "No names, addresses or codes in it. Attach it to a bug report."}},
    {"SEND A REPORT", {"Sends a problem report to the online service. You see what first.", "3 a day at most. Needs an online account."}},
    {"DELETE MY ONLINE ACCOUNT", {"Removes your name, friends and reports from the online service.", "This Vita forgets its online name. It cannot be undone."}},
};
constexpr int kActionTotal = int(sizeof(kActions) / sizeof(kActions[0]));
constexpr int kActionBase = 1000;   // PageIndex stores an action row as -(kActionBase + i) (information lines are -1 - i, options are >= 0)
char g_infoValue[kInfoTotal][48];
constexpr int kPageTotal = int(sizeof(kPages) / sizeof(kPages[0]));
static_assert(kPageTotal >= 2, "the pages");

const char* const kBlockStart = "# ---- Settings screen (LiveArea > Settings): the lines below are written by it; the rest of this file is kept ----";
const char* const kBlockEnd = "# ---- end of the Settings screen's lines ----";

// Keys whose row or choice is gone from the screen (1.3 prune and set18; the rows are in docs/SETTINGS.md "Rows that went"). Every one keeps its reader and the kit's files set it:
// nothing is removed at start-up. What the screen does with them: Load ignores them (no row owns them); Serialize drops the line when it stood inside the block this screen writes
// (only the screen writes there, so a player's own line is never touched) and anywhere once RESET TO DEFAULTS was used; a line outside the block, without a reset, stays byte for byte
// (the kit's files and a hand-written file). "stays with" names the row whose variant the key belongs to: while that row reads CUSTOM the key is kept with it.
struct RetiredKey {
    const char* key;
    const char* staysWith;   // an option's title or null
};
constexpr RetiredKey kRetiredKeys[] = {
    {"MSC_FPS_BETWEEN", nullptr},        // FRAME RATE "UP TO 60": the build's default (pace)
    {"MSC_FPS_REPLAY", nullptr},         // REPLAYS & CUT-SCENES: the cadence holds held scenes at 30 by itself
    {"MSC_PICTURE_HOLD", nullptr},       // FULL PICTURE: the default; the plan lets go of it under 30 FPS, SMOOTH 60 MODE and a fixed size
    {"MSC_PICTURE_HOLD_MSAA", nullptr},  // ANTI-ALIASING "4X"
    {"MSC_DYNRES", nullptr},             // DYNAMIC RESOLUTION
    {"MSC_DYNRES_MAX", nullptr},         // SHARPER WHEN POSSIBLE
    {"MSC_DYNRES_FG", nullptr},          // PRIORITIZE PLAYERS
    {"MSC_DYNRES_MIN60", nullptr},       // SMALLEST PICTURE AT 60
    {"MSC_FX_LOWRES", nullptr},          // EFFECTS QUALITY
    {"MSC_FX_LOWRES_SCENES", nullptr},
    {"MSC_FX_LOWRES_MINPX", nullptr},
    {"MSC_VITA_ANISO", nullptr},         // DISTANT TEXTURES
    {"MSC_FRAME_PACING", nullptr},       // FRAME PACING
    {"MSC_FX_GLOW", nullptr},            // GLOW
    {"MSC_SHADOW_EVERY", nullptr},       // REPLAY SHADOWS
    {"MSC_REPLAY_FARLOD", nullptr},      // REPLAY FAR PLAYERS
    {"MSC_VGX_GXM_DIRECT", nullptr},     // GXM DIRECT
    {"MSC_VITA_CORE3", nullptr},         // FOURTH CORE
    {"MSC_VITA_CORE3_SEND", nullptr},
    {"MSC_VGX_LODBIAS", nullptr},        // TEXTURE DETAIL
    {"MSC_VITA_SHAPE", nullptr},         // PICTURE SHAPE
    {"MSC_VGX_HUDNATIVE", nullptr},      // NATIVE HUD
    {"MSC_MENU_STILL", nullptr},         // STILL BACKDROPS
    {"MSC_PAUSE_STILL", nullptr},
    {"MSC_FX_GOV", nullptr},             // EFFECTS UNDER LOAD
    {"MSC_FX_GOV_MEGA", nullptr},
    {"MSC_SHADOW_PLAY", nullptr},        // MATCH SHADOWS
    {"MSC_VGX_LA88", nullptr},           // TEXTURE MEMORY
    {"MSC_VGX_CMPRA", nullptr},
    {"MSC_VGX_TEX16", nullptr},
    {"NP_MENU_NAV", nullptr},            // MENU STICK (the native screen and the popups need the D-pad and stick moves)
    {"MSC_VITA_SHARPEN", "SHARPNESS"},   // SHARPNESS "CRISP+"
    {"MSC_VITA_EDGE_AA", "SHARPNESS"},   // SHARPNESS "SMOOTH NO EDGE AA"
    // set18 (owner, 23 Nov)
    {"MSC_VITA_RENDER", nullptr},        // RESOLUTION (SMALLER 864X488: a fixed size never adapts and turns the dynamic resolution, its smoothing and SMOOTH 60 off)
    {"MSC_VGX_MSAA", nullptr},           // ANTI-ALIASING (OFF: the picture already gives the smoothing up first when frames run late)
    {"MSC_LAN_INPUT_DELAY", nullptr},    // INPUT DELAY (a tuning knob: the game's own delay adapts to the link)
    {"NP_TOUCH_POINTER", nullptr},       // TOUCH POINTER
    {"MSC_LAN_MODE", nullptr},           // MULTIPLAYER LINK (the game's menu asks the link every time; the key only chose the button lit first)
    {"MSC_LAN_DIALOG", nullptr},         // NEARBY PAIRING (mp24, owner: removed unless it helps; no console run showed HOST / JOIN fixing anything, the system dialog's default mode is what the retail game uses; a hand line MSC_LAN_DIALOG=create|join outside the block still works)
    {"MSC_SHADOW_LIVE", nullptr},        // LIVE SHADOWS (FULL: the original shadow pictures in live play, a pass more for nothing a player asked for)
    {"NP_WIDESCREEN", nullptr},          // PICTURE FORMAT (4:3 BOX: bars on the Vita's wide screen, nothing gained)
};

// set18: values an earlier 1.3 screen wrote into its own block that are only worse for the player, and that no row offers any more. At start-up the loader skips such a line (and logs
// why), the screen reads the row at its default, and the next save drops the line (WithoutObsolete). Only inside the screen's block: the kit's files and a player's own lines (outside
// it) are applied as written. The 1.0-1.2 files (no stamp or an older one) are handled before this by np_settings_guard.cpp, which turns such lines off with the backup kept.
struct Obsolete {
    const char* key;
    const char* values;   // '|'-separated, or "*"
    const char* why;
};
constexpr Obsolete kObsolete[] = {
    {"MSC_VITA_RENDER", "*", "RESOLUTION is gone: a fixed size turned the dynamic resolution, its smoothing and SMOOTH 60 off"},
    {"MSC_VGX_MSAA", "0", "ANTI-ALIASING OFF is gone: the picture gives the smoothing up by itself when frames run late"},
    {"MSC_GAME_FIXES", "none", "ORIGINAL GAME FIXES OFF is gone: it brought the retail drawing bugs back"},
    {"MSC_MEGA_POINTER_SPEED", "1|2", "MEGA SAVE SPEED ORIGINAL / 2x are gone: the glove did not reach the far side in time"},
    // (23 Nov console A/B: SMOOTH 60 is the frame-rate mode; the earlier screens' AUTO and UP TO 60 lines would keep a player on the old governor)
    {"MSC_FPS", "auto", "FRAME RATE AUTO is gone: SMOOTH 60 is the frame-rate mode"},
    {"MSC_FPS_BETWEEN", "pace", "FRAME RATE UP TO 60 is gone: SMOOTH 60 is the frame-rate mode"},
};

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

// set18: kObsolete. A line is the screen's own when it stands between kBlockStart and kBlockEnd.
bool ValueIn(const char* values, const std::string& value) {
    if (std::strcmp(values, "*") == 0) return true;
    for (const char* p = values; *p;) {
        const char* end = std::strchr(p, '|');
        const size_t n = end ? size_t(end - p) : std::strlen(p);
        if (value.size() == n && value.compare(0, n, p, n) == 0) return true;
        if (!end) break;
        p = end + 1;
    }
    return false;
}
const Obsolete* FindObsolete(const std::string& key, const std::string& value) {
    for (const Obsolete& o : kObsolete)
        if (key == o.key && ValueIn(o.values, value)) return &o;
    return nullptr;
}

// Values the build itself applies when a file names nothing (vita_plan.cpp "quality and timing switches"): a file that names the same value (a 1.2 test file) reads as the row's default
// choice, and the next save drops the line. Only keys a row owns (1.3 prune: the keys of the removed rows are no longer listed: nothing owns them). set18: MSC_LOCK60=1 is the build's
// own value once VitaPlanDefaults::kLock60 is true (the FRAME RATE row's first choice is then SMOOTH 60).
bool IsBuildDefault(const char* key, const std::string& value) {
    static const char* const kBuildDefaults[][2] = {{"MSC_DOF", "0"}, {"MSC_UNLOCK_CAPTAINS", "0"}};
    for (const auto& d : kBuildDefaults)
        if (std::strcmp(key, d[0]) == 0 && value == d[1]) return true;
    if (VitaPlanDefaults::kLock60 && std::strcmp(key, "MSC_LOCK60") == 0 && value == "1") return true;
    return false;
}

// set18: a key a row owns without any of its choices writing it: MSC_LOCK60 once SMOOTH 60 is the build's mode (the row's first choice writes nothing). A file may name its build value
// (1 reads as the row's choices say); any other value is none of the choices (CUSTOM), and the row's next save drops the line.
const char* ExtraKey(const char* title) { return VitaPlanDefaults::kLock60 && std::strcmp(title, "FRAME RATE") == 0 ? "MSC_LOCK60" : nullptr; }

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
            if (have && !want && std::strcmp(a.key, "MSC_FPS") == 0 && *have == "auto") have = nullptr;   // earlier builds' AUTO line: today's default
            if (have && !want && IsBuildDefault(a.key, *have)) have = nullptr;                              // the build's own value, named in the file
            if (want ? !(have && *have == want) : have != nullptr) return false;
        }
    }
    if (const char* k = ExtraKey(option.title))
        if (const std::string* have = Lookup(vars, k))
            if (!IsBuildDefault(k, *have)) return false;
    return true;
}

bool CorePlanOverride(const std::string& key) {
    return key == "MSC_FRAMESEND" || key == "MSC_VGX_BACKEND_THREAD" || key == "MSC_VITA_CORE_FRAMESEND" || key == "MSC_FS_PIPE";
}

bool OptionOverridden(int option, const std::vector<Line>& vars) {
    for (const Line& line : vars) {
        if (std::strcmp(kTable[option].title, "CORE PLAN") == 0 && CorePlanOverride(line.key)) return true;
    }
    return false;
}

bool OptionOwnsKey(int option, const std::string& key) {
    if (const char* k = ExtraKey(kTable[option].title))
        if (key == k) return true;
    if (std::strcmp(kTable[option].title, "CORE PLAN") == 0 && CorePlanOverride(key)) return true;
    for (const Choice& c : kTable[option].choices) {
        if (!c.label) break;
        for (const Assign& a : c.set)
            if (a.key && key == a.key) return true;
    }
    return false;
}

void Describe(State& s, const std::vector<Line>& vars, int option) {
    std::string t;
    for (const Line& line : vars) {
        if (OptionOwnsKey(option, line.key)) {
            const std::string item = line.key + "=" + *Lookup(vars, line.key.c_str());
            if (t.find(item) == std::string::npos) t += (t.empty() ? "" : " ") + item;
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

std::string WithoutObsolete(const std::string& text, std::vector<std::string>* notes) {
    std::string out;
    out.reserve(text.size());
    bool inBlock = false;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        const bool last = end == std::string::npos;
        if (last) end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;
        std::string bare = line;
        if (!bare.empty() && bare[bare.size() - 1] == '\r') bare.erase(bare.size() - 1);
        if (bare == kBlockStart) inBlock = true;
        else if (bare == kBlockEnd) inBlock = false;
        else if (inBlock) {
            const std::vector<Line> v = Variables(bare);
            if (v.size() == 1)
                if (const Obsolete* o = FindObsolete(v[0].key, v[0].value)) {
                    if (notes) notes->push_back("settings: " + v[0].key + "=" + v[0].value + " ignored (" + o->why + "); the next save drops it");
                    continue;
                }
        }
        out += line;
        if (!last) out += "\n";
    }
    return out;
}

void Load(State& s, const char* fileText) {
    s.text = fileText ? fileText : "";
    s.reset = false;
    std::vector<Line> vars = Variables(WithoutObsolete(s.text));   // (set18: an obsolete value in the screen's block reads as the row's default; the next save drops it)
    if (const std::string* selector = Lookup(vars, "MSC_PICTURE")) {
        const std::string preset = *selector;
        np_picture::Expand(preset.c_str(), [&](const char* key) { return Lookup(vars, key) != nullptr; },
                           [&](const char* key, const char* value) { vars.push_back({key, value}); });
    }
    for (int o = 0; o < kMaxOptions; ++o) {
        s.choice[o] = 0;
        s.loaded[o] = 0;
        s.touched[o] = false;
        s.custom[o][0] = '\0';
    }
    for (int o = 0; o < kOptionTotal; ++o) {
        s.choice[o] = -1;
        // These low-level switches override a named plan (or its pipeline), so AUTO would not describe what starts; the keys of older rows likewise (LegacyKey).
        const bool overrides = OptionOverridden(o, vars);
        if (!overrides)
            for (int c = 0; c < ChoiceCount(o); ++c)
                if (Matches(vars, kTable[o], kTable[o].choices[c])) {
                    s.choice[o] = c;
                    break;
                }
        if (s.choice[o] < 0) Describe(s, vars, o);
        s.loaded[o] = s.choice[o];
    }
}

bool Changed(const State& s) {
    for (int o = 0; o < kOptionTotal; ++o)
        if (s.choice[o] != s.loaded[o]) return true;
    return false;
}

// 1.3 in-game settings: three rows apply to the running game when they are saved (STADIUM CROWD, REDUCED CAMERA SHAKE, REAR TOUCH: one shared cell each, np/live_settings.h; they
// only change what is drawn or which local panel is read). Every other row is read at process start (some at static initialisation) and applies at the next start.
bool IsLive(int option) {
    if (option < 0 || option >= kOptionTotal) return false;
    const char* title = kTable[option].title;
    return !std::strcmp(title, "STADIUM CROWD") || !std::strcmp(title, "REDUCED CAMERA SHAKE") || !std::strcmp(title, "REAR TOUCH");
}

bool RestartNeeded(const State& s) {
    for (int o = 0; o < kOptionTotal; ++o)
        if (s.choice[o] != s.loaded[o] && !IsLive(o)) return true;
    return false;
}

bool NextStartChanged(const State& s) { return RestartNeeded(s); }

// After a successful save while a game runs: every live row whose choice differs from the loaded one gives its value to `set` (key and value of the choice; a choice that writes nothing
// gives the key of the row with a null value = the unset rule). A CUSTOM row (choice -1: the file holds a line this row cannot name) is left as it is. Returns the rows applied.
int ApplyLive(const State& s, void (*set)(const char* key, const char* valueOrNull)) {
    if (!set) return 0;
    int applied = 0;
    for (int o = 0; o < kOptionTotal; ++o) {
        if (!IsLive(o) || s.choice[o] == s.loaded[o] || s.choice[o] < 0 || s.choice[o] >= ChoiceCount(o)) continue;
        const Choice& c = kTable[o].choices[s.choice[o]];
        if (c.set[0].key) {
            set(c.set[0].key, c.set[0].value);
        } else {   // the choice writes nothing: the key is the one the row's other choices write
            for (int k = 0; k < ChoiceCount(o); ++k)
                if (kTable[o].choices[k].set[0].key) {
                    set(kTable[o].choices[k].set[0].key, nullptr);
                    break;
                }
        }
        ++applied;
    }
    return applied;
}

// The leave dialog: nothing changed, no dialog. Start-up (no game running): KEEP EDITING, DISCARD, SAVE. Running game: SAVE (lit first: it loses nothing), DON'T SAVE, KEEP EDITING; never a restart (a
// match is never ended from here). The dialog adds a note when restartNeeded (a next-start row changed).
int LeaveDialog(bool running, bool changed, bool restartNeeded, int ids[kMaxLeave]) {
    (void)restartNeeded;
    if (!changed) return 0;
    int n = 0;
    if (running) {
        ids[n++] = kLeaveSave;
        ids[n++] = kLeaveDiscard;
        ids[n++] = kLeaveKeepEditing;
    } else {
        ids[n++] = kLeaveKeepEditing;
        ids[n++] = kLeaveDiscard;
        ids[n++] = kLeaveSave;
    }
    return n;
}

// The one that is lit when the dialog opens: the one that loses nothing and costs nothing (SAVE; never the restart, which ends a match).
int LeaveDefault(const int* ids, int count) {
    for (int i = 0; i < count; ++i)
        if (ids[i] == kLeaveSave) return i;
    return 0;
}

int LeaveMove(int index, int count, int direction) {
    if (count <= 0) return 0;
    return ((index + direction) % count + count) % count;
}

const char* LeaveLabel(int id, bool running) {
    switch (id) {
    case kLeaveKeepEditing: return "KEEP EDITING";
    case kLeaveDiscard: return running ? "DON'T SAVE" : "DISCARD";
    case kLeaveSave: return "SAVE";
    case kLeaveSaveRestart: return "SAVE AND START AGAIN";   // (the start-up screen and the front-end entry only; a running game's dialog never offers it)
    default: return "";
    }
}

void SetChoice(State& s, int option, int choice) {
    if (option < 0 || option >= kOptionTotal || choice < 0 || choice >= ChoiceCount(option)) return;
    s.choice[option] = choice;
    s.touched[option] = true;
}

void Step(State& s, int option, int direction) {
    const int n = ChoiceCount(option);
    if (n <= 0) return;   // (an option without choices: nothing to step through, and no remainder by zero)
    if (n < 2 && s.choice[option] >= 0) return;
    int at = s.choice[option];
    if (at < 0) at = direction > 0 ? -1 : n;
    SetChoice(s, option, ((at + direction) % n + n) % n);
}

void ResetAll(State& s) {
    for (int o = 0; o < kOptionTotal; ++o) SetChoice(s, o, kTable[o].defaultChoice);
    s.reset = true;   // (Serialize then drops the lines of the retired keys wherever they stand)
}

namespace {
int FindOption(const char* title) {
    for (int o = 0; o < kOptionTotal; ++o)
        if (std::strcmp(kTable[o].title, title) == 0) return o;
    return -1;
}
} // namespace

namespace {
const RetiredKey* FindRetired(const std::string& key) {
    for (const RetiredKey& r : kRetiredKeys)
        if (key == r.key) return &r;
    return nullptr;
}

// Whether MSC_PICTURE=<selector> can be written out as the rows' own lines: every setting it would add must be one a row holds as a real choice. The selector expands at start-up as it
// always did (np_picture_presets.h, kept for hand-written files); a preset with a setting of a removed row ("full": MSC_PICTURE_HOLD, MSC_FX_GOV) or a value no choice has any more
// (quality's MSC_VITA_RENDER=960x544) would lose that setting if the line were dropped, so the line stays.
bool SelectorFitsRows(const State& s, const std::vector<Line>& vars, const char* selector) {
    for (const np_picture::Setting* p = np_picture::Find(selector); p && p->name; ++p) {
        if (Lookup(vars, p->name)) continue;   // (the file names it itself: the expansion does not set it)
        if (FindObsolete(p->name, p->value)) continue;   // (set18: a fixed size no row offers any more; the rewrite leaves it out, as the start-up does for the screen's own lines)
        int owner = -1;
        for (int o = 0; o < kOptionTotal && owner < 0; ++o)
            if (OptionOwnsKey(o, p->name)) owner = o;
        if (owner < 0 || s.choice[owner] < 0) return false;
    }
    return true;
}
}  // namespace

bool Serialize(const State& s, std::string& out, size_t limit) {
    // The original text without the lines this screen owns: those of options the player changed or that equal a choice (they are written again below
    // from the choice), and its own block comments. The lines of an option holding a value that is none of the choices stay where they are. 1.3 prune: a retired key's
    // line goes when it stood inside the block (only this screen writes there) or after RESET TO DEFAULTS; anywhere else it stays (kRetiredKeys).
    std::string kept;
    const std::vector<Line> vars = Variables(s.text);
    const std::string* selector = Lookup(vars, "MSC_PICTURE");
    // Materialise a recognized legacy preset using the effective choices loaded above; otherwise it would restore removed defaults.
    const bool normalizePicture = selector && np_picture::Find(selector->c_str()) && SelectorFitsRows(s, vars, selector->c_str());
    bool inBlock = false;
    size_t at = 0;
    while (at < s.text.size()) {
        size_t end = s.text.find('\n', at);
        if (end == std::string::npos) end = s.text.size();
        std::string line = s.text.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
        bool drop = line == kBlockStart || line == kBlockEnd;
        const bool wasInBlock = inBlock;
        if (line == kBlockStart) inBlock = true;
        else if (line == kBlockEnd) inBlock = false;
        if (!drop) {
            const std::vector<Line> v = Variables(line);
            if (v.size() == 1) {
                if (normalizePicture && v[0].key == "MSC_PICTURE") drop = true;
                for (int o = 0; o < kOptionTotal && !drop; ++o)
                    if (OptionOwnsKey(o, v[0].key) && (s.touched[o] || s.choice[o] >= 0)) drop = true;
                if (!drop)
                    if (const RetiredKey* r = FindRetired(v[0].key))
                        if (wasInBlock || s.reset) {
                            const int row = r->staysWith ? FindOption(r->staysWith) : -1;
                            drop = !(row >= 0 && s.choice[row] < 0 && !s.touched[row]);   // (a variant the row still reads as CUSTOM keeps its companion line)
                        }
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
    if (out.size() > limit) return false;
    // LoadEnvFile records at most 96 assignments, including duplicates; refuse a save whose trailing settings it would ignore.
    size_t count = 0;
    for (const Line& line : Variables(out))
        if (line.key.size() < 48 && ++count > 96) return false;
    return true;
}

// ---- the LiveArea's button while the game runs -----------------------------------------------------------------------------------------------------------------
bool OpenRequest::Observe(bool present) {
    const bool press = present && !seen_;
    seen_ = present;
    if (press) {
        pending_ = true;
        failures_ = 0;
    }
    return press;
}

bool OpenRequest::Failed() {
    if (!pending_) return false;
    if (++failures_ < kMaxOpenFailures) return false;
    pending_ = false;
    return true;
}

// ---- the screen ---------------------------------------------------------------------------------------------------------------------------------------------
namespace {

// The rows of each page, in screen order (kPages resolved against kTable, once): an option's index, or -1 - n for information line n. An option stands once on its page; a row named on no
// page is not on this screen (set18: the game's own menu shows it).
struct PageIndex {
    int count[kPageTotal] = {};
    int option[kPageTotal][kMaxOptions] = {};
    PageIndex() {
        bool placedAny[kMaxOptions] = {};
        for (int p = 0; p < kPageTotal; ++p) {
            bool placed[kMaxOptions] = {};
            for (const char* title : kPages[p].rows) {
                if (!title) break;
                const int o = FindOption(title);
                if (o < 0) {   // not a row of kTable: an information line, or an action
                    for (int i = 0; i < kInfoTotal; ++i)
                        if (std::strcmp(kInfo[i].title, title) == 0 && count[p] < kMaxOptions) option[p][count[p]++] = -1 - i;
                    for (int i = 0; i < kActionTotal; ++i)
                        if (std::strcmp(kActions[i].title, title) == 0 && count[p] < kMaxOptions) option[p][count[p]++] = -(kActionBase + i);
                    continue;
                }
                if (placed[o]) continue;
                placed[o] = placedAny[o] = true;
                option[p][count[p]++] = o;
            }
        }
        (void)placedAny;   // (set18: a row named on no page is the game menu's; it is not drawn here)
    }
};
const PageIndex& Pages() {
    static const PageIndex pages;
    return pages;
}
int ClampPage(int page) { return page < 0 ? 0 : page >= kPageTotal ? kPageTotal - 1 : page; }

// Why a row does nothing while the other rows stand as they do (null: it acts). set18: none of the remaining rows depends on another one (SMOOTH 60 and 30 FPS are one row now, the
// Mega Save aim is one row, the link is chosen in the game's own menu), so every row acts; the function stays for the rows a later build links.
const char* NoEffect(const State& s, int option) {
    (void)s;
    (void)option;
    return nullptr;
}

} // namespace

const char* WhyNoEffect(const State& s, int option) { return option >= 0 && option < kOptionTotal ? NoEffect(s, option) : nullptr; }
int PageCount() { return kPageTotal; }
const char* PageName(int page) { return kPages[ClampPage(page)].name; }
int RowCount(int page) {
    page = ClampPage(page);
    return Pages().count[page] + 3;
}
Row GetRow(int page, int index) {
    page = ClampPage(page);
    const int options = Pages().count[page];
    if (index < options) {
        const int o = Pages().option[page][index];
        return o >= 0 ? Row{kRowOption, o} : o <= -kActionBase ? Row{kRowAction, -kActionBase - o} : Row{kRowInfo, -1 - o};
    }
    return {RowKind(kRowReset + (index - options)), -1};
}
bool FindRow(int option, int* page, int* row) {
    for (int p = 0; p < kPageTotal; ++p)
        for (int i = 0; i < Pages().count[p]; ++i)
            if (Pages().option[p][i] == option) {
                *page = p;
                *row = i;
                return true;
            }
    return false;
}
int ActionCount() { return kActionTotal; }
const char* ActionTitle(int action) { return action >= 0 && action < kActionTotal ? kActions[action].title : ""; }
int InfoCount() { return kInfoTotal; }
const char* InfoTitle(int info) { return info >= 0 && info < kInfoTotal ? kInfo[info].title : ""; }
const char* InfoHelp(int info, int line) { return info >= 0 && info < kInfoTotal && (line == 0 || line == 1) ? kInfo[info].help[line] : ""; }
void SetInfo(int info, const char* value) {
    if (info >= 0 && info < kInfoTotal) std::snprintf(g_infoValue[info], sizeof g_infoValue[info], "%s", value ? value : "");
}
const char* InfoValue(int info) {
    if (info < 0 || info >= kInfoTotal) return "";
    if (g_infoValue[info][0]) return g_infoValue[info];
    // the two facts that are this build's own
    static char version[32], build[32];
    if (std::strcmp(kInfo[info].title, "VERSION") == 0) {
        std::snprintf(version, sizeof version, "%s", NpBuildIdentity::Version());
        return version;
    }
    if (std::strcmp(kInfo[info].title, "BUILD") == 0) {
        std::snprintf(build, sizeof build, "%s", NpBuildIdentity::BuildId()[0] ? NpBuildIdentity::BuildId() : "UNNAMED");
        return build;
    }
    return "";
}

namespace {

// Strikers colours, chosen by hand (nothing is read from the game): the pause panel's steel frame, its blue-glow inner panel, grey-blue striped row bars with a
// lighter selected bar, white lettering with a dark outline, a warm orange title band. All 0xAABBGGRR through Rgb().
constexpr uint32_t Rgb(int r, int g, int b) { return 0xFF000000u | (uint32_t(b) << 16) | (uint32_t(g) << 8) | uint32_t(r); }
constexpr uint32_t kBack = Rgb(20, 26, 34), kBackLine = Rgb(25, 32, 41);          // the screen behind the panel (and the dialogs' box: test_pause_settings kBox)
constexpr uint32_t kPanel = Rgb(30, 38, 48);                                      // the dialog box: flat, its text keeps to 112..848
constexpr uint32_t kFrame = Rgb(84, 88, 94), kFrameHi = Rgb(150, 154, 162), kFrameLo = Rgb(36, 38, 42);   // the steel frame
constexpr uint32_t kGlow = Rgb(76, 178, 224), kTealTop = Rgb(30, 88, 114), kTealLow = Rgb(16, 46, 64);   // the panel's blue glow
constexpr uint32_t kBarTop = Rgb(86, 96, 106), kBarLow = Rgb(46, 54, 62);                                // an unselected bar
constexpr uint32_t kSelTop = Rgb(176, 182, 188), kSelLow = Rgb(104, 110, 118);                           // the lit bar
constexpr uint32_t kBandTop = Rgb(226, 66, 36), kBandLow = Rgb(255, 196, 70), kBandDark = Rgb(40, 30, 26);   // the orange title band, and its dark line
constexpr uint32_t kSelect = kSelLow, kEdge = kGlow;
constexpr int kVisibleRows = 14;   // rows of the list that fit above the help text at 25 px
constexpr uint32_t kWhite = Rgb(244, 246, 248), kDim = Rgb(206, 214, 222), kYellow = Rgb(255, 224, 64), kGreen = Rgb(116, 232, 116), kRed = Rgb(255, 104, 104), kHint = Rgb(150, 162, 174);
constexpr uint32_t kBlack = 0xFF000000u;

const Art* g_art = nullptr;   // SetArt


// color over what is there, `alpha` 0..255 of it
void Shade(uint32_t* px, int x, int y, int w, int h, uint32_t color, int alpha) {
    const int inv = 255 - alpha;
    for (int j = y < 0 ? 0 : y; j < y + h && j < kHeight; ++j)
        for (int i = x < 0 ? 0 : x; i < x + w && i < kWidth; ++i) {
            const uint32_t d = px[j * kWidth + i];
            const uint32_t r = ((d & 255) * inv + (color & 255) * alpha) / 255, g = (((d >> 8) & 255) * inv + ((color >> 8) & 255) * alpha) / 255,
                           b = (((d >> 16) & 255) * inv + ((color >> 16) & 255) * alpha) / 255;
            px[j * kWidth + i] = 0xFF000000u | (b << 16) | (g << 8) | r;
        }
}

void Rect(uint32_t* px, int x, int y, int w, int h, uint32_t color) {
    for (int j = y < 0 ? 0 : y; j < y + h && j < kHeight; ++j)
        for (int i = x < 0 ? 0 : x; i < x + w && i < kWidth; ++i) px[j * kWidth + i] = color;
}

// 1.3: the language the screen is drawn in (Draw sets it from the LANGUAGE row, so a change shows at once). Every text goes through the table of
// np_text.h on its way to the screen: the model (titles, choices, the file's lines) stays English, only the drawing changes.
int g_language = NpText::kEnglish;
const char* Tr(const char* en) { return NpText::In(g_language, en); }
int Chars(const char* s) { return VitaUtf8Length(Tr(s)); }

int Text(uint32_t* px, int x, int y, uint32_t color, const char* s, int scale = 1) {
    s = Tr(s);
    while (*s) {
        const uint16_t* glyph = VitaFontGlyph(VitaUtf8Next(s));
        for (int j = 0; j < kVitaFontHeight; ++j) {
            const uint16_t bits = glyph[j];
            if (!bits) continue;
            for (int i = 0; i < kVitaFontWidth; ++i)
                if (bits & (1u << (kVitaFontWidth - 1 - i))) Rect(px, x + i * scale, y + j * scale, scale, scale, color);
        }
        x += kVitaFontWidth * scale;
    }
    return x;
}

bool ArtOn() { return g_art && g_art->backdrop; }

// (1.3) the game's heading lettering has ASCII only: a translated heading loses its accents there (PARAMÈTRES reads PARAMETRES)
const char* Folded(const char* text, char* folded, size_t size) {
    static const char kFold[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";   // U+00C0..U+00FF
    const char* t = Tr(text);
    size_t n = 0;
    while (*t && n + 1 < size) {
        const uint32_t cp = VitaUtf8Next(t);
        folded[n++] = cp < 0x80 ? char(cp) : cp >= 0xC0 && cp <= 0xFF ? kFold[cp - 0xC0] : '?';
    }
    folded[n] = 0;
    return folded;
}

// The heading in the game's lettering when there is a font, else false (the caller draws the plain one). Right-aligned when rightEdge > 0.
bool Heading(uint32_t* px, int x, int rightEdge, int y, int height, const char* text) {
    if (!ArtOn() || !g_art->heading) return false;
    char folded[64];
    text = Folded(text, folded, sizeof folded);
    const int w = g_art->heading(nullptr, 0, 0, height, text, false);
    if (w <= 0) return false;
    g_art->heading(px, rightEdge > 0 ? rightEdge - w : x, y, height, text, true);
    return true;
}

// ---- the Strikers look: bars, bands, panels and the heavy lettering, all drawn from constants (no game art) ----
uint32_t Mix(uint32_t a, uint32_t b, int t, int of) {   // a at t = 0, b at t = of
    auto ch = [&](int shift) { return int(((a >> shift) & 255) * (of - t) + ((b >> shift) & 255) * t) / of; };
    return Rgb(ch(0), ch(8), ch(16));
}

// a row of colour, opaque or over what is there
void Line(uint32_t* px, int x, int y, int w, uint32_t color, int alpha) {
    if (alpha >= 255) Rect(px, x, y, w, 1, color);
    else Shade(px, x, y, w, 1, color, alpha);
}

// a slanted bar: the top edge leans right by `slant` px (a parallelogram), a vertical gradient, `alpha` 255 = opaque, pins: every other line a little darker (the game's striped bars)
void Slant(uint32_t* px, int x, int y, int w, int h, int slant, uint32_t top, uint32_t low, int alpha = 255, bool pins = false) {
    for (int j = 0; j < h; ++j) {
        const int off = h > 1 ? slant * (h - 1 - j) / (h - 1) : 0;
        uint32_t c = Mix(top, low, j, h > 1 ? h - 1 : 1);
        if (pins && (j & 1)) c = Mix(c, kBlack, 1, 7);
        Line(px, x + off, y + j, w, c, alpha);
    }
}

// a button bar of the pause panel: striped gradient, a bright top edge, a dark lower edge, the corners cut; `lit`: the lighter selected bar with a blue glow around it
void Bar(uint32_t* px, int x, int y, int w, int h, bool lit) {
    if (lit) {
        Shade(px, x - 3, y - 3, w + 6, h + 6, kGlow, 110);
        Shade(px, x - 2, y - 2, w + 4, h + 4, kGlow, 200);
    }
    Slant(px, x, y, w, h, 0, lit ? kSelTop : kBarTop, lit ? kSelLow : kBarLow, 255, true);
    Rect(px, x, y, w, 1, lit ? Rgb(236, 240, 244) : Rgb(120, 132, 144));
    Rect(px, x, y + h - 1, w, 1, lit ? Rgb(70, 76, 84) : Rgb(24, 30, 36));
    const uint32_t around = lit ? kGlow : kTealLow;
    Rect(px, x, y, 1, 1, around); Rect(px, x + w - 1, y, 1, 1, around);   // (cut corners)
    Rect(px, x, y + h - 1, 1, 1, around); Rect(px, x + w - 1, y + h - 1, 1, 1, around);
}

// the pause panel's blue inner panel: a teal gradient with a glowing edge (alpha < 255 lets a backdrop show through)
void BluePanel(uint32_t* px, int x, int y, int w, int h, int alpha) {
    Slant(px, x, y, w, h, 0, kTealTop, kTealLow, alpha);
    Shade(px, x, y, w, 2, kGlow, 235);
    Shade(px, x, y + h - 2, w, 2, kGlow, 235);
    Shade(px, x, y, 2, h, kGlow, 235);
    Shade(px, x + w - 2, y, 2, h, kGlow, 235);
}

// The lettering: the font's glyphs made heavy (bold), optionally leaning (italic: the top of each glyph is pushed right) and outlined in black, like the game's text. Two passes
// (every outline first, then every fill) so one letter's outline never cuts into its neighbour. Returns the right edge like Text. `scale` 2 is for headings.
int Label(uint32_t* px, int x, int y, uint32_t color, const char* s, int scale = 1, bool italic = false, bool bold = true) {
    s = Tr(s);
    const int o = scale, bw = bold ? scale * 2 : scale;
    int end = x;
    for (int pass = 0; pass < 2; ++pass) {
        const char* p = s;
        int cx = x;
        while (*p) {
            const uint16_t* glyph = VitaFontGlyph(VitaUtf8Next(p));
            for (int j = 0; j < kVitaFontHeight; ++j) {
                const uint16_t bits = glyph[j];
                if (!bits) continue;
                const int lean = italic ? ((kVitaFontHeight - 1 - j) * scale) / 5 : 0;
                for (int i = 0; i < kVitaFontWidth; ++i) {
                    if (!(bits & (1u << (kVitaFontWidth - 1 - i)))) continue;
                    const int gx = cx + i * scale + lean, gy = y + j * scale;
                    if (pass == 0) Rect(px, gx - o, gy - o, bw + 2 * o, scale + 2 * o, kBlack);
                    else Rect(px, gx, gy, bw, scale, color);
                }
            }
            cx += kVitaFontWidth * scale;
        }
        end = cx;
    }
    return end;
}

// body text with a one pixel shadow (right and down only: the dialogs keep their text out of the box's 32 px margins)
int Shadowed(uint32_t* px, int x, int y, uint32_t color, const char* s) {
    Text(px, x + 1, y + 1, kBlack, s);
    return Text(px, x, y, color, s);
}

// the screen's frame: a steel border with rivets, over everything else
void Frame(uint32_t* px) {
    Rect(px, 0, 0, kWidth, 6, kFrame);
    Rect(px, 0, kHeight - 6, kWidth, 6, kFrame);
    Rect(px, 0, 0, 6, kHeight, kFrame);
    Rect(px, kWidth - 6, 0, 6, kHeight, kFrame);
    Rect(px, 0, 0, kWidth, 1, kFrameHi); Rect(px, 0, 0, 1, kHeight, kFrameHi);
    Rect(px, 0, kHeight - 1, kWidth, 1, kFrameLo); Rect(px, kWidth - 1, 0, 1, kHeight, kFrameLo);
    Rect(px, 5, 5, kWidth - 10, 1, kFrameLo); Rect(px, 5, kHeight - 6, kWidth - 10, 1, kFrameHi);
    Rect(px, 5, 5, 1, kHeight - 10, kFrameLo); Rect(px, kWidth - 6, 5, 1, kHeight - 10, kFrameHi);
    for (int i = 0; i < 4; ++i) {   // rivets
        const int rx = (i & 1) ? kWidth - 5 : 1, ry = (i & 2) ? kHeight - 5 : 1;
        Rect(px, rx, ry, 4, 4, kFrameLo);
        Rect(px, rx, ry, 3, 3, kFrameHi);
    }
}

// the screen behind and the header: the plain look has a dark steel ground with fine lines and an orange band with dark slanted streaks; over the game's own backdrop
// the band is dark steel (the game's heading lettering is orange and needs it)
void Chrome(uint32_t* px, bool art) {
    if (art) {
        std::memcpy(px, g_art->backdrop, sizeof(uint32_t) * kWidth * kHeight);
        Slant(px, 6, 6, kWidth - 12, 54, 0, Rgb(70, 78, 88), Rgb(24, 30, 38), 235, true);
        Rect(px, 6, 58, kWidth - 12, 2, kBandTop);
    } else {
        for (int y = 0; y < kHeight; ++y) Rect(px, 0, y, kWidth, 1, (y & 3) < 2 ? kBack : kBackLine);
        Slant(px, 6, 6, kWidth - 12, 54, 0, kBandTop, kBandLow);
        for (int i = 0; i < 9; ++i) Slant(px, 40 + i * 118 + (i & 1) * 22, 6, 14 + (i % 3) * 8, 54, 22, kBandDark, kBandDark, 46);   // slanted streaks
        Rect(px, 6, 6, kWidth - 12, 2, Rgb(255, 232, 150));
    }
    Rect(px, 6, 60, kWidth - 12, 2, kBandDark);
    Rect(px, 6, 62, kWidth - 12, 3, kGlow);
    Frame(px);
}

} // namespace

void SetArt(const Art* art) { g_art = art; }

namespace {
// The LANGUAGE row's choice (AUTOMATIC, ENGLISH, SPANISH, FRENCH); a hand-written value is read the way the game reads it; AUTOMATIC or no row: the game's own start-up language.
void UseLanguageOf(const State& s) {
    const int o = OptionIndex("LANGUAGE");
    const int c = o >= 0 ? s.choice[o] : -1;
    int l = c == 1 ? NpText::kEnglish : c == 2 ? NpText::kSpanish : c == 3 ? NpText::kFrench : -1;   // (0, AUTOMATIC: what the start has made of the Vita's language: the environment)
    if (l < 0 && o >= 0 && c < 0) {
        const char* v = std::strchr(s.custom[o], '=');
        l = NpText::ParseLanguage(v ? v + 1 : s.custom[o]);
    }
    g_language = l >= 0 ? l : NpText::LanguageFromEnv();
}
}  // namespace

void Draw(uint32_t* px, const State& s, int page, int selected, const char* message, bool error, bool running) {
    page = ClampPage(page);
    UseLanguageOf(s);
    const int rows = RowCount(page);
    const bool art = ArtOn();
    const uint32_t edge = kEdge;
    Chrome(px, art);
    if (!Heading(px, 24, 0, 8, 40, "MARIO STRIKERS CHARGED")) Label(px, 34, 8, kWhite, "MARIO STRIKERS CHARGED", 2, true);
    if (!Heading(px, 0, kWidth - 28, 8, 40, "SETTINGS")) Label(px, kWidth - 36 - Chars("SETTINGS") * 24, 8, kYellow, "SETTINGS", 2, true);

    // 1.3: the pages, one line above the list (the triggers change page): slanted tabs, the open one in the band's orange
    {
        // each name as wide as it is, the spare room shared between them (the longest set, MULTIJUGADOR and INFORMACIÓN, still fits at 12 px a letter)
        int total = 0;
        for (int p = 0; p < kPageTotal; ++p) total += Chars(kPages[p].name) * kVitaFontWidth;
        const int gap = (kWidth - 104 - total) / kPageTotal;
        Slant(px, 14, 68, 26, 25, 5, kBarTop, kBarLow);
        Label(px, 20, 68, kDim, "L");
        Slant(px, kWidth - 42, 68, 26, 25, 5, kBarTop, kBarLow);
        Label(px, kWidth - 36, 68, kDim, "R");
        int at = 52 + gap / 2;
        for (int p = 0; p < kPageTotal; ++p) {
            const int width = Chars(kPages[p].name) * kVitaFontWidth;
            const int x = at;
            at += width + gap;
            if (p == page) {
                Slant(px, x - 9, 66, width + 18, 29, 5, kBandTop, kBandLow);
                Rect(px, x - 9, 94, width + 23, 2, kBandDark);
            } else {
                Slant(px, x - 9, 68, width + 18, 25, 5, kBarTop, kBarLow, 150, true);
            }
            Label(px, x, 68, p == page ? kWhite : kDim, kPages[p].name, 1, true);
        }
    }

    // 25 px per row (the font is 24 px high); W5 CTL: more rows than fit above the help text scroll (they used to squeeze the pitch)
    const int top = 100, labelX = 52, valueX = 404;
    const int pitch = 25;
    const int first = FirstVisibleRow(page, selected);
    const int last = first + kVisibleRows - 1 < rows - 1 ? first + kVisibleRows - 1 : rows - 1;
    BluePanel(px, 10, 98, kWidth - 20, 354, art ? 205 : 255);
    if (first > 0) Label(px, kWidth - 44, top, edge, "^");
    if (last < rows - 1) Label(px, kWidth - 44, top + (kVisibleRows - 1) * pitch, edge, "v");
    int ys[kVisibleRows + 1];
    {   // where each visible row stands (the RESET row sits 4 px lower), so that every bar is drawn before any text and a lit bar's glow is not cut by the next bar
        int yy = top;
        for (int r = first; r <= last; ++r) {
            if (GetRow(page, r).kind == kRowReset && r != first) yy += 4;
            ys[r - first] = yy;
            yy += pitch;
        }
    }
    for (int r = first; r <= last; ++r)
        if (r != selected) Bar(px, 22, ys[r - first], kWidth - 44, pitch - 1, false);
    if (selected >= first && selected <= last) Bar(px, 22, ys[selected - first], kWidth - 44, pitch - 1, true);
    int y = top;
    for (int r = first; r <= last; ++r) {
        const Row row = GetRow(page, r);
        y = ys[r - first];
        const bool on = r == selected;
        const char* label = "";
        char value[96] = "";
        uint32_t valueColor = kYellow;
        bool changeable = false;
        switch (row.kind) {
        case kRowOption: {
            const Option& o = kTable[row.option];
            label = o.title;
            const int c = s.choice[row.option];
            if (c >= 0) std::snprintf(value, sizeof value, "%s", Tr(o.choices[c].label));
            else std::snprintf(value, sizeof value, "%s: %.31s%s", Tr("CUSTOM"), s.custom[row.option], std::strlen(s.custom[row.option]) > 31 ? ".." : "");   // (42 characters fit)
            if (NoEffect(s, row.option)) valueColor = kHint;
            changeable = true;
            break;
        }
        case kRowInfo: {
            label = kInfo[row.option].title;
            const char* v = InfoValue(row.option);
            if (!*v) v = std::strcmp(label, "ONLINE NAME") == 0 ? "NOT SET YET" : "-";
            std::snprintf(value, sizeof value, "%s", Tr(v));
            valueColor = kWhite;
            break;
        }
        case kRowAction: {
            label = kActions[row.option].title;
            std::snprintf(value, sizeof value, "%s", Tr("PRESS CROSS"));
            valueColor = on ? kGreen : kDim;
            break;
        }
        case kRowReset: label = "RESET TO DEFAULTS"; valueColor = kDim; break;
        case kRowSaveStart: label = running ? "SAVE" : "SAVE AND START"; valueColor = kGreen; break;
        case kRowSaveExit: label = running ? "DON'T SAVE" : "SAVE AND EXIT"; valueColor = kGreen; break;   // (running: leaves without writing; a match is never restarted from here)
        default: break;
        }
        const uint32_t labelColor = (row.kind == kRowSaveStart || row.kind == kRowSaveExit) ? kGreen : (on ? kWhite : kDim);
        Label(px, labelX, y, labelColor, label, 1, true);
        if (row.kind == kRowInfo || row.kind == kRowAction) Label(px, valueX, y, valueColor, value);   // (a fact or an action: no arrows)
        if (changeable) {
            int x = valueX;
            if (on) x = Label(px, x - 24, y, edge, "<") + 12;
            x = Label(px, x, y, valueColor, value);
            if (on) Label(px, x + 12, y, edge, ">");
        }
    }

    // help for the selected row (or the status line instead of its second line), the buttons
    const Row sel = GetRow(page, selected);
    const char* help1 = "";
    const char* help2 = "";
    switch (sel.kind) {
    case kRowOption:
        help1 = kTable[sel.option].help[0];
        help2 = kTable[sel.option].help[1];
        if (const char* why = NoEffect(s, sel.option)) help2 = why;
        break;
    case kRowInfo:
        help1 = kInfo[sel.option].help[0];
        help2 = kInfo[sel.option].help[1];
        break;
    case kRowAction:
        help1 = kActions[sel.option].help[0];
        help2 = kActions[sel.option].help[1];
        break;
    case kRowReset:
        help1 = "Puts every row of every page back to its default.";
        help2 = "Nothing is written until you save.";
        break;
    case kRowSaveStart:
        help1 = running ? "Saves. Crowd, camera shake and rear touch apply now; the rest next start."
                        : "Writes ux0:data/mscharged/np-player.txt and starts the game.";
        break;
    case kRowSaveExit:
        help1 = running ? "Leaves without saving. Nothing is written."
                        : "Writes np-player.txt and returns to the LiveArea.";
        break;
    default: break;
    }
    Slant(px, 6, 456, kWidth - 12, 82, 0, Rgb(34, 42, 52), Rgb(16, 20, 26), art ? 225 : 255);   // the help panel: dark steel under the list
    Rect(px, 6, 454, kWidth - 12, 2, kGlow);
    Text(px, 24, 462, kWhite, help1);
    if (message && *message) Text(px, 24, 486, error ? kRed : kGreen, message);
    else Text(px, 24, 486, kDim, help2);
    // (CLASSIC: the game's own menus go back with Square (np_control_table.h); the screen takes Square as Circle in every layout and names the layout's own button)
    const bool classic = ClassicLayout(s);
    Text(px, 24, 516, kHint, running ? (classic ? "L/R: page  Up/Down: row  Left/Right: change  Triangle: buttons  Square: back"
                                                 : "L/R: page  Up/Down: row  Left/Right: change  Triangle: buttons  Circle: back")
                                     : (classic ? "L/R: page  Up/Down: row  Left/Right: change  Triangle: buttons  Square: leave"
                                                : "L/R: page  Up/Down: row  Left/Right: change  Triangle: buttons  Circle: leave"));
}

// The dialogs dim the whole screen first (the list behind must not read as live, and nothing of it shows left or right of the box), then draw the box.
static void DimScreen(uint32_t* px) { Shade(px, 0, 0, kWidth, kHeight, 0xFF000000u, 170); }

namespace {
// A dialog's box in the pause panel's popup look: a flat steel-blue box (test_pause_settings: its 32 px margins stay this one colour), a glowing top edge, the title leaning in heavy
// lettering over a slanted orange stripe. The title starts 4 px inside the text's own left edge so its outline stays out of the margin.
void DialogBox(uint32_t* px, int x, int y, int w, int h, int titleY, const char* title) {
    Rect(px, x, y, w, h, kPanel);
    Rect(px, x, y, w, 3, kGlow);
    const int tw = Chars(title) * 24;
    Slant(px, 112, titleY + 46, tw + 20, 5, 8, kBandTop, kBandLow);
    Label(px, 116, titleY, kWhite, title, 2, true);
}
// a choice in a dialog: a bar of the list's look, lit or not, its words inside
void DialogChoice(uint32_t* px, int x, int y, int w, bool lit, const char* label, int textX) {
    Bar(px, x + 3, y + 3, w - 6, 32, lit);   // (inset: the lit bar's glow reaches 3 px beyond it and must stay out of the box's margins)
    Label(px, textX, y + 7, lit ? kWhite : kDim, label, 1, true);
}
}  // namespace

void DrawRestartConfirmation(uint32_t* px, bool restart, bool classic) {
    DimScreen(px);
    DialogBox(px, 80, 160, 800, 220, 178, "RESTART THE GAME?");
    Shadowed(px, 112, 242, kDim, "A match in progress will be lost.");
    DialogChoice(px, 112, 300, 320, !restart, "CANCEL", 136);
    DialogChoice(px, 480, 300, 320, restart, "SAVE AND RESTART", 504);
    Text(px, 112, 348, kHint, classic ? "Left/Right: choose  Cross: confirm  Square: cancel" : "Left/Right: choose  Cross: confirm  Circle: cancel");
}

void DrawLeaveConfirmation(uint32_t* px, int choice, bool classic) {
    DimScreen(px);
    DialogBox(px, 80, 160, 800, 220, 178, "SAVE YOUR CHANGES?");
    Shadowed(px, 112, 242, kDim, "You changed settings and have not saved them.");
    static const char* const kLabels[kLeaveChoices] = {"KEEP EDITING", "DISCARD", "SAVE"};
    for (int i = 0; i < 3; ++i) DialogChoice(px, 112 + i * 248, 300, 240, i == choice, kLabels[i], 112 + i * 248 + 24);
    Text(px, 112, 348, kHint, classic ? "Left/Right: choose  Cross: confirm  Square: keep editing" : "Left/Right: choose  Cross: confirm  Circle: keep editing");
}

// The running game's dialog: the choices stand in a column (SAVE first, lit first), `index` is the lit one. Every text keeps inside the box (x 112 to 848: 61 characters at 12 px;
// the test checks each text of each language), the sentence is two lines.
// W22: the consent screen of SEND A REPORT, the confirmation of DELETE MY ONLINE ACCOUNT and the wait while either runs. `choice` 0 is the left button (CANCEL / NO,
// the one that is lit when the dialog opens), 1 the right one (SEND / YES). Every line is a row of np_text_strings.tsv, at most 61 characters (the box).
void DrawReportDialog(uint32_t* px, ReportDialog kind, int choice, bool classic) {
    DimScreen(px);
    if (kind == kReportWorking || kind == kReportDeleting) {
        DialogBox(px, 80, 160, 800, 190, 178, "PLEASE WAIT");
        Shadowed(px, 112, 242, kDim, kind == kReportDeleting ? "Deleting your online account..." : "Sending the report...");
        Text(px, 112, 300, kHint, classic ? "Square: cancel" : "Circle: cancel");
        return;
    }
    const bool send = kind == kReportSend;
    static const char* const kSend[] = {"This is sent to the online service:", "- your online name", "- the last lines of the multiplayer log", "- your last connection check",
                                        "- numbers from your last game: length, delay, loss", "IP addresses, ids and codes are removed first.", "Your friend list is not sent.",
                                        "It is kept for 30 days or until you delete the account."};
    static const char* const kDelete[] = {"This removes your name, friends, invitations and reports", "from the online service, and this Vita forgets its name.",
                                          "Your game progress is not touched. This cannot be undone."};
    const char* const* lines = send ? kSend : kDelete;
    const int count = send ? int(sizeof kSend / sizeof kSend[0]) : int(sizeof kDelete / sizeof kDelete[0]);
    const int h = 150 + count * 24 + 40 + 44;
    DialogBox(px, 80, 60, 800, h, 76, send ? "SEND A REPORT?" : "DELETE THE ACCOUNT?");
    for (int i = 0; i < count; ++i) Shadowed(px, 112, 132 + i * 24, i == 0 && send ? kWhite : kDim, lines[i]);
    const int y = 132 + count * 24 + 16;
    DialogChoice(px, 112, y, 320, choice == 0, send ? "CANCEL" : "NO", 136);
    DialogChoice(px, 480, y, 320, choice == 1, send ? "SEND" : "YES", 504);
    Text(px, 112, y + 48, kHint, classic ? "Left/Right: choose  Cross: confirm  Square: cancel" : "Left/Right: choose  Cross: confirm  Circle: cancel");
}

void DrawLeaveDialog(uint32_t* px, const int* ids, int count, int index, bool running, bool classic, bool note) {
    if (count <= 0) return;
    DimScreen(px);
    const int h = 150 + 24 + count * 44 + 40;
    DialogBox(px, 80, 100, 800, h, 116, "SAVE YOUR CHANGES?");
    bool restart = false;
    for (int i = 0; i < count; ++i) restart |= ids[i] == kLeaveSaveRestart;
    Shadowed(px, 112, 172, kDim, "You changed settings and have not saved them.");
    if (note && !restart) Shadowed(px, 112, 196, kDim, "Some changes apply the next time the game starts.");
    for (int i = 0; i < count; ++i) DialogChoice(px, 112, 238 + i * 44, 420, i == index, LeaveLabel(ids[i], running), 136);
    Text(px, 112, 238 + count * 44 + 6, kHint, classic ? "Up/Down: choose  Cross: confirm  Square: keep editing" : "Up/Down: choose  Cross: confirm  Circle: keep editing");
}

int FirstVisibleRow(int page, int selected) {
    const int rows = RowCount(page);
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

// CONTROLS CLASSIC: the game's own menus use Square for back (np_control_table.h), so the screen's hints name it
bool ClassicLayout(const State& s) {
    const int o = OptionIndex("CONTROLS");
    return o >= 0 && s.choice[o] == 1;
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
    const int aimRow = OptionIndex("MEGA SAVE AIM");   // (set18: FRONT TOUCH, REAR + CROSS, STICK ONLY; the old TOUCH MEGA SAVE OFF is STICK ONLY)
    const int aim = aimRow >= 0 ? s.choice[aimRow] : 0;
    c.rearSave = aim == 1;
    c.touchSave = aim != 1 && aim != 2;
    c.touchMeter = on("TOUCH MEGA STRIKE");
    c.touchShake = on("TOUCH SHAKEDOWN");
    c.touchPointer = true;   // (set18: the TOUCH POINTER row went; the pointer follows a finger by default)
    return c;
}
}  // namespace

void DrawControls(uint32_t* px, const State& s) {
    UseLanguageOf(s);
    using NpControlTable::LineAt;
    using NpControlTable::LineCount;
    const NpControlTable::Config cfg = ControlsConfig(s);
    const bool modern = cfg.layout == 1;
    const bool art = ArtOn();
    const uint32_t edge = kEdge;
    Chrome(px, art);
    BluePanel(px, 10, 70, kWidth - 20, 460, art ? 205 : 255);
    int titleEnd = 36 + (Chars("CONTROLS") + 1) * 24;   // (the plain title: its characters at scale 2, and one of space)
    char heading[64];
    const int w = art && g_art->heading ? g_art->heading(nullptr, 0, 0, 40, Folded("CONTROLS", heading, sizeof heading), false) : 0;
    if (w > 0) { g_art->heading(px, 36, 8, 40, heading, true); titleEnd = 36 + w + 28; }
    else Label(px, 36, 8, kWhite, "CONTROLS", 2, true);
    Label(px, titleEnd, 8, kYellow, modern ? "MODERN" : "CLASSIC", 2, true);
    Label(px, kWidth - 36 - 12 * 20, 32, kWhite, modern ? "BATTLE LEAGUE / EA FC" : "GAMECUBE STRIKERS");

    const int col0 = 24, col1 = 236, col2 = 596, top = 72;
    Label(px, col0, top, edge, "BUTTON", 1, true);
    Label(px, col1, top, edge, "WITH THE BALL", 1, true);
    Label(px, col2, top, edge, "WITHOUT THE BALL", 1, true);
    Rect(px, 20, top + 25, kWidth - 40, 2, kGlow);
    int y = top + 30;
    bool rule = false;
    char line[256];
    for (int i = 0; i < LineCount(); ++i) {
        const NpControlTable::Line& l = LineAt(i);
        if (!NpControlTable::Shown(l, cfg)) continue;
        if (NpControlTable::IsSentence(l)) {
            if (!rule) {   // the minigames and the menus below a rule
                Rect(px, 20, y + 2, kWidth - 40, 2, kGlow);
                y += 8;
                rule = true;
            }
            // (1.3) each part translated on its own: the table has the line and its extras, not their sum
            char text[192];
            std::snprintf(text, sizeof text, "%s", Tr(l.text ? l.text : ""));
            for (int e = 0; e < 2; ++e)
                if (l.extra[e] && NpControlTable::NeedsMet(l.extraNeed[e], cfg)) {
                    const size_t used = std::strlen(text);
                    std::snprintf(text + used, sizeof text - used, "%s", Tr(l.extra[e]));
                }
            std::snprintf(line, sizeof line, "%s: %s", Tr(l.input), text);
            Shadowed(px, col0, y, kDim, line);
            y += 24;
            continue;
        }
        const bool on = NpControlTable::Active(l, cfg);
        Shadowed(px, col0, y, on ? kYellow : kHint, l.input);
        Shadowed(px, col1, y, on ? kWhite : kHint, on ? l.withBall : "(REAR TOUCH OFF)");
        if (on) Shadowed(px, col2, y, kWhite, l.withoutBall);
        y += 24;
    }
    Shadowed(px, 24, 516, kDim, "Left/Right: MODERN or CLASSIC   Circle, Cross or Triangle: back to the list");
}

} // namespace NpSettings
