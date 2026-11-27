// socket_census_test.cpp (wave 13, netclient13, O9): failing polls, run over and over, leave NO socket open, counted by the census the console log prints at the end
// of the online screens and at game exit ("[net] sockets open N", np_net_census.h): the HTTPS client's sockets and epolls, the online transport's UDP socket.
// No service and no python: the "servers" are listeners made here. The ways a held poll fails: nothing listening, accepted and silent (a blackhole), accepted and
// closed at once (a reset), a cancel in the middle, a release while running, and the UDP socket opened and closed again and again (also after a failed bind).
//   Run by tools/net/run_census_test.sh (needs the host32 mbedtls like tools/https/run_https_test.sh). -DMUTANT_LEAK keeps one socket open per round: must fail.
#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "platform/sdk/net/np_https.h"
#include "platform/sdk/net/np_net_census.h"
#include "platform/sdk/net/online/np_net.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { if (cond) { ++g_pass; } else { ++g_fail; printf("  FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int Fds() {
    int n = 0;
    if (DIR* d = opendir("/proc/self/fd")) { while (readdir(d)) ++n; closedir(d); }
    return n;
}
static int Listen(bool holdBacklogOnly, int* portOut) {
    const int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(s, 64) != 0) { perror("listen"); exit(2); }
    socklen_t l = sizeof a;
    getsockname(s, reinterpret_cast<sockaddr*>(&a), &l);
    *portOut = ntohs(a.sin_port);
    (void)holdBacklogOnly;
    return s;
}
// accepts and closes every connection at once (a reset for the client)
static volatile int g_stop = 0;
static void* Closer(void* arg) {
    const int ls = *static_cast<int*>(arg);
    while (!g_stop) {
        fd_set r;
        FD_ZERO(&r);
        FD_SET(ls, &r);
        timeval tv = {0, 20000};
        if (select(ls + 1, &r, 0, 0, &tv) > 0) { const int c = accept(ls, 0, 0); if (c >= 0) close(c); }
    }
    return 0;
}
static std::string Url(int port) { char b[96]; snprintf(b, sizeof b, "https://127.0.0.1:%d/v1/friends", port); return b; }

int main() {
    setenv("MSC_NET_CENSUS", "1", 1);
    printf("[socket census: failing polls repeated leave nothing open]\n");
    CHECK(NpNetCensus::Count().All() == 0, "the census starts at zero (%d)", NpNetCensus::Count().All());
    int deadPort = 0, holePort = 0, resetPort = 0;
    const int deadSock = Listen(false, &deadPort);
    close(deadSock);                                  // nothing listens there any more: connection refused
    const int holeSock = Listen(true, &holePort);     // never accepted: the connection completes in the kernel, then silence
    int resetSock = Listen(false, &resetPort);
    pthread_t th;
    pthread_create(&th, 0, Closer, &resetSock);
    const std::string dead = Url(deadPort), hole = Url(holePort), reset = Url(resetPort);
    np_https_shutdown(2000);
    const int fds0 = Fds();
    int bad = 0, worst = 0, leaked = 0;
    const int rounds = 120;
    for (int i = 0; i < rounds; ++i) {
        NpHttpsRequest rq;
        memset(&rq, 0, sizeof rq);
        rq.keep_alive = 1;
        rq.connect_timeout_ms = 120;
        rq.io_timeout_ms = 80;
        rq.total_timeout_ms = 3000;
        const int kind = i % 5;
        NpHttpsError e = NP_HTTPS_OK;
        if (kind == 0) rq.url = dead.c_str();
        else if (kind == 1 || kind == 4) rq.url = hole.c_str();
        else rq.url = reset.c_str();
        NpHttpsHandle h = np_https_start(&rq, &e);
        if (!h) { ++bad; continue; }
        NpHttpsResult r;
        if (kind == 4) {   // cancelled in the middle of the handshake wait
            usleep(30000 + (i % 4) * 10000);
            np_https_cancel(h);
        }
        int polls = 0;
        while (np_https_poll(h, &r) == 0 && ++polls < 2000) usleep(3000);
        if (r.error == NP_HTTPS_OK || r.error == NP_HTTPS_E_HTTP_STATUS) ++bad;
        np_https_release(h);
        // the UDP socket of the online transport: opened, closed, and opened again with a bound port refused in between
        NpNet::Udp* u = NpNet::CreatePlatformUdp();
        if (u) {
            if (u->Open(0)) u->Close();
            NpNet::Udp* v = NpNet::CreatePlatformUdp();
            if (v) { if (v->Open(0)) { /* closed by the destructor */ } delete v; }
            delete u;
        }
#if defined(MUTANT_LEAK)
        if (i % 40 == 0) { NpNet::Udp* l = NpNet::CreatePlatformUdp(); if (l) { l->Open(0); } leaked++; /* never closed */ }
#endif
        const int n = NpNetCensus::Count().All();
        if (n > worst) worst = n;
    }
    // a released request still runs to its end on the worker: one more request behind it, then the worker is idle
    {
        NpHttpsRequest rq;
        memset(&rq, 0, sizeof rq);
        rq.url = dead.c_str();
        rq.connect_timeout_ms = 500;
        NpHttpsError e = NP_HTTPS_OK;
        NpHttpsHandle h = np_https_start(&rq, &e);
        NpHttpsResult r;
        for (int t = 0; h && np_https_poll(h, &r) == 0 && t < 2000; ++t) usleep(3000);
        if (h) np_https_release(h);
    }
    np_https_shutdown(3000);
    const NpNetCensus::Total t = NpNetCensus::Count();
    const int n = NpNetCensus::Log("the test's online screens were left");   // prints the [net] line (MSC_NET_CENSUS=1)
    printf("  %d failing polls: %u sockets opened, %u closed, %d open now (sockets %d + epolls %d), most after a round %d; file descriptors %d -> %d\n", rounds, t.opened, t.closed, n,
           t.Sockets(), t.epolls, worst, fds0, Fds());
    CHECK(bad == 0, "%d polls did not fail the way they were meant to", bad);
    CHECK(t.opened >= 200, "the rounds opened sockets (%u)", t.opened);
    CHECK(n == 0, "the census returns to zero after the failing polls (%d open: udp %d + %d, https %d, epoll %d)", n, t.onlineUdp, t.lanUdp, t.https, t.epolls);
    CHECK(worst <= 3, "never more than three at once (the HTTPS connection, its epoll and a UDP socket): %d", worst);
    CHECK(t.opened == t.closed, "every socket opened was closed (%u / %u)", t.opened, t.closed);
    CHECK(Fds() == fds0, "the process holds as many file descriptors as before (%d -> %d)", fds0, Fds());
    (void)leaked;
    g_stop = 1;
    pthread_join(th, 0);
    close(resetSock);
    close(holeSock);
    printf("socket_census_test: %s (%d checks, %d failed)\n", g_fail ? "FAILED" : "PASS", g_pass + g_fail, g_fail);
    return g_fail ? 1 : 0;
}
