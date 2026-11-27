// Always-on multiplayer diagnostics (wave 11, mpdiag18): a small in-memory ring of the multiplayer events that matter, kept in every build (release
// builds write no log), and "SAVE A REPORT": one scrubbed text file for a player to attach to a bug report.
//
//  - The ring is fixed storage (kRingSize records of 80 bytes in static memory, 20 KiB): Note() takes a spin lock for a few stores, never allocates, never
//    writes a file, and is called only on events (a session starts, a peer set is known, the input delay is chosen, a desync is detected, a link is lost,
//    a connection test finishes), never per frame or per packet. At most kPerKindCap records of one kind are kept per session (a desync that repeats every
//    tick cannot push the session's start out of the ring); the rest are only counted.
//  - Nothing here changes what a console sends or when a step runs. The hooks are one line each in np_lan.cpp / np_menu_flow.cpp.
//  - The report is written only when the player asks for it (Settings > MULTIPLAYER > SAVE A REPORT), never during play. Every line passes through
//    Scrub() first: addresses, names, ids, tokens, keys, invite codes, host names and user folders do not reach the file. The scrubber is a denylist of what
//    such things look like (Galaxy's mp_scrub, mp_report.cpp, is the model), so the report is also built from typed events that carry no such text by design.
#ifndef NP_MPDIAG_H
#define NP_MPDIAG_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

namespace NpMpDiag {

enum Kind : uint8_t {
    kSessionStart = 0,   // a = link (0 local, 1 online), b = machines when known
    kPeers,              // a = machines in the draft (including this one)
    kInputDelay,         // a = ticks, b = round trip ms (-1 unknown), c = 1 chosen by auto from the round trip, 0 fixed or the host's choice
    kDesync,             // a = network tick, b = 1 frame, 2 checksum, 3 random seed
    kConnectionLost,     // the game's own "connection lost": a = connection, b = reason code
    kLinkLost,           // the transport declared the link lost: text = the reason
    kMatchEnd,           // a = steps, b = starved steps, c = round trip ms; text = the last digest each console reported
    kSessionEnd,         // this console left the multiplayer session
    kSuspend,            // the console is going to sleep inside a session
    kResume,             // it woke up inside a session: a = 1 the link was closed
    kOnlineFail,         // a sentence the online menu showed for a failure: text
    kConnTest,           // a = verdict headline (np_conntest.h), text = the checks' words
    kNote,               // anything else of interest: text
    // (mpbeta27) appended: the session trail (see "the session trail" below); the ring only, never on the wire or in a file
    kStage,              // a = Stage reached, b = Link, c = Play
    kError,              // a = Stage it happened in, b = code (an HTTP status, a transport error number, or 0), text = the error's word ("TIMEOUT", "over_budget")
    kMismatch,           // a = network tick, b = 1 frame, 2 checksum, 3 seed; text = "ours %08x theirs %08x"
    kResultUpload,       // a = attempts, b = 1 accepted, 0 failed and kept for a retry, -1 dropped for good; text = the failure's word
    kPeerBuild,          // a = 1 the partner's frames carry our fingerprint, 0 a console with another build or rules was refused; text = "fingerprint %08x rules %02x"
    kLastRunCrashed,     // the previous start of the game ended inside a multiplayer session without leaving it (a crash or a power cut): text = the stage it was in
    kKindCount
};

const int kRingSize = 256;
const int kTextBytes = 56;
const int kPerKindCap = 12;

struct Event {
    uint32_t ms;         // since the first event / the process's start, milliseconds
    uint8_t kind;
    uint8_t reserved[3];
    int32_t a, b, c;
    char text[kTextBytes];
};

// ---- the ring (an instance is plain storage: the tests use their own) ----------------------------------------------------------------------
class Ring {
public:
    Ring() { Clear(); }
    void Clear();
    // Adds one record (text may be null; it is cut to kTextBytes - 1 and control characters become '?'). Returns false when the per-session cap of this kind was
    // reached (the record is only counted). No allocation, no file, no clock read of its own: ms is given.
    bool Push(uint32_t ms, Kind kind, int a, int b, int c, const char* text);
    void SessionBegin();                 // a new session: the per-kind counts start again (old records stay until they are overwritten)
    int Count() const { return count_ < kRingSize ? count_ : kRingSize; }
    uint32_t Total() const { return total_; }          // records pushed since Clear, kept or not
    uint32_t Suppressed() const { return suppressed_; }
    uint32_t Overwritten() const { const uint32_t kept = total_ - suppressed_; return kept > uint32_t(kRingSize) ? kept - uint32_t(kRingSize) : 0u; }   // kept records pushed out by newer ones
    // The i-th kept record, oldest first (0 .. Count() - 1).
    const Event& At(int i) const { return ev_[(head_ + kRingSize - Count() + i) % kRingSize]; }
private:
    Event ev_[kRingSize];
    int head_;
    int count_;
    uint32_t total_, suppressed_;
    uint16_t kindCount_[kKindCount];
};

// ---- the process's ring ---------------------------------------------------------------------------------------------------------------------
// Thread-safe (a spin lock around a few stores). `ms` is read from a monotonic clock inside.
void Note(Kind kind, int a = 0, int b = 0, int c = 0, const char* text = nullptr);
// The game's own log calls (NpLanLog): the lines that mean something are recorded as typed events ("DESYNC ... at network tick %d", "connection lost (connection %u,
// reason %d)", "sync error reported to the player", "input queue overflow"); every other format is ignored. Reads the arguments of the formats it knows.
void NoteGameLog(const char* format, va_list args);
void ClearForTest();
int Events();                            // records currently kept
uint32_t TotalEvents();

// ---- the scrubber ---------------------------------------------------------------------------------------------------------------------------
// Copies in[0..n) to out (NUL terminated, at most cap-1 bytes), line by line, replacing what must not leave the console:
//   name=, nick=, console=, friend=, room=, token=, key=, secret=, password=, account=, profile=, id=, user=, host=<text>   -> <hidden>   (a small number after host/peer/id= is kept)
//   code=<anything but a lower-case error word>                                                                           -> <hidden>
//   a.b.c.d, a.b.c.d:port, 81.12.x.x, IPv6, MAC-like, the core's hex "0a000105:41100"                                     -> <address>
//   32 or more hex digits, long letters+digits tokens, base64 blobs, UUIDs, "Bearer <x>"                                  -> <secret> / <id>
//   XXXX-XXXX, and a 6-8 character code after "joining" / "room" / "friend" / "code" / "invite"                           -> <code>
//   http(s)://host..., name.tld                                                                                           -> <host>
//   /home/<user>, /Users/<user>, C:\Users\<user>                                                                          -> .../<user>
// Returns the length written. Never reads past n, never writes past cap-1, no allocation. Deterministic.
size_t Scrub(const char* in, size_t n, char* out, size_t cap);

// ---- the report -----------------------------------------------------------------------------------------------------------------------------
struct ReportParts {
    const char* version;          // "1.3.0"
    const char* buildId;          // the build's identity (np_build_identity.h)
    unsigned fingerprint;         // the multiplayer fingerprint (0 unknown)
    const char* settings;         // "KEY = value" lines of a fixed allow-list (the caller leaves everything else out; the scrubber is a second guard)
    const Ring* ring;
    uint32_t nowMs;               // the same clock as the events: "now" in the report
    const char* connTest;         // the last connection test's lines (null: none)
    const char* trail;            // (mpbeta27) the session trail's sections (TrailReport; null or "": left out)
};
// Builds the report text (NUL terminated) into out and returns its length. If cap is too small the oldest events are dropped first, and the report says so.
size_t BuildReport(char* out, size_t cap, const ReportParts& parts);

// Gathers what this console knows (version, build, fingerprint, the multiplayer settings, the process's ring, the last connection test), builds the report and
// writes <dir>/mp-report.txt (dir null: "ux0:data/mscharged" on the console, NP_DATA_DIR or "." elsewhere). Returns true and fills `path` with the file's path
// (what the player is told). Game thread; blocks for one small file write. Never during play: the Settings screen is not offered in a session.
bool SaveReport(const char* dir, char* path, size_t pathCap);
extern const char* const kReportFile;   // "mp-report.txt"
// The same report text as SaveReport writes, built into `out` (at most cap-1 characters + NUL; the oldest events are dropped first when it does not fit). Returns its length.
size_t BuildCurrentReport(char* out, size_t cap);

// The last connection test's lines, kept for the report (scrubbed again there). Called by np_menu_flow.cpp when a test ends.
void SetLastConnTest(const char* text);


// ---- the log tail (public beta, 15 Nov 2026: SEND A REPORT) -------------------------------------------------------------------------------------
// The multiplayer lines the game logs (NpLanLog: sync checks, desyncs, lost connections ...) are also kept in 8 KiB of static memory, the newest last, whether or not
// the build writes a log. NoteLogLine formats one line (never more than 200 characters) and appends it; LogTail returns the newest lines, scrubbed (Scrub above), cut at
// a line start. Called only from NpLanLog, i.e. only in a multiplayer session; no allocation, no file.
const int kLogTailBytes = 8192;
void NoteLogLine(const char* format, va_list args);
size_t LogTail(char* out, size_t cap);
void LogTailClearForTest();

// ---- the last session's numbers, and the "offer a report once" rule ------------------------------------------------------------------------------
// Anonymous numbers of the newest session in the ring: no name, no id, no address. They go (1) in the problem report and (2) in the header X-Msc-Stats of the leave request
// (online/np_signal_http.cpp; the service keeps them in a ring and a daily aggregate).
struct SessionNumbers {
    bool valid;            // a session start was seen
    bool online;
    int durationSec;       // start to end (or to now while it runs)
    int delayTicks;        // the input delay used (the last one chosen), 0 unknown
    int rebuilds;          // link rebuilds / reconnects in the session (resumes from sleep that closed the link, lost links)
    int lossPermille;      // packet loss of the worst link at the last match end (0 unknown)
    int starved;           // starved lockstep steps (the console waited for the partner's input)
    int steps;
    int rttMs;             // the last round trip, -1 unknown
    bool ended;            // this console left the session
    bool errored;          // a desync, a lost connection, a lost link or an online failure came up
};
SessionNumbers SessionOf(const Ring& ring, uint32_t nowMs, int lossPermille);
SessionNumbers LastSession();
// "d=312;dl=2;rs=0;dr=1;sv=0;e=1" (the service's header; 0 characters when no session was seen). The weak hook of the leave request: NpMpDiagSessionHeader.
size_t SessionHeader(const SessionNumbers& n, char* out, size_t cap);
void SetLoss(int permille);                      // np_lan.cpp at a match's end: the worst link's loss (NpNetLossPermille), -1 unknown
// The offer: after a session that ended with an error the Multiplayer menu offers SEND A REPORT, once for that session. OfferWanted() is true from the first error of a
// session until MarkOffered(); a new session start arms it again.
bool OfferWanted();
void MarkOffered();
const char* LastConnTest();                       // the last connection check's text ("" none)

// ---- the session trail (mpbeta27: multiplayer is a public beta, the first matches between two Vitas happen in players' hands) ---------------------------
// What a report needs to say what went wrong where: the link and kind of play, the furthest stage reached, the errors on the way, the online requests (counted,
// with the last few kept without host, query, ids or codes), the lockstep's health (desync tick and both checksums, the longest wait for the partner) and the
// partner's build. Every call is made on an EVENT (a stage change, a request's end, a mismatch), never per frame or per step: a spin lock and a few stores. Nothing
// here is sent or written by itself: it is read when a report is built (BuildReport's [session] / [network] / [lockstep] / [peers] sections). MSC_MP_REPORT_PLUS=0
// leaves those sections out of the report (the trail is still kept: it is the same few stores).
enum Link : uint8_t { kLinkUnknown = 0, kLinkNearby, kLinkSameWifi, kLinkOnline };
enum Play : uint8_t { kPlayUnknown = 0, kPlayRoomHost, kPlayRoomJoin, kPlayQuick, kPlayRanked, kPlayFriends, kPlayLocal };
enum Stage : uint8_t {
    kStageNone = 0,
    kStageMenu,          // the Multiplayer menu was opened
    kStageLogin,         // online: the account / sign-in
    kStageQueue,         // online: in the ranked / quick queue
    kStageMatchFound,    // online: the service paired us
    kStageSignaling,     // online: room / endpoints through the service
    kStageConnect,       // the link to the partner is being made (punch, relay, radio)
    kStageLobby,         // the game's lobby / draft (the partner is seen)
    kStageInMatch,       // the lockstep match runs
    kStageResult,        // the match ended: the result screen
    kStageResultUpload,  // online ranked: the result is being reported to the service
    kStageCount
};
const char* LinkName(Link l);
const char* PlayName(Play p);
const char* StageName(Stage s);

void SetMode(Link link, Play play);                 // the player chose (menu); a new session starts the trail again (BeginTrail)
void BeginTrail();                                  // a new visit of the Multiplayer menu: furthest stage, errors and counters start again
void Reach(Stage s);                                // the stage now (the furthest one is kept); a repeat of the current stage is not recorded again
void Error(Stage where, int code, const char* word);   // where = kStageNone: the current stage
// One request to the online service finished (np_reqlog.h calls it for every request, whatever MSC_NET_REQLOG says). `path` is already safe (ReqLog::SafePath),
// failName null for an HTTP answer. Counted; the last kRequestLines are kept.
// An answer of 400 or more, or no answer (other than a cancel), is also an Error of the current stage, with the service's error word (`code` of its JSON) or the failure's name.
void NoteRequest(const char* method, const char* path, int status, const char* failName, uint32_t ms, int reused, int attempt, const char* codeWord);
const int kRequestLines = 12;
void NoteMismatch(int what, int tick, uint32_t ours, uint32_t theirs);   // the first mismatch of each kind in a match (InputRouter: the compare that failed)
void NoteWait(uint32_t waitedMs);                   // the lockstep waited this long for the partner (the end of a starved run): the longest is kept
void NotePeerBuild(bool same, uint32_t fingerprint, unsigned rules);
void NoteResultUpload(int attempts, int outcome, const char* word);   // outcome 1 accepted, 0 kept for a retry, -1 dropped
void NoteSockets(int live, int opened, int closed, int most, int refused);   // np_https: the socket counters at the end of a request

struct Trail {
    Link link;
    Play play;
    Stage now, furthest;
    uint32_t stageMs[kStageCount];      // when each stage was first reached in this trail (0 never), the events' clock
    int errors;
    Stage lastErrorStage;
    int lastErrorCode;
    char lastErrorWord[28];
    // requests
    int requests, answered2xx, answered4xx, answered5xx, failed, retries;
    uint32_t slowestMs;
    char lastFail[24];
    int failByName[6];                  // TIMEOUT, DNS, CONNECT, TLS, CANCELLED, other
    char lines[kRequestLines][64];      // "POST /v1/ranked/queue -> 200 312ms r" newest last
    int lineCount, lineHead;
    // sockets (np_https)
    int sockLive, sockOpened, sockClosed, sockMost, sockRefused;
    // lockstep
    uint32_t longestWaitMs;
    int waits;                          // starved runs over 1.5 s (the banner showed)
    int mismatchTick[4];                // by kind 1..3 (index 0 unused), -1 none
    uint32_t mismatchOurs[4], mismatchTheirs[4];
    // partner
    int peerSame;                       // 1 same build seen, 0 another build refused, -1 unknown
    uint32_t peerFingerprint;
    unsigned peerRules;
    // ranked result
    int uploadAttempts, uploadOutcome;  // outcome: 1 accepted, 0 retrying, -1 dropped, -2 none
    char uploadWord[24];
    bool lastRunCrashed;
    char crashedStage[24];
};
Trail CurrentTrail();
void TrailClearForTest();
// The [session] [network] [lockstep] [peers] lines of a report from a trail and the transport's figures (rtt/jitter/loss -1 unknown). Returns the length.
struct LinkFigures { int rttMs, jitterMs, lossPermille, relay, nat, keepAliveMs, peerLostMs, delayTicks, steps, starved; };
size_t TrailReport(const Trail& t, const LinkFigures& f, char* out, size_t cap);
bool ReportPlusOn();                                // MSC_MP_REPORT_PLUS (default 1)

// ---- crash on the next start (mpbeta27) ----------------------------------------------------------------------------------------------------------------
// A small marker file (<data>/mp-session.txt, one line: the stage) is written when a multiplayer session starts and at a stage change outside a match (the
// menu, the lobby; never during the match: no file is written in play) and removed when the session is left. If it is still there at the next start, the last run
// ended inside a session without leaving it: the event kLastRunCrashed arms the report offer for the next visit of the Multiplayer menu. Game thread only.
// MSC_MP_CRASH_MARK=0: no file, no check.
void SessionMarkWrite(const char* dir);             // dir null: the data folder (as SaveReport)
void SessionMarkClear(const char* dir);
bool SessionMarkCheckAtStart(const char* dir);      // true when the previous run left a marker (it is removed; the event and the offer are set)
extern const char* const kSessionMarkFile;          // "mp-session.txt"

}  // namespace NpMpDiag

extern "C" size_t NpMpDiagSessionHeader(char* out, size_t cap);
// (mpbeta27) C entry points for the online layer (weak there: the small host tests link without this file)
extern "C" void NpMpDiagNoteRequest(const char* method, const char* path, int status, const char* failName, unsigned ms, int reused, int attempt, const char* codeWord);
extern "C" void NpMpDiagNoteSockets(int live, int opened, int closed, int most, int refused);
extern "C" void NpMpDiagReach(int stage);
extern "C" void NpMpDiagError(int stage, int code, const char* word);
extern "C" void NpMpDiagResultUpload(int attempts, int outcome, const char* word);   // the numbers of the newest session as the header text; 0 when none

#endif
