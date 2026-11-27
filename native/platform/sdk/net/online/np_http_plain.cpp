// Plain HTTP/1.1 for the desktop test builds (the local fake of the online service). One detached thread per request: connect, send,
// read the answer (Content-Length, chunked or until close), with time limits. Not built for the Vita.
#include "platform/sdk/net/online/np_http.h"

#if !defined(NP_TARGET_VITA)

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

namespace NpNet {
namespace {

uint64_t MonoMs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000 + uint64_t(ts.tv_nsec) / 1000000;
}

// One request. state: 0 running, 1 finished (the owner takes the answer and deletes it), 2 abandoned (the thread deletes it).
struct Req {
    volatile int state;
    std::string method, host, path, body, extra;
    uint16_t port;
    uint32_t timeoutMs;
    HttpResponse res;
    Req() : state(0), port(80), timeoutMs(10000) {}
};

bool WaitFd(int fd, short events, uint64_t deadline) {
    for (;;) {
        const uint64_t now = MonoMs();
        if (now >= deadline) return false;
        pollfd p;
        p.fd = fd;
        p.events = events;
        p.revents = 0;
        const int rc = poll(&p, 1, int(deadline - now));
        if (rc > 0) return true;
        if (rc < 0 && errno != EINTR) return false;
    }
}

void Fail(Req* r, const char* text) {
    r->res.error = 1;
    r->res.status = 0;
    r->res.text = text;
}

void Run(Req* r) {
    const uint64_t deadline = MonoMs() + r->timeoutMs;
    addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = 0;
    char portText[8];
    snprintf(portText, sizeof portText, "%u", unsigned(r->port));
    if (getaddrinfo(r->host.c_str(), portText, &hints, &res) != 0 || !res) { Fail(r, "The server name could not be found."); return; }
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { freeaddrinfo(res); Fail(r, "No socket."); return; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    int rc = ::connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc != 0 && errno != EINPROGRESS) { ::close(fd); Fail(r, "The server refused the connection."); return; }
    if (rc != 0) {
        int err = 0;
        socklen_t l = sizeof err;
        if (!WaitFd(fd, POLLOUT, deadline) || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &l) != 0 || err) { ::close(fd); Fail(r, "Could not connect to the server."); return; }
    }
    std::string req = r->method + " " + r->path + " HTTP/1.1\r\nHost: " + r->host + "\r\nConnection: close\r\nAccept-Encoding: identity\r\n";
    req += r->extra;
    if (!r->body.empty() || r->method == "POST" || r->method == "PUT") {
        char cl[64];
        snprintf(cl, sizeof cl, "Content-Length: %u\r\n", unsigned(r->body.size()));
        req += cl;
    }
    req += "\r\n";
    req += r->body;
    size_t sent = 0;
    while (sent < req.size()) {
        if (!WaitFd(fd, POLLOUT, deadline)) { ::close(fd); Fail(r, "The server did not accept the request in time."); return; }
        const ssize_t n = ::send(fd, req.data() + sent, req.size() - sent, MSG_NOSIGNAL);
        if (n < 0 && errno != EAGAIN && errno != EINTR) { ::close(fd); Fail(r, "The connection to the server broke."); return; }
        if (n > 0) sent += size_t(n);
    }
    std::string in;
    char buf[4096];
    bool closed = false;
    size_t headEnd = std::string::npos;
    long contentLength = -1;
    bool chunked = false;
    for (;;) {
        if (headEnd == std::string::npos) {
            headEnd = in.find("\r\n\r\n");
            if (headEnd != std::string::npos) {
                std::string head = in.substr(0, headEnd);
                for (size_t i = 0; i < head.size(); ++i) head[i] = char(tolower(static_cast<unsigned char>(head[i])));
                size_t p = head.find("content-length:");
                if (p != std::string::npos) contentLength = atol(head.c_str() + p + 15);
                chunked = head.find("transfer-encoding: chunked") != std::string::npos;
                headEnd += 4;
            }
        }
        if (headEnd != std::string::npos) {
            if (!chunked && contentLength >= 0 && in.size() - headEnd >= size_t(contentLength)) break;
            if (chunked && in.size() >= 5 && in.compare(in.size() - 5, 5, "0\r\n\r\n") == 0) break;
        }
        if (closed) break;
        if (in.size() > 256 * 1024) { ::close(fd); Fail(r, "The answer is too large."); return; }
        if (!WaitFd(fd, POLLIN, deadline)) { ::close(fd); Fail(r, "The server did not answer in time."); return; }
        const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
        if (n == 0) closed = true;
        else if (n < 0) { if (errno != EAGAIN && errno != EINTR) closed = true; }
        else in.append(buf, size_t(n));
    }
    ::close(fd);
    if (headEnd == std::string::npos || in.compare(0, 5, "HTTP/") != 0) { Fail(r, "The server's answer was not understood."); return; }
    const size_t sp = in.find(' ');
    r->res.status = sp == std::string::npos ? 0 : atoi(in.c_str() + sp + 1);
    std::string body = in.substr(headEnd);
    if (chunked) {
        std::string out;
        size_t p = 0;
        while (p < body.size()) {
            const size_t e = body.find("\r\n", p);
            if (e == std::string::npos) break;
            const long len = strtol(body.c_str() + p, 0, 16);
            if (len <= 0) break;
            p = e + 2;
            if (p + size_t(len) > body.size()) break;
            out.append(body, p, size_t(len));
            p += size_t(len) + 2;
        }
        body.swap(out);
    } else if (contentLength >= 0 && body.size() > size_t(contentLength)) {
        body.resize(size_t(contentLength));
    }
    r->res.body.swap(body);
}

void* Thread(void* arg) {
    Req* r = static_cast<Req*>(arg);
    Run(r);
    int expected = 0;
    if (!__atomic_compare_exchange_n(&r->state, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) delete r;   // abandoned
    return 0;
}

}  // namespace

// Called by np_http.cpp for http:// URLs. Returns an opaque request or null.
void* PlainStart(const char* method, const std::string& url, const std::string& body, const std::vector<std::string>& headers, uint32_t timeoutMs, std::string* err) {
    if (url.compare(0, 7, "http://") != 0) { *err = "Not an http URL."; return 0; }
    Req* r = new Req;
    std::string rest = url.substr(7);
    const size_t slash = rest.find('/');
    std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
    r->path = slash == std::string::npos ? "/" : rest.substr(slash);
    const size_t colon = hostport.rfind(':');
    if (colon != std::string::npos) {
        r->port = uint16_t(atoi(hostport.c_str() + colon + 1));
        hostport.resize(colon);
    }
    r->host = hostport;
    r->method = method;
    r->body = body;
    for (size_t i = 0; i < headers.size(); ++i) r->extra += headers[i] + "\r\n";
    r->timeoutMs = timeoutMs ? timeoutMs : 10000;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 128 * 1024);
    pthread_t t;
    const int rc = pthread_create(&t, &attr, Thread, r);
    pthread_attr_destroy(&attr);
    if (rc != 0) { delete r; *err = "Could not start the request."; return 0; }
    pthread_detach(t);
    return r;
}

int PlainPoll(void* request, HttpResponse* out) {
    Req* r = static_cast<Req*>(request);
    if (__atomic_load_n(&r->state, __ATOMIC_ACQUIRE) != 1) return 0;
    *out = r->res;
    delete r;
    return 1;
}

void PlainCancel(void* request) {
    Req* r = static_cast<Req*>(request);
    int expected = 0;
    if (!__atomic_compare_exchange_n(&r->state, &expected, 2, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) delete r;   // it had finished
}

}  // namespace NpNet

#endif  // !NP_TARGET_VITA
