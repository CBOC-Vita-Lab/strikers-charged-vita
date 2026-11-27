// The in-process fake of the online service. See np_online_fake.h.
#include "platform/sdk/dwc/np_online_fake.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>

namespace NpOnline {
namespace {

// ---- MD5 / HMAC-MD5 (RFC 1321 / RFC 2104): the digest the game puts at 0x70 of its ranking record ----------------------------
struct Md5 {
    uint32_t a, b, c, d;
    uint64_t bytes;
    uint8_t buf[64];
    static uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
    void Init() { a = 0x67452301u; b = 0xefcdab89u; c = 0x98badcfeu; d = 0x10325476u; bytes = 0; }
    void Block(const uint8_t* p) {
        static const uint32_t K[64] = {
            0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
            0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
            0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
            0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
            0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
            0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
            0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
            0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
        static const int S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                                  4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
        uint32_t m[16];
        for (int i = 0; i < 16; ++i) m[i] = uint32_t(p[4 * i]) | uint32_t(p[4 * i + 1]) << 8 | uint32_t(p[4 * i + 2]) << 16 | uint32_t(p[4 * i + 3]) << 24;
        uint32_t A = a, B = b, C = c, D = d;
        for (int i = 0; i < 64; ++i) {
            uint32_t f;
            int g;
            if (i < 16) { f = (B & C) | (~B & D); g = i; }
            else if (i < 32) { f = (D & B) | (~D & C); g = (5 * i + 1) & 15; }
            else if (i < 48) { f = B ^ C ^ D; g = (3 * i + 5) & 15; }
            else { f = C ^ (B | ~D); g = (7 * i) & 15; }
            const uint32_t t = D;
            D = C;
            C = B;
            B = B + rol(A + f + K[i] + m[g], S[i]);
            A = t;
        }
        a += A; b += B; c += C; d += D;
    }
    void Update(const uint8_t* p, size_t n) {
        size_t fill = size_t(bytes & 63);
        bytes += n;
        if (fill) {
            const size_t take = std::min(n, 64 - fill);
            memcpy(buf + fill, p, take);
            p += take; n -= take; fill += take;
            if (fill < 64) return;
            Block(buf);
        }
        while (n >= 64) { Block(p); p += 64; n -= 64; }
        memcpy(buf, p, n);
    }
    void Final(uint8_t out[16]) {
        const uint64_t bits = bytes * 8;
        const uint8_t pad = 0x80;
        Update(&pad, 1);
        const uint8_t zero = 0;
        while ((bytes & 63) != 56) Update(&zero, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; ++i) len[i] = uint8_t(bits >> (8 * i));
        Update(len, 8);
        const uint32_t v[4] = {a, b, c, d};
        for (int i = 0; i < 4; ++i)
            for (int k = 0; k < 4; ++k) out[4 * i + k] = uint8_t(v[i] >> (8 * k));
    }
};

void HmacMd5(const uint8_t* key, size_t keyLen, const uint8_t* msg, size_t msgLen, uint8_t out[16]) {
    uint8_t k[64] = {0};
    if (keyLen > 64) { Md5 h; h.Init(); h.Update(key, keyLen); h.Final(k); }
    else memcpy(k, key, keyLen);
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
    uint8_t inner[16];
    Md5 h;
    h.Init(); h.Update(ipad, 64); h.Update(msg, msgLen); h.Final(inner);
    h.Init(); h.Update(opad, 64); h.Update(inner, 16); h.Final(out);
}

// NetworkStats.cpp: sRankingHmacKey (the game's own constant, public in the decompilation)
const uint8_t kRankingKey[32] = {
    0x17, 0xB2, 0x40, 0x52, 0x5F, 0x89, 0xF0, 0xFF, 0xE9, 0xB9, 0xB0, 0x17, 0x66, 0x06, 0x37, 0xA1,
    0x63, 0x70, 0x9E, 0xC9, 0xCF, 0x42, 0xD1, 0x89, 0x91, 0x36, 0xC7, 0xBD, 0x2B, 0x58, 0xBC, 0x39};

}  // namespace

void FakeMakeUserData(uint8_t out[128], const char* name, int wins, int losses, int region, int day, int month, int year) {
    memset(out, 0, 128);
    out[0] = uint8_t(day);
    out[1] = uint8_t(month);
    out[2] = uint8_t(year & 255);
    out[3] = uint8_t(year >> 8);
    memcpy(out + 4, &wins, 4);
    memcpy(out + 8, &losses, 4);
    out[0xC] = uint8_t(region & 255);
    out[0xD] = uint8_t(region >> 8);
    for (int i = 0; i < 10 && name && name[i]; ++i) {
        out[0xE + 2 * i] = uint8_t(name[i]);   // u16 characters, little-endian host
        out[0xE + 2 * i + 1] = 0;
    }
    HmacMd5(kRankingKey, sizeof kRankingKey, out, 0x70, out + 0x70);
}

FakeApi::FakeApi(const FakeOptions& options)
    : loginCount(0), timeCount(0), putCount(0), getCount(0), reportCount(0), statusCount(0), friendsCount(0), friendOpCount(0), playedCount(0), lastFriendsWaitSec(0), hasLastPut(false), m_options(options), m_next(0), m_nextProfile(1001),
      m_lastProfile(0), m_session(0), m_rng(0x2545F491u), m_failNext(false), m_failAll(false), m_stalled(false), m_refuse(false),
      m_failKind(Fail::None), m_failHttp(0), m_nameCheck(NameCheck::Valid), m_matchPolls(0), m_clamp(false), m_clampStored(0), m_failWhenKind(Fail::None), m_failWhenHttp(0), m_failWhenMask(0), m_failFromMs(0), m_synthOwn(false), m_tamper(nullptr), m_tamperUser(nullptr) {
    m_timeBase = options.serverTimeMs ? options.serverTimeMs : int64_t(time(nullptr)) * 1000;
    m_clockBase = int64_t(NowMs());
    if (options.seedPlayers) {
        static const char* const names[] = {"Ana", "Bo", "Cleo", "Dax", "Eli", "Fay", "Gus", "Hana", "Ivo", "Jun", "Kai", "Lena"};
        static const int scores[] = {980, 910, 860, 790, 720, 640, 590, 520, 430, 360, 280, 190};
        static const uint32_t cats[] = {0, 1, 3, 4};
        for (int i = 0; i < 12; ++i) {
            for (uint32_t cat : cats) {
                const int s = cat == 1 || cat == 4 ? scores[(i * 5) % 12] / 2 : scores[i];
                SeedPlayer(names[i], cat, s, 0x07, 5 + 20 * uint32_t(i), 20 - i, 8 + i);
            }
        }
    }
}

FakeApi::~FakeApi() {}

int32_t FakeApi::SeedPlayer(const char* name, uint32_t category, int32_t score, uint32_t region, uint32_t minutesAgo, int wins, int losses) {
    int32_t pid = 0;
    for (std::map<int32_t, std::string>::const_iterator it = m_names.begin(); it != m_names.end(); ++it)
        if (it->second == name && it->first < 1000) pid = it->first;
    if (!pid) {
        pid = 101 + int32_t(m_names.size());
        m_names[pid] = name;
    }
    uint8_t data[128];
    FakeMakeUserData(data, name, wins, losses, int(region & 0xFF), 1, 1, 2026);
    SeedRaw(pid, category, score, region, minutesAgo, data, sizeof data);
    return pid;
}

void FakeApi::SeedRaw(int32_t pid, uint32_t category, int32_t score, uint32_t region, uint32_t minutesAgo, const uint8_t* data, uint32_t size) {
    Entry e;
    memset(&e, 0, sizeof e);
    e.pid = pid;
    e.score = score;
    e.region = region;
    const uint32_t now = NowMinutes();
    e.lastupdate = now > minutesAgo ? now - minutesAgo : 0;
    e.size = size;
    memcpy(e.data, data, std::min(size, kMaxRowData));
    std::vector<Entry>& board = m_boards[category];
    for (size_t i = 0; i < board.size(); ++i)
        if (board[i].pid == pid) { board[i] = e; return; }
    board.push_back(e);
}

int64_t FakeApi::NowServerMs() const { return m_timeBase + (int64_t(NowMs()) - m_clockBase); }
uint32_t FakeApi::NowMinutes() const { return uint32_t(NowServerMs() / 60000); }
void FakeApi::SetServerTimeMs(int64_t unixMs) { m_timeBase = unixMs; m_clockBase = int64_t(NowMs()); }
void FakeApi::FailNext(Fail fail, int http) { m_failNext = true; m_failKind = fail; m_failHttp = http; }
void FakeApi::FailAll(Fail fail, int http) { m_failAll = true; m_failKind = fail; m_failHttp = http; }
void FakeApi::Heal() { m_failAll = false; m_failNext = false; m_stalled = false; m_refuse = false; }
void FakeApi::Stall(bool stalled) { m_stalled = stalled; }
void FakeApi::RefuseStart(bool refuse) { m_refuse = refuse; }
void FakeApi::SetNameCheck(NameCheck check) { m_nameCheck = check; }
void FakeApi::SetMatchFinalAfterPolls(uint32_t n) { m_matchPolls = n; }
void FakeApi::SetPutClamp(bool clamp, int32_t stored) { m_clamp = clamp; m_clampStored = stored; }
void FakeApi::SetTamper(void (*fn)(GetReply*, void*), void* user) { m_tamper = fn; m_tamperUser = user; }

uint32_t FakeApi::Start(int kind, Pending** out) {
    if (m_refuse) return 0;
    Pending p;
    p.kind = kind;
    p.polls = 0;
    p.fail = Fail::None;
    p.http = 0;
    if (m_failWhenMask && NowMs() >= m_failFromMs && (m_failWhenMask & (kind <= 4 ? (1 << (kind == 1 ? 0 : kind == 2 ? 1 : kind == 3 ? 2 : 3)) : 16))) {
        p.fail = m_failWhenKind;
        p.http = m_failWhenHttp;
    } else if (m_failNext || m_failAll) {
        p.fail = m_failKind;
        p.http = m_failHttp;
        m_failNext = false;
    }
    const uint32_t id = ++m_next;
    events += "LTPGRS"[kind - 1];
    Pending& slot = m_pending[id] = p;
    *out = &slot;
    return id;
}

bool FakeApi::Settle(uint32_t id, int kind, Pending** p, Result* r) {
    std::map<uint32_t, Pending>::iterator it = m_pending.find(id);
    if (it == m_pending.end() || it->second.kind != kind) {
        r->status = Status::Failed;
        r->fail = Fail::Cancelled;
        return false;
    }
    if (m_stalled) return false;                       // stays Pending
    if (it->second.polls < m_options.latencyPolls) {
        ++it->second.polls;
        return false;
    }
    *p = &it->second;
    return true;
}

uint32_t FakeApi::BeginLogin(const LoginRequest& request) {
    Pending* p;
    const uint32_t id = Start(1, &p);
    if (!id) return 0;
    ++loginCount;
    lastLogin = request;
    if (p->fail != Fail::None) return id;
    LoginReply& reply = p->login;
    if (request.haveToken()) {
        std::map<std::string, int32_t>::const_iterator it = m_tokens.find(request.token);
        if (it == m_tokens.end()) { p->fail = Fail::Auth; p->http = 401; return id; }
        reply.profileId = it->second;
    } else {
        static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-";
        reply.profileId = m_nextProfile++;
        for (uint32_t i = 0; i < 43; ++i) {   // the service's token: 43 characters of [A-Za-z0-9_-]
            m_rng ^= m_rng << 13; m_rng ^= m_rng >> 17; m_rng ^= m_rng << 5;
            reply.token[i] = alphabet[(m_rng >> 8) & 63];
        }
        reply.token[43] = 0;
        reply.newToken = true;
        m_tokens[reply.token] = reply.profileId;
    }
    m_names[reply.profileId] = request.name;
    m_session = reply.profileId;
    m_lastProfile = reply.profileId;
    reply.nameCheck = m_nameCheck;
    if (m_nameCheck == NameCheck::Invalid) {   // like the service: no account is created for a refused name
        if (reply.newToken) { m_tokens.erase(reply.token); --m_nextProfile; }
        m_names.erase(reply.profileId);
        reply.profileId = 0;
        reply.newToken = false;
        reply.token[0] = 0;
        m_session = 0;
    }
    reply.serverTimeMs = NowServerMs();
    if (reply.profileId > 0) snprintf(reply.code, sizeof reply.code, "FAKE-%04d", int(reply.profileId % 10000));
    if (reply.profileId > 0 && !m_ownSeeds.empty()) {
        uint8_t data[128];
        FakeMakeUserData(data, request.name, 7, 3, int(request.region & 0xFF), 1, 1, 2026);
        for (size_t i = 0; i < m_ownSeeds.size(); ++i)
            SeedRaw(reply.profileId, m_ownSeeds[i].first, m_ownSeeds[i].second, request.region ? request.region : 2, 1, data, sizeof data);
    }
    return id;
}

uint32_t FakeApi::BeginTime() {
    Pending* p;
    const uint32_t id = Start(2, &p);
    if (!id) return 0;
    ++timeCount;
    p->time.unixMs = NowServerMs();
    return id;
}

uint32_t FakeApi::BeginPutScore(const PutRequest& request) {
    Pending* p;
    const uint32_t id = Start(3, &p);
    if (!id) return 0;
    ++putCount;
    lastPut = request;
    puts.push_back(request);
    hasLastPut = true;
    if (p->fail != Fail::None) return id;
    if (!m_session) { p->fail = Fail::Auth; p->http = 401; return id; }
    if (request.size > kMaxRowData || request.category > 100) { p->fail = Fail::Rejected; p->http = 400; return id; }
    SeedRaw(m_session, request.category, m_clamp ? m_clampStored : request.score, request.region, 0, request.data, request.size);
    p->put.stored = m_clamp ? m_clampStored : request.score;
    p->put.clamped = m_clamp;
    return id;
}

uint32_t FakeApi::BeginReportResult(const ResultReport& report) {
    Pending* p;
    const uint32_t id = Start(5, &p);
    if (!id) return 0;
    ++reportCount;
    reports.push_back(report);
    if (p->fail != Fail::None) return id;
    if (!m_session) { p->fail = Fail::Auth; p->http = 401; return id; }
    return id;
}

uint32_t FakeApi::BeginMatchStatus(const char* matchId) {
    Pending* p;
    const uint32_t id = Start(6, &p);
    if (!id) return 0;
    ++statusCount;
    if (p->fail != Fail::None) return id;
    if (!m_session) { p->fail = Fail::Auth; p->http = 401; return id; }
    const bool fin = statusCount > m_matchPolls;
    p->match.final = fin;
    snprintf(p->match.status, sizeof p->match.status, "%s", fin ? "verified" : "awaiting_peer");
    p->match.points = fin ? 13 : 0;
    p->match.won = fin;
    (void)matchId;
    return id;
}

Result FakeApi::PollMatchStatus(uint32_t id, MatchStatus* status) {
    Result r;
    Pending* p = nullptr;
    if (!Settle(id, 6, &p, &r)) return r;
    if (p->fail != Fail::None) { r.status = Status::Failed; r.fail = p->fail; r.http = p->http; }
    else { r.status = Status::Ok; r.http = 200; if (status) *status = p->match; }
    m_pending.erase(id);
    return r;
}

Result FakeApi::PollReportResult(uint32_t id) {
    Result r;
    Pending* p = nullptr;
    if (!Settle(id, 5, &p, &r)) return r;
    if (p->fail != Fail::None) { r.status = Status::Failed; r.fail = p->fail; r.http = p->http; }
    else { r.status = Status::Ok; r.http = 200; }
    m_pending.erase(id);
    return r;
}

// ---- friends (the model of the service's friends.js: mutual links, blocks, derived presence, invites) -----------------------------------------
namespace {
void CopyStr(char* dst, size_t size, const std::string& v) { snprintf(dst, size, "%s", v.c_str()); }
}

int32_t FakeApi::FakeAddPerson(const char* name) {
    const int32_t pid = m_nextProfile++;
    Person p;
    p.name = name;
    char code[16];
    snprintf(code, sizeof code, "FAKE-%04d", int(pid % 10000));
    p.code = code;
    p.presence = Presence::Menu;
    p.day = NowServerMs() / 86400000;
    m_people[pid] = p;
    return pid;
}
void FakeApi::FakeSetPresence(int32_t pid, Presence presence) { if (m_people.count(pid)) m_people[pid].presence = presence; }
void FakeApi::FakeSetStatus(int32_t pid, const char* status) { if (m_people.count(pid)) m_people[pid].status = status; }
void FakeApi::FakeSetOpenRoom(int32_t pid, const char* roomCode) { if (m_people.count(pid)) m_people[pid].room = roomCode; }
void FakeApi::FakePersonAdds(int32_t pid) { if (m_session && !m_blocks.count(std::make_pair(m_session, pid))) m_links.insert(std::make_pair(pid, m_session)); }
void FakeApi::FakePersonInvites(int32_t pid, const char* roomCode) {
    if (m_session && m_links.count(std::make_pair(pid, m_session)) && m_links.count(std::make_pair(m_session, pid)) && !m_blocks.count(std::make_pair(m_session, pid)))
        m_invites[std::make_pair(m_session, pid)] = roomCode;
}
void FakeApi::FakePlayedWith(int32_t pid, bool ranked) { m_played.push_back(std::make_pair(pid, ranked)); }

uint32_t FakeApi::BeginFriends(const FriendsRequest& request) {
    Pending* p;
    const uint32_t id = Start(7, &p);
    if (!id) return 0;
    ++friendsCount;
    lastFriendsWaitSec = request.waitSec;
    if (p->fail != Fail::None) return id;
    if (!m_session) { p->fail = Fail::Auth; p->http = 401; return id; }
    FriendsReply& r = p->friends;
    std::string rev;
    r.max = 64;
    CopyStr(r.ownCode, sizeof r.ownCode, "FAKE-0000");
    std::set<int32_t> ids;
    for (std::set<std::pair<int32_t, int32_t> >::const_iterator it = m_links.begin(); it != m_links.end(); ++it) {
        if (it->first == m_session) ids.insert(it->second);
        else if (it->second == m_session && !m_blocks.count(std::make_pair(m_session, it->first))) ids.insert(it->first);
    }
    for (std::set<int32_t>::const_iterator it = ids.begin(); it != ids.end(); ++it) {
        std::map<int32_t, Person>::const_iterator pp = m_people.find(*it);
        if (pp == m_people.end()) continue;
        const bool mine = m_links.count(std::make_pair(m_session, *it)) != 0, theirs = m_links.count(std::make_pair(*it, m_session)) != 0;
        FriendRow row;
        row.pid = *it;
        CopyStr(row.code, sizeof row.code, pp->second.code);
        CopyStr(row.name, sizeof row.name, pp->second.name);
        row.state = mine && theirs ? FriendState::Friend : mine ? FriendState::Outgoing : FriendState::Incoming;
        if (row.state == FriendState::Friend) {
            row.presence = pp->second.presence;
            row.lastSeenDay = pp->second.day;
            CopyStr(row.status, sizeof row.status, pp->second.status);
            CopyStr(row.roomCode, sizeof row.roomCode, pp->second.room);
            std::map<std::pair<int32_t, int32_t>, std::string>::const_iterator inv = m_invites.find(std::make_pair(m_session, *it));
            if (inv != m_invites.end()) CopyStr(row.inviteRoom, sizeof row.inviteRoom, inv->second);
        }
        char one[640];
        snprintf(one, sizeof one, "%d:%d:%d:%s:%s:%s;", int(row.pid), int(row.state), int(row.presence), row.status, row.roomCode, row.inviteRoom);
        rev += one;
        r.rows.push_back(row);
    }
    for (std::set<std::pair<int32_t, int32_t> >::const_iterator it = m_blocks.begin(); it != m_blocks.end(); ++it) {
        if (it->first != m_session) continue;
        std::map<int32_t, Person>::const_iterator pp = m_people.find(it->second);
        if (pp == m_people.end()) continue;
        FriendRow row;
        row.pid = it->second;
        CopyStr(row.code, sizeof row.code, pp->second.code);
        CopyStr(row.name, sizeof row.name, pp->second.name);
        r.blocked.push_back(row);
        rev += "b" + std::to_string(row.pid) + ";";
    }
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < rev.size(); ++i) { h ^= uint8_t(rev[i]); h *= 16777619u; }
    snprintf(r.rev, sizeof r.rev, "%x", h);
    if (request.rev[0] && strcmp(request.rev, r.rev) == 0) { r.changed = false; r.rows.clear(); r.blocked.clear(); }
    else r.changed = true;
    return id;
}

uint32_t FakeApi::BeginPlayedWith() {
    Pending* p;
    const uint32_t id = Start(8, &p);
    if (!id) return 0;
    ++playedCount;
    if (p->fail != Fail::None) return id;
    if (!m_session) { p->fail = Fail::Auth; p->http = 401; return id; }
    for (size_t i = m_played.size(); i-- > 0 && p->played.rows.size() < 20;) {
        std::map<int32_t, Person>::const_iterator pp = m_people.find(m_played[i].first);
        if (pp == m_people.end() || m_blocks.count(std::make_pair(m_session, m_played[i].first))) continue;
        PlayedRow row;
        row.pid = m_played[i].first;
        CopyStr(row.code, sizeof row.code, pp->second.code);
        CopyStr(row.name, sizeof row.name, pp->second.name);
        row.wasRanked = m_played[i].second;
        row.isFriend = m_links.count(std::make_pair(m_session, row.pid)) != 0;
        row.day = pp->second.day;
        p->played.rows.push_back(row);
    }
    return id;
}

uint32_t FakeApi::BeginFriendOp(const FriendOpRequest& request) {
    Pending* p;
    const uint32_t id = Start(9, &p);
    if (!id) return 0;
    ++friendOpCount;
    friendOps.push_back(request);
    p->code[0] = 0;
    if (p->fail != Fail::None) return id;
    if (!m_session) { p->fail = Fail::Auth; p->http = 401; return id; }
    if (request.op == FriendOp::SetStatus) { m_ownStatus = request.status; return id; }
    int32_t pid = request.pid;
    if (pid <= 0) {
        for (std::map<int32_t, Person>::const_iterator it = m_people.begin(); it != m_people.end(); ++it)
            if (it->second.code == request.code) pid = it->first;
    }
    std::map<int32_t, Person>::const_iterator pp = m_people.find(pid);
    if (pp == m_people.end()) { p->fail = Fail::Rejected; p->http = 404; snprintf(p->code, sizeof p->code, "player_not_found"); return id; }
    if (pid == m_session) { p->fail = Fail::Rejected; p->http = 400; snprintf(p->code, sizeof p->code, "bad_friend"); return id; }
    const std::pair<int32_t, int32_t> mine(m_session, pid), theirs(pid, m_session);
    switch (request.op) {
    case FriendOp::Add: {
        if (m_blocks.count(mine)) { p->fail = Fail::Rejected; p->http = 409; snprintf(p->code, sizeof p->code, "blocked"); return id; }
        if (!m_links.count(mine)) {
            uint32_t n = 0;
            for (std::set<std::pair<int32_t, int32_t> >::const_iterator it = m_links.begin(); it != m_links.end(); ++it) n += it->first == m_session;
            if (n >= 64) { p->fail = Fail::Rejected; p->http = 409; snprintf(p->code, sizeof p->code, "friends_full"); return id; }
            m_links.insert(mine);
        }
        p->fop.state = m_links.count(theirs) && !m_blocks.count(theirs) ? FriendState::Friend : FriendState::Outgoing;
        p->fop.pid = pid;
        CopyStr(p->fop.code, sizeof p->fop.code, pp->second.code);
        CopyStr(p->fop.name, sizeof p->fop.name, pp->second.name);
        break;
    }
    case FriendOp::Decline: m_links.erase(theirs); break;
    case FriendOp::Remove: m_links.erase(mine); m_links.erase(theirs); m_invites.erase(std::make_pair(m_session, pid)); break;
    case FriendOp::Block: m_blocks.insert(mine); m_links.erase(mine); m_links.erase(theirs); m_invites.erase(std::make_pair(m_session, pid)); break;
    case FriendOp::Unblock: m_blocks.erase(mine); break;
    case FriendOp::Invite:
        if (!m_links.count(mine) || !m_links.count(theirs)) { p->fail = Fail::Rejected; p->http = 403; snprintf(p->code, sizeof p->code, "not_friends"); return id; }
        break;
    case FriendOp::ClearInvite: m_invites.erase(std::make_pair(m_session, pid)); break;
    default: break;
    }
    return id;
}

Result FakeApi::PollFriends(uint32_t id, FriendsReply* reply) {
    Result r;
    Pending* p = nullptr;
    if (!Settle(id, 7, &p, &r)) return r;
    if (p->fail != Fail::None) { r.status = Status::Failed; r.fail = p->fail; r.http = p->http; }
    else { r.status = Status::Ok; r.http = 200; if (reply) *reply = p->friends; }
    m_pending.erase(id);
    return r;
}
Result FakeApi::PollPlayedWith(uint32_t id, PlayedReply* reply) {
    Result r;
    Pending* p = nullptr;
    if (!Settle(id, 8, &p, &r)) return r;
    if (p->fail != Fail::None) { r.status = Status::Failed; r.fail = p->fail; r.http = p->http; }
    else { r.status = Status::Ok; r.http = 200; if (reply) *reply = p->played; }
    m_pending.erase(id);
    return r;
}
Result FakeApi::PollFriendOp(uint32_t id, FriendOpReply* reply) {
    Result r;
    Pending* p = nullptr;
    if (!Settle(id, 9, &p, &r)) return r;
    if (p->fail != Fail::None) { r.status = Status::Failed; r.fail = p->fail; r.http = p->http; snprintf(r.code, sizeof r.code, "%s", p->code); }
    else { r.status = Status::Ok; r.http = 200; if (reply) *reply = p->fop; }
    m_pending.erase(id);
    return r;
}

void FakeApi::Build(const GetRequest& req, GetReply* out) {
    out->mode = req.mode;
    const uint32_t now = NowMinutes();
    std::vector<Entry> eligible;
    std::map<uint32_t, std::vector<Entry> >::const_iterator board = m_boards.find(req.category);
    if (board != m_boards.end()) {
        for (size_t i = 0; i < board->second.size(); ++i) {
            const Entry& e = board->second[i];
            if (!(e.region & req.region)) continue;
            if (req.since && now - e.lastupdate > req.since) continue;
            if (req.mode == 3) {
                bool listed = e.pid == m_session;
                for (uint32_t f = 0; f < req.friendCount && !listed; ++f) listed = req.friends[f] == e.pid;
                if (!listed) continue;
            }
            eligible.push_back(e);
        }
    }
    if (m_synthOwn && m_session > 0 && (req.mode == 2 || req.mode == 3)) {   // like the service: a caller without a row gets a zero row of its own
        bool have = false;
        for (size_t i = 0; i < eligible.size(); ++i) have = have || eligible[i].pid == m_session;
        if (!have) {
            Entry e;
            memset(&e, 0, sizeof e);
            e.pid = m_session;
            e.region = req.region ? req.region : 2;
            e.lastupdate = now;
            e.size = 128;
            eligible.push_back(e);
        }
    }
    const bool desc = req.sort != 0;
    std::sort(eligible.begin(), eligible.end(), [desc](const Entry& a, const Entry& b) {
        if (a.score != b.score) return desc ? a.score > b.score : a.score < b.score;
        return a.pid < b.pid;
    });
    std::vector<uint32_t> rank(eligible.size());
    for (size_t i = 0; i < eligible.size(); ++i) rank[i] = (i && eligible[i].score == eligible[i - 1].score) ? rank[i - 1] : uint32_t(i + 1);
    out->total = uint32_t(eligible.size());
    size_t own = eligible.size();
    for (size_t i = 0; i < eligible.size(); ++i)
        if (eligible[i].pid == m_session) own = i;
    size_t first = 0, count = 0;
    switch (req.mode) {
    case 0:   // ORDER
        out->order = own < eligible.size() ? rank[own] : 0;
        return;
    case 1:   // TOPLIST
    case 3:   // FRIENDS
        first = 0;
        count = std::min<size_t>(req.limit, eligible.size());
        break;
    case 2:   // NEAR: a window of `limit` rows around the player; nothing when the player has no score
        if (own < eligible.size()) {
            count = std::min<size_t>(req.limit, eligible.size());
            const size_t half = req.limit / 2;
            first = own > half ? own - half : 0;
            if (first + count > eligible.size()) first = eligible.size() - count;
        }
        break;
    }
    for (size_t i = first; i < first + count; ++i) {
        Row row;
        row.order = rank[i];
        row.pid = eligible[i].pid;
        row.score = eligible[i].score;
        row.region = eligible[i].region;
        row.lastupdate = eligible[i].lastupdate;
        row.size = eligible[i].size;
        memcpy(row.data, eligible[i].data, std::min(row.size, kMaxRowData));
        out->rows.push_back(row);
    }
}

uint32_t FakeApi::BeginGetScores(const GetRequest& request) {
    Pending* p;
    const uint32_t id = Start(4, &p);
    if (!id) return 0;
    ++getCount;
    lastGet = request;
    if (p->fail != Fail::None) return id;
    if (!m_session) { p->fail = Fail::Auth; p->http = 401; return id; }
    Build(request, &p->get);
    if (m_tamper) m_tamper(&p->get, m_tamperUser);
    return id;
}

Result FakeApi::PollLogin(uint32_t id, LoginReply* reply) {
    Result r;
    Pending* p = nullptr;
    if (!Settle(id, 1, &p, &r)) return r;
    if (p->fail != Fail::None) { r.status = Status::Failed; r.fail = p->fail; r.http = p->http; }
    else { r.status = Status::Ok; r.http = 200; if (reply) *reply = p->login; }
    m_pending.erase(id);
    return r;
}

Result FakeApi::PollTime(uint32_t id, TimeReply* reply) {
    Result r;
    Pending* p = nullptr;
    if (!Settle(id, 2, &p, &r)) return r;
    if (p->fail != Fail::None) { r.status = Status::Failed; r.fail = p->fail; r.http = p->http; }
    else { r.status = Status::Ok; r.http = 200; p->time.unixMs = NowServerMs(); if (reply) *reply = p->time; }
    m_pending.erase(id);
    return r;
}

Result FakeApi::PollPutScore(uint32_t id, PutReply* reply) {
    Result r;
    Pending* p = nullptr;
    if (!Settle(id, 3, &p, &r)) return r;
    if (p->fail != Fail::None) { r.status = Status::Failed; r.fail = p->fail; r.http = p->http; }
    else { r.status = Status::Ok; r.http = 200; if (reply) *reply = p->put; }
    m_pending.erase(id);
    return r;
}

Result FakeApi::PollGetScores(uint32_t id, GetReply* reply) {
    Result r;
    Pending* p = nullptr;
    if (!Settle(id, 4, &p, &r)) return r;
    if (p->fail != Fail::None) { r.status = Status::Failed; r.fail = p->fail; r.http = p->http; }
    else { r.status = Status::Ok; r.http = 200; if (reply) *reply = p->get; }
    m_pending.erase(id);
    return r;
}

void FakeApi::Cancel(uint32_t id) { m_pending.erase(id); }

void FakeApi::SeedOwnScore(uint32_t category, int32_t score) { m_ownSeeds.push_back(std::make_pair(category, score)); }

void FakeApi::SynthesizeOwnRow(bool on) { m_synthOwn = on; }

void FakeApi::FailFrom(uint64_t nowMs) { m_failFromMs = nowMs; }

void FakeApi::FailWhen(Fail fail, int http, int kinds) { m_failWhenKind = fail; m_failWhenHttp = http; m_failWhenMask = kinds; }

void FakeApi::SeedBigRoster() {
    // names: the longest the game takes (10), the narrowest and widest glyph runs, every punctuation the service allows, digits only, two-character names
    static const char* const names[30] = {
        "Maxlen1234", "WWWWWWWWWW", "iiiiiiiiii", "MMMMMMMMMM", "W.W.W.W.W", "a_b-c.d_e", "1234567890", "Zz", "00", "Mario 64",
        "Toad Fan", "peach", "DAISY", "luigi.mp", "Bowser-Jr", "Yoshi_007", "Wario", "Waluigi 2", "DK.Rules", "Birdo",
        "Shy Guy", "Koopa-K", "Boo.Boo", "Petey P", "Kid_Goal", "Strikers1", "Mid Field", "Keeper 1", "Last.Place", "x.y"};
    // scores per board (season boards 0, 2, 3, 5; daily boards 1 and 4): descending with ties at 700, 450 and 100
    static const int base[30] = {980, 940, 900, 860, 820, 780, 740, 700, 700, 700, 660, 620, 580, 540, 500, 450, 450, 410, 370, 330,
                                 290, 250, 210, 170, 130, 100, 100, 60, 20, 1};
    for (uint32_t cat = 0; cat < 6; ++cat) {
        const bool daily = cat == 1 || cat == 4;
        for (int i = 0; i < 30; ++i) {
            if (daily && i % 3 == 2) continue;                          // not everybody played today
            if (cat == 5) continue;                                      // an empty board
            const int score = daily ? base[(i * 2) % 30] / 2 + 1 : (cat == 2 ? base[(i * 7) % 30] : cat == 3 ? base[29 - i] + i : base[i]);
            const uint32_t region = 1u << (i % 3);                        // JP, US, EU
            SeedPlayer(names[i], cat, score, 0x07, 3 + 25 * uint32_t(i), 30 - i, 3 + (i * 5) % 17);
            (void)region;
        }
    }
}

FakeOptions FakeOptionsFromEnvironment() {
    FakeOptions options;
    if (const char* roster = getenv("MSC_ONLINE_FAKE_ROSTER")) if (!strcmp(roster, "big")) options.seedPlayers = false;
    if (const char* t = getenv("MSC_ONLINE_FAKE_TIME")) if (*t) options.serverTimeMs = int64_t(strtoll(t, nullptr, 10)) * 1000;
    return options;
}

void FakeApplyEnvironment(Api* api) {
    FakeApi* fake = static_cast<FakeApi*>(api);
    if (const char* roster = getenv("MSC_ONLINE_FAKE_ROSTER")) if (!strcmp(roster, "big")) fake->SynthesizeOwnRow(true);
    if (const char* roster = getenv("MSC_ONLINE_FAKE_ROSTER")) if (!strcmp(roster, "big")) fake->SeedBigRoster();
    if (const char* own = getenv("MSC_ONLINE_FAKE_OWN")) {
        const char* p = own;
        while (*p) {
            char* end;
            const long cat = strtol(p, &end, 10);
            if (*end != ':') break;
            const long score = strtol(end + 1, &end, 10);
            fake->SeedOwnScore(uint32_t(cat), int32_t(score));
            p = end;
            while (*p == ';' || *p == ',' || *p == ' ') ++p;
        }
    }
    if (const char* f = getenv("MSC_ONLINE_FAKE_FAIL")) if (*f) {
        char kind[24] = "";
        char what[16] = "rank";
        const char* colon = strchr(f, ':');
        snprintf(kind, sizeof kind, "%.*s", colon ? int(colon - f) : 23, f);
        if (colon) snprintf(what, sizeof what, "%s", colon + 1);
        Fail fail = Fail::Transport;
        int http = 0;
        if (!strcmp(kind, "timeout")) fail = Fail::Timeout;
        else if (!strcmp(kind, "server")) { fail = Fail::Server; http = 503; }
        else if (!strcmp(kind, "auth")) { fail = Fail::Auth; http = 401; }
        else if (!strcmp(kind, "upgrade")) { fail = Fail::Upgrade; http = 426; }
        else if (!strcmp(kind, "disabled")) { fail = Fail::Disabled; http = 503; }
        else if (!strcmp(kind, "ratelimit")) { fail = Fail::RateLimited; http = 429; }
        const int mask = !strcmp(what, "login") ? 1 : !strcmp(what, "all") ? 31 : 8;
        fake->FailWhen(fail, http, mask);
        if (const char* from = getenv("MSC_ONLINE_FAKE_FAIL_FROM")) if (*from) fake->FailFrom(NowMs() + uint64_t(strtoull(from, nullptr, 10)) * 1000);
    }
}

Api* CreateFakeApi(const FakeOptions* options) {
    FakeOptions defaults;
    return new FakeApi(options ? *options : defaults);
}

}  // namespace NpOnline
