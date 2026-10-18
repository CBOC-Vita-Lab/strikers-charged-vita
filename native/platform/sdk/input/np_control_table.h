// np_control_table.h: THE table of what every Vita control does in a match (W5 CTL), one source for everything that tells the player:
//   * the Settings screen's controls page (Triangle; platform/vita/src/np_settings.cpp DrawControls),
//   * the pause menu's controller map (a picture drawn from these lines; np_prompt_art.cpp),
//   * the button names in the game's rewritten texts and button pictures (Roles below; np_prompts.cpp, np_prompt_art.cpp).
// It is checked against the mapping itself: platform/sdk/tests/test_controls.cpp runs every Test below through np_controls.h (Mapper
// for MODERN, the by-position map for CLASSIC, WiiHold for the Wii Remote view) and compares the engine bits; tools/ctl/control_lines.cpp
// turns the same Tests into a harness script that presses each input in its context in the real game and checks that the engine did
// the action ([ctl] counters, platform/sdk/input/np_game_input.cpp). Pure code, header only (the game and the Vita's Settings screen,
// which links without the game, both include it): no game headers.
#ifndef NP_CONTROL_TABLE_H
#define NP_CONTROL_TABLE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "np_controls.h"

namespace NpControlTable {

// What a line needs to be on (the player's switches; np_controls.h, np_touch.cpp).
enum Need : uint8_t { kAlways = 0, kRearOn = 1, kTouchSave = 2, kTouchMeter = 4, kTouchShake = 8, kTouchPointer = 16 };
struct Config {
    int layout;   // 0 CLASSIC, 1 MODERN (np_controls.h Layout)
    bool rear, touchSave, touchMeter, touchShake, touchPointer;
};
inline uint8_t LayoutBit(int layout) { return (uint8_t)(layout == 1 ? 2 : 1); }

// Where an input is tried (the contexts of the scripted test, and the column of the line).
enum Ctx : uint8_t {
    kAttack,    // the local player controls the ball carrier (a fielder)
    kDefence,   // the other team has the ball
    kCaptain,   // the local captain has the ball (the Mega Strike: the shot held until the meter opens)
    kMeter,     // the own Mega Strike meter is up
    kSave,      // the opponent's Mega Strike: the keeper's Mega Save
    kShake,     // the opponent's Mega Strike meter: the shakedown
    kPlay,      // open play, any possession
    kPaused,    // the pause menu
    kMenu,      // the main menu
    kSubMenu    // a menu one step from the main menu (the options)
};
// What the unit test compares: the engine's GameCube bits after the layout's mapping, the Wii Remote view, or nothing (a layer the unit
// test cannot run: the touch gate, the right stick, the pause menu; the scripted test covers those).
enum Expect : uint8_t { kGc, kWii, kScriptOnly };

struct Test {
    uint8_t layouts;   // LayoutBit()s
    uint8_t need;      // Need bits
    Ctx ctx;
    uint32_t vita;     // the Vita buttons pressed (SCE_CTRL bits, np_controls.h kCross ...)
    uint8_t rearTap;   // 1 a rear-left tap, 2 a rear-right tap
    Expect expect;
    uint32_t bits;     // kGc: the GameCube bits the engine gets; kWii: the Wii Remote bits
    const char* drive;     // the harness commands that press it (';' between commands)
    const char* counter;   // the harness variable that shows the action: "cpass" must go up, "=cpass" must stay, "-scene" must go
                           // down, "~onctx" must change; a drive starting "?<cond>;" also needs <cond> (harness syntax)
    const char* setup;     // harness commands before (items for the item lines), or 0
};

struct Line {
    uint8_t layouts;       // LayoutBit()s
    uint8_t need;          // the line works only with these switches on (shown greyed otherwise)
    const char* input;     // "CROSS", "R + CROSS", "BACK RIGHT TAP", "MEGA STRIKE" ...
    const char* withBall;  // a two-column line: what it does with the ball / without it ("-" nothing);
    const char* withoutBall;
    const char* text;      // a sentence line (minigames, menus): withBall == 0; extras appended when their need is met
    const char* extra[2];
    uint8_t extraNeed[2];
    Test tests[6];         // the checks of this line (drive == 0 ends the list)
};

inline bool IsSentence(const Line& l) { return l.withBall == 0; }

// ---- roles: the Wii controls the game names, and what they are on the Vita in each layout ---------------------------------
enum Role {
    kRoleA,       // Wii A: pass / switch player / OK
    kRoleB,       // Wii B: shoot / slide tackle / back (and the Mega Strike meter)
    kRoleC,       // Nunchuk C: use item
    kRoleZ,       // Nunchuk Z: the modifier (chip the ball ahead, lob, chip shot)
    kRoleOne,     // Wii 1: pause
    kRolePlus,    // Wii +: next page in menus
    kRoleMinus,   // Wii -: previous page in menus
    kRoleDpad,    // the + Control Pad: deke / slide tackle
    kRoleShake,   // shaking the Wii Remote: the big hit
    kRoleFlick,   // flicking the Nunchuk: cycle items / shakedown
    kRoleCount
};
// RoleName: the Vita control's name ("CROSS", "R", "D-PAD", "SELECT"); RoleSymbol: its picture (np_art.h Symbol: 0 cross 1 circle
// 2 square 3 triangle 4 L 5 R 6 start 7 select; -1 none: the D-pad).

// ---- the table ------------------------------------------------------------------------------------------------------------
// What the player reads is the text; the tests under each line are how that line is proven (unit: the bits through np_controls.h;
// script: the action in the game).
namespace detail {

using namespace NpControls;

constexpr uint8_t C = 1, M = 2, B = 3;   // layouts: CLASSIC, MODERN, both
// Wii Remote view bits (np_game_input.h NPW_*)
constexpr uint32_t kWiiA = 0x0800, kWiiB = 0x0400, kWii1 = 0x0200;

// Harness snippets (tools/ctl/control_lines.cpp turns ';' into lines): run with the ball toward the net; run to the ball carrier.
#define ATK "vchase 8 none goal;"
#define DEF "vchase 20 none ball;"
#define ITEMS1 "item me green 1 set;idle 10"
#define ITEMS2 "item me green 1 set;item me banana 1 add;idle 10"

// clang-format off
inline const Line* Lines(int* count) {
    static const Line kLines[] = {
    // ---- MODERN (Mario Strikers: Battle League / EA FC positions) ----------------------------------------------------------------
    {M, kAlways, "CROSS", "PASS", "SWITCH PLAYER (HOLD: CAPTAIN)", 0, {0, 0}, {0, 0}, {
        {M, 0, kAttack, kCross, 0, kGc, kGcA, ATK "vpress cross 3 0", "cpass", 0},
        {M, 0, kDefence, kCross, 0, kGc, kGcA, "vpress cross 3 0", "cswitch", 0},
        {M, 0, kDefence, kCross, 0, kGc, kGcA, "?cplayer != 0;vhold 24 cross", "ccaptain", 0}}},
    {M, kAlways, "CIRCLE", "SHOOT (HOLD: CHARGE)", "SLIDE TACKLE", 0, {0, 0}, {0, 0}, {
        {M, 0, kAttack, kCircle, 0, kGc, kGcB, ATK "vchase 6 circle goal", "cshot", 0},
        {M, 0, kDefence, kCircle, 0, kGc, kGcB, DEF "vpress circle 3 0", "cslide", 0}}},
    {M, kAlways, "SQUARE", "LOB PASS", "BIG HIT", 0, {0, 0}, {0, 0}, {
        {M, 0, kAttack, kSquare, 0, kGc, kGcL | kGcA, ATK "vpress square 3 0", "clob", 0},
        {M, 0, kDefence, kSquare, 0, kGc, kGcY, DEF "vpress square 3 0", "chit", 0}}},
    {M, kAlways, "TRIANGLE", "USE NEXT ITEM / SUPER ABILITY", "USE NEXT ITEM / SUPER ABILITY", 0, {0, 0}, {0, 0}, {
        {M, 0, kAttack, kTriangle, 0, kGc, kGcX, ATK "vpress triangle 3 0", "citem", ITEMS1},
        {M, 0, kDefence, kTriangle, 0, kGc, kGcX, "vpress triangle 3 0", "citem", ITEMS1}}},
    {M, kAlways, "R", "CHIP AHEAD AND RUN (TURBO)", "-", 0, {0, 0}, {0, 0}, {
        {M, 0, kAttack, kR, 0, kGc, kGcL, ATK "vchase 3 r goal", "cturbo", 0}}},
    {M, kAlways, "R + CROSS/CIRCLE", "LOB PASS / CHIP SHOT", "-", 0, {0, 0}, {0, 0}, {
        {M, 0, kAttack, kR | kCross, 0, kGc, kGcL | kGcA, ATK "vpress r+cross 3 0", "clob", 0},
        {M, 0, kAttack, kR | kCircle, 0, kGc, kGcL | kGcB, ATK "vchase 6 r+circle goal;vchase 4 r goal", "cchip", 0}}},
    {M, kAlways, "L", "AS R (L + CIRCLE: CHIP SHOT)", "SWITCH PLAYER (HOLD: CAPTAIN)", 0, {0, 0}, {0, 0}, {
        {M, 0, kAttack, kL, 0, kGc, kGcL, ATK "vchase 3 l goal", "cturbo", 0},
        {M, 0, kAttack, kL | kCircle, 0, kGc, kGcL | kGcB, ATK "vchase 6 l+circle goal;vchase 4 l goal", "cchip", 0},
        {M, 0, kDefence, kL, 0, kGc, kGcA, "vpress l 3 0", "cswitch", 0},
        {M, 0, kDefence, kL, 0, kGc, kGcA, "?cplayer != 0;vhold 24 l", "ccaptain", 0}}},
    // ---- CLASSIC (Super Mario Strikers on the GameCube: every button by position) --------------------------------------------
    {C, kAlways, "CROSS", "PASS", "SWITCH PLAYER (HOLD: CAPTAIN)", 0, {0, 0}, {0, 0}, {
        {C, 0, kAttack, kCross, 0, kGc, kGcA, ATK "vpress cross 3 0", "cpass", 0},
        {C, 0, kDefence, kCross, 0, kGc, kGcA, "vpress cross 3 0", "cswitch", 0},
        {C, 0, kDefence, kCross, 0, kGc, kGcA, "?cplayer != 0;vhold 24 cross", "ccaptain", 0}}},
    {C, kAlways, "SQUARE", "SHOOT (HOLD: CHARGE)", "SLIDE TACKLE", 0, {0, 0}, {0, 0}, {
        {C, 0, kAttack, kSquare, 0, kGc, kGcB, ATK "vchase 6 square goal", "cshot", 0},
        {C, 0, kDefence, kSquare, 0, kGc, kGcB, DEF "vpress square 3 0", "cslide", 0}}},
    {C, kAlways, "CIRCLE", "USE NEXT ITEM / SUPER ABILITY", "USE NEXT ITEM / SUPER ABILITY", 0, {0, 0}, {0, 0}, {
        {C, 0, kAttack, kCircle, 0, kGc, kGcX, ATK "vpress circle 3 0", "citem", ITEMS1},
        {C, 0, kDefence, kCircle, 0, kGc, kGcX, "vpress circle 3 0", "citem", ITEMS1}}},
    {C, kAlways, "TRIANGLE", "DEKE", "BIG HIT", 0, {0, 0}, {0, 0}, {
        {C, 0, kAttack, kTriangle, 0, kGc, kGcY, ATK "vpress triangle 3 0", "cdeke", 0},
        {C, 0, kDefence, kTriangle, 0, kGc, kGcY, DEF "vpress triangle 3 0", "chit", 0}}},
    {C, kAlways, "L", "CHIP AHEAD AND RUN (TURBO)", "-", 0, {0, 0}, {0, 0}, {
        {C, 0, kAttack, kL, 0, kGc, kGcL, ATK "vchase 3 l goal", "cturbo", 0}}},
    {C, kAlways, "L + CROSS/SQUARE", "LOB PASS / CHIP SHOT", "-", 0, {0, 0}, {0, 0}, {
        {C, 0, kAttack, kL | kCross, 0, kGc, kGcL | kGcA, ATK "vpress l+cross 3 0", "clob", 0},
        {C, 0, kAttack, kL | kSquare, 0, kGc, kGcL | kGcB, ATK "vchase 6 l+square goal;vchase 4 l goal", "cchip", 0}}},
    {C, kAlways, "R", "DEKE TOWARD THE STICK", "SLIDE TOWARD THE STICK", 0, {0, 0}, {0, 0}, {
        {C, 0, kAttack, kR, 0, kGc, kGcR, ATK "vchase 3 r goal", "cdeke", 0},
        {C, 0, kDefence, kR, 0, kGc, kGcR, DEF "vchase 3 r ball", "cslide", 0}}},
    // ---- both layouts ----------------------------------------------------------------------------------------------------------
    {B, kAlways, "RIGHT STICK", "DEKE (FLICK)", "-", 0, {0, 0}, {0, 0}, {
        {B, 0, kAttack, 0, 0, kScriptOnly, 0, ATK "vchase 4 none goal 72 0", "cdeke", 0}}},
    {B, kAlways, "D-PAD", "DEKE", "SLIDE TACKLE", 0, {0, 0}, {0, 0}, {
        {B, 0, kAttack, kRight, 0, kGc, kGcRight, ATK "vhold 3 right", "cdeke", 0},
        {B, 0, kDefence, kRight, 0, kGc, kGcRight, DEF "vhold 3 right", "cslide", 0}}},
    {B, kAlways, "SELECT", "NEXT ITEM", "NEXT ITEM", 0, {0, 0}, {0, 0}, {
        {B, 0, kAttack, kSelect, 0, kGc, kGcZ, ATK "vpress select 3 0", "ccycle", ITEMS2},
        {B, 0, kDefence, kSelect, 0, kGc, kGcZ, "vpress select 3 0", "ccycle", ITEMS2}}},
    {B, kRearOn, "BACK RIGHT TAP", "- (NEVER PASSES)", "SWITCH PLAYER", 0, {0, 0}, {0, 0}, {
        {B, kRearOn, kAttack, 0, 2, kGc, 0, ATK "rtouch 720 272 3 0", "=cpass", 0},
        {B, kRearOn, kDefence, 0, 2, kGc, kGcA, "rtouch 720 272 3 0", "cswitch", 0}}},
    {B, kRearOn, "BACK LEFT TAP", "NEXT ITEM", "NEXT ITEM", 0, {0, 0}, {0, 0}, {
        {B, kRearOn, kAttack, 0, 1, kGc, kGcZ, ATK "rtouch 220 272 3 0", "ccycle", ITEMS2},
        {B, kRearOn, kDefence, 0, 1, kGc, kGcZ, "rtouch 220 272 3 0", "ccycle", ITEMS2}}},
    {B, kAlways, "START", "PAUSE", "PAUSE", 0, {0, 0}, {0, 0}, {
        {B, 0, kPlay, kStart, 0, kGc, kGcStart, "vpress start 3 0", "paused", 0},
        {B, 0, kPaused, kStart, 0, kWii, kWii1, "idle 20;vpress start 3 0", "-paused", 0}}},
    // ---- the minigames and the menus ------------------------------------------------------------------------------------------
    {M, kAlways, "MEGA STRIKE", 0, 0, "CAPTAIN HOLDS CIRCLE, THEN CIRCLE ON EACH MARK", {" (OR A FINGER)", 0}, {kTouchMeter, 0}, {
        {M, 0, kCaptain, kCircle, 0, kGc, kGcB, "vchase 15 none goal;try vmash circle 1 cmode == 2 || chave == 0 200", "cmega", 0},
        {M, 0, kMeter, kCircle, 0, kWii, kWiiB, "idle 3;vpress circle 3 0", "cmeterpress", 0},
        {M, kTouchMeter, kMeter, 0, 0, kScriptOnly, 0, "idle 3;touch 480 272 6 0", "cmeterpress", 0}}},
    {C, kAlways, "MEGA STRIKE", 0, 0, "CAPTAIN HOLDS SQUARE, THEN SQUARE ON EACH MARK", {" (OR A FINGER)", 0}, {kTouchMeter, 0}, {
        {C, 0, kCaptain, kSquare, 0, kGc, kGcB, "vchase 15 none goal;try vmash square 1 cmode == 2 || chave == 0 200", "cmega", 0},
        {C, 0, kMeter, kSquare, 0, kWii, kWiiB, "idle 3;vpress square 3 0", "cmeterpress", 0},
        {C, kTouchMeter, kMeter, 0, 0, kScriptOnly, 0, "idle 3;touch 480 272 6 0", "cmeterpress", 0}}},
    {B, kAlways, "MEGA SAVE", 0, 0, "AIM WITH A STICK, BLOCK WITH CROSS OR R", {" (OR TOUCH)", 0}, {kTouchSave, 0}, {
        {B, 0, kSave, kCross, 0, kWii, kWiiA, "vpress cross 3 0", "csavea", 0},
        {B, 0, kSave, kR, 0, kScriptOnly, 0, "idle 3;vpress r 3 0", "csavea", 0},
        {B, kTouchSave, kSave, 0, 0, kScriptOnly, 0, "touch 480 300 3 0", "csavea", 0}}},
    {M, kAlways, "THEIR MEGA STRIKE", 0, 0, "MASH SQUARE, TRIANGLE OR SELECT", {", TAP BACK LEFT", ", SWIPE"}, {kRearOn, kTouchShake}, {
        {M, 0, kShake, kSquare, 0, kGc, kGcZ, "vpress square 3 0", "cshake", 0},
        {M, 0, kShake, kTriangle, 0, kGc, kGcZ, "vpress triangle 3 0", "cshake", 0},
        {M, 0, kShake, kSelect, 0, kGc, kGcZ, "vpress select 3 0", "cshake", 0},
        {M, kRearOn, kShake, 0, 1, kGc, kGcZ, "rtouch 220 272 3 0", "cshake", 0},
        {M, kTouchShake, kShake, 0, 0, kScriptOnly, 0, "swipe 300 300 420 300 6 0", "cshake", 0}}},
    {C, kAlways, "THEIR MEGA STRIKE", 0, 0, "MASH SELECT", {", TAP BACK LEFT", ", SWIPE"}, {kRearOn, kTouchShake}, {
        {C, 0, kShake, kSelect, 0, kGc, kGcZ, "vpress select 3 0", "cshake", 0},
        {C, kRearOn, kShake, 0, 1, kGc, kGcZ, "rtouch 220 272 3 0", "cshake", 0},
        {C, kTouchShake, kShake, 0, 0, kScriptOnly, 0, "swipe 300 300 420 300 6 0", "cshake", 0}}},
    {M, kAlways, "MENUS", 0, 0, "CROSS OK, CIRCLE BACK, D-PAD/STICKS MOVE, L/R PAGE", {", A FINGER POINTS", 0}, {kTouchPointer, 0}, {
        {M, 0, kMenu, kCross, 0, kWii, kWiiA, "vpress cross 3 0", "scene", 0},
        {M, 0, kSubMenu, kCircle, 0, kWii, kWiiB, "vpress circle 3 0", "-scene", 0},
        {M, 0, kMenu, kDown, 0, kScriptOnly, 0, "vpress down 1 0", "~onctx", 0},
        {M, kTouchPointer, kMenu, 0, 0, kScriptOnly, 0, "touch 474 58 3 0", "scene", 0}}},
    {C, kAlways, "MENUS", 0, 0, "CROSS OK, SQUARE BACK, D-PAD/STICKS MOVE, L/R PAGE", {", A FINGER POINTS", 0}, {kTouchPointer, 0}, {
        {C, 0, kMenu, kCross, 0, kWii, kWiiA, "vpress cross 3 0", "scene", 0},
        {C, 0, kSubMenu, kSquare, 0, kWii, kWiiB, "vpress square 3 0", "-scene", 0},
        {C, 0, kMenu, kDown, 0, kScriptOnly, 0, "vpress down 1 0", "~onctx", 0},
        {C, kTouchPointer, kMenu, 0, 0, kScriptOnly, 0, "touch 474 58 3 0", "scene", 0}}},
    };
    *count = (int)(sizeof kLines / sizeof kLines[0]);
    return kLines;
}
// clang-format on
#undef ATK
#undef DEF
#undef ITEMS1
#undef ITEMS2

struct RoleRow {
    const char* name[2];   // CLASSIC, MODERN
    int symbol[2];
    uint32_t buttons[2];
};
// symbols: 0 cross 1 circle 2 square 3 triangle 4 L 5 R 6 start 7 select (np_art.h)
inline const RoleRow* Roles() {
    static const RoleRow kRoles[kRoleCount] = {
    {{"CROSS", "CROSS"}, {0, 0}, {kCross, kCross}},                  // A
    {{"SQUARE", "CIRCLE"}, {2, 1}, {kSquare, kCircle}},              // B
    {{"CIRCLE", "TRIANGLE"}, {1, 3}, {kCircle, kTriangle}},          // C
    {{"L", "R"}, {4, 5}, {kL, kR}},                                  // Z
    {{"START", "START"}, {6, 6}, {kStart, kStart}},                  // 1
    {{"R", "R"}, {5, 5}, {kR, kR}},                                  // +
    {{"L", "L"}, {4, 4}, {kL, kL}},                                  // -
    {{"D-PAD", "D-PAD"}, {-1, -1}, {kUp | kDown | kLeft | kRight, kUp | kDown | kLeft | kRight}},
    {{"TRIANGLE", "SQUARE"}, {3, 2}, {kTriangle, kSquare}},          // shake the remote: the big hit
    {{"SELECT", "SELECT"}, {7, 7}, {kSelect, kSelect}},              // flick the nunchuk: next item / shakedown
    };
    return kRoles;
}

inline bool EnvOff(const char* name) {
    const char* e = getenv(name);
    return e && *e == '0';
}

}  // namespace detail

inline Config ConfigFromEnv() {
    using namespace NpControls;
    using detail::EnvOff;
    Config c;
    const Layout layout = ConfiguredLayout();
    c.layout = (int)layout;
    c.rear = RearEnabled(layout);
    const bool minigames = !EnvOff("MSC_TOUCH_MINIGAMES");
    c.touchSave = minigames && !EnvOff("MSC_TOUCH_SAVE");
    c.touchMeter = minigames && !EnvOff("MSC_TOUCH_METER");
    c.touchShake = minigames && !EnvOff("MSC_TOUCH_SHAKE");
    c.touchPointer = !EnvOff("NP_TOUCH_POINTER");
    return c;
}

inline bool NeedsMet(uint8_t need, const Config& c) {
    if ((need & kRearOn) && !c.rear) return false;
    if ((need & kTouchSave) && !c.touchSave) return false;
    if ((need & kTouchMeter) && !c.touchMeter) return false;
    if ((need & kTouchShake) && !c.touchShake) return false;
    if ((need & kTouchPointer) && !c.touchPointer) return false;
    return true;
}

inline int LineCount() {
    int n = 0;
    detail::Lines(&n);
    return n;
}
inline const Line& LineAt(int i) {
    int n = 0;
    return detail::Lines(&n)[i];
}
inline bool Shown(const Line& l, const Config& c) { return (l.layouts & LayoutBit(c.layout)) != 0; }
inline bool Active(const Line& l, const Config& c) { return NeedsMet(l.need, c); }

inline const char* Sentence(const Line& l, const Config& c, char* buf, size_t size) {
    if (!buf || !size) return "";
    snprintf(buf, size, "%s", l.text ? l.text : "");
    for (int i = 0; i < 2; ++i)
        if (l.extra[i] && NeedsMet(l.extraNeed[i], c)) {
            const size_t n = strlen(buf);
            if (n < size) snprintf(buf + n, size - n, "%s", l.extra[i]);
        }
    return buf;
}

inline const char* RoleName(int layout, Role r) { return (unsigned)r < (unsigned)kRoleCount ? detail::Roles()[r].name[layout == 1 ? 1 : 0] : ""; }
inline int RoleSymbol(int layout, Role r) { return (unsigned)r < (unsigned)kRoleCount ? detail::Roles()[r].symbol[layout == 1 ? 1 : 0] : -1; }
inline uint32_t RoleButtons(int layout, Role r) { return (unsigned)r < (unsigned)kRoleCount ? detail::Roles()[r].buttons[layout == 1 ? 1 : 0] : 0; }

}  // namespace NpControlTable

#endif  // NP_CONTROL_TABLE_H
