// Signaling over HTTPS to the online service (server/PROTOCOL.md section 4, "Invite rooms and signaling"). One Signal object per room.
//
//   Host()  : GET /v1/info (reachability, kill switch, server clock, STUN list), POST /v1/rooms
//   Join()  : GET /v1/info, POST /v1/rooms/join {"code"}
//   in the room: GET /v1/rooms/{code}?v=<version>&wait=<sec> (held by the service: answered when the room changes or after 12 s, 45 s once the match
//            started; an old service ignores wait and the poll comes every 1.7 s / 4 s as before; the service may stretch everything with `pollScale`), POST .../endpoint when the candidates change, POST .../punch with each attempt's verdict, POST .../started once
//            the link is confirmed both ways, POST .../leave when the session ends.
// Every request is retried with the protocol's backoff (1, 2, 4, 8 s, then 15 s) for network failures and 5xx; 429 waits `retryAfterSec`; other
// 4xx end the attempt with a sentence for the player. Nothing blocks: requests run through np_http (worker threads), Poll() moves results over.
#ifndef NP_NET_SIGNAL_HTTP_H
#define NP_NET_SIGNAL_HTTP_H

#include <string>

#include "platform/sdk/net/online/np_signal.h"

namespace NpNet {

struct HttpSignalConfig {
    char baseUrl[320];          // "https://host" (no trailing slash); several, comma separated, are tried in turn when one does not answer
    char token[64];             // the account's bearer token (POST /v1/register, kept in the save file)
    char appVersion[24];        // X-Msc-App: the game version "1.2.0"
    char build[72];             // X-Msc-Build: hex build / rule fingerprint hash, "" = none
    uint32_t pollMs;            // room poll interval while the match has not started (the protocol asks for 1.5 to 2 s)
    uint32_t pollStartedMs;     // ... once it has (must stay under the 5 s after which the server shows the member as quiet)
    uint32_t holdSec;           // held polls (PROTOCOL.md "Held polls"): the poll asks the service to answer when the room changes or after this long; 0 = plain polls
    uint32_t holdStartedSec;    // ... once the match has started
    uint32_t heldGapMs;         // the pause after an answer that was held (the service said `heldMs`)
    uint32_t requestTimeoutMs;
    int maxAttempts;            // for the requests that decide whether the room exists (info, create, join)
    HttpSignalConfig() : pollMs(1700), pollStartedMs(4000), holdSec(12), holdStartedSec(45), heldGapMs(250), requestTimeoutMs(10000), maxAttempts(3) {
        baseUrl[0] = token[0] = appVersion[0] = build[0] = 0;
    }
};

Signal* CreateHttpSignal(const HttpSignalConfig& config);

// The sentence for a relay (POST /v1/turn) refusal: error code of the service and its message. Shared with the test fakes.
std::string TurnSentence(const std::string& code, const std::string& message);

}  // namespace NpNet

#endif
