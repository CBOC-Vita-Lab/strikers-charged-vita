// mp24: np_quick_policy.h, the rule that PLAY WITH ANYONE searches the UNRANKED quick queue and the hub's RANKED MATCH the ranked one.
// Run by tools/net/quick_policy_mutants.sh (which also proves the test fails on each broken rule).
#include <stdio.h>
#ifndef QUICK_POLICY_HEADER
#define QUICK_POLICY_HEADER "platform/sdk/dwc/np_quick_policy.h"
#endif
#include QUICK_POLICY_HEADER

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

int main() {
    // PLAY WITH ANYONE: login, then the hub would open -> straight to the search, quick on
    {
        QuickState s;
        CHECK(!QuickPolicyScene(s, kQsLogin, true) && !s.quick, "the login does not jump and is not quick");
        CHECK(QuickPolicyScene(s, kQsHub, true), "the first hub after the login becomes the search");
        CHECK(s.quick, "the quick queue is chosen for that search");
        CHECK(!QuickPolicyScene(s, kQsOther, true) && s.quick, "other scenes (the search screen itself, the match) keep it");
        // Circle from the search: the hub opens again; it must stay the hub and RANKED MATCH there is ranked
        CHECK(!QuickPolicyScene(s, kQsHub, true), "the hub after Circle is the hub (no second jump)");
        CHECK(!s.quick, "the hub's RANKED MATCH is the ranked queue");
        CHECK(!QuickPolicyScene(s, kQsOther, true) && !s.quick, "ranked search keeps ranked");
        // a sign-in in the middle of a quick search (a lost link signs in again) ends the quick choice at once
        {
            QuickState t;
            QuickPolicyScene(t, kQsHub, true);
            QuickPolicyScene(t, kQsLogin, true);
            CHECK(!t.quick, "a new login while quick drops the quick choice");
        }
        // a new sign-in during the same run jumps again
        CHECK(!QuickPolicyScene(s, kQsLogin, true), "a new login does not jump");
        CHECK(QuickPolicyScene(s, kQsHub, true) && s.quick, "after a new login the hub jumps to the quick search again");
        // the main menu ends it
        QuickPolicyScene(s, kQsMainMenu, true);
        CHECK(!s.quick, "the main menu ends the quick choice");
    }
    // entering through the hub (RANKED MATCH / FRIENDS): never quick
    {
        QuickState s;
        QuickPolicyScene(s, kQsLogin, false);
        CHECK(!QuickPolicyScene(s, kQsHub, false) && !s.quick, "ranked entry: the hub is the hub, ranked");
        QuickPolicyScene(s, kQsOther, false);
        CHECK(!s.quick, "ranked entry: the search is ranked");
    }
    // a quick entry that left to the main menu and came back as a ranked entry (wanted off): ranked
    {
        QuickState s;
        QuickPolicyScene(s, kQsLogin, true);
        CHECK(QuickPolicyScene(s, kQsHub, true) && s.quick, "setup: quick");
        QuickPolicyScene(s, kQsMainMenu, true);
        QuickPolicyScene(s, kQsLogin, false);
        CHECK(!QuickPolicyScene(s, kQsHub, false) && !s.quick, "later ranked entry is ranked");
    }
    // a hub reached without any login scene first (the sign-in was kept): the first hub still jumps once
    {
        QuickState s;
        CHECK(QuickPolicyScene(s, kQsHub, true) && s.quick, "no login scene: the first hub jumps");
        CHECK(!QuickPolicyScene(s, kQsHub, true) && !s.quick, "and only once");
    }
    printf(g_fail ? "%d FAILED\n" : "all quick policy checks passed\n", g_fail);
    return g_fail != 0;
}
