// np_online_api.h: the seam between the game's DWC calls and the online service.
//
// The game's login, clock and ranking calls (DWC_LoginAsync, DWC_GetDateTime, DWC_Rnk*) are implemented in this directory on top of
// the small asynchronous interface below. Behind it sits one of
//   - the in-process fake server (np_online_fake.cpp: host tests, scripted runs, offline checks on the console),
//   - the HTTPS client of the service (np_online_http.cpp: created by NpOnlineCreateHttpApi, supplied by the TLS layer),
//   - the "unavailable" API (every request fails at once with Fail::Transport) when neither exists.
//
// Contract:
//   - A Begin call returns a request id (> 0), or 0 when the request cannot be started. It may be called on any game thread.
//   - Poll is called on the game thread only, from the pump (DWC_ProcessFriendsMatch / DWC_RnkProcess). It never blocks. While the
//     request runs it answers Status::Pending. The first answer other than Pending is final: Ok (the reply is filled) or Failed;
//     the id is released by that answer. Cancel releases an id that was not polled to its end.
//   - Replies are untrusted: the DWC layer validates every field before the game sees it.
//   - Secrets (the per-player token) travel in LoginRequest / LoginReply only and are never logged.
#ifndef NP_ONLINE_API_H
#define NP_ONLINE_API_H

#include <stddef.h>
#include <stdint.h>

#include <vector>

namespace NpOnline {

const uint32_t kMaxRowData = 768;   // DWC_RNK_DATA_MAX
const uint32_t kMaxFriends = 64;    // DWC_RNK_FRIENDS_MAX
const uint32_t kMaxRows = 65;       // FRIENDS: 64 friends and the player
const uint32_t kTokenChars = 64;   // the service's bearer token as text (43 characters today) and its NUL
const uint32_t kNameBytes = 48;     // UTF-8, NUL included

enum class Status : uint8_t { Pending = 0, Ok, Failed };

// Why a request failed (the DWC layer maps these to the game's error codes, see np_dwc_error.h).
enum class Fail : uint8_t {
    None = 0,
    Transport,   // no network, DNS, TCP or TLS failure (including a refused certificate): nothing reached the service
    Timeout,     // the service did not answer in time
    Server,      // the service answered with an error of its own (5xx, maintenance)
    Rejected,    // the service refused the request (4xx other than 401/403)
    Auth,        // the token or account was refused (401/403)
    Protocol,    // the answer could not be understood
    Cancelled,
    RateLimited, // 429: wait and try again (Result::retryAfterSec)
    Upgrade,     // 426 upgrade_required: this build is too old for the service
    Disabled     // the service is switched off (kill switch, maintenance); StatusMessage() may explain
};

struct Result {
    Status status;
    Fail fail;
    int http;   // the HTTP status when one was received, else 0 (diagnostics only)
    int retryAfterSec;   // RateLimited / Disabled: the service's hint, else 0
    char code[28];       // the service's error code when it gave one ("friends_full", "bad_friend", "blocked" ...), else ""
    Result() : status(Status::Pending), fail(Fail::None), http(0), retryAfterSec(0) { code[0] = 0; }
    bool ok() const { return status == Status::Ok; }
};

// ---- login ------------------------------------------------------------------------------------------------------------------
struct LoginRequest {
    char name[kNameBytes];          // the public name, UTF-8, already validated (np_online_name.h)
    char token[kTokenChars];        // "" on the first login: the service creates the account and returns a token
    uint32_t region;                // DWC_RNK_REGION_* of the title (1 JP, 2 US, 4 EU, 8 KR): the account's region
    uint32_t gameCode;              // the title (R4QE / R4QP / R4QJ as a big-endian word)
    bool haveToken() const { return token[0] != 0; }
    LoginRequest() : region(2), gameCode(0) { name[0] = 0; token[0] = 0; }
};

enum class NameCheck : uint8_t { Unchecked = 0, Valid, Invalid };

struct LoginReply {
    int32_t profileId;              // > 0
    bool newToken;                  // `token` is a new token to store (account just created)
    char token[kTokenChars];
    NameCheck nameCheck;            // Invalid: the service refused the name (profileId is then 0); the game shows its "name not allowed" message
    int64_t serverTimeMs;           // UTC, milliseconds since 1970 (0 = not supplied)
    char code[16];                  // the account's public code "XXXX-XXXX" (safe to show and share; "" = not supplied)
    LoginReply() : profileId(0), newToken(false), nameCheck(NameCheck::Unchecked), serverTimeMs(0) { token[0] = 0; code[0] = 0; }
};

// ---- clock ------------------------------------------------------------------------------------------------------------------
struct TimeReply {
    int64_t unixMs;                 // UTC, milliseconds since 1970
    TimeReply() : unixMs(0) {}
};

// ---- ranking ----------------------------------------------------------------------------------------------------------------
struct PutRequest {
    uint32_t category;              // 0..100
    uint32_t region;                // DWC_RNK_REGION_*
    int32_t score;
    uint32_t size;                  // 0..768
    uint8_t data[kMaxRowData];      // the game's NetworkRankingSubmission (0x80 bytes), opaque to the client
    uint32_t attempt;               // 1 for the first try, then counts the retries from the persistent queue (header X-Msc-Attempt)
    char build[20];                 // the build that produced the score (header X-Msc-Build; "" = this build)
    PutRequest() : category(0), region(0), score(0), size(0), attempt(1) { build[0] = 0; }
};

struct GetRequest {
    uint32_t mode;                  // DWC_RNK_GET_MODE_*: 0 order, 1 top list, 2 near, 3 friends
    uint32_t category;
    uint32_t region;
    uint32_t sort;                  // 0 ascending, 1 descending
    uint32_t limit;                 // rows wanted (0 for ORDER)
    uint32_t since;                 // minutes: only scores updated within that window count
    uint32_t friendCount;           // FRIENDS: the profile ids below (zeros and duplicates already removed)
    int32_t friends[kMaxFriends];
    GetRequest() : mode(0), category(0), region(0), sort(0), limit(0), since(0), friendCount(0) {
        for (uint32_t i = 0; i < kMaxFriends; ++i) friends[i] = 0;
    }
};

struct Row {
    uint32_t order;                 // rank, 1 = best
    int32_t pid;
    int32_t score;
    uint32_t region;
    uint32_t lastupdate;
    uint32_t size;                  // userdata bytes (the game requires 0x80)
    uint8_t data[kMaxRowData];      // valid for min(size, kMaxRowData) bytes
    Row() : order(0), pid(0), score(0), region(0), lastupdate(0), size(0) {}
};

// ---- match result (the matchmaking layer reports each ranked match; both sides must agree, PROTOCOL.md section 6) -------------------
struct ResultReport {
    char matchId[32];
    int32_t result;                 // CalculateAndReportGameResult: 0 finished, 2 desync, 3/4 connection lost / queue overflow
    int32_t score0, score1;         // final goals of slot 0 and slot 1
    char checksum[72];              // hex digest of the end-of-match simulation state (the same on both sides)
    char mode[12];                  // "ranked" / "invite" (diagnostics)
    uint32_t attempt;               // 1 for the first try
    char build[20];                 // the build that played the match ("" = this build)
    ResultReport() : result(0), score0(0), score1(0), attempt(1) { matchId[0] = 0; checksum[0] = 0; mode[0] = 0; build[0] = 0; }
};

// The service's answer to a put: what it stored. `clamped`: the local total was ahead of what verified matches earned, so the service kept a
// lower value (PROTOCOL.md section 8); the next read of the player's own row shows the value to continue from.
struct PutReply {
    int32_t stored;
    bool clamped;
    PutReply() : stored(0), clamped(false) {}
};

// A match record as the service sees it (GET /v1/matches/{id}); `final` once the result is settled (verified, disputed, abandoned ...).
struct MatchStatus {
    bool final;
    char status[16];                // "verified" "disputed" "no_contest" "abandoned" "void" "awaiting_peer" "pending" "started"
    int32_t points;                 // the credit for this player when final
    bool won;
    MatchStatus() : final(false), points(0), won(false) { status[0] = 0; }
};

struct GetReply {
    uint32_t mode;                  // echo of the request
    uint32_t total;                 // players on that board (ORDER: the player's rank is in `order`)
    uint32_t order;
    std::vector<Row> rows;
    GetReply() : mode(0), total(0), order(0) {}
};

// ---- friends (PROTOCOL.md section 13) --------------------------------------------------------------------------------------------
enum class FriendState : uint8_t { Friend = 0, Incoming, Outgoing };           // Friend: both added each other
enum class Presence : uint8_t { Offline = 0, Menu, Queue, Room, Match };      // what the service derives from the polls that already exist
const uint32_t kStatusChars = 488;     // the game's friend status string, base64 (a few dozen bytes of payload), NUL included

struct FriendRow {
    int32_t pid;                  // the service's profile id (= the game's gs_profile_id)
    char code[16];                // "XXXX-XXXX"
    char name[kNameBytes];
    FriendState state;
    Presence presence;            // Friend only
    uint32_t lastSeenDay;         // days since 1970 (Friend only)
    char status[kStatusChars];    // the friend's own status string as the game set it ("" = none)
    char roomCode[8];             // the friend's open invite room ("Join their game"), "" = none
    char inviteRoom[8];           // "X invites you": the room code of a pending invite from this friend, "" = none
    FriendRow() : pid(0), state(FriendState::Friend), presence(Presence::Offline), lastSeenDay(0) { code[0] = name[0] = status[0] = roomCode[0] = inviteRoom[0] = 0; }
};

struct FriendsRequest {
    char rev[16];                 // the revision of the last answer ("" = send everything)
    uint32_t waitSec;             // > 0: a held poll (the service answers at once when something changes; PROTOCOL.md "Held polls")
    FriendsRequest() : waitSec(0) { rev[0] = 0; }
};

struct FriendsReply {
    bool changed;                 // false: nothing is new since `rev` (rows are empty)
    char rev[16];
    char ownCode[16];
    uint32_t max;                 // the list's size (64)
    uint32_t heldMs;              // > 0: the service held this answer
    uint32_t pollScalePct;        // 100 = none; above: stretch the gaps
    std::vector<FriendRow> rows;
    std::vector<FriendRow> blocked;   // pid, code, name only
    FriendsReply() : changed(false), max(0), heldMs(0), pollScalePct(100) { rev[0] = ownCode[0] = 0; }
};

struct PlayedRow {
    int32_t pid;
    char code[16];
    char name[kNameBytes];
    bool wasRanked, isFriend;
    uint32_t day;                 // days since 1970 of the last match together
    PlayedRow() : pid(0), wasRanked(false), isFriend(false), day(0) { code[0] = name[0] = 0; }
};
struct PlayedReply { std::vector<PlayedRow> rows; };

enum class FriendOp : uint8_t { Add = 0, Decline, Remove, Block, Unblock, Invite, ClearInvite, SetStatus };
struct FriendOpRequest {
    FriendOp op;
    int32_t pid;                  // the other player (> 0) ...
    char code[16];                // ... or their "XXXX-XXXX" code
    char roomCode[8];             // Invite: your own open room
    char status[kStatusChars];    // SetStatus
    FriendOpRequest() : op(FriendOp::Add), pid(0) { code[0] = roomCode[0] = status[0] = 0; }
};
struct FriendOpReply {
    FriendState state;            // Add: Friend or Outgoing
    int32_t pid;
    char code[16];
    char name[kNameBytes];
    FriendOpReply() : state(FriendState::Outgoing), pid(0) { code[0] = name[0] = 0; }
};

// ---- the interface ----------------------------------------------------------------------------------------------------------
class Api {
public:
    virtual ~Api() {}
    virtual uint32_t BeginLogin(const LoginRequest& request) = 0;
    virtual uint32_t BeginTime() = 0;
    virtual uint32_t BeginPutScore(const PutRequest& request) = 0;
    virtual uint32_t BeginGetScores(const GetRequest& request) = 0;
    virtual uint32_t BeginReportResult(const ResultReport& report) = 0;
    virtual uint32_t BeginMatchStatus(const char* matchId) = 0;
    virtual Result PollLogin(uint32_t id, LoginReply* reply) = 0;
    virtual Result PollTime(uint32_t id, TimeReply* reply) = 0;
    virtual Result PollPutScore(uint32_t id, PutReply* reply = nullptr) = 0;
    virtual Result PollGetScores(uint32_t id, GetReply* reply) = 0;
    virtual Result PollReportResult(uint32_t id) = 0;
    virtual Result PollMatchStatus(uint32_t id, MatchStatus* status) = 0;
    virtual void Cancel(uint32_t id) = 0;
    // friends: not pure, so an Api that predates them (a test double) still compiles; the default fails at once (Fail::Disabled)
    virtual uint32_t BeginFriends(const FriendsRequest&) { return 0; }
    virtual uint32_t BeginPlayedWith() { return 0; }
    virtual uint32_t BeginFriendOp(const FriendOpRequest&) { return 0; }
    virtual Result PollFriends(uint32_t, FriendsReply*) { Result r; r.status = Status::Failed; r.fail = Fail::Disabled; return r; }
    virtual Result PollPlayedWith(uint32_t, PlayedReply*) { Result r; r.status = Status::Failed; r.fail = Fail::Disabled; return r; }
    virtual Result PollFriendOp(uint32_t, FriendOpReply*) { Result r; r.status = Status::Failed; r.fail = Fail::Disabled; return r; }
};

// The API in use. Never null: without a service it is the "unavailable" API. The first call chooses:
//   MSC_ONLINE_API=fake            the in-process fake server (np_online_fake.h), seeded with a few players
//   otherwise                      NpOnlineCreateHttpApi() when the TLS layer provides it, else "unavailable"
Api* GetApi();
void SetApi(Api* api);      // tests and the fake; not owning. nullptr restores the default choice.

// Monotonic milliseconds (the clock of every timeout and of the cached server time). Tests replace it.
uint64_t NowMs();
void SetClockForTest(uint64_t (*clock)(void));

// What every request tells the service about this build (headers X-Msc-App, X-Msc-Build, X-Msc-Rules): the app version and build hash
// (compile-time NP_ONLINE_APP_VERSION / NP_ONLINE_BUILD or MSC_ONLINE_APP_VERSION), and the rules tag. The service may answer an old build with
// Fail::Upgrade.
struct ClientInfo {
    char appVersion[24];
    char build[20];
};
const ClientInfo& GetClientInfo();
// "" when the game plays as retail, "nk" while the NK-bug (Boo) fix of MSC_GAME_FIXES changes the simulation (np/gamefixes.h). Sent with every request.
const char* RulesTag();
// Ranked play and score submission are refused while RulesTag() is not empty (a modified simulation would disagree with the other player and
// pollute the boards). MSC_ONLINE_ALLOW_MODIFIED=1 lifts it for tests.
bool RulesAllowRanked();

// One line for the player about the last refusal (kill-switch message, "update required", "set the date and time"); "" when none.
// The game's pop-ups are fixed texts; the platform's UI may show this as well. Thread safe.
void SetStatusMessage(const char* text);
void GetStatusMessage(char* out, size_t size);

// Plain reason the service refused the chosen online name (taken, reserved, not allowed, format); "" when none. Thread safe.
void SetNameRejectMessage(const char* text);
void GetNameRejectMessage(char* out, size_t size);

// The account's public code ("K7QM-4DXT") as of the last login, or "" before the first one. Kept in <data_dir>/online_code.txt (not a secret).
// The game has no friend-code screen of its own that takes it, so it is only stored; the platform UI may show it.
void GetPublicCode(char* out, size_t size);
void SetPublicCode(const char* code);

// Where the client keeps its files (token, name): <NpSdk data_dir> on the console and the harness, NP_DATA_DIR or "." in tests.
const char* DataDir();

}  // namespace NpOnline

// The HTTPS-backed implementation, defined by the TLS layer when it is linked (np_online_http.cpp). Weak: may be absent.
extern "C" NpOnline::Api* NpOnlineCreateHttpApi(void) __attribute__((weak));

#endif
