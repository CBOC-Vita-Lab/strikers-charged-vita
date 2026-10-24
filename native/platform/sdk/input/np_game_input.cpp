// np_game_input.cpp: the game-aware half of the Vita controls (see np_game_input.h).
//
//  1. NpPadAdjust: the SDK calls it for every sample of the emulated Wii Remote (platform/sdk/src/input_native.cpp).
//     It is the port of ApplyCommon in the recompiled runtime's hle/input/pad.cpp: menu navigation, the pointer that stays
//     where it is left, the touch minigames. Face buttons sit where a GameCube controller's do (cross = A, square = B,
//     circle = X ...), see docs/sdk.md.
//  2. NpInputAfterCapture: the engine still has Super Mario Strikers' GameCube controls (DetInput kind 3: the pass, shoot,
//     switch player, item and lob buttons of the GameCube table). The Vita pad plays a match through them. The recompiled
//     build converted the tagged remote at apply time (msc_controller_input.cpp); here it happens once, when the local
//     input is captured, so the record that is simulated (and sent over the network) is already the final one.
//  3. The GameCube forms of the Wii-only actions (msc_game.cpp): big hit (was: shake the remote) = Y without the ball,
//     cycle item (was: shake the Nunchuk) = Z, deke (was: D-pad) = Y or the C stick with the ball, slide tackle (was: D-pad)
//     = B while the other team has the ball, the character special = R.
#include <revolution/pad.h>   // first: revolution/types.h and the game's types.h differ in u32 (as in DetInput.cpp)

#include "Game/AI/AIPad.h"
#include "Game/AI/Fielder.h"
#include "Game/Ball.h"
#include "Game/Net.h"
#include "Game/DetInput.h"
#include "Game/NetworkPeer.h"
#include "Game/Player.h"
#include "Game/Render/ShootToScoreMeter.h"
#include "Game/Team.h"
#include "NL/globalpad.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "platform/sdk/include/np/np_pad.h"
#include "platform/sdk/input/np_controls.h"
#include "platform/sdk/input/np_game_input.h"

// Weak: a build without the SDK layer has no controller (the capture then leaves the record alone).
extern "C" int NpPad_ReadGameCube(int chan, PADStatus* out) __attribute__((weak));
extern "C" int NpPad_ReadRaw(int chan, NpRawPad* out) __attribute__((weak));
extern "C" int NpHarnessActive(void) __attribute__((weak));   // platform/harness: a scripted run
extern "C" void NpHarnessAtExit(void (*fn)(void)) __attribute__((weak));

static_assert(NpControls::kCross == NP_BTN_CROSS && NpControls::kSquare == NP_BTN_SQUARE && NpControls::kCircle == NP_BTN_CIRCLE &&
                  NpControls::kTriangle == NP_BTN_TRIANGLE && NpControls::kL == NP_BTN_L && NpControls::kR == NP_BTN_R &&
                  NpControls::kSelect == NP_BTN_SELECT && NpControls::kStart == NP_BTN_START,
              "np_controls.h button bits");
static_assert(NpControls::kGcA == PAD_BUTTON_A && NpControls::kGcB == PAD_BUTTON_B && NpControls::kGcX == PAD_BUTTON_X && NpControls::kGcY == PAD_BUTTON_Y &&
                  NpControls::kGcL == PAD_TRIGGER_L && NpControls::kGcR == PAD_TRIGGER_R && NpControls::kGcZ == PAD_TRIGGER_Z &&
                  NpControls::kGcStart == PAD_BUTTON_START,
              "np_controls.h GameCube bits");
static_assert(NpControls::kWA == NPW_A && NpControls::kWB == NPW_B && NpControls::kWC == NPW_C && NpControls::kW2 == NPW_2, "np_controls.h WPAD bits");

uint64_t NpGameNowNsFallback() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

namespace {

float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

float pointer_speed() {
    static float v = -1.0f;
    if (v < 0.0f) {
        const char* e = getenv("NP_POINTER_SPEED");
        v = (e && *e) ? (float)atof(e) : 1.0f;
        if (v <= 0.0f) v = 1.0f;
    }
    return v;
}
bool touch_pointer_enabled() {
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("NP_TOUCH_POINTER");
        on = !(e && *e == '0');
    }
    return on == 1;
}

uint32_t g_epoch;   // samples so far (the touch gesture state counts them)

// W5 CTL: the layout of each channel's last sample, its rear touchpad and the MODERN mapper (np_controls.h)
struct CtlChannel {
    NpControls::Layout layout = NpControls::kClassic;
    bool rear = false;
    bool rearBlocked = false;
    NpControls::RearPad rearPad;
    NpControls::Mapper mapper;
    int rearRight = 0, rearLeft = 0;   // taps waiting for a capture
    uint32_t rearTapMs = 0;            // when the newest of them happened (taps older than 250 ms when a capture comes are dropped: menus)
    uint32_t vita = 0;                 // the sample's Vita buttons and time: the capture uses them (no second clock read or sample: the
    uint32_t sampleMs = 0;             // harness's virtual clock advances per read, and CLASSIC runs must stay step for step what they were)
};
CtlChannel g_ctl[4];
unsigned g_meterPresses;   // [ctl] counters: presses of the meter button (remote view, buttons or a finger) while the local Mega Strike meter runs
bool g_meterButton, g_meterMode;
bool bare_remote() {
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("NP_PAD_PROFILE");
        v = (e && !strcmp(e, "remote")) ? 1 : 0;
    }
    return v == 1;
}
int ctl_trace();
// the [ctl] counters run in traced and in scripted runs (read only: they never change the match)
bool ctl_counting() {
    static int v = -1;
    if (v < 0) v = (ctl_trace() || (NpHarnessActive && NpHarnessActive())) ? 1 : 0;
    return v == 1;
}
int ctl_trace() {
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("MSC_CTL_TRACE");
        v = (e && *e) ? atoi(e) : 0;
    }
    return v;
}

// Action indices of the engine's controller map (shared by Super Mario Strikers and Charged): DetInput::IsPressed(action, true)
enum { kActToggleItem = 22, kActHit = 24, kActSlide = 25, kActDeke = 29 };
constexpr uint16_t kPadTriggerR = 0x0020;

}  // namespace

// ---- title screen: tap anywhere to start (the maintainer, 23 Oct: "like Galaxy's") ----------------------------------------------------
// The title (SHTitleScreen.cpp) asks for new front-panel contacts: every live (not scripted) channel-0 sample whose touch count goes
// from 0 to more counts one contact start. The title takes a baseline when it becomes ready, so a finger held from the intro or the
// health screen never starts the game; only a new contact does, wherever it lands. After the title used a contact it asks that the
// finger is not turned into a menu press (the generic pointer block below) until every finger is lifted, so it cannot press a
// button of the main menu. NP_TOUCH_POINTER=0 (the kill switch of every front-panel press) turns this off too.
namespace {
uint32_t g_touchStarts = 0;   // __atomic builtins: <atomic> clashes with the game's placement new
int g_touchHoldOff = 0;
int g_prevTouchCount = 0;
}  // namespace
extern "C" uint32_t NpFrontTouchStarts(void) { return __atomic_load_n(&g_touchStarts, __ATOMIC_ACQUIRE); }
extern "C" int NpFrontTouchPressAllowed(void) { return touch_pointer_enabled() ? 1 : 0; }
extern "C" void NpFrontTouchHoldOffUntilRelease(void) { __atomic_store_n(&g_touchHoldOff, 1, __ATOMIC_RELEASE); }

extern "C" void NpPadAdjust(NpPadSample* s) {
    using namespace NpMenuNav;
    const int chan = s->chan;
    const NpRawPad& raw = *s->raw;
    ++g_epoch;

    // W5 CTL: the layout. A GameCube-pad script keeps its meaning (CLASSIC, by position); the player's controls and Vita-button
    // scripts get the configured one. The Wii Remote view (menus, the minigames) first, then the rear touchpad's taps.
    if (chan >= 0 && chan < 4) {
        CtlChannel& ctl = g_ctl[chan];
        const NpControls::Layout layout = raw.source == NP_RAW_SCRIPT_GC ? NpControls::kClassic : NpControls::ConfiguredLayout();
        ctl.layout = layout;
        ctl.vita = raw.buttons;
        s->hold = NpControls::WiiHold(layout, raw.buttons, s->hold, bare_remote());
        ctl.rear = raw.source != NP_RAW_SCRIPT_GC && NpControls::RearEnabled(layout);
    }


    uint32_t navDpad = 0;
    if (raw.buttons & NP_BTN_UP) navDpad |= kUp;
    if (raw.buttons & NP_BTN_DOWN) navDpad |= kDown;
    if (raw.buttons & NP_BTN_LEFT) navDpad |= kLeft;
    if (raw.buttons & NP_BTN_RIGHT) navDpad |= kRight;
    const int pageInput = (raw.buttons & NP_BTN_R) ? 1 : (raw.buttons & NP_BTN_L) ? -1 : 0;

    // Menus: the D-pad and either stick navigate (the stronger stick wins); flicks step between buttons, and on ring
    // menus the stick angle picks directly.
    NpPoint navStick{s->stick[0], s->stick[1]};
    if (hypotf(s->free_stick[0], s->free_stick[1]) > hypotf(navStick.x, navStick.y)) navStick = {s->free_stick[0], s->free_stick[1]};
    uint32_t nav = navDpad;
    if (navStick.y > 0.6f) nav |= kUp;
    if (navStick.y < -0.6f) nav |= kDown;
    if (navStick.x < -0.6f) nav |= kLeft;
    if (navStick.x > 0.6f) nav |= kRight;
    const Result nr = Update(chan, nav, navStick, (s->hold & NPW_B) != 0, pageInput, s->cursor, s->dt);
    if (nr.pressA) s->hold |= NPW_A;
    if (nr.pressPause) s->hold |= NPW_1;
    if (nr.page > 0) s->hold |= NPW_PLUS;
    if (nr.page < 0) s->hold |= NPW_MINUS;

    float moveX = s->stick[0], moveY = s->stick[1];
    bool localSave = false;
    if (nr.menu) {
        // Only the real D-pad reaches the game as D-pad here, not the sticks used for navigating.
        s->hold &= ~(NPW_UP | NPW_DOWN | NPW_LEFT | NPW_RIGHT);
        if (navDpad & kUp) s->hold |= NPW_UP;
        if (navDpad & kDown) s->hold |= NPW_DOWN;
        if (navDpad & kLeft) s->hold |= NPW_LEFT;
        if (navDpad & kRight) s->hold |= NPW_RIGHT;
        moveX = moveY = 0.0f;   // the pointer belongs to navigation
    } else {
        localSave = NpTouch::SelectSavePointerAxes(chan, s->free_stick[0], s->free_stick[1], &moveX, &moveY);
    }
    // Pointer: the stick moves a cursor that stays where it is left (menus need absolute aim; Mega Save aims with it).
    float multiplier = pointer_speed();
    const bool liveSave = localSave && raw.source == NP_RAW_LIVE && (s->hold & 0x80u) != 0 && NpControls::PhysicalMegaControlsAllowed();
    if (liveSave && NpControls::MegaPointerGain() > 1.0f) {
        if (!isfinite(multiplier) || multiplier <= 0) multiplier = 1.0f;
        multiplier = fmaxf(multiplier, fminf(4.0f, multiplier * NpControls::MegaPointerGain()));
    }
    const float speed = 1.6f * multiplier;
    auto dz = [](float v) { return fabsf(v) < 0.2f ? 0.0f : v; };
    s->cursor[0] = clampf(s->cursor[0] + dz(moveX) * speed * s->dt, -1.0f, 1.0f);
    s->cursor[1] = clampf(s->cursor[1] - dz(moveY) * speed * s->dt, -1.0f, 1.0f);
    s->pointer_hidden = nr.hidePointer ? 1 : 0;

    // The touch minigames (they may move the cursor: the Mega Save pointer) ...
    const uint32_t nowMs = (uint32_t)(NpGameNowNs() / 1000000ull);
    // W5 CTL: the rear touchpad's taps (np_controls.h RearPad), counted per sample, turned into presses by the capture
    if (chan >= 0 && chan < 4) {
        CtlChannel& ctl = g_ctl[chan];
        ctl.sampleMs = nowMs;
        const bool rearAim = liveSave && NpControls::RearSaveEnabled();
        const bool invalidRear = raw.rear_count < 0 || raw.rear_count > NP_MAX_TOUCH;
        if (rearAim || invalidRear) ctl.rearBlocked = true;
        if (ctl.rearBlocked) {
            ctl.rearPad.Reset();
            ctl.rearLeft = ctl.rearRight = 0;
            ctl.mapper.ClearRearPulses();
            // A release on exit is consumed here, never interpreted as an ordinary tap.
            if (!rearAim && raw.rear_count == 0) ctl.rearBlocked = false;
        } else if (ctl.rear) {
            NpControls::RearPoint points[NP_MAX_TOUCH];
            const int n = raw.rear_count < 0 ? 0 : raw.rear_count > NP_MAX_TOUCH ? NP_MAX_TOUCH : raw.rear_count;
            for (int i = 0; i < n; ++i) points[i] = {raw.rear[i].id, raw.rear[i].x, raw.rear[i].y};
            const unsigned taps = ctl.rearPad.Update(points, n, nowMs);
            if ((taps & 1u) && ctl.rearLeft < 4) ++ctl.rearLeft;
            if ((taps & 2u) && ctl.rearRight < 4) ++ctl.rearRight;
            if (taps) {
                ctl.rearTapMs = nowMs;
                if (ctl_trace()) fprintf(stderr, "[ctl] rear tap%s%s t=%u\n", (taps & 2u) ? " right" : "", (taps & 1u) ? " left" : "", (unsigned)nowMs);
            }
        } else {
            ctl.rearPad.Reset();
            ctl.rearLeft = ctl.rearRight = 0;
        }
    }
    const bool minigame = NpTouch::Apply(chan, raw, &s->hold, s->cursor, g_epoch, nowMs, nr.menu);
    if (chan == 0) {
        // a press that starts while the meter is already up (the shoot button still held from the charge when it opens does not count)
        const bool meter = NpTouch::CurrentMode(0) == NpTouch::Mode::Meter;
        const bool b = (s->hold & (NPW_A | NPW_B)) != 0;
        if (meter && g_meterMode && b && !g_meterButton) {
            ++g_meterPresses;
            if (ctl_trace()) fprintf(stderr, "[ctl] METER press (%s)\n", raw.touch_count > 0 ? "touch" : "button");
        }
        g_meterButton = b;
        g_meterMode = meter;
    }
    if (chan == 0 && raw.source == NP_RAW_LIVE) {
        const int n = raw.touch_count < 0 ? 0 : raw.touch_count;
        if (n > 0 && g_prevTouchCount == 0) __atomic_fetch_add(&g_touchStarts, 1u, __ATOMIC_ACQ_REL);
        if (n == 0) __atomic_store_n(&g_touchHoldOff, 0, __ATOMIC_RELEASE);
        g_prevTouchCount = n;
    }
    const bool holdOff = chan == 0 && raw.touch_count > 0 && __atomic_load_n(&g_touchHoldOff, __ATOMIC_ACQUIRE) != 0;
    // ... and in a menu a finger is a pointer: it points where it touches and presses A while it is down.
    if (!minigame && !holdOff && nr.menu && touch_pointer_enabled() && raw.touch_count > 0) {
        NpTouch::PanelToCursor(raw.touch[0].x, raw.touch[0].y, s->cursor);
        s->hold |= NPW_A;
    }
}

// ---- capture ---------------------------------------------------------------------------------------------------------
namespace {
int g_shakeHoldFrames;   // the touch shakedown swipe is held as Z for two captures

// W5 CTL: does this channel control the ball carrier (the MODERN context buttons)?
bool local_has_ball(NetworkPeerChannel* channel) {
    cPlayer* owner = g_pBall ? g_pBall->m_pOwner : 0;
    return owner && owner->m_pController && owner->m_pController->m_pGlobalPad == &channel->mInput;
}

// ---- W5 CTL: what the controls did (MSC_CTL_TRACE=1 "[ctl]" lines; the harness reads the counters as script variables) ----
struct CtlStats {
    unsigned pass, lob, shot, chip, turbo, deke, hit, slide, switches, itemUse, itemCycle, mega, save, shakeMode, shake, skill, super,
        loose, onetimer, receive, keeperPass, keeperShot, saveA;
    int act;                  // action state of the controlled player (-2 none)
    const void* player;       // the controlled player
    int playerIndex;
    int itemTotal, itemKey;   // the team's items (count, order)
    int mode;                 // touch gate mode
    bool meter;
    uint16_t lastGc, lastGcPrev;
    uint32_t lastVita;
    unsigned captures;
    unsigned captain;   // switches that went to the captain (player 0)
    bool prevZ;         // the record's Z in the previous capture (the shakedown count)
};
CtlStats g_stats = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -2, 0, -1, -1, -1, 0, false, 0, 0, 0, 0};

const char* act_name(int a) {
    static const char* const names[] = {"crowd_riot", "deke", "electrocution", "fall", "hit", "hit_react", "bomb_hit_react", "idle_turn",
                                        "late_onetimer", "loose_ball_pass", "loose_ball_shot", "mega_strike", "shoot_to_score", "onetimer",
                                        "onetouch_pass", "pass", "post_whistle", "receive_pass", "running", "running_pass(turbo)", "running_wb",
                                        "regular_shot", "slide_attack", "slide_attack_react", "sucked_away", "bomb_react", "shell_react",
                                        "banana_react", "knockdown_react", "super_power", "shot_windup", "burning", "skillshot", "boo_skillshot",
                                        "monty_burrowed", "launched", "wait"};
    if (a == -1) return "need_action";
    if (a < 0 || a >= (int)(sizeof(names) / sizeof(names[0]))) return "?";
    return names[a];
}

void ctl_summary() {
    const CtlStats& st = g_stats;
    fprintf(stderr,
            "[ctl] summary layout=%s captures=%u pass=%u lob=%u shot=%u chip=%u turbo=%u deke=%u hit=%u slide=%u switch=%u captain=%u item=%u cycle=%u "
            "mega=%u meterpress=%u save=%u saveA=%u shakedown=%u shakes=%u skillshot=%u super=%u loose=%u onetimer=%u receive=%u keeperpass=%u keeperpunt=%u\n",
            NpControls::LayoutName(NpControls::ConfiguredLayout()), st.captures, st.pass, st.lob, st.shot, st.chip, st.turbo, st.deke, st.hit,
            st.slide, st.switches, st.captain, st.itemUse, st.itemCycle, st.mega, g_meterPresses, st.save, st.saveA, st.shakeMode, st.shake, st.skill, st.super, st.loose,
            st.onetimer, st.receive, st.keeperPass, st.keeperShot);
}

void ctl_observe(NetworkPeerChannel* channel, int physical, uint32_t vita, uint16_t gc) {
    CtlStats& st = g_stats;
    if (!st.captures && NpHarnessAtExit) NpHarnessAtExit(ctl_summary);
    ++st.captures;
    const int trace = ctl_trace();
    if (trace && (vita != st.lastVita || gc != st.lastGc))
        fprintf(stderr, "[ctl] cap=%u layout=%s vita=0x%04x -> gc=0x%04x have=%d\n", st.captures, NpControls::LayoutName(g_ctl[physical].layout),
                (unsigned)vita, (unsigned)gc, local_has_ball(channel) ? 1 : 0);
    st.lastVita = vita;
    st.lastGc = gc;
    // the controlled player and its team
    cPlayer* mine = 0;
    int index = -1;
    cTeam* team = 0;
    for (int t = 0; t < 2 && !mine; ++t) {
        cTeam* tm = g_pTeams[t];
        if (!tm) continue;
        for (int i = 0; i < 5; ++i) {
            cPlayer* p = tm->m_pPlayers[i];
            if (p && p->m_pController && p->m_pController->m_pGlobalPad == &channel->mInput) {
                mine = p;
                index = i;
                team = tm;
                break;
            }
        }
    }
    const bool have = local_has_ball(channel);
    if (mine && index == 4 && have) {   // the keeper with the ball: A passes, B punts (no fielder action state)
        if ((gc & PAD_BUTTON_A) && !(st.lastGcPrev & PAD_BUTTON_A)) ++st.keeperPass;
        if ((gc & PAD_BUTTON_B) && !(st.lastGcPrev & PAD_BUTTON_B)) ++st.keeperShot;
    }
    st.lastGcPrev = gc;
    if (mine != st.player) {
        if (st.player && mine && !have) {
            ++st.switches;
            if (index == 0) ++st.captain;
            if (trace) fprintf(stderr, "[ctl] SWITCH player %d -> %d\n", st.playerIndex, index);
        }
        st.player = mine;
        st.playerIndex = index;
        st.act = -2;
    }
    if (mine) {
        const int act = index == 4 ? -3 : (int)static_cast<cFielder*>(mine)->m_eActionState;   // index 4: the keeper (Goalie, no fielder action)
        if (act != st.act) {
            const bool modifier = (gc & PAD_TRIGGER_L) != 0;
            switch (act) {
            case ACTION_PASS: ++st.pass; if (modifier) ++st.lob; break;
            case ACTION_REGULAR_SHOT: ++st.shot; if (modifier) ++st.chip; break;
            case ACTION_RUNNING_PASS: ++st.turbo; break;
            case ACTION_DEKE: ++st.deke; break;
            case ACTION_HIT: ++st.hit; break;
            case ACTION_SLIDE_ATTACK: ++st.slide; break;
            case ACTION_MEGA_STRIKE: case ACTION_SHOOT_TO_SCORE: if (st.act != ACTION_MEGA_STRIKE && st.act != ACTION_SHOOT_TO_SCORE) ++st.mega; break;
            case ACTION_SKILLSHOT: case ACTION_BOO_SKILLSHOT: ++st.skill; break;
            case ACTION_SUPER_POWER: ++st.super; break;
            case ACTION_LOOSE_BALL_PASS: case ACTION_LOOSE_BALL_SHOT: ++st.loose; break;
            case ACTION_ONETIMER: case ACTION_LATE_ONETIMER_FROM_VOLLEY: case ACTION_ONETOUCH_PASS_FROM_VOLLEY: ++st.onetimer; break;
            case ACTION_RECEIVE_PASS: ++st.receive; break;
            default: break;
            }
            if (trace && act >= -1) fprintf(stderr, "[ctl] ACT %s player=%d have=%d mod=%d\n", act_name(act), index, have ? 1 : 0, modifier ? 1 : 0);
            st.act = act;
        }
    }
    if (team) {
        const int total = team->m_ePowerupList[0].nnumOfPowerups + team->m_ePowerupList[1].nnumOfPowerups;
        const int key = (int)team->m_ePowerupList[0].eType * 64 + (int)team->m_ePowerupList[1].eType;
        if (st.itemTotal >= 0 && total < st.itemTotal) {
            ++st.itemUse;
            if (trace) fprintf(stderr, "[ctl] ITEM used (%d -> %d)\n", st.itemTotal, total);
        } else if (st.itemTotal >= 0 && total == st.itemTotal && key != st.itemKey && total > 0) {
            ++st.itemCycle;
            if (trace) fprintf(stderr, "[ctl] ITEM order changed\n");
        }
        st.itemTotal = total;
        st.itemKey = key;
    }
    const int mode = (int)NpTouch::CurrentMode(physical);
    // the shakedown: each new Z of the record while the opponent's meter runs is one shake (buttons, rear taps and touch swipes alike)
    const bool z = (gc & PAD_TRIGGER_Z) != 0;
    if (mode == (int)NpTouch::Mode::Shake && z && !st.prevZ) ++st.shake;
    st.prevZ = z;
    if (mode != st.mode) {
        if (mode == (int)NpTouch::Mode::Save) ++st.save;
        if (mode == (int)NpTouch::Mode::Shake) ++st.shakeMode;
        if (trace) fprintf(stderr, "[ctl] MINIGAME %s\n", mode == 1 ? "mega save" : mode == 2 ? "mega strike meter" : mode == 3 ? "shakedown" : "none");
        st.mode = mode;
    }
    st.meter = ShootToScoreMeter::instance.m_bMeterVisible;
}
}  // namespace

// The script variables of the harness (np_harness.cpp VarValue: names it does not know are asked here).
extern "C" int NpHarnessGameVar(const char* name, double* out) {
    const CtlStats& st = g_stats;
    struct V { const char* n; double v; };
    const V table[] = {{"cpass", (double)st.pass}, {"clob", (double)st.lob}, {"cshot", (double)st.shot}, {"cchip", (double)st.chip},
                       {"cturbo", (double)st.turbo}, {"cdeke", (double)st.deke}, {"chit", (double)st.hit}, {"cslide", (double)st.slide},
                       {"cswitch", (double)st.switches}, {"ccaptain", (double)st.captain}, {"citem", (double)st.itemUse}, {"ccycle", (double)st.itemCycle}, {"cmega", (double)st.mega},
                       {"csave", (double)st.save}, {"cshakemode", (double)st.shakeMode}, {"cshake", (double)st.shake}, {"cskill", (double)st.skill},
                       {"csuper", (double)st.super}, {"cloose", (double)st.loose}, {"conetimer", (double)st.onetimer}, {"creceive", (double)st.receive},
                       {"cact", (double)st.act}, {"cplayer", (double)st.playerIndex}, {"citems", (double)st.itemTotal},
                       {"cmode", (double)(int)NpTouch::CurrentMode(0)},
                       {"cmeter", ShootToScoreMeter::instance.m_bMeterVisible ? 1.0 : 0.0}, {"cgc", (double)st.lastGc},
                       {"cmeterpress", (double)g_meterPresses}, {"ckpass", (double)st.keeperPass}, {"ckpunt", (double)st.keeperShot}, {"csavea", (double)st.saveA}};
    for (const V& v : table)
        if (!strcmp(name, v.n)) {
            *out = v.v;
            return 1;
        }
    if (!strcmp(name, "cbdx") || !strcmp(name, "cbdy") || !strcmp(name, "cgdx") || !strcmp(name, "cgdy")) {
        // the left stick (unit vector, GameCube convention: +y up) that runs the controlled player toward the ball (cbd*) or toward the
        // opponent's net (cgd*): world direction minus the pad's remap angle (cAIPad::GetMovementStickDirection = remap + stick angle)
        *out = 0;
        const cPlayer* mine = static_cast<const cPlayer*>(st.player);
        if (!mine || !mine->m_pController || !mine->m_pController->m_pGlobalPad || !g_pBall) return 1;
        nlVector3 target = g_pBall->m_v3Position;
        if (name[1] == 'g') {
            cTeam* team = mine->m_pTeam;
            cNet* net = team ? team->GetOtherNet() : 0;
            if (!net) return 1;
            target = net->m_v3NetLocation;
        }
        const nlVector3& me = mine->m_DetChar.m_v3Position;
        const float dx = target.x - me.x, dy = target.y - me.y;
        if (dx * dx + dy * dy < 1e-6f) return 1;
        const float remap = (float)mine->m_pController->m_pGlobalPad->m_aRemapAngle * (6.2831853f / 65536.0f);
        const float a = atan2f(dy, dx) - remap;
        *out = name[3] == 'x' ? cosf(a) : sinf(a);
        return 1;
    }
    if (!strcmp(name, "chave") || !strcmp(name, "cball")) {
        // chave: the local pad controls the ball carrier; cball: -1 loose, 0 the local pad's team has it, 1 the other team
        cPlayer* owner = g_pBall ? g_pBall->m_pOwner : 0;
        const cPlayer* mine = static_cast<const cPlayer*>(st.player);
        if (!strcmp(name, "chave")) *out = (owner && mine && owner == mine) ? 1 : 0;
        else *out = !owner ? -1 : (mine && owner->m_pTeam == mine->m_pTeam) ? 0 : 1;
        return 1;
    }
    return 0;
}

extern "C" void NpInputAfterCapture(NetworkPeerChannel* channel) {
    if (!channel) return;
    DetInput& in = channel->mLocalInput;
    // Only the emulated Wii Remote + Nunchuk (kind 2) carries the tag; a real remote never does.
    if (in.m_nConnected != 2 || !(in.m_ButtonBitfield & NP_GC_TAG)) return;
    const int physical = (in.m_ButtonBitfield >> NP_GC_CHAN_SHIFT) & 3;
    // Mega Save: the engine must keep the Wii controller of the keeper (it reads the pointer and A from it). The tag is
    // dropped so that a receiver does not evaluate its own Save eligibility.
    if (NpTouch::PreserveWii(&channel->mInput, physical)) {
        in.m_ButtonBitfield &= ~(uint32_t)(NP_GC_TAG | (3u << NP_GC_CHAN_SHIFT));
        if (ctl_counting()) {
            // W5 CTL counters: the Mega Save (the Wii controller is kept for the keeper: count its A presses, the block)
            static bool prevA = false;
            const bool a = (in.m_ButtonBitfield & NPW_A) != 0;
            if (a && !prevA) {
                ++g_stats.saveA;
                if (ctl_trace()) fprintf(stderr, "[ctl] SAVE block (A)\n");
            }
            prevA = a;
            const int mode = (int)NpTouch::CurrentMode(physical);
            if (mode != g_stats.mode) {
                if (mode == (int)NpTouch::Mode::Save) ++g_stats.save;
                if (ctl_trace()) fprintf(stderr, "[ctl] MINIGAME %s\n", mode == 1 ? "mega save" : "other");
                g_stats.mode = mode;
            }
        }
        return;
    }
    PADStatus pad;
    if (!NpPad_ReadGameCube || !NpPad_ReadGameCube(physical, &pad)) return;   // the controller is gone: leave the capture as it is
    in.m_nConnected = 3;                                // the engine's GameCube controls
    uint16_t buttons = pad.button & 0x1F7F;
    uint8_t triggerL = pad.triggerLeft, triggerR = pad.triggerRight;
    // W5 CTL: the layout (np_controls.h). CLASSIC is the GameCube pad by position, as before; MODERN maps the Vita buttons with the
    // match context; the rear taps join either when the rear touchpad is on. Only the record changes: what follows is the same.
    uint32_t vita = physical >= 0 && physical < 4 ? g_ctl[physical].vita : 0;
    if (physical >= 0 && physical < 4 && (g_ctl[physical].layout == NpControls::kModern || g_ctl[physical].rear)) {
        CtlChannel& ctl = g_ctl[physical];
        if (ctl.sampleMs - ctl.rearTapMs > 250) ctl.rearLeft = ctl.rearRight = 0;   // taps made in a menu or before a pause are not kept
        NpControls::Context cx;
        cx.possession = local_has_ball(channel);
        cx.shakedown = NpTouch::CurrentMode(physical) == NpTouch::Mode::Shake;
        if (ctl.layout == NpControls::kModern) {
            buttons = ctl.mapper.Map(vita, ctl.rear ? &ctl.rearRight : 0, ctl.rear ? &ctl.rearLeft : 0, cx) & 0x1F7F;
            triggerL = (buttons & PAD_TRIGGER_L) ? 255 : 0;
            triggerR = (buttons & PAD_TRIGGER_R) ? 255 : 0;
        } else if (ctl.rear) {
            buttons |= ctl.mapper.Map(0, &ctl.rearRight, &ctl.rearLeft, cx) & (PAD_BUTTON_A | PAD_TRIGGER_Z);   // CLASSIC + rear taps
        }
    }
    in.m_ButtonBitfield = buttons;
    in.m_LeftTrigger = triggerL;
    in.m_RightTrigger = triggerR;
    in.m_AnalogRightX = clampf(pad.substickX / 72.0f, -1.0f, 1.0f);
    in.m_AnalogRightY = clampf(pad.substickY / 72.0f, -1.0f, 1.0f);
    in.m_v3RevRemoteAccel.x = in.m_v3RevRemoteAccel.y = in.m_v3RevRemoteAccel.z = 0.0f;
    in.m_v3RevFreeStyleAccel.x = in.m_v3RevFreeStyleAccel.y = in.m_v3RevFreeStyleAccel.z = 0.0f;
    in.m_nRevDPDNumTargets = 0;
    in.m_v2RevDPDCoord.x = in.m_v2RevDPDCoord.y = 0.0f;
    // The shakedown swipe of the touch panel is the item toggle (Z) for two captures
    if (NpTouch::ConsumeShake(&channel->mInput)) g_shakeHoldFrames = 2;
    if (g_shakeHoldFrames > 0) {
        in.m_ButtonBitfield |= PAD_TRIGGER_Z;
        --g_shakeHoldFrames;
    }
    if (ctl_counting()) ctl_observe(channel, physical, vita, in.m_ButtonBitfield);
}

// The capture is already the final record: nothing to normalize for the network.
extern "C" bool NpInputNormalizeForNetwork(DetInput*) { return true; }

// ---- the GameCube forms of the Wii-only actions -------------------------------------------------------------------------
extern "C" bool NpInputGcRightShake(cAIPad* pad, uint16_t* direction) {
    // The big hit: Y without the ball (SMS), where the Wii shakes the remote.
    *direction = 0;
    DetInput* det = pad->m_pGlobalPad;
    if (!det) return false;
    cPlayer* owner = g_pBall ? g_pBall->m_pOwner : 0;
    const bool hasBall = owner && owner->m_pController && owner->m_pController->m_pGlobalPad == det;
    return !hasBall && det->JustPressed(kActHit, true);
}

extern "C" bool NpInputGcLeftShake(cAIPad* pad, uint16_t* direction) {
    // Cycle the item: Z (SMS), where the Wii shakes the Nunchuk. A touch swipe was folded into Z at capture.
    *direction = 0;
    DetInput* det = pad->m_pGlobalPad;
    return det && det->JustPressed(kActToggleItem, true);
}

extern "C" bool NpInputGcActionAngle(cFielder* fielder, uint16_t* direction) {
    // The D-pad-driven actions of a GameCube controller (slide tackle, deke, the character special), as the angle
    // GetDekePadDirection reports: right 0, up 0x4000 (screen space, like the D-pad and the sticks).
    DetInput* det = fielder && fielder->m_pController ? fielder->m_pController->m_pGlobalPad : 0;
    if (!det || det->m_nConnected != 3) return false;
    auto polar = [](const nlPolar& p, uint16_t* a) {
        *a = p.a;
        return p.r == p.r && p.r > 0.3f;   // NaN-safe: the game's fast sqrt gives NaN for a centred stick
    };
    uint16_t leftAngle = 0, cAngle = 0;
    const bool left = polar(det->m_PolarAnalogLeft, &leftAngle);
    // A centred stick: the way the player faces, back in screen space.
    const uint16_t facing = (uint16_t)(fielder->m_DetChar.m_aActualFacingDirection - det->m_aRemapAngle);
    const uint16_t moveAngle = left ? leftAngle : facing;
    if (fielder->m_pBall != 0) {
        // With the ball: deke on Y (toward the left stick) or the C stick (toward the C stick).
        if (polar(det->m_PolarAnalogRight, &cAngle)) {
            *direction = cAngle;
            return true;
        }
        if (det->IsPressed(kActDeke, true)) {
            *direction = moveAngle;
            return true;
        }
    } else {
        // Without it: slide tackle on B, only while the other team has the ball.
        cPlayer* owner = g_pBall ? g_pBall->m_pOwner : 0;
        if (owner && owner->m_pTeam != fielder->m_pTeam && det->IsPressed(kActSlide, true)) {
            *direction = moveAngle;
            return true;
        }
    }
    // Charged extra: R does what the D-pad does in Charged (special move, slide or deke).
    if (det->m_ButtonBitfield & kPadTriggerR) {
        *direction = moveAngle;
        return true;
    }
    return false;
}
