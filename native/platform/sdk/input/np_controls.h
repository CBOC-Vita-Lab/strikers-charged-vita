// np_controls.h: the Vita button layouts of a match (MODERN, the default, and CLASSIC) and the rear touchpad (wave 5 CTL).
//
// Pure code (no game headers): the game-aware layer (np_game_input.cpp) feeds it the Vita buttons of the sample, the rear-panel
// fingers and two facts about the match (does this pad control the ball carrier; is the opponent's Mega Strike meter running
// against it), and gets back the engine's GameCube controls (the bits a GameCube pad would have) and the Wii Remote view that
// menus and the Mega Strike / Mega Save minigames read. The mapping runs BEFORE the capture: the record the simulation uses
// (and sends over the network) is already the final one, so two Vitas with different layouts play one match.
//
//   CLASSIC (Super Mario Strikers on the GameCube, the port's layout until wave 5): every button by position, the identity:
//     Cross A pass/switch, Square B shoot/slide, Circle X item, Triangle Y deke/big hit, L modifier (Z+A lob, Z+B chip,
//     tap = chip ahead), R special (deke/slide toward the stick), Select Z cycle item/shakedown, Start, D-pad.
//   MODERN (Mario Strikers: Battle League and EA FC; docs/wave5-CTL.md):
//     Cross     A          pass; without the ball switch player (held: to the captain)
//     Circle    B          shoot (held: charge, Mega Strike, Skillshot); without the ball slide tackle
//     Square    L+A / Y    with the ball lob pass; without it the big hit       (decided when pressed, kept until released)
//     Triangle  X          use item / the captain's super ability
//     R         L          the "sprint": Charged's turbo (tap = chip the ball ahead and run after it); R+Cross lob, R+Circle chip shot
//     L         L / A      with the ball the same modifier as R; without it switch player (decided when pressed)
//     Select    Z          cycle item; during the opponent's Mega Strike: shakedown
//     rear right tap  A    switch player (only without the ball: a tap never passes)
//     rear left tap   Z    cycle item / shakedown (a swipe there counts too)
//     during the opponent's Mega Strike: Square and Triangle are the shakedown (Z) as well
//   Wii Remote view (menus, Mega Strike meter, Mega Save): CLASSIC Cross A, Square B, Circle C; MODERN Cross A, Circle B,
//   Square C (so Circle goes back in menus and is the shoot button of the meter, as in the match).
#ifndef NP_CONTROLS_H
#define NP_CONTROLS_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace NpControls {

// The Vita's buttons (SCE_CTRL_*, = NP_BTN_* of np_sdk.h) and the GameCube pad bits (PAD_BUTTON_* / PAD_TRIGGER_*).
enum : uint32_t {
    kSelect = 0x0001, kStart = 0x0008, kUp = 0x0010, kRight = 0x0020, kDown = 0x0040, kLeft = 0x0080, kL = 0x0100, kR = 0x0200,
    kTriangle = 0x1000, kCircle = 0x2000, kCross = 0x4000, kSquare = 0x8000
};
enum : uint16_t {
    kGcLeft = 0x0001, kGcRight = 0x0002, kGcDown = 0x0004, kGcUp = 0x0008, kGcZ = 0x0010, kGcR = 0x0020, kGcL = 0x0040,
    kGcA = 0x0100, kGcB = 0x0200, kGcX = 0x0400, kGcY = 0x0800, kGcStart = 0x1000
};
// WPAD bits of the emulated remote (np_game_input.h NPW_*)
enum : uint32_t { kWA = 0x0800, kWB = 0x0400, kWC = 0x4000, kW2 = 0x0100 };

enum Layout { kClassic = 0, kModern = 1 };
// MSC_CONTROLS=classic|modern (np-player.txt; default modern). Read once. (Inline: the Vita platform layer uses these too.)
inline Layout ConfiguredLayout() {
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("MSC_CONTROLS");
        v = (e && (!strcmp(e, "classic") || !strcmp(e, "CLASSIC") || !strcmp(e, "0"))) ? kClassic : kModern;
    }
    return (Layout)v;
}
// MSC_REAR_TOUCH: 0 off, 1 on; unset = on with MODERN, off with CLASSIC. Read once.
inline bool RearEnabled(Layout layout) {
    static int v = -2;
    if (v == -2) {
        const char* e = getenv("MSC_REAR_TOUCH");
        v = (e && *e) ? (*e != '0' ? 1 : 0) : -1;
    }
    return v < 0 ? layout == kModern : v == 1;
}
const char* LayoutName(Layout layout);

// The GameCube bits of the Vita buttons by position (CLASSIC; the SDK's gc_from_raw).
uint16_t ClassicGc(uint32_t vita);
// The Wii Remote view: `hold` as the SDK built it by position (CLASSIC); returns it for `layout` (MODERN swaps the roles of
// Circle and Square). `bareRemote` = the NP_PAD_PROFILE=remote profile (Circle is "2" there, not C).
uint32_t WiiHold(Layout layout, uint32_t vita, uint32_t hold, bool bareRemote);

// ---- reading the Vita's buttons without losing a tap ----------------------------------------------------------------------
// The controller is sampled at every vertical blank (60 Hz) and the game reads it once per frame (30 fps): a button pressed and
// released between two reads would be lost. Given the buffered samples of one read (any order, with their time stamps) returns
// the buttons to report: the newest sample's, plus every button that was down in a sample newer than the previous read's newest
// (so a quick tap is reported once, for one read). `lastStamp` carries the newest stamp from read to read. MSC_TAP_LATCH=0 = off.
inline uint32_t LatchTaps(const uint32_t* buttons, const uint64_t* stamps, int count, uint64_t* lastStamp) {
    if (count <= 0) return 0;
    int newest = 0;
    for (int i = 1; i < count; ++i)
        if (stamps[i] > stamps[newest]) newest = i;
    uint32_t out = buttons[newest];
    const uint64_t since = lastStamp ? *lastStamp : 0;
    if (lastStamp && since != 0)
        for (int i = 0; i < count; ++i)
            if (i != newest && stamps[i] > since) out |= buttons[i];
    if (lastStamp) *lastStamp = stamps[newest];
    return out;
}
inline bool TapLatchEnabled() {
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("MSC_TAP_LATCH");
        v = !(e && *e == '0');
    }
    return v == 1;
}

// ---- the rear touchpad ----------------------------------------------------------------------------------------------------
// Fingers on the back in the 960 x 544 space of the front panel (the panel's own range scaled to it; x grows to the right
// as seen from the FRONT, so the "right half" is under the right hand). What counts:
//   * a TAP: a finger that lands inside a zone and lifts within kTapMs, having moved less than kTapMovePx;
//   * on the left zone also a SWIPE (kSwipePx within kSwipeMs) as soon as it happens (the shakedown);
//   * nothing else: a finger already resting when the zone was armed, one that stays, one landing in the edge margin or the
//     middle strip, and every finger while three or more touch the back (a grip) never count.
struct RearPoint {
    int id;
    float x, y;
};
class RearPad {
public:
    static constexpr int kMaxPoints = 8;
    static constexpr uint32_t kTapMs = 300, kSwipeMs = 400;
    static constexpr float kTapMovePx = 60.0f, kSwipePx = 48.0f;
    static constexpr float kEdgeX = 40.0f, kEdgeY = 30.0f, kMidLo = 440.0f, kMidHi = 520.0f;
    enum Zone { kNone = 0, kLeftZone = 1, kRightZone = 2 };
    static Zone ZoneOf(float x, float y);
    // One sample. Returns the taps (and the left swipe) completed in it: bit 0 left, bit 1 right.
    unsigned Update(const RearPoint* points, int count, uint32_t nowMs);
    void Reset();

private:
    struct Contact {
        int id;
        Zone zone;
        bool valid;      // landed in a zone, not part of a grip
        bool fired;      // the swipe already counted
        uint32_t downMs;
        float x0, y0, maxMove;
        bool seen;
    };
    Contact c_[kMaxPoints];
    int n_ = 0;
};

// ---- MODERN: the Vita buttons of one capture as the engine's GameCube controls ----------------------------------------
struct Context {
    bool possession;   // this pad controls the ball carrier
    bool shakedown;    // the opponent's Mega Strike meter runs against this pad
};
class Mapper {
public:
    // `rearRight` / `rearLeft`: taps not yet turned into a press (decremented when taken). One call per capture.
    uint16_t Map(uint32_t vita, int* rearRight, int* rearLeft, const Context& ctx);
    void Reset();

private:
    uint32_t prev_ = 0;
    uint16_t squareBits_ = 0, lBits_ = 0, triangleBits_ = 0;
    int rightPulse_ = 0, rightGap_ = 0, leftPulse_ = 0, leftGap_ = 0;
};

}  // namespace NpControls

#endif  // NP_CONTROLS_H
