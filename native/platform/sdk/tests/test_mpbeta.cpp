// mpbeta27: the multiplayer beta's session trail in a problem report, the offer rules, the crash marker, the scrub of everything the trail adds, and the BETA labels.
//   c++ -std=c++17 -Wall -Wextra -I<native> native/platform/sdk/tests/test_mpbeta.cpp native/platform/sdk/net/np_mpdiag.cpp native/platform/sdk/net/np_mpreport.cpp -pthread -o /tmp/t
// Mutants (each must FAIL, tests/test_mpbeta.py): -DNP_MPDIAG_SCRUB_OFF, -DNP_MPDIAG_MUT_TRAIL_NOSCRUB, -DNP_MPDIAG_MUT_UPLOAD_NO_OFFER, -DNP_MPDIAG_MUT_MARK_KEEP,
// -DNP_MPBETA_MUT_NOTAG.
#include "platform/sdk/net/np_mpdiag.h"
#include "platform/sdk/net/np_mpreport.h"
#include "platform/sdk/net/np_mpbeta.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>

static int g_checks = 0, g_failed = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_failed; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define HAS(out, s) do { ++g_checks; if (!strstr((out), (s))) { ++g_failed; std::printf("FAIL %s:%d: \"%s\" missing in:\n%s\n", __FILE__, __LINE__, (s), (out)); } } while (0)
#define CLEAN(out, secret) do { ++g_checks; if (strstr((out), (secret))) { ++g_failed; std::printf("FAIL %s:%d: \"%s\" leaked in:\n%s\n", __FILE__, __LINE__, (secret), (out)); } } while (0)

using namespace NpMpDiag;

static void FreshState() {
    ClearForTest();
    TrailClearForTest();
    LogTailClearForTest();
}

static void LogLine(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    NoteLogLine(fmt, a);
    va_end(a);
}

static bool Exists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

// Secrets of every kind a session can meet; none may reach a report or a payload, whatever channel it came through.
static const char* const kToken = "q8Zk3mFv0pLw7xYtR2uN5oJcH1dGb4aE9sIeKfMh6Vc";
static const char* const kIp = "192.168.1.23";
static const char* const kPublicIp = "81.12.44.7";
static const char* const kFriend = "Zorbax";
static const char* const kRoom = "K7QM4D";
static const char* const kAccountId = "100003";

int main() {
    unsetenv("MSC_MP_BETA");
    unsetenv("MSC_MP_REPORT_PLUS");
    unsetenv("MSC_MP_CRASH_MARK");
    unsetenv("MSC_MP_BETA_NOTICE");

    // ---- 1. the trail of an online ranked session that ended in a desync ----
    {
        FreshState();
        BeginTrail();
        SetMode(kLinkOnline, kPlayRanked);
        Reach(kStageMenu);
        Reach(kStageLogin);
        Reach(kStageQueue);
        NoteRequest("POST", "/v1/ranked/queue", 200, nullptr, 312, 1, 0, "");
        NoteRequest("GET", "/v1/ranked/queue", 503, nullptr, 40, 0, 0, "over_budget");
        NoteRequest("GET", "/v1/friends", 0, "TIMEOUT", 10001, 0, 0, nullptr);
        NoteRequest("GET", "/v1/friends", 0, "CANCELLED", 31000, 0, 0, nullptr);
        NoteSockets(0, 106, 106, 1, 0);
        Reach(kStageMatchFound);
        Reach(kStageConnect);
        Reach(kStageLobby);
        NotePeerBuild(true, 0x1a2b3c4du, 1);
        Reach(kStageInMatch);
        NoteWait(120);
        NoteWait(1800);
        NoteWait(2600);
        NoteMismatch(2, 1504, 0x1a2b3c4du, 0x5e6f7a8bu);
        NoteMismatch(2, 1508, 0x11111111u, 0x22222222u);   // only the first of a kind is kept
        Reach(kStageResult);
        Reach(kStageResultUpload);
        NoteResultUpload(3, 0, "timeout");
        const Trail t = CurrentTrail();
        CHECK(t.link == kLinkOnline && t.play == kPlayRanked);
        CHECK(t.furthest == kStageResultUpload && t.now == kStageResultUpload);
        CHECK(t.requests == 4 && t.answered2xx == 1 && t.answered5xx == 1 && t.failed == 2);
        CHECK(t.errors == 2);                                 // the 503 and the time-out; a cancel is not an error
        CHECK(t.mismatchTick[2] == 1504 && t.mismatchOurs[2] == 0x1a2b3c4du && t.mismatchTheirs[2] == 0x5e6f7a8bu);
        CHECK(t.longestWaitMs == 2600 && t.waits == 2);
        CHECK(t.peerSame == 1);
        LinkFigures f = {84, 12, 3, 0, 2, 20000, 25000, 2, 5400, 120};
        char out[4096];
        const size_t n = TrailReport(t, f, out, sizeof out);
        CHECK(n == strlen(out) && n > 200);
        HAS(out, "[session]");
        HAS(out, "mode = online, ranked match");
        HAS(out, "stage reached = result upload");
        HAS(out, "login T+");
        HAS(out, "in-match T+");
        HAS(out, "errors = 2, the last in queue: TIMEOUT");
        HAS(out, "ranked result upload = failed, kept for a retry after 3 attempts, timeout");
        HAS(out, "[network]");
        HAS(out, "requests = 4 (2xx 1, 4xx 0, 5xx 1, no answer 2)");
        HAS(out, "timeout 1");
        HAS(out, "sockets = 0 open, 106 opened, 106 closed, most at once 1, refused 0");
        HAS(out, "round trip 84 ms, jitter 12 ms, loss 3 permille, relay no, router cone NAT");
        HAS(out, "POST /v1/ranked/queue -> 200 312ms reused");
        HAS(out, "GET /v1/friends -> TIMEOUT 10001ms");
        HAS(out, "[lockstep]");
        HAS(out, "input delay = 2 ticks; partner-lost limit 20000 ms (link), 25000 ms (online transport)");
        HAS(out, "steps = 5400, starved 120 (2.1%)");
        HAS(out, "retries 1");                                  // the same request again after one with no answer
        HAS(out, "waits for the partner = 2 over 1.5 s, the longest 2600 ms");
        HAS(out, "desync = checksum at step 1504: ours 1a2b3c4d theirs 5e6f7a8b");
        CLEAN(out, "11111111");
        HAS(out, "[peers]");
        HAS(out, "partner = the same build (fingerprint 1a2b3c4d, rules 01)");
        // the same trail in the whole report, under the [connection test] section
        char rep[16384];
        BuildCurrentReport(rep, sizeof rep);
        HAS(rep, "[session]");
        HAS(rep, "desync = checksum at step 1504");
        HAS(rep, "stage: in-match (online, ranked match)");     // the event list has the stages too
        HAS(rep, "mismatch at step 1504: checksum, ours 1a2b3c4d theirs 5e6f7a8b");
        CHECK(strstr(rep, "[connection test]") < strstr(rep, "[session]") && strstr(rep, "[session]") < strstr(rep, "[events]"));
        // MSC_MP_REPORT_PLUS=0: the report as before (no trail sections)
        setenv("MSC_MP_REPORT_PLUS", "0", 1);
        BuildCurrentReport(rep, sizeof rep);
        CHECK(!strstr(rep, "[session]") && !strstr(rep, "[lockstep]") && strstr(rep, "[events]"));
        unsetenv("MSC_MP_REPORT_PLUS");
    }

    // ---- 2. a partner with another build; a Nearby session; nothing seen ----
    {
        FreshState();
        SetMode(kLinkNearby, kPlayLocal);
        NotePeerBuild(false, 0xcafef00du, 3);
        char out[4096];
        LinkFigures f = {-1, -1, -1, -1, 0, -1, -1, 0, 0, 0};
        TrailReport(CurrentTrail(), f, out, sizeof out);
        HAS(out, "mode = Nearby, local match");
        HAS(out, "partner = ANOTHER build or rules was refused (fingerprint cafef00d, rules 03)");
        HAS(out, "desync = none detected");
        HAS(out, "errors = none");
        HAS(out, "relay unknown, router not measured");
        HAS(out, "input delay = 0 ticks (the game's own)");
        HAS(out, "steps = none recorded");
        CHECK(!strstr(out, "ranked result upload"));
        FreshState();
        TrailReport(CurrentTrail(), f, out, sizeof out);
        HAS(out, "partner = not seen");
        HAS(out, "stage reached = none");
    }

    // ---- 3. the offer: when a session ends badly, once ----
    {
        FreshState();
        Note(kSessionStart, 1);
        CHECK(!OfferWanted());
        NoteResultUpload(1, 0, "timeout");        // a failure kept for a retry: no offer yet
        CHECK(!OfferWanted());
        NoteResultUpload(2, 1, "reported");       // accepted
        CHECK(!OfferWanted());
        NoteResultUpload(24, -1, "timeout");      // dropped for good: offer
        CHECK(OfferWanted());
        MarkOffered();
        CHECK(!OfferWanted());
        Note(kSessionStart, 0);                   // a new session arms it again
        CHECK(!OfferWanted());
        Note(kDesync, 1504, 2);
        CHECK(OfferWanted());
        MarkOffered();
        Note(kSessionStart, 0);
        Note(kLinkLost, 0, 0, 0, "Wi-Fi lost");
        CHECK(OfferWanted());
        MarkOffered();
        Note(kSessionStart, 1);
        Note(kOnlineFail, 0, 0, 0, "No direct route to your partner was found.");
        CHECK(OfferWanted());
        MarkOffered();
        Note(kSessionStart, 1);
        Error(kStageQueue, 503, "over_budget");   // an error on the way that the session survived: no offer by itself
        NoteMismatch(2, 10, 1, 2);                 // (the router's DESYNC line follows a mismatch; the mismatch alone does not offer)
        CHECK(!OfferWanted());
    }

    // ---- 4. the crash marker ----
    {
        FreshState();
        char tmpl[] = "/tmp/mpbeta-XXXXXX";
        const char* dir = mkdtemp(tmpl);
        CHECK(dir != nullptr);
        const std::string mark = std::string(dir) + "/" + kSessionMarkFile;
        Reach(kStageLobby);
        SessionMarkWrite(dir);
        CHECK(Exists(mark));
        SessionMarkClear(dir);                    // the session was left: no marker
        CHECK(!Exists(mark));
        CHECK(!SessionMarkCheckAtStart(dir));
        CHECK(!OfferWanted());
        SessionMarkWrite(dir);                    // ... and this time the run "crashes" in the session
        CHECK(Exists(mark));
        FreshState();                             // the next start
        CHECK(SessionMarkCheckAtStart(dir));
        CHECK(!Exists(mark));                     // found once
        CHECK(OfferWanted());
        const Trail t = CurrentTrail();
        CHECK(t.lastRunCrashed && !strcmp(t.crashedStage, "lobby"));
        BeginTrail();                             // the menu's new visit keeps the finding
        CHECK(CurrentTrail().lastRunCrashed);
        char rep[16384];
        BuildCurrentReport(rep, sizeof rep);
        HAS(rep, "last run = ended inside a multiplayer session (stage lobby) without leaving it");
        HAS(rep, "the last run of the game ended inside a multiplayer session");
        MarkOffered();
        CHECK(!SessionMarkCheckAtStart(dir));     // not twice
        // MSC_MP_CRASH_MARK=0: no file, no finding
        setenv("MSC_MP_CRASH_MARK", "0", 1);
        SessionMarkWrite(dir);
        CHECK(!Exists(mark));
        unsetenv("MSC_MP_CRASH_MARK");
        rmdir(dir);
    }

    // ---- 5. the scrub: nothing a trail or a log line carries names a person, a network or an account ----
    {
        FreshState();
        SetMode(kLinkOnline, kPlayRoomJoin);
        Reach(kStageSignaling);
        // a path that was not made safe (it would be ReqLog::SafePath), error words with addresses and tokens in them, a log line with all of it
        NoteRequest("GET", "/v1/rooms/K7QM4D/q8Zk3mFv0pLw7xYtR2uN5oJcH1dGb4aE9sIeKfMh6Vc", 404, nullptr, 50, 0, 0, "room_not_found 192.168.1.23 q8Zk3mFv0pLw7xYtR2uN5oJcH1dGb4aE9sIeKfMh6Vc");
        NoteRequest("POST", "/v1/rooms", 0, "CONNECT 81.12.44.7:443", 6000, 0, 0, nullptr);
        Error(kStageNone, 0, "peer 192.168.1.23 token q8Zk3mFv0pLw7xYtR2uN5oJcH1dGb4aE9sIeKfMh6Vc");
        NoteResultUpload(1, -1, "match 100003 refused");
        LogLine("joining %s as name=%s friend=%s from %s:41100 with token=%s\n", kRoom, kFriend, kFriend, kIp, kToken);
        LogLine("login ok: profile %s (new account)\n", kAccountId);
        LogLine("partner name: %s at %s, invite from friend: %s\n", kFriend, kPublicIp, kFriend);
        LogLine("Bearer %s\n", kToken);
        char rep[16384];
        BuildCurrentReport(rep, sizeof rep);
        char tail[8192];
        LogTail(tail, sizeof tail);
        const char* const secrets[] = {kToken, "q8Zk3mFv0pLw7xYtR2uN5oJcH1dG", kIp, kPublicIp, kFriend, kRoom, kAccountId, "192168", "811244"};
        for (const char* s : secrets) {
            CLEAN(rep, s);
            CLEAN(tail, s);
        }
        HAS(rep, "room_not_found");                 // the service's word survives
        HAS(rep, "CONNECT");
        HAS(rep, "error in signaling: peer");          // the first word of a sentence with secrets in it, nothing more
        HAS(rep, "this Vita = version");
        // the payload of SEND A REPORT: built from the same texts, scrubbed again
        NpMpReport::Inputs in;
        in.version = "1.3.0";
        in.build = "1a2b3c4d";
        in.name = "Kit";
        in.log = tail;
        in.events = rep;
        in.conn = "";
        in.session = LastSession();
        static char payload[NpMpReport::kMaxBytes + 16];
        const size_t pn = NpMpReport::BuildPayload(in, payload, sizeof payload);
        CHECK(pn > 0 && pn <= NpMpReport::kMaxBytes);
        for (const char* s : secrets) CLEAN(payload, s);
        HAS(payload, "[session]");
        HAS(payload, "friend room, joining");
    }

    // ---- 6. the beta popup, clean labels and texts ----
    {
        // int34: the one-time beta popup is enough; menu entry labels stay clean without redundant BETA suffixes
        CHECK(!strcmp(NpMpBeta::Tag("ONLINE"), "ONLINE"));
        CHECK(!strcmp(NpMpBeta::Tag("LOCAL"), "LOCAL"));
        CHECK(!strcmp(NpMpBeta::Tag("NEARBY"), "NEARBY"));
        CHECK(!strcmp(NpMpBeta::Tag("SAME WI-FI"), "SAME WI-FI"));
        CHECK(!strcmp(NpMpBeta::Tag("RANKED MATCH"), "RANKED MATCH"));
        CHECK(!strcmp(NpMpBeta::Tag("PLAY WITH ANYONE"), "PLAY WITH ANYONE"));
        CHECK(!strcmp(NpMpBeta::Tag("FRIENDS"), "FRIENDS"));
        CHECK(!strcmp(NpMpBeta::Tag("HOST A ROOM"), "HOST A ROOM"));
        CHECK(!strcmp(NpMpBeta::Tag("BACK"), "BACK"));
        CHECK(NpMpBeta::NoticeMode() == 1 && NpMpBeta::ReportEntryOn());
        setenv("MSC_MP_BETA_NOTICE", "always", 1);
        CHECK(NpMpBeta::NoticeMode() == 2);
        setenv("MSC_MP_BETA_NOTICE", "0", 1);
        CHECK(NpMpBeta::NoticeMode() == 0);
        unsetenv("MSC_MP_BETA_NOTICE");
        setenv("MSC_MP_BETA", "0", 1);                                   // off: as before
        CHECK(!strcmp(NpMpBeta::Tag("ONLINE"), "ONLINE"));
        CHECK(NpMpBeta::NoticeMode() == 0);
        unsetenv("MSC_MP_BETA");
        setenv("MSC_MP_REPORT_ENTRY", "0", 1);
        CHECK(!NpMpBeta::ReportEntryOn());
        unsetenv("MSC_MP_REPORT_ENTRY");
        CHECK(strstr(NpMpBeta::NoticeText(), "Multiplayer is in beta.") == NpMpBeta::NoticeText());
        CHECK(strstr(NpMpBeta::NoticeText(), "problem report") != nullptr);
        CHECK(strstr(NpMpBeta::ResultLine(), "beta") == nullptr);        // no redundant beta mention on result screen
        CHECK(strlen(NpMpBeta::ResultLine()) <= 120);                    // np_notice.cpp kMaxChars
        CHECK(strstr(NpMpBeta::OfferLead(true, true), "closed") != nullptr);
        CHECK(!strcmp(NpMpBeta::OfferLead(false, true), "Your last online game ended with a problem."));
    }

    std::printf("mpbeta: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
