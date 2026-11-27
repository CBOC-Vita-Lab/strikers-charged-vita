// The persistent retry queue. See np_online_queue.h.
#include "platform/sdk/dwc/np_online_queue.h"
#include "platform/sdk/net/online/np_service_busy.h"

// (mpbeta27) a ranked result's upload goes to the problem report (np_mpdiag.h NoteResultUpload: accepted, kept for a retry, dropped; a dropped one offers a report).
// Weak: the small tests link without np_mpdiag.cpp.
extern "C" void NpMpDiagResultUpload(int attempts, int outcome, const char* word) __attribute__((weak));
extern "C" void NpMpDiagReach(int stage) __attribute__((weak));

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <vector>

#include "platform/sdk/dwc/np_dwc.h"
#include "platform/sdk/save/np_safe_file.h"
#include "np/host_alloc.h"

#define QUEUE_LOG(...) fprintf(stderr, "[online] " __VA_ARGS__)

namespace NpOnline {
namespace {

const uint32_t kMaxEntries = 16;
const uint32_t kMaxAttempts = 24;
const uint32_t kMaxAgeSec = 24u * 3600u;
const uint32_t kSendTimeoutMs = 15000;
const uint32_t kKindPut = 1, kKindResult = 2, kKindStatus = 3;   // kKindStatus: a flight only (polling the match record)
const uint32_t kFinalWaitMs = 5u * 60u * 1000u;                   // how long a reported match may stay unsettled before it stops holding scores back
const uint32_t kStatusPollMs = 2000;

#pragma pack(push, 4)
struct Disk {   // one entry on disk; every byte is initialised (memset) so the file is deterministic
    uint32_t kind, category, region;
    int32_t score;
    uint32_t size;
    uint8_t data[kMaxRowData];
    char matchId[32];
    int32_t result, score0, score1;
    char checksum[72];
    char mode[12];
    uint32_t attempts;
    char build[20];
    uint32_t created;
    uint32_t stage;       // results: 0 = to be reported, 1 = reported, waiting for the match to become final
};
#pragma pack(pop)

struct Entry {
    Disk d;
    uint32_t id;
    uint64_t dueMs;     // monotonic ms, memory only
    uint64_t waitSince; // when the match was reported (stage 1), memory only
    bool held;          // the DWC ranking client is sending it itself
};

pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
np::HostVector<Entry> g_entries;   // (1.3: on the host heap: grown on the game thread, kept for the session; np/host_alloc.h)
bool g_loaded = false;
uint32_t g_nextId = 0;

struct Flight { uint32_t kind, apiId, entryId; uint64_t deadline; } g_flight = {0, 0, 0, 0};

bool Path(char* out, size_t size, const char* suffix) {
    const int n = snprintf(out, size, "%s/online_queue.dat%s", DataDir(), suffix);
    return n > 0 && size_t(n) < size;
}

void Put32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24); }
uint32_t Get32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

uint32_t WallSec() { return uint32_t(time(nullptr)); }

// magic[4] version[4] count[4] entries crc[4]
// Replaced and recovered by np_safe_file.h (synced; a power cut leaves the old queue or the new one; a damaged file is set aside as online_queue.dat.bad).
bool QueueSound(const std::string& d) {
    const uint8_t* b = reinterpret_cast<const uint8_t*>(d.data());
    const size_t n = d.size();
    if (n < 16 || memcmp(b, "MSCQ", 4) || Get32(b + 4) != 1) return false;
    const uint32_t count = Get32(b + 8);
    return count != 0 && count <= kMaxEntries && n == 12 + count * sizeof(Disk) + 4 && Get32(b + n - 4) == NpDwcCrc32(b, n - 4);
}
NpSafeFile::Options QueueOptions() { NpSafeFile::Options o; o.valid = QueueSound; o.mode = 0600; o.maxBytes = 12 + kMaxEntries * sizeof(Disk) + 64; return o; }

bool SaveLocked() {
    char path[512];
    if (!Path(path, sizeof path, "")) return false;
    if (g_entries.empty()) { NpSafeFile::RemoveAll(path); return true; }
    const size_t bytes = 12 + g_entries.size() * sizeof(Disk) + 4;
    std::vector<uint8_t> buf(bytes, 0);
    memcpy(&buf[0], "MSCQ", 4);
    Put32(&buf[4], 1);
    Put32(&buf[8], uint32_t(g_entries.size()));
    for (size_t i = 0; i < g_entries.size(); ++i) memcpy(&buf[12 + i * sizeof(Disk)], &g_entries[i].d, sizeof(Disk));
    Put32(&buf[bytes - 4], NpDwcCrc32(&buf[0], bytes - 4));
    return NpSafeFile::Replace(path, std::string(reinterpret_cast<const char*>(&buf[0]), bytes), QueueOptions());
}

void LoadLocked() {
    if (g_loaded) return;
    g_loaded = true;
    g_entries.clear();
    char path[512];
    if (!Path(path, sizeof path, "")) return;
    NpSafeFile::Recover(path, QueueOptions());
    FILE* f = fopen(path, "rb");
    if (!f) return;
    std::vector<uint8_t> buf(12 + kMaxEntries * sizeof(Disk) + 8);
    const size_t n = fread(&buf[0], 1, buf.size(), f);
    fclose(f);
    if (n < 16 || memcmp(&buf[0], "MSCQ", 4) || Get32(&buf[4]) != 1) return;
    const uint32_t count = Get32(&buf[8]);
    if (count == 0 || count > kMaxEntries || n != 12 + count * sizeof(Disk) + 4 || Get32(&buf[n - 4]) != NpDwcCrc32(&buf[0], n - 4)) {
        QUEUE_LOG("the retry queue file is damaged; ignored\n");
        return;
    }
    const uint32_t now = WallSec();
    for (uint32_t i = 0; i < count; ++i) {
        Entry e;
        memset(&e, 0, sizeof e);
        memcpy(&e.d, &buf[12 + i * sizeof(Disk)], sizeof(Disk));
        e.d.matchId[sizeof e.d.matchId - 1] = 0;
        e.d.checksum[sizeof e.d.checksum - 1] = 0;
        e.d.mode[sizeof e.d.mode - 1] = 0;
        e.d.build[sizeof e.d.build - 1] = 0;
        if ((e.d.kind != kKindPut && e.d.kind != kKindResult) || e.d.size > kMaxRowData) continue;
        if (e.d.created > 1000000000u && now > e.d.created && now - e.d.created > kMaxAgeSec) continue;
        e.id = ++g_nextId;
        e.dueMs = 0;
        e.waitSince = 0;
        g_entries.push_back(e);
    }
    if (!g_entries.empty()) QUEUE_LOG("retry queue: %u entr%s from an earlier run\n", unsigned(g_entries.size()), g_entries.size() == 1 ? "y" : "ies");
}

Entry* Find(uint32_t id) {
    for (size_t i = 0; i < g_entries.size(); ++i)
        if (g_entries[i].id == id) return &g_entries[i];
    return nullptr;
}

void Remove(uint32_t id) {
    for (size_t i = 0; i < g_entries.size(); ++i)
        if (g_entries[i].id == id) { g_entries.erase(g_entries.begin() + long(i)); return; }
}

uint64_t BackoffMs(uint32_t attempts) {
    static const uint32_t steps[4] = {1000, 2000, 4000, 8000};
    return attempts >= 1 && attempts <= 4 ? steps[attempts - 1] : 15000;
}

void CopyBuild(char* out, size_t size) { snprintf(out, size, "%s", GetClientInfo().build); }

PutRequest ToPut(const Entry& e) {
    PutRequest r;
    r.category = e.d.category;
    r.region = e.d.region;
    r.score = e.d.score;
    r.size = e.d.size;
    memcpy(r.data, e.d.data, e.d.size);
    r.attempt = e.d.attempts + 1;
    snprintf(r.build, sizeof r.build, "%s", e.d.build);
    return r;
}

ResultReport ToResult(const Entry& e) {
    ResultReport r;
    snprintf(r.matchId, sizeof r.matchId, "%s", e.d.matchId);
    r.result = e.d.result;
    r.score0 = e.d.score0;
    r.score1 = e.d.score1;
    snprintf(r.checksum, sizeof r.checksum, "%s", e.d.checksum);
    snprintf(r.mode, sizeof r.mode, "%s", e.d.mode);
    r.attempt = e.d.attempts + 1;
    snprintf(r.build, sizeof r.build, "%s", e.d.build);
    return r;
}

const char* FailWord(Fail f) {
    switch (f) {
    case Fail::None: return "ok";
    case Fail::Transport: return "no_connection";
    case Fail::Timeout: return "timeout";
    case Fail::Server: return "server_error";
    case Fail::Rejected: return "refused";
    case Fail::Auth: return "account_refused";
    case Fail::Protocol: return "bad_answer";
    case Fail::Cancelled: return "cancelled";
    case Fail::RateLimited: return "rate_limited";
    case Fail::Upgrade: return "update_required";
    case Fail::Disabled: return "service_off";
    }
    return "other";
}

void FinishLocked(uint32_t id, Fail fail) {
    Entry* e = Find(id);
    if (!e) return;
    e->held = false;
    const bool result = e->d.kind == kKindResult;
    if (fail == Fail::None) {
        if (result && NpMpDiagResultUpload) NpMpDiagResultUpload(int(e->d.attempts) + 1, 1, "settled");
        Remove(id);
        SaveLocked();
        return;
    }
    const uint32_t busyMs = NpNet::ServiceBusyRemainingMs();
    if (busyMs && (fail == Fail::Disabled || fail == Fail::Server)) {
        // the service is over its daily budget: wait what it said (at most a minute at a time) and do not use up the attempts; the 24 hour limit still applies
        e->dueMs = NowMs() + (busyMs < 60000 ? busyMs : 60000);
        SaveLocked();
        return;
    }
    ++e->d.attempts;
    if (!QueueIsRetryable(fail) || e->d.attempts >= kMaxAttempts) {
        QUEUE_LOG("retry queue: %s dropped after %u attempt(s) (reason %d)\n", e->d.kind == kKindPut ? "a score" : "a match result", unsigned(e->d.attempts), int(fail));
        if (result && NpMpDiagResultUpload) NpMpDiagResultUpload(int(e->d.attempts), -1, FailWord(fail));
        Remove(id);
    } else {
        if (result && NpMpDiagResultUpload) NpMpDiagResultUpload(int(e->d.attempts), 0, FailWord(fail));
        e->dueMs = NowMs() + BackoffMs(e->d.attempts);
    }
    SaveLocked();
}

}  // namespace

bool QueueIsRetryable(Fail fail) { return fail != Fail::Rejected && fail != Fail::Protocol; }

uint32_t QueuePut(const PutRequest& request) {
    pthread_mutex_lock(&g_lock);
    LoadLocked();
    Entry e;
    memset(&e, 0, sizeof e);
    e.d.kind = kKindPut;
    e.d.category = request.category;
    e.d.region = request.region;
    e.d.score = request.score;
    e.d.size = request.size > kMaxRowData ? kMaxRowData : request.size;
    memcpy(e.d.data, request.data, e.d.size);
    CopyBuild(e.d.build, sizeof e.d.build);
    e.d.created = WallSec();
    e.held = true;
    for (size_t i = 0; i < g_entries.size(); ++i) {   // a newer total replaces an undelivered older one
        Entry& old = g_entries[i];
        if (old.d.kind == kKindPut && old.d.category == e.d.category && old.d.region == e.d.region && !old.held) {
            g_entries.erase(g_entries.begin() + long(i));
            break;
        }
    }
    if (g_entries.size() >= kMaxEntries) g_entries.erase(g_entries.begin());   // the oldest goes
    e.id = ++g_nextId;
    g_entries.push_back(e);
    const bool saved = SaveLocked();
    const uint32_t id = saved ? e.id : 0;
    if (!saved) { Remove(e.id); QUEUE_LOG("retry queue: the entry could not be stored\n"); }
    pthread_mutex_unlock(&g_lock);
    return id;
}

bool QueueResult(const ResultReport& report) {
    if (!report.matchId[0] || report.score0 < 0 || report.score0 > 99 || report.score1 < 0 || report.score1 > 99) return false;
    pthread_mutex_lock(&g_lock);
    LoadLocked();
    Entry e;
    memset(&e, 0, sizeof e);
    e.d.kind = kKindResult;
    snprintf(e.d.matchId, sizeof e.d.matchId, "%s", report.matchId);
    e.d.result = report.result;
    e.d.score0 = report.score0;
    e.d.score1 = report.score1;
    snprintf(e.d.checksum, sizeof e.d.checksum, "%s", report.checksum);
    snprintf(e.d.mode, sizeof e.d.mode, "%s", report.mode);
    CopyBuild(e.d.build, sizeof e.d.build);
    e.d.created = WallSec();
    for (size_t i = 0; i < g_entries.size(); ++i)
        if (g_entries[i].d.kind == kKindResult && !strcmp(g_entries[i].d.matchId, e.d.matchId)) { g_entries.erase(g_entries.begin() + long(i)); break; }
    if (g_entries.size() >= kMaxEntries) g_entries.erase(g_entries.begin());
    e.id = ++g_nextId;
    e.dueMs = 0;
    g_entries.push_back(e);
    SaveLocked();
    pthread_mutex_unlock(&g_lock);
    if (NpMpDiagReach) NpMpDiagReach(10);   // (mpbeta27) np_mpdiag.h kStageResultUpload
    return true;
}

void QueueFinished(uint32_t id, Fail fail) {
    if (!id) return;
    pthread_mutex_lock(&g_lock);
    FinishLocked(id, fail);
    pthread_mutex_unlock(&g_lock);
}

void QueueKick() {
    pthread_mutex_lock(&g_lock);
    LoadLocked();
    for (size_t i = 0; i < g_entries.size(); ++i) g_entries[i].dueMs = 0;
    pthread_mutex_unlock(&g_lock);
}

bool QueueMayPut() {
    pthread_mutex_lock(&g_lock);
    LoadLocked();
    bool may = true;
    for (size_t i = 0; i < g_entries.size(); ++i) may = may && g_entries[i].d.kind != kKindResult;   // PROTOCOL.md section 8: results first, final, then the totals
    pthread_mutex_unlock(&g_lock);
    return may;
}

void QueuePump(bool rankingBusy) {
    pthread_mutex_lock(&g_lock);
    LoadLocked();
    Api* api = GetApi();
    const uint64_t now = NowMs();
    if (g_flight.apiId) {
        Result r;
        MatchStatus match;
        if (g_flight.kind == kKindPut) r = api->PollPutScore(g_flight.apiId, nullptr);
        else if (g_flight.kind == kKindStatus) r = api->PollMatchStatus(g_flight.apiId, &match);
        else r = api->PollReportResult(g_flight.apiId);
        const uint32_t id = g_flight.entryId;
        if (r.status == Status::Pending) {
            if (now >= g_flight.deadline) {
                api->Cancel(g_flight.apiId);
                g_flight.apiId = 0;
                FinishLocked(id, Fail::Timeout);
            }
        } else {
            const uint32_t kind = g_flight.kind;
            g_flight.apiId = 0;
            Entry* e = Find(id);
            if (r.status != Status::Ok) {
                FinishLocked(id, r.fail == Fail::None ? Fail::Protocol : r.fail);
            } else if (kind == kKindResult && e) {   // reported: now wait for the service to settle it
                if (NpMpDiagResultUpload) NpMpDiagResultUpload(int(e->d.attempts) + 1, 1, "reported");
                e->held = false;
                e->d.stage = 1;
                e->d.attempts = 0;
                e->dueMs = now + kStatusPollMs;
                e->waitSince = now;
                SaveLocked();
            } else if (kind == kKindStatus && e) {
                e->held = false;
                if (match.final) {
                    QUEUE_LOG("match %s settled: %s, %d point(s)\n", e->d.matchId, match.status, int(match.points));
                    Remove(id);
                    SaveLocked();
                } else if (e->waitSince && now - e->waitSince > kFinalWaitMs) {
                    QUEUE_LOG("match %s is still not settled after %u s; scores are no longer held back\n", e->d.matchId, unsigned(kFinalWaitMs / 1000));
                    Remove(id);
                    SaveLocked();
                } else {
                    if (!e->waitSince) e->waitSince = now;
                    e->dueMs = now + kStatusPollMs;
                }
            } else {
                FinishLocked(id, Fail::None);
            }
        }
        pthread_mutex_unlock(&g_lock);
        return;
    }
    if (g_entries.empty() || NpDwcLoggedInProfile() <= 0) { pthread_mutex_unlock(&g_lock); return; }
    bool results = false;
    for (size_t i = 0; i < g_entries.size(); ++i) results = results || g_entries[i].d.kind == kKindResult;
    for (size_t i = 0; i < g_entries.size(); ++i) {
        Entry& e = g_entries[i];
        if (e.held || e.dueMs > now) continue;
        uint32_t apiId = 0, kind = e.d.kind;
        if (e.d.kind == kKindResult) {
            if (e.d.stage == 0) apiId = api->BeginReportResult(ToResult(e));
            else { kind = kKindStatus; apiId = api->BeginMatchStatus(e.d.matchId); }
        } else {
            if (rankingBusy || results) continue;                         // totals follow the settled results, one request at a time
            if (!RulesAllowRanked()) { e.dueMs = now + 60000; continue; }   // never submit scores of a modified game
            apiId = api->BeginPutScore(ToPut(e));
        }
        if (!apiId) { FinishLocked(e.id, Fail::Transport); break; }
        g_flight.kind = kind;
        g_flight.apiId = apiId;
        g_flight.entryId = e.id;
        g_flight.deadline = now + kSendTimeoutMs;
        break;
    }
    pthread_mutex_unlock(&g_lock);
}

uint32_t QueuePending() {
    pthread_mutex_lock(&g_lock);
    LoadLocked();
    const uint32_t n = uint32_t(g_entries.size());
    pthread_mutex_unlock(&g_lock);
    return n;
}

bool QueueDescribe(uint32_t index, char* out, size_t size) {
    pthread_mutex_lock(&g_lock);
    LoadLocked();
    bool ok = index < g_entries.size();
    if (ok) {
        const Entry& e = g_entries[index];
        if (e.d.kind == kKindPut) snprintf(out, size, "put cat=%u region=%u attempts=%u", unsigned(e.d.category), unsigned(e.d.region), unsigned(e.d.attempts));
        else snprintf(out, size, "result match=%s %s attempts=%u", e.d.matchId, e.d.stage ? "reported" : "unsent", unsigned(e.d.attempts));
    }
    pthread_mutex_unlock(&g_lock);
    return ok;
}

void QueueForget() { QueueResetForTest(false); }

void QueueResetForTest(bool keepFile) {
    pthread_mutex_lock(&g_lock);
    if (!keepFile) {
        char path[512];
        if (Path(path, sizeof path, "")) NpSafeFile::RemoveAll(path);
    }
    g_entries.clear();
    g_loaded = false;
    g_flight = Flight{0, 0, 0, 0};
    pthread_mutex_unlock(&g_lock);
}

}  // namespace NpOnline
