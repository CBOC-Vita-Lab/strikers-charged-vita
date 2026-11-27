// Tests of the pause menu item's decision (platform/sdk/net/np_pause_settings.h) and of the leave dialog's logic (np_settings.cpp), run with mutants by
// run_pause_settings_tests.py.
#include "np_settings.h"
#include "np_pause_settings.h"

#include <cstdio>
#include <cstring>

static int g_checks = 0, g_failed = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failed; std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #cond); } } while (0)

int main() {
    using NpPauseSettings::Decide;
    // offered: a settings screen, switch unset or on, no networked session
    CHECK(Decide(nullptr, true, false));
    CHECK(Decide("1", true, false));
    CHECK(Decide("", true, false));
    // not offered: no screen (host / lab build), switch 0, a LAN or online session (pausing would stall and desync the match)
    CHECK(!Decide(nullptr, false, false));
    CHECK(!Decide("0", true, false));
    CHECK(!Decide(nullptr, true, true));
    CHECK(!Decide("1", true, true));
    CHECK(!Decide("0", false, true));

    // 1.3 front19: the OPTIONS screen's corner button follows the same rules, with its own function
    using NpPauseSettings::DecideOptions;
    CHECK(DecideOptions(nullptr, true, false) && DecideOptions("1", true, false) && DecideOptions("", true, false));
    CHECK(!DecideOptions(nullptr, false, false));     // no settings screen (host / lab build)
    CHECK(!DecideOptions("0", true, false));          // MSC_PAUSE_SETTINGS=0
    CHECK(!DecideOptions(nullptr, true, true));       // a LAN or online session (any phase)
    CHECK(!DecideOptions("1", true, true) && !DecideOptions("0", false, true));
    for (int sw = 0; sw < 3; ++sw)
        for (int screen = 0; screen < 2; ++screen)
            for (int net = 0; net < 2; ++net) {           // the same answer as the pause item for every input
                const char* v = sw == 0 ? nullptr : sw == 1 ? "1" : "0";
                CHECK(DecideOptions(v, screen != 0, net != 0) == Decide(v, screen != 0, net != 0));
            }

    // 1.3 front20: the press that closes a front-end popup is swallowed for a few frames; the match's (pause menu) path is untouched
    CHECK(NpPauseSettings::PopupSwallowFrames(true) >= 4 && NpPauseSettings::PopupSwallowFrames(true) <= 30);
    CHECK(NpPauseSettings::PopupSwallowFrames(false) == 0);
    // a lock whose owner has left is waited for only while its destructor can still come
    CHECK(!NpPauseSettings::StaleLockExpired(true, 0, 20) && !NpPauseSettings::StaleLockExpired(true, 19, 20));
    CHECK(NpPauseSettings::StaleLockExpired(true, 20, 20) && NpPauseSettings::StaleLockExpired(true, 300, 20));
    CHECK(!NpPauseSettings::StaleLockExpired(false, 500, 20));   // a lock whose owner is still on the stack is never released

    using namespace NpSettings;
    int ids[kMaxLeave];
    // nothing changed: no dialog at all
    CHECK(LeaveDialog(true, false, false, ids) == 0 && LeaveDialog(false, false, false, ids) == 0);
    // changed, running (1.3): SAVE (first, and lit first), DON'T SAVE, KEEP EDITING; never a restart from a match
    int n = LeaveDialog(true, true, true, ids);
    CHECK(n == 3 && ids[0] == kLeaveSave && ids[1] == kLeaveDiscard && ids[2] == kLeaveKeepEditing);
    CHECK(ids[LeaveDefault(ids, n)] == kLeaveSave && LeaveDefault(ids, n) == 0);
    CHECK(std::strcmp(LeaveLabel(kLeaveSave, true), "SAVE") == 0 && std::strcmp(LeaveLabel(kLeaveDiscard, true), "DON'T SAVE") == 0 && std::strcmp(LeaveLabel(kLeaveSaveRestart, false), "SAVE AND START AGAIN") == 0);
    // changed, start-up (nothing running to restart)
    n = LeaveDialog(false, true, true, ids);
    CHECK(n == 3 && ids[0] == kLeaveKeepEditing && ids[1] == kLeaveDiscard && ids[2] == kLeaveSave && ids[LeaveDefault(ids, n)] == kLeaveSave);
    // changed, running, live-only (1.3: CROWD, LESS SHAKE, REAR TOUCH): the same three, no restart
    n = LeaveDialog(true, true, false, ids);
    CHECK(n == 3 && ids[0] == kLeaveSave && ids[1] == kLeaveDiscard && ids[2] == kLeaveKeepEditing);
    for (int i = 0; i < n; ++i) CHECK(ids[i] != kLeaveSaveRestart);
    // every dialog has KEEP EDITING
    for (int running = 0; running < 2; ++running)
        for (int restart = 0; restart < 2; ++restart) {
            n = LeaveDialog(running, true, restart, ids);
            bool keep = false;
            for (int i = 0; i < n; ++i) keep |= ids[i] == kLeaveKeepEditing;
            CHECK(keep);
        }
    CHECK(LeaveMove(0, 4, -1) == 3 && LeaveMove(3, 4, 1) == 0 && LeaveMove(2, 4, 1) == 3);
    // an edit marks the state as changed and as needing a restart; putting it back clears both
    State s;
    Load(s, nullptr);
    CHECK(!Changed(s) && !RestartNeeded(s));
    SetChoice(s, OptionIndex("FRAME RATE"), 1);
    CHECK(Changed(s) && RestartNeeded(s));
    SetChoice(s, OptionIndex("FRAME RATE"), 0);
    CHECK(!Changed(s) && !RestartNeeded(s));

    // ---- the dialogs' layout, in each language (verify9 F1-F3): a dialog dims the whole screen outside its box, and no text reaches past the box's margins ----
    {
        static uint32_t list[kWidth * kHeight], shown[kWidth * kHeight];
        const auto dim = [](uint32_t c) {   // what the 170/255 black shade makes of a list pixel (np_settings.cpp Shade)
            const uint32_t r = (c & 255) * 85 / 255, g = ((c >> 8) & 255) * 85 / 255, b = ((c >> 16) & 255) * 85 / 255;
            return 0xFF000000u | (b << 16) | (g << 8) | r;
        };
        const uint32_t kBox = 0xFF30261Eu;   // the box's own colour (np_settings.cpp kPanel = Rgb(30, 38, 48))
        struct Box { int x, y, w, h; };
        const char* const kLang[4] = {"AUTOMATIC", "ENGLISH", "SPANISH", "FRENCH"};
        (void)kLang;
        int worst = 0;   // the most characters of any line drawn in a box (the margin test below is what holds the limit)
        for (int language = 1; language <= 3; ++language)
            for (int classic = 0; classic < 2; ++classic)
                for (int kind = 0; kind < 4; ++kind) {
                    State st;
                    Load(st, nullptr);
                    SetChoice(st, OptionIndex("LANGUAGE"), language);
                    SetChoice(st, OptionIndex("CONTROLS"), classic);
                    SetChoice(st, OptionIndex("FRAME RATE"), 1);   // (a change, so the running dialog has its four choices)
                    Draw(list, st, 1, 0, nullptr, false, true);
                    std::memcpy(shown, list, sizeof list);
                    Box box{80, 160, 800, 220};
                    int ids2[kMaxLeave];
                    const int count = LeaveDialog(true, true, true, ids2);
                    if (kind == 0) { DrawRestartConfirmation(shown, false, classic != 0); }
                    else if (kind == 1) { DrawRestartConfirmation(shown, true, classic != 0); }
                    else if (kind == 2) { DrawLeaveConfirmation(shown, 2, classic != 0); }
                    else { DrawLeaveDialog(shown, ids2, count, 0, true, classic != 0, true); box = Box{80, 100, 800, 150 + 24 + count * 44 + 40}; }
                    int outsideBad = 0, marginBad = 0, textRows = 0;
                    for (int y = 0; y < kHeight; ++y)
                        for (int x = 0; x < kWidth; ++x) {
                            const uint32_t v = shown[y * kWidth + x];
                            const bool inside = x >= box.x && x < box.x + box.w && y >= box.y && y < box.y + box.h;
                            if (!inside) {
                                if (v != dim(list[y * kWidth + x])) ++outsideBad;   // a list pixel left undimmed, or text past the box
                            } else if (y >= box.y + 3 && (x < box.x + 32 || x >= box.x + box.w - 32) && v != kBox) {
                                ++marginBad;   // text inside the box's 32 px margins: it needs 112 + 12 * characters <= 848
                            }
                        }
                    (void)worst;
                    (void)textRows;
                    CHECK(outsideBad == 0);
                    CHECK(marginBad == 0);
                    if (outsideBad || marginBad) std::fprintf(stderr, "  language %d classic %d kind %d: %d outside, %d margin\n", language, classic, kind, outsideBad, marginBad);
                }
        // and the box holds something: the dialog is drawn at all (a mutant that draws nothing must not pass the pixel rules by silence)
        State st;
        Load(st, nullptr);
        SetChoice(st, OptionIndex("FRAME RATE"), 1);
        Draw(list, st, 1, 0, nullptr, false, true);
        std::memcpy(shown, list, sizeof list);
        int ids3[kMaxLeave];
        DrawLeaveDialog(shown, ids3, LeaveDialog(true, true, true, ids3), 0, true);
        int boxPixels = 0;
        for (int y = 104; y < 280; ++y)
            for (int x = 90; x < 870; ++x)
                if (shown[y * kWidth + x] == kBox) ++boxPixels;
        CHECK(boxPixels > 100000);
        // 1.3: the note ("Some changes apply the next time the game starts.") is drawn only when a next-start row changed
        static uint32_t withNote[kWidth * kHeight], withoutNote[kWidth * kHeight];
        std::memcpy(withNote, list, sizeof list);
        std::memcpy(withoutNote, list, sizeof list);
        DrawLeaveDialog(withNote, ids3, LeaveDialog(true, true, true, ids3), 0, true, false, true);
        DrawLeaveDialog(withoutNote, ids3, LeaveDialog(true, true, true, ids3), 0, true, false, false);
        CHECK(std::memcmp(withNote, withoutNote, sizeof withNote) != 0);
    }
    // ---- set18: SMOOTH 60 is a choice of FRAME RATE (the SMOOTH 60 MODE row went): no row is grey, and choosing a frame rate moves no other row ----
    {
        State st;
        Load(st, nullptr);
        const int fr = OptionIndex("FRAME RATE");
        CHECK(fr >= 0 && OptionIndex("SMOOTH 60 MODE") < 0 && OptionIndex("RESOLUTION") < 0);
        for (int c = 0; c < ChoiceCount(fr); ++c) {
            SetChoice(st, fr, c);
            for (int o = 0; o < OptionCount(); ++o) {
                CHECK(!WhyNoEffect(st, o));
                CHECK(o == fr || st.choice[o] == GetOption(o).defaultChoice);   // no other row follows it
            }
        }
        SetChoice(st, fr, 0);
        CHECK(!Changed(st));
        std::string out;
        CHECK(Serialize(st, out) && out.find("MSC_PICTURE_HOLD") == std::string::npos);
        CHECK(OptionIndex("60 FPS (PERFORMANCE)") < 0 && OptionIndex("FULL PICTURE") < 0);
    }
    std::printf("test_pause_settings: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
