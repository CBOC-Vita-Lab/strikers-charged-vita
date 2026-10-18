// VLink POSIX UDP backend: host32 / armhf test builds (two game processes on 127.0.0.1, or real machines on a LAN).
// Not built for the Vita (vlink_vita.cpp has the Vita's backends).
#include "platform/sdk/net/vlink.h"

#if !defined(NP_TARGET_VITA)

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>
#include <vector>

#define VLINK_LOG(...) fprintf(stderr, "[vlink] " __VA_ARGS__)

namespace VLink {
namespace {

struct UdpState {
    Backend backend;
    int fd;
    uint16_t port;
    uint32_t vip;
    std::vector<sockaddr_in> targets;
    UdpState() : fd(-1), port(0), vip(0) { memset(&backend, 0, sizeof backend); }
};

bool ParseTargets(UdpState& b, const char* specText) {
    b.targets.clear();
    std::string spec = specText ? specText : "";
    if (spec.empty()) spec = "255.255.255.255:" + std::to_string(b.port);
    size_t start = 0;
    while (start <= spec.size()) {
        const size_t comma = spec.find(',', start);
        const std::string item = spec.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        start = comma == std::string::npos ? spec.size() + 1 : comma + 1;
        if (item.empty()) continue;
        const size_t colon = item.rfind(':');
        sockaddr_in t;
        memset(&t, 0, sizeof t);
        t.sin_family = AF_INET;
        const std::string host = colon == std::string::npos ? item : item.substr(0, colon);
        const int port = colon == std::string::npos ? b.port : atoi(item.c_str() + colon + 1);
        if (inet_pton(AF_INET, host.c_str(), &t.sin_addr) != 1 || port <= 0 || port > 65535) {
            VLINK_LOG("bad broadcast target '%s'\n", item.c_str());
            return false;
        }
        t.sin_port = htons(uint16_t(port));
        b.targets.push_back(t);
    }
    return !b.targets.empty();
}

bool UdpOpen(UdpState& b) {
    if (b.fd >= 0) return true;
    b.fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (b.fd < 0) return false;
    int one = 1;
    setsockopt(b.fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    sockaddr_in local;
    memset(&local, 0, sizeof local);
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(b.port);
    if (::bind(b.fd, reinterpret_cast<sockaddr*>(&local), sizeof local) != 0) {
        VLINK_LOG("cannot bind UDP port %u (errno %d)\n", b.port, errno);
        ::close(b.fd);
        b.fd = -1;
        return false;
    }
    fcntl(b.fd, F_SETFL, fcntl(b.fd, F_GETFL, 0) | O_NONBLOCK);
    return true;
}

uint32_t UdpLocalVip(void* ctx) {
    UdpState& b = *static_cast<UdpState*>(ctx);
    if (!UdpOpen(b)) return 0;
    return b.vip;
}

int UdpSend(void* ctx, const PeerAddr* to, bool broadcast, const void* data, size_t len) {
    UdpState& b = *static_cast<UdpState*>(ctx);
    if (!UdpOpen(b)) return -1;
    if (!broadcast && to) {
        sockaddr_in addr;
        memset(&addr, 0, sizeof addr);
        addr.sin_family = AF_INET;
        memcpy(&addr.sin_addr.s_addr, to->bytes, 4);
        memcpy(&addr.sin_port, to->bytes + 4, 2);
        return int(::sendto(b.fd, data, len, 0, reinterpret_cast<sockaddr*>(&addr), sizeof addr));
    }
    int best = -1;
    for (size_t i = 0; i < b.targets.size(); ++i) {
        const sockaddr_in& t = b.targets[i];
        // never to ourselves: a loopback target on our own port is this process
        if (t.sin_port == htons(b.port) && t.sin_addr.s_addr == htonl(INADDR_LOOPBACK)) continue;
        const ssize_t n = ::sendto(b.fd, data, len, 0, reinterpret_cast<const sockaddr*>(&t), sizeof t);
        if (n >= 0) best = int(n);
    }
    return best;
}

int UdpRecv(void* ctx, PeerAddr* from, void* buffer, size_t capacity) {
    UdpState& b = *static_cast<UdpState*>(ctx);
    if (b.fd < 0) return 0;
    sockaddr_in addr;
    socklen_t addrLen = sizeof addr;
    const ssize_t n = ::recvfrom(b.fd, buffer, capacity, 0, reinterpret_cast<sockaddr*>(&addr), &addrLen);
    if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
    memset(from, 0, sizeof *from);
    memcpy(from->bytes, &addr.sin_addr.s_addr, 4);
    memcpy(from->bytes + 4, &addr.sin_port, 2);
    return int(n);
}

void UdpClose(void* ctx) {
    UdpState& b = *static_cast<UdpState*>(ctx);
    if (b.fd >= 0) ::close(b.fd);
    b.fd = -1;
}

}  // namespace

Backend* CreateUdpBackend(uint16_t bindPort, const char* broadcastTargets, uint32_t vip) {
    UdpState* b = new UdpState;
    b->port = bindPort ? bindPort : 41000;
    if (vip == 0) {
        static uint32_t counter = 0;  // process id, time and a counter: unique enough between two consoles
        uint32_t x = uint32_t(getpid()) * 2654435761u ^ uint32_t(time(0)) * 40503u ^ (++counter * 2246822519u);
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        vip = 0x0A000000u | (x & 0x00FFFFFEu) | 1u;  // 10.x.y.z, never .0
    }
    b->vip = vip;
    if (!ParseTargets(*b, broadcastTargets)) {
        delete b;
        return 0;
    }
    b->backend.name = "udp";
    b->backend.ctx = b;
    b->backend.local_vip = UdpLocalVip;
    b->backend.send = UdpSend;
    b->backend.recv = UdpRecv;
    b->backend.close = UdpClose;
    return &b->backend;
}

Backend* CreateUdpBackendFromEnvironment() {
    const char* port = getenv("MSC_LINK_PORT");
    const char* targets = getenv("MSC_LINK_BCAST");
    const char* vipText = getenv("MSC_LINK_VIP");
    uint32_t vip = 0;
    if (vipText && *vipText) {
        in_addr a;
        if (inet_pton(AF_INET, vipText, &a) == 1) vip = ntohl(a.s_addr);
    }
    Backend* backend = CreateUdpBackend(port ? uint16_t(atoi(port)) : 41000, targets, vip);
    // MSC_LINK_MFS=<bytes>: pretend the medium carries at most this many bytes per datagram (the Vita's ad hoc PDP: 1444), so that a desktop run
    // exercises the link's fragmentation with the real game (tools/mp/turn.sh --env MSC_LINK_MFS=700). Test only.
    if (backend)
        if (const char* mfs = getenv("MSC_LINK_MFS"))
            if (atoi(mfs) > int(kFragHeader) + 16) backend->maxFrame = size_t(atoi(mfs));
    return backend;
}

void DestroyUdpBackend(Backend* backend) {
    if (!backend) return;
    UdpState* b = static_cast<UdpState*>(backend->ctx);
    if (b->fd >= 0) ::close(b->fd);
    delete b;
}

}  // namespace VLink

#else  // NP_TARGET_VITA

namespace VLink {
Backend* CreateUdpBackend(uint16_t, const char*, uint32_t) { return 0; }
Backend* CreateUdpBackendFromEnvironment() { return 0; }
void DestroyUdpBackend(Backend*) {}
}  // namespace VLink

#endif
