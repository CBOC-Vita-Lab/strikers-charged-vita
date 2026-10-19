// The Settings screen of the player build on the console: the LiveArea's Settings button starts this application with the parameter "-settings"
// (sce_sys/livearea/contents/template.xml, target psla:-settings; read back with sceAppMgrGetAppParam, as Mario Kart Wii Vita 1.3.0 does), or the player holds
// L + R while the game starts. The screen (np_settings.cpp) edits ux0:data/mscharged/np-player.txt; "Save and start" writes it and starts the application again
// (every setting is read from the environment at start-up, some at static initialisation, so a fresh process is the only way to be sure it applies), "Save and
// exit" writes it and quits to the LiveArea, Circle quits without saving.
//
// Test hooks (Vita3K has no LiveArea Settings button to press, and no one to press keys): a file ux0:data/mscharged/np-settings-open opens the screen; its
// words, if any, are the keys pressed one after another (up down left right cross circle, "shot:NAME" writes shots/settings-NAME.ppm, "wait:MS"); the file is
// deleted when the screen opens so that a restart does not open it again. The game that "Save and start" starts again is told by the argument -play.
#if defined(__vita__) && defined(NP_PLAYER)

#include "np_vita_settings.h"

#include "np_settings.h"
#include "np_vita_internal.h"

#include <psp2/appmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <cctype>

extern "C" bool np_vita_radio_dialog_active(void) __attribute__((weak));

namespace NpSettingsScreen {
namespace {

constexpr const char* kSettingsFile = "ux0:/data/mscharged/np-player.txt";
constexpr const char* kSettingsBackup = "ux0:/data/mscharged/np-player.txt.bak";
constexpr const char* kOpenMarker = "ux0:/data/mscharged/np-settings-open";

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

// Keep the last file until its replacement is installed. A failed rename must not erase it.
bool WriteFile(const char* path, const std::string& text) {
    std::string temporary = std::string(path) + ".tmp";
    std::string backup = std::string(path) + ".bak";
    if (Exists(backup.c_str())) {
        if (!Exists(path)) {
            if (sceIoRename(backup.c_str(), path) < 0) return false;
        } else if (sceIoRemove(backup.c_str()) < 0) return false;
    }
    const SceUID fd = sceIoOpen(temporary.c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) return false;
    bool ok = text.empty() || sceIoWrite(fd, text.data(), static_cast<SceSize>(text.size())) == static_cast<int>(text.size());
    if (sceIoClose(fd) < 0) ok = false;
    if (ok) {
        const bool had = Exists(path);
        if (had && sceIoRename(path, backup.c_str()) < 0) ok = false;
        if (ok) {
            ok = sceIoRename(temporary.c_str(), path) >= 0;
            if (!ok && had) sceIoRename(backup.c_str(), path);
            if (ok && had) sceIoRemove(backup.c_str());
        }
    }
    if (!ok) sceIoRemove(temporary.c_str());
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

bool TakeSettingsParameter() {
    char param[1024] = {};
    const int rc = sceAppMgrGetAppParam(param);
    if (rc < 0 || !std::memchr(param, 0, sizeof(param))) return false;
    if (param[0]) std::fprintf(stderr, "[settings] received parameter: \"%.200s\"\n", param);
    return IsSettingsParameter(param);
}

bool HasArgument(int argc, char** argv, const char* word) {
    // from argv[0] on: the arguments of sceAppMgrLoadExec become the new process's argv as they are (Vita3K logs "argument at 0: -play"), so "-play" may be argv[0]
    for (int i = 0; i < argc; ++i)
        if (argv && argv[i] && std::strstr(argv[i], word)) return true;
    return false;
}

// What the screen reads as button presses.
enum Key { kKeyNone, kKeyUp, kKeyDown, kKeyLeft, kKeyRight, kKeyCross, kKeyCircle, kKeyTriangle };

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
    WriteFile(path, ppm);
    std::fprintf(stderr, "[settings] screenshot %s\n", path);
}

} // namespace

const char* SavedFilePath() {
    if (!Exists(kSettingsFile) && Exists(kSettingsBackup)) {
        if (sceIoRename(kSettingsBackup, kSettingsFile) < 0) return kSettingsBackup;
        std::fprintf(stderr, "[settings] recovered interrupted config replacement\n");
    }
    return kSettingsFile;
}

bool Requested(int argc, char** argv) {
    // what the system passed (a LiveArea start has no arguments; the relaunch of "Save and start" has "-play"), for the log of a console test
    if (argc > 0 && argv)
        std::fprintf(stderr, "[settings] start arguments: argc=%d%s%.60s%s%.60s\n", argc, argv[0] ? " argv[0]=" : "", argv[0] ? argv[0] : "",
                     argc > 1 && argv[1] ? " argv[1]=" : "", argc > 1 && argv[1] ? argv[1] : "");
    // GetAppParam consumes the pending parameter, including an old -settings on an explicit restart.
    const bool parameter = TakeSettingsParameter();
    if (HasArgument(argc, argv, "-play")) return false;   // started again by "Save and start"
    if (Exists(kOpenMarker)) return true;
    if (HasArgument(argc, argv, "-settings")) return true;
    if (parameter) return true;
    // L + R held while the game starts (a few samples: the controller needs a moment after the start)
    for (int i = 0; i < 4; ++i) {
        VitaInput::Raw raw;
        if (VitaInput::Read(raw) && (raw.buttons & (VitaInput::kLTrigger | VitaInput::kRTrigger)) == (VitaInput::kLTrigger | VitaInput::kRTrigger)) return true;
        sceKernelDelayThread(16 * 1000);
    }
    return false;
}

Result Run(bool running) {
    uint32_t* screen = VitaConsole::TakeScreen();
    if (!screen) return running ? Result::kReturn : Result::kStart;
    std::string text;
    const bool had = ReadFile(SavedFilePath(), text);
    NpSettings::State state;
    NpSettings::Load(state, had ? text.c_str() : nullptr);
    std::fprintf(stderr, "[settings] screen open (%s, %u bytes)\n", had ? "np-player.txt read" : "no np-player.txt yet", static_cast<unsigned>(text.size()));

    Scripted script;
    if (Exists(kOpenMarker)) {
        std::string words;
        ReadFile(kOpenMarker, words);
        sceIoRemove(kOpenMarker);
        for (size_t at = 0; at < words.size();) {
            while (at < words.size() && (words[at] == ' ' || words[at] == '\n' || words[at] == '\r' || words[at] == '\t')) ++at;
            size_t end = at;
            while (end < words.size() && words[end] != ' ' && words[end] != '\n' && words[end] != '\r' && words[end] != '\t') ++end;
            if (end > at) script.words.push_back(words.substr(at, end - at));
            at = end;
        }
        std::fprintf(stderr, "[settings] test script: %u word(s)\n", static_cast<unsigned>(script.words.size()));
    }

    int selected = 0;
    bool controlsPage = false;   // W5 CTL: Triangle shows every button of the chosen layout
    bool confirming = false, confirmRestart = false;
    std::string message;
    bool messageIsError = false;
    uint64_t messageUntil = 0;
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
        if (!WriteFile(kSettingsFile, out)) {
            message = "Could not write np-player.txt";
            messageIsError = true;
            return false;
        }
        std::fprintf(stderr, "[settings] np-player.txt written (%u bytes)\n", static_cast<unsigned>(out.size()));
        message = "Saved to ux0:data/mscharged/np-player.txt";
        messageIsError = false;
        return true;
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
                else if (w.compare(0, 5, "shot:") == 0) {
                    if (controlsPage) NpSettings::DrawControls(screen, state);
                    else NpSettings::Draw(screen, state, selected, message.c_str(), messageIsError, running);
                    if (confirming) NpSettings::DrawRestartConfirmation(screen, confirmRestart);
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
                else if (pick & VitaInput::kCircle) key = kKeyCircle;
                else if (pick & VitaInput::kTriangle) key = kKeyTriangle;
            }
        }

        if (key != kKeyNone && confirming) {
            dirty = true;
            if (key == kKeyCircle || (key == kKeyCross && !confirmRestart)) confirming = false;
            else if (key == kKeyLeft || key == kKeyRight || key == kKeyUp || key == kKeyDown) confirmRestart = !confirmRestart;
            else if (key == kKeyCross && confirmRestart) {
                if (save()) { result = Result::kRestart; done = true; }
                else { confirming = false; messageUntil = now + 4000; }
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
            const int rows = NpSettings::RowCount();
            const NpSettings::Row row = NpSettings::GetRow(selected);
            message.clear();
            if (key == kKeyUp) selected = (selected + rows - 1) % rows;
            else if (key == kKeyDown) selected = (selected + 1) % rows;
            else if (key == kKeyCircle) {
                std::fprintf(stderr, "[settings] left without saving\n");
                result = running ? Result::kReturn : Result::kExit;
                done = true;
            } else {
                const int direction = key == kKeyLeft ? -1 : 1;
                if (row.kind == NpSettings::kRowPreset && (key == kKeyLeft || key == kKeyRight || key == kKeyCross)) {
                    const int count = NpSettings::PresetCount();
                    const int at = NpSettings::MatchPreset(state);
                    const int to = at < 0 ? (direction > 0 ? 0 : count - 1) : ((at + direction) % count + count) % count;
                    NpSettings::ApplyPreset(state, to);
                } else if (row.kind == NpSettings::kRowOption && (key == kKeyLeft || key == kKeyRight || key == kKeyCross)) {
                    NpSettings::Step(state, row.option, direction);
                } else if (key == kKeyCross) {
                    if (row.kind == NpSettings::kRowReset) {
                        NpSettings::ResetAll(state);
                    } else if (row.kind == NpSettings::kRowSaveStart || row.kind == NpSettings::kRowSaveExit) {
                        if (running && row.kind == NpSettings::kRowSaveExit) {
                            confirming = true;
                            confirmRestart = false;
                        } else if (save()) {
                            result = running ? Result::kReturn : (row.kind == NpSettings::kRowSaveStart ? Result::kStart : Result::kExit);
                            NpSettings::Draw(screen, state, selected, message.c_str(), messageIsError, running);
                            VitaConsole::PresentScreen();
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
            if (controlsPage) NpSettings::DrawControls(screen, state);
            else NpSettings::Draw(screen, state, selected, message.c_str(), messageIsError, running);
            if (confirming) NpSettings::DrawRestartConfirmation(screen, confirmRestart);
            VitaConsole::PresentScreen();
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
        const int rc = sceAppMgrLoadExec("app0:eboot.bin", argv, nullptr);
        std::fprintf(stderr, "[settings] sceAppMgrLoadExec returned 0x%X: carrying on in this process\n", static_cast<unsigned>(rc));
    }
    return result;
}

void Pump() {
    static uint64_t nextPoll = 0;
    static bool pending = false;
    const uint64_t now = NowMs();
    if (now < nextPoll) return;
    nextPoll = now + 250;
    pending = TakeSettingsParameter() || pending;
    if (!pending || VitaBootScreen::Active() || (np_vita_radio_dialog_active && np_vita_radio_dialog_active())) return;
    pending = false;
    if (!VitaPlatform::BeginSettings()) return;
    const Result result = Run(true);
    VitaPlatform::EndSettings();
    std::fprintf(stderr, "[settings] returned to running game%s\n", result == Result::kRestart ? "; restart confirmed" : "");
    VitaLog::Flush();
    if (result == Result::kRestart) {
        char* argv[] = {const_cast<char*>("-play"), nullptr};
        const int rc = sceAppMgrLoadExec("app0:eboot.bin", argv, nullptr);
        std::fprintf(stderr, "[settings] restart failed (0x%X); saved changes apply next launch\n", static_cast<unsigned>(rc));
    }
}

} // namespace NpSettingsScreen

#endif // __vita__ && NP_PLAYER
