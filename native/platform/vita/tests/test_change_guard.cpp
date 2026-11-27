// Host test of the start check for Picture and Language changes (np_change_guard.h) over a fake card: the whole state machine, no console.
//   c++ -std=c++17 -I../src test_change_guard.cpp ../src/np_change_guard.cpp -o t && ./t
// The core plan guard's side is simulated here by the same four events (Cancel, Restore, Failed, Stable) the glue forwards, and a copy of its rules (reserve +1 at start, clear at
// the stable point, step on failure) runs beside it, so that "the two guards do not fight" is a checked property: the same failures drive both, neither touches the other's file.
#include "np_change_guard.h"

#include <cstdio>
#include <string>

using namespace NpChangeGuard;

static int g_failed = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_failed; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

struct Card : Files {
    std::string player, change;
    bool playerExists = true, changeExists = false;
    int readSettingsError = 0, readGuardError = 0;
    bool writeFails = false;
    bool errorHandsText = false;           // (a read error that still hands over what it read: the guard must not trust it)
    int writes = 0, playerWrites = 0;
    int ReadSettings(std::string& out) override {
        if (!playerExists) return 1;
        out = player;                       // (an error still hands over what it read: the guard must not trust it)
        return readSettingsError ? 2 : 0;
    }
    int ReadGuard(std::string& out) override {
        if (readGuardError) { if (errorHandsText) out = change; return 2; }
        if (!changeExists) return 1;
        out = change;
        return 0;
    }
    bool WriteGuard(const std::string& text) override {
        if (writeFails) return false;
        change = text;
        changeExists = true;
        ++writes;
        return true;
    }
};

static const char* kStandard = "MSC_CONTROLS=classic\nMSC_REAR_TOUCH=0\n";
static const char* kSmall = "MSC_CONTROLS=classic\nMSC_VITA_RENDER=480x272\n";

// One start: Begin, then either the stable point or not. Returns the guard's state.
static Guard::State Start(Card& card, bool stable, bool kitEnv = false) {
    Guard g(card);
    const Guard::State s = g.Begin(kitEnv);
    if (stable) g.Stable();
    return s;
}

// the core plan guard's rule, as vita_plan.cpp has it (np-plan.txt: one number)
struct PlanGuard {
    int file = 0;
    int level = -1;
    bool reserved = false, cleared = false;
    void Start() { level = file <= 0 ? 0 : file == 1 ? 1 : file == 2 ? 2 : 3; file += 1; reserved = true; cleared = false; }
    void Stable() { if (!cleared) { cleared = true; reserved = false; file = 0; } }
};

int main() {
    // ---- the fingerprint and the watched lines
    CHECK(Fingerprint("") == 0);
    CHECK(Fingerprint(kStandard) == 0);
    CHECK(Fingerprint(kSmall) != 0);
    CHECK(Fingerprint("MSC_VITA_RENDER=480x272\nMSC_FPS=30\n") == Fingerprint("MSC_FPS=30\nMSC_VITA_RENDER=480x272\n"));          // order of lines does not matter
    CHECK(Fingerprint("MSC_FPS=30\n") != Fingerprint("MSC_FPS=60\n"));
    CHECK(Fingerprint("# MSC_FPS=30\n") == 0);                                                                                        // a commented line is not a line
    CHECK(Fingerprint("MSC_FPS=60\nMSC_FPS=30\n") == Fingerprint("MSC_FPS=30\n"));                                                   // the last one of a key wins, as the loader has it
    CHECK(Fingerprint("  MSC_FPS=30  \r\n") == Fingerprint("MSC_FPS=30\n"));
    CHECK(Fingerprint("MSC_PLAN=S\n") == 0);                                                                                          // the way out is not watched
    CHECK(Fingerprint("MSC_CROWD=0\nMSC_REDUCED_SHAKE=1\nMSC_REAR_TOUCH=1\nMSC_CONTROLS=classic\n") == 0);                           // live rows and controls are not watched
    for (const char* k : {"MSC_VITA_RENDER", "MSC_FPS", "MSC_LOCK60", "MSC_VGX_MSAA", "MSC_DOF", "MSC_VITA_UPSCALE", "NP_WIDESCREEN", "MSC_HDTEX", "MSC_SHADOW_LIVE", "MSC_LANGUAGE", "NP_LANG"}) {
        CHECK(IsWatched(k));
        CHECK(Fingerprint(std::string(k) + "=x\n") != 0);
    }
    CHECK(!IsWatched("MSC_PLAN") && !IsWatched(nullptr) && !IsWatched("MSC_FPS_X"));
    CHECK(WatchedLines(kSmall).size() == 1 && WatchedLines(kSmall)[0] == "MSC_VITA_RENDER=480x272");
    Record r;
    CHECK(Parse("12 34 2\n", r) && r.good == 12 && r.tried == 34 && r.tries == 2);
    CHECK(!Parse("12 34\n", r) && !Parse("a b c", r) && !Parse("1 2 3 4", r) && !Parse("", r));
    CHECK(Format(r) == "12 34 2\n");

    // ---- an ordinary start writes nothing
    {
        Card c;
        c.player = kStandard;
        CHECK(Start(c, true) == Guard::State::kOff);
        CHECK(c.writes == 0 && !c.changeExists);
        Card none;
        none.playerExists = false;
        CHECK(Start(none, true) == Guard::State::kOff && none.writes == 0);
        Card empty;
        empty.player = "";
        CHECK(Start(empty, true) == Guard::State::kOff && empty.writes == 0);
    }
    // ---- a change, two starts that fail, the next start is standard (and only that start)
    {
        Card c;
        c.player = kSmall;
        const uint32_t fp = Fingerprint(kSmall);
        CHECK(Start(c, false) == Guard::State::kReserved);
        CHECK(c.change == Format({0, fp, 1}));
        CHECK(Start(c, false) == Guard::State::kReserved);
        CHECK(c.change == Format({0, fp, 2}));
        const int writes = c.writes;
        const std::string before = c.change;
        {
            Guard g(c);
            CHECK(g.Begin(false) == Guard::State::kFallback);
            CHECK(g.Skips("MSC_VITA_RENDER") && !g.Skips("MSC_CONTROLS") && !g.Skips("MSC_PLAN"));
            CHECK(g.Lines() == "MSC_VITA_RENDER=480x272");
            g.Stable();   // a start on the standard lines getting through does not bless the changed ones
            g.Cancel();
            g.Restore();
            g.Failed();
        }
        CHECK(c.writes == writes && c.change == before);   // not rewritten at the fallback start, and not by anything after it
        // the same fingerprint stays standard
        {
            Guard g(c);
            CHECK(g.Begin(false) == Guard::State::kFallback);
        }
        CHECK(c.change == before);
        // the player changes a row: a new fingerprint starts at 0 tries
        c.player = std::string(kSmall) + "MSC_FPS=30\n";
        const uint32_t fp2 = Fingerprint(c.player);
        CHECK(fp2 != fp);
        CHECK(Start(c, false) == Guard::State::kReserved);
        CHECK(c.change == Format({0, fp2, 1}));
        // and the old one is tried afresh only as a new fingerprint (no memory of the standard stretch)
    }
    // ---- a change that gets through: good moves, tries cleared
    {
        Card c;
        c.player = kSmall;
        const uint32_t fp = Fingerprint(kSmall);
        CHECK(Start(c, false) == Guard::State::kReserved);   // one failed start
        CHECK(Start(c, true) == Guard::State::kReserved);    // the second gets through
        CHECK(c.change == Format({fp, fp, 0}));
        const int writes = c.writes;
        CHECK(Start(c, true) == Guard::State::kOff);          // now an ordinary start again
        CHECK(Start(c, false) == Guard::State::kOff);
        CHECK(c.writes == writes);
        // back to the standard lines: nothing is written (the standard cannot be blamed)
        c.player = kStandard;
        CHECK(Start(c, true) == Guard::State::kOff && c.writes == writes);
        // and the good lines again are good again
        c.player = kSmall;
        CHECK(Start(c, false) == Guard::State::kOff && c.writes == writes);
        // a different change after a good one: tried against the good one
        c.player = "MSC_LANGUAGE=fr\n";
        CHECK(Start(c, false) == Guard::State::kReserved);
        CHECK(c.change == Format({fp, Fingerprint(c.player), 1}));
    }
    // ---- a cancel is not a failed start; a restore arms it again; a failure re-arms a cancelled one
    {
        Card c;
        c.player = kSmall;
        const uint32_t fp = Fingerprint(kSmall);
        Guard g(c);
        CHECK(g.Begin(false) == Guard::State::kReserved && c.change == Format({0, fp, 1}));
        CHECK(g.Cancel() && c.change == Format({0, 0, 0}));
        CHECK(!g.Cancel());                                   // nothing left to cancel
        g.Restore();
        CHECK(c.change == Format({0, fp, 1}) && g.state() == Guard::State::kReserved);
        g.Restore();                                          // not cancelled: nothing
        CHECK(c.change == Format({0, fp, 1}));
        CHECK(g.Cancel());
        g.Failed();                                           // a crash after a cancel counts the start
        CHECK(c.change == Format({0, fp, 1}) && g.state() == Guard::State::kReserved);
        g.Failed();                                           // a crash with the reservation in place: it stays
        CHECK(c.change == Format({0, fp, 1}));
        g.Stable();
        CHECK(c.change == Format({fp, fp, 0}) && g.state() == Guard::State::kStable);
        g.Failed();                                           // a late failure after the stable point is not a failed start
        g.Cancel();
        g.Restore();
        CHECK(c.change == Format({fp, fp, 0}));
        // a cancelled start that reaches the stable point
        Card d;
        d.player = kSmall;
        Guard h(d);
        h.Begin(false);
        h.Cancel();
        h.Stable();
        CHECK(d.change == Format({fp, fp, 0}));
        h.Restore();
        CHECK(d.change == Format({fp, fp, 0}));
        // two cancelled starts are no failed starts
        Card e;
        e.player = kSmall;
        for (int i = 0; i < 5; ++i) { Guard k(e); CHECK(k.Begin(false) == Guard::State::kReserved); k.Cancel(); }
        CHECK(Start(e, false) == Guard::State::kReserved);
        CHECK(e.change == Format({0, fp, 1}));
    }
    // ---- F2 (r16): a save from a settings screen (or RESET TO DEFAULTS) clears the failed-start count: the same lines are tried again
    {
        Card c;
        c.player = kSmall;
        const uint32_t fp = Fingerprint(kSmall);
        CHECK(Start(c, false) == Guard::State::kReserved);
        CHECK(Start(c, false) == Guard::State::kReserved);
        CHECK(Start(c, false) == Guard::State::kFallback);           // two failed starts: the standard lines, for ever...
        CHECK(Start(c, false) == Guard::State::kFallback);
        {
            Guard g(c);
            CHECK(g.Begin(false) == Guard::State::kFallback);
            g.SettingsSaved();                                       // ...until a screen saves
            CHECK(c.change == Format({0, 0, 0}));
            CHECK(g.state() == Guard::State::kFallback && g.Skips("MSC_VITA_RENDER"));   // (this start goes on with the standard lines)
        }
        CHECK(Start(c, false) == Guard::State::kReserved);           // the saved choice is tried again
        CHECK(c.change == Format({0, fp, 1}));
        CHECK(Start(c, true) == Guard::State::kReserved);            // and blessed when it gets through
        CHECK(c.change == Format({fp, fp, 0}));
        // no count standing: nothing is written
        const int writes = c.writes;
        { Guard g(c); g.Begin(false); g.SettingsSaved(); }
        CHECK(c.writes == writes);
        Card none;
        none.player = kSmall;
        { Guard g(none); g.Begin(true); g.SettingsSaved(); }
        CHECK(!none.changeExists && none.writes == 0);
        // a card error or a failed write changes nothing and does not crash
        Card bad;
        bad.player = kSmall;
        Start(bad, false);
        bad.readGuardError = 2;
        bad.errorHandsText = true;
        { Guard g(bad); g.SettingsSaved(); }
        CHECK(bad.change == Format({0, fp, 1}));
        bad.readGuardError = 0;
        bad.writeFails = true;
        { Guard g(bad); g.SettingsSaved(); }
        CHECK(bad.change == Format({0, fp, 1}));
        bad.writeFails = false;
        // a saved screen during a counted start: a later Cancel or a second Begin does not write the old count back
        Card mid;
        mid.player = kSmall;
        Start(mid, false);
        Guard g(mid);
        CHECK(g.Begin(false) == Guard::State::kReserved);
        CHECK(mid.change == Format({0, fp, 2}));
        g.SettingsSaved();
        CHECK(mid.change == Format({0, 0, 0}));
        CHECK(g.Cancel());
        CHECK(mid.change == Format({0, 0, 0}));
        CHECK(g.Begin(false) == Guard::State::kReserved && mid.change == Format({0, fp, 1}));
    }
    // ---- a second Begin in the same process (the settings screen saved and the start went on)
    {
        Card c;
        c.player = kSmall;
        const uint32_t fp = Fingerprint(kSmall);
        Guard g(c);
        g.Begin(false);
        CHECK(c.change == Format({0, fp, 1}));
        c.player = std::string(kSmall) + "MSC_FPS=30\n";   // the player changed a row on the screen
        const uint32_t fp2 = Fingerprint(c.player);
        CHECK(g.Begin(false) == Guard::State::kReserved);
        CHECK(c.change == Format({0, fp2, 1}));          // the first start's count was taken back, the new lines count once
        c.player = kStandard;
        CHECK(g.Begin(false) == Guard::State::kOff);
        CHECK(c.change == Format({0, 0, 0}));            // taken back, and nothing new
    }
    // ---- kit runs and the switch never trigger, and write nothing
    {
        for (const char* kit : {"MSC_BOOT_MATCH=1\n", "MSC_INPUT_SCRIPT=/x/y\n", "MSC_CHANGE_GUARD=0\n"}) {
            Card c;
            c.player = std::string(kSmall) + kit;
            c.change = Format({0, Fingerprint(c.player), 2});   // even with two failures on record
            c.changeExists = true;
            const std::string before = c.change;
            CHECK(Start(c, true) == Guard::State::kOff);
            CHECK(c.writes == 0 && c.change == before);
            Guard g(c);
            g.Begin(false);
            CHECK(!g.Skips("MSC_VITA_RENDER"));
        }
        Card c;
        c.player = kSmall;
        CHECK(Start(c, true, true) == Guard::State::kOff && c.writes == 0);                   // the environment's own switch
        c.player = std::string(kSmall) + "MSC_BOOT_MATCH=\n";
        CHECK(Start(c, true) == Guard::State::kReserved);                                      // an empty value is not a kit run
        c.player = std::string(kSmall) + "# MSC_BOOT_MATCH=1\nMSC_CHANGE_GUARD=1\n";
        c.change.clear(); c.changeExists = false;
        CHECK(Start(c, false) == Guard::State::kReserved);                                     // a commented kit line is not one
    }
    // ---- card errors never block and never write
    {
        Card c;
        c.player = kSmall;
        c.writeFails = true;
        Guard g(c);
        CHECK(g.Begin(false) == Guard::State::kOff && c.writes == 0 && !g.Skips("MSC_VITA_RENDER"));
        g.Stable(); g.Cancel(); g.Restore(); g.Failed();
        CHECK(c.writes == 0);
        Card d;
        d.player = kSmall;
        d.readGuardError = 1;
        CHECK(Start(d, true) == Guard::State::kOff && d.writes == 0);
        Card e;
        e.player = kSmall;
        e.readSettingsError = 1;
        CHECK(Start(e, true) == Guard::State::kOff && e.writes == 0);
        // a failed write at the stable point leaves the reservation: the next start counts, nothing is blocked
        Card f;
        f.player = kSmall;
        Guard h(f);
        h.Begin(false);
        f.writeFails = true;
        h.Stable();
        CHECK(f.change == Format({0, Fingerprint(kSmall), 1}));
        // a damaged np-change.txt counts as none
        Card m;
        m.player = kSmall;
        m.change = "garbage";
        m.changeExists = true;
        CHECK(Start(m, false) == Guard::State::kReserved && m.change == Format({0, Fingerprint(kSmall), 1}));
    }
    // ---- np-player.txt is never written (the fake has no writer for it: the interface cannot), and the two guards react to the same failures without fighting
    {
        Card c;
        c.player = kSmall;
        const std::string player = c.player;
        PlanGuard plan;
        // three starts that all fail the same way (a crash before 300 frames)
        for (int i = 0; i < 3; ++i) {
            Guard g(c);
            const Guard::State s = g.Begin(false);
            plan.Start();
            if (i < 2) CHECK(s == Guard::State::kReserved);
            else CHECK(s == Guard::State::kFallback);
            g.Failed();                       // the crash hook
            plan.file = plan.level + 1;       // FailedUnlocked: the next start takes the next plan
        }
        CHECK(c.player == player);
        CHECK(plan.file == 3);                // the plan guard stepped down by its own rule (the third start ran plan A, the next runs S)
        CHECK(c.change == Format({0, Fingerprint(kSmall), 2}));
        // the fourth start: plan S; the changed lines are still standard; both get through
        {
            Guard g(c);
            CHECK(g.Begin(false) == Guard::State::kFallback);
            plan.Start();
            CHECK(plan.level == 3);
            g.Stable();
            plan.Stable();
            CHECK(plan.file == 0);            // the plan guard clears itself
            CHECK(c.change == Format({0, Fingerprint(kSmall), 2}));   // and this guard's file did not move: still standard until a row changes
        }
        // the player fixes the row; a start that works clears this guard without the plan guard's help
        c.player = kStandard;
        CHECK(Start(c, true) == Guard::State::kOff);
        CHECK(c.player == kStandard);
    }

    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
