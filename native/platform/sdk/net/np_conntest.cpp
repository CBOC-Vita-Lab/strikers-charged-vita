// The Connection Test, deeper: see np_conntest.h.
#include "np_conntest.h"

#include <stdio.h>
#include <string.h>

namespace NpConnTest {

// ---- pure helpers ---------------------------------------------------------------------------------------------------------------------------
uint8_t WifiResult(const Wifi& w) {
    if (!w.connected) return kWifiNone;
    if (w.quality < 0 || w.quality > 100) return kWifiUnknown;   // (a number outside 0..100 is not a measurement)
    return w.quality < kWeakPercent ? kWifiWeak : kWifiGood;
}

uint8_t DelayResult(int ms) {
    if (ms < 0 || ms > 600000) return kNone;
    if (ms < kFastMs) return kDelayFast;
    return ms < kChoppyMs ? kDelayFine : kDelayChoppy;
}

uint8_t HostResult(int nat) {
    if (nat == 1 || nat == 2) return kHostDirect;
    if (nat == 3) return kHostRelay;
    return kHostUnknown;
}

uint8_t InfoResult(const Info& info) {
    switch (info.outcome) {
    case kInfoOk: return kNetOk;
    case kInfoClock: return kNetClock;
    case kInfoNotSetUp: return kNetNotSetUp;
    case kInfoUpdate: return kNetUpdate;
    case kInfoOff: return kNetOff;
    default: return kNetDown;   // down, offline, or a code this build does not know
    }
}

Verdict MakeVerdict(const uint8_t r[kChecks]) {
    Verdict v;
    memset(&v, 0, sizeof v);
    const uint8_t w = r[kWifi], l = r[kLocal], n = r[kInternet], h = r[kHosting], d = r[kDelay];
    auto add = [&v](Line line) {
        if (v.lineCount >= kMaxLines) return;
        for (int i = 0; i < v.lineCount; i++)
            if (v.lines[i] == line) return;
        v.lines[v.lineCount++] = uint8_t(line);
    };
    const Line delayLine = d == kDelayFast ? kLDelayFast : d == kDelayFine ? kLDelayFine : kLNone;
    if (w == kWifiNone) {
        v.headline = kHNoWifi;
        add(kLConnectWifi), add(kLNearbyStill);
    } else if (n == kNetClock) {
        v.headline = kHClock;
        add(kLClockBreaks), add(kLSetClock);
    } else if (w == kWifiWeak) {
        v.headline = kHWeakWifi;
        add(kLMoveCloser);
        add(n == kNetOk ? kLTrySmoother : kLLocalStill);
    } else if (n == kNetNotSetUp) {
        v.headline = kHNotSetUp;
        add(kLLocalStill);
    } else if (n == kNetUpdate) {
        v.headline = kHUpdate;
        add(kLUpdate), add(kLLocalStill);
    } else if (n == kNetOff) {
        v.headline = kHPaused;
        add(kLTryLater), add(kLLocalStill);
    } else if (n != kNetOk) {   // down, skipped, or never finished
        v.headline = kHDown;
        add(kLCheckWifi), add(kLLocalStill);
    } else if (d == kDelayChoppy) {
        v.headline = kHChoppy;
        add(kLDelayChoppy), add(kLTrySmoother);
    } else {
        v.headline = h == kHostRelay ? kHRelay : h == kHostDirect ? kHGood : kHUnknown;
        if (delayLine) add(delayLine);
    }
    if (n == kNetOk && l == kLocalBlocked) add(kLLocalBlocked);
    else if (n == kNetOk && l == kLocalFound && v.headline == kHGood) add(kLLocalFound);
    v.good = v.headline == kHGood;
    return v;
}

const char* HeadlineText(uint8_t headline) {
    switch (headline) {
    case kHGood: return "Your connection is good: other players should be able to reach you.";
    case kHRelay: return "Online play works, but your Wi-Fi hides this Vita from other players, so a match may have to go through a relay and lag. Another Wi-Fi network may work better.";
    case kHUnknown: return "The online service answered, but your Wi-Fi could not be checked fully. Try a match; if it does not connect, try another Wi-Fi network.";
    case kHChoppy: return "The connection is choppy.";
    case kHNoWifi: return "This Vita is not connected to Wi-Fi.";
    case kHWeakWifi: return "The Wi-Fi signal is weak.";
    case kHClock: return "The date and time on this Vita look wrong.";
    case kHDown: return "The online service could not be reached.";
    case kHNotSetUp: return "Online play is not available in this version of the game.";
    case kHUpdate: return "The game needs an update to play online.";
    case kHPaused: return "Online play is paused right now.";
    default: return "";
    }
}

const char* HeadlineName(uint8_t headline) {
    static const char* const kNames[kHCount] = {"-", "good", "relay", "unknown-hosting", "choppy", "no-wifi", "weak-wifi", "wrong-clock", "service-down", "not-set-up", "update", "paused"};
    return headline < kHCount ? kNames[headline] : "?";
}

const char* LineText(uint8_t line) {
    switch (line) {
    case kLConnectWifi: return "Connect to Wi-Fi in the Vita's Settings.";
    case kLNearbyStill: return "Nearby play needs no Wi-Fi and still works.";
    case kLLocalStill: return "Same Wi-Fi and Nearby play still work.";
    case kLClockBreaks: return "A clock that is off, even by a year, breaks secure connections.";
    case kLSetClock: return "Set it in Settings > Date & Time, then test again.";
    case kLCheckWifi: return "Check that your Wi-Fi has internet.";
    case kLUpdate: return "Update the game, then try again.";
    case kLTryLater: return "Please try again later.";
    case kLMoveCloser: return "Move closer to your router.";
    case kLDelayFast: return "The delay looks fast.";
    case kLDelayFine: return "The delay looks fine.";
    case kLDelayChoppy: return "Online games may lag.";
    case kLTrySmoother: return "Same Wi-Fi or Nearby may be smoother.";
    case kLLocalFound: return "Another Vita on this Wi-Fi was found.";
    case kLLocalBlocked: return "This Wi-Fi may block other Vitas.";
    default: return "";
    }
}

// ---- Who can I play with? ----------------------------------------------------------------------------------------------------------------------
namespace {
// Why the internet cannot be used now (kPNone: it can).
uint8_t InternetBlock(const uint8_t r[kChecks]) {
    if (r[kWifi] == kWifiNone) return kPNoWifi;
    switch (r[kInternet]) {
    case kNetOk: return kPNone;
    case kNetClock: return kPNoClock;
    case kNetUpdate: return kPNoUpdate;
    case kNetOff: return kPNoPaused;
    case kNetNotSetUp: return kPNoNotSetUp;
    default: return kPNoDown;   // down, skipped or never finished
    }
}
}  // namespace

uint8_t WhoAnswer(const uint8_t r[kChecks], int who) {
    if (who == kWhoRoom) {
        if (r[kWifi] == kWifiNone) return kPRoomNearbyOnly;
        return r[kLocal] == kLocalBlocked ? kPRoomBlocked : kPRoomYes;
    }
    if (who != kWhoFar && who != kWhoAnyone) return kPNone;
    const uint8_t no = InternetBlock(r);
    if (no) return no;
    const bool lag = r[kWifi] == kWifiWeak || r[kHosting] == kHostRelay || r[kDelay] == kDelayChoppy;
    if (who == kWhoFar) return r[kHosting] == kHostRelay || r[kWifi] == kWifiWeak ? kPCodeLag : kPCodeYes;
    return lag ? kPAnyLag : kPAnyYes;
}

const char* WhoTitleText() { return "Who can I play with?"; }

const char* WhoLabelText(int who) {
    switch (who) {
    case kWhoRoom: return "People in this room:";
    case kWhoFar: return "A friend far away:";
    case kWhoAnyone: return "Anyone online:";
    default: return "";
    }
}

const char* WhoPhraseText(uint8_t phrase) {
    switch (phrase) {
    case kPRoomYes: return "yes, on the same Wi-Fi or with Nearby play.";
    case kPRoomNearbyOnly: return "yes, with Nearby play (no Wi-Fi needed).";
    case kPRoomBlocked: return "Nearby play works; this Wi-Fi may block other Vitas.";
    case kPCodeYes: return "yes, with a code you give them.";
    case kPCodeLag: return "yes, with a code, but it may lag on this Wi-Fi.";
    case kPAnyYes: return "yes.";
    case kPAnyLag: return "yes, but games may lag.";
    case kPNoWifi: return "not yet: connect to Wi-Fi first.";
    case kPNoClock: return "not yet: set the date and time first.";
    case kPNoUpdate: return "not yet: update the game first.";
    case kPNoPaused: return "not right now: online play is paused.";
    case kPNoDown: return "not right now: the online service could not be reached.";
    case kPNoNotSetUp: return "not in this version of the game.";
    default: return "";
    }
}

const char* WhoPhraseName(uint8_t phrase) {
    static const char* const kNames[kPCount] = {"-", "room-yes", "room-nearby-only", "room-blocked", "code-yes", "code-lag", "any-yes", "any-lag",
                                                "no-wifi", "no-clock", "no-update", "no-paused", "no-down", "no-not-set-up"};
    return phrase < kPCount ? kNames[phrase] : "?";
}

bool WhoNo(uint8_t phrase) { return phrase >= kPNoWifi && phrase < kPCount; }

size_t VerdictText(const Verdict& v, char* out, size_t cap) {
    if (!out || !cap) return 0;
    size_t n = 0;
    out[0] = 0;
    auto put = [&](const char* s) {
        if (!*s) return;
        if (n && n + 1 < cap) out[n++] = ' ';
        while (*s && n + 1 < cap) out[n++] = *s++;
        out[n] = 0;
    };
    put(HeadlineText(v.headline));
    for (int i = 0; i < v.lineCount && i < kMaxLines; ++i) put(LineText(v.lines[i]));
    return n;
}

const char* ResultName(uint8_t r) {
    static const char* const kNames[kResultCount] = {"-", "skipped", "good", "weak", "none", "unknown", "found", "ok", "blocked", "ok", "down", "wrong-clock", "not-set-up",
                                                     "update", "paused", "direct", "relay", "unknown", "fast", "fine", "choppy"};
    return r < kResultCount ? kNames[r] : "?";
}

size_t ReportLine(const uint8_t r[kChecks], const int16_t d[kChecks], char* out, size_t cap) {
    if (!out || !cap) return 0;
    const int n = snprintf(out, cap, "wifi=%s local=%s internet=%s hosting=%s delay=%s", ResultName(r[kWifi]), ResultName(r[kLocal]), ResultName(r[kInternet]),
                           ResultName(r[kHosting]), ResultName(r[kDelay]));
    size_t len = n < 0 ? 0 : (size_t(n) >= cap ? cap - 1 : size_t(n));
    if (d && d[kDelay] >= 0 && len + 12 < cap) len += size_t(snprintf(out + len, cap - len, " (%d ms)", int(d[kDelay])));
    return len;
}

// ---- the engine -----------------------------------------------------------------------------------------------------------------------------
Run::Run() : step_(kChecks), start_(0), stepStart_(0), stepEnd_(0), begun_(false), infoRunning_(false), subBegun_(false), infoMs_(-1), samples_(0), sampleMin_(-1) {
    memset(&view_, 0, sizeof view_);
}

uint64_t Run::Deadline(uint32_t stepMs) const {
    const uint64_t byStep = stepStart_ + stepMs;
    const uint64_t byRun = start_ + b_.total;
    return byStep < byRun ? byStep : byRun;
}

void Run::Release() {
    if (infoRunning_ && p_.infoCancel) p_.infoCancel(p_.ctx);
    infoRunning_ = false;
}

void Run::Start(const Probes& probes, const Budget& budget) {
    if (view_.running) Cancel();
    p_ = probes;
    b_ = budget;
    const uint32_t serial = view_.serial + 1;
    memset(&view_, 0, sizeof view_);
    view_.serial = serial;
    for (int i = 0; i < kChecks; ++i) view_.detail[i] = -1;
    view_.running = p_.now != 0;
    step_ = 0;
    begun_ = false;
    infoRunning_ = false;
    infoMs_ = -1;
    start_ = p_.now ? p_.now(p_.ctx) : 0;
}

void Run::Cancel() {
    Release();
    if (view_.running) {
        view_.running = false;
        ++view_.serial;
    }
}

void Run::Begin(int step) {
    begun_ = true;
    stepStart_ = p_.now(p_.ctx);
    view_.state[step] = kRunning;
    infoRunning_ = false;
    subBegun_ = false;
    samples_ = 0;
    sampleMin_ = -1;
    ++view_.serial;
}

void Run::Finish(uint8_t result, int detail) {
    Release();
    view_.state[step_] = kDone;
    view_.result[step_] = result;
    view_.detail[step_] = int16_t(detail < -1 ? -1 : (detail > 32000 ? 32000 : detail));
    ++step_;
    begun_ = false;
    ++view_.serial;
}

void Run::FinishRun() {
    Release();
    view_.running = false;
    view_.done = true;
    view_.verdict = MakeVerdict(view_.result);
    const uint64_t now = p_.now(p_.ctx);
    view_.elapsedMs = uint32_t(now > start_ ? now - start_ : 0);
    ++view_.serial;
}

void Run::Pump() {
    if (!view_.running) return;
    for (;;) {
        if (step_ >= kChecks) {
            FinishRun();
            return;
        }
        const uint64_t now = p_.now(p_.ctx);
        if (!begun_) Begin(step_);
        const bool outOfTime = now >= start_ + b_.total;
        const uint8_t net = view_.result[kInternet];
        switch (step_) {
        case kWifi: {
            Wifi w;
            memset(&w, 0, sizeof w);
            w.quality = -1;
            const bool known = p_.wifi && p_.wifi(p_.ctx, &w);
            Finish(known ? WifiResult(w) : uint8_t(kWifiUnknown), known && w.quality >= 0 && w.quality <= 100 ? w.quality : -1);
            break;
        }
        case kLocal: {
            if (!p_.local || (outOfTime && !subBegun_)) { Finish(kSkipped, -1); break; }
            subBegun_ = true;
            const int n = p_.local(p_.ctx);
            if (n > 0) Finish(kLocalFound, n);
            else if (n == -1) Finish(kLocalBlocked, -1);
            else if (n == -2) Finish(kSkipped, -1);
            else if (now >= Deadline(b_.local)) Finish(kLocalOk, 0);
            else return;
            break;
        }
        case kInternet: {
            if (view_.result[kWifi] == kWifiNone || (outOfTime && !infoRunning_)) { Finish(kSkipped, -1); break; }
            if (p_.clock && !infoRunning_ && p_.clock(p_.ctx) == 0) { Finish(kNetClock, -1); break; }
            if (!p_.infoBegin || !p_.infoPoll) { Finish(kSkipped, -1); break; }
            if (!infoRunning_) {
                const uint64_t left = Deadline(b_.info) - now;
                if (!p_.infoBegin(p_.ctx, uint32_t(left))) { Finish(kNetDown, -1); break; }
                infoRunning_ = true;
            }
            Info info;
            memset(&info, 0, sizeof info);
            info.ms = -1;
            if (p_.infoPoll(p_.ctx, &info) == 1) {
                infoMs_ = info.ms >= 0 && info.ms <= 600000 ? info.ms : -1;
                Finish(InfoResult(info), infoMs_);
            } else if (now >= Deadline(b_.info)) {
                Finish(kNetDown, -1);   // (Release cancels the call)
            } else {
                return;
            }
            break;
        }
        case kHosting: {
            if (net != kNetOk || (outOfTime && !subBegun_) || !p_.hostBegin || !p_.hostPoll) { Finish(kSkipped, -1); break; }
            if (!subBegun_) {
                if (!p_.hostBegin(p_.ctx)) { Finish(kHostUnknown, -1); break; }
                subBegun_ = true;
            }
            int nat = 0;
            const int got = p_.hostPoll(p_.ctx, &nat);
            if (got == 1) Finish(HostResult(nat), nat);
            else if (got < 0 || now >= Deadline(b_.host)) Finish(kHostUnknown, -1);
            else return;
            break;
        }
        case kDelay: {
            if (net != kNetOk || (outOfTime && samples_ == 0)) { Finish(kSkipped, -1); break; }
            if (p_.rtt && samples_ < kDelaySamples) {
                const int ms = p_.rtt(p_.ctx);
                ++samples_;
                if (ms >= 0 && ms <= 600000 && (sampleMin_ < 0 || ms < sampleMin_)) sampleMin_ = ms;
                if (samples_ < kDelaySamples && now < Deadline(b_.rtt * kDelaySamples)) return;   // the next sample on the next pump
            }
            int ms = sampleMin_;
            if (ms < 0) ms = infoMs_;   // no round trip sampled: the service call's own time when it told one
            const uint8_t r = DelayResult(ms);
            Finish(r == kNone ? uint8_t(kSkipped) : r, r == kNone ? -1 : ms);
            break;
        }
        default:
            Finish(kSkipped, -1);
            break;
        }
    }
}

}  // namespace NpConnTest
