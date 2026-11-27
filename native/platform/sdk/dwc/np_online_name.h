// np_online_name.h: the public leaderboard name. Typed by the player the first time they pick Online (a Vita user name can be their
// real name, so it is only offered as a suggestion), validated, stored in <data_dir>/online_name.txt and shown on the rankings.
//
// Rules (the service's, PROTOCOL.md section 3, which the game's fonts and 10-character record also fit): 2..10 characters of
// A-Z a-z 0-9 space . _ -, starting and ending with a letter or digit, no double spaces, at least two letters or digits. The service
// additionally refuses reserved and filtered names and names that are taken; the game then shows its "name not allowed" message.
//
// NameRequest is a small state machine to call once per frame while it answers Pending:
//   Vita:  stored name -> Ready; otherwise the system keyboard (sceImeDialog) opens pre-filled with the Vita user name; Enter stores
//          the validated text (Ready), Cancel -> Cancelled (nothing stored; asked again next time).
//   host:  MSC_ONLINE_NAME (any build: it overrides, nothing stored), else the stored name, else "Player" (nothing stored).
#ifndef NP_ONLINE_NAME_H
#define NP_ONLINE_NAME_H

#include <stddef.h>

namespace NpOnline {

const size_t kMaxNameChars = 10;

// Cleans `in` into `out` (kMaxNameChars + 1 bytes): drops unsupported characters, collapses and trims spaces, cuts at 10 characters.
// Returns false (out = "") when nothing usable is left. `changed`, when given, tells whether `out` differs from `in`.
bool SanitizeName(const char* in, char* out, bool* changed = nullptr);
bool IsValidName(const char* name);               // SanitizeName(name) leaves it unchanged and non-empty

enum class NameState { Pending, Ready, Cancelled };

NameState NameRequest();                          // the current answer; starts the request the first time
const char* Name();                               // the name of the last Ready answer ("" before)
bool LoadStoredName(char* out);                   // out: kMaxNameChars + 1 bytes
bool StoreName(const char* name);
void ForgetName();                                // the next NameRequest asks again
void NameResetForTest();

}  // namespace NpOnline

// The platform's keyboard (np_online_ime_vita.cpp on the console). Absent on hosts: NameRequest then uses MSC_ONLINE_NAME.
//   NpOnlineImeOpen: start the dialog with an initial text and a title (ASCII). 0 = started.
//   NpOnlineImePoll: 0 running, 1 accepted (UTF-8 text in `out`), 2 cancelled, -1 failed.
//   NpOnlineImeSuggestion: the Vita user name (UTF-8), or "".
extern "C" int NpOnlineImeAvailable(void) __attribute__((weak));
extern "C" int NpOnlineImeOpen(const char* initial, const char* title) __attribute__((weak));
extern "C" int NpOnlineImePoll(char* out, size_t size) __attribute__((weak));
extern "C" const char* NpOnlineImeSuggestion(void) __attribute__((weak));

#endif
