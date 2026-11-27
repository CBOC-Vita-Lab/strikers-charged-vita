// np_online_fake.h: an in-process stand-in for the online service, for host tests, scripted runs and offline checks on a console
// (MSC_ONLINE_API=fake). It keeps accounts, one leaderboard per category and a server clock, answers deterministically (a request
// stays Pending for `latencyPolls` polls, then completes) and can be told to fail, stall or deliver damaged replies.
#ifndef NP_ONLINE_FAKE_H
#define NP_ONLINE_FAKE_H

#include "platform/sdk/dwc/np_online_api.h"

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace NpOnline {

struct FakeOptions {
    uint32_t latencyPolls;     // polls answered Pending before a request completes (default 2)
    int64_t serverTimeMs;      // the server clock at creation; 0 = the system clock. It advances with NowMs().
    bool seedPlayers;          // a dozen players on the season and Striker of the Day boards (default true)
    FakeOptions() : latencyPolls(2), serverTimeMs(0), seedPlayers(true) {}
};

// The game's NetworkRankingSubmission as the fake stores it (0x80 bytes, host byte order) with a valid HMAC-MD5 digest.
void FakeMakeUserData(uint8_t out[128], const char* name, int wins, int losses, int region, int day, int month, int year);

class FakeApi : public Api {
public:
    explicit FakeApi(const FakeOptions& options);
    ~FakeApi() override;

    uint32_t BeginLogin(const LoginRequest& request) override;
    uint32_t BeginTime() override;
    uint32_t BeginPutScore(const PutRequest& request) override;
    uint32_t BeginGetScores(const GetRequest& request) override;
    uint32_t BeginReportResult(const ResultReport& report) override;
    uint32_t BeginMatchStatus(const char* matchId) override;
    Result PollLogin(uint32_t id, LoginReply* reply) override;
    Result PollTime(uint32_t id, TimeReply* reply) override;
    Result PollPutScore(uint32_t id, PutReply* reply = nullptr) override;
    Result PollGetScores(uint32_t id, GetReply* reply) override;
    Result PollReportResult(uint32_t id) override;
    Result PollMatchStatus(uint32_t id, MatchStatus* status) override;
    void Cancel(uint32_t id) override;
    uint32_t BeginFriends(const FriendsRequest& request) override;
    uint32_t BeginPlayedWith() override;
    uint32_t BeginFriendOp(const FriendOpRequest& request) override;
    Result PollFriends(uint32_t id, FriendsReply* reply) override;
    Result PollPlayedWith(uint32_t id, PlayedReply* reply) override;
    Result PollFriendOp(uint32_t id, FriendOpReply* reply) override;

    // ---- friends: the other people of the fake service (the signed-in player is the one that logged in) ----
    int32_t FakeAddPerson(const char* name);                    // a registered player; returns the profile id
    void FakeSetPresence(int32_t pid, Presence presence);
    void FakeSetStatus(int32_t pid, const char* status);
    void FakeSetOpenRoom(int32_t pid, const char* roomCode);   // the person's open invite room ("Join their game")
    void FakePersonAdds(int32_t pid);                           // that person adds the signed-in player
    void FakePersonInvites(int32_t pid, const char* roomCode);
    void FakePlayedWith(int32_t pid, bool ranked);              // they played a match together
    uint32_t friendsCount, friendOpCount, playedCount;          // requests started
    std::vector<FriendOpRequest> friendOps;                     // every operation received
    uint32_t lastFriendsWaitSec;                                // `waitSec` of the last friends request

    // ---- test controls ----
    void FailNext(Fail fail, int http = 0);          // the next request that starts fails with this
    void FailAll(Fail fail, int http = 0);           // every request fails until Heal()
    void Heal();
    void Stall(bool stalled);                        // requests never complete (timeout tests)
    void RefuseStart(bool refuse);                   // Begin* return 0
    void SetServerTimeMs(int64_t unixMs);            // jump the server clock
    void SetMatchFinalAfterPolls(uint32_t statusRequests);   // match status stays "pending" for this many requests (default 0: final at once)
    void SetPutClamp(bool clamp, int32_t stored);            // puts answer clamped:true with this stored value
    void SetNameCheck(NameCheck check);              // what the next logins report
    void SetTamper(void (*fn)(GetReply* reply, void* user), void* user);   // damage replies before delivery
    void SeedBigRoster();                            // ~30 players on all six boards: names of 2..10 characters, ties, narrow/wide names (scripted UI checks)
    void SeedOwnScore(uint32_t category, int32_t score);   // the player's own row on this board, created when the player logs in
    void SynthesizeOwnRow(bool on);                  // NEAR and FRIENDS answer a zero row for a player without a score, as the service does
    void FailFrom(uint64_t nowMs);                   // FailWhen applies from this NowMs() on (default 0: at once)
    void FailWhen(Fail fail, int http, int kinds);   // every request of these kinds fails: bit mask of 1 login, 2 time, 4 put, 8 get (+16 report/status)
    int32_t SeedPlayer(const char* name, uint32_t category, int32_t score, uint32_t region, uint32_t minutesAgo, int wins = 0, int losses = 0);
    void SeedRaw(int32_t pid, uint32_t category, int32_t score, uint32_t region, uint32_t minutesAgo, const uint8_t* data, uint32_t size);
    int32_t LastLoginProfile() const { return m_lastProfile; }
    uint32_t loginCount, timeCount, putCount, getCount, reportCount, statusCount;   // requests started
    std::vector<ResultReport> reports;                     // every report received
    std::vector<PutRequest> puts;                          // every put received
    std::string events;                                    // one letter per request started: L login, T time, P put, G get, R report, S match status
    GetRequest lastGet;
    PutRequest lastPut;
    LoginRequest lastLogin;
    bool hasLastPut;

private:
    struct Entry { int32_t pid; int32_t score; uint32_t region; uint32_t lastupdate; uint32_t size; uint8_t data[kMaxRowData]; };
    struct Pending {
        int kind;                 // 1 login 2 time 3 put 4 get
        uint32_t polls;
        Fail fail; int http;
        LoginReply login; TimeReply time; GetReply get; MatchStatus match; PutReply put;
        FriendsReply friends; PlayedReply played; FriendOpReply fop; char code[28];
    };
    struct Person { std::string name, code, status, room; Presence presence; uint32_t day; };
    uint32_t Start(int kind, Pending** out);
    bool Settle(uint32_t id, int kind, Pending** p, Result* r);
    int64_t NowServerMs() const;
    uint32_t NowMinutes() const;
    void Build(const GetRequest& req, GetReply* out);

    FakeOptions m_options;
    int64_t m_timeBase, m_clockBase;
    std::map<uint32_t, Pending> m_pending;
    uint32_t m_next;
    std::map<std::string, int32_t> m_tokens;   // token -> profile id
    std::map<int32_t, std::string> m_names;
    std::map<uint32_t, std::vector<Entry> > m_boards;
    int32_t m_nextProfile, m_lastProfile, m_session;
    uint32_t m_rng;
    bool m_failNext, m_failAll, m_stalled, m_refuse;
    Fail m_failKind; int m_failHttp;
    NameCheck m_nameCheck;
    uint32_t m_matchPolls;
    bool m_clamp;
    int32_t m_clampStored;
    std::vector<std::pair<uint32_t, int32_t> > m_ownSeeds;
    Fail m_failWhenKind; int m_failWhenHttp; int m_failWhenMask; uint64_t m_failFromMs; bool m_synthOwn;
    std::map<int32_t, Person> m_people;
    std::set<std::pair<int32_t, int32_t> > m_links, m_blocks;          // (a, b): a added / blocked b
    std::map<std::pair<int32_t, int32_t>, std::string> m_invites;      // (to, from) -> room
    std::vector<std::pair<int32_t, bool> > m_played;
    std::string m_ownStatus;
    void (*m_tamper)(GetReply*, void*);
    void* m_tamperUser;
};

// For GetApi (MSC_ONLINE_API=fake) and tests.
Api* CreateFakeApi(const FakeOptions* options);

// Scripted-run scenarios from the environment (host and device checks; every variable is optional):
//   MSC_ONLINE_FAKE_ROSTER=big        replaces the dozen default players by SeedBigRoster()
//   MSC_ONLINE_FAKE_OWN=0:640;1:90    the player's own score per category (the row exists after login)
//   MSC_ONLINE_FAKE_FAIL=kind[:what]  kind transport|timeout|server|auth|upgrade|disabled|ratelimit; what login|rank|all (default rank)
//   MSC_ONLINE_FAKE_FAIL_FROM=<s>    the failures start this many seconds after the fake was created (the first screens stay normal)
//   MSC_ONLINE_FAKE_TIME=<unix s>     the service clock
void FakeApplyEnvironment(Api* api);
FakeOptions FakeOptionsFromEnvironment();

}  // namespace NpOnline

#endif
