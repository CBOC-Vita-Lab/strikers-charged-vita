// Picture presets (LOOK, wave 5). MSC_PICTURE=performance|default|quality in np-player.txt / env.txt expands to the
// switches below, for each one the file does not set itself (so a settings screen can write MSC_PICTURE and then
// override single keys, and a hand-written file keeps working). "default" and an absent key set nothing: the
// build's own defaults are the Default preset. Header only, no platform calls: tools/look/preset_test.cpp builds it
// on the host.
//
// Every number is an estimate until a console measures it ([E]); the evidence is in docs/wave5-LOOK.md.
#ifndef NP_PICTURE_PRESETS_H
#define NP_PICTURE_PRESETS_H

#include <cstring>

#include "vita_plan_defaults.h"

namespace np_picture {

struct Setting {
    const char* name;
    const char* value;
};

// Performance: fewer pixels (18 Nov, the owner's floor: 864x488, the smallest picture there is, 81 % of 960x544's pixels; it was 640x362 in 1.0-1.1, which the Vita plan now raises to this; the
// picture goes up to 960x544 through the present blit as now) and the MSP "Performance Mode" (no crowd impostors, no
// decorative non-player characters: fewer draws and less game-thread work in the stadium).
static const Setting kPerformance[] = {
    {"MSC_VITA_RENDER", "864x488"},
    {"MSC_PERFORMANCE_MODE", "1"},
    {nullptr, nullptr},
};

// Default: nothing. The game's own 640x448 picture, blit-scaled to the screen.
static const Setting kDefault[] = {
    {nullptr, nullptr},
};

// Quality: the picture is drawn at the screen's own size, so every pass target, shadow, glow and 2D layer is 1.5 x
// finer in each direction and the present is 1:1 (no scaling blur). 1.82 x the pixels of the default [E].
static const Setting kQuality[] = {
    {"MSC_VITA_RENDER", "960x544"},
    {nullptr, nullptr},
};

// Full (1.3, MSC_PICTURE_HOLD, renderer/src/vgx_hold.h): the picture is drawn at the screen's own size and held there; a frame over the 60 budget is made cheaper by taking work away in a
// fixed order (far players' shadow pictures, all shadow pictures, the crowd's refresh, the effects pass's target) before one small size step is ever taken. The effects pass's smaller
// target needs the compositor (MSC_FX_COMPOSITE); without it that step is skipped. Not run on a console.
static const Setting kFull[] = {
    {"MSC_PICTURE_HOLD", "full"},
    {"MSC_FX_GOV", "1"},
    {"MSC_FX_COMPOSITE", "1"},
    {nullptr, nullptr},
};

// Lock60: the LOCKED 60 mode (vita_plan.cpp adds its supporting defaults when it reads MSC_LOCK60).
static const Setting kLock60[] = {
    {"MSC_LOCK60", "1"},
    {nullptr, nullptr},
};

inline const Setting* Find(const char* preset) {
    if (!preset || !*preset) return nullptr;
    if (!std::strcmp(preset, "performance") || !std::strcmp(preset, "perf") || !std::strcmp(preset, "fast")) return kPerformance;
    if (!std::strcmp(preset, "default") || !std::strcmp(preset, "normal")) return kDefault;
    if (!std::strcmp(preset, "quality") || !std::strcmp(preset, "high")) return kQuality;
    if (!std::strcmp(preset, "full") || !std::strcmp(preset, "hold")) return kFull;
    if (!std::strcmp(preset, "lock60") || !std::strcmp(preset, "locked60") || !std::strcmp(preset, "60")) return kLock60;
    return nullptr;
}

// Calls set(name, value) for every setting of the preset that `isSet(name)` says is not set already. Returns the number
// of settings applied, or -1 for an unknown preset name.
template <typename IsSet, typename Set>
int Expand(const char* preset, IsSet isSet, Set set) {
    const Setting* list = Find(preset);
    if (!list) return -1;
    int n = 0;
    for (; list->name; ++list) {
        if (isSet(list->name)) continue;
        set(list->name, list->value);
        ++n;
    }
    return n;
}

// ---- the gates of the ladder / child keys (galaxyport18, from Galaxy c171/c172: a picture mode that sets a key whose switch is off is a feature that never runs, and nothing says so) ----
// A child key set to a nonzero value is read only while its gate is on (kNeedsOn) or off (kNeedsOff); a picture-mode column (every preset of np_settings.cpp, the lists above, and every choice of
// every row on its own) must not set a child while the gate it needs is off in that column. A key the column does not name has the build's default (gateDefaultOn). The readers: renderer/src/
// vgx_backend_gl.cpp (DYNRES, FX_LOWRES, PICTURE_HOLD), np_fps.cpp (lock60, replay, between, pair, head), np_bg30.cpp. tests/test_settings.cpp runs the check; a new ladder key goes in this table.
enum GateRule { kNeedsOn, kNeedsOff };
struct Gate {
    const char* child;
    const char* gate;
    GateRule rule;
    bool gateDefaultOn;   // the gate's state when the column does not name it (the build's default)
};
static const Gate kGates[] = {
    // the dynamic-resolution ladder: its rungs and limits exist only while the ladder runs (MSC_DYNRES not off, no fixed picture size, and not the full-picture hold: with MSC_PICTURE_HOLD on the plan
    // fixes the size at 960x544 and the hold's own policy replaces the ladder: vita_plan.cpp "MSC_PICTURE_HOLD=full: fixed picture size ..., no ladder")
    {"MSC_DYNRES_MAX", "MSC_DYNRES", kNeedsOn, true},       {"MSC_DYNRES_MAX", "MSC_VITA_RENDER", kNeedsOff, false}, {"MSC_DYNRES_MAX", "MSC_PICTURE_HOLD", kNeedsOff, true},
    {"MSC_DYNRES_MIN60", "MSC_DYNRES", kNeedsOn, true},     {"MSC_DYNRES_MIN60", "MSC_VITA_RENDER", kNeedsOff, false}, {"MSC_DYNRES_MIN60", "MSC_PICTURE_HOLD", kNeedsOff, true},
    {"MSC_DYNRES_MIN", "MSC_DYNRES", kNeedsOn, true},       {"MSC_DYNRES_MIN", "MSC_VITA_RENDER", kNeedsOff, false}, {"MSC_DYNRES_MIN", "MSC_PICTURE_HOLD", kNeedsOff, true},
    {"MSC_VGX_MSAA", "MSC_DYNRES", kNeedsOn, true},         {"MSC_VGX_MSAA", "MSC_VITA_RENDER", kNeedsOff, false}, {"MSC_VGX_MSAA", "MSC_PICTURE_HOLD", kNeedsOff, true},
    // the effects pass's smaller target: its scene list and size floor are read only while MSC_FX_LOWRES is not off
    {"MSC_FX_LOWRES_SCENES", "MSC_FX_LOWRES", kNeedsOn, true}, {"MSC_FX_LOWRES_MINPX", "MSC_FX_LOWRES", kNeedsOn, true},
    // the full-picture hold's own options
    {"MSC_PICTURE_HOLD_MSAA", "MSC_PICTURE_HOLD", kNeedsOn, true}, {"MSC_PICTURE_HOLD_MSAA_HELD", "MSC_PICTURE_HOLD", kNeedsOn, true},
    {"MSC_PICTURE_HOLD_START", "MSC_PICTURE_HOLD", kNeedsOn, true}, {"MSC_PICTURE_HOLD_SCENES", "MSC_PICTURE_HOLD", kNeedsOn, true},
    {"MSC_PICTURE_HOLD_AALAST", "MSC_PICTURE_HOLD", kNeedsOn, true}, {"MSC_PICTURE_HOLD_SCENEPOL", "MSC_PICTURE_HOLD", kNeedsOn, true},
    // the effects governor's Mega Strike step
    {"MSC_FX_GOV_MEGA", "MSC_FX_GOV", kNeedsOn, false},
    // the pair controller's options (np_fps.cpp: nothing of them is read while MSC_PAIR_RATE is 0), the 30 background's screens (needs MSC_BG30=2), the [head] line (needs the [late] clocks)
    {"MSC_PAIR_FAST_LEAVE", "MSC_PAIR_RATE", kNeedsOn, false}, {"MSC_PAIR_FAST_MS", "MSC_PAIR_RATE", kNeedsOn, false}, {"MSC_PAIR_SCREENS", "MSC_PAIR_RATE", kNeedsOn, false},
    {"MSC_BG30_SCREENS", "MSC_BG30", kNeedsOn, false},
    {"MSC_HEAD_STAT", "MSC_LATE_CAUSE", kNeedsOn, true},
    // LOCKED 60 runs only with the ladder, no fixed size and the AUTO frame rate (np_fps.cpp lock60, vita_plan.cpp: "does nothing here"); under it a replay's rate and UP TO 60 are not read
    {"MSC_LOCK60", "MSC_DYNRES", kNeedsOn, true},
    {"MSC_LOCK60", "MSC_VITA_RENDER", kNeedsOff, false},
    {"MSC_LOCK60", "MSC_FPS", kNeedsOff, false},
    {"MSC_FPS_REPLAY", "MSC_LOCK60", kNeedsOff, false},
    {"MSC_FPS_BETWEEN", "MSC_LOCK60", kNeedsOff, false},
    {"MSC_FPS_BETWEEN", "MSC_FPS", kNeedsOff, false},
};

// Whether a value turns its key on. 0, off, no, none and empty are off; MSC_FPS "auto" is the automatic rate (the gate MSC_FPS means a named rate); MSC_BG30 acts as 2 only.
inline bool KeyOn(const char* key, const char* value) {
    if (!value || !*value) return false;
    if (!std::strcmp(value, "0") || !std::strcmp(value, "off") || !std::strcmp(value, "no") || !std::strcmp(value, "none")) return false;
    if (!std::strcmp(key, "MSC_FPS") && !std::strcmp(value, "auto")) return false;
    if (!std::strcmp(key, "MSC_BG30")) return !std::strcmp(value, "2");
    return true;
}

// The first gate that a column breaks, or null. get(name) returns the column's value for a key, or null when the column does not name it.
template <typename Get>
const Gate* FirstGateBroken(Get get) {
    for (const Gate& g : kGates) {
        const char* child = get(g.child);
        if (!child || !KeyOn(g.child, child)) continue;   // (a child set to 0 or off is the switch turned off: harmless)
        const char* gv = get(g.gate);
        bool on = gv ? KeyOn(g.gate, gv) : g.gateDefaultOn;
        // the hold is on by default only where vita_plan.cpp turns it on: not with a fixed size, LOCKED 60, a 30 or 45 FPS rate, the dynamic resolution off or the older MSC_VITA_MSAA
        if (!gv && !std::strcmp(g.gate, "MSC_PICTURE_HOLD")) {
            const char* r = get("MSC_VITA_RENDER"); const char* l = get("MSC_LOCK60"); const char* f = get("MSC_FPS"); const char* d = get("MSC_DYNRES"); const char* m = get("MSC_VITA_MSAA");
            const char* c45 = get("MSC_CADENCE45");   // (helper f45: a 45 FPS rate keeps the 30 FPS ladder, as 30 FPS does)
            // plan24 FIX-01: SMOOTH 60 is the build's own mode (VitaPlanDefaults::kLock60): a file that names neither MSC_LOCK60 nor a frame rate is LOCKED 60, which the plan gives no held picture
            const bool lock60Default = VitaPlanDefaults::kLock60 && !(l && *l) && (!f || !*f || !std::strcmp(f, "auto"));
            if ((r && *r) || KeyOn("MSC_LOCK60", l) || lock60Default || (f && (!std::strcmp(f, "30") || !std::strcmp(f, "45")))   /* (45: read as 30) */ || KeyOn("MSC_CADENCE45", c45) || (d && !KeyOn("MSC_DYNRES", d)) || (m && *m)) on = false;
        }
        if ((g.rule == kNeedsOn) != on) return &g;
    }
    return nullptr;
}

} // namespace np_picture

#endif // NP_PICTURE_PRESETS_H
