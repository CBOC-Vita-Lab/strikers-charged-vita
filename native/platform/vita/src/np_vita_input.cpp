// PS Vita controls -> the GameCube pad of the nav scripts and of the SDK layer's PAD functions
// (np_vita.h: NpVitaMapGcPad), the raw controls and the front touch panel (NpVitaReadInput).
// Ported from the recompiled build's vita_input.cpp (one/vita/src); the mapping and the stick
// scaling are unchanged:
//
//   Vita            GameCube pad      what the game does with it (through the SDK layer)
//   Cross           A                 menus: select; match: pass
//   Square          B                 menus: back;   match: shoot
//   Circle          X                 Nunchuk C
//   Triangle        Y
//   L               L (digital+255)   Nunchuk Z; menus: previous page
//   R               R (digital+255)   menus: next page
//   Select          Z                 (the Vita has no third shoulder button)
//   Start           Start             Wii Remote "1" / pause
//   D-pad           D-pad             menu navigation
//   Left stick      main stick        Nunchuk stick, menu navigation
//   Right stick     C-stick           free pointer in menus
//
// Sticks: the game's input path treats +-72 as full deflection (it divides by 72 and clamps). The
// Vita's sticks rest anywhere within about +-12 of 128 and do not reach 0/255 on every unit, so
// a 16-unit radial dead zone is removed and the rest is scaled so that 80% of physical travel
// already gives +-72. Vita Y grows downwards; the GameCube's grows upwards.
#include "np_vita.h"
#include "np_vita_internal.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

#if defined(__vita__)
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#endif

#include "../../sdk/input/np_controls.h"   // W5 CTL: the tap latch and the rear-touch switch (inline, no game code)

// GameCube pad bits (the nav scripts' and the SDK's PAD_* values).
namespace {
enum : uint16_t {
    PAD_BUTTON_LEFT = 0x0001, PAD_BUTTON_RIGHT = 0x0002, PAD_BUTTON_DOWN = 0x0004, PAD_BUTTON_UP = 0x0008,
    PAD_TRIGGER_Z = 0x0010, PAD_TRIGGER_R = 0x0020, PAD_TRIGGER_L = 0x0040,
    PAD_BUTTON_A = 0x0100, PAD_BUTTON_B = 0x0200, PAD_BUTTON_X = 0x0400, PAD_BUTTON_Y = 0x0800, PAD_BUTTON_START = 0x1000,
};
using s8 = int8_t;
}

namespace VitaInput {
namespace {

constexpr float kDeadZone = 16.0f;       // raw units around the centre
constexpr float kFullTravel = 102.0f;    // raw units from centre that count as full (80% of 127)
constexpr float kFullScale = 72.0f;      // what the runtime treats as full deflection

uint16_t g_lastButtons = 0;
int g_lastStickX = 0, g_lastStickY = 0;

void MapStick(uint8_t rawX, uint8_t rawY, int8_t& outX, int8_t& outY) {
    const float x = static_cast<float>(rawX) - 128.0f;
    const float y = 128.0f - static_cast<float>(rawY);
    const float magnitude = std::sqrt(x * x + y * y);
    if (magnitude <= kDeadZone) {
        outX = 0;
        outY = 0;
        return;
    }
    float scaled = (magnitude - kDeadZone) / (kFullTravel - kDeadZone);
    if (scaled > 1.0f) scaled = 1.0f;
    const float k = scaled * kFullScale / magnitude;
    outX = static_cast<int8_t>(std::lround(x * k));
    outY = static_cast<int8_t>(std::lround(y * k));
}

} // namespace

void Map(const Raw& raw, NpPadState& status) {
    uint16_t buttons = 0;
    const auto map = [&](uint32_t vita, uint16_t gc) { if (raw.buttons & vita) buttons |= gc; };
    map(kCross, PAD_BUTTON_A);
    map(kSquare, PAD_BUTTON_B);
    map(kCircle, PAD_BUTTON_X);
    map(kTriangle, PAD_BUTTON_Y);
    map(kLTrigger, PAD_TRIGGER_L);
    map(kRTrigger, PAD_TRIGGER_R);
    map(kSelect, PAD_TRIGGER_Z);
    map(kStart, PAD_BUTTON_START);
    map(kUp, PAD_BUTTON_UP);
    map(kDown, PAD_BUTTON_DOWN);
    map(kLeft, PAD_BUTTON_LEFT);
    map(kRight, PAD_BUTTON_RIGHT);
    status.buttons = buttons;
    MapStick(raw.lx, raw.ly, status.stickX, status.stickY);
    MapStick(raw.rx, raw.ry, status.substickX, status.substickY);
    status.triggerL = (raw.buttons & kLTrigger) ? 255 : 0;
    status.triggerR = (raw.buttons & kRTrigger) ? 255 : 0;
    g_lastButtons = buttons;
    g_lastStickX = status.stickX;
    g_lastStickY = status.stickY;
}

void LastState(uint16_t* buttons, int* stickX, int* stickY) {
    if (buttons) *buttons = g_lastButtons;
    if (stickX) *stickX = g_lastStickX;
    if (stickY) *stickY = g_lastStickY;
}

#if defined(__vita__)
bool Read(Raw& raw) {
    static bool configured = false;
    if (!configured) {
        // Analog mode: without it the sticks read as centred. WIDE: the sticks report
        // their whole 0..255 range, which the scaling above assumes (the plain analog
        // mode clips earlier); the ACGC port and SDL's Vita driver, both proven on
        // consoles, set the same (docs/acgc-vita-port-findings.md, item 4).
        sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
        configured = true;
    }
    // Peek, not Read: Read blocks until the next controller sample (a vblank). W5 CTL: the last few buffered samples, so that a
    // button pressed and released between two reads (the pad is sampled at 60 Hz, the game reads at 30) is still reported once
    // (np_controls.h LatchTaps; MSC_TAP_LATCH=0 reads only the newest sample, as before).
    constexpr int kBuffers = 4;
    SceCtrlData pads[kBuffers];
    std::memset(pads, 0, sizeof(pads));
    const int want = NpControls::TapLatchEnabled() ? kBuffers : 1;
    const int got = sceCtrlPeekBufferPositive(0, pads, want);
    if (got <= 0) return false;
    int newest = 0;
    for (int i = 1; i < got && i < kBuffers; ++i)
        if (pads[i].timeStamp > pads[newest].timeStamp) newest = i;
    const SceCtrlData& pad = pads[newest];
    uint32_t buttons = pad.buttons;
    if (got > 1) {
        static uint64_t lastStamp = 0;
        uint32_t b[kBuffers];
        uint64_t t[kBuffers];
        const int n = got < kBuffers ? got : kBuffers;
        for (int i = 0; i < n; ++i) { b[i] = pads[i].buttons; t[i] = pads[i].timeStamp; }
        buttons = NpControls::LatchTaps(b, t, n, &lastStamp);
    }
    raw.buttons = buttons;
    raw.lx = pad.lx;
    raw.ly = pad.ly;
    raw.rx = pad.rx;
    raw.ry = pad.ry;
    return true;
}
#else
bool Read(Raw&) { return false; }
#endif

} // namespace VitaInput

// ---------------------------------------------------------------------------------------------
// np_vita.h
// ---------------------------------------------------------------------------------------------
extern "C" {

// True while the system's ad hoc connection dialog (SceNetCheckDialog, multiplayer) is on screen: the dialog reads the pad itself,
// so the game must not also see it (a Cross press that confirms the dialog would otherwise press A in the menu behind it).
extern "C" bool np_vita_radio_dialog_active(void) __attribute__((weak));

int NpVitaReadInput(NpVitaInput* out) {
    if (!out) return 0;
    std::memset(out, 0, sizeof(*out));
    out->lx = out->ly = out->rx = out->ry = 128;
    if (np_vita_radio_dialog_active && np_vita_radio_dialog_active()) return 1;   // neutral pad, no touch, while the dialog shows
    VitaInput::Raw raw;
    if (!VitaInput::Read(raw)) return 0;
    out->buttons = raw.buttons;
    out->lx = raw.lx; out->ly = raw.ly; out->rx = raw.rx; out->ry = raw.ry;
#if defined(__vita__)
    // The front panel is read when anything uses it: the menu pointer (NP_TOUCH_POINTER) or a minigame (MSC_TOUCH_MINIGAMES and the
    // per-minigame switches). W5 CTL: before, MSC_TOUCH_MINIGAMES=0 (the MEGA STRIKE TOUCH row) also switched the menu pointer off.
    static const bool touchEnabled = [] {
        auto on = [](const char* name) {
            const char* value = std::getenv(name);
            return !value || !*value || value[0] != '0';
        };
        const bool minigames = on("MSC_TOUCH_MINIGAMES") && (on("MSC_TOUCH_SAVE") || on("MSC_TOUCH_METER") || on("MSC_TOUCH_SHAKE"));
        return on("NP_TOUCH_POINTER") || minigames;
    }();
    if (touchEnabled && NpVitaTouchAllowed()) {
        static bool started = false;
        if (!started) {
            if (sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START) >= 0) started = true;
        }
        SceTouchPanelInfo panel{};
        SceTouchData data{};
        if (started && sceTouchGetPanelInfo(SCE_TOUCH_PORT_FRONT, &panel) >= 0 && panel.maxDispX > panel.minDispX &&
            panel.maxDispY > panel.minDispY && sceTouchPeek(SCE_TOUCH_PORT_FRONT, &data, 1) > 0 && data.reportNum <= 8) {
            out->touchCount = static_cast<int>(data.reportNum);
            for (int i = 0; i < out->touchCount; ++i) {
                out->touch[i].id = data.report[i].id;
                out->touch[i].x = (data.report[i].x - panel.minDispX) * 960.0f / (panel.maxDispX - panel.minDispX);
                out->touch[i].y = (data.report[i].y - panel.minDispY) * 544.0f / (panel.maxDispY - panel.minDispY);
            }
        }
    }
    // W5 CTL: the rear panel, for the layout's rear taps (np_controls.h RearPad). Read only when it is switched on
    // (MSC_REAR_TOUCH, default on with the MODERN layout) and a person holds the console (not in a scripted run).
    static const bool rearEnabled = NpControls::RearEnabled(NpControls::ConfiguredLayout());
    if (rearEnabled && NpVitaTouchAllowed()) {
        static bool rearStarted = false;
        static SceTouchPanelInfo rearPanel{};
        static bool rearPanelOk = false;
        if (!rearStarted) {
            if (sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START) >= 0) rearStarted = true;
            rearPanelOk = rearStarted && sceTouchGetPanelInfo(SCE_TOUCH_PORT_BACK, &rearPanel) >= 0 && rearPanel.maxAaX > rearPanel.minAaX &&
                          rearPanel.maxAaY > rearPanel.minAaY;
        }
        // The back panel reports x as seen from the front (the right half under the right hand). MSC_REAR_MIRROR=1 flips it, in case
        // a console shows the halves the other way round (console test list, docs/wave5-CTL.md).
        static const bool mirror = [] {
            const char* value = std::getenv("MSC_REAR_MIRROR");
            return value && value[0] == '1';
        }();
        SceTouchData data{};
        if (rearPanelOk && sceTouchPeek(SCE_TOUCH_PORT_BACK, &data, 1) > 0 && data.reportNum <= 8) {
            out->rearCount = static_cast<int>(data.reportNum);
            for (int i = 0; i < out->rearCount; ++i) {
                out->rear[i].id = data.report[i].id;
                out->rear[i].x = (data.report[i].x - rearPanel.minAaX) * 960.0f / (rearPanel.maxAaX - rearPanel.minAaX);
                if (mirror) out->rear[i].x = 960.0f - out->rear[i].x;
                out->rear[i].y = (data.report[i].y - rearPanel.minAaY) * 544.0f / (rearPanel.maxAaY - rearPanel.minAaY);
            }
        }
    }
#endif
    return 1;
}

void NpVitaMapGcPad(const NpVitaInput* in, NpPadState* pad) {
    if (!in || !pad) return;
    VitaInput::Raw raw;
    raw.buttons = in->buttons;
    raw.lx = in->lx; raw.ly = in->ly; raw.rx = in->rx; raw.ry = in->ry;
    std::memset(pad, 0, sizeof(*pad));
    VitaInput::Map(raw, *pad);
}

// Touch and the physical Mega Save controls are for a person: a nonempty MSC_INPUT_SCRIPT (a scripted
// run, also after the script has ended) turns the minigame adapter off for the whole run.
int NpVitaTouchAllowed(void) {
    const char* script = std::getenv("MSC_INPUT_SCRIPT");
    return !script || !*script;
}

} // extern "C"
