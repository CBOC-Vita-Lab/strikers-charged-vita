// Host tests of the online layer (platform/sdk/dwc): the DWC login, clock and ranking calls against the in-process fake service.
// No game data. Build and run: tools/dwc/run_dwc_test.sh
#include <dwc/dwc_account.h>
#include <dwc/dwc_base64.h>
#include <dwc/dwc_friend.h>
#include <dwc/dwc_error.h>
#include <dwc/dwc_init_fwd.h>
#include <dwc/dwc_main_fwd.h>
#include <dwc/dwc_nastime.h>
#include <dwc/dwc_ranking.h>

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

#include "platform/sdk/dwc/np_dwc.h"
#include "platform/sdk/dwc/np_online_api.h"
#include "platform/sdk/dwc/np_online_fake.h"
#include "platform/sdk/dwc/np_online_friends.h"
#include "platform/sdk/dwc/np_online_name.h"
#include "platform/sdk/dwc/np_online_json.h"
#include "platform/sdk/net/online/np_text_safe.h"
#include "platform/sdk/dwc/np_online_queue.h"
#include "platform/sdk/net/np_mpdiag.h"

using namespace NpOnline;


static int g_fail = 0, g_checks = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        ++g_checks;                                                               \
        if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } \
    } while (0)
#define CHECK_EQ(a, b)                                                                                           \
    do {                                                                                                         \
        ++g_checks;                                                                                              \
        const long long va = (long long)(a), vb = (long long)(b);                                                \
        if (va != vb) { printf("FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, va, vb); ++g_fail; } \
    } while (0)

// ---- scripted keyboard -------------------------------------------------------------------------------------------------------
static std::vector<std::string> g_imeAnswers;   // "" or "\x01" = cancel, else the text
static int g_imeOpens = 0, g_imePolls = 0;
static bool g_imeOpen = false;
static std::string g_imeInitial, g_imeTitle;
extern "C" int NpOnlineImeAvailable(void) { return 1; }
extern "C" int NpOnlineImeOpen(const char* initial, const char* title) { ++g_imeOpens; g_imeOpen = true; g_imeInitial = initial; g_imeTitle = title; g_imePolls = 0; return 0; }
extern "C" int NpOnlineImePoll(char* out, size_t size) {
    if (!g_imeOpen) return -1;
    if (++g_imePolls < 3) return 0;   // the dialog runs for two polls
    g_imeOpen = false;
    if (g_imeAnswers.empty()) return 2;
    std::string a = g_imeAnswers.front();
    g_imeAnswers.erase(g_imeAnswers.begin());
    if (a == "\x01") return 2;
    snprintf(out, size, "%s", a.c_str());
    return 1;
}
extern "C" const char* NpOnlineImeSuggestion(void) { return "Sam Example-Longname"; }

// ---- the test clock ----------------------------------------------------------------------------------------------------------
static uint64_t g_now = 1000000;
static uint64_t TestClock() { return g_now; }

// ---- helpers -----------------------------------------------------------------------------------------------------------------
static char g_dataDir[256];
alignas(8) static uint8_t g_block[0xB10];
static FakeApi* g_fake = nullptr;

static uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

// reference CRC-32, bitwise
static uint32_t RefCrc(const uint8_t* p, size_t n) {
    uint32_t crc = ~0u;
    for (size_t i = 0; i < n; ++i) { crc ^= p[i]; for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u))); }
    return ~crc;
}

extern "C" void NpDwcFriendsShutdown(void);
void NpDwcFriendsResetForTest();
static void Fresh(const FakeOptions& options = FakeOptions()) {
    NpDwcFriendsShutdown();
    NpDwcFriendsResetForTest();
    NpDwcResetForTest();
    NameResetForTest();
    SetApi(nullptr);
    delete g_fake;
    g_now = 1000000;
    SetClockForTest(TestClock);
    g_fake = new FakeApi(options);
    SetApi(g_fake);
    NpOnlineForgetCredentials();
    QueueResetForTest(false);
    unsetenv("MSC_ONLINE_ALLOW_MODIFIED");
    memset(g_block, 0, sizeof g_block);
    ForgetName();
    DWC_Init(DWC_SVR_RELEASE, "mschargedwii", 0x52345145, 0, 0);
    DWC_CreateUserData((DWCUserData*)g_block);
    DWC_InitFriendsMatch(0, (DWCUserData*)g_block, 0x2AAF, "mschargedwii", "key", 0, 0, 0, 0);
}

static int g_cbCount = 0, g_cbError = -1, g_cbPid = -1;
static void* g_cbParam = nullptr;
static bool g_inLoginCall = false, g_cbInsideCall = false;
static void LoginCb(DWCError error, int pid, void* param) { ++g_cbCount; g_cbError = (int)error; g_cbPid = pid; g_cbParam = param; if (g_inLoginCall) g_cbInsideCall = true; }

static void Pump(int n) { for (int i = 0; i < n; ++i) DWC_ProcessFriendsMatch(); }

static bool Login(const char* name = "Tester") {
    unsigned short wide[16] = {0};
    for (int i = 0; name[i] && i < 15; ++i) wide[i] = (unsigned short)name[i];
    g_cbCount = 0; g_cbError = -1; g_cbPid = -1;
    g_inLoginCall = true; g_cbInsideCall = false;
    const int started = DWC_LoginAsync(wide, 0, LoginCb, (void*)0);
    g_inLoginCall = false;
    if (!started) return false;
    for (int i = 0; i < 20 && !g_cbCount; ++i) Pump(1);
    return g_cbCount == 1 && g_cbError == 0;
}

static std::string Sequence(int maxCalls = 40) {
    std::string s;
    for (int i = 0; i < maxCalls; ++i) {
        const DWCRnkError r = DWC_RnkProcess();
        if (r == DWC_RNK_SUCCESS) s += 'S';
        else if (r == DWC_RNK_PROCESS_NOTASK) { s += 'N'; break; }
        else if (r == DWC_RNK_IN_ERROR) { s += 'E'; break; }
        else { s += '?'; break; }
    }
    return s;
}

static bool RankReady() {
    if (!Login()) return false;
    return DWC_RnkInitialize("initdata", (DWCUserData*)g_block) == DWC_RNK_SUCCESS;
}

static DWCRnkGetParam NearParam(u32 limit, u32 since = 100) { DWCRnkGetParam p; memset(&p, 0, sizeof p); p.size = sizeof(p.near); p.near.sort = 1; p.near.limit = limit; p.near.since = since; return p; }
static DWCRnkGetParam TopParam(u32 limit, u32 since = 100) { DWCRnkGetParam p; memset(&p, 0, sizeof p); p.size = sizeof(p.toplist); p.toplist.sort = 1; p.toplist.limit = limit; p.toplist.since = since; return p; }

static bool PutOwn(u32 category, s32 score, const char* name = "Tester") {
    uint8_t data[128];
    FakeMakeUserData(data, name, 3, 1, 2, 30, 10, 2026);
    if (DWC_RnkPutScoreAsync(category, DWC_RNK_REGION_US, score, data, sizeof data) != DWC_RNK_SUCCESS) return false;
    return Sequence() == "SSSN";
}

// ---- tests -------------------------------------------------------------------------------------------------------------------
static void TestCrcAndUserData() {
    printf("-- user data\n");
    const char* nine = "123456789";
    CHECK_EQ(NpDwcCrc32(nine, 9), 0xCBF43926u);

    Fresh();
    const uint8_t* b = g_block;
    CHECK(DWC_CheckUserData((DWCUserData*)g_block));
    CHECK_EQ(Rd32(b + 0), 64);
    CHECK_EQ(Rd32(b + 0x1C), 0);
    CHECK_EQ(Rd32(b + 0x24), 0x52345145u);
    CHECK_EQ(Rd32(b + 0x3C), RefCrc(b, 60));
    CHECK(DWC_CheckDirtyFlag((DWCUserData*)g_block));
    CHECK(!DWC_CheckHasProfile((DWCUserData*)g_block));
    CHECK_EQ((Rd32(b + 4) >> 11) & 3, 1);          // pseudo login id: data type 1
    CHECK_EQ((Rd32(b + 0x10) >> 11) & 3, 0);       // authentic: none yet
    g_block[5] ^= 1;                                // any change breaks the CRC
    CHECK(!DWC_CheckUserData((DWCUserData*)g_block));
    g_block[5] ^= 1;
    CHECK(DWC_CheckUserData((DWCUserData*)g_block));
    uint8_t zero[0x40] = {0};
    CHECK(!DWC_CheckUserData((const DWCUserData*)zero));

    NpDwcUserDataSetProfile(g_block, 4711);
    CHECK_EQ(Rd32(b + 0x1C), 4711);
    CHECK_EQ((Rd32(b + 0x10) >> 11) & 3, 1);       // authentic login id: data type 1
    CHECK_EQ(Rd32(b + 0x14), Rd32(b + 8));          // authentic := pseudo
    CHECK_EQ(Rd32(b + 0x18), Rd32(b + 12));
    CHECK_EQ(Rd32(b + 0x3C), RefCrc(b, 60));
    CHECK(DWC_CheckUserData((DWCUserData*)g_block));
    CHECK(DWC_CheckHasProfile((DWCUserData*)g_block));
    CHECK(DWC_CheckDirtyFlag((DWCUserData*)g_block));
    DWC_ClearDirtyFlag((DWCUserData*)g_block);
    CHECK(!DWC_CheckDirtyFlag((DWCUserData*)g_block));
    CHECK_EQ(Rd32(b + 0x3C), RefCrc(b, 60));
    CHECK(DWC_CheckUserData((DWCUserData*)g_block));
    CHECK_EQ(Rd32(b + 0x1C), 4711);
}

static void TestCredentialStore() {
    printf("-- credential store\n");
    Fresh();
    NpOnlineCredentials in, out;
    in.profileId = 1234;
    snprintf(in.token, sizeof in.token, "%s", "aB3dE5gH7jK9mN2pQ4sT6vX8zA1cF0hJ_lO-qR5uW");
    CHECK(!NpOnlineLoadCredentials(&out));
    CHECK(NpOnlineSaveCredentials(in));
    CHECK(NpOnlineLoadCredentials(&out));
    CHECK_EQ(out.profileId, 1234);
    CHECK(!strcmp(out.token, in.token));
    NpOnlineCredentials bad = in;
    snprintf(bad.token, sizeof bad.token, "%s", "short");
    CHECK(!NpOnlineSaveCredentials(bad));                   // not a token the service could have issued
    snprintf(bad.token, sizeof bad.token, "%s", "has a space in it and is long enough!!");
    CHECK(!NpOnlineSaveCredentials(bad));
    char path[300];
    snprintf(path, sizeof path, "%s/online.dat", g_dataDir);
    struct stat st;
    CHECK(stat(path, &st) == 0);
    CHECK_EQ(st.st_mode & 0777, 0600);
    CHECK_EQ(st.st_size, 4 + 4 + 4 + 64 + 4);
    snprintf(path, sizeof path, "%s/online.dat.tmp", g_dataDir);
    CHECK(stat(path, &st) != 0);
    snprintf(path, sizeof path, "%s/online.dat", g_dataDir);
    FILE* f = fopen(path, "r+b");
    fseek(f, 20, SEEK_SET);
    fputc(0xEE, f);
    fclose(f);
    CHECK(!NpOnlineLoadCredentials(&out));   // CRC mismatch: treated as no token
    NpOnlineForgetCredentials();
}

static void TestLogin() {
    printf("-- login\n");
    Fresh();
    unsigned short n[8] = {'A', 'b', 0};
    // before DWC_Init / DWC_InitFriendsMatch the call is refused
    NpDwcResetForTest();
    CHECK_EQ(DWC_LoginAsync(n, 0, LoginCb, 0), 0);
    DWC_ClearError();
    Fresh();
    g_fake->SetServerTimeMs(1793363696789ll);
    g_inLoginCall = true; g_cbInsideCall = false; g_cbCount = 0;
    const int started = DWC_LoginAsync(n, 0, LoginCb, (void*)7);
    g_inLoginCall = false;
    CHECK(started);
    CHECK_EQ(g_cbCount, 0);                    // never from inside the call
    CHECK(!g_cbInsideCall);
    CHECK(!DWC_LoginAsync(n, 0, LoginCb, 0));  // busy
    for (int i = 0; i < 2; ++i) { Pump(1); CHECK_EQ(g_cbCount, 0); }   // two polls pending
    Pump(1);
    CHECK_EQ(g_cbCount, 1);
    CHECK_EQ(g_cbError, 0);
    CHECK(g_cbPid > 0);
    CHECK_EQ((long)g_cbParam, 7);
    Pump(5);
    CHECK_EQ(g_cbCount, 1);                    // once only
    CHECK_EQ(DWC_GetIngamesnCheckResult(), 0);
    CHECK_EQ(Rd32(g_block + 0x1C), (uint32_t)g_cbPid);
    CHECK_EQ((Rd32(g_block + 0x10) >> 11) & 3, 1);
    CHECK_EQ(Rd32(g_block + 0x3C), RefCrc(g_block, 60));
    CHECK(DWC_CheckUserData((DWCUserData*)g_block));
    CHECK(DWC_CheckDirtyFlag((DWCUserData*)g_block));
    CHECK(!g_fake->lastLogin.haveToken());
    CHECK(!strcmp(g_fake->lastLogin.name, "Ab"));
    CHECK_EQ(g_fake->lastLogin.gameCode, 0x52345145u);
    const int firstPid = g_cbPid;
    NpOnlineCredentials creds;
    CHECK(NpOnlineLoadCredentials(&creds));
    CHECK_EQ(creds.profileId, firstPid);

    // second login (a new run): the stored token is used, same profile, no new write
    DWC_ClearDirtyFlag((DWCUserData*)g_block);
    const uint32_t crcBefore = Rd32(g_block + 0x3C);
    DWC_Shutdown();
    DWC_Init(DWC_SVR_RELEASE, "mschargedwii", 0x52345145, 0, 0);
    DWC_InitFriendsMatch(0, (DWCUserData*)g_block, 0x2AAF, "mschargedwii", "key", 0, 0, 0, 0);
    CHECK(Login("Again"));
    CHECK_EQ(g_cbPid, firstPid);
    CHECK(g_fake->lastLogin.haveToken());
    CHECK(!strcmp(g_fake->lastLogin.token, creds.token));
    CHECK_EQ(strlen(creds.token), 43u);
    CHECK(!DWC_CheckDirtyFlag((DWCUserData*)g_block));   // profile unchanged: the save is not touched again
    CHECK_EQ(Rd32(g_block + 0x3C), crcBefore);

    // the service no longer knows the token: one more login as a new player
    NpOnlineCredentials bogus = creds;
    bogus.token[0] = bogus.token[0] == 'a' ? 'b' : 'a';
    NpOnlineSaveCredentials(bogus);
    const uint32_t loginsBefore = g_fake->loginCount;
    CHECK(Login("Again"));
    CHECK_EQ(g_fake->loginCount, loginsBefore + 2);
    CHECK(g_cbPid != firstPid);
    CHECK_EQ(Rd32(g_block + 0x1C), (uint32_t)g_cbPid);
    CHECK(NpOnlineLoadCredentials(&creds) && creds.profileId == g_cbPid);

    // rejected name
    Fresh();
    g_fake->SetNameCheck(NameCheck::Invalid);
    StoreName("Badname");
    CHECK(Login("Badname"));
    CHECK_EQ(DWC_GetIngamesnCheckResult(), 2);
    CHECK_EQ(g_cbPid, 0);                                    // no account: nothing written into the save, nothing stored
    CHECK_EQ(Rd32(g_block + 0x1C), 0);
    NpOnlineCredentials noCreds;
    CHECK(!NpOnlineLoadCredentials(&noCreds));
    char stored[16];
    CHECK(!LoadStoredName(stored));                          // the player is asked for another name next time
    Fresh();
    g_fake->SetNameCheck(NameCheck::Valid);
    CHECK(Login("Good"));
    CHECK_EQ(DWC_GetIngamesnCheckResult(), 0);

    // wide names: non-ASCII becomes '?', long names are cut
    Fresh();
    unsigned short odd[16] = {'J', 0xE9, 'a', 'n', 'N', 'a', 'm', 'e', 'T', 'o', 'o', 'L', 'o', 'n', 'g', 0};
    g_cbCount = 0;
    CHECK(DWC_LoginAsync(odd, 0, LoginCb, 0));
    Pump(5);
    CHECK(!strcmp(g_fake->lastLogin.name, "J?anNameTo"));
    // an empty name is sent as "unnamed"
    Fresh();
    unsigned short empty[2] = {0, 0};
    CHECK(DWC_LoginAsync(empty, 0, LoginCb, 0));
    Pump(5);
    CHECK(!strcmp(g_fake->lastLogin.name, "unnamed"));
}

static void TestLoginFailures() {
    printf("-- login failures\n");
    long code = 0;
    DWCErrorType type = DWC_ERROR_NONE;
    Fresh();
    g_fake->FailNext(Fail::Transport);
    CHECK(!Login());
    CHECK_EQ(g_cbCount, 1);
    CHECK(g_cbError != 0);
    CHECK(DWC_GetLastErrorEx(&code, &type) != 0);
    CHECK_EQ(code, 20100);
    DWC_ClearError();
    CHECK_EQ(DWC_GetLastErrorEx(&code, &type), 0);
    CHECK_EQ(code, 0);
    CHECK_EQ(Rd32(g_block + 0x1C), 0);          // nothing written into the save
    CHECK_EQ(NpDwcLoggedInProfile(), 0);

    Fresh();
    g_fake->FailNext(Fail::Server, 503);
    CHECK(!Login());
    DWC_GetLastErrorEx(&code, &type);
    CHECK_EQ(code, 20101);
    DWC_ClearError();

    // a service that never answers: the callback arrives with a timeout error once the deadline passes
    Fresh();
    g_fake->Stall(true);
    unsigned short n[4] = {'A', 0};
    g_cbCount = 0;
    CHECK(DWC_LoginAsync(n, 0, LoginCb, 0));
    Pump(10);
    CHECK_EQ(g_cbCount, 0);
    g_now += 19000;
    Pump(1);
    CHECK_EQ(g_cbCount, 0);
    g_now += 2000;
    Pump(1);
    CHECK_EQ(g_cbCount, 1);
    CHECK(g_cbError != 0);
    DWC_GetLastErrorEx(&code, &type);
    CHECK_EQ(code, 80430);
    DWC_ClearError();
    // and a new login works afterwards
    g_fake->Stall(false);
    CHECK(Login());

    // the service refuses to start the request
    Fresh();
    g_fake->RefuseStart(true);
    CHECK(!DWC_LoginAsync(n, 0, LoginCb, 0));
    DWC_GetLastErrorEx(&code, &type);
    CHECK_EQ(code, 20100);
    DWC_ClearError();

    // a reply without a profile id
    Fresh();
    g_fake->FailNext(Fail::Protocol);
    CHECK(!Login());
    DWC_GetLastErrorEx(&code, &type);
    CHECK_EQ(code, 20104);
    DWC_ClearError();

    // a stale save: LoginAsync refuses a block with a bad CRC
    Fresh();
    g_block[9] ^= 0x55;
    CHECK(!DWC_LoginAsync(n, 0, LoginCb, 0));
    DWC_ClearError();
    // shutdown during a login: no callback
    Fresh();
    g_cbCount = 0;
    CHECK(DWC_LoginAsync(n, 0, LoginCb, 0));
    DWC_ShutdownFriendsMatch();
    Pump(10);
    CHECK_EQ(g_cbCount, 0);
}

static void TestToken_NotLogged() {
    printf("-- the token is never printed\n");
    // stderr into a file for the whole login/rank run, then look for the token's hex form
    char path[300];
    snprintf(path, sizeof path, "%s/stderr.txt", g_dataDir);
    fflush(stderr);
    const int saved = dup(2);
    FILE* f = freopen(path, "w", stderr);
    (void)f;
    Fresh();
    CHECK(RankReady());
    CHECK(PutOwn(0, 500));
    NpOnlineCredentials creds;
    NpOnlineLoadCredentials(&creds);
    DWC_Shutdown();
    DWC_Init(DWC_SVR_RELEASE, "mschargedwii", 0x52345145, 0, 0);
    DWC_InitFriendsMatch(0, (DWCUserData*)g_block, 0x2AAF, "mschargedwii", "key", 0, 0, 0, 0);
    Login();
    g_fake->FailNext(Fail::Server);
    DWC_RnkShutdown();
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    FILE* in = fopen(path, "rb");
    std::string text;
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) text.append(buf, n);
    fclose(in);
    const std::string token = creds.token;
    CHECK(!text.empty());
    CHECK(token.size() == 43);
    CHECK(text.find(token) == std::string::npos);
    CHECK(text.find(token.substr(0, 12)) == std::string::npos);
}

static void TestClockSuite() {
    printf("-- clock\n");
    Fresh();
    DWCDate d; DWCTime t;
    CHECK(!DWC_GetDateTime(&d, &t));                       // before any sync: FALSE, the game falls back
    g_fake->SetServerTimeMs(1793363696789ll);              // 2026-10-30 12:34:56.789 UTC, a Friday
    CHECK(Login());
    CHECK(DWC_GetDateTime(&d, &t));
    CHECK_EQ(d.year, 2026); CHECK_EQ(d.month, 9); CHECK_EQ(d.mday, 30); CHECK_EQ(d.wday, 5); CHECK_EQ(d.yday, 302);
    CHECK_EQ(t.hour, 12); CHECK_EQ(t.min, 34); CHECK_EQ(t.sec, 56); CHECK_EQ(t.msec, 789 + 0 * 1); CHECK_EQ(t.usec, 0);
    CHECK(DWC_GetDateTime(&d, nullptr));
    CHECK(DWC_GetDateTime(nullptr, &t));
    g_now += 61000;                                        // monotonic advance
    CHECK(DWC_GetDateTime(&d, &t));
    CHECK_EQ(t.hour, 12); CHECK_EQ(t.min, 35); CHECK_EQ(t.sec, 57);
    g_now += 12 * 3600 * 1000;                             // crosses midnight
    CHECK(DWC_GetDateTime(&d, &t));
    CHECK_EQ(d.mday, 31); CHECK_EQ(d.wday, 6); CHECK_EQ(t.hour, 0); CHECK_EQ(t.min, 35);

    // refresh: 30 minutes after the last sync one time request goes out, and a server jump is followed
    Fresh();
    g_fake->SetServerTimeMs(1793363696789ll);
    CHECK(Login());
    const uint32_t before = g_fake->timeCount;
    Pump(5);
    CHECK_EQ(g_fake->timeCount, before);
    g_now += 31 * 60 * 1000;
    g_fake->SetServerTimeMs(1793363696789ll + 7 * 86400000ll);   // the server says: a week later
    Pump(1);
    CHECK_EQ(g_fake->timeCount, before + 1);
    Pump(4);
    CHECK(DWC_GetDateTime(&d, &t));
    CHECK_EQ(d.mday, 6); CHECK_EQ(d.month, 10);            // 2026-11-06
    // a failing refresh keeps the cached time and retries only after 5 minutes
    g_now += 31 * 60 * 1000;
    g_fake->FailNext(Fail::Transport);
    Pump(5);
    CHECK(DWC_GetDateTime(&d, &t));
    const uint32_t afterFail = g_fake->timeCount;
    Pump(5);
    CHECK_EQ(g_fake->timeCount, afterFail);
    // offline from now on: the cached clock still answers
    g_fake->FailAll(Fail::Transport);
    g_now += 3600 * 1000;
    Pump(5);
    CHECK(DWC_GetDateTime(&d, &t));

    // civil conversion against libc over the 32-bit range, plus values beyond it
    uint32_t seed = 12345;
    int bad = 0;
    for (int i = 0; i < 20000; ++i) {
        seed = seed * 1664525u + 1013904223u;
        const time_t secs = (time_t)((seed >> 1) % 2147483647u);
        struct tm tmv;
        gmtime_r(&secs, &tmv);
        int y, mo, md, h, mi, s, ms, wd, yd;
        NpDwcClockCivil(int64_t(secs) * 1000 + 123, &y, &mo, &md, &h, &mi, &s, &ms, &wd, &yd);
        if (y != tmv.tm_year + 1900 || mo != tmv.tm_mon || md != tmv.tm_mday || h != tmv.tm_hour || mi != tmv.tm_min || s != tmv.tm_sec ||
            wd != tmv.tm_wday || yd != tmv.tm_yday || ms != 123) ++bad;
    }
    CHECK_EQ(bad, 0);
    int y, mo, md, h, mi, s, ms, wd, yd;
    NpDwcClockCivil(1709251199000ll, &y, &mo, &md, &h, &mi, &s, &ms, &wd, &yd);   // 2024-02-29 23:59:59, Thursday, leap day
    CHECK_EQ(y, 2024); CHECK_EQ(mo, 1); CHECK_EQ(md, 29); CHECK_EQ(wd, 4); CHECK_EQ(yd, 59);
    NpDwcClockCivil(2147483648000ll, &y, &mo, &md, &h, &mi, &s, &ms, &wd, &yd);   // 2038-01-19 03:14:08
    CHECK_EQ(y, 2038); CHECK_EQ(mo, 0); CHECK_EQ(md, 19); CHECK_EQ(h, 3); CHECK_EQ(mi, 14); CHECK_EQ(s, 8); CHECK_EQ(wd, 2); CHECK_EQ(yd, 18);
    NpDwcClockCivil(4107542400000ll, &y, &mo, &md, &h, &mi, &s, &ms, &wd, &yd);   // 2100-03-01 (2100 is not a leap year)
    CHECK_EQ(y, 2100); CHECK_EQ(mo, 2); CHECK_EQ(md, 1); CHECK_EQ(wd, 1); CHECK_EQ(yd, 59);
    CHECK(!NpDwcClockCivil(-1, &y, &mo, &md, &h, &mi, &s, &ms, &wd, &yd));
}

static void TestPublicCode() {
    printf("-- public code\n");
    Fresh();
    CHECK(Login());
    char code[16];
    GetPublicCode(code, sizeof code);
    CHECK(strlen(code) == 9 && code[4] == '-' && !strncmp(code, "FAKE-", 5));
    char path[300];
    snprintf(path, sizeof path, "%s/online_code.txt", g_dataDir);
    struct stat st;
    CHECK(stat(path, &st) == 0);
    SetPublicCode("not a code");                                  // refused: unchanged
    GetPublicCode(code, sizeof code);
    CHECK(!strncmp(code, "FAKE-", 5));
}

static void TestErrorNumbers() {
    printf("-- error numbers\n");
    struct { Fail fail; long code; } cases[] = {{Fail::Upgrade, 20110}, {Fail::Disabled, 20101}, {Fail::RateLimited, 20109}, {Fail::Auth, 20103}};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        Fresh();
        g_fake->FailNext(cases[i].fail, 0);
        CHECK(!Login());
        long code = 0;
        DWCErrorType type;
        DWC_GetLastErrorEx(&code, &type);
        CHECK_EQ(code, cases[i].code);
        DWC_ClearError();
    }
}

static void TestQueue() {
    printf("-- persistent retry queue\n");
    uint8_t data[128];
    FakeMakeUserData(data, "Tester", 3, 1, 2, 30, 10, 2026);
    Fresh();
    CHECK(RankReady());
    g_fake->FailNext(Fail::Transport);
    CHECK_EQ(DWC_RnkPutScoreAsync(0, DWC_RNK_REGION_US, 700, data, 128), DWC_RNK_SUCCESS);
    CHECK(Sequence() == "SSE");                              // the game is told
    DWC_ClearError();
    CHECK_EQ(QueuePending(), 1u);                              // and the score waits on disk
    char path[300];
    snprintf(path, sizeof path, "%s/online_queue.dat", g_dataDir);
    struct stat st;
    CHECK(stat(path, &st) == 0);
    CHECK_EQ(st.st_mode & 0777, 0600);
    char line[96];
    CHECK(QueueDescribe(0, line, sizeof line) && strstr(line, "put cat=0") && strstr(line, "attempts=1"));
    // a newer total for the same board replaces the undelivered one
    g_fake->FailNext(Fail::Server);
    FakeMakeUserData(data, "Tester", 4, 1, 2, 30, 10, 2026);
    CHECK_EQ(DWC_RnkPutScoreAsync(0, DWC_RNK_REGION_US, 750, data, 128), DWC_RNK_SUCCESS);
    CHECK(Sequence() == "SSE");
    DWC_ClearError();
    CHECK_EQ(QueuePending(), 1u);
    // another board is a second entry
    g_fake->FailNext(Fail::Timeout);
    CHECK_EQ(DWC_RnkPutScoreAsync(1, DWC_RNK_REGION_US, 90, data, 128), DWC_RNK_SUCCESS);
    CHECK(Sequence() == "SSE");
    DWC_ClearError();
    CHECK_EQ(QueuePending(), 2u);
    // a match result joins them
    ResultReport rep;
    snprintf(rep.matchId, sizeof rep.matchId, "aB3dE5gH7jK9");
    rep.result = 0; rep.score0 = 3; rep.score1 = 1;
    snprintf(rep.checksum, sizeof rep.checksum, "0f1e2d3c4b5a6978");
    snprintf(rep.mode, sizeof rep.mode, "ranked");
    CHECK(QueueResult(rep));
    CHECK_EQ(QueuePending(), 3u);
    ResultReport bad = rep;
    bad.matchId[0] = 0;
    CHECK(!QueueResult(bad));
    bad = rep; bad.score0 = 100;
    CHECK(!QueueResult(bad));
    CHECK_EQ(QueuePending(), 3u);

    // the Vita sleeps or the game ends: a new run reads the file
    NpDwcResetForTest();
    QueueResetForTest(true);
    DWC_Init(DWC_SVR_RELEASE, "mschargedwii", 0x52345145, 0, 0);
    DWC_InitFriendsMatch(0, (DWCUserData*)g_block, 0x2AAF, "mschargedwii", "key", 0, 0, 0, 0);
    CHECK_EQ(QueuePending(), 3u);
    const uint32_t putsBefore = g_fake->putCount;
    Pump(10);
    CHECK_EQ(g_fake->putCount, putsBefore);                    // nothing is sent before a login
    CHECK(Login());
    CHECK_EQ(DWC_RnkInitialize("initdata", (DWCUserData*)g_block), DWC_RNK_SUCCESS);
    for (int i = 0; i < 40 && QueuePending(); ++i) { g_now += 2500; Pump(1); }   // the status look is due 2 s after the report
    CHECK_EQ(QueuePending(), 0u);
    CHECK(stat(path, &st) != 0);                               // an empty queue leaves no file
    CHECK_EQ(g_fake->putCount, putsBefore + 2);
    CHECK_EQ(g_fake->reportCount, 1u);
    CHECK_EQ(g_fake->statusCount, 1u);                         // the report was followed by a look at the match record
    CHECK_EQ(g_fake->reports.back().score0, 3);
    CHECK(!strcmp(g_fake->reports.back().matchId, "aB3dE5gH7jK9"));
    CHECK(!strcmp(g_fake->reports.back().checksum, "0f1e2d3c4b5a6978"));
    CHECK(!strcmp(g_fake->reports.back().mode, "ranked"));
    CHECK_EQ(g_fake->reports.back().attempt, 1u);
    CHECK(!strcmp(g_fake->reports.back().build, GetClientInfo().build));
    bool saw750 = false, saw90 = false;
    for (size_t i = putsBefore; i < g_fake->puts.size(); ++i) {
        saw750 = saw750 || (g_fake->puts[i].category == 0 && g_fake->puts[i].score == 750 && g_fake->puts[i].attempt == 2);
        saw90 = saw90 || (g_fake->puts[i].category == 1 && g_fake->puts[i].score == 90);
    }
    CHECK(saw750 && saw90);
    CHECK(PutOwn(0, 800));                                     // normal puts still go through the queue and leave it
    CHECK_EQ(QueuePending(), 0u);

    // retry timing: a failing retry waits 2 s, then 4 s; a refusal drops the entry
    g_fake->FailNext(Fail::Transport);
    CHECK_EQ(DWC_RnkPutScoreAsync(2, DWC_RNK_REGION_US, 10, data, 128), DWC_RNK_SUCCESS);
    CHECK(Sequence() == "SSE");
    DWC_ClearError();
    Pump(1);                                                    // attempts=1: due in 1 s
    uint32_t sent = g_fake->putCount;
    Pump(5);
    CHECK_EQ(g_fake->putCount, sent);
    g_now += 1100;
    g_fake->FailNext(Fail::Server);
    Pump(1);
    CHECK_EQ(g_fake->putCount, sent + 1);
    Pump(5);                                                    // that retry failed: attempts=2, due in 2 s
    CHECK_EQ(QueuePending(), 1u);
    sent = g_fake->putCount;
    g_now += 1500;
    Pump(5);
    CHECK_EQ(g_fake->putCount, sent);
    g_now += 700;
    g_fake->FailNext(Fail::Rejected, 400);
    for (int i = 0; i < 6; ++i) Pump(1);
    CHECK_EQ(g_fake->putCount, sent + 1);
    CHECK_EQ(QueuePending(), 0u);                               // refused for good: dropped

    // a damaged file is ignored
    g_fake->FailNext(Fail::Transport);
    CHECK_EQ(DWC_RnkPutScoreAsync(3, DWC_RNK_REGION_US, 10, data, 128), DWC_RNK_SUCCESS);
    Sequence();
    DWC_ClearError();
    CHECK_EQ(QueuePending(), 1u);
    FILE* f = fopen(path, "r+b");
    CHECK(f != nullptr);
    if (f) {
        fseek(f, 40, SEEK_SET);
        fputc(0x5A, f);
        fclose(f);
    }
    QueueResetForTest(true);
    CHECK_EQ(QueuePending(), 0u);
}

extern "C" int g_npGameFixes;

static void TestFinalHold() {
    printf("-- puts wait for the match to become final\n");
    uint8_t data[128];
    FakeMakeUserData(data, "Tester", 3, 1, 2, 30, 10, 2026);
    Fresh();
    CHECK(RankReady());
    g_fake->SetMatchFinalAfterPolls(2);
    ResultReport rep;
    snprintf(rep.matchId, sizeof rep.matchId, "m1");
    rep.score0 = 2; rep.score1 = 1;
    snprintf(rep.checksum, sizeof rep.checksum, "abcdef0123456789");
    CHECK(QueueResult(rep));
    CHECK(!QueueMayPut());
    CHECK_EQ(DWC_RnkPutScoreAsync(0, DWC_RNK_REGION_US, 700, data, 128), DWC_RNK_SUCCESS);
    CHECK_EQ(g_fake->putCount, 0u);                           // held back
    std::string seq;
    for (int i = 0; i < 80; ++i) {
        const DWCRnkError r = DWC_RnkProcess();
        seq += r == DWC_RNK_SUCCESS ? 'S' : r == DWC_RNK_PROCESS_NOTASK ? 'N' : 'E';
        g_now += 700;
        Pump(1);
        if (r != DWC_RNK_SUCCESS) break;
    }
    CHECK(!seq.empty() && seq[seq.size() - 1] == 'N');
    CHECK_EQ(g_fake->putCount, 1u);
    const size_t rpos = g_fake->events.find('R');
    const size_t ppos = g_fake->events.find('P');
    if (rpos == std::string::npos || ppos == std::string::npos || rpos > ppos) printf("   events=%s seq=%s\n", g_fake->events.c_str(), seq.c_str());
    CHECK(rpos != std::string::npos && ppos != std::string::npos && rpos < ppos);
    if (ppos != std::string::npos) CHECK_EQ(std::count(g_fake->events.begin(), g_fake->events.begin() + long(ppos), 'S'), 3);   // two "not yet", then final
    CHECK(QueueMayPut());
    CHECK_EQ(QueuePending(), 0u);

    // a match that never settles: the game is told "no response" after 30 s, the put stays queued, and after 5 minutes it goes out anyway
    Fresh();
    CHECK(RankReady());
    g_fake->SetMatchFinalAfterPolls(100000);
    snprintf(rep.matchId, sizeof rep.matchId, "m2");
    CHECK(QueueResult(rep));
    CHECK_EQ(DWC_RnkPutScoreAsync(0, DWC_RNK_REGION_US, 700, data, 128), DWC_RNK_SUCCESS);
    for (int i = 0; i < 10; ++i) { CHECK_EQ(DWC_RnkProcess(), DWC_RNK_SUCCESS); Pump(1); }
    g_now += 31000;
    CHECK_EQ(DWC_RnkProcess(), DWC_RNK_IN_ERROR);
    long code; DWCErrorType type;
    DWC_GetLastErrorEx(&code, &type);
    CHECK_EQ(code, 80430);
    DWC_ClearError();
    CHECK_EQ(QueuePending(), 2u);                              // the result and the put
    g_now += 301000;
    for (int i = 0; i < 30 && QueuePending(); ++i) { Pump(1); g_now += 2100; }
    CHECK_EQ(QueuePending(), 0u);
    CHECK_EQ(g_fake->putCount, 1u);

    // a clamped answer is delivered to the game as a normal completion
    Fresh();
    CHECK(RankReady());
    g_fake->SetPutClamp(true, 600);
    CHECK(PutOwn(0, 700));
    DWCRnkGetParam p = NearParam(2, 0);
    CHECK_EQ(DWC_RnkGetScoreAsync(DWC_RNK_GET_MODE_NEAR, 0, DWC_RNK_REGION_US, &p), DWC_RNK_SUCCESS);
    Sequence();
    u32 count; DWCRnkData row;
    DWC_RnkResGetRowCount(&count);
    bool mine = false;
    for (u32 i = 0; i < count; ++i) { DWC_RnkResGetRow(&row, i); if (row.pid == NpDwcLoggedInProfile()) { mine = true; CHECK_EQ(row.score, 600); } }
    CHECK(mine);                                                // the row the game continues from is the server's
}

static void TestRules() {
    printf("-- modified rules\n");
    uint8_t data[128];
    FakeMakeUserData(data, "Tester", 3, 1, 2, 30, 10, 2026);
    Fresh();
    CHECK(RankReady());
    CHECK(!strcmp(RulesTag(), ""));
    g_npGameFixes = 4;   // NP_FIX_NK_BUG: the simulation differs from retail
    CHECK(!strcmp(RulesTag(), "nk"));
    CHECK(!RulesAllowRanked());
    const uint32_t puts = g_fake->putCount;
    CHECK_EQ(DWC_RnkPutScoreAsync(0, DWC_RNK_REGION_US, 700, data, 128), DWC_RNK_SUCCESS);
    CHECK(Sequence() == "E");
    CHECK_EQ(g_fake->putCount, puts);
    CHECK_EQ(QueuePending(), 0u);
    long code;
    DWCErrorType type;
    DWC_GetLastErrorEx(&code, &type);
    CHECK_EQ(code, 20102);
    DWC_ClearError();
    char msg[160];
    GetStatusMessage(msg, sizeof msg);
    CHECK(strstr(msg, "NK-bug"));
    // reading the boards stays possible
    DWCRnkGetParam p = TopParam(5, 0);
    CHECK_EQ(DWC_RnkGetScoreAsync(DWC_RNK_GET_MODE_TOPLIST, 0, DWC_RNK_REGION_US, &p), DWC_RNK_SUCCESS);
    CHECK(Sequence() == "SSSN");
    // the test switch
    setenv("MSC_ONLINE_ALLOW_MODIFIED", "1", 1);
    CHECK(RulesAllowRanked());
    CHECK(PutOwn(0, 700));
    unsetenv("MSC_ONLINE_ALLOW_MODIFIED");
    g_npGameFixes = 0;
    CHECK(RulesAllowRanked());
}

static void TestJson() {
    printf("-- json and base64\n");
    const char* text = "{\"proto\":1,\"kill\":[\"ranked\",\"turn\"],\"message\":null,\"x\":{\"a\":[1,2.5,-3e2,true,false]},\"s\":\"q\\\"x\"}";
    JValue j;
    CHECK(JsonParse(text, strlen(text), &j));
    CHECK(j.IsObj());
    CHECK_EQ((int)j.Num_("proto"), 1);
    CHECK(j.Get("kill") && j.Get("kill")->a.size() == 2 && j.Get("kill")->a[1].s == "turn");
    CHECK(j.Get("message") && j.Get("message")->type == JValue::Null);
    CHECK(j.Get("x")->Get("a")->a[2].n == -300.0);
    CHECK(j.Get("x")->Get("a")->a[3].b);
    CHECK(j.Str_("s") == "q\"x");
    {   // netclient13 (MSC_NET_TEXT_ASCII, default on): bytes outside 0x20..0x7E become '?' and the answer is KEPT (the admin message of /v1/info may hold UTF-8 or an en dash); off: as before
        const char* lf = "{\"s\":\"a\\nb\"}"; const char* u8 = "{\"s\":\"caf\\u00e9\"}";
        const char* msg = "{\"message\":\"Back at 5 \xE2\x80\x93 thanks for waiting\xE2\x80\x99s patience \\u2013 sorry\"}"; JValue v;
        CHECK(JsonParse(lf, strlen(lf), &v) && v.Str_("s") == "a?b");
        CHECK(JsonParse(u8, strlen(u8), &v) && v.Str_("s") == "caf?");
        CHECK(JsonParse(msg, strlen(msg), &v) && v.Str_("message") == "Back at 5 ? thanks for waiting?s patience ? sorry");
        NpNet::TextAsciiOverride() = 0;
        CHECK(JsonParse(lf, strlen(lf), &v) && v.Str_("s") == "a\nb"); CHECK(JsonParse(u8, strlen(u8), &v));
        NpNet::TextAsciiOverride() = -1;
    }
    CHECK(j.Str_("missing", "dflt") == "dflt");
    const char* badDocs[] = {"", "{", "{\"a\":}", "[1,]", "{\"a\":1}x", "\"\\x\"", "{'a':1}", "nul", "[1 2]", "{\"a\" 1}", "\"\\ud800\\"};
    for (size_t i = 0; i < sizeof badDocs / sizeof badDocs[0]; ++i) { JValue v; CHECK(!JsonParse(badDocs[i], strlen(badDocs[i]), &v)); }
    std::string deep(40, '[');
    deep += std::string(40, ']');
    JValue v;
    CHECK(!JsonParse(deep.data(), deep.size(), &v));            // depth limit
    CHECK(JsonQuote("a\"b\\c\n\x01") == "\"a\\\"b\\\\c\\n\\u0001\"");
    for (size_t n = 0; n < 40; ++n) {
        std::vector<uint8_t> in(n);
        for (size_t i = 0; i < n; ++i) in[i] = uint8_t(i * 37 + n);
        const std::string enc = Base64Encode(in.empty() ? nullptr : &in[0], n);
        std::vector<uint8_t> out;
        CHECK(Base64Decode(enc, &out) && out == in);
    }
    CHECK(Base64Encode((const uint8_t*)"Man", 3) == "TWFu");
    CHECK(Base64Encode((const uint8_t*)"Ma", 2) == "TWE=");
    std::vector<uint8_t> out;
    CHECK(!Base64Decode("TWE", &out));                           // padding is required
    CHECK(!Base64Decode("TW=u", &out));
    CHECK(!Base64Decode("T!Fu", &out));
    CHECK(!Base64Decode("TWE=TWFu", &out));                     // padding only at the end
}

static void TestRankInitialize() {
    printf("-- rank: initialize and shutdown\n");
    Fresh();
    // not logged in: the block has no profile
    CHECK_EQ(DWC_RnkInitialize("x", (DWCUserData*)g_block), DWC_RNK_ERROR_INIT_INVALID_USERDATA);
    CHECK_EQ(DWC_RnkPutScoreAsync(0, DWC_RNK_REGION_US, 1, 0, 0), DWC_RNK_ERROR_PUT_NOTREADY);
    CHECK_EQ(DWC_RnkProcess(), DWC_RNK_PROCESS_NOTASK);
    CHECK(Login());
    CHECK_EQ(DWC_RnkInitialize(nullptr, (DWCUserData*)g_block), DWC_RNK_ERROR_INIT_INVALID_INITDATA);
    g_block[9] ^= 1;
    CHECK_EQ(DWC_RnkInitialize("x", (DWCUserData*)g_block), DWC_RNK_ERROR_INIT_INVALID_USERDATA);   // CRC
    g_block[9] ^= 1;
    CHECK_EQ(DWC_RnkInitialize("x", nullptr), DWC_RNK_ERROR_INIT_INVALID_USERDATA);
    CHECK_EQ(DWC_RnkInitialize("x", (DWCUserData*)g_block), DWC_RNK_SUCCESS);
    CHECK_EQ(DWC_RnkInitialize("x", (DWCUserData*)g_block), DWC_RNK_ERROR_INIT_ALREADYINITIALIZED);
    CHECK_EQ(DWC_RnkProcess(), DWC_RNK_PROCESS_NOTASK);
    u32 count = 99;
    DWCRnkData row;
    CHECK_EQ(DWC_RnkResGetRowCount(&count), DWC_RNK_ERROR_NOTCOMPLETED);
    CHECK_EQ(DWC_RnkResGetRow(&row, 0), DWC_RNK_ERROR_NOTCOMPLETED);
    CHECK_EQ(DWC_RnkShutdown(), DWC_RNK_SUCCESS);
    CHECK_EQ(DWC_RnkProcess(), DWC_RNK_PROCESS_NOTASK);
    CHECK_EQ(DWC_RnkPutScoreAsync(0, DWC_RNK_REGION_US, 1, 0, 0), DWC_RNK_ERROR_PUT_NOTREADY);
    CHECK_EQ(DWC_RnkInitialize("x", (DWCUserData*)g_block), DWC_RNK_SUCCESS);
    // a profile that is not the one that logged in
    Fresh();
    CHECK(Login());
    NpDwcUserDataSetProfile(g_block, 9999);
    CHECK_EQ(DWC_RnkInitialize("x", (DWCUserData*)g_block), DWC_RNK_ERROR_INIT_INVALID_USERDATA);
}

static void TestRankPut() {
    printf("-- rank: put\n");
    Fresh();
    CHECK(RankReady());
    uint8_t data[768];
    memset(data, 0xAB, sizeof data);
    CHECK_EQ(DWC_RnkPutScoreAsync(101, DWC_RNK_REGION_US, 1, data, 16), DWC_RNK_ERROR_INVALID_PARAMETER);
    CHECK_EQ(DWC_RnkPutScoreAsync(100, DWC_RNK_REGION_US, 1, data, 769), DWC_RNK_ERROR_INVALID_PARAMETER);
    CHECK_EQ(DWC_RnkPutScoreAsync(0, DWC_RNK_REGION_US, 1, nullptr, 4), DWC_RNK_ERROR_INVALID_PARAMETER);
    CHECK_EQ(DWC_RnkPutScoreAsync(0, (DWCRnkRegion)3, 1, data, 16), DWC_RNK_ERROR_INVALID_PARAMETER);
    CHECK_EQ(DWC_RnkPutScoreAsync(0, (DWCRnkRegion)0, 1, data, 16), DWC_RNK_ERROR_INVALID_PARAMETER);
    CHECK_EQ(g_fake->putCount, 0);                          // nothing reached the service
    // accepted corner cases: category 100, 768 bytes, no data
    CHECK_EQ(DWC_RnkPutScoreAsync(100, DWC_RNK_REGION_ALL, -5, data, 768), DWC_RNK_SUCCESS);
    CHECK_EQ(g_fake->lastPut.size, 768);
    CHECK_EQ(g_fake->lastPut.score, -5);
    CHECK_EQ(g_fake->lastPut.data[767], 0xAB);
    // busy: another request is refused, the state machine shows SUCCESS until completion, once, then NOTASK
    CHECK_EQ(DWC_RnkPutScoreAsync(1, DWC_RNK_REGION_US, 1, nullptr, 0), DWC_RNK_ERROR_PUT_NOTREADY);
    DWCRnkGetParam p = NearParam(5);
    CHECK_EQ(DWC_RnkGetScoreAsync(DWC_RNK_GET_MODE_NEAR, 0, DWC_RNK_REGION_US, &p), DWC_RNK_ERROR_GET_NOTREADY);
    u32 count;
    CHECK_EQ(DWC_RnkResGetRowCount(&count), DWC_RNK_ERROR_NOTCOMPLETED);
    CHECK(Sequence() == "SSSN");
    CHECK_EQ(DWC_RnkProcess(), DWC_RNK_PROCESS_NOTASK);
    CHECK_EQ(DWC_RnkResGetRowCount(&count), DWC_RNK_ERROR_EMPTY_RESPONSE);   // a put has no rows
    DWCRnkData row;
    CHECK_EQ(DWC_RnkResGetRow(&row, 0), DWC_RNK_ERROR_EMPTY_RESPONSE);
    // from COMPLETED a new request is fine; no data at all is fine
    CHECK_EQ(DWC_RnkPutScoreAsync(0, DWC_RNK_REGION_US, 7, nullptr, 0), DWC_RNK_SUCCESS);
    CHECK(Sequence() == "SSSN");
    // the service refuses to start
    g_fake->RefuseStart(true);
    CHECK_EQ(DWC_RnkPutScoreAsync(0, DWC_RNK_REGION_US, 7, nullptr, 0), DWC_RNK_ERROR_PUT_NOMEMORY);
    g_fake->RefuseStart(false);
    CHECK_EQ(DWC_RnkProcess(), DWC_RNK_PROCESS_NOTASK);     // the refused request left the machine as it was
    CHECK_EQ(DWC_RnkPutScoreAsync(0, DWC_RNK_REGION_US, 7, nullptr, 0), DWC_RNK_SUCCESS);
    CHECK(Sequence() == "SSSN");
}

static void TestRankGetLimits() {
    printf("-- rank: get argument limits\n");
    Fresh();
    CHECK(RankReady());
    DWCRnkGetParam p;
    const DWCRnkRegion us = DWC_RNK_REGION_US;
    memset(&p, 0, sizeof p);
    // unknown modes
    p.size = 12; p.near.sort = 1; p.near.limit = 5;
    CHECK_EQ(DWC_RnkGetScoreAsync((DWCRnkGetMode)4, 0, us, &p), DWC_RNK_ERROR_INVALID_MODE);
    CHECK_EQ(DWC_RnkGetScoreAsync((DWCRnkGetMode)-1, 0, us, &p), DWC_RNK_ERROR_INVALID_MODE);
    // null param, category
    CHECK_EQ(DWC_RnkGetScoreAsync(DWC_RNK_GET_MODE_NEAR, 0, us, nullptr), DWC_RNK_ERROR_INVALID_PARAMETER);
    p = NearParam(5);
    CHECK_EQ(DWC_RnkGetScoreAsync(DWC_RNK_GET_MODE_NEAR, 101, us, &p), DWC_RNK_ERROR_INVALID_PARAMETER);
    CHECK_EQ(DWC_RnkGetScoreAsync(DWC_RNK_GET_MODE_NEAR, 0, (DWCRnkRegion)6, &p), DWC_RNK_ERROR_INVALID_PARAMETER);
    // ORDER: size 8, sort 0/1
    p = NearParam(0); p.size = 8; p.order.sort = 2;
    CHECK_EQ(DWC_RnkGetScoreAsync(DWC_RNK_GET_MODE_ORDER, 0, us, &p), DWC_RNK_ERROR_INVALID_PARAMETER);
    p.order.sort = 1; p.size = 12;
    CHECK_EQ(DWC_RnkGetScoreAsync(DWC_RNK_GET_MODE_ORDER, 0, us, &p), DWC_RNK_ERROR_INVALID_PARAMETER);
    // TOPLIST: size 12, sort 0/1, limit 1..10
    p = TopParam(0); CHECK_EQ(DWC_RnkGetScoreAsync(DWC_RNK_GET_MODE_TOPLIST, 0, us, &p), DWC_RNK_ERROR_INVALID_PARAMETER);
    p = TopParam(11); CHECK_EQ(DWC_RnkGetScoreAsync(DWC_RNK_GET_MODE_TOPLIST, 0, us, &p), DWC_RNK_ERROR_INVALID_PARAMETER);
    p = TopParam(5); p.toplist.sort = 2; CHECK_EQ(DWC_RnkGetScoreAsync(DWC_RNK_GET_MODE_TOPLIST, 0, us, &p), DWC_RNK_ERROR_INVALID_PARAMETER);
    p = TopParam(5); p.size = 8; CHECK_EQ(DWC_RnkGetScoreAsync(DWC_RNK_GET_MODE_TOPLIST, 0, us, &p), DWC_RNK_ERROR_INVALID_PARAMETER);
    // NEAR: limit 2..10
    p = NearParam(1); CHECK_EQ(DWC_RnkGetScoreAsync(DWC_RNK_GET_MODE_NEAR, 0, us, &p), DWC_RNK_ERROR_INVALID_PARAMETER);
    p = NearParam(0); CHECK_EQ(DWC_RnkGetScoreAsync(DWC_RNK_GET_MODE_NEAR, 0, us, &p), DWC_RNK_ERROR_INVALID_PARAMETER);
    p = NearParam(11); CHECK_EQ(DWC_RnkGetScoreAsync(DWC_RNK_GET_MODE_NEAR, 0, us, &p), DWC_RNK_ERROR_INVALID_PARAMETER);
    // FRIENDS: size 268, limit 2..65
    memset(&p, 0, sizeof p); p.size = sizeof(p.friends); p.friends.sort = 1; p.friends.limit = 66;
    CHECK_EQ(DWC_RnkGetScoreAsync(DWC_RNK_GET_MODE_FRIENDS, 0, us, &p), DWC_RNK_ERROR_INVALID_PARAMETER);
    p.friends.limit = 1; CHECK_EQ(DWC_RnkGetScoreAsync(DWC_RNK_GET_MODE_FRIENDS, 0, us, &p), DWC_RNK_ERROR_INVALID_PARAMETER);
    p.friends.limit = 65; p.size = 12; CHECK_EQ(DWC_RnkGetScoreAsync(DWC_RNK_GET_MODE_FRIENDS, 0, us, &p), DWC_RNK_ERROR_INVALID_PARAMETER);
    CHECK_EQ(sizeof(p.friends), 268u);
    CHECK_EQ(g_fake->getCount, 0);                          // none of them reached the service
    // the boundaries that are accepted
    p = TopParam(10); CHECK_EQ(DWC_RnkGetScoreAsync(DWC_RNK_GET_MODE_TOPLIST, 100, us, &p), DWC_RNK_SUCCESS); Sequence();
    p = TopParam(1); CHECK_EQ(DWC_RnkGetScoreAsync(DWC_RNK_GET_MODE_TOPLIST, 0, us, &p), DWC_RNK_SUCCESS); Sequence();
    p = NearParam(2); p.near.sort = 0; CHECK_EQ(DWC_RnkGetScoreAsync(DWC_RNK_GET_MODE_NEAR, 0, us, &p), DWC_RNK_SUCCESS); Sequence();
    p = NearParam(10); CHECK_EQ(DWC_RnkGetScoreAsync(DWC_RNK_GET_MODE_NEAR, 0, us, &p), DWC_RNK_SUCCESS); Sequence();
    memset(&p, 0, sizeof p); p.size = sizeof(p.friends); p.friends.sort = 1; p.friends.limit = 65;
    CHECK_EQ(DWC_RnkGetScoreAsync(DWC_RNK_GET_MODE_FRIENDS, 0, us, &p), DWC_RNK_SUCCESS); Sequence();
    p.friends.limit = 2;
    CHECK_EQ(DWC_RnkGetScoreAsync(DWC_RNK_GET_MODE_FRIENDS, 0, us, &p), DWC_RNK_SUCCESS); Sequence();
    memset(&p, 0, sizeof p); p.size = sizeof(p.order); p.order.sort = 0; p.order.since = 5;
    CHECK_EQ(DWC_RnkGetScoreAsync(DWC_RNK_GET_MODE_ORDER, 0, us, &p), DWC_RNK_SUCCESS);
    CHECK(Sequence() == "SSSN");
    u32 count;
    DWCRnkData row;
    CHECK_EQ(DWC_RnkResGetRowCount(&count), DWC_RNK_ERROR_INVALID_MODE);     // ORDER has no rows to read
    CHECK_EQ(DWC_RnkResGetRow(&row, 0), DWC_RNK_ERROR_INVALID_MODE);
}

static void TestRankRows() {
    printf("-- rank: rows\n");
    Fresh();
    CHECK(RankReady());
    const DWCRnkRegion us = DWC_RNK_REGION_US;
    DWCRnkGetParam p = TopParam(10, 0);
    u32 count = 0;
    DWCRnkData row;
    // top list: ten rows, best first, orders 1.., 0x80 bytes of user data with the digest
    CHECK_EQ(DWC_RnkGetScoreAsync(DWC_RNK_GET_MODE_TOPLIST, 0, us, &p), DWC_RNK_SUCCESS);
    CHECK(Sequence() == "SSSN");
    CHECK_EQ(DWC_RnkResGetRowCount(&count), DWC_RNK_SUCCESS);
    CHECK_EQ(count, 10);
    s32 last = 0x7FFFFFFF;
    for (u32 i = 0; i < count; ++i) {
        CHECK_EQ(DWC_RnkResGetRow(&row, i), DWC_RNK_SUCCESS);
        CHECK_EQ(row.order, i + 1);
        CHECK(row.pid > 0);
        CHECK(row.score <= last);
        last = row.score;
        CHECK_EQ(row.size, 0x80);
        CHECK(row.userdata != nullptr);
        const uint8_t* u = (const uint8_t*)row.userdata;
        CHECK(u[0xE] >= 'A' && u[0xE] <= 'Z');        // a name in the record
        uint8_t again[128];
        char name[11] = {0};
        for (int k = 0; k < 10 && u[0xE + 2 * k]; ++k) name[k] = char(u[0xE + 2 * k]);
        int wins, losses;
        memcpy(&wins, u + 4, 4); memcpy(&losses, u + 8, 4);
        FakeMakeUserData(again, name, wins, losses, u[0xC], u[0], u[1], u[2] | u[3] << 8);
        CHECK(!memcmp(again, u, 128));
    }
    CHECK_EQ(DWC_RnkResGetRow(&row, 10), DWC_RNK_ERROR_INVALID_PARAMETER);
    CHECK_EQ(DWC_RnkResGetRow(nullptr, 0), DWC_RNK_ERROR_INVALID_PARAMETER);
    CHECK_EQ(DWC_RnkResGetRowCount(nullptr), DWC_RNK_ERROR_INVALID_PARAMETER);
    // limit is honoured
    p = TopParam(3, 0);
    CHECK_EQ(DWC_RnkGetScoreAsync(DWC_RNK_GET_MODE_TOPLIST, 0, us, &p), DWC_RNK_SUCCESS);
    Sequence();
    CHECK_EQ(DWC_RnkResGetRowCount(&count), DWC_RNK_SUCCESS);
    CHECK_EQ(count, 3);
    // the request that went out
    CHECK_EQ(g_fake->lastGet.mode, 1); CHECK_EQ(g_fake->lastGet.limit, 3); CHECK_EQ(g_fake->lastGet.region, 2u); CHECK_EQ(g_fake->lastGet.sort, 1);
    // a new request clears the old rows
    CHECK_EQ(DWC_RnkGetScoreAsync(DWC_RNK_GET_MODE_TOPLIST, 0, us, &p), DWC_RNK_SUCCESS);
    CHECK_EQ(DWC_RnkResGetRowCount(&count), DWC_RNK_ERROR_NOTCOMPLETED);
    Sequence();

    // since window: the seeded players updated long ago are left out
    p = TopParam(10, 30);
    CHECK_EQ(DWC_RnkGetScoreAsync(DWC_RNK_GET_MODE_TOPLIST, 0, us, &p), DWC_RNK_SUCCESS);
    Sequence();
    CHECK_EQ(DWC_RnkResGetRowCount(&count), DWC_RNK_SUCCESS);
    CHECK(count > 0 && count < 10);
    // empty board: zero rows, still success
    p = TopParam(10, 0);
    CHECK_EQ(DWC_RnkGetScoreAsync(DWC_RNK_GET_MODE_TOPLIST, 50, us, &p), DWC_RNK_SUCCESS);
    Sequence();
    CHECK_EQ(DWC_RnkResGetRowCount(&count), DWC_RNK_SUCCESS);
    CHECK_EQ(count, 0);
}

static void TestRankNear() {
    printf("-- rank: near\n");
    Fresh();
    CHECK(RankReady());
    const DWCRnkRegion us = DWC_RNK_REGION_US;
    const int32_t me = NpDwcLoggedInProfile();
    u32 count;
    DWCRnkData row;
    // no score yet (the normal state of a new player): the game needs a row with order >= 1 -> the placeholder
    DWCRnkGetParam p = NearParam(5, 0);
    CHECK_EQ(DWC_RnkGetScoreAsync(DWC_RNK_GET_MODE_NEAR, 0, us, &p), DWC_RNK_SUCCESS);
    CHECK(Sequence() == "SSSN");
    CHECK_EQ(DWC_RnkResGetRowCount(&count), DWC_RNK_SUCCESS);
    CHECK_EQ(count, 1);
    CHECK_EQ(DWC_RnkResGetRow(&row, 0), DWC_RNK_SUCCESS);
    CHECK_EQ(row.pid, me);
    CHECK(row.order >= 1);
    CHECK_EQ(row.order, 13);                                // 12 players on the board: next free place
    CHECK_EQ(row.size, 0);                                   // no record: the game shows an empty row

    // after a put the real row comes back, with the data that was put
    CHECK(PutOwn(0, 700));
    CHECK_EQ(DWC_RnkGetScoreAsync(DWC_RNK_GET_MODE_NEAR, 0, us, &p), DWC_RNK_SUCCESS);
    Sequence();
    CHECK_EQ(DWC_RnkResGetRowCount(&count), DWC_RNK_SUCCESS);
    CHECK_EQ(count, 5);
    bool found = false;
    int ranked = 0;
    for (u32 i = 0; i < count; ++i) {
        DWC_RnkResGetRow(&row, i);
        if (row.order >= 1) ++ranked;
        if (row.pid == me) {
            found = true;
            CHECK_EQ(row.score, 700);
            CHECK_EQ(row.size, 0x80);
            CHECK_EQ(((const uint8_t*)row.userdata)[0xE], 'T');
            CHECK_EQ(row.order, 6);                         // 980 910 860 790 720 | 700
        }
    }
    CHECK(found);
    CHECK_EQ(ranked, 5);
    // the limit counts
    p = NearParam(2, 0);
    CHECK_EQ(DWC_RnkGetScoreAsync(DWC_RNK_GET_MODE_NEAR, 0, us, &p), DWC_RNK_SUCCESS);
    Sequence();
    DWC_RnkResGetRowCount(&count);
    CHECK_EQ(count, 2);
}

static void TamperAllOrdersZero(GetReply* reply, void*) { for (size_t i = 0; i < reply->rows.size(); ++i) reply->rows[i].order = 0; }
static void TamperNoRows(GetReply* reply, void*) { reply->rows.clear(); reply->total = 0; }
static void TamperJunk(GetReply* reply, void*) {
    std::vector<Row> rows = reply->rows;
    Row bad;
    bad = rows.empty() ? Row() : rows[0];
    Row r1 = bad; r1.pid = 0;                        reply->rows.push_back(r1);   // no profile id
    Row r2 = bad; r2.pid = 4001; r2.size = 0x40;      reply->rows.push_back(r2);   // wrong record size
    Row r3 = bad; r3.pid = 4002; r3.size = 5000;      reply->rows.push_back(r3);   // longer than anything allowed
    Row r4 = bad; r4.pid = 4003; r4.size = 0;         reply->rows.push_back(r4);   // no record
    if (!rows.empty()) reply->rows.push_back(rows[0]);                            // duplicate profile id
    for (int i = 0; i < 20; ++i) { Row x = bad; x.pid = 5000 + i; x.order = 50 + i; x.size = 0x80; reply->rows.push_back(x); }   // way beyond the limit
}

static void TestRankRepair() {
    printf("-- rank: damaged replies\n");
    Fresh();
    CHECK(RankReady());
    CHECK(PutOwn(0, 700));
    const DWCRnkRegion us = DWC_RNK_REGION_US;
    u32 count;
    DWCRnkData row;
    DWCRnkGetParam p = NearParam(5, 0);
    // orders missing: numbered by score; some row has order >= 1
    g_fake->SetTamper(TamperAllOrdersZero, nullptr);
    CHECK_EQ(DWC_RnkGetScoreAsync(DWC_RNK_GET_MODE_NEAR, 0, us, &p), DWC_RNK_SUCCESS);
    Sequence();
    DWC_RnkResGetRowCount(&count);
    CHECK_EQ(count, 5);
    int ranked = 0;
    u32 minOrder = 99;
    for (u32 i = 0; i < count; ++i) { DWC_RnkResGetRow(&row, i); if (row.order >= 1) ++ranked; if (row.order < minOrder) minOrder = row.order; }
    CHECK_EQ(ranked, 5);
    CHECK_EQ(minOrder, 1);
    // no rows at all
    g_fake->SetTamper(TamperNoRows, nullptr);
    CHECK_EQ(DWC_RnkGetScoreAsync(DWC_RNK_GET_MODE_NEAR, 0, us, &p), DWC_RNK_SUCCESS);
    Sequence();
    DWC_RnkResGetRowCount(&count);
    CHECK_EQ(count, 1);
    DWC_RnkResGetRow(&row, 0);
    CHECK(row.order >= 1);
    CHECK_EQ(row.pid, NpDwcLoggedInProfile());
    // top list with no rows stays empty: the game does not index it
    CHECK_EQ(DWC_RnkGetScoreAsync(DWC_RNK_GET_MODE_TOPLIST, 0, us, &(p = TopParam(5, 0))), DWC_RNK_SUCCESS);
    Sequence();
    DWC_RnkResGetRowCount(&count);
    CHECK_EQ(count, 0);
    // junk rows
    g_fake->SetTamper(TamperJunk, nullptr);
    p = TopParam(5, 0);
    CHECK_EQ(DWC_RnkGetScoreAsync(DWC_RNK_GET_MODE_TOPLIST, 0, us, &p), DWC_RNK_SUCCESS);
    Sequence();
    DWC_RnkResGetRowCount(&count);
    CHECK_EQ(count, 5);
    std::vector<int> pids;
    for (u32 i = 0; i < count; ++i) {
        DWC_RnkResGetRow(&row, i);
        CHECK(row.pid > 0); CHECK_EQ(row.size, 0x80);
        for (size_t k = 0; k < pids.size(); ++k) CHECK(pids[k] != row.pid);
        pids.push_back(row.pid);
    }
    p = NearParam(4, 0);
    CHECK_EQ(DWC_RnkGetScoreAsync(DWC_RNK_GET_MODE_NEAR, 0, us, &p), DWC_RNK_SUCCESS);
    Sequence();
    DWC_RnkResGetRowCount(&count);
    CHECK(count >= 1 && count <= 4);
    g_fake->SetTamper(nullptr, nullptr);
}

static void TestRankFriends() {
    printf("-- rank: friends\n");
    Fresh();
    CHECK(RankReady());
    const int32_t me = NpDwcLoggedInProfile();
    const DWCRnkRegion us = DWC_RNK_REGION_US;
    CHECK(PutOwn(0, 800));
    DWCRnkGetParam p;
    memset(&p, 0, sizeof p);
    p.size = sizeof(p.friends); p.friends.sort = 1; p.friends.limit = 65; p.friends.since = 0;
    p.friends.friends[0] = 101; p.friends.friends[1] = 103; p.friends.friends[2] = 101;   // a duplicate
    p.friends.friends[3] = 0; p.friends.friends[4] = -4; p.friends.friends[5] = me;       // empty slot, junk, the player
    p.friends.friends[63] = 105;
    CHECK_EQ(DWC_RnkGetScoreAsync(DWC_RNK_GET_MODE_FRIENDS, 0, us, &p), DWC_RNK_SUCCESS);
    Sequence();
    CHECK_EQ(g_fake->lastGet.friendCount, 3);                 // 101 103 105: no zeros, no duplicates, not the player
    u32 count;
    DWC_RnkResGetRowCount(&count);
    CHECK_EQ(count, 4);                                        // the three friends and the player
    DWCRnkData row;
    bool seenMe = false;
    for (u32 i = 0; i < count; ++i) { DWC_RnkResGetRow(&row, i); if (row.pid == me) seenMe = true; }
    CHECK(seenMe);
}

static void TestRankErrors() {
    printf("-- rank: failures\n");
    long code;
    DWCErrorType type;
    Fresh();
    CHECK(RankReady());
    const DWCRnkRegion us = DWC_RNK_REGION_US;
    DWCRnkGetParam p = TopParam(5, 0);
    // the service answers with an error: the failing call returns IN_ERROR, and so does everything after it
    g_fake->FailNext(Fail::Server, 503);
    CHECK_EQ(DWC_RnkGetScoreAsync(DWC_RNK_GET_MODE_TOPLIST, 0, us, &p), DWC_RNK_SUCCESS);
    CHECK(Sequence() == "SSE");
    CHECK_EQ(DWC_RnkProcess(), DWC_RNK_IN_ERROR);
    u32 count;
    DWCRnkData row;
    CHECK_EQ(DWC_RnkResGetRowCount(&count), DWC_RNK_IN_ERROR);
    CHECK_EQ(DWC_RnkResGetRow(&row, 0), DWC_RNK_IN_ERROR);
    CHECK_EQ(DWC_RnkGetScoreAsync(DWC_RNK_GET_MODE_TOPLIST, 0, us, &p), DWC_RNK_IN_ERROR);
    CHECK_EQ(DWC_RnkPutScoreAsync(0, us, 1, nullptr, 0), DWC_RNK_IN_ERROR);
    CHECK_EQ(DWC_RnkInitialize("x", (DWCUserData*)g_block), DWC_RNK_IN_ERROR);
    CHECK(DWC_GetLastErrorEx(&code, &type) != 0);
    CHECK_EQ(code, 20101);
    // clearing the error (the game does after showing its message) re-arms the client
    DWC_ClearError();
    CHECK_EQ(DWC_RnkProcess(), DWC_RNK_PROCESS_NOTASK);
    CHECK_EQ(DWC_RnkGetScoreAsync(DWC_RNK_GET_MODE_TOPLIST, 0, us, &p), DWC_RNK_SUCCESS);
    CHECK(Sequence() == "SSSN");
    // put failure
    g_fake->FailNext(Fail::Transport);
    CHECK_EQ(DWC_RnkPutScoreAsync(0, us, 1, nullptr, 0), DWC_RNK_SUCCESS);
    CHECK(Sequence() == "SSE");
    DWC_GetLastErrorEx(&code, &type);
    CHECK_EQ(code, 20100);
    // shutdown also leaves the error state
    CHECK_EQ(DWC_RnkShutdown(), DWC_RNK_SUCCESS);
    DWC_ClearError();
    CHECK_EQ(DWC_RnkInitialize("x", (DWCUserData*)g_block), DWC_RNK_SUCCESS);

    // timeout: the service never answers
    g_fake->Stall(true);
    CHECK_EQ(DWC_RnkGetScoreAsync(DWC_RNK_GET_MODE_TOPLIST, 0, us, &p), DWC_RNK_SUCCESS);
    CHECK(Sequence(10) == "SSSSSSSSSS");
    g_now += 14000;
    CHECK_EQ(DWC_RnkProcess(), DWC_RNK_SUCCESS);
    g_now += 1500;
    CHECK_EQ(DWC_RnkProcess(), DWC_RNK_IN_ERROR);
    DWC_GetLastErrorEx(&code, &type);
    CHECK_EQ(code, 80430);
    CHECK_EQ(DWC_RnkProcess(), DWC_RNK_IN_ERROR);
    DWC_ClearError();
    g_fake->Stall(false);
    CHECK_EQ(DWC_RnkGetScoreAsync(DWC_RNK_GET_MODE_TOPLIST, 0, us, &p), DWC_RNK_SUCCESS);
    CHECK(Sequence() == "SSSN");

    // auth lost on the service side (session gone): an error, not a hang
    g_fake->FailNext(Fail::Auth, 401);
    CHECK_EQ(DWC_RnkGetScoreAsync(DWC_RNK_GET_MODE_NEAR, 0, us, &(p = NearParam(5))), DWC_RNK_SUCCESS);
    CHECK(Sequence() == "SSE");
    DWC_GetLastErrorEx(&code, &type);
    CHECK_EQ(code, 20103);
    DWC_ClearError();
    // shutdown with a request in flight leaves nothing pending
    CHECK_EQ(DWC_RnkGetScoreAsync(DWC_RNK_GET_MODE_NEAR, 0, us, &p), DWC_RNK_SUCCESS);
    CHECK_EQ(DWC_RnkShutdown(), DWC_RNK_SUCCESS);
    CHECK_EQ(DWC_RnkProcess(), DWC_RNK_PROCESS_NOTASK);
}

static bool FileThere(const char* name, const char* suffix) {
    char path[400];
    snprintf(path, sizeof path, "%s/%s%s", g_dataDir, name, suffix);
    return access(path, F_OK) == 0;
}
static void PutRaw(const char* name, const char* suffix, const char* text) {
    char path[400];
    snprintf(path, sizeof path, "%s/%s%s", g_dataDir, name, suffix);
    if (FILE* f = fopen(path, "wb")) { fputs(text, f); fclose(f); }
}

// Forgetting the account leaves no copy of the token, the name or the retry queue: not the files, not their .tmp / .bak, not the set-aside .bad copies.

// ---- friends (np_dwc_friend.cpp, np_online_friends.h) ------------------------------------------------------------------------------
static int g_buddyCalls = 0, g_buddyIndex = -1, g_statusCalls = 0, g_statusIndex = -1, g_updateCalls = 0, g_updateError = -1;
static unsigned g_statusCode = 99;
static std::string g_statusText;
static void BuddyCb(int index, void*) { ++g_buddyCalls; g_buddyIndex = index; }
static void StatusCb(int index, u8 status, const char* text, void*) { ++g_statusCalls; g_statusIndex = index; g_statusCode = status; g_statusText = text; }
static void UpdateCb(int error, BOOL, void*) { ++g_updateCalls; g_updateError = error; }
static void DelCb(int, int, void*) {}
static void Tick(int n) { for (int i = 0; i < n * 2; ++i) { g_now += 1200; DWC_ProcessFriendsMatch(); } }
static void SelfKeyToken(DWCFriendData* d, int pid) { DWC_CreateFriendKeyToken(d, DWC_Acc_CreateFriendKey(pid, 0x52345145)); }

static void TestFriends() {
    // keys: the 12 digits of the original service
    {
        const u64 key = DWC_Acc_CreateFriendKey(1234567, 0x52345145);
        CHECK_EQ(key & 0xffffffffu, 1234567);
        CHECK(DWC_Acc_CheckFriendKey(key, 0x52345145));
        CHECK(!DWC_Acc_CheckFriendKey(key + (u64(1) << 32), 0x52345145));       // a wrong check value
        CHECK(!DWC_Acc_CheckFriendKey(key, 0x52345150));                         // another game
        CHECK(!DWC_Acc_CheckFriendKey(DWC_Acc_CreateFriendKey(-5, 0x52345145), 0x52345145));   // negative profile ids are invalid
        char text[16];
        DWC_Acc_FriendKeyToNumericKey(key, text);
        CHECK_EQ(strlen(text), 12);
        CHECK_EQ(DWC_Acc_NumericKeyToFriendKey(text), key);
        CHECK_EQ(DWC_Acc_FriendKeyToGsProfileId(key, 0x52345145), 1234567);
        DWCFriendData t, u;
        DWC_CreateFriendKeyToken(&t, key);
        CHECK(DWC_IsValidFriendData(&t));
        CHECK_EQ(DWC_GetFriendDataType(&t), DWC_FRIENDDATA_FRIEND_KEY);
        CHECK_EQ(DWC_GetFriendKey(&t), key);
        CHECK(!DWC_IsBuddyFriendData(&t));
        DWC_CreateFriendKeyToken(&u, key);
        CHECK(DWC_IsEqualFriendData(&t, &u));
        DWC_SetGsProfileId(&u, 77);
        CHECK(!DWC_IsEqualFriendData(&t, &u));
        CHECK_EQ(DWC_GetFriendDataType(&u), DWC_FRIENDDATA_GS_PROFILE_ID);
        DWCFriendData zero;
        memset(&zero, 0, sizeof zero);
        CHECK(!DWC_IsValidFriendData(&zero));
    }
    // DWC's base64 (own alphabet, '*' padding)
    {
        const char data[] = {1, 2, 3, 4, 5, 6, 7, 8};
        char enc[32], dec[16];
        const int n = DWC_Base64Encode(data, 8, enc, sizeof enc);
        CHECK_EQ(n, 12);
        CHECK_EQ(enc[11], 0x2a);
        CHECK_EQ(DWC_Base64Decode(enc, n, nullptr, 0), 8);
        CHECK_EQ(DWC_Base64Decode(enc, n, dec, sizeof dec), 8);
        CHECK(memcmp(dec, data, 8) == 0);
        CHECK_EQ(DWC_Base64Decode("abc", 3, dec, sizeof dec), -1);
    }
    // the roster, status and operations against the fake service
    Fresh();
    CHECK(Login("Tester"));
    const int me = NpDwcLoggedInProfile();
    DWCUserData* ud = (DWCUserData*)g_block;
    CHECK_EQ(DWC_CreateFriendKey(ud), DWC_Acc_CreateFriendKey(me, 0x52345145));
    char own[16];
    CHECK(NpFriends::OwnKeyText(own, sizeof own));
    CHECK_EQ(strlen(own), 12);
    const int32_t a = g_fake->FakeAddPerson("Anna1"), b = g_fake->FakeAddPerson("Boris");
    static DWCFriendData list[64];
    memset(list, 0, sizeof list);
    SelfKeyToken(&list[0], a);               // typed in by key
    DWC_SetGsProfileId(&list[1], b);         // already a profile id (an older sync)
    DWC_InitFriendsMatch(0, ud, 0x2AAF, "mschargedwii", "key", 0, 0, list, 64);
    g_buddyCalls = g_statusCalls = g_updateCalls = 0;
    CHECK(DWC_SetBuddyFriendCallback(BuddyCb, 0));
    CHECK(DWC_SetFriendStatusCallback(StatusCb, 0));
    CHECK_EQ(DWC_GetNumFriend(list, 64), 2);
    CHECK(DWC_UpdateServersAsync(0, UpdateCb, 0, StatusCb, 0, DelCb, 0));
    CHECK(!DWC_CanChangeFriendList());
    Tick(30);
    CHECK_EQ(g_updateCalls, 1);
    CHECK_EQ(g_updateError, 0);
    CHECK(DWC_CanChangeFriendList());
    CHECK_EQ(DWC_GetFriendDataType(&list[0]), DWC_FRIENDDATA_GS_PROFILE_ID);   // the key became the profile id
    CHECK_EQ(list[0].gs_profile_id.id, a);
    CHECK(!DWC_IsBuddyFriendData(&list[0]));                                   // they have not added us
    CHECK_EQ(g_buddyCalls, 0);
    CHECK_EQ(g_fake->friendOpCount, 2);                                        // both entries were sent as adds
    char text[256];
    CHECK_EQ(DWC_GetFriendStatus(&list[0], text), DWC_STATUS_OFFLINE);
    // Anna adds us: the entry becomes a buddy and the status callback gets her state
    g_fake->FakeSetStatus(a, "QUJD");
    g_fake->FakePersonAdds(a);
    Tick(10);
    CHECK_EQ(g_buddyCalls, 1);
    CHECK_EQ(g_buddyIndex, 0);
    CHECK(DWC_IsBuddyFriendData(&list[0]));
    CHECK_EQ(g_statusIndex, 0);
    CHECK_EQ(g_statusCode, DWC_STATUS_ONLINE);
    CHECK(g_statusText == "QUJD");
    CHECK_EQ(DWC_GetFriendStatus(&list[0], text), DWC_STATUS_ONLINE);
    CHECK(std::string(text) == "QUJD");
    const int calls = g_statusCalls;
    Tick(6);
    CHECK_EQ(g_statusCalls, calls);                                            // nothing changed: no callback
    g_fake->FakeSetPresence(a, Presence::Queue);
    Tick(10);
    CHECK_EQ(g_statusCode, DWC_STATUS_MATCH_ANYBODY);
    g_fake->FakeSetPresence(a, Presence::Room);
    Tick(10);
    CHECK_EQ(g_statusCode, DWC_STATUS_MATCH_FRIEND);
    g_fake->FakeSetPresence(a, Presence::Match);
    Tick(10);
    CHECK_EQ(g_statusCode, DWC_STATUS_PLAYING);
    // the list is kept fresh with ONE held poll at a time, not a loop of plain ones
    const uint32_t before = g_fake->friendsCount;
    Tick(20);
    CHECK(g_fake->friendsCount - before <= 20);
    CHECK_EQ(g_fake->lastFriendsWaitSec, 45);
    // our own status string goes to the service (DWC's base64 of the payload), only when set
    const char payload[] = {'S', 'C', '2', 1, 0, 0, 0, 0};
    const uint32_t ops = g_fake->friendOpCount;
    CHECK(DWC_SetOwnStatusData(payload, sizeof payload));
    Tick(4);
    CHECK_EQ(g_fake->friendOpCount, ops + 1);
    CHECK(g_fake->friendOps.back().op == FriendOp::SetStatus);
    char back[32];
    const std::string sent = g_fake->friendOps.back().status;
    CHECK_EQ(DWC_Base64Decode(sent.c_str(), sent.size(), back, sizeof back), 8);
    CHECK(memcmp(back, payload, 8) == 0);
    // the menus' calls: add by code, requests, block, invite, played with
    const int32_t c = g_fake->FakeAddPerson("Cara");
    char code[16];
    snprintf(code, sizeof code, "fake%04d", c % 10000);   // lower case, no dash: accepted
    CHECK(NpFriends::Add(code));
    Tick(6);
    CHECK(NpFriends::LastOp().done && NpFriends::LastOp().ok);
    CHECK(NpFriends::LastOp().state == FriendState::Outgoing);
    CHECK_EQ(NpFriends::LastOp().pid, c);
    CHECK(!NpFriends::Add("12"));                                // bad text: not queued, the reason is in LastOp
    CHECK(std::string(NpFriends::LastOp().code) == "bad_code");
    CHECK(NpFriends::AddPid(me));                                // yourself
    Tick(6);
    CHECK(!NpFriends::LastOp().ok);
    CHECK(std::string(NpFriends::LastOp().code) == "player_not_found" || std::string(NpFriends::LastOp().code) == "bad_friend");
    g_fake->FakePersonAdds(c);
    Tick(8);
    const FriendRow* rc = NpFriends::Find(c);
    CHECK(rc && rc->state == FriendState::Friend);
    CHECK(NpFriends::Invite(c, "ABC234"));
    Tick(4);
    CHECK(NpFriends::LastOp().ok);
    g_fake->FakePersonInvites(b, "ZZZ234");                      // not a friend of ours yet: ignored
    g_fake->FakePersonAdds(b);
    Tick(8);
    g_fake->FakePersonInvites(b, "ZZZ234");
    Tick(8);
    int32_t from = 0;
    char room[8];
    CHECK(NpFriends::HasInvite(&from, room, sizeof room));
    CHECK_EQ(from, b);
    CHECK(std::string(room) == "ZZZ234");
    CHECK(NpFriends::ClearInvite(b));
    Tick(6);
    CHECK(!NpFriends::HasInvite(&from, room, sizeof room));
    g_fake->FakePlayedWith(c, false);
    g_fake->FakePlayedWith(b, true);
    NpFriends::RefreshPlayedWith();
    Tick(4);
    CHECK(NpFriends::PlayedWithReady());
    CHECK_EQ(NpFriends::PlayedWith().size(), 2);
    CHECK_EQ(NpFriends::PlayedWith()[0].pid, b);
    CHECK(NpFriends::PlayedWith()[0].wasRanked && NpFriends::PlayedWith()[0].isFriend);
    CHECK(NpFriends::Block(c));
    Tick(6);
    CHECK(!NpFriends::Find(c));
    CHECK_EQ(NpFriends::Snapshot().blocked.size(), 1);
    CHECK(NpFriends::Unblock(c));
    Tick(6);
    CHECK_EQ(NpFriends::Snapshot().blocked.size(), 0);
    // deleting a friend in the game's roster tells the service and clears the entry
    const uint32_t opsBefore = g_fake->friendOpCount;
    DWC_DeleteBuddyFriendData(&list[0]);
    CHECK(!DWC_IsValidFriendData(&list[0]));
    Tick(6);
    CHECK(g_fake->friendOpCount > opsBefore);
    CHECK(g_fake->friendOps.back().op == FriendOp::Remove);
    CHECK(!NpFriends::Find(a));
    // a match being made pauses the friends traffic
    NpFriends::SetPaused(true);
    const uint32_t polls = g_fake->friendsCount;
    Tick(30);
    CHECK(g_fake->friendsCount - polls <= 1);
    NpFriends::SetPaused(false);
    NpDwcFriendsShutdown();
}

static void TestForgetAccount() {
    printf("-- forget the account\n");
    Fresh();
    NpOnlineCredentials in;
    memset(&in, 0, sizeof in);
    in.profileId = 4242;
    snprintf(in.token, sizeof in.token, "abcdefghijklmnopqrstuvwx");
    CHECK(NpOnlineSaveCredentials(in));
    CHECK(StoreName("Forgetful"));
    ResultReport rep;
    snprintf(rep.matchId, sizeof rep.matchId, "aB3dE5gH7jK9");
    rep.result = 0; rep.score0 = 3; rep.score1 = 1;
    snprintf(rep.checksum, sizeof rep.checksum, "0f1e2d3c4b5a6978");
    snprintf(rep.mode, sizeof rep.mode, "ranked");
    CHECK(QueueResult(rep));
    const char* files[] = {"online.dat", "online_name.txt", "online_queue.dat"};
    const char* sfx[] = {"", ".tmp", ".bak", ".bad", ".bad2", ".bad3"};
    for (const char* f : files) {
        CHECK(FileThere(f, ""));
        for (const char* x : sfx) if (*x) PutRaw(f, x, "set-aside copy");
    }
    NpOnlineForgetAccount();
    for (const char* f : files)
        for (const char* x : sfx) { if (FileThere(f, x)) printf("left behind: %s%s\n", f, x); CHECK(!FileThere(f, x)); }
    CHECK_EQ(QueuePending(), 0u);
    // the two single forgets each take their own family
    CHECK(NpOnlineSaveCredentials(in));
    PutRaw("online.dat", ".bad", "x"); PutRaw("online.dat", ".bak", "x");
    NpOnlineForgetCredentials();
    CHECK(!FileThere("online.dat", "") && !FileThere("online.dat", ".bad") && !FileThere("online.dat", ".bak"));
    CHECK(StoreName("Forgetful"));
    PutRaw("online_name.txt", ".bad", "x");
    ForgetName();
    CHECK(!FileThere("online_name.txt", "") && !FileThere("online_name.txt", ".bad"));
}

static void TestNames() {
    printf("-- names\n");
    char out[16];
    bool changed;
    CHECK(SanitizeName("Sam", out, &changed) && !strcmp(out, "Sam") && !changed);
    CHECK(SanitizeName("  Two   Words  ", out, &changed) && !strcmp(out, "Two Words") && changed);
    CHECK(SanitizeName("ABCDEFGHIJKLMNOP", out, &changed) && !strcmp(out, "ABCDEFGHIJ") && changed);
    CHECK(SanitizeName("Caf\xC3\xA9 Bob", out, &changed) && !strcmp(out, "Caf Bob"));
    CHECK(SanitizeName("\xE6\x97\xA5\xE6\x9C\xAC" "x1", out) && !strcmp(out, "x1"));
    CHECK(SanitizeName("-ab_", out, &changed) && !strcmp(out, "ab") && changed);   // starts and ends with a letter or digit
    CHECK(SanitizeName("a.b-c_d", out, &changed) && !strcmp(out, "a.b-c_d") && !changed);
    CHECK(SanitizeName("Fo!o#", out, &changed) && !strcmp(out, "Foo"));          // the charset is A-Z a-z 0-9 space . _ -
    CHECK(!SanitizeName("x", out));                          // two letters or digits at least
    CHECK(!SanitizeName("a.", out));
    CHECK(!SanitizeName("", out)); CHECK(!out[0]);
    CHECK(!SanitizeName("   ", out));
    CHECK(!SanitizeName("!!! ???", out));                   // no letter or digit
    CHECK(!SanitizeName("\xE6\x97\xA5\xE6\x9C\xAC", out));
    CHECK(!SanitizeName(nullptr, out));
    CHECK(SanitizeName("a\tb\nc", out) && !strcmp(out, "a bc"));
    CHECK(SanitizeName("Name With Spaces", out) && !strcmp(out, "Name With"));   // cut at 10, trailing space trimmed
    CHECK(IsValidName("Player_1")); CHECK(!IsValidName("  x")); CHECK(!IsValidName("")); CHECK(!IsValidName("12345678901"));

    // storage
    Fresh();
    ForgetName();
    CHECK(!LoadStoredName(out));
    CHECK(StoreName("Sam"));
    CHECK(LoadStoredName(out) && !strcmp(out, "Sam"));
    CHECK(!StoreName(" bad name "));                         // refuses what would change
    char path[300];
    snprintf(path, sizeof path, "%s/online_name.txt", g_dataDir);
    FILE* f = fopen(path, "wb"); fputs("Hand\xC3\xA9" "dited\n", f); fclose(f);
    CHECK(!LoadStoredName(out));                             // a damaged file is ignored, not trusted
    ForgetName();

    // the request: the keyboard opens pre-filled with the sanitised suggestion; Enter stores the cleaned text
    unsetenv("MSC_ONLINE_NAME");
    g_imeAnswers.clear();
    g_imeAnswers.push_back("  Typed   Name!  ");
    g_imeOpens = 0;
    NameResetForTest();
    CHECK(NameRequest() == NameState::Pending);
    CHECK_EQ(g_imeOpens, 1);
    CHECK(g_imeInitial == "Sam Exampl");
    CHECK(NameRequest() == NameState::Pending);
    CHECK(NameRequest() == NameState::Pending);
    CHECK(NameRequest() == NameState::Ready);
    CHECK(!strcmp(Name(), "Typed Name"));
    CHECK(LoadStoredName(out) && !strcmp(out, "Typed Name"));
    CHECK(NameRequest() == NameState::Ready);          // later runs: no keyboard
    CHECK_EQ(g_imeOpens, 1);
    NameResetForTest();
    CHECK(NameRequest() == NameState::Ready);          // the stored name, no keyboard
    CHECK_EQ(g_imeOpens, 1);
    CHECK(!strcmp(Name(), "Typed Name"));

    // an unusable answer reopens the keyboard (empty), three times at most, then counts as cancelled
    ForgetName();
    g_imeAnswers.clear();
    g_imeAnswers.push_back("!!!"); g_imeAnswers.push_back("???"); g_imeAnswers.push_back("   ");
    g_imeOpens = 0;
    NameState st = NameState::Pending;
    for (int i = 0; i < 30 && st == NameState::Pending; ++i) st = NameRequest();
    CHECK(st == NameState::Cancelled);
    CHECK_EQ(g_imeOpens, 3);
    CHECK(!LoadStoredName(out));
    // cancelled: asked again at the next request, and this time accepted
    g_imeAnswers.clear();
    g_imeAnswers.push_back("Okay");
    st = NameState::Pending;
    for (int i = 0; i < 30 && (st = NameRequest()) == NameState::Pending; ++i) {}
    CHECK(st == NameState::Ready);
    CHECK(!strcmp(Name(), "Okay"));
    // plain cancel
    ForgetName();
    g_imeAnswers.clear();
    g_imeAnswers.push_back("\x01");
    st = NameState::Pending;
    for (int i = 0; i < 30 && (st = NameRequest()) == NameState::Pending; ++i) {}
    CHECK(st == NameState::Cancelled);
    CHECK(!LoadStoredName(out));

    // the environment overrides everything and stores nothing
    ForgetName();
    setenv("MSC_ONLINE_NAME", "  From Env  ", 1);
    g_imeOpens = 0;
    CHECK(NameRequest() == NameState::Ready);
    CHECK(!strcmp(Name(), "From Env"));
    CHECK_EQ(g_imeOpens, 0);
    CHECK(!LoadStoredName(out));
    unsetenv("MSC_ONLINE_NAME");
}

// (mpbeta27) "block the result upload": a ranked result the service refuses for good is dropped, the problem report says so and a report is offered; one that fails once
// and is then accepted is only noted (no offer). The fake service fails the request on purpose (FailNext).
static void TestResultUploadReport() {
    printf("-- (mpbeta27) a ranked result that cannot be delivered: the report says so and offers itself\n");
    ResultReport rep;
    snprintf(rep.matchId, sizeof rep.matchId, "aB3dE5gH7jK9");
    rep.result = 0; rep.score0 = 2; rep.score1 = 1;
    snprintf(rep.checksum, sizeof rep.checksum, "0f1e2d3c4b5a6978");
    snprintf(rep.mode, sizeof rep.mode, "ranked");
    // refused for good (a 4xx): dropped at the first answer
    Fresh();
    CHECK(RankReady());
    NpMpDiag::ClearForTest();
    NpMpDiag::TrailClearForTest();
    NpMpDiag::Note(NpMpDiag::kSessionStart, 1);
    CHECK(QueueResult(rep));
    CHECK(NpMpDiag::CurrentTrail().furthest == NpMpDiag::kStageResultUpload);
    g_fake->FailNext(Fail::Rejected, 400);
    for (int i = 0; i < 20 && QueuePending(); ++i) { g_now += 2500; Pump(1); }
    CHECK_EQ(QueuePending(), 0u);
    NpMpDiag::Trail t = NpMpDiag::CurrentTrail();
    CHECK_EQ(t.uploadOutcome, -1);
    CHECK(!strcmp(t.uploadWord, "refused"));
    CHECK(NpMpDiag::OfferWanted());
    {
        char out[4096];
        NpMpDiag::LinkFigures f = {-1, -1, -1, -1, 0, -1, -1, 0, 0, 0};
        NpMpDiag::TrailReport(t, f, out, sizeof out);
        CHECK(strstr(out, "ranked result upload = dropped after 1 attempt, refused") != nullptr);
    }
    // a time-out, then accepted on the retry: noted, no offer
    Fresh();
    CHECK(RankReady());
    NpMpDiag::ClearForTest();
    NpMpDiag::TrailClearForTest();
    NpMpDiag::Note(NpMpDiag::kSessionStart, 1);
    CHECK(QueueResult(rep));
    g_fake->FailNext(Fail::Timeout);
    g_now += 2500;
    Pump(1);
    for (int i = 0; i < 4 && NpMpDiag::CurrentTrail().uploadOutcome == -2; ++i) { g_now += 2500; Pump(1); }
    CHECK_EQ(NpMpDiag::CurrentTrail().uploadOutcome, 0);
    CHECK(!NpMpDiag::OfferWanted());
    for (int i = 0; i < 40 && QueuePending(); ++i) { g_now += 2500; Pump(1); }
    CHECK_EQ(QueuePending(), 0u);
    CHECK_EQ(NpMpDiag::CurrentTrail().uploadOutcome, 1);
    CHECK(NpMpDiag::CurrentTrail().uploadAttempts >= 2);
    CHECK(!NpMpDiag::OfferWanted());
}

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    snprintf(g_dataDir, sizeof g_dataDir, "/tmp/dwc_test_XXXXXX");
    if (!mkdtemp(g_dataDir)) { perror("mkdtemp"); return 2; }
    setenv("NP_DATA_DIR", g_dataDir, 1);
    unsetenv("MSC_ONLINE_NAME");
    TestCrcAndUserData();
    TestCredentialStore();
    TestLogin();
    TestLoginFailures();
    TestClockSuite();
    TestPublicCode();
    TestErrorNumbers();
    TestQueue();
    TestFinalHold();
    TestRules();
    TestJson();
    TestRankInitialize();
    TestRankPut();
    TestRankGetLimits();
    TestRankRows();
    TestRankNear();
    TestRankRepair();
    TestRankFriends();
    TestRankErrors();
    TestToken_NotLogged();
    TestNames();
    TestForgetAccount();
    TestFriends();
    TestResultUploadReport();
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf %s", g_dataDir);
    if (system(cmd)) {}
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
