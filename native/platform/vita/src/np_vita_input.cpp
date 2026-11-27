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
#include "../../../include/np/touch_diag.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__vita__)
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#endif

#include "../../compat/np_lat16.h"   // wave 11 (press16): MSC_LAT_SAMPLE / MSC_LAT_PADAGE, the controller sample clock
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
// Codex input follow-up (controller-history.patch, merged 23 Oct): the game's and the passive debug observer's tap histories are
// separate, so the renderer's debug-chord poll can no longer swallow a gameplay tap.
uint64_t g_lastTapStamp = 0;
#if defined(__vita__)
uint64_t g_debugTapStamp = 0;
#endif

// Wave 24 (feel24, X10; default OFF): MSC_STICK_DEAD=<raw units, 4..32> (default 16, the line above) and MSC_STICK_CURVE=<power 1..2, default 1 = linear> for the LEFT
// stick (movement, pass and shot aim). The same radial zone and the same full travel; the power curve makes small deflections finer. With neither set, MapStick is the code
// above, bit for bit (the curve is skipped at 1, the zone is the constant). The mapped value is what the game captures, records and sends, so a replay and a peer see
// the result of the player's own mapping: multiplayer-safe (nothing here is a second input stream).
struct StickTune { float dead; float power; };
const StickTune& StickTuneLeft() {
    static const StickTune t = [] {
        StickTune v{kDeadZone, 1.0f};
        const char* d = std::getenv("MSC_STICK_DEAD");
        const char* c = std::getenv("MSC_STICK_CURVE");
        if (d && *d) { const float x = static_cast<float>(std::atof(d)); if (x >= 4.0f && x <= 32.0f) v.dead = x; }
        if (c && *c) { const float x = static_cast<float>(std::atof(c)); if (x >= 1.0f && x <= 2.0f) v.power = x; }
        return v;
    }();
    return t;
}

void MapStick(uint8_t rawX, uint8_t rawY, int8_t& outX, int8_t& outY, const StickTune* tune = nullptr) {
    const float dead = tune ? tune->dead : kDeadZone;
    const float x = static_cast<float>(rawX) - 128.0f;
    const float y = 128.0f - static_cast<float>(rawY);
    const float magnitude = std::sqrt(x * x + y * y);
    if (magnitude <= dead) {
        outX = 0;
        outY = 0;
        return;
    }
    float scaled = (magnitude - dead) / (kFullTravel - dead);
    if (scaled > 1.0f) scaled = 1.0f;
    if (tune && tune->power > 1.0f) scaled = std::pow(scaled, tune->power);
    const float k = scaled * kFullScale / magnitude;
    outX = static_cast<int8_t>(std::lround(x * k));
    outY = static_cast<int8_t>(std::lround(y * k));
}

} // namespace

void InvalidateTapHistory() { g_lastTapStamp = 0; TapCarryClear(); }

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
    MapStick(raw.lx, raw.ly, status.stickX, status.stickY, &StickTuneLeft());
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
static bool ReadWithHistory(Raw& raw, uint64_t& lastStamp) {
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
    const bool diag=NpDiagActive()!=0;
    const uint64_t begin=diag?NpDiagNow():0;
    const int got = sceCtrlPeekBufferPositive(0, pads, want);
    const uint64_t end=diag?NpDiagNow():0;
    if(diag&&got<=0)NpDiagEvent(NPD_CTRL,0,NpDiagPollId(),begin,end,0,NpDiagInts(got,want),0);
    if (got <= 0) {
        lastStamp = 0;   // resume newest-only after an interval without input
        return false;
    }
    int newest = 0;
    for (int i = 1; i < got && i < kBuffers; ++i)
        if (pads[i].timeStamp > pads[newest].timeStamp) newest = i;
    const SceCtrlData& pad = pads[newest];
    if (&lastStamp == &g_lastTapStamp && (npl16::Wanted() || npl16::AgeLog())) npl16::GameRead(pad.timeStamp, npl16::RealUs());   // wave 11 (press16)
    if(diag)NpDiagEvent(NPD_CTRL,0,NpDiagPollId(),begin,end,pad.timeStamp,NpDiagInts(got,want),
        NpDiagInts(pad.buttons,uint32_t(pad.lx)|(uint32_t(pad.ly)<<8)|(uint32_t(pad.rx)<<16)|(uint32_t(pad.ry)<<24)));
    // every successful read advances the history, a single sample too (otherwise a later tap could be lost or replayed)
    uint32_t b[kBuffers];
    uint64_t t[kBuffers];
    const int n = got < kBuffers ? got : kBuffers;
    for (int i = 0; i < n; ++i) { b[i] = pads[i].buttons; t[i] = pads[i].timeStamp; }
    raw.buttons = NpControls::LatchTaps(b, t, n, &lastStamp, NpControls::TapGapUs());   // wave 6: MSC_TAP_GAP_MS
    raw.newestButtons = pad.buttons;
    raw.lx = pad.lx;
    raw.ly = pad.ly;
    raw.rx = pad.rx;
    raw.ry = pad.ry;
    return true;
}

static NpControls::TapCarry g_tapCarry;
static bool TapCarryOn() {
    static const bool on = [] { const char* e = getenv("MSC_TAP_CARRY"); return e && *e && atoi(e) != 0; }();
    return on;
}
void TapCarryHold(bool on) { g_tapCarry.SetHold(on && TapCarryOn()); }
void TapCarryClear() { g_tapCarry.Clear(); }

bool Read(Raw& raw) {
    if (!ReadWithHistory(raw, g_lastTapStamp)) return false;
#if defined(__vita__)
    if (TapCarryOn()) raw.buttons = g_tapCarry.Apply(raw.buttons, raw.newestButtons);   // wave 9 MSC_TAP_CARRY: a frame without a step takes no tap from the next step
#endif
    g_debugTapStamp = g_lastTapStamp;   // game reads drain the observer's history (old settings presses never replay as chords)
    return true;
}

bool ReadDebug(Raw& raw) { return ReadWithHistory(raw, g_debugTapStamp); }
#else
bool Read(Raw&) { return false; }
bool ReadDebug(Raw&) { return false; }
#endif

} // namespace VitaInput

#if defined(__vita__)
// wave 11 (press16, np_lat16.h): the stamp of the newest controller sample, read without touching the tap history (a Peek); 0 when there is none
extern "C" uint64_t NpVitaCtrlStampUs(void)
{
    SceCtrlData d;
    std::memset(&d, 0, sizeof d);
    return sceCtrlPeekBufferPositive(0, &d, 1) > 0 ? (uint64_t)d.timeStamp : 0u;
}
#endif

// ---------------------------------------------------------------------------------------------
// np_vita.h
// ---------------------------------------------------------------------------------------------
extern "C" {

// True while the system's ad hoc connection dialog (SceNetCheckDialog, multiplayer) is on screen: the dialog reads the pad itself,
// so the game must not also see it (a Cross press that confirms the dialog would otherwise press A in the menu behind it).
extern "C" bool np_vita_radio_dialog_active(void) __attribute__((weak));

extern "C" void NpVitaPadHoldTaps(int on) { VitaInput::TapCarryHold(on != 0); }   // wave 9 (MSC_TAP_CARRY): FixedUpdateTask, before the end-of-frame read of a frame without a step
extern "C" void NpVitaPadClearTaps(void) { VitaInput::TapCarryClear(); }

static uint32_t g_inputActivity = 0;   // see the end of NpVitaReadInput
extern "C" uint32_t NpVitaInputActivity(void) { return g_inputActivity; }

int NpVitaReadInput(NpVitaInput* out) {
    if (!out) return 0;
    std::memset(out, 0, sizeof(*out));
    out->lx = out->ly = out->rx = out->ry = 128;
    // A disabled or failed rear read is not evidence that all contacts lifted.
    out->rearCount = -1;
    if (np_vita_radio_dialog_active && np_vita_radio_dialog_active()) {
        VitaInput::InvalidateTapHistory();   // presses released during the dialog are not replayed into the game
        return 1;   // neutral pad, no touch, while the dialog shows
    }
    VitaInput::Raw raw;
    const bool diag=NpDiagActive()!=0;
    if(diag)NpDiagPollNext();
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
        const uint64_t begin=diag?NpDiagNow():0;
        // wave 24 (feel24, X4): the panel's size does not change; asked once when it answers sensibly (as the rear panel is), not at every poll
        static SceTouchPanelInfo frontPanel{};
        static bool frontPanelOk = false;
        int panelRc = -1;
        if (started) {
            if (frontPanelOk) { panel = frontPanel; panelRc = 0; }
            else {
                panelRc = sceTouchGetPanelInfo(SCE_TOUCH_PORT_FRONT, &panel);
                if (panelRc >= 0 && panel.maxDispX > panel.minDispX && panel.maxDispY > panel.minDispY) { frontPanel = panel; frontPanelOk = true; }
            }
        }
        const uint64_t panelEnd=diag?NpDiagNow():0;
        const bool validPanel=started&&panelRc>=0&&panel.maxDispX>panel.minDispX&&panel.maxDispY>panel.minDispY;
        // Short taps (23 Oct, Codex handoff "front touches wholly between polls can be missed"): the panel keeps a few samples. When
        // the newest one has no finger but a sample since the previous read had one, that contact is reported for this read, once
        // (a tap shorter than the game's read interval still lands; the next read reports the lifted panel). MSC_TOUCH_LATCH=0: off.
        static const bool touchLatch = [] { const char* e = std::getenv("MSC_TOUCH_LATCH"); return !(e && e[0] == '0'); }();
        static uint64_t lastTouchStamp = 0;
        constexpr int kTouchBufs = 4;
        SceTouchData bufs[kTouchBufs];
        int peekRc = 0;
        if (validPanel && touchLatch) {
            std::memset(bufs, 0, sizeof(bufs));
            peekRc = sceTouchPeek(SCE_TOUCH_PORT_FRONT, bufs, kTouchBufs);
            if (peekRc > 0) {
                const int n = peekRc < kTouchBufs ? peekRc : kTouchBufs;
                int newest = 0;
                for (int i = 1; i < n; ++i) if (bufs[i].timeStamp > bufs[newest].timeStamp) newest = i;
                data = bufs[newest];
                if (data.reportNum == 0 && lastTouchStamp != 0) {
                    int pick = -1;
                    for (int i = 0; i < n; ++i)
                        if (i != newest && bufs[i].timeStamp > lastTouchStamp && bufs[i].reportNum > 0 && bufs[i].reportNum <= 8 &&
                            (pick < 0 || bufs[i].timeStamp > bufs[pick].timeStamp))
                            pick = i;
                    if (pick >= 0) data = bufs[pick];
                }
                lastTouchStamp = bufs[newest].timeStamp;
                peekRc = 1;
            } else {
                lastTouchStamp = 0;
            }
        } else if (validPanel) {
            peekRc = sceTouchPeek(SCE_TOUCH_PORT_FRONT, &data, 1);
        }
        const uint64_t touchEnd=diag?NpDiagNow():0;
        if(diag)NpDiagEvent(NPD_POLL,data.reportNum<<8,NpDiagPollId(),begin,panelEnd,touchEnd,data.timeStamp,NpDiagInts(panelRc,peekRc));
        if (validPanel && peekRc>0 && data.reportNum <= 8) {
            out->touchCount = static_cast<int>(data.reportNum);
            for (int i = 0; i < out->touchCount; ++i) {
                out->touch[i].id = data.report[i].id;
                out->touch[i].x = (data.report[i].x - panel.minDispX) * 960.0f / (panel.maxDispX - panel.minDispX);
                out->touch[i].y = (data.report[i].y - panel.minDispY) * 544.0f / (panel.maxDispY - panel.minDispY);
                if(diag)NpDiagEvent(NPD_CONTACT,data.report[i].force,NpDiagPollId(),data.report[i].id,NpDiagInts(data.report[i].x,data.report[i].y),NpDiagFloats(out->touch[i].x,out->touch[i].y),NpDiagFrame(),NpDiagStepId());
            }
        }
    }
    // W5 CTL: the rear panel, for the layout's rear taps (np_controls.h RearPad). Read only when it is switched on
    // (MSC_REAR_TOUCH, default on with the MODERN layout) and a person holds the console (not in a scripted run).
    // 1.3: read at every poll (a cell, np/live_settings.h): the settings screen's REAR TOUCH applies to the running game. The panel starts lazily below.
    const bool rearEnabled = NpControls::RearEnabled(NpControls::ConfiguredLayout()) || NpControls::RearSaveEnabled();
    if (rearEnabled && NpVitaTouchAllowed()) {
        static bool rearStarted = false;
        static SceTouchPanelInfo rearPanel{};
        static bool rearPanelOk = false;
        if (!rearStarted) {
            if (sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START) >= 0) rearStarted = true;
        }
        if (rearStarted && !rearPanelOk)
            rearPanelOk = sceTouchGetPanelInfo(SCE_TOUCH_PORT_BACK, &rearPanel) >= 0 && rearPanel.maxAaX > rearPanel.minAaX &&
                          rearPanel.maxAaY > rearPanel.minAaY;
        // The back panel reports x as seen from the front (the right half under the right hand). MSC_REAR_MIRROR=1 flips it, in case
        // a console shows the halves the other way round (console test list, docs/wave5-CTL.md).
        static const bool mirror = [] {
            const char* value = std::getenv("MSC_REAR_MIRROR");
            return value && value[0] == '1';
        }();
        SceTouchData data{};
        const uint64_t begin=diag?NpDiagNow():0;
        // wave 24 (feel24, X4; MSC_REAR_LATCH=0: off): the panel's last few samples, so a finger that came and went wholly between two reads is reported once
        // (np_controls.h PickLostContact), as the front panel's tap latch does. Only a NEW finger in a sample newer than the previous read's newest, with the
        // newest sample empty: what the old single-sample read could not see at all.
        static uint64_t rearLastStamp = 0;
        static int rearPrevIds[8];
        static int rearPrevCount = 0;
        static unsigned long rearRecovered = 0;
        int rearRc = 0;
        if (rearPanelOk && NpControls::RearLatchEnabled()) {
            constexpr int kRearBufs = 4;
            SceTouchData bufs[kRearBufs];
            std::memset(bufs, 0, sizeof(bufs));
            const int got = sceTouchPeek(SCE_TOUCH_PORT_BACK, bufs, kRearBufs);
            rearRc = got;
            if (got > 0) {
                const int n = got < kRearBufs ? got : kRearBufs;
                NpControls::TouchSample ts[kRearBufs];
                int newest = 0;
                for (int i = 0; i < n; ++i) {
                    ts[i].stamp = bufs[i].timeStamp;
                    ts[i].count = bufs[i].reportNum <= 8 ? static_cast<int>(bufs[i].reportNum) : 9;
                    for (int k = 0; k < 8; ++k) ts[i].ids[k] = (k < ts[i].count) ? static_cast<int>(bufs[i].report[k].id) : -1;
                    if (bufs[i].timeStamp > bufs[newest].timeStamp) newest = i;
                }
                data = bufs[newest];
                const int pick = NpControls::PickLostContact(ts, n, rearLastStamp, rearPrevIds, rearPrevCount, NpControls::TapGapUs());
                if (pick >= 0) {
                    data = bufs[pick];
                    if (++rearRecovered == 1 || rearRecovered % 50 == 0)
                        std::fprintf(stderr, "[touch] rear: %lu short touches recovered (a finger that came and went between two reads)\n", rearRecovered);
                }
                rearLastStamp = bufs[newest].timeStamp;
                rearRc = 1;
                rearPrevCount = data.reportNum <= 8 ? static_cast<int>(data.reportNum) : 0;
                for (int k = 0; k < rearPrevCount; ++k) rearPrevIds[k] = static_cast<int>(data.report[k].id);
            } else {
                rearLastStamp = 0;
                rearPrevCount = 0;
            }
        } else {
            rearRc = rearPanelOk ? sceTouchPeek(SCE_TOUCH_PORT_BACK, &data, 1) : 0;
        }
        const uint64_t end=diag?NpDiagNow():0;
        if(diag)NpDiagEvent(NPD_POLL,1,NpDiagPollId(),begin,begin,end,data.timeStamp,NpDiagInts(rearRc,data.reportNum));
        if (rearPanelOk && rearRc > 0 && data.reportNum <= 8) {
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
    // r16 (fable-da21 F3): the keep-awake idle limit (np_keep_awake.h) watches this counter: it moves on a poll with a button, a stick well off
    // centre or a finger on the panel (about ten instructions; a pad at rest costs nothing more).
    if (out->buttons || out->touchCount > 0 || out->lx < 88 || out->lx > 168 || out->ly < 88 || out->ly > 168 || out->rx < 88 || out->rx > 168 || out->ry < 88 || out->ry > 168)
        ++g_inputActivity;
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
