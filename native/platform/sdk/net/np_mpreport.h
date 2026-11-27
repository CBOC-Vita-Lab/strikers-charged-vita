// SEND A REPORT and DELETE MY ONLINE ACCOUNT (public beta, 15 Nov 2026): what the player is told, what is sent, and the one-shot operation that sends it.
//
// The owner has one console, so what two consoles do to each other is learned from what players choose to send. Nothing here runs unless the player asks:
//   - SEND A REPORT: Settings > MULTIPLAYER, or the one offer the Multiplayer menu makes after an online session ended with an error (NpMpDiag::OfferWanted).
//   - DELETE MY ONLINE ACCOUNT: Settings > MULTIPLAYER.
// Both go through Op, which has a consent step that cannot be skipped: Start() refuses (and sends nothing) unless the player pressed SEND / YES after Ask() showed what
// would happen. The screens (np_menu_flow.cpp: the game's popups; np_vita_settings.cpp: the settings screen's dialog) only show the texts below and call Op.
//
// What a report carries (BuildPayload; the consent text says the same): the online name, the last lines of the multiplayer log (8 KiB kept in memory, scrubbed), the
// event list of the diagnostics ring (np_mpdiag.h, scrubbed), the last connection check, and the last session's anonymous numbers. IP addresses, MAC addresses, account
// ids, codes, tokens and the friend list never go: the scrubber (np_mpdiag.h Scrub) removes what looks like one, and the service scrubs again on the way in. At most 48 KiB.
//
// No game types and no network types here: the transport, the account and the clock are injected (Env), so the unit tests drive it with fakes. np_mpreport_net.cpp is
// the production Env (np_http, the menu's account files).
#ifndef NP_MPREPORT_H
#define NP_MPREPORT_H

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "np_mpdiag.h"

namespace NpMpReport {

const size_t kMaxBytes = 48 * 1024;        // the service's limit for one report (server/src/core/reports.js)
const size_t kMaxLogChars = 24000;         // the service's limits per field
const size_t kMaxEventChars = 16000;
const size_t kMaxConnChars = 1200;

// ---- the payload ------------------------------------------------------------------------------------------------------------------------------
struct Inputs {
    const char* version;     // "1.3.0"
    const char* build;       // hex build fingerprint
    const char* name;        // the online name (letters, digits, space . _ -)
    const char* log;         // the log tail, already scrubbed (BuildPayload scrubs it again)
    const char* events;      // the event list, already scrubbed (scrubbed again)
    const char* conn;        // the last connection check ("" none)
    NpMpDiag::SessionNumbers session;
};
// The JSON body of POST /v1/report in `out` (NUL terminated), never more than min(cap - 1, kMaxBytes) bytes. Anything that looks like an address, a token or an id is
// removed from the log, events and connection text (a second scrub, whatever the caller did). When the pieces do not fit, the oldest log lines go first. 0 when cap is
// too small for even the frame.
size_t BuildPayload(const Inputs& in, char* out, size_t cap);
// The payload of this process: the version, build, the account's name (Env::account), the diagnostics' log tail, event list, connection check and last session.
size_t GatherPayload(char* out, size_t cap);

// ---- the texts (English; every sentence is in np_text_strings.tsv in Spanish and French) ----------------------------------------------------------
const char* ConsentText();        // SEND A REPORT: exactly what is sent
const char* OfferText();          // the one offer after an online session ended with an error
const char* SendingText(int kind);   // "Sending the report..." / "Deleting your online account..."

// ---- the environment ---------------------------------------------------------------------------------------------------------------------------
struct Answer {
    int error;               // 0: an HTTP answer arrived (status, body); else a transport failure (text says why, for the player)
    int status;
    std::string body;
    std::string text;
    Answer() : error(0), status(0) {}
};
struct Account {
    char name[16];
    char token[72];
    char urls[320];          // "https://a,https://b", the service's addresses in order
    bool Ready() const { return token[0] != 0 && urls[0] != 0; }
    Account() { name[0] = token[0] = urls[0] = 0; }
};
struct Env {
    bool (*account)(Account* out);                     // the online account of this Vita (false: none)
    void (*forget)();                                  // forget the local online identity (token, name, code, queue)
    int (*start)(const char* method, const std::string& url, const std::string& body, const std::vector<std::string>& headers, uint32_t timeoutMs, std::string* err);   // handle > 0
    int (*poll)(int handle, Answer* out);              // 0 pending, 1 finished, -1 not a live handle
    void (*cancel)(int handle);
    uint64_t (*nowMs)();
    const char* (*appVersion)();
    unsigned (*fingerprint)();
    Env() : account(0), forget(0), start(0), poll(0), cancel(0), nowMs(0), appVersion(0), fingerprint(0) {}
};
void SetEnv(const Env& env);        // the production environment: UseNetwork() (np_mpreport_net.cpp); tests install fakes
const Env& GetEnv();

// ---- one operation ------------------------------------------------------------------------------------------------------------------------------
class Op {
public:
    enum Kind { kNone = 0, kReport = 1, kDelete = 2 };
    enum Phase { kIdle = 0, kAsked, kConsented, kSending, kDone };
    enum Outcome {
        kPending = 0,
        kSent,              // the report is stored
        kLimitReached,      // 3 a day, or too many tries
        kNoConnection,      // nothing reached the service
        kBusy,              // the service is over its day's budget or the reports are switched off
        kNoAccount,         // no online account on this Vita
        kTooBig,
        kRefused,           // the service refused it for another reason
        kDeleted,           // the account is gone (or was already gone) and this Vita forgot it
        kNotYet,            // the service does not know the call (an older version): nothing was changed
        kKeptNoConnection   // delete: the service could not be reached, so this Vita kept its identity (nothing was changed)
    };
    Op() : kind_(kNone), phase_(kIdle), outcome_(kPending), handle_(0), baseIdx_(0), startedAt_(0) {}
    // Shows what would happen: Idle or Done -> Asked. A report needs an account (false and kNoAccount otherwise).
    bool Ask(Kind kind);
    void Consent();                  // the player pressed SEND / YES: Asked -> Consented
    void Cancel();                   // the player pressed CANCEL / NO (or Circle): back to Idle; a request in flight is cancelled
    // Consented -> Sending: the request is started. False, and nothing is sent, in any other phase.
    bool Start();
    // Drive it (once a frame / loop). Returns the phase.
    Phase Step();
    Phase GetPhase() const { return phase_; }
    Kind GetKind() const { return kind_; }
    Outcome GetOutcome() const { return outcome_; }
    // The sentence for the player once Done.
    std::string Sentence() const;
    bool Succeeded() const { return outcome_ == kSent || outcome_ == kDeleted; }
    void Reset() { Cancel(); phase_ = kIdle; outcome_ = kPending; kind_ = kNone; }
    // For the tests: the request body and the number of requests this Op started.
    const std::string& LastBody() const { return body_; }
private:
    bool BeginRequest();
    void Finish(const Answer& a);
    Kind kind_;
    Phase phase_;
    Outcome outcome_;
    int handle_;
    int baseIdx_;
    uint64_t startedAt_;
    Account acct_;
    std::string body_, method_, path_, transportText_;
    bool triedMe_ = false;
};

// The production environment (np_mpreport_net.cpp): np_http, the menu's account files, the game clock. Idempotent.
void UseNetwork();

}  // namespace NpMpReport

#endif
