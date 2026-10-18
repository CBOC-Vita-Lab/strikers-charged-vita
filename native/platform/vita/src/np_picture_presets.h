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

namespace np_picture {

struct Setting {
    const char* name;
    const char* value;
};

// Performance: fewer pixels (640x362 is the screen's 16:9 at the game's own width: 81 % of the default's pixels; the
// picture goes up to 960x544 through the present blit as now) and the MSP "Performance Mode" (no crowd impostors, no
// decorative non-player characters: fewer draws and less game-thread work in the stadium).
static const Setting kPerformance[] = {
    {"MSC_VITA_RENDER", "640x362"},
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

inline const Setting* Find(const char* preset) {
    if (!preset || !*preset) return nullptr;
    if (!std::strcmp(preset, "performance") || !std::strcmp(preset, "perf") || !std::strcmp(preset, "fast")) return kPerformance;
    if (!std::strcmp(preset, "default") || !std::strcmp(preset, "normal")) return kDefault;
    if (!std::strcmp(preset, "quality") || !std::strcmp(preset, "high")) return kQuality;
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

} // namespace np_picture

#endif // NP_PICTURE_PRESETS_H
