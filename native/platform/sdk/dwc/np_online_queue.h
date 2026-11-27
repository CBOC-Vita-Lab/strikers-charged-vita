// np_online_queue.h: the persistent retry queue for what a player has earned online: score submissions (DWC_RnkPutScoreAsync) and match
// result reports (the matchmaking layer). An entry is written to <data_dir>/online_queue.dat BEFORE the first send and removed only when the
// service accepted it, so a suspend, a crash or a lost connection cannot lose it: the next login flushes what is left. Retries back off
// 1, 2, 4, 8 s, then every 15 s; an entry the service refuses for good (4xx other than 401/429, an unreadable answer) is dropped, one that
// is still failing after 24 attempts or older than 24 hours is dropped. Puts are keyed by (category, region): a newer total replaces an older
// one that has not been delivered (the service wants the latest totals); results by match id.
// Results are sent first, then polled (GET /v1/matches/{id}, every 2 s) until the service calls the match final; only then do puts go out.
// Each entry remembers the matchId, the attempts, the build that produced it and the mode (headers X-Msc-Attempt / X-Msc-Build / X-Msc-Mode).
#ifndef NP_ONLINE_QUEUE_H
#define NP_ONLINE_QUEUE_H

#include "platform/sdk/dwc/np_online_api.h"

namespace NpOnline {

// Adds (or replaces) an entry and persists it. Returns its id (> 0), or 0 when it could not be stored (the caller may still send it once).
uint32_t QueuePut(const PutRequest& request);
// For the matchmaking layer: report the result of a match (it is sent by the pump after login, and retried until accepted or refused).
// Returns false when the arguments are unusable (empty match id, scores outside 0..99).
bool QueueResult(const ResultReport& report);

// The sender of an entry (the DWC ranking client for the put it started itself) tells how it went. `fail` is Fail::None on success.
void QueueFinished(uint32_t id, Fail fail);
bool QueueIsRetryable(Fail fail);

// Game thread, once per frame (DWC_ProcessFriendsMatch): sends the entries that are due, one request at a time, when a login succeeded in
// this run and the ranking client has nothing in flight (`busy`).
void QueuePump(bool rankingBusy);
// A login just succeeded: everything waiting is due now.
void QueueKick();

// PROTOCOL.md section 8: "submit the result, wait until the match is final, THEN put the new totals". False while a reported match is not
// settled yet (or its report is still waiting to go out): the ranking client then holds a put back instead of sending totals that would be
// clamped. A result the service cannot settle stops holding after 5 minutes.
bool QueueMayPut();

uint32_t QueuePending();                    // entries waiting
bool QueueDescribe(uint32_t index, char* out, size_t size);   // "put cat=0 region=2 attempts=3" (diagnostics, no secrets)
void QueueForget();                         // the account is forgotten: the memory state and online_queue.dat with every .tmp / .bak / .bad copy are gone
void QueueResetForTest(bool keepFile);      // forget the memory state; the file stays when keepFile (a restart), else it is deleted

}  // namespace NpOnline

#endif
