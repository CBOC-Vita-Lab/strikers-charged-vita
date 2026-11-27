// Multiplayer is a public BETA (mpbeta27, owner 18 Nov: "It will be a public launch, but we'll call it a beta on the mp for now."). The owner has one Vita, so the first
// matches between two Vitas happen in players' hands and the problem reports are how we learn. This header holds what the player SEES of that:
//   - the BETA tag on the multiplayer entry points (the Multiplayer menu's buttons ONLINE, LOCAL, NEARBY, SAME WI-FI, FRIENDS, PLAY WITH ANYONE, RANKED MATCH; the main
//     menu's MULTIPLAYER entry and the ONLINE CENTRAL title through np_lan_text.h),
//   - the one-time notice the first time the Multiplayer menu is opened ("Multiplayer is in beta. ..."),
//   - the line on the result screen of a multiplayer match (np_notice.cpp, render only),
//   - the plain sentence of the sync-error popup (the game's own text said little) and of a partner with another build,
//   - REPORT A PROBLEM on the Multiplayer menu (np_menu_flow.cpp, where CANCEL was: Circle still goes back).
// Nothing here changes what a match computes or sends: texts and one small file (the notice was seen). Header only, no game types: the host tests read it as it is.
//
// Switches (each off-able by env for the coordinator's A/B runs; code default ON, as MSC_WAIT_BANNER):
//   MSC_MP_BETA=0         no tags, no notice, no result line, no REPORT A PROBLEM button, the game's own sync-error text (the 1.3.0 screens)
//   MSC_MP_BETA_NOTICE=0  no one-time notice; =always shows it at every visit (tests); unset in a scripted run (MSC_INPUT_SCRIPT): none
//   MSC_MP_REPORT_ENTRY=0 the Multiplayer menu's third button is CANCEL again
// Every action is counted (Counters) and logged once per visit by the menu: "[mpbeta] tags N notice N stamp N sync-text N report-entry N".
#ifndef NP_MPBETA_H
#define NP_MPBETA_H

#include <stdlib.h>
#include <string.h>

namespace NpMpBeta {

inline bool EnvOff(const char* name) {
    const char* v = getenv(name);
    return v && v[0] == '0';
}
inline bool On() { return !EnvOff("MSC_MP_BETA"); }
inline bool ReportEntryOn() { return On() && !EnvOff("MSC_MP_REPORT_ENTRY"); }
// 0 never, 1 once (default), 2 at every visit. A scripted run (MSC_INPUT_SCRIPT: the harness, the pair rig, the console kit routes) sees no notice unless it asks for one
// with MSC_MP_BETA_NOTICE: the routes written before the notice existed walk straight into the mode choice.
inline int NoticeMode() {
    if (!On()) return 0;
    const char* v = getenv("MSC_MP_BETA_NOTICE");
    if (!v || !*v) {
        const char* script = getenv("MSC_INPUT_SCRIPT");
        return script && *script ? 0 : 1;
    }
    if (v[0] == '0') return 0;
    if (!strcmp(v, "always") || v[0] == '2') return 2;
    return 1;
}

// The action counters (one process; nothing reads them but the log line and the tests).
struct Counters { unsigned tags, notice, stamp, syncText, reportEntry, versionText; };
inline Counters& Count() { static Counters c = {0, 0, 0, 0, 0, 0}; return c; }

// Entry-point labels stay clean (owner 26 Nov: the one-time beta popup is enough, no redundant BETA tag on every button).
inline const char* Tag(const char* label) {
    return label;
}

// ---- the texts (English; every sentence is a row of np_text_strings.tsv in Spanish and French) ----------------------------------------------------------------
inline const char* NoticeText() { return "Multiplayer is in beta. If something goes wrong, the game will offer to send a problem report. Thank you!"; }
inline const char* ResultLine() { return "Something wrong? Choose REPORT A PROBLEM in the Multiplayer menu."; }
inline const char* SyncErrorText() {
    return "The two Vitas got out of step, so this match ends here. Sorry! The next time you open Multiplayer, the game will offer to send a problem report.";
}
inline const char* OtherBuildText() { return "The other Vita has another version of the game, so the match could not start. Update both Vitas to the latest version."; }
inline const char* ReportButton() { return "REPORT A PROBLEM"; }
// The offer after a session that ended badly: why it is offered (the last run closed inside a session / an online game / a local one).
inline const char* OfferLead(bool crashed, bool online) {
    if (crashed) return "The game closed during your last multiplayer game.";
    return online ? "Your last online game ended with a problem." : "Your last multiplayer game ended with a problem.";
}
inline const char* OfferTail() { return "Send a report to help fix it? You will see what is sent first."; }
// No online account on this Vita: a report cannot be sent, so it is written to a file the player can attach to a message.
inline const char* SavedInstead() {
    return "No online account on this Vita yet, so the report cannot be sent. It was saved as ux0:data/mscharged/mp-report.txt. To send reports, play online once.";
}
inline const char* NotSaved() { return "The report could not be saved."; }

}  // namespace NpMpBeta

#endif
