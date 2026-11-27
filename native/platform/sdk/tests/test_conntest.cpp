// np_conntest.{h,cpp}: the five budgeted checks, the delay classes, the wrong-clock sentence, driven by fakes and a clock the test advances (no network).
//   c++ -std=c++17 -Wall -Wextra -I<native> native/platform/sdk/tests/test_conntest.cpp native/platform/sdk/net/np_conntest.cpp -o /tmp/t && /tmp/t
// Mutant (must FAIL): add -DNP_CONNTEST_CHOPPY_MS=1000.
#include "platform/sdk/net/np_conntest.h"

#include <stdio.h>
#include <string.h>
#include <string>

static int g_checks = 0, g_failed = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_failed; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

using namespace NpConnTest;

struct Fake {
    uint64_t now = 1000;
    // wifi
    bool wifiKnown = true; Wifi wifi{true, 80};
    // local
    bool hasLocal = true; int localAnswer = 1; uint64_t localAfter = 0;
    // clock
    bool hasClock = true; int clockAnswer = 1;
    // service
    bool hasInfo = true; bool infoStarts = true; uint64_t infoAfter = 40; Info info{kInfoOk, 40};
    int infoBegins = 0, infoCancels = 0; uint32_t infoTimeout = 0; uint64_t infoStart = 0; bool infoOn = false;
    // hosting
    bool hasHost = true; int nat = 1; uint64_t hostAfter = 100; int hostAnswer = 1; int hostBegins = 0; uint64_t hostStart = 0;
    // delay
    bool hasRtt = true; int rttSamples[4] = {30, 35, 30, 30}; int rttCalls = 0;
    uint64_t localStart = 0; bool localOn = false;

    static uint64_t Now(void* c) { return static_cast<Fake*>(c)->now; }
    static bool Wifi_(void* c, Wifi* o) { Fake* f = static_cast<Fake*>(c); if (!f->wifiKnown) return false; *o = f->wifi; return true; }
    static int Local_(void* c) {
        Fake* f = static_cast<Fake*>(c);
        if (!f->localOn) { f->localOn = true; f->localStart = f->now; }
        return f->now - f->localStart >= f->localAfter ? f->localAnswer : 0;
    }
    static int Clock_(void* c) { return static_cast<Fake*>(c)->clockAnswer; }
    static bool InfoBegin(void* c, uint32_t t) { Fake* f = static_cast<Fake*>(c); ++f->infoBegins; f->infoTimeout = t; f->infoStart = f->now; f->infoOn = true; return f->infoStarts; }
    static int InfoPoll(void* c, Info* o) { Fake* f = static_cast<Fake*>(c); if (f->now - f->infoStart < f->infoAfter) return 0; *o = f->info; return 1; }
    static void InfoCancel(void* c) { static_cast<Fake*>(c)->infoCancels++; }
    static bool HostBegin(void* c) { Fake* f = static_cast<Fake*>(c); ++f->hostBegins; f->hostStart = f->now; return true; }
    static int HostPoll(void* c, int* nat) { Fake* f = static_cast<Fake*>(c); if (f->now - f->hostStart < f->hostAfter) return 0; *nat = f->nat; return f->hostAnswer; }
    static int Rtt(void* c) { Fake* f = static_cast<Fake*>(c); const int v = f->rttSamples[f->rttCalls < 4 ? f->rttCalls : 3]; ++f->rttCalls; return v; }

    Probes Make() {
        Probes p;
        p.ctx = this;
        p.now = Now;
        if (true) p.wifi = Wifi_;
        if (hasLocal) p.local = Local_;
        if (hasClock) p.clock = Clock_;
        if (hasInfo) { p.infoBegin = InfoBegin; p.infoPoll = InfoPoll; p.infoCancel = InfoCancel; }
        if (hasHost) { p.hostBegin = HostBegin; p.hostPoll = HostPoll; }
        if (hasRtt) p.rtt = Rtt;
        return p;
    }
    // runs a test to its end, advancing the clock 50 ms a pump (a frame at 20 Hz is coarse enough to show the deadlines); returns the pumps
    int RunAll(Run& r, uint32_t stepMs = 50, int maxPumps = 2000) {
        int n = 0;
        while (r.Running() && n < maxPumps) { r.Pump(); now += stepMs; ++n; }
        return n;
    }
};

static std::string Text(const Run& r) { char b[600]; VerdictText(r.GetView().verdict, b, sizeof b); return b; }

int main() {
    // ---- pure helpers: the delay classes at their edges ----
    CHECK(DelayResult(0) == kDelayFast && DelayResult(89) == kDelayFast);
    CHECK(DelayResult(90) == kDelayFine && DelayResult(219) == kDelayFine);
    CHECK(DelayResult(220) == kDelayChoppy && DelayResult(599999) == kDelayChoppy);
    CHECK(DelayResult(-5) == kNone && DelayResult(1000000000) == kNone && DelayResult(600001) == kNone);
    CHECK(WifiResult({false, 90}) == kWifiNone && WifiResult({true, 34}) == kWifiWeak && WifiResult({true, 35}) == kWifiGood);
    CHECK(WifiResult({true, -1}) == kWifiUnknown && WifiResult({true, 400}) == kWifiUnknown);
    CHECK(HostResult(1) == kHostDirect && HostResult(2) == kHostDirect && HostResult(3) == kHostRelay && HostResult(0) == kHostUnknown && HostResult(40) == kHostUnknown && HostResult(-1) == kHostUnknown);
    { Info i{99, 5}; CHECK(InfoResult(i) == kNetDown); i.outcome = kInfoClock; CHECK(InfoResult(i) == kNetClock); i.outcome = kInfoOk; CHECK(InfoResult(i) == kNetOk); }

    // ---- everything good ----
    {
        Fake f; Run r;
        r.Start(f.Make());
        CHECK(r.Running());
        f.RunAll(r);
        const View& v = r.GetView();
        CHECK(!v.running && v.done);
        CHECK(v.result[kWifi] == kWifiGood && v.result[kLocal] == kLocalFound && v.result[kInternet] == kNetOk && v.result[kHosting] == kHostDirect && v.result[kDelay] == kDelayFast);
        CHECK(v.detail[kDelay] == 30 && f.rttCalls == kDelaySamples);   // two samples, the smaller counts
        CHECK(v.verdict.headline == kHGood && v.verdict.good);
        CHECK(v.elapsedMs <= Budget().total);
        const std::string t = Text(r);
        CHECK(t.find("Your connection is good") == 0 && t.find("The delay looks fast.") != std::string::npos);
        char line[200];
        ReportLine(v.result, v.detail, line, sizeof line);
        CHECK(!strcmp(line, "wifi=good local=found internet=ok hosting=direct delay=fast (30 ms)"));
    }
    // ---- choppy: the class of the better of two samples ----
    {
        Fake f; f.rttSamples[0] = 400; f.rttSamples[1] = 260; Run r; r.Start(f.Make()); f.RunAll(r);
        CHECK(r.GetView().result[kDelay] == kDelayChoppy && r.GetView().detail[kDelay] == 260);
        CHECK(r.GetView().verdict.headline == kHChoppy && !r.GetView().verdict.good);
        CHECK(Text(r).find("choppy") != std::string::npos && Text(r).find("lag") != std::string::npos);
    }
    {   // 219 is fine, 220 choppy, 89 fast, 90 fine (through the engine too)
        const int cases[][2] = {{89, kDelayFast}, {90, kDelayFine}, {219, kDelayFine}, {220, kDelayChoppy}};
        for (const auto& c : cases) {
            Fake f; f.rttSamples[0] = c[0]; f.rttSamples[1] = c[0]; Run r; r.Start(f.Make()); f.RunAll(r);
            CHECK(r.GetView().result[kDelay] == c[1]);
        }
    }
    // ---- the wrong clock: told plainly, and the network is not even tried ----
    {
        Fake f; f.clockAnswer = 0; Run r; r.Start(f.Make()); f.RunAll(r);
        const View& v = r.GetView();
        CHECK(v.result[kInternet] == kNetClock && v.verdict.headline == kHClock);
        CHECK(f.infoBegins == 0 && f.hostBegins == 0 && f.rttCalls == 0);
        CHECK(v.result[kHosting] == kSkipped && v.result[kDelay] == kSkipped);
        const std::string t = Text(r);
        CHECK(t.find("date and time on this Vita look wrong") != std::string::npos);
        CHECK(t.find("even by a year") != std::string::npos);
        CHECK(t.find("Settings > Date & Time") != std::string::npos);
    }
    {   // the clock is plausible for this build but the service's certificate fails on dates (a clock a year ahead): the service call says so
        Fake f; f.info.outcome = kInfoClock; Run r; r.Start(f.Make()); f.RunAll(r);
        CHECK(r.GetView().result[kInternet] == kNetClock && r.GetView().verdict.headline == kHClock && f.hostBegins == 0);
    }
    // ---- budgets ----
    {   // the service never answers: down at its 3 s, cancelled, the rest skipped, the whole run well inside 8 s
        Fake f; f.infoAfter = 1000000; Run r; r.Start(f.Make()); f.RunAll(r);
        const View& v = r.GetView();
        CHECK(v.result[kInternet] == kNetDown && f.infoCancels >= 1);
        CHECK(v.result[kHosting] == kSkipped && v.result[kDelay] == kSkipped);
        CHECK(v.verdict.headline == kHDown);
        CHECK(v.elapsedMs >= 3000 - 50 && v.elapsedMs <= 3000 + 200);
        CHECK(f.infoTimeout <= 3000);
    }
    {   // the same-Wi-Fi search with no room answering ends at its 1.5 s, and everything slow together stays inside the total
        Fake f; f.localAnswer = 0; f.localAfter = 100000; f.infoAfter = 2900; f.hostAfter = 2100; Run r; r.Start(f.Make()); f.RunAll(r);
        const View& v = r.GetView();
        CHECK(v.result[kLocal] == kLocalOk && v.result[kInternet] == kNetOk && v.result[kHosting] == kHostDirect);
        CHECK(v.elapsedMs <= Budget().total + 100);
    }
    {   // a smaller total: the running step ends with what it has, the later ones are skipped
        Fake f; f.localAnswer = 0; f.localAfter = 100000; f.infoAfter = 2900; Run r; Budget b; b.total = 2000; r.Start(f.Make(), b); f.RunAll(r);
        const View& v = r.GetView();
        CHECK(v.done && v.result[kInternet] == kNetDown && v.result[kHosting] == kSkipped && v.result[kDelay] == kSkipped);
        CHECK(v.elapsedMs <= 2000 + 100);
    }
    {   // the menu flow's own, larger, budget: a room that takes 12 s is still ok
        Fake f; f.infoAfter = 12000; Run r; Budget b; b.total = 60000; b.info = 30000; r.Start(f.Make(), b); f.RunAll(r, 50, 4000);
        CHECK(r.GetView().result[kInternet] == kNetOk);
    }
    // ---- hosting words ----
    {
        Fake f; f.nat = 3; Run r; r.Start(f.Make()); f.RunAll(r);
        CHECK(r.GetView().result[kHosting] == kHostRelay && r.GetView().verdict.headline == kHRelay && Text(r).find("relay") != std::string::npos);
    }
    {
        Fake f; f.hostAnswer = -1; Run r; r.Start(f.Make()); f.RunAll(r);
        CHECK(r.GetView().result[kHosting] == kHostUnknown && r.GetView().verdict.headline == kHUnknown);
    }
    // ---- no Wi-Fi, the service's own answers ----
    {
        Fake f; f.wifi = {false, -1}; Run r; r.Start(f.Make()); f.RunAll(r);
        CHECK(r.GetView().verdict.headline == kHNoWifi && f.infoBegins == 0 && r.GetView().result[kInternet] == kSkipped);
        CHECK(Text(r).find("Connect to Wi-Fi") != std::string::npos);
    }
    {
        const struct { uint8_t outcome; uint8_t headline; } cases[] = {{kInfoNotSetUp, kHNotSetUp}, {kInfoUpdate, kHUpdate}, {kInfoOff, kHPaused}, {kInfoDown, kHDown}, {kInfoOffline, kHDown}};
        for (const auto& c : cases) { Fake f; f.info.outcome = c.outcome; Run r; r.Start(f.Make()); f.RunAll(r); CHECK(r.GetView().verdict.headline == c.headline); }
    }
    {   // a weak signal is said before anything else about delay
        Fake f; f.wifi = {true, 20}; Run r; r.Start(f.Make()); f.RunAll(r);
        CHECK(r.GetView().verdict.headline == kHWeakWifi && Text(r).find("Move closer") != std::string::npos);
    }
    // ---- missing probes: skipped, never a crash; delay falls back to the service call's own time ----
    {
        Fake f; f.hasLocal = false; f.hasHost = false; f.hasRtt = false; f.hasClock = false; f.wifiKnown = false; f.info.ms = 150;
        Run r; r.Start(f.Make()); f.RunAll(r);
        const View& v = r.GetView();
        CHECK(v.result[kWifi] == kWifiUnknown && v.result[kLocal] == kSkipped && v.result[kHosting] == kSkipped && v.result[kDelay] == kDelayFine);
        Fake g; g.hasInfo = false; Run s; s.Start(g.Make()); g.RunAll(s);
        CHECK(s.GetView().done && s.GetView().result[kInternet] == kSkipped && s.GetView().verdict.headline == kHDown);
        Probes empty; Run e; e.Start(empty); CHECK(!e.Running()); e.Pump();   // no clock: not started
    }
    // ---- another machine's word is clamped ----
    {
        Fake f; f.rttSamples[0] = -5; f.rttSamples[1] = 1000000000; f.nat = 40; f.wifi = {true, 400}; f.info.ms = -7;
        Run r; r.Start(f.Make()); f.RunAll(r);
        const View& v = r.GetView();
        CHECK(v.result[kDelay] == kSkipped && v.result[kHosting] == kHostUnknown && v.result[kWifi] == kWifiUnknown);
        CHECK(v.detail[kWifi] == -1 && v.detail[kDelay] == -1);
    }
    // ---- cancel and restart ----
    {
        Fake f; f.infoAfter = 100000; Run r; r.Start(f.Make());
        for (int i = 0; i < 10; ++i) { r.Pump(); f.now += 50; }
        CHECK(r.Running() && f.infoBegins == 1);
        r.Cancel();
        CHECK(!r.Running() && !r.GetView().done && f.infoCancels == 1);
        const uint32_t serial = r.GetView().serial;
        r.Start(f.Make());
        CHECK(r.Running() && r.GetView().serial > serial && r.GetView().result[kWifi] == kNone);
        r.Start(f.Make());   // a run in progress is replaced
        CHECK(r.Running());
    }
    // ---- every sentence of the verdict exists and is short enough for a popup ----
    for (int h = 1; h < kHCount; ++h) CHECK(HeadlineText(uint8_t(h))[0]);
    for (int l = 1; l < kLCount; ++l) CHECK(LineText(uint8_t(l))[0] && strlen(LineText(uint8_t(l))) < 76);
    {
        uint8_t all[kChecks];
        for (int a = 0; a < kResultCount; ++a)
            for (int b = 0; b < kResultCount; ++b) {
                memset(all, 0, sizeof all);
                all[kWifi] = uint8_t(a); all[kInternet] = uint8_t(b); all[kDelay] = uint8_t((a + b) % kResultCount);
                char t[600];
                const Verdict v = MakeVerdict(all);
                CHECK(v.headline != kHNone && VerdictText(v, t, sizeof t) < 420);
            }
    }
    // ---- ux13 (O8): Who can I play with? three answers from the same results ----
    {
        uint8_t r[kChecks];
        auto set = [&](uint8_t w, uint8_t l, uint8_t n, uint8_t h, uint8_t d) { r[kWifi] = w; r[kLocal] = l; r[kInternet] = n; r[kHosting] = h; r[kDelay] = d; };
        set(kWifiGood, kSkipped, kNetOk, kHostDirect, kDelayFast);
        CHECK(WhoAnswer(r, kWhoRoom) == kPRoomYes && WhoAnswer(r, kWhoFar) == kPCodeYes && WhoAnswer(r, kWhoAnyone) == kPAnyYes);
        set(kWifiNone, kSkipped, kNetDown, kHostUnknown, kNone);                       // no Wi-Fi: Nearby still works, nothing online
        CHECK(WhoAnswer(r, kWhoRoom) == kPRoomNearbyOnly && WhoAnswer(r, kWhoFar) == kPNoWifi && WhoAnswer(r, kWhoAnyone) == kPNoWifi);
        set(kWifiGood, kLocalBlocked, kNetOk, kHostDirect, kDelayFine);
        CHECK(WhoAnswer(r, kWhoRoom) == kPRoomBlocked && WhoAnswer(r, kWhoFar) == kPCodeYes);
        set(kWifiGood, kSkipped, kNetOk, kHostRelay, kDelayFine);                      // a symmetric NAT: a relay, so lag
        CHECK(WhoAnswer(r, kWhoFar) == kPCodeLag && WhoAnswer(r, kWhoAnyone) == kPAnyLag);
        set(kWifiGood, kSkipped, kNetOk, kHostDirect, kDelayChoppy);
        CHECK(WhoAnswer(r, kWhoFar) == kPCodeYes && WhoAnswer(r, kWhoAnyone) == kPAnyLag);   // a code game picks its own partner: only "anyone" is told about a choppy delay
        set(kWifiWeak, kSkipped, kNetOk, kHostDirect, kDelayFast);
        CHECK(WhoAnswer(r, kWhoFar) == kPCodeLag && WhoAnswer(r, kWhoAnyone) == kPAnyLag);
        set(kWifiGood, kSkipped, kNetClock, kHostUnknown, kNone);
        CHECK(WhoAnswer(r, kWhoFar) == kPNoClock && WhoAnswer(r, kWhoRoom) == kPRoomYes);   // the room never depends on the service
        set(kWifiGood, kSkipped, kNetUpdate, kHostUnknown, kNone);  CHECK(WhoAnswer(r, kWhoAnyone) == kPNoUpdate);
        set(kWifiGood, kSkipped, kNetOff, kHostUnknown, kNone);     CHECK(WhoAnswer(r, kWhoAnyone) == kPNoPaused);
        set(kWifiGood, kSkipped, kNetNotSetUp, kHostUnknown, kNone); CHECK(WhoAnswer(r, kWhoAnyone) == kPNoNotSetUp);
        set(kWifiGood, kSkipped, kNetDown, kHostUnknown, kNone);    CHECK(WhoAnswer(r, kWhoFar) == kPNoDown);
        set(kNone, kNone, kNone, kNone, kNone);                     CHECK(WhoAnswer(r, kWhoFar) == kPNoDown && WhoAnswer(r, kWhoRoom) == kPRoomYes);
        CHECK(WhoAnswer(r, 7) == kPNone);
        for (int who = 0; who < kWhoCount; ++who) CHECK(WhoLabelText(who)[0]);
        for (int p = 1; p < kPCount; ++p) CHECK(WhoPhraseText(uint8_t(p))[0] && strlen(WhoPhraseText(uint8_t(p))) < 76 && strcmp(WhoPhraseName(uint8_t(p)), "?") != 0);
        CHECK(WhoNo(kPNoWifi) && WhoNo(kPNoNotSetUp) && !WhoNo(kPAnyLag) && !WhoNo(kPNone));
        // every combination gives an answer for every question, and a "yes" never comes with the internet down
        uint8_t all[kChecks];
        for (int a = 0; a < kResultCount; ++a)
            for (int b = 0; b < kResultCount; ++b) {
                memset(all, 0, sizeof all);
                all[kWifi] = uint8_t(a); all[kInternet] = uint8_t(b); all[kHosting] = uint8_t((a + b) % kResultCount); all[kDelay] = uint8_t(b % kResultCount);
                for (int who = 0; who < kWhoCount; ++who) CHECK(WhoAnswer(all, who) != kPNone);
                if (b != kNetOk) CHECK(WhoNo(WhoAnswer(all, kWhoFar)) && WhoNo(WhoAnswer(all, kWhoAnyone)));
            }
    }
    std::printf("conntest: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
