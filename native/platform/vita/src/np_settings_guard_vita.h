// Console glue of np_settings_guard.h (player build only).
#pragma once

namespace NpSettingsGuardVita {
void Run();                   // before the settings file is read: stamp, back up and neutralise an older build's file
const char* LoadingLine();    // the one line for the loading screen, or null
}  // namespace NpSettingsGuardVita
