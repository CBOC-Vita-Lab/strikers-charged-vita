// The real menu state machine with only its UI, keyboard and network boundaries replaced.
#include "platform/sdk/net/np_menu_flow.h"
#include "platform/sdk/net/np_keyboard.h"
#include "platform/sdk/net/online/np_http.h"
#include "platform/sdk/net/online/np_online_api.h"
#include "platform/sdk/include/np/np_sdk.h"
#include "platform/sdk/net/np_mpreport.h"
#include "platform/sdk/net/np_mpbeta.h"
#include "platform/sdk/net/np_mpdiag.h"
#include <sys/stat.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {
bool open = false, back = false, sceneReady = true;
int answer = -1, highlight = -1, networkCalls = 0, keyboardCalls = 0, leaves = 0;
const char* netError = "";
std::string shownText;
std::string buttons[3];
int failures = 0;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)
void Popup(const char* first) {
    for (int i = 0; i < 30 && !open; ++i) CHECK(NpMenuStep() == kNpMenuBusy);
    CHECK(open); CHECK(buttons[0] == first);
}
void Press(int choice) { answer = choice; CHECK(NpMenuStep() == kNpMenuBusy); }
void Back() { back = true; CHECK(NpMenuStep() == kNpMenuBusy); }
int Finish() {
    for (int i = 0; i < 30; ++i) { const int result = NpMenuStep(); if (result != kNpMenuBusy) return result; }
    CHECK(false); return -1;
}
bool g_beta = false;   // (mpbeta27) the beta cases ("beta-*") run with MSC_MP_BETA on; the 1.3.0 cases with MSC_MP_BETA=0 (the switch's off path is the old screens)
void Begin() { CHECK(NpMenuWanted() == 1); NpMenuBegin(); Popup("ONLINE"); }
void Local() { Begin(); Press(1); Popup("NEARBY"); }
bool FileThere(const char* p) { struct stat st; return stat(p, &st) == 0; }
bool Env(const char* key, const char* value) { const char* actual = std::getenv(key); return actual && !std::strcmp(actual, value); }
// A stored account (the format of np_menu_flow.cpp: online.dat v2), so that the flow reaches the transport.
void StoreAccount() {
    unsigned char b[4 + 4 + 4 + 64 + 4] = {'M', 'S', 'C', 'O', 2, 0, 0, 0, 7, 0, 0, 0};
    std::memset(b + 12, 'A', 43);
    unsigned crc = ~0u;
    for (size_t i = 0; i + 4 < sizeof b; ++i) {
        crc ^= b[i];
        for (int k = 0; k < 8; ++k) crc = (crc & 1) ? 0xEDB88320u ^ (crc >> 1) : crc >> 1;
    }
    crc = ~crc;
    for (int i = 0; i < 4; ++i) b[sizeof b - 4 + size_t(i)] = (unsigned char)(crc >> (8 * i));
    FILE* f = std::fopen("online.dat", "wb");
    CHECK(f != 0);
    if (f) { std::fwrite(b, 1, sizeof b, f); std::fclose(f); }
    f = std::fopen("online_name.txt", "wb");
    if (f) { std::fputs("Tester\n", f); std::fclose(f); }
}
}

extern "C" int NpMenuUiShow(const char* text, const char* a, const char* b, const char* c, int selected) {
    CHECK(!open); open = true; answer = -1; highlight = selected; shownText = text ? text : "";
    buttons[0] = a ? a : ""; buttons[1] = b ? b : ""; buttons[2] = c ? c : ""; return 1;
}
namespace NpMpReport { void UseNetwork() {} }   // (the production environment lives in np_mpreport_net.cpp; this test makes no request)
extern "C" void NpMenuUiSetText(const char*) {}
extern "C" int NpMenuUiPoll() { const int result = answer; answer = -1; if (result >= 0) open = false; return result; }
extern "C" int NpMenuUiOpen() { return open; }
extern "C" void NpMenuUiClose() { open = false; }
extern "C" int NpMenuUiBack() { const bool result = back; back = false; return result; }
extern "C" int NpMenuUiSceneReady(int) { return sceneReady; }
extern "C" int NpKeyboardOpen(const char*, const char*, int, int) { ++keyboardCalls; return 0; }
extern "C" int NpKeyboardPoll(char*, size_t) { return kNpKeyboardCancelled; }
extern "C" const char* NpKeyboardSuggestion() { return ""; }
extern "C" const char* NpLanPlayerName() { return "Test"; }
extern "C" unsigned NpLanFingerprint() { return 1; }
extern "C" void NpLanLinkRestored() {}
extern "C" int NpPlatformWifiUp() { return 1; }
extern "C" const NpSdkConfig* NpSdk_GetConfig() { return 0; }
extern "C" int np_https_clock_ok() { return 1; }
extern "C" uint64_t np_time_ns() { return 1000000; }
extern "C" void NpNetConfigureService(const char*, const char*, const char*, const char*) { ++networkCalls; }
extern "C" int NpNetHost(const char*) { ++networkCalls; return 0; }
extern "C" int NpNetJoin(const char*, const char*) { ++networkCalls; return 0; }
extern "C" void NpNetPoll() { ++networkCalls; }
extern "C" void NpNetLeave() { ++networkCalls; ++leaves; }
extern "C" int NpNetActive() { return 0; }
extern "C" int NpNetPhase() { return 0; }
extern "C" int NpNetNat() { return 0; }
extern "C" int NpNetRttMs() { return -1; }   // wave 11: the connection test reads the round trip to a partner (none in this test)
extern "C" const char* NpNetInviteCode() { return ""; }
extern "C" int NpNetMemberCount() { return 0; }
extern "C" const char* NpNetError() { return netError; }
extern "C" const char* NpNetEndReason() { return ""; }
extern "C" int NpNetReconnect() { ++networkCalls; return 0; }
namespace NpNet {
uint64_t SteadyNowMs(void*) { return 1; }
int HttpStart(const char*, const std::string&, const std::string&, const std::vector<std::string>&, uint32_t, std::string*) { ++networkCalls; return 0; }
int HttpPoll(int, HttpResponse*) { ++networkCalls; return -1; }
void HttpCancel(int) { ++networkCalls; }
}

int main(int argc, char** argv) {
    CHECK(argc == 2);
    unsetenv("MSC_LAN_MODE"); unsetenv("MSC_NET_MODE"); unsetenv("MSC_LAN_DIALOG");
    setenv("NP_DATA_DIR", ".", 1);
    const std::string which = argc == 2 ? argv[1] : "";
    g_beta = which.compare(0, 5, "beta-") == 0;
    if (g_beta) { unsetenv("MSC_MP_BETA"); setenv("MSC_MP_BETA_NOTICE", "0", 1); } else setenv("MSC_MP_BETA", "0", 1);
    unsetenv("MSC_INPUT_SCRIPT");
    remove("mp-beta-seen.txt"); remove("mp-session.txt"); remove("mp-report.txt"); remove("online.dat"); remove("online_name.txt");
    if (which == "beta-notice") {
        // the first visit: the notice with OK, then the mode choice with clean labels and REPORT A PROBLEM; the second visit: no notice
        unsetenv("MSC_MP_BETA_NOTICE");
        NpMenuBegin(); Popup("OK"); CHECK(shownText == NpMpBeta::NoticeText()); CHECK(buttons[1].empty());
        CHECK(FileThere("mp-beta-seen.txt"));
        Press(0); Popup("ONLINE"); CHECK(buttons[1] == "LOCAL"); CHECK(buttons[2] == "REPORT A PROBLEM");
        Press(1); Popup("NEARBY"); CHECK(buttons[1] == "SAME WI-FI"); CHECK(buttons[2] == "BACK");
        Back(); Popup("ONLINE"); Back(); CHECK(Finish() == kNpMenuDone);
        NpMenuBegin(); Popup("ONLINE"); Back(); CHECK(Finish() == kNpMenuDone);   // seen: never again
        setenv("MSC_MP_BETA_NOTICE", "always", 1);
        NpMenuBegin(); Popup("OK"); Press(0); Popup("ONLINE"); Back(); CHECK(Finish() == kNpMenuDone);
        CHECK(NpMpBeta::Count().notice == 2 && NpMpBeta::Count().reportEntry >= 4);
    } else if (which == "beta-scripted") {
        // a scripted run (harness, pair rig, kit route) walks straight into the mode choice unless it asks for the notice
        unsetenv("MSC_MP_BETA_NOTICE"); setenv("MSC_INPUT_SCRIPT", "route.txt", 1);
        Begin(); Back(); CHECK(Finish() == kNpMenuDone); CHECK(!FileThere("mp-beta-seen.txt"));
    } else if (which == "beta-online") {
        setenv("MSC_ONLINE_FRIENDS", "1", 1);
        Begin(); Press(0); Popup("FRIENDS"); CHECK(buttons[1] == "PLAY WITH ANYONE");
        Press(1); CHECK(Finish() == kNpMenuQuick);
        CHECK(NpMpDiag::CurrentTrail().link == NpMpDiag::kLinkOnline && NpMpDiag::CurrentTrail().play == NpMpDiag::kPlayQuick);
        unsetenv("MSC_ONLINE_FRIENDS");
        Begin(); Press(0); Popup("HOST A ROOM"); Press(0);   // the host's account step: the trail says so (no network: no account, no address)
        Popup("OK");
        CHECK(NpMpDiag::CurrentTrail().play == NpMpDiag::kPlayRoomHost && NpMpDiag::CurrentTrail().furthest >= NpMpDiag::kStageLogin);
        Press(0); CHECK(Finish() == kNpMenuDone);
    } else if (which == "beta-report-noaccount") {
        // REPORT A PROBLEM without an online account: the report is saved to a file and the player is told where; nothing is sent
        Begin(); Press(2); Popup("OK"); CHECK(shownText == NpMpBeta::SavedInstead()); CHECK(FileThere("mp-report.txt"));
        Press(0); Popup("ONLINE"); Back(); CHECK(Finish() == kNpMenuDone);
    } else if (which == "beta-report-account") {
        // with an account: the consent popup (CANCEL lit first); CANCEL sends nothing and goes back to the mode choice
        NpMpReport::Env e;
        e.account = [](NpMpReport::Account* a) { std::snprintf(a->name, sizeof a->name, "Tester"); std::snprintf(a->token, sizeof a->token, "t"); std::snprintf(a->urls, sizeof a->urls, "https://service.invalid"); return true; };
        NpMpReport::SetEnv(e);   // (UseNetwork is a no-op in this test: the account comes from here; start is null, so nothing could be sent)
        Begin(); Press(2); Popup("SEND"); CHECK(buttons[1] == "CANCEL"); CHECK(highlight == 1); CHECK(shownText == NpMpReport::ConsentText());
        Press(1); Popup("ONLINE"); Back(); CHECK(Finish() == kNpMenuDone);
    } else if (which == "beta-offer-crash") {
        // the last run closed inside a session (the marker is still there): the offer comes once, with its reason, before the mode choice
        FILE* f = std::fopen("mp-session.txt", "wb"); CHECK(f != 0); if (f) { std::fputs("lobby\n", f); std::fclose(f); }
        NpMenuBegin(); Popup("SEND A REPORT"); CHECK(buttons[1] == "NOT NOW");
        CHECK(shownText.compare(0, std::strlen(NpMpBeta::OfferLead(true, false)), NpMpBeta::OfferLead(true, false)) == 0);
        CHECK(!FileThere("mp-session.txt"));
        Press(1); Popup("ONLINE"); Back(); CHECK(Finish() == kNpMenuDone);
        NpMenuBegin(); Popup("ONLINE"); Back(); CHECK(Finish() == kNpMenuDone);   // once
    } else if (which == "beta-off-entry") {
        setenv("MSC_MP_REPORT_ENTRY", "0", 1);
        Begin(); CHECK(buttons[2] == "CANCEL"); Press(2); CHECK(Finish() == kNpMenuDone);
        unsetenv("MSC_MP_REPORT_ENTRY");
    } else if (which == "discoverable") {
        Begin(); CHECK(buttons[1] == "LOCAL"); CHECK(buttons[2] == "CANCEL");
        Back(); CHECK(Finish() == kNpMenuDone); CHECK(!NpMenuActive());
        CHECK(std::getenv("MSC_LAN_MODE") == 0);
    } else if (which == "cancel-button") {
        Begin(); Press(2); CHECK(Finish() == kNpMenuDone); CHECK(!NpMenuActive());
    } else if (which == "online-back") {
        Begin(); Press(0); Popup("HOST A ROOM"); CHECK(buttons[1] == "JOIN WITH A CODE");
        Back(); CHECK(Finish() == kNpMenuDone); CHECK(std::getenv("MSC_LAN_MODE") == 0);
    } else if (which == "local-back") {
        Local(); Press(2); Popup("ONLINE"); Back(); CHECK(Finish() == kNpMenuDone);
    } else if (which == "wifi") {
        setenv("MSC_LAN_MODE", "online", 1); Local(); Press(1);
        CHECK(NpMenuLinkMode() == kNpMenuOnline);
        CHECK(Finish() == kNpMenuLocal); CHECK(!NpMenuActive()); CHECK(NpMenuLinkMode() == kNpMenuWifi);
        CHECK(Env("MSC_LAN_MODE", "online"));
    } else if (which == "nearby-host" || which == "nearby-join" || which == "nearby-auto") {
        const int role = which == "nearby-host" ? 0 : which == "nearby-join" ? 1 : 2;
        setenv("MSC_LAN_DIALOG", role == 0 ? "create" : role == 1 ? "join" : "conn", 1);
        setenv("MSC_LAN_MODE", "online", 1); Local(); Press(0);
        CHECK(Finish() == kNpMenuLocal); CHECK(NpMenuLinkMode() == kNpMenuNearby);
        CHECK(Env("MSC_LAN_MODE", "online"));
        CHECK(Env("MSC_LAN_DIALOG", role == 0 ? "create" : role == 1 ? "join" : "conn"));
    } else if (which == "repeat") {
        Local(); Press(1); CHECK(Finish() == kNpMenuLocal);
        Begin(); Press(0); Popup("HOST A ROOM"); Back(); CHECK(Finish() == kNpMenuDone);
        CHECK(std::getenv("MSC_LAN_MODE") == 0); CHECK(NpMenuLinkMode() == kNpMenuWifi);
    } else if (which == "online-cancel") {
        Begin(); Press(0); Popup("HOST A ROOM"); Press(2); CHECK(Finish() == kNpMenuDone);
        CHECK(NpMenuLinkMode() == kNpMenuNearby);
    } else if (which == "online-failure") {
        unsetenv("MSC_ONLINE_URL");
        setenv("MSC_LAN_MODE", "wifi", 1); Begin(); Press(0); Popup("HOST A ROOM"); Press(0);
        Popup("OK"); Press(0); CHECK(Finish() == kNpMenuDone); CHECK(NpMenuLinkMode() == kNpMenuWifi);
    } else if (which == "start-failure") {
        // the transport refuses to start (the ad hoc radio is still closing, Wi-Fi is off): its sentence is shown and the controller is gone,
        // otherwise it keeps answering "failed" to the radio poll of a later Nearby / Same Wi-Fi start
        StoreAccount();
        setenv("MSC_ONLINE_URL", "https://service.invalid", 1);
        netError = "The ad hoc radio is in use. Leave the ad hoc game first, then try again.";
        Begin(); Press(0); Popup("HOST A ROOM"); Press(0); Popup("OK");
        CHECK(shownText == netError); CHECK(leaves == 1);
        Press(0); CHECK(Finish() == kNpMenuDone); CHECK(!NpMenuActive());
        networkCalls = 0;
    } else if (which == "radio-failure") {
        NpMenuShowFailure("Same Wi-Fi needs a Wi-Fi connection."); CHECK(NpMenuActive());
        Popup("OK"); CHECK(shownText == "Same Wi-Fi needs a Wi-Fi connection.");
        Press(0); CHECK(Finish() == kNpMenuDone); CHECK(!NpMenuActive());
    } else if (which == "shortcuts") {
        setenv("MSC_NET_MODE", "host", 1); CHECK(NpMenuWanted() == 0);
        setenv("MSC_NET_MODE", "join", 1); CHECK(NpMenuWanted() == 0);
    } else if (which == "scene-wait") {
        sceneReady = false; NpMenuBegin(); CHECK(NpMenuStep() == kNpMenuBusy); CHECK(!open);
        sceneReady = true; Popup("ONLINE"); Back(); CHECK(Finish() == kNpMenuDone);
    } else if (which == "popup-gone") {
        Local(); answer = -2; open = false; CHECK(NpMenuStep() == kNpMenuBusy); CHECK(Finish() == kNpMenuDone);
    } else if (which == "saved-preference") {
        setenv("MSC_LAN_MODE", "wifi", 1); Begin(); CHECK(highlight == 1); Press(1); Popup("NEARBY"); CHECK(highlight == 1);
        Back(); Popup("ONLINE"); Back(); CHECK(Finish() == kNpMenuDone);
    } else CHECK(false);
    CHECK(networkCalls == 0); CHECK(keyboardCalls == 0);
    if (!failures) std::printf("PASS %s\n", which.c_str());
    return failures ? 1 : 0;
}
