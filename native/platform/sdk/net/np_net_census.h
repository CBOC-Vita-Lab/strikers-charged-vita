// np_net_census.h (wave 13, netclient13, O9, MSC_NET_CENSUS): how many sockets this process holds open, counted where they are made and closed, so a console
// log can say "[net] sockets open N" at the end of the online screens and at game exit BEFORE anyone blames the game for the Vita's FTP wedge (the FTP of the
// vitacompanion plug-in wedged after ranked searches on 17 Nov, and Galaxy saw the same wedge after ~400 FTP transfers with no leak at all: its fix was fewer
// FTP calls and a reboot every ten arms).
//   Opened(kind) / Closed(kind): the two UDP sockets the game makes (the online transport's, np_udp_vita.cpp / np_udp_posix.cpp; the Same Wi-Fi link's,
//   vlink_vita.cpp / vlink_udp.cpp). The HTTPS client keeps its own count (np_https_sys.cpp, np_https_socket_stats): it is read through weak references, so a
//   test build without np_https counts only the UDP sockets.
//   Count(): everything now; Log(where): the log line, only with MSC_NET_CENSUS=1 (default OFF, a diagnostic); returns the count either way.
// Header only, no platform types (tests/net/socket_census_test.cpp). Counters are atomic: the HTTPS worker and the game thread both open sockets.
#ifndef NP_NET_CENSUS_H
#define NP_NET_CENSUS_H

#include <stdio.h>
#include <stdlib.h>

extern "C" void np_https_socket_stats(int* live, int* opened, int* closed, int* refused) __attribute__((weak));
extern "C" int np_https_epolls_live(void) __attribute__((weak));

namespace NpNetCensus {

enum Kind { OnlineUdp = 0, LanUdp = 1, kKinds = 2 };

struct Book {
    int live[kKinds];
    unsigned opened[kKinds], closed[kKinds];
};
inline Book& Ledger() { static Book b = {{0, 0}, {0, 0}, {0, 0}}; return b; }
inline void Opened(Kind k) { Book& b = Ledger(); __atomic_add_fetch(&b.live[k], 1, __ATOMIC_RELAXED); __atomic_add_fetch(&b.opened[k], 1u, __ATOMIC_RELAXED); }
inline void Closed(Kind k) { Book& b = Ledger(); __atomic_sub_fetch(&b.live[k], 1, __ATOMIC_RELAXED); __atomic_add_fetch(&b.closed[k], 1u, __ATOMIC_RELAXED); }

struct Total {
    int onlineUdp, lanUdp, https, epolls;
    unsigned opened, closed;
    int Sockets() const { return onlineUdp + lanUdp + https; }
    int All() const { return Sockets() + epolls; }   // sockets, epoll ids and the one kept HTTPS connection (a socket): "N" of the log line
};
inline Total Count() {
    Total t;
    const Book& b = Ledger();
    t.onlineUdp = __atomic_load_n(&b.live[OnlineUdp], __ATOMIC_RELAXED);
    t.lanUdp = __atomic_load_n(&b.live[LanUdp], __ATOMIC_RELAXED);
    t.opened = __atomic_load_n(&b.opened[OnlineUdp], __ATOMIC_RELAXED) + __atomic_load_n(&b.opened[LanUdp], __ATOMIC_RELAXED);
    t.closed = __atomic_load_n(&b.closed[OnlineUdp], __ATOMIC_RELAXED) + __atomic_load_n(&b.closed[LanUdp], __ATOMIC_RELAXED);
    t.https = 0;
    t.epolls = 0;
    if (np_https_socket_stats) {
        int live = 0, opened = 0, closed = 0, refused = 0;
        np_https_socket_stats(&live, &opened, &closed, &refused);
        t.https = live;
        t.opened += unsigned(opened);
        t.closed += unsigned(closed);
    }
    if (np_https_epolls_live) t.epolls = np_https_epolls_live();
    return t;
}
inline bool On() {
    static const bool on = [] { const char* v = getenv("MSC_NET_CENSUS"); return v && v[0] == '1'; }();
    return on;
}
// "[net] sockets open N (...) at <where>": call it when the online screens are left and when the game exits. Returns N (computed even when the switch is off).
inline int Log(const char* where) {
    const Total t = Count();
    if (On())
        fprintf(stderr, "[net] sockets open %d (online udp %d, lan udp %d, https %d, epoll %d; %u opened, %u closed so far) at %s%s\n", t.All(), t.onlineUdp, t.lanUdp, t.https, t.epolls,
                t.opened, t.closed, where, t.All() ? " (expected 0 unless a kept connection or a session is still up)" : "");
    return t.All();
}

}  // namespace NpNetCensus

#endif
