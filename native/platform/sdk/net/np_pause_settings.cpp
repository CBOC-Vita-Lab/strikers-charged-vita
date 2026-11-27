#include "platform/sdk/net/np_pause_settings.h"

#include "platform/sdk/dwc/np_online_flow.h"
#include "platform/sdk/include/np/np_text.h"
#include "platform/sdk/net/np_lan.h"
#include "platform/sdk/net/np_native_settings.h"

#include "Game/FE/feFinder_impl.h"
#include "Game/FE/tlComponentInstance.h"
#include "Game/FE/tlTextInstance.h"

#include <cstdio>
#include <cstdlib>

namespace {

bool g_offered = false;   // for the pause menu that is open: set by the offer, read by the press (so a session that starts meanwhile cannot change what a press does)

// The localized label is stored as the game's UTF-16 text; it must outlive the pause menu's text instances.
unsigned short g_pauseLabel[32];

} // namespace

namespace {
// scene18: the screen is the game's own panel over the settings model (np_native_settings.h); the build has one when the model has a backend (the player build
// on a console; the host proof build with MSC_SETTINGS_FILE).
bool ModelHasScreen() {
    bool hasScreen = false;
    if (NpNativeSettings::NpNativeSettingsBackendRead && NpNativeSettings::NpNativeSettingsBackendWrite) {
        hasScreen = NpNativeSettings::Open();
        NpNativeSettings::Close();
    }
    return hasScreen;
}
bool g_optionsOffered = false;
}  // namespace

bool NpOptionsSettingsOffer() {
    g_optionsOffered = NpPauseSettings::DecideOptions(std::getenv("MSC_PAUSE_SETTINGS"), ModelHasScreen(), NpLanActive() || NpOnlineFlowActive());
    std::fprintf(stderr, "[options] VITA SETTINGS corner button %s\n", g_optionsOffered ? "offered" : "not offered");
    return g_optionsOffered;
}

bool NpOptionsSettingsPress() {
    if (!g_optionsOffered) return false;
    if (NpLanActive() || NpOnlineFlowActive()) {
        std::fprintf(stderr, "[options] VITA SETTINGS press refused: a multiplayer session exists\n");
        return false;
    }
    std::fprintf(stderr, "[options] VITA SETTINGS selected\n");
    return true;
}

bool NpPauseSettingsOffer(TLComponentInstance* button) {
    // Without a settings screen the button stays the game's controller map.
    const bool hasScreen = ModelHasScreen();
    g_offered = NpPauseSettings::Decide(std::getenv("MSC_PAUSE_SETTINGS"), hasScreen, NpLanActive() || NpOnlineFlowActive());
    if (!g_offered || !button) {
        g_offered = false;
        return false;
    }
    NpText::ToUtf16(NpText::Tr("VITA SETTINGS"), g_pauseLabel, 31);
    static const char* const kStates[3] = {"off", "over", "down"};
    for (const char* state : kStates) FEFinder<TLTextInstance, 3>::FindOrDefault(button, state, "option")->SetString(g_pauseLabel);
    std::fprintf(stderr, "[pause] VITA SETTINGS offered on the pause menu\n");
    return true;
}

bool NpPauseSettingsPress() {
    if (!g_offered) return false;
    // decided again at the press: a session that started after the menu was made (or an online screen being set up) takes the press away; the button stays the game's own
    if (NpLanActive() || NpOnlineFlowActive()) {
        std::fprintf(stderr, "[pause] VITA SETTINGS press refused: a multiplayer session exists\n");
        return false;
    }
    std::fprintf(stderr, "[pause] VITA SETTINGS selected\n");
    return true;   // the caller slides the pause panel out and pushes the VITA SETTINGS scene (SHVitaSettings.cpp)
}
