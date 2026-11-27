// The two addresses of the online service and the rule that moves between them (strikers.psvita.dev first, the old workers.dev address as the fallback).
// Both names are the SAME Worker with the same data, so an account made through one works on the other.
//
// What falls back, and what never does:
//  - Only a failure BEFORE any request byte left the console moves a request to the other host: the name did not resolve (DNS), the TCP connect failed (CONNECT),
//    the TLS handshake failed for a reason that is not the certificate (TLS_HANDSHAKE), or a time limit ran out while still resolving / connecting / handshaking.
//  - An HTTP answer from the new name that is NOT from our service (a Cloudflare challenge, rate-limit, block or 52x page: the zone answered, the request never
//    reached our Worker, so sending it again to the old address is safe) counts like a pre-send failure: the new host is marked down and the request is resent once
//    to the old address (one log line "[np-net] service host: answered by the zone (HTTP N), trying the other address"). "From our service" = the Worker's
//    x-msc-proto response header, or, when the TLS layer does not hand response headers over (np_https.h NpHttpsResult has none: today's case), a body whose first
//    non-space byte is '{' (the Worker always answers JSON; server/src/core/app.js sets the header on success answers only, the JSON error answers are told by the body).
//    An answer from our service, any status (our own 429 / 503 JSON included), is final.
//  - A refused certificate (CERT), a wrong console clock (CLOCK), no network (OFFLINE), a failure after the request was sent, and a REDIRECT are
//    reported as they are. The fallback is also fully verified TLS (roots, host name, dates: np_https.cpp has no way to turn that off); nothing here touches it.
//  - Both hosts failing gives the error of the fallback host, which is the error the player saw before this existed.
//  - After a failure (or a zone answer) the new host is skipped for kRetryAfterMs (per run of the game, never stored), so one dead name costs one try, not one per request.
// Switch MSC_ONLINE_HOST (np-player.txt or the environment): auto (DEFAULT; unset or any unknown value) = new host first with the fallbacks above; old = only the old
// host, and a URL naming the new host is sent to the old one; new = only the new host, no fallback. An address the player names with MSC_ONLINE_URL is used as
// written (no fallback unless it is the new host in auto mode).
#ifndef NP_SERVICE_HOSTS_H
#define NP_SERVICE_HOSTS_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#if defined(__has_include)
#if __has_include("platform/sdk/net/np_https.h")
#include "platform/sdk/net/np_https.h"
#define NP_SERVICE_HOSTS_HAVE_HTTPS 1
#endif
#endif

namespace NpNet {
uint64_t SteadyNowMs(void*);   // np_net.h: CLOCK_MONOTONIC / sceKernelGetProcessTimeWide
namespace ServiceHosts {

enum Mode { MODE_AUTO = 0, MODE_NEW = 1, MODE_OLD = 2 };
const uint32_t kNewHostConnectMs = 6000;   // connect + handshake limit on the new address (the usual limit is 10 s) so the fallback has time left
const uint32_t kRetryAfterMs = 300000;   // how long the new host is skipped after it failed before the connection

#ifdef NP_SERVICE_HOSTS_TEST   // host tests only: local stand-ins for the two addresses and a clock
struct Test { const char* newBase; const char* oldBase; uint64_t (*clock)(void); };
inline Test& TestState() { static Test t = {0, 0, 0}; return t; }
#endif

inline const char* NewBase() {
#ifdef NP_SERVICE_HOSTS_TEST
    if (TestState().newBase) return TestState().newBase;
#endif
    return "https://strikers.psvita.dev";
}
inline const char* OldBase() {
#ifdef NP_SERVICE_HOSTS_TEST
    if (TestState().oldBase) return TestState().oldBase;
#endif
    return "https://msc-online.cboc-vita.workers.dev";
}

inline Mode GetMode() {
    const char* e = getenv("MSC_ONLINE_HOST");
    if (e && !strcmp(e, "new")) return MODE_NEW;
    if (e && !strcmp(e, "old")) return MODE_OLD;
    return MODE_AUTO;   // the default: new address first, with the fallbacks (see the top of this file)
}

// The build's compiled address with the switch applied: the stock new address becomes the old one under an explicit MSC_ONLINE_HOST=old. Any other address is kept.
inline std::string ApplyMode(const char* compiled) {
    std::string s = compiled ? compiled : "";
    if (s == NewBase() && GetMode() == MODE_OLD) return OldBase();
    return s;
}

inline bool HasPrefix(const std::string& url, const char* base) {
    const size_t n = strlen(base);
    return url.size() >= n && url.compare(0, n, base) == 0 && (url.size() == n || url[n] == '/' || url[n] == '?');
}

inline uint64_t Now() {
#ifdef NP_SERVICE_HOSTS_TEST
    if (TestState().clock) return TestState().clock();
#endif
    return NpNet::SteadyNowMs(0);
}

// When the new host last failed before the connection (stored as the time plus one; 0 = never / forgotten). One value for the whole run; the game thread and the DWC thread
// may both ask, a stale read only costs one extra try.
inline volatile uint64_t& DownAt() { static volatile uint64_t t = 0; return t; }
inline void Reset() { DownAt() = 0; }

// True when this URL is on the new host and the switch allows the fallback.
inline bool FallbackApplies(const std::string& url) { return GetMode() == MODE_AUTO && HasPrefix(url, NewBase()); }

inline std::string SwapToOld(const std::string& url) { return std::string(OldBase()) + url.substr(strlen(NewBase())); }

// The address a new request starts with: the old host while the new one is known to be down (and the retry time has not come), else as given.
inline std::string Route(const std::string& url) {
    if (GetMode() == MODE_OLD && HasPrefix(url, NewBase())) return SwapToOld(url);   // explicit old never reaches the new name (servers.json may name it)
    if (!FallbackApplies(url)) return url;
    const uint64_t at = DownAt();
    if (at && Now() + 1 - at < kRetryAfterMs) return SwapToOld(url);
    return url;
}

// A request that went to the new host ended with `error` (an NpHttpsError, 0 = an answer) at `stage` (an NpHttpsStage). True = send it again to
// SwapToOld(url). Only a failure before any request byte was sent qualifies (see the top of this file).
inline bool PreSendFailure(int error, int stage) {
#ifdef NP_SERVICE_HOSTS_HAVE_HTTPS
    switch (error) {
    case NP_HTTPS_E_DNS: case NP_HTTPS_E_CONNECT: case NP_HTTPS_E_TLS_HANDSHAKE: return true;
    case NP_HTTPS_E_TIMEOUT: return stage >= NP_HTTPS_S_QUEUED && stage <= NP_HTTPS_S_TLS;
    default: return false;
    }
#else
    (void)error; (void)stage;
    return false;
#endif
}

// True when an answer body looks like our Worker's: first non-space byte '{' (it always answers JSON). A challenge / rate-limit / block page (HTML, text, empty) is not.
inline bool BodyIsOurs(const void* body, size_t len) {
    const unsigned char* b = static_cast<const unsigned char*>(body);
    for (size_t i = 0; b && i < len; ++i) {
        if (b[i] == ' ' || b[i] == '\t' || b[i] == '\r' || b[i] == '\n') continue;
        return b[i] == '{';
    }
    return false;
}

// Called for every finished request. `callerUrl` is the URL as the caller wrote it (before Route); returns true when the caller must resend it
// to SwapToOld(callerUrl). Also keeps the memo: a failure or a zone answer marks the new host down, an answer from our service clears the mark.
// error 0 = an HTTP answer arrived (any status, `body` / `bodyLen` is its body); that is final only when it is from our service (BodyIsOurs).
inline bool ShouldFallBack(const std::string& callerUrl, bool wentToNew, int error, int stage, const void* body = 0, size_t bodyLen = 0) {
    if (!wentToNew || !FallbackApplies(callerUrl)) return false;
    if (error == 0) {
        if (BodyIsOurs(body, bodyLen)) { DownAt() = 0; return false; }
        DownAt() = Now() + 1;   // the zone answered, not our Worker
        return true;
    }
    if (!PreSendFailure(error, stage)) return false;
    DownAt() = Now() + 1;   // (stored plus one: 0 means "never")
    return true;
}

}  // namespace ServiceHosts
}  // namespace NpNet
#endif
