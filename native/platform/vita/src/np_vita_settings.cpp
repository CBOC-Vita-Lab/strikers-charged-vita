// set18: the player's settings are in the game's own menu (OPTIONS > VITA SETTINGS and the pause menu's VITA SETTINGS: platform/sdk/net/np_native_settings.cpp, the backend at the end of
// this file). This file keeps the pixel screen (np_settings.cpp) for two things only:
//   - SAFE START: L + R held while the game starts (the fatal screen says so). The way out when a setting stops the game from starting: CORE PLAN, RESET TO DEFAULTS, the facts a
//     report needs, then SAVE AND START (the game starts again with "-play") or SAVE AND EXIT. It is not a settings menu: the other rows are not on it.
//   - ONLINE: the game menu's ONLINE ACCOUNT row opens it over the paused game (RequestOpen -> Pump): the online name, SEND A REPORT, SAVE A REPORT, DELETE MY ONLINE ACCOUNT. Not in a
//     LAN/online session.
// The LiveArea's Settings tile is gone (1.3, template.xml content-rev 5). A console that still has the old tile starts the game with the parameter "-settings": the game starts
// normally (a log line says so) and a press of the old tile while the game runs is logged and dropped. The player finds the settings in the game.
//
// Test hooks (Vita3K has no one to press keys): a file ux0:data/mscharged/np-settings-open opens the start-up screen; its words, if any, are the keys pressed one after another (up down
// left right cross circle triangle ltrigger rtrigger, "shot:NAME" writes shots/settings-NAME.ppm, "wait:MS"); the file is deleted when the screen opens so that a restart does not open
// it again. The game that "Save and start" starts again is told by the argument -play.
#if defined(__vita__) && defined(NP_PLAYER)

#include "np_vita_settings.h"

#include "np_brand_art.h"
#include "np_build_identity.h"
#include "np_settings_guard.h"
#include "np_settings.h"
#include "../../../include/np/live_settings.h"

extern "C" int VgxQuiesceForExit(unsigned ms) __attribute__((weak));   // renderer/src/vgx_play.cpp
extern "C" void VgxQuiesceCancel(void) __attribute__((weak));
#include "np_vita_internal.h"
#include "../../sdk/dwc/np_online_name.h"
#include "../../sdk/save/np_safe_file.h"
#include "../../sdk/net/np_mpdiag.h"
#include "../../sdk/net/np_mpreport.h"

#include <psp2/appmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <cctype>

extern "C" bool np_vita_radio_dialog_active(void) __attribute__((weak));
extern "C" const char* NpGameRootPath(void);
namespace NpChangeGuardVita { void SettingsSaved() __attribute__((weak)); }   // np_change_guard_vita.cpp (r16 F2): a saved screen clears the start check's failed-start count
bool NpLanActive();   // platform/sdk/net/np_lan.cpp: a multiplayer session (any phase) exists
bool NpOnlineFlowActive();   // platform/sdk/dwc/np_online_flow.cpp: between selecting the online entry and leaving its screens

namespace NpSettingsScreen {
namespace {

constexpr const char* kSettingsFile = "ux0:/data/mscharged/np-player.txt";
constexpr const char* kSettingsBackup = "ux0:/data/mscharged/np-player.txt.bak";
constexpr const char* kOpenMarker = "ux0:/data/mscharged/np-settings-open";
constexpr const char* kMenuScript = "ux0:/data/mscharged/np-settings-menu-script";

bool ReadFile(const char* path, std::string& out) {
    const SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return false;
    out.clear();
    char buffer[2048];
    for (;;) {
        const int n = sceIoRead(fd, buffer, sizeof(buffer));
        if (n <= 0) break;
        out.append(buffer, static_cast<size_t>(n));
        if (out.size() > 64 * 1024) break;
    }
    sceIoClose(fd);
    return true;
}

bool Exists(const char* path);

// A settings file that holds a run of zero bytes is a torn one (a rename that survived a power cut without its data). Text files edited on a PC (UTF-16 with a BOM)
// have single zero bytes and are not torn. An empty file is a valid (all defaults) file.
bool SettingsTextSound(const std::string& text) {
    size_t run = 0;
    for (char c : text) {
        run = c == 0 ? run + 1 : 0;
        if (run >= 8) return false;
    }
    return true;
}

NpSafeFile::Options SettingsOptions() {
    NpSafeFile::Options o;
    o.valid = SettingsTextSound;
    o.keepBackup = false;
    o.mode = 0666;
    o.maxBytes = 256 * 1024;
    return o;
}

void NoteRecovery(const char* path, NpSafeFile::Found f) {
    if (f == NpSafeFile::Found::Nothing || f == NpSafeFile::Found::Primary) return;
    std::fprintf(stderr, "[settings] %s: %s\n", path, NpSafeFile::FoundName(f));
}

// The settings file: written complete and synced under a temporary name, then put in place so that a power cut leaves the old file or the new one (never half of
// it, never none): np_safe_file.h (the cut test is native/platform/sdk/tests/test_safe_file.cpp). A damaged file found at start-up is set aside as
// np-player.txt.bad, not loaded and not overwritten.
bool WriteFile(const char* path, const std::string& text) {
    return NpSafeFile::Replace(path, text, SettingsOptions());
}

// Not the player's data: a screenshot of the test hook, written as it always was.
bool WritePlain(const char* path, const std::string& text) {
    const SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) return false;
    bool ok = text.empty() || sceIoWrite(fd, text.data(), static_cast<SceSize>(text.size())) == static_cast<int>(text.size());
    if (sceIoClose(fd) < 0) ok = false;
    return ok;
}

bool Exists(const char* path) {
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}

uint64_t NowMs() { return sceKernelGetProcessTimeWide() / 1000u; }

bool IsSettingsParameter(const char* param) {
    const char* p = std::strstr(param, "-settings");
    if (!p) return false;
    auto boundary = [](char c) { return !c || std::isspace(static_cast<unsigned char>(c)) || c == ':' || c == '=' || c == '&' || c == '"'; };
    return (p == param || boundary(p[-1])) && boundary(p[9]);
}

// Whether the parameter says -settings now. A read may or may not consume it (OpenRequest, np_settings.h), so what follows from it is decided there.
bool ReadSettingsParameter() {
    char param[1024] = {};
    const int rc = sceAppMgrGetAppParam(param);
    if (rc < 0 || !std::memchr(param, 0, sizeof(param))) return false;
    return IsSettingsParameter(param);
}

NpSettings::OpenRequest g_openRequest;
volatile bool g_refusedRestart = false;   // Pump: the system refused a restart; the screen that opens next says so (Run)
volatile bool g_menuRequest = false;   // RequestOpen: set by the pause menu, taken by Pump

bool HasArgument(int argc, char** argv, const char* word) {
    // from argv[0] on: the arguments of sceAppMgrLoadExec become the new process's argv as they are (Vita3K logs "argument at 0: -play"), so "-play" may be argv[0]
    for (int i = 0; i < argc; ++i)
        if (argv && argv[i] && std::strstr(argv[i], word)) return true;
    return false;
}

// What the screen reads as button presses.
enum Key { kKeyNone, kKeyUp, kKeyDown, kKeyLeft, kKeyRight, kKeyCross, kKeyCircle, kKeyTriangle, kKeyPagePrevious, kKeyPageNext };

struct Scripted {
    std::vector<std::string> words;
    size_t next = 0;
    uint64_t readyAt = 0;
};

void Shot(const char* name, const uint32_t* pixels) {
    sceIoMkdir("ux0:/data/mscharged/shots", 0777);
    char path[160];
    std::snprintf(path, sizeof(path), "ux0:/data/mscharged/shots/settings-%s.ppm", name);
    std::string ppm = "P6\n960 544\n255\n";
    ppm.reserve(ppm.size() + 960 * 544 * 3);
    for (int i = 0; i < 960 * 544; ++i) {
        const uint32_t p = pixels[i];
        ppm.push_back(static_cast<char>(p));
        ppm.push_back(static_cast<char>(p >> 8));
        ppm.push_back(static_cast<char>(p >> 16));
    }
    WritePlain(path, ppm);
    std::fprintf(stderr, "[settings] screenshot %s\n", path);
}

} // namespace

bool WriteSettingsText(const char* path, const std::string& text) { return WriteFile(path, text); }

const char* SavedFilePath() {
    NoteRecovery(kSettingsFile, NpSafeFile::Recover(kSettingsFile, SettingsOptions()));
    if (!Exists(kSettingsFile) && Exists(kSettingsBackup)) return kSettingsBackup;   // (Recover could not put it back: read it where it is)
    return kSettingsFile;
}

bool Requested(int argc, char** argv) {
    // what the system passed (a LiveArea start has no arguments; the relaunch of "Save and start" has "-play"), for the log of a console test
    if (argc > 0 && argv)
        std::fprintf(stderr, "[settings] start arguments: argc=%d%s%.60s%s%.60s\n", argc, argv[0] ? " argv[0]=" : "", argv[0] ? argv[0] : "",
                     argc > 1 && argv[1] ? " argv[1]=" : "", argc > 1 && argv[1] ? argv[1] : "");
    // What this start does with the parameter is settled here, whatever the path below: the running game's polls (Pump) only act on one that appears later. That also
    // covers an old -settings still readable on an explicit restart, and the screen of this start when the relaunch of "Save and start" fails.
    // 1.3: the test hook of the in-game entry is for test runs only: a file left on a player's card would press keys by itself the next time the screen opens over a game, so it is removed at
    // every start unless this is a fast-boot test run (MSC_BOOT_MATCH, or the route's pad script MSC_INPUT_SCRIPT: both come from env.txt, which is read before main).
    if (Exists(kMenuScript) && !std::getenv("MSC_BOOT_MATCH") && !std::getenv("MSC_INPUT_SCRIPT")) {
        sceIoRemove(kMenuScript);
        std::fprintf(stderr, "[settings] removed a left-over np-settings-menu-script (test hook, not a test run)\n");
    }
    const bool parameter = ReadSettingsParameter();
    g_openRequest.Seed(parameter);
    if (HasArgument(argc, argv, "-play")) return false;   // started again by "Save and start"
    if (Exists(kOpenMarker)) return true;
    // set18: "-settings" is the old LiveArea tile (gone since 1.3's template.xml content-rev 5; a console keeps the tile until the bubble is refreshed). It no longer opens a settings
    // screen: the game starts as from the bubble, and the settings are in its own menu.
    if (parameter || HasArgument(argc, argv, "-settings"))
        VitaLog::Note("Started from the old LiveArea Settings tile: the game starts normally; the settings are in the game (OPTIONS or pause, VITA SETTINGS)");
    // SAFE START: L + R held while the game starts (the recovery path the fatal screen names): 10 samples 16 ms apart (160 ms; it was 4 = 64 ms), so that a player who taps both just
    // after the logo is still caught. A start with nothing held pays the whole window (+96 ms once per start); one with both held leaves at the first sample. The controller needs a
    // moment after the start, which the first samples may miss.
    for (int i = 0; i < 10; ++i) {
        VitaInput::Raw raw;
        if (VitaInput::Read(raw) && (raw.buttons & (VitaInput::kLTrigger | VitaInput::kRTrigger)) == (VitaInput::kLTrigger | VitaInput::kRTrigger)) return true;
        sceKernelDelayThread(16 * 1000);
    }
    return false;
}

// The system refused the restart the player asked for: the screen opens again at the next poll with a red line that says so (the log line alone would show the player nothing).
void RestartRefused() {
    VitaLog::Note("restart refused by the system: the settings are saved and apply at the next start");
    g_refusedRestart = true;
    g_openRequest.Request();
    g_menuRequest = true;   // (no polling delay)
}

void RequestOpen() {
    // decided again at the press (the pause menu decided when it was made): not in a LAN/online session, nor while one is being set up
    if (NpLanActive() || NpOnlineFlowActive()) {
        VitaLog::Note("Settings not opened: a multiplayer session is running or being set up");
        return;
    }
    g_menuRequest = true;
}

namespace { std::atomic<bool> g_screenOpen{false}; }
bool IsOpen() { return g_screenOpen.load(std::memory_order_relaxed); }

Result Run(bool running) {
    uint32_t* screen = VitaConsole::TakeScreen();
    if (!screen) return running ? Result::kNotShown : Result::kStart;
    // galaxyport18: the freeze button (np_freeze_button.h) does not count while this screen owns the pad.
    struct OpenFlag { OpenFlag() { g_screenOpen.store(true, std::memory_order_relaxed); } ~OpenFlag() { g_screenOpen.store(false, std::memory_order_relaxed); } } openFlag;
    // The player's own game art behind the screen and in its headings, read from the disc files now (about 2.5 MiB, freed when the screen closes);
    // MSC_SETTINGS_ART=0 or missing files: the plain screen. Nothing of it is in the package.
    struct ArtScope {
        NpBrandArt::Art art;
        NpSettings::Art screen;
        ArtScope() {
            const char* off = std::getenv("MSC_SETTINGS_ART");
            if (off && off[0] == '0') return;
            const char* root = NpGameRootPath();
            const uint64_t t0 = NowMs();
            if (!root || !art.Load(root)) {
                std::fprintf(stderr, "[settings] game art: not available (plain screen)\n");
                return;
            }
            screen.backdrop = art.Backdrop();
            if (art.HasFont())
                screen.heading = [this](uint32_t* p, int x, int y, int h, const char* t, bool draw) { return art.Heading(p, x, y, h, t, draw); };
            NpSettings::SetArt(&screen);
            std::fprintf(stderr, "[settings] game art loaded in %u ms%s\n", static_cast<unsigned>(NowMs() - t0), art.HasFont() ? "" : " (no font)");
        }
        ~ArtScope() { NpSettings::SetArt(nullptr); }
    } artScope;
    std::string text;
    const bool had = ReadFile(SavedFilePath(), text);
    NpSettings::State state;
    NpSettings::Load(state, had ? text.c_str() : nullptr);
    {   // the facts of the SAFE START and ONLINE pages that only this file knows (the version and build are the build's own)
        auto info = [](const char* title, const char* value) {
            for (int i = 0; i < NpSettings::InfoCount(); ++i)
                if (std::strcmp(NpSettings::InfoTitle(i), title) == 0) NpSettings::SetInfo(i, value);
        };
        char name[NpOnline::kMaxNameChars + 1] = "";
        if (NpOnline::LoadStoredName(name)) info("ONLINE NAME", name);
        const NpSettingsGuard::Version stamp = NpSettingsGuard::ReadStamp(text);
        char written[32] = "NO STAMP";
        if (stamp.valid) std::snprintf(written, sizeof written, "WRITTEN BY %d.%d.%d", stamp.major, stamp.minor, stamp.patch);
        info("SETTINGS FILE", had ? written : "NONE YET");
        char sys[200];
        std::snprintf(sys, sizeof sys, "%s/sys/fst.bin", NpGameRootPath());
        info("GAME FILES", Exists(sys) ? "FOUND" : "MISSING");
    }
    std::fprintf(stderr, "[settings] screen open (%s, %u bytes)\n", had ? "np-player.txt read" : "no np-player.txt yet", static_cast<unsigned>(text.size()));

    Scripted script;
    // 1.3 test hook for the in-game entry (the pause menu's VITA SETTINGS): a file ux0:data/mscharged/np-settings-menu-script holds the same words as the np-settings-open marker; it is used (and
    // deleted) when the screen opens over a running game. (An environment variable did not reach this code in the emulator.) A player's card never has it.
    const bool menuScript = running && Exists(kMenuScript);
    if (Exists(kOpenMarker) || menuScript) {
        std::string words;
        if (Exists(kOpenMarker)) {
            ReadFile(kOpenMarker, words);
            sceIoRemove(kOpenMarker);
        } else {
            ReadFile(kMenuScript, words);
            sceIoRemove(kMenuScript);
        }
        for (size_t at = 0; at < words.size();) {
            while (at < words.size() && (words[at] == ' ' || words[at] == '\n' || words[at] == '\r' || words[at] == '\t')) ++at;
            size_t end = at;
            while (end < words.size() && words[end] != ' ' && words[end] != '\n' && words[end] != '\r' && words[end] != '\t') ++end;
            if (end > at) script.words.push_back(words.substr(at, end - at));
            at = end;
        }
        std::fprintf(stderr, "[settings] test script: %u word(s)\n", static_cast<unsigned>(script.words.size()));
    }

    // set18: the start-up screen is SAFE START (page 0); the running game's is the ONLINE page (the game menu's ONLINE ACCOUNT row)
    int page = 0, selected = 0;
    if (running)
        for (int p = 0; p < NpSettings::PageCount(); ++p)
            if (std::strcmp(NpSettings::PageName(p), "ONLINE") == 0) page = p;
    bool controlsPage = false;   // W5 CTL: Triangle shows every button of the chosen layout
    bool leaving = false;        // 1.3: Circle with unsaved changes
    int leaveIds[NpSettings::kMaxLeave] = {};   // the dialog's choices (NpSettings::LeaveDialog)
    int leaveCount = 0, leaveIndex = 0;
    // W22: SEND A REPORT / DELETE MY ONLINE ACCOUNT. The dialog is on screen from the press until the answer; NpMpReport::Op holds the rule (nothing is sent before SEND / YES).
    NpMpReport::Op report;
    bool reportDialog = false;   // the consent / confirmation or the waiting screen is up
    int reportChoice = 0;        // 0 CANCEL / NO (lit first), 1 SEND / YES
    std::string message;
    bool messageIsError = false;
    uint64_t messageUntil = 0;
    if (running && g_refusedRestart) {   // the system refused the restart that was asked for: say so on the screen that comes back (Pump)
        g_refusedRestart = false;
        message = "The system refused the restart. Your changes are saved: they apply next start.";
        messageIsError = true;
        messageUntil = NowMs() + 8000;
    }
    bool dirty = true;
    uint32_t previous = 0xFFFFFFFFu;   // buttons of the last sample (a press is a change to down)
    uint64_t repeatAt = 0;
    Result result = running ? Result::kReturn : Result::kExit;
    bool done = false;

    auto save = [&]() {
        std::string out;
        if (!NpSettings::Serialize(state, out)) {
            message = "np-player.txt would be too large";
            messageIsError = true;
            return false;
        }
        out = NpSettingsGuard::Stamped(out, NpBuildIdentity::Version(), NpBuildIdentity::BuildId());   // 1.3: the file says which build wrote it (the stale-settings guard reads it)
        if (!WriteFile(kSettingsFile, out)) {
            message = "Could not write np-player.txt";
            messageIsError = true;
            return false;
        }
        std::fprintf(stderr, "[settings] np-player.txt written (%u bytes)\n", static_cast<unsigned>(out.size()));
        if (NpChangeGuardVita::SettingsSaved) NpChangeGuardVita::SettingsSaved();
        if (running) {
            // 1.3: the live rows (STADIUM CROWD, REDUCED CAMERA SHAKE, REAR TOUCH) take effect now, by their shared cells (np/live_settings.h): no environment read or write.
            const bool nextStart = NpSettings::RestartNeeded(state);
            const int applied = NpSettings::ApplyLive(state, [](const char* key, const char* value) {
                if (NpLiveSettingsSet(key, value)) std::fprintf(stderr, "[settings] live: %s=%s\n", key, value ? value : "(unset)");
            });
            std::fprintf(stderr, "[settings] %d live row(s) applied, %s\n", applied, nextStart ? "other rows apply next start" : "nothing waits for the next start");
            message = nextStart ? "Saved. It applies the next time the game starts." : "Saved. It applies now.";
        } else {
            message = "Saved to ux0:data/mscharged/np-player.txt";
        }
        messageIsError = false;
        return true;
    };
    auto draw = [&]() {
        if (controlsPage) NpSettings::DrawControls(screen, state);
        else NpSettings::Draw(screen, state, page, selected, message.c_str(), messageIsError, running);
        const bool classic = NpSettings::ClassicLayout(state);
        if (reportDialog) {
            const bool deleting = report.GetKind() == NpMpReport::Op::kDelete;
            NpSettings::DrawReportDialog(screen, report.GetPhase() == NpMpReport::Op::kSending ? (deleting ? NpSettings::kReportDeleting : NpSettings::kReportWorking)
                                                                                             : (deleting ? NpSettings::kReportDelete : NpSettings::kReportSend), reportChoice, classic);
        } else if (leaving) {
            if (running) NpSettings::DrawLeaveDialog(screen, leaveIds, leaveCount, leaveIndex, running, classic, NpSettings::NextStartChanged(state));
            else NpSettings::DrawLeaveConfirmation(screen, leaveIds[leaveIndex], classic);
        }
    };

    while (!done) {
        const uint64_t now = NowMs();
        Key key = kKeyNone;
        // scripted keys first (one word per step), then the pad
        if (script.next < script.words.size()) {
            if (now >= script.readyAt) {
                const std::string& w = script.words[script.next++];
                script.readyAt = now + 250;
                if (w == "up") key = kKeyUp;
                else if (w == "down") key = kKeyDown;
                else if (w == "left") key = kKeyLeft;
                else if (w == "right") key = kKeyRight;
                else if (w == "cross") key = kKeyCross;
                else if (w == "circle") key = kKeyCircle;
                else if (w == "triangle") key = kKeyTriangle;
                else if (w == "square") key = kKeyCircle;
                else if (w == "ltrigger") key = kKeyPagePrevious;
                else if (w == "rtrigger") key = kKeyPageNext;
                else if (w.compare(0, 5, "shot:") == 0) {
                    draw();
                    Shot(w.c_str() + 5, screen);
                } else if (w.compare(0, 5, "wait:") == 0) script.readyAt = now + static_cast<uint64_t>(std::atoi(w.c_str() + 5));
            }
        } else {
            VitaInput::Raw raw;
            if (VitaInput::Read(raw)) {
                uint32_t b = raw.buttons;
                if (raw.ly < 48) b |= VitaInput::kUp;
                if (raw.ly > 208) b |= VitaInput::kDown;
                if (raw.lx < 48) b |= VitaInput::kLeft;
                if (raw.lx > 208) b |= VitaInput::kRight;
                const uint32_t moves = VitaInput::kUp | VitaInput::kDown | VitaInput::kLeft | VitaInput::kRight;
                const uint32_t pressed = b & ~previous;
                uint32_t pick = pressed;
                // a held direction repeats: after 350 ms, then every 90 ms
                if (pressed & moves) repeatAt = now + 350;
                else if ((b & moves) && now >= repeatAt) {
                    pick |= b & moves;
                    repeatAt = now + 90;
                }
                if (previous == 0xFFFFFFFFu) pick = 0;   // the first sample only sets what is held (L + R, or Cross of the LiveArea tap)
                previous = b;
                if (pick & VitaInput::kUp) key = kKeyUp;
                else if (pick & VitaInput::kDown) key = kKeyDown;
                else if (pick & VitaInput::kLeft) key = kKeyLeft;
                else if (pick & VitaInput::kRight) key = kKeyRight;
                else if (pick & VitaInput::kCross) key = kKeyCross;
                else if (pick & (VitaInput::kCircle | VitaInput::kSquare)) key = kKeyCircle;   // (Square is the game's own back button in the CLASSIC layout; it has no other use here)
                else if (pick & VitaInput::kTriangle) key = kKeyTriangle;
                else if (pick & VitaInput::kLTrigger) key = kKeyPagePrevious;
                else if (pick & VitaInput::kRTrigger) key = kKeyPageNext;
            }
        }

        if (reportDialog) {   // the report dialogs own the keys until they close
            if (report.GetPhase() == NpMpReport::Op::kSending) {
                report.Step();
                dirty = true;
                if (key == kKeyCircle) {   // Circle: stop waiting (a request in flight is cancelled; a delete that was not answered keeps the identity)
                    report.Cancel();
                    reportDialog = false;
                    message = "Nothing was sent.";
                    messageIsError = false;
                    messageUntil = now + 4000;
                } else if (report.GetPhase() == NpMpReport::Op::kDone) {
                    reportDialog = false;
                    message = report.Sentence();
                    messageIsError = !report.Succeeded();
                    messageUntil = now + 8000;
                    std::fprintf(stderr, "[settings] %s: outcome %d\n", report.GetKind() == NpMpReport::Op::kDelete ? "delete account" : "send a report", int(report.GetOutcome()));
                }
            } else if (key != kKeyNone) {
                dirty = true;
                if (key == kKeyCircle) {
                    report.Cancel();
                    reportDialog = false;
                } else if (key == kKeyLeft || key == kKeyUp) reportChoice = 0;
                else if (key == kKeyRight || key == kKeyDown) reportChoice = 1;
                else if (key == kKeyCross) {
                    if (reportChoice == 0) {
                        report.Cancel();
                        reportDialog = false;
                    } else {
                        report.Consent();
                        if (!report.Start()) reportDialog = false;
                    }
                }
            }
            key = kKeyNone;
        } else if (key != kKeyNone && leaving) {
            dirty = true;
            if (key == kKeyCircle) leaving = false;   // Circle is always KEEP EDITING: nothing is lost
            else if (key == kKeyLeft || key == kKeyUp) leaveIndex = NpSettings::LeaveMove(leaveIndex, leaveCount, -1);
            else if (key == kKeyRight || key == kKeyDown) leaveIndex = NpSettings::LeaveMove(leaveIndex, leaveCount, 1);
            else if (key == kKeyCross) {
                const int pick = leaveIds[leaveIndex];
                leaving = false;
                if (pick == NpSettings::kLeaveDiscard) {
                    std::fprintf(stderr, "[settings] left without saving (changes discarded)\n");
                    result = running ? Result::kReturn : Result::kExit;
                    done = true;
                } else if (pick == NpSettings::kLeaveSave) {
                    if (save()) {
                        std::fprintf(stderr, "[settings] left after saving\n");
                        result = running ? Result::kReturn : Result::kExit;
                        done = true;
                    } else {
                        messageUntil = now + 4000;   // the write failed: the screen stays, the edits stay
                    }
                }
            }
            key = kKeyNone;
        }
        if (key != kKeyNone && (controlsPage || key == kKeyTriangle)) {
            // the controls page: Left/Right change the layout (the CONTROLS row), Circle / Cross / Triangle go back to the list
            dirty = true;
            if (!controlsPage) {
                controlsPage = true;
            } else if (key == kKeyLeft || key == kKeyRight) {
                const int o = NpSettings::OptionIndex("CONTROLS");
                if (o >= 0) NpSettings::Step(state, o, key == kKeyLeft ? -1 : 1);
            } else if (key == kKeyCircle || key == kKeyCross || key == kKeyTriangle) {
                controlsPage = false;
            }
            key = kKeyNone;
        }

        if (key != kKeyNone) {
            dirty = true;
            const int rows = NpSettings::RowCount(page) > 0 ? NpSettings::RowCount(page) : 1;   // (an empty page: stay on row 0, no remainder by zero)
            const NpSettings::Row row = NpSettings::GetRow(page, selected);
            message.clear();
            if (key == kKeyUp) selected = (selected + rows - 1) % rows;
            else if (key == kKeyDown) selected = (selected + 1) % rows;
            else if (key == kKeyPagePrevious || key == kKeyPageNext) {
                const int pages = NpSettings::PageCount() > 0 ? NpSettings::PageCount() : 1;
                page = (page + (key == kKeyPageNext ? 1 : pages - 1)) % pages;
                selected = 0;
            } else if (key == kKeyCircle) {
                leaveCount = NpSettings::LeaveDialog(running, NpSettings::Changed(state), NpSettings::RestartNeeded(state), leaveIds);
                if (leaveCount > 0) {
                    leaving = true;
                    leaveIndex = NpSettings::LeaveDefault(leaveIds, leaveCount);
                } else {
                    std::fprintf(stderr, "[settings] left without saving\n");
                    result = running ? Result::kReturn : Result::kExit;
                    done = true;
                }
            } else {
                const int direction = key == kKeyLeft ? -1 : 1;
                if (row.kind == NpSettings::kRowOption && (key == kKeyLeft || key == kKeyRight || key == kKeyCross)) {
                    NpSettings::Step(state, row.option, direction);
                } else if (key == kKeyCross) {
                    if (row.kind == NpSettings::kRowAction && (!std::strcmp(NpSettings::ActionTitle(row.option), "SEND A REPORT") || !std::strcmp(NpSettings::ActionTitle(row.option), "DELETE MY ONLINE ACCOUNT"))) {
                        // W22: the same row kind. Ask shows what would happen (no network); only SEND / YES starts anything. No account: the sentence says so, no dialog.
                        NpMpReport::UseNetwork();
                        const bool del = !std::strcmp(NpSettings::ActionTitle(row.option), "DELETE MY ONLINE ACCOUNT");
                        if (report.Ask(del ? NpMpReport::Op::kDelete : NpMpReport::Op::kReport)) {
                            reportDialog = true;
                            reportChoice = 0;
                        } else {
                            message = report.Sentence();
                            messageIsError = true;
                            messageUntil = now + 6000;
                        }
                    } else if (row.kind == NpSettings::kRowAction) {
                        // SAVE A REPORT (np_settings.cpp kActions): the multiplayer diagnostics, scrubbed, to ux0:data/mscharged/mp-report.txt (np_mpdiag.h). Not during play: the
                        // screen is not offered in a session. One function; the page the row stands on does not matter.
                        char path[300];
                        const bool ok = !std::strcmp(NpSettings::ActionTitle(row.option), "SAVE A REPORT") && NpMpDiag::SaveReport(nullptr, path, sizeof path);
                        std::fprintf(stderr, "[settings] save a report: %s\n", ok ? "written" : "failed");
                        message = ok ? "Report saved: ux0:data/mscharged/mp-report.txt" : "Could not write the report. Is the memory card full?";
                        messageIsError = !ok;
                        messageUntil = now + 8000;
                    } else if (row.kind == NpSettings::kRowReset) {
                        NpSettings::ResetAll(state);
                        if (NpChangeGuardVita::SettingsSaved) NpChangeGuardVita::SettingsSaved();   // (r16 F2: RESET TO DEFAULTS also clears the failed-start count)
                        message = "Every row is back to its default. Save to keep that.";
                        messageIsError = false;
                        messageUntil = now + 4000;
                    } else if (row.kind == NpSettings::kRowSaveStart || row.kind == NpSettings::kRowSaveExit) {
                        if (running && row.kind == NpSettings::kRowSaveExit) {   // DON'T SAVE: leave, nothing is written (no restart from a running game)
                            std::fprintf(stderr, "[settings] left without saving (DON'T SAVE)\n");
                            result = Result::kReturn;
                            done = true;
                        } else if (save()) {
                            result = running ? Result::kReturn : (row.kind == NpSettings::kRowSaveStart ? Result::kStart : Result::kExit);
                            draw();
                            screen = VitaConsole::PresentScreen();
                            if (!screen) break;
                            sceKernelDelayThread(600 * 1000);
                            done = true;
                        } else {
                            messageUntil = now + 4000;
                        }
                    }
                }
            }
        }
        if (message.size() && messageUntil && now >= messageUntil) {
            message.clear();
            messageUntil = 0;
            dirty = true;
        }
        if (dirty) {
            draw();
            screen = VitaConsole::PresentScreen();
            if (!screen) break;
            dirty = false;
        }
        if (!done) sceKernelDelayThread(16 * 1000);
        // a scripted run that has used up its words and had no pad: stop after the last one (nobody is there to press anything)
        if (!script.words.empty() && script.next >= script.words.size() && now >= script.readyAt + 1000 && !done) {
            std::fprintf(stderr, "[settings] test script finished\n");
            result = running ? Result::kReturn : Result::kExit;
            done = true;
        }
    }
    VitaConsole::ReleaseScreen(running);
    if (result == Result::kStart) {
        // The game starts again as a fresh process so that every variable of the new file is read where the game reads it. If the system will not do that,
        // the caller reads the file again and goes on in this process.
        char* argv[] = {const_cast<char*>("-play"), nullptr};
        const bool cancelled = VitaPlan::CancelPendingStart();
        const int rc = sceAppMgrLoadExec("app0:eboot.bin", argv, nullptr);
        VitaPlan::RestorePendingStart(cancelled);
        std::fprintf(stderr, "[settings] sceAppMgrLoadExec returned 0x%X: carrying on in this process\n", static_cast<unsigned>(rc));
    }
    return result;
}

// The running game starts again as a fresh process ("-play"): used by the technical screen's SAVE AND RESTART (Pump) and by the native screen (native18 block below). Returns only
// when the system refused (false): the game goes on in this process.
bool RelaunchRunningGame() {
    char* argv[] = {const_cast<char*>("-play"), nullptr};
    const bool cancelled = VitaPlan::CancelPendingStart();
    if (VgxQuiesceForExit) VgxQuiesceForExit(200);   // the running game's back end finishes its picture before the process is replaced
    const int rc = sceAppMgrLoadExec("app0:eboot.bin", argv, nullptr);
    if (VgxQuiesceCancel) VgxQuiesceCancel();         // refused: the game goes on in this process and must get its pictures again
    VitaPlan::RestorePendingStart(cancelled);
    std::fprintf(stderr, "[settings] restart failed (0x%X); saved changes apply next launch\n", static_cast<unsigned>(rc));
    return false;
}

void Pump() {
    static uint64_t nextPoll = 0;
    const uint64_t now = NowMs();
    if (g_menuRequest) {   // the pause menu's item: no polling delay, no parameter
        g_menuRequest = false;
        g_openRequest.Request();
        nextPoll = 0;
    }
    if (now < nextPoll) return;
    nextPoll = now + 250;
    {   // set18: an old LiveArea Settings tile pressed while the game runs: logged and dropped (the settings are in the game's menu); a pending request of the game's menu stays
        const bool menu = g_openRequest.Pending();
        if (g_openRequest.Observe(ReadSettingsParameter())) {
            VitaLog::Note("The old LiveArea Settings tile was pressed: nothing opens; the settings are in the game (pause, VITA SETTINGS)");
            if (!menu) g_openRequest.Opened();
        }
    }
    // 1.3: never over a LAN/online match: the screen takes the display and the game thread stops, which would desync the peers. An old shortcut's press is dropped (logged).
    if (g_openRequest.Pending() && (NpLanActive() || NpOnlineFlowActive())) {
        g_openRequest.Opened();
        VitaLog::Note("Settings not opened: a multiplayer session is running (leave it first)");
        return;
    }
    if (!g_openRequest.Pending() || VitaBootScreen::Active() || (np_vita_radio_dialog_active && np_vita_radio_dialog_active())) return;
    const auto failed = [] {
        if (g_openRequest.Failed()) VitaLog::Note("Settings could not open after 3 tries: the press was dropped, the game goes on");
    };
    if (!VitaPlatform::BeginSettings()) {
        failed();
        return;
    }
    const Result result = Run(true);
    VitaPlatform::EndSettings();
    if (result == Result::kNotShown) {
        failed();
        return;
    }
    g_openRequest.Opened();
    std::fprintf(stderr, "[settings] returned to running game\n");
    VitaLog::Flush();
}

} // namespace NpSettingsScreen

// ---- native18: the backend of platform/sdk/net/np_native_settings.h (the game's own settings screen, SHVitaSettings). The model reads and writes the whole file through these four. ----
extern "C" bool NpNativeSettingsBackendRead(char* out, int cap, int* length) {
    std::string text;
    if (!NpSettingsScreen::ReadFile(NpSettingsScreen::SavedFilePath(), text)) text.clear();   // no file: every row at its default
    if (static_cast<int>(text.size()) > cap) return false;
    std::memcpy(out, text.data(), text.size());
    *length = static_cast<int>(text.size());
    return true;
}
extern "C" bool NpNativeSettingsBackendWrite(const char* text, int length) {
    const bool ok = NpSettingsScreen::WriteSettingsText(NpSettingsScreen::kSettingsFile, std::string(text, static_cast<size_t>(length)));
    std::fprintf(stderr, "[settings] native screen: np-player.txt %s (%d bytes)\n", ok ? "written" : "NOT written", length);
    if (ok && NpChangeGuardVita::SettingsSaved) NpChangeGuardVita::SettingsSaved();
    return ok;
}
extern "C" bool NpNativeSettingsBackendRestart(void) { return NpSettingsScreen::RelaunchRunningGame(); }   // returns only when refused
extern "C" bool NpNativeSettingsBackendOpenAll(void) {
    NpSettingsScreen::RequestOpen();
    return true;
}

// platform/sdk/net/np_pause_settings.cpp reaches the screen through this (a weak reference there: other builds have none)
extern "C" void NpVitaSettingsRequestOpen(void) { NpSettingsScreen::RequestOpen(); }

#endif // __vita__ && NP_PLAYER
