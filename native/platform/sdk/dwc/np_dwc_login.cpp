// DWC_Init / DWC_InitFriendsMatch / DWC_LoginAsync / DWC_ProcessFriendsMatch / DWC_GetIngamesnCheckResult / DWC_GetDateTime
// on top of NpOnline::Api. See np/docs (ranked-online notes) and np_dwc.h.
//
// Login: DWC_LoginAsync starts the request and returns TRUE (or FALSE with the DWC error set); the callback is delivered once, on the
// game thread, from DWC_ProcessFriendsMatch (the game's own pump). The first login has no token: the service creates the account and
// returns the profile id and the token, which are stored (np_online_store.cpp). On success the profile id is written into the save
// slot's user-data block at 0x1C, the authentic login id gets data type 1, the dirty flag is set and the CRC at 0x3C is recomputed
// (so the game saves the slot, ValidateSaveSlot). A token the service no longer knows is dropped and the login repeated once without it.
//
// Clock: the login reply and a refresh every 30 minutes (or after a failure, 5 minutes later) feed a cached UTC time that advances with
// the monotonic clock; DWC_GetDateTime answers FALSE until the first sync, which the game already handles (it falls back to 2000-01-01).
#include <dwc/dwc_account.h>
#include <dwc/dwc_error.h>
#include <dwc/dwc_main_fwd.h>
#include <dwc/dwc_nastime.h>
#include <dwc/dwc_init_fwd.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/sdk/dwc/np_dwc.h"

// (mpbeta27) the session trail of a problem report (np_mpdiag.h; weak: the small tests link without it). Stage 2 = login.
extern "C" void NpMpDiagReach(int stage) __attribute__((weak));
extern "C" void NpMpDiagError(int stage, int code, const char* word) __attribute__((weak));
#include "platform/sdk/dwc/np_online_name.h"
#include "platform/sdk/dwc/np_online_queue.h"

#define ONLINE_LOG(...) fprintf(stderr, "[online] " __VA_ARGS__)

namespace {

using namespace NpOnline;

enum class LoginState { Idle, Requesting };

struct Login {
    bool inited;
    bool friendsReady;
    DWCUserData* userdata;
    LoginState state;
    uint32_t request;
    uint64_t deadline;
    bool haveToken, retried;
    LoginRequest sent;
    DWCLoginCallback callback;
    void* param;
    int32_t profile;            // the profile of the last successful login in this run
    DWCIngamesnCheckResult nameCheck;
};

pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
Login g_login;

struct Clock {
    bool synced;
    int64_t unixMs;
    uint64_t monoMs;
    uint64_t nextRefreshMs;
    uint32_t request;
    uint64_t deadline;
} g_clock;

const int kMaxPumps = 8;
void (*g_pumps[kMaxPumps])(void);
int g_pumpCount = 0;

uint32_t LoginTimeoutMs() {
    static const uint32_t ms = [] {
        const char* e = getenv("MSC_ONLINE_TIMEOUT_MS");
        const long v = e && *e ? atol(e) : 20000;
        return uint32_t(v < 1000 ? 1000 : v);
    }();
    return ms;
}

const uint64_t kRefreshMs = 30ull * 60 * 1000;
const uint64_t kRetryMs = 5ull * 60 * 1000;
const uint32_t kClockTimeoutMs = 10000;

// The game's wide name (UTF-16) as printable ASCII, at most 10 characters; anything else becomes '?'.
void NameToUtf8(const unsigned short* wide, char* out, size_t size) {
    size_t n = 0;
    for (size_t i = 0; wide && wide[i] && n < 10 && n + 1 < size; ++i) {
        const unsigned short c = wide[i];
        out[n++] = (c >= 0x20 && c < 0x7F) ? char(c) : '?';
    }
    if (!n) { const char* d = "unnamed"; while (*d && n + 1 < size) out[n++] = *d++; }
    out[n] = 0;
}

void FailLogin(Fail fail, DWCLoginCallback callback, void* param) {
    int type, code;
    memset(g_login.sent.token, 0, sizeof g_login.sent.token);
    NpDwcErrorFor(fail, &type, &code);
    NpDwcSetError(type, code);
    ONLINE_LOG("login failed (reason %d): error %d code %d\n", int(fail), type, code);
    if (NpMpDiagError) NpMpDiagError(2, code, "login_failed");
    if (callback) callback((DWCError)type, 0, param);
}

void PumpLogin() {
    pthread_mutex_lock(&g_lock);
    if (g_login.state != LoginState::Requesting) { pthread_mutex_unlock(&g_lock); return; }
    Api* api = GetApi();
    LoginReply reply;
    const Result r = api->PollLogin(g_login.request, &reply);
    DWCLoginCallback callback = g_login.callback;
    void* param = g_login.param;
    if (r.status == Status::Pending) {
        if (NowMs() >= g_login.deadline) {
            api->Cancel(g_login.request);
            g_login.state = LoginState::Idle;
            pthread_mutex_unlock(&g_lock);
            FailLogin(Fail::Timeout, callback, param);
            return;
        }
        pthread_mutex_unlock(&g_lock);
        return;
    }
    if (r.status == Status::Failed) {
        if (r.fail == Fail::Auth && g_login.haveToken && !g_login.retried) {
            // The service does not know this token any more (a reset): start over as a new player, once.
            ONLINE_LOG("login: the stored token was refused (http %d); registering again\n", r.http);
            NpOnlineForgetCredentials();
            g_login.retried = true;
            g_login.haveToken = false;
            LoginRequest again = g_login.sent;
            memset(again.token, 0, sizeof again.token);
            const uint32_t id = api->BeginLogin(again);
            if (id) {
                g_login.request = id;
                g_login.deadline = NowMs() + LoginTimeoutMs();
                pthread_mutex_unlock(&g_lock);
                return;
            }
        }
        g_login.state = LoginState::Idle;
        pthread_mutex_unlock(&g_lock);
        FailLogin(r.fail == Fail::None ? Fail::Protocol : r.fail, callback, param);
        return;
    }
    // Ok: validate the reply
    g_login.state = LoginState::Idle;
    if (reply.nameCheck == NameCheck::Invalid && reply.profileId <= 0) {
        // The service refused the name (invalid, reserved, or taken): the game shows its own "name not allowed" message when the callback
        // reports success and DWC_GetIngamesnCheckResult() says 2. The player is asked for another name next time.
        g_login.nameCheck = DWC_INGAMESN_INVALID;
        memset(g_login.sent.token, 0, sizeof g_login.sent.token);
        pthread_mutex_unlock(&g_lock);
        ONLINE_LOG("login: the service did not accept the name\n");
        NpOnline::ForgetName();
        if (callback) callback((DWCError)0, 0, param);
        return;
    }
    if (reply.profileId <= 0) {
        pthread_mutex_unlock(&g_lock);
        FailLogin(Fail::Protocol, callback, param);
        return;
    }
    if (reply.newToken) {
        NpOnlineCredentials creds;
        creds.profileId = reply.profileId;
        memcpy(creds.token, reply.token, sizeof creds.token);
        if (!NpOnlineSaveCredentials(creds))
            ONLINE_LOG("warning: the new account's credentials could not be stored; the next start registers again\n");
        memset(&creds, 0, sizeof creds);
    }
    memset(reply.token, 0, sizeof reply.token);
    memset(g_login.sent.token, 0, sizeof g_login.sent.token);
    g_login.profile = reply.profileId;
    g_login.nameCheck = reply.nameCheck == NameCheck::Invalid ? DWC_INGAMESN_INVALID : (reply.nameCheck == NameCheck::Valid ? DWC_INGAMESN_VALID : DWC_INGAMESN_NOT_CHECKED);
    if (reply.code[0]) NpOnline::SetPublicCode(reply.code);
    if (reply.serverTimeMs > 0) NpDwcClockSet(reply.serverTimeMs);
    DWCUserData* userdata = g_login.userdata;
    pthread_mutex_unlock(&g_lock);
    if (userdata && (!NpDwcUserDataHasProfile(userdata) || NpDwcUserDataProfile(userdata) != reply.profileId))
        NpDwcUserDataSetProfile(userdata, reply.profileId);
    ONLINE_LOG("login ok: profile %d%s\n", int(reply.profileId), reply.newToken ? " (new account)" : "");
    if (NpMpDiagReach) NpMpDiagReach(2);   // (mpbeta27) signed in
    NpOnline::QueueKick();   // scores and results that did not get through earlier are due now
    if (callback) callback((DWCError)0, int(reply.profileId), param);
}

void PumpClock() {
    if (!g_clock.synced) return;
    Api* api = GetApi();
    const uint64_t now = NowMs();
    if (g_clock.request) {
        TimeReply reply;
        const Result r = api->PollTime(g_clock.request, &reply);
        if (r.status == Status::Pending) {
            if (now >= g_clock.deadline) { api->Cancel(g_clock.request); g_clock.request = 0; g_clock.nextRefreshMs = now + kRetryMs; }
            return;
        }
        g_clock.request = 0;
        if (r.status == Status::Ok && reply.unixMs > 0) { NpDwcClockSet(reply.unixMs); return; }
        g_clock.nextRefreshMs = now + kRetryMs;
        return;
    }
    if (now >= g_clock.nextRefreshMs && NpDwcLoggedInProfile() > 0) {
        g_clock.request = api->BeginTime();
        g_clock.deadline = now + kClockTimeoutMs;
        if (!g_clock.request) g_clock.nextRefreshMs = now + kRetryMs;
    }
}

}  // namespace

// ---- the clock -------------------------------------------------------------------------------------------------------------------
void NpDwcClockSet(int64_t unixMs) {
    g_clock.unixMs = unixMs;
    g_clock.monoMs = NowMs();
    g_clock.synced = true;
    g_clock.nextRefreshMs = g_clock.monoMs + kRefreshMs;
}

int64_t NpDwcClockNowMs() {
    if (!g_clock.synced) return -1;
    return g_clock.unixMs + int64_t(NowMs() - g_clock.monoMs);
}

// Civil date of a UTC time (Howard Hinnant's algorithm). month0: 0 = January, wday: 0 = Sunday, yday: 0 = 1 January.
bool NpDwcClockCivil(int64_t unixMs, int* year, int* month0, int* mday, int* hour, int* minute, int* second, int* msec, int* wday, int* yday) {
    if (unixMs < 0) return false;
    int64_t days = unixMs / 86400000;
    int64_t rem = unixMs % 86400000;
    const int64_t z = days + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    const int64_t d = doy - (153 * mp + 2) / 5 + 1;
    const int64_t m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) ++y;
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    static const int cumulative[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    *year = int(y);
    *month0 = int(m - 1);
    *mday = int(d);
    *yday = cumulative[m - 1] + int(d) - 1 + (leap && m > 2 ? 1 : 0);
    *wday = int((days + 4) % 7);
    *hour = int(rem / 3600000);
    *minute = int(rem / 60000 % 60);
    *second = int(rem / 1000 % 60);
    *msec = int(rem % 1000);
    return true;
}

int32_t NpDwcLoggedInProfile() { return g_login.profile; }

void NpDwcAddPump(void (*hook)(void)) {
    pthread_mutex_lock(&g_lock);
    bool present = false;
    for (int i = 0; i < g_pumpCount; ++i) present = present || g_pumps[i] == hook;
    if (!present && g_pumpCount < kMaxPumps) g_pumps[g_pumpCount++] = hook;
    pthread_mutex_unlock(&g_lock);
}

void NpDwcResetForTest() {
    pthread_mutex_lock(&g_lock);
    Api* api = GetApi();
    if (g_login.state == LoginState::Requesting) api->Cancel(g_login.request);
    if (g_clock.request) api->Cancel(g_clock.request);
    memset(&g_login, 0, sizeof g_login);
    memset(&g_clock, 0, sizeof g_clock);
    g_pumpCount = 0;
    pthread_mutex_unlock(&g_lock);
    NpDwcErrorResetForTest();
    NpDwcRankResetForTest();
}

// ---- DWC entry points ------------------------------------------------------------------------------------------------------------
extern "C" int DWC_Init(DWC_AuthServer, const char*, unsigned long gameCode, DWCAllocEx, DWCFreeEx) {
    pthread_mutex_lock(&g_lock);
    if (g_login.state == LoginState::Requesting) GetApi()->Cancel(g_login.request);
    g_login.state = LoginState::Idle;
    g_login.inited = true;
    g_login.friendsReady = false;
    g_login.userdata = 0;
    g_login.profile = 0;
    pthread_mutex_unlock(&g_lock);
    NpDwcSetGameCode(uint32_t(gameCode));
    return 0;
}

extern "C" void DWC_Shutdown() {
    pthread_mutex_lock(&g_lock);
    if (g_login.state == LoginState::Requesting) GetApi()->Cancel(g_login.request);
    g_login.state = LoginState::Idle;
    g_login.inited = false;
    g_login.friendsReady = false;
    pthread_mutex_unlock(&g_lock);
}

extern "C" void DWC_SetReportLevel(unsigned long) {}

extern "C" void NpDwcFriendsRegisterList(DWCFriendData* list, int len);   // np_dwc_friend.cpp: the game's friend list

extern "C" void DWC_InitFriendsMatch(DWCFriendsMatchControl*, DWCUserData* userdata, int, const char*, const char*, int, int, DWCFriendData* friendList, int friendListLen) {
    NpDwcFriendsRegisterList(friendList, friendListLen);
    pthread_mutex_lock(&g_lock);
    g_login.userdata = userdata;
    g_login.friendsReady = true;
    pthread_mutex_unlock(&g_lock);
}

extern "C" int DWC_LoginAsync(const unsigned short* ingamesn, const char*, DWCLoginCallback callback, void* param) {
    pthread_mutex_lock(&g_lock);
    int type, code;
    if (!g_login.inited || !g_login.friendsReady || !g_login.userdata || g_login.state != LoginState::Idle || !NpDwcUserDataValid(g_login.userdata)) {
        NpDwcErrorFor(Fail::Rejected, &type, &code);
        pthread_mutex_unlock(&g_lock);
        NpDwcSetError(type, code);
        return 0;
    }
    LoginRequest request;
    NameToUtf8(ingamesn, request.name, sizeof request.name);
    request.gameCode = NpDwcGameCode();
    request.region = request.gameCode == 0x5234514Au ? 1 : (request.gameCode == 0x52345150u ? 4 : 2);   // R4QJ, R4QP, else R4QE
    NpOnlineCredentials creds;
    if (NpOnlineLoadCredentials(&creds)) {
        memcpy(request.token, creds.token, sizeof request.token);
    }
    memset(&creds, 0, sizeof creds);
    Api* api = GetApi();
    const uint32_t id = api->BeginLogin(request);
    if (!id) {
        memset(request.token, 0, sizeof request.token);
        NpDwcErrorFor(Fail::Transport, &type, &code);
        pthread_mutex_unlock(&g_lock);
        NpDwcSetError(type, code);
        return 0;
    }
    g_login.sent = request;
    memset(request.token, 0, sizeof request.token);
    g_login.haveToken = g_login.sent.haveToken();
    g_login.retried = false;
    g_login.request = id;
    g_login.deadline = NowMs() + LoginTimeoutMs();
    g_login.callback = callback;
    g_login.param = param;
    g_login.state = LoginState::Requesting;
    g_login.nameCheck = DWC_INGAMESN_NOT_CHECKED;
    pthread_mutex_unlock(&g_lock);
    return 1;
}

extern "C" void DWC_ProcessFriendsMatch(void) {
    PumpLogin();
    PumpClock();
    NpOnline::QueuePump(NpDwcRankBusy());
    for (int i = 0; i < g_pumpCount; ++i) g_pumps[i]();
}

extern "C" void NpDwcFriendsShutdown(void);

extern "C" void DWC_ShutdownFriendsMatch(void) {
    NpDwcFriendsShutdown();
    pthread_mutex_lock(&g_lock);
    if (g_login.state == LoginState::Requesting) GetApi()->Cancel(g_login.request);
    g_login.state = LoginState::Idle;
    g_login.friendsReady = false;
    pthread_mutex_unlock(&g_lock);
}

// The game's check of the typed name: 0 = not checked / fine. 2 (DWC_INGAMESN_INVALID) only when the service rejected the name,
// which makes the game show its "name not allowed" message instead of continuing.
extern "C" DWCIngamesnCheckResult DWC_GetIngamesnCheckResult(void) {
    return g_login.nameCheck == DWC_INGAMESN_INVALID ? DWC_INGAMESN_INVALID : DWC_INGAMESN_NOT_CHECKED;
}

extern "C" BOOL DWC_GetDateTime(DWCDate* date, DWCTime* time) {
    const int64_t now = NpDwcClockNowMs();
    int year, month0, mday, hour, minute, second, msec, wday, yday;
    if (now < 0 || !NpDwcClockCivil(now, &year, &month0, &mday, &hour, &minute, &second, &msec, &wday, &yday)) return 0;
    if (date) {
        date->mday = mday;
        date->month = month0;
        date->year = year;
        date->wday = wday;
        date->yday = yday;
    }
    if (time) {
        time->sec = second;
        time->min = minute;
        time->hour = hour;
        time->msec = msec;
        time->usec = 0;
    }
    return 1;
}

// Forgetting the online account: the three files and every copy of them that a safe replace or a damaged-file set-aside can have left (np_safe_file.h RemoveAll).
// Nothing of the token, the name or the pending results stays on the card.
void NpOnlineForgetAccount() {
    NpOnlineForgetCredentials();
    NpOnline::ForgetName();
    NpOnline::QueueForget();
}
