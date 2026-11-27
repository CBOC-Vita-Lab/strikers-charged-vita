// np_dwc.h: what the DWC modules of this directory share (login, clock, ranking, errors, the user-data block, the stored credentials).
// The game never includes this; the matchmaking layer may (pump hook, error reporting, session state).
#ifndef NP_DWC_H
#define NP_DWC_H

#include <stddef.h>
#include <stdint.h>

#include "platform/sdk/dwc/np_online_api.h"

// ---- errors (np_dwc_error.cpp) ---------------------------------------------------------------------------------------------------
// The DWC "last error": DWC_GetLastErrorEx reads it, DWC_ClearError clears it. `lastError` is a DWCErrorType value (DWC_ERROR_*), `code`
// the number the game shows ("error code 20100" in its pop-ups, ranges in SHOnlineLogin.cpp's sOnlineErrorPopupRanges).
// A fatal error (DWC_ERROR_FATAL) is never overwritten or cleared, as in the original.
void NpDwcSetError(int lastError, int code);
void NpDwcClearErrorHook(void (*hook)(void));            // called by DWC_ClearError after the error is cleared (the ranking client re-arms)
// The last error and code for a failed request (ErrorFor does not set anything).
void NpDwcErrorFor(NpOnline::Fail fail, int* lastError, int* code);

// ---- the pump (np_dwc_login.cpp) -------------------------------------------------------------------------------------------------
// DWC_ProcessFriendsMatch runs the login machine, the clock refresh and these hooks, in registration order, on the game thread.
// The matchmaking layer registers its own per-frame work here instead of defining DWC_ProcessFriendsMatch a second time.
void NpDwcAddPump(void (*hook)(void));

// ---- the user-data block (np_dwc_account.cpp) -------------------------------------------------------------------------------------
// 64 bytes at the start of a save slot (DWCAccUserData): size, pseudo login id, authentic login id, profile id, flag, game code,
// 5 reserved words, CRC-32 of the first 60 bytes (little-endian words: the host's own memory).
const unsigned kUserDataBytes = 64;
bool NpDwcUserDataValid(const void* userdata);                       // size and CRC
bool NpDwcUserDataHasProfile(const void* userdata);                  // authentic login id present and profile id > 0
int32_t NpDwcUserDataProfile(const void* userdata);
void NpDwcUserDataSetProfile(void* userdata, int32_t profileId);      // authentic := pseudo (type 1), profile id, dirty flag, CRC
uint32_t NpDwcCrc32(const void* data, size_t size);
uint32_t NpDwcGameCode();
void NpDwcSetGameCode(uint32_t code);                                // DWC_Init's game code, stored into new blocks

// ---- the player's credentials (np_online_store.cpp) ------------------------------------------------------------------------------
// <data_dir>/online.dat: profile id and the per-player token (the service's 43-character bearer token) and a CRC. The token is a secret:
// it is never printed, logged or put into the save.
struct NpOnlineCredentials {
    int32_t profileId;
    char token[NpOnline::kTokenChars];   // the bearer token (text), NUL terminated
};
bool NpOnlineTokenValid(const char* token);                          // 16..63 characters of [A-Za-z0-9_-]
bool NpOnlineLoadCredentials(NpOnlineCredentials* out);
bool NpOnlineSaveCredentials(const NpOnlineCredentials& in);
void NpOnlineForgetCredentials();                                    // online.dat with its .tmp / .bak / .bad copies
void NpOnlineForgetAccount();                                        // all three account files (online.dat, online_name.txt, online_queue.dat), every copy of each

// ---- login state for the ranking client and the clock -------------------------------------------------------------------------------
bool NpDwcRankBusy();                                                // a ranking request is in flight (np_dwc_rank.cpp)
int32_t NpDwcLoggedInProfile();                                      // > 0 once a login succeeded in this run
// Server time. NpDwcClockSet is fed by the login reply and by refreshes; NpDwcClockNowMs is UTC ms since 1970 or -1 before the first
// sync.
void NpDwcClockSet(int64_t unixMs);
int64_t NpDwcClockNowMs();
bool NpDwcClockCivil(int64_t unixMs, int* year, int* month0, int* mday, int* hour, int* minute, int* second, int* msec, int* wday, int* yday);

// ---- test support ----------------------------------------------------------------------------------------------------------------
// Unranked quick match ("play with anyone" that does not count on the boards): the next DWC_ConnectToAnybodyAsync queues in the service's quick
// queue instead of the ranked one (POST /v1/ranked/queue {"mode":"quick"}). The match is an ordinary unranked one: no rating, no board, its
// result is recorded as unranked. The menu sets it before it starts matchmaking; it stays until set again.
void NpDwcSetQuickMatch(bool quick);
bool NpDwcQuickMatch();
bool NpDwcQuickMatchActive();     // the match in progress (or being made) is a quick match

void NpDwcResetForTest();           // login, clock, errors and ranking back to the state of a fresh process
void NpDwcErrorResetForTest();
void NpDwcRankResetForTest();

#endif
