// Console glue of np_change_guard.h (the start check for Picture and Language changes). The calls are safe from any thread; the player build calls Begin, the rest are inert
// until it has.
#pragma once

namespace NpChangeGuardVita {
bool Begin();                    // at start, before np-player.txt is read: true when this start uses the standard Picture and Language lines
bool Skips(const char* key);     // the loader must not apply this key
const char* LoadingLine();       // the notice for the loading screen (an English key of np_text_strings.tsv), or null
bool Cancel();                   // the core plan guard's Cancel/Restore/Failed/Heartbeat, mirrored (vita_plan.cpp)
void Restore();
void Failed();
void Stable();
void SettingsSaved();            // a settings screen saved np-player.txt, or RESET TO DEFAULTS: the failed-start count is cleared (F2)
}  // namespace NpChangeGuardVita
