// np_quick_policy.h (mp24): which queue a search joins. PLAY WITH ANYONE is the UNRANKED quick queue (no rating, no board); the hub's RANKED MATCH is the ranked one.
// Pure logic (no game types) so a host test can drive it: np_online_flow.cpp maps the game's scenes onto QuickScene and applies the answer.
#ifndef NP_QUICK_POLICY_H
#define NP_QUICK_POLICY_H

enum QuickScene { kQsOther = 0, kQsLogin, kQsHub, kQsMainMenu };

struct QuickState {
    bool done = false;    // the straight-to-search jump of this sign-in was taken
    bool quick = false;   // the next matchmaking is the quick queue (what NpDwcSetQuickMatch holds)
};

// A scene is about to open. `wanted` = this run's entry was PLAY WITH ANYONE. Returns true when the scene must become the match search (and then s.quick is on).
inline bool QuickPolicyScene(QuickState& s, QuickScene scene, bool wanted) {
    if (scene == kQsLogin) s.done = false;
    if (scene == kQsHub && !s.done && wanted) {
        s.done = true;
        s.quick = true;
        return true;
    }
    // the hub (Circle from the quick search, or a FRIENDS / RANKED entry), a new sign-in and the main menu end the quick choice
    if (scene == kQsHub || scene == kQsLogin || scene == kQsMainMenu) s.quick = false;
    return false;
}

#endif
