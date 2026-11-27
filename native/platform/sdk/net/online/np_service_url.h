// The online service the Vita builds use when nothing else names one. Order of use: MSC_ONLINE_URL (np-player.txt or the environment), then servers.json
// (the menu flow), then this (strikers.psvita.dev, with a fallback to the old workers.dev address: np_service_hosts.h). A build can choose another with -DNP_ONLINE_URL='"https://..."', or none with -DNP_ONLINE_URL_NONE (online play then needs
// MSC_ONLINE_URL). Host builds have no default, so tests never reach a live service by accident.
#ifndef NP_SERVICE_URL_H
#define NP_SERVICE_URL_H

#if !defined(NP_ONLINE_URL) && !defined(NP_ONLINE_URL_NONE) && (defined(NP_VITA) || defined(__vita__))
#define NP_ONLINE_URL "https://strikers.psvita.dev"   // the same Worker as the old address; np_service_hosts.h falls back to it, MSC_ONLINE_HOST=old / new forces either (unset = auto)
#endif

#ifdef NP_ONLINE_URL
#include <string>
#include "platform/sdk/net/online/np_service_hosts.h"
// The build's address with the MSC_ONLINE_HOST switch applied (old = the workers.dev address instead of the new one).
inline std::string NpServiceBuildUrl() { return NpNet::ServiceHosts::ApplyMode(NP_ONLINE_URL); }
#endif

#include <stdlib.h>
#include <string>

// The console talks TLS or nothing: only https:// service addresses. A host test build takes the same rule with -DNP_ONLINE_CONSOLE_URL_RULES=1
// (tools/console-1.2/online_host_proof.py --tls), so the host proofs run the console's branches.
#if defined(NP_TARGET_VITA) || defined(NP_ONLINE_CONSOLE_URL_RULES)
#define NP_ONLINE_HTTPS_ONLY 1
#endif

// The ONE service address of Online Central: the sign-in and every request after it (np_online_https.cpp) and the ranked / quick queue with its room
// (np_dwc_match.cpp) ask here, so they can never name different services. MSC_ONLINE_URL (np-player.txt / environment; the first of a list), else
// MSC_NET_SERVER (the desktop tests' local fake), else the build's address with MSC_ONLINE_HOST applied. No trailing slash. Empty = no service; under the
// console's rule anything that is not https:// is empty. The move between the two service hosts happens per request, below this (np_service_hosts.h Route /
// ShouldFallBack in np_http.cpp and np_online_https.cpp), which is why both callers must hand over the same address.
inline std::string NpOnlineCentralBase() {
    const char* list = getenv("MSC_ONLINE_URL");
    if (!list || !*list) list = getenv("MSC_NET_SERVER");
    std::string s = list ? list : "";
#ifdef NP_ONLINE_URL
    if (s.empty()) s = NpServiceBuildUrl();
#endif
    const size_t comma = s.find(',');
    if (comma != std::string::npos) s.resize(comma);
    while (!s.empty() && s[0] == ' ') s.erase(0, 1);
    while (!s.empty() && (s[s.size() - 1] == '/' || s[s.size() - 1] == ' ')) s.resize(s.size() - 1);
    const bool https = s.compare(0, 8, "https://") == 0 && s.size() > 12;
#if defined(NP_ONLINE_HTTPS_ONLY)
    if (!https) s.clear();
#else
    if (!https && !(s.compare(0, 7, "http://") == 0 && s.size() > 10)) s.clear();
#endif
    return s;
}

#endif  // NP_SERVICE_URL_H
