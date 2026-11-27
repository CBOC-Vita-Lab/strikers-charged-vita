// The seven DWC_Rnk* calls on top of NpOnline::Api: the original's state machine (dwc_ranking.c) with its return values, argument
// limits and result accessors, and a validation layer between the service and the game.
//
// States: NOTREADY -> (Initialize) INITIALIZED -> (Put/Get) PUT_ASYNC / GET_ASYNC -> COMPLETED, ERROR on failure; a new Put/Get is
// accepted from INITIALIZED and COMPLETED. DWC_RnkProcess answers SUCCESS while the request runs and once when it completes, then
// DWC_RNK_PROCESS_NOTASK ("read the results now") until the next request. Where the original hid a failed request behind SUCCESS, this
// returns DWC_RNK_IN_ERROR (and sets the DWC error with a number the game shows); the state stays ERROR, every call answers IN_ERROR,
// until DWC_ClearError (state back to INITIALIZED) or DWC_RnkShutdown. Only this client's own failures count as the error: another
// module's stale DWC error does not block ranking (the original's DWCi_IsError was global).
//
// Rows from the service are untrusted. Before the game sees them: rows without a positive profile id, with a userdata size other than
// the game's 0x80, duplicate profile ids and rows beyond the requested limit are dropped. For NEAR the game (NetworkStats.cpp
// SortLeaderboardResults) reads metadata[-1] unless some row has order >= 1: a reply without such a row (the normal answer for a
// player who has no score yet) gets the player's own placeholder row with order total+1 and no userdata (the game shows an empty row
// and takes the "first-time user" path); rows that come without orders are numbered by score.
#include <dwc/dwc_account.h>
#include <dwc/dwc_error.h>
#include <dwc/dwc_ranking.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <set>

#include "platform/sdk/dwc/np_dwc.h"
#include "platform/sdk/dwc/np_online_queue.h"
#include "np/host_alloc.h"

#define RANK_LOG(...) fprintf(stderr, "[online] " __VA_ARGS__)

namespace {

using namespace NpOnline;

const uint32_t kGameRowBytes = 0x80;   // NetworkRankingSubmission
const uint32_t kHoldMs = 30000;        // longest a put waits for the match to become final

struct Rank {
    DWCRnkState state;
    int32_t pid;
    uint32_t request;
    uint32_t queueId;                   // the retry-queue entry of the put in flight
    bool refused;                       // a put that is refused (modified rules): fails at the next Process
    bool waitingPut;                    // a put held back until the reported match is final (QueueMayPut)
    uint64_t holdDeadline;
    PutRequest pendingPut;
    bool isPut;
    uint64_t deadline;
    GetRequest get;                     // the request in flight (for validation)
    bool hasResponse;
    uint32_t responseMode;
    np::HostVector<Row> rows;           // validated rows of the last Get (1.3: on the host heap; filled on the game thread, kept between Gets: np/host_alloc.h)
    Rank() : state(DWC_RNK_STATE_NOTREADY), pid(0), request(0), queueId(0), refused(false), waitingPut(false), holdDeadline(0), isPut(false), deadline(0), hasResponse(false), responseMode(0) {}
};

pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
Rank g_rank;
bool g_hookSet = false;

uint32_t RankTimeoutMs() {
    static const uint32_t ms = [] {
        const char* e = getenv("MSC_ONLINE_TIMEOUT_MS");
        const long v = e && *e ? atol(e) : 15000;
        return uint32_t(v < 1000 ? 1000 : v);
    }();
    return ms;
}

bool KnownRegion(uint32_t region) {
    return region == DWC_RNK_REGION_JP || region == DWC_RNK_REGION_US || region == DWC_RNK_REGION_EU || region == DWC_RNK_REGION_KR ||
           region == DWC_RNK_REGION_ALL;
}

void OnClearError() {
    pthread_mutex_lock(&g_lock);
    if (g_rank.state == DWC_RNK_STATE_ERROR) g_rank.state = DWC_RNK_STATE_INITIALIZED;
    pthread_mutex_unlock(&g_lock);
}

void CancelLocked() {
    if (g_rank.request) GetApi()->Cancel(g_rank.request);
    g_rank.request = 0;
    if (g_rank.queueId) { NpOnline::QueueFinished(g_rank.queueId, Fail::Cancelled); g_rank.queueId = 0; }   // stays queued for a later try
}

void FailLocked(Fail fail) {
    int type, code;
    NpDwcErrorFor(fail == Fail::None ? Fail::Protocol : fail, &type, &code);
    RANK_LOG("ranking %s failed (reason %d): error %d code %d\n", g_rank.isPut ? "put" : "get", int(fail), type, code);
    g_rank.request = 0;
    g_rank.refused = false;
    g_rank.waitingPut = false;
    if (g_rank.queueId) { NpOnline::QueueFinished(g_rank.queueId, fail); g_rank.queueId = 0; }   // retried later unless the service refused it for good
    g_rank.state = DWC_RNK_STATE_ERROR;
    g_rank.hasResponse = false;
    g_rank.rows.clear();
    NpDwcSetError(type, code);
}

// Scores of a descending board: competition rank (1 + rows with a better score).
uint32_t RankByScore(const np::HostVector<Row>& rows, size_t index, bool descending) {
    uint32_t better = 0;
    for (size_t i = 0; i < rows.size(); ++i)
        if (descending ? rows[i].score > rows[index].score : rows[i].score < rows[index].score) ++better;
    return better + 1;
}

// The reply of a Get into the rows the game may read. Returns the number of rows dropped.
int ValidateRows(const GetRequest& req, const GetReply& reply, int32_t ownPid, np::HostVector<Row>* out) {
    out->clear();
    int dropped = 0;
    std::set<int32_t> seen;
    for (size_t i = 0; i < reply.rows.size(); ++i) {
        const Row& row = reply.rows[i];
        if (row.pid <= 0 || row.size != kGameRowBytes || !seen.insert(row.pid).second || out->size() >= req.limit) {
            ++dropped;
            continue;
        }
        out->push_back(row);
    }
    if (req.mode == DWC_RNK_GET_MODE_NEAR) {
        bool ranked = false;
        for (size_t i = 0; i < out->size(); ++i) ranked = ranked || (*out)[i].order >= 1;
        if (!ranked) {
            if (!out->empty()) {
                for (size_t i = 0; i < out->size(); ++i) (*out)[i].order = RankByScore(*out, i, req.sort != 0);
            } else {
                Row own;
                own.order = reply.total + 1;
                own.pid = ownPid;
                own.score = 0;
                own.region = req.region;
                own.lastupdate = 0;
                own.size = 0;
                out->push_back(own);
                RANK_LOG("ranking: no row near the player; placeholder row at order %u\n", unsigned(own.order));
            }
        }
    }
    return dropped;
}

}  // namespace

extern "C" DWCRnkError DWC_RnkInitialize(const char* initdata, const DWCUserData* userdata) {
    pthread_mutex_lock(&g_lock);
    DWCRnkError result = DWC_RNK_SUCCESS;
    if (g_rank.state == DWC_RNK_STATE_ERROR) result = DWC_RNK_IN_ERROR;
    else if (g_rank.state != DWC_RNK_STATE_NOTREADY) result = DWC_RNK_ERROR_INIT_ALREADYINITIALIZED;
    else if (!initdata) result = DWC_RNK_ERROR_INIT_INVALID_INITDATA;
    else if (!NpDwcUserDataValid(userdata) || !NpDwcUserDataHasProfile(userdata) ||
             NpDwcUserDataProfile(userdata) != NpDwcLoggedInProfile())
        result = DWC_RNK_ERROR_INIT_INVALID_USERDATA;
    if (result == DWC_RNK_SUCCESS) {
        g_rank.pid = NpDwcUserDataProfile(userdata);
        g_rank.state = DWC_RNK_STATE_INITIALIZED;
        g_rank.hasResponse = false;
        g_rank.rows.clear();
        if (!g_hookSet) { NpDwcClearErrorHook(OnClearError); g_hookSet = true; }
    }
    pthread_mutex_unlock(&g_lock);
    return result;
}

extern "C" DWCRnkError DWC_RnkShutdown(void) {
    pthread_mutex_lock(&g_lock);
    CancelLocked();
    g_rank = Rank();
    pthread_mutex_unlock(&g_lock);
    return DWC_RNK_SUCCESS;
}

extern "C" DWCRnkError DWC_RnkPutScoreAsync(u32 category, DWCRnkRegion region, s32 score, void* data, u32 size) {
    pthread_mutex_lock(&g_lock);
    DWCRnkError result = DWC_RNK_SUCCESS;
    if (g_rank.state == DWC_RNK_STATE_ERROR) result = DWC_RNK_IN_ERROR;
    else if (g_rank.state != DWC_RNK_STATE_INITIALIZED && g_rank.state != DWC_RNK_STATE_COMPLETED) result = DWC_RNK_ERROR_PUT_NOTREADY;
    else if (category > DWC_RNK_CATEGORY_MAX || size > DWC_RNK_DATA_MAX || (!data && size != 0) || !KnownRegion(region))
        result = DWC_RNK_ERROR_INVALID_PARAMETER;
    if (result == DWC_RNK_SUCCESS) {
        PutRequest request;
        request.category = category;
        request.region = region;
        request.score = score;
        request.size = size;
        if (size) memcpy(request.data, data, size);
        if (!RulesAllowRanked()) {
            // A game that plays differently from retail must not post scores. The refusal shows when the game asks how the put went.
            RANK_LOG("ranking: scores are not submitted while the NK-bug fix (MSC_GAME_FIXES) is on\n");
            SetStatusMessage("Scores are not sent while the NK-bug fix of MSC_GAME_FIXES is on.");
            g_rank.request = 0;
            g_rank.queueId = 0;
            g_rank.refused = true;
            g_rank.isPut = true;
            g_rank.deadline = NowMs() + RankTimeoutMs();
            g_rank.hasResponse = false;
            g_rank.rows.clear();
            g_rank.state = DWC_RNK_STATE_PUT_ASYNC;
            pthread_mutex_unlock(&g_lock);
            return result;
        }
        const uint32_t queueId = NpOnline::QueuePut(request);   // on disk before the first send
        if (!NpOnline::QueueMayPut()) {
            // A reported match is not settled yet: totals sent now would be clamped (PROTOCOL.md section 8). Hold the put; the game sees it busy.
            RANK_LOG("ranking: the put waits for the reported match to become final\n");
            g_rank.queueId = queueId;
            g_rank.request = 0;
            g_rank.waitingPut = true;
            g_rank.pendingPut = request;
            g_rank.holdDeadline = NowMs() + kHoldMs;
            g_rank.isPut = true;
            g_rank.deadline = 0;
            g_rank.hasResponse = false;
            g_rank.rows.clear();
            g_rank.state = DWC_RNK_STATE_PUT_ASYNC;
            pthread_mutex_unlock(&g_lock);
            return result;
        }
        const uint32_t id = GetApi()->BeginPutScore(request);
        if (!id) { result = DWC_RNK_ERROR_PUT_NOMEMORY; if (queueId) NpOnline::QueueFinished(queueId, Fail::Transport); }
        else {
            g_rank.queueId = queueId;
            g_rank.request = id;
            g_rank.isPut = true;
            g_rank.deadline = NowMs() + RankTimeoutMs();
            g_rank.hasResponse = false;
            g_rank.rows.clear();
            g_rank.state = DWC_RNK_STATE_PUT_ASYNC;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return result;
}

extern "C" DWCRnkError DWC_RnkGetScoreAsync(DWCRnkGetMode mode, u32 category, DWCRnkRegion region, DWCRnkGetParam* param) {
    pthread_mutex_lock(&g_lock);
    DWCRnkError result = DWC_RNK_SUCCESS;
    GetRequest request;
    if (g_rank.state == DWC_RNK_STATE_ERROR) result = DWC_RNK_IN_ERROR;
    else if (g_rank.state != DWC_RNK_STATE_INITIALIZED && g_rank.state != DWC_RNK_STATE_COMPLETED) result = DWC_RNK_ERROR_GET_NOTREADY;
    else if (category > DWC_RNK_CATEGORY_MAX || !param || !KnownRegion(region)) result = DWC_RNK_ERROR_INVALID_PARAMETER;
    else {
        request.mode = mode;
        request.category = category;
        request.region = region;
        switch (mode) {
        case DWC_RNK_GET_MODE_ORDER:
            if (param->size != sizeof(param->order) || param->order.sort > DWC_RNK_ORDER_DES) { result = DWC_RNK_ERROR_INVALID_PARAMETER; break; }
            request.sort = param->order.sort;
            request.since = param->order.since;
            break;
        case DWC_RNK_GET_MODE_TOPLIST:
            if (param->size != sizeof(param->toplist) || param->toplist.sort > DWC_RNK_ORDER_DES || param->toplist.limit > DWC_RNK_GET_MAX ||
                param->toplist.limit == 0) { result = DWC_RNK_ERROR_INVALID_PARAMETER; break; }
            request.sort = param->toplist.sort;
            request.limit = param->toplist.limit;
            request.since = param->toplist.since;
            break;
        case DWC_RNK_GET_MODE_NEAR:
            if (param->size != sizeof(param->near) || param->near.sort > DWC_RNK_ORDER_DES || param->near.limit > DWC_RNK_GET_MAX ||
                param->near.limit <= 1) { result = DWC_RNK_ERROR_INVALID_PARAMETER; break; }
            request.sort = param->near.sort;
            request.limit = param->near.limit;
            request.since = param->near.since;
            break;
        case DWC_RNK_GET_MODE_FRIENDS:
            if (param->size != sizeof(param->friends) || param->friends.sort > DWC_RNK_ORDER_DES ||
                param->friends.limit > DWC_RNK_FRIENDS_MAX + 1 || param->friends.limit <= 1) { result = DWC_RNK_ERROR_INVALID_PARAMETER; break; }
            request.sort = param->friends.sort;
            request.limit = param->friends.limit;
            request.since = param->friends.since;
            for (int i = 0; i < DWC_RNK_FRIENDS_MAX; ++i) {   // empty slots are zeros; the player is added by the service
                const int32_t pid = param->friends.friends[i];
                if (pid <= 0 || pid == g_rank.pid) continue;
                bool dup = false;
                for (uint32_t k = 0; k < request.friendCount; ++k) dup = dup || request.friends[k] == pid;
                if (!dup) request.friends[request.friendCount++] = pid;
            }
            break;
        default:
            result = DWC_RNK_ERROR_INVALID_MODE;   // unknown modes are refused
            break;
        }
    }
    if (result == DWC_RNK_SUCCESS) {
        const uint32_t id = GetApi()->BeginGetScores(request);
        if (!id) result = DWC_RNK_ERROR_GET_NOMEMORY;
        else {
            g_rank.request = id;
            g_rank.isPut = false;
            g_rank.get = request;
            g_rank.deadline = NowMs() + RankTimeoutMs();
            g_rank.hasResponse = false;
            g_rank.rows.clear();
            g_rank.state = DWC_RNK_STATE_GET_ASYNC;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return result;
}

extern "C" DWCRnkError DWC_RnkProcess(void) {
    pthread_mutex_lock(&g_lock);
    DWCRnkError result = DWC_RNK_SUCCESS;
    if (g_rank.state == DWC_RNK_STATE_ERROR) {
        result = DWC_RNK_IN_ERROR;
    } else if (g_rank.state != DWC_RNK_STATE_PUT_ASYNC && g_rank.state != DWC_RNK_STATE_GET_ASYNC) {
        result = DWC_RNK_PROCESS_NOTASK;
    } else if (g_rank.refused) {
        FailLocked(Fail::Rejected);
        result = DWC_RNK_IN_ERROR;
    } else if (g_rank.waitingPut) {
        NpOnline::QueuePump(true);   // sends the result report and polls the match record (this frame may be the only pump)
        if (NpOnline::QueueMayPut()) {
            const uint32_t id = GetApi()->BeginPutScore(g_rank.pendingPut);
            g_rank.waitingPut = false;
            if (!id) { FailLocked(Fail::Transport); result = DWC_RNK_IN_ERROR; }
            else { g_rank.request = id; g_rank.deadline = NowMs() + RankTimeoutMs(); }
        } else if (NowMs() >= g_rank.holdDeadline) {
            RANK_LOG("ranking: the match is still not settled; the put stays queued\n");
            FailLocked(Fail::Timeout);
            result = DWC_RNK_IN_ERROR;
        }
    } else {
        Api* api = GetApi();
        Result r;
        GetReply reply;
        PutReply putReply;
        if (g_rank.isPut) r = api->PollPutScore(g_rank.request, &putReply);
        else r = api->PollGetScores(g_rank.request, &reply);
        if (r.status == Status::Pending) {
            if (NowMs() >= g_rank.deadline) {
                api->Cancel(g_rank.request);
                FailLocked(Fail::Timeout);
                result = DWC_RNK_IN_ERROR;
            }
        } else if (r.status == Status::Failed) {
            FailLocked(r.fail);
            result = DWC_RNK_IN_ERROR;
        } else {
            g_rank.request = 0;
            if (g_rank.queueId) { NpOnline::QueueFinished(g_rank.queueId, Fail::None); g_rank.queueId = 0; }
            if (g_rank.isPut && putReply.clamped)
                RANK_LOG("ranking: the service stored %d (lower than sent): the total is ahead of verified matches; the next read of the player's row shows the value to continue from\n", int(putReply.stored));
            if (!g_rank.isPut) {
                const int dropped = ValidateRows(g_rank.get, reply, g_rank.pid, &g_rank.rows);
                if (dropped) RANK_LOG("ranking: %d malformed row(s) dropped\n", dropped);
                g_rank.responseMode = g_rank.get.mode;
                g_rank.hasResponse = true;
            }
            g_rank.state = DWC_RNK_STATE_COMPLETED;   // this call answers SUCCESS, the next one NOTASK
        }
    }
    pthread_mutex_unlock(&g_lock);
    return result;
}

// Common prelude of the result accessors (DWCi_RankingGetResponse): error, completion, an answer with rows.
static DWCRnkError ResponseStatus() {
    if (g_rank.state == DWC_RNK_STATE_ERROR) return DWC_RNK_IN_ERROR;
    if (g_rank.state != DWC_RNK_STATE_COMPLETED) return DWC_RNK_ERROR_NOTCOMPLETED;
    if (!g_rank.hasResponse) return DWC_RNK_ERROR_EMPTY_RESPONSE;
    if (g_rank.responseMode == DWC_RNK_GET_MODE_ORDER) return DWC_RNK_ERROR_INVALID_MODE;
    return DWC_RNK_SUCCESS;
}

extern "C" DWCRnkError DWC_RnkResGetRow(DWCRnkData* out, u32 index) {
    pthread_mutex_lock(&g_lock);
    DWCRnkError result = ResponseStatus();
    if (result == DWC_RNK_SUCCESS) {
        if (!out || index >= g_rank.rows.size()) result = DWC_RNK_ERROR_INVALID_PARAMETER;
        else {
            Row& row = g_rank.rows[index];
            out->order = row.order;
            out->pid = row.pid;
            out->score = row.score;
            out->region = (DWCRnkRegion)row.region;
            out->lastupdate = row.lastupdate;
            out->size = row.size;
            out->userdata = row.data;   // valid until the next request
        }
    }
    pthread_mutex_unlock(&g_lock);
    return result;
}

extern "C" DWCRnkError DWC_RnkResGetRowCount(u32* count) {
    pthread_mutex_lock(&g_lock);
    DWCRnkError result = ResponseStatus();
    if (result == DWC_RNK_SUCCESS) {
        if (!count) result = DWC_RNK_ERROR_INVALID_PARAMETER;
        else *count = u32(g_rank.rows.size());
    }
    pthread_mutex_unlock(&g_lock);
    return result;
}

bool NpDwcRankBusy() { return g_rank.state == DWC_RNK_STATE_PUT_ASYNC || g_rank.state == DWC_RNK_STATE_GET_ASYNC; }

// test support: used by NpDwcResetForTest
void NpDwcRankResetForTest() {
    pthread_mutex_lock(&g_lock);
    CancelLocked();
    g_rank = Rank();
    pthread_mutex_unlock(&g_lock);
}
