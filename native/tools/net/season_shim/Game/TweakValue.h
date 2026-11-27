// season_clock_test: stands in for the game's tweak registry (only TweakIntBinding's constructor is used by NetworkSeasonCalendar.cpp)
#pragma once
struct TweakIntBinding {
    TweakIntBinding(const char*, const char*, int*, bool) {}
};
