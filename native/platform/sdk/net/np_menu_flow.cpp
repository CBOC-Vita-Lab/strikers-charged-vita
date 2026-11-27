// The online entry of the game's menus: see np_menu_flow.h. The state machine is stepped once per game loop (NpMenuStep); it never blocks and does
// at most one piece of work per frame (one disk access, one request started, or one poll of the transport, which handles a bounded number of packets).
#include "platform/sdk/net/np_menu_flow.h"
#include "platform/sdk/save/np_safe_file.h"
#include "platform/sdk/include/np/np_text.h"
#include "platform/sdk/net/online/np_app_version.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

#include "platform/sdk/include/np/np_sdk.h"
#include "platform/sdk/net/np_keyboard.h"
#include "platform/sdk/net/online/np_http.h"
#include "platform/sdk/net/online/np_json.h"
#include "platform/sdk/net/online/np_net.h"
#include "platform/sdk/net/online/np_service_url.h"
#include "platform/sdk/net/online/np_online_api.h"
#include "platform/sdk/net/np_conntest.h"
#include "platform/sdk/net/np_mpdiag.h"
#include "platform/sdk/net/np_mpreport.h"
#include "platform/sdk/net/np_mpbeta.h"

#define MENU_LOG(...) fprintf(stderr, "[online-menu] " __VA_ARGS__)

extern "C" uint64_t np_time_ns(void) __attribute__((weak));               // the SDK layer's clock (virtual in harness runs)
extern "C" int np_https_clock_ok(void) __attribute__((weak));             // np_https.h: the console clock is not earlier than this build
extern "C" const char* NpLanPlayerName(void);                              // np_lan.h
extern "C" unsigned NpLanFingerprint(void);                                // np_lan.h
extern "C" int NpLanPartnerRefused(void) __attribute__((weak));            // np_lan.cpp (mpbeta27): the link refused a console with another build in this session
extern "C" void NpLanLinkRestored(void) __attribute__((weak));             // np_lan.h: the link was rebuilt, the session layer may send again
extern "C" int NpPlatformWifiUp(void) __attribute__((weak));              // vlink_vita.cpp: 1 connected, 0 not, -1 unknown
extern "C" const NpSdkConfig* NpSdk_GetConfig(void) __attribute__((weak));   // the SDK layer's configuration (data_dir); absent in the standalone tests
extern "C" int NpOnlineCentralAvailable(void) __attribute__((weak));
extern "C++" void NpOnlineForgetAccount() __attribute__((weak));   // np_dwc_login.cpp: online.dat, online_name.txt and online_queue.dat with every copy (absent in the standalone tests)       // np_online_flow.cpp: Online Central is linked (absent in the standalone tests)

namespace {

// ---- clock ----------------------------------------------------------------------------------------------------------------------------------
// The game clock: the SDK layer's (virtual in a scripted harness run), else the monotonic clock.
uint64_t NowMs() {
    if (np_time_ns) return np_time_ns() / 1000000ull;
    return NpNet::SteadyNowMs(0);
}

int EnvInt(const char* name, int fallback) {
    const char* v = getenv(name);
    return v && *v ? atoi(v) : fallback;
}

// ---- files --------------------------------------------------------------------------------------------------------------------------------------
const char* DataDir() {
    if (NpSdk_GetConfig) {
        const NpSdkConfig* cfg = NpSdk_GetConfig();
        if (cfg && cfg->data_dir && cfg->data_dir[0]) return cfg->data_dir;
    }
    const char* env = getenv("NP_DATA_DIR");
    return env && *env ? env : ".";
}

bool PathFor(const char* file, char* out, size_t size) {
    const int n = snprintf(out, size, "%s/%s", DataDir(), file);
    return n > 0 && size_t(n) < size;
}

// ---- the account: the public name and the token of the service (the formats of the ranking client: online_name.txt, online.dat v2) --------------------------
const size_t kMaxName = 10;
const size_t kTokenChars = 64;

uint32_t Crc32(const void* data, size_t size) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t crc = ~0u;
    for (size_t i = 0; i < size; ++i) crc = (crc >> 8) ^ table[(crc ^ p[i]) & 0xFF];
    return ~crc;
}
void Put32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24); }
uint32_t Get32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

// The service's name rules (server/PROTOCOL.md section 3): 2..10 characters of A-Z a-z 0-9 space . _ -, starting and ending with a letter or digit,
// no double spaces, at least two letters or digits. Everything else is dropped, spaces collapse, the ends are trimmed.
bool SanitizeName(const char* in, char* out) {
    size_t n = 0;
    bool lastSpace = false;
    for (size_t i = 0; in && in[i]; ++i) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        const bool alnum = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        if (c == ' ' || c == '\t') {
            if (n && !lastSpace) { out[n++] = ' '; lastSpace = true; }
        } else if (alnum || c == '.' || c == '_' || c == '-') {
            if (!n && !alnum) continue;
            out[n++] = char(c);
            lastSpace = false;
        }
        if (n >= kMaxName) break;
    }
    while (n && !isalnum(static_cast<unsigned char>(out[n - 1]))) --n;
    out[n] = 0;
    size_t letters = 0;
    for (size_t i = 0; i < n; ++i) letters += isalnum(static_cast<unsigned char>(out[i])) ? 1 : 0;
    if (letters < 2) { out[0] = 0; n = 0; }
    return n > 0;
}

struct Account {
    char name[kMaxName + 1];
    char token[kTokenChars];
    int profile;
    Account() : profile(0) { name[0] = 0; token[0] = 0; }
    bool Ready() const { return name[0] && token[0] && profile > 0; }
};

bool TokenValid(const char* token) {
    size_t n = 0;
    for (; token && token[n]; ++n) {
        const char c = token[n];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
        if (n >= kTokenChars - 1) return false;
    }
    return n >= 16;
}

const uint8_t kMagic[4] = {'M', 'S', 'C', 'O'};
const uint32_t kStoreVersion = 2;
const size_t kStoreSize = 4 + 4 + 4 + kTokenChars + 4;   // magic version profile token crc

// The two account files are replaced and recovered by np_safe_file.h (synced, a power cut leaves the old file or the new one; on the console a plain rename over an
// existing file removes it first, so a cut between the two steps used to lose the account). A damaged file is set aside as <file>.bad: not loaded, never overwritten.
bool NameFileSound(const std::string& d) {
    char line[64], clean[kMaxName + 1];
    snprintf(line, sizeof line, "%.*s", int(d.size() < sizeof line - 1 ? d.size() : sizeof line - 1), d.c_str());
    if (char* end = strpbrk(line, "\r\n")) *end = 0;
    return SanitizeName(line, clean) && !strcmp(clean, line);
}
bool TokenFileSound(const std::string& d) {
    const uint8_t* b = reinterpret_cast<const uint8_t*>(d.data());
    return d.size() == kStoreSize && !memcmp(b, kMagic, 4) && Get32(b + 4) == kStoreVersion && Get32(b + kStoreSize - 4) == Crc32(b, kStoreSize - 4);
}
NpSafeFile::Options NameFileOptions() { NpSafeFile::Options o; o.valid = NameFileSound; o.mode = 0644; o.maxBytes = 4096; return o; }
NpSafeFile::Options TokenFileOptions() { NpSafeFile::Options o; o.valid = TokenFileSound; o.mode = 0600; o.maxBytes = 4096; return o; }

bool LoadAccount(Account* a) {
    char path[512];
    if (const char* env = getenv("MSC_ONLINE_NAME")) {   // an environment name wins on every build (scripted runs, the console's np-player.txt)
        char clean[kMaxName + 1];
        if (*env && SanitizeName(env, clean)) snprintf(a->name, sizeof a->name, "%s", clean);
    }
    if (!a->name[0] && PathFor("online_name.txt", path, sizeof path)) {
        char line[64];
        NpSafeFile::Recover(path, NameFileOptions());
        if (FILE* f = fopen(path, "rb")) {
            const size_t n = fread(line, 1, sizeof line - 1, f);
            fclose(f);
            line[n] = 0;
            if (char* end = strpbrk(line, "\r\n")) *end = 0;
            char clean[kMaxName + 1];
            if (SanitizeName(line, clean) && !strcmp(clean, line)) snprintf(a->name, sizeof a->name, "%s", clean);
        }
    }
    if (!PathFor("online.dat", path, sizeof path)) return false;
    NpSafeFile::Recover(path, TokenFileOptions());
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    uint8_t buf[kStoreSize + 1];
    const size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    bool ok = n == kStoreSize && !memcmp(buf, kMagic, 4) && Get32(buf + 4) == kStoreVersion && Get32(buf + kStoreSize - 4) == Crc32(buf, kStoreSize - 4);
    if (ok) {
        a->profile = int(int32_t(Get32(buf + 8)));
        memcpy(a->token, buf + 12, kTokenChars);
        a->token[kTokenChars - 1] = 0;
        ok = a->profile > 0 && TokenValid(a->token);
        if (!ok) { a->profile = 0; a->token[0] = 0; }
    }
    memset(buf, 0, sizeof buf);
    return ok;
}

bool SaveName(const char* name) {
    char path[512];
    if (!PathFor("online_name.txt", path, sizeof path)) return false;
    return NpSafeFile::Replace(path, std::string(name) + "\n", NameFileOptions());
}

bool SaveToken(int profile, const char* token) {
    char path[512];
    if (profile <= 0 || !TokenValid(token) || !PathFor("online.dat", path, sizeof path)) return false;
    uint8_t buf[kStoreSize];
    memcpy(buf, kMagic, 4);
    Put32(buf + 4, kStoreVersion);
    Put32(buf + 8, uint32_t(profile));
    memset(buf + 12, 0, kTokenChars);
    strncpy(reinterpret_cast<char*>(buf + 12), token, kTokenChars - 1);
    Put32(buf + kStoreSize - 4, Crc32(buf, kStoreSize - 4));
    std::string bytes(reinterpret_cast<const char*>(buf), kStoreSize);
    memset(buf, 0, sizeof buf);
    const bool ok = NpSafeFile::Replace(path, bytes, TokenFileOptions());
    bytes.assign(bytes.size(), '\0');
    return ok;
}

// ---- the service address (C5) ---------------------------------------------------------------------------------------------------------------------
// MSC_ONLINE_URL (np-player.txt / environment; "https://a,https://b"), else MSC_NET_SERVER (the desktop tests' local fake), else the downloaded server list
// (<data dir>/servers.json: the shape of server/servers.example.json, lowest priority first), else the build's NP_ONLINE_URL. https only on the console.
bool UrlAllowed(const std::string& u) {
    if (u.compare(0, 8, "https://") == 0) return u.size() > 12;
#if !defined(NP_ONLINE_HTTPS_ONLY)   // (np_service_url.h: the console, and a host test build under the console's rule)
    if (u.compare(0, 7, "http://") == 0) return u.size() > 10;   // the desktop tests' local fake; the console talks TLS or nothing
#endif
    return false;
}

void AddUrls(const char* list, std::vector<std::string>* out) {
    std::string all = list ? list : "";
    size_t p = 0;
    while (p <= all.size()) {
        size_t e = all.find(',', p);
        if (e == std::string::npos) e = all.size();
        std::string u = all.substr(p, e - p);
        while (!u.empty() && (u[0] == ' ')) u.erase(0, 1);
        while (!u.empty() && (u[u.size() - 1] == '/' || u[u.size() - 1] == ' ')) u.resize(u.size() - 1);
        if (UrlAllowed(u) && std::find(out->begin(), out->end(), u) == out->end()) out->push_back(u);
        p = e + 1;
    }
}

std::vector<std::string> ServiceUrls() {
    std::vector<std::string> urls;
    AddUrls(getenv("MSC_ONLINE_URL"), &urls);
    if (urls.empty()) AddUrls(getenv("MSC_NET_SERVER"), &urls);
    if (urls.empty()) {
        char path[512];
        if (PathFor("servers.json", path, sizeof path)) {
            if (FILE* f = fopen(path, "rb")) {
                std::string text;
                char buf[1024];
                size_t n;
                while ((n = fread(buf, 1, sizeof buf, f)) > 0 && text.size() < 16384) text.append(buf, n);
                fclose(f);
                NpNet::Json j;
                if (j.Parse(text.data(), text.size())) {
                    const int arr = j.Get(j.Root(), "servers");
                    std::vector<std::pair<int, std::string> > list;
                    for (int i = 0; i < j.Size(arr) && i < 8; ++i) {
                        const int s = j.At(arr, i);
                        list.push_back(std::make_pair(int(j.GetNum(s, "priority", 0)), j.GetStr(s, "url")));
                    }
                    std::stable_sort(list.begin(), list.end(), [](const std::pair<int, std::string>& a, const std::pair<int, std::string>& b) { return a.first < b.first; });
                    for (size_t i = 0; i < list.size(); ++i) AddUrls(list[i].second.c_str(), &urls);
                }
            }
        }
    }
#ifdef NP_ONLINE_URL
    if (urls.empty()) AddUrls(NpServiceBuildUrl().c_str(), &urls);
#endif
    return urls;
}

std::string Join(const std::vector<std::string>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) s += (i ? "," : "") + v[i];
    return s;
}

const char* AppVersion() {
    const char* e = getenv("MSC_NET_APP");
    if (e && *e) return NpNet::AppVersionOrDefault(e);
    return NpNet::DefaultAppVersion();   // never empty: the production service refuses a request without X-Msc-App
}

// ---- sentences ------------------------------------------------------------------------------------------------------------------------------------
// One plain sentence for each failure (the codes of server/PROTOCOL.md section 1; the player never sees a code).
std::string ServiceSentence(const std::string& code, const std::string& message, const std::string& reason, int status, const std::string& latest) {
    if (code == "update_required" || code == "upgrade_required") {
        if (latest.empty()) return "Different version: please update the game.";
        char line[160];   // (1.3: the sentence in the game's language around the version number; np_text.h)
        snprintf(line, sizeof line, NpText::Tr("Different version: please update the game to %s."), latest.c_str());
        return line;
    }
    if (code == "name_taken") return "That name is already taken. Choose another one.";
    if (code == "bad_name") {
        if (reason == "reserved") return "That name is reserved. Choose another one.";
        if (reason == "filtered") return "That name is not allowed. Choose another one.";
        return "Use 2 to 10 letters or digits for the name.";
    }
    if (code == "rate_limited" && reason == "daily_cap") return "Daily limit reached. Try again tomorrow.";
    if (code == "rate_limited") return "Too many tries. Wait a minute and try again.";
    if (code == "service_disabled") return message.empty() ? "Online play is switched off for now." : message;
    if (code == "server_full" || code == "busy" || code == "turn_budget") return "The service is busy. Try again in a moment.";
    if (status >= 500) return "The service is busy or down. Try again in a moment.";
    if (code == "unauthorized") return "The online account was not accepted. Try again.";
    char st[24];
    snprintf(st, sizeof st, "status %d", status);
    char line[160];
    snprintf(line, sizeof line, NpText::Tr("The service refused the request (%s)."), code.empty() ? st : code.c_str());
    return line;
}

// "K7Q-M4D": a room code the way a person reads it aloud (the service ignores case, spaces and dashes).
void CodeForDisplay(const char* code, char* out, size_t size) {
    char raw[16];
    size_t n = 0;
    for (size_t i = 0; code && code[i] && n < sizeof raw - 1; ++i)
        if (isalnum(static_cast<unsigned char>(code[i]))) raw[n++] = char(toupper(static_cast<unsigned char>(code[i])));
    raw[n] = 0;
    if (n > 3) snprintf(out, size, "%.3s-%s", raw, raw + 3);
    else snprintf(out, size, "%s", raw);
}

// What a person typed as an invite code: letters and digits of the service's alphabet, 6 of them. False when it cannot be one.
bool NormalizeCode(const char* typed, char* out, size_t size) {
    static const char kAlphabet[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
    size_t n = 0;
    char raw[16];
    for (size_t i = 0; typed && typed[i]; ++i) {
        const unsigned char c = static_cast<unsigned char>(typed[i]);
        if (c == ' ' || c == '-' || c == '_' || c == '.') continue;
        const char u = char(toupper(c));   // (the alphabet has no I L O 0 1: they are refused, not guessed)
        if (!strchr(kAlphabet, u) || n >= 8) return false;
        raw[n++] = u;
    }
    raw[n] = 0;
    if (n != 6 || size < 7) return false;
    memcpy(out, raw, 7);
    return true;
}

// ---- the flow ---------------------------------------------------------------------------------------------------------------------------------------
enum State {
    S_IDLE,
    S_MODE,          // popup: ONLINE / LOCAL / CANCEL
    S_LOCAL,         // popup: NEARBY / SAME WI-FI / BACK
    S_ONLINE,        // popup: FRIENDS / PLAY WITH ANYONE / RANKED MATCH (when NpMenuFriendsOffered)
    S_CHOOSE,        // popup: HOST / JOIN / CANCEL (with friends: FRIEND LIST / HOST / JOIN)
    S_ACCOUNT,       // the checks before anything is sent, then: account known? else the name
    S_NAME_KBD,      // the name keyboard
    S_REGISTER,      // POST /v1/register, with a status popup
    S_CODE_KBD,      // the invite code keyboard
    S_CONNECT,       // bring-up of the transport with a status popup
    S_FAIL,          // a failure popup: wait for OK
    S_RPT_OFFER,     // popup: "your last online game ended with a problem": SEND A REPORT / NOT NOW (once per session)
    S_RPT_ASK,       // popup: exactly what a report contains: SEND / CANCEL (nothing is sent before SEND)
    S_RPT_SEND,      // status popup while the report goes out (cancellable); then an OK popup with the result
    S_BETA_NOTICE,   // (mpbeta27) popup, once: "Multiplayer is in beta. ..." with OK
    S_CLOSING        // a popup was closed by us: wait until it is gone, then continue
};
enum Role { R_HOST, R_JOIN, R_TEST };   // R_TEST: the connection test (a room is made and left at once; nobody is invited)

struct Flow {
    State state;
    State after;                // S_CLOSING: where to go next
    int pendingResult;          // S_CLOSING: the result to report when the popup is gone (-1 = none, go on with `after`)
    int settle;                 // frames to wait after a popup was closed
    Role role;
    Account account;
    char code[16];              // the typed invite code, normalized (join)
    char typedCode[16];         // what the player typed for the code (as typed, up to 8 characters): the keyboard opens with it again after a failure
    char typedName[16];         // what the player typed for the name: the keyboard opens with it again after a refusal
    std::string failText;
    State failNext;             // after OK: S_IDLE ends the flow, S_NAME_KBD / S_CODE_KBD ask again
    bool offerTest;             // the failure popup also offers TEST CONNECTION (a cause the test can tell: the service, the route)
    int keyboardTries;
    bool keyboardOpen;
    uint64_t stageAt;           // game clock at the start of the stage (limits)
    int httpHandle;
    int baseTry;
    int stage;                  // the status line shown: 0 none, 1 service, 2 route, 3 waiting, 4 account
    char lastStatus[520];
    bool inviteFileWritten;
    bool popupWanted;           // a popup must be (re)opened
    bool choiceShown;           // S_CHOOSE: the HOST / JOIN popup was opened
    bool friendsLevel;          // S_CHOOSE is the FRIENDS popup (FRIEND LIST / HOST A ROOM / JOIN WITH A CODE), reached from S_ONLINE
    int localPick;
    Flow() : state(S_IDLE), after(S_IDLE), pendingResult(-1), settle(0), role(R_HOST), failNext(S_IDLE), offerTest(false), keyboardTries(0), keyboardOpen(false), stageAt(0),
             httpHandle(0), baseTry(0), stage(0), inviteFileWritten(false), popupWanted(false), choiceShown(false), friendsLevel(false), localPick(kNpMenuNearby) {
        code[0] = 0;
        typedCode[0] = 0;
        typedName[0] = 0;
        lastStatus[0] = 0;
    }
};
Flow g_flow;
NpMpReport::Op g_report;         // the report being offered / sent from the Multiplayer menu (np_mpreport.h)
bool g_quick = false;           // the Online Central entry of this run is PLAY WITH ANYONE
int g_linkMode = -1;            // current-run choice only; never rewrites the environment or the saved settings

// ---- the lobby after a lost link
struct Lost {
    bool offer;                 // the link was lost and the game ended its session: RECONNECT is to be offered
    uint64_t at;
    int step;                   // 0 idle, 1 asking, 2 reconnecting, 3 failure popup, 4 reconnected: waiting for the status popup to go
    bool failShown;             // step 3: the failure popup was opened
    std::string failText;
    char reason[120];
    Lost() : offer(false), at(0), step(0), failShown(false) { reason[0] = 0; }
};
Lost g_lost;
char g_endReasonKeep[120];      // why the last online link ended (for the match popup)

const uint32_t kRegisterLimitMs = 20000;
const uint32_t kServiceLimitMs = 30000;     // "Connecting to the service..." (the transport's own room limit is 15 s; this is the menu's net)
const uint32_t kRouteLimitMs = 60000;       // "Finding a route..."
const uint32_t kPartnerLimitMs = 600000;    // "Waiting for your partner..."
const uint32_t kResumeWindowMs = 30000;     // how long after a loss RECONNECT is still offered (the service drops silent tickets after 30 s)
const uint32_t kReconnectLimitMs = 45000;   // the reconnect itself (the transport's own rebuild limit)
const int kSettleFrames = 8;                // after a popup closes: the scene stack settles before the next scene change

void Enter(State s) {
    g_flow.state = s;
    g_flow.stageAt = NowMs();
    if (s == S_MODE || s == S_LOCAL || s == S_ONLINE || s == S_CHOOSE || s == S_RPT_OFFER || s == S_RPT_ASK || s == S_BETA_NOTICE) g_flow.choiceShown = false;
}

// Closes our popup (if one is open); once it is gone: report `result` (kNpMenuDone / kNpMenuEnter) or, when -1, go on with `next`.
void CloseThen(State next, int result) {
    NpMenuUiClose();
    g_flow.after = next;
    g_flow.pendingResult = result;
    g_flow.settle = kSettleFrames;
    g_flow.state = S_CLOSING;
}

void Fail(const std::string& text, State next, bool offerTest = false) {
    MENU_LOG("failure: %s\n", text.c_str());
    if (g_flow.role != R_TEST) NpMpDiag::Note(NpMpDiag::kOnlineFail, 0, 0, 0, text.c_str());   // the diagnostics ring (np_mpdiag.h): what the player was told
    g_flow.failText = text;
    g_flow.failNext = next;
    g_flow.offerTest = offerTest && g_flow.role != R_TEST;
    NpMenuUiClose();
    g_flow.state = S_FAIL;
    g_flow.popupWanted = true;
    g_flow.settle = kSettleFrames;   // the failure popup follows the status popup's removal after the scene stack settled (as after CloseThen)
}

// A plain OK popup that is not a failure of the online menu (a report's result): same screen as Fail, but nothing is written to the diagnostics ring (a "report sent"
// must not arm the offer of another report).
void Notice(const std::string& text, State next) {
    g_flow.failText = text;
    g_flow.failNext = next;
    g_flow.offerTest = false;
    NpMenuUiClose();
    g_flow.state = S_FAIL;
    g_flow.popupWanted = true;
    g_flow.settle = kSettleFrames;
}

// Starts the account request (POST /v1/register) on the next address of the list.
bool StartRegister() {
    std::vector<std::string> urls = ServiceUrls();
    if (urls.empty() || g_flow.baseTry >= int(urls.size())) return false;
    // (No relayOnly: that profile flag makes the service refuse direct addresses (relay_only_account); the transport's relay worker owns MSC_NET_RELAY_ONLY.)
    char body[160];
    snprintf(body, sizeof body, "{\"name\":\"%s\",\"region\":%d}", g_flow.account.name, EnvInt("MSC_ONLINE_REGION", 2));
    std::vector<std::string> headers;
    headers.push_back("Content-Type: application/json");
    headers.push_back(std::string("X-Msc-App: ") + AppVersion());
    char build[40];
    snprintf(build, sizeof build, "X-Msc-Build: %08x", NpLanFingerprint());
    headers.push_back(build);
    std::string err;
    g_flow.httpHandle = NpNet::HttpStart("POST", urls[size_t(g_flow.baseTry)] + "/v1/register", body, headers, 15000, &err);
    if (!g_flow.httpHandle) {
        MENU_LOG("register not started: %s\n", err.c_str());
        g_flow.failText = err;
        return false;
    }
    return true;
}

// ---- status lines ---------------------------------------------------------------------------------------------------------------------------------
// The text of a status popup: the status line, a blank line and a hint. Every status has the same shape so the box does not need to grow.
void StatusText(int stage, const char* invite, char* out, size_t size) {
    char shown[24];
    CodeForDisplay(invite, shown, sizeof shown);
    // Three lines, always (the popup keeps the size it was opened with): what is happening, what to know, how to go back. {p} is the game's own line break; each
    // line is put into the game's language here, because the text of a popup is translated sentence by sentence and a line break joins sentences.
    const char* l1 = "Connecting...";
    const char* l2 = "Please wait.";
    switch (stage) {
    case 1: l1 = "Connecting to the service..."; break;
    case 2: l1 = "Finding a route..."; l2 = "A direct link is being made."; break;
    case 3: l1 = g_flow.role == R_TEST ? "Checking your connection..." : "Waiting for your partner. Give them this code:"; l2 = g_flow.role == R_TEST ? "Please wait." : shown; break;
    case 4: l1 = "Creating your online account..."; break;
    default: break;
    }
    char a[160], b[160], c[160];
    NpText::Sentences(NpText::CurrentLanguage(), l1, a, sizeof a);
    NpText::Sentences(NpText::CurrentLanguage(), l2, b, sizeof b);
    NpText::Sentences(NpText::CurrentLanguage(), "Press Circle to cancel.", c, sizeof c);
    snprintf(out, size, "%s{p}%s{p}%s", a, b, c);
}

// The answer of the connection test: what the room probe found, put through the five checks and the classifier of np_conntest.h (the same code the host tests drive with fakes):
// Wi-Fi, same Wi-Fi (not asked here: skipped), the service (the room was made, or the clock was the reason it was not), hosting (np_net.h NatClass: 0 not measured, 1 no NAT, 2 cone,
// 3 symmetric) and the delay (the round trip to a partner, only when one is in the room: skipped otherwise). The answer is up to three plain sentences, and a one-line form of it
// goes to the diagnostics report (np_mpdiag.h). Nothing about the connection is sent anywhere by this.
struct TestFacts {
    int wifi;         // 1 connected, 0 not, -1 unknown
    int clockOk;      // 1 plausible, 0 wrong, -1 unknown
    bool roomMade;    // the service answered: the room exists
    bool clockFault;  // the room could not be made because the certificate dates failed (the console's date is far off)
    int nat;          // np_net.h NatClass
    int rtt;          // ms to a partner, -1 none
};
bool TestWifi(void* c, NpConnTest::Wifi* out) { const TestFacts* f = static_cast<TestFacts*>(c); if (f->wifi < 0) return false; out->connected = f->wifi != 0; out->quality = -1; return true; }
int TestClock(void* c) { return static_cast<TestFacts*>(c)->clockOk; }
bool TestInfoBegin(void* c, uint32_t) { const TestFacts* f = static_cast<TestFacts*>(c); return f->roomMade || f->clockFault; }
int TestInfoPoll(void* c, NpConnTest::Info* out) { const TestFacts* f = static_cast<TestFacts*>(c); out->outcome = f->clockFault ? NpConnTest::kInfoClock : NpConnTest::kInfoOk; out->ms = -1; return 1; }
bool TestHostBegin(void*) { return true; }
int TestHostPoll(void* c, int* nat) { *nat = static_cast<TestFacts*>(c)->nat; return 1; }
int TestRtt(void* c) { return static_cast<TestFacts*>(c)->rtt; }
uint64_t TestNow(void*) { return NowMs(); }

// ux13 (O8): MSC_WHO_CAN_I_PLAY_WITH=1 words the connection test's result as "Who can I play with?": three plain answers (this room, a friend far away by code, anyone online)
// built from the same five results, and the failure popup's second button says so. Default off until the console test; the checks and the diagnostics line are unchanged.
bool WhoCanIPlayWithOn() {
    const char* e = getenv("MSC_WHO_CAN_I_PLAY_WITH");
    return e && e[0] == '1';
}

// Title, then one paragraph per question: label and phrase translated piece by piece (np_text_strings.tsv), joined by the game's paragraph mark.
void WhoCanIPlayWithText(const uint8_t result[NpConnTest::kChecks], char* out, size_t cap) {
    if (!out || !cap) return;
    const int lang = NpText::CurrentLanguage();
    std::string text;
    char a[200], b[200];
    NpText::Sentences(lang, NpConnTest::WhoTitleText(), a, sizeof a);
    text = a;
    char log[120] = "";
    for (int who = 0; who < NpConnTest::kWhoCount; ++who) {
        const uint8_t phrase = NpConnTest::WhoAnswer(result, who);
        NpText::Sentences(lang, NpConnTest::WhoLabelText(who), a, sizeof a);
        NpText::Sentences(lang, NpConnTest::WhoPhraseText(phrase), b, sizeof b);
        text += "{p}";
        text += a;
        text += " ";
        text += b;
        snprintf(log + strlen(log), sizeof log - strlen(log), "%s%s", who ? " " : "", NpConnTest::WhoPhraseName(phrase));
    }
    MENU_LOG("who can I play with: %s\n", log);
    snprintf(out, cap, "%s", text.c_str());
}

std::string ConnectionTestResult(TestFacts facts) {
    NpConnTest::Probes p;
    p.ctx = &facts;
    p.now = TestNow;
    p.wifi = TestWifi;
    p.clock = TestClock;
    p.infoBegin = TestInfoBegin;
    p.infoPoll = TestInfoPoll;
    if (facts.roomMade) {
        p.hostBegin = TestHostBegin;
        p.hostPoll = TestHostPoll;
        p.rtt = TestRtt;
    }
    NpConnTest::Run run;
    run.Start(p);
    for (int i = 0; i < 16 && run.Running(); ++i) run.Pump();   // every probe answers at once: the checks follow each other within one frame
    const NpConnTest::View& v = run.GetView();
    char text[700], line[160];
    NpConnTest::VerdictText(v.verdict, text, sizeof text);
    if (WhoCanIPlayWithOn()) WhoCanIPlayWithText(v.result, text, sizeof text);   // ux13 (O8): the same results as three plain answers
    NpConnTest::ReportLine(v.result, v.detail, line, sizeof line);
    MENU_LOG("connection test: nat %d; %s\n", facts.nat, line);
    NpMpDiag::SetLastConnTest(line);
    NpMpDiag::Note(NpMpDiag::kConnTest, v.verdict.headline, 0, 0, NpConnTest::HeadlineName(v.verdict.headline));
    return text;
}

int StageFor(bool inRoom, int members, bool host) {
    if (!inRoom) return 1;
    if (host && members == 0) return 3;
    return 2;
}

void UpdateStatus(int stage, const char* invite) {
    char text[520];
    StatusText(stage, invite, text, sizeof text);
    if (strcmp(text, g_flow.lastStatus) == 0) return;
    snprintf(g_flow.lastStatus, sizeof g_flow.lastStatus, "%s", text);
    NpMenuUiSetText(text);
}

void OpenStatus(int stage, const char* invite) {
    char text[520];
    StatusText(stage, invite, text, sizeof text);
    if (NpMenuUiShow(text, "CANCEL", 0, 0, 0)) {
        snprintf(g_flow.lastStatus, sizeof g_flow.lastStatus, "%s", text);
        g_flow.popupWanted = false;
    }
}

// The reason a transport phase failed, as the player reads it. The transport's own sentences are plain.
std::string TransportFailure(int phase) {
    const char* e = NpNetError();
    if (e && *e) return e;
    if (phase == 7) return "No direct route to your partner was found. Try another Wi-Fi network.";
    if (phase == 9) return "The online link was lost.";
    return "The online connection failed. Check your Wi-Fi and try again.";
}

// Checks before anything is sent: a sentence when the console cannot reach a service at all.
bool Preflight(std::string* why, int* cause = nullptr) {   // cause: 1 no Wi-Fi, 2 the clock (the connection test words these through np_conntest.h)
    if (const char* t = getenv("MSC_NET_TEST_WIFI")) {   // tests: 0 = pretend Wi-Fi is off
        if (*t == '0') { *why = "Check your Wi-Fi: this Vita is not connected to a network."; if (cause) *cause = 1; return false; }
    } else if (NpPlatformWifiUp && NpPlatformWifiUp() == 0) {
        *why = "Check your Wi-Fi: this Vita is not connected to a network.";
        if (cause) *cause = 1;
        return false;
    }
    int clockOk = -1;
    if (const char* t = getenv("MSC_NET_TEST_CLOCK")) clockOk = *t == '0' ? 0 : 1;
    else if (np_https_clock_ok) clockOk = np_https_clock_ok();
    if (clockOk == 0) {
        *why = "Set your Vita's date and time (Settings > Date & Time), then try again.";
        if (cause) *cause = 2;
        return false;
    }
    if (ServiceUrls().empty()) {
        *why = "Online play is not available in this version of the game.";
        return false;
    }
    return true;
}

void WriteInviteFile(const char* invite) {
#if !defined(NP_TARGET_VITA)
    // desktop harness hand-over of the host's invite code to the joining process
    const char* file = getenv("MSC_NET_INVITE_FILE");
    if (!file || !*file || g_flow.inviteFileWritten || !invite[0]) return;
    if (FILE* f = fopen(file, "w")) {
        fputs(invite, f);
        fclose(f);
        g_flow.inviteFileWritten = true;
    }
#else
    (void)invite;
#endif
}

// The account exists: start the transport (host: create the room; join: with the typed code) and show the status popup.
void BeginConnect() {
    NpMpDiag::Reach(NpMpDiag::kStageSignaling);   // (mpbeta27) the room through the service
    const std::vector<std::string> urls = ServiceUrls();
    char build[24];
    snprintf(build, sizeof build, "%08x", NpLanFingerprint());
    NpNetConfigureService(Join(urls).c_str(), g_flow.account.token, AppVersion(), build);
    const int ok = g_flow.role != R_JOIN ? NpNetHost(g_flow.account.name) : NpNetJoin(g_flow.code, g_flow.account.name);
    MENU_LOG("%s started (%d)\n", g_flow.role == R_HOST ? "host" : g_flow.role == R_TEST ? "connection test" : "join", ok);
    if (!ok) {
        // The transport's own sentence, and the controller goes: one left behind in its failed phase kept answering the radio poll with "failed", so Nearby and
        // Same Wi-Fi stayed refused until the game was restarted (NpNetRadioPoll only steps aside when there is no session).
        const char* e = NpNetError();
        const std::string text = e && *e ? e : "The online connection could not be started. Try again.";
        NpNetLeave();
        Fail(text, S_IDLE);
        return;
    }
    Enter(S_CONNECT);
    g_flow.stage = 0;
    g_flow.lastStatus[0] = 0;
    g_flow.inviteFileWritten = false;
    g_flow.popupWanted = true;
}

// The account is known: join asks for the code first, host goes straight to the transport.
// ---- (mpbeta27) the beta notice, the offer, REPORT A PROBLEM ----------------------------------------------------------------------------------------------------------
void BetaLog() {
    const NpMpBeta::Counters& c = NpMpBeta::Count();
    MENU_LOG("[mpbeta] beta %s: tags %u notice %u stamp %u sync-text %u report-entry %u version-text %u\n", NpMpBeta::On() ? "on" : "off", c.tags, c.notice, c.stamp, c.syncText,
             c.reportEntry, c.versionText);
}

const char* kBetaSeenFile = "mp-beta-seen.txt";

bool BetaNoticeWanted() {
    const int mode = NpMpBeta::NoticeMode();
    if (mode == 0) return false;
    if (mode == 2) return true;
    char path[512];
    if (!PathFor(kBetaSeenFile, path, sizeof path)) return false;
    struct stat st;
    return stat(path, &st) != 0;
}

void BetaNoticeSeen() {
    char path[512];
    NpSafeFile::Options o;
    o.mode = 0644;
    o.maxBytes = 64;
    if (PathFor(kBetaSeenFile, path, sizeof path) && !NpSafeFile::Replace(path, std::string("1\n"), o)) MENU_LOG("the beta notice could not be marked as seen\n");
}

// After a session that ended with an error (a lost link, a desync, a failed connection, a lost ranked result, a crash inside a session in the last run) the menu offers SEND
// A REPORT: once for that session (np_mpdiag.h OfferWanted; marked as offered when shown). 1.3.0 offered it only with an online account; with MSC_MP_REPORT_PLUS (default)
// it is offered without one too, and SEND A REPORT then saves the report to a file instead (nothing can be sent without an account).
bool OfferDue() {
    if (!NpMpDiag::OfferWanted()) return false;
    NpMpReport::Account a;
    const bool account = NpMpReport::GetEnv().account && NpMpReport::GetEnv().account(&a) && a.Ready();
    if (!account && !NpMpDiag::ReportPlusOn()) return false;
    MENU_LOG("offering a problem report%s\n", account ? "" : " (no online account: it would be saved to a file)");
    return true;
}

std::string OfferText() {
    if (!NpMpDiag::ReportPlusOn()) return NpMpReport::OfferText();
    const NpMpDiag::Trail t = NpMpDiag::CurrentTrail();
    const NpMpDiag::SessionNumbers n = NpMpDiag::LastSession();
    return std::string(NpMpBeta::OfferLead(t.lastRunCrashed && !(n.valid && n.errored), n.valid && n.online)) + " " + NpMpBeta::OfferTail();
}

// SEND A REPORT (the offer) or REPORT A PROBLEM (the menu) was pressed: the consent popup when there is an account, else the report is saved to a file and the player is told.
void ReportPressed() {
    if (g_report.Ask(NpMpReport::Op::kReport)) { MENU_LOG("report: the player wants to see what is sent\n"); CloseThen(S_RPT_ASK, -1); return; }
    if (g_report.GetOutcome() == NpMpReport::Op::kNoAccount && NpMpDiag::ReportPlusOn()) {
        char path[300];
        const bool saved = NpMpDiag::SaveReport(nullptr, path, sizeof path);
        MENU_LOG("report: no online account, %s\n", saved ? "saved to a file instead" : "could not be saved");
        Notice(saved ? NpMpBeta::SavedInstead() : NpMpBeta::NotSaved(), S_MODE);
        return;
    }
    Notice(g_report.Sentence(), S_MODE);
}

void AccountReady() {
    MENU_LOG("account: name \"%s\", profile %d\n", g_flow.account.name, g_flow.account.profile);
    if (g_flow.role == R_JOIN) {
        g_flow.keyboardOpen = false;
        Enter(S_CODE_KBD);
    } else {
        BeginConnect();
    }
}

}  // namespace

// ---- C API ----------------------------------------------------------------------------------------------------------------------------------------------
extern "C" int NpMenuWanted(void) {
    const char* netMode = getenv("MSC_NET_MODE");
    return netMode && *netMode ? 0 : 1;   // MSC_NET_MODE=host|join is the desktop tests' older shortcut: it skips the menus
}

extern "C" int NpMenuLinkMode(void) {
    if (g_linkMode >= 0) return g_linkMode;
    const char* mode = getenv("MSC_LAN_MODE");
    if (mode && strcmp(mode, "online") == 0) return kNpMenuOnline;
    return mode && strcmp(mode, "wifi") == 0 ? kNpMenuWifi : kNpMenuNearby;
}

extern "C" int NpMenuActive(void) { return g_flow.state != S_IDLE ? 1 : 0; }

extern "C" void NpMenuShowFailure(const char* text) {
    if (g_flow.state == S_IDLE) Fail(text ? text : "", S_IDLE);
}

extern "C" const char* NpMenuPlayerName(void) { return g_flow.account.name; }

// 1.3: FRIENDS / PLAY WITH ANYONE / RANKED MATCH as the online choice. On when the game can do all of it (the friend calls and the unranked queue of the service client
// say so through NpOnlineFriendsAvailable); MSC_ONLINE_FRIENDS=1 / 0 forces it (tests, the picture runs); off, the choice is HOST A ROOM / JOIN WITH A CODE / RANKED MATCH.
extern "C" int NpOnlineFriendsAvailable(void) __attribute__((weak));
extern "C" int NpMenuFriendsOffered(void) {
    const char* e = getenv("MSC_ONLINE_FRIENDS");
    if (e && *e) return e[0] != '0' ? 1 : 0;
    return NpOnlineFriendsAvailable && NpOnlineFriendsAvailable() ? 1 : 0;
}
extern "C" int NpMenuQuickWanted(void) { return g_quick ? 1 : 0; }

// 1.3: the ranked entry of the online choice (Online Central: ranked matchmaking and the ranking tables). MSC_ONLINE_RANKED=0 hides it.
extern "C" int NpMenuRankedOffered(void) {
    const char* e = getenv("MSC_ONLINE_RANKED");
    return !(e && e[0] == '0') && NpOnlineCentralAvailable ? 1 : 0;
}

extern "C" void NpMenuBegin(void) {
    if (g_flow.state != S_IDLE) return;
    g_flow.pendingResult = -1;
    g_flow.keyboardTries = 0;
    g_flow.keyboardOpen = false;
    g_flow.choiceShown = false;
    g_flow.baseTry = 0;
    g_flow.code[0] = 0;
    g_flow.typedCode[0] = 0;
    g_flow.typedName[0] = 0;
    g_flow.friendsLevel = false;
    g_quick = false;
    NpMpReport::UseNetwork();
    // (mpbeta27) the session trail starts again for this visit; a session the last run did not leave (a crash, a power cut) is found once per process
    NpMpDiag::BeginTrail();
    NpMpDiag::Reach(NpMpDiag::kStageMenu);
    static bool markChecked = false;
    if (!markChecked) {
        markChecked = true;
        if (NpMpDiag::SessionMarkCheckAtStart(nullptr)) MENU_LOG("the last run ended inside a multiplayer session: a report will be offered\n");
    }
    BetaLog();
    // (mpbeta27) the first visit: the beta notice, then the offer (if any), then the mode choice
    if (BetaNoticeWanted()) {
        MENU_LOG("multiplayer beta notice\n");
        Enter(S_BETA_NOTICE);
        return;
    }
    if (OfferDue()) {
        Enter(S_RPT_OFFER);
        return;
    }
    MENU_LOG("multiplayer mode choice\n");
    Enter(S_MODE);
}

extern "C" int NpMenuStep(void) {
    Flow& f = g_flow;
    const uint64_t now = NowMs();
    switch (f.state) {
    case S_IDLE: return kNpMenuDone;

    case S_MODE:
    case S_LOCAL: {
        const bool local = f.state == S_LOCAL;
        if (!f.choiceShown) {
            if (!NpMenuUiSceneReady(1)) return kNpMenuBusy;
            const int preferred = NpMenuLinkMode();
            // (mpbeta27) the entry points carry the BETA tag (np_mpbeta.h); the mode choice's third button is REPORT A PROBLEM (Circle still goes back)
            const bool entry = !local && NpMpBeta::ReportEntryOn();
            const int shown = local
                ? NpMenuUiShow("Nearby: both Vitas close together, no network needed. Same Wi-Fi: both Vitas on one Wi-Fi network. Circle goes back.", NpMpBeta::Tag("NEARBY"),
                               NpMpBeta::Tag("SAME WI-FI"), "BACK", preferred == kNpMenuWifi ? 1 : 0)
                : NpMenuUiShow("Online: play over the internet. Local: play with a Vita next to you. Circle goes back.", NpMpBeta::Tag("ONLINE"), NpMpBeta::Tag("LOCAL"),
                               entry ? NpMpBeta::ReportButton() : "CANCEL", preferred == kNpMenuOnline ? 0 : 1);
            if (shown) {
                f.choiceShown = true;
                if (entry) ++NpMpBeta::Count().reportEntry;
            }
            return kNpMenuBusy;
        }
        const int r = NpMenuUiPoll();
        const bool back = NpMenuUiBack();
        if (r == -1 && !back) return kNpMenuBusy;
        if (!back && !local && r == 2 && NpMpBeta::ReportEntryOn()) {   // (mpbeta27) REPORT A PROBLEM: the same consent popup as the offer (or a file without an account)
            MENU_LOG("report a problem chosen\n");
            ReportPressed();
            return kNpMenuBusy;
        }
        if (!back && (r == 0 || r == 1)) {
            if (local) {
                f.localPick = r == 0 ? kNpMenuNearby : kNpMenuWifi;
                MENU_LOG("chose %s\n", r == 0 ? "nearby" : "same wi-fi");
                NpMpDiag::SetMode(r == 0 ? NpMpDiag::kLinkNearby : NpMpDiag::kLinkSameWifi, NpMpDiag::kPlayLocal);   // (mpbeta27) the report's mode line
                CloseThen(S_IDLE, kNpMenuLocal);
            } else {
                MENU_LOG("%s selected\n", r == 0 ? "online" : "local");
                CloseThen(r == 0 ? (NpMenuFriendsOffered() ? S_ONLINE : S_CHOOSE) : S_LOCAL, -1);
            }
        } else if (local && (back || r == 2)) {
            CloseThen(S_MODE, -1);
        } else {
            MENU_LOG("cancelled at the mode choice\n");
            CloseThen(S_IDLE, kNpMenuDone);
        }
        return kNpMenuBusy;
    }

    case S_ONLINE: {
        if (!f.choiceShown) {
            if (!NpMenuUiSceneReady(1)) return kNpMenuBusy;
            f.friendsLevel = false;
            const bool ranked = NpMenuRankedOffered();
            if (NpMenuUiShow("Friends: play with someone you know. Play with anyone: a quick match that does not count on the leaderboards. "
                             "Ranked match: counts on the leaderboards. Circle goes back.",
                             NpMpBeta::Tag("FRIENDS"), NpMpBeta::Tag("PLAY WITH ANYONE"), ranked ? NpMpBeta::Tag("RANKED MATCH") : "BACK", 0))
                f.choiceShown = true;
            return kNpMenuBusy;
        }
        const int r = NpMenuUiPoll();
        if (r == -1 && !NpMenuUiBack()) return kNpMenuBusy;
        if (!NpMenuUiBack() && r == 0) { MENU_LOG("chose friends\n"); f.friendsLevel = true; CloseThen(S_CHOOSE, -1); return kNpMenuBusy; }
        if (!NpMenuUiBack() && r == 1) {
            MENU_LOG("chose anyone (unranked quick match): Online Central\n");
            g_quick = true;
            NpMpDiag::SetMode(NpMpDiag::kLinkOnline, NpMpDiag::kPlayQuick);
            CloseThen(S_IDLE, kNpMenuQuick);
            return kNpMenuBusy;
        }
        if (!NpMenuUiBack() && r == 2 && NpMenuRankedOffered()) {
            MENU_LOG("chose ranked: Online Central\n");
            g_quick = false;
            NpMpDiag::SetMode(NpMpDiag::kLinkOnline, NpMpDiag::kPlayRanked);
            CloseThen(S_IDLE, kNpMenuRanked);
            return kNpMenuBusy;
        }
        MENU_LOG("cancelled at the online choice\n");
        CloseThen(S_IDLE, kNpMenuDone);
        return kNpMenuBusy;
    }

    case S_CHOOSE: {
        if (f.friendsLevel) {
            if (!f.choiceShown) {
                if (!NpMenuUiSceneReady(1)) return kNpMenuBusy;
                if (NpMenuUiShow("Friend list: your friends, their invitations and who you played. Host a room: you get a code to give to a friend. Join with a code: type the code a friend gave you. "
                                 "Circle goes back.",
                                 "FRIEND LIST", "HOST A ROOM", "JOIN WITH A CODE", 0))
                    f.choiceShown = true;
                return kNpMenuBusy;
            }
            const int r = NpMenuUiPoll();
            if (r == -1 && !NpMenuUiBack()) return kNpMenuBusy;
            if (!NpMenuUiBack() && r == 0) {
                MENU_LOG("chose friend list: Online Central\n");
                g_quick = false;
                NpMpDiag::SetMode(NpMpDiag::kLinkOnline, NpMpDiag::kPlayFriends);
                CloseThen(S_IDLE, kNpMenuFriends);
                return kNpMenuBusy;
            }
            if (!NpMenuUiBack() && (r == 1 || r == 2)) {
                f.role = r == 1 ? R_HOST : R_JOIN;
                NpMpDiag::SetMode(NpMpDiag::kLinkOnline, r == 1 ? NpMpDiag::kPlayRoomHost : NpMpDiag::kPlayRoomJoin);
                MENU_LOG("chose %s\n", r == 1 ? "host" : "join");
                CloseThen(S_ACCOUNT, -1);
                return kNpMenuBusy;
            }
            MENU_LOG("back from the friends choice\n");
            CloseThen(S_ONLINE, -1);   // Circle goes back one step, to the online choice
            return kNpMenuBusy;
        }
        if (!f.choiceShown) {
            if (!NpMenuUiSceneReady(1)) return kNpMenuBusy;   // the main menu is the scene on top (another popup of the game is answered first)
            // 1.3: RANKED MATCH (MSC_ONLINE_RANKED, on by default) takes the third button; Circle still cancels, as it always did
            const bool ranked = NpMenuRankedOffered();
            if (ranked ? NpMenuUiShow("Play a friend: one of you hosts a room and shares its code, the other joins with it (not ranked). RANKED MATCH plays "
                                      "anyone online and counts on the season and daily leaderboards. Circle goes back.",
                                      "HOST A ROOM", "JOIN WITH A CODE", NpMpBeta::Tag("RANKED MATCH"), 0)
                       : NpMenuUiShow("Play online with a friend. One of you hosts a room and shares its code, the other joins with it.", "HOST A ROOM", "JOIN WITH A CODE", "CANCEL", 0))
                f.choiceShown = true;
            return kNpMenuBusy;
        }
        const int r = NpMenuUiPoll();
        if (r == -1 && !NpMenuUiBack()) return kNpMenuBusy;   // Circle is CANCEL
        if (r == 0 || r == 1) {
            f.role = r == 0 ? R_HOST : R_JOIN;
            MENU_LOG("chose %s\n", r == 0 ? "host" : "join");
            NpMpDiag::SetMode(NpMpDiag::kLinkOnline, r == 0 ? NpMpDiag::kPlayRoomHost : NpMpDiag::kPlayRoomJoin);
            CloseThen(S_ACCOUNT, -1);
            return kNpMenuBusy;
        }
        if (r == 2 && NpMenuRankedOffered() && !NpMenuUiBack()) {
            MENU_LOG("chose ranked: Online Central\n");
            NpMpDiag::SetMode(NpMpDiag::kLinkOnline, NpMpDiag::kPlayRanked);
            CloseThen(S_IDLE, kNpMenuRanked);
            return kNpMenuBusy;
        }
        MENU_LOG("cancelled at the choice\n");
        CloseThen(S_IDLE, kNpMenuDone);
        return kNpMenuBusy;
    }

    case S_RPT_OFFER:
    case S_RPT_ASK: {
        const bool offer = f.state == S_RPT_OFFER;
        if (!f.choiceShown) {
            if (!NpMenuUiSceneReady(1)) return kNpMenuBusy;
            const std::string offerText = offer ? OfferText() : std::string();
            if (NpMenuUiShow(offer ? offerText.c_str() : NpMpReport::ConsentText(), offer ? "SEND A REPORT" : "SEND", offer ? "NOT NOW" : "CANCEL", 0, offer ? 0 : 1)) {
                f.choiceShown = true;
                if (offer) NpMpDiag::MarkOffered();   // shown once: it is never offered again for this session, whatever the player answers
            }
            return kNpMenuBusy;
        }
        const int r = NpMenuUiPoll();
        const bool back = NpMenuUiBack();
        if (r == -1 && !back) return kNpMenuBusy;
        if (!back && r == 0) {
            if (offer) {
                ReportPressed();
                return kNpMenuBusy;
            }
            g_report.Consent();   // SEND on the consent popup: the only place a report is started from
            if (g_report.Start() && g_report.GetPhase() == NpMpReport::Op::kSending) { MENU_LOG("report: sending\n"); f.popupWanted = true; CloseThen(S_RPT_SEND, -1); }
            else Notice(g_report.Sentence(), S_MODE);
            return kNpMenuBusy;
        }
        MENU_LOG("%s\n", offer ? "report: not now" : "report: cancelled at the consent screen");
        g_report.Cancel();
        CloseThen(S_MODE, -1);
        return kNpMenuBusy;
    }

    case S_RPT_SEND: {
        if (f.popupWanted) {
            if (!NpMenuUiOpen() && NpMenuUiShow(NpMpReport::SendingText(NpMpReport::Op::kReport), "CANCEL", 0, 0, 0)) f.popupWanted = false;
            return kNpMenuBusy;
        }
        const int ui = NpMenuUiPoll();
        if (NpMenuUiBack() || ui == 0 || ui == -2) {
            MENU_LOG("report: cancelled while sending\n");
            g_report.Cancel();
            CloseThen(S_MODE, -1);
            return kNpMenuBusy;
        }
        if (g_report.Step() == NpMpReport::Op::kDone) {
            MENU_LOG("report: done (outcome %d)\n", int(g_report.GetOutcome()));
            Notice(g_report.Sentence(), S_MODE);
        }
        return kNpMenuBusy;
    }

    case S_BETA_NOTICE: {   // (mpbeta27) once: the beta notice with OK; then the offer (if any) or the mode choice
        if (!f.choiceShown) {
            if (!NpMenuUiSceneReady(1)) return kNpMenuBusy;
            if (NpMenuUiShow(NpMpBeta::NoticeText(), "OK", 0, 0, 0)) {
                f.choiceShown = true;
                ++NpMpBeta::Count().notice;
                BetaNoticeSeen();   // shown is seen: it never comes again, whatever is pressed
            }
            return kNpMenuBusy;
        }
        const int r = NpMenuUiPoll();
        if (r == -1 && !NpMenuUiBack()) return kNpMenuBusy;
        CloseThen(OfferDue() ? S_RPT_OFFER : S_MODE, -1);
        return kNpMenuBusy;
    }

    case S_CLOSING:
        if (NpMenuUiOpen()) { NpMenuUiPoll(); return kNpMenuBusy; }
        if (f.settle > 0) { --f.settle; return kNpMenuBusy; }
        if (f.pendingResult >= 0) {
            const int r = f.pendingResult;
            f.pendingResult = -1;
            f.state = S_IDLE;
            if (r == kNpMenuLocal) g_linkMode = f.localPick;
            else if (r == kNpMenuEnter) g_linkMode = kNpMenuOnline;
            return r;
        }
        Enter(f.after);
        return kNpMenuBusy;

    case S_ACCOUNT: {
        if (f.role != R_TEST) NpMpDiag::Reach(NpMpDiag::kStageLogin);   // (mpbeta27) the account / sign-in
        std::string why;
        int cause = 0;
        if (!Preflight(&why, &cause)) {
            if (f.role == R_TEST && cause) why = ConnectionTestResult(TestFacts{cause == 1 ? 0 : 1, cause == 2 ? 0 : 1, false, false, 0, -1});   // the test says it with its plain sentences
            Fail(why, S_IDLE);
            return kNpMenuBusy;
        }
        f.account = Account();
        LoadAccount(&f.account);   // one disk read in this frame
        if (f.account.Ready()) { AccountReady(); return kNpMenuBusy; }
        if (f.account.name[0]) {   // a name from the environment or an earlier run, but no token yet: register it without asking
            f.baseTry = 0;
            Enter(S_REGISTER);
            f.stage = 4;
            f.popupWanted = true;
            if (!StartRegister()) Fail(f.failText.empty() ? "The online account could not be created." : f.failText, S_IDLE);
            return kNpMenuBusy;
        }
        f.keyboardOpen = false;
        Enter(S_NAME_KBD);
        return kNpMenuBusy;
    }

    case S_NAME_KBD: {
        if (!f.keyboardOpen) {
            char initial[16] = "";
            if (f.typedName[0]) snprintf(initial, sizeof initial, "%s", f.typedName);   // what was typed before a refusal: never typed again from nothing
            else SanitizeName(NpKeyboardSuggestion(), initial);
            if (NpKeyboardOpen(NpText::Tr("Choose a name for online play (2-10 characters)"), initial, int(kMaxName), kNpKeyboardName) != 0) {
                Fail("This build has no keyboard to type a name.", S_IDLE);
                return kNpMenuBusy;
            }
            f.keyboardOpen = true;
        }
        char text[96];
        const int r = NpKeyboardPoll(text, sizeof text);
        if (r == kNpKeyboardRunning) return kNpMenuBusy;
        f.keyboardOpen = false;
        if (r != kNpKeyboardAccepted) {
            MENU_LOG("name keyboard %s\n", r == kNpKeyboardCancelled ? "cancelled" : "failed");
            CloseThen(S_IDLE, kNpMenuDone);
            return kNpMenuBusy;
        }
        char clean[kMaxName + 1];
        snprintf(f.typedName, sizeof f.typedName, "%.*s", int(kMaxName), text);
        if (!SanitizeName(text, clean)) {
            Fail("Use 2 to 10 letters or digits for the name.", ++f.keyboardTries >= 3 ? S_IDLE : S_NAME_KBD);
            return kNpMenuBusy;
        }
        snprintf(f.account.name, sizeof f.account.name, "%s", clean);
        f.baseTry = 0;
        Enter(S_REGISTER);
        f.stage = 4;
        f.popupWanted = true;
        if (!StartRegister()) Fail(f.failText.empty() ? "The online account could not be created." : f.failText, S_IDLE);
        return kNpMenuBusy;
    }

    case S_REGISTER: {
        if (f.popupWanted) {   // the status popup (cancellable) while the request runs
            if (!NpMenuUiOpen()) OpenStatus(4, "");
            return kNpMenuBusy;
        }
        const int ui = NpMenuUiPoll();
        if (NpMenuUiBack() || ui == 0 || ui == -2) {
            MENU_LOG("cancelled while creating the account\n");
            NpNet::HttpCancel(f.httpHandle);
            f.httpHandle = 0;
            CloseThen(S_IDLE, kNpMenuDone);
            return kNpMenuBusy;
        }
        if (now - f.stageAt > kRegisterLimitMs) {
            NpNet::HttpCancel(f.httpHandle);
            f.httpHandle = 0;
            Fail("The online service did not answer. Check your Wi-Fi and try again.", S_IDLE, true);
            return kNpMenuBusy;
        }
        NpNet::HttpResponse r;
        const int rc = NpNet::HttpPoll(f.httpHandle, &r);
        if (rc == 0) return kNpMenuBusy;
        f.httpHandle = 0;
        if (rc < 0) { Fail("The online account could not be created.", S_IDLE); return kNpMenuBusy; }
        if (r.error != 0) {
            // no answer from this address: the next of the list, else the transport's own sentence (Wi-Fi, date and time, ...)
            ++f.baseTry;
            if (StartRegister()) return kNpMenuBusy;
            Fail(r.text.empty() ? "The online service could not be reached. Check your Wi-Fi." : r.text, S_IDLE, true);
            return kNpMenuBusy;
        }
        NpNet::Json j;
        const bool parsed = j.Parse(r.body.data(), r.body.size());
        if (r.status >= 200 && r.status < 300 && parsed) {
            const std::string token = j.GetStr(j.Root(), "token");
            const int pid = int(j.GetNum(j.Root(), "pid", 0));
            if (TokenValid(token.c_str()) && pid > 0) {
                f.account.profile = pid;
                snprintf(f.account.token, sizeof f.account.token, "%s", token.c_str());
                if (!SaveName(f.account.name) || !SaveToken(pid, f.account.token)) MENU_LOG("the account could not be stored; it is created again next time\n");
                NpMenuUiClose();
                f.popupWanted = false;
                f.stage = 0;
                AccountReady();   // join: the code keyboard; host: the transport (the account popup is replaced by the status popup)
                return kNpMenuBusy;
            }
            Fail("The online service's answer was not understood.", S_IDLE);
            return kNpMenuBusy;
        }
        const int e = j.Get(j.Root(), "error");
        const std::string code = j.GetStr(e, "code");
        const std::string text = ServiceSentence(code, j.GetStr(e, "message"), j.GetStr(e, "reason"), r.status, j.GetStr(e, "latestVersion"));
        f.keyboardOpen = false;
        Fail(text, (code == "name_taken" || code == "bad_name") && !getenv("MSC_ONLINE_NAME") && f.keyboardTries++ < 3 ? S_NAME_KBD : S_IDLE,
             r.status >= 500 || code == "busy" || code == "server_full");
        return kNpMenuBusy;
    }

    case S_CODE_KBD: {
        if (!f.keyboardOpen) {
            if (NpKeyboardOpen(NpText::Tr("Enter the invite code of the room"), f.typedCode, 8, kNpKeyboardCode) != 0) {
                Fail("This build has no keyboard to type the code.", S_IDLE);
                return kNpMenuBusy;
            }
            f.keyboardOpen = true;
        }
        char text[96];
        const int r = NpKeyboardPoll(text, sizeof text);
        if (r == kNpKeyboardRunning) return kNpMenuBusy;
        f.keyboardOpen = false;
        if (r != kNpKeyboardAccepted) {
            MENU_LOG("code keyboard %s\n", r == kNpKeyboardCancelled ? "cancelled" : "failed");
            CloseThen(S_IDLE, kNpMenuDone);
            return kNpMenuBusy;
        }
        snprintf(f.typedCode, sizeof f.typedCode, "%.8s", text);
        if (!NormalizeCode(text, f.code, sizeof f.code)) {
            Fail("That is not an invite code. A code has 6 letters and digits. It never has the letters O, I or L, or the digits 0 or 1.", S_CODE_KBD);
            return kNpMenuBusy;
        }
        MENU_LOG("invite code typed\n");
        BeginConnect();
        return kNpMenuBusy;
    }

    case S_CONNECT: {
        NpNetPoll();   // the transport (a bounded number of packets) and the signaling's replies
        const int phase = NpNetPhase();
        const char* invite = NpNetInviteCode();
        if (f.role == R_HOST) WriteInviteFile(invite);
        const bool inRoom = f.role != R_JOIN ? invite[0] != 0 : NpNetMemberCount() > 0;
        const int stage = StageFor(inRoom, NpNetMemberCount(), f.role != R_JOIN);
        if (stage != f.stage) {
            f.stage = stage;
            f.stageAt = now;
            if (stage == 2 && f.role != R_TEST) NpMpDiag::Reach(NpMpDiag::kStageConnect);   // (mpbeta27) "Finding a route...": the partner is in the room
        }
        if (phase == 8 || phase == 7 || phase == 9 || phase == 0) {   // failed, no direct route, lost, or gone
            std::string text = phase == 0 ? std::string("The online connection ended.") : TransportFailure(phase);
            if (f.role == R_TEST && strstr(text.c_str(), "date and time")) text = ConnectionTestResult(TestFacts{1, 1, false, true, 0, -1});   // the certificate dates failed: a clock far off (a year breaks TLS)
            NpNetLeave();
            Fail(text, f.role == R_JOIN && phase != 0 && phase != 9 ? S_CODE_KBD : S_IDLE, phase == 7 || (phase == 8 && f.role != R_JOIN));   // (a join that ends in phase 8 is mostly a wrong or closed room: the test would not explain it)   // a join that failed asks for the code again, with the typed text in it
            return kNpMenuBusy;
        }
        if (f.popupWanted) {
            if (!NpMenuUiOpen()) OpenStatus(f.stage ? f.stage : 1, invite);
            return kNpMenuBusy;
        }
        if (f.role == R_TEST && phase >= 4 && phase != 7 && phase != 8 && phase != 9) {   // the room exists: the service and the STUN servers answered
            const int nat = NpNetNat();
            const int rtt = NpNetRttMs();   // only a partner in the room gives one: -1 here, and the delay check says nothing then
            const std::string answer = ConnectionTestResult(TestFacts{1, 1, true, false, nat, rtt});
            NpNetLeave();
            Fail(answer, S_IDLE);
            return kNpMenuBusy;
        }
        if (phase == 6) {   // READY: a member is connected
            MENU_LOG("link up\n");
            CloseThen(S_IDLE, kNpMenuEnter);
            return kNpMenuBusy;
        }
        UpdateStatus(f.stage ? f.stage : 1, invite);
        const int ui = NpMenuUiPoll();
        if (NpMenuUiBack() || ui == 0 || ui == -2) {
            MENU_LOG("cancelled while connecting (stage %d)\n", f.stage);
            NpNetLeave();
            CloseThen(S_IDLE, kNpMenuDone);
            return kNpMenuBusy;
        }
        // the limits of the stages, on the game clock
        const uint64_t inStage = now - f.stageAt;
        if ((f.stage <= 1 && inStage > kServiceLimitMs) || (f.stage == 2 && inStage > kRouteLimitMs) || (f.stage == 3 && inStage > kPartnerLimitMs)) {
            const char* text = f.stage == 3 ? "Nobody joined the room in 10 minutes. Try again, and share the code again."
                             : f.stage == 2 ? "No route to your partner was found in time. Try again."
                                            : "The online service did not answer. Check your Wi-Fi and try again.";
            NpNetLeave();
            Fail(text, f.role == R_JOIN ? S_CODE_KBD : S_IDLE, f.stage != 3);
        }
        return kNpMenuBusy;
    }

    case S_FAIL: {
        if (f.popupWanted) {
            if (NpMenuUiOpen()) return kNpMenuBusy;
            if (f.settle > 0) { --f.settle; return kNpMenuBusy; }
            if (NpMenuUiShow(f.failText.c_str(), "OK", f.offerTest ? (WhoCanIPlayWithOn() ? "WHO CAN I PLAY WITH?" : "TEST CONNECTION") : 0, 0, 0)) f.popupWanted = false;
            return kNpMenuBusy;
        }
        const int r = NpMenuUiPoll();
        if (r == -1 && !NpMenuUiBack()) return kNpMenuBusy;
        if (r == 1 && f.offerTest && !NpMenuUiBack()) {   // TEST CONNECTION: the account is known (or made), a room is made and left at once
            MENU_LOG("connection test chosen\n");
            f.role = R_TEST;
            f.offerTest = false;
            f.baseTry = 0;
            f.typedCode[0] = 0;
            CloseThen(S_ACCOUNT, -1);
            return kNpMenuBusy;
        }
        if (f.failNext == S_IDLE) CloseThen(S_IDLE, kNpMenuDone);
        else CloseThen(f.failNext, -1);
        return kNpMenuBusy;
    }
    }
    return kNpMenuBusy;
}


// ---- the account for SEND A REPORT / DELETE MY ONLINE ACCOUNT (np_mpreport_net.cpp) --------------------------------------------------------------------------------
extern "C" int NpMenuReportAccount(char* name, size_t nameSize, char* token, size_t tokenSize, char* urls, size_t urlsSize) {
    Account a;
    LoadAccount(&a);
    if (name && nameSize) snprintf(name, nameSize, "%s", a.name);
    if (token && tokenSize) snprintf(token, tokenSize, "%s", a.token);
    if (urls && urlsSize) snprintf(urls, urlsSize, "%s", Join(ServiceUrls()).c_str());
    const bool ready = a.token[0] && a.profile > 0;
    memset(a.token, 0, sizeof a.token);
    return ready ? 1 : 0;
}

// Forget the local online identity: the token, the name, the code and the retry queue, every copy of each (np_safe_file.h RemoveAll), and this flow's memory of them.
// (MSC_ONLINE_NAME in np-player.txt or the environment is the player's own setting and stays; it only suggests the name for the next account.)
extern "C" void NpMenuForgetAccount(void) {
    char path[512];
    static const char* const kFiles[] = {"online.dat", "online_name.txt", "online_code.txt"};
    for (size_t i = 0; i < sizeof kFiles / sizeof kFiles[0]; ++i)
        if (PathFor(kFiles[i], path, sizeof path)) NpSafeFile::RemoveAll(path);
    g_flow.account = Account();
    if (NpOnlineForgetAccount) NpOnlineForgetAccount();
    MENU_LOG("the online identity of this Vita was forgotten\n");
}

extern "C" int NpMenuConnectionLostText(char* out, size_t size) {
    if (NpMpBeta::On() && NpLanPartnerRefused && NpLanPartnerRefused()) {   // (mpbeta27) a console with another build: the link refused every frame it sent
        ++NpMpBeta::Count().versionText;
        snprintf(out, size, "%s", NpMpBeta::OtherBuildText());
        return 1;
    }
    if (!NpNetActive()) { g_endReasonKeep[0] = 0; return 0; }   // Nearby / Same Wi-Fi: the game's own text
    const char* why = NpNetEndReason();
    if (!why || !*why) why = g_endReasonKeep;
    if (why && *why) snprintf(g_endReasonKeep, sizeof g_endReasonKeep, "%s", why);
    snprintf(out, size, "%s A match cannot be picked up again after a break, so it ends here.", why && *why ? why : "The online link ended.");
    return 1;
}

extern "C" void NpMenuLinkLostNote(void) {
    if (!NpNetActive()) return;
    g_lost.offer = true;
    g_lost.at = NowMs();
    g_lost.step = 0;
    const char* why = NpNetEndReason();
    snprintf(g_lost.reason, sizeof g_lost.reason, "%s", why && *why ? why : "The online link ended.");
    snprintf(g_endReasonKeep, sizeof g_endReasonKeep, "%s", g_lost.reason);
}

extern "C" int NpMenuLobbyStep(void) {
    Lost& l = g_lost;
#if !defined(NP_TARGET_VITA)
    // The Vita drives the transport once per frame whatever the game does (np_vita_lan_frame). The desktop build only pumps it while a game session or the menu
    // flow asks for it, so a restored lobby (the link is back, no session yet) would go quiet and the peers would declare each other lost again: pump it here.
    if (NpNetActive()) NpNetPoll();
#endif
    if (!l.offer) return 0;
    if (!NpNetActive()) { l.offer = false; return 0; }
    const uint64_t now = NowMs();
    switch (l.step) {
    case 0: {
        if (NpMenuUiOpen()) return 0;                       // the game's own popup first
        char text[240];
        snprintf(text, sizeof text, "%s Reconnect to the same room?", l.reason);
        if (NpMenuUiShow(text, "RECONNECT", "LEAVE", 0, 0)) l.step = 1;
        return 0;
    }
    case 1: {
        if (now - l.at > kResumeWindowMs) {
            NpMenuUiClose();
            l.failText = "Too much time has passed: the room cannot be rejoined.";
            l.step = 3;
            return 0;
        }
        const int r = NpMenuUiPoll();
        if (r == -1) return 0;
        if (r == 0) {
            // phase 9: the link is still down, rebuild it now; any other phase: the game's lobby restart already started the rebuild (OnLinkStart)
            if (NpNetPhase() == 9 && !NpNetReconnect()) {
                l.failText = "The connection could not be restored.";
                l.step = 3;
                return 0;
            }
            MENU_LOG("reconnecting\n");
            l.at = now;
            l.step = 2;
            return 0;
        }
        l.offer = false;
        l.step = 0;
        return 1;   // LEAVE (or the popup went away)
    }
    case 2: {
        NpNetPoll();
        const int phase = NpNetPhase();
        if (!NpMenuUiOpen()) {
            NpMenuUiShow("Reconnecting to the same room... Press Circle to cancel.", "CANCEL", 0, 0, 0);
            return 0;
        }
        const int ui = NpMenuUiPoll();
        if (phase == 6) {
            NpMenuUiClose();
            if (NpLanLinkRestored) NpLanLinkRestored();
            MENU_LOG("reconnected\n");
            l.step = 4;   // the lobby is entered again once the status popup is gone
            return 0;
        }
        if (phase == 8 || phase == 7 || phase == 0 || NpMenuUiBack() || ui == 0 || now - l.at > kReconnectLimitMs) {
            const char* e = NpNetError();
            l.failText = (e && *e) ? e : "The connection could not be restored.";
            NpMenuUiClose();
            l.failShown = false;
            l.step = 3;
        }
        return 0;
    }
    case 3: {
        if (!l.failShown) {
            if (NpMenuUiOpen()) return 0;   // the status popup is still going away
            if (NpMenuUiShow(l.failText.c_str(), "OK", 0, 0, 0)) {
                l.failShown = true;
                MENU_LOG("lobby: reconnect failed: %s\n", l.failText.c_str());
            }
            return 0;
        }
        const int r = NpMenuUiPoll();
        if (r == -1 && !NpMenuUiBack()) return 0;
        NpMenuUiClose();
        l.offer = false;
        l.step = 0;
        l.failShown = false;
        return 1;
    }
    case 4: {
        if (NpMenuUiOpen()) return 0;
        l.offer = false;
        l.step = 0;
        return 2;   // the link is back: the lobby is entered again
    }
    }
    return 0;
}
