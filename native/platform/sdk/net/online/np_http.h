// The HTTP client the signaling uses: asynchronous, poll-based, one small request at a time per caller; nothing here blocks the game thread.
//
//   https://   -> np_https (platform/sdk/net/np_https.h: mbedtls, our own roots, certificate checks always on). Compiled in when that header
//                 exists (NP_HAVE_NPHTTPS); without it an https URL fails at once with a sentence.
//   http://    -> a plain POSIX client for the desktop test builds (the local fake server of server/fake). Never compiled for the Vita:
//                 the console talks TLS or nothing.
//
// Requests are numbered; HttpPoll() answers 0 while pending and 1 once (the answer is then handed over and the number is free). HttpFire()
// starts a request nobody will ask about (the leave message): HttpPump() (called every frame by NpNetPoll) reaps it.
#ifndef NP_NET_HTTP_H
#define NP_NET_HTTP_H

#include <stdint.h>

#include <string>
#include <vector>

namespace NpNet {

struct HttpResponse {
    int error;            // 0 = an HTTP answer arrived (status says which); else a transport failure (text says why)
    int status;           // HTTP status
    std::string body;
    std::string text;     // a sentence for the player when error != 0 ("Set your Vita's date and time.")
    HttpResponse() : error(0), status(0) {}
};

// Starts a request. `headers` are "Name: value" lines. Returns a handle > 0, or 0 with *err set.
int HttpStart(const char* method, const std::string& url, const std::string& body, const std::vector<std::string>& headers, uint32_t timeoutMs,
              std::string* err);
// The same with the longest wait for any byte spelled out (ioTimeoutMs; 0 = the HTTPS layer's 10 s). A held poll passes its hold plus a margin
// (HeldIoMs), so the service may keep the request open without data for the whole hold; ordinary requests keep the short no-data limit.
int HttpStart(const char* method, const std::string& url, const std::string& body, const std::vector<std::string>& headers, uint32_t timeoutMs,
              uint32_t ioTimeoutMs, std::string* err);
inline uint32_t HeldIoMs(uint32_t holdMs) { return holdMs + 10000; }
int HttpPoll(int handle, HttpResponse* out);     // 0 pending, 1 finished (handed over), -1 not a live handle
void HttpCancel(int handle);                      // gives the handle up (a running request ends by itself)
void HttpFire(const char* method, const std::string& url, const std::string& body, const std::vector<std::string>& headers, uint32_t timeoutMs);
void HttpPump();                                  // reaps fired requests; cheap

}  // namespace NpNet

#endif
