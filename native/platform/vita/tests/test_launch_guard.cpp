// np_launch_guard.h on the host (stab13): the decisions, a model of the card file over several launches, and the proof that a clean history changes nothing.
// Build twice: plain (must pass) and with -DMUT=n (a mutant of the header's rules: must fail), see run_launch_guard_test.sh.
#include "np_launch_guard.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)
using namespace NpLaunchGuard;

// The environment of one launch: the plan's own defaults, the owner's lines, and what ApplySafe changed.
struct Env {
    std::map<std::string, std::string> vars;
    std::map<std::string, bool> planDefault;
    int sets = 0;
    bool IsPlanDefault(const char* n) const { auto it = planDefault.find(n); return it != planDefault.end() && it->second; }
    void Set(const char* n, const char* v) { vars[n] = v; ++sets; }
};
static Env PlanEnv(const char* ownerKey = nullptr) {
    Env e;
    for (const Risky& r : kRisky) { e.vars[r.name] = "1"; e.planDefault[r.name] = true; }
    if (ownerKey) { e.vars[ownerKey] = "1"; e.planDefault[ownerKey] = false; }   // the owner's own line
    return e;
}

// The card: np-launch.txt as text, and the model of a launch: Begin (read, write n+1), then either a clean end (write 0) or nothing (a freeze / kill).
struct Card { bool exists = false; std::string text; };
struct Launch { State state; int previous; };
static Launch Begin(Card& c) {
    int prev = c.exists ? ParseCount(c.text.c_str(), c.text.size()) : 0;
    Launch l{Decide(prev), prev};
    c.exists = true; c.text = std::to_string(NextCount(prev)) + "\n";
    return l;
}
static void Clean(Card& c) { c.exists = true; c.text = "0\n"; }

int main() {
    // parsing
    CHECK(ParseCount("", 0) == 0);
    CHECK(ParseCount("0\n", 2) == 0);
    CHECK(ParseCount("2\n", 2) == 2);
    CHECK(ParseCount("  7", 3) == 7);
    CHECK(ParseCount("junk", 4) == 0);
    CHECK(ParseCount("1000000", 7) == kCountCap);
    CHECK(NextCount(0) == 1 && NextCount(kCountCap) == kCountCap);
    CHECK(MinutesFromText(nullptr) == kStableMinutes && MinutesFromText("5") == 5 && MinutesFromText("0") == kStableMinutes && MinutesFromText("9999") == 120);
    CHECK(!Stable(299, 99999999ull, 10) && !Stable(300, 9 * 60000ull + 59999ull, 10) && Stable(300, 10 * 60000ull, 10));

    // a clean history changes nothing: no file, "0", garbage, every launch cleared
    {
        Card c;
        for (int i = 0; i < 5; ++i) {
            Launch l = Begin(c);
            CHECK(l.state == State::kNormal);
            Env e = PlanEnv();
            const auto before = e.vars;
            CHECK(ApplySafe(l.state, e) == 0 && e.sets == 0 && e.vars == before);   // the environment is identical
            Clean(c);
        }
        Card g; g.exists = true; g.text = "not a number";
        CHECK(Begin(g).state == State::kNormal);
        Card z; z.exists = true; z.text = "0\n";
        CHECK(Begin(z).state == State::kNormal);
    }
    // a freeze: the next launch is safe, the owner's own line is kept, a clean end returns to normal
    {
        Card c;
        CHECK(Begin(c).state == State::kNormal);          // launch 1 freezes (no Clean)
        Launch l2 = Begin(c);
        CHECK(l2.state == State::kSafe && l2.previous == 1);
        Env e = PlanEnv("MSC_POSE_JOBS");
        const int n = ApplySafe(l2.state, e);
        CHECK(n == kRiskyCount - 1);
        for (const Risky& r : kRisky) CHECK(e.vars[r.name] == (std::strcmp(r.name, "MSC_POSE_JOBS") == 0 ? "1" : r.off));
        Clean(c);                                           // launch 2 ends cleanly
        CHECK(Begin(c).state == State::kNormal);
    }
    // two unclean launches in a row: both safe; the third is an ordinary start (the player who always kills the game early); a clear resets
    {
        Card c;
        Begin(c);                                           // 1: freezes
        CHECK(Begin(c).state == State::kSafe);              // 2: safe, freezes again
        CHECK(Begin(c).state == State::kSafe);              // 3: safe (n = 2), freezes again
        Launch l4 = Begin(c);
        CHECK(l4.state == State::kGaveUp && l4.previous == 3);
        Env e = PlanEnv();
        CHECK(ApplySafe(l4.state, e) == 0 && e.sets == 0);
        Clean(c);
        CHECK(Begin(c).state == State::kNormal);
    }
    // the marker survives a launch that is switched off (no file touched is the caller's rule): a kOff state changes nothing either
    { Env e = PlanEnv(); CHECK(ApplySafe(State::kOff, e) == 0 && e.sets == 0); }
    // the table: every name distinct, off values "0"
    for (int i = 0; i < kRiskyCount; ++i) {
        CHECK(std::strcmp(kRisky[i].off, "0") == 0);
        for (int j = i + 1; j < kRiskyCount; ++j) CHECK(std::strcmp(kRisky[i].name, kRisky[j].name) != 0);
    }
    std::puts("test_launch_guard: ok");
    return 0;
}
