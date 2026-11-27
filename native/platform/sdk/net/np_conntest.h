// The Connection Test, deeper (wave 11, mpdiag18; borrowed from Galaxy 3.0's platform/src/mp/mp_conntest.{h,cpp}): five budgeted checks run one after another through a
// table of probes, a classifier that turns what they found into plain words (fast / fine / choppy delay, relay or direct, the wrong clock), and the text of the result.
//
// Pure code: no sockets, no SDK, no game. The probes are function pointers the caller fills in: the menu flow (np_menu_flow.cpp) fills them from its own room probe
// (the room is made and left at once; nobody is invited), the host tests fill them with fakes and a clock they advance. pump() once a frame advances what runs and never
// blocks; every step has a deadline of its own and the whole run ends within the total budget (Galaxy: 8 s). Nothing here touches the wire or the lockstep.
//
// Everything a probe returns is another machine's word (the service's answer, a router, a STUN server): it is clamped here (a delay of -5 ms or 10^9 ms, a NAT class of 40
// and an unknown outcome code all end as a plain result), never trusted.
#ifndef NP_CONNTEST_H
#define NP_CONNTEST_H

#include <stddef.h>
#include <stdint.h>

namespace NpConnTest {

enum Check : uint8_t { kWifi = 0, kLocal, kInternet, kHosting, kDelay, kChecks };
enum State : uint8_t { kWaiting = 0, kRunning, kDone };

// What one check found. A check that was not run (nothing to test, an earlier check failed, out of time) is kSkipped; kNone is only the value of a check not yet finished.
enum Result : uint8_t {
    kNone = 0,
    kSkipped,
    // Wi-Fi
    kWifiGood, kWifiWeak, kWifiNone, kWifiUnknown,
    // Same Wi-Fi: other Vitas on this network
    kLocalFound, kLocalOk, kLocalBlocked,
    // Internet: the online service (and the clock its certificate is read with)
    kNetOk, kNetDown, kNetClock, kNetNotSetUp, kNetUpdate, kNetOff,
    // Hosting: how the router shows this Vita to the service's STUN servers (np_net.h NatClass)
    kHostDirect,      // no NAT or a cone NAT: another Vita can be reached directly
    kHostRelay,       // a symmetric NAT: the match may have to go through the relay (more delay)
    kHostUnknown,     // not measured
    // Delay (round trip, milliseconds)
    kDelayFast, kDelayFine, kDelayChoppy,
    kResultCount
};

// Budgets (milliseconds). Galaxy's own numbers are the defaults (the sum of the steps is a little over the total: the total wins, the running step then ends with what it has).
// The menu flow's probe is a room made on the service (TLS, registration, STUN), which takes longer than Galaxy's one info call: it passes its own, larger, Budget.
struct Budget {
    uint32_t total;
    uint32_t local;
    uint32_t info;
    uint32_t host;
    uint32_t rtt;
    Budget() : total(8000), local(1500), info(3000), host(2200), rtt(1200) {}
};
const int kDelaySamples = 2;
// Delay classes: fast below kFastMs, choppy from kChoppyMs (the same steps Galaxy's player list uses for its connection bars).
const int kFastMs = 90;
#ifndef NP_CONNTEST_CHOPPY_MS
#define NP_CONNTEST_CHOPPY_MS 220    // (a test builds a mutant with another value: tests/test_mpdiag.py must notice)
#endif
const int kChoppyMs = NP_CONNTEST_CHOPPY_MS;
const int kWeakPercent = 35;       // Wi-Fi quality below this is weak (platforms that tell)

struct Wifi {
    bool connected;
    int quality;                   // 0..100, -1 = the platform does not say
};
enum InfoOutcome : uint8_t { kInfoOk = 0, kInfoDown, kInfoOffline, kInfoClock, kInfoOff, kInfoUpdate, kInfoNotSetUp };
struct Info {
    uint8_t outcome;               // InfoOutcome
    int ms;                        // round trip of the call, -1 unknown
};

struct Probes {
    void* ctx;
    uint64_t (*now)(void* ctx);                        // monotonic milliseconds (required)
    bool (*wifi)(void* ctx, Wifi* out);                // null / false: not known
    int (*local)(void* ctx);                           // polled while that check runs: rooms found so far (0 none yet), -1 the search cannot go out, -2 not possible now. null: skipped
    int (*clock)(void* ctx);                           // 1 the date is plausible, 0 it is wrong, -1 unknown. null: only the service's answer can tell
    bool (*infoBegin)(void* ctx, uint32_t timeoutMs);  // false: the call could not start. null: the service check is skipped
    int (*infoPoll)(void* ctx, Info* out);             // 0 pending, 1 finished (*out filled)
    void (*infoCancel)(void* ctx);
    bool (*hostBegin)(void* ctx);                      // false / null: hosting is skipped
    int (*hostPoll)(void* ctx, int* nat);              // 0 pending, 1 answered (*nat = np_net.h NatClass), -1 no answer
    int (*rtt)(void* ctx);                             // one delay sample in ms, -1 none. null: the delay is taken from the service call's own time when it told one
    Probes() : ctx(0), now(0), wifi(0), local(0), clock(0), infoBegin(0), infoPoll(0), infoCancel(0), hostBegin(0), hostPoll(0), rtt(0) {}
};

// ---- the words ------------------------------------------------------------------------------------------------------------------------------
enum Headline : uint8_t {
    kHNone = 0,
    kHGood,            // "Your connection is good: other players should be able to reach you."
    kHRelay,           // "Online play works, but your Wi-Fi hides this Vita ... relay and lag."
    kHUnknown,         // "The online service answered, but your Wi-Fi could not be checked fully. ..."
    kHChoppy,          // "The connection is choppy."
    kHNoWifi,          // "This Vita is not connected to Wi-Fi."
    kHWeakWifi,        // "The Wi-Fi signal is weak."
    kHClock,           // the wrong-clock sentence
    kHDown,            // "The online service could not be reached."
    kHNotSetUp,        // "Online play is not available in this version of the game."
    kHUpdate,          // "The game needs an update to play online."
    kHPaused,          // "Online play is paused right now."
    kHCount
};
enum Line : uint8_t {
    kLNone = 0,
    kLConnectWifi, kLNearbyStill, kLLocalStill, kLClockBreaks, kLSetClock, kLCheckWifi, kLUpdate, kLTryLater, kLMoveCloser,
    kLDelayFast, kLDelayFine, kLDelayChoppy, kLTrySmoother, kLLocalFound, kLLocalBlocked,
    kLCount
};
const int kMaxLines = 2;
struct Verdict {
    uint8_t headline;              // Headline
    uint8_t lineCount;
    uint8_t lines[kMaxLines];      // Line
    bool good;                     // everything that was measured is fine
};

// ---- pure helpers (tested) ------------------------------------------------------------------------------------------------------------------
uint8_t WifiResult(const Wifi& w);                  // kWifiNone / kWifiWeak / kWifiGood / kWifiUnknown
uint8_t DelayResult(int ms);                        // kDelayFast / kDelayFine / kDelayChoppy; a nonsense number (negative, over ten minutes) is kNone
uint8_t HostResult(int nat);                        // np_net.h NatClass: 1 and 2 direct, 3 relay, anything else unknown
uint8_t InfoResult(const Info& info);               // InfoOutcome -> kNet*; an unknown outcome code is kNetDown
Verdict MakeVerdict(const uint8_t result[kChecks]);
// The sentences of a verdict joined by one space (the popup wraps them; each is a sentence of np_text_strings.tsv). Returns the length written, NUL terminated.
size_t VerdictText(const Verdict& v, char* out, size_t cap);
const char* HeadlineText(uint8_t headline);
const char* LineText(uint8_t line);
const char* HeadlineName(uint8_t headline);       // one word for the diagnostics ring: "good", "relay", "choppy", "wrong-clock", ...
// One short line for the diagnostics report: "wifi=good local=- internet=ok hosting=direct delay=fine (35 ms)". No names, no addresses.
size_t ReportLine(const uint8_t result[kChecks], const int16_t detail[kChecks], char* out, size_t cap);
const char* ResultName(uint8_t result);

// ---- "Who can I play with?" (ux13 O8; borrowed from Galaxy 3.0, which replaced its Connection Test with it) ------------------------------------------------------
// The same five results, read as three plain answers: people in this room (same Wi-Fi or Nearby), a friend far away (with a code), anyone online. Pure: a table, no sockets.
// The caller (np_menu_flow.cpp, MSC_WHO_CAN_I_PLAY_WITH=1) joins title, then label + phrase of each question, translating each piece through np_text_strings.tsv.
enum Who : uint8_t { kWhoRoom = 0, kWhoFar, kWhoAnyone, kWhoCount };
enum WhoPhrase : uint8_t {
    kPNone = 0,
    kPRoomYes, kPRoomNearbyOnly, kPRoomBlocked,   // this room: Wi-Fi and Nearby / Nearby only (no Wi-Fi) / Nearby, the Wi-Fi may block other Vitas
    kPCodeYes, kPCodeLag,                          // far away: yes with a code / yes, but it may lag
    kPAnyYes, kPAnyLag,                            // anyone online: yes / yes, but it may lag
    kPNoWifi, kPNoClock, kPNoUpdate, kPNoPaused, kPNoDown, kPNoNotSetUp,   // not now, and why (far away and anyone online)
    kPCount
};
uint8_t WhoAnswer(const uint8_t result[kChecks], int who);   // WhoPhrase
const char* WhoTitleText();
const char* WhoLabelText(int who);
const char* WhoPhraseText(uint8_t phrase);
const char* WhoPhraseName(uint8_t phrase);                    // one word for the log
bool WhoNo(uint8_t phrase);                                    // the answer is "not now"

// ---- the engine -----------------------------------------------------------------------------------------------------------------------------
struct View {
    uint8_t state[kChecks];        // State
    uint8_t result[kChecks];       // Result (kNone until the check is done)
    int16_t detail[kChecks];       // Wi-Fi quality, rooms found, delay ms; -1 none
    bool running;
    bool done;                     // the run finished and `verdict` is valid
    Verdict verdict;
    uint32_t elapsedMs;            // the whole run's own time
    uint32_t serial;               // changes whenever anything above changes
};

class Run {
public:
    Run();
    void Start(const Probes& probes, const Budget& budget = Budget());   // a run in progress is cancelled first
    void Pump();                                                         // once a frame; never blocks
    void Cancel();                                                       // stops what runs; a finished run stays as it is
    bool Running() const { return view_.running; }
    const View& GetView() const { return view_; }

private:
    void Begin(int step);
    void Finish(uint8_t result, int detail);
    void FinishRun();
    uint64_t Deadline(uint32_t stepMs) const;
    void Release();

    Probes p_;
    Budget b_;
    View view_;
    int step_;
    uint64_t start_, stepStart_, stepEnd_;
    bool begun_;
    bool infoRunning_;
    bool subBegun_;
    int infoMs_;
    int samples_, sampleMin_;
};

}  // namespace NpConnTest

#endif
