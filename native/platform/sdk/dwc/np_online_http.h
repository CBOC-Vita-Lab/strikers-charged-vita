// np_online_http.h: NpOnline::Api over the service's HTTPS/JSON protocol (server/PROTOCOL.md, version 1).
// The transport underneath is a small interface: np_online_https.cpp implements it on the game's TLS client (np_https.h); the host tests
// implement it on a loopback socket against the service's local fake. Everything here is non-blocking: Begin* starts the first HTTP request
// of an operation, Poll* advances the operation (the next HTTP request of a multi-step operation starts from inside Poll).
//
//   login   GET /v1/info -> (token ? GET /v1/me [-> PUT /v1/me when the typed name differs] : POST /v1/register)
//   time    GET /v1/time
//   put     POST /v1/rank/put        get   POST /v1/rank/get        result   POST /v1/matches/{id}/result
#ifndef NP_ONLINE_HTTP_H
#define NP_ONLINE_HTTP_H

#include <string>
#include <vector>

#include "platform/sdk/dwc/np_online_api.h"

namespace NpOnline {

struct HttpRequest {
    std::string method;                  // "GET" / "POST" / "PUT"
    std::string path;                    // "/v1/me", with the query
    std::vector<std::string> headers;    // "Name: value" lines (Authorization among them: never log these)
    std::string body;                    // JSON or empty
    uint32_t timeoutMs;
    uint32_t ioTimeoutMs;                // longest wait for any byte; 0 = the transport default (10 s). A held poll: its hold + a margin
    HttpRequest() : timeoutMs(0), ioTimeoutMs(0) {}
};

struct HttpResponse {
    Fail fail;           // Fail::None: an HTTP answer arrived (status, body); else the request did not get an answer
    int status;
    std::string body;
    std::string detail;  // one line for the log when fail != None ("certificate is not for this host name")
    std::string advice;  // a line for the player ("Set your Vita's date and time."), or ""
    HttpResponse() : fail(Fail::None), status(0) {}
};

class HttpTransport {
public:
    virtual ~HttpTransport() {}
    virtual uint32_t Start(const HttpRequest& request) = 0;               // 0: cannot start (too many requests, no TLS)
    virtual bool Poll(uint32_t id, HttpResponse* response) = 0;           // true when finished (the id is released)
    virtual void Cancel(uint32_t id) = 0;
};

// Takes ownership of the transport.
Api* CreateHttpApi(HttpTransport* transport);

}  // namespace NpOnline

#endif
