// Host test of the HTTP client (platform/sdk/dwc/np_online_http.cpp) and the DWC calls on top of it, against the service's local fake
// (server/fake/server.mjs, the same code as the Worker). Needs node; tools/dwc/run_dwc_http_test.sh starts the fakes and passes their URLs.
// Plain HTTP on the loopback only (the TLS layer has its own tests).
#include <dwc/dwc_account.h>
#include <dwc/dwc_error.h>
#include <dwc/dwc_init_fwd.h>
#include <dwc/dwc_main_fwd.h>
#include <dwc/dwc_nastime.h>
#include <dwc/dwc_ranking.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <map>
#include <string>
#include <vector>

#include "platform/sdk/dwc/np_dwc.h"
#include "platform/sdk/net/online/np_service_busy.h"
#include "platform/sdk/dwc/np_online_fake.h"
#include "platform/sdk/dwc/np_online_http.h"
#include "platform/sdk/dwc/np_online_json.h"
#include "platform/sdk/dwc/np_online_name.h"
#include "platform/sdk/dwc/np_online_queue.h"

using namespace NpOnline;

static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
static NpOnline::Row g_emptyRow;
#define ROWAT(reply, i) ((reply).rows.size() > (size_t)(i) ? (reply).rows[i] : g_emptyRow)
#define CHECK_EQ(a, b) do { ++g_checks; const long long va = (long long)(a), vb = (long long)(b); \
    if (va != vb) { printf("FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, va, vb); ++g_fail; } } while (0)

// ---- a blocking-socket transport, one thread per request --------------------------------------------------------------------------
class SockTransport : public HttpTransport {
public:
    explicit SockTransport(const std::string& base) {
        const size_t colon = base.rfind(':');
        m_host = base.substr(7, colon - 7);   // "http://127.0.0.1:PORT"
        m_port = atoi(base.c_str() + colon + 1);
        pthread_mutex_init(&m_lock, nullptr);
    }
    ~SockTransport() override {
        for (std::map<uint32_t, Slot*>::iterator it = m_slots.begin(); it != m_slots.end(); ++it) { pthread_join(it->second->th, nullptr); delete it->second; }
    }
    uint32_t Start(const HttpRequest& request) override {
        Slot* s = new Slot();
        s->self = this;
        s->req = request;
        s->done = false;
        pthread_mutex_lock(&m_lock);
        const uint32_t id = ++m_next;
        m_slots[id] = s;
        pthread_mutex_unlock(&m_lock);
        pthread_create(&s->th, nullptr, &Run, s);
        return id;
    }
    bool Poll(uint32_t id, HttpResponse* out) override {
        pthread_mutex_lock(&m_lock);
        std::map<uint32_t, Slot*>::iterator it = m_slots.find(id);
        if (it == m_slots.end()) { pthread_mutex_unlock(&m_lock); out->fail = Fail::Cancelled; return true; }
        Slot* s = it->second;
        const bool done = s->done;
        pthread_mutex_unlock(&m_lock);
        if (!done) return false;
        pthread_join(s->th, nullptr);
        *out = s->res;
        pthread_mutex_lock(&m_lock);
        m_slots.erase(id);
        pthread_mutex_unlock(&m_lock);
        delete s;
        return true;
    }
    void Cancel(uint32_t id) override {
        pthread_mutex_lock(&m_lock);
        std::map<uint32_t, Slot*>::iterator it = m_slots.find(id);
        Slot* s = it == m_slots.end() ? nullptr : it->second;
        if (s) m_slots.erase(it);
        pthread_mutex_unlock(&m_lock);
        if (s) { pthread_join(s->th, nullptr); delete s; }
    }
private:
    struct Slot { SockTransport* self; HttpRequest req; HttpResponse res; bool done; pthread_t th; };
    static void* Run(void* arg) {
        Slot* s = static_cast<Slot*>(arg);
        s->self->Exchange(s);
        pthread_mutex_lock(&s->self->m_lock);
        s->done = true;
        pthread_mutex_unlock(&s->self->m_lock);
        return nullptr;
    }
    void Exchange(Slot* s) {
        HttpResponse& res = s->res;
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr;
        memset(&addr, 0, sizeof addr);
        addr.sin_family = AF_INET;
        addr.sin_port = htons(uint16_t(m_port));
        inet_pton(AF_INET, m_host.c_str(), &addr.sin_addr);
        timeval tv = {10, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        if (fd < 0 || connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) { res.fail = Fail::Transport; res.detail = "connect"; if (fd >= 0) close(fd); return; }
        std::string text = s->req.method + " " + s->req.path + " HTTP/1.1\r\nHost: " + m_host + "\r\nConnection: close\r\n";
        for (size_t i = 0; i < s->req.headers.size(); ++i) text += s->req.headers[i] + "\r\n";
        if (!s->req.body.empty()) { char b[64]; snprintf(b, sizeof b, "Content-Type: application/json\r\nContent-Length: %zu\r\n", s->req.body.size()); text += b; }
        text += "\r\n" + s->req.body;
        size_t sent = 0;
        while (sent < text.size()) { const ssize_t n = send(fd, text.data() + sent, text.size() - sent, 0); if (n <= 0) break; sent += size_t(n); }
        std::string raw;
        char buf[4096];
        for (;;) { const ssize_t n = recv(fd, buf, sizeof buf, 0); if (n <= 0) break; raw.append(buf, size_t(n)); }
        close(fd);
        const size_t headEnd = raw.find("\r\n\r\n");
        if (raw.compare(0, 5, "HTTP/") != 0 || headEnd == std::string::npos) { res.fail = Fail::Transport; res.detail = "no answer"; return; }
        res.status = atoi(raw.c_str() + 9);
        std::string head = raw.substr(0, headEnd);
        std::string body = raw.substr(headEnd + 4);
        const bool chunked = head.find("chunked") != std::string::npos || head.find("Chunked") != std::string::npos;
        if (chunked) {   // decode
            std::string out;
            size_t p = 0;
            while (p < body.size()) {
                const size_t eol = body.find("\r\n", p);
                if (eol == std::string::npos) break;
                const size_t len = strtoul(body.substr(p, eol - p).c_str(), nullptr, 16);
                if (!len) break;
                out += body.substr(eol + 2, len);
                p = eol + 2 + len + 2;
            }
            body = out;
        }
        res.body = body;
    }
    std::string m_host;
    int m_port;
    pthread_mutex_t m_lock;
    std::map<uint32_t, Slot*> m_slots;
    uint32_t m_next = 0;
};

// ---- helpers -------------------------------------------------------------------------------------------------------------------------
static std::string g_dataDir;

static void SleepMs(int ms) { usleep(unsigned(ms) * 1000); }

template <class F>
static Result Wait(F poll) {
    for (int i = 0; i < 5000; ++i) {
        const Result r = poll();
        if (r.status != Status::Pending) return r;
        SleepMs(2);
    }
    Result r;
    r.status = Status::Failed;
    r.fail = Fail::Timeout;
    return r;
}

// A raw JSON call through a transport (setup of the scenario; not the code under test).
static JValue Raw(HttpTransport* t, const char* method, const std::string& path, const std::string& token, const std::string& body, int* status = nullptr) {
    HttpRequest rq;
    rq.method = method;
    rq.path = path;
    rq.body = body;
    rq.timeoutMs = 10000;
    if (!token.empty()) rq.headers.push_back("Authorization: Bearer " + token);
    const uint32_t id = t->Start(rq);
    HttpResponse res;
    for (int i = 0; i < 5000 && !t->Poll(id, &res); ++i) SleepMs(2);
    if (status) *status = res.status;
    JValue j;
    JsonParse(res.body.data(), res.body.size(), &j);
    return j;
}

struct Player {
    std::string name, token;
    int32_t pid;
    Api* api;
    HttpTransport* rawT;
    Player() : pid(0), api(nullptr), rawT(nullptr) {}
};

static Api* NewApi(const std::string& url) { return CreateHttpApi(new SockTransport(url)); }

static bool Register(Player* p, const std::string& url, const std::string& name) {
    p->name = name;
    p->api = NewApi(url);
    p->rawT = new SockTransport(url);
    LoginRequest rq;
    snprintf(rq.name, sizeof rq.name, "%s", name.c_str());
    rq.region = 2;
    rq.gameCode = 0x52345145;
    const uint32_t id = p->api->BeginLogin(rq);
    LoginReply rp;
    const Result r = Wait([&] { return p->api->PollLogin(id, &rp); });
    if (!r.ok()) return false;
    p->pid = rp.profileId;
    p->token = rp.token;
    return rp.newToken && rp.nameCheck == NameCheck::Valid;
}

static std::string Blob(const Player& p, int wins = 1, int losses = 0) {
    uint8_t data[128];
    FakeMakeUserData(data, p.name.c_str(), wins, losses, 2, 30, 10, 2026);
    return Base64Encode(data, 128);
}

static bool Put(Player& p, int cat, int score, PutReply* reply, Fail* fail = nullptr) {
    PutRequest rq;
    rq.category = uint32_t(cat);
    rq.region = 2;
    rq.score = score;
    rq.size = 128;
    FakeMakeUserData(rq.data, p.name.c_str(), 1, 0, 2, 30, 10, 2026);
    const uint32_t id = p.api->BeginPutScore(rq);
    if (!id) return false;
    const Result r = Wait([&] { return p.api->PollPutScore(id, reply); });
    if (fail) *fail = r.fail;
    return r.ok();
}

static bool GetRows(Player& p, uint32_t mode, uint32_t limit, GetReply* reply, int cat = 0) {
    GetRequest rq;
    rq.mode = mode; rq.category = uint32_t(cat); rq.region = 0xFF; rq.sort = 1; rq.limit = limit;
    const uint32_t id = p.api->BeginGetScores(rq);
    if (!id) return false;
    return Wait([&] { return p.api->PollGetScores(id, reply); }).ok();
}

static uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

alignas(8) static uint8_t g_block[0xB10];
static int g_cbCount = 0, g_cbError = -1, g_cbPid = -1;
static void LoginCb(DWCError error, int pid, void*) { ++g_cbCount; g_cbError = (int)error; g_cbPid = pid; }

static void SetUpDwc(Api* api) {
    NpDwcResetForTest();
    SetApi(api);
    QueueResetForTest(false);
    NpOnlineForgetCredentials();
    memset(g_block, 0, sizeof g_block);
    DWC_Init(DWC_SVR_RELEASE, "mschargedwii", 0x52345145, 0, 0);
    DWC_CreateUserData((DWCUserData*)g_block);
    DWC_InitFriendsMatch(0, (DWCUserData*)g_block, 0x2AAF, "mschargedwii", "key", 0, 0, 0, 0);
}

static bool DwcLogin(const char* name) {
    unsigned short wide[16] = {0};
    for (int i = 0; name[i] && i < 15; ++i) wide[i] = (unsigned short)name[i];
    g_cbCount = 0; g_cbError = -1; g_cbPid = -1;
    if (!DWC_LoginAsync(wide, 0, LoginCb, 0)) return false;
    for (int i = 0; i < 5000 && !g_cbCount; ++i) { DWC_ProcessFriendsMatch(); SleepMs(2); }
    return g_cbCount == 1 && g_cbError == 0;
}

static std::string Seq() {
    std::string s;
    for (int i = 0; i < 5000; ++i) {
        const DWCRnkError r = DWC_RnkProcess();
        if (r == DWC_RNK_SUCCESS) s += 'S';
        else if (r == DWC_RNK_PROCESS_NOTASK) { s += 'N'; break; }
        else { s += 'E'; break; }
        DWC_ProcessFriendsMatch();
        SleepMs(2);
    }
    return s;
}

// ---- the scenarios ---------------------------------------------------------------------------------------------------------------------
static void TestLoginAndClock(const std::string& url) {
    printf("-- login, rename, name refusals, clock\n");
    Api* api = NewApi(url);
    SetUpDwc(api);
    CHECK(DwcLogin("Tester1"));
    const int pid = g_cbPid;
    CHECK(pid > 100000);
    CHECK_EQ(DWC_GetIngamesnCheckResult(), 0);
    CHECK_EQ(Rd32(g_block + 0x1C), (uint32_t)pid);
    NpOnlineCredentials creds;
    CHECK(NpOnlineLoadCredentials(&creds) && creds.profileId == pid && strlen(creds.token) == 43);
    DWCDate d; DWCTime t;
    CHECK(DWC_GetDateTime(&d, &t));
    const time_t now = time(nullptr);
    tm g;
    gmtime_r(&now, &g);
    CHECK_EQ(d.year, g.tm_year + 1900);
    CHECK_EQ(d.month, g.tm_mon);
    CHECK(d.mday == g.tm_mday || d.mday == g.tm_mday + 1 || d.mday == g.tm_mday - 1);
    CHECK_EQ(d.yday >= 0 && d.yday < 366, 1);

    // second login: the token, no registration
    DWC_Shutdown();
    DWC_Init(DWC_SVR_RELEASE, "mschargedwii", 0x52345145, 0, 0);
    DWC_InitFriendsMatch(0, (DWCUserData*)g_block, 0x2AAF, "mschargedwii", "key", 0, 0, 0, 0);
    CHECK(DwcLogin("Tester1"));
    CHECK_EQ(g_cbPid, pid);
    // another typed name: the account is renamed once (24 h cooldown on the next)
    CHECK(DwcLogin("Testerone"));
    CHECK_EQ(g_cbPid, pid);
    CHECK_EQ(DWC_GetIngamesnCheckResult(), 0);
    CHECK(DwcLogin("Testertwo"));                        // rename_too_soon: signs in with the registered name
    CHECK_EQ(g_cbPid, pid);

    // a name that is taken (look-alikes count), and one that breaks the rules: no account, the game's "name not allowed" path
    Player other;
    CHECK(Register(&other, url, "Hannah"));
    NpOnlineForgetCredentials();
    memset(g_block, 0, sizeof g_block);
    DWC_CreateUserData((DWCUserData*)g_block);
    StoreName("Hannah");
    CHECK(DwcLogin("H4nnah"));
    CHECK_EQ(DWC_GetIngamesnCheckResult(), 2);
    CHECK_EQ(g_cbPid, 0);
    CHECK_EQ(Rd32(g_block + 0x1C), 0u);
    char stored[16];
    CHECK(!LoadStoredName(stored));
    CHECK(DwcLogin("x"));                                // too short for the service
    CHECK_EQ(DWC_GetIngamesnCheckResult(), 2);
    NpOnlineCredentials none;
    CHECK(!NpOnlineLoadCredentials(&none));

    // a token the service does not know: registered again, once
    NpOnlineCredentials bogus;
    bogus.profileId = 123456;
    snprintf(bogus.token, sizeof bogus.token, "%s", "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz");
    NpOnlineSaveCredentials(bogus);
    CHECK(DwcLogin("Newcomer"));
    CHECK(g_cbPid > 100000 && g_cbPid != 123456);
    NpOnlineCredentials fresh;
    CHECK(NpOnlineLoadCredentials(&fresh) && !strcmp(fresh.token, bogus.token) == false);

    // the clock follows the service
    TimeReply tr;
    const uint32_t id = api->BeginTime();
    CHECK(Wait([&] { return api->PollTime(id, &tr); }).ok());
    CHECK(tr.unixMs > int64_t(now) * 1000 - 5000 && tr.unixMs < int64_t(now) * 1000 + 5000);
    SetApi(nullptr);
    delete api;
}

static void TestRankingOpen(const std::string& url) {
    printf("-- ranking against the service (puts accepted as sent)\n");
    Player a, b, c;
    CHECK(Register(&a, url, "Anna"));
    CHECK(Register(&b, url, "Bertie"));
    CHECK(Register(&c, url, "Carla"));
    PutReply pr;
    CHECK(Put(b, 0, 500, &pr) && pr.stored == 500 && !pr.clamped);
    CHECK(Put(c, 0, 900, &pr) && pr.stored == 900);
    CHECK(Put(a, 0, 700, &pr) && pr.stored == 700);
    GetReply g;
    CHECK(GetRows(a, 1, 10, &g));
    CHECK_EQ(g.rows.size(), 3u);
    CHECK_EQ(ROWAT(g, 0).score, 900); CHECK_EQ(ROWAT(g, 0).order, 1u); CHECK_EQ(ROWAT(g, 0).pid, c.pid);
    CHECK_EQ(ROWAT(g, 1).pid, a.pid); CHECK_EQ(ROWAT(g, 2).score, 500); CHECK_EQ(ROWAT(g, 2).order, 3u);
    for (size_t i = 0; i < g.rows.size(); ++i) CHECK_EQ(g.rows[i].size, 128u);
    CHECK(GetRows(a, 2, 2, &g));                            // NEAR: the caller's row is always in it
    bool mine = false;
    for (size_t i = 0; i < g.rows.size(); ++i) mine = mine || g.rows[i].pid == a.pid;
    CHECK(mine && g.rows.size() == 2);
    GetRequest fr;
    fr.mode = 3; fr.category = 0; fr.region = 0xFF; fr.sort = 1; fr.limit = 5; fr.friendCount = 1; fr.friends[0] = c.pid;
    const uint32_t id = a.api->BeginGetScores(fr);
    CHECK(Wait([&] { return a.api->PollGetScores(id, &g); }).ok());
    CHECK_EQ(g.rows.size(), 2u);                            // the friend and the caller
    CHECK(GetRows(a, 0, 0, &g));                            // ORDER
    CHECK_EQ(g.order, 2u);

    // a rejected blob (digest) is a refusal, not a retry
    PutRequest bad;
    bad.category = 0; bad.region = 2; bad.score = 800; bad.size = 128;
    FakeMakeUserData(bad.data, "Anna", 1, 0, 2, 30, 10, 2026);
    bad.data[5] ^= 1;
    uint32_t pid2 = a.api->BeginPutScore(bad);
    Result r = Wait([&] { return a.api->PollPutScore(pid2, &pr); });
    CHECK(!r.ok() && r.fail == Fail::Rejected && r.http == 400);
    FakeMakeUserData(bad.data, "Wrongname", 1, 0, 2, 30, 10, 2026);   // the name inside the blob must be the registered one
    pid2 = a.api->BeginPutScore(bad);
    r = Wait([&] { return a.api->PollPutScore(pid2, &pr); });
    CHECK(!r.ok() && r.fail == Fail::Rejected);
    CHECK(!QueueIsRetryable(r.fail));

    // the same through the DWC calls, with a player who has no score: the NEAR row is the service's synthesized one
    Api* api = NewApi(url);
    SetUpDwc(api);
    CHECK(DwcLogin("Dora"));
    CHECK_EQ(DWC_RnkInitialize("x", (DWCUserData*)g_block), DWC_RNK_SUCCESS);
    DWCRnkGetParam p;
    memset(&p, 0, sizeof p);
    p.size = sizeof(p.near); p.near.sort = 1; p.near.limit = 5;
    CHECK_EQ(DWC_RnkGetScoreAsync(DWC_RNK_GET_MODE_NEAR, 0, DWC_RNK_REGION_US, &p), DWC_RNK_SUCCESS);
    CHECK(Seq().back() == 'N');
    u32 count = 0;
    CHECK_EQ(DWC_RnkResGetRowCount(&count), DWC_RNK_SUCCESS);
    bool ranked = false;
    for (u32 i = 0; i < count; ++i) { DWCRnkData row; DWC_RnkResGetRow(&row, i); ranked = ranked || row.order >= 1; }
    CHECK(count >= 1 && ranked);
    uint8_t data[128];
    FakeMakeUserData(data, "Dora", 2, 1, 2, 30, 10, 2026);
    CHECK_EQ(DWC_RnkPutScoreAsync(0, DWC_RNK_REGION_US, 650, data, 128), DWC_RNK_SUCCESS);
    CHECK(Seq().back() == 'N');
    CHECK_EQ(QueuePending(), 0u);
    SetApi(nullptr);
    delete api;
}

static void TestRankedMatchFlow(const std::string& url) {
    printf("-- ranked match: result, final, then the totals (credit policy)\n");
    Player a, b;
    CHECK(Register(&a, url, "Alpha"));
    CHECK(Register(&b, url, "Bravo"));
    PutReply pr;
    Fail fail = Fail::None;
    CHECK(!Put(a, 0, 700, &pr, &fail) || pr.clamped);       // no verified match yet: the total is clamped (to 0)
    CHECK(Put(a, 0, 5, &pr) ? pr.clamped || pr.stored <= 5 : true);

    // pair them through the queue and start the match
    JValue j = Raw(a.rawT, "POST", "/v1/ranked/queue", a.token, "{}");
    j = Raw(b.rawT, "POST", "/v1/ranked/queue", b.token, "{}");
    std::string code = j.Str_("roomCode");
    if (code.empty()) {   // the first player to be told
        for (int i = 0; i < 20 && code.empty(); ++i) { j = Raw(a.rawT, "GET", "/v1/ranked/queue", a.token, ""); code = j.Str_("roomCode"); SleepMs(50); }
    }
    CHECK(!code.empty());
    const std::string ep = "{\"public\":\"203.0.113.7:41000\",\"local\":\"192.168.1.20:41000\",\"nonce\":\"AAECAwQFBgc=\",\"nat\":\"cone\"}";
    const std::string ep2 = "{\"public\":\"203.0.113.9:41000\",\"local\":\"192.168.1.21:41000\",\"nonce\":\"CAkKCwwNDg8=\",\"nat\":\"cone\"}";
    Raw(a.rawT, "POST", "/v1/rooms/" + code + "/endpoint", a.token, ep);
    Raw(b.rawT, "POST", "/v1/rooms/" + code + "/endpoint", b.token, ep2);
    Raw(a.rawT, "POST", "/v1/rooms/" + code + "/started", a.token, "{}");
    JValue room = Raw(b.rawT, "POST", "/v1/rooms/" + code + "/started", b.token, "{}");
    std::string matchId = room.Str_("matchId");
    CHECK(!matchId.empty());
    const int slotA = int(Raw(a.rawT, "GET", "/v1/rooms/" + code, a.token, "").Num_("slot", 0));

    // both report the same result; the slot-0 player scored 3 and wins
    ResultReport rep;
    snprintf(rep.matchId, sizeof rep.matchId, "%s", matchId.c_str());
    rep.result = 0; rep.score0 = 3; rep.score1 = 1;
    snprintf(rep.checksum, sizeof rep.checksum, "0f1e2d3c4b5a6978");
    snprintf(rep.mode, sizeof rep.mode, "ranked");
    Player* players[2] = {&a, &b};
    for (int i = 0; i < 2; ++i) {
        const uint32_t id = players[i]->api->BeginReportResult(rep);
        CHECK(id != 0);
        CHECK(Wait([&] { return players[i]->api->PollReportResult(id); }).ok());
    }
    MatchStatus st;
    for (int i = 0; i < 100; ++i) {
        const uint32_t id = a.api->BeginMatchStatus(matchId.c_str());
        CHECK(Wait([&] { return a.api->PollMatchStatus(id, &st); }).ok());
        if (st.final) break;
        SleepMs(20);
    }
    CHECK(st.final);
    CHECK(!strcmp(st.status, "verified"));
    Player& winner = slotA == 0 ? a : b;
    Player& loser = slotA == 0 ? b : a;
    MatchStatus wst;
    {
        const uint32_t id = winner.api->BeginMatchStatus(matchId.c_str());
        CHECK(Wait([&] { return winner.api->PollMatchStatus(id, &wst); }).ok());
    }
    CHECK(wst.won && wst.points == 13);
    MatchStatus lst;
    {
        const uint32_t id = loser.api->BeginMatchStatus(matchId.c_str());
        CHECK(Wait([&] { return loser.api->PollMatchStatus(id, &lst); }).ok());
    }
    CHECK(!lst.won && lst.points == 2);
    // resubmitting the identical report is fine; a settled match is "closed" and not an error for the queue
    {
        const uint32_t id = a.api->BeginReportResult(rep);
        Result r = Wait([&] { return a.api->PollReportResult(id); });
        CHECK(r.ok());
    }
    // now the totals: the credit lets the winner's score rise by what the match earned, not more
    CHECK(Put(winner, 0, 13, &pr) && pr.stored == 13 && !pr.clamped);
    CHECK(Put(winner, 0, 700, &pr) && pr.clamped && pr.stored >= 13 && pr.stored < 700);
    CHECK(Put(loser, 0, 2, &pr) && pr.stored == 2 && !pr.clamped);
}

static Result FOp(Player& p, FriendOp op, int32_t pid, const char* room = "", FriendOpReply* rep = nullptr) {
    FriendOpRequest rq;
    rq.op = op;
    rq.pid = pid;
    snprintf(rq.roomCode, sizeof rq.roomCode, "%s", room);
    const uint32_t id = p.api->BeginFriendOp(rq);
    if (!id) { Result r; r.status = Status::Failed; r.fail = Fail::Disabled; return r; }
    FriendOpReply tmp;
    return Wait([&] { return p.api->PollFriendOp(id, rep ? rep : &tmp); });
}
static bool Friends(Player& p, FriendsReply* out, const char* rev = "", uint32_t wait = 0) {
    FriendsRequest fr;
    snprintf(fr.rev, sizeof fr.rev, "%s", rev);
    fr.waitSec = wait;
    const uint32_t id = p.api->BeginFriends(fr);
    if (!id) return false;
    return Wait([&] { return p.api->PollFriends(id, out); }).ok();
}
static const FriendRow* RowOf(const FriendsReply& r, int32_t pid) {
    for (size_t i = 0; i < r.rows.size(); ++i) if (r.rows[i].pid == pid) return &r.rows[i];
    return nullptr;
}

static void TestFriendsAndQuick(const std::string& url) {
    printf("-- friends, invites, blocks and the unranked quick queue (real local service)\n");
    Player a, b, c;
    CHECK(Register(&a, url, "Fa" + Seq()));
    CHECK(Register(&b, url, "Fb" + Seq()));
    CHECK(Register(&c, url, "Fc" + Seq()));
    FriendOpReply rep;
    Result r = FOp(a, FriendOp::Add, b.pid, "", &rep);
    CHECK(r.ok());
    CHECK(rep.state == FriendState::Outgoing);
    FriendsReply fb;
    CHECK(Friends(b, &fb));
    const FriendRow* in = RowOf(fb, a.pid);
    CHECK(in && in->state == FriendState::Incoming);
    r = FOp(b, FriendOp::Add, a.pid, "", &rep);
    CHECK(r.ok() && rep.state == FriendState::Friend);
    FriendsReply fa;
    CHECK(Friends(a, &fa));
    const FriendRow* ra = RowOf(fa, b.pid);
    CHECK(ra && ra->state == FriendState::Friend && ra->presence != Presence::Offline);
    // an unchanged poll answers "changed: false" and is held at the server for at most the wait
    FriendsReply again;
    CHECK(Friends(a, &again, fa.rev, 0));
    CHECK(!again.changed);
    // yourself, a stranger code, and the codes of the errors
    r = FOp(a, FriendOp::Add, a.pid);
    CHECK(!r.ok() && strcmp(r.code, "bad_friend") == 0);
    // a block is silent for the blocked side and hides their request
    CHECK(FOp(a, FriendOp::Block, c.pid).ok());
    r = FOp(c, FriendOp::Add, a.pid, "", &rep);
    FriendsReply fa2;
    CHECK(Friends(a, &fa2));
    CHECK(!RowOf(fa2, c.pid));
    CHECK(fa2.blocked.size() == 1);
    r = FOp(a, FriendOp::Add, c.pid);
    CHECK(!r.ok() && strcmp(r.code, "blocked") == 0);
    CHECK(FOp(a, FriendOp::Unblock, c.pid).ok());
    // invite: a needs an open unranked room of their own; the invitation shows on b's friends row
    r = FOp(a, FriendOp::Invite, b.pid, "ZZZ999");
    CHECK(!r.ok());
    JValue room = Raw(a.rawT, "POST", "/v1/rooms", a.token, "{}");
    const std::string code = room.Str_("code").empty() ? room.Str_("roomCode") : room.Str_("code");
    CHECK(!code.empty());
    CHECK(FOp(a, FriendOp::Invite, b.pid, code.c_str()).ok());
    CHECK(Friends(b, &fb));
    const FriendRow* ib = RowOf(fb, a.pid);
    CHECK(ib && code == ib->inviteRoom);
    CHECK(code == ib->roomCode);
    CHECK(FOp(b, FriendOp::ClearInvite, a.pid).ok());
    CHECK(Friends(b, &fb));
    ib = RowOf(fb, a.pid);
    CHECK(ib && !ib->inviteRoom[0]);
    // played with: empty so far
    {
        const uint32_t id = a.api->BeginPlayedWith();
        PlayedReply pr;
        CHECK(id && Wait([&] { return a.api->PollPlayedWith(id, &pr); }).ok());
        CHECK(pr.rows.empty());
    }
    // removal: both lose the friendship (b keeps nothing of a's incoming request)
    CHECK(FOp(a, FriendOp::Remove, b.pid).ok());
    CHECK(Friends(b, &fb));
    CHECK(!RowOf(fb, a.pid) || RowOf(fb, a.pid)->state != FriendState::Friend);

    // the quick queue: a body {"mode":"quick"} pairs anyone and the room is unranked; old clients ({}) stay ranked
    Player q1, q2, q3, q4;
    CHECK(Register(&q1, url, "Q1" + Seq()));
    CHECK(Register(&q2, url, "Q2" + Seq()));
    CHECK(Register(&q3, url, "Q3" + Seq()));
    CHECK(Register(&q4, url, "Q4" + Seq()));
    JValue j = Raw(q1.rawT, "POST", "/v1/ranked/queue", q1.token, "{\"mode\":\"quick\"}");
    CHECK(strcmp(j.Str_("mode").c_str(), "quick") == 0);
    j = Raw(q2.rawT, "POST", "/v1/ranked/queue", q2.token, "{\"mode\":\"quick\"}");
    std::string qc = j.Str_("roomCode");
    for (int i = 0; i < 20 && qc.empty(); ++i) { j = Raw(q1.rawT, "GET", "/v1/ranked/queue", q1.token, ""); qc = j.Str_("roomCode"); SleepMs(50); }
    CHECK(!qc.empty());
    JValue rv = Raw(q1.rawT, "GET", "/v1/rooms/" + qc, q1.token, "");
    CHECK(rv.Str_("kind") == "unranked");
    // an old client (no body) is ranked, and a ranked player is never paired with a quick one
    j = Raw(q3.rawT, "POST", "/v1/ranked/queue", q3.token, "");
    CHECK(strcmp(j.Str_("mode").c_str(), "ranked") == 0 || j.Str_("mode").empty());
    j = Raw(q4.rawT, "POST", "/v1/ranked/queue", q4.token, "{\"mode\":\"quick\"}");
    CHECK(j.Str_("roomCode").empty());
    Raw(q3.rawT, "DELETE", "/v1/ranked/queue", q3.token, "");
    Raw(q4.rawT, "DELETE", "/v1/ranked/queue", q4.token, "");
}

static void TestAdmin(const std::string& url) {
    printf("-- kill switch\n");
    Api* api = NewApi(url);
    SetUpDwc(api);
    SockTransport admin(url);
    int status = 0;
    Raw(&admin, "POST", "/v1/admin/switches", "fake-admin", "{\"kill\":[\"all\"],\"message\":\"Back soon\"}", &status);
    CHECK_EQ(status, 200);
    CHECK(!DwcLogin("Eve"));
    long code = 0; DWCErrorType type;
    DWC_GetLastErrorEx(&code, &type);
    CHECK_EQ(code, 20101);
    char msg[160];
    GetStatusMessage(msg, sizeof msg);
    CHECK(!strcmp(msg, "Back soon"));
    DWC_ClearError();
    Raw(&admin, "POST", "/v1/admin/switches", "fake-admin", "{\"kill\":[],\"message\":\"\"}", &status);
    CHECK(DwcLogin("Eve"));
    SetApi(nullptr);
    delete api;
}

static void TestUpgrade(const std::string& url) {
    printf("-- upgrade required\n");
    Api* api = NewApi(url);
    SetUpDwc(api);
    CHECK(!DwcLogin("Frank"));
    long code = 0; DWCErrorType type;
    DWC_GetLastErrorEx(&code, &type);
    CHECK_EQ(code, 20110);
    char msg[160];
    GetStatusMessage(msg, sizeof msg);
    CHECK(strstr(msg, "newer version") != nullptr);
    DWC_ClearError();
    SetApi(nullptr);
    delete api;
}

static void TestOffline(const std::string& deadUrl) {
    printf("-- no service\n");
    Api* api = NewApi(deadUrl);
    SetUpDwc(api);
    CHECK(!DwcLogin("Gina"));
    long code = 0; DWCErrorType type;
    DWC_GetLastErrorEx(&code, &type);
    CHECK_EQ(code, 20100);
    DWC_ClearError();
    // a score earned offline waits in the queue
    uint8_t data[128];
    FakeMakeUserData(data, "Gina", 1, 0, 2, 30, 10, 2026);
    PutRequest rq;
    rq.category = 0; rq.region = 2; rq.score = 40; rq.size = 128;
    memcpy(rq.data, data, 128);
    CHECK(QueuePut(rq) != 0);
    CHECK_EQ(QueuePending(), 1u);
    SetApi(nullptr);
    delete api;
}

// gxnet14 (Galaxy quota32): Cloudflare's own 429 page (the free daily quota is spent). A tiny raw responder stands in for the edge.
static int g_edgeHits = 0;
static int g_edgeListen = -1;
static void* EdgeThread(void*) {
    for (;;) {
        const int c = accept(g_edgeListen, nullptr, nullptr);
        if (c < 0) return nullptr;
        char buf[2048];
        (void)!read(c, buf, sizeof buf);
        ++g_edgeHits;
        static const char kPage[] = "<html><body>error code: 1027 Worker exceeded free tier daily request limit</body></html>";
        char head[256];
        const int n = snprintf(head, sizeof head, "HTTP/1.1 429 Too Many Requests\r\nContent-Type: text/html\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", int(sizeof kPage - 1));
        (void)!write(c, head, n);
        (void)!write(c, kPage, sizeof kPage - 1);
        close(c);
    }
}
static void TestEdgeBusy() {
    g_edgeListen = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    int one = 1; setsockopt(g_edgeListen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (bind(g_edgeListen, (sockaddr*)&a, sizeof a) != 0 || listen(g_edgeListen, 8) != 0) { CHECK(!"edge listener"); return; }
    socklen_t al = sizeof a; getsockname(g_edgeListen, (sockaddr*)&a, &al);
    const std::string url = "http://127.0.0.1:" + std::to_string(ntohs(a.sin_port));
    pthread_t th; pthread_create(&th, nullptr, EdgeThread, nullptr);
    NpNet::ResetServiceBusy();
    { Player p; CHECK(!Register(&p, url, "EdgeA")); }
    CHECK(NpNet::ServiceBusyBlocksNew());                 // remembered: a decisive call is now answered by the console itself
    CHECK(g_edgeHits >= 1);
    NpNet::ResetServiceBusy();
    setenv("MSC_NET_EDGE_BUSY", "0", 1);                  // the older behaviour: an ordinary refusal, nothing remembered
    { Player p; CHECK(!Register(&p, url, "EdgeB")); }
    CHECK(!NpNet::ServiceBusyBlocksNew());
    unsetenv("MSC_NET_EDGE_BUSY");
    shutdown(g_edgeListen, SHUT_RDWR); close(g_edgeListen);
    pthread_join(th, nullptr);
    NpNet::ResetServiceBusy();
}

// gxnet14: the registration proof of work. <pow-url> is a fake with REGISTER_POW_BITS=9 REGISTER_POW_FREE=1 (the price rises one bit per registration from this address),
// <hard-url> one with REGISTER_POW_BITS=20 (above what this client grinds: it must say "busy" and not spin).
static void TestPow(const std::string& powUrl, const std::string& hardUrl) {
    for (int i = 0; i < 4; ++i) {   // 9, 10, 11, 12 bits: all solved, each account made once
        Player p;
        CHECK(Register(&p, powUrl, "PowA" + std::to_string(i)));
        CHECK(p.pid > 0 && !p.token.empty());
    }
    { Player p; CHECK(!Register(&p, hardUrl, "HardA")); }   // 20 bits: refused calmly (and fast: nothing is ground)
    setenv("MSC_NET_POW", "0", 1);   // switched off: the 428 is an ordinary refusal, as before wave 14
    { Player p; CHECK(!Register(&p, powUrl, "NoPowA")); }
    unsetenv("MSC_NET_POW");
    { Player p; CHECK(Register(&p, powUrl, "PowAgain")); }
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc < 5) { fprintf(stderr, "usage: http_test <credit-url> <open-url> <upgrade-url> <dead-url>\n"); return 2; }
    char dir[] = "/tmp/dwc_http_XXXXXX";
    if (!mkdtemp(dir)) return 2;
    g_dataDir = dir;
    setenv("NP_DATA_DIR", dir, 1);
    unsetenv("MSC_ONLINE_NAME");
    SetClockForTest(nullptr);
    TestLoginAndClock(argv[2]);
    TestRankingOpen(argv[2]);
    TestRankedMatchFlow(argv[1]);
    TestFriendsAndQuick(argv[2]);
    TestAdmin(argv[2]);
    setenv("MSC_ONLINE_APP_VERSION", "0.0.1", 1);
    TestUpgrade(argv[3]);
    TestOffline(argv[4]);
    if (argc >= 7) TestPow(argv[5], argv[6]);
    TestEdgeBusy();
    std::string cmd = "rm -rf " + g_dataDir;
    if (system(cmd.c_str())) {}
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
