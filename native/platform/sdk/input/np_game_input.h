// np_game_input.h: the game-aware controller layer of the native port (platform/sdk/input).
//
// Compiled with the game (PLAT_DIRS of the main Makefile: the game's flags and headers), because it reads the game's own
// state: the menu's pointer regions, the scene stack, the teams and players of a match. It is what the recompiled
// build did in hle/input/{pad,kpad}.cpp (menu navigation, the touch minigames) and hle/msc_controller_input.cpp +
// msc_game.cpp (GameCube controls from the Vita pad), written against the decompiled classes instead of guest
// memory. docs/wave2-NPS2.md describes it; the pieces:
//
//   np_menu_nav.cpp     D-pad / stick navigation between the buttons of a menu (the pointer jumps to the next button)
//   np_touch.cpp        the touch minigames (Mega Save aiming, the Mega Strike meter, the shakedown swipe): the gate that
//                       says which minigame the local pad owns, the gesture state, the physical R-button punch
//   np_game_input.cpp   NpPadAdjust (called by the SDK for every sample of the emulated Wii Remote), the capture-time
//                       conversion of the Vita pad into the engine's GameCube controls, the GameCube forms of the
//                       Wii-only actions (big hit, item toggle, deke / slide / special)
#ifndef NP_GAME_INPUT_H
#define NP_GAME_INPUT_H

#include <stdint.h>

#include "platform/sdk/include/np/np_sdk.h"

class DetInput;
class NetworkPeerChannel;
class cAIPad;
class cFielder;

// The SDK layer's clock (virtual in harness runs). Weak: a build without the SDK layer (plain host32) falls back to the
// libc clock and has no controller anyway.
extern "C" uint64_t np_time_ns(void) __attribute__((weak));
uint64_t NpGameNowNsFallback();
static inline uint64_t NpGameNowNs() { return np_time_ns ? np_time_ns() : NpGameNowNsFallback(); }

struct NpPoint {
    float x, y;
};

// WPAD button bits (the emulated remote's hold word)
enum : uint32_t {
    NPW_LEFT = 0x0001, NPW_RIGHT = 0x0002, NPW_DOWN = 0x0004, NPW_UP = 0x0008, NPW_PLUS = 0x0010, NPW_2 = 0x0100,
    NPW_1 = 0x0200, NPW_B = 0x0400, NPW_A = 0x0800, NPW_MINUS = 0x1000, NPW_Z = 0x2000, NPW_C = 0x4000
};

// ---- menu navigation ----------------------------------------------------------------------------------------------
namespace NpMenuNav {
enum : uint32_t { kUp = 1, kDown = 2, kLeft = 4, kRight = 8 };
struct Result {
    bool menu = false;        // a menu is up: navigation owns the D-pad and the left stick
    bool hidePointer = false; // report no IR pointer
    bool pressA = false;      // tap A (B goes back by tapping BACK)
    bool pressPause = false;  // tap the pause button (B on an in-match overlay)
    int page = 0;             // tap - (-1) or + (+1): L/R page through screens
};
bool Enabled();
// One navigation step for channel `chan`, once per sample. `dir` = kUp|kDown|kLeft|kRight from the D-pad and the sticks,
// `stick` the stronger stick (ring menus), `back` = B held, `pageInput` = +1 (R) / -1 (L) / 0. `cursor` is the persistent
// pointer in KPAD space (-1..1, +y down); a navigation step places it. `dt` = seconds since the previous sample.
Result Update(int chan, uint32_t dir, NpPoint stick, bool back, int pageInput, float cursor[2], float dt);
// The hand marks the selection: the game hides it for a moment while B goes back.
void ForgetChannel(int chan);
}  // namespace NpMenuNav

// ---- the touch minigames -----------------------------------------------------------------------------------------
namespace NpTouch {
enum class Mode { None, Save, Meter, Shake };
// Applies the minigame controls to one sample of the emulated remote of `chan` (channel 0 only; others and
// untagged samples are left alone): touch -> A / B holds, the Mega Save pointer, the R-button punch. Returns true
// when the sample was touched by a minigame.
bool Apply(int chan, const NpRawPad& raw, uint32_t* hold, float cursor[2], uint32_t epoch, uint32_t nowMs);
// Capture side: the local pad is in the gate of a Mega Save (the engine must keep the Wii controller for the keeper)?
// `det` = the channel's apply-side DetInput (&NetworkPeerChannel::mInput).
bool PreserveWii(const DetInput* det, int channel);
// A shakedown swipe happened (the defender "shakes"): returns once per swipe, then false until the next one.
bool ConsumeShake(const DetInput* det);
// Mega Save: the right stick aims instead of the left one while it is deflected.
void SelectSavePointerAxes(int chan, float rightX, float rightY, float* x, float* y);
Mode CurrentMode(int chan);
// A touch position (panel pixels) as a pointer position (KPAD space, -1..1, +y down) over the picture's rectangle.
void PanelToCursor(float px, float py, float out[2]);
}  // namespace NpTouch

// The part of the 960 x 544 touch panel the game's picture covers (the whole panel by default; a letterboxed or pillarboxed
// presentation passes its rectangle, flipY = the picture's y runs the other way). Touch outside it does not count in a minigame.
extern "C" void NpTouchSetViewport(float x, float y, float width, float height, int flipY);

// ---- the game side (called from patches in the game's own files, weak there) --------------------------------------------
extern "C" {
// feBackButton.cpp: FEBackButton() records its vtable pointer so the menu navigation can tell the BACK button.
void NpNoteBackButtonVptr(void* backButton);
// NetworkInput.cpp: NetworkPeerChannel::CaptureNetworkPeerChannelInput, after mLocalInput.ReadFromPad(pad).
void NpInputAfterCapture(NetworkPeerChannel* channel);
// AIPad.cpp: cAIPad::DetectLeftShake / DetectRightShake for a GameCube-kind controller (see np_game_input.cpp).
bool NpInputGcLeftShake(cAIPad* pad, uint16_t* direction);
bool NpInputGcRightShake(cAIPad* pad, uint16_t* direction);
// Fielder.cpp: cFielder::GetDekePadDirection for a GameCube-kind controller: the button forms of slide tackle, deke and
// the character special. Returns true and the direction when one is requested.
bool NpInputGcActionAngle(cFielder* fielder, uint16_t* direction);
// For the LAN glue (platform/sdk/net): the captured record must not depend on this console's controller layout.
bool NpInputNormalizeForNetwork(DetInput* captured);
}

#endif  // NP_GAME_INPUT_H
