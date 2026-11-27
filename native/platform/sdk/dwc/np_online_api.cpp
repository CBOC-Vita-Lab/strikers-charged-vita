// The API registry, the clock, the data directory and the "unavailable" API. See np_online_api.h.
#include "platform/sdk/dwc/np_online_api.h"
#include "platform/sdk/dwc/np_online_fake.h"
#include "platform/sdk/net/online/np_app_version.h"
#include "platform/sdk/include/np/np_text.h"

#include <pthread.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "np/gamefixes.h"
#include "platform/sdk/include/np/np_sdk.h"

// The SDK layer's configuration (data_dir). Weak: the standalone tests do not link it.
extern "C" const NpSdkConfig* NpSdk_GetConfig(void) __attribute__((weak));

namespace NpOnline {
namespace {

pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
Api* g_api = nullptr;        // the installed API (SetApi)
Api* g_default = nullptr;    // the one chosen by GetApi
uint64_t (*g_clock)(void) = nullptr;

// Every request fails at once: nothing can reach a service.
class Unavailable : public Api {
public:
    uint32_t BeginLogin(const LoginRequest&) override { return Next(); }
    uint32_t BeginTime() override { return Next(); }
    uint32_t BeginPutScore(const PutRequest&) override { return Next(); }
    uint32_t BeginGetScores(const GetRequest&) override { return Next(); }
    uint32_t BeginReportResult(const ResultReport&) override { return Next(); }
    uint32_t BeginMatchStatus(const char*) override { return Next(); }
    Result PollLogin(uint32_t, LoginReply*) override { return Failed(); }
    Result PollTime(uint32_t, TimeReply*) override { return Failed(); }
    Result PollPutScore(uint32_t, PutReply*) override { return Failed(); }
    Result PollGetScores(uint32_t, GetReply*) override { return Failed(); }
    Result PollReportResult(uint32_t) override { return Failed(); }
    Result PollMatchStatus(uint32_t, MatchStatus*) override { return Failed(); }
    void Cancel(uint32_t) override {}

private:
    uint32_t Next() { return ++m_next; }
    static Result Failed() {
        Result r;
        r.status = Status::Failed;
        r.fail = Fail::Transport;
        return r;
    }
    uint32_t m_next = 0;
};

}  // namespace

Api* GetApi() {
    pthread_mutex_lock(&g_lock);
    Api* api = g_api;
    if (!api) {
        if (!g_default) {
            const char* choice = getenv("MSC_ONLINE_API");
            if (choice && !strcmp(choice, "fake")) {
                const FakeOptions options = FakeOptionsFromEnvironment();
                g_default = CreateFakeApi(&options);
                FakeApplyEnvironment(g_default);
                fprintf(stderr, "[online] API: in-process fake server\n");
            } else if (NpOnlineCreateHttpApi) {
                g_default = NpOnlineCreateHttpApi();
            }
            if (!g_default) {
                g_default = new Unavailable();
                fprintf(stderr, "[online] API: none available (no service client linked)\n");
            }
        }
        api = g_default;
    }
    pthread_mutex_unlock(&g_lock);
    return api;
}

void SetApi(Api* api) {
    pthread_mutex_lock(&g_lock);
    g_api = api;
    pthread_mutex_unlock(&g_lock);
}

uint64_t NowMs() {
    if (g_clock) return g_clock();
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000u + uint64_t(ts.tv_nsec / 1000000);
}

void SetClockForTest(uint64_t (*clock)(void)) { g_clock = clock; }

}  // namespace NpOnline
extern "C" unsigned NpLanFingerprint() __attribute__((weak));   // so_net.cpp (absent in the small tests: no header then, as before)
namespace NpOnline {

const ClientInfo& GetClientInfo() {
    static ClientInfo info;
    static bool ready = false;
    if (!ready) {
        const char* version = NpNet::DefaultAppVersion();   // platform/sdk/net/online/np_app_version.h: shared with the signaling client
        const char* build =
#ifdef NP_ONLINE_BUILD
            NP_ONLINE_BUILD;
#else
            "";   // the service wants 8..64 hex digits (the build / rule fingerprint) or no header
        // (mpbeta27) the link fingerprint (so_net.cpp NpLanFingerprint: this build and its match rules) as X-Msc-Build, so that the ranked / quick queue pairs only
        // consoles that can play each other (the service's QUEUE_SAME_BUILD): two other builds are refused by the link on every frame. MSC_ONLINE_BUILD_HEADER=0: none.
        static char fingerprint[16];
        const char* header = getenv("MSC_ONLINE_BUILD_HEADER");
        if (NpLanFingerprint && !(header && header[0] == '0')) {
            snprintf(fingerprint, sizeof fingerprint, "%08x", NpLanFingerprint());
            build = fingerprint;
        }
#endif
        snprintf(info.appVersion, sizeof info.appVersion, "%s", version);
        snprintf(info.build, sizeof info.build, "%s", build);
        {   // what the service accepts: dotted numbers for the version, 8..64 hex digits for the build; anything else falls back / is left out
            bool okVersion = info.appVersion[0] != 0;
            int dots = 0;
            for (const char* c = info.appVersion; *c; ++c) {
                if (*c == '.') ++dots;
                else if (*c < '0' || *c > '9') okVersion = false;
            }
            if (!okVersion || dots > 3 || info.appVersion[0] == '.' || info.appVersion[strlen(info.appVersion) - 1] == '.') snprintf(info.appVersion, sizeof info.appVersion, "1.2.0");
            size_t n = strlen(info.build);
            bool okBuild = n >= 8 && n <= 64;
            for (const char* c = info.build; *c && okBuild; ++c) okBuild = isxdigit(static_cast<unsigned char>(*c)) != 0;
            if (!okBuild) info.build[0] = 0;
        }
        ready = true;
    }
    return info;
}

const char* RulesTag() { return NpGameFix(NP_FIX_NK_BUG) ? "nk" : ""; }

bool RulesAllowRanked() {
    if (!RulesTag()[0]) return true;
    const char* allow = getenv("MSC_ONLINE_ALLOW_MODIFIED");
    return allow && allow[0] == '1';
}

namespace {
char g_status[128] = "";
char g_nameReject[160] = "";
}
void SetStatusMessage(const char* text) {
    pthread_mutex_lock(&g_lock);
    snprintf(g_status, sizeof g_status, "%s", text ? text : "");
    pthread_mutex_unlock(&g_lock);
}
void GetStatusMessage(char* out, size_t size) {
    char text[sizeof g_status];
    pthread_mutex_lock(&g_lock);
    snprintf(text, sizeof text, "%s", g_status);
    pthread_mutex_unlock(&g_lock);
    NpText::Sentences(NpText::CurrentLanguage(), text, out, size);   // (1.3) in the game's language; a text the table does not have stays as it is
}
void SetNameRejectMessage(const char* text) {
    pthread_mutex_lock(&g_lock);
    snprintf(g_nameReject, sizeof g_nameReject, "%s", text ? text : "");
    pthread_mutex_unlock(&g_lock);
}
void GetNameRejectMessage(char* out, size_t size) {
    if (!out || !size) return;
    char text[sizeof g_nameReject];
    pthread_mutex_lock(&g_lock);
    snprintf(text, sizeof text, "%s", g_nameReject);
    pthread_mutex_unlock(&g_lock);
    NpText::Sentences(NpText::CurrentLanguage(), text, out, size);
}
extern "C" void NpOnlineGetNameRejectMessage(char* out, size_t size) { GetNameRejectMessage(out, size); }

namespace {
char g_code[16];
bool CodePath(char* out, size_t size) {
    const int n = snprintf(out, size, "%s/online_code.txt", DataDir());
    return n > 0 && size_t(n) < size;
}
bool CodeValid(const char* c) {
    if (!c || strlen(c) != 9 || c[4] != '-') return false;
    for (int i = 0; i < 9; ++i) if (i != 4 && !isalnum(static_cast<unsigned char>(c[i]))) return false;
    return true;
}
}  // namespace

void SetPublicCode(const char* code) {
    if (!CodeValid(code)) return;
    pthread_mutex_lock(&g_lock);
    const bool changed = strcmp(g_code, code) != 0;
    snprintf(g_code, sizeof g_code, "%s", code);
    pthread_mutex_unlock(&g_lock);
    char path[512];
    if (changed && CodePath(path, sizeof path)) {
        FILE* f = fopen(path, "wb");
        if (f) { fprintf(f, "%s\n", code); fclose(f); }
    }
}

void GetPublicCode(char* out, size_t size) {
    if (!out || !size) return;
    pthread_mutex_lock(&g_lock);
    bool have = g_code[0] != 0;
    snprintf(out, size, "%s", g_code);
    pthread_mutex_unlock(&g_lock);
    if (have) return;
    char path[512], line[32] = "";
    if (CodePath(path, sizeof path)) {
        FILE* f = fopen(path, "rb");
        if (f) {
            const size_t n = fread(line, 1, sizeof line - 1, f);
            fclose(f);
            line[n] = 0;
            char* end = strpbrk(line, "\r\n");
            if (end) *end = 0;
        }
    }
    snprintf(out, size, "%s", CodeValid(line) ? line : "");
}

const char* DataDir() {
    if (NpSdk_GetConfig) {
        const NpSdkConfig* cfg = NpSdk_GetConfig();
        if (cfg && cfg->data_dir && cfg->data_dir[0]) return cfg->data_dir;
    }
    const char* env = getenv("NP_DATA_DIR");
    return env && env[0] ? env : ".";
}

}  // namespace NpOnline
