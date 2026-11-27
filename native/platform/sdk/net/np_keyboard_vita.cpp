// The Vita's keyboard for np_keyboard.h: the system keyboard (sceImeDialog) of platform/sdk/dwc/np_online_ime_vita.cpp, the one helper the leaderboard
// name uses too. It takes a title and an initial text; the length limit there is the name limit (10), which also fits an invite code (6).
#if defined(NP_TARGET_VITA)  // the Vita target only (the np Makefile compiles every platform/ source for every target)

#include "platform/sdk/net/np_keyboard.h"

extern "C" int NpOnlineImeAvailable(void);
extern "C" int NpOnlineImeOpen(const char* initial, const char* title);
extern "C" int NpOnlineImePoll(char* out, size_t size);
extern "C" const char* NpOnlineImeSuggestion(void);

extern "C" int NpKeyboardAvailable(void) { return NpOnlineImeAvailable(); }
extern "C" int NpKeyboardOpen(const char* title, const char* initial, int maxChars, int kind) {
    (void)maxChars;
    (void)kind;
    return NpOnlineImeOpen(initial, title);
}
extern "C" int NpKeyboardPoll(char* out, size_t size) { return NpOnlineImePoll(out, size); }
extern "C" const char* NpKeyboardSuggestion(void) { return NpOnlineImeSuggestion(); }

#endif
