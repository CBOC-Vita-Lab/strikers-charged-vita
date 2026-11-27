// np_mpdiag.{h,cpp}: the scrubber, the event ring and the report.
//   c++ -std=c++17 -Wall -Wextra -I<native> native/platform/sdk/tests/test_mpdiag.cpp native/platform/sdk/net/np_mpdiag.cpp -o /tmp/t && /tmp/t
// Mutant (must FAIL): add -DNP_MPDIAG_SCRUB_OFF.
#include "platform/sdk/net/np_mpdiag.h"

#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <new>
#include <string>
#include <thread>
#include <vector>

static int g_checks = 0, g_failed = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_failed; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_CLEAN(out, secret) do { ++g_checks; if (strstr((out), (secret))) { ++g_failed; std::printf("FAIL %s:%d: \"%s\" leaked in: %s\n", __FILE__, __LINE__, (secret), (out)); } } while (0)

static std::atomic<long> g_news(0);
void* operator new(size_t n) { g_news.fetch_add(1); void* p = malloc(n ? n : 1); if (!p) abort(); return p; }
void* operator new(size_t n, const std::nothrow_t&) noexcept { g_news.fetch_add(1); return malloc(n ? n : 1); }
void operator delete(void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }

using namespace NpMpDiag;

static std::string Scr(const char* in) {
    char out[1024];
    Scrub(in, strlen(in), out, sizeof out);
    return out;
}

static void CallGameLog(const char* format, ...) {
    va_list a;
    va_start(a, format);
    NoteGameLog(format, a);
    va_end(a);
}

int main() {
    // ---- the scrubber ----
    {
        // a report that holds an IP address, a name, an id and a token comes out clean (every secret below is made up)
        const char* dirty =
            "[events]\n"
            "T+  12.4s  link lost: no packets from 192.168.1.23:41100 for 20000 ms\n"
            "T+  12.5s  peer 10.0.0.5 and 81.12.x.x:41100 and 0a000105:41100 answered\n"
            "T+  12.6s  joined name=Zorbax Fan 77 slot=1 build=abc\n"
            "T+  12.7s  account profile=48213 id=77 nick=Luigi\n"
            "T+  12.8s  Authorization: Bearer q8Zk3mFv0pLw7xYtR2uN5oJcH1dGb4aE9sIeKfMh6Vc\n"
            "T+  12.9s  token=q8Zk3mFv0pLw7xYtR2uN5oJcH1dGb4aE9sIeKfMh6Vc and q8Zk3mFv0pLw7xYtR2uN5oJcH1dGb4aE9sIeKfMh6Vc bare\n"
            "T+  13.0s  joining KQ7M2X and invite code KQ7M2X and room=KQ7M2X friend ABCD-EFG2\n"
            "T+  13.1s  GET https://service.example.org/v1/rooms/KQ7M2X failed; nas.lan:8443 refused\n"
            "T+  13.2s  saved in /home/alice/mscharged and C:\\Users\\Bob\\x\n"
            "T+  13.3s  fe80::1c2d:3eff:fe4f:5a6b and [2001:db8::1]:41100\n"
            "T+  13.4s  uuid 123e4567-e89b-12d3-a456-426614174000 key=00112233445566778899aabbccddeeff\n"
            "T+  13.5s  console=PSV-1234 friend=ABCD-EFG2 code=ZZ9X8Y\n";
        const std::string s = Scr(dirty);
        const char* o = s.c_str();
        CHECK_CLEAN(o, "192.168.1.23");
        CHECK_CLEAN(o, "10.0.0.5");
        CHECK_CLEAN(o, "81.12");
        CHECK_CLEAN(o, "0a000105");
        CHECK_CLEAN(o, "Zorbax");
        CHECK_CLEAN(o, "Luigi");
        CHECK_CLEAN(o, "48213");
        CHECK_CLEAN(o, "q8Zk3m");
        CHECK_CLEAN(o, "KQ7M2X");
        CHECK_CLEAN(o, "ABCD-EFG2");
        CHECK_CLEAN(o, "service.example.org");
        CHECK_CLEAN(o, "nas.lan");
        CHECK_CLEAN(o, "alice");
        CHECK_CLEAN(o, "Bob");
        CHECK_CLEAN(o, "fe80::");
        CHECK_CLEAN(o, "2001:db8");
        CHECK_CLEAN(o, "123e4567");
        CHECK_CLEAN(o, "00112233445566778899");
        CHECK_CLEAN(o, "PSV-1234");
        CHECK_CLEAN(o, "ZZ9X8Y");
        CHECK(strstr(o, "<hidden>") && strstr(o, "<address>") && strstr(o, "<secret>") && strstr(o, "<code>") && strstr(o, "<host>"));
        // what is not secret survives: step numbers, versions, the build id, slots, counts, error words, times
        const std::string keep = Scr("T+  4.2s  DESYNC at step 4521: the checksum differs\nversion = 1.3.0\nbuild = 0123456789abcdef\nfingerprint = 1a2b3c4d\nhost=2 peers=3\ncode=room_full\nmatch end: 5000 steps, 12 starved (0.2%), round trip 35 ms; last digests: t=4999 crc=deadbeef/deadbeef\nMSC_LAN_MODE = online\n");
        CHECK(keep.find("step 4521") != std::string::npos);
        CHECK(keep.find("version = 1.3.0") != std::string::npos);
        CHECK(keep.find("build = 0123456789abcdef") != std::string::npos);
        CHECK(keep.find("fingerprint = 1a2b3c4d") != std::string::npos);
        CHECK(keep.find("host=2") != std::string::npos && keep.find("peers=3") != std::string::npos);
        CHECK(keep.find("code=room_full") != std::string::npos);
        CHECK(keep.find("crc=deadbeef/deadbeef") != std::string::npos);
        CHECK(keep.find("MSC_LAN_MODE = online") != std::string::npos);
        // bounds: never writes past cap - 1, always terminates, reads no more than n
        char small[16];
        memset(small, 'z', sizeof small);
        const size_t w = Scrub("name=ABCDEFGHIJKLMNOPQRSTUVWXYZ 192.168.0.1 abcdefghijklmnopqrstuvwxyz", 70, small, 12);
        CHECK(w <= 11 && small[w] == 0 && small[12] == 'z');
        CHECK(Scrub(nullptr, 0, small, sizeof small) == 0 && small[0] == 0);
        CHECK(Scrub("abc", 3, nullptr, 0) == 0);
        // exactly n bytes are read: a secret after n is not looked at
        char two[64];
        Scrub("abc 1.2.3.4", 3, two, sizeof two);
        CHECK(!strcmp(two, "abc"));
    }

    // ---- fuzz: random bytes and mutated dirty text never overrun, always terminate, and never leave a planted address behind ----
    {
        unsigned seed = 12345u;
        auto next = [&seed]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
        const char* alphabet = "abcXYZ019 .:=-_/[]\n\"+,{}@?!<>\xff\x01";
        const size_t alen = strlen(alphabet);
        for (int i = 0; i < 20000; ++i) {
            char in[200];
            const size_t n = next() % sizeof in;
            for (size_t k = 0; k < n; ++k) in[k] = (next() % 7 == 0) ? char(next() & 0xFF) : alphabet[next() % alen];
            char out[96];
            const size_t cap = 1 + next() % sizeof out;
            const size_t w = Scrub(in, n, out, cap);
            CHECK(w < cap && out[w] == 0);
            char planted[260];
            const int pn = snprintf(planted, sizeof planted, "x 10.%u.%u.%u:%u y", unsigned(next() % 200), unsigned(next() % 200), unsigned(next() % 200), unsigned(next() % 60000));
            char big[600];
            const size_t bw = Scrub(planted, size_t(pn), big, sizeof big);
            CHECK(bw > 0 && strstr(big, "<address>") != nullptr);
        }
    }

    // ---- the ring ----
    {
        static Ring ring;   // (static: 20 KiB)
        ring.Clear();
        CHECK(ring.Count() == 0 && ring.Total() == 0);
        const long news0 = g_news.load();
        const struct mallinfo2 m0 = mallinfo2();
        for (int i = 0; i < 100000; ++i) {
            if (i % 40 == 0) ring.SessionBegin();
            ring.Push(uint32_t(i), Kind(i % kKindCount), i, -i, i * 3, "reason text that is far longer than the fifty-six bytes the record keeps for it");
        }
        const struct mallinfo2 m1 = mallinfo2();
        CHECK(g_news.load() == news0);                       // no operator new
        CHECK(m1.uordblks == m0.uordblks && m1.arena == m0.arena);   // no malloc either
        CHECK(ring.Count() == kRingSize);                    // bounded
        CHECK(ring.Total() == 100000);
        CHECK(sizeof(Ring) < 24 * 1024);
        // oldest first, newest last
        for (int i = 1; i < ring.Count(); ++i) CHECK(ring.At(i).ms > ring.At(i - 1).ms);
        CHECK(ring.At(ring.Count() - 1).ms == 99999 || ring.At(ring.Count() - 1).ms > 99900);
        CHECK(strlen(ring.At(0).text) == size_t(kTextBytes) - 1);   // cut
        // per-session cap: a desync that repeats every step cannot push the session start out
        ring.Clear();
        ring.SessionBegin();
        ring.Push(1, kSessionStart, 1, 2, 0, nullptr);
        int kept = 0;
        for (int i = 0; i < 500; ++i) kept += ring.Push(10 + uint32_t(i), kDesync, 4000 + i, 2, 0, nullptr);
        CHECK(kept == kPerKindCap && ring.Suppressed() == 500u - kPerKindCap);
        CHECK(ring.Count() == 1 + kPerKindCap && ring.At(0).kind == kSessionStart);
        ring.SessionBegin();
        CHECK(ring.Push(900, kDesync, 1, 2, 0, nullptr));    // the next session counts again
        // control characters in text become '?', an unknown kind is a note
        ring.Clear();
        ring.Push(1, Kind(200), 0, 0, 0, "a\nb\x01\xff");
        CHECK(ring.At(0).kind == kNote && !strcmp(ring.At(0).text, "a?b??"));
    }

    // ---- the process ring: events from the game's own log calls, from several threads ----
    {
        ClearForTest();
        Note(kSessionStart, 1, 2);
        CallGameLog("DESYNC checksum mismatch at network tick %d\n", 4521);
        CallGameLog("DESYNC frame mismatch at network tick %d\n", 77);
        CallGameLog("DESYNC random seed mismatch at network tick %d\n", 12);
        CallGameLog("connection lost (connection %u, reason %d)\n", 3u, 9);
        CallGameLog("input queue overflow\n");
        CallGameLog("malformed input message rejected\n");   // not an event
        CHECK(Events() == 6);
        ClearForTest();
        std::vector<std::thread> ts;
        for (int t = 0; t < 4; ++t) ts.emplace_back([] { for (int i = 0; i < 1000; ++i) Note(kNote, i, 0, 0, "x"); });
        for (auto& t : ts) t.join();
        CHECK(TotalEvents() == 4000 && Events() <= kPerKindCap);
        ClearForTest();
    }

    // ---- the report ----
    {
        static Ring ring;
        ring.Clear();
        ring.Push(1200, kSessionStart, 1, 2, 0, nullptr);
        ring.Push(1500, kPeers, 2, 0, 0, nullptr);
        ring.Push(1600, kInputDelay, 4, 120, 1, nullptr);
        ring.Push(30000, kDesync, 4521, 2, 0, nullptr);
        ring.Push(30500, kLinkLost, 0, 0, 0, "no packets from 192.168.1.23:41100 (name=Zorbax token=q8Zk3mFv0pLw7xYtR2uN5oJcH1dGb4aE9sIeKfMh6Vc)");
        ring.Push(30600, kOnlineFail, 0, 0, 0, "Room KQ7M2X is full; ask nas.lan");
        ring.Push(31000, kMatchEnd, 5000, 12, 35, "t=4999 crc=deadbeef/deadbeef");
        ring.Push(31100, kSessionEnd, 0, 0, 0, nullptr);
        ReportParts p;
        memset(&p, 0, sizeof p);
        p.version = "1.3.0";
        p.buildId = "0123456789abcdef";
        p.fingerprint = 0x1a2b3c4d;
        p.settings = "MSC_LAN_MODE = online\nMSC_LAN_INPUT_DELAY = auto\n";
        p.ring = &ring;
        p.nowMs = 40000;
        p.connTest = "wifi=good internet=ok hosting=direct delay=fine (35 ms)\n";
        char out[8192];
        const size_t n = BuildReport(out, sizeof out, p);
        CHECK(n == strlen(out) && n > 300);
        CHECK(strstr(out, "[build]") && strstr(out, "[settings]") && strstr(out, "[connection test]") && strstr(out, "[events]"));
        CHECK(strstr(out, "DESYNC at step 4521: the checksum differs"));
        CHECK(strstr(out, "input delay: 4 ticks, chosen by AUTO from a 120 ms round trip"));
        CHECK(strstr(out, "session end"));
        CHECK(strstr(out, "T+  30.0s") != nullptr);
        CHECK(strstr(out, "version = 1.3.0") && strstr(out, "fingerprint = 1a2b3c4d") && strstr(out, "MSC_LAN_MODE = online"));
        CHECK(strstr(out, "delay=fine (35 ms)"));
        CHECK_CLEAN(out, "192.168.1.23");
        CHECK_CLEAN(out, "Zorbax");
        CHECK_CLEAN(out, "q8Zk3m");
        CHECK_CLEAN(out, "KQ7M2X");
        CHECK_CLEAN(out, "nas.lan");
        CHECK(strstr(out, "ms") != nullptr);
        for (const char* c = out; *c; ++c) CHECK((unsigned char)*c == '\n' || ((unsigned char)*c >= 0x20 && (unsigned char)*c < 0x7F));
        // newest last
        CHECK(strstr(out, "session start") < strstr(out, "session end"));
        // a settings value that looks secret is still scrubbed (second guard)
        p.settings = "MSC_LAN_MODE = 192.168.4.4\n";
        BuildReport(out, sizeof out, p);
        CHECK_CLEAN(out, "192.168.4.4");
        p.settings = "";
        // a small buffer: the oldest events go first, the report says so, nothing is written past the end
        char tight[1100];
        memset(tight, 'q', sizeof tight);
        const size_t tn = BuildReport(tight, 1000, p);
        CHECK(tn < 1000 && tight[tn] == 0 && tight[1000] == 'q');
        CHECK(strstr(tight, "older events did not fit") != nullptr);
        CHECK(strstr(tight, "session end") != nullptr);       // the newest survives
        // an empty ring and no test
        Ring empty;
        p.ring = &empty;
        p.connTest = nullptr;
        BuildReport(out, sizeof out, p);
        CHECK(strstr(out, "no multiplayer events since the game started") && strstr(out, "none run since the game started"));
    }

    // ---- saving ----
    {
        char dir[] = "/tmp/mpdiag-test-XXXXXX";
        CHECK(mkdtemp(dir) != nullptr);
        ClearForTest();
        Note(kSessionStart, 0, 2);
        Note(kLinkLost, 0, 0, 0, "peer 172.16.9.9:41100 went quiet");
        SetLastConnTest("wifi=good internet=ok");
        char path[300];
        CHECK(SaveReport(dir, path, sizeof path));
        CHECK(strstr(path, "/mp-report.txt") != nullptr);
        std::string text;
        if (FILE* f = fopen(path, "rb")) { char b[512]; size_t k; while ((k = fread(b, 1, sizeof b, f)) > 0) text.append(b, k); fclose(f); }
        CHECK(text.find("link lost: peer") != std::string::npos);
        CHECK(text.find("172.16.9.9") == std::string::npos);
        CHECK(text.find("wifi=good internet=ok") != std::string::npos);
        CHECK(!SaveReport("/nonexistent-dir-for-test/x", path, sizeof path));
        ClearForTest();
        std::string rm = std::string("rm -rf ") + dir;
        if (system(rm.c_str())) {}
    }

    std::printf("mpdiag: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
