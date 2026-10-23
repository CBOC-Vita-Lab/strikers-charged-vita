// np_touch.cpp: the touch minigames of the Vita controls.
//
// Port of the recompiled runtime's touch layer (one/port/runtime/src/hle/input/msc_touch.cpp, include/hle/
// msc_touch_{gate,state,runtime}.h; the gate identity fix of commit 777dd91 is kept: the pad object is not part of the
// identity of a minigame owner), with the gate written against the decompiled classes instead of guest memory.
//
// Three minigames take the touch panel (and the R button) while the LOCAL pad owns them:
//   Save   the defending goalkeeper's Mega Strike save: the finger is the aiming pointer and holds A (the punch);
//          R held (after it was released once) also holds A; the right stick may aim.
//   Meter  the attacker's Mega Strike meter: a finger holds the meter button (A when gMegaStrikeUsePassButton, else B).
//   Shake  the "shakedown" while the other side shoots the meter: a swipe of 48 px within 400 ms is one shake (the
//          engine's GameCube controls turn it into Z, see np_game_input.cpp).
// The engine reads the Save pointer and the meter buttons from the pad backend (the emulated remote), so these controls
// are written into the SDK's sample (NpPadAdjust), never into the GameCube view of the pad.
#include "Game/AI/Fielder.h"
#include "Game/AI/AIPad.h"
#include "Game/Ball.h"
#include "Game/BaseGameSceneManager.h"
#include "Game/DetInput.h"
#include "Game/FE/Overlay/OverlayHandlerMegaStrikeMeter.h"
#include "Game/FE/feManager.h"
#include "Game/Game.h"
#include "Game/Goalie.h"
#include "Game/NetworkPeer.h"
#include "Game/NetworkSession.h"
#include "Game/Player.h"
#include "Game/Render/ShootToScoreMeter.h"
#include "Game/Team.h"
#include "NL/globalpad.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/sdk/input/np_game_input.h"
#include "platform/sdk/input/np_controls.h"
#include "np/touch_diag.h"

extern float gMegaBallPointerXScale;
extern float gMegaBallPointerYScale;
extern bool gMegaStrikeUsePassButton;

namespace {

using NpTouch::Mode;

struct Gate {
    Mode mode = Mode::None;
    const void* token = 0;       // the keeper (Save) or the attacker (Meter, Shake)
    const DetInput* det = 0;     // the owner's apply-side input
    const void* pad = 0;
    int channel = 0;
    bool meterA = false;
    // The same owner of the same minigame. The pad object is NOT part of the identity: the game's pad manager keeps two
    // banks and switches the active one inside a frame (bank 1 for the fixed simulation ticks, bank 0 for the frame-level
    // update), so one owner can resolve to two different pad objects within a single frame. Keying the gesture state on
    // the pad object reset it at every switch and no touch or R press was ever accepted (recompiled build, 777dd91).
    bool SameOwner(const Gate& o) const {
        return mode == o.mode && token == o.token && det == o.det && channel == o.channel && meterA == o.meterA;
    }
};

struct LocalPad {
    const DetInput* det = 0;
    const void* pad = 0;
};

// NetworkPeerChannel::GetLocalChannelPad: the local pad behind a player's input. The kind of DetInput or the team
// number alone does not establish ownership; the pad must be the one the channel reads, on the physical channel asked.
LocalPad resolve_player(cPlayer* player, int channel) {
    LocalPad none;
    if (!player || !player->m_pController) return none;
    DetInput* det = player->m_pController->m_pGlobalPad;
    if (!det) return none;
    const int kind = det->m_nConnected;
    if (kind < 1 || kind > 3) return none;
    NetworkPeerChannel* peerChannel = static_cast<NetworkPeerChannel*>(det->m_pMyUser);
    if (!peerChannel) return none;
    const int index = peerChannel->mGlobalPadIndex;
    if (index < 0 || index >= 4 || !g_pNetworkSessionBase) return none;
    cGlobalPad* pad = peerChannel->GetLocalChannelPad();   // null unless the channel belongs to the local peer
    if (!pad || pad->m_padIndex != channel) return none;
    LocalPad r;
    r.det = det;
    r.pad = pad;
    return r;
}

LocalPad team_pad(cTeam* team, int channel) {
    for (int i = 0; i < 5; ++i) {
        cPlayer* player = team->m_pPlayers[i];
        if (!player || player->m_pTeam != team) continue;
        LocalPad local = resolve_player(player, channel);
        if (local.pad) return local;
    }
    return LocalPad();
}

// The player the Mega Strike meter overlay (scene 100) belongs to; none when there are two overlays.
cFielder* meter_player() {
    BaseGameSceneManager* manager = g_pOverlayManager;
    if (!manager) return 0;
    const unsigned long count = manager->mCurrentStackDepth;
    if (count > BaseGameSceneManager::MAX_SCENE_DEPTH) return 0;
    cFielder* result = 0;
    for (unsigned long i = 0; i < count; ++i) {
        if ((int)manager->m_sceneStack[i] != 100) continue;
        if (result) return 0;
        MegaStrikeMeterOverlay* overlay = static_cast<MegaStrikeMeterOverlay*>(manager->mBaseSceneHandlerStack[i]);
        if (!overlay) return 0;
        result = overlay->mFielder;
    }
    return result;
}

Gate query_gate(int channel) {
    Gate none;
    if (channel < 0 || channel >= 4) return none;
    if (FrontEnd::m_bInPauseMenuState) return none;
    if (!g_pGame || !g_pNetworkSessionBase) return none;
    cTeam* teams[2] = {g_pTeams[0], g_pTeams[1]};
    if (!teams[0] || !teams[1] || teams[0] == teams[1]) return none;

    // Both keepers use the Mega Strike action; only the defender has substates 0..9. Do not couple eligibility to the other
    // keeper's independently updated state.
    for (int side = 0; side < 2; ++side) {
        Goalie* keeper = static_cast<Goalie*>(teams[side]->m_pPlayers[4]);
        if (!keeper) continue;
        if ((int)keeper->mGoalieActionState != GOALIEACTION_MEGA_STRIKE || keeper->mnSubstate > 9 ||
            keeper->m_pTeam != teams[side])
            continue;
        if (keeper->mMegaMachine != g_pNetworkSessionBase->GetLocalMachineId()) continue;
        LocalPad local = resolve_player(keeper, channel);
        if (!local.pad) {
            // The original updater only searches fielders when the keeper lacks a Wii-capable pad. Never commandeer
            // another local or remote Wii keeper.
            cAIPad* ai = keeper->m_pController;
            if (ai && ai->m_pGlobalPad) {
                const int kind = ai->m_pGlobalPad->m_nConnected;
                if (kind == 1 || kind == 2) continue;
            }
            local = team_pad(teams[side], channel);
        }
        if (local.pad) {
            Gate g;
            g.mode = Mode::Save;
            g.token = keeper;
            g.det = local.det;
            g.pad = local.pad;
            g.channel = channel;
            return g;
        }
    }

    cFielder* attacker = meter_player();
    if (!attacker || attacker->m_eActionState != ACTION_SHOOT_TO_SCORE || attacker->m_eClassType != FIELDER) return none;
    cTeam* attackTeam = attacker->m_pTeam;
    if (attackTeam != teams[0] && attackTeam != teams[1]) return none;
    if (!g_pBall || g_pBall->m_pOwner != attacker) return none;
    LocalPad own = resolve_player(attacker, channel);
    if (own.pad) {
        Gate g;
        g.mode = Mode::Meter;
        g.token = attacker;
        g.det = own.det;
        g.pad = own.pad;
        g.channel = channel;
        g.meterA = gMegaStrikeUsePassButton;
        return g;
    }
    if (!ShootToScoreMeter::instance.m_bMeterVisible) return none;
    cTeam* defendingTeam = attackTeam == teams[0] ? teams[1] : teams[0];
    LocalPad defender = team_pad(defendingTeam, channel);
    if (defender.pad) {
        Gate g;
        g.mode = Mode::Shake;
        g.token = attacker;
        g.det = defender.det;
        g.pad = defender.pad;
        g.channel = channel;
        return g;
    }
    return none;
}

// ---- gestures ---------------------------------------------------------------------------------------------------
struct Point {
    unsigned id;
    float x, y;
};
struct Frame {
    bool valid = false;
    unsigned count = 0;
    Point points[8];
};
struct Viewport {
    float x = 0, y = 0, width = 960, height = 544;
    bool flipY = false;
};
struct Output {
    bool hold = false, pointer = false, shake = false;
    float x = 0, y = 0;
};

bool project_save_pointer(float x, float y, float scaleX, float scaleY, float* px, float* py) {
    // Mega Save maps the pointer to a 640x480 canvas using (400, 300) as its scale, not the half-canvas dimensions. Invert
    // its actual calibration.
    if (!isfinite(scaleX) || !isfinite(scaleY) || scaleX < 320 || scaleY < 240) return false;
    *px = x * 320.0f / scaleX;
    *py = y * 240.0f / scaleY;
    return isfinite(*px) && isfinite(*py);
}

class State {
public:
    Output Update(const Gate& gate, uint32_t epoch, uint32_t nowMs, const Frame& frame, const Viewport& viewport, bool blocked,
                  bool clampDrag = false) {
        // Revocation must win even if WPAD and KPAD read the same sample.
        if (blocked || gate.mode == Mode::None || !valid(frame, viewport)) {
            Reset();
            return Output();
        }
        if (!gate_.SameOwner(gate)) {
            Reset();
            gate_ = gate;
        }
        if (cached_ && epoch_ == epoch) return output_;
        cached_ = true;
        epoch_ = epoch;
        output_ = Output();
        if (!frame.count) {
            armed_ = true;
            active_ = false;
            fired_ = false;
            return output_;
        }
        if (!armed_) return output_;
        const Point* point = 0;
        for (unsigned i = 0; i < frame.count; ++i) {
            if (active_ ? frame.points[i].id == id_ : (!point || frame.points[i].id < point->id)) point = &frame.points[i];
        }
        if (!point || ((!clampDrag || !active_) && (point->x < viewport.x || point->x > viewport.x + viewport.width ||
            point->y < viewport.y || point->y > viewport.y + viewport.height))) {
            armed_ = active_ = false;
            return output_;
        }
        if (!active_) {
            active_ = true;
            id_ = point->id;
            startX_ = point->x;
            startY_ = point->y;
            startMs_ = nowMs;
            fired_ = false;
        }
        if (gate.mode == Mode::Shake) {
            const float dx = point->x - startX_, dy = point->y - startY_;
            if (!fired_ && nowMs - startMs_ <= 400 && dx * dx + dy * dy >= 48.0f * 48.0f) {
                output_.shake = true;
                fired_ = true;
            }
        } else {
            output_.hold = true;
            output_.pointer = gate.mode == Mode::Save;
            output_.x = 2.0f * (point->x - viewport.x) / viewport.width - 1.0f;
            output_.y = 2.0f * (point->y - viewport.y) / viewport.height - 1.0f;
            if (clampDrag) {
                output_.x = fmaxf(-1.0f, fminf(1.0f, output_.x));
                output_.y = fmaxf(-1.0f, fminf(1.0f, output_.y));
            }
            if (viewport.flipY) output_.y = -output_.y;
        }
        return output_;
    }
    void Reset() { *this = State(); }

private:
    static bool valid(const Frame& frame, const Viewport& v) {
        if (!frame.valid || frame.count > 8 || !isfinite(v.x) || !isfinite(v.y) || !isfinite(v.width) || !isfinite(v.height) ||
            v.width <= 0 || v.height <= 0)
            return false;
        for (unsigned i = 0; i < frame.count; ++i) {
            if (!isfinite(frame.points[i].x) || !isfinite(frame.points[i].y)) return false;
            for (unsigned j = 0; j < i; ++j)
                if (frame.points[i].id == frame.points[j].id) return false;
        }
        return true;
    }
    Gate gate_;
    Output output_;
    uint32_t epoch_ = 0, startMs_ = 0;
    unsigned id_ = 0;
    float startX_ = 0, startY_ = 0;
    bool cached_ = false, armed_ = false, active_ = false, fired_ = false;
};

struct Channel {
    State state;
    Gate gate;
    Output output;
    Gate shoulderGate;
    float scaleX = 0, scaleY = 0;
    bool shakeConsumed = false;
    bool shoulderArmed = false, shoulderOutput = false;
    bool crossArmed = false, haveRearSample = false;
    uint32_t lastRearMs = 0;
};
Channel s_touch[4];
Viewport g_viewport;   // the part of the 960 x 544 panel the picture covers (the whole panel by default)

bool touch_enabled() {
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("MSC_TOUCH_MINIGAMES");
        on = !(e && *e == '0');
    }
    return on == 1;
}
// W5 CTL: each minigame's touch can be switched off on its own (MSC_TOUCH_SAVE / MSC_TOUCH_METER / MSC_TOUCH_SHAKE = 0;
// MSC_TOUCH_MINIGAMES=0 switches all three off). The buttons of the minigames work either way.
bool mode_touch_enabled(Mode mode) {
    static int on[4] = {-1, -1, -1, -1};
    const int i = (int)mode;
    if (i < 1 || i > 3) return false;
    if (on[i] < 0) {
        const char* names[4] = {0, "MSC_TOUCH_SAVE", "MSC_TOUCH_METER", "MSC_TOUCH_SHAKE"};
        const char* e = getenv(names[i]);
        on[i] = !(e && *e == '0');
    }
    return on[i] == 1;
}
bool trace_enabled() {
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("MSC_TOUCH_TRACE");
        on = (e && *e == '1');
    }
    return on == 1;
}

void trace(int channel, const Gate& gate, const Output& output) {
    if (!trace_enabled() || channel < 0 || channel >= 4) return;
    struct Previous {
        Gate gate;
        Output output;
    };
    static Previous previous[4];
    static unsigned events = 0;
    Previous& last = previous[channel];
    const bool mode = !gate.SameOwner(last.gate);
    const bool edge = output.hold != last.output.hold || (output.shake && !last.output.shake);
    if ((mode || edge) && events < 64) {
        ++events;
        const char* name = gate.mode == Mode::Save ? "save" : gate.mode == Mode::Meter ? "meter" : gate.mode == Mode::Shake ? "shake" : "none";
        fprintf(stderr, "[touch] channel=%d mode=%s owner=%p det=%p event=%s\n", channel, name, gate.token, (const void*)gate.det,
                output.shake ? "swipe" : edge ? (output.hold ? "down" : "up") : "mode");
    }
    last.gate = gate;
    last.output = output;
}

}  // namespace

extern "C" void NpTouchSetViewport(float x, float y, float width, float height, int flipY) {
    g_viewport.x = x;
    g_viewport.y = y;
    g_viewport.width = width;
    g_viewport.height = height;
    g_viewport.flipY = flipY != 0;
}

namespace NpTouch {

bool Apply(int chan, const NpRawPad& raw, uint32_t* hold, float cursor[2], uint32_t epoch, uint32_t nowMs, bool menu) {
    // Real HID remotes never contain the synthetic GameCube tag. The Vita owns only physical channel zero; no touch may
    // leak to another or a disconnected pad.
    const bool emulated = chan == 0 && ((*hold & 0x80u) != 0) && (((*hold >> 5) & 3u) == (uint32_t)chan);
    if (chan < 0 || chan >= 4) return false;
    Channel& t = s_touch[chan];
    if (raw.source == NP_RAW_BLOCKED && NpControls::RearSaveEnabled()) {
        t = Channel();
        return false;
    }
    if (!emulated) {
        t = Channel();
        trace(chan, Gate(), Output());
        return false;
    }
    t.shakeConsumed = false;   // a new sample: a new swipe may be consumed
    t.gate = query_gate(chan);
    if (t.gate.mode == Mode::None) {
        t = Channel();
        trace(chan, Gate(), Output());
        return false;
    }
    if (t.gate.mode == Mode::Save) {
        t.scaleX = gMegaBallPointerXScale;
        t.scaleY = gMegaBallPointerYScale;
        float px, py;
        if (!project_save_pointer(0, 0, t.scaleX, t.scaleY, &px, &py)) {
            t = Channel();
            trace(chan, Gate(), Output());
            return false;
        }
    }
    const bool rear = t.gate.mode == Mode::Save && raw.source == NP_RAW_LIVE && NpControls::RearSaveEnabled() &&
                      NpControls::PhysicalMegaControlsAllowed();
    if (rear && menu) {
        t = Channel();
        return false;
    }
    if (rear && t.haveRearSample && nowMs - t.lastRearMs > 1000u) {
        t.state.Reset();
        t.shoulderArmed = t.shoulderOutput = t.crossArmed = false;
    }
    t.haveRearSample = rear;
    t.lastRearMs = nowMs;
    Frame frame;
    Viewport viewport = g_viewport;
    const bool panel = touch_enabled() && mode_touch_enabled(t.gate.mode);
    if (rear) {
        // A grip or unavailable sample cannot arm a new gesture. An acquired finger may slide to an edge.
        frame.valid = raw.rear_count >= 0 && raw.rear_count <= 1;
        frame.count = raw.rear_count == 1 ? 1 : 0;
        if (frame.count) frame.points[0] = {(unsigned)raw.rear[0].id, raw.rear[0].x, raw.rear[0].y};
        viewport.x = 40; viewport.y = 30; viewport.width = 880; viewport.height = 484;
    } else if (panel) {
        frame.valid = true;
        frame.count = raw.touch_count > 8 ? 8 : (raw.touch_count < 0 ? 0 : (unsigned)raw.touch_count);
        for (unsigned i = 0; i < frame.count; ++i) frame.points[i] = {(unsigned)raw.touch[i].id, raw.touch[i].x, raw.touch[i].y};
    }
    const bool shoulderHeld = (raw.buttons & NP_BTN_R) != 0;
    t.output = t.state.Update(t.gate, epoch, nowMs, frame, viewport, false, rear);
    if (rear) t.output.hold = false;
    // R as the save-only punch: it counts only after it was released once during this minigame (a button already held
    // when the save began is not a punch).
    const Gate& a = t.shoulderGate;
    const Gate& b = t.gate;
    if (b.mode != Mode::Save || !a.SameOwner(b)) {
        t.shoulderArmed = t.shoulderOutput = false;
        t.crossArmed = false;
        t.shoulderGate = b;
    }
    if (b.mode == Mode::Save) {
        if (rear) {
            if (!(raw.buttons & NP_BTN_CROSS)) t.crossArmed = true;
            if (!t.crossArmed) *hold &= ~NPW_A;
        }
        if (!shoulderHeld) t.shoulderArmed = true;
        t.shoulderOutput = shoulderHeld && t.shoulderArmed;
    }
    trace(chan, t.gate, t.output);
    if(NpDiagActive())NpDiagEvent(NPD_GESTURE,(unsigned)t.gate.mode|(t.output.hold?16u:0u)|(t.output.pointer?32u:0u)|(rear?64u:0u)|(rear&&!frame.valid?128u:0u),
        NpDiagFrame(),NpDiagUsedPoll(),epoch,NpDiagStepId(),frame.count,NpDiagFloats(t.output.x,t.output.y));

    // compose the hold: the finger holds A (Save, or the meter on the pass button) or B (the other meter button)
    if (t.output.hold) {
        if (t.gate.mode == Mode::Save || (t.gate.mode == Mode::Meter && t.gate.meterA)) *hold |= NPW_A;
        else if (t.gate.mode == Mode::Meter) *hold |= NPW_B;
    }
    if (t.shoulderOutput) *hold |= NPW_A;
    if (t.output.pointer) {
        float x, y;
        if (project_save_pointer(t.output.x, t.output.y, t.scaleX, t.scaleY, &x, &y)) {
            cursor[0] = x;
            cursor[1] = y;
        }
    }
    return true;
}

bool PreserveWii(const DetInput* det, int channel) {
    // Called only for the tagged emulated input. Eligibility lasts throughout the defender's substates 0..9, independently
    // of contact, so it cannot oscillate.
    if (channel != 0) return false;
    const Gate g = query_gate(0);
    return g.mode == Mode::Save && g.det == det;
}

bool ConsumeShake(const DetInput* det) {
    Channel& t = s_touch[0];
    if (t.shakeConsumed || !t.output.shake) return false;
    const Gate g = query_gate(0);
    if (g.mode != Mode::Shake || g.det != det) return false;
    t.shakeConsumed = true;
    return true;
}

bool SelectSavePointerAxes(int chan, float rightX, float rightY, float* x, float* y) {
    if (chan != 0) return false;
    if (query_gate(0).mode != Mode::Save) return false;
    if (isfinite(rightX) && isfinite(rightY) && (fabsf(rightX) >= 0.2f || fabsf(rightY) >= 0.2f)) {
        *x = rightX;
        *y = rightY;
    }
    return true;
}

void PanelToCursor(float px, float py, float out[2]) {
    float x = 2.0f * (px - g_viewport.x) / g_viewport.width - 1.0f;
    float y = 2.0f * (py - g_viewport.y) / g_viewport.height - 1.0f;
    if (g_viewport.flipY) y = -y;
    out[0] = x < -1.0f ? -1.0f : x > 1.0f ? 1.0f : x;
    out[1] = y < -1.0f ? -1.0f : y > 1.0f ? 1.0f : y;
}

Mode CurrentMode(int chan) {
    if (chan < 0 || chan >= 4) return Mode::None;
    return s_touch[chan].gate.mode;
}

}  // namespace NpTouch

extern "C" int NpDiagInputMode(){return (int)NpTouch::CurrentMode(0);}
